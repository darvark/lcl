#include "config.h"
#include "contest.h"
#include "cty.h"
#include "export.h"
#include "maidenhead.h"
#include "qso.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
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

static int make_temp_dir(char *out, size_t out_size) {
  if (!out || out_size < 16)
    return -1;

  snprintf(out, out_size, "/tmp/lnx_logger_reg_XXXXXX");
  if (!mkdtemp(out))
    return -1;

  return 0;
}

static void test_config_loading(const char *tmp_dir) {
  char conf_path[512];

  snprintf(conf_path, sizeof(conf_path), "%s/logger.conf", tmp_dir);

  const char *conf_text =
      "# Regression config\n"
      "LAT = 52.2297\n"
      "LON=21.0122\n"
      "LOCATOR = JO92AA\n"
      "DXC_HOST = dx.example.net\n"
      "DXC_PORT = 9000\n"
      "DXC_CALL = SP9XYZ\n"
      "CAT_MODE_FROM_RIG = 1\n"
      "CONTEST_TX_EXCHANGE = 28\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write test logger.conf");

  expect_int_eq(config_load(conf_path), 0, "config_load should succeed");
  expect_double_close(config.lat, 52.2297, 0.0001, "config LAT parsed");
  expect_double_close(config.lon, 21.0122, 0.0001, "config LON parsed");
  expect_str_eq(config.locator, "JO92AA", "config locator parsed");
  expect_str_eq(config.dxc_host, "dx.example.net", "config host parsed");
  expect_int_eq(config.dxc_port, 9000, "config port parsed");
  expect_str_eq(config.dxc_call, "SP9XYZ", "config call parsed");
  expect_int_eq(config.cat_mode_from_rig, 1,
                "config CAT mode-from-rig parsed");
  expect_str_eq(config.contest_tx_exchange, "28",
                "config contest tx exchange parsed");

  expect_int_eq(config_load("/definitely/missing/logger.conf"), -1,
                "missing config should return -1");
  expect_str_eq(config.dxc_host, "telnet.reversebeacon.net",
                "defaults restored when config file is missing");
  expect_int_eq(config.dxc_port, 7000,
                "default port restored when config file is missing");
  expect_int_eq(config.cat_mode_from_rig, 0,
                "default CAT mode-from-rig restored when config file is missing");
}

static void test_config_save_roundtrip(const char *tmp_dir) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/logger_saved.conf", tmp_dir);

  config.cat_model = 2345;
  snprintf(config.cat_device, sizeof(config.cat_device), "%s", "/dev/ttyS8");
  config.cat_baud = 19200;
  config.cat_data_bits = 8;
  config.cat_stop_bits = 1;
  snprintf(config.cat_parity, sizeof(config.cat_parity), "%s", "Odd");
  snprintf(config.cat_handshake, sizeof(config.cat_handshake), "%s", "XONXOFF");
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
  expect_int_eq(config.cat_model, 2345, "saved CAT model restored");
  expect_str_eq(config.cat_device, "/dev/ttyS8", "saved CAT device restored");
  expect_int_eq(config.cat_baud, 19200, "saved CAT baud restored");
  expect_int_eq(config.cat_data_bits, 8, "saved CAT data bits restored");
  expect_int_eq(config.cat_stop_bits, 1, "saved CAT stop bits restored");
  expect_str_eq(config.cat_parity, "Odd", "saved CAT parity restored");
  expect_str_eq(config.cat_handshake, "XONXOFF",
                "saved CAT handshake restored");
  expect_int_eq(config.cat_mode_from_rig, 1,
                "saved CAT mode-from-rig restored");
  expect_str_eq(config.contest_tx_exchange, "28",
                "saved contest tx exchange restored");
}

static void test_cty_loading_and_lookup(const char *tmp_dir) {
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
  expect_true(loaded > 0, "cty_load should load at least one entry");

  const CtyEntry *sp = cty_lookup("sp9abc");
  expect_true(sp != NULL, "SP9ABC should resolve in CTY");
  if (sp) {
    expect_str_eq(sp->country, "Poland", "SP9ABC country");
    expect_int_eq(sp->cq_zone, 15, "SP9ABC CQ zone");
    expect_int_eq(sp->itu_zone, 28, "SP9ABC ITU zone");
  }

  const CtyEntry *k1 = cty_lookup("K1ABC");
  expect_true(k1 != NULL, "K1ABC should resolve in CTY");
  if (k1) {
    expect_str_eq(k1->country, "United States K1",
                  "longest prefix K1 should win over K");
  }

  const CtyEntry *unknown = cty_lookup("ZZ9ZZZ");
  expect_true(unknown == NULL, "Unknown prefix should not resolve");
}

