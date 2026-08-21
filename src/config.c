#include "config.h"

Config config;
static char config_last_loaded_path[512] = {0};

/*
 * Trim leading and trailing whitespace from a string in place.
 *
 * @param s String to trim.
 * @return Nothing.
 */
static void trim(char *s) {
  if (!s)
    return;

  char *p = s;

  while (isspace((unsigned char)*p))
    p++;

  if (p != s)
    memmove(s, p, strlen(p) + 1);

  size_t len = strlen(s);

  while (len && isspace((unsigned char)s[len - 1])) {
    s[--len] = 0;
  }
}

/*
 * Restore default configuration values.
 *
 * @return Nothing.
 */
static void set_defaults(void) {
  config.lat = 0.0;
  config.lon = 0.0;

  strcpy(config.locator, "");

  strcpy(config.dxc_host, "telnet.reversebeacon.net");

  config.dxc_port = 7000;

  strcpy(config.dxc_call, "N0CALL");

  config.cat_model = 2;
  strcpy(config.cat_device, "/dev/ttyUSB0");
  config.cat_baud = 9600;
  config.cat_data_bits = 8;
  config.cat_stop_bits = 1;
  strcpy(config.cat_parity, "None");
  strcpy(config.cat_handshake, "None");
  config.cat_mode_from_rig = 0;
  config.cat_auto_connect = 1;

  config.cat2_model = 2;
  strcpy(config.cat2_device, "/dev/ttyUSB1");
  config.cat2_baud = 9600;
  config.cat2_data_bits = 8;
  config.cat2_stop_bits = 1;
  strcpy(config.cat2_parity, "None");
  strcpy(config.cat2_handshake, "None");

  strcpy(config.station_call, "N0CALL");
  strcpy(config.operator_call, "N0CALL");
  strcpy(config.operator_name, "");
  strcpy(config.contest_definition_path, "contest.conf");
  strcpy(config.contest_tx_exchange, "");
  config.contest_technique = CONTEST_TECH_SO1R;

  strcpy(config.cw_device, "/dev/ttyUSB2");
  strcpy(config.cw_keyer_line, "DTR");
  config.cw_wpm = 20;
  config.cw_auto_connect = 1;

  config.net_enabled = 0;
  strcpy(config.net_role, "client");
  strcpy(config.net_station_id, "");
  strcpy(config.net_server_host, "127.0.0.1");
  config.net_server_port = 9230;
  strcpy(config.net_auth_token, "");
  strcpy(config.net_shared_key, "");
  strcpy(config.net_tls_cert_file, "logger_net_cert.pem");
  strcpy(config.net_tls_key_file, "logger_net_key.pem");
  strcpy(config.net_tls_peer_fingerprint, "");
  config.net_sync_interval_ms = 1000;
  config.net_heartbeat_sec = 5;
  config.net_retry_min_ms = 1000;
  config.net_retry_max_ms = 30000;
  config.net_tls = 0;
  config.net_rate_limit_window_sec = 1;
  config.net_rate_limit_burst = 32;
  config.net_max_frame_bytes = 65536;
}

/*
 * Load configuration values from a logger.conf-style file.
 *
 * @param filename Path to the configuration file.
 * @return 0 on success, or -1 if the file cannot be opened.
 */
