#include "config.h"
#include "contest.h"
#include "cty.h"
#include "export.h"
#include "maidenhead.h"
#include "qso.h"
#include "qtc.h"
#include "suggestion.h"
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
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
      "CONTEST_TX_EXCHANGE = 28\n"
      "NET_STATION_ID = RUN1\n"
      "NET_SHARED_KEY = secret123\n"
      "NET_TLS_CERT_FILE = cert.pem\n"
      "NET_TLS_KEY_FILE = key.pem\n"
      "NET_TLS_PEER_FINGERPRINT = AA:BB:CC\n"
      "NET_HEARTBEAT_SEC = 9\n"
      "NET_RETRY_MIN_MS = 1500\n"
      "NET_RETRY_MAX_MS = 5500\n"
      "NET_TLS = 1\n"
      "NET_RATE_LIMIT_WINDOW_SEC = 3\n"
      "NET_RATE_LIMIT_BURST = 77\n"
      "NET_MAX_FRAME_BYTES = 12000\n";

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
  expect_str_eq(config.net_station_id, "RUN1",
                "config NET_STATION_ID parsed");
  expect_str_eq(config.net_shared_key, "secret123",
                "config NET_SHARED_KEY parsed");
  expect_str_eq(config.net_auth_token, "secret123",
                "config shared key should mirror auth token");
  expect_str_eq(config.net_tls_cert_file, "cert.pem",
                "config NET_TLS_CERT_FILE parsed");
  expect_str_eq(config.net_tls_key_file, "key.pem",
                "config NET_TLS_KEY_FILE parsed");
  expect_str_eq(config.net_tls_peer_fingerprint, "AA:BB:CC",
                "config NET_TLS_PEER_FINGERPRINT parsed");
  expect_int_eq(config.net_heartbeat_sec, 9,
                "config NET_HEARTBEAT_SEC parsed");
  expect_int_eq(config.net_retry_min_ms, 1500,
                "config NET_RETRY_MIN_MS parsed");
  expect_int_eq(config.net_retry_max_ms, 5500,
                "config NET_RETRY_MAX_MS parsed");
  expect_int_eq(config.net_tls, 1, "config NET_TLS parsed");
  expect_int_eq(config.net_rate_limit_window_sec, 3,
                "config NET_RATE_LIMIT_WINDOW_SEC parsed");
  expect_int_eq(config.net_rate_limit_burst, 77,
                "config NET_RATE_LIMIT_BURST parsed");
  expect_int_eq(config.net_max_frame_bytes, 12000,
                "config NET_MAX_FRAME_BYTES parsed");

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
  snprintf(config.net_station_id, sizeof(config.net_station_id), "%s",
           "RUN2");
  snprintf(config.net_shared_key, sizeof(config.net_shared_key), "%s",
           "secret999");
  snprintf(config.net_auth_token, sizeof(config.net_auth_token), "%s",
           "secret999");
  snprintf(config.net_tls_cert_file, sizeof(config.net_tls_cert_file), "%s",
           "saved_cert.pem");
  snprintf(config.net_tls_key_file, sizeof(config.net_tls_key_file), "%s",
           "saved_key.pem");
  snprintf(config.net_tls_peer_fingerprint,
           sizeof(config.net_tls_peer_fingerprint), "%s",
           "11:22:33");
  config.net_heartbeat_sec = 11;
  config.net_retry_min_ms = 2000;
  config.net_retry_max_ms = 12000;
  config.net_tls = 1;
  config.net_rate_limit_window_sec = 4;
  config.net_rate_limit_burst = 88;
  config.net_max_frame_bytes = 24000;

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
  config.net_station_id[0] = 0;
  config.net_shared_key[0] = 0;
  config.net_auth_token[0] = 0;
  config.net_tls_cert_file[0] = 0;
  config.net_tls_key_file[0] = 0;
  config.net_tls_peer_fingerprint[0] = 0;
  config.net_heartbeat_sec = 0;
  config.net_retry_min_ms = 0;
  config.net_retry_max_ms = 0;
  config.net_tls = 0;
  config.net_rate_limit_window_sec = 0;
  config.net_rate_limit_burst = 0;
  config.net_max_frame_bytes = 0;

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
  expect_str_eq(config.net_station_id, "RUN2",
                "saved NET_STATION_ID restored");
  expect_str_eq(config.net_shared_key, "secret999",
                "saved NET_SHARED_KEY restored");
  expect_str_eq(config.net_tls_cert_file, "saved_cert.pem",
                "saved NET_TLS_CERT_FILE restored");
  expect_str_eq(config.net_tls_key_file, "saved_key.pem",
                "saved NET_TLS_KEY_FILE restored");
  expect_str_eq(config.net_tls_peer_fingerprint, "11:22:33",
                "saved NET_TLS_PEER_FINGERPRINT restored");
  expect_int_eq(config.net_heartbeat_sec, 11,
                "saved NET_HEARTBEAT_SEC restored");
  expect_int_eq(config.net_retry_min_ms, 2000,
                "saved NET_RETRY_MIN_MS restored");
  expect_int_eq(config.net_retry_max_ms, 12000,
                "saved NET_RETRY_MAX_MS restored");
  expect_int_eq(config.net_tls, 1, "saved NET_TLS restored");
  expect_int_eq(config.net_rate_limit_window_sec, 4,
                "saved NET_RATE_LIMIT_WINDOW_SEC restored");
  expect_int_eq(config.net_rate_limit_burst, 88,
                "saved NET_RATE_LIMIT_BURST restored");
  expect_int_eq(config.net_max_frame_bytes, 24000,
                "saved NET_MAX_FRAME_BYTES restored");
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
  char status[128];
  char expected_serial_1[16];
  char expected_serial_2[16];
  char expected_fragment_1[64];
  char expected_fragment_2[64];

  snprintf(contest_path, sizeof(contest_path), "%s/cqww.conf", tmp_dir);
  snprintf(cabrillo_path, sizeof(cabrillo_path), "%s/out.cbr", tmp_dir);

  const char *contest_text =
      "NAME=CQ-WW-CW\n"
      "CABRILLO_NAME=CQ-WW-CW\n"
      "MODE=CW\n"
      "CATEGORY_OPERATOR=SINGLE-OP\n"
      "CATEGORY_BAND=ALL\n"
      "CATEGORY_POWER=LOW\n"
      "EXCHANGE_SENT=#\n"
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
                "cabrillo export should succeed");

  char *cbr = read_whole_file(cabrillo_path);
  expect_true(cbr != NULL, "Cabrillo output should be readable");
  if (cbr) {
    expect_true(strstr(cbr, "START-OF-LOG: 3.0") != NULL,
                "Cabrillo header exists");
    expect_true(strstr(cbr, "CONTEST: CQ-WW-CW") != NULL,
                "Cabrillo contest header exists");
    expect_true(strstr(cbr, "QSO:") != NULL, "Cabrillo contains QSO rows");
    expect_true(strstr(cbr, expected_fragment_1) != NULL,
                "Cabrillo should generate first serial exchange from # template");
    expect_true(strstr(cbr, expected_fragment_2) != NULL,
                "Cabrillo should generate second serial exchange from # template");
    expect_true(strstr(cbr, "599 #") == NULL,
                "Cabrillo should not emit literal # as sent exchange");
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

static int bandmap_target_row_for_frequency(const int *freqs, int count,
                                            int target_freq_khz) {
  int target_row = -1;
  if (target_freq_khz > 0) {
    for (int i = 0; i < count; i++) {
      if (freqs[i] == target_freq_khz) {
        target_row = i;
        break;
      }
    }
  }

  if (target_row >= 0)
    return target_row;

  int best_delta = INT_MAX;
  for (int i = 0; i < count; i++) {
    const int delta = abs(freqs[i] - target_freq_khz);
    if (delta < best_delta) {
      best_delta = delta;
      target_row = i;
    }
  }

  return target_row;
}

static int bandmap_refresh_keeps_current_row(const int *freqs, int count,
                                            int target_freq_khz,
                                            int current_row) {
  if (current_row >= 0 && current_row < count)
    return current_row;

  return bandmap_target_row_for_frequency(freqs, count, target_freq_khz);
}

static int bandmap_navigate_step(const int *freqs, int count,
                                int active_freq_khz, int current_row,
                                int step) {
  int row = current_row;
  if (row < 0) {
    if (step > 0) {
      row = count - 1;
      for (int i = 0; i < count; i++) {
        if (freqs[i] > active_freq_khz) {
          row = i;
          break;
        }
      }
    } else {
      row = 0;
      for (int i = count - 1; i >= 0; i--) {
        if (freqs[i] < active_freq_khz) {
          row = i;
          break;
        }
      }
    }
  } else {
    row += step;
  }

  row = row < 0 ? 0 : row;
  row = row >= count ? count - 1 : row;
  return row;
}

static void test_bandmap_ctrl_navigation_regression(void) {
  const int freqs[] = {14000, 14025, 14050, 14075, 14100};

  expect_int_eq(bandmap_refresh_keeps_current_row(freqs, 5, 14010, 2), 2,
                "refresh should preserve the active bandmap row instead of snapping to nearest frequency");
  expect_int_eq(bandmap_target_row_for_frequency(freqs, 5, 14010), 1,
                "nearest-frequency fallback should still select the closest row when no current row exists");

  int row = 0;
  row = bandmap_navigate_step(freqs, 5, 14000, row, 1);
  expect_int_eq(row, 1, "Ctrl+Down should move to the next bandmap row");
  row = bandmap_refresh_keeps_current_row(freqs, 5, 14010, row);
  expect_int_eq(row, 1, "Ctrl+Down selection should remain stable after refresh");

  row = bandmap_navigate_step(freqs, 5, 14010, row, 1);
  expect_int_eq(row, 2, "Ctrl+Down should continue to the next row after refresh");
  row = bandmap_refresh_keeps_current_row(freqs, 5, 14060, row);
  expect_int_eq(row, 2, "bandmap refresh must not reset the user-selected row");

  row = bandmap_navigate_step(freqs, 5, 14060, row, -1);
  expect_int_eq(row, 1, "Ctrl+Up should move back one row");
  row = bandmap_refresh_keeps_current_row(freqs, 5, 14020, row);
  expect_int_eq(row, 1, "Ctrl+Up selection should also remain stable after refresh");

  row = bandmap_navigate_step(freqs, 5, 14020, row, -1);
  expect_int_eq(row, 0, "Ctrl+Up should continue to the previous row without truncating");
}

/* ------------------------------------------------------------------ */
/* QTC regression tests                                                 */
/* ------------------------------------------------------------------ */

static void test_wae_definition_qtc_round_trip(const char *tmp_dir) {
  char in_path[512];
  char out_path[512];
  snprintf(in_path,  sizeof(in_path),  "%s/wae_round.conf", tmp_dir);
  snprintf(out_path, sizeof(out_path), "%s/wae_round_out.conf", tmp_dir);

  const char *conf_text =
      "NAME=WAE-ROUND\n"
      "CABRILLO_NAME=WAE-DX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "MULTIPLIER=DXCC_PER_BAND\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(in_path, conf_text), 0,
                "write WAE round-trip input");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(in_path, &def, err, sizeof(err)), 0,
                "load WAE round-trip definition");
  expect_str_eq(def.qtc_sender_side, "EU", "round-trip qtc_sender_side");
  expect_int_eq(def.points_per_qtc,  1,    "round-trip points_per_qtc");

  /* Export via import-export path. */
  char warn[128] = {0};
  expect_int_eq(
      contest_definition_import_dxlog(in_path, out_path, err, sizeof(err),
                                      warn, sizeof(warn)),
      0, "import_dxlog round-trip should succeed");

  ContestDefinition def2;
  expect_int_eq(contest_definition_load(out_path, &def2, err, sizeof(err)), 0,
                "load re-exported definition");
  expect_str_eq(def2.qtc_sender_side, "EU",
                "re-exported qtc_sender_side should be EU");
  expect_int_eq(def2.points_per_qtc, 1,
                "re-exported points_per_qtc should be 1");
}

