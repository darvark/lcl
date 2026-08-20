#include "app_controller.h"
#include "config.h"
#include "contest.h"
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
#include "qso.h"
#include "qtc.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <math.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static void set_test_db_path(const char *dir_path) {
  char db_path[512];
  join_path(db_path, sizeof(db_path), dir_path, "unit.sqlite3");
  db_shutdown();
  setenv("LOGGER_DB_PATH", db_path, 1);
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
      "CAT_MODE_FROM_RIG = 1\n"
      "CONTEST_TX_EXCHANGE = 28\n"
      "CONTEST_TECHNIQUE = SO2R\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write unit logger.conf");

  expect_int_eq(config_load(conf_path), 0, "config_load should succeed");
  expect_double_close(config.lat, 52.2297, 0.0001, "config LAT parsed");
  expect_double_close(config.lon, 21.0122, 0.0001, "config LON parsed");
  expect_str_eq(config.locator, "JO92AA", "config locator parsed");
  expect_str_eq(config.dxc_host, "dx.example.net", "config host parsed");
  expect_int_eq(config.dxc_port, 9000, "config port parsed");
  expect_str_eq(config.dxc_call, "SP9XYZ", "config call parsed");
  expect_int_eq(config.cat_mode_from_rig, 1,
                "config CAT mode-from-rig parsed");
  expect_int_eq((int)config.contest_technique, (int)CONTEST_TECH_SO2R,
                "contest technique parsed");
  expect_str_eq(config.contest_tx_exchange, "28",
                "contest tx exchange override parsed");

  expect_int_eq(config_load("/definitely/missing/logger.conf"), -1,
                "missing config should return -1");
  expect_str_eq(config.dxc_host, "telnet.reversebeacon.net",
                "default host restored on missing config");
  expect_int_eq(config.dxc_port, 7000,
                "default port restored on missing config");
  expect_str_eq(config.dxc_call, "N0CALL",
                "default call restored on missing config");
  expect_int_eq(config.cat_mode_from_rig, 0,
                "default CAT mode-from-rig restored on missing config");
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
  config.cat_model = 1234;
  snprintf(config.cat_device, sizeof(config.cat_device), "%s", "/dev/ttyS9");
  config.cat_baud = 38400;
  config.cat_data_bits = 7;
  config.cat_stop_bits = 2;
  snprintf(config.cat_parity, sizeof(config.cat_parity), "%s", "Even");
  snprintf(config.cat_handshake, sizeof(config.cat_handshake), "%s", "RTSCTS");
  config.cat_mode_from_rig = 1;
  snprintf(config.contest_tx_exchange, sizeof(config.contest_tx_exchange),
           "%s", "28");

  expect_int_eq(config_save(conf_path), 0, "config_save should succeed");

  config.cat_model = 0;
  config.cat_device[0] = 0;
  config.cat_baud = 0;
  config.cat_data_bits = 0;
  config.cat_stop_bits = 0;
  config.cat_parity[0] = 0;
  config.cat_handshake[0] = 0;
  config.cat_mode_from_rig = 0;
  config.contest_tx_exchange[0] = 0;

  expect_int_eq(config_load(conf_path), 0,
                "config_load should read saved config");
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
  expect_str_eq(config.contest_tx_exchange, "28",
                "saved contest tx exchange restored");
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
      "CONTEST_DEF_FILE=contest.conf\n"
      "CONTEST_TX_EXCHANGE=28\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf with static tx exchange override");

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
    expect_str_eq(state.contest_exchange_sent, "28",
                  "static tx exchange should use config override");

  send_controller_chars("SP9AAA");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("28");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "one QSO should be saved in static tx exchange test");
  expect_str_eq(logbook[base_qso_count].exchange_sent, "28",
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
      "NAME=SERIAL-TEST\n"
      "CABRILLO_NAME=SERIAL-TEST\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";
  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write incremental exchange contest definition");

  char conf_path[512];
  join_path(conf_path, sizeof(conf_path), case_dir, "logger.conf");
  const char *conf_text =
      "CONTEST_DEF_FILE=contest.conf\n"
      "CONTEST_TX_EXCHANGE=28\n";
  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write logger.conf for incremental exchange test");

  char old_cwd[512];
  expect_true(getcwd(old_cwd, sizeof(old_cwd)) != NULL,
              "getcwd before incremental exchange test");
  expect_int_eq(chdir(case_dir), 0,
                "chdir to incremental exchange test directory");

  app_controller_init();
  app_controller_handle_key(APP_KEY_F2);
  const int base_qso_count = qso_count;
  AppRenderState state;
  char expected_sent[16];
  char expected_next_sent[16];
  char expected_third_sent[16];
  app_controller_get_render_state(&state);

  expect_true(state.contest_entry_mode,
              "contest mode should be active in incremental exchange test");
  expect_true(state.contest_exchange_label != NULL,
              "contest exchange label should be present");
  if (state.contest_exchange_label)
    expect_str_eq(state.contest_exchange_label, "EXCH",
                  "contest readback field should be labelled EXCH");
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

  send_controller_chars("SP9SER");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("101");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 1,
                "first QSO should be saved in incremental exchange test");
  expect_str_eq(logbook[base_qso_count].exchange_sent, expected_sent,
                "first incremental TX exchange should ignore static override");
  expect_str_eq(logbook[base_qso_count].exchange_recv, "101",
                "first incremental RX exchange should be saved");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should remain present after first incremental QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_next_sent,
                  "second incremental TX exchange should advance to serial 2");

  send_controller_chars("SP9SEQ");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("102");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, base_qso_count + 2,
                "second QSO should be saved in incremental exchange test");
  expect_str_eq(logbook[base_qso_count + 1].exchange_sent, expected_next_sent,
                "second incremental TX exchange should be serial 2");
  expect_str_eq(logbook[base_qso_count + 1].exchange_recv, "102",
                "second incremental RX exchange should be saved");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "contest tx exchange should remain present after second incremental QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, expected_third_sent,
                  "next incremental TX exchange should advance to serial 3");

  app_controller_shutdown();
  expect_int_eq(chdir(old_cwd), 0,
                "restore cwd after incremental exchange test");
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
  expect_str_eq(status, "QSO OK", "sync test QSO should report success");
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

