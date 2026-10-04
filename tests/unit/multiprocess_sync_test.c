#include "config.h"
#include "db.h"
#include "net_protocol.h"
#include "net_sync.h"
#include "qso.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MP_AUTH_TOKEN "MultiProc-Logger-Strong-Secret-2026!"
#define MP_CLIENT_COUNT 2

typedef struct {
  char code;
  int phase;
  long long target_seq;
} MpCommand;

typedef struct {
  int rc;
  int pending;
  int failed;
  int serial;
  long long cursor;
  char reservation_id[64];
  char qso_uid[40];
  char last_error[128];
} MpReply;

typedef struct {
  pid_t pid;
  int to_child;
  int from_child;
  char db_path[512];
} MpClient;

static int failures = 0;

static void expect_true(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "[FAIL] %s\n", message);
    failures++;
  }
}

static void expect_eq(int actual, int expected, const char *message) {
  if (actual != expected) {
    fprintf(stderr, "[FAIL] %s (actual=%d expected=%d)\n", message, actual,
            expected);
    failures++;
  }
}

static int read_exact(int fd, void *buffer, size_t size) {
  char *bytes = (char *)buffer;
  size_t offset = 0;
  while (offset < size) {
    ssize_t count = read(fd, bytes + offset, size - offset);
    if (count <= 0)
      return -1;
    offset += (size_t)count;
  }
  return 0;
}

static int write_exact(int fd, const void *buffer, size_t size) {
  const char *bytes = (const char *)buffer;
  size_t offset = 0;
  while (offset < size) {
    ssize_t count = write(fd, bytes + offset, size - offset);
    if (count <= 0)
      return -1;
    offset += (size_t)count;
  }
  return 0;
}

static void pause_100ms(void) {
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
  nanosleep(&delay, NULL);
}

static int set_database_path(const char *path) {
  db_shutdown();
  return setenv("LOGGER_DB_PATH", path, 1);
}

static int set_process_current_logbook(const char *db_path, int logbook_id) {
  if (!db_path || !db_path[0] || logbook_id <= 0)
    return -1;
  sqlite3 *sqlite_db = NULL;
  if (sqlite3_open(db_path, &sqlite_db) != SQLITE_OK) {
    if (sqlite_db)
      sqlite3_close(sqlite_db);
    return -1;
  }
  char sql[256] = {0};
  snprintf(sql, sizeof(sql),
           "INSERT OR IGNORE INTO named_logbooks (id,name) "
           "VALUES (%d,'MP-LOG-%d');"
           "INSERT INTO app_meta (key,value) VALUES ('current_logbook_id',%d) "
           "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
           logbook_id, logbook_id, logbook_id);
  int rc = sqlite3_exec(sqlite_db, sql, NULL, NULL, NULL);
  sqlite3_close(sqlite_db);
  return rc == SQLITE_OK ? 0 : -1;
}

static int count_qsos_for_logbook(const char *db_path, int logbook_id) {
  sqlite3 *sqlite_db = NULL;
  if (sqlite3_open(db_path, &sqlite_db) != SQLITE_OK) {
    if (sqlite_db)
      sqlite3_close(sqlite_db);
    return -1;
  }
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(sqlite_db,
                         "SELECT COUNT(*) FROM qso WHERE logbook_id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK) {
    sqlite3_close(sqlite_db);
    return -1;
  }
  sqlite3_bind_int(stmt, 1, logbook_id);
  int count = sqlite3_step(stmt) == SQLITE_ROW
                  ? sqlite3_column_int(stmt, 0)
                  : -1;
  sqlite3_finalize(stmt);
  sqlite3_close(sqlite_db);
  return count;
}

static void child_send_reply(int fd, MpReply *reply) {
  (void)db_sync_get_pending_outbox_count(&reply->pending);
  (void)db_sync_get_failed_outbox_count(&reply->failed);
  (void)db_sync_get_last_global_seq(&reply->cursor);
  NetSyncStatus status;
  memset(&status, 0, sizeof(status));
  net_sync_get_status(&status);
  snprintf(reply->last_error, sizeof(reply->last_error), "%s",
           status.last_error);
  (void)write_exact(fd, reply, sizeof(*reply));
}