static void test_cabrillo_qtc_section(const char *tmp_dir) {
  char conf_path[512];
  char cab_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/wae_cab.conf", tmp_dir);
  snprintf(cab_path,  sizeof(cab_path),  "%s/wae_cab.cbr",  tmp_dir);

  const char *conf_text =
      "NAME=WAE-CAB-TEST\n"
      "CABRILLO_NAME=WAE-DX-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write WAE cabrillo test definition");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                "load WAE cabrillo test definition");

  /* Seed QTC store with a two-record bundle. */
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
                "export_cabrillo_with_qtc should succeed");

  char *cab = read_whole_file(cab_path);
  expect_true(cab != NULL, "cabrillo file should be readable");
  if (cab) {
    expect_true(strstr(cab, "QTC:") != NULL,
                "exported cabrillo must contain QTC: section");
    expect_true(strstr(cab, "G3XYZ") != NULL,
                "exported cabrillo QTC must contain second record call");
    expect_true(strstr(cab, "END-OF-LOG:") != NULL,
                "cabrillo must end with END-OF-LOG:");
    free(cab);
  }

  qtc_bundle_count = 0;
}

static void test_qtc_no_lines_without_qtc_contest(const char *tmp_dir) {
  char conf_path[512];
  char cab_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/no_qtc.conf", tmp_dir);
  snprintf(cab_path,  sizeof(cab_path),  "%s/no_qtc.cbr",  tmp_dir);

  const char *conf_text =
      "NAME=NO-QTC-TEST\n"
      "CABRILLO_NAME=NO-QTC\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "FIELD=SERIAL,Serial Number,required\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write no-qtc contest definition");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                "load no-qtc contest definition");
  expect_str_eq(def.qtc_sender_side, "NONE",
                "default qtc_sender_side should be NONE");

  /* Even with bundles in store, no QTC lines should appear. */
  qtc_bundle_count = 0;
  QTCBundle b;
  memset(&b, 0, sizeof(b));
  snprintf(b.sender_call,   sizeof(b.sender_call),   "%s", "SP5XYZ");
  snprintf(b.receiver_call, sizeof(b.receiver_call), "%s", "W1AW");
  b.bundle_nr    = 1;
  b.record_count = 1;
  b.sent         = 1;
  qtc_record_init(&b.records[0], "20241201", "1430", "DK5AI", "42");
  qtc_bundles[0] = b;
  qtc_bundle_count = 1;

  expect_int_eq(export_cabrillo_with_qtc(cab_path, &def, "SP5XYZ"), 0,
                "export should succeed even when qtc disabled");

  char *cab = read_whole_file(cab_path);
  expect_true(cab != NULL, "cabrillo file should be readable");
  if (cab) {
    expect_true(strstr(cab, "QTC:") == NULL,
                "cabrillo without qtc contest must not contain QTC: lines");
    free(cab);
  }

  qtc_bundle_count = 0;
}

