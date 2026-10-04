#include "net_sync.h"

#include "config.h"
#include "db.h"
#include "net_protocol.h"
#include "net_server.h"
#include "net_tls.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static NetSyncStatus sync_status;
static pthread_t sync_worker_thread;
static int sync_worker_created = 0;
static int sync_poll_requested = 0;
static int sync_active_socket = -1;
static unsigned long sync_poll_request_generation = 0;
static unsigned long sync_poll_completed_generation = 0;
static int sync_worker_last_poll_rc = 0;
static int sync_serial_prefetch_enabled = 0;
static unsigned long long sync_reservation_request_counter = 0;

#define NET_SYNC_BATCH_MAX 16
#define NET_SYNC_PUSH_DRAIN_MAX 8

static int sync_failure_streak = 0;
static time_t sync_next_attempt_utc = 0;
static time_t sync_last_heartbeat_utc = 0;
static int sync_reconnect_count = 0;
static unsigned int sync_rng_state = 0;

static int net_sync_poll_once_impl(void);
static void *net_sync_worker_main(void *unused);
static void net_sync_worker_service_serials(void);
static int net_sync_make_reservation_request_id(const char *station_id,
                                               char *out, size_t out_size);

int net_sync_token_is_strong(const char *token) {
  if (!token)
    return 0;
  size_t length = strlen(token);
  if (length < 32 || length >= 128)
    return 0;

  unsigned int classes = 0;
  unsigned char seen[128] = {0};
  size_t distinct = 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)token[i];
    if (ch < 33 || ch > 126)
      return 0;
    if (islower(ch))
      classes |= 1u;
    else if (isupper(ch))
      classes |= 2u;
    else if (isdigit(ch))
      classes |= 4u;
    else
      classes |= 8u;
    if (!seen[ch]) {
      seen[ch] = 1;
      distinct++;
    }
  }

  unsigned int class_count = 0;
  for (unsigned int mask = classes; mask; mask &= mask - 1)
    class_count++;
  return class_count >= 3 && distinct >= 12;
}

int net_sync_validate_config(char *error, size_t error_size) {
  if (error && error_size > 0)
    error[0] = 0;

  if (strcasecmp(config.net_role, "client") != 0 &&
      strcasecmp(config.net_role, "server") != 0) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET_ROLE must be client or server");
    return -1;
  }
  if (config.net_server_port < 1 || config.net_server_port > 65535) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET_SERVER_PORT is out of range");
    return -1;
  }
  if (strcasecmp(config.net_role, "client") == 0 &&
      !config.net_server_host[0]) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET_SERVER_HOST is required for clients");
    return -1;
  }
  if (strcasecmp(config.net_role, "client") == 0 &&
      !db_sync_validate_shared_log_id(config.net_shared_log_id)) {
    if (error && error_size > 1)
      snprintf(error, error_size,
               "NET_SHARED_LOG_ID must be explicitly paired by the operator");
    return -1;
  }
  if (config.net_shared_log_id[0] &&
      !db_sync_validate_shared_log_id(config.net_shared_log_id)) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET_SHARED_LOG_ID has an invalid format");
    return -1;
  }
  const char *auth_token = config.net_auth_token[0]
                               ? config.net_auth_token
                               : config.net_shared_key;
  if (config.net_auth_token[0] && config.net_shared_key[0] &&
      strcmp(config.net_auth_token, config.net_shared_key) != 0) {
    if (error && error_size > 1)
      snprintf(error, error_size,
               "NET_AUTH_TOKEN and NET_SHARED_KEY must match");
    return -1;
  }
  if (strcasecmp(config.net_role, "server") == 0 &&
      !net_sync_token_is_strong(auth_token)) {
    if (error && error_size > 1)
      snprintf(error, error_size,
               "NET_AUTH_TOKEN must be at least 32 strong characters");
    return -1;
  }
  if (config.net_tls && strcasecmp(config.net_role, "client") == 0 &&
      !config.net_tls_peer_fingerprint[0]) {
    if (error && error_size > 1)
      snprintf(error, error_size,
               "NET_TLS_PEER_FINGERPRINT is required for TLS clients");
    return -1;
  }
  if (config.net_sync_interval_ms < 100 ||
      config.net_sync_interval_ms > 60000 || config.net_heartbeat_sec < 1 ||
      config.net_heartbeat_sec > 300 || config.net_retry_min_ms < 100 ||
      config.net_retry_max_ms < config.net_retry_min_ms ||
      config.net_max_frame_bytes < 1024 || config.net_max_frame_bytes > 65536) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET timing or frame settings are invalid");
    return -1;
  }
  if (config.net_tls && strcasecmp(config.net_role, "server") == 0 &&
      (!config.net_tls_cert_file[0] || !config.net_tls_key_file[0])) {
    if (error && error_size > 1)
      snprintf(error, error_size, "NET TLS certificate or key is unavailable");
    return -1;
  }
  return 0;
}

static void sync_set_active_socket(int fd) {
  pthread_mutex_lock(&sync_mutex);
  sync_active_socket = fd;
  pthread_mutex_unlock(&sync_mutex);
}

static void sync_clear_active_socket(int fd) {
  pthread_mutex_lock(&sync_mutex);
  if (sync_active_socket == fd)
    sync_active_socket = -1;
  pthread_mutex_unlock(&sync_mutex);
}

static void net_sync_close_transport(NetTransport *transport) {
  if (!transport)
    return;
  int fd = transport->fd;
  sync_clear_active_socket(fd);
  net_transport_close(transport);
}

static void net_sync_update_queue_status(void) {
  int pending = 0;
  int failed = 0;
  (void)db_sync_get_pending_outbox_count(&pending);
  (void)db_sync_get_failed_outbox_count(&failed);
  pthread_mutex_lock(&sync_mutex);
  sync_status.pending_outbox = pending;
  sync_status.failed_outbox = failed;
  pthread_mutex_unlock(&sync_mutex);
}

