#include "app_controller.h"
#include "cat.h"
#include "config.h"
#include "contest.h"
#include "contest_rules.h"
#include "cty.h"
#include "db.h"
#include "cw_keys.h"
#include "dxcluster.h"
#include "export.h"
#include "maidenhead.h"
#include "net_protocol.h"
#include "net_server.h"
#include "net_sync.h"
#include "net_tls.h"
#include "live_upload.h"
#include "qso.h"
#include "qtc.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

static void failf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[FAIL] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  g_failures++;
}

static void expect_true(int condition, const char *message) {
  if (!condition)
    failf("%s", message);
}

static void expect_int_eq(int actual, int expected, const char *message) {
  if (actual != expected)
    failf("%s (actual=%d expected=%d)", message, actual, expected);
}

static void expect_str_eq(const char *actual, const char *expected,
                          const char *message) {
  if (!actual || strcmp(actual, expected) != 0)
    failf("%s (actual='%s' expected='%s')", message, actual ? actual : "(null)",
          expected ? expected : "(null)");
}

static void expect_double_close(double actual, double expected, double eps,
                                const char *message) {
  if (fabs(actual - expected) > eps)
    failf("%s (actual=%.8f expected=%.8f eps=%.8f)", message, actual, expected,
          eps);
}

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fputs(text, f);
  fclose(f);
  return 0;
}

static char *read_whole_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }

  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  char *buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t n = fread(buf, 1, (size_t)size, f);
  fclose(f);

  buf[n] = 0;
  return buf;
}

static void join_path(char *out, size_t out_size,
                      const char *base, const char *leaf) {
  if (!out || out_size == 0)
    return;

  out[0] = 0;

  if (!base)
    base = "";
  if (!leaf)
    leaf = "";

  strncpy(out, base, out_size - 1);
  out[out_size - 1] = 0;

  if (out[0]) {
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] != '/')
      strncat(out, "/", out_size - strlen(out) - 1);
  }

  strncat(out, leaf, out_size - strlen(out) - 1);
}

static void send_controller_chars(const char *text);
static void send_controller_text(const char *text);

#define TEST_NET_AUTH_TOKEN "UnitTest-Contest-Logger-Secret-2026!"
#define TEST_SHARED_LOG_ID "sl-0123456789abcdef0123456789abcdef"

static void set_test_db_path(const char *dir_path) {
  char db_path[512];
  join_path(db_path, sizeof(db_path), dir_path, "unit.sqlite3");
  unlink(db_path);
  db_shutdown();
  unsetenv("LOGGER_DB_PATH");
  setenv("LOGGER_DB_PATH", db_path, 1);
  snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
           TEST_NET_AUTH_TOKEN);
  snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
           TEST_NET_AUTH_TOKEN);
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           TEST_SHARED_LOG_ID);
  config.net_allow_insecure_lan = 1;
  config.net_tls_require_client_cert = 0;
  config.net_tls_client_ca_file[0] = 0;
  config.net_tls_client_cert_file[0] = 0;
  config.net_tls_client_key_file[0] = 0;
}

static int make_temp_dir(char *out, size_t out_size) {
  const char *tmp_base = getenv("TMPDIR");

  if (!tmp_base || !tmp_base[0])
    tmp_base = "/tmp";

  if (!out || out_size < 32)
    return -1;

  snprintf(out, out_size, "%s/lnx_logger_unit_XXXXXX", tmp_base);
  if (!mkdtemp(out))
    return -1;

  return 0;
}

static void test_config_load(const char *tmp_dir) {
  char conf_path[512];

  snprintf(conf_path, sizeof(conf_path), "%s/logger.conf", tmp_dir);

  const char *conf_text =
      "# Unit config\n"
      " LAT = 52.2297  \n"
      "LON=21.0122\n"
      "LOCATOR = JO92AA\n"
      "DXC_HOST = dx.example.net\n"
      "DXC_PORT = 9000\n"
      "DXC_CALL = SP9XYZ\n"
      "STATION_CALL = SP9STAC\n"
      "STATION_EXCHANGE = DOK1234\n"
      "STATION_TX_POWER_WATTS = 75\n"
      "OPERATOR_CALL = SP9OPER\n"
      "CAT_MODE_FROM_RIG = 1\n"
      "CONTEST_TECHNIQUE = SO2R\n"
      "CW_ESM = 1\n"
      "LIVE_UPLOAD_ENABLED = 1\n"
      "LIVE_UPLOAD_HOST = upload.local\n"
      "LIVE_UPLOAD_PORT = 9991\n"
      "LIVE_UPLOAD_TOKEN = secret-token\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write unit logger.conf");
  expect_int_eq(chmod(conf_path, 0644), 0,
                "make test config permissive before load");

  expect_int_eq(config_load(conf_path), 0, "config_load should succeed");
  struct stat config_stat;
  expect_int_eq(stat(conf_path, &config_stat), 0,
                "stat config after secure load");
  expect_int_eq(config_stat.st_mode & 0777, 0600,
                "config load should restrict permissions to owner");
  expect_double_close(config.lat, 52.2297, 0.0001, "config LAT parsed");
  expect_double_close(config.lon, 21.0122, 0.0001, "config LON parsed");
  expect_str_eq(config.locator, "JO92AA", "config locator parsed");
  expect_str_eq(config.dxc_host, "dx.example.net", "config host parsed");
  expect_int_eq(config.dxc_port, 9000, "config port parsed");
  expect_str_eq(config.dxc_call, "SP9XYZ", "config call parsed");
  expect_str_eq(config.station_call, "SP9STAC", "config station call parsed");
  expect_str_eq(config.station_exchange, "DOK1234",
                "config station exchange parsed");
  expect_int_eq(config.station_tx_power_watts, 75,
                "config station TX power parsed");
  expect_str_eq(config.operator_call, "SP9OPER", "config operator call parsed");
  expect_int_eq(config.cat_mode_from_rig, 1,
                "config CAT mode-from-rig parsed");
  expect_int_eq((int)config.contest_technique, (int)CONTEST_TECH_SO2R,
                "contest technique parsed");
  expect_int_eq(config.cw_esm_enabled, 1,
                "CW ESM flag parsed");
  expect_int_eq(config.live_upload_enabled, 1,
                "live upload enabled parsed");
  expect_str_eq(config.live_upload_host, "upload.local",
                "live upload host parsed");
  expect_int_eq(config.live_upload_port, 9991,
                "live upload port parsed");
  expect_str_eq(config.live_upload_token, "secret-token",
                "live upload token parsed");

  char config_link[512];
  snprintf(config_link, sizeof(config_link), "%s/logger-link.conf", tmp_dir);
  expect_int_eq(symlink(conf_path, config_link), 0,
                "create config symlink rejection fixture");
  expect_int_eq(config_load(config_link), -1,
                "config loader should reject symlink paths");
  unlink(config_link);

  expect_int_eq(config_load("/definitely/missing/logger.conf"), -1,
                "missing config should return -1");
  expect_str_eq(config.dxc_host, "telnet.reversebeacon.net",
                "default host restored on missing config");
  expect_int_eq(config.dxc_port, 7000,
                "default port restored on missing config");
  expect_str_eq(config.dxc_call, "N0CALL",
                "default call restored on missing config");
  expect_str_eq(config.station_call, "N0CALL",
                "default station call restored on missing config");
  expect_str_eq(config.station_exchange, "",
                "default station exchange restored on missing config");
  expect_int_eq(config.station_tx_power_watts, 0,
                "default station TX power restored on missing config");
  expect_str_eq(config.operator_call, "N0CALL",
                "default operator call restored on missing config");
  expect_int_eq(config.cat_mode_from_rig, 0,
                "default CAT mode-from-rig restored on missing config");
  expect_int_eq(config.cw_esm_enabled, 0,
                "default CW ESM restored on missing config");
  expect_int_eq(config.live_upload_enabled, 0,
                "default live upload enabled restored on missing config");
}

static void test_config_save_roundtrip(const char *tmp_dir) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/logger_saved.conf", tmp_dir);

  config.lat = 51.234567;
  config.lon = 19.765432;
  snprintf(config.locator, sizeof(config.locator), "%s", "JO91AA");
  snprintf(config.dxc_host, sizeof(config.dxc_host), "%s", "persist.example.net");
  config.dxc_port = 7100;
  snprintf(config.dxc_call, sizeof(config.dxc_call), "%s", "SP0PERSIST");
  snprintf(config.station_exchange, sizeof(config.station_exchange), "%s",
           "NM");
  config.station_tx_power_watts = 75;
  config.cat_model = 1234;
  snprintf(config.cat_device, sizeof(config.cat_device), "%s", "/dev/ttyS9");
  config.cat_baud = 38400;
  config.cat_data_bits = 7;
  config.cat_stop_bits = 2;
  snprintf(config.cat_parity, sizeof(config.cat_parity), "%s", "Even");
  snprintf(config.cat_handshake, sizeof(config.cat_handshake), "%s", "RTSCTS");
  config.cat_mode_from_rig = 1;
  config.cw_esm_enabled = 1;
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           TEST_SHARED_LOG_ID);
  config.net_allow_insecure_lan = 1;
  config.net_tls_require_client_cert = 1;
  snprintf(config.net_tls_client_ca_file,
           sizeof(config.net_tls_client_ca_file), "%s", "client_ca.pem");
  snprintf(config.net_tls_client_cert_file,
           sizeof(config.net_tls_client_cert_file), "%s", "client.pem");
  snprintf(config.net_tls_client_key_file,
           sizeof(config.net_tls_client_key_file), "%s", "client_key.pem");
  config.live_upload_enabled = 1;
  snprintf(config.live_upload_host, sizeof(config.live_upload_host), "%s",
           "persist-upload.local");
  config.live_upload_port = 9875;
  snprintf(config.live_upload_token, sizeof(config.live_upload_token), "%s",
           "persist-token");

  expect_int_eq(config_save(conf_path), 0, "config_save should succeed");
  struct stat saved_config_stat;
  expect_int_eq(stat(conf_path, &saved_config_stat), 0,
                "stat saved config file");
  expect_int_eq(saved_config_stat.st_mode & 0777, 0600,
                "saved config should be owner-only");

  config.cat_model = 0;
  config.cat_device[0] = 0;
  config.cat_baud = 0;
  config.cat_data_bits = 0;
  config.cat_stop_bits = 0;
  config.cat_parity[0] = 0;
  config.cat_handshake[0] = 0;
  config.cat_mode_from_rig = 0;
  config.cw_esm_enabled = 0;
  config.net_shared_log_id[0] = 0;
  config.net_allow_insecure_lan = 0;
  config.live_upload_enabled = 0;
  config.net_tls_require_client_cert = 0;
  config.net_tls_client_ca_file[0] = 0;
  config.net_tls_client_cert_file[0] = 0;
  config.net_tls_client_key_file[0] = 0;
  config.live_upload_host[0] = 0;
  config.live_upload_port = 0;
  config.live_upload_token[0] = 0;

  expect_int_eq(config_load(conf_path), 0,
                "config_load should read saved config");
  expect_str_eq(config.station_exchange, "NM",
                "saved station exchange restored");
  expect_int_eq(config.station_tx_power_watts, 75,
                "saved station TX power restored");
  expect_int_eq(config.cat_model, 1234, "saved CAT model restored");
  expect_str_eq(config.cat_device, "/dev/ttyS9", "saved CAT device restored");
  expect_int_eq(config.cat_baud, 38400, "saved CAT baud restored");
  expect_int_eq(config.cat_data_bits, 7, "saved CAT data bits restored");
  expect_int_eq(config.cat_stop_bits, 2, "saved CAT stop bits restored");
  expect_str_eq(config.cat_parity, "Even", "saved CAT parity restored");
  expect_str_eq(config.cat_handshake, "RTSCTS",
                "saved CAT handshake restored");
  expect_int_eq(config.cat_mode_from_rig, 1,
                "saved CAT mode-from-rig restored");
  expect_int_eq(config.cw_esm_enabled, 1,
                "saved CW ESM restored");
  expect_str_eq(config.net_shared_log_id, TEST_SHARED_LOG_ID,
                "saved shared log pairing restored");
  expect_int_eq(config.net_allow_insecure_lan, 1,
                "saved trusted-LAN opt-in restored");
  expect_int_eq(config.net_tls_require_client_cert, 1,
                "saved mTLS requirement restored");
  expect_str_eq(config.net_tls_client_ca_file, "client_ca.pem",
                "saved mTLS client CA path restored");
  expect_str_eq(config.net_tls_client_cert_file, "client.pem",
                "saved client certificate path restored");
  expect_str_eq(config.net_tls_client_key_file, "client_key.pem",
                "saved client key path restored");
  expect_int_eq(config.live_upload_enabled, 1,
                "saved live upload enabled restored");
  expect_str_eq(config.live_upload_host, "persist-upload.local",
                "saved live upload host restored");
  expect_int_eq(config.live_upload_port, 9875,
                "saved live upload port restored");
  expect_str_eq(config.live_upload_token, "persist-token",
                "saved live upload token restored");
}

static void test_live_upload_publish(void) {
  int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
  expect_true(server_fd >= 0, "create LiveScore UDP test socket");
  if (server_fd < 0)
    return;

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  expect_int_eq(bind(server_fd, (struct sockaddr *)&address, sizeof(address)), 0,
                "bind LiveScore UDP test socket");

  socklen_t address_size = sizeof(address);
  expect_int_eq(getsockname(server_fd, (struct sockaddr *)&address,
                            &address_size), 0,
                "get LiveScore test port");
  struct timeval timeout = {2, 0};
  setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  config.live_upload_enabled = 0;
  snprintf(config.live_upload_host, sizeof(config.live_upload_host), "%s",
           "127.0.0.1");
  config.live_upload_port = ntohs(address.sin_port);
  expect_int_eq(live_upload_publish_qso_and_stats(NULL, NULL, NULL), 0,
                "disabled LiveScore upload should be a no-op");

  config.live_upload_enabled = 1;
  char previous_station_call[sizeof(config.station_call)];
  char previous_operator_call[sizeof(config.operator_call)];
  snprintf(previous_station_call, sizeof(previous_station_call), "%s",
           config.station_call);
  snprintf(previous_operator_call, sizeof(previous_operator_call), "%s",
           config.operator_call);
  snprintf(config.live_upload_token, sizeof(config.live_upload_token), "%s",
           "token\nkey");
  snprintf(config.station_call, sizeof(config.station_call), "%s", "SP9\"ST");
  snprintf(config.operator_call, sizeof(config.operator_call), "%s",
           "OP\\CALL");
  QSO qso = {0};
  snprintf(qso.date, sizeof(qso.date), "%s", "20261004");
  snprintf(qso.utc, sizeof(qso.utc), "%s", "1200");
  snprintf(qso.call, sizeof(qso.call), "%s", "DL1\"ABC");
  snprintf(qso.band, sizeof(qso.band), "%s", "20M");
  snprintf(qso.mode, sizeof(qso.mode), "%s", "CW");
  snprintf(qso.country, sizeof(qso.country), "%s", "Germany");
  qso.freq = 14000;
  qso.points = 3;
  Statistics current_stats = {0};
  current_stats.total_qso = 12;
  current_stats.contest_score = 42;

  expect_int_eq(live_upload_publish_qso_and_stats(&qso, &current_stats,
                                                  "TEST CONTEST"),
                0, "LiveScore upload should queue successfully");

  char payload[2048] = {0};
  ssize_t received = recvfrom(server_fd, payload, sizeof(payload) - 1, 0, NULL,
                              NULL);
  expect_true(received > 0, "receive LiveScore UDP update");
  if (received > 0) {
    payload[received] = 0;
    expect_true(strstr(payload, "\"station\":\"SP9\\\"ST\"") != NULL,
                "LiveScore station string should be JSON escaped");
    expect_true(strstr(payload, "\"token\":\"token\\nkey\"") != NULL,
                "LiveScore token should escape JSON control characters");
    expect_true(strstr(payload, "\"call\":\"DL1\\\"ABC\"") != NULL,
                "LiveScore QSO should include escaped callsign");
    expect_true(strstr(payload, "\"total_qso\":12") != NULL &&
            strstr(payload, "\"contest_score\":42") != NULL,
                "LiveScore update should include current score");
  }

  close(server_fd);
  snprintf(config.station_call, sizeof(config.station_call), "%s",
           previous_station_call);
  snprintf(config.operator_call, sizeof(config.operator_call), "%s",
           previous_operator_call);
  config.live_upload_enabled = 0;
}

static void test_controller_static_tx_exchange_override(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/static_tx_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for static tx exchange test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");

  const char *contest_text =
      "NAME=IARU-LIKE\n"
      "CABRILLO_NAME=IARU-LIKE\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=ITU\n"
      "FIELD=ITU_ZONE,ITU Zone,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write static exchange contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for static exchange test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before static tx exchange test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to static tx exchange test directory");

  app_controller_init();
  const int base_qso_count = qso_count;
  AppRenderState state;
  app_controller_get_render_state(&state);

  expect_true(state.contest_entry_mode,
              "contest mode should be active in static tx exchange test");
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should be present in render state");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "ITU",
                  "static tx exchange should use its configured literal");

  send_controller_chars("SP9AAA");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("28");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one QSO should be saved in static tx exchange test");
  expect_str_eq(logbook[base_qso_count].exchange_sent, "ITU",
                "saved QSO should use configured static tx exchange");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "28",
                "saved QSO should store entered received exchange");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after static tx exchange test");
}

static void test_controller_numeric_static_exchange_template(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/numeric_static_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for numeric static exchange test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");

  const char *contest_text =
      "NAME=IARU-LIKE\n"
      "CABRILLO_NAME=IARU-LIKE\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=28\n"
      "FIELD=ITU_ZONE,ITU Zone,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write numeric static exchange contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for numeric static exchange test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before numeric static exchange test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to numeric static exchange test directory");

  app_controller_init();
  const int base_qso_count = qso_count;
  AppRenderState state;
  app_controller_get_render_state(&state);

  expect_true(state.contest_entry_mode,
              "contest mode should be active in numeric static exchange test");
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should be present in numeric static exchange test");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "28",
                  "numeric static EXCHANGE_SENT should stay literal");

  send_controller_chars("SP9NSE");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("27");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one QSO should be saved in numeric static exchange test");
  expect_str_eq(logbook[base_qso_count].exchange_sent, "28",
                "saved QSO should keep numeric static TX exchange");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "27",
                "saved QSO should store entered received exchange");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after numeric static exchange test");
}

static void test_controller_incremental_exchange_generation(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/incremental_exchange_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for incremental exchange test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");

  const char *contest_text =
      "NAME=WAG\n"
      "CABRILLO_NAME=WAG\n"
      "MODE=CW\n"
      "CATEGORY_POWER=HIGH\n"
      "EXCHANGE_SENT=WAG_EXCHANGE\n"
      "FIELD=EXCHANGE,Rcv Exch,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write incremental exchange contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "STATION_TX_POWER_WATTS=150\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for incremental exchange test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before incremental exchange test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to incremental exchange test directory");

  app_controller_init();
  app_controller_set_active_frequency_khz(14050);
  app_controller_handle_key(APP_KEY_F2);
  const int base_qso_count = qso_count;
  AppRenderState state;
  char expected_sent[16];
  char expected_next_sent[16];
  char expected_third_sent[16];
  char expected_fourth_sent[16];
  app_controller_get_render_state(&state);

  expect_true(state.contest_entry_mode,
              "contest mode should be active in incremental exchange test");
  expect_true(state.contest_exchange_label != NULL,
              "contest exchange label should be present");
  if (state.contest_exchange_label)
    expect_str_eq(state.contest_exchange_label, "Rcv Exch",
                  "WAG received exchange should use its configured label");
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should be present in incremental exchange test");
  if (state.contest_exchange_sent) {
    snprintf(expected_sent, sizeof(expected_sent), "%d", base_qso_count + 1);
    expect_str_eq(state.contest_exchange_sent, expected_sent,
                  "incremental EXCHANGE_SENT should show next serial number");
  }

  snprintf(expected_next_sent, sizeof(expected_next_sent), "%d",
           base_qso_count + 2);
  snprintf(expected_third_sent, sizeof(expected_third_sent), "%d",
           base_qso_count + 3);
  snprintf(expected_fourth_sent, sizeof(expected_fourth_sent), "%d",
           base_qso_count + 4);

  send_controller_chars("SP9SER");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("B12");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "WAG QSO with a DOK exchange should save");
  expect_str_eq(logbook[base_qso_count].exchange_sent, expected_sent,
                "WAG should send the next serial number");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "B12",
                "WAG should save a received B12 exchange");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should remain present after first incremental QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_next_sent,
                  "second incremental TX exchange should advance to serial 2");

  send_controller_chars("SP9SEQ");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("NM");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 2,
                "WAG QSO with an NM exchange should save");
  expect_str_eq(logbook[base_qso_count + 1].exchange_sent, expected_next_sent,
                "second incremental TX exchange should be serial 2");
  expect_str_eq(logbook[base_qso_count + 1].exchange_recv, "NM",
                "WAG should save a received NM exchange");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should remain present after second incremental QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_third_sent,
                  "next incremental TX exchange should advance to serial 3");

  send_controller_chars("SP9NUM");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("102");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 3,
                "WAG QSO with a numeric serial exchange should save");
  expect_str_eq(logbook[base_qso_count + 2].exchange_sent, expected_third_sent,
                "third WAG TX exchange should be serial 3");
  expect_str_eq(logbook[base_qso_count + 2].exchange_recv, "102",
                "WAG should save a received numeric serial exchange");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "WAG TX exchange should remain present after three QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_fourth_sent,
                  "next WAG TX exchange should advance to serial 4");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after incremental exchange test");
}

static void test_controller_wag_station_exchange(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/wag_station_exchange_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for WAG station exchange test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=WAG\n"
      "CABRILLO_NAME=WAG\n"
      "MODE=CW\n"
      "CATEGORY_POWER=HIGH\n"
      "EXCHANGE_SENT=WAG_EXCHANGE\n"
      "FIELD=EXCHANGE,Rcv Exch,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write WAG station exchange contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "STATION_CALL=SP9XYZ\n"
      "STATION_EXCHANGE=DOK1234\n"
      "STATION_TX_POWER_WATTS=150\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf with German station DOK");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before WAG station exchange test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to WAG station exchange test directory");

  app_controller_init();
  app_controller_set_active_frequency_khz(14050);
  const int base_qso_count = qso_count;
  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_label != NULL,
              "WAG received exchange label should be available");
  if (state.contest_exchange_label)
    expect_str_eq(state.contest_exchange_label, "Rcv Exch",
                  "WAG received exchange field should use its configured label");
  expect_true(state.contest_exchange_sent != NULL,
              "WAG German TX exchange should be present");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "DOK1234",
                  "WAG German station should send configured DOK");

  send_controller_chars("SP9WAG");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("DOKA");
  app_controller_handle_key(APP_KEY_ENTER);
  expect_int_eq(qso_count, base_qso_count + 1,
                "WAG German station should save QSO with alphanumeric DOK");
  if (qso_count > base_qso_count) {
    expect_str_eq(logbook[base_qso_count].exchange_sent, "DOK1234",
                  "WAG German QSO should store sent DOK");
    expect_str_eq(logbook[base_qso_count].exchange_recv, "DOKA",
                  "WAG German QSO should store received DOK");
  }

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after WAG station exchange test");
}

static void test_controller_reopen_resume_from_last_sent_serial(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/reopen_serial_resume_case", tmp_dir);

  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for reopen serial resume test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");

  const char *contest_text =
      "NAME=SERIAL-RESUME\n"
      "CABRILLO_NAME=SERIAL-RESUME\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write contest definition for reopen serial resume test");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for reopen serial resume test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before reopen serial resume test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to reopen serial resume case directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  qso_count = 3;

  snprintf(logbook[0].exchange_sent, sizeof(logbook[0].exchange_sent), "%s", "7");
  snprintf(logbook[0].exchange_recv, sizeof(logbook[0].exchange_recv), "%s", "200");
  snprintf(logbook[1].exchange_sent, sizeof(logbook[1].exchange_sent), "%s", "9");
  snprintf(logbook[1].exchange_recv, sizeof(logbook[1].exchange_recv), "%s", "201");
  snprintf(logbook[2].exchange_sent, sizeof(logbook[2].exchange_sent), "%s", "10");
  snprintf(logbook[2].exchange_recv, sizeof(logbook[2].exchange_recv), "%s", "202");

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "contest TX exchange should be available after reopen");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "11",
                  "reopened contest should resume from highest saved sent serial, not received exchange or row count");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after reopen serial resume test");
}

static void test_controller_received_exchange_persists_after_reopen(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/received_exchange_persist_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for received exchange persist test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=RECV-PERSIST\n"
      "CABRILLO_NAME=RECV-PERSIST\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write contest definition for received exchange persist test");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text = "CONTEST_DEF_FILE=contest.conf\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for received exchange persist test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before received exchange persist test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to received exchange persist case directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);

  send_controller_text("7020");
  send_controller_chars("SP9RECV");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("123");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, 1,
                "first contest QSO should be saved in received exchange persist test");
  expect_str_eq(logbook[0].exchange_sent, "1",
                "sent exchange should be generated as serial 1");
  expect_str_eq(logbook[0].exchange_recv, "123",
                "received exchange should be present in memory right after save");

  app_controller_shutdown();
  app_controller_init();

  expect_int_eq(qso_count, 1,
                "reloaded log should contain the saved QSO after reopen");
  expect_str_eq(logbook[0].exchange_sent, "1",
                "reload should preserve sent exchange after reopen");
  expect_str_eq(logbook[0].exchange_recv, "123",
                "reload should preserve received exchange after reopen");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after received exchange persist test");
}

static void test_controller_contest_mode_overrides_detected_mode(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/mode_override_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for contest mode override test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");

  const char *contest_text =
      "NAME=MODE-OVERRIDE\n"
      "CABRILLO_NAME=MODE-OVERRIDE\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write mode override contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "CAT_MODE_FROM_RIG=1\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for mode override test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before mode override test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to mode override test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  send_controller_text("14150");
  send_controller_chars("SP9MOD");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("001");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one QSO should be saved in mode override test");
  expect_str_eq(logbook[base_qso_count].mode, "CW",
                "contest MODE should override detected mode and CAT mode setting");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after mode override test");
}

static void test_cty_load_and_lookup(const char *tmp_dir) {
  char cty_path[512];
  snprintf(cty_path, sizeof(cty_path), "%s/wl_cty.dat", tmp_dir);

  const char *cty_text =
      "Poland:15:28:EU:52.0:21.0:0:SP:\n"
      "SP,HF;\n"
      "United States:5:8:NA:38.0:-97.0:0:K:\n"
      "K;\n"
      "United States K1:5:8:NA:41.0:-71.0:0:K1:\n"
      "K1;\n";

  expect_int_eq(write_text_file(cty_path, cty_text), 0, "write test CTY file");

  int loaded = cty_load(cty_path);
  expect_true(loaded > 0, "cty_load should load entries");

  const CtyEntry *sp = cty_lookup("sp9abc");
  expect_true(sp != NULL, "SP9ABC should resolve");
  if (sp) {
    expect_str_eq(sp->country, "Poland", "SP9ABC country");
    expect_int_eq(sp->cq_zone, 15, "SP9ABC CQ zone");
    expect_int_eq(sp->itu_zone, 28, "SP9ABC ITU zone");
  }

  const CtyEntry *k1 = cty_lookup("K1ABC");
  expect_true(k1 != NULL, "K1ABC should resolve");
  if (k1) {
    expect_str_eq(k1->country, "United States K1",
                  "longest prefix K1 should win");
  }

  const CtyEntry *unknown = cty_lookup("ZZ9ZZZ");
  expect_true(unknown == NULL, "Unknown prefix should not resolve");
}

static void test_cty_download_latest_failure_path(const char *tmp_dir) {
  char cty_path[512];
  snprintf(cty_path, sizeof(cty_path), "%s/downloaded_wl_cty.dat", tmp_dir);

  const char *old_path = getenv("PATH");
  char old_path_buf[2048] = {0};

  if (old_path)
    snprintf(old_path_buf, sizeof(old_path_buf), "%s", old_path);

  setenv("PATH", "", 1);

  int rc = cty_download_latest(cty_path);
  expect_int_eq(rc, -1,
                "cty_download_latest should fail when curl/wget are unavailable");

  if (old_path)
    setenv("PATH", old_path_buf, 1);
  else
    unsetenv("PATH");
}

static void test_qso_helpers(void) {
  char band[8] = {0};
  char mode[16] = {0};

  detect_band(14074, band);
  expect_str_eq(band, "20M", "detect_band 14074");

  detect_band(144100, band);
  expect_str_eq(band, "2M", "detect_band 144100");

  detect_band(999, band);
  expect_str_eq(band, "?", "detect_band unknown");

  detect_mode(14074, mode);
  expect_str_eq(mode, "FT8", "detect_mode FT8");

  detect_mode(14080, mode);
  expect_str_eq(mode, "FT4", "detect_mode FT4 exact");

  detect_mode(14071, mode);
  expect_str_eq(mode, "PSK31", "detect_mode PSK31");

  detect_mode(14090, mode);
  expect_str_eq(mode, "RTTY", "detect_mode RTTY");

  detect_mode(7020, mode);
  expect_str_eq(mode, "CW", "detect_mode CW");

  detect_mode(14150, mode);
  expect_str_eq(mode, "SSB", "detect_mode SSB");
}

static void test_cw_esm_enter_planner(void) {
  AppRenderState state;
  memset(&state, 0, sizeof(state));

  const int saved_cw_esm_enabled = config.cw_esm_enabled;
  config.cw_esm_enabled = 1;

  state.active_radio = 1;
  state.radio1_mode = "CW";
  state.radio1_run = true;
  state.radio2_mode = "CW";
  state.radio2_run = false;

  int pre = 0;
  int post = 0;

  /* Active CALL field, empty CALL in RUN -> CQ macro (F1). */
  state.active_input_field = 0;
  state.input_call = "";
  state.input_rst = "";
  expect_true(app_controller_plan_cw_esm_enter(&state, &pre, &post) != 0,
              "ESM planner should be active in CW when enabled");
  expect_int_eq(pre, 1, "RUN empty CALL should pick F1 before Enter");
  expect_int_eq(post, 0, "RUN empty CALL should not queue post macro");

  /* Active CALL field, non-empty CALL -> call repeat macro (F9). */
  state.input_call = "SP9ABC";
  state.input_rst = "";
  pre = 0;
  post = 0;
  expect_true(app_controller_plan_cw_esm_enter(&state, &pre, &post) != 0,
              "ESM planner should remain active for filled CALL");
  expect_int_eq(pre, 9, "filled CALL should pick F9 before Enter");
  expect_int_eq(post, 0, "filled CALL should not queue post macro");

  /* Active EXCH field, empty EXCH -> exchange macro (F2). */
  state.active_input_field = 1;
  state.input_call = "SP9ABC";
  state.input_rst = "";
  pre = 0;
  post = 0;
  expect_true(app_controller_plan_cw_esm_enter(&state, &pre, &post) != 0,
              "ESM planner should be active on EXCH field");
  expect_int_eq(pre, 2, "empty EXCH should pick F2 before Enter");
  expect_int_eq(post, 0, "empty EXCH should not queue post macro");

  /* Active EXCH field, filled EXCH in RUN -> post-QSO QRZ macro (F8). */
  state.input_rst = "001";
  pre = 0;
  post = 0;
  expect_true(app_controller_plan_cw_esm_enter(&state, &pre, &post) != 0,
              "ESM planner should be active for filled EXCH in RUN");
  expect_int_eq(pre, 0, "filled EXCH in RUN should not send pre macro");
  expect_int_eq(post, 8, "filled EXCH in RUN should queue F8 post macro");

  /* Same EXCH case in S&P -> post-QSO own-call macro (F4). */
  state.radio1_run = false;
  pre = 0;
  post = 0;
  expect_true(app_controller_plan_cw_esm_enter(&state, &pre, &post) != 0,
              "ESM planner should be active for filled EXCH in S&P");
  expect_int_eq(pre, 0, "filled EXCH in S&P should not send pre macro");
  expect_int_eq(post, 4, "filled EXCH in S&P should queue F4 post macro");

  /* Non-CW mode should disable ESM decisions. */
  state.radio1_mode = "SSB";
  pre = 11;
  post = 12;
  expect_int_eq(app_controller_plan_cw_esm_enter(&state, &pre, &post), 0,
                "ESM planner should be inactive outside CW");
  expect_int_eq(pre, 0, "non-CW should zero pre action");
  expect_int_eq(post, 0, "non-CW should zero post action");

  /* Disabled config should also disable planner. */
  config.cw_esm_enabled = 0;
  state.radio1_mode = "CW";
  pre = 13;
  post = 14;
  expect_int_eq(app_controller_plan_cw_esm_enter(&state, &pre, &post), 0,
                "ESM planner should be inactive when disabled in config");
  expect_int_eq(pre, 0, "disabled ESM should zero pre action");
  expect_int_eq(post, 0, "disabled ESM should zero post action");

  config.cw_esm_enabled = saved_cw_esm_enabled;
}

static void test_cat_cw_busy_state_disconnected(void) {
  cat_disconnect_cw_keyer();
  expect_int_eq(cat_is_cw_keyer_connected(), 0,
                "CW keyer should be disconnected in baseline busy-state test");
  expect_int_eq(cat_cw_is_busy(), 0,
                "CW keyer should not be busy when disconnected");
}

