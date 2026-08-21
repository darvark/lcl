#include "net_sync.h"

#include "config.h"
#include "db.h"
#include "net_protocol.h"
#include "net_server.h"
#include "net_tls.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
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
static NetSyncStatus sync_status;

#define NET_SYNC_BATCH_MAX 16
#define NET_SYNC_PUSH_DRAIN_MAX 8

static int sync_failure_streak = 0;
static time_t sync_next_attempt_utc = 0;
static time_t sync_last_heartbeat_utc = 0;
static int sync_reconnect_count = 0;
static unsigned int sync_rng_state = 0;

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
  long long hello_server_seq = 0;
  if (net_protocol_parse_hello_ack(hello_ack, &hello_accepted,
                                   &hello_server_seq) != 0 ||
      !hello_accepted) {
    if (out_error && out_error_size > 1)
      snprintf(out_error, out_error_size, "HELLO rejected");
    return -1;
  }

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

    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
      break;

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  return fd;
}

/*
 * Parse a server response and extract last_global_seq if present.
 *
 * @param text Server frame text.
 * @param out_seq Destination for parsed sequence.
 * @return 1 when parsed, 0 when not present, or -1 on invalid args.
 */
static int parse_last_global_seq(const char *text, long long *out_seq) {
  if (!text || !out_seq)
    return -1;

  const char *key = strstr(text, "\"last_global_seq\":");
  if (!key)
    key = strstr(text, "\"server_global_seq\":");
  if (!key)
    return 0;

  if (strncmp(key, "\"last_global_seq\":", strlen("\"last_global_seq\":")) == 0)
    key += strlen("\"last_global_seq\":");
  else
    key += strlen("\"server_global_seq\":");
  while (*key == ' ' || *key == '\t')
    key++;

  char *endptr = NULL;
  long long value = strtoll(key, &endptr, 10);
  if (endptr == key || value < 0)
    return 0;

  *out_seq = value;
  return 1;
}

static int apply_broadcast_frame(const char *frame, long long *inout_last_seq) {
  if (!frame || !inout_last_seq)
    return -1;

  SyncLogOpEntry op;
  memset(&op, 0, sizeof(op));
  if (net_protocol_parse_op_broadcast(frame, &op) != 0)
    return -1;

  long long applied_seq = 0;
  int rc = db_sync_apply_remote_op(op.op_id, op.station_id, op.station_seq,
                                   op.logbook_id, op.op_type, op.entity_id,
                                   op.payload_json, op.op_utc, &applied_seq);
  if (rc < 0)
    return -1;

  if (applied_seq > *inout_last_seq)
    *inout_last_seq = applied_seq;
  if (op.global_seq > *inout_last_seq)
    *inout_last_seq = op.global_seq;

  return 0;
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

  char needle[128] = {0};
  snprintf(needle, sizeof(needle), "\"%s\"", op_id);
  return strstr(acked, needle) != NULL;
}

int net_sync_start(void) {
  if (db_init() != 0)
    return -1;

  pthread_mutex_lock(&sync_mutex);
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
  db_sync_get_last_global_seq(&sync_status.last_pulled_global_seq);

  if (strcasecmp(config.net_role, "server") == 0) {
    if (net_server_start() != 0) {
      snprintf(sync_status.last_error, sizeof(sync_status.last_error),
               "net server start failed");
      pthread_mutex_unlock(&sync_mutex);
      return -1;
    }
    sync_status.connected = 1;
  }
  pthread_mutex_unlock(&sync_mutex);

  return 0;
}

void net_sync_stop(void) {
  pthread_mutex_lock(&sync_mutex);
  sync_status.running = 0;
  sync_status.connected = 0;
  pthread_mutex_unlock(&sync_mutex);

  net_server_stop();
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

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  int logbook_id = 1;
  if (db_get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;

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
    close(sock);
    return -1;
  }

  int rc = -1;
  do {
    char hello_error[64] = {0};
    if (net_sync_send_hello_and_expect_ack(&transport, station_id,
                                           hello_error,
                                           sizeof(hello_error)) != 0)
      break;

    char frame[512] = {0};
    char req_id[32] = {0};
    snprintf(req_id, sizeof(req_id), "req-%lld", (long long)time(NULL));

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

    char ack_req_id[32] = {0};
    int serial = 0;
    char expires_utc[32] = {0};
    if (net_protocol_parse_reserve_serial_ack(
            response, ack_req_id, sizeof(ack_req_id), out_reservation_id,
            out_reservation_id_size, &serial, expires_utc,
            sizeof(expires_utc)) != 0 ||
        serial <= 0 || strcmp(ack_req_id, req_id) != 0)
      break;

    *out_serial = serial;
    rc = 0;
  } while (0);

  net_transport_close(&transport);
  return rc;
}