static void test_net_protocol_frames(void) {
  char frame[2048] = {0};
  NetMessageType type = NET_MSG_UNKNOWN;

  expect_int_eq(net_protocol_encode_hello("st-abc123", "logger", "token-1",
                                          frame, sizeof(frame)),
                0, "HELLO frame should encode");
  expect_true(strstr(frame, "\"type\":\"HELLO\"") != NULL,
              "HELLO frame should contain message type");
  expect_true(strstr(frame, "\"auth_token\":\"token-1\"") != NULL,
              "HELLO frame should contain auth token");
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
}

typedef struct {
  int port;
  long long ack_last_global_seq;
  char acked_json[256];
  char received[8192];
  int mode;
  int delay_sec;
  int ok;
} MockSyncServerArgs;

enum {
  MOCK_SYNC_MODE_NORMAL = 0,
  MOCK_SYNC_MODE_DROP_APPEND_ACK = 1,
  MOCK_SYNC_MODE_DELAY_PULL_RESP = 2
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
  if (net_protocol_encode_hello_ack(1, 1, 0, response, sizeof(response)) != 0) {
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
    snprintf(response, sizeof(response),
             "{\"type\":\"APPEND_ACK\",\"accepted_ops\":[%s],\"rejected_ops\":[],\"last_acked_station_seq\":1,\"server_global_seq\":%lld}",
             ctx->acked_json[0] ? ctx->acked_json : "", ctx->ack_last_global_seq);
  } else {
    snprintf(response, sizeof(response),
             "{\"type\":\"ACK\",\"acked\":[],\"last_global_seq\":%lld}",
             ctx->ack_last_global_seq);
  }
  (void)net_protocol_send_framed(cli, response);

  ctx->ok = 1;
  close(cli);
  close(srv);
  return NULL;
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
  char saved_role[16];
  char saved_host[128];
  int saved_port = config.net_server_port;

  snprintf(saved_role, sizeof(saved_role), "%s", config.net_role);
  snprintf(saved_host, sizeof(saved_host), "%s", config.net_server_host);