static void test_db_sync_identity_and_sequence(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_sync_identity", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create db_sync_identity test directory");

  set_test_db_path(case_dir);
  expect_int_eq(db_init(), 0, "db_init should succeed for sync identity test");

  char station_a[32] = {0};
  char station_b[32] = {0};
  expect_int_eq(db_sync_get_or_create_station_id(station_a, sizeof(station_a)),
                0, "station id should be generated");
  expect_true(station_a[0] != 0, "station id should be non-empty");
  expect_true(strstr(station_a, "st-") == station_a,
              "station id should use st- prefix");

  expect_int_eq(db_sync_get_or_create_station_id(station_b, sizeof(station_b)),
                0, "station id should be readable after creation");
  expect_str_eq(station_b, station_a, "station id should be stable");

  long long seq1 = 0;
  long long seq2 = 0;
  expect_int_eq(db_sync_next_station_seq(&seq1), 0,
                "first station seq should be allocated");
  expect_int_eq(db_sync_next_station_seq(&seq2), 0,
                "second station seq should be allocated");
  expect_true(seq1 > 0, "first station seq should be positive");
  expect_true(seq2 > seq1, "station seq should be monotonic");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_outbox_lifecycle(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_sync_outbox", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create db_sync_outbox test directory");

  set_test_db_path(case_dir);
  expect_int_eq(db_init(), 0, "db_init should succeed for outbox test");

  long long seq = 0;
  expect_int_eq(db_sync_next_station_seq(&seq), 0,
                "station seq should be allocated for outbox");

  expect_int_eq(db_sync_outbox_enqueue("op-test-1", seq, 1, "QSO_INSERT",
                                       "q-test-1",
                                       "{\"kind\":\"qso_insert\"}",
                                       "2026-01-01T00:00:00Z"),
                0, "enqueue should succeed");

  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable");
  expect_int_eq(pending, 1, "one outbox entry should be pending");

  expect_int_eq(db_sync_outbox_mark_sent("op-test-1"), 0,
                "mark sent should succeed");
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after mark sent");
  expect_int_eq(pending, 1,
                "sent entry should still count as not-acked pending");

  expect_int_eq(db_sync_outbox_mark_acked("op-test-1"), 0,
                "mark acked should succeed");
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after mark acked");
  expect_int_eq(pending, 0, "acked entry should not be pending");

  long long last_seq = 0;
  expect_int_eq(db_sync_set_last_global_seq(1234), 0,
                "set global seq should succeed");
  expect_int_eq(db_sync_get_last_global_seq(&last_seq), 0,
                "get global seq should succeed");
  expect_true(last_seq == 1234,
              "stored global seq should match expected value");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_outbox_retry_limit_marks_failed(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_sync_retry_limit", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create db_sync_retry_limit test directory");

  set_test_db_path(case_dir);
  expect_int_eq(db_init(), 0, "db_init should succeed for retry-limit test");

  long long seq = 0;
  expect_int_eq(db_sync_next_station_seq(&seq), 0,
                "station seq should be allocated for retry-limit test");

  expect_int_eq(db_sync_outbox_enqueue("op-retry-limit", seq, 1, "QSO_INSERT",
                                       "q-retry-1",
                                       "{\"kind\":\"qso_insert\"}",
                                       "2026-01-01T00:00:00Z"),
                0, "enqueue should succeed for retry-limit test");

  for (int i = 0; i < 6; i++) {
    expect_int_eq(db_sync_outbox_mark_retry("op-retry-limit", 1), 0,
                  "mark retry should succeed");
  }

  int pending = -1;
  int failed = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable for retry-limit test");
  expect_int_eq(db_sync_get_failed_outbox_count(&failed), 0,
                "failed count should be readable for retry-limit test");
  expect_int_eq(pending, 0,
                "operation should leave pending queue after retry limit");
  expect_int_eq(failed, 1,
                "operation should be marked failed after retry limit");

  SyncOutboxEntry ops[4];
  int ops_count = 0;
  memset(ops, 0, sizeof(ops));
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "pending loader should work after retry-limit transition");
  expect_int_eq(ops_count, 0,
                "failed operation should not be returned as pending");

  SyncOutboxEntry failed_ops[2];
  int failed_ops_count = 0;
  memset(failed_ops, 0, sizeof(failed_ops));
  expect_int_eq(db_sync_load_failed_outbox(failed_ops, 2, &failed_ops_count),
                0, "failed outbox loader should succeed");
  expect_int_eq(failed_ops_count, 1,
                "failed outbox loader should return failed operation");
  expect_str_eq(failed_ops[0].op_id, "op-retry-limit",
                "failed outbox should retain stable operation ID");
  expect_int_eq(db_sync_retry_failed_outbox("op-retry-limit"), 0,
                "failed operation should be retryable");
  expect_int_eq(db_sync_retry_failed_outbox("op-retry-limit"), -1,
                "retry should reject an operation no longer failed");
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "pending loader should succeed after failed retry");
  expect_int_eq(ops_count, 1,
                "retried failed operation should return to pending");
  expect_str_eq(ops[0].op_id, "op-retry-limit",
                "retry should preserve stable operation ID");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_qso_sync_metadata_roundtrip(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/qso_sync_roundtrip", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create qso_sync_roundtrip test directory");

  set_test_db_path(case_dir);
  qso_init();

  char status[128] = {0};
  int idx = qso_add_fields("SP9SYNC", 7020, "599", "CW", "", status,
                           sizeof(status));
  expect_true(idx >= 0, "qso_add_fields should create sync test QSO");
  expect_true(strncmp(status, "QSO OK", strlen("QSO OK")) == 0,
              "sync test QSO should report success with optional queue status");
  expect_true(qso_count == 1, "sync test logbook should contain one QSO");

  expect_true(logbook[0].qso_uid[0] != 0, "QSO should have qso_uid");
  expect_true(logbook[0].origin_station_id[0] != 0,
              "QSO should have origin station id");
  expect_true(logbook[0].origin_station_seq > 0,
              "QSO should have positive origin station seq");
  expect_true(logbook[0].version >= 1, "QSO should have version");
  expect_true(logbook[0].last_modified_utc[0] != 0,
              "QSO should have last_modified_utc");

  char uid_before[40] = {0};
  snprintf(uid_before, sizeof(uid_before), "%s", logbook[0].qso_uid);
  long long seq_before = logbook[0].origin_station_seq;

  qso_init();
  expect_true(qso_count == 1, "qso_init should reload single QSO");
  expect_str_eq(logbook[0].qso_uid, uid_before,
                "qso_uid should persist after reload");
  expect_true(logbook[0].origin_station_seq == seq_before,
              "origin station seq should persist after reload");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_atomic_writes_and_legacy_migration(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_sync_atomic", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create db_sync_atomic test directory");

  set_test_db_path(case_dir);
  expect_int_eq(db_init(), 0, "db_init should succeed for atomic-write test");
  qso_init();

  char db_path[512];
  join_path(db_path, sizeof(db_path), case_dir, "unit.sqlite3");
  sqlite3 *fault_db = NULL;
  expect_int_eq(sqlite3_open(db_path, &fault_db), SQLITE_OK,
                "open database to install failure triggers");
  if (!fault_db) {
    set_test_db_path(tmp_dir);
    qso_init();
    return;
  }

  expect_int_eq(sqlite3_exec(
                    fault_db,
                    "CREATE TRIGGER fail_outbox_insert BEFORE INSERT ON "
                    "log_outbox BEGIN SELECT RAISE(ABORT, 'injected outbox "
                    "failure'); END;",
                    NULL, NULL, NULL),
                SQLITE_OK, "install outbox failure trigger");

  char status[128] = {0};
  expect_int_eq(qso_add_fields("SP9ATOMIC", 7020, "599", "CW", "", status,
                               sizeof(status)),
                -1, "outbox failure should reject local QSO insert");
  expect_str_eq(status, "Database save failed",
                "failed local save should be reported to the caller");
  expect_int_eq(qso_count, 0,
                "failed outbox insert should roll back the QSO row");
  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending outbox count should remain readable after rollback");
  expect_int_eq(pending, 0,
                "failed local transaction should not leave an outbox row");
  long long next_local_seq = 0;
  expect_int_eq(db_sync_next_station_seq(&next_local_seq), 0,
                "station sequence should remain allocatable after rollback");
  expect_true(next_local_seq == 1,
              "failed local transaction should roll back its sequence reservation");

  expect_int_eq(sqlite3_exec(fault_db, "DROP TRIGGER fail_outbox_insert;",
                             NULL, NULL, NULL),
                SQLITE_OK, "remove outbox failure trigger");
  expect_int_eq(sqlite3_exec(
                    fault_db,
                    "CREATE TRIGGER fail_log_ops_insert BEFORE INSERT ON "
                    "log_ops BEGIN SELECT RAISE(ABORT, 'injected log_ops "
                    "failure'); END;",
                    NULL, NULL, NULL),
                SQLITE_OK, "install log_ops failure trigger");

  const char *remote_payload =
      "{\"kind\":\"qso_full\",\"qso_uid\":\"q-atomic-remote\","
      "\"origin_station_id\":\"st-atomic\",\"origin_station_seq\":4,"
      "\"last_modified_utc\":\"2026-10-04T12:01:00Z\",\"version\":1,"
      "\"date\":\"20261004\",\"utc\":\"1201\",\"call\":\"SP9REMOTE\","
      "\"freq\":7021,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\","
      "\"comments\":\"\",\"exchange_sent\":\"\",\"exchange_recv\":\"\","
      "\"operator_mode\":\"\",\"contest_id\":\"\",\"radio_nr\":1,"
      "\"points\":1,\"country\":\"POLAND\",\"cq_zone\":15,"
      "\"itu_zone\":28,\"invalid\":false}";
  long long remote_global_seq = 0;
  expect_int_eq(db_sync_apply_remote_op_with_cursor(
                    "op-atomic-remote", "st-atomic", 4, 1, "QSO_INSERT",
                    "q-atomic-remote", remote_payload,
                    "2026-10-04T12:01:00Z", 25, &remote_global_seq),
                DB_SYNC_APPLY_ERR,
                "log_ops failure should reject remote apply");
  qso_init();
  expect_int_eq(qso_count, 0,
                "log_ops failure should roll back remote QSO upsert");
  long long cursor = -1;
  expect_int_eq(db_sync_get_last_global_seq(&cursor), 0,
                "global cursor should remain readable after rollback");
  expect_true(cursor == 0,
              "failed remote apply should not advance the global cursor");

  expect_int_eq(sqlite3_exec(fault_db, "DROP TRIGGER fail_log_ops_insert;",
                             NULL, NULL, NULL),
                SQLITE_OK, "remove log_ops failure trigger");
  sqlite3_close(fault_db);

  expect_int_eq(db_sync_apply_remote_op_with_cursor(
                    "op-atomic-remote", "st-atomic", 4, 1, "QSO_INSERT",
                    "q-atomic-remote", remote_payload,
                    "2026-10-04T12:01:00Z", 25, &remote_global_seq),
                DB_SYNC_APPLY_CHANGED,
                "remote apply should succeed when log_ops insert is available");
  expect_int_eq(db_sync_get_last_global_seq(&cursor), 0,
                "global cursor should be readable after remote commit");
  expect_true(cursor == 25,
              "remote apply should commit its global cursor with the operation");
  expect_int_eq(db_sync_apply_remote_op_with_cursor(
                    "op-atomic-remote", "st-atomic", 4, 1, "QSO_INSERT",
                    "q-atomic-remote", remote_payload,
                    "2026-10-04T12:01:00Z", 25, &remote_global_seq),
                DB_SYNC_APPLY_ALREADY_PRESENT,
                "replayed remote apply should remain idempotent");

  expect_int_eq(sqlite3_open(db_path, &fault_db), SQLITE_OK,
                "reopen database to inject local update failure");
  if (fault_db) {
    expect_int_eq(sqlite3_exec(
                      fault_db,
                      "CREATE TRIGGER fail_outbox_update BEFORE INSERT ON "
                      "log_outbox BEGIN SELECT RAISE(ABORT, 'injected outbox "
                      "failure'); END;",
                      NULL, NULL, NULL),
                  SQLITE_OK, "install update outbox failure trigger");
    qso_init();
    long long remote_qso_id = qso_count > 0 ? logbook[0].db_id : 0;
    expect_true(remote_qso_id > 0,
                "remote QSO should have a database id for update test");
    expect_int_eq(db_update_qso_invalid(remote_qso_id, 1), -1,
                  "outbox failure should reject local QSO update");
    qso_init();
    expect_true(qso_count == 1 && !logbook[0].invalid,
                "failed outbox insert should roll back QSO field update");
    qso_mark_invalid(0);
    expect_true(qso_count == 1 && !logbook[0].invalid,
          "failed invalid toggle should restore committed in-memory state");
    expect_int_eq(sqlite3_exec(fault_db, "DROP TRIGGER fail_outbox_update;",
                               NULL, NULL, NULL),
                  SQLITE_OK, "remove update outbox failure trigger");
    sqlite3_close(fault_db);
    fault_db = NULL;
  }

  set_test_db_path(tmp_dir);
  qso_init();

  char migration_dir[512];
  snprintf(migration_dir, sizeof(migration_dir), "%s/legacy_qso_migration",
           tmp_dir);
  expect_int_eq(mkdir(migration_dir, 0777), 0,
                "create legacy QSO migration directory");
  set_test_db_path(migration_dir);
  join_path(db_path, sizeof(db_path), migration_dir, "unit.sqlite3");
  sqlite3 *legacy_db = NULL;
  expect_int_eq(sqlite3_open(db_path, &legacy_db), SQLITE_OK,
                "create pre-sync legacy database");
  if (legacy_db) {
    expect_int_eq(sqlite3_exec(
                      legacy_db,
                      "CREATE TABLE qso (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "date TEXT NOT NULL,utc TEXT NOT NULL,call TEXT NOT NULL,"
                      "freq INTEGER NOT NULL,band TEXT NOT NULL,mode TEXT NOT NULL,"
                      "rst TEXT NOT NULL,comments TEXT NOT NULL DEFAULT '',"
                      "country TEXT NOT NULL,cq_zone INTEGER NOT NULL,"
                      "itu_zone INTEGER NOT NULL,invalid INTEGER NOT NULL DEFAULT 0);"
                      "INSERT INTO qso (date,utc,call,freq,band,mode,rst,country,"
                      "cq_zone,itu_zone) VALUES ('20200101','0100','SP9OLD',"
                      "7020,'40M','CW','599','POLAND',15,28);"
                      "CREATE TABLE log_outbox (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "op_id TEXT NOT NULL,station_seq INTEGER NOT NULL,"
                      "logbook_id INTEGER NOT NULL,op_type TEXT NOT NULL,"
                      "entity_id TEXT NOT NULL,payload_json TEXT NOT NULL,"
                      "op_utc TEXT NOT NULL,status TEXT NOT NULL DEFAULT 'pending',"
                      "retry_count INTEGER NOT NULL DEFAULT 0,"
                      "next_retry_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
                      "CREATE TABLE log_ops (global_seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "op_id TEXT NOT NULL,station_id TEXT NOT NULL,"
                      "station_seq INTEGER NOT NULL,logbook_id INTEGER NOT NULL,"
                      "op_type TEXT NOT NULL,entity_id TEXT NOT NULL,"
                      "payload_json TEXT NOT NULL,op_utc TEXT NOT NULL,"
                      "applied_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);",
                      NULL, NULL, NULL),
                  SQLITE_OK, "seed legacy QSO schema and row");
    sqlite3_close(legacy_db);
    legacy_db = NULL;
  }
  expect_int_eq(db_init(), 0, "legacy QSO database should migrate");
  qso_init();
  expect_int_eq(qso_count, 1, "legacy QSO should survive migration");
  expect_str_eq(logbook[0].qso_uid, "legacy-1",
                "legacy QSO should receive a stable unique sync identifier");

  sqlite3 *migrated_db = NULL;
  expect_int_eq(sqlite3_open(db_path, &migrated_db), SQLITE_OK,
                "open migrated database to inspect uniqueness indexes");
  if (migrated_db) {
    expect_int_eq(sqlite3_exec(
                      migrated_db,
                      "INSERT INTO log_ops (op_id,station_id,station_seq,"
                      "logbook_id,op_type,entity_id,payload_json,op_utc) "
                      "VALUES ('migration-op-1','st-migration',1,1,'NOOP',"
                      "'entity','{}','2026-10-04T12:02:00Z');",
                      NULL, NULL, NULL),
                  SQLITE_OK, "migrated log_ops should accept its first op");
    expect_int_eq(sqlite3_exec(
                      migrated_db,
                      "INSERT INTO log_ops (op_id,station_id,station_seq,"
                      "logbook_id,op_type,entity_id,payload_json,op_utc) "
                      "VALUES ('migration-op-1','st-other',2,1,"
                      "'NOOP','entity','{}','2026-10-04T12:02:01Z');",
                      NULL, NULL, NULL),
                  SQLITE_CONSTRAINT,
                  "migrated log_ops should preserve op_id uniqueness");
    expect_int_eq(sqlite3_exec(
                      migrated_db,
                      "INSERT INTO log_ops (op_id,station_id,station_seq,"
                      "logbook_id,op_type,entity_id,payload_json,op_utc) "
                      "VALUES ('migration-op-2','st-migration',1,1,'NOOP',"
                      "'entity','{}','2026-10-04T12:02:02Z');",
                      NULL, NULL, NULL),
                  SQLITE_CONSTRAINT,
                  "migrated log_ops should preserve station sequence uniqueness");
    sqlite3_close(migrated_db);
  }

    expect_int_eq(db_sync_outbox_enqueue(
            "op-migration-outbox-1", 1, 1, "QSO_INSERT", "q-mig-1",
            "{}", "2026-10-04T12:02:00Z"),
          0, "migrated outbox should accept its first operation");
    expect_int_eq(db_sync_outbox_enqueue(
            "op-migration-outbox-2", 1, 1, "QSO_INSERT", "q-mig-2",
            "{}", "2026-10-04T12:02:01Z"),
          -1, "migrated outbox should preserve station sequence uniqueness");
    expect_int_eq(db_sync_outbox_enqueue(
            "op-migration-outbox-1", 2, 1, "QSO_INSERT", "q-mig-3",
            "{}", "2026-10-04T12:02:02Z"),
          -1, "migrated outbox should preserve op_id uniqueness");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_protocol_frames(void) {
  char saved_shared_log_id[sizeof(config.net_shared_log_id)] = {0};
  snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
           config.net_shared_log_id);
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           TEST_SHARED_LOG_ID);

  char frame[2048] = {0};
  NetMessageType type = NET_MSG_UNKNOWN;

  expect_int_eq(net_protocol_encode_hello("st-abc123", "logger", "token-1",
                                          frame, sizeof(frame)),
                0, "HELLO frame should encode");
  expect_true(strstr(frame, "\"type\":\"HELLO\"") != NULL,
              "HELLO frame should contain message type");
  expect_true(strstr(frame, "\"auth_token\":\"token-1\"") != NULL,
              "HELLO frame should contain auth token");
  expect_true(strstr(frame, TEST_SHARED_LOG_ID) != NULL,
              "HELLO envelope should carry shared_log_id");
  NetSessionMeta hello_meta;
  memset(&hello_meta, 0, sizeof(hello_meta));
  expect_int_eq(net_protocol_parse_hello_meta(frame, &hello_meta), 0,
                "HELLO metadata should parse shared log identity");
  expect_str_eq(hello_meta.shared_log_id, TEST_SHARED_LOG_ID,
                "HELLO metadata should retain shared_log_id");
  expect_int_eq(net_protocol_validate_shared_log_id(frame, TEST_SHARED_LOG_ID),
                0, "matching shared_log_id should validate");
  expect_int_eq(net_protocol_validate_shared_log_id(
                    frame, "sl-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                -1, "mismatched shared_log_id should be rejected");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "HELLO frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_HELLO,
                "HELLO frame should map to HELLO enum");

  memset(frame, 0, sizeof(frame));
  expect_int_eq(net_protocol_encode_hello_ack(1, 12, 44, frame,
                                              sizeof(frame)),
                0, "HELLO_ACK frame should encode");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "HELLO_ACK frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_HELLO_ACK,
                "HELLO_ACK frame should map to HELLO_ACK enum");
  int accepted = 0;
  long long next_expected = 0;
  long long server_global_seq = 0;
  char parsed_shared_log_id[36] = {0};
  expect_int_eq(net_protocol_parse_hello_ack(
                    frame, &accepted, &next_expected, &server_global_seq,
                    parsed_shared_log_id, sizeof(parsed_shared_log_id)),
                0, "HELLO_ACK should return server shared_log_id");
  expect_str_eq(parsed_shared_log_id, TEST_SHARED_LOG_ID,
                "HELLO_ACK should negotiate the canonical shared log");

  memset(frame, 0, sizeof(frame));
  expect_int_eq(net_protocol_encode_pull_ops(77, 25, frame, sizeof(frame)),
                0, "PULL_OPS frame should encode");
  expect_true(strstr(frame, "\"from_global_seq\":77") != NULL,
              "PULL_OPS frame should contain from_global_seq");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "PULL_OPS frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_PULL_OPS,
                "PULL_OPS frame should map to PULL_OPS enum");

  memset(frame, 0, sizeof(frame));
  expect_int_eq(net_protocol_encode_catchup_request(77, 25, frame,
                                                    sizeof(frame)),
                0, "CATCHUP_REQUEST frame should encode");
  expect_true(strstr(frame, "\"type\":\"CATCHUP_REQUEST\"") != NULL,
              "CATCHUP_REQUEST frame should contain message type");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "CATCHUP_REQUEST frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_CATCHUP_REQUEST,
                "CATCHUP_REQUEST frame should map to CATCHUP_REQUEST enum");

  char wrong_ver[2048] = {0};
  snprintf(wrong_ver, sizeof(wrong_ver), "%s", frame);
  char *pver = strstr(wrong_ver, "\"protocol_version\":2");
  if (pver)
    memcpy(pver + strlen("\"protocol_version\":"), "1", 1);
  char *pver_legacy = strstr(wrong_ver, "\"protocol_ver\":2");
  if (pver_legacy)
    memcpy(pver_legacy + strlen("\"protocol_ver\":"), "1", 1);
  expect_int_eq(net_protocol_validate_protocol_version(wrong_ver), -1,
                "protocol version mismatch should be rejected");

  memset(frame, 0, sizeof(frame));
  expect_int_eq(net_protocol_encode_heartbeat(frame, sizeof(frame)), 0,
                "HEARTBEAT frame should encode");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "HEARTBEAT frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_HEARTBEAT,
                "HEARTBEAT frame should map to HEARTBEAT enum");

  SyncOutboxEntry op;
  memset(&op, 0, sizeof(op));
  snprintf(op.op_id, sizeof(op.op_id), "%s", "op-abc");
  op.station_seq = 1;
  op.logbook_id = 1;
  snprintf(op.op_type, sizeof(op.op_type), "%s", "QSO_INSERT");
  snprintf(op.entity_id, sizeof(op.entity_id), "%s", "q-1");
  snprintf(op.payload_json, sizeof(op.payload_json), "%s",
           "{\"kind\":\"qso_insert\"}");
  snprintf(op.op_utc, sizeof(op.op_utc), "%s", "2026-01-01T00:00:00Z");

  memset(frame, 0, sizeof(frame));
  expect_int_eq(net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame)),
                0, "APPEND_OPS frame should encode");
  expect_true(strstr(frame, "\"type\":\"APPEND_OPS\"") != NULL,
              "APPEND_OPS frame should contain message type");
  expect_true(strstr(frame, "\"op_id\":\"op-abc\"") != NULL,
              "APPEND_OPS frame should contain op id");
  expect_int_eq(net_protocol_detect_type(frame, &type), 0,
                "APPEND_OPS frame type detection should succeed");
  expect_int_eq((int)type, (int)NET_MSG_APPEND_OPS,
                "APPEND_OPS frame should map to APPEND_OPS enum");

    memset(frame, 0, sizeof(frame));
    expect_int_eq(net_protocol_encode_commit_serial("rsv-123", "q-123", frame,
                            sizeof(frame)),
          0, "COMMIT_SERIAL frame should encode");
    expect_true(strstr(frame, "\"type\":\"COMMIT_SERIAL\"") != NULL,
          "COMMIT_SERIAL frame should contain message type");
    expect_int_eq(net_protocol_detect_type(frame, &type), 0,
          "COMMIT_SERIAL frame type detection should succeed");
    expect_int_eq((int)type, (int)NET_MSG_COMMIT_SERIAL,
          "COMMIT_SERIAL frame should map to COMMIT_SERIAL enum");

    const char *reserve_ack =
      "{\"type\":\"RESERVE_SERIAL_ACK\",\"request_id\":\"req-1\",\"reservation_id\":\"rsv-1\",\"serial\":42,\"expires_utc\":\"2026-01-01T00:10:00Z\"}";
    char parsed_request_id[32] = {0};
    char parsed_reservation_id[64] = {0};
    int parsed_serial = 0;
    char parsed_expires[32] = {0};
    expect_int_eq(net_protocol_parse_reserve_serial_ack(
            reserve_ack, parsed_request_id,
            sizeof(parsed_request_id), parsed_reservation_id,
            sizeof(parsed_reservation_id), &parsed_serial,
            parsed_expires, sizeof(parsed_expires)),
          0, "RESERVE_SERIAL_ACK parser should succeed");
    expect_str_eq(parsed_request_id, "req-1",
          "RESERVE_SERIAL_ACK should parse request_id");
    expect_str_eq(parsed_reservation_id, "rsv-1",
          "RESERVE_SERIAL_ACK should parse reservation_id");
    expect_int_eq(parsed_serial, 42,
          "RESERVE_SERIAL_ACK should parse serial");

    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
             saved_shared_log_id);
}

  static void test_net_sync_config_validation(void) {
    const Config saved_config = config;
    int saved_port = config.net_server_port;
    int saved_enabled = config.net_enabled;
    int saved_tls = config.net_tls;
    int saved_allow_insecure_lan = config.net_allow_insecure_lan;
    char saved_role[sizeof(config.net_role)];
    char saved_host[sizeof(config.net_server_host)];
    char saved_token[sizeof(config.net_auth_token)];
    char saved_shared_key[sizeof(config.net_shared_key)];
    char saved_shared_log_id[sizeof(config.net_shared_log_id)];
    char saved_fingerprint[sizeof(config.net_tls_peer_fingerprint)];
    snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
    snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);
    snprintf(saved_token, sizeof(saved_token), "%s", config.net_auth_token);
    snprintf(saved_shared_key, sizeof(saved_shared_key), "%s",
             config.net_shared_key);
    snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
         config.net_shared_log_id);
    snprintf(saved_fingerprint, sizeof(saved_fingerprint), "%s",
             config.net_tls_peer_fingerprint);
    config.net_tls_require_client_cert = 0;
    config.net_tls_client_ca_file[0] = 0;
    config.net_tls_client_cert_file[0] = 0;
    config.net_tls_client_key_file[0] = 0;

    char error[128] = {0};
    snprintf(config.net_role, sizeof(config.net_role), "%s", "peer");
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "unsupported network roles should be rejected");
    expect_true(error[0] != 0, "invalid role should include validation detail");

    snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
    config.net_server_port = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "invalid server ports should be rejected");

    snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
    config.net_tls = 0;
    config.net_allow_insecure_lan = 0;
    config.net_server_port = 9230;
    config.net_server_host[0] = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "client configuration without a host should be rejected");

    snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
             "127.0.0.1");
    config.net_shared_log_id[0] = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
            "client without explicit shared log pairing should be rejected");
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
         TEST_SHARED_LOG_ID);
    config.net_enabled = 1;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
            "enabled sync without TLS should require explicit LAN opt-in");
    expect_true(strstr(error, "NET_TLS=1") != NULL,
          "plaintext sync rejection should explain TLS requirement");
    config.net_allow_insecure_lan = 1;
        config.net_tls_require_client_cert = 0;
    config.net_tls_client_ca_file[0] = 0;
    config.net_tls_client_cert_file[0] = 0;
    config.net_tls_client_key_file[0] = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), 0,
            "explicit trusted LAN opt-in should allow plaintext tests");

    snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
             "Different-Strong-Secret-2026-Value!");
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "different auth token aliases should be rejected");
    snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
             config.net_auth_token);

    config.net_tls = 1;
    config.net_allow_insecure_lan = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "TLS clients without a configured pin should be rejected");
    snprintf(config.net_tls_peer_fingerprint,
             sizeof(config.net_tls_peer_fingerprint), "%s",
             "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF");
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), 0,
                  "TLS clients with a configured certificate pin should pass");

    config.net_tls_require_client_cert = 1;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
            "mTLS clients without identity files should be rejected");
    snprintf(config.net_tls_client_cert_file,
         sizeof(config.net_tls_client_cert_file), "%s", "client.pem");
    snprintf(config.net_tls_client_key_file,
         sizeof(config.net_tls_client_key_file), "%s", "client_key.pem");
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), 0,
            "mTLS clients with certificate and key should pass");

    snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
    config.net_tls = 1;
    config.net_allow_insecure_lan = 0;
    config.net_server_port = 9230;
    snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
         TEST_NET_AUTH_TOKEN);
    snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
         TEST_NET_AUTH_TOKEN);
    config.net_tls_client_ca_file[0] = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
            "mTLS server without client CA should be rejected");
    snprintf(config.net_tls_client_ca_file,
         sizeof(config.net_tls_client_ca_file), "%s", "client_ca.pem");
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), 0,
            "mTLS server with a CA should pass validation");
    config.net_tls_require_client_cert = 0;
    config.net_auth_token[0] = 0;
    config.net_shared_key[0] = 0;
    expect_int_eq(net_sync_validate_config(error, sizeof(error)), -1,
                  "server startup should reject a missing auth token");
    expect_int_eq(net_sync_token_is_strong(TEST_NET_AUTH_TOKEN), 1,
                  "test secret should satisfy the strong token policy");
    expect_int_eq(net_sync_token_is_strong("short"), 0,
                  "short auth tokens should be rejected");
    expect_int_eq(net_server_start(), -1,
                  "direct server startup should reject a missing secret");

    config.net_server_port = saved_port;
    config.net_enabled = saved_enabled;
    config.net_tls = saved_tls;
    config.net_allow_insecure_lan = saved_allow_insecure_lan;
    snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
    snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
             saved_host);
    snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
             saved_token);
    snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
             saved_shared_key);
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
         saved_shared_log_id);
    snprintf(config.net_tls_peer_fingerprint,
             sizeof(config.net_tls_peer_fingerprint), "%s", saved_fingerprint);
    config = saved_config;
  }

typedef struct {
  int port;
  long long ack_last_global_seq;
  char acked_json[256];
  char rejected_json[256];
  char received[8192];
  int mode;
  int delay_sec;
  int ok;
} MockSyncServerArgs;

enum {
  MOCK_SYNC_MODE_NORMAL = 0,
  MOCK_SYNC_MODE_DROP_APPEND_ACK = 1,
  MOCK_SYNC_MODE_DELAY_PULL_RESP = 2,
  MOCK_SYNC_MODE_TRUNCATE_PULL_RESP = 3
};

static void *mock_sync_server_thread(void *arg) {
  MockSyncServerArgs *ctx = (MockSyncServerArgs *)arg;
  if (!ctx)
    return NULL;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    return NULL;

  int reuse = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)ctx->port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(srv);
    return NULL;
  }

  if (listen(srv, 1) != 0) {
    close(srv);
    return NULL;
  }

  int cli = accept(srv, NULL, NULL);
  if (cli < 0) {
    close(srv);
    return NULL;
  }

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char frame[8192] = {0};
  if (net_protocol_recv_framed(cli, frame, sizeof(frame)) != 0) {
    close(cli);
    close(srv);
    return NULL;
  }
  snprintf(ctx->received + strlen(ctx->received),
           sizeof(ctx->received) - strlen(ctx->received), "%s", frame);

  char response[1024] = {0};
  if (net_protocol_encode_hello_ack(1, 1, 9999, response,
                                   sizeof(response)) != 0) {
    close(cli);
    close(srv);
    return NULL;
  }
  (void)net_protocol_send_framed(cli, response);

  memset(frame, 0, sizeof(frame));
  if (net_protocol_recv_framed(cli, frame, sizeof(frame)) != 0) {
    close(cli);
    close(srv);
    return NULL;
  }
  snprintf(ctx->received + strlen(ctx->received),
           sizeof(ctx->received) - strlen(ctx->received), "%s", frame);

  if (ctx->mode == MOCK_SYNC_MODE_TRUNCATE_PULL_RESP) {
    const uint32_t declared_length = htonl(128);
    (void)send(cli, &declared_length, sizeof(declared_length), 0);
    (void)send(cli, "{\"type\":\"PULL_OPS_RESP\"", 22, 0);
    ctx->ok = 1;
    close(cli);
    close(srv);
    return NULL;
  }

  char pull_resp[1024] = {0};
  if (ctx->mode == MOCK_SYNC_MODE_DELAY_PULL_RESP && ctx->delay_sec > 0)
    sleep((unsigned int)ctx->delay_sec);
  if (net_protocol_encode_pull_ops_resp(NULL, 0, 0, 0, pull_resp,
                                        sizeof(pull_resp)) != 0) {
    close(cli);
    close(srv);
    return NULL;
  }
  (void)net_protocol_send_framed(cli, pull_resp);

  memset(frame, 0, sizeof(frame));
  if (net_protocol_recv_framed(cli, frame, sizeof(frame)) != 0) {
    close(cli);
    close(srv);
    return NULL;
  }
  snprintf(ctx->received + strlen(ctx->received),
           sizeof(ctx->received) - strlen(ctx->received), "%s", frame);

  if (ctx->mode == MOCK_SYNC_MODE_DROP_APPEND_ACK) {
    ctx->ok = 1;
    close(cli);
    close(srv);
    return NULL;
  }

  if (strstr(frame, "\"type\":\"APPEND_OPS\"")) {
    char accepted_json[256] = {0};
    snprintf(accepted_json, sizeof(accepted_json), "[%s]",
             ctx->acked_json[0] ? ctx->acked_json : "");
        if (net_protocol_encode_append_ack(
          accepted_json, ctx->rejected_json[0] ? ctx->rejected_json : "[]", 1,
                                       ctx->ack_last_global_seq, response,
                                       sizeof(response)) != 0) {
      close(cli);
      close(srv);
      return NULL;
    }
  } else {
    if (net_protocol_encode_ack_empty(ctx->ack_last_global_seq, response,
                                      sizeof(response)) != 0) {
      close(cli);
      close(srv);
      return NULL;
    }
  }
  (void)net_protocol_send_framed(cli, response);

  ctx->ok = 1;
  close(cli);
  close(srv);
  return NULL;
}

typedef struct {
  int port;
  int page_count;
  SyncLogOpEntry pages[2][2];
  int page_sizes[2];
  int page_has_more[2];
  int broadcast_before_pages;
  SyncLogOpEntry broadcast_op;
  char received[8192];
  int ok;
} MockPagedSyncServerArgs;

static void *mock_paged_sync_server_thread(void *arg) {
  MockPagedSyncServerArgs *ctx = (MockPagedSyncServerArgs *)arg;
  if (!ctx || ctx->page_count < 1 || ctx->page_count > 2)
    return NULL;

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return NULL;

  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)ctx->port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(server_fd, 1) != 0) {
    close(server_fd);
    return NULL;
  }

  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd < 0) {
    close(server_fd);
    return NULL;
  }

  struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  char frame[16384] = {0};
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0)
    goto paged_server_done;
  strncat(ctx->received, frame, sizeof(ctx->received) -
                                    strlen(ctx->received) - 1);

  char response[16384] = {0};
  if (net_protocol_encode_hello_ack(1, 1, 9999, response,
                                    sizeof(response)) != 0 ||
      net_protocol_send_framed(client_fd, response) != 0)
    goto paged_server_done;

  if (ctx->broadcast_before_pages) {
    memset(response, 0, sizeof(response));
    if (net_protocol_encode_op_broadcast(&ctx->broadcast_op, response,
                                         sizeof(response)) != 0 ||
        net_protocol_send_framed(client_fd, response) != 0)
      goto paged_server_done;
  }

  for (int page = 0; page < ctx->page_count; page++) {
    memset(frame, 0, sizeof(frame));
    if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0)
      goto paged_server_done;
    strncat(ctx->received, frame, sizeof(ctx->received) -
                                      strlen(ctx->received) - 1);

    long long page_last_seq = 0;
    if (ctx->page_sizes[page] > 0)
      page_last_seq = ctx->pages[page][ctx->page_sizes[page] - 1].global_seq;
    memset(response, 0, sizeof(response));
    if (net_protocol_encode_catchup_batch(
            ctx->pages[page], ctx->page_sizes[page], page_last_seq,
            ctx->page_has_more[page], response, sizeof(response)) != 0 ||
        net_protocol_send_framed(client_fd, response) != 0)
      goto paged_server_done;
    ctx->ok = 1;
  }

  memset(frame, 0, sizeof(frame));
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) == 0) {
    strncat(ctx->received, frame, sizeof(ctx->received) -
                                      strlen(ctx->received) - 1);
    if (net_protocol_encode_ack_empty(0, response, sizeof(response)) == 0)
      (void)net_protocol_send_framed(client_fd, response);
  }

