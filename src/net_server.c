#include "net_server.h"

#include "config.h"
#include "db.h"
#include "net_protocol.h"
#include "net_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NET_SERVER_MAX_OPS 64
#define NET_SERVER_MAX_SESSIONS 16
#define NET_SERVER_SESSION_IDLE_SEC 60

typedef struct {
  int in_use;
  int authenticated;
  int client_fd;
  time_t last_activity_utc;
  NetTransport transport;
  pthread_t thread;
  pthread_mutex_t write_mutex;
  char station_id[32];
} NetServerSession;

static pthread_t net_server_thread;
static int net_server_running = 0;
static int net_server_stop_flag = 0;
static int net_server_listen_fd = -1;

static NetServerSession net_server_sessions[NET_SERVER_MAX_SESSIONS];
static pthread_mutex_t net_server_sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

static int session_rate_limit_exceeded(time_t *window_started,
                                       int *window_count) {
  if (!window_started || !window_count)
    return 0;

  time_t now = time(NULL);
  int window_sec = config.net_rate_limit_window_sec;
  int burst = config.net_rate_limit_burst;
  if (window_sec < 1)
    window_sec = 1;
  if (burst < 1)
    burst = 1;

  if (*window_started == 0 || now - *window_started >= window_sec) {
    *window_started = now;
    *window_count = 0;
  }

  (*window_count)++;
  return *window_count > burst;
}

static int send_session_frame(NetServerSession *session, const char *data) {
  if (!session || !data)
    return -1;

  pthread_mutex_lock(&session->write_mutex);
  int rc = net_protocol_send_framed_io(&session->transport, net_transport_write_cb,
                                       data);
  pthread_mutex_unlock(&session->write_mutex);
  return rc;
}

