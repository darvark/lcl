#include "app_controller.h"
#include "config.h"
#include "contest.h"
#include "cty.h"
#include "dxcluster.h"
#include "export.h"
#include "maidenhead.h"
#include "qso.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <math.h>
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
  const int base_qso_count = qso_count;
  AppRenderState state;
  char expected_sent[16];
  char expected_next_sent[16];
  char expected_third_sent[16];
  app_controller_get_render_state(&state);

  expect_true(state.contest_entry_mode,
              "contest mode should be active in incremental exchange test");
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

static void test_app_controller_shutdown_stops_cluster(void) {
  AppRenderState state;

  app_controller_init();
  app_controller_get_render_state(&state);
  expect_true(state.status != NULL, "shutdown test status is present");

  app_controller_shutdown();

  expect_true(strstr(dxcluster_status, "Disconnected") != NULL ||
                  strstr(dxcluster_status, "failed") != NULL ||
                  strstr(dxcluster_status, "timeout") != NULL ||
                  strstr(dxcluster_status, "Connecting") != NULL,
              "app_controller_shutdown should stop DXCluster worker");
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

static void test_app_controller_key_flow(void) {
  AppRenderState state;

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

  ev = app_controller_handle_key(APP_KEY_ESC);
  expect_int_eq((int)ev, (int)APP_CTRL_EVENT_NONE,
                "ESC should cancel export prompt");

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

static void test_named_log_commands(void) {
  AppRenderState state;

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
  test_qso_add_mark_and_stats();
  test_export_csv_adif(tmp_dir);
  test_contest_definition_and_cabrillo(tmp_dir);
  test_dxlog_definition_compatibility(tmp_dir);
  test_dxlog_importer_generates_local_conf(tmp_dir);
  test_maidenhead();
  test_dxcluster_set_status();
  test_dxcluster_start_stop();
  test_app_controller_shutdown_stops_cluster();
  test_call_suggestions();
  test_app_controller_key_flow();
  test_controller_contest_mode_points(tmp_dir);
  test_controller_static_tx_exchange_override(tmp_dir);
  test_controller_numeric_static_exchange_template(tmp_dir);
  test_controller_incremental_exchange_generation(tmp_dir);
  test_controller_contest_mode_overrides_detected_mode(tmp_dir);
  test_manual_frequency_entry_from_call_field();
  test_named_log_commands();
  test_contest_preset_from_build_dir_uses_defined_settings();
  test_contest_import_only_does_not_autoload_or_set_active_path(tmp_dir);

  if (g_failures == 0) {
    printf("All unit tests passed.\n");
    return 0;
  }

  printf("Unit tests failed: %d\n", g_failures);
  return 1;
}