static void *net_sync_worker_main(void *unused) {
  (void)unused;

  pthread_mutex_lock(&sync_mutex);
  while (sync_status.running) {
    int interval_ms = config.net_sync_interval_ms;
    if (interval_ms < 100)
      interval_ms = 1000;

    if (!sync_poll_requested) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += interval_ms / 1000;
      deadline.tv_nsec += (long)(interval_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      while (sync_status.running && !sync_poll_requested) {
        int wait_rc = pthread_cond_timedwait(&sync_cond, &sync_mutex,
                                             &deadline);
        if (wait_rc == ETIMEDOUT)
          break;
      }
    }

    if (!sync_status.running)
      break;

    sync_poll_requested = 0;
    unsigned long request_generation = sync_poll_request_generation;
    pthread_mutex_unlock(&sync_mutex);

    int poll_rc = net_sync_poll_once_impl();
    if (poll_rc == 0)
      net_sync_worker_service_serials();
    net_sync_update_queue_status();

    pthread_mutex_lock(&sync_mutex);
    sync_worker_last_poll_rc = poll_rc;
    if (request_generation > sync_poll_completed_generation)
      sync_poll_completed_generation = request_generation;
    pthread_cond_broadcast(&sync_cond);
  }
  pthread_mutex_unlock(&sync_mutex);
  return NULL;
}

static void net_sync_format_utc(time_t when, char *out, size_t out_size) {
  if (!out || out_size < 2) {
    return;
  }

  out[0] = 0;
  if (when <= 0)
    return;

  struct tm tm_utc;
  if (!gmtime_r(&when, &tm_utc))
    return;

  strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static const char *net_sync_auth_token(void) {
  if (config.net_shared_key[0])
    return config.net_shared_key;
  return config.net_auth_token;
}

static int net_sync_make_reservation_request_id(const char *station_id,
                                               char *out, size_t out_size) {
  if (!station_id || !station_id[0] || !out || out_size < 2)
    return -1;
  pthread_mutex_lock(&sync_mutex);
  unsigned long long request_counter = ++sync_reservation_request_counter;
  unsigned int nonce = rand_r(&sync_rng_state);
  pthread_mutex_unlock(&sync_mutex);
  int n = snprintf(out, out_size, "req-%s-%08x-%08x", station_id,
                   (unsigned int)request_counter, nonce);
  return n > 0 && (size_t)n < out_size ? 0 : -1;
}

static void net_sync_set_socket_timeout(int fd) {
  int timeout_sec = config.net_heartbeat_sec;
  if (timeout_sec < 1)
    timeout_sec = 1;
  if (timeout_sec > 5)
    timeout_sec = 5;
  struct timeval timeout = {.tv_sec = timeout_sec, .tv_usec = 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int net_sync_active_log_matches_pairing(void) {
  char active_shared_log_id[36] = {0};
  return config.net_shared_log_id[0] &&
         db_sync_get_shared_log_id(active_shared_log_id,
                                   sizeof(active_shared_log_id)) == 0 &&
         strcmp(active_shared_log_id, config.net_shared_log_id) == 0;
}

static int net_sync_backoff_seconds(int failure_streak) {
  int min_ms = config.net_retry_min_ms;
  int max_ms = config.net_retry_max_ms;
  if (min_ms < 100)
    min_ms = 1000;
  if (max_ms < min_ms)
    max_ms = min_ms;

  if (failure_streak < 1)
    failure_streak = 1;
  if (failure_streak > 8)
    failure_streak = 8;

  long long delay_ms = (long long)min_ms << (failure_streak - 1);
  if (delay_ms > max_ms)
    delay_ms = max_ms;

  if (sync_rng_state == 0)
    sync_rng_state = (unsigned int)(time(NULL) ^ (unsigned int)getpid());

  int jitter_percent = 80 + (int)(rand_r(&sync_rng_state) % 41);
  delay_ms = (delay_ms * jitter_percent) / 100;
  if (delay_ms < min_ms)
    delay_ms = min_ms;
  if (delay_ms > max_ms)
    delay_ms = max_ms;

  return (int)((delay_ms + 999) / 1000);
}

static int net_sync_retry_delay_for_entry(const SyncOutboxEntry *entry) {
  if (!entry)
    return 1;

  int streak = entry->retry_count + 1;
  return net_sync_backoff_seconds(streak);
}

static int response_is_error(const char *frame) {
  NetMessageType mt = NET_MSG_UNKNOWN;
  if (!frame || net_protocol_detect_type(frame, &mt) != 0)
    return 1;
  return mt == NET_MSG_ERROR;
}

static int response_is_unsupported_protocol_error(const char *frame) {
  if (!response_is_error(frame))
    return 0;

  char code[64] = {0};
  if (net_protocol_parse_error_code(frame, code, sizeof(code)) != 0)
    return 0;

  return strcmp(code, "ERROR_UNSUPPORTED_PROTOCOL") == 0;
}

static int net_sync_send_hello_and_expect_ack(NetTransport *transport,
                                              const char *station_id,
                                              char *out_error,
                                              size_t out_error_size) {
  if (!transport || !station_id || !station_id[0])
    return -1;

  char frame[1024] = {0};
  if (net_protocol_encode_hello(station_id, "logger", net_sync_auth_token(),
                                frame, sizeof(frame)) != 0 ||
      net_protocol_send_framed_io(transport, net_transport_write_cb, frame) !=
          0)
    return -1;

  char hello_ack[2048] = {0};
  if (net_protocol_recv_framed_io_limited(transport, net_transport_read_cb,
                                          hello_ack, sizeof(hello_ack),
                                          (size_t)config.net_max_frame_bytes) !=
      0)
    return -1;

  if (net_protocol_validate_protocol_version(hello_ack) != 0) {
    if (out_error && out_error_size > 1)
      snprintf(out_error, out_error_size, "ERROR_UNSUPPORTED_PROTOCOL");
    return -1;
  }

  if (response_is_error(hello_ack)) {
    if (out_error && out_error_size > 1)
      snprintf(out_error, out_error_size,
               "%s", response_is_unsupported_protocol_error(hello_ack)
                         ? "ERROR_UNSUPPORTED_PROTOCOL"
                         : "HELLO server error");
    return -1;
  }

  int hello_accepted = 0;
  long long hello_next_expected_seq = 0;
  long long hello_server_seq = 0;
  char hello_shared_log_id[36] = {0};
  if (net_protocol_parse_hello_ack(hello_ack, &hello_accepted,
                                   &hello_next_expected_seq,
                                   &hello_server_seq, hello_shared_log_id,
                                   sizeof(hello_shared_log_id)) != 0 ||
      !hello_accepted ||
      strcmp(hello_shared_log_id, config.net_shared_log_id) != 0) {
    if (out_error && out_error_size > 1)
      snprintf(out_error, out_error_size, "HELLO rejected");
    return -1;
  }
  (void)hello_next_expected_seq;
  (void)hello_server_seq;

  return 0;
}

/*
 * Open a TCP connection to the configured central log endpoint.
 *
 * @param host Destination hostname or address.
 * @param port Destination TCP port.
 * @return Connected socket fd, or -1 on failure.
 */
static int net_connect_tcp(const char *host, int port) {
  if (!host || !host[0] || port < 1 || port > 65535)
    return -1;

  char port_text[16];
  snprintf(port_text, sizeof(port_text), "%d", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_text, &hints, &res) != 0)
    return -1;

  int fd = -1;
  for (struct addrinfo *it = res; it; it = it->ai_next) {
    fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0)
      continue;

    int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 ||
        fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
      close(fd);
      fd = -1;
      continue;
    }

    sync_set_active_socket(fd);
    int connect_rc = connect(fd, it->ai_addr, it->ai_addrlen);
    if (connect_rc != 0 && errno == EINPROGRESS) {
      fd_set write_fds;
      FD_ZERO(&write_fds);
      FD_SET(fd, &write_fds);
      struct timeval connect_timeout = {.tv_sec = 3, .tv_usec = 0};
      connect_rc = select(fd + 1, NULL, &write_fds, NULL, &connect_timeout);
      if (connect_rc > 0) {
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) != 0 || socket_error != 0) {
          errno = socket_error;
          connect_rc = -1;
        } else {
          connect_rc = 0;
        }
      } else {
        connect_rc = -1;
      }
    }

    if (connect_rc == 0 && fcntl(fd, F_SETFL, original_flags) == 0)
      break;

    sync_clear_active_socket(fd);
    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  if (fd >= 0)
    sync_set_active_socket(fd);
  return fd;
}