static int parse_number_after_key(const char *text, const char *key,
                                  long long *out) {
  if (!text || !key || !out)
    return -1;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(text, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  char *endptr = NULL;
  long long value = strtoll(p, &endptr, 10);
  if (endptr == p)
    return -1;

  *out = value;
  return 0;
}

static int send_unsupported_protocol(NetServerSession *session) {
  return send_session_frame(session,
                            "{\"type\":\"ERROR\",\"code\":\"ERROR_UNSUPPORTED_PROTOCOL\",\"message\":\"protocol_version mismatch\"}");
}

static void broadcast_applied_op(const NetServerSession *source,
                                 const SyncLogOpEntry *op) {
  if (!op || !op->op_id[0] || !op->station_id[0])
    return;

  char frame[8192] = {0};
  if (net_protocol_encode_op_broadcast(op, frame, sizeof(frame)) != 0)
    return;

  pthread_mutex_lock(&net_server_sessions_mutex);
  for (int i = 0; i < NET_SERVER_MAX_SESSIONS; i++) {
    NetServerSession *target = &net_server_sessions[i];
    if (!target->in_use || !target->authenticated)
      continue;
    if (source && target == source)
      continue;

    (void)send_session_frame(target, frame);
  }
  pthread_mutex_unlock(&net_server_sessions_mutex);
}

static void handle_append_ops(NetServerSession *session, const char *frame) {
  NetAppendOp ops[NET_SERVER_MAX_OPS];
  int op_count = 0;
  memset(ops, 0, sizeof(ops));

  if (net_protocol_parse_append_ops(frame, ops, NET_SERVER_MAX_OPS,
                                    &op_count) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"BAD_APPEND\"}");
    return;
  }

  long long last_global_seq = 0;
  long long last_acked_station_seq = 0;
  long long expected_seq = 0;
  if (db_sync_get_next_expected_station_seq(session->station_id, &expected_seq) !=
      0)
    expected_seq = 1;

  char accepted_json[2048] = {0};
  size_t accepted_used = 0;
  int accepted_count = 0;
  char rejected_json[512] = {0};
  size_t rejected_used = 0;
  int rejected_count = 0;

  accepted_used = (size_t)snprintf(accepted_json, sizeof(accepted_json), "[");
  rejected_used = (size_t)snprintf(rejected_json, sizeof(rejected_json), "[");

  for (int i = 0; i < op_count; i++) {
    if (ops[i].station_seq > expected_seq) {
      int n = snprintf(rejected_json + rejected_used,
                       sizeof(rejected_json) - rejected_used,
                       "%s{\"op_id\":\"%s\",\"code\":\"SEQ_GAP\"}",
                       rejected_count == 0 ? "" : ",", ops[i].op_id);
      if (n > 0 && (size_t)n < sizeof(rejected_json) - rejected_used) {
        rejected_used += (size_t)n;
        rejected_count++;
      }
      continue;
    }

    long long applied_seq = 0;
    int rc = db_sync_apply_remote_op(ops[i].op_id, session->station_id,
                                     ops[i].station_seq, ops[i].logbook_id,
                                     ops[i].op_type, ops[i].entity_id,
                                     ops[i].payload_json, ops[i].op_utc,
                                     &applied_seq);
    if (rc == DB_SYNC_APPLY_STATION_SEQ_CONFLICT) {
      int n = snprintf(rejected_json + rejected_used,
                       sizeof(rejected_json) - rejected_used,
                       "%s{\"op_id\":\"%s\",\"code\":\"STATION_SEQ_CONFLICT\"}",
                       rejected_count == 0 ? "" : ",", ops[i].op_id);
      if (n > 0 && (size_t)n < sizeof(rejected_json) - rejected_used) {
        rejected_used += (size_t)n;
        rejected_count++;
      }
      continue;
    }

    if (rc == DB_SYNC_APPLY_QSO_UID_CONFLICT) {
      int n = snprintf(rejected_json + rejected_used,
                       sizeof(rejected_json) - rejected_used,
                       "%s{\"op_id\":\"%s\",\"code\":\"QSO_UID_CONFLICT\"}",
                       rejected_count == 0 ? "" : ",", ops[i].op_id);
      if (n > 0 && (size_t)n < sizeof(rejected_json) - rejected_used) {
        rejected_used += (size_t)n;
        rejected_count++;
      }
      continue;
    }

    if (rc < 0) {
      (void)send_session_frame(session,
                               "{\"type\":\"ERROR\",\"code\":\"APPLY_FAILED\"}");
      return;
    }

    int n = snprintf(accepted_json + accepted_used,
                     sizeof(accepted_json) - accepted_used, "%s\"%s\"",
                     accepted_count == 0 ? "" : ",", ops[i].op_id);
    if (n > 0 && (size_t)n < sizeof(accepted_json) - accepted_used) {
      accepted_used += (size_t)n;
      accepted_count++;
    }

    if (ops[i].station_seq >= expected_seq)
      expected_seq = ops[i].station_seq + 1;
    if (ops[i].station_seq > last_acked_station_seq)
      last_acked_station_seq = ops[i].station_seq;
    if (applied_seq > last_global_seq)
      last_global_seq = applied_seq;

    if (rc == DB_SYNC_APPLY_CHANGED && applied_seq > 0) {
      SyncLogOpEntry bcast;
      memset(&bcast, 0, sizeof(bcast));
      bcast.global_seq = applied_seq;
      bcast.station_seq = ops[i].station_seq;
      bcast.logbook_id = ops[i].logbook_id;
      snprintf(bcast.op_id, sizeof(bcast.op_id), "%s", ops[i].op_id);
      snprintf(bcast.station_id, sizeof(bcast.station_id), "%s",
               session->station_id);
      snprintf(bcast.op_type, sizeof(bcast.op_type), "%s", ops[i].op_type);
      snprintf(bcast.entity_id, sizeof(bcast.entity_id), "%s", ops[i].entity_id);
      snprintf(bcast.payload_json, sizeof(bcast.payload_json), "%s",
               ops[i].payload_json);
      snprintf(bcast.op_utc, sizeof(bcast.op_utc), "%s", ops[i].op_utc);
      broadcast_applied_op(session, &bcast);
    }
  }

  if (accepted_used < sizeof(accepted_json) - 2)
    accepted_used += (size_t)snprintf(accepted_json + accepted_used,
                                      sizeof(accepted_json) - accepted_used,
                                      "]");
  if (rejected_used < sizeof(rejected_json) - 2)
    rejected_used += (size_t)snprintf(rejected_json + rejected_used,
                                      sizeof(rejected_json) - rejected_used,
                                      "]");

  char ack[4096] = {0};
  if (net_protocol_encode_append_ack(accepted_json, rejected_json,
                                     last_acked_station_seq, last_global_seq,
                                     ack, sizeof(ack)) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"ACK_BUILD_FAILED\"}");
    return;
  }
  (void)send_session_frame(session, ack);
}