paged_server_done:
  close(client_fd);
  close(server_fd);
  return NULL;
}

static void fill_test_log_op(SyncLogOpEntry *op, long long global_seq,
                             long long station_seq, const char *op_id) {
  memset(op, 0, sizeof(*op));
  op->global_seq = global_seq;
  op->station_seq = station_seq;
  op->logbook_id = 1;
  snprintf(op->op_id, sizeof(op->op_id), "%s", op_id);
  snprintf(op->station_id, sizeof(op->station_id), "%s", "st-page-client");
  snprintf(op->op_type, sizeof(op->op_type), "%s", "NOOP");
  snprintf(op->entity_id, sizeof(op->entity_id), "entity-%lld", station_seq);
  snprintf(op->payload_json, sizeof(op->payload_json), "%s", "{}");
  snprintf(op->op_utc, sizeof(op->op_utc), "%s", "2026-10-04T12:00:00Z");
}

static void test_net_sync_paged_catchup_replay_and_cursor(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_paged_catchup", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create paged catch-up test directory");
  set_test_db_path(case_dir);
  qso_init();

  char db_path[512];
  join_path(db_path, sizeof(db_path), case_dir, "unit.sqlite3");
  sqlite3 *fault_db = NULL;
  expect_int_eq(sqlite3_open(db_path, &fault_db), SQLITE_OK,
                "open database to inject a page apply failure");
  if (!fault_db)
    return;
  expect_int_eq(sqlite3_exec(
                    fault_db,
                    "CREATE TRIGGER fail_second_page_op BEFORE INSERT ON "
                    "log_ops WHEN NEW.op_id = 'op-page-2' BEGIN "
                    "SELECT RAISE(ABORT, 'injected page failure'); END;",
                    NULL, NULL, NULL),
                SQLITE_OK, "install catch-up page failure trigger");

  int saved_net_enabled = config.net_enabled;
  int saved_port = config.net_server_port;
  int saved_heartbeat = config.net_heartbeat_sec;
  char saved_role[16];
  char saved_host[128];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);
  config.net_enabled = 1;
  config.net_heartbeat_sec = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19328;

  MockPagedSyncServerArgs failed_page_server;
  memset(&failed_page_server, 0, sizeof(failed_page_server));
  failed_page_server.port = config.net_server_port;
  failed_page_server.page_count = 2;
  failed_page_server.broadcast_before_pages = 1;
  fill_test_log_op(&failed_page_server.broadcast_op, 100, 1, "op-broadcast-100");
  snprintf(failed_page_server.broadcast_op.station_id,
           sizeof(failed_page_server.broadcast_op.station_id), "%s",
           "st-live-broadcast");
  failed_page_server.page_sizes[0] = 2;
  failed_page_server.page_has_more[0] = 1;
  failed_page_server.page_sizes[1] = 1;
  fill_test_log_op(&failed_page_server.pages[0][0], 1, 1, "op-page-1");
  fill_test_log_op(&failed_page_server.pages[0][1], 2, 2, "op-page-2");
  fill_test_log_op(&failed_page_server.pages[1][0], 3, 3, "op-page-3");

  pthread_t server_thread;
  expect_int_eq(pthread_create(&server_thread, NULL,
                               mock_paged_sync_server_thread,
                               &failed_page_server),
                0, "start mock server for failed catch-up page");
  usleep(120000);
  expect_int_eq(net_sync_start(), 0, "start client for failed catch-up page");
    expect_int_eq(net_sync_poll_once(), -1,
                "apply failure should fail the current catch-up page");
  net_sync_stop();
  pthread_join(server_thread, NULL);
  expect_true(failed_page_server.ok,
              "mock server should deliver the failing catch-up page");
  expect_true(strstr(failed_page_server.received,
                     "\"from_global_seq\":0") != NULL,
              "failed page should start at the persisted cursor");

  long long cursor = -1;
  expect_int_eq(db_sync_get_last_global_seq(&cursor), 0,
                "cursor should be readable after page failure");
  expect_true(cursor == 0,
              "neither a broadcast nor a partial page may advance the cursor");

  expect_int_eq(sqlite3_exec(fault_db, "DROP TRIGGER fail_second_page_op;",
                             NULL, NULL, NULL),
                SQLITE_OK, "remove catch-up page failure trigger");
  sqlite3_close(fault_db);
  fault_db = NULL;

  config.net_server_port = 19329;
  MockPagedSyncServerArgs retry_server;
  memset(&retry_server, 0, sizeof(retry_server));
  retry_server.port = config.net_server_port;
  retry_server.page_count = 2;
  retry_server.broadcast_before_pages = 1;
  retry_server.broadcast_op = failed_page_server.broadcast_op;
  retry_server.page_sizes[0] = 2;
  retry_server.page_has_more[0] = 1;
  retry_server.page_sizes[1] = 1;
  fill_test_log_op(&retry_server.pages[0][0], 1, 1, "op-page-1");
  fill_test_log_op(&retry_server.pages[0][1], 2, 2, "op-page-2");
  fill_test_log_op(&retry_server.pages[1][0], 3, 3, "op-page-3");

  pthread_t retry_thread;
  expect_int_eq(pthread_create(&retry_thread, NULL,
                               mock_paged_sync_server_thread, &retry_server),
                0, "start mock server for catch-up restart");
  usleep(120000);
  expect_int_eq(net_sync_start(), 0, "restart client after partial page apply");
  expect_int_eq(net_sync_poll_once(), 0,
                "retry should replay partial page and continue to next page");
  net_sync_stop();
  pthread_join(retry_thread, NULL);
  expect_true(retry_server.ok,
              "mock server should complete both catch-up pages");
  expect_true(strstr(retry_server.received, "\"from_global_seq\":0") != NULL,
              "restart should request the uncommitted page from cursor zero");
  expect_true(strstr(retry_server.received, "\"from_global_seq\":2") != NULL,
              "client should request the next page from the prior page end");
  expect_int_eq(db_sync_get_last_global_seq(&cursor), 0,
                "cursor should be readable after catch-up restart");
  expect_true(cursor == 3,
              "successful replay and pagination should advance cursor to page end");

  config.net_enabled = saved_net_enabled;
  config.net_heartbeat_sec = saved_heartbeat;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_mock_server_roundtrip(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_roundtrip", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create net_sync_roundtrip test directory");

  set_test_db_path(case_dir);
  qso_init();

  char status[128] = {0};
  int idx = qso_add_fields("SP9NET", 7020, "599", "CW", "", status,
                           sizeof(status));
  expect_true(idx >= 0, "net sync roundtrip should create one QSO");

  SyncOutboxEntry ops[4];
  int ops_count = 0;
  memset(ops, 0, sizeof(ops));
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "outbox should be readable before sync");
  expect_true(ops_count > 0, "outbox should contain pending operation");

  int saved_net_enabled = config.net_enabled;
  int saved_sync_interval = config.net_sync_interval_ms;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;

  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  config.net_sync_interval_ms = 100;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19321 + ((int)getpid() % 20000);
  expect_int_eq(db_sync_set_shared_log_id(config.net_shared_log_id), 0,
                "explicitly pair mock client DB before starting sync");
  char pairing_before_start[36] = {0};
  expect_true(db_sync_get_shared_log_id(pairing_before_start,
                                        sizeof(pairing_before_start)) == 0,
              "mock client database should have an explicit shared-log pairing");
  expect_str_eq(pairing_before_start, config.net_shared_log_id,
                "mock client database pairing should match config");
  char validation_before_start[128] = {0};
  expect_true(net_sync_validate_config(validation_before_start,
                                      sizeof(validation_before_start)) == 0,
              "mock client config should pass shared-log validation");

  MockSyncServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  server.ack_last_global_seq = 4321;
  snprintf(server.acked_json, sizeof(server.acked_json), "\"%s\"", ops[0].op_id);

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "mock sync server thread should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "net sync should start");
  pthread_join(tid, NULL);
  net_sync_stop();

  expect_true(server.ok == 1, "mock server should accept one client session");
  expect_true(strstr(server.received, "\"type\":\"HELLO\"") != NULL,
              "client should send HELLO frame");
  expect_true(strstr(server.received, "\"type\":\"CATCHUP_REQUEST\"") !=
                  NULL,
              "client should send CATCHUP_REQUEST frame");
  expect_true(strstr(server.received, "\"from_global_seq\":0") != NULL,
              "catch-up should start from the locally committed cursor");
  expect_true(strstr(server.received, "\"type\":\"APPEND_OPS\"") != NULL,
              "client should send APPEND_OPS frame");
  expect_true(strstr(server.received, ops[0].op_id) != NULL,
              "APPEND_OPS should contain pending op id");

  long long last_seq = 0;
  expect_int_eq(db_sync_get_last_global_seq(&last_seq), 0,
                "global seq should be readable after sync");
  expect_true(last_seq == 0,
              "HELLO_ACK and APPEND_ACK must not advance the pull cursor");

  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after ACK");
  expect_int_eq(pending, 0, "acked outbox item should no longer be pending");

  NetSyncStatus st;
  memset(&st, 0, sizeof(st));
  net_sync_get_status(&st);
  expect_true(st.last_pulled_global_seq == 0,
              "net sync status should expose the committed pull cursor");
  expect_true(st.connected,
              "periodic worker should complete a sync without manual polling");
  expect_true(st.pending_outbox == 0,
              "net sync status should expose empty pending outbox");

  config.net_enabled = saved_net_enabled;
  config.net_sync_interval_ms = saved_sync_interval;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  set_test_db_path(tmp_dir);
  qso_init();
}

typedef struct {
  int port;
  int ok;
  char request_id[64];
} MockSerialWorkerServerArgs;

static int mock_accept_with_timeout(int server_fd) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(server_fd, &read_fds);
  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  if (select(server_fd + 1, &read_fds, NULL, NULL, &timeout) <= 0)
    return -1;
  return accept(server_fd, NULL, NULL);
}

static int mock_serial_worker_handshake(int client_fd) {
  char frame[2048] = {0};
  char response[2048] = {0};
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0 ||
      net_protocol_encode_hello_ack(1, 1, 9999, response,
                                    sizeof(response)) != 0 ||
      net_protocol_send_framed(client_fd, response) != 0)
    return -1;
  return 0;
}

static void *mock_serial_worker_server_thread(void *arg) {
  MockSerialWorkerServerArgs *ctx = (MockSerialWorkerServerArgs *)arg;
  if (!ctx)
    return NULL;

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0)
    return NULL;
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)ctx->port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(server_fd, 4) != 0) {
    close(server_fd);
    return NULL;
  }

  int client_fd = mock_accept_with_timeout(server_fd);
  if (client_fd < 0)
    goto serial_server_done;
  struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  if (mock_serial_worker_handshake(client_fd) != 0)
    goto serial_server_done;

  char frame[2048] = {0};
  char response[2048] = {0};
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0 ||
      net_protocol_encode_catchup_batch(NULL, 0, 0, 0, response,
                                        sizeof(response)) != 0 ||
      net_protocol_send_framed(client_fd, response) != 0)
    goto serial_server_done;
  memset(frame, 0, sizeof(frame));
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0 ||
      net_protocol_encode_ack_empty(0, response, sizeof(response)) != 0 ||
      net_protocol_send_framed(client_fd, response) != 0)
    goto serial_server_done;
  close(client_fd);

  client_fd = mock_accept_with_timeout(server_fd);
  if (client_fd < 0)
    goto serial_server_done;
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  if (mock_serial_worker_handshake(client_fd) != 0)
    goto serial_server_done;

  int logbook_id = 0;
  int ttl_sec = 0;
  if (net_protocol_recv_framed(client_fd, frame, sizeof(frame)) != 0 ||
      net_protocol_parse_reserve_serial(frame, ctx->request_id,
                                        sizeof(ctx->request_id), &logbook_id,
                                        &ttl_sec) != 0 ||
      logbook_id != 1 || ttl_sec <= 0 ||
      net_protocol_encode_reserve_serial_ack(
          ctx->request_id, "rsv-worker-serial-72", 72,
          "2099-01-01T00:00:00Z", response, sizeof(response)) != 0 ||
      net_protocol_send_framed(client_fd, response) != 0)
    goto serial_server_done;
  ctx->ok = 1;

serial_server_done:
  if (client_fd >= 0)
    close(client_fd);
  close(server_fd);
  return NULL;
}

static void test_net_worker_prefetches_serial_reservations(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_worker_serial_pool", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create worker serial pool test directory");
  set_test_db_path(case_dir);
  qso_init();

  int saved_net_enabled = config.net_enabled;
  int saved_interval = config.net_sync_interval_ms;
  int saved_heartbeat = config.net_heartbeat_sec;
  int saved_port = config.net_server_port;
  char saved_role[16];
  char saved_host[128];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);
  config.net_enabled = 1;
  config.net_sync_interval_ms = 100;
  config.net_heartbeat_sec = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19333;
  net_sync_set_serial_prefetch_enabled(1);

  MockSerialWorkerServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  pthread_t server_thread;
  expect_int_eq(pthread_create(&server_thread, NULL,
                               mock_serial_worker_server_thread, &server),
                0, "start mock serial worker server");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "start client serial worker");
  int serial = 0;
  int reserved = 0;
  for (int attempt = 0; attempt < 50; attempt++) {
    if (net_sync_peek_serial_reservation(&serial) == 0) {
      reserved = 1;
      break;
    }
    usleep(100000);
  }

  expect_true(reserved,
              "background worker should prefetch a serial without UI networking");
  expect_int_eq(serial, 72, "prefetched serial should be visible to QSO entry");
  pthread_join(server_thread, NULL);
  expect_true(server.ok, "mock server should complete poll and reservation");
  expect_true(strncmp(server.request_id, "req-", 4) == 0,
              "reservation request should carry a unique generated ID");

  char reservation_id[64] = {0};
  int claimed_serial = 0;
  int commit_remote = 0;
  expect_int_eq(net_sync_reserve_serial_for_qso(
                    &claimed_serial, reservation_id, sizeof(reservation_id),
                    &commit_remote),
                0, "QSO entry should claim its cached reservation locally");
  expect_int_eq(claimed_serial, 72, "claimed serial should match preview");
  expect_int_eq(commit_remote, 1,
                "client reservation should be committed asynchronously");
  expect_str_eq(reservation_id, "rsv-worker-serial-72",
                "claim should retain server reservation identity");
  expect_int_eq(db_sync_release_serial_reservation(reservation_id, 1), 0,
                "test should release claimed reservation after verification");

  net_sync_stop();
  net_sync_set_serial_prefetch_enabled(0);
  config.net_enabled = saved_net_enabled;
  config.net_sync_interval_ms = saved_interval;
  config.net_heartbeat_sec = saved_heartbeat;
  config.net_server_port = saved_port;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_partial_ack_keeps_unacked_pending(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_partial_ack", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create net_sync_partial_ack test directory");

  set_test_db_path(case_dir);
  qso_init();

  char status[128] = {0};
  expect_true(qso_add_fields("SP9A1", 7020, "599", "CW", "", status,
                             sizeof(status)) >= 0,
              "first QSO for partial ACK should be created");
  expect_true(qso_add_fields("SP9A2", 7021, "599", "CW", "", status,
                             sizeof(status)) >= 0,
              "second QSO for partial ACK should be created");

  SyncOutboxEntry ops[4];
  int ops_count = 0;
  memset(ops, 0, sizeof(ops));
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "outbox should load for partial ACK test");
  expect_true(ops_count >= 2,
              "partial ACK test should have at least two pending ops");

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;

  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19322;

  MockSyncServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  server.ack_last_global_seq = 4333;
  snprintf(server.acked_json, sizeof(server.acked_json), "\"%s\"", ops[0].op_id);
  snprintf(server.rejected_json, sizeof(server.rejected_json),
           "[{\"op_id\":\"%s\",\"code\":\"SEQ_GAP\"}]",
           ops[1].op_id);

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "mock server for partial ACK should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "net sync should start for partial ACK");
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync poll should succeed for partial ACK");
  NetSyncStatus gap_status;
  memset(&gap_status, 0, sizeof(gap_status));
  net_sync_get_status(&gap_status);
  expect_true(strstr(gap_status.last_error, "station sequence gap") != NULL,
              "sequence gap rejection should be visible in sync status");
  net_sync_stop();
  pthread_join(tid, NULL);

  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after partial ACK");
  expect_int_eq(pending, 1,
                "one operation should remain pending after partial ACK");

  usleep(2200000);

  SyncOutboxEntry after_partial[4];
  int after_partial_count = 0;
  memset(after_partial, 0, sizeof(after_partial));
  expect_int_eq(db_sync_outbox_load_pending(after_partial, 4, &after_partial_count),
                0, "pending outbox should be loadable after retry delay");
  expect_int_eq(after_partial_count, 1,
                "exactly one op should remain for retry after partial ACK");
  expect_true(after_partial[0].retry_count >= 1,
              "remaining op should have incremented retry_count");

  config.net_server_port = 19323;
  MockSyncServerArgs server2;
  memset(&server2, 0, sizeof(server2));
  server2.port = config.net_server_port;
  server2.ack_last_global_seq = 4334;
  snprintf(server2.acked_json, sizeof(server2.acked_json), "\"%s\"",
           after_partial[0].op_id);

  pthread_t tid2;
  expect_int_eq(pthread_create(&tid2, NULL, mock_sync_server_thread, &server2),
                0, "mock server for retry resend should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0,
                "net sync should restart for retry resend phase");
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync poll should resend and ack remaining op");
  net_sync_stop();
  pthread_join(tid2, NULL);

  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "no pending operations should remain after second ACK");
  expect_int_eq(pending, 0,
                "retry resend phase should fully drain outbox");

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_connect_backoff(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_backoff", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create net_sync_backoff test directory");

  set_test_db_path(case_dir);
  qso_init();

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;

  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 1;

  expect_int_eq(net_sync_start(), 0, "net sync should start for backoff test");
  expect_int_eq(net_sync_poll_once(), -1,
                "first poll should fail when no server is available");
  expect_int_eq(net_sync_poll_once(), 0,
                "second immediate poll should be skipped by backoff");

  NetSyncStatus st;
  memset(&st, 0, sizeof(st));
  net_sync_get_status(&st);
  expect_true(!st.connected,
              "status should remain disconnected during backoff window");

  net_sync_stop();

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_protocol_append_and_pull_parsing(void) {
  SyncOutboxEntry out_ops[2];
  memset(out_ops, 0, sizeof(out_ops));

  snprintf(out_ops[0].op_id, sizeof(out_ops[0].op_id), "%s", "op-a");
  out_ops[0].station_seq = 10;
  out_ops[0].logbook_id = 1;
  snprintf(out_ops[0].op_type, sizeof(out_ops[0].op_type), "%s",
           "QSO_INSERT");
  snprintf(out_ops[0].entity_id, sizeof(out_ops[0].entity_id), "%s", "q-a");
  snprintf(out_ops[0].payload_json, sizeof(out_ops[0].payload_json), "%s",
           "{\"qso_uid\":\"q-a\",\"version\":1}");
  snprintf(out_ops[0].op_utc, sizeof(out_ops[0].op_utc), "%s",
           "2026-01-01T00:00:00Z");

  char frame[4096] = {0};
  expect_int_eq(net_protocol_encode_append_ops(out_ops, 1, frame, sizeof(frame)),
                0, "encode APPEND_OPS should succeed");

  NetAppendOp parsed[2];
  int parsed_count = 0;
  memset(parsed, 0, sizeof(parsed));
  expect_int_eq(net_protocol_parse_append_ops(frame, parsed, 2, &parsed_count),
                0, "parse APPEND_OPS should succeed");
  expect_int_eq(parsed_count, 1, "parsed APPEND_OPS count");
  expect_str_eq(parsed[0].op_id, "op-a", "parsed APPEND_OPS op_id");

  char missing_logbook_frame[4096] = {0};
  snprintf(missing_logbook_frame, sizeof(missing_logbook_frame),
           "{\"type\":\"APPEND_OPS\",\"ops\":[{\"op_id\":\"op-missing-logbook\",\"station_id\":\"st-1\",\"station_seq\":9,\"op_type\":\"QSO_INSERT\",\"entity_id\":\"q-missing\",\"payload\":{\"qso_uid\":\"q-missing\",\"version\":1},\"op_utc\":\"2026-01-01T00:00:00Z\"}]}" );
  NetAppendOp missing_logbook[2];
  int missing_logbook_count = 0;
  memset(missing_logbook, 0, sizeof(missing_logbook));
  expect_true(net_protocol_parse_append_ops(missing_logbook_frame,
                                          missing_logbook, 2,
                                          &missing_logbook_count) == 0 &&
              missing_logbook_count == 0,
              "APPEND_OPS without logbook_id must be rejected");

  SyncLogOpEntry pull_ops[1];
  memset(pull_ops, 0, sizeof(pull_ops));
  pull_ops[0].global_seq = 123;
  snprintf(pull_ops[0].op_id, sizeof(pull_ops[0].op_id), "%s", "op-pull");
  snprintf(pull_ops[0].station_id, sizeof(pull_ops[0].station_id), "%s",
           "st-server");
  pull_ops[0].station_seq = 44;
  pull_ops[0].logbook_id = 1;
  snprintf(pull_ops[0].op_type, sizeof(pull_ops[0].op_type), "%s",
           "QSO_INSERT");
  snprintf(pull_ops[0].entity_id, sizeof(pull_ops[0].entity_id), "%s",
           "q-pull");
  snprintf(pull_ops[0].payload_json, sizeof(pull_ops[0].payload_json), "%s",
           "{\"qso_uid\":\"q-pull\",\"version\":2}");
  snprintf(pull_ops[0].op_utc, sizeof(pull_ops[0].op_utc), "%s",
           "2026-01-01T00:00:01Z");

  char pull_frame[4096] = {0};
  expect_int_eq(net_protocol_encode_pull_ops_resp(pull_ops, 1, 123, 0,
                                                  pull_frame,
                                                  sizeof(pull_frame)),
                0, "encode PULL_OPS_RESP should succeed");

  SyncLogOpEntry parsed_pull[2];
  int pull_count = 0;
  long long last_seq = 0;
  int has_more = 1;
  memset(parsed_pull, 0, sizeof(parsed_pull));
  expect_int_eq(net_protocol_parse_pull_ops_resp(pull_frame, parsed_pull, 2,
                                                 &pull_count, &last_seq,
                                                 &has_more),
                0, "parse PULL_OPS_RESP should succeed");
  expect_int_eq(pull_count, 1, "parsed pull ops count");
  expect_int_eq((int)last_seq, 123, "parsed last_global_seq");
  expect_int_eq(has_more, 0, "parsed has_more flag");
  expect_str_eq(parsed_pull[0].op_id, "op-pull", "parsed pull op id");
}

static void test_db_sync_apply_remote_op_and_pull(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_apply_remote", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create db_apply_remote test directory");

  set_test_db_path(case_dir);
  qso_init();

  const char *payload =
      "{\"kind\":\"qso_full\",\"qso_uid\":\"q-remote-1\",\"origin_station_id\":\"st-remote\",\"origin_station_seq\":7,\"last_modified_utc\":\"2026-01-01T01:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0100\",\"call\":\"SP9RMT\",\"freq\":7020,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"hi\",\"exchange_sent\":\"001\",\"exchange_recv\":\"123\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}";

  long long gseq1 = 0;
  expect_true(db_sync_apply_remote_op("op-remote-1", "st-remote", 7, 1,
                                      "QSO_INSERT", "q-remote-1", payload,
                                      "2026-01-01T01:00:00Z", &gseq1) >= 0,
              "apply remote op should succeed");
  expect_int_eq(qso_count, 1,
                "apply remote op should refresh the in-memory logbook immediately");
  expect_str_eq(logbook[0].call, "SP9RMT",
                "remote payload should already be visible in the active log");

  long long gseq_dup = 0;
  expect_int_eq(db_sync_apply_remote_op("op-remote-1", "st-remote", 7, 1,
                                        "QSO_INSERT", "q-remote-1", payload,
                                        "2026-01-01T01:00:00Z", &gseq_dup),
                0, "duplicate remote op should be idempotent");
  expect_true(gseq_dup == gseq1,
              "duplicate apply should return same global_seq");

  long long gseq_conflict = 0;
  expect_int_eq(db_sync_apply_remote_op("op-remote-2", "st-remote", 7, 1,
                                        "QSO_INSERT", "q-remote-2", payload,
                                        "2026-01-01T01:00:01Z",
                                        &gseq_conflict),
                DB_SYNC_APPLY_STATION_SEQ_CONFLICT,
                "same station_id+station_seq with different op_id should conflict");

  qso_init();
  expect_int_eq(qso_count, 1, "remote apply should materialize one QSO");
  expect_str_eq(logbook[0].call, "SP9RMT", "remote payload should set call");

  SyncLogOpEntry pulled[4];
  int pulled_count = 0;
  long long last_pull_seq = 0;
  memset(pulled, 0, sizeof(pulled));
  expect_int_eq(db_sync_pull_ops(0, 10, pulled, 4, &pulled_count,
                                 &last_pull_seq),
                0, "db_sync_pull_ops should succeed");
  expect_int_eq(pulled_count, 1, "db_sync_pull_ops should return one op");
  expect_true(last_pull_seq >= 1,
              "db_sync_pull_ops should return last seq");
  expect_str_eq(pulled[0].op_id, "op-remote-1", "pulled op id matches");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_publish_local_logbook_ops(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/db_publish_local_ops", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create local publisher test directory");

  set_test_db_path(case_dir);
  qso_init();

  QSO current_qso = {0};
  snprintf(current_qso.date, sizeof(current_qso.date), "%s", "20261004");
  snprintf(current_qso.utc, sizeof(current_qso.utc), "%s", "1200");
  snprintf(current_qso.call, sizeof(current_qso.call), "%s", "SP9PUB");
  current_qso.freq = 7020;
  snprintf(current_qso.band, sizeof(current_qso.band), "%s", "40M");
  snprintf(current_qso.mode, sizeof(current_qso.mode), "%s", "CW");
  snprintf(current_qso.rst, sizeof(current_qso.rst), "%s", "599");
  long long current_qso_id = 0;
  expect_int_eq(db_insert_qso(&current_qso, &current_qso_id), 0,
                "insert local QSO with outbox operation");
  expect_int_eq(db_update_qso_invalid(current_qso_id, 1), 0,
                "enqueue local QSO update for publisher");

  QSO legacy_qso = {0};
  snprintf(legacy_qso.date, sizeof(legacy_qso.date), "%s", "20261003");
  snprintf(legacy_qso.utc, sizeof(legacy_qso.utc), "%s", "1100");
  snprintf(legacy_qso.call, sizeof(legacy_qso.call), "%s", "SP9OLD");
  legacy_qso.freq = 7025;
  snprintf(legacy_qso.band, sizeof(legacy_qso.band), "%s", "40M");
  snprintf(legacy_qso.mode, sizeof(legacy_qso.mode), "%s", "CW");
  snprintf(legacy_qso.rst, sizeof(legacy_qso.rst), "%s", "599");
  long long legacy_qso_id = 0;
  expect_int_eq(db_insert_qso(&legacy_qso, &legacy_qso_id), 0,
                "insert QSO to model a pre-sync local record");

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/unit.sqlite3", case_dir);
  sqlite3 *test_db = NULL;
  expect_int_eq(sqlite3_open(db_path, &test_db), SQLITE_OK,
                "open test database to restore legacy QSO metadata");
  if (test_db) {
    sqlite3_stmt *stmt = NULL;
    expect_int_eq(sqlite3_prepare_v2(
                      test_db,
                      "DELETE FROM log_outbox WHERE entity_id = ?;", -1,
                      &stmt, NULL),
                  SQLITE_OK, "prepare removal of legacy QSO outbox row");
    if (stmt) {
      sqlite3_bind_text(stmt, 1, legacy_qso.qso_uid, -1, SQLITE_TRANSIENT);
      expect_int_eq(sqlite3_step(stmt), SQLITE_DONE,
                    "remove legacy QSO outbox row");
      sqlite3_finalize(stmt);
    }
    stmt = NULL;
    expect_int_eq(sqlite3_prepare_v2(
                      test_db,
                      "UPDATE qso SET origin_station_id = '', "
                      "origin_station_seq = 0, last_op_id = '' WHERE id = ?;",
                      -1, &stmt, NULL),
                  SQLITE_OK, "prepare legacy QSO metadata reset");
    if (stmt) {
      sqlite3_bind_int64(stmt, 1, legacy_qso_id);
      expect_int_eq(sqlite3_step(stmt), SQLITE_DONE,
                    "reset legacy QSO synchronization metadata");
      sqlite3_finalize(stmt);
    }
    sqlite3_close(test_db);
  }

  expect_int_eq(db_sync_publish_local_logbook_ops(), 0,
                "publish local outbox and backfill legacy QSO");
  int pending_count = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending_count), 0,
                "read local outbox count after server publication");
  expect_int_eq(pending_count, 0,
                "published server-local operations should be acknowledged");
  SyncLogOpEntry published[8];
  int published_count = 0;
  long long last_global_seq = 0;
  memset(published, 0, sizeof(published));
  expect_int_eq(db_sync_pull_ops(0, 8, published, 8, &published_count,
                                 &last_global_seq),
                0, "read published local operations");
  expect_int_eq(published_count, 3,
                "publisher should include local insert, update and backfill");
  if (published_count == 3) {
    expect_str_eq(published[0].op_type, "QSO_INSERT",
                  "first local operation should be the original QSO");
    expect_str_eq(published[1].op_type, "QSO_INVALID",
                  "local QSO update should be published in sequence");
    expect_str_eq(published[2].op_type, "QSO_INSERT",
                  "legacy QSO should be backfilled as a full insert");
    expect_str_eq(published[2].entity_id, legacy_qso.qso_uid,
                  "backfill should retain the legacy QSO identity");
  }

  expect_int_eq(db_sync_publish_local_logbook_ops(), 0,
                "repeated publication should succeed");
  published_count = 0;
  expect_int_eq(db_sync_pull_ops(0, 8, published, 8, &published_count,
                                 &last_global_seq),
                0, "read operations after repeated publication");
  expect_int_eq(published_count, 3,
                "repeated publication should not create duplicates");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_server_client_roundtrip_apply_pull(const char *tmp_dir) {
  char server_dir[512];
  snprintf(server_dir, sizeof(server_dir), "%s/net_server_roundtrip_server",
           tmp_dir);
  expect_int_eq(mkdir(server_dir, 0777), 0,
                "create server test directory for net server roundtrip");

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  set_test_db_path(server_dir);
  qso_init();
  QSO server_qso = {0};
  snprintf(server_qso.date, sizeof(server_qso.date), "%s", "20261004");
  snprintf(server_qso.utc, sizeof(server_qso.utc), "%s", "1159");
  snprintf(server_qso.call, sizeof(server_qso.call), "%s", "SP9LOCAL");
  server_qso.freq = 7021;
  snprintf(server_qso.band, sizeof(server_qso.band), "%s", "40M");
  snprintf(server_qso.mode, sizeof(server_qso.mode), "%s", "CW");
  snprintf(server_qso.rst, sizeof(server_qso.rst), "%s", "599");
  long long server_qso_id = 0;
  expect_int_eq(db_insert_qso(&server_qso, &server_qso_id), 0,
                "insert a local QSO before starting the server");
  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  config.net_server_port = 19324;
  expect_int_eq(net_sync_start(), 0, "server role sync start");
  usleep(150000);

  int cli = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli >= 0, "client socket should be created for server test");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "client socket should connect to server");

  SyncOutboxEntry op;
  memset(&op, 0, sizeof(op));
  snprintf(op.op_id, sizeof(op.op_id), "%s", "op-server-1");
  op.station_seq = 1;
  op.logbook_id = 1;
  snprintf(op.op_type, sizeof(op.op_type), "%s", "QSO_INSERT");
  snprintf(op.entity_id, sizeof(op.entity_id), "%s", "q-server-1");
  snprintf(op.payload_json, sizeof(op.payload_json), "%s",
           "{\"kind\":\"qso_full\",\"qso_uid\":\"q-server-1\",\"origin_station_id\":\"st-client\",\"origin_station_seq\":1,\"last_modified_utc\":\"2026-01-01T00:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0000\",\"call\":\"SP9CLT\",\"freq\":7020,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"\",\"exchange_sent\":\"001\",\"exchange_recv\":\"123\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}");
  snprintf(op.op_utc, sizeof(op.op_utc), "%s", "2026-01-01T00:00:00Z");

  char frame[8192] = {0};
  expect_int_eq(net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame)), 0,
                "append frame for server test should encode");
  char hello_frame[1024] = {0};
  expect_int_eq(net_protocol_encode_hello("st-client", "logger",
                                          config.net_auth_token, hello_frame,
                                          sizeof(hello_frame)),
                0, "hello frame for server test should encode");
  expect_int_eq(net_protocol_send_framed(cli, hello_frame), 0,
                "hello frame should be sent to server");

  char response[4096] = {0};
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "server should respond with HELLO_ACK");
  expect_true(strstr(response, "\"type\":\"HELLO_ACK\"") != NULL,
              "server hello response type should be HELLO_ACK");

  expect_int_eq(net_protocol_send_framed(cli, frame), 0,
                "append frame should be sent to server");

  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "server should respond with ACK to append");
  expect_true(strstr(response, "\"type\":\"APPEND_ACK\"") != NULL,
              "server append response type should be APPEND_ACK");

  char pull_frame[256] = {0};
  expect_int_eq(net_protocol_encode_pull_ops(0, 10, pull_frame,
                                             sizeof(pull_frame)),
                0, "pull frame for server test should encode");
  expect_int_eq(net_protocol_send_framed(cli, pull_frame), 0,
                "pull frame should be sent to server");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "server should respond to pull");
  expect_true(strstr(response, "\"type\":\"PULL_OPS_RESP\"") != NULL,
              "server pull response type should be PULL_OPS_RESP");
  expect_true(strstr(response, server_qso.qso_uid) != NULL,
              "server pull should include a QSO created at the central station");

  close(cli);

  qso_init();
  expect_int_eq(qso_count, 2,
                "server DB should contain local and synced client QSOs");

  net_sync_stop();

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_server_client_roundtrip_apply_pull_tls(const char *tmp_dir) {
#ifndef HAVE_OPENSSL
  (void)tmp_dir;
  return;
#else
  char server_dir[512];
  snprintf(server_dir, sizeof(server_dir), "%s/net_server_roundtrip_tls_server",
           tmp_dir);
  expect_int_eq(mkdir(server_dir, 0777), 0,
                "create server test directory for TLS net server roundtrip");

  int saved_net_enabled = config.net_enabled;
  int saved_net_tls = config.net_tls;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  set_test_db_path(server_dir);
  qso_init();
  config.net_enabled = 1;
  config.net_tls = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  config.net_server_port = 19325;
  expect_int_eq(net_sync_start(), 0, "server role TLS sync start");
  usleep(200000);

  int cli = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli >= 0, "client TLS socket should be created for server test");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "client TLS socket should connect to server");

  NetTransport transport;
  char transport_error[128] = {0};
  expect_int_eq(net_transport_init_client(&transport, cli, "127.0.0.1", 1,
                                          NULL, transport_error,
                                          sizeof(transport_error)),
                0, "TLS transport client init should succeed");

  char hello_frame[1024] = {0};
  expect_int_eq(net_protocol_encode_hello("st-client-tls", "logger",
                                          config.net_auth_token, hello_frame,
                                          sizeof(hello_frame)),
                0, "TLS hello frame should encode");
  expect_int_eq(net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                            hello_frame),
                0, "TLS hello frame should be sent");

  char response[4096] = {0};
  expect_int_eq(net_protocol_recv_framed_io(&transport, net_transport_read_cb,
                                            response, sizeof(response)),
                0, "TLS server should respond with HELLO_ACK");
  expect_true(strstr(response, "\"type\":\"HELLO_ACK\"") != NULL,
              "TLS server hello response type should be HELLO_ACK");

  SyncOutboxEntry op;
  memset(&op, 0, sizeof(op));
  snprintf(op.op_id, sizeof(op.op_id), "%s", "op-server-tls-1");
  op.station_seq = 1;
  op.logbook_id = 1;
  snprintf(op.op_type, sizeof(op.op_type), "%s", "QSO_INSERT");
  snprintf(op.entity_id, sizeof(op.entity_id), "%s", "q-server-tls-1");
  snprintf(op.payload_json, sizeof(op.payload_json), "%s",
           "{\"kind\":\"qso_full\",\"qso_uid\":\"q-server-tls-1\",\"origin_station_id\":\"st-client-tls\",\"origin_station_seq\":1,\"last_modified_utc\":\"2026-01-01T00:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0000\",\"call\":\"SP9TLS\",\"freq\":7020,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"\",\"exchange_sent\":\"001\",\"exchange_recv\":\"123\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}");
  snprintf(op.op_utc, sizeof(op.op_utc), "%s", "2026-01-01T00:00:00Z");

  char frame[8192] = {0};
  expect_int_eq(net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame)),
                0, "TLS append frame should encode");
  expect_int_eq(net_protocol_send_framed_io(&transport, net_transport_write_cb,
                                            frame),
                0, "TLS append frame should be sent to server");

  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed_io(&transport, net_transport_read_cb,
                                            response, sizeof(response)),
                0, "TLS server should respond with APPEND_ACK");
  expect_true(strstr(response, "\"type\":\"APPEND_ACK\"") != NULL,
              "TLS server append response type should be APPEND_ACK");

  net_transport_close(&transport);

  qso_init();
  expect_int_eq(qso_count, 1,
                "TLS server DB should contain one synced QSO from client");

  net_sync_stop();

  config.net_enabled = saved_net_enabled;
  config.net_tls = saved_net_tls;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  set_test_db_path(tmp_dir);
  qso_init();