static int apply_broadcast_frame(const char *frame) {
  if (!frame)
    return -1;

  SyncLogOpEntry op;
  memset(&op, 0, sizeof(op));
  if (net_protocol_parse_op_broadcast(frame, &op) != 0)
    return -1;

  int local_logbook_id = 0;
  if (!net_sync_active_log_matches_pairing() ||
      db_get_current_logbook_id(&local_logbook_id) != 0 ||
      local_logbook_id <= 0)
    return -1;

  int rc = db_sync_apply_remote_op(
      op.op_id, op.station_id, op.station_seq, local_logbook_id, op.op_type,
      op.entity_id, op.payload_json, op.op_utc, NULL);
  return rc < 0 ? -1 : 0;
}

/*
 * Parse ACK frame and mark acked operation ids in outbox.
 *
 * @param text Server response payload.
 * @return Number of marked operations, or -1 on invalid args.
 */
static int apply_acked_op_ids(const char *text) {
  if (!text)
    return -1;

  const char *acked = strstr(text, "\"acked\":[");
  if (!acked)
    acked = strstr(text, "\"accepted_ops\":[");
  if (!acked)
    return 0;

  acked = strchr(acked, '[');
  if (!acked)
    return 0;
  acked++;

  int changed = 0;
  while (*acked && *acked != ']') {
    while (*acked == ' ' || *acked == '\t' || *acked == ',')
      acked++;
    if (*acked != '"') {
      acked++;
      continue;
    }

    acked++;
    const char *end = strchr(acked, '"');
    if (!end)
      break;

    char op_id[96] = {0};
    size_t len = (size_t)(end - acked);
    if (len >= sizeof(op_id))
      len = sizeof(op_id) - 1;
    memcpy(op_id, acked, len);
    op_id[len] = 0;

    if (op_id[0] && db_sync_outbox_mark_acked(op_id) == 0)
      changed++;

    acked = end + 1;
  }

  return changed;
}

/*
 * Check whether an ACK payload contains a given operation id.
 */
static int ack_contains_op_id(const char *text, const char *op_id) {
  if (!text || !op_id || !op_id[0])
    return 0;

  const char *acked = strstr(text, "\"acked\":[");
  if (!acked)
    acked = strstr(text, "\"accepted_ops\":[");
  if (!acked)
    return 0;

  const char *cursor = strchr(acked, '[');
  if (!cursor)
    return 0;
  cursor++;

  while (*cursor && *cursor != ']') {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
      cursor++;
    if (*cursor != '"') {
      cursor++;
      continue;
    }

    const char *start = ++cursor;
    while (*cursor && *cursor != '"') {
      if (*cursor == '\\' && cursor[1])
        cursor++;
      cursor++;
    }
    if (!*cursor)
      return 0;

    size_t len = (size_t)(cursor - start);
    if (strlen(op_id) == len && memcmp(start, op_id, len) == 0)
      return 1;
    cursor++;
  }

  return 0;
}