static void handle_pull_ops(NetServerSession *session, const char *frame,
                            int as_catchup_batch) {
  long long from_seq = 0;
  long long limit_ll = 200;

  (void)parse_number_after_key(frame, "from_global_seq", &from_seq);
  (void)parse_number_after_key(frame, "from_global_seq_exclusive", &from_seq);
  (void)parse_number_after_key(frame, "limit", &limit_ll);
  (void)parse_number_after_key(frame, "max_batch_size", &limit_ll);

  if (from_seq < 0)
    from_seq = 0;

  int limit = (int)limit_ll;
  if (limit < 1)
    limit = 1;
  if (limit > NET_SERVER_MAX_OPS)
    limit = NET_SERVER_MAX_OPS;

  SyncLogOpEntry ops[NET_SERVER_MAX_OPS];
  int count = 0;
  long long last_seq = from_seq;
  memset(ops, 0, sizeof(ops));

  if (db_sync_pull_ops(from_seq, limit, ops, NET_SERVER_MAX_OPS, &count,
                       &last_seq) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"PULL_FAILED\"}");
    return;
  }

  char resp[16384] = {0};
  int enc_rc = as_catchup_batch
                   ? net_protocol_encode_catchup_batch(ops, count, last_seq, 0,
                                                       resp, sizeof(resp))
                   : net_protocol_encode_pull_ops_resp(ops, count, last_seq, 0,
                                                       resp, sizeof(resp));
  if (enc_rc != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"RESP_BUILD_FAILED\"}");
    return;
  }

  (void)send_session_frame(session, resp);
}

static void handle_reserve_serial(NetServerSession *session, const char *frame) {
  char request_id[64] = {0};
  int logbook_id = 0;
  int ttl_sec = 120;
  if (net_protocol_parse_reserve_serial(frame, request_id, sizeof(request_id),
                                        &logbook_id, &ttl_sec) != 0 ||
      logbook_id <= 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"BAD_RESERVE\"}");
    return;
  }

  char reservation_id[64] = {0};
  int serial = 0;
  char expires_utc[32] = {0};
  if (db_sync_reserve_serial(logbook_id, session->station_id, request_id,
                             ttl_sec, reservation_id,
                             sizeof(reservation_id), &serial, expires_utc,
                             sizeof(expires_utc)) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"RESERVE_FAILED\"}");
    return;
  }

  char resp[512] = {0};
  if (net_protocol_encode_reserve_serial_ack(request_id, reservation_id, serial,
                                             expires_utc, resp,
                                             sizeof(resp)) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"RESP_BUILD_FAILED\"}");
    return;
  }

  (void)send_session_frame(session, resp);
}

static void handle_commit_serial(NetServerSession *session, const char *frame) {
  char reservation_id[64] = {0};
  char qso_uid[40] = {0};

  if (net_protocol_parse_commit_serial(frame, reservation_id,
                                       sizeof(reservation_id), qso_uid,
                                       sizeof(qso_uid)) != 0) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"BAD_COMMIT\"}");
    return;
  }

  int rc = db_sync_commit_serial(reservation_id, qso_uid);
  if (rc == DB_SYNC_COMMIT_NOT_FOUND) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"COMMIT_NOT_FOUND_OR_EXPIRED\"}");
    return;
  }

  if (rc != DB_SYNC_COMMIT_OK) {
    (void)send_session_frame(session,
                             "{\"type\":\"ERROR\",\"code\":\"COMMIT_FAILED\"}");
    return;
  }

  char ack[256] = {0};
  if (net_protocol_encode_ack_empty(0, ack, sizeof(ack)) == 0)
    (void)send_session_frame(session, ack);
}