#endif
}

static void test_tls_transport_fingerprint_pinning(const char *tmp_dir) {
#ifndef HAVE_OPENSSL
  (void)tmp_dir;
  return;
#else
  char server_dir[512];
  snprintf(server_dir, sizeof(server_dir), "%s/tls_fp_server", tmp_dir);
  expect_int_eq(mkdir(server_dir, 0777), 0,
                "create server directory for TLS fingerprint test");

  int saved_net_enabled = config.net_enabled;
  int saved_net_tls = config.net_tls;
  char saved_role[16];
  int saved_port = config.net_server_port;
  char saved_cert_file[256];
  char saved_key_file[256];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_cert_file, sizeof(saved_cert_file), "%s",
           config.net_tls_cert_file);
  snprintf(saved_key_file, sizeof(saved_key_file), "%s",
           config.net_tls_key_file);

  char cert_path[512];
  char key_path[512];
  join_path(cert_path, sizeof(cert_path), server_dir, "server_cert.pem");
  join_path(key_path, sizeof(key_path), server_dir, "server_key.pem");

  set_test_db_path(server_dir);
  qso_init();
  config.net_enabled = 1;
  config.net_tls = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  snprintf(config.net_tls_cert_file, sizeof(config.net_tls_cert_file), "%s",
           cert_path);
  snprintf(config.net_tls_key_file, sizeof(config.net_tls_key_file), "%s",
           key_path);
  config.net_server_port = 19326;
  expect_int_eq(net_sync_start(), 0, "TLS fingerprint server start");
  usleep(200000);

  int cli1 = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli1 >= 0, "first TLS fingerprint client socket created");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect_int_eq(connect(cli1, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "first TLS fingerprint client connected");

  NetTransport transport1;
  char error_text[128] = {0};
  expect_int_eq(net_transport_init_client(&transport1, cli1, "127.0.0.1", 1,
                                          NULL, error_text,
                                          sizeof(error_text)),
                0, "first TLS fingerprint handshake succeeds");
  struct stat key_stat;
  expect_int_eq(stat(key_path, &key_stat), 0,
                "stat generated TLS private key");
  expect_int_eq(key_stat.st_mode & 0777, 0600,
                "generated TLS private key should be owner-only");
  const char *fp = net_transport_peer_fingerprint(&transport1);
  expect_true(fp != NULL && fp[0] != 0,
              "first TLS handshake should expose peer fingerprint");
  net_transport_close(&transport1);

  int cli2 = socket(AF_INET, SOCK_STREAM, 0);
  expect_int_eq(chmod(key_path, 0644), 0,
                "make existing private key permissive for repair test");
  expect_true(cli2 >= 0, "second TLS fingerprint client socket created");
  expect_int_eq(connect(cli2, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "second TLS fingerprint client connected");

  NetTransport transport2;
  memset(&transport2, 0, sizeof(transport2));
  expect_int_eq(net_transport_init_client(&transport2, cli2, "127.0.0.1", 1,
                                          "00:11:22", error_text,
                                          sizeof(error_text)),
                -1, "TLS handshake should fail on fingerprint mismatch");
  expect_int_eq(stat(key_path, &key_stat), 0,
                "stat TLS private key after secure load");
  expect_int_eq(key_stat.st_mode & 0777, 0600,
                "TLS load should repair permissive private-key mode");
  close(cli2);

  int cli3 = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli3 >= 0, "third TLS fingerprint client socket created");
  expect_int_eq(connect(cli3, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "third TLS fingerprint client connected");

  NetTransport transport3;
  expect_int_eq(net_transport_init_client(&transport3, cli3, "127.0.0.1", 1,
                                          fp, error_text,
                                          sizeof(error_text)),
                0, "TLS handshake should pass on pinned fingerprint");
  net_transport_close(&transport3);

  net_sync_stop();
  config.net_enabled = saved_net_enabled;
  config.net_tls = saved_net_tls;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_tls_cert_file, sizeof(config.net_tls_cert_file), "%s",
           saved_cert_file);
  snprintf(config.net_tls_key_file, sizeof(config.net_tls_key_file), "%s",
           saved_key_file);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
#endif
}

static void test_net_server_rate_limit(const char *tmp_dir) {
  char server_dir[512];
  snprintf(server_dir, sizeof(server_dir), "%s/net_server_rate_limit", tmp_dir);
  expect_int_eq(mkdir(server_dir, 0777), 0,
                "create server directory for rate limit test");

  int saved_net_enabled = config.net_enabled;
  int saved_window = config.net_rate_limit_window_sec;
  int saved_burst = config.net_rate_limit_burst;
  int saved_port = config.net_server_port;
  char saved_role[16];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);

  set_test_db_path(server_dir);
  qso_init();
  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  config.net_server_port = 19327;
  config.net_rate_limit_window_sec = 10;
  config.net_rate_limit_burst = 1;
  expect_int_eq(net_sync_start(), 0, "server role sync start for rate limit");
  usleep(150000);

  int cli = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli >= 0, "client socket should be created for rate limit test");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "client socket should connect for rate limit test");

  char hello_frame[1024] = {0};
  expect_int_eq(net_protocol_encode_hello("st-rate", "logger",
                                          config.net_auth_token, hello_frame,
                                          sizeof(hello_frame)),
                0, "hello frame for rate limit should encode");
  expect_int_eq(net_protocol_send_framed(cli, hello_frame), 0,
                "hello frame should be sent for rate limit");

  char response[4096] = {0};
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "server should respond with HELLO_ACK for rate limit test");

  char pull_frame[256] = {0};
  expect_int_eq(net_protocol_encode_pull_ops(0, 10, pull_frame,
                                             sizeof(pull_frame)),
                0, "pull frame should encode for rate limit test");
  expect_int_eq(net_protocol_send_framed(cli, pull_frame), 0,
                "first pull frame should be sent for rate limit");
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "first pull response should be received before limit");
  expect_true(strstr(response, "\"type\":\"PULL_OPS_RESP\"") != NULL,
              "first response should be pull response");

  expect_int_eq(net_protocol_send_framed(cli, pull_frame), 0,
                "second pull frame should be sent for rate limit");
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "second response should be received for rate limit");
  expect_true(strstr(response, "\"code\":\"RATE_LIMIT\"") != NULL,
              "second response should hit rate limit");

  close(cli);

  for (int strike = 2; strike <= 3; strike++) {
    cli = socket(AF_INET, SOCK_STREAM, 0);
    expect_true(cli >= 0, "reconnecting client socket should be created");
    expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                  "reconnecting client should connect before blacklist");
    expect_int_eq(net_protocol_send_framed(cli, hello_frame), 0,
                  "reconnecting client should send HELLO");
    expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                  "server should authenticate reconnecting client");
    expect_int_eq(net_protocol_send_framed(cli, pull_frame), 0,
                  "reconnecting client should send first pull");
    expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                  "reconnecting client first pull should pass");
    expect_int_eq(net_protocol_send_framed(cli, pull_frame), 0,
                  "reconnecting client should send over-limit pull");
    expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                  "reconnecting client should receive rate-limit error");
    expect_true(strstr(response, "\"code\":\"RATE_LIMIT\"") != NULL,
                "reconnecting client should incur another rate-limit strike");
    close(cli);
  }

  NetServerMetrics metrics;
  net_server_get_metrics(&metrics);
  expect_true(metrics.accepted_connections >= 3,
              "server metrics should count accepted client connections");
  expect_int_eq((int)metrics.requests, 3,
                "server metrics should count allowed requests");
  expect_int_eq((int)metrics.rate_limit_rejections, 3,
                "server metrics should count rate-limit rejections");
  expect_int_eq(metrics.blacklisted_ips, 1,
                "repeated rate-limit violations should blacklist peer IP");

  cli = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli >= 0, "blacklisted client socket should be created");
  expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "blacklisted peer TCP connect should reach listener");
  expect_int_eq(net_protocol_send_framed(cli, hello_frame), 0,
                "blacklisted peer can attempt HELLO before server close");
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), -1,
                "server should close a blacklisted peer before HELLO");
  close(cli);
  net_server_get_metrics(&metrics);
  expect_int_eq(metrics.blacklist_rejections, 1,
                "server metrics should count blacklist rejections");

  net_sync_stop();
  config.net_enabled = saved_net_enabled;
  config.net_rate_limit_window_sec = saved_window;
  config.net_rate_limit_burst = saved_burst;
  config.net_server_port = saved_port;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_fault_drop_append_ack_retries(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_drop_ack", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create net_sync_drop_ack test directory");

  set_test_db_path(case_dir);
  qso_init();

  char status[128] = {0};
  expect_true(qso_add_fields("SP9DROP", 7020, "599", "CW", "", status,
                             sizeof(status)) >= 0,
              "drop-ack test should create one QSO");

  SyncOutboxEntry ops[4];
  int ops_count = 0;
  memset(ops, 0, sizeof(ops));
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "outbox should load before drop-ack test");
  expect_true(ops_count > 0, "drop-ack test should have pending op");

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19328;

  MockSyncServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  server.mode = MOCK_SYNC_MODE_DROP_APPEND_ACK;

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "mock drop-ack server thread should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "net sync should start for drop-ack test");
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync poll should complete despite dropped append ack");
  net_sync_stop();
  pthread_join(tid, NULL);

  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after dropped ack");
  expect_int_eq(pending, 1,
                "dropped append ack should keep one operation pending");

  usleep(2200000);

  memset(ops, 0, sizeof(ops));
  ops_count = 0;
  expect_int_eq(db_sync_outbox_load_pending(ops, 4, &ops_count), 0,
                "pending outbox should reload after dropped ack retry");
  expect_int_eq(ops_count, 1,
                "dropped append ack should leave one retryable op");

  config.net_server_port = 19329;
  MockSyncServerArgs retry_server;
  memset(&retry_server, 0, sizeof(retry_server));
  retry_server.port = config.net_server_port;
  retry_server.ack_last_global_seq = 4401;
  snprintf(retry_server.acked_json, sizeof(retry_server.acked_json), "\"%s\"",
           ops[0].op_id);

  pthread_t tid2;
  expect_int_eq(pthread_create(&tid2, NULL, mock_sync_server_thread,
                               &retry_server),
                0, "mock retry server thread should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "net sync should restart after dropped ack");
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync retry should drain outbox after dropped ack");
  net_sync_stop();
  pthread_join(tid2, NULL);

  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "dropped ack retry should clear pending ops");
  expect_int_eq(pending, 0,
                "outbox should drain after successful retry");

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_fault_delayed_pull_response(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_delay_pull", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create delayed pull test directory");

  set_test_db_path(case_dir);
  qso_init();

  int saved_net_enabled = config.net_enabled;
  int saved_heartbeat = config.net_heartbeat_sec;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  config.net_heartbeat_sec = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19330;

  MockSyncServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  server.mode = MOCK_SYNC_MODE_DELAY_PULL_RESP;
  server.delay_sec = 2;

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "mock delayed-pull server should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0,
                "net sync should start for delayed pull response test");
  expect_int_eq(net_sync_poll_once(), -1,
                "net sync poll should fail on delayed pull response");
  net_sync_stop();
  pthread_join(tid, NULL);

  NetSyncStatus st;
  memset(&st, 0, sizeof(st));
  net_sync_get_status(&st);
  expect_true(!st.connected,
              "delayed pull response should leave sync disconnected");
  expect_true(st.failure_streak >= 1,
              "delayed pull response should increment failure streak");

  config.net_enabled = saved_net_enabled;
  config.net_heartbeat_sec = saved_heartbeat;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_sync_fault_truncated_pull_response(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_sync_truncated_pull", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create truncated-pull fault test directory");

  set_test_db_path(case_dir);
  qso_init();
  char status[128] = {0};
  expect_true(qso_add_fields("SP9TRUNC", 7020, "599", "CW", "", status,
                             sizeof(status)) >= 0,
              "truncated-pull test should queue a local QSO");

  const int saved_net_enabled = config.net_enabled;
  const int saved_net_tls = config.net_tls;
  const int saved_allow_insecure = config.net_allow_insecure_lan;
  char saved_role[sizeof(config.net_role)];
  char saved_host[sizeof(config.net_server_host)];
  const int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  config.net_tls = 0;
  config.net_allow_insecure_lan = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19331;

  MockSyncServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.net_server_port;
  server.mode = MOCK_SYNC_MODE_TRUNCATE_PULL_RESP;
  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "truncated-pull mock server should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0,
                "net sync should start for truncated-pull test");
  expect_int_eq(net_sync_poll_once(), -1,
                "truncated framed response should fail the poll");
  net_sync_stop();
  pthread_join(tid, NULL);

  int pending = 0;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending outbox should remain readable after truncated frame");
  expect_int_eq(pending, 1,
                "truncated response must not discard an unacknowledged QSO");

  config.net_enabled = saved_net_enabled;
  config.net_tls = saved_net_tls;
  config.net_allow_insecure_lan = saved_allow_insecure;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static int send_shared_log_mismatch_request(int port, const char *request_type) {
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd < 0)
    return -1;

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(client_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(client_fd);
    return -1;
  }

  char frame[4096] = {0};
  char response[4096] = {0};
  char station_id[32] = {0};
  snprintf(station_id, sizeof(station_id), "st-mismatch-%s", request_type);
  if (net_protocol_encode_hello(station_id, "logger", config.net_auth_token,
                                frame, sizeof(frame)) != 0 ||
      net_protocol_send_framed(client_fd, frame) != 0 ||
      net_protocol_recv_framed(client_fd, response, sizeof(response)) != 0 ||
      !strstr(response, "\"accepted\":true")) {
    close(client_fd);
    return -1;
  }

  char saved_shared_log_id[36] = {0};
  snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
           config.net_shared_log_id);
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           "sl-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  int encode_rc = -1;
  if (strcmp(request_type, "APPEND_OPS") == 0) {
    SyncOutboxEntry op;
    memset(&op, 0, sizeof(op));
    snprintf(op.op_id, sizeof(op.op_id), "%s", "op-mismatch-append");
    op.station_seq = 1;
    op.logbook_id = 1;
    snprintf(op.op_type, sizeof(op.op_type), "%s", "NOOP");
    snprintf(op.entity_id, sizeof(op.entity_id), "%s", "mismatch-entity");
    snprintf(op.payload_json, sizeof(op.payload_json), "%s", "{}");
    snprintf(op.op_utc, sizeof(op.op_utc), "%s", "2026-10-04T12:00:00Z");
    encode_rc = net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame));
  } else if (strcmp(request_type, "CATCHUP_REQUEST") == 0) {
    encode_rc = net_protocol_encode_catchup_request(0, 8, frame,
                                                   sizeof(frame));
  } else if (strcmp(request_type, "RESERVE_SERIAL") == 0) {
    encode_rc = net_protocol_encode_reserve_serial("req-mismatch", 1, 120,
                                                   frame, sizeof(frame));
  }
  snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
           saved_shared_log_id);

  int rc = -1;
  if (encode_rc == 0 && net_protocol_send_framed(client_fd, frame) == 0 &&
      net_protocol_recv_framed(client_fd, response, sizeof(response)) == 0 &&
      strstr(response, "SHARED_LOG_MISMATCH"))
    rc = 0;
  close(client_fd);
  return rc;
}

static void test_net_server_pages_and_station_sequence_gaps(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_server_pages_and_gaps",
           tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create server paging and sequence-gap test directory");
  set_test_db_path(case_dir);
  qso_init();

  for (int i = 1; i <= 65; i++) {
    char op_id[64] = {0};
    char entity_id[64] = {0};
    snprintf(op_id, sizeof(op_id), "op-page-source-%d", i);
    snprintf(entity_id, sizeof(entity_id), "entity-page-source-%d", i);
    long long global_seq = 0;
    expect_true(db_sync_apply_remote_op(
                    op_id, "st-page-source", i, 1, "NOOP", entity_id, "{}",
                    "2026-10-04T12:00:00Z", &global_seq) >= 0,
                "seed server operations for multiple catch-up pages");
  }

  int saved_net_enabled = config.net_enabled;
  int saved_port = config.net_server_port;
  char saved_role[16];
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  config.net_server_port = 19330;
  expect_int_eq(net_sync_start(), 0, "start server for paging and gap test");
  usleep(120000);

    long long before_mismatch_global_seq = 0;
    int before_mismatch_serial = 0;
    expect_int_eq(db_sync_get_max_global_seq(&before_mismatch_global_seq), 0,
          "read server sequence before shared-log mismatch probes");
    expect_int_eq(db_sync_peek_next_serial(1, &before_mismatch_serial), 0,
          "read serial counter before shared-log mismatch probes");
    expect_true(send_shared_log_mismatch_request(
            config.net_server_port, "APPEND_OPS") == 0,
          "server should reject APPEND with another shared_log_id");
    expect_true(send_shared_log_mismatch_request(
            config.net_server_port, "CATCHUP_REQUEST") == 0,
          "server should reject CATCHUP with another shared_log_id");
    expect_true(send_shared_log_mismatch_request(
            config.net_server_port, "RESERVE_SERIAL") == 0,
          "server should reject serial reservation with another shared_log_id");
    long long after_mismatch_global_seq = 0;
    int after_mismatch_serial = 0;
    expect_int_eq(db_sync_get_max_global_seq(&after_mismatch_global_seq), 0,
          "read server sequence after shared-log mismatch probes");
    expect_int_eq(db_sync_peek_next_serial(1, &after_mismatch_serial), 0,
          "read serial counter after shared-log mismatch probes");
    expect_true(after_mismatch_global_seq == before_mismatch_global_seq,
          "mismatched APPEND must not apply or allocate a global sequence");
    expect_true(after_mismatch_serial == before_mismatch_serial,
          "mismatched reservation must not advance central numbering");

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)config.net_server_port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  char canonical_shared_log_id[36] = {0};
  snprintf(canonical_shared_log_id, sizeof(canonical_shared_log_id), "%s",
           config.net_shared_log_id);
  const char *wrong_shared_log_id = "sl-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  int mismatch_hello_fd = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(mismatch_hello_fd >= 0,
              "create mismatched shared-ID HELLO socket");
  if (mismatch_hello_fd >= 0) {
    expect_int_eq(connect(mismatch_hello_fd, (struct sockaddr *)&address,
                          sizeof(address)),
                  0, "connect mismatched shared-ID HELLO socket");
    char saved_shared_log_id[36] = {0};
    snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
             config.net_shared_log_id);
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
             wrong_shared_log_id);
    char mismatch_frame[1024] = {0};
    char mismatch_response[2048] = {0};
    expect_int_eq(net_protocol_encode_hello(
                      "st-wrong-log", "logger", config.net_auth_token,
                      mismatch_frame, sizeof(mismatch_frame)),
                  0, "encode HELLO with another shared log");
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
             saved_shared_log_id);
    expect_int_eq(net_protocol_send_framed(mismatch_hello_fd, mismatch_frame),
                  0, "send mismatched shared-ID HELLO");
    expect_int_eq(net_protocol_recv_framed(mismatch_hello_fd,
                                           mismatch_response,
                                           sizeof(mismatch_response)),
                  0, "receive mismatched shared-ID HELLO rejection");
    expect_true(strstr(mismatch_response, "\"accepted\":false") != NULL,
                "server should reject HELLO for another shared log");
    close(mismatch_hello_fd);
  }

  int unauthenticated_fd = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(unauthenticated_fd >= 0,
              "create unauthenticated protocol test socket");
  if (unauthenticated_fd >= 0) {
    expect_int_eq(connect(unauthenticated_fd,
                          (struct sockaddr *)&address, sizeof(address)),
                  0, "connect unauthenticated protocol test socket");
    char unauthenticated_frame[1024] = {0};
    char unauthenticated_response[2048] = {0};
    expect_int_eq(net_protocol_encode_hello("st-no-token", "logger", "",
                                            unauthenticated_frame,
                                            sizeof(unauthenticated_frame)),
                  0, "encode empty-token HELLO");
    expect_int_eq(net_protocol_send_framed(unauthenticated_fd,
                                           unauthenticated_frame),
                  0, "send empty-token HELLO");
    expect_int_eq(net_protocol_recv_framed(unauthenticated_fd,
                                           unauthenticated_response,
                                           sizeof(unauthenticated_response)),
                  0, "receive empty-token HELLO rejection");
    expect_true(strstr(unauthenticated_response, "\"accepted\":false") !=
                    NULL,
                "server should explicitly reject an empty authentication token");
    close(unauthenticated_fd);
  }

  int mismatch_request_fd = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(mismatch_request_fd >= 0,
              "create mismatched shared-ID request socket");
  if (mismatch_request_fd >= 0) {
    expect_int_eq(connect(mismatch_request_fd, (struct sockaddr *)&address,
                          sizeof(address)),
                  0, "connect mismatched shared-ID request socket");
    char mismatch_frame[1024] = {0};
    char mismatch_response[2048] = {0};
    expect_int_eq(net_protocol_encode_hello(
                      "st-mismatch-request", "logger", config.net_auth_token,
                      mismatch_frame, sizeof(mismatch_frame)),
                  0, "encode valid HELLO for mismatch request test");
    expect_int_eq(net_protocol_send_framed(mismatch_request_fd,
                                           mismatch_frame),
                  0, "send valid HELLO for mismatch request test");
    expect_int_eq(net_protocol_recv_framed(mismatch_request_fd,
                                           mismatch_response,
                                           sizeof(mismatch_response)),
                  0, "receive valid HELLO_ACK before mismatch request");
    char saved_shared_log_id[36] = {0};
    snprintf(saved_shared_log_id, sizeof(saved_shared_log_id), "%s",
             config.net_shared_log_id);
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
             wrong_shared_log_id);
    expect_int_eq(net_protocol_encode_reserve_serial(
                      "req-wrong-shared-log", 1, 120, mismatch_frame,
                      sizeof(mismatch_frame)),
                  0, "encode serial reservation for another shared log");
    snprintf(config.net_shared_log_id, sizeof(config.net_shared_log_id), "%s",
             saved_shared_log_id);
    expect_int_eq(net_protocol_send_framed(mismatch_request_fd,
                                           mismatch_frame),
                  0, "send mismatched shared-ID reservation request");
    memset(mismatch_response, 0, sizeof(mismatch_response));
    expect_int_eq(net_protocol_recv_framed(mismatch_request_fd,
                                           mismatch_response,
                                           sizeof(mismatch_response)),
                  0, "receive shared-ID reservation rejection");
    expect_true(strstr(mismatch_response,
                       "SHARED_LOG_MISMATCH") != NULL,
                "server should reject mismatched ID before serial reservation");
    close(mismatch_request_fd);
  }

  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(client_fd >= 0, "create direct protocol test socket");
  if (client_fd < 0)
    goto server_pages_cleanup;

  struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  expect_int_eq(connect(client_fd, (struct sockaddr *)&address, sizeof(address)),
                0, "connect direct protocol test socket");

  char frame[16384] = {0};
  char response[16384] = {0};
  expect_int_eq(net_protocol_encode_hello(
                    "st-page-reader", "logger", config.net_auth_token, frame,
                    sizeof(frame)),
                0, "encode HELLO for server page test");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send HELLO for server page test");
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive HELLO_ACK for server page test");

  expect_int_eq(net_protocol_encode_catchup_request(0, 64, frame,
                                                    sizeof(frame)),
                0, "encode first page request");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send first page request");
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive first catch-up page");
  SyncLogOpEntry page_ops[64];
  int page_count = 0;
  int has_more = 0;
  long long page_last_seq = 0;
  memset(page_ops, 0, sizeof(page_ops));
  expect_int_eq(net_protocol_parse_pull_ops_resp(
                    response, page_ops, 64, &page_count, &page_last_seq,
                    &has_more),
                0, "parse first catch-up page");
  expect_int_eq(page_count, 64, "first catch-up page should fill its limit");
  expect_true(has_more, "server should report more operations after page one");
  expect_true(page_last_seq == 64,
              "first page cursor should identify its final returned operation");

  expect_int_eq(net_protocol_encode_catchup_request(64, 64, frame,
                                                    sizeof(frame)),
                0, "encode second page request");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send second page request");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive second catch-up page");
  memset(page_ops, 0, sizeof(page_ops));
  page_count = 0;
  has_more = 1;
  page_last_seq = 0;
  expect_int_eq(net_protocol_parse_pull_ops_resp(
                    response, page_ops, 64, &page_count, &page_last_seq,
                    &has_more),
                0, "parse second catch-up page");
  expect_int_eq(page_count, 1, "second catch-up page should contain the tail");
  expect_true(!has_more && page_last_seq == 65,
              "final catch-up page should end at the current operation tail");

  SyncOutboxEntry append_op;
  memset(&append_op, 0, sizeof(append_op));
  append_op.logbook_id = 1;
  snprintf(append_op.op_type, sizeof(append_op.op_type), "%s", "NOOP");
  snprintf(append_op.op_utc, sizeof(append_op.op_utc), "%s",
           "2026-10-04T12:01:00Z");
  snprintf(append_op.entity_id, sizeof(append_op.entity_id), "%s",
           "entity-gap-2");
  snprintf(append_op.payload_json, sizeof(append_op.payload_json), "%s", "{}");
  snprintf(append_op.op_id, sizeof(append_op.op_id), "%s", "op-gap-2");
  append_op.station_seq = 2;
  expect_int_eq(net_protocol_encode_append_ops(&append_op, 1, frame,
                                               sizeof(frame)),
                0, "encode out-of-order station sequence");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send out-of-order station sequence");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive sequence-gap rejection");
  expect_true(strstr(response, "\"code\":\"SEQ_GAP\"") != NULL,
              "server should explicitly reject an operation past a sequence gap");

  append_op.station_seq = 1;
  snprintf(append_op.op_id, sizeof(append_op.op_id), "%s", "op-gap-1");
  snprintf(append_op.entity_id, sizeof(append_op.entity_id), "%s",
           "entity-gap-1");
  expect_int_eq(net_protocol_encode_append_ops(&append_op, 1, frame,
                                               sizeof(frame)),
                0, "encode missing station sequence");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send missing station sequence");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive missing sequence acceptance");
  expect_true(strstr(response, "\"op-gap-1\"") != NULL,
              "server should accept the missing lower sequence");

  append_op.station_seq = 2;
  snprintf(append_op.op_id, sizeof(append_op.op_id), "%s", "op-gap-2");
  snprintf(append_op.entity_id, sizeof(append_op.entity_id), "%s",
           "entity-gap-2");
  expect_int_eq(net_protocol_encode_append_ops(&append_op, 1, frame,
                                               sizeof(frame)),
                0, "encode retry after sequence gap recovery");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "send retry after sequence gap recovery");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive sequence gap retry acceptance");
  expect_true(strstr(response, "\"op-gap-2\"") != NULL,
              "server should accept the retried operation after the gap is filled");

  long long max_seq_before_duplicate = 0;
  expect_int_eq(db_sync_get_max_global_seq(&max_seq_before_duplicate), 0,
                "read operation high-water mark before duplicate append");
  expect_int_eq(net_protocol_send_framed(client_fd, frame), 0,
                "resend accepted operation to test idempotency");
  memset(response, 0, sizeof(response));
  expect_int_eq(net_protocol_recv_framed(client_fd, response,
                                         sizeof(response)),
                0, "receive duplicate operation acknowledgement");
  long long max_seq_after_duplicate = 0;
  expect_int_eq(db_sync_get_max_global_seq(&max_seq_after_duplicate), 0,
                "read operation high-water mark after duplicate append");
  expect_true(max_seq_after_duplicate == max_seq_before_duplicate,
              "repeated append should not create a second log operation");

    char reserve_frame[1024] = {0};
    char reserve_response[2048] = {0};
    expect_int_eq(net_protocol_encode_reserve_serial(
            "req-reserve-idempotent", 3, 120, reserve_frame,
            sizeof(reserve_frame)),
          0, "encode idempotent serial reservation request");
    expect_int_eq(net_protocol_send_framed(client_fd, reserve_frame), 0,
          "send first serial reservation request");
    expect_int_eq(net_protocol_recv_framed(client_fd, reserve_response,
                       sizeof(reserve_response)),
          0, "receive first serial reservation ACK");
    char reserve_ack_request_id[64] = {0};
    char first_reservation_id[64] = {0};
    int first_reserved_serial = 0;
    char first_reservation_expiry[32] = {0};
    expect_int_eq(net_protocol_parse_reserve_serial_ack(
                      reserve_response, reserve_ack_request_id,
                      sizeof(reserve_ack_request_id), first_reservation_id,
            sizeof(first_reservation_id), &first_reserved_serial,
            first_reservation_expiry,
            sizeof(first_reservation_expiry)),
          0, "parse first reservation ACK");

    memset(reserve_response, 0, sizeof(reserve_response));
    expect_int_eq(net_protocol_send_framed(client_fd, reserve_frame), 0,
          "retry serial reservation after a simulated lost ACK");
    expect_int_eq(net_protocol_recv_framed(client_fd, reserve_response,
                       sizeof(reserve_response)),
          0, "receive idempotent reservation retry ACK");
    char retry_reservation_id[64] = {0};
    int retry_reserved_serial = 0;
    char retry_reservation_expiry[32] = {0};
    expect_int_eq(net_protocol_parse_reserve_serial_ack(
                      reserve_response, reserve_ack_request_id,
                      sizeof(reserve_ack_request_id), retry_reservation_id,
            sizeof(retry_reservation_id), &retry_reserved_serial,
            retry_reservation_expiry,
            sizeof(retry_reservation_expiry)),
          0, "parse retried reservation ACK");
    expect_str_eq(retry_reservation_id, first_reservation_id,
          "retry should return the original reservation ID");
    expect_int_eq(retry_reserved_serial, first_reserved_serial,
          "retry should return the original serial number");
    expect_str_eq(retry_reservation_expiry, first_reservation_expiry,
          "retry should return the original reservation expiry");
    int next_reserved_serial = 0;
    expect_int_eq(db_sync_peek_next_serial(1, &next_reserved_serial), 0,
          "read central counter after repeated reservation request");
    expect_true(next_reserved_serial == first_reserved_serial + 1,
          "retry must not consume a second central serial number");

  close(client_fd);