static int child_send_duplicate_append(int client_id, const char *db_path) {
  sqlite3 *read_db = NULL;
  if (sqlite3_open(db_path, &read_db) != SQLITE_OK) {
    if (read_db)
      sqlite3_close(read_db);
    return -1;
  }

  sqlite3_stmt *stmt = NULL;
  const char *query =
      "SELECT op_id,station_seq,logbook_id,op_type,entity_id,payload_json,op_utc "
      "FROM log_outbox ORDER BY station_seq DESC LIMIT 1;";
  if (sqlite3_prepare_v2(read_db, query, -1, &stmt, NULL) != SQLITE_OK) {
    sqlite3_close(read_db);
    return -1;
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_close(read_db);
    return -1;
  }

  SyncOutboxEntry op;
  memset(&op, 0, sizeof(op));
  const unsigned char *op_id = sqlite3_column_text(stmt, 0);
  const unsigned char *op_type = sqlite3_column_text(stmt, 3);
  const unsigned char *entity_id = sqlite3_column_text(stmt, 4);
  const unsigned char *payload = sqlite3_column_text(stmt, 5);
  const unsigned char *op_utc = sqlite3_column_text(stmt, 6);
  snprintf(op.op_id, sizeof(op.op_id), "%s", op_id ? (const char *)op_id : "");
  op.station_seq = sqlite3_column_int64(stmt, 1);
  op.logbook_id = sqlite3_column_int(stmt, 2);
  snprintf(op.op_type, sizeof(op.op_type), "%s",
           op_type ? (const char *)op_type : "");
  snprintf(op.entity_id, sizeof(op.entity_id), "%s",
           entity_id ? (const char *)entity_id : "");
  snprintf(op.payload_json, sizeof(op.payload_json), "%s",
           payload ? (const char *)payload : "{}");
  snprintf(op.op_utc, sizeof(op.op_utc), "%s",
           op_utc ? (const char *)op_utc : "");
  sqlite3_finalize(stmt);
  sqlite3_close(read_db);

  if (!op.op_id[0] || !op.entity_id[0])
    return -1;

  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0)
    return -1;
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)config.net_server_port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(socket_fd);
    return -1;
  }

  char frame[8192] = {0};
  char response[4096] = {0};
  char station_id[32] = {0};
  snprintf(station_id, sizeof(station_id), "st-mp-client-%c",
           client_id == 0 ? 'a' : 'b');
  int rc = -1;
  if (net_protocol_encode_hello(station_id, "logger", config.net_auth_token,
                                frame, sizeof(frame)) != 0 ||
      net_protocol_send_framed(socket_fd, frame) != 0 ||
      net_protocol_recv_framed(socket_fd, response, sizeof(response)) != 0)
    goto done;

  if (net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame)) != 0)
    goto done;
  for (int i = 0; i < 2; i++) {
    memset(response, 0, sizeof(response));
    if (net_protocol_send_framed(socket_fd, frame) != 0 ||
        net_protocol_recv_framed(socket_fd, response, sizeof(response)) != 0 ||
        !strstr(response, "\"type\":\"APPEND_ACK\""))
      goto done;
  }
  rc = 0;

done:
  close(socket_fd);
  return rc;
}