  config.net_enabled = 1;
  snprintf(config.net_role, sizeof(config.net_role), "%s", "client");
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           "127.0.0.1");
  config.net_server_port = 19321;

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
  expect_int_eq(net_sync_poll_once(), 0, "net sync poll should succeed");
  net_sync_stop();

  pthread_join(tid, NULL);

  expect_true(server.ok == 1, "mock server should accept one client session");
  expect_true(strstr(server.received, "\"type\":\"HELLO\"") != NULL,
              "client should send HELLO frame");
  expect_true(strstr(server.received, "\"type\":\"PULL_OPS\"") != NULL,
              "client should send PULL_OPS frame");
  expect_true(strstr(server.received, "\"type\":\"APPEND_OPS\"") != NULL,
              "client should send APPEND_OPS frame");
  expect_true(strstr(server.received, ops[0].op_id) != NULL,
              "APPEND_OPS should contain pending op id");

  long long last_seq = 0;
  expect_int_eq(db_sync_get_last_global_seq(&last_seq), 0,
                "global seq should be readable after sync");
  expect_true(last_seq == server.ack_last_global_seq,
              "ACK should update last_global_seq cursor");

  int pending = -1;
  expect_int_eq(db_sync_get_pending_outbox_count(&pending), 0,
                "pending count should be readable after ACK");
  expect_int_eq(pending, 0, "acked outbox item should no longer be pending");

  NetSyncStatus st;
  memset(&st, 0, sizeof(st));
  net_sync_get_status(&st);
  expect_true(st.last_pulled_global_seq == server.ack_last_global_seq,
              "net sync status should expose updated cursor");
  expect_true(st.pending_outbox == 0,
              "net sync status should expose empty pending outbox");

  config.net_enabled = saved_net_enabled;
  snprintf(config.net_role, sizeof(config.net_role), "%s", saved_role);
  snprintf(config.net_server_host, sizeof(config.net_server_host), "%s",
           saved_host);
  config.net_server_port = saved_port;

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

  pthread_t tid;
  expect_int_eq(pthread_create(&tid, NULL, mock_sync_server_thread, &server), 0,
                "mock server for partial ACK should start");
  usleep(120000);

  expect_int_eq(net_sync_start(), 0, "net sync should start for partial ACK");
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync poll should succeed for partial ACK");
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
  memset(parsed_pull, 0, sizeof(parsed_pull));
  expect_int_eq(net_protocol_parse_pull_ops_resp(pull_frame, parsed_pull, 2,
                                                 &pull_count, &last_seq),
                0, "parse PULL_OPS_RESP should succeed");
  expect_int_eq(pull_count, 1, "parsed pull ops count");
  expect_int_eq((int)last_seq, 123, "parsed last_global_seq");
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

  long long gseq_dup = 0;
  expect_int_eq(db_sync_apply_remote_op("op-remote-1", "st-remote", 7, 1,
                                        "QSO_INSERT", "q-remote-1", payload,
                                        "2026-01-01T01:00:00Z", &gseq_dup),
                0, "duplicate remote op should be idempotent");
  expect_true(gseq_dup == gseq1,
              "duplicate apply should return same global_seq");

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
  expect_int_eq(net_protocol_encode_hello("st-client", "logger", "",
                                          hello_frame, sizeof(hello_frame)),
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

  close(cli);

  qso_init();
  expect_int_eq(qso_count, 1,
                "server DB should contain one synced QSO from client");

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
  expect_int_eq(net_protocol_encode_hello("st-client-tls", "logger", "",
                                          hello_frame, sizeof(hello_frame)),
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
  const char *fp = net_transport_peer_fingerprint(&transport1);
  expect_true(fp != NULL && fp[0] != 0,
              "first TLS handshake should expose peer fingerprint");
  net_transport_close(&transport1);

  int cli2 = socket(AF_INET, SOCK_STREAM, 0);
  expect_true(cli2 >= 0, "second TLS fingerprint client socket created");
  expect_int_eq(connect(cli2, (struct sockaddr *)&addr, sizeof(addr)), 0,
                "second TLS fingerprint client connected");

  NetTransport transport2;
  memset(&transport2, 0, sizeof(transport2));
  expect_int_eq(net_transport_init_client(&transport2, cli2, "127.0.0.1", 1,
                                          "00:11:22", error_text,
                                          sizeof(error_text)),
                -1, "TLS handshake should fail on fingerprint mismatch");
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
  expect_int_eq(net_protocol_encode_hello("st-rate", "logger", "",
                                          hello_frame, sizeof(hello_frame)),
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
  expect_int_eq(net_sync_poll_once(), 0,
                "net sync poll should tolerate delayed pull response");
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
  expect_int_eq(net_protocol_encode_hello("st-dup", "logger", "",
                                          hello_frame, sizeof(hello_frame)),
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

static void test_db_sync_serial_reservation_and_commit(const char *tmp_dir) {
  char case_dir[512];
  snprintf(case_dir, sizeof(case_dir), "%s/serial_reservation_case", tmp_dir);
  expect_int_eq(mkdir(case_dir, 0777), 0,
                "create serial reservation test directory");

  set_test_db_path(case_dir);
  qso_init();

  char reservation_id[64] = {0};
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

  expect_int_eq(db_sync_commit_serial(reservation_id, "q-serial-1"), 0,
                "serial commit should succeed");

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
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write empty logger.conf for net command test");

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

  send_controller_text("net status");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "SYNC") != NULL,
                "net status should render sync summary");

  send_controller_text("net off");
  app_controller_get_render_state(&state);
  if (state.status)
    expect_true(strstr(state.status, "NET disabled") != NULL,
                "net off command should disable network");

  app_controller_shutdown();
  chdir("..");
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
    expect_true(strstr(csv, "K1ABC") == NULL,
                "CSV excludes invalid entries");
  }

  if (adi) {
    expect_true(strstr(adi, "<EOH>") != NULL, "ADIF header exists");
    expect_true(strstr(adi, "<CALL:6>SP9ABC") != NULL,
                "ADIF contains SP9ABC");
    expect_true(strstr(adi, "<CALL:5>K1ABC") == NULL,
                "ADIF excludes invalid entries");
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

  const int base_qso_count = qso_count;
  snprintf(expected_serial_1, sizeof(expected_serial_1), "%d",
           base_qso_count + 1);
  snprintf(expected_serial_2, sizeof(expected_serial_2), "%d",
           base_qso_count + 2);
  snprintf(expected_fragment_1, sizeof(expected_fragment_1), "599 %-6s SP9SER",
           expected_serial_1);
  snprintf(expected_fragment_2, sizeof(expected_fragment_2), "599 %-6s SP9SEQ",
           expected_serial_2);

  expect_int_eq(qso_add_contest_fields("SP9SER", 7020, "599", "CW", "", "",
                                       "101", "RUN", def.cabrillo_name, 1, 1,
                                       status, sizeof(status)),
                base_qso_count,
                "first contest QSO for Cabrillo serial fallback should save");
  expect_int_eq(qso_add_contest_fields("SP9SEQ", 7020, "599", "CW", "", "",
                                       "102", "RUN", def.cabrillo_name, 1, 1,
                                       status, sizeof(status)),
                base_qso_count + 1,
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
  char err[128] = {0};
  ContestDefinition def;

  snprintf(path_wpx, sizeof(path_wpx), "%s/dxlog_cqwpx.txt", tmp_dir);
  snprintf(path_ww, sizeof(path_ww), "%s/dxlog_cqww.txt", tmp_dir);
  snprintf(path_spdx, sizeof(path_spdx), "%s/dxlog_spdx.txt", tmp_dir);

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
                (int)CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND,
                "DXLog CQWW should map to combined DXCC+ZONE per band multiplier");

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
    expect_true(strstr(imported, "MULTIPLIER=DXCC_PLUS_ZONE_PER_BAND") != NULL,
                "imported config should normalize combined CQWW multipliers");
    expect_true(strstr(imported, "FIELD=CQZONE,CQ Zone,required") != NULL,
                "imported config should include normalized exchange field");
  }
  free(imported);
}

static void test_maidenhead(void) {
  double lat = 0.0;
  double lon = 0.0;

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
  expect_true(list.count >= 4, "SP prefix should return multiple suggestions");
  expect_str_eq(list.matches[0], "SP8QWE", "newest matching call appears first");
  expect_str_eq(list.matches[1], "SP9AAA", "second suggestion respects recency");
  expect_str_eq(list.matches[2], "SP9XYZ", "third suggestion respects recency");
  expect_str_eq(list.matches[3], "SP3ABC", "older suggestion still listed");

  call_suggestion_select_next(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP9AAA",
                "down arrow selection should move to next match");

  call_suggestion_select_prev(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP8QWE",
                "up arrow selection should move to previous match");

  call_suggestion_select_prev(&list);
  expect_str_eq(call_suggestion_selected(&list), "SP3ABC",
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
  expect_int_eq(chdir(old_cwd), 0, "restore cwd after controller points test");
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

  send_controller_text("newlog Summer Contest");
  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, 0, "newlog <name> clears active logbook");
  expect_true(state.status != NULL, "newlog status exists");
  if (state.status)
    expect_true(strstr(state.status, "New log created: Summer Contest") != NULL,
                "newlog should confirm selected name");

  send_controller_text("logs");
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "logs status exists");
  expect_true(state.info != NULL, "logs info exists");
  if (state.status)
    expect_true(strstr(state.status, "Named logs:") != NULL,
                "logs should report available named archives");
  if (state.info)
    expect_true(strstr(state.info, "Summer Contest") != NULL,
                "logs output should include archived log name");

  app_controller_handle_key(APP_KEY_F3);
  expect_int_eq(qso_count, 1,
                "previous log should restore original QSO set");

  send_controller_text("openlog Summer Contest");
  app_controller_get_render_state(&state);
  expect_int_eq(qso_count, 0,
                "openlog <name> opens independent empty logbook");
  expect_true(state.status != NULL, "openlog status exists");
  if (state.status)
    expect_true(strstr(state.status, "Log opened: Summer Contest") != NULL,
                "openlog should confirm selected log name");

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
  expect_int_eq(write_text_file("logger.conf", "CONTEST_DEF_FILE=\n"), 0,
                "write isolated logger.conf for contest preset test");

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
    expect_str_eq(state.contest_exchange_sent, "1",
                  "EXCHANGE_SENT=# should start incremental exchange from 1");

  send_controller_text("7020");
  send_controller_chars("SP9WPX");
  app_controller_handle_key(APP_KEY_SPACE);
  send_controller_chars("100");
  app_controller_handle_key(APP_KEY_ENTER);

  expect_int_eq(qso_count, 1,
                "contest preset test should save one QSO");
  expect_str_eq(logbook[0].exchange_sent, "1",
                "contest preset should save incremented TX exchange from preset");
  expect_str_eq(logbook[0].exchange_recv, "100",
                "contest preset should save entered RX exchange");

  app_controller_get_render_state(&state);
  expect_true(state.contest_exchange_sent != NULL,
              "next TX exchange should remain visible after first saved QSO");
  if (state.contest_exchange_sent)
    expect_str_eq(state.contest_exchange_sent, "2",
                  "EXCHANGE_SENT=# should always increment upward after save");

  app_controller_shutdown();
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
      "DARC-WAEDC-CW", 1, 1, status, sizeof(status));
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
      "DARC-WAEDC-CW", 1, 1, status, sizeof(status));
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

  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/unit.sqlite3", tmp_dir);
  setenv("LOGGER_DB_PATH", db_path, 1);

  test_config_load(tmp_dir);
  test_config_save_roundtrip(tmp_dir);
  test_cty_load_and_lookup(tmp_dir);
  test_cty_download_latest_failure_path(tmp_dir);
  test_qso_helpers();
  test_db_sync_identity_and_sequence(tmp_dir);
  test_db_sync_outbox_lifecycle(tmp_dir);
  test_db_sync_outbox_retry_limit_marks_failed(tmp_dir);
  test_qso_sync_metadata_roundtrip(tmp_dir);
  test_net_protocol_frames();
  test_net_sync_mock_server_roundtrip(tmp_dir);
  test_net_sync_partial_ack_keeps_unacked_pending(tmp_dir);
  test_net_sync_connect_backoff(tmp_dir);
  test_tls_transport_fingerprint_pinning(tmp_dir);
  test_net_server_rate_limit(tmp_dir);
  test_net_sync_fault_drop_append_ack_retries(tmp_dir);
  test_net_sync_fault_delayed_pull_response(tmp_dir);
  test_protocol_append_and_pull_parsing();
  test_db_sync_apply_remote_op_and_pull(tmp_dir);
  test_net_server_client_roundtrip_apply_pull(tmp_dir);
  test_net_server_client_roundtrip_apply_pull_tls(tmp_dir);
  test_net_server_duplicate_append_is_idempotent(tmp_dir);
  test_db_sync_serial_reservation_and_commit(tmp_dir);
  test_qso_add_mark_and_stats();
  test_export_csv_adif(tmp_dir);
  test_export_command_exports_cabrillo_too(tmp_dir);
  test_contest_definition_and_cabrillo(tmp_dir);
  test_dxlog_definition_compatibility(tmp_dir);
  test_dxlog_importer_generates_local_conf(tmp_dir);
  test_maidenhead();
  test_dxcluster_set_status();
  test_dxcluster_start_stop();
  test_app_controller_shutdown_stops_cluster(tmp_dir);
  test_call_suggestions();
  test_app_controller_key_flow(tmp_dir);
  test_controller_contest_mode_points(tmp_dir);
  test_controller_static_tx_exchange_override(tmp_dir);
  test_controller_numeric_static_exchange_template(tmp_dir);
  test_controller_incremental_exchange_generation(tmp_dir);
  test_controller_reopen_resume_from_last_sent_serial(tmp_dir);
  test_controller_received_exchange_persists_after_reopen(tmp_dir);
  test_controller_contest_mode_overrides_detected_mode(tmp_dir);
  test_manual_frequency_entry_from_call_field();
  test_named_log_commands(tmp_dir);
  test_syncstatus_command_reports_failed_queue(tmp_dir);
  test_net_command_on_off_role_status(tmp_dir);
  test_contest_preset_from_build_dir_uses_defined_settings();
  test_missing_default_contest_file_is_nonfatal();
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

  if (g_failures == 0) {
    printf("All unit tests passed.\n");
    return 0;
  }

  printf("Unit tests failed: %d\n", g_failures);
  return 1;
}