/*
 * Verify that the local WAE CW config file loads with the correct parameters
 * derived from the official DXLog definition (DARC-WAEDC-CW, DXCC_PER_BAND,
 * EU QTC sender).
 */
static void test_wae_cabrillo_name_from_config(const char *tmp_dir) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/wae_darc.conf", tmp_dir);

  /* Minimal definition matching what our wae_cw.conf produces. */
  const char *conf_text =
      "NAME=WAE-DX-CW\n"
      "CABRILLO_NAME=DARC-WAEDC-CW\n"
      "MODE=CW\n"
      "EXCHANGE_SENT=#\n"
      "POINTS_PER_QSO=1\n"
      "MULTIPLIER=DXCC_PER_BAND\n"
      "QTC_SENDER=EU\n"
      "POINTS_PER_QTC=1\n"
      "FIELD=SERIAL,Rcv Nr,required\n";

  expect_int_eq(write_text_file(conf_path, conf_text), 0,
                "write WAE DARC cabrillo test conf");

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                "load WAE DARC cabrillo test conf");

  expect_str_eq(def.name,           "WAE-DX-CW",       "WAE name correct");
  expect_str_eq(def.cabrillo_name,  "DARC-WAEDC-CW",   "WAE cabrillo name = DARC-WAEDC-CW");
  expect_str_eq(def.mode,           "CW",               "WAE mode = CW");
  expect_str_eq(def.qtc_sender_side,"EU",               "WAE qtc sender = EU");
  expect_int_eq(def.points_per_qtc, 1,                  "WAE points_per_qtc = 1");
  expect_int_eq((int)def.multiplier_type,
                (int)CONTEST_MULT_DXCC_PER_BAND,        "WAE multiplier = DXCC_PER_BAND");
  expect_int_eq(def.field_count, 1,                     "WAE should have 1 exchange field");
  if (def.field_count >= 1) {
    expect_str_eq(def.fields[0].name, "SERIAL", "WAE field name = SERIAL");
    expect_int_eq(def.fields[0].required, 1,    "WAE SERIAL field is required");
  }
}