int net_sync_start(void) {
  char validation_error[128] = {0};
  if (!config.net_enabled ||
      net_sync_validate_config(validation_error, sizeof(validation_error)) != 0) {
    pthread_mutex_lock(&sync_mutex);
    snprintf(sync_status.last_error, sizeof(sync_status.last_error), "%s",
             validation_error[0] ? validation_error : "NET is disabled");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  if (db_init() != 0)
    return -1;
  char active_shared_log_id[36] = {0};
  if (strcasecmp(config.net_role, "server") == 0) {
    int shared_id_rc = db_sync_get_shared_log_id(active_shared_log_id,
                                                 sizeof(active_shared_log_id));
    if (shared_id_rc == 1) {
      if (db_sync_create_shared_log_id(active_shared_log_id,
                                       sizeof(active_shared_log_id)) != 0)
        return -1;
    }
    if (shared_id_rc < 0)
      return -1;
  } else if (db_sync_set_shared_log_id(config.net_shared_log_id) != 0 ||
             db_sync_get_shared_log_id(active_shared_log_id,
                                       sizeof(active_shared_log_id)) != 0 ||
             strcmp(active_shared_log_id, config.net_shared_log_id) != 0) {
    return -1;
  }
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           active_shared_log_id);
  if (strcasecmp(config.net_role, "client") == 0 &&
      db_sync_recover_serial_claims() != 0)
    return -1;

  pthread_mutex_lock(&sync_mutex);
  if (sync_status.running) {
    pthread_mutex_unlock(&sync_mutex);
    return 0;
  }
  memset(&sync_status, 0, sizeof(sync_status));
  sync_failure_streak = 0;
  sync_next_attempt_utc = 0;
  sync_last_heartbeat_utc = 0;
  sync_reconnect_count = 0;
  sync_rng_state = (unsigned int)(time(NULL) ^ (unsigned int)getpid());
  sync_status.running = 1;
  sync_status.tls_enabled = config.net_tls ? 1 : 0;
  sync_status.reconnect_count = 0;
  sync_status.failure_streak = 0;
  sync_status.last_success_utc[0] = 0;
  sync_status.last_heartbeat_utc[0] = 0;
  if (config.net_station_id[0])
    (void)db_sync_set_station_id(config.net_station_id);
  if (db_sync_get_or_create_station_id(sync_status.station_id,
                                       sizeof(sync_status.station_id)) != 0) {
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "station id unavailable");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }
  db_sync_get_pending_outbox_count(&sync_status.pending_outbox);
  db_sync_get_failed_outbox_count(&sync_status.failed_outbox);
  db_sync_get_last_global_seq(&sync_status.last_pulled_global_seq);
  sync_poll_requested = 0;
  sync_poll_request_generation = 0;
  sync_poll_completed_generation = 0;
  sync_worker_last_poll_rc = 0;
  if (strcasecmp(config.net_role, "client") == 0) {
    sync_poll_request_generation++;
    sync_poll_requested = 1;
  }
  pthread_mutex_unlock(&sync_mutex);

  if (strcasecmp(config.net_role, "server") == 0) {
    if (net_server_start() != 0) {
      pthread_mutex_lock(&sync_mutex);
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "net server start failed");
      sync_status.running = 0;
      pthread_mutex_unlock(&sync_mutex);
      return -1;
    }
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 1;
    pthread_mutex_unlock(&sync_mutex);
  } else {
    pthread_mutex_lock(&sync_mutex);
    int thread_rc = pthread_create(&sync_worker_thread, NULL,
                                   net_sync_worker_main, NULL);
    if (thread_rc == 0) {
      sync_worker_created = 1;
    } else {
      sync_status.running = 0;
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "client worker start failed");
    }
    pthread_mutex_unlock(&sync_mutex);
    if (thread_rc != 0)
      return -1;
  }

  return 0;
}

void net_sync_stop(void) {
  pthread_mutex_lock(&sync_mutex);
  sync_status.running = 0;
  sync_status.connected = 0;
  sync_poll_requested = 0;
  int active_socket = sync_active_socket;
  int join_worker = sync_worker_created;
  pthread_cond_broadcast(&sync_cond);
  pthread_mutex_unlock(&sync_mutex);

  if (active_socket >= 0)
    shutdown(active_socket, SHUT_RDWR);
  if (join_worker && !pthread_equal(pthread_self(), sync_worker_thread))
    pthread_join(sync_worker_thread, NULL);
  net_server_stop();

  pthread_mutex_lock(&sync_mutex);
  sync_worker_created = 0;
  sync_active_socket = -1;
  pthread_cond_broadcast(&sync_cond);
  pthread_mutex_unlock(&sync_mutex);
}

int net_sync_reserve_serial_remote_ex(int *out_serial,
                                      char *out_reservation_id,
                                      size_t out_reservation_id_size) {
  if (!out_serial || !out_reservation_id || out_reservation_id_size < 2)
    return -1;

  *out_serial = 0;
  out_reservation_id[0] = 0;

  if (!config.net_enabled || strcasecmp(config.net_role, "client") != 0)
    return -1;
  if (!net_sync_active_log_matches_pairing())
    return -1;
  if (net_sync_validate_config(NULL, 0) != 0)
    return -1;

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  int logbook_id = 1;
  if (db_get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;

  char candidate_request_id[64] = {0};
  char req_id[64] = {0};
  if (net_sync_make_reservation_request_id(station_id, candidate_request_id,
                                           sizeof(candidate_request_id)) != 0 ||
      db_sync_get_or_create_serial_request_id(
          candidate_request_id, req_id, sizeof(req_id)) != 0)
    return -1;

  int sock = net_connect_tcp(config.net_server_host, config.net_server_port);
  if (sock < 0)
    return -1;

  NetTransport transport;
  char transport_error[128] = {0};
  if (net_transport_init_client(&transport, sock, config.net_server_host,
                                config.net_tls,
                                config.net_tls_peer_fingerprint,
                                transport_error,
                                sizeof(transport_error)) != 0) {
                  sync_clear_active_socket(sock);
                  close(sock);
    return -1;
  }
  net_sync_set_socket_timeout(sock);

  int rc = -1;
  do {
    char hello_error[64] = {0};
    if (net_sync_send_hello_and_expect_ack(&transport, station_id,
                                           hello_error,
                                           sizeof(hello_error)) != 0)
      break;

    char frame[512] = {0};

    if (net_protocol_encode_reserve_serial(req_id, logbook_id, 120, frame,
                                           sizeof(frame)) != 0 ||
        net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                    frame) != 0)
      break;

    char response[2048] = {0};
    if (net_protocol_recv_framed_io_limited(&transport, net_transport_read_cb,
                                            response, sizeof(response),
                                            (size_t)config.net_max_frame_bytes) !=
        0)
      break;

    if (net_protocol_validate_protocol_version(response) != 0)
      break;

    if (response_is_error(response))
      break;
    if (net_protocol_validate_shared_log_id(response,
                                            config.net_shared_log_id) != 0)
      break;

    char ack_req_id[64] = {0};
    int serial = 0;
    char expires_utc[32] = {0};
    if (net_protocol_parse_reserve_serial_ack(
            response, ack_req_id, sizeof(ack_req_id), out_reservation_id,
            out_reservation_id_size, &serial, expires_utc,
            sizeof(expires_utc)) != 0 ||
        serial <= 0 || strcmp(ack_req_id, req_id) != 0)
      break;

    if (db_sync_cache_serial_reservation(out_reservation_id, logbook_id,
                                         station_id, serial,
                                         expires_utc) != 0)
      break;
    if (db_sync_clear_serial_request_id(req_id) != 0)
      break;

    *out_serial = serial;
    rc = 0;
  } while (0);

  net_sync_close_transport(&transport);
  return rc;
}