server_pages_cleanup:
  net_sync_stop();
  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_server_duplicate_append_is_idempotent(const char *tmp_dir) {
  char server_dir[512];
  snprintf(server_dir, sizeof(server_dir), "%s/net_server_duplicate_append",
           tmp_dir);
  expect_int_eq(mkdir(server_dir, 0777), 0,
                "create duplicate append test directory");

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  set_test_db_path(server_dir);
  qso_init();
  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "server");
  config.net_server_port = 19331;
  expect_int_eq(net_sync_start(), 0, "duplicate append server start");
  usleep(150000);

  int cli = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli >= 0, "client socket should be created for duplicate test");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)config.net_server_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect_int_eq(connect(cli, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "client socket should connect for duplicate test");

  char hello_frame[1024] = {0};
  expect_int_eq(net_protocol_encode_hello("st-dup", "logger",
                                          config.net_auth_token, hello_frame,
                                          sizeof(hello_frame)),
                0, "hello frame for duplicate test should encode");
  expect_int_eq(net_protocol_send_framed(cli, hello_frame), 0,
                "hello frame should be sent for duplicate test");

  char response[4096] = {0};
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "duplicate test should receive HELLO_ACK");

  SyncOutboxEntry op;
  memset(&op, 0, sizeof(op));
  snprintf(op.op_id, sizeof(op.op_id), "%s", "op-dup-1");
  op.station_seq = 1;
  op.logbook_id = 1;
  snprintf(op.op_type, sizeof(op.op_type), "%s", "QSO_INSERT");
  snprintf(op.entity_id, sizeof(op.entity_id), "%s", "q-dup-1");
  snprintf(op.payload_json, sizeof(op.payload_json), "%s",
           "{\"kind\":\"qso_full\",\"qso_uid\":\"q-dup-1\",\"origin_station_id\":\"st-dup\",\"origin_station_seq\":1,\"last_modified_utc\":\"2026-01-01T00:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0000\",\"call\":\"SP9DUP\",\"freq\":7020,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"\",\"exchange_sent\":\"001\",\"exchange_recv\":\"123\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}");
  snprintf(op.op_utc, sizeof(op.op_utc), "%s", "2026-01-01T00:00:00Z");

  char frame[8192] = {0};
  expect_int_eq(net_protocol_encode_append_ops(&op, 1, frame, sizeof(frame)),
                0, "duplicate append frame should encode");
  expect_int_eq(net_protocol_send_framed(cli, frame), 0,
                "first duplicate append frame should send");
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "first duplicate append should ack");
  expect_int_eq(net_protocol_send_framed(cli, frame), 0,
                "second duplicate append frame should send");
  expect_int_eq(net_protocol_recv_framed(cli, response, sizeof(response)), 0,
                "second duplicate append should ack");

  close(cli);
  qso_init();
  expect_int_eq(qso_count, 1,
                "duplicate append should still materialize one QSO");

  net_sync_stop();
  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;
  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_qso_uid_conflict_is_rejected(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/qso_uid_conflict_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create qso_uid conflict test directory");

  set_test_db_path(case_dir);
  qso_init();

  const char *payload_a =
      "{\"kind\":\"qso_full\",\"qso_uid\":\"q-conflict-1\",\"origin_station_id\":\"st-a\",\"origin_station_seq\":5,\"last_modified_utc\":\"2026-01-01T01:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0100\",\"call\":\"SP9A\",\"freq\":7020,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"one\",\"exchange_sent\":\"001\",\"exchange_recv\":\"123\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}";

  const char *payload_b =
      "{\"kind\":\"qso_full\",\"qso_uid\":\"q-conflict-1\",\"origin_station_id\":\"st-b\",\"origin_station_seq\":9,\"last_modified_utc\":\"2026-01-01T02:00:00Z\",\"version\":1,\"date\":\"20260101\",\"utc\":\"0200\",\"call\":\"SP9B\",\"freq\":7021,\"band\":\"40M\",\"mode\":\"CW\",\"rst\":\"599\",\"comments\":\"two\",\"exchange_sent\":\"002\",\"exchange_recv\":\"124\",\"operator_mode\":\"RUN\",\"contest_id\":\"CQWW\",\"radio_nr\":1,\"points\":3,\"country\":\"POLAND\",\"cq_zone\":15,\"itu_zone\":28,\"invalid\":false}";

  long long gseq_a = 0;
  expect_true(db_sync_apply_remote_op("op-qso-conflict-a", "st-a", 5, 1,
                                      "QSO_INSERT", "q-conflict-1", payload_a,
                                      "2026-01-01T01:00:00Z", &gseq_a) >= 0,
              "first remote QSO should apply successfully");

  long long gseq_b = 0;
  expect_int_eq(db_sync_apply_remote_op("op-qso-conflict-b", "st-b", 9, 1,
                                        "QSO_INSERT", "q-conflict-1", payload_b,
                                        "2026-01-01T02:00:00Z", &gseq_b),
                DB_SYNC_APPLY_QSO_UID_CONFLICT,
                "same qso_uid on different station should be rejected");

  qso_init();
  expect_int_eq(qso_count, 1, "conflicting qso_uid should not create duplicate row");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_db_sync_serial_reservation_and_commit(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/serial_reservation_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create serial reservation test directory");

  set_test_db_path(case_dir);
  qso_init();

  QSO existing_qso = {0};
  snprintf(existing_qso.date, sizeof(existing_qso.date), "%s", "20261004");
  snprintf(existing_qso.utc, sizeof(existing_qso.utc), "%s", "1200");
  snprintf(existing_qso.call, sizeof(existing_qso.call), "%s", "SP9SERIAL");
  existing_qso.freq = 7020;
  snprintf(existing_qso.band, sizeof(existing_qso.band), "%s", "40M");
  snprintf(existing_qso.mode, sizeof(existing_qso.mode), "%s", "CW");
  snprintf(existing_qso.rst, sizeof(existing_qso.rst), "%s", "599");
  snprintf(existing_qso.exchange_sent, sizeof(existing_qso.exchange_sent),
           "%s", "42");
  snprintf(existing_qso.country, sizeof(existing_qso.country), "%s", "POLAND");
  existing_qso.cq_zone = 15;
  existing_qso.itu_zone = 28;
  long long existing_qso_id = 0;
  expect_int_eq(db_insert_qso(&existing_qso, &existing_qso_id), 0,
                "seed existing log before central serial initialization");

  int next_serial = 0;
  expect_int_eq(db_sync_peek_next_serial(1, &next_serial), 0,
                "peek next central serial from existing log");
  expect_int_eq(next_serial, 43,
                "central serial should start above existing exchange values");

  char pending_request_id[64] = {0};
  expect_int_eq(db_sync_get_or_create_serial_request_id(
                    "req-persist-a", pending_request_id,
                    sizeof(pending_request_id)),
                0, "create durable pending serial request ID");
  expect_str_eq(pending_request_id, "req-persist-a",
                "first reservation attempt should store its request ID");
  char retried_request_id[64] = {0};
  expect_int_eq(db_sync_get_or_create_serial_request_id(
                    "req-persist-b", retried_request_id,
                    sizeof(retried_request_id)),
                0, "read durable serial request ID on retry");
  expect_str_eq(retried_request_id, "req-persist-a",
                "retry should reuse the unacknowledged request ID");
  db_shutdown();
  expect_int_eq(db_init(), 0,
                "reopen DB while a serial request is waiting for ACK");
  memset(retried_request_id, 0, sizeof(retried_request_id));
  expect_int_eq(db_sync_get_or_create_serial_request_id(
                    "req-persist-b", retried_request_id,
                    sizeof(retried_request_id)),
                0, "recover the request ID after process restart");
  expect_str_eq(retried_request_id, "req-persist-a",
                "restart should retain the in-flight request ID");
  expect_int_eq(db_sync_clear_serial_request_id("req-persist-a"), 0,
                "clear request ID after its reservation ACK is persisted");
  memset(retried_request_id, 0, sizeof(retried_request_id));
  expect_int_eq(db_sync_get_or_create_serial_request_id(
                    "req-persist-b", retried_request_id,
                    sizeof(retried_request_id)),
                0, "create next request ID after previous ACK");
  expect_str_eq(retried_request_id, "req-persist-b",
                "next reservation should receive a fresh request ID");
  expect_int_eq(db_sync_clear_serial_request_id("req-persist-b"), 0,
                "clear second pending request after test ACK");

  char reservation_id[64] = {0};
  char second_reservation_id[64] = {0};
  int serial = 0;
  char expires_utc[32] = {0};
  expect_int_eq(db_sync_reserve_serial(1, "st-a", "req-a", 120,
                                       reservation_id,
                                       sizeof(reservation_id), &serial,
                                       expires_utc, sizeof(expires_utc)),
                0, "serial reserve should succeed");
  expect_true(serial >= 1, "serial reserve should return positive serial");
  expect_true(reservation_id[0] != 0,
              "serial reserve should return reservation id");
    expect_int_eq(serial, 43,
          "first reservation should continue existing serial sequence");

  char retry_reservation_id[64] = {0};
  int retry_serial = 0;
  char retry_expires_utc[32] = {0};
  expect_int_eq(db_sync_reserve_serial(
                    1, "st-a", "req-a", 120, retry_reservation_id,
                    sizeof(retry_reservation_id), &retry_serial,
                    retry_expires_utc, sizeof(retry_expires_utc)),
                0, "retry with the same station/request ID should succeed");
  expect_str_eq(retry_reservation_id, reservation_id,
                "retried request should return the original reservation ID");
  expect_int_eq(retry_serial, serial,
                "retried request should return the original serial");
  expect_str_eq(retry_expires_utc, expires_utc,
                "retried request should return the original expiry");

  char other_station_reservation_id[64] = {0};
  int other_station_serial = 0;
  char other_station_expires_utc[32] = {0};
  expect_int_eq(db_sync_reserve_serial(
                    1, "st-b", "req-a", 120, other_station_reservation_id,
                    sizeof(other_station_reservation_id),
                    &other_station_serial, other_station_expires_utc,
                    sizeof(other_station_expires_utc)),
                0, "same request ID on another station should be independent");
  expect_true(strcmp(other_station_reservation_id, reservation_id) != 0,
              "reservation IDs must remain unique across stations");
  expect_int_eq(other_station_serial, 44,
                "different station request should receive the next serial");

    int second_serial = 0;
    char second_expires_utc[32] = {0};
    expect_int_eq(db_sync_reserve_serial(
            1, "st-a", "req-b", 120, second_reservation_id,
            sizeof(second_reservation_id), &second_serial,
            second_expires_utc, sizeof(second_expires_utc)),
          0, "second unique serial reservation should succeed");
    expect_true(strcmp(reservation_id, second_reservation_id) != 0,
          "different reservation request IDs should produce unique IDs");
    expect_int_eq(second_serial, 45,
          "central counter should advance across reservations");

  expect_int_eq(db_sync_commit_serial(reservation_id, "q-serial-1"), 0,
                "serial commit should succeed");
    expect_int_eq(db_sync_commit_serial(reservation_id, "q-serial-1"), 0,
          "repeated serial commit should be idempotent");

  char serial_db_path[512];
  join_path(serial_db_path, sizeof(serial_db_path), case_dir, "unit.sqlite3");
  sqlite3 *serial_db = NULL;
  expect_int_eq(sqlite3_open(serial_db_path, &serial_db), SQLITE_OK,
                "open serial database to seed pending remote commit");
  sqlite3_stmt *serial_stmt = NULL;
  expect_int_eq(sqlite3_prepare_v2(
                    serial_db,
                    "UPDATE serial_reservations SET status = 'commit_pending', "
                    "consumed_qso_uid = 'q-serial-2' WHERE reservation_id = ?;",
                    -1, &serial_stmt, NULL),
                SQLITE_OK, "prepare pending serial commit fixture");
  sqlite3_bind_text(serial_stmt, 1, second_reservation_id, -1,
                    SQLITE_TRANSIENT);
  expect_int_eq(sqlite3_step(serial_stmt), SQLITE_DONE,
                "seed pending serial commit fixture");
  sqlite3_finalize(serial_stmt);
  sqlite3_close(serial_db);
  expect_int_eq(db_sync_mark_serial_commit_failed(second_reservation_id), 0,
                "pending serial commit should be markable as failed");
  SyncFailedSerialCommitEntry failed_commits[2];
  int failed_commit_count = 0;
  memset(failed_commits, 0, sizeof(failed_commits));
  expect_int_eq(db_sync_load_failed_serial_commits(
                    failed_commits, 2, &failed_commit_count),
                0, "failed serial commit loader should succeed");
  expect_int_eq(failed_commit_count, 1,
                "failed serial commit loader should return failed entry");
  expect_str_eq(failed_commits[0].reservation_id, second_reservation_id,
                "failed serial commit should retain reservation ID");
  expect_str_eq(failed_commits[0].qso_uid, "q-serial-2",
                "failed serial commit should retain QSO ID");
  expect_int_eq(db_sync_retry_failed_serial_commit(second_reservation_id), 0,
                "failed serial commit should be retryable");
  expect_int_eq(db_sync_retry_failed_serial_commit(second_reservation_id), -1,
                "serial retry should reject an entry no longer failed");
  SyncSerialCommitEntry pending_commits[2];
  int pending_commit_count = 0;
  memset(pending_commits, 0, sizeof(pending_commits));
  expect_int_eq(db_sync_load_pending_serial_commits(
                    pending_commits, 2, &pending_commit_count),
                0, "pending serial commit loader should succeed after retry");
  expect_true(pending_commit_count > 0,
              "retried serial commit should return to pending");
  expect_int_eq(db_sync_mark_serial_commit_acked(second_reservation_id), 0,
                "clear retried serial fixture after assertions");

    expect_int_eq(db_sync_cache_serial_reservation(
            "rsv-client-cache-80", 1, "st-client", 80,
            "2099-01-01T00:00:00Z"),
          0, "client should persist a prefetched reservation");
    int available_count = 0;
    expect_int_eq(db_sync_count_available_serial_reservations(
            1, "st-client", &available_count),
          0, "cached reservation count should be readable");
    expect_int_eq(available_count, 1,
          "cached reservation should be available to the local operator");
    char claimed_id[64] = {0};
    int claimed_serial = 0;
    expect_int_eq(db_sync_claim_available_serial_reservation(
            1, "st-client", claimed_id, sizeof(claimed_id),
            &claimed_serial),
          0, "operator should atomically claim a cached serial");
    expect_int_eq(claimed_serial, 80, "claim should return the reserved serial");
    expect_str_eq(claimed_id, "rsv-client-cache-80",
          "claim should return its stable reservation ID");
    expect_int_eq(db_sync_recover_serial_claims(), 0,
          "restart recovery should release stale local claims");
    memset(claimed_id, 0, sizeof(claimed_id));
    expect_int_eq(db_sync_claim_available_serial_reservation(
            1, "st-client", claimed_id, sizeof(claimed_id),
            &claimed_serial),
          0, "recovered serial should be claimable after restart");

    expect_int_eq(db_update_qso_contest_fields_with_reservation(
            existing_qso_id, "80", "123", "RUN", "TEST", 1, 3,
            claimed_id, 1),
          0, "QSO contest update should queue its remote serial commit");
    SyncSerialCommitEntry commits[4];
    int commit_count = 0;
    memset(commits, 0, sizeof(commits));
    expect_int_eq(db_sync_load_pending_serial_commits(commits, 4, &commit_count),
          0, "pending serial commits should be readable");
    expect_int_eq(commit_count, 1,
          "QSO save should persist one remote serial commit job");
    expect_str_eq(commits[0].reservation_id, claimed_id,
          "queued commit should retain the reservation ID");
    expect_str_eq(commits[0].qso_uid, existing_qso.qso_uid,
          "queued commit should target the saved QSO UID");
    expect_int_eq(db_sync_mark_serial_commit_acked(claimed_id), 0,
          "successful remote commit should consume the local reservation");

    expect_int_eq(db_sync_cache_serial_reservation(
            "rsv-client-cache-81", 1, "st-client", 81,
            "2099-01-01T00:00:00Z"),
          0, "cache a second reservation for rollback testing");
    memset(claimed_id, 0, sizeof(claimed_id));
    expect_int_eq(db_sync_claim_available_serial_reservation(
            1, "st-client", claimed_id, sizeof(claimed_id),
            &claimed_serial),
          0, "claim second cached serial");
    char db_path[512];
    join_path(db_path, sizeof(db_path), case_dir, "unit.sqlite3");
    sqlite3 *fault_db = NULL;
    expect_int_eq(sqlite3_open(db_path, &fault_db), SQLITE_OK,
          "open serial DB to inject outbox failure");
    if (fault_db) {
    expect_int_eq(sqlite3_exec(
              fault_db,
              "CREATE TRIGGER fail_serial_outbox BEFORE INSERT ON "
              "log_outbox BEGIN SELECT RAISE(ABORT, 'injected'); END;",
              NULL, NULL, NULL),
            SQLITE_OK, "install serial outbox failure trigger");
    expect_int_eq(db_update_qso_contest_fields_with_reservation(
              existing_qso_id, "81", "124", "RUN", "TEST", 1, 4,
              claimed_id, 1),
            -1, "outbox failure should roll back serial commit state");
    expect_int_eq(sqlite3_exec(fault_db, "DROP TRIGGER fail_serial_outbox;",
                   NULL, NULL, NULL),
            SQLITE_OK, "remove serial outbox failure trigger");
    sqlite3_close(fault_db);
    }
    memset(commits, 0, sizeof(commits));
    commit_count = 0;
    expect_int_eq(db_sync_load_pending_serial_commits(commits, 4, &commit_count),
          0, "pending commits should remain queryable after rollback");
    expect_int_eq(commit_count, 0,
          "failed QSO update must not queue a serial commit");
    expect_int_eq(db_sync_release_serial_reservation(claimed_id, 1), 0,
          "rolled back reservation should return to the available pool");

    expect_int_eq(db_sync_cache_serial_reservation(
                      "rsv-client-qso-82", 1, "st-client", 82,
                      "2099-01-01T00:00:00Z"),
                  0, "cache reservation for public QSO save test");
    memset(claimed_id, 0, sizeof(claimed_id));
    expect_int_eq(db_sync_claim_available_serial_reservation(
                      1, "st-client", claimed_id, sizeof(claimed_id),
                      &claimed_serial),
                  0, "claim reservation for public QSO save test");
    char qso_status[128] = {0};
    int saved_idx = qso_add_contest_fields_with_reservation(
        "SP9RSV", 7022, "599", "CW", "", "82", "001", "RUN", "TEST", 1,
        3, 1, claimed_id, 1, qso_status, sizeof(qso_status));
    expect_true(saved_idx >= 0,
                "contest QSO should save with a preclaimed serial reservation");
    if (saved_idx >= 0) {
      expect_str_eq(logbook[saved_idx].exchange_sent, "82",
                    "saved QSO should retain the reserved contest serial");
      expect_true(strstr(qso_status, "QSO OK") == qso_status,
                  "reserved QSO save should report success");
    }
    memset(commits, 0, sizeof(commits));
    commit_count = 0;
    expect_int_eq(db_sync_load_pending_serial_commits(commits, 4, &commit_count),
                  0, "load commit job from public QSO save");
    expect_int_eq(commit_count, 1,
                  "public QSO save should create one serial commit job");
    if (saved_idx >= 0)
      expect_str_eq(commits[0].qso_uid, logbook[saved_idx].qso_uid,
                    "serial commit job should reference the saved QSO UID");

  set_test_db_path(tmp_dir);
  qso_init();
}

static void test_net_command_on_off_role_status(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/net_command_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for net command test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to net command test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file(
                    "logger.conf",
                    "CONTEST_DEF_FILE=\nNET_AUTH_TOKEN=" TEST_NET_AUTH_TOKEN
                    "\nNET_ALLOW_INSECURE_LAN=1\n"),
                0, "write authenticated logger.conf for net command test");

  app_controller_init();

  send_controller_text("net role server");
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "net role status text exists");
  if (state.status)
    expect_true(strstr(state.status, "NET role=server") != NULL,
                "net role command should set server role");

  send_controller_text("net on");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET enabled") != NULL,
                "net on command should enable network");

  int qso_count_before_blocked_switch = qso_count;
  send_controller_text("newlog blocked-while-online");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET active: stop sync") != NULL,
                "controller should explain why active log switching is blocked");
  expect_int_eq(qso_count, qso_count_before_blocked_switch,
                "blocked log switch should leave the active log data untouched");
  expect_int_eq(db_clear_logbook(), DB_ERR_LOG_CHANGE_WHILE_NET_ACTIVE,
                "DB clear should be blocked while network service is enabled");

  send_controller_text("net status");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "SYNC") != NULL,
                "net status should render sync summary");

  send_controller_text("netsync status");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "SYNC") != NULL,
                "netsync status alias should render sync summary");

  send_controller_text("netsync catchup");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "SYNC") != NULL,
                "netsync catchup alias should poll and render sync summary");

  send_controller_text("netserver stop");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET disabled") != NULL,
                "netserver stop alias should disable network");

  send_controller_text("netserver start");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET enabled") != NULL,
                "netserver start alias should enable network in server role");

  send_controller_text("net off");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET disabled") != NULL,
                "net off command should disable network");

  app_controller_shutdown();
  chdir("..");
}

static void test_netsync_offline_queue_status(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/netsync_offline_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for netsync offline test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to netsync offline test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file(
                    "logger.conf",
                    "CONTEST_DEF_FILE=\nNET_SHARED_LOG_ID=" TEST_SHARED_LOG_ID
                    "\nNET_ALLOW_INSECURE_LAN=1\n"),
                0,
                "write empty logger.conf for netsync offline test");

  int saved_net_enabled = config.net_enabled;
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  app_controller_init();

  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 1;

  send_controller_text("netsync on");
  send_controller_text("netsync catchup");
  send_controller_text("netsync status");
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "netsync status should set status text");
  if (state.status)
    expect_true(strstr(state.status, "NET OFFLINE - local queue:") != NULL,
                "netsync status should report offline queue diagnostics");

  send_controller_text("netsync off");

  app_controller_shutdown();

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

  chdir("..");
}

static void test_qso_status_includes_sync_pending_when_net_enabled(void) {
  int saved_net_enabled = config.net_enabled;
  char saved_role[16] = {0};
  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");

  qso_init();

  char status[128] = {0};
  int idx = qso_add("SP9SYNC 7020 599", status, sizeof(status));
  expect_true(idx >= 0, "sync-pending status QSO should be created");
  expect_true(strstr(status, "SYNC:PENDING:") != NULL,
              "QSO status should include sync pending suffix when net is enabled");

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
}

static void test_qso_add_mark_and_stats(void) {
  char status[128];
  AppRenderState state;

  app_controller_get_render_state(&state);
  expect_true(state.cluster_view, "DXCluster window is shown by default");

  qso_init();
  expect_int_eq(qso_count, 0, "qso_init resets qso_count");

  int idx1 = qso_add("SP9ABC 14074 599", status, sizeof(status));
  expect_int_eq(idx1, 0, "first QSO index");
  expect_str_eq(status, "QSO OK", "first QSO status");
  expect_str_eq(logbook[0].call, "SP9ABC", "callsign normalized");
  expect_str_eq(logbook[0].band, "20M", "band assigned");
  expect_str_eq(logbook[0].mode, "FT8", "mode assigned");

  int idx2 = qso_add("K1ABC 14150 59", status, sizeof(status));
  expect_int_eq(idx2, 1, "second QSO index");
  expect_str_eq(logbook[1].mode, "SSB", "second mode assigned");

  int bad_format = qso_add("K1ABC 14150", status, sizeof(status));
  expect_int_eq(bad_format, -1, "bad format rejected");
  expect_str_eq(status, "Bad format", "bad format status");

  int bad_call = qso_add("ABCDEF 14074 599", status, sizeof(status));
  expect_int_eq(bad_call, -1, "invalid call rejected");
  expect_str_eq(status, "Invalid callsign", "invalid call status");

  qso_mark_invalid(-1);
  qso_mark_invalid(99);

  qso_mark_invalid(1);
  expect_true(logbook[1].invalid, "qso_mark_invalid toggles on");

  qso_mark_invalid(1);
  expect_true(!logbook[1].invalid, "qso_mark_invalid toggles off");

  qso_mark_invalid(1);
  stats_update();

  expect_int_eq(stats.total_qso, 1, "stats total excludes invalid");
  expect_int_eq(stats.total_dxcc, 1, "stats DXCC excludes invalid");
  expect_int_eq(stats.ft8, 1, "stats FT8 count");
  expect_int_eq(stats.ssb, 0, "stats SSB count");

  qso_mark_invalid(1);
  stats_update();

  expect_int_eq(stats.total_qso, 2, "stats total after re-enable");
  expect_int_eq(stats.total_dxcc, 2, "stats DXCC after re-enable");
  expect_int_eq(stats.ssb, 1, "stats SSB after re-enable");

  app_controller_handle_key(APP_KEY_F2);
  app_controller_get_render_state(&state);

  expect_int_eq(qso_count, 0, "F2 clears the logbook");
  expect_true(state.status != NULL, "F2 status is present");
  if (state.status)
    expect_str_eq(state.status, "New clean log created",
                  "F2 status confirms clean log creation");

  int idx3 = qso_add("SP9ABC 14074 599", status, sizeof(status));
  expect_int_eq(idx3, 0, "first QSO after clean log reuses index 0");

  int idx4 = qso_add("K1ABC 14150 59", status, sizeof(status));
  expect_int_eq(idx4, 1, "second QSO after clean log reuses index 1");

  app_controller_handle_key(APP_KEY_F3);
  app_controller_get_render_state(&state);

  expect_int_eq(qso_count, 2, "F3 restores previous logbook");
  expect_true(state.status != NULL, "F3 status is present");
  if (state.status)
    expect_str_eq(state.status, "Previous log opened",
                  "F3 status confirms previous log restore");
  expect_str_eq(logbook[0].call, "SP9ABC", "restored first QSO call");
  expect_str_eq(logbook[1].call, "K1ABC", "restored second QSO call");

  app_controller_handle_key(APP_KEY_F5);
  app_controller_get_render_state(&state);
  expect_true(!state.cluster_view, "F5 disables cluster view");

  app_controller_handle_key(APP_KEY_F5);
  app_controller_get_render_state(&state);
  expect_true(state.cluster_view, "F5 enables cluster view");
}

static void test_export_csv_adif(const char *tmp_dir) {
  char csv_path[512];
  char adif_path[512];

  snprintf(csv_path, sizeof(csv_path), "%s/unit_log.csv", tmp_dir);
  snprintf(adif_path, sizeof(adif_path), "%s/unit_log.adi", tmp_dir);

  qso_mark_invalid(1);

  expect_int_eq(export_csv(csv_path), 0, "export_csv should succeed");
  expect_int_eq(export_adif(adif_path), 0, "export_adif should succeed");

  char *csv = read_whole_file(csv_path);
  char *adi = read_whole_file(adif_path);

  expect_true(csv != NULL, "CSV output should be readable");
  expect_true(adi != NULL, "ADIF output should be readable");

  if (csv) {
    expect_true(strstr(csv, "DATE,UTC,CALL,FREQ,BAND,MODE,RST,COMMENTS,COUNTRY") != NULL,
                "CSV header exists");
    expect_true(strstr(csv, "SP9ABC") != NULL, "CSV contains SP9ABC");
    expect_true(strstr(csv, "K1ABC") != NULL,
                "CSV keeps duplicate/invalid entries in export");
  }

  if (adi) {
    expect_true(strstr(adi, "<EOH>") != NULL, "ADIF header exists");
    expect_true(strstr(adi, "<CALL:6>SP9ABC") != NULL,
                "ADIF contains SP9ABC");
    expect_true(strstr(adi, "<CALL:5>K1ABC") != NULL,
                "ADIF keeps duplicate/invalid entries in export");
  }

  free(csv);
  free(adi);

  qso_mark_invalid(1);
}

static void test_export_command_exports_cabrillo_too(const char *tmp_dir) {
  char old_cwd[512];
  char export_dir[512];
  char adif_path[512];
  char cab_path[512];

  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before export command test");
  join_path(export_dir, sizeof(export_dir), tmp_dir, "export_command_case");
  expect_int_eq(mkdir(export_dir, 0777), 0,
                "create export command test directory");
  expect_int_eq(chdir(export_dir), 0,
                "chdir to export command test directory");

  app_controller_init();
  app_controller_submit_command_text("export");

  join_path(adif_path, sizeof(adif_path), export_dir, "log.adi");
  join_path(cab_path, sizeof(cab_path), export_dir, "log.cbr");

  expect_true(access(adif_path, F_OK) == 0,
              "export command should generate ADIF output");
  expect_true(access(cab_path, F_OK) == 0,
              "export command should generate Cabrillo output too");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after export command test");
}

static void test_contest_definition_and_cabrillo(const char *tmp_dir) {
  char contest_path[512];
  char cabrillo_path[512];
  char status[128];
  char expected_serial_1[16];
  char expected_serial_2[16];
  char expected_fragment_1[64];
  char expected_fragment_2[64];

  snprintf(contest_path, sizeof(contest_path), "%s/contest.conf", tmp_dir);
  snprintf(cabrillo_path, sizeof(cabrillo_path), "%s/unit_log.cbr", tmp_dir);

  const char *contest_text =
      "NAME=TEST-CONTEST\n"
      "CABRILLO_NAME=TEST-CONTEST\n"
      "MODE=MIXED\n"
      "CATEGORY_OPERATOR=SINGLE-OP\n"
      "CATEGORY_BAND=ALL\n"
      "CATEGORY_POWER=LOW\n"
      "POINTS_PER_QSO=3\n"
      "POINTS_CW=5\n"
      "POINTS_PHONE=1\n"
      "POINTS_DIGI=2\n"
      "POINTS_NEW_DXCC=4\n"
      "POINTS_SAME_DXCC=1\n"
      "POINTS_NEW_BAND_DXCC=6\n"
      "POINTS_SAME_BAND_DXCC=2\n"
      "EXCHANGE_SENT=#\n"
      "EXCHANGE_RECEIVED_TYPE=SERIAL\n"
      "SERIAL_WIDTH=3\n"
      "SCORING_RULE=7;SAME_COUNTRY=1\n"
      "SCORING_RULE=2\n"
      "MULTIPLIER=DXCC_PER_BAND,ZONE_PER_BAND\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write unit contest definition");

  ContestDefinition def;
  char err[128] = {0};
  expect_int_eq(contest_definition_load(contest_path, &def, err, sizeof(err)),
                0, "contest definition load should succeed");
  expect_int_eq(def.points_per_qso, 3, "POINTS_PER_QSO parsed");
  expect_int_eq(def.points_cw, 5, "POINTS_CW parsed");
  expect_int_eq(def.points_phone, 1, "POINTS_PHONE parsed");
  expect_int_eq(def.points_digi, 2, "POINTS_DIGI parsed");
  expect_int_eq(def.points_new_dxcc, 4, "POINTS_NEW_DXCC parsed");
  expect_int_eq(def.points_same_dxcc, 1, "POINTS_SAME_DXCC parsed");
  expect_int_eq(def.points_new_band_dxcc, 6,
                "POINTS_NEW_BAND_DXCC parsed");
  expect_int_eq(def.points_same_band_dxcc, 2,
                "POINTS_SAME_BAND_DXCC parsed");
    expect_str_eq(def.exchange_received_type, "SERIAL",
          "exchange validation type parsed");
    expect_int_eq(def.serial_width, 3, "serial width parsed");
    expect_int_eq(def.score_rule_count, 2, "ordered scoring rules parsed");
    expect_int_eq(def.multiplier_count, 2, "multiple multiplier families parsed");

    ContestScoreContext score_context = {
      .source_country = "Poland",
      .destination_country = "Poland",
      .source_continent = "EU",
      .destination_continent = "EU"};
    int calculated_points = 0;
    expect_true(contest_definition_score_qso(&def, &score_context,
                         &calculated_points),
          "declarative scoring should match a QSO context");
    expect_int_eq(calculated_points, 7,
          "first matching scoring rule should determine points");

    struct tm window_start_tm = {0};
    window_start_tm.tm_year = 2026 - 1900;
    window_start_tm.tm_mon = 0;
    window_start_tm.tm_mday = 1;
    const time_t window_start = timegm(&window_start_tm);
    char timed_path[512];
    snprintf(timed_path, sizeof(timed_path), "%s/timed_contest.conf", tmp_dir);
    expect_int_eq(write_text_file(timed_path,
        "NAME=UNRELATED-NAME\n"
        "START_UTC=2026-01-01T00:00:00Z\n"
        "END_UTC=2026-01-01T01:00:00Z\n"
        "ENFORCE_TIME_WINDOW=1\n"),
        0, "write UTC window definition");
    ContestDefinition timed_definition;
    expect_int_eq(contest_definition_load(timed_path, &timed_definition, err,
                                          sizeof(err)),
                  0, "load UTC window definition");
    expect_true(timed_definition.enforce_time_window,
                "UTC time-window enforcement parsed");
    expect_true(contest_rules_validate_target_qso(
                    &timed_definition, "unrelated-name", window_start, 14020, "CW", 75,
            err, sizeof(err)),
          "configured contest accepts the exact UTC window start");
    expect_true(!contest_rules_validate_target_qso(
                    &timed_definition, "unrelated-name", window_start - 1, 14020, "CW", 75,
            err, sizeof(err)),
          "configured contest rejects QSO before the UTC window");

  expect_int_eq((int)contest_multiplier_from_text("DXCC"),
                (int)CONTEST_MULT_DXCC,
                "MULTIPLIER DXCC parsed");
  expect_int_eq((int)contest_multiplier_from_text("DXCC_PER_BAND"),
                (int)CONTEST_MULT_DXCC_PER_BAND,
                "MULTIPLIER DXCC_PER_BAND parsed");
  expect_int_eq((int)contest_multiplier_from_text("ZONE_PER_BAND"),
                (int)CONTEST_MULT_ZONE_PER_BAND,
                "MULTIPLIER ZONE_PER_BAND parsed");
  expect_int_eq((int)contest_multiplier_from_text("ZONE"),
                (int)CONTEST_MULT_ZONE,
                "MULTIPLIER ZONE parsed");
  expect_int_eq((int)contest_multiplier_from_text("PREFIX"),
                (int)CONTEST_MULT_PREFIX,
                "MULTIPLIER PREFIX parsed");
  expect_int_eq((int)contest_multiplier_from_text("PREFIX_PER_BAND"),
                (int)CONTEST_MULT_PREFIX_PER_BAND,
                "MULTIPLIER PREFIX_PER_BAND parsed");
  expect_int_eq((int)contest_multiplier_from_text("WAG"),
                (int)CONTEST_MULT_WAG,
                "MULTIPLIER WAG parsed");
  expect_int_eq((int)contest_multiplier_from_text("CQWW"),
                (int)CONTEST_MULT_CQWW,
                "MULTIPLIER CQWW parsed");
  expect_int_eq((int)contest_multiplier_from_text("IARU"),
                (int)CONTEST_MULT_IARU,
                "MULTIPLIER IARU parsed");

  const int base_qso_count = qso_count;

  char dupe_contest_path[512];
  snprintf(dupe_contest_path, sizeof(dupe_contest_path), "%s/dupe_contest.conf",
           tmp_dir);
  expect_int_eq(write_text_file(dupe_contest_path,
      "NAME=DUPE-CONTEST\n"
      "CABRILLO_NAME=DUPE-CONTEST\n"
      "MODE=CW\n"
      "CATEGORY_OPERATOR=SINGLE-OP\n"
      "CATEGORY_BAND=ALL\n"
      "CATEGORY_POWER=LOW\n"
      "EXCHANGE_SENT=#\n"
      "DOUBLE_QSO=1\n"
      "FIELD=SERIAL,Serial Number,required\n"),
      0, "write duplicate-check contest definition");
  expect_int_eq(contest_definition_load(dupe_contest_path, &def, err, sizeof(err)),
                0, "duplicate-rule contest definition should load");
  expect_int_eq(def.duplicate_qso, 1, "DOUBLE_QSO should parse as enabled");

  snprintf(config.contest_definition_path, sizeof(config.contest_definition_path),
           "%s", dupe_contest_path);

  int dupe_idx_1 = qso_add_contest_fields("DUPE01", 7020, "599", "CW", "",
                                          "001", "", "RUN",
                                          def.cabrillo_name, 1, 1,
                                          def.duplicate_qso,
                                          status, sizeof(status));
  expect_int_eq(dupe_idx_1, base_qso_count,
                "first duplicate-check QSO should be saved");
  expect_str_eq(status, "QSO OK TX:001 RX:-",
                "first QSO should be accepted normally");

  int dupe_idx_2 = qso_add_contest_fields("DUPE01", 7020, "599", "CW", "",
                                          "002", "", "RUN",
                                          def.cabrillo_name, 1, 1,
                                          def.duplicate_qso,
                                          status, sizeof(status));
  expect_int_eq(dupe_idx_2, base_qso_count + 1,
                "second duplicate-check QSO should still be stored");
  expect_true(!logbook[dupe_idx_2].invalid,
              "duplicate QSO should stay valid when DOUBLE_QSO is enabled");
  expect_str_eq(status, "QSO OK TX:002 RX:-",
                "duplicate QSO should be accepted when DOUBLE_QSO is enabled");

  qso_init();
  expect_int_eq(contest_definition_load(contest_path, &def, err, sizeof(err)),
                0, "restore original contest definition after duplicate test");
  snprintf(config.contest_definition_path, sizeof(config.contest_definition_path),
           "%s", contest_path);
  const int post_dupe_qso_count = qso_count;
  snprintf(expected_serial_1, sizeof(expected_serial_1), "%d",
           post_dupe_qso_count + 1);
  snprintf(expected_serial_2, sizeof(expected_serial_2), "%d",
           post_dupe_qso_count + 2);
  snprintf(expected_fragment_1, sizeof(expected_fragment_1), "599 %-6s SP9SER",
           expected_serial_1);
  snprintf(expected_fragment_2, sizeof(expected_fragment_2), "599 %-6s SP9SEQ",
           expected_serial_2);

  expect_int_eq(qso_add_contest_fields("SP9SER", 7020, "599", "CW", "", "",
                                       "101", "RUN", def.cabrillo_name, 1, 1,
                                       def.duplicate_qso,
                                       status, sizeof(status)),
                post_dupe_qso_count,
                "first contest QSO for Cabrillo serial fallback should save");
  expect_int_eq(qso_add_contest_fields("SP9SEQ", 7020, "599", "CW", "", "",
                                       "102", "RUN", def.cabrillo_name, 1, 1,
                                       def.duplicate_qso,
                                       status, sizeof(status)),
                post_dupe_qso_count + 1,
                "second contest QSO for Cabrillo serial fallback should save");

  expect_int_eq(export_cabrillo(cabrillo_path, &def, "SP9ABC"), 0,
                "export_cabrillo should succeed");

  char *cbr = read_whole_file(cabrillo_path);
  expect_true(cbr != NULL, "Cabrillo file should be readable");

  if (cbr) {
    expect_true(strstr(cbr, "CONTEST: TEST-CONTEST") != NULL,
                "Cabrillo contains contest header");
    expect_true(strstr(cbr, "QSO:") != NULL,
                "Cabrillo contains at least one QSO");
    expect_true(strstr(cbr, expected_fragment_1) != NULL,
                "Cabrillo should generate first serial exchange from # template");
    expect_true(strstr(cbr, expected_fragment_2) != NULL,
                "Cabrillo should generate second serial exchange from # template");
    expect_true(strstr(cbr, "599 #") == NULL,
                "Cabrillo should not emit literal # as sent exchange");
  }

  free(cbr);
}