int config_load(const char *filename) {
  set_defaults();

  config_last_loaded_path[0] = 0;

  FILE *f = fopen(filename, "r");

  if (!f)
    return -1;

  if (filename && filename[0]) {
    strncpy(config_last_loaded_path, filename, sizeof(config_last_loaded_path));
    config_last_loaded_path[sizeof(config_last_loaded_path) - 1] = 0;
  }

  char line[256];
  int saw_operator_call = 0;

  while (fgets(line, sizeof(line), f)) {
    trim(line);

    if (line[0] == 0)
      continue;

    if (line[0] == '#')
      continue;

    char *eq = strchr(line, '=');

    if (!eq)
      continue;

    *eq = 0;

    char *key = line;
    char *value = eq + 1;

    trim(key);
    trim(value);

    if (strcmp(key, "LAT") == 0) {
      config.lat = atof(value);
    } else if (strcmp(key, "LON") == 0) {
      config.lon = atof(value);
    } else if (strcmp(key, "LOCATOR") == 0) {
      strncpy(config.locator, value, sizeof(config.locator));

      config.locator[sizeof(config.locator) - 1] = 0;
    } else if (strcmp(key, "DXC_HOST") == 0) {
      strncpy(config.dxc_host, value, sizeof(config.dxc_host));

      config.dxc_host[sizeof(config.dxc_host) - 1] = 0;
    } else if (strcmp(key, "DXC_PORT") == 0) {
      config.dxc_port = atoi(value);
    } else if (strcmp(key, "DXC_CALL") == 0) {
      strncpy(config.dxc_call, value, sizeof(config.dxc_call));

      config.dxc_call[sizeof(config.dxc_call) - 1] = 0;
    } else if (strcmp(key, "CAT_MODEL") == 0) {
      config.cat_model = atoi(value);
    } else if (strcmp(key, "CAT_DEVICE") == 0) {
      strncpy(config.cat_device, value, sizeof(config.cat_device));

      config.cat_device[sizeof(config.cat_device) - 1] = 0;
    } else if (strcmp(key, "CAT_BAUD") == 0) {
      config.cat_baud = atoi(value);
    } else if (strcmp(key, "CAT_DATA_BITS") == 0) {
      config.cat_data_bits = atoi(value);
    } else if (strcmp(key, "CAT_STOP_BITS") == 0) {
      config.cat_stop_bits = atoi(value);
    } else if (strcmp(key, "CAT_PARITY") == 0) {
      strncpy(config.cat_parity, value, sizeof(config.cat_parity));

      config.cat_parity[sizeof(config.cat_parity) - 1] = 0;
    } else if (strcmp(key, "CAT_HANDSHAKE") == 0) {
      strncpy(config.cat_handshake, value, sizeof(config.cat_handshake));

      config.cat_handshake[sizeof(config.cat_handshake) - 1] = 0;
    } else if (strcmp(key, "CAT_MODE_FROM_RIG") == 0) {
      config.cat_mode_from_rig = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "CAT_AUTO_CONNECT") == 0) {
      config.cat_auto_connect = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "CAT2_MODEL") == 0) {
      config.cat2_model = atoi(value);
    } else if (strcmp(key, "CAT2_DEVICE") == 0) {
      strncpy(config.cat2_device, value, sizeof(config.cat2_device));

      config.cat2_device[sizeof(config.cat2_device) - 1] = 0;
    } else if (strcmp(key, "CAT2_BAUD") == 0) {
      config.cat2_baud = atoi(value);
    } else if (strcmp(key, "CAT2_DATA_BITS") == 0) {
      config.cat2_data_bits = atoi(value);
    } else if (strcmp(key, "CAT2_STOP_BITS") == 0) {
      config.cat2_stop_bits = atoi(value);
    } else if (strcmp(key, "CAT2_PARITY") == 0) {
      strncpy(config.cat2_parity, value, sizeof(config.cat2_parity));

      config.cat2_parity[sizeof(config.cat2_parity) - 1] = 0;
    } else if (strcmp(key, "CAT2_HANDSHAKE") == 0) {
      strncpy(config.cat2_handshake, value, sizeof(config.cat2_handshake));

      config.cat2_handshake[sizeof(config.cat2_handshake) - 1] = 0;
    } else if (strcmp(key, "STATION_CALL") == 0) {
      strncpy(config.station_call, value, sizeof(config.station_call));

      config.station_call[sizeof(config.station_call) - 1] = 0;
    } else if (strcmp(key, "OPERATOR_CALL") == 0) {
      strncpy(config.operator_call, value, sizeof(config.operator_call));
      config.operator_call[sizeof(config.operator_call) - 1] = 0;
      saw_operator_call = 1;
    } else if (strcmp(key, "OPERATOR_NAME") == 0) {
      strncpy(config.operator_name, value, sizeof(config.operator_name));

      config.operator_name[sizeof(config.operator_name) - 1] = 0;
    } else if (strcmp(key, "CONTEST_DEF_FILE") == 0) {
      strncpy(config.contest_definition_path, value,
              sizeof(config.contest_definition_path));

      config.contest_definition_path[sizeof(config.contest_definition_path) -
                                     1] = 0;
    } else if (strcmp(key, "CONTEST_TX_EXCHANGE") == 0) {
      strncpy(config.contest_tx_exchange, value,
              sizeof(config.contest_tx_exchange));

      config.contest_tx_exchange[sizeof(config.contest_tx_exchange) - 1] = 0;
    } else if (strcmp(key, "CONTEST_TECHNIQUE") == 0) {
      config.contest_technique = contest_technique_from_text(value);
    } else if (strcmp(key, "CW_DEVICE") == 0) {
      strncpy(config.cw_device, value, sizeof(config.cw_device));
      config.cw_device[sizeof(config.cw_device) - 1] = 0;
    } else if (strcmp(key, "CW_KEYER_LINE") == 0) {
      strncpy(config.cw_keyer_line, value, sizeof(config.cw_keyer_line));
      config.cw_keyer_line[sizeof(config.cw_keyer_line) - 1] = 0;
    } else if (strcmp(key, "CW_WPM") == 0) {
      config.cw_wpm = atoi(value);
      if (config.cw_wpm < 1) config.cw_wpm = 1;
      if (config.cw_wpm > 60) config.cw_wpm = 60;
    } else if (strcmp(key, "CW_AUTO_CONNECT") == 0) {
      config.cw_auto_connect = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "NET_ENABLED") == 0) {
      config.net_enabled = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "NET_ROLE") == 0) {
      strncpy(config.net_role, value, sizeof(config.net_role));
      config.net_role[sizeof(config.net_role) - 1] = 0;
    } else if (strcmp(key, "NET_STATION_ID") == 0) {
      strncpy(config.net_station_id, value, sizeof(config.net_station_id));
      config.net_station_id[sizeof(config.net_station_id) - 1] = 0;
    } else if (strcmp(key, "NET_SERVER_HOST") == 0) {
      strncpy(config.net_server_host, value, sizeof(config.net_server_host));
      config.net_server_host[sizeof(config.net_server_host) - 1] = 0;
    } else if (strcmp(key, "NET_SERVER_PORT") == 0) {
      config.net_server_port = atoi(value);
      if (config.net_server_port < 1) config.net_server_port = 1;
      if (config.net_server_port > 65535) config.net_server_port = 65535;
    } else if (strcmp(key, "NET_AUTH_TOKEN") == 0) {
      strncpy(config.net_auth_token, value, sizeof(config.net_auth_token));
      config.net_auth_token[sizeof(config.net_auth_token) - 1] = 0;
      if (!config.net_shared_key[0]) {
        strncpy(config.net_shared_key, value, sizeof(config.net_shared_key));
        config.net_shared_key[sizeof(config.net_shared_key) - 1] = 0;
      }
    } else if (strcmp(key, "NET_SHARED_KEY") == 0) {
      strncpy(config.net_shared_key, value, sizeof(config.net_shared_key));
      config.net_shared_key[sizeof(config.net_shared_key) - 1] = 0;
      if (!config.net_auth_token[0]) {
        strncpy(config.net_auth_token, value, sizeof(config.net_auth_token));
        config.net_auth_token[sizeof(config.net_auth_token) - 1] = 0;
      }
    } else if (strcmp(key, "NET_TLS_CERT_FILE") == 0) {
      strncpy(config.net_tls_cert_file, value, sizeof(config.net_tls_cert_file));
      config.net_tls_cert_file[sizeof(config.net_tls_cert_file) - 1] = 0;
    } else if (strcmp(key, "NET_TLS_KEY_FILE") == 0) {
      strncpy(config.net_tls_key_file, value, sizeof(config.net_tls_key_file));
      config.net_tls_key_file[sizeof(config.net_tls_key_file) - 1] = 0;
    } else if (strcmp(key, "NET_TLS_PEER_FINGERPRINT") == 0) {
      strncpy(config.net_tls_peer_fingerprint, value,
              sizeof(config.net_tls_peer_fingerprint));
      config.net_tls_peer_fingerprint[sizeof(config.net_tls_peer_fingerprint) -
                                      1] = 0;
    } else if (strcmp(key, "NET_SYNC_INTERVAL_MS") == 0) {
      config.net_sync_interval_ms = atoi(value);
      if (config.net_sync_interval_ms < 100) config.net_sync_interval_ms = 100;
      if (config.net_sync_interval_ms > 60000) config.net_sync_interval_ms = 60000;
    } else if (strcmp(key, "NET_HEARTBEAT_SEC") == 0) {
      config.net_heartbeat_sec = atoi(value);
      if (config.net_heartbeat_sec < 1) config.net_heartbeat_sec = 1;
      if (config.net_heartbeat_sec > 300) config.net_heartbeat_sec = 300;
    } else if (strcmp(key, "NET_RETRY_MIN_MS") == 0) {
      config.net_retry_min_ms = atoi(value);
      if (config.net_retry_min_ms < 100) config.net_retry_min_ms = 100;
      if (config.net_retry_min_ms > 60000) config.net_retry_min_ms = 60000;
    } else if (strcmp(key, "NET_RETRY_MAX_MS") == 0) {
      config.net_retry_max_ms = atoi(value);
      if (config.net_retry_max_ms < 100) config.net_retry_max_ms = 100;
      if (config.net_retry_max_ms > 600000) config.net_retry_max_ms = 600000;
    } else if (strcmp(key, "NET_TLS") == 0) {
      config.net_tls = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "NET_RATE_LIMIT_WINDOW_SEC") == 0) {
      config.net_rate_limit_window_sec = atoi(value);
      if (config.net_rate_limit_window_sec < 1) config.net_rate_limit_window_sec = 1;
      if (config.net_rate_limit_window_sec > 300) config.net_rate_limit_window_sec = 300;
    } else if (strcmp(key, "NET_RATE_LIMIT_BURST") == 0) {
      config.net_rate_limit_burst = atoi(value);
      if (config.net_rate_limit_burst < 1) config.net_rate_limit_burst = 1;
      if (config.net_rate_limit_burst > 10000) config.net_rate_limit_burst = 10000;
    } else if (strcmp(key, "NET_MAX_FRAME_BYTES") == 0) {
      config.net_max_frame_bytes = atoi(value);
      if (config.net_max_frame_bytes < 1024) config.net_max_frame_bytes = 1024;
      if (config.net_max_frame_bytes > 1048576) config.net_max_frame_bytes = 1048576;
    }
  }

  if (!saw_operator_call && config.station_call[0]) {
    strncpy(config.operator_call, config.station_call, sizeof(config.operator_call));
    config.operator_call[sizeof(config.operator_call) - 1] = 0;
  }

  if (config.net_retry_max_ms < config.net_retry_min_ms)
    config.net_retry_max_ms = config.net_retry_min_ms;

  fclose(f);

  return 0;
}