static void test_qso_and_stats_logic(void) {
  char status[128];

  qso_init();

  int idx1 = qso_add("SP9ABC 14074 599", status, sizeof(status));
  expect_int_eq(idx1, 0, "first QSO index");
  expect_str_eq(status, "QSO OK", "first QSO status");
  expect_int_eq(qso_count, 1, "QSO count after first add");

  expect_str_eq(logbook[0].call, "SP9ABC", "callsign normalized");
  expect_str_eq(logbook[0].band, "20M", "band detection for 14074");
  expect_str_eq(logbook[0].mode, "FT8", "mode detection for 14074");
  expect_str_eq(logbook[0].country, "Poland", "country assigned from CTY");

  int idx2 = qso_add("K1ABC 14150 59", status, sizeof(status));
  expect_int_eq(idx2, 1, "second QSO index");
  expect_str_eq(logbook[1].band, "20M", "band detection for 14150");
  expect_str_eq(logbook[1].mode, "SSB", "mode detection for 14150");

  int bad_format = qso_add("K1ABC 14150", status, sizeof(status));
  expect_int_eq(bad_format, -1, "bad format should fail");
  expect_str_eq(status, "Bad format", "bad format status");

  int bad_call = qso_add("ABCDEF 14074 599", status, sizeof(status));
  expect_int_eq(bad_call, -1, "callsign without digit should fail");
  expect_str_eq(status, "Invalid callsign", "invalid callsign status");

  qso_mark_invalid(1);
  expect_true(logbook[1].invalid, "second QSO marked invalid");

  stats_update();
  expect_int_eq(stats.total_qso, 1, "stats excludes invalid QSO");
  expect_int_eq(stats.total_dxcc, 1, "stats DXCC excludes invalid QSO");
  expect_int_eq(stats.ft8, 1, "stats FT8 count");
  expect_int_eq(stats.ssb, 0, "stats SSB count after invalidation");

  qso_mark_invalid(1);
  expect_true(!logbook[1].invalid, "invalid toggle restores validity");

  stats_update();
  expect_int_eq(stats.total_qso, 2, "stats total after unmark invalid");
  expect_int_eq(stats.total_dxcc, 2, "stats DXCC after unmark invalid");
  expect_int_eq(stats.ssb, 1, "stats SSB after unmark invalid");
}

static void test_export_outputs(const char *tmp_dir) {
  char csv_path[512];
  char adi_path[512];

  snprintf(csv_path, sizeof(csv_path), "%s/out.csv", tmp_dir);
  snprintf(adi_path, sizeof(adi_path), "%s/out.adi", tmp_dir);

  expect_int_eq(export_csv(csv_path), 0, "CSV export should succeed");
  expect_int_eq(export_adif(adi_path), 0, "ADIF export should succeed");

  char *csv = read_whole_file(csv_path);
  char *adi = read_whole_file(adi_path);

  expect_true(csv != NULL, "CSV file should be readable");
  expect_true(adi != NULL, "ADIF file should be readable");

  if (csv) {
    expect_true(strstr(csv, "DATE,UTC,CALL,FREQ,BAND,MODE,RST,COMMENTS,COUNTRY") != NULL,
                "CSV header exists");
    expect_true(strstr(csv, "SP9ABC") != NULL, "CSV contains first call");
    expect_true(strstr(csv, "K1ABC") != NULL, "CSV contains second call");
  }

  if (adi) {
    expect_true(strstr(adi, "<EOH>") != NULL, "ADIF header terminator exists");
    expect_true(strstr(adi, "<CALL:6>SP9ABC") != NULL,
                "ADIF contains first call");
    expect_true(strstr(adi, "<CALL:5>K1ABC") != NULL,
                "ADIF contains second call");
    expect_true(strstr(adi, "<MODE:3>SSB") != NULL,
                "ADIF contains SSB mode field");
  }

  free(csv);
  free(adi);
}

static void test_contest_definition_and_cabrillo(const char *tmp_dir) {
  char contest_path[512];
  char cabrillo_path[512];

  snprintf(contest_path, sizeof(contest_path), "%s/cqww.conf", tmp_dir);
  snprintf(cabrillo_path, sizeof(cabrillo_path), "%s/out.cbr", tmp_dir);

  const char *contest_text =
      "NAME=CQ-WW-CW\n"
      "CABRILLO_NAME=CQ-WW-CW\n"
      "MODE=CW\n"
      "CATEGORY_OPERATOR=SINGLE-OP\n"
      "CATEGORY_BAND=ALL\n"
      "CATEGORY_POWER=LOW\n"
      "EXCHANGE_SENT=001\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(contest_path, contest_text), 0,
                "write contest definition file");

  ContestDefinition def;
  char err[128] = {0};
  expect_int_eq(contest_definition_load(contest_path, &def, err, sizeof(err)),
                0, "contest definition should load");
  expect_str_eq(def.name, "CQ-WW-CW", "contest name parsed");
  expect_str_eq(def.cabrillo_name, "CQ-WW-CW", "cabrillo name parsed");
  expect_int_eq(def.field_count, 1, "contest field parsed");

  expect_int_eq(export_cabrillo(cabrillo_path, &def, "SP9ABC"), 0,
                "cabrillo export should succeed");

  char *cbr = read_whole_file(cabrillo_path);
  expect_true(cbr != NULL, "Cabrillo output should be readable");
  if (cbr) {
    expect_true(strstr(cbr, "START-OF-LOG: 3.0") != NULL,
                "Cabrillo header exists");
    expect_true(strstr(cbr, "CONTEST: CQ-WW-CW") != NULL,
                "Cabrillo contest header exists");
    expect_true(strstr(cbr, "QSO:") != NULL, "Cabrillo contains QSO rows");
  }

  free(cbr);
}

static void test_maidenhead_conversion(void) {
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
                "invalid locator ZZ99 should fail");
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

int main(void) {
  char tmp_dir[256];
  if (make_temp_dir(tmp_dir, sizeof(tmp_dir)) != 0) {
    fprintf(stderr, "Cannot create temp dir: %s\n", strerror(errno));
    return 2;
  }
  
  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/regression.sqlite3", tmp_dir);
  setenv("LOGGER_DB_PATH", db_path, 1);

  test_config_loading(tmp_dir);
  test_config_save_roundtrip(tmp_dir);
  test_cty_loading_and_lookup(tmp_dir);
  test_qso_and_stats_logic();
  test_export_outputs(tmp_dir);
  test_contest_definition_and_cabrillo(tmp_dir);
  test_maidenhead_conversion();
  test_call_suggestions();

  if (g_failures == 0) {
    printf("All regression tests passed.\n");
    return 0;
  }

  printf("Regression tests failed: %d\n", g_failures);
  return 1;
}