static void run_client_process(int client_id, const char *db_path, int server_port,
                               int command_fd, int reply_fd) {
  if (set_database_path(db_path) != 0)
    _exit(10);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_station_id, sizeof(config.net_station_id),
           "st-mp-client-%c", client_id == 0 ? 'a' : 'b');
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = server_port;
  snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
           MP_AUTH_TOKEN);
  snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
           MP_AUTH_TOKEN);
  config.net_tls = 0;
  config.net_allow_insecure_lan = 1;
  config.net_sync_interval_ms = 250;
  config.net_heartbeat_sec = 1;
  config.net_retry_min_ms = 100;
  config.net_retry_max_ms = 2000;
  config.net_max_frame_bytes = 65536;
  config.net_tls_peer_fingerprint[0] = 0;

  MpReply reply;
  memset(&reply, 0, sizeof(reply));
  reply.rc = db_init();
  if (reply.rc == 0)
    reply.rc = set_process_current_logbook(db_path, client_id + 3);
  if (reply.rc == 0)
    reply.rc = db_sync_set_station_id(config.net_station_id);
  qso_init();
  if (write_exact(reply_fd, &reply, sizeof(reply)) != 0)
    _exit(11);
  if (reply.rc != 0)
    _exit(12);

  int running = 1;
  while (running) {
    MpCommand command;
    if (read_exact(command_fd, &command, sizeof(command)) != 0)
      break;
    memset(&reply, 0, sizeof(reply));

    switch (command.code) {
    case 'B':
      net_sync_set_serial_prefetch_enabled(1);
      reply.rc = net_sync_start();
      break;
    case 'W': {
      reply.rc = -1;
      int local_logbook_id = 1;
      (void)db_get_current_logbook_id(&local_logbook_id);
      for (int attempt = 0; attempt < 150; attempt++) {
        if (db_sync_peek_available_serial_reservation(local_logbook_id,
                                                      config.net_station_id,
                                                      &reply.serial) == 0) {
          reply.rc = 0;
          break;
        }
        pause_100ms();
      }
      break;
    }
    case 'N': {
      char call[16] = {0};
      snprintf(call, sizeof(call), "SP9MP%c%d", client_id == 0 ? 'A' : 'B',
               command.phase);
      char status[128] = {0};
      reply.rc = qso_add_fields(call, 7020, "599", "CW", "mp", status,
                                sizeof(status));
      if (reply.rc >= 0)
        snprintf(reply.qso_uid, sizeof(reply.qso_uid), "%s",
                 logbook[reply.rc].qso_uid);
      break;
    }
    case 'S': {
      char call[16] = {0};
      snprintf(call, sizeof(call), "SP9MP%c%d", client_id == 0 ? 'A' : 'B',
               command.phase);
      int commit_remote = 0;
      reply.rc = net_sync_reserve_serial_for_qso(
          &reply.serial, reply.reservation_id, sizeof(reply.reservation_id),
          &commit_remote);
      if (reply.rc == 0 && commit_remote) {
        char exchange_sent[24] = {0};
        snprintf(exchange_sent, sizeof(exchange_sent), "%d", reply.serial);
        char status[128] = {0};
        reply.rc = qso_add_contest_fields_with_reservation(
            call, 7020, "599", "CW", "mp", exchange_sent, "001", "RUN",
            "MULTIPROCESS", 1, 3, 1, reply.reservation_id, 1, status,
            sizeof(status));
        if (reply.rc >= 0)
          snprintf(reply.qso_uid, sizeof(reply.qso_uid), "%s",
                   logbook[reply.rc].qso_uid);
      } else if (reply.rc == 0) {
        reply.rc = -1;
      }
      break;
    }
    case 'G':
      reply.rc = -1;
      for (int attempt = 0; attempt < 300; attempt++) {
        (void)db_sync_get_pending_outbox_count(&reply.pending);
        (void)db_sync_get_failed_outbox_count(&reply.failed);
        if (reply.pending == 0 && reply.failed == 0) {
          reply.rc = 0;
          break;
        }
        pause_100ms();
      }
      break;
    case 'C':
      reply.rc = -1;
      for (int attempt = 0; attempt < 300; attempt++) {
        (void)db_sync_get_last_global_seq(&reply.cursor);
        if (reply.cursor >= command.target_seq) {
          reply.rc = 0;
          break;
        }
        pause_100ms();
      }
      break;
    case 'R':
      net_sync_stop();
      reply.rc = net_sync_start();
      break;
    case 'D':
      reply.rc = child_send_duplicate_append(client_id, db_path);
      break;
    case 'T':
      reply.rc = 0;
      break;
    case 'X':
      net_sync_stop();
      db_shutdown();
      reply.rc = 0;
      running = 0;
      break;
    default:
      reply.rc = -1;
      break;
    }

    child_send_reply(reply_fd, &reply);
  }

  net_sync_stop();
  db_shutdown();
  close(command_fd);
  close(reply_fd);
  _exit(0);
}