/*
 * Save configuration values to a logger.conf-style file.
 *
 * @param filename Path to the configuration file.
 * @return 0 on success, or -1 if the file cannot be opened for writing.
 */
int config_save(const char *filename) {
  if (!filename || !filename[0])
    return -1;

  FILE *f = fopen(filename, "w");
  if (!f)
    return -1;

  fprintf(f, "LAT=%.6f\n", config.lat);
  fprintf(f, "LON=%.6f\n", config.lon);
  fprintf(f, "LOCATOR=%s\n", config.locator);
  fprintf(f, "\n");
  fprintf(f, "DXC_HOST=%s\n", config.dxc_host);
  fprintf(f, "DXC_PORT=%d\n", config.dxc_port);
  fprintf(f, "DXC_CALL=%s\n", config.dxc_call);
  fprintf(f, "\n");
  fprintf(f, "CAT_MODEL=%d\n", config.cat_model);
  fprintf(f, "CAT_DEVICE=%s\n", config.cat_device);
  fprintf(f, "CAT_BAUD=%d\n", config.cat_baud);
  fprintf(f, "CAT_DATA_BITS=%d\n", config.cat_data_bits);
  fprintf(f, "CAT_STOP_BITS=%d\n", config.cat_stop_bits);
  fprintf(f, "CAT_PARITY=%s\n", config.cat_parity);
  fprintf(f, "CAT_HANDSHAKE=%s\n", config.cat_handshake);
  fprintf(f, "CAT_MODE_FROM_RIG=%d\n", config.cat_mode_from_rig ? 1 : 0);
  fprintf(f, "CAT_AUTO_CONNECT=%d\n", config.cat_auto_connect ? 1 : 0);
  fprintf(f, "CAT2_MODEL=%d\n", config.cat2_model);
  fprintf(f, "CAT2_DEVICE=%s\n", config.cat2_device);
  fprintf(f, "CAT2_BAUD=%d\n", config.cat2_baud);
  fprintf(f, "CAT2_DATA_BITS=%d\n", config.cat2_data_bits);
  fprintf(f, "CAT2_STOP_BITS=%d\n", config.cat2_stop_bits);
  fprintf(f, "CAT2_PARITY=%s\n", config.cat2_parity);
  fprintf(f, "CAT2_HANDSHAKE=%s\n", config.cat2_handshake);
  fprintf(f, "\n");
  fprintf(f, "STATION_CALL=%s\n", config.station_call);
  fprintf(f, "OPERATOR_CALL=%s\n", config.operator_call);
  fprintf(f, "OPERATOR_NAME=%s\n", config.operator_name);
  fprintf(f, "CONTEST_DEF_FILE=%s\n", config.contest_definition_path);
  fprintf(f, "CONTEST_TX_EXCHANGE=%s\n", config.contest_tx_exchange);
  fprintf(f, "CONTEST_TECHNIQUE=%s\n",
          contest_technique_to_text(config.contest_technique));
  fprintf(f, "\n");
  fprintf(f, "CW_DEVICE=%s\n", config.cw_device);
  fprintf(f, "CW_KEYER_LINE=%s\n", config.cw_keyer_line);
  fprintf(f, "CW_WPM=%d\n", config.cw_wpm);
  fprintf(f, "CW_AUTO_CONNECT=%d\n", config.cw_auto_connect ? 1 : 0);
  fprintf(f, "\n");
  fprintf(f, "# Network Basic\n");
  fprintf(f, "NET_ENABLED=%d\n", config.net_enabled ? 1 : 0);
  fprintf(f, "NET_ROLE=%s\n", config.net_role);
  fprintf(f, "NET_STATION_ID=%s\n", config.net_station_id);
  fprintf(f, "NET_SERVER_HOST=%s\n", config.net_server_host);
  fprintf(f, "NET_SERVER_PORT=%d\n", config.net_server_port);
  fprintf(f, "\n");
  fprintf(f, "# Network Security\n");
  fprintf(f, "NET_AUTH_TOKEN=%s\n", config.net_auth_token);
  fprintf(f, "NET_SHARED_KEY=%s\n", config.net_shared_key);
  fprintf(f, "NET_TLS_CERT_FILE=%s\n", config.net_tls_cert_file);
  fprintf(f, "NET_TLS_KEY_FILE=%s\n", config.net_tls_key_file);
  fprintf(f, "NET_TLS_PEER_FINGERPRINT=%s\n", config.net_tls_peer_fingerprint);
  fprintf(f, "NET_TLS=%d\n", config.net_tls ? 1 : 0);
  fprintf(f, "\n");
  fprintf(f, "# Network Runtime\n");
  fprintf(f, "NET_SYNC_INTERVAL_MS=%d\n", config.net_sync_interval_ms);
  fprintf(f, "NET_HEARTBEAT_SEC=%d\n", config.net_heartbeat_sec);
  fprintf(f, "NET_RETRY_MIN_MS=%d\n", config.net_retry_min_ms);
  fprintf(f, "NET_RETRY_MAX_MS=%d\n", config.net_retry_max_ms);
  fprintf(f, "NET_RATE_LIMIT_WINDOW_SEC=%d\n", config.net_rate_limit_window_sec);
  fprintf(f, "NET_RATE_LIMIT_BURST=%d\n", config.net_rate_limit_burst);
  fprintf(f, "NET_MAX_FRAME_BYTES=%d\n", config.net_max_frame_bytes);
  fprintf(f, "\n");

  fclose(f);
  return 0;
}

const char *config_loaded_path(void) {
  return config_last_loaded_path[0] ? config_last_loaded_path : NULL;
}

const char *config_effective_operator_call(void) {
  if (config.operator_call[0])
    return config.operator_call;
  return config.station_call[0] ? config.station_call : "N0CALL";
}

int config_save_active(void) {
  if (config_last_loaded_path[0])
    return config_save(config_last_loaded_path);
  return config_save("logger.conf");
}