/* ------------------------------------------------------------------ */
/* New contest definition regression tests                              */
/* ------------------------------------------------------------------ */

/*
 * Helper: load a contest definition from an inline conf text and check basics.
 */
static void check_conf_loads(const char *tmp_dir, const char *filename,
                             const char *conf_text,
                             const char *expected_cabrillo_name,
                             int expected_multiplier) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "%s/%s", tmp_dir, filename);

  expect_int_eq(write_text_file(conf_path, conf_text), 0, filename);

  ContestDefinition def;
  char err[64] = {0};
  expect_int_eq(contest_definition_load(conf_path, &def, err, sizeof(err)), 0,
                filename);
  expect_str_eq(def.cabrillo_name, expected_cabrillo_name, filename);
  if (expected_multiplier >= 0)
    expect_int_eq((int)def.multiplier_type, expected_multiplier, filename);
}

static void test_new_contest_defs_load(const char *tmp_dir) {
  /* SAC CW */
  check_conf_loads(tmp_dir, "sac_cw.conf",
    "NAME=SAC-CW\nCABRILLO_NAME=SAC-CW\nMODE=CW\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Nr,required\n",
    "SAC-CW", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* SAC SSB */
  check_conf_loads(tmp_dir, "sac_ssb.conf",
    "NAME=SAC-SSB\nCABRILLO_NAME=SAC-SSB\nMODE=SSB\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Nr,required\n",
    "SAC-SSB", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* ARRL DX CW */
  check_conf_loads(tmp_dir, "arrl_dx_cw.conf",
    "NAME=ARRL-DX-CW\nCABRILLO_NAME=ARRL-DX-CW\nMODE=CW\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Exch,required\n",
    "ARRL-DX-CW", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* ARRL DX SSB */
  check_conf_loads(tmp_dir, "arrl_dx_ssb.conf",
    "NAME=ARRL-DX-SSB\nCABRILLO_NAME=ARRL-DX-SSB\nMODE=SSB\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Exch,required\n",
    "ARRL-DX-SSB", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* WAG */
  check_conf_loads(tmp_dir, "wag.conf",
    "NAME=WAG\nCABRILLO_NAME=WAG\nMODE=MIXED\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Exch,required\n",
    "WAG", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* Oceania DX CW - PREFIX_PER_BAND */
  check_conf_loads(tmp_dir, "oceania_dx_cw.conf",
    "NAME=OCEANIA-DX-CW\nCABRILLO_NAME=OCEANIA-DX-CW\nMODE=CW\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=PREFIX_PER_BAND\nFIELD=SERIAL,Rcv Nr,required\n",
    "OCEANIA-DX-CW", (int)CONTEST_MULT_PREFIX_PER_BAND);

  /* RDXC CW */
  check_conf_loads(tmp_dir, "rdxc_cw.conf",
    "NAME=RDXC-CW\nCABRILLO_NAME=RDXC\nMODE=CW\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Exch,required\n",
    "RDXC", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* Holyland */
  check_conf_loads(tmp_dir, "holyland.conf",
    "NAME=HOLYLAND-DX\nCABRILLO_NAME=HOLYLAND-DX\nMODE=MIXED\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=DXCC_PER_BAND\nFIELD=SERIAL,Rcv Exch,required\n",
    "HOLYLAND-DX", (int)CONTEST_MULT_DXCC_PER_BAND);

  /* IARU HF - verify ZONE_PER_BAND */
  check_conf_loads(tmp_dir, "iaru_hf.conf",
    "NAME=IARU-HF-CHAMPIONSHIP\nCABRILLO_NAME=IARU-HF\nMODE=MIXED\nEXCHANGE_SENT=ITU\n"
    "MULTIPLIER=ZONE_PER_BAND\nFIELD=ITU_ZONE,ITU Zone or HQ,required\n",
    "IARU-HF", (int)CONTEST_MULT_ZONE_PER_BAND);

  /* CQ WPX SSB - must now have PREFIX multiplier (was NONE before fix) */
  check_conf_loads(tmp_dir, "cq_wpx_ssb.conf",
    "NAME=CQ-WPX-SSB\nCABRILLO_NAME=CQ-WPX-SSB\nMODE=SSB\nEXCHANGE_SENT=#\n"
    "MULTIPLIER=PREFIX\nFIELD=SERIAL,Serial Number,required\n",
    "CQ-WPX-SSB", (int)CONTEST_MULT_PREFIX);
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

  /* QTC regression tests */
  test_wae_definition_qtc_round_trip(tmp_dir);
  test_cabrillo_qtc_section(tmp_dir);
  test_qtc_no_lines_without_qtc_contest(tmp_dir);
  test_wae_cabrillo_name_from_config(tmp_dir);
  test_new_contest_defs_load(tmp_dir);

  if (g_failures == 0) {
    printf("All regression tests passed.\n");
    return 0;
  }

  printf("Regression tests failed: %d\n", g_failures);
  return 1;
}