static void test_dxlog_definition_compatibility(const char *tmp_dir) {
  char path_wpx[512];
  char path_ww[512];
  char path_spdx[512];
  char path_iaru[512];
  char path_iaru_vhf[512];
  char err[128] = {0};
  ContestDefinition def;

  snprintf(path_wpx, sizeof(path_wpx), "%s/dxlog_cqwpx.txt", tmp_dir);
  snprintf(path_ww, sizeof(path_ww), "%s/dxlog_cqww.txt", tmp_dir);
  snprintf(path_spdx, sizeof(path_spdx), "%s/dxlog_spdx.txt", tmp_dir);
  snprintf(path_iaru, sizeof(path_iaru), "%s/dxlog_iaru.txt", tmp_dir);
  snprintf(path_iaru_vhf, sizeof(path_iaru_vhf), "%s/dxlog_iaru_vhf.txt", tmp_dir);

  expect_int_eq(write_text_file(path_wpx,
      "CONTESTNAME=CQ WPX Contest\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=WPX\n"
      "MULT1_COUNT=ALL\n"
      "FIELD_RCVD_TYPE=NR\n"),
      0, "write DXLog CQWPX sample");
  expect_int_eq(contest_definition_load(path_wpx, &def, err, sizeof(err)), 0,
                "load DXLog CQWPX sample");
  expect_str_eq(def.exchange_sent_template, "#",
                "DXLog CQWPX should map to serial TX exchange");
  expect_int_eq((int)def.multiplier_type, (int)CONTEST_MULT_PREFIX,
                "DXLog CQWPX should map WPX multiplier to PREFIX");

  expect_int_eq(write_text_file(path_ww,
      "CONTESTNAME=CQ World Wide\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=DXCC\n"
      "MULT1_COUNT=PER_BAND\n"
      "MULT2_TYPE=CQZONE\n"
      "MULT2_COUNT=PER_BAND\n"
      "FIELD_RCVD_TYPE=CQZONE\n"),
      0, "write DXLog CQWW sample");
  expect_int_eq(contest_definition_load(path_ww, &def, err, sizeof(err)), 0,
                "load DXLog CQWW sample");
  expect_str_eq(def.exchange_sent_template, "CQZONE",
                "DXLog CQWW should map TX exchange to CQZONE");
  expect_int_eq((int)def.multiplier_type,
                (int)CONTEST_MULT_DXCC_PER_BAND,
                "DXLog CQWW should retain its first explicit multiplier family");
  expect_int_eq(def.multiplier_count, 2,
                "DXLog CQWW should preserve both DXCC and zone families");

  expect_int_eq(write_text_file(path_spdx,
      "CONTESTNAME=SP DX Contest\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=CUSTOM\n"
      "MULT1_COUNT=PER_BAND\n"
      "MULT2_TYPE=DXCC\n"
      "MULT2_COUNT=PER_BAND\n"
      "FIELD_RCVD_TYPE=DXCC:SP=MULT;!DXCC:SP=NR\n"),
      0, "write DXLog SPDX sample");
  expect_int_eq(contest_definition_load(path_spdx, &def, err, sizeof(err)), 0,
                "load DXLog SPDX sample");
  expect_int_eq((int)def.multiplier_type, (int)CONTEST_MULT_SPDX,
                "DXLog SPDX should map to dedicated SPDX multiplier mode");

  expect_int_eq(write_text_file(path_iaru,
      "CONTESTNAME=IARU HF\n"
      "MODES=CW;SSB\n"
      "POINTS_TYPE=STANDARD\n"
      "QSO_NUMBER_CATEGORY=ALL\n"
      "FIELD_RCVD_TYPE=ITUZONE\n"),
      0, "write DXLog IARU sample");
  expect_int_eq(contest_definition_load(path_iaru, &def, err, sizeof(err)), 0,
                "load DXLog IARU sample");
  expect_str_eq(def.exchange_sent_template, "#",
                "DXLog QSO_NUMBER_CATEGORY should enable serial TX exchange");
  expect_str_eq(def.fields[0].name, "ITUZONE",
                "DXLog ITUZONE should map to ITUZONE exchange field");
  expect_int_eq(def.points_configured, 0,
                "DXLog POINTS_TYPE alone should not override point rules");

  expect_int_eq(write_text_file(path_iaru_vhf,
      "CONTESTNAME=IARU VHF\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=WWL\n"
      "MULT1_COUNT=PER_BAND\n"
      "FIELD_RCVD_TYPE=NR;GRID\n"
      "QSO_NUMBER_CATEGORY=ALL\n"),
      0, "write DXLog IARU VHF sample");
  expect_int_eq(contest_definition_load(path_iaru_vhf, &def, err, sizeof(err)), 0,
                "load DXLog IARU VHF sample");
  expect_str_eq(def.exchange_sent_template, "#",
                "DXLog IARU VHF should still enable serial numbering");
  expect_str_eq(def.fields[0].name, "SERIAL_GRID",
                "DXLog NR+GRID should map to serial+grid field");
  expect_int_eq((int)def.multiplier_type, (int)CONTEST_MULT_GRID_PER_BAND,
                "DXLog WWL per band should map to grid-per-band multiplier");
}

static void test_dxlog_custom_multiplier_metadata_and_section_area_parsing(
    const char *tmp_dir) {
  char path[512];
  char err[128] = {0};
  ContestDefinition def;

  snprintf(path, sizeof(path), "%s/dxlog_custom_section.txt", tmp_dir);
  expect_int_eq(write_text_file(path,
      "CONTESTNAME=Custom Section Test\n"
      "MULT1_TYPE=CUSTOM\n"
      "MULT1_COUNT=PER_BAND\n"
      "MULT3_TYPE=SECTION\n"
      "MULT3_COUNT=PER_BAND\n"
      "MULT3_FIELD=SECTION\n"
      "CUSTOM_MULT_LIST=EA,EI,IL,IO,IZ\n"
      "SECTION=QTH\n"
      "AREA=IT\n"
      "PFX_AREA=ARRL\n"),
      0, "write DXLog custom multiplier sample");
  expect_int_eq(contest_definition_load(path, &def, err, sizeof(err)), 0,
                "load DXLog custom multiplier metadata");
  expect_int_eq((int)def.multiplier_type, (int)CONTEST_MULT_CUSTOM_LIST,
                "DXLog CUSTOM multiplier should map to custom list mode");
  expect_str_eq(def.custom_mult_list, "EA,EI,IL,IO,IZ",
                "DXLog CUSTOM_MULT_LIST should be preserved");
  expect_str_eq(def.mult3_type, "SECTION",
                "DXLog MULT3_TYPE should be preserved");
  expect_str_eq(def.mult3_field, "SECTION",
                "DXLog MULT3_FIELD should be preserved");
  expect_str_eq(def.section_name, "QTH",
                "DXLog SECTION should be preserved");
  expect_str_eq(def.area_name, "IT",
                "DXLog AREA should be preserved");
  expect_str_eq(def.pfx_area, "ARRL",
                "DXLog PFX_AREA should be preserved");
}

static void test_dxlog_importer_generates_local_conf(const char *tmp_dir) {
  char src_path[512];
  char dst_path[512];
  char err[128] = {0};
  char warn[256] = {0};

  snprintf(src_path, sizeof(src_path), "%s/raw_dxlog_cqww.txt", tmp_dir);
  snprintf(dst_path, sizeof(dst_path), "%s/contest_imported.conf", tmp_dir);

  expect_int_eq(write_text_file(src_path,
      "CONTESTNAME=CQ World Wide\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=DXCC\n"
      "MULT1_COUNT=PER_BAND\n"
      "MULT2_TYPE=CQZONE\n"
      "MULT2_COUNT=PER_BAND\n"
      "FIELD_RCVD_TYPE=CQZONE\n"
      "POINTS_FIELD_BAND_MODE=ALL;ALL;ALL;ALL;3\n"
      "SCORE_TOTAL_FX=$FIELDVALUE.Points*$FIELDVALUE.Mult1\n"),
      0, "write raw DXLog source for importer");

    expect_int_eq(contest_definition_import_dxlog(src_path, dst_path, err,
                          sizeof(err), warn,
                          sizeof(warn)),
                0, "DXLog importer should create normalized file");
    expect_true(strstr(warn, "Ignored DXLog rules") != NULL,
          "importer should report ignored DXLog rules");
    expect_true(strstr(warn, "POINTS_FIELD_BAND_MODE") != NULL,
          "importer warning should include POINTS_FIELD_BAND_MODE");

  char *imported = read_whole_file(dst_path);
  expect_true(imported != NULL, "imported contest.conf should be readable");
  if (imported) {
    expect_true(strstr(imported, "NAME=CQ World Wide") != NULL,
                "imported config should keep contest name");
    expect_true(strstr(imported,
               "MULTIPLIER=DXCC_PER_BAND,ZONE_PER_BAND") != NULL,
          "imported config should preserve independent CQWW multipliers");
    expect_true(strstr(imported, "POINTS_PER_QSO=") == NULL,
          "imported config should not override contest-specific points without parsed point rules");
    expect_true(strstr(imported, "FIELD=CQZONE,CQ Zone,required") != NULL,
                "imported config should include normalized exchange field");
  }
  free(imported);
}

static void test_maidenhead(void) {
  double lat = 0.0;
  double lon = 0.0;
  int distance_km = 0;

  expect_int_eq(locator_to_latlon("JO90", &lat, &lon), 0,
                "locator JO90 should parse");
  expect_double_close(lat, 50.5, 0.001, "JO90 latitude");
  expect_double_close(lon, 19.0, 0.001, "JO90 longitude");

  expect_int_eq(locator_to_latlon("JO90aa", &lat, &lon), 0,
                "locator JO90aa should parse");
  expect_double_close(lat, 50.020833, 0.001, "JO90aa latitude");
  expect_double_close(lon, 18.041666, 0.001, "JO90aa longitude");

  expect_int_eq(locator_to_latlon("ZZ99", &lat, &lon), -1,
                "invalid locator should fail");
  expect_int_eq(locator_to_latlon(NULL, &lat, &lon), -1,
                "NULL locator should fail");
  expect_true(locator_is_valid("JO90AA"),
              "valid 6-char locator should be accepted");
  expect_true(!locator_is_valid("BAD"),
              "short invalid locator should be rejected");
  expect_int_eq(locator_distance_km("JO90AA", "JO91AA", &distance_km), 0,
                "locator distance should be calculable");
  expect_true(distance_km > 0,
              "locator distance should be positive for different locators");
}

static void test_controller_vhf_locator_exchange_and_distance_points(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/vhf_locator_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for VHF locator test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=VHF-LOCATOR\n"
      "CABRILLO_NAME=VHF-LOCATOR\n"
      "MODE=MIXED\n"
      "DISTANCE_SCORING=1\n"
      "EXCHANGE_SENT=LOCATOR\n"
      "FIELD=GRID,Grid,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write VHF locator contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "LOCATOR=JO90AA\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for VHF locator test");

  set_test_db_path(case_dir);

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before VHF locator test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to VHF locator test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "VHF contest exchange should be visible");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "JO90AA",
                  "VHF locator contest should send local locator");

  send_controller_text("144300");
  send_controller_chars("SP9VHF");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("JO91AA");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one VHF locator QSO should be saved");
  expect_str_eq(logbook[base_qso_count].exchange_sent, "JO90AA",
                "saved VHF QSO should use local locator as sent exchange");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "JO91AA",
                "saved VHF QSO should preserve received locator");
  expect_true(logbook[base_qso_count].points > 0,
              "VHF locator QSO should receive positive distance-based points");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after VHF locator test");
}

static void test_controller_vhf_serial_locator_exchange_and_distance_points(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/vhf_serial_locator_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for VHF serial+locator test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=IARU-VHF\n"
      "CABRILLO_NAME=IARU-VHF\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=SERIAL_GRID\n"
      "MULTIPLIER=GRID_PER_BAND\n"
      "FIELD=SERIAL_GRID,Serial + Locator,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write VHF serial+locator contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "LOCATOR=JO90AA\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for VHF serial+locator test");

  set_test_db_path(case_dir);

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before VHF serial+locator test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to VHF serial+locator test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "VHF serial+locator exchange should be visible");
  char expected_sent[32] = {0};
  if (state.contest_exchange_sent) {
    snprintf(expected_sent, sizeof(expected_sent), "%s",
             state.contest_exchange_sent);
    expect_str_eq(state.contest_exchange_sent, expected_sent,
                  "VHF serial+locator contest should send serial plus locator");
  }

  send_controller_text("144300");
  send_controller_chars("SP9IVH");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("001JO91AA");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one VHF serial+locator QSO should be saved");
  expect_str_eq(logbook[base_qso_count].exchange_sent, expected_sent,
                "saved VHF QSO should use serial plus local locator");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "001JO91AA",
                "saved VHF QSO should preserve composite received exchange");
  expect_true(logbook[base_qso_count].points > 0,
              "VHF serial+locator QSO should receive positive distance-based points");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after VHF serial+locator test");
}

static void test_controller_vhf_spaced_serial_locator_exchange(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/vhf_spaced_serial_locator_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for VHF spaced serial+locator test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=IARU-VHF\n"
      "CABRILLO_NAME=IARU-VHF\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=# LOCATOR\n"
      "MULTIPLIER=GRID_PER_BAND\n"
      "FIELD=SERIAL_GRID,Serial + Locator,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write VHF spaced serial+locator contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "LOCATOR=JO90AA\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for VHF spaced serial+locator test");

  set_test_db_path(case_dir);

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before VHF spaced serial+locator test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to VHF spaced serial+locator test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "VHF spaced serial+locator exchange should be visible");
  char expected_sent[32] = {0};
  snprintf(expected_sent, sizeof(expected_sent), "%d JO90AA",
           base_qso_count + 1);
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_sent,
                  "VHF spaced serial+locator contest should send serial, space and locator");

  send_controller_text("144300");
  send_controller_chars("SP9IVH");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("001JO91AA");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one VHF spaced serial+locator QSO should be saved");
  expect_str_eq(logbook[base_qso_count].exchange_sent, expected_sent,
                "saved VHF QSO should use spaced serial plus local locator");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "001JO91AA",
                "saved VHF QSO should preserve composite received exchange");
  expect_true(logbook[base_qso_count].points > 0,
              "VHF spaced serial+locator QSO should receive positive distance-based points");

  app_controller_get_render_state(&state);
  char expected_next_sent[32] = {0};
  snprintf(expected_next_sent, sizeof(expected_next_sent), "%d JO90AA",
           base_qso_count + 2);
  expect_true(state.contest_exchange_sent != NULL,
              "VHF spaced serial+locator exchange should remain visible");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_next_sent,
                  "next spaced serial+locator exchange should advance the serial");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after VHF spaced serial+locator test");
}

static void test_controller_spaced_serial_locator_exchange_and_suggestions(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/spaced_serial_loc_suggest_case", tmp_dir);
  expect_int_eq(make_temp_dir(case_dir, sizeof(case_dir)), 0,
                "create isolated directory for spaced serial locator suggest test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=IARU-VHF\n"
      "CABRILLO_NAME=IARU-VHF\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=# LOCATOR\n"
      "FIELD=# LOCATOR,Serial + Locator,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write # LOCATOR contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "LOCATOR=JO90AA\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for # LOCATOR suggest test");

  set_test_db_path(case_dir);

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before # LOCATOR suggest test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to # LOCATOR suggest test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  /* First QSO: log SP9IVH with exchange 001 JO91AA */
  send_controller_text("144300");
  send_controller_chars("SP9IVH");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("001 JO91AA");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "first QSO with # LOCATOR exchange should be saved");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "001 JO91AA",
                "saved exchange should be 001 JO91AA");

  /* Second QSO: log SP9IVH again. When moving to exchange field and typing serial 002,
     suggestion should offer '002 JO91AA', and Tab should autocomplete it. */
  send_controller_chars("SP9IVH");
  app_controller_handle_key(APP_KEY_SPACE);

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.info != NULL && strstr(state.info, "JO91AA") != NULL,
              "exchange field suggestion should show known locator JO91AA");

  send_controller_chars("002");
  app_controller_get_render_state(&state);
  expect_true(state.info != NULL && strstr(state.info, "002 JO91AA") != NULL,
              "exchange field suggestion should show 002 JO91AA when 002 typed");

  app_controller_handle_key(APP_KEY_TAB);
  app_controller_get_render_state(&state);
  expect_str_eq(state.input_rst_r1, "002 JO91AA",
                "Tab should autocomplete exchange to 002 JO91AA");

  app_controller_handle_key(APP_KEY_ENTER);
  expect_int_eq(qso_count, base_qso_count + 2,
                "second QSO with autocompleted # LOCATOR exchange should be saved");
  expect_str_eq(logbook[base_qso_count + 1].exchange_recv, "002 JO91AA",
                "second QSO saved exchange should be 002 JO91AA");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after # LOCATOR suggest test");
}

static void test_controller_fixed_locator_serial_suggestion(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/fixed_serial_loc_suggest_case", tmp_dir);
  expect_int_eq(make_temp_dir(case_dir, sizeof(case_dir)), 0,
                "create isolated directory for fixed serial locator suggest test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "contest.conf");
  const char *contest_text =
      "NAME=IARU-UHF\n"
      "CABRILLO_NAME=IARU-UHF\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=# LOCATOR\n"
      "FIELD=# LOCATOR,Serial + Locator,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write # LOCATOR contest definition for fixed locator suggestion test");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "LOCATOR=JO81PC\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for fixed locator suggestion test");

  set_test_db_path(case_dir);

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before fixed locator suggestion test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to fixed locator suggestion test directory");

  app_controller_init();

  send_controller_chars("SP9IVH");
  app_controller_handle_key(APP_KEY_SPACE);

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.info != NULL && strstr(state.info, "JO81PC") != NULL,
              "exchange field suggestion should show fixed locator JO81PC");

  send_controller_chars("002");
  app_controller_get_render_state(&state);
  expect_true(state.info != NULL && strstr(state.info, "002 JO81PC") != NULL,
              "exchange field suggestion should show 002 JO81PC when serial typed");

  app_controller_handle_key(APP_KEY_TAB);
  app_controller_get_render_state(&state);
  expect_str_eq(state.input_rst_r1, "002 JO81PC",
                "Tab should autocomplete exchange to 002 JO81PC using fixed locator");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after fixed locator suggestion test");
}

static void test_dxcluster_set_status(void) {
  expect_int_eq(config_load("/definitely/missing/logger.conf"), -1,
                "missing config returns -1 but applies defaults");

  dxcluster_set_status("Connected");
  expect_true(strstr(dxcluster_status, "Connected") != NULL,
              "status should include message");
  expect_true(strstr(dxcluster_status, "telnet.reversebeacon.net") != NULL,
              "status should include default host");
  expect_true(strstr(dxcluster_status, ":7000") != NULL,
              "status should include default port");

  snprintf(config.dxc_host, sizeof(config.dxc_host), "%s", "cluster.local");
  config.dxc_port = 7300;

  dxcluster_set_status("Ready");
  expect_true(strstr(dxcluster_status, "Ready") != NULL,
              "custom status message applied");
  expect_true(strstr(dxcluster_status, "cluster.local") != NULL,
              "custom host reflected in status");
  expect_true(strstr(dxcluster_status, ":7300") != NULL,
              "custom port reflected in status");
}

static void test_dxcluster_start_stop(void) {
  snprintf(config.dxc_host, sizeof(config.dxc_host), "%s", "127.0.0.1");
  config.dxc_port = 9;
  snprintf(config.dxc_call, sizeof(config.dxc_call), "%s", "N0CALL");

  int rc = dxcluster_start();
  expect_int_eq(rc, 0, "dxcluster_start should create thread");

  usleep(50000);

  dxcluster_stop();

  expect_true(strstr(dxcluster_status, "Disconnected") != NULL ||
                  strstr(dxcluster_status, "failed") != NULL ||
                  strstr(dxcluster_status, "timeout") != NULL ||
                  strstr(dxcluster_status, "Connecting") != NULL,
              "dxcluster_stop should finish worker lifecycle");
}

typedef struct {
  int port;
  int ok;
  char received[4096];
} MockDxclusterServerArgs;

static void *mock_dxcluster_server_thread(void *arg) {
  MockDxclusterServerArgs *ctx = (MockDxclusterServerArgs *)arg;
  if (!ctx)
    return NULL;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    return NULL;

  int reuse = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)ctx->port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(srv);
    return NULL;
  }

  if (listen(srv, 1) != 0) {
    close(srv);
    return NULL;
  }

  int cli = accept(srv, NULL, NULL);
  if (cli < 0) {
    close(srv);
    return NULL;
  }

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  const char *prompt = "login: ";
  (void)send(cli, prompt, strlen(prompt), 0);

  for (int i = 0; i < 30; i++) {
    char buf[512] = {0};
    int n = (int)recv(cli, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      const size_t cur = strlen(ctx->received);
      const size_t room = sizeof(ctx->received) - cur - 1;
      if (room > 0)
        strncat(ctx->received, buf, room);

      if (strstr(ctx->received, "dx 14074 SP9TEST CQ TEST") != NULL &&
          strstr(ctx->received, "DX 14075 SP9TEST2 CQ TEST") != NULL) {
        ctx->ok = 1;
        break;
      }
    } else {
      usleep(100000);
    }
  }

  close(cli);
  close(srv);
  return NULL;
}

static void test_dxcluster_send_spot_requires_connection(void) {
  dxcluster_disconnect();

  expect_int_eq(dxcluster_send_spot(NULL), -1,
                "dxcluster_send_spot should reject NULL input");
  expect_true(strstr(dxcluster_status, "empty text") != NULL,
              "spot status should explain empty payload failure");

  expect_int_eq(dxcluster_send_spot("   \t"), -1,
                "dxcluster_send_spot should reject blank input");
  expect_true(strstr(dxcluster_status, "empty text") != NULL,
              "spot status should explain blank payload failure");

  expect_int_eq(dxcluster_send_spot("14074 SP9TEST CQ TEST"), -1,
                "dxcluster_send_spot should fail when disconnected");
  expect_true(strstr(dxcluster_status, "not connected") != NULL,
              "spot status should report disconnected state");
}

static void test_dxcluster_connect_disconnect_and_send_spot(void) {
  snprintf(config.dxc_host, sizeof(config.dxc_host), "%s", "127.0.0.1");
  config.dxc_port = 19431;
  snprintf(config.dxc_call, sizeof(config.dxc_call), "%s", "SP9UNIT");

  MockDxclusterServerArgs server;
  memset(&server, 0, sizeof(server));
  server.port = config.dxc_port;

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_dxcluster_server_thread, &server),
                0, "mock DXCluster server thread should start");
  usleep(120000);

  expect_int_eq(dxcluster_connect(), 0,
                "dxcluster_connect should start worker thread");

  int sent_ok = 0;
  for (int i = 0; i < 30; i++) {
    if (dxcluster_send_spot("14074 SP9TEST CQ TEST") == 0 &&
        dxcluster_send_spot("DX 14075 SP9TEST2 CQ TEST") == 0) {
      sent_ok = 1;
      break;
    }
    usleep(100000);
  }

  dxcluster_disconnect();
  pthread_join(tid, NULL);

  expect_true(sent_ok,
              "dxcluster_send_spot should succeed after connection is established");
  expect_true(server.ok == 1,
              "mock DXCluster server should receive both spot commands");
  expect_true(strstr(server.received, "dx 14074 SP9TEST CQ TEST") != NULL,
              "spot without DX prefix should be normalized to dx command");
  expect_true(strstr(server.received, "DX 14075 SP9TEST2 CQ TEST") != NULL,
              "spot with DX prefix should be sent unchanged");
  expect_true(strstr(dxcluster_status, "Disconnected") != NULL ||
                  strstr(dxcluster_status, "Spot sent") != NULL,
              "disconnect after spot send should leave valid cluster status");
}

static void test_app_controller_shutdown_stops_cluster(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/dxcluster_shutdown_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for DXCluster shutdown test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to DXCluster shutdown test directory");
  set_test_db_path(case_dir);

  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for DXCluster shutdown test");

  app_controller_init();
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "shutdown test status is present");

  app_controller_shutdown();

  expect_true(strstr(dxcluster_status, "Disconnected") != NULL ||
                  strstr(dxcluster_status, "failed") != NULL ||
                  strstr(dxcluster_status, "timeout") != NULL ||
                  strstr(dxcluster_status, "Connecting") != NULL,
              "app_controller_shutdown should stop DXCluster worker");

  chdir("..");
}

static void test_call_suggestions(void) {
  CallSuggestionList list;
  char history[8][CALL_SUGGESTION_LEN] = {
      "SP3ABC", "SQ9XYZ", "SP9AAA", "SN0HQ", "SP9XYZ", "SP9AAA", "K1ABC", "SP8QWE"};

  call_suggestion_list_clear(&list);

  call_suggestion_refresh(&list, "sp", history, 8);
  expect_int_eq(list.count, 0,
                "less than 3 chars should not return suggestions");

  call_suggestion_refresh(&list, "sp9", history, 8);
  expect_int_eq(list.count, 2,
                "suggestions should include unique calls matching the prefix");
  expect_str_eq(list.matches[0], "SP9AAA", "newest matching call appears first");
  expect_str_eq(list.matches[1], "SP9XYZ", "older matching call appears second");

  call_suggestion_select_next(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP9XYZ",
                "down arrow selection should move to next match");

  call_suggestion_select_prev(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP9AAA",
                "up arrow selection should move to previous match");

  call_suggestion_select_prev(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP9XYZ",
                "previous on first match should wrap to last");

  char input[64] = "sp9;599";
  int len = (int)strlen(input);
  call_suggestion_refresh(&list, input, history, 8);
  call_suggestion_select_next(&list);
  expect_true(call_suggestion_apply(&list, input, &len, sizeof(input)) == 1,
              "apply should replace first token with selected suggestion");
  expect_str_eq(input, "SP9XYZ;599",
                "apply should keep suffix after first token and use selected match");

  call_suggestion_refresh(&list, "SP9AAA", history, 8);
  expect_int_eq(list.count, 0,
                "exact callsign should not suggest the same value");

  call_suggestion_refresh(&list, "SP9 ", history, 8);
  expect_int_eq(list.count, 0,
                "no suggestions after first token is completed");
}

static void test_app_controller_key_flow(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/keyflow_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for key-flow test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to key-flow test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for key-flow test");

  app_controller_init();
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "controller render state status is present");
  expect_int_eq(state.active_input_field, 0,
                "controller starts with CALL input field active");
  if (state.status)
    expect_str_eq(state.status, "Ready", "controller starts with Ready status");

  AppControllerEvent ev = app_controller_handle_key(APP_KEY_F1);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "F1 should not request special controller event");

  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "controller status after F1 is present");
  if (state.status)
    expect_true(strstr(state.status, "CALL RST COMMENTS") != NULL,
                "F1 updates status help text");

  ev = app_controller_handle_key(APP_KEY_F4);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "F4 export prompt should not request special controller event");
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "F4 export status is present");
  if (state.status)
    expect_true(strstr(state.status, "Enter ADIF filename") != NULL,
                "F4 prompts for export filename");

  app_controller_set_export_filename_text("AB12");
  expect_true(app_controller_export_prompt_active(),
              "export prompt should remain active while typing");
  expect_str_eq(app_controller_export_filename_text(), "AB12",
                "typed export filename should remain visible in the buffer");

  ev = app_controller_handle_key(APP_KEY_ESC);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "ESC should cancel export prompt");
  expect_true(!app_controller_export_prompt_active(),
              "ESC should close the export prompt");
  expect_str_eq(app_controller_export_filename_text(), "",
                "canceling export should clear the filename buffer");

  ev = app_controller_handle_key(APP_KEY_F5);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "F5 toggle should not request special controller event");
  app_controller_get_render_state(&state);
  expect_true(!state.cluster_view, "F5 should hide the cluster view");

  ev = app_controller_handle_key(APP_KEY_F5);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "second F5 toggle should not request special event");
  app_controller_get_render_state(&state);
  expect_true(state.cluster_view, "second F5 should show the cluster view again");

  ev = app_controller_handle_key(APP_KEY_F6);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "F6 stats refresh should not request special event");

  ev = app_controller_handle_key(APP_KEY_F7);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_REQUEST_CTY_UPDATE,
                "F7 should request CTY update");

  ev = app_controller_handle_key(APP_KEY_F10);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_EXIT,
                "F10 should request exit event");

  app_controller_shutdown();
  chdir("..");
}

static void send_controller_text(const char *text) {
  if (!text)
    return;

  for (const char *p = text; *p; p++)
    app_controller_handle_key((unsigned char)*p);

  app_controller_handle_key(APP_KEY_ENTER);
}