int net_sync_reserve_serial_remote(int *out_serial) {
  char reservation_id[64] = {0};
  return net_sync_reserve_serial_remote_ex(out_serial, reservation_id,
                                           sizeof(reservation_id));
}

void net_sync_set_serial_prefetch_enabled(int enabled) {
  pthread_mutex_lock(&sync_mutex);
  sync_serial_prefetch_enabled = enabled ? 1 : 0;
  if (sync_serial_prefetch_enabled && sync_status.running &&
      strcasecmp(config.net_role, "client") == 0) {
    sync_poll_request_generation++;
    sync_poll_requested = 1;
  }
  pthread_cond_signal(&sync_cond);
  pthread_mutex_unlock(&sync_mutex);
}

int net_sync_peek_serial_reservation(int *out_serial) {
  if (!out_serial || !config.net_enabled)
    return -1;
  if (!net_sync_active_log_matches_pairing())
    return -1;
  *out_serial = 0;

  int logbook_id = 1;
  if (db_get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;

  if (strcasecmp(config.net_role, "server") == 0)
    return db_sync_peek_next_serial(logbook_id, out_serial);
  if (strcasecmp(config.net_role, "client") != 0)
    return -1;

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;
  return db_sync_peek_available_serial_reservation(logbook_id, station_id,
                                                   out_serial);
}

int net_sync_reserve_serial_for_qso(int *out_serial,
                                    char *out_reservation_id,
                                    size_t out_reservation_id_size,
                                    int *out_commit_remote) {
  if (!out_serial || !out_reservation_id || out_reservation_id_size < 2 ||
      !out_commit_remote || !config.net_enabled)
    return -1;
  *out_serial = 0;
  out_reservation_id[0] = 0;
  *out_commit_remote = 0;

  pthread_mutex_lock(&sync_mutex);
  int running = sync_status.running;
  pthread_mutex_unlock(&sync_mutex);
  if (!running || !net_sync_active_log_matches_pairing())
    return -1;

  int logbook_id = 1;
  if (db_get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;
  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  if (strcasecmp(config.net_role, "client") == 0) {
    int rc = db_sync_claim_available_serial_reservation(
        logbook_id, station_id, out_reservation_id, out_reservation_id_size,
        out_serial);
    if (rc == 0)
      *out_commit_remote = 1;
    return rc == 0 ? 0 : -1;
  }

  if (strcasecmp(config.net_role, "server") != 0)
    return -1;

  char request_id[64] = {0};
  char expires_utc[32] = {0};
  if (net_sync_make_reservation_request_id(station_id, request_id,
                                           sizeof(request_id)) != 0)
    return -1;
  return db_sync_reserve_serial(logbook_id, station_id, request_id, 120,
                                out_reservation_id, out_reservation_id_size,
                                out_serial, expires_utc, sizeof(expires_utc));
}

int net_sync_commit_serial_remote(const char *reservation_id,
                                  const char *qso_uid) {
  if (!reservation_id || !reservation_id[0])
    return -1;

  if (!config.net_enabled || strcasecmp(config.net_role, "client") != 0)
    return -1;
  if (!net_sync_active_log_matches_pairing())
    return -1;
  if (net_sync_validate_config(NULL, 0) != 0)
    return -1;

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  int sock = net_connect_tcp(config.net_server_host, config.net_server_port);
  if (sock < 0)
    return -1;

  NetTransport transport;
  char transport_error[128] = {0};
  if (net_transport_init_client(&transport, sock, config.net_server_host,
                                config.net_tls,
                                config.net_tls_peer_fingerprint,
                                transport_error,
                                sizeof(transport_error)) != 0) {
                  sync_clear_active_socket(sock);
                  close(sock);
    return -1;
  }
  net_sync_set_socket_timeout(sock);

  int rc = -1;
  do {
    char hello_error[64] = {0};
    if (net_sync_send_hello_and_expect_ack(&transport, station_id,
                                           hello_error,
                                           sizeof(hello_error)) != 0)
      break;

    char frame[512] = {0};
    if (net_protocol_encode_commit_serial(reservation_id, qso_uid, frame,
                                          sizeof(frame)) != 0 ||
        net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                    frame) != 0)
      break;

    char response[2048] = {0};
    if (net_protocol_recv_framed_io_limited(&transport, net_transport_read_cb,
                                            response, sizeof(response),
                                            (size_t)config.net_max_frame_bytes) !=
        0)
      break;

    if (net_protocol_validate_protocol_version(response) != 0)
      break;

    NetMessageType response_type = NET_MSG_UNKNOWN;
    (void)net_protocol_detect_type(response, &response_type);
    if (response_is_error(response))
      rc = NET_SYNC_REMOTE_REJECTED;
    else if (response_type == NET_MSG_ACK &&
             net_protocol_validate_shared_log_id(
                 response, config.net_shared_log_id) == 0)
      rc = 0;
  } while (0);

  net_sync_close_transport(&transport);
  return rc;
}

static void net_sync_worker_service_serials(void) {
  if (!config.net_enabled || strcasecmp(config.net_role, "client") != 0)
    return;

  SyncSerialCommitEntry commits[4];
  int commit_count = 0;
  memset(commits, 0, sizeof(commits));
  if (db_sync_load_pending_serial_commits(commits, 4, &commit_count) == 0 &&
      commit_count > 0) {
    int commit_rc = net_sync_commit_serial_remote(
        commits[0].reservation_id, commits[0].qso_uid);
    if (commit_rc == 0)
      (void)db_sync_mark_serial_commit_acked(commits[0].reservation_id);
    else if (commit_rc == NET_SYNC_REMOTE_REJECTED)
      (void)db_sync_mark_serial_commit_failed(commits[0].reservation_id);
    return;
  }

  pthread_mutex_lock(&sync_mutex);
  int prefetch_enabled = sync_serial_prefetch_enabled;
  pthread_mutex_unlock(&sync_mutex);
  if (!prefetch_enabled)
    return;

  int logbook_id = 1;
  if (db_get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;
  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return;

  int available = 0;
  if (db_sync_count_available_serial_reservations(logbook_id, station_id,
                                                  &available) != 0 ||
      available >= 4)
    return;

  int serial = 0;
  char reservation_id[64] = {0};
  (void)net_sync_reserve_serial_remote_ex(&serial, reservation_id,
                                          sizeof(reservation_id));
}

static int net_sync_poll_once_impl(void) {
  pthread_mutex_lock(&sync_mutex);
  int running = sync_status.running;
  pthread_mutex_unlock(&sync_mutex);

  if (!running)
    return 0;

  if (!config.net_enabled)
    return 0;

  if (strcasecmp(config.net_role, "server") == 0) {
    int paired = net_sync_active_log_matches_pairing();
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = net_server_is_running() && paired ? 1 : 0;
    if (!paired)
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "active log shared_log_id changed; restart or pair the server");
    else
      sync_status.last_error[0] = 0;
    pthread_mutex_unlock(&sync_mutex);
    return 0;
  }

  if (strcasecmp(config.net_role, "client") != 0) {
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "unsupported NET_ROLE=%s", config.net_role);
    pthread_mutex_unlock(&sync_mutex);
    return 0;
  }

  if (!net_sync_active_log_matches_pairing()) {
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "active log is not paired with NET_SHARED_LOG_ID");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  int pending = 0;
  long long last_seq = 0;

  if (db_sync_get_pending_outbox_count(&pending) != 0)
    return -1;
  if (db_sync_get_last_global_seq(&last_seq) != 0)
    return -1;

  time_t now = time(NULL);
  if (sync_next_attempt_utc > now) {
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    sync_status.pending_outbox = pending;
    sync_status.last_pulled_global_seq = last_seq;
    pthread_mutex_unlock(&sync_mutex);
    return 0;
  }

  int sock = net_connect_tcp(config.net_server_host, config.net_server_port);
  if (sock < 0) {
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    sync_status.pending_outbox = pending;
    sync_status.last_pulled_global_seq = last_seq;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "connect failed: %.90s:%d", config.net_server_host,
             config.net_server_port);
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  NetTransport transport;
  char transport_error[128] = {0};
  if (net_transport_init_client(&transport, sock, config.net_server_host,
                                config.net_tls,
                                config.net_tls_peer_fingerprint,
                                transport_error,
                                sizeof(transport_error)) != 0) {
                  sync_clear_active_socket(sock);
                  close(sock);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    sync_status.pending_outbox = pending;
    sync_status.last_pulled_global_seq = last_seq;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "%.120s", transport_error[0] ? transport_error : "transport init failed");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  net_sync_set_socket_timeout(sock);

  char frame[8192];
  if (net_protocol_encode_hello(sync_status.station_id, "logger",
                                net_sync_auth_token(), frame,
                                sizeof(frame)) != 0 ||
      net_protocol_send_framed_io(&transport, net_transport_write_cb,
                  frame) != 0) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "send HELLO failed");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  char hello_ack[2048] = {0};
  if (net_protocol_recv_framed_io_limited(&transport, net_transport_read_cb,
                                          hello_ack, sizeof(hello_ack),
                                          (size_t)config.net_max_frame_bytes) !=
      0) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "recv HELLO_ACK failed");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  if (net_protocol_validate_protocol_version(hello_ack) != 0) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "ERROR_UNSUPPORTED_PROTOCOL");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  if (response_is_unsupported_protocol_error(hello_ack)) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "ERROR_UNSUPPORTED_PROTOCOL");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  int hello_accepted = 0;
  long long hello_next_expected_seq = 0;
  long long hello_server_seq = 0;
  char hello_shared_log_id[36] = {0};
  if (net_protocol_parse_hello_ack(hello_ack, &hello_accepted,
                                   &hello_next_expected_seq,
                                   &hello_server_seq, hello_shared_log_id,
                                   sizeof(hello_shared_log_id)) != 0 ||
      !hello_accepted ||
      strcmp(hello_shared_log_id, config.net_shared_log_id) != 0) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "HELLO rejected");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }
  (void)hello_next_expected_seq;
  (void)hello_server_seq;

  if (net_protocol_encode_catchup_request(last_seq, NET_SYNC_BATCH_MAX, frame,
                                          sizeof(frame)) != 0 ||
      net_protocol_send_framed_io(&transport, net_transport_write_cb,
                  frame) != 0) {
    net_sync_close_transport(&transport);
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.failure_streak = sync_failure_streak;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error),
             "send CATCHUP_REQUEST failed");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  SyncLogOpEntry pulled_ops[NET_SYNC_BATCH_MAX];
  char response[16384] = {0};
  char pull_error[128] = {0};
  int recv_ok = 0;
  int has_more = 0;
  for (;;) {
    int pulled_count = 0;
    long long parsed_last_seq = last_seq;
    memset(pulled_ops, 0, sizeof(pulled_ops));
    time_t pull_start_utc = time(NULL);

    if (net_protocol_recv_framed_io_limited(&transport, net_transport_read_cb,
                                            response, sizeof(response),
                                            (size_t)config.net_max_frame_bytes) !=
        0) {
      snprintf(pull_error, sizeof(pull_error), "CATCHUP response failed");
      break;
    }

    if (time(NULL) - pull_start_utc > (time_t)config.net_heartbeat_sec) {
      snprintf(pull_error, sizeof(pull_error),
               "CATCHUP response exceeded heartbeat");
      break;
    }

    if (net_protocol_validate_protocol_version(response) != 0) {
      snprintf(pull_error, sizeof(pull_error), "ERROR_UNSUPPORTED_PROTOCOL");
      break;
    }

    if (response_is_unsupported_protocol_error(response)) {
      snprintf(pull_error, sizeof(pull_error), "ERROR_UNSUPPORTED_PROTOCOL");
      break;
    }

    if (response_is_error(response)) {
      snprintf(pull_error, sizeof(pull_error), "CATCHUP request rejected");
      break;
    }
    if (net_protocol_validate_shared_log_id(response,
                                            config.net_shared_log_id) != 0) {
      snprintf(pull_error, sizeof(pull_error), "shared log ID mismatch");
      break;
    }

    NetMessageType mt = NET_MSG_UNKNOWN;
    (void)net_protocol_detect_type(response, &mt);
    if (mt == NET_MSG_OP_BROADCAST) {
      if (net_protocol_validate_shared_log_id(
          response, config.net_shared_log_id) != 0 ||
          apply_broadcast_frame(response) != 0) {
        snprintf(pull_error, sizeof(pull_error), "OP_BROADCAST apply failed");
        break;
      }
      continue;
    }

    if (net_protocol_parse_pull_ops_resp(response, pulled_ops,
                                         NET_SYNC_BATCH_MAX, &pulled_count,
                                         &parsed_last_seq, &has_more) != 0) {
      snprintf(pull_error, sizeof(pull_error), "Invalid CATCHUP response");
      break;
    }

    if (has_more && pulled_count == 0) {
      snprintf(pull_error, sizeof(pull_error),
               "CATCHUP page is empty but has_more is set");
      break;
    }

    long long page_last_seq = last_seq;
    for (int i = 0; i < pulled_count; i++) {
      long long applied_seq = 0;
      int local_logbook_id = 0;
      if (!net_sync_active_log_matches_pairing() ||
          db_get_current_logbook_id(&local_logbook_id) != 0 ||
          local_logbook_id <= 0) {
        snprintf(pull_error, sizeof(pull_error),
                 "active logbook changed during catch-up");
        break;
      }
      if (pulled_ops[i].global_seq <= page_last_seq) {
        snprintf(pull_error, sizeof(pull_error),
                 "CATCHUP page sequence is not increasing");
        break;
      }
      int apply_rc = db_sync_apply_remote_op(
          pulled_ops[i].op_id, pulled_ops[i].station_id,
          pulled_ops[i].station_seq, local_logbook_id,
          pulled_ops[i].op_type, pulled_ops[i].entity_id,
          pulled_ops[i].payload_json, pulled_ops[i].op_utc,
          &applied_seq);
      if (apply_rc < 0) {
        snprintf(pull_error, sizeof(pull_error), "CATCHUP apply failed");
        break;
      }
      page_last_seq = pulled_ops[i].global_seq;
    }

    if (pull_error[0])
      break;

    if (pulled_count > 0) {
      if (parsed_last_seq < page_last_seq ||
          db_sync_set_last_global_seq(page_last_seq) != 0) {
        snprintf(pull_error, sizeof(pull_error),
                 "CATCHUP cursor commit failed");
        break;
      }
      last_seq = page_last_seq;
    } else if (parsed_last_seq > last_seq) {
      snprintf(pull_error, sizeof(pull_error),
               "CATCHUP cursor advanced without operations");
      break;
    }

    if (has_more) {
      if (net_protocol_encode_catchup_request(last_seq, NET_SYNC_BATCH_MAX, frame,
                                              sizeof(frame)) != 0 ||
          net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                     frame) != 0) {
        snprintf(pull_error, sizeof(pull_error),
                 "send next CATCHUP_REQUEST failed");
        break;
      }
      continue;
    }

    recv_ok = 1;
    break;
  }

  if (!recv_ok) {
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);
    (void)db_sync_get_pending_outbox_count(&pending);
    net_sync_close_transport(&transport);

    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = 0;
    sync_status.tls_enabled = config.net_tls ? 1 : 0;
    sync_status.failure_streak = sync_failure_streak;
    sync_status.pending_outbox = pending;
    sync_status.last_pulled_global_seq = last_seq;
    snprintf(sync_status.last_error, sizeof(sync_status.last_error), "%s",
             pull_error[0] ? pull_error : "CATCHUP response failed");
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }

  SyncOutboxEntry pending_ops[NET_SYNC_BATCH_MAX];
  int ops_count = 0;
  memset(pending_ops, 0, sizeof(pending_ops));

  if (db_sync_outbox_load_pending(pending_ops, NET_SYNC_BATCH_MAX,
                                  &ops_count) != 0)
    ops_count = 0;

  int sent_count = 0;

  if (ops_count > 0) {
    if (net_protocol_encode_append_ops(pending_ops, ops_count, frame,
                                       sizeof(frame)) == 0 &&
      net_protocol_send_framed_io(&transport, net_transport_write_cb,
                frame) == 0) {
      for (int i = 0; i < ops_count; i++)
        (void)db_sync_outbox_mark_sent(pending_ops[i].op_id);

      sent_count = ops_count;
    } else {
      for (int i = 0; i < ops_count; i++)
        (void)db_sync_outbox_mark_retry(
            pending_ops[i].op_id,
            net_sync_retry_delay_for_entry(&pending_ops[i]));
    }
  } else if (config.net_heartbeat_sec > 0 &&
             (sync_last_heartbeat_utc == 0 ||
              now - sync_last_heartbeat_utc >= config.net_heartbeat_sec)) {
    if (net_protocol_encode_heartbeat(frame, sizeof(frame)) == 0) {
      (void)net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                        frame);
      sync_last_heartbeat_utc = now;
    }
  }

  memset(response, 0, sizeof(response));
  int got_terminal_ack = 0;
  char ack_error[128] = {0};
  for (int spin = 0; spin < NET_SYNC_PUSH_DRAIN_MAX; spin++) {
    if (net_protocol_recv_framed_io_limited(&transport, net_transport_read_cb,
                                            response, sizeof(response),
                                            (size_t)config.net_max_frame_bytes) !=
        0)
      break;

    if (net_protocol_validate_protocol_version(response) != 0) {
      recv_ok = 0;
      pthread_mutex_lock(&sync_mutex);
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "ERROR_UNSUPPORTED_PROTOCOL");
      pthread_mutex_unlock(&sync_mutex);
      break;
    }

    if (response_is_unsupported_protocol_error(response)) {
      recv_ok = 0;
      pthread_mutex_lock(&sync_mutex);
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "ERROR_UNSUPPORTED_PROTOCOL");
      pthread_mutex_unlock(&sync_mutex);
      break;
    }

    NetMessageType mt = NET_MSG_UNKNOWN;
    (void)net_protocol_detect_type(response, &mt);
    if (mt == NET_MSG_OP_BROADCAST) {
      if (apply_broadcast_frame(response) != 0) {
        recv_ok = 0;
        snprintf(ack_error, sizeof(ack_error), "OP_BROADCAST apply failed");
        break;
      }
      continue;
    }

    if (response_is_error(response) ||
        (mt != NET_MSG_APPEND_ACK && mt != NET_MSG_ACK)) {
      recv_ok = 0;
      snprintf(ack_error, sizeof(ack_error), "invalid APPEND acknowledgement");
      break;
    }
    if (net_protocol_validate_shared_log_id(response,
                                            config.net_shared_log_id) != 0) {
      recv_ok = 0;
      snprintf(ack_error, sizeof(ack_error), "shared log ID mismatch");
      break;
    }

    recv_ok = 1;
    got_terminal_ack = 1;
    (void)apply_acked_op_ids(response);
    if (strstr(response, "\"code\":\"SEQ_GAP\""))
      snprintf(ack_error, sizeof(ack_error),
               "station sequence gap; unaccepted operations will retry");
    else if (strstr(response, "\"code\":\"STATION_SEQ_CONFLICT\""))
      snprintf(ack_error, sizeof(ack_error),
               "station sequence conflict; operation remains queued");
    else if (strstr(response, "\"code\":\"QSO_UID_CONFLICT\""))
      snprintf(ack_error, sizeof(ack_error),
               "QSO UID conflict; operation remains queued");
    else if (strstr(response, "\"rejected_ops\":[{"))
      snprintf(ack_error, sizeof(ack_error),
               "server rejected one or more operations");
    break;
  }

  if (sent_count > 0 && !got_terminal_ack)
    recv_ok = 0;

  if (sent_count > 0) {
    for (int i = 0; i < sent_count; i++) {
      if (!recv_ok || !ack_contains_op_id(response, pending_ops[i].op_id))
        (void)db_sync_outbox_mark_retry(
            pending_ops[i].op_id,
            net_sync_retry_delay_for_entry(&pending_ops[i]));
    }
  }

  if (!recv_ok) {
    sync_failure_streak++;
    sync_next_attempt_utc = now + net_sync_backoff_seconds(sync_failure_streak);
  } else {
    if (sync_failure_streak > 0)
      sync_reconnect_count++;
    sync_failure_streak = 0;
    sync_next_attempt_utc = 0;
    sync_last_heartbeat_utc = now;
  }

  (void)db_sync_get_pending_outbox_count(&pending);

  net_sync_close_transport(&transport);

  pthread_mutex_lock(&sync_mutex);
  sync_status.connected = recv_ok ? 1 : 0;
  sync_status.tls_enabled = config.net_tls ? 1 : 0;
  sync_status.reconnect_count = sync_reconnect_count;
  sync_status.failure_streak = sync_failure_streak;
  sync_status.pending_outbox = pending;
  sync_status.last_pulled_global_seq = last_seq;
  net_sync_format_utc(recv_ok ? now : 0, sync_status.last_success_utc,
                      sizeof(sync_status.last_success_utc));
  net_sync_format_utc(sync_last_heartbeat_utc, sync_status.last_heartbeat_utc,
                      sizeof(sync_status.last_heartbeat_utc));
  if (ack_error[0])
    snprintf(sync_status.last_error, sizeof(sync_status.last_error), "%s",
             ack_error);
  else if (recv_ok && sent_count > 0)
    sync_status.last_error[0] = 0;
  pthread_mutex_unlock(&sync_mutex);

  return 0;
}