int net_sync_reserve_serial_remote(int *out_serial) {
  char reservation_id[64] = {0};
  return net_sync_reserve_serial_remote_ex(out_serial, reservation_id,
                                           sizeof(reservation_id));
}

int net_sync_commit_serial_remote(const char *reservation_id,
                                  const char *qso_uid) {
  if (!reservation_id || !reservation_id[0])
    return -1;

  if (!config.net_enabled || strcasecmp(config.net_role, "client") != 0)
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
    close(sock);
    return -1;
  }

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

    rc = response_is_error(response) ? -1 : 0;
  } while (0);

  net_transport_close(&transport);
  return rc;
}

int net_sync_poll_once(void) {
  pthread_mutex_lock(&sync_mutex);
  int running = sync_status.running;
  pthread_mutex_unlock(&sync_mutex);

  if (!running)
    return 0;

  if (!config.net_enabled)
    return 0;

  if (strcasecmp(config.net_role, "server") == 0) {
    pthread_mutex_lock(&sync_mutex);
    sync_status.connected = net_server_is_running() ? 1 : 0;
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

  if (config.net_tls && !config.net_tls_peer_fingerprint[0]) {
    const char *peer_fp = net_transport_peer_fingerprint(&transport);
    if (peer_fp && peer_fp[0]) {
      snprintf(config.net_tls_peer_fingerprint,
               sizeof(config.net_tls_peer_fingerprint), "%s", peer_fp);
      (void)config_save_active();
    }
  }

  struct timeval tv;
  tv.tv_sec = config.net_heartbeat_sec > 1 ? config.net_heartbeat_sec : 1;
  tv.tv_usec = 0;
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char frame[8192];
  if (net_protocol_encode_hello(sync_status.station_id, "logger",
                                net_sync_auth_token(), frame,
                                sizeof(frame)) != 0 ||
      net_protocol_send_framed_io(&transport, net_transport_write_cb,
                  frame) != 0) {
    net_transport_close(&transport);
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
    net_transport_close(&transport);
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
    net_transport_close(&transport);
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
    net_transport_close(&transport);
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
  long long hello_server_seq = 0;
  if (net_protocol_parse_hello_ack(hello_ack, &hello_accepted,
                                   &hello_server_seq) != 0 ||
      !hello_accepted) {
    net_transport_close(&transport);
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

  if (hello_server_seq > last_seq)
    last_seq = hello_server_seq;

  if (net_protocol_encode_catchup_request(last_seq, 200, frame,
                                          sizeof(frame)) != 0 ||
      net_protocol_send_framed_io(&transport, net_transport_write_cb,
                  frame) != 0) {
    net_transport_close(&transport);
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
  int pulled_count = 0;
  memset(pulled_ops, 0, sizeof(pulled_ops));

  char response[16384] = {0};
  int recv_ok = 0;
  for (;;) {
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
      if (apply_broadcast_frame(response, &last_seq) == 0)
        (void)db_sync_set_last_global_seq(last_seq);
      continue;
    }

    recv_ok = 1;

    long long parsed_last_seq = last_seq;
    if (net_protocol_parse_pull_ops_resp(response, pulled_ops,
                                         NET_SYNC_BATCH_MAX, &pulled_count,
                                         &parsed_last_seq) == 0) {
      long long max_global_seq = parsed_last_seq;
      for (int i = 0; i < pulled_count; i++) {
        long long applied_seq = 0;
        int apply_rc = db_sync_apply_remote_op(
            pulled_ops[i].op_id, pulled_ops[i].station_id,
            pulled_ops[i].station_seq, pulled_ops[i].logbook_id,
            pulled_ops[i].op_type, pulled_ops[i].entity_id,
            pulled_ops[i].payload_json, pulled_ops[i].op_utc, &applied_seq);
        if (apply_rc >= 0 && applied_seq > max_global_seq)
          max_global_seq = applied_seq;
      }

      if (max_global_seq > last_seq) {
        (void)db_sync_set_last_global_seq(max_global_seq);
        last_seq = max_global_seq;
      }
    }

    break;
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
      if (apply_broadcast_frame(response, &last_seq) == 0)
        (void)db_sync_set_last_global_seq(last_seq);
      continue;
    }

    recv_ok = 1;
    got_terminal_ack = 1;
    (void)apply_acked_op_ids(response);

    long long parsed_seq = 0;
    int parsed = parse_last_global_seq(response, &parsed_seq);
    if (parsed > 0) {
      (void)db_sync_set_last_global_seq(parsed_seq);
      last_seq = parsed_seq;
    }
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

  net_transport_close(&transport);

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
  if (recv_ok)
    sync_status.last_error[0] = 0;
  pthread_mutex_unlock(&sync_mutex);

  return 0;
}

void net_sync_get_status(NetSyncStatus *out) {
  if (!out)
    return;

  pthread_mutex_lock(&sync_mutex);
  *out = sync_status;
  pthread_mutex_unlock(&sync_mutex);
}