static void release_session_slot(NetServerSession *session) {
  if (!session)
    return;

  pthread_mutex_lock(&net_server_sessions_mutex);
  session->authenticated = 0;
  session->station_id[0] = 0;
  session->client_fd = -1;
  session->last_activity_utc = 0;
  session->in_use = 0;
  pthread_mutex_unlock(&net_server_sessions_mutex);
}

static void *net_server_client_worker(void *arg) {
  NetServerSession *session = (NetServerSession *)arg;
  if (!session)
    return NULL;

  time_t rate_window_started = 0;
  int rate_window_count = 0;

  size_t max_frame_size = (size_t)config.net_max_frame_bytes;
  if (max_frame_size < 1024)
    max_frame_size = 1024;
  if (max_frame_size > 16383)
    max_frame_size = 16383;

  for (;;) {
    if (net_server_stop_flag)
      break;

    if (time(NULL) - session->last_activity_utc >= NET_SERVER_SESSION_IDLE_SEC) {
      break;
    }

    char frame[16384] = {0};
    if (net_protocol_recv_framed_io_limited(&session->transport,
                                            net_transport_read_cb, frame,
                                            sizeof(frame),
                                            max_frame_size) != 0)
      break;

    session->last_activity_utc = time(NULL);

    if (net_protocol_validate_protocol_version(frame) != 0) {
      (void)send_unsupported_protocol(session);
      break;
    }

    NetMessageType mt = NET_MSG_UNKNOWN;
    (void)net_protocol_detect_type(frame, &mt);

    if (mt == NET_MSG_HELLO) {
      NetSessionMeta meta;
      memset(&meta, 0, sizeof(meta));
      int accepted = 0;
      if (net_protocol_parse_hello_meta(frame, &meta) == 0 &&
          meta.station_id[0]) {
        if (!config.net_auth_token[0] ||
            strcmp(config.net_auth_token, meta.auth_token) == 0) {
          accepted = 1;
          pthread_mutex_lock(&net_server_sessions_mutex);
          snprintf(session->station_id, sizeof(session->station_id), "%s",
                   meta.station_id);
          session->authenticated = 1;
          pthread_mutex_unlock(&net_server_sessions_mutex);
        }
      }

      long long next_expected = 1;
      long long max_global = 0;
      if (accepted) {
        (void)db_sync_get_next_expected_station_seq(session->station_id,
                                                    &next_expected);
        (void)db_sync_get_max_global_seq(&max_global);
      }

      char hello_ack[512] = {0};
      if (net_protocol_encode_hello_ack(accepted, next_expected, max_global,
                                        hello_ack, sizeof(hello_ack)) == 0)
        (void)send_session_frame(session, hello_ack);

      if (!accepted)
        break;
      continue;
    }

    if (!session->authenticated) {
      (void)send_session_frame(
          session,
          "{\"type\":\"ERROR\",\"code\":\"NOT_AUTHENTICATED\"}");
      break;
    }

    if (session_rate_limit_exceeded(&rate_window_started, &rate_window_count)) {
      (void)send_session_frame(session,
                               "{\"type\":\"ERROR\",\"code\":\"RATE_LIMIT\"}");
      break;
    }

    if (mt == NET_MSG_APPEND_OPS) {
      handle_append_ops(session, frame);
    } else if (mt == NET_MSG_PULL_OPS) {
      handle_pull_ops(session, frame, 0);
    } else if (mt == NET_MSG_CATCHUP_REQUEST) {
      handle_pull_ops(session, frame, 1);
    } else if (mt == NET_MSG_RESERVE_SERIAL) {
      handle_reserve_serial(session, frame);
    } else if (mt == NET_MSG_COMMIT_SERIAL) {
      handle_commit_serial(session, frame);
    } else if (mt == NET_MSG_HEARTBEAT) {
      char ack[256] = {0};
      if (net_protocol_encode_ack_empty(0, ack, sizeof(ack)) == 0)
        (void)send_session_frame(session, ack);
    } else {
      (void)send_session_frame(session,
                               "{\"type\":\"ERROR\",\"code\":\"UNSUPPORTED\"}");
    }
  }

  net_transport_close(&session->transport);
  release_session_slot(session);
  return NULL;
}