static int spawn_client(MpClient *client, int client_id, const char *db_path,
                        int port) {
  int commands[2];
  int replies[2];
  if (pipe(commands) != 0 || pipe(replies) != 0)
    return -1;

  pid_t child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    close(commands[1]);
    close(replies[0]);
    run_client_process(client_id, db_path, port, commands[0], replies[1]);
    _exit(0);
  }

  close(commands[0]);
  close(replies[1]);
  client->pid = child;
  client->to_child = commands[1];
  client->from_child = replies[0];
  snprintf(client->db_path, sizeof(client->db_path), "%s", db_path);
  return 0;
}

static int command_client(MpClient *client, char code, int phase,
                          long long target_seq, MpReply *out_reply) {
  MpCommand command = {.code = code,
                       .phase = phase,
                       .target_seq = target_seq};
  if (write_exact(client->to_child, &command, sizeof(command)) != 0)
    return -1;
  memset(out_reply, 0, sizeof(*out_reply));
  return read_exact(client->from_child, out_reply, sizeof(*out_reply));
}

static int send_client_command(MpClient *client, char code, int phase,
                               long long target_seq) {
  MpCommand command = {.code = code,
                       .phase = phase,
                       .target_seq = target_seq};
  return write_exact(client->to_child, &command, sizeof(command));
}

static int receive_client_reply(MpClient *client, MpReply *out_reply) {
  memset(out_reply, 0, sizeof(*out_reply));
  return read_exact(client->from_child, out_reply, sizeof(*out_reply));
}