static void send_controller_chars(const char *text) {
  if (!text)
    return;

  for (const char *p = text; *p; p++)
    app_controller_handle_key((unsigned char)*p);
}

  static void run_target_contest_entry_case(
    const char *tmp_dir, const char *case_name, const char *contest_text,
    const char *station_call, const char *station_exchange,
    const char *worked_call, const char *received_exchange,
    const char *expected_sent, int frequency_khz, int expected_points,
    int expected_total_points) {
    char case_dir[512];
    snprintf(case_dir, sizeof(case_dir), "%s/%s", tmp_dir, case_name);
    expect_int_eq(mkdir(case_dir, 0777), 0,
          "create isolated target-contest controller case");

    char path[512];
    join_path(path, sizeof(path), case_dir, "contest.conf");
    expect_int_eq(write_text_file(path, contest_text), 0,
          "write target contest definition");
    join_path(path, sizeof(path), case_dir, "wl_cty.dat");
    const char *cty_text =
      "Poland:15:28:EU:52.0:21.0:0:SP:\n"
      "SP,HF,SN,SO,3Z,SQ;\n"
      "United States:5:8:NA:38.0:-97.0:0:K:\n"
        "K;\n"
        "United States ITU 28:5:28:NA:38.0:-97.0:0:WZ:\n"
        "WZ;\n"
      "United Kingdom:14:27:EU:52.0:0.0:0:G:\n"
      "G;\n";
    expect_int_eq(write_text_file(path, cty_text), 0,
          "write target-contest CTY fixture");
    join_path(path, sizeof(path), case_dir, "logger.conf");
    char logger_conf[256];
    snprintf(logger_conf, sizeof(logger_conf),
             "CONTEST_DEF_FILE=contest.conf\nSTATION_CALL=%s\nSTATION_EXCHANGE=%s\nSTATION_TX_POWER_WATTS=75\n",
         station_call, station_exchange ? station_exchange : "");
    expect_int_eq(write_text_file(path, logger_conf), 0,
          "write target-contest logger configuration");

    char old_cwd[512];
    expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
          "getcwd before target-contest controller case");
    set_test_db_path(case_dir);
    expect_int_eq(chdir(case_dir), 0,
          "enter target-contest controller case directory");

    struct tm test_utc = {0};
    test_utc.tm_year = 2026 - 1900;
    test_utc.tm_hour = 16;
    if (strstr(case_name, "spdx")) {
      test_utc.tm_mon = 3;
      test_utc.tm_mday = 4;
    } else if (strstr(case_name, "iaru")) {
      test_utc.tm_mon = 6;
      test_utc.tm_mday = 11;
    } else if (strstr(case_name, "wpx")) {
      test_utc.tm_mon = 4;
      test_utc.tm_mday = 30;
    } else {
      test_utc.tm_mon = 10;
      test_utc.tm_mday = 28;
    }
    contest_rules_set_test_time(timegm(&test_utc));

    app_controller_init();
    char frequency_text[16];
    snprintf(frequency_text, sizeof(frequency_text), "%d", frequency_khz);
    send_controller_text(frequency_text);
    send_controller_chars(worked_call);
    app_controller_handle_key(APP_KEY_SPACE);
    send_controller_chars(received_exchange);
    app_controller_handle_key(APP_KEY_ENTER);

    expect_int_eq(qso_count, 1, "target-contest QSO should be logged");
    if (qso_count > 0) {
    expect_str_eq(logbook[0].exchange_sent, expected_sent,
            "target contest should send the station-specific exchange");
    expect_str_eq(logbook[0].exchange_recv, received_exchange,
            "target contest should preserve received exchange");
    expect_int_eq(logbook[0].points, expected_points,
            "target contest should calculate official QSO points");
    }
    if (expected_total_points >= 0) {
    stats_update();
    expect_int_eq(stats.contest_qso_points, expected_total_points,
            "aggregate score should preserve official zero-point QSOs");
    }

    app_controller_shutdown();
    expect_int_eq(chdir(old_cwd), 0,
          "restore cwd after target-contest controller case");
    set_test_db_path(tmp_dir);
    qso_init();
  }

  static void test_target_contest_exchange_and_points(const char *tmp_dir) {
    const char *cqww =
      "NAME=CQ-WW-CW\n"
      "CABRILLO_NAME=CQ-WW-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=CQZONE\n"
      "EXCHANGE_RECEIVED_TYPE=CQ_ZONE\n"
      "MULTIPLIER=CQWW\n"
      "FIELD=ZONE,CQ Zone,required\n"
      "SCORING_RULE=0;CALL_SUFFIX=/MM,/AM\n"
      "SCORING_RULE=0;SAME_COUNTRY=1;CALL_PREFIX=!IG9,!IH9\n"
      "SCORING_RULE=2;SOURCE_CONTINENT=NA;DESTINATION_CONTINENT=NA\n"
      "SCORING_RULE=1;SAME_CONTINENT=1\n"
      "SCORING_RULE=3\n";
    run_target_contest_entry_case(tmp_dir, "cqww_dx_points_case", cqww,
                                  "SP9HOME", "", "K1AAA", "5", "15", 14020, 3, -1);
    run_target_contest_entry_case(tmp_dir, "cqww_same_country_case", cqww,
                                  "SP9HOME", "", "SP2XYZ", "15", "15", 14020, 0, 0);

    const char *wpx =
      "NAME=CQ-WPX-CW\n"
      "CABRILLO_NAME=CQ-WPX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "SERIAL_WIDTH=3\n"
      "MULTIPLIER=PREFIX\n"
      "FIELD=SERIAL,Serial Number,required\n"
      "SCORING_RULE=1;SAME_COUNTRY=1\n"
      "SCORING_RULE=4;SOURCE_CONTINENT=NA;DESTINATION_CONTINENT=NA;BAND_CLASS=LOW\n"
      "SCORING_RULE=2;SOURCE_CONTINENT=NA;DESTINATION_CONTINENT=NA\n"
      "SCORING_RULE=2;SAME_CONTINENT=1;BAND_CLASS=LOW\n"
      "SCORING_RULE=1;SAME_CONTINENT=1\n"
      "SCORING_RULE=6;BAND_CLASS=LOW\n"
      "SCORING_RULE=3\n";
    run_target_contest_entry_case(tmp_dir, "wpx_points_case", wpx,
                                  "SP9HOME", "", "G3ABC", "001", "001", 14020, 1, -1);
    run_target_contest_entry_case(tmp_dir, "wpx_low_band_points_case", wpx,
                                  "SP9HOME", "", "G3ABC", "001", "001", 7020, 2, -1);

    const char *spdx =
      "NAME=SP-DX\n"
      "CABRILLO_NAME=SP-DX\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=SPDX_EXCHANGE\n"
      "EXCHANGE_RECEIVED_TYPE=SP_PROVINCE_OR_SERIAL\n"
      "SERIAL_WIDTH=3\n"
      "MULTIPLIER=SPDX\n"
      "FIELD=EXCHANGE,Province or Serial,required\n"
      "REGION=SP;COUNTRIES=Poland;EXCHANGES=B,C,D,F,G,J,K,L,M,O,P,R,S,U,W,Z\n"
      "SCORING_RULE=0;SOURCE_REGION=SP;DESTINATION_REGION=SP\n"
      "SCORING_RULE=3;SOURCE_REGION=!SP;DESTINATION_REGION=SP\n"
      "SCORING_RULE=1;SOURCE_REGION=SP;DESTINATION_REGION=!SP;DESTINATION_CONTINENT=EU\n"
      "SCORING_RULE=3;SOURCE_REGION=SP;DESTINATION_REGION=!SP\n"
      "SCORING_RULE=0\n";
    run_target_contest_entry_case(tmp_dir, "spdx_sp_points_case", spdx,
                                  "SP9HOME", "B", "K1AAA", "001", "B", 14020, 3, -1);
    run_target_contest_entry_case(tmp_dir, "spdx_dx_points_case", spdx,
                                  "K1HOME", "", "SP9AAA", "B", "001", 14020, 3, -1);
    run_target_contest_entry_case(tmp_dir, "spdx_visiting_sp_points_case", spdx,
                                  "K1HOME", "B", "G3ABC", "001", "B", 14020, 1,
                                  -1);

    const char *iaru =
      "NAME=IARU-HF-CHAMPIONSHIP\n"
      "CABRILLO_NAME=IARU-HF\n"
      "MODE=MIXED\n"
      "EXCHANGE_SENT=IARU_EXCHANGE\n"
      "EXCHANGE_RECEIVED_TYPE=ITU_ZONE_OR_HQ\n"
      "DUPLICATE_MODE_SENSITIVE=0\n"
      "MULTIPLIER=IARU\n"
      "FIELD=EXCHANGE,ITU Zone, HQ Society or Official,required\n"
      "SCORING_RULE=1;EXCHANGE_CLASS=ALPHA\n"
      "SCORING_RULE=1;EXCHANGE_VALUE=AC,R1,R2,R3\n"
      "SCORING_RULE=1;STATION_EXCHANGE_CLASS=ALPHA\n"
      "SCORING_RULE=1;SAME_ITU_ZONE=1\n"
      "SCORING_RULE=3;SAME_CONTINENT=1\n"
      "SCORING_RULE=5\n";
    run_target_contest_entry_case(tmp_dir, "iaru_zone_points_case", iaru,
                                  "SP9HOME", "", "K1AAA", "8", "28", 14020, 5, -1);
    run_target_contest_entry_case(tmp_dir, "iaru_same_zone_other_continent_case",
                                  iaru, "SP9HOME", "", "WZ1ABC", "28", "28",
                                  14020, 1, -1);
    run_target_contest_entry_case(tmp_dir, "iaru_hq_points_case", iaru,
                                  "SP9HOME", "", "K1HQ", "ARRL", "28", 14020, 1, -1);
    run_target_contest_entry_case(tmp_dir, "iaru_local_hq_station_case", iaru,
                                  "SP9HOME", "ARRL", "K1ABC", "8", "ARRL", 14020, 1,
                                  -1);

    char case_dir[512];
    snprintf(case_dir, sizeof(case_dir), "%s/iaru_same_band_dupe_case", tmp_dir);
    expect_int_eq(mkdir(case_dir, 0777), 0,
                  "create IARU cross-mode dupe test case");
    char path[512];
    join_path(path, sizeof(path), case_dir, "contest.conf");
    expect_int_eq(write_text_file(path, iaru), 0,
                  "write IARU cross-mode dupe contest definition");
    join_path(path, sizeof(path), case_dir, "wl_cty.dat");
    const char *cty_text =
        "Poland:15:28:EU:52.0:21.0:0:SP:\nSP;\n"
        "United States:5:8:NA:38.0:-97.0:0:K:\nK;\n";
    expect_int_eq(write_text_file(path, cty_text), 0,
                  "write IARU cross-mode CTY fixture");
    join_path(path, sizeof(path), case_dir, "logger.conf");
    expect_int_eq(write_text_file(path,
        "CONTEST_DEF_FILE=contest.conf\nSTATION_CALL=SP9HOME\nSTATION_TX_POWER_WATTS=75\n"),
        0, "write IARU cross-mode logger config");
    char old_cwd[512];
    expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
                "getcwd before IARU cross-mode dupe case");
    set_test_db_path(case_dir);
    expect_int_eq(chdir(case_dir), 0,
                  "enter IARU cross-mode dupe test directory");
    struct tm iaru_time = {0};
    iaru_time.tm_year = 2026 - 1900;
    iaru_time.tm_mon = 6;
    iaru_time.tm_mday = 11;
    iaru_time.tm_hour = 16;
    contest_rules_set_test_time(timegm(&iaru_time));
    app_controller_init();
    send_controller_text("14020");
    send_controller_chars("K1ABC");
    app_controller_handle_key(APP_KEY_SPACE);
    send_controller_chars("8");
    app_controller_handle_key(APP_KEY_ENTER);
    send_controller_text("14150");
    send_controller_chars("K1ABC");
    app_controller_handle_key(APP_KEY_SPACE);
    send_controller_chars("8");
    app_controller_handle_key(APP_KEY_ENTER);
    expect_int_eq(qso_count, 2,
                  "IARU cross-mode contact remains in the audit log");
    if (qso_count >= 2) {
      expect_true(logbook[1].invalid,
                  "IARU repeat on a different mode is a duplicate on that band");
      expect_int_eq(logbook[1].points, 0,
                    "IARU cross-mode duplicate should receive zero points");
    }
    app_controller_shutdown();
    contest_rules_set_test_time((time_t)-1);
    expect_int_eq(chdir(old_cwd), 0,
                  "restore cwd after IARU cross-mode dupe case");
    set_test_db_path(tmp_dir);
    qso_init();
  }

static void test_controller_contest_mode_points(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/mode_points_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for mode points test");

  char contest_path[512];
    strncpy(contest_path, case_dir, sizeof(contest_path) - 1);
    contest_path[sizeof(contest_path) - 1] = '\0';
  strncat(contest_path, "/contest.conf",
      sizeof(contest_path) - strlen(contest_path) - 1);

  const char *contest_text =
      "NAME=MODE-POINTS\n"
      "CABRILLO_NAME=MODE-POINTS\n"
      "MODE=MIXED\n"
      "POINTS_PER_QSO=3\n"
      "POINTS_CW=5\n"
      "POINTS_PHONE=1\n"
      "POINTS_DIGI=2\n"
      "MULTIPLIER=NONE\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write mode points contest definition");

    char old_cwd[512];
    expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
          "getcwd before controller points test");
    expect_int_eq(chdir(case_dir), 0, "chdir to controller test temp dir");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  const int base_qso_count = qso_count;
  AppRenderState state;
  char expected_tx_before[16];
  char expected_tx_after_first[16];
  char expected_status_after_first[64];

  snprintf(expected_tx_before, sizeof(expected_tx_before), "%d",
           base_qso_count + 1);
  snprintf(expected_tx_after_first, sizeof(expected_tx_after_first), "%d",
           base_qso_count + 2);
  snprintf(expected_status_after_first, sizeof(expected_status_after_first),
           "QSO OK TX:%s RX:599", expected_tx_before);

  app_controller_get_render_state(&state);
  expect_true(state.contest_entry_mode,
              "contest mode should be active when contest definition is loaded");
  expect_true(state.contest_exchange_sent != NULL,
              "contest sent exchange should be available in render state");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_tx_before,
                  "contest TX exchange should be live before first saved QSO");

  send_controller_chars("SP9BAD");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("ABCD");
  app_controller_handle_key(APP_KEY_ENTER);

  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, base_qso_count,
                "non-numeric serial exchange should be rejected");
  expect_true(state.status != NULL, "invalid exchange status should exist");
  if (state.status)
    expect_true(strstr(state.status, "must be numeric") != NULL,
                "invalid exchange should report numeric requirement");

  send_controller_text("7020");
  send_controller_chars("SP9AAA");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("599");
  app_controller_handle_key(APP_KEY_ENTER);

  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "contest save status should exist");
  if (state.status)
    expect_true(strstr(state.status, expected_status_after_first) != NULL,
                "contest status should display sent and received exchange");
  expect_true(state.contest_exchange_sent != NULL,
              "next contest TX exchange should stay visible");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_tx_after_first,
                  "contest TX exchange should update live after first saved QSO");

  send_controller_text("14074");
  send_controller_chars("SP9BBB");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("599");
  app_controller_handle_key(APP_KEY_ENTER);

  send_controller_text("14150");
  send_controller_chars("SP9CCC");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("599");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 3,
                "three QSOs saved with mode-specific points");
  expect_int_eq(logbook[base_qso_count].points, 5, "CW points rule applied");
  expect_int_eq(logbook[base_qso_count + 1].points, 2,
                "DIGI points rule applied");
  expect_int_eq(logbook[base_qso_count + 2].points, 1,
                "PHONE points rule applied");

  stats_update();
  expect_true(stats.contest_qso_points >= 8,
              "contest qso points include per-mode values");

  app_controller_shutdown();
  contest_rules_set_test_time((time_t)-1);
  expect_int_eq(chdir(old_cwd), 0, "restore cwd after controller points test");
}

static void test_target_contest_presets(const char *tmp_dir) {
  (void)tmp_dir;
  const struct {
    const char *path;
    ContestMultiplierType multiplier;
      int multiplier_count;
  } cases[] = {
      {"contest_defs/sp_dx.conf", CONTEST_MULT_SPDX, 1},
      {"contest_defs/cq_wpx_cw.conf", CONTEST_MULT_PREFIX, 1},
      {"contest_defs/cq_wpx_ssb.conf", CONTEST_MULT_PREFIX, 1},
      {"contest_defs/cq_ww_cw.conf", CONTEST_MULT_DXCC_PER_BAND, 2},
      {"contest_defs/cq_ww_ssb.conf", CONTEST_MULT_DXCC_PER_BAND, 2},
      {"contest_defs/iaru_hf_championship.conf", CONTEST_MULT_IARU, 1},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ContestDefinition definition;
    char error[128] = {0};
    char preset_path[512] = {0};
    snprintf(preset_path, sizeof(preset_path), "%s/%s", LOGGER_SOURCE_DIR,
             cases[i].path);
    expect_int_eq(contest_definition_load(preset_path, &definition, error,
                                          sizeof(error)),
                  0, "target contest preset should load");
    expect_int_eq((int)definition.multiplier_type, (int)cases[i].multiplier,
                  "target contest preset should select official multiplier mode");
    expect_int_eq(definition.multiplier_count, cases[i].multiplier_count,
            "target contest preset should declare all multiplier families");
    expect_true(definition.score_rule_count > 0,
          "target preset should declare contest-specific scoring rules");
  }
}

static void test_manual_frequency_entry_from_call_field(void) {
  AppRenderState state;

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  expect_int_eq(qso_count, 0, "manual freq test starts from clean log");

  send_controller_text("14074");
  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, 0,
                "manual frequency entry should not create a QSO");
  expect_true(state.status != NULL, "manual frequency status should exist");
  if (state.status)
    expect_true(strstr(state.status, "Frequency set to 14074 kHz") != NULL,
                "numeric call field should set manual frequency");

  send_controller_chars("SP9ABC");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("599");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, 1, "split entry should create one QSO");
  expect_int_eq(logbook[0].freq, 14074,
                "split entry should use manually selected frequency");

  app_controller_shutdown();
}

static void test_named_log_commands(const char *tmp_dir) {
  AppRenderState state;
  char named_log[64];
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/named_logs_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for named-log test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to named-log test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for named-log test");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  expect_int_eq(qso_count, 0, "start named-log test from clean logbook");

  send_controller_text("SP9ABC 14074 599");
  expect_int_eq(qso_count, 1, "one QSO added before named archive");

  snprintf(named_log, sizeof(named_log), "Summer_Contest_%d", (int)getpid());
  char command[128];
  snprintf(command, sizeof(command), "newlog %s", named_log);
  send_controller_text(command);
  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, 0, "newlog <name> clears active logbook");
  expect_true(state.status != NULL, "newlog status exists");
  if (state.status)
    expect_true(strstr(state.status, named_log) != NULL,
                "newlog should confirm selected name");

  send_controller_text("logs");
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "logs status exists");
  expect_true(state.info != NULL, "logs info exists");
  if (state.status)
    expect_true(strstr(state.status, "Named logs:") != NULL,
                "logs should report available named archives");
  if (state.info)
    expect_true(strstr(state.info, named_log) != NULL,
                "logs output should include archived log name");

  app_controller_handle_key(APP_KEY_F3);
  expect_int_eq(qso_count, 1,
                "previous log should restore original QSO set");

  snprintf(command, sizeof(command), "openlog %s", named_log);
  send_controller_text(command);
  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, 1,
                "openlog <name> opens the archived QSO set");
  expect_true(state.status != NULL, "openlog status exists");
  if (state.status)
    expect_true(strstr(state.status, named_log) != NULL,
                "openlog should confirm selected log name");

  app_controller_shutdown();
  chdir("..");
}

static void test_newlog_creates_database_file(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/newlog_file_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for newlog file creation test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to newlog file creation test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for newlog file creation test");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  expect_int_eq(qso_count, 0,
                "newlog file creation test starts from clean logbook");

  const char *runtime_dir = config_runtime_dir();
  expect_true(runtime_dir != NULL && runtime_dir[0] == '/',
              "runtime dir should be available before newlog file creation");
  if (!runtime_dir || runtime_dir[0] != '/') {
    app_controller_shutdown();
    chdir("..");
    return;
  }

  char log_name[64];
  snprintf(log_name, sizeof(log_name), "UnitLogFile_%d", (int)getpid());

  char expected_db_path[768];
  snprintf(expected_db_path, sizeof(expected_db_path), "%s/logs/%s.db",
           runtime_dir, log_name);

  (void)unlink(expected_db_path);

  char command[160];
  snprintf(command, sizeof(command), "newlog %s", log_name);
  send_controller_text(command);

  app_controller_get_render_state(&state);
  expect_true(state.status != NULL,
              "newlog file creation status should exist");
  if (state.status)
    expect_true(strstr(state.status, "New log created:") != NULL,
                "newlog should report successful creation");

  struct stat st;
  expect_int_eq(stat(expected_db_path, &st), 0,
                "newlog should create a new .db file on disk");
  if (stat(expected_db_path, &st) == 0)
    expect_true(S_ISREG(st.st_mode),
                "created log database path should be a regular file");

  app_controller_shutdown();
  chdir("..");
}

static void test_syncstatus_command_reports_failed_queue(const char *tmp_dir) {
  AppRenderState state;
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/syncstatus_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for syncstatus test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to syncstatus test directory");
  set_test_db_path(case_dir);
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for syncstatus test");

  app_controller_init();

  long long seq = 0;
  expect_int_eq(db_sync_next_station_seq(&seq), 0,
                "allocate station seq for syncstatus test");
  expect_int_eq(db_sync_outbox_enqueue("op-syncstatus-1", seq, 1, "QSO_INSERT",
                                       "q-syncstatus-1",
                                       "{\"kind\":\"qso_insert\"}",
                                       "2026-01-01T00:00:00Z"),
                0, "enqueue outbox op for syncstatus test");

  for (int i = 0; i < 6; i++) {
    expect_int_eq(db_sync_outbox_mark_retry("op-syncstatus-1", 1), 0,
                  "advance retry count for syncstatus test");
  }

  send_controller_text("syncstatus");
  app_controller_get_render_state(&state);

  expect_true(state.status != NULL, "syncstatus should set status text");
  expect_true(state.info != NULL, "syncstatus should set info text");
  if (state.status) {
    expect_true(strstr(state.status, "SYNC pending=0") != NULL,
                "syncstatus should report zero pending operations");
    expect_true(strstr(state.status, "failed=1") != NULL,
                "syncstatus should report one failed operation");
    expect_true(strstr(state.status, "connected=0") != NULL,
                "syncstatus should report disconnected state when NET is off");
  }
  if (state.info) {
    expect_true(strstr(state.info, "station=") != NULL,
                "syncstatus info should include station marker");
    expect_true(strstr(state.info, "seq=") != NULL,
                "syncstatus info should include cursor sequence");
  }

  app_controller_shutdown();
  chdir("..");
}

static void test_contest_preset_from_build_dir_uses_defined_settings(void) {
  AppRenderState state;
  char old_cwd[512];
  int mkdir_rc;

  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before contest preset path test");

  errno = 0;
  mkdir_rc = mkdir("build", 0777);
  expect_true(mkdir_rc == 0 || errno == EEXIST,
              "build directory should exist for contest preset path test");
  expect_int_eq(chdir("build"), 0,
                "chdir to build for contest preset path test");
  set_test_db_path(".");
  expect_int_eq(write_text_file("logger.conf",
                               "CONTEST_DEF_FILE=\n"
                               "STATION_TX_POWER_WATTS=75\n"), 0,
                "write isolated logger.conf for contest preset test");

  struct tm wpx_test_time = {0};
  wpx_test_time.tm_year = 2026 - 1900;
  wpx_test_time.tm_mon = 4;
  wpx_test_time.tm_mday = 30;
  wpx_test_time.tm_hour = 16;
  contest_rules_set_test_time(timegm(&wpx_test_time));
  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  expect_int_eq(qso_count, 0,
                "contest preset test should start from clean logbook");

  app_controller_submit_command_text("contest contest_defs/cq_wpx_cw.conf");
  app_controller_get_render_state(&state);
  expect_true(state.contest_entry_mode,
              "contest preset from contest_defs should enable contest mode");
  expect_true(state.status != NULL, "contest preset status should exist");
  if (state.status)
    expect_true(strstr(state.status, "Contest loaded: CQ-WPX-CW") != NULL,
                "contest preset should load CQ-WPX-CW definition");
  expect_true(state.contest_exchange_sent != NULL,
              "contest preset should expose generated TX exchange");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "001",
                  "EXCHANGE_SENT=# should start incremental exchange from 1");

  send_controller_text("7020");
  send_controller_chars("SP9WPX");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("100");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, 1,
                "contest preset test should save one QSO");
  expect_str_eq(logbook[0].exchange_sent, "001",
                "contest preset should save incremented TX exchange from preset");
  expect_str_eq(logbook[0].exchange_recv, "100",
                "contest preset should save entered RX exchange");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "next TX exchange should remain visible after first saved QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "002",
                  "EXCHANGE_SENT=# should always increment upward after save");

  app_controller_shutdown();
  contest_rules_set_test_time((time_t)-1);
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after contest preset path test");
}

static void test_missing_default_contest_file_is_nonfatal(void) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/missing_default_contest_case", "/tmp");

  /* Ensure the named directory is clean and isolated from the repository state. */
  if (mkdir(case_dir, 0777) != 0 && errno != EEXIST) {
    failf("create isolated directory for missing default contest test");
    return;
  }

  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/logger.conf", case_dir);
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=contest.conf\n"), 0,
                "write logger.conf with default missing contest path");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before missing default contest file test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to missing default contest file test directory");

  app_controller_init();

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(!state.contest_entry_mode,
              "missing default contest file should keep contest mode off");
  expect_true(state.contest_exchange_label == NULL ||
                  strcmp(state.contest_exchange_label, "EXCH") == 0,
              "missing default contest file should leave contest label unset or default");

  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after missing default contest file test");
}

static void test_current_directory_config_has_priority_over_runtime_copy(void) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/cwd_priority_case", "/tmp");
  if (mkdir(case_dir, 0777) != 0 && errno != EEXIST) {
    failf("create isolated directory for cwd config priority test");
    return;
  }

  char contest_path[512];
  if (snprintf(contest_path, sizeof(contest_path), "%s/contest.conf", case_dir) >=
      (int)sizeof(contest_path)) {
    failf("contest_path buffer too small for cwd config priority test");
    return;
  }

  char logger_conf_path[512];
  if (snprintf(logger_conf_path, sizeof(logger_conf_path), "%s/logger.conf", case_dir) >=
      (int)sizeof(logger_conf_path)) {
    failf("logger_conf_path buffer too small for cwd config priority test");
    return;
  }

  const char *contest_text =
      "NAME=CWD-PRIORITY\n"
      "CABRILLO_NAME=CWD-PRIORITY\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write local contest file for cwd priority test");
  expect_int_eq(write_text_file(logger_conf_path,
                "CONTEST_DEF_FILE=contest.conf\n"
                "STATION_CALL=ZZ1LOCAL\n"), 0,
                "write local logger.conf for cwd priority test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before cwd config priority test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to cwd config priority test directory");

  config_load("logger.conf");
  expect_str_eq(config.contest_definition_path, "contest.conf",
                "cwd logger.conf should keep a relative contest path");
  expect_str_eq(config.station_call, "ZZ1LOCAL",
                "cwd logger.conf should override runtime defaults");

  char resolved[512] = {0};
  expect_int_eq(config_resolve_contest_path("contest.conf", resolved, sizeof(resolved)), 0,
                "contest path should resolve from the current directory");
  expect_true(strstr(resolved, "/contest.conf") != NULL,
              "resolved contest path should point at the local contest file");

  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after cwd config priority test");
}

static void test_openlog_restores_saved_contest_definition(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/openlog_contest_restore", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for openlog contest restore test");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=\n"), 0,
                "write logger.conf for openlog contest restore test");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "wae_restore.conf");
  const char *contest_text =
      "NAME=WAE-RESTORE\n"
      "CABRILLO_NAME=WAE-DX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write WAE restore contest definition");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before openlog contest restore test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to openlog contest restore test directory");

  app_controller_init();
  app_controller_submit_command_text("contest wae_restore.conf");

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_entry_mode,
              "contest should load before saving log state");

  expect_int_eq(db_archive_current_logbook_named("Contest Restore Log"), 0,
                "archive current logbook before reopening it");

  app_controller_submit_command_text("openlog Contest Restore Log");
  app_controller_get_render_state(&state);
  expect_true(state.contest_entry_mode,
              "opening a named log should restore the saved contest definition");
  expect_true(state.contest_name != NULL,
              "reopened log should expose a contest name");
  if (state.contest_name)
    expect_true(strstr(state.contest_name, "WAE") != NULL,
                "reopened log should restore WAE contest metadata");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after openlog contest restore test");
}

static void test_contest_import_only_does_not_autoload_or_set_active_path(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/import_only_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create isolated directory for import-only command test");

  char src_path[512];
  char out_path[512];
  char conf_path[512];
  strncpy(src_path, case_dir, sizeof(src_path) - 1);
  src_path[sizeof(src_path) - 1] = '\0';
  strncat(src_path, "/raw_dxlog_import_only.txt",
    sizeof(src_path) - strlen(src_path) - 1);

  strncpy(out_path, case_dir, sizeof(out_path) - 1);
  out_path[sizeof(out_path) - 1] = '\0';
  strncat(out_path, "/import_only.conf",
    sizeof(out_path) - strlen(out_path) - 1);

  strncpy(conf_path, case_dir, sizeof(conf_path) - 1);
  conf_path[sizeof(conf_path) - 1] = '\0';
  strncat(conf_path, "/logger.conf",
    sizeof(conf_path) - strlen(conf_path) - 1);

  const char *raw_dxlog_text =
      "CONTESTNAME=IMPORT-ONLY-CHECK\n"
      "MODES=CW;SSB\n"
      "MULT1_TYPE=DXCC\n"
      "MULT1_COUNT=PER_BAND\n"
      "FIELD_RCVD_TYPE=NR\n"
      "POINTS_FIELD_BAND_MODE=ALL;ALL;ALL;ALL;3\n";

  expect_int_eq(write_text_file(src_path, raw_dxlog_text), 0,
                "write raw DXLog for import-only command test");
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for import-only command test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before import-only command test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to import-only command test directory");

  app_controller_init();

  char original_path[sizeof(config.contest_definition_path)];
  snprintf(original_path, sizeof(original_path), "%s",
           config.contest_definition_path);

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(!state.contest_entry_mode,
              "startup without contest definition should keep contest mode off");

  app_controller_submit_command_text(
      "contest import-only raw_dxlog_import_only.txt import_only.conf");

  app_controller_get_render_state(&state);

  expect_true(state.status != NULL, "import-only status should exist");
  if (state.status)
    expect_true(strstr(state.status, "Contest imported (not loaded)") != NULL,
                "import-only should report imported but not loaded status");

  expect_true(!state.contest_entry_mode,
              "import-only should not auto-load contest entry mode");
  expect_str_eq(config.contest_definition_path, original_path,
                "import-only should not change active contest definition path");

  char *imported = read_whole_file(out_path);
  expect_true(imported != NULL,
              "import-only command should generate output contest file");
  if (imported)
    expect_true(strstr(imported, "NAME=IMPORT-ONLY-CHECK") != NULL,
                "import-only output should contain normalized contest name");
  free(imported);

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after import-only command test");
}

static void test_wae_qso_scoring(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/wae_scoring", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create WAE scoring test directory");

  char contest_path[512];
  join_path(contest_path, sizeof(contest_path), case_dir, "wae.conf");

  const char *contest_text =
      "NAME=WAE-TEST-SCORING\n"
      "CABRILLO_NAME=DARC-WAEDC-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "POINTS_PER_QSO=1\n"
      "MULTIPLIER=DXCC_PER_BAND\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Rcv Nr,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write WAE scoring contest definition");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before WAE scoring test");
  expect_int_eq(chdir(case_dir), 0, "chdir to WAE scoring test directory");

  app_controller_init();
  const int base_qso_count = qso_count;

  /* Load the WAE contest definition. */
  app_controller_submit_command_text("contest wae.conf");

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.contest_entry_mode,
              "WAE contest mode should be active");

  /*
   * Verify QTC is flagged as enabled in the render state.
   * Note: app_controller_qtc_enabled() checks loaded definition only;
   * qtc_can_send() additionally checks CTY (which requires wl_cty.dat).
   * Since CTY is unavailable in the test environment, we only test
   * the definition-level flag here.
   */
  expect_true(state.qtc_enabled, "WAE contest should report qtc_enabled=true");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0, "restore cwd after WAE scoring test");
  (void)base_qso_count;
}

static void test_qtc_enabled_for_opened_wae_log_without_loaded_definition(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/wae_qtc_openlog", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create WAE openlog QTC test directory");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=\n"), 0,
                "write logger.conf without contest definition");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before WAE openlog QTC test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to WAE openlog QTC test directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);

  char status[128] = {0};
  int idx = qso_add_contest_fields(
      "W1AW", 14025, "599", "CW", "", "001", "123", "RUN",
      "DARC-WAEDC-CW", 1, 1, 0, status, sizeof(status));
  expect_true(idx >= 0, "WAE-tagged QSO should be added");

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(!state.contest_entry_mode,
              "contest mode should remain disabled without loaded definition");
  expect_true(state.qtc_enabled,
              "WAE-tagged log should enable QTC fallback");

  expect_int_eq(app_controller_handle_key(APP_KEY_CTRL_L),
                APP_CTRL_EVENT_OPEN_QTC_WINDOW,
                "Ctrl+L should open QTC window for opened WAE log");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after WAE openlog QTC test");
}

static void test_qtc_sendable_prefill_uses_call_and_received_exchange(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/qtc_prefill_values", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create QTC prefill values test directory");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=\n"), 0,
                "write logger.conf for QTC prefill values test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before QTC prefill values test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to QTC prefill values test directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);

  char status[128] = {0};
  int idx = qso_add_contest_fields(
      "W1AW", 14025, "599", "CW", "", "001", "123", "RUN",
      "DARC-WAEDC-CW", 1, 1, 0, status, sizeof(status));
  expect_true(idx >= 0, "QSO for QTC prefill should be added");

  /* Simulate one malformed legacy row that should never be offered as QTC. */
  QSO bad;
  memset(&bad, 0, sizeof(bad));
  snprintf(bad.call, sizeof(bad.call), "%s", "NOCALL");
  snprintf(bad.date, sizeof(bad.date), "%s", "20260101");
  snprintf(bad.utc, sizeof(bad.utc), "%s", "1200");
  logbook[qso_count++] = bad;

  QTCRecord sendable[QTC_MAX_RECORDS_PER_BUNDLE];
  memset(sendable, 0, sizeof(sendable));
  const int n = app_controller_qtc_get_sendable(sendable,
                                                 QTC_MAX_RECORDS_PER_BUNDLE);

  expect_true(n >= 1, "QTC prefill should return at least one record");
  if (n >= 1) {
    expect_str_eq(sendable[0].call, "W1AW",
                  "QTC prefill should expose worked callsign");
    expect_str_eq(sendable[0].exch, "123",
                  "QTC prefill should use received exchange, not sent serial");
  }

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after QTC prefill values test");
}

static void test_qtc_enabled_for_opened_log_with_qtc_bundles_only(
    const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/qtc_enabled_bundles_only", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create QTC bundles-only test directory");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  expect_int_eq(write_text_file(conf_path, "CONTEST_DEF_FILE=\n"), 0,
                "write logger.conf without contest definition for bundles-only test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before QTC bundles-only test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to QTC bundles-only test directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);

  qso_count = 0;
  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call, sizeof(b.sender_call), "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr = 1;
  b.record_count = 1;
  b.sent = 1;
  qtc_record_init(&b.records[0], "20260101", "1200", "DL1ABC", "77");
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  AppRenderState state;
  app_controller_get_render_state(&state);
  expect_true(state.qtc_enabled,
              "log with existing QTC bundles should enable QTC fallback");

  expect_int_eq(app_controller_handle_key(APP_KEY_CTRL_L),
                APP_CTRL_EVENT_OPEN_QTC_WINDOW,
                "Ctrl+L should open QTC window when bundles exist");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after QTC bundles-only test");
}

/* ------------------------------------------------------------------ */
/* QTC unit tests                                                       */
/* ------------------------------------------------------------------ */

static void test_qtc_record_init(void) {
  QTCRecord r;
  qtc_record_init(&r, "20241201", "1430", "DK5AI", "42");
  expect_str_eq(r.date, "20241201", "qtc record date");
  expect_str_eq(r.time, "1430",     "qtc record time");
  expect_str_eq(r.call, "DK5AI",   "qtc record call");
  expect_str_eq(r.exch, "42",      "qtc record exch");
}

static void test_qtc_bundle_validate_valid(void) {
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr   = 1;
  b.record_count = 2;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI",  "42");
  qtc_record_init(&b.records[1], "20241201", "1432", "G3XYZ",  "43");
  b.sent = 1;

  char err[64] = {0};
  expect_int_eq(qtc_bundle_validate(&b, err, sizeof(err)), 0,
                "valid qtc bundle should pass validation");
  expect_str_eq(err, "", "error text should be empty for valid bundle");
}

static void test_qtc_bundle_validate_empty_sender(void) {
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  /* sender_call intentionally left empty */
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.record_count = 1;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI", "42");

  char err[64] = {0};
  expect_int_eq(qtc_bundle_validate(&b, err, sizeof(err)), -1,
                "empty sender should fail validation");
}

static void test_qtc_bundle_validate_record_count_zero(void) {
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.record_count = 0;  /* invalid: must be >= 1 */

  char err[64] = {0};
  expect_int_eq(qtc_bundle_validate(&b, err, sizeof(err)), -1,
                "zero record_count should fail validation");
}