static int allocate_session_slot(NetServerSession **out_session) {
  if (!out_session)
    return -1;

  *out_session = NULL;

  pthread_mutex_lock(&net_server_sessions_mutex);
  for (int i = 0; i < NET_SERVER_MAX_SESSIONS; i++) {
    if (!net_server_sessions[i].in_use) {
      net_server_sessions[i].in_use = 1;
      net_server_sessions[i].authenticated = 0;
      net_server_sessions[i].station_id[0] = 0;
      net_server_sessions[i].client_fd = -1;
      net_server_sessions[i].last_activity_utc = time(NULL);
      *out_session = &net_server_sessions[i];
      break;
    }
  }
  pthread_mutex_unlock(&net_server_sessions_mutex);

  return *out_session ? 0 : -1;
}

static void *net_server_worker(void *arg) {
  (void)arg;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    return NULL;

  net_server_listen_fd = srv;

  int reuse = 1;
  (void)setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(srv);
    net_server_listen_fd = -1;
    return NULL;
  }

  if (listen(srv, 8) != 0) {
    close(srv);
    net_server_listen_fd = -1;
    return NULL;
  }

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;

  while (!net_server_stop_flag) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(srv, &fds);

    int rc = select(srv + 1, &fds, NULL, NULL, &tv);
    if (rc <= 0)
      continue;

    int cli = accept(srv, NULL, NULL);
    if (cli < 0)
      continue;

    NetTransport transport;
    char transport_error[128] = {0};
    if (net_transport_init_server(&transport, cli, config.net_tls,
                                  config.net_tls_cert_file,
                                  config.net_tls_key_file, transport_error,
                                  sizeof(transport_error)) != 0) {
      close(cli);
      continue;
    }

    NetServerSession *session = NULL;
    if (allocate_session_slot(&session) != 0 || !session) {
      net_transport_close(&transport);
      continue;
    }

    session->transport = transport;
    session->client_fd = cli;

    if (pthread_create(&session->thread, NULL, net_server_client_worker,
                       session) != 0) {
      net_transport_close(&session->transport);
      release_session_slot(session);
      continue;
    }

    (void)pthread_detach(session->thread);
  }

  close(srv);
  net_server_listen_fd = -1;
  return NULL;
}

int net_server_start(void) {
  if (net_server_running)
    return 0;

  net_server_stop_flag = 0;
  for (int i = 0; i < NET_SERVER_MAX_SESSIONS; i++) {
    net_server_sessions[i].in_use = 0;
    net_server_sessions[i].authenticated = 0;
    net_server_sessions[i].client_fd = -1;
    net_server_sessions[i].last_activity_utc = 0;
    net_server_sessions[i].station_id[0] = 0;
    (void)pthread_mutex_init(&net_server_sessions[i].write_mutex, NULL);
  }

  if (pthread_create(&net_server_thread, NULL, net_server_worker, NULL) != 0)
    return -1;

  net_server_running = 1;
  return 0;
}

void net_server_stop(void) {
  if (!net_server_running)
    return;

  net_server_stop_flag = 1;

  if (net_server_listen_fd >= 0) {
    close(net_server_listen_fd);
    net_server_listen_fd = -1;
  }

  pthread_mutex_lock(&net_server_sessions_mutex);
  for (int i = 0; i < NET_SERVER_MAX_SESSIONS; i++) {
    if (net_server_sessions[i].in_use) {
      net_server_sessions[i].last_activity_utc = time(NULL);
      net_transport_close(&net_server_sessions[i].transport);
    }
  }
  pthread_mutex_unlock(&net_server_sessions_mutex);

  pthread_join(net_server_thread, NULL);
  net_server_running = 0;
}

int net_server_is_running(void) { return net_server_running ? 1 : 0; }