int net_sync_request_poll(void) {
  pthread_mutex_lock(&sync_mutex);
  if (!sync_status.running || !sync_worker_created ||
      strcasecmp(config.net_role, "client") != 0) {
    pthread_mutex_unlock(&sync_mutex);
    return -1;
  }
  sync_poll_request_generation++;
  sync_poll_requested = 1;
  pthread_cond_signal(&sync_cond);
  pthread_mutex_unlock(&sync_mutex);
  return 0;
}

int net_sync_poll_once(void) {
  pthread_mutex_lock(&sync_mutex);
  int running = sync_status.running;
  int worker_created = sync_worker_created;
  int called_from_worker = worker_created &&
                           pthread_equal(pthread_self(), sync_worker_thread);
  if (worker_created && running && !called_from_worker) {
    unsigned long request_generation = ++sync_poll_request_generation;
    sync_poll_requested = 1;
    pthread_cond_signal(&sync_cond);
    while (sync_status.running &&
           sync_poll_completed_generation < request_generation)
      pthread_cond_wait(&sync_cond, &sync_mutex);
    int poll_rc = sync_status.running ? sync_worker_last_poll_rc : -1;
    pthread_mutex_unlock(&sync_mutex);
    return poll_rc;
  }
  pthread_mutex_unlock(&sync_mutex);
  return net_sync_poll_once_impl();
}

void net_sync_get_status(NetSyncStatus *out) {
  if (!out)
    return;

  pthread_mutex_lock(&sync_mutex);
  *out = sync_status;
  pthread_mutex_unlock(&sync_mutex);
}