static void test_qtc_bundle_validate_too_many_records(void) {
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.record_count = QTC_MAX_RECORDS_PER_BUNDLE + 1;

  char err[64] = {0};
  expect_int_eq(qtc_bundle_validate(&b, err, sizeof(err)), -1,
                "record_count > 10 should fail validation");
}

static void test_qtc_qso_already_sent(void) {
  /* Reset the QTC store before the test. */
  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 1;
  b.sent         = 1;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI", "42");

  /* Manually add to in-memory store without DB. */
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  expect_int_eq(qtc_qso_already_sent("DK5AI", "20241201", "1430"), 1,
                "QSO already in sent bundle should be detected");
  expect_int_eq(qtc_qso_already_sent("DK5AI", "20241201", "1431"), 0,
                "different time should not match");
  expect_int_eq(qtc_qso_already_sent("G3XYZ", "20241201", "1430"), 0,
                "different call should not match");

  /* Reset after test. */
  qtc_bundle_count = 0;
}

static void test_qtc_next_bundle_nr(void) {
  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  /* No bundles yet → next nr = 1. */
  expect_int_eq(qtc_next_bundle_nr("SP5XYZ", "W1AW"), 1,
                "next bundle nr with empty store should be 1");

  /* Add two sent bundles. */
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 1;
  b.sent         = 1;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI", "42");
  qtc_bundles[0] = b;
  b.bundle_nr    = 2;
  qtc_bundles[1] = b;
  qtc_bundle_count = 2;

  expect_int_eq(qtc_next_bundle_nr("SP5XYZ", "W1AW"), 3,
                "next bundle nr after two bundles should be 3");

  qtc_bundle_count = 0;
}

static void test_qtc_total_records(void) {
  qtc_bundle_count = 0;

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 3;
  b.sent         = 1;
  qtc_bundles[0] = b;
  b.bundle_nr    = 2;
  b.record_count = 5;
  qtc_bundles[1] = b;
  qtc_bundle_count = 2;

  expect_int_eq(qtc_total_records(), 8,
                "total QTC records should sum all bundles");

  qtc_bundle_count = 0;
}

static void test_contest_definition_qtc_fields(const char *tmp_dir) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/wae_test.conf", tmp_dir);

  const char *conf_text =
      "NAME=WAE-TEST\n"
      "CABRILLO_NAME=WAE-DX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "MULTIPLIER=DXCC_PER_BAND\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write WAE test contest definition");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                "WAE test definition should load without error");
  expect_str_eq(def.qtc_sender_side, "EU", "qtc_sender_side parsed correctly");
  expect_int_eq(def.points_per_qtc,  1,    "points_per_qtc parsed correctly");
}

static void test_wae_contest_definition_files(const char *tmp_dir) {
  (void)tmp_dir;

  /* Try loading the real WAE CW definition from contest_defs/. */
  ContestDefinition def;
  char err[64] = {0};

  const char *paths[] = {
      "contest_defs/wae_cw.conf",
      "../contest_defs/wae_cw.conf",
      "../../contest_defs/wae_cw.conf",
  };

  int loaded = 0;
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    if (contest_definition_load(paths[i], &def, err, sizeof(err)) == 0) {
      loaded = 1;
      break;
    }
  }

  if (!loaded) {
    /* Not finding the file is not a failure — skip further checks. */
    return;
  }

  expect_str_eq(def.qtc_sender_side, "EU",
                "WAE CW definition should have EU as QTC sender");
  expect_true(def.points_per_qtc > 0,
              "WAE CW definition should have positive points_per_qtc");
  /* Verify the correct Cabrillo name per DXLog WAE definition. */
  expect_str_eq(def.cabrillo_name, "DARC-WAEDC-CW",
                "WAE CW Cabrillo name should be DARC-WAEDC-CW");
  expect_int_eq((int)def.multiplier_type, (int)CONTEST_MULT_DXCC_PER_BAND,
                "WAE CW multiplier should be DXCC_PER_BAND");
}

static void test_stats_qtc_scoring(void) {
  /* Set up a minimal WAE-like contest definition. */
  ContestDefinition def;
  contest_definition_init_defaults(&def);
  snprintf(def.qtc_sender_side, sizeof(def.qtc_sender_side), "%s", "EU");
  def.points_per_qtc = 1;
  def.multiplier_type = CONTEST_MULT_NONE;

  stats_set_contest_definition(&def);

  /* Reset QTC store and add a bundle with 5 records. */
  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 5;
  b.sent         = 1;
  for (int i = 0; i < 5; i++)
    qtc_record_init(&b.records[i], "20241201", "1430", "DK5AI", "42");
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  stats_update();

  expect_int_eq(stats.qtc_records, 5, "stats qtc_records should count 5");
  expect_int_eq(stats.qtc_points,  5, "stats qtc_points = 5 * 1");

  /* Cleanup. */
  qtc_bundle_count = 0;
  ContestDefinition empty;
  contest_definition_init_defaults(&empty);
  stats_set_contest_definition(&empty);
}

static void test_stats_wag_multipliers(void) {
  const int saved_qso_count = qso_count;
  const Config saved_config = config;
  QSO saved_qsos[6];
  memcpy(saved_qsos, logbook, sizeof(saved_qsos));

  ContestDefinition def;
  contest_definition_init_defaults(&def);
  def.multiplier_type = CONTEST_MULT_WAG;
  stats_set_contest_definition(&def);

  qso_count = 4;
  memset(logbook, 0, sizeof(logbook));
  snprintf(config.station_call, sizeof(config.station_call), "%s", "SP9XYZ");
  snprintf(config.station_exchange, sizeof(config.station_exchange), "%s", "DOK1234");
  for (int i = 0; i < qso_count; i++) {
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", "K1ABC");
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", "United States");
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", "20M");
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    logbook[i].points = 3;
  }
  snprintf(logbook[2].mode, sizeof(logbook[2].mode), "%s", "SSB");
  snprintf(logbook[3].band, sizeof(logbook[3].band), "%s", "40M");
  stats_update();
  expect_int_eq(stats.contest_mults, 3,
                "WAG German DXCC multiplier should be unique per band and mode");

  qso_count = 6;
  snprintf(config.station_exchange, sizeof(config.station_exchange), "%s", "");
  snprintf(config.station_call, sizeof(config.station_call), "%s", "SP9XYZ");
  const char *received[] = {"C12", "C99", "C12", "NM", "B01", "C33"};
  const char *bands[] = {"20M", "20M", "40M", "80M", "20M", "20M"};
  const char *countries[] = {"Germany", "Germany", "Germany", "Germany", "Poland", "Germany"};
  for (int i = 0; i < qso_count; i++) {
    memset(&logbook[i], 0, sizeof(logbook[i]));
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", "DL1ABC");
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", countries[i]);
    snprintf(logbook[i].exchange_recv, sizeof(logbook[i].exchange_recv), "%s", received[i]);
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", bands[i]);
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s",
         (i == 2 || i == 5) ? "SSB" : "CW");
    logbook[i].points = 3;
  }
  stats_update();
  expect_int_eq(stats.contest_mults, 3,
                "WAG DOK multiplier should ignore repeat districts, NM, and non-DL QSOs");

  qso_count = 3;
  snprintf(config.station_exchange, sizeof(config.station_exchange), "%s", "DOK1234");
  for (int i = 0; i < qso_count; i++) {
    memset(&logbook[i], 0, sizeof(logbook[i]));
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", "Italy");
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", "20M");
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    logbook[i].points = 5;
  }
  snprintf(logbook[0].call, sizeof(logbook[0].call), "%s", "IK2ABC");
  snprintf(logbook[1].call, sizeof(logbook[1].call), "%s", "IG9ABC");
  snprintf(logbook[2].call, sizeof(logbook[2].call), "%s", "IH9ABC");
  stats_update();
  expect_int_eq(stats.contest_mults, 3,
                "WAG treats IG9 and IH9 as separate multipliers from Italy");

  qso_count = saved_qso_count;
  memcpy(logbook, saved_qsos, sizeof(saved_qsos));
  config = saved_config;
  ContestDefinition empty;
  contest_definition_init_defaults(&empty);
  stats_set_contest_definition(&empty);
  stats_update();
}

static void test_stats_target_contest_multipliers(void) {
  const int saved_qso_count = qso_count;
  const Config saved_config = config;
  QSO saved_qsos[4];
  memcpy(saved_qsos, logbook, sizeof(saved_qsos));
  snprintf(config.station_call, sizeof(config.station_call), "%s", "K1ABC");
  config.station_exchange[0] = 0;

  ContestDefinition def;
  contest_definition_init_defaults(&def);
  def.multiplier_type = CONTEST_MULT_PREFIX;
  stats_set_contest_definition(&def);
  qso_count = 5;
  memset(logbook, 0, sizeof(logbook));
  const char *wpx_calls[] = {"N8BJQ", "PA/N8BJQ", "XEFTJW", "N8BJQ/P",
                             "3D2CR"};
  for (int i = 0; i < qso_count; i++) {
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", wpx_calls[i]);
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", "20M");
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", "Test");
    logbook[i].points = 1;
  }
  stats_update();
  expect_int_eq(stats.contest_mults, 4,
                "WPX prefix multiplier should normalize portable and no-number calls");

  def.multiplier_type = CONTEST_MULT_CQWW;
  stats_set_contest_definition(&def);
  qso_count = 4;
  memset(logbook, 0, sizeof(logbook));
  const char *cqww_calls[] = {"K1ABC", "IG9ABC", "IH9ABC", "K1ABC/MM"};
  const char *cqww_countries[] = {"United States", "Italy", "Italy", "United States"};
  const int cqww_zones[] = {5, 15, 15, 8};
  for (int i = 0; i < qso_count; i++) {
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", cqww_calls[i]);
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", cqww_countries[i]);
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", "20M");
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    logbook[i].cq_zone = cqww_zones[i];
    logbook[i].points = 3;
  }
  stats_update();
  expect_int_eq(stats.contest_mults, 6,
                "CQWW should split IG9/IH9, count zones per band, and exclude /MM country");

  def.multiplier_type = CONTEST_MULT_DXCC_PER_BAND;
  def.multiplier_count = 2;
  def.multipliers[0] = CONTEST_MULT_DXCC_PER_BAND;
  def.multipliers[1] = CONTEST_MULT_ZONE_PER_BAND;
  snprintf(def.multiplier_excluded_suffixes[0],
           sizeof(def.multiplier_excluded_suffixes[0]), "%s", "/MM,/AM");
  snprintf(def.multiplier_special_prefixes[0],
           sizeof(def.multiplier_special_prefixes[0]), "%s", "IG9,IH9");
  stats_set_contest_definition(&def);
  stats_update();
  expect_int_eq(stats.contest_mults, 6,
                "configured DXCC and zone families should retain CQWW exceptions");
  expect_int_eq(stats.contest_score, 108,
                "configured CQWW families should multiply independently");

  def.multiplier_type = CONTEST_MULT_DXCC_PER_BAND;
  def.multiplier_count = 2;
  def.multipliers[0] = CONTEST_MULT_DXCC_PER_BAND;
  def.multipliers[1] = CONTEST_MULT_ZONE_PER_BAND;
  stats_set_contest_definition(&def);
  stats_update();
  expect_int_eq(stats.contest_mults, 6,
                "independent DXCC and zone families should be counted separately");
  expect_int_eq(stats.contest_score, 108,
                "contest score should multiply family counts, not add them");

  def.multiplier_type = CONTEST_MULT_IARU;
  stats_set_contest_definition(&def);
  qso_count = 4;
  memset(logbook, 0, sizeof(logbook));
  const char *iaru_exchanges[] = {"28", "28", "ARRL", "R1"};
  const char *iaru_bands[] = {"20M", "20M", "20M", "40M"};
  for (int i = 0; i < qso_count; i++) {
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", "DL1ABC");
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", "Germany");
    snprintf(logbook[i].exchange_recv, sizeof(logbook[i].exchange_recv), "%s",
             iaru_exchanges[i]);
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", iaru_bands[i]);
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    logbook[i].itu_zone = 28;
    logbook[i].points = 1;
  }
  stats_update();
  expect_int_eq(stats.contest_mults, 3,
                "IARU should count ITU zones and HQ/official exchanges per band");

  def.multiplier_type = CONTEST_MULT_SPDX;
  stats_set_contest_definition(&def);
  qso_count = 3;
  memset(logbook, 0, sizeof(logbook));
  const char *spdx_exchanges[] = {"59 B", "59B", "C"};
  for (int i = 0; i < qso_count; i++) {
    snprintf(logbook[i].call, sizeof(logbook[i].call), "%s", "SP9ABC");
    snprintf(logbook[i].country, sizeof(logbook[i].country), "%s", "Poland");
    snprintf(logbook[i].exchange_recv, sizeof(logbook[i].exchange_recv), "%s",
             spdx_exchanges[i]);
    snprintf(logbook[i].band, sizeof(logbook[i].band), "%s", "20M");
    snprintf(logbook[i].mode, sizeof(logbook[i].mode), "%s", "CW");
    logbook[i].points = 3;
  }
  stats_update();
  expect_int_eq(stats.contest_mults, 2,
                "SP DX should deduplicate province letters per band despite report formatting");

  qso_count = saved_qso_count;
  memcpy(logbook, saved_qsos, sizeof(saved_qsos));
  config = saved_config;
  ContestDefinition empty;
  contest_definition_init_defaults(&empty);
  stats_set_contest_definition(&empty);

  qso_count = 1;
  memset(&logbook[0], 0, sizeof(logbook[0]));
  snprintf(logbook[0].call, sizeof(logbook[0].call), "%s", "SP9ZERO");
  snprintf(logbook[0].country, sizeof(logbook[0].country), "%s", "Poland");
  snprintf(logbook[0].band, sizeof(logbook[0].band), "%s", "20M");
  snprintf(logbook[0].mode, sizeof(logbook[0].mode), "%s", "CW");
  logbook[0].points = 0;
  empty.multiplier_type = CONTEST_MULT_NONE;
  stats_set_contest_definition(&empty);
  stats_update();
  expect_int_eq(stats.contest_qso_points, 0,
                "valid zero-point QSO should not receive fallback points");

  qso_count = saved_qso_count;
  memcpy(logbook, saved_qsos, sizeof(saved_qsos));
  config = saved_config;
  contest_definition_init_defaults(&empty);
  stats_set_contest_definition(&empty);
  stats_update();
}

static time_t wag_test_utc(int year, int month, int day, int hour, int minute,
                           int second) {
  struct tm utc = {0};
  utc.tm_year = year - 1900;
  utc.tm_mon = month - 1;
  utc.tm_mday = day;
  utc.tm_hour = hour;
  utc.tm_min = minute;
  utc.tm_sec = second;
  return timegm(&utc);
}

static void expect_wag_rules(int expected, time_t when, int freq_khz,
                             const char *mode, int power_watts,
                             const char *category, const char *message) {
  (void)when;
  ContestDefinition definition;
  contest_definition_init_defaults(&definition);
  definition.validate_operating_rules = 1;
  snprintf(definition.allowed_modes, sizeof(definition.allowed_modes),
           "%s", "CW,SSB");
  snprintf(definition.allowed_bands, sizeof(definition.allowed_bands),
           "%s", "80M,40M,20M,15M,10M");
  snprintf(definition.category_power, sizeof(definition.category_power),
           "%s", category);
  definition.max_power_watts = 1500;
  const struct {
    const char *mode;
    int low;
    int high;
  } excluded[] = {
      {"CW", 3560, 3800}, {"CW", 7040, 7200}, {"CW", 14060, 14350},
      {"SSB", 3650, 3700}, {"SSB", 7080, 7130}, {"SSB", 14100, 14125},
      {"SSB", 14280, 14350}, {"SSB", 21350, 21450}, {"SSB", 28225, 28400}};
  for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
    ContestExcludedSegment *segment =
        &definition.excluded_segments[definition.excluded_segment_count++];
    snprintf(segment->mode, sizeof(segment->mode), "%s", excluded[i].mode);
    segment->low_khz = excluded[i].low;
    segment->high_khz = excluded[i].high;
  }
  char error[128] = {0};
  const int actual = contest_rules_validate_target_qso(
      &definition, "not-a-contest-name", when, freq_khz, mode, power_watts,
      error, sizeof(error));
  expect_int_eq(actual, expected, message);
}

static void test_wag_operating_rules(void) {
  const time_t start = wag_test_utc(2026, 10, 17, 15, 0, 0);
  expect_wag_rules(1, wag_test_utc(2026, 10, 9, 14, 59, 59), 7020, "CW",
                   150, "HIGH", "WAG accepts QSO before contest weekend");
  expect_wag_rules(1, start, 7020, "CW", 150, "HIGH",
                   "WAG accepts QSO at scheduled start");
  expect_wag_rules(1, wag_test_utc(2026, 10, 18, 14, 59, 59), 7020, "CW",
                   150, "HIGH", "WAG accepts QSO through scheduled end");
  expect_wag_rules(1, wag_test_utc(2026, 10, 24, 16, 0, 0), 7020, "CW",
                   150, "HIGH", "WAG accepts QSO after contest weekend");

  expect_wag_rules(1, start, 3559, "CW", 150, "HIGH",
                   "WAG accepts frequency before 80m excluded segment");
  expect_wag_rules(0, start, 3560, "CW", 150, "HIGH",
                   "WAG rejects 80m CW excluded segment");
  expect_wag_rules(0, start, 3650, "SSB", 150, "HIGH",
                   "WAG rejects 80m SSB excluded segment");
  expect_wag_rules(0, start, 14100, "SSB", 150, "HIGH",
                   "WAG rejects 20m SSB excluded segment");
  expect_wag_rules(0, start, 7040, "CW", 150, "HIGH",
                   "WAG rejects 40m CW excluded segment");
  expect_wag_rules(0, start, 7080, "SSB", 150, "HIGH",
                   "WAG rejects 40m SSB excluded segment");
  expect_wag_rules(0, start, 14280, "SSB", 150, "HIGH",
                   "WAG rejects upper 20m SSB excluded segment");
  expect_wag_rules(0, start, 21350, "SSB", 150, "HIGH",
                   "WAG rejects 15m SSB excluded segment");
  expect_wag_rules(0, start, 28225, "SSB", 150, "HIGH",
                   "WAG rejects 10m SSB excluded segment");
  expect_wag_rules(0, start, 10120, "CW", 150, "HIGH",
                   "WAG rejects a non-contest band");
  expect_wag_rules(0, start, 7020, "RTTY", 150, "HIGH",
                   "WAG rejects a non-contest mode");

  expect_wag_rules(1, start, 7020, "CW", 5, "QRP",
                   "WAG accepts QRP at 5 watts");
  expect_wag_rules(0, start, 7020, "CW", 6, "QRP",
                   "WAG rejects QRP above 5 watts");
  expect_wag_rules(1, start, 7020, "CW", 100, "LOW",
                   "WAG accepts low power at 100 watts");
  expect_wag_rules(0, start, 7020, "CW", 101, "LOW",
                   "WAG rejects low power above 100 watts");
  expect_wag_rules(1, start, 7020, "CW", 101, "HIGH",
                   "WAG accepts high power above 100 watts");
  expect_wag_rules(0, start, 7020, "CW", 100, "HIGH",
                   "WAG rejects high power at 100 watts");
  expect_wag_rules(0, start, 7020, "CW", 0, "HIGH",
                   "WAG requires configured station output power");
}

static void expect_target_contest_rule(int expected, const char *contest_name,
                                       const char *category_mode,
                                       time_t when, int frequency_khz,
                                       const char *mode, int power_watts,
                                       const char *category_power,
                                       const char *message) {
  ContestDefinition definition;
  contest_definition_init_defaults(&definition);
  snprintf(definition.name, sizeof(definition.name), "%s", contest_name);
  snprintf(definition.cabrillo_name, sizeof(definition.cabrillo_name), "%s",
           contest_name);
  snprintf(definition.mode, sizeof(definition.mode), "%s", category_mode);
  snprintf(definition.category_band, sizeof(definition.category_band), "%s",
           "ALL");
  snprintf(definition.category_power, sizeof(definition.category_power), "%s",
           category_power);
  if (strstr(contest_name, "IARU-VHF") == NULL) {
    definition.validate_operating_rules = 1;
    snprintf(definition.allowed_modes, sizeof(definition.allowed_modes),
             "%s", "CW,SSB");
    snprintf(definition.allowed_bands, sizeof(definition.allowed_bands),
             "%s", "160M,80M,40M,20M,15M,10M");
  }
  char error[128] = {0};
  const int actual = contest_rules_validate_target_qso(
      &definition, contest_name, when, frequency_khz, mode, power_watts,
      error, sizeof(error));
  expect_int_eq(actual, expected, message);
}

static void test_target_contest_schedule_band_mode_and_power(void) {
  const time_t spdx_start = wag_test_utc(2026, 4, 4, 15, 0, 0);
  expect_target_contest_rule(1, "SP-DX", "MIXED", spdx_start, 7020, "CW",
                             75, "LOW", "SP DX accepts official start");
  expect_target_contest_rule(1, "SP-DX", "MIXED",
                             wag_test_utc(2026, 4, 4, 14, 59, 59), 7020,
                             "CW", 75, "LOW", "SP DX accepts outside contest period");
  expect_target_contest_rule(0, "SP-DX", "MIXED", spdx_start, 10120, "CW",
                             75, "LOW", "SP DX rejects 30 m");

  expect_target_contest_rule(1, "CQ-WPX-CW", "CW",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 7020, "CW",
                             5, "QRP", "WPX accepts QSO outside contest period");
  expect_target_contest_rule(0, "CQ-WPX-CW", "CW",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 7020, "CW",
                             6, "QRP", "WPX enforces the QRP power limit");
  expect_target_contest_rule(0, "CQ-WPX-CW", "CW",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 7020, "CW",
                             75, "UNKNOWN", "Contest gate rejects unknown power category");
  expect_target_contest_rule(0, "CQ-WPX-CW", "CW",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 10120, "CW",
                             75, "LOW", "WPX rejects 30 m");

  expect_target_contest_rule(1, "CQ-WW-SSB", "SSB",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 14150, "SSB",
                             75, "LOW", "CQ WW accepts QSO outside contest period");
  expect_target_contest_rule(0, "CQ-WW-SSB", "SSB",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 14150, "RTTY",
                             75, "LOW", "CQ WW rejects non-CW/SSB modes");

  expect_target_contest_rule(1, "IARU-HF-CHAMPIONSHIP", "MIXED",
                             wag_test_utc(2026, 10, 9, 0, 0, 0), 14020, "CW",
                             75, "LOW", "IARU accepts QSO outside contest period");
  expect_target_contest_rule(1, "IARU-VHF", "MIXED",
                             wag_test_utc(2026, 7, 11, 12, 0, 0), 144300, "SSB",
                             75, "LOW", "IARU HF gate leaves VHF contests alone");
}

static void test_cw_qtc_expand(void) {
  char out[256];

  cw_qtc_expand("QTC {QTC_NR}/{QTC_COUNT}",
                "SP5XYZ", "W1AW",
                3, 10,
                "", "", "",
                out, sizeof(out));
  expect_str_eq(out, "QTC 3/10", "qtc expand preamble");

  cw_qtc_expand("{QTC_TIME} {QTC_CALL} {QTC_EXCH}",
                "SP5XYZ", "W1AW",
                1, 5,
                "1430", "DK5AI", "42",
                out, sizeof(out));
  expect_str_eq(out, "1430 DK5AI 42", "qtc expand record");

  cw_qtc_expand("{MYCALL} DE {HISCALL}",
                "SP5XYZ", "W1AW",
                1, 1,
                "", "", "",
                out, sizeof(out));
  expect_str_eq(out, "SP5XYZ DE W1AW", "qtc expand mycall/hiscall");
}

static void test_export_cabrillo_qtc_lines(const char *tmp_dir) {
  char conf_path[512];
  char cab_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/wae_export.conf", tmp_dir);
  snprintf(cab_path,  sizeof(cab_path),  "%s/wae_export.cbr",  tmp_dir);

  const char *conf_text =
      "NAME=WAE-EXPORT-TEST\n"
      "CABRILLO_NAME=WAE-DX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write WAE export contest definition");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                "load WAE export contest definition");

  /* Seed one QTC bundle. */
  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 2;
  b.sent         = 1;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI", "42");
  qtc_record_init(&b.records[1], "20241201", "1432", "G3XYZ", "43");
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  expect_int_eq(export_cabrillo_with_qtc(cab_path, &def, "SP5XYZ"), 0,
                "cabrillo with qtc export should succeed");

  char *cab = read_whole_file(cab_path);
  expect_true(cab != NULL, "cabrillo file should be readable");
  if (cab) {
    expect_true(strstr(cab, "QTC:") != NULL,
                "cabrillo file should contain QTC: lines");
    expect_true(strstr(cab, "DK5AI") != NULL,
                "cabrillo QTC line should contain first record call");
    free(cab);
  }

  /* Cleanup. */
  qtc_bundle_count = 0;
}

static void test_export_cabrillo_qtc_lines_without_qtc_definition(
    const char *tmp_dir) {
  char cab_path[512];
  snprintf(cab_path, sizeof(cab_path), "%s/general_with_qtc.cbr", tmp_dir);

  ContestDefinition def;
  contest_definition_init_defaults(&def);
  snprintf(def.name, sizeof(def.name), "%s", "GENERAL");
  snprintf(def.cabrillo_name, sizeof(def.cabrillo_name), "%s", "GENERAL");
  snprintf(def.mode, sizeof(def.mode), "%s", "CW");

  qtc_bundle_count = 0;
  memset(qtc_bundles, 0, sizeof(qtc_bundles));

  const int saved_qso_count = qso_count;
  QSO saved_qso = {0};
  if (saved_qso_count > 0)
    saved_qso = logbook[saved_qso_count - 1];

  qso_count = 1;
  memset(&logbook[0], 0, sizeof(logbook[0]));
  snprintf(logbook[0].contest_id, sizeof(logbook[0].contest_id), "%s",
           "DARC-WAEDC-CW");

  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 3;
  b.record_count = 1;
  b.sent         = 1;
  qtc_record_init(&b.records[0], "20241201", "1500", "DL1ABC", "77");
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  expect_int_eq(export_cabrillo_with_qtc(cab_path, &def, "SP5XYZ"), 0,
                "cabrillo export should succeed without explicit QTC contest config");

  char *cab = read_whole_file(cab_path);
  expect_true(cab != NULL,
              "cabrillo without qtc definition should be readable");
  if (cab) {
    expect_true(strstr(cab, "QTC:") != NULL,
                "cabrillo should still contain QTC lines when bundles exist");
    expect_true(strstr(cab, "DL1ABC") != NULL,
                "cabrillo should contain bundled QTC callsign");
    free(cab);
  }

  qtc_bundle_count = 0;
  if (saved_qso_count > 0)
    logbook[saved_qso_count - 1] = saved_qso;
  qso_count = saved_qso_count;
}

int main(void) {
  char tmp_dir[256];
  if (make_temp_dir(tmp_dir, sizeof(tmp_dir)) != 0) {
    fprintf(stderr, "Cannot create temp dir: %s\n", strerror(errno));
    return 2;
  }

  if (setenv("HOME", tmp_dir, 1) != 0) {
    fprintf(stderr, "Cannot isolate test HOME: %s\n", strerror(errno));
    return 2;
  }

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/unit.sqlite3", tmp_dir);
  setenv("LOGGER_DB_PATH", db_path, 1);

  test_config_load(tmp_dir);
  test_config_save_roundtrip(tmp_dir);
  test_live_upload_publish();
#ifdef LOGGER_NETWORK_TESTS_ONLY
  test_db_sync_identity_and_sequence(tmp_dir);
  test_db_sync_outbox_lifecycle(tmp_dir);
  test_db_sync_outbox_retry_limit_marks_failed(tmp_dir);
  test_qso_sync_metadata_roundtrip(tmp_dir);
  test_db_sync_atomic_writes_and_legacy_migration(tmp_dir);
  test_net_sync_config_validation();
  test_net_protocol_frames();
  test_net_sync_mock_server_roundtrip(tmp_dir);
  test_net_worker_prefetches_serial_reservations(tmp_dir);
  test_net_sync_paged_catchup_replay_and_cursor(tmp_dir);
  test_net_sync_partial_ack_keeps_unacked_pending(tmp_dir);
  test_net_sync_connect_backoff(tmp_dir);
#ifdef HAVE_OPENSSL
  test_tls_transport_fingerprint_pinning(tmp_dir);
#endif
  test_net_server_rate_limit(tmp_dir);
  test_net_server_pages_and_station_sequence_gaps(tmp_dir);
  test_net_sync_fault_drop_append_ack_retries(tmp_dir);
  test_net_sync_fault_delayed_pull_response(tmp_dir);
  test_net_sync_fault_truncated_pull_response(tmp_dir);
  test_protocol_append_and_pull_parsing();
  test_db_sync_apply_remote_op_and_pull(tmp_dir);
  test_db_sync_publish_local_logbook_ops(tmp_dir);
  test_net_server_client_roundtrip_apply_pull(tmp_dir);
#ifdef HAVE_OPENSSL
  test_net_server_client_roundtrip_apply_pull_tls(tmp_dir);
#endif
  test_net_server_duplicate_append_is_idempotent(tmp_dir);
  test_db_sync_qso_uid_conflict_is_rejected(tmp_dir);
  test_db_sync_serial_reservation_and_commit(tmp_dir);
#else
  test_cty_load_and_lookup(tmp_dir);
  test_cty_download_latest_failure_path(tmp_dir);
  test_qso_helpers();
  test_cw_esm_enter_planner();
  test_cat_cw_busy_state_disconnected();
  test_db_sync_identity_and_sequence(tmp_dir);
  test_db_sync_outbox_lifecycle(tmp_dir);
  test_db_sync_outbox_retry_limit_marks_failed(tmp_dir);
  test_qso_sync_metadata_roundtrip(tmp_dir);
  test_db_sync_atomic_writes_and_legacy_migration(tmp_dir);
  test_net_sync_config_validation();
  test_net_protocol_frames();
  test_net_sync_mock_server_roundtrip(tmp_dir);
  test_net_worker_prefetches_serial_reservations(tmp_dir);
  test_net_sync_paged_catchup_replay_and_cursor(tmp_dir);
  test_net_sync_partial_ack_keeps_unacked_pending(tmp_dir);
  test_net_sync_connect_backoff(tmp_dir);
  test_tls_transport_fingerprint_pinning(tmp_dir);
  test_net_server_rate_limit(tmp_dir);
  test_net_server_pages_and_station_sequence_gaps(tmp_dir);
  test_net_sync_fault_drop_append_ack_retries(tmp_dir);
  test_net_sync_fault_delayed_pull_response(tmp_dir);
  test_net_sync_fault_truncated_pull_response(tmp_dir);
  test_protocol_append_and_pull_parsing();
  test_db_sync_apply_remote_op_and_pull(tmp_dir);
  test_db_sync_publish_local_logbook_ops(tmp_dir);
  test_net_server_client_roundtrip_apply_pull(tmp_dir);
  test_net_server_client_roundtrip_apply_pull_tls(tmp_dir);
  test_net_server_duplicate_append_is_idempotent(tmp_dir);
  test_db_sync_qso_uid_conflict_is_rejected(tmp_dir);
  test_db_sync_serial_reservation_and_commit(tmp_dir);
  test_qso_add_mark_and_stats();
  test_stats_wag_multipliers();
  test_stats_target_contest_multipliers();
  test_wag_operating_rules();
  test_target_contest_schedule_band_mode_and_power();
  test_export_csv_adif(tmp_dir);
  test_export_command_exports_cabrillo_too(tmp_dir);
  test_contest_definition_and_cabrillo(tmp_dir);
  test_dxlog_definition_compatibility(tmp_dir);
  test_dxlog_custom_multiplier_metadata_and_section_area_parsing(tmp_dir);
  test_dxlog_importer_generates_local_conf(tmp_dir);
  test_maidenhead();
  test_controller_vhf_locator_exchange_and_distance_points(tmp_dir);
  test_controller_vhf_serial_locator_exchange_and_distance_points(tmp_dir);
  test_controller_vhf_spaced_serial_locator_exchange(tmp_dir);
  test_controller_spaced_serial_locator_exchange_and_suggestions(tmp_dir);
  test_controller_fixed_locator_serial_suggestion(tmp_dir);
  test_dxcluster_set_status();
  test_dxcluster_start_stop();
  test_dxcluster_send_spot_requires_connection();
  test_dxcluster_connect_disconnect_and_send_spot();
  test_app_controller_shutdown_stops_cluster(tmp_dir);
  test_call_suggestions();
  test_app_controller_key_flow(tmp_dir);
  test_controller_contest_mode_points(tmp_dir);
  test_controller_static_tx_exchange_override(tmp_dir);
  test_controller_numeric_static_exchange_template(tmp_dir);
  test_controller_incremental_exchange_generation(tmp_dir);
  test_controller_wag_station_exchange(tmp_dir);
  test_target_contest_exchange_and_points(tmp_dir);
  test_target_contest_presets(tmp_dir);
  test_controller_reopen_resume_from_last_sent_serial(tmp_dir);
  test_controller_received_exchange_persists_after_reopen(tmp_dir);
  test_controller_contest_mode_overrides_detected_mode(tmp_dir);
  test_manual_frequency_entry_from_call_field();
  test_named_log_commands(tmp_dir);
  test_newlog_creates_database_file(tmp_dir);
  test_syncstatus_command_reports_failed_queue(tmp_dir);
  test_net_command_on_off_role_status(tmp_dir);
  test_netsync_offline_queue_status(tmp_dir);
  test_qso_status_includes_sync_pending_when_net_enabled();
  test_contest_preset_from_build_dir_uses_defined_settings();
  test_missing_default_contest_file_is_nonfatal();
  test_current_directory_config_has_priority_over_runtime_copy();
  test_openlog_restores_saved_contest_definition(tmp_dir);
  test_contest_import_only_does_not_autoload_or_set_active_path(tmp_dir);

  /* QTC tests */
  test_wae_qso_scoring(tmp_dir);
  test_qtc_enabled_for_opened_wae_log_without_loaded_definition(tmp_dir);
  test_qtc_sendable_prefill_uses_call_and_received_exchange(tmp_dir);
  test_qtc_enabled_for_opened_log_with_qtc_bundles_only(tmp_dir);
  test_qtc_record_init();
  test_qtc_bundle_validate_valid();
  test_qtc_bundle_validate_empty_sender();
  test_qtc_bundle_validate_record_count_zero();
  test_qtc_bundle_validate_too_many_records();
  test_qtc_qso_already_sent();
  test_qtc_next_bundle_nr();
  test_qtc_total_records();
  test_contest_definition_qtc_fields(tmp_dir);
  test_wae_contest_definition_files(tmp_dir);
  test_stats_qtc_scoring();
  test_cw_qtc_expand();
  test_export_cabrillo_qtc_lines(tmp_dir);
  test_export_cabrillo_qtc_lines_without_qtc_definition(tmp_dir);

#endif
  if (g_failures == 0) {
    printf("All unit tests passed.\n");
    return 0;
  }

  printf("Unit tests failed: %d\n", g_failures);
  return 1;
}