static int mkdir_if_missing(const char *path) {
  return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static void test_multiprocess_server_two_clients(void) {
  char root_template[] = "/tmp/contest_logger_mp_XXXXXX";
  char *root = mkdtemp(root_template);
  expect_true(root != NULL, "create multiprocess test root");
  if (!root)
    return;

  char server_dir[512] = {0};
  char client_a_dir[512] = {0};
  char client_b_dir[512] = {0};
  char server_db_path[512] = {0};
  char client_a_db_path[512] = {0};
  char client_b_db_path[512] = {0};
  snprintf(server_dir, sizeof(server_dir), "%s/server", root);
  snprintf(client_a_dir, sizeof(client_a_dir), "%s/client_a", root);
  snprintf(client_b_dir, sizeof(client_b_dir), "%s/client_b", root);
  snprintf(server_db_path, sizeof(server_db_path), "%s/unit.sqlite3", server_dir);
  snprintf(client_a_db_path, sizeof(client_a_db_path), "%s/unit.sqlite3",
           client_a_dir);
  snprintf(client_b_db_path, sizeof(client_b_db_path), "%s/unit.sqlite3",
           client_b_dir);
  expect_eq(mkdir_if_missing(server_dir), 0, "create multiprocess server dir");
  expect_eq(mkdir_if_missing(client_a_dir), 0, "create multiprocess client A dir");
  expect_eq(mkdir_if_missing(client_b_dir), 0, "create multiprocess client B dir");

  signal(SIGPIPE, SIG_IGN);
  int saved_net_enabled = config.net_enabled;
  int saved_port = config.net_server_port;
  int saved_interval = config.net_sync_interval_ms;
  char saved_role[sizeof(config.net_role)];
  char saved_token[sizeof(config.net_auth_token)];
  char saved_shared[sizeof(config.net_shared_key)];
  char saved_shared_log_id[sizeof(config.net_shared_log_id)];
  char saved_host[sizeof(config.net_server_host)];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_token, sizeof(saved_token), "%s", config.net_auth_token);
  snprintf(saved_shared, sizeof(saved_shared), "%s", config.net_shared_key);
  snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
           config.net_shared_log_id);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  db_shutdown();
  expect_eq(setenv("LOGGER_DB_PATH", server_db_path, 1), 0,
            "point parent process at the server database");
  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
           MP_AUTH_TOKEN);
  snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
           MP_AUTH_TOKEN);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 20000 + ((int)getpid() % 20000);
  config.net_tls = 0;
  config.net_allow_insecure_lan = 1;
  config.net_rate_limit_burst = 64;
  config.net_rate_limit_window_sec = 1;
  config.net_sync_interval_ms = 250;
  config.net_heartbeat_sec = 1;
  config.net_retry_min_ms = 100;
  config.net_retry_max_ms = 2000;
  config.net_max_frame_bytes = 65536;

  expect_eq(db_init(), 0, "initialize multiprocess server database");
  expect_eq(set_process_current_logbook(server_db_path, 7), 0,
            "assign a server logbook ID different from both clients");
  int active_server_logbook_id = 0;
  expect_eq(db_get_current_logbook_id(&active_server_logbook_id), 0,
            "read server active logical logbook ID");
  expect_eq(active_server_logbook_id, 7,
            "server should retain its distinct active logbook ID");
  char server_shared_log_id[36] = {0};
  expect_eq(db_sync_create_shared_log_id(server_shared_log_id,
                                         sizeof(server_shared_log_id)),
            0, "create the canonical shared log ID before forking clients");
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           server_shared_log_id);
  for (int index = 1; index <= 65; index++) {
    char op_id[64] = {0};
    char entity_id[64] = {0};
    snprintf(op_id, sizeof(op_id), "mp-seed-%d", index);
    snprintf(entity_id, sizeof(entity_id), "mp-seed-entity-%d", index);
    long long global_seq = 0;
    expect_true(db_sync_apply_remote_op(
                    op_id, "st-mp-seed", index, 7, "NOOP", entity_id, "{}",
                    "2026-10-04T12:00:00Z", &global_seq) >= 0,
                "seed more than one catch-up page in the server process");
  }

  QSO seed_qso;
  memset(&seed_qso, 0, sizeof(seed_qso));
  snprintf(seed_qso.date, sizeof(seed_qso.date), "%s", "20261004");
  snprintf(seed_qso.utc, sizeof(seed_qso.utc), "%s", "1200");
  snprintf(seed_qso.call, sizeof(seed_qso.call), "%s", "SP9SEED");
  seed_qso.freq = 7020;
  snprintf(seed_qso.band, sizeof(seed_qso.band), "%s", "40M");
  snprintf(seed_qso.mode, sizeof(seed_qso.mode), "%s", "CW");
  snprintf(seed_qso.rst, sizeof(seed_qso.rst), "%s", "599");
  snprintf(seed_qso.exchange_sent, sizeof(seed_qso.exchange_sent), "%s", "10");
  snprintf(seed_qso.country, sizeof(seed_qso.country), "%s", "POLAND");
  seed_qso.cq_zone = 15;
  seed_qso.itu_zone = 28;
  long long seed_id = 0;
  expect_eq(db_insert_qso(&seed_qso, &seed_id), 0,
            "seed server log with an existing contest serial");
  SyncOutboxEntry seed_operation;
  memset(&seed_operation, 0, sizeof(seed_operation));
  int seed_operation_count = 0;
  expect_eq(db_sync_outbox_load_pending(&seed_operation, 1,
                                        &seed_operation_count),
            0, "load the server's preexisting QSO operation");
  expect_eq(seed_operation_count, 1,
            "server seed QSO should have one pending journal operation");
  if (seed_operation_count == 1) {
    long long seeded_operation_global_seq = 0;
    expect_true(db_sync_apply_remote_op(
                    seed_operation.op_id, seed_qso.origin_station_id,
                    seed_qso.origin_station_seq, 7, seed_operation.op_type,
                    seed_operation.entity_id, seed_operation.payload_json,
                    seed_operation.op_utc, &seeded_operation_global_seq) >= 0,
                "publish the existing server QSO into shared operation history");
    expect_eq(db_sync_outbox_mark_acked(seed_operation.op_id), 0,
              "mark the server's journaled seed operation as applied");
  }
  long long seed_global_seq = 0;
  expect_eq(db_sync_get_max_global_seq(&seed_global_seq), 0,
            "read seeded server global sequence");
  expect_true(seed_global_seq >= 65,
              "server seed should exceed the client page size");
  db_shutdown();

  MpClient clients[MP_CLIENT_COUNT];
  memset(clients, 0, sizeof(clients));
  int spawned = 0;
  if (spawn_client(&clients[0], 0, client_a_db_path,
                   config.net_server_port) == 0)
    spawned++;
  if (spawned == 1 &&
      spawn_client(&clients[1], 1, client_b_db_path,
                   config.net_server_port) == 0)
    spawned++;
  expect_eq(spawned, MP_CLIENT_COUNT,
            "spawn two independent client processes");

  MpReply reply;
  for (int index = 0; index < spawned; index++) {
    expect_eq(read_exact(clients[index].from_child, &reply, sizeof(reply)), 0,
              "receive isolated client initialization");
    expect_eq(reply.rc, 0, "initialize isolated client SQLite database");
  }

  expect_eq(setenv("LOGGER_DB_PATH", server_db_path, 1), 0,
            "restore server database path in parent process");
  expect_eq(db_init(), 0, "reopen server database after client forks");
  expect_eq(net_sync_start(), 0,
            "start authenticated server in the parent process");
  pause_100ms();

  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'B', 0, 0, &reply), 0,
              "start client background worker process");
    expect_eq(reply.rc, 0, "client worker should start with strong token");
  }

  MpReply initial_serials[MP_CLIENT_COUNT];
  memset(initial_serials, 0, sizeof(initial_serials));
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'W', 0, 0,
                             &initial_serials[index]),
              0, "wait for client-side prefetched serial");
    if (initial_serials[index].rc != 0)
      fprintf(stderr,
              "[DEBUG] client %d reservation wait error=%s shared=%s\n", index,
              initial_serials[index].last_error,
              config.net_shared_log_id);
    expect_eq(initial_serials[index].rc, 0,
              "each independent client should receive a central serial");
  }
  expect_true(initial_serials[0].serial != initial_serials[1].serial,
              "simultaneous clients must receive different serial reservations");

  MpReply initial_qsos[MP_CLIENT_COUNT];
  for (int index = 0; index < spawned; index++)
    expect_eq(send_client_command(&clients[index], 'N', 1, 0), 0,
              "send simultaneous QSO command to both clients");
  for (int index = 0; index < spawned; index++)
    expect_eq(receive_client_reply(&clients[index], &initial_qsos[index]), 0,
              "receive simultaneous QSO result from both clients");
  for (int index = 0; index < spawned; index++)
    expect_true(initial_qsos[index].rc >= 0,
                "each client should commit its simultaneous local QSO");

  MpReply serial_qsos[MP_CLIENT_COUNT];
  for (int index = 0; index < spawned; index++)
    expect_eq(send_client_command(&clients[index], 'S', 2, 0), 0,
              "send simultaneous reserved-serial QSO command");
  for (int index = 0; index < spawned; index++)
    expect_eq(receive_client_reply(&clients[index], &serial_qsos[index]), 0,
              "receive concurrent reserved-serial QSO results");
  for (int index = 0; index < spawned; index++)
    expect_true(serial_qsos[index].rc >= 0,
                "each client should save a QSO with its claimed reservation");
  expect_true(serial_qsos[0].serial != serial_qsos[1].serial,
              "simultaneous reserved QSO serials must be globally unique");

  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'G', 0, 0, &reply), 0,
              "wait for first online outbox and commit drain");
    if (reply.rc != 0 || reply.pending != 0 || reply.failed != 0)
      fprintf(stderr,
              "[DEBUG] client %d initial rc=%d pending=%d failed=%d cursor=%lld error=%s\n",
              index, reply.rc, reply.pending, reply.failed, reply.cursor,
              reply.last_error);
    expect_eq(reply.rc, 0, "first online client queues should drain");
  }

  MpReply standby_serials[MP_CLIENT_COUNT];
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'W', 0, 0,
                             &standby_serials[index]),
              0, "wait for an unused serial before simulating outage");
    expect_eq(standby_serials[index].rc, 0,
              "each client should have an unused serial before outage");
    expect_true(standby_serials[index].serial != serial_qsos[index].serial,
                "standby reservation should differ from the serial already used");
  }

  expect_eq(db_sync_get_max_global_seq(&seed_global_seq), 0,
            "read server high-water mark after simultaneous appends");
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'C', 0, seed_global_seq, &reply),
              0, "wait for first multi-page catch-up");
    expect_eq(reply.rc, 0,
              "each client should catch up beyond one server page");
  }

  long long before_duplicate_seq = 0;
  expect_eq(db_sync_get_max_global_seq(&before_duplicate_seq), 0,
            "read server sequence before repeated APPEND");
  expect_eq(command_client(&clients[0], 'D', 0, 0, &reply), 0,
            "send duplicate APPEND from an independent client process");
  expect_eq(reply.rc, 0, "server should acknowledge a repeated operation twice");
  long long after_duplicate_seq = 0;
  expect_eq(db_sync_get_max_global_seq(&after_duplicate_seq), 0,
            "read server sequence after repeated APPEND");
  expect_true(after_duplicate_seq == before_duplicate_seq,
              "repeated APPEND must not duplicate the server operation");

  net_sync_stop();
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'N', 3, 0, &reply), 0,
              "write offline QSO to each local outbox");
    expect_true(reply.rc >= 0, "offline QSO should be durable locally");
    expect_eq(command_client(&clients[index], 'W', 0, 0, &reply), 0,
              "wait for a prefetched reservation during network outage");
    expect_eq(reply.rc, 0,
              "client should retain unused reservations during outage");
    expect_eq(command_client(&clients[index], 'S', 4, 0, &reply), 0,
              "write offline contest QSO using cached reservation");
    expect_true(reply.rc >= 0,
                "offline contest QSO should commit locally with its reservation");
    expect_eq(command_client(&clients[index], 'T', 0, 0, &reply), 0,
              "inspect client outbox during outage");
    expect_true(reply.pending > 0,
                "offline QSO operations should remain pending for reconnect");
  }

  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'X', 0, 0, &reply), 0,
              "stop client process during outage before restart");
    expect_eq(reply.rc, 0, "client process should stop before restart");
    int status = 0;
    expect_true(waitpid(clients[index].pid, &status, 0) == clients[index].pid,
                "wait for client process before restart");
    expect_true(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "client process should exit cleanly before restart");
    close(clients[index].to_child);
    close(clients[index].from_child);
  }

  db_shutdown();
  unsetenv("LOGGER_DB_PATH");
  for (int index = 0; index < spawned; index++) {
    const char *persistent_db =
        index == 0 ? client_a_db_path : client_b_db_path;
    expect_eq(spawn_client(&clients[index], index, persistent_db,
                           config.net_server_port),
              0, "restart client process with the same database file");
    expect_eq(read_exact(clients[index].from_child, &reply, sizeof(reply)), 0,
              "receive restarted client database initialization");
    expect_eq(reply.rc, 0,
              "restarted client should reopen its durable SQLite state");
  }

  expect_eq(setenv("LOGGER_DB_PATH", server_db_path, 1), 0,
            "restore parent server DB path after client process restart");
  expect_eq(db_init(), 0, "reopen server DB before reconnect phase");
  expect_eq(net_sync_start(), 0, "restart server after simulated outage");
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'B', 0, 0, &reply), 0,
              "start worker in restarted client process");
    expect_eq(reply.rc, 0, "restarted client worker should start");
    expect_eq(command_client(&clients[index], 'G', 0, 0, &reply), 0,
              "wait for clients to reconnect and drain durable queues");
    if (reply.rc != 0 || reply.pending != 0 || reply.failed != 0)
      fprintf(stderr,
              "[DEBUG] client %d reconnect rc=%d pending=%d failed=%d cursor=%lld error=%s\n",
              index, reply.rc, reply.pending, reply.failed, reply.cursor,
              reply.last_error);
    expect_eq(reply.rc, 0,
              "client outbox and serial commits should drain after reconnect");
  }

  long long final_global_seq = 0;
  expect_eq(db_sync_get_max_global_seq(&final_global_seq), 0,
            "read final server sequence after reconnect");
  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'C', 0, final_global_seq, &reply),
              0, "wait for post-reconnect catch-up");
    expect_eq(reply.rc, 0,
              "restarted clients should catch up to final server sequence");
    expect_eq(reply.pending, 0,
              "reconnected client should have no pending or failed work");
    expect_eq(reply.failed, 0,
              "reconnected client should have no permanently failed work");
  }

  expect_eq(db_sync_commit_serial(serial_qsos[0].reservation_id,
                                  serial_qsos[0].qso_uid),
            DB_SYNC_COMMIT_OK,
            "server should treat repeated serial commit as idempotent");
  expect_eq(db_sync_commit_serial(serial_qsos[1].reservation_id,
                                  serial_qsos[1].qso_uid),
            DB_SYNC_COMMIT_OK,
            "second client's serial commit should be consumed on server");

  qso_init();
  expect_eq(qso_count, 9,
            "server should materialize seed and eight client QSOs");
  expect_eq(count_qsos_for_logbook(server_db_path, 7), 9,
            "server should route all QSO rows to its active logbook ID");
  expect_eq(count_qsos_for_logbook(client_a_db_path, 3), 9,
            "client A should map pulled operations to its local logbook ID");
  expect_eq(count_qsos_for_logbook(client_b_db_path, 4), 9,
            "client B should map pulled operations to its local logbook ID");
  int next_serial = 0;
  expect_eq(db_sync_peek_next_serial(7, &next_serial), 0,
            "read shared serial allocator after multiprocess activity");
  expect_true(next_serial > serial_qsos[0].serial &&
                  next_serial > serial_qsos[1].serial,
              "central serial counter should remain above both client serials");

  for (int index = 0; index < spawned; index++) {
    expect_eq(command_client(&clients[index], 'X', 0, 0, &reply), 0,
              "stop client process workers cleanly");
    expect_eq(reply.rc, 0, "client process should stop successfully");
  }
  net_sync_stop();
  db_shutdown();

  for (int index = 0; index < spawned; index++) {
    int status = 0;
    expect_true(waitpid(clients[index].pid, &status, 0) == clients[index].pid,
                "wait for client process exit");
    expect_true(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "client process should exit successfully");
    close(clients[index].to_child);
    close(clients[index].from_child);
  }

  config.net_enabled = saved_net_enabled;
  config.net_server_port = saved_port;
  config.net_sync_interval_ms = saved_interval;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
           saved_token);
  snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
           saved_shared);
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           saved_shared_log_id);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  unsetenv("LOGGER_DB_PATH");
}

int main(void) {
  if (!net_sync_token_is_strong(MP_AUTH_TOKEN)) {
    fprintf(stderr, "multiprocess test token does not satisfy policy\n");
    return 2;
  }
  test_multiprocess_server_two_clients();
  if (failures > 0) {
    fprintf(stderr, "Multiprocess sync tests failed: %d\n", failures);
    return 1;
  }
  printf("All multiprocess sync tests passed.\n");
  return 0;
}
