#include "config.h"

Config config;

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

  config.cat2_model = 2;
  strcpy(config.cat2_device, "/dev/ttyUSB1");
  config.cat2_baud = 9600;
  config.cat2_data_bits = 8;
  config.cat2_stop_bits = 1;
  strcpy(config.cat2_parity, "None");
  strcpy(config.cat2_handshake, "None");

  strcpy(config.station_call, "N0CALL");
  strcpy(config.operator_name, "");
  strcpy(config.contest_definition_path, "contest.conf");
  strcpy(config.contest_tx_exchange, "");
  config.contest_technique = CONTEST_TECH_SO1R;

  strcpy(config.cw_device, "/dev/ttyUSB2");
  strcpy(config.cw_keyer_line, "DTR");
  config.cw_wpm = 20;
}

/*
 * Load configuration values from a logger.conf-style file.
 *
 * @param filename Path to the configuration file.
 * @return 0 on success, or -1 if the file cannot be opened.
 */
int config_load(const char *filename) {
  set_defaults();

  FILE *f = fopen(filename, "r");

  if (!f)
    return -1;

  char line[256];

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
      if (config.cw_wpm < 5) config.cw_wpm = 5;
      if (config.cw_wpm > 60) config.cw_wpm = 60;
    }
  }

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
  fprintf(f, "CAT2_MODEL=%d\n", config.cat2_model);
  fprintf(f, "CAT2_DEVICE=%s\n", config.cat2_device);
  fprintf(f, "CAT2_BAUD=%d\n", config.cat2_baud);
  fprintf(f, "CAT2_DATA_BITS=%d\n", config.cat2_data_bits);
  fprintf(f, "CAT2_STOP_BITS=%d\n", config.cat2_stop_bits);
  fprintf(f, "CAT2_PARITY=%s\n", config.cat2_parity);
  fprintf(f, "CAT2_HANDSHAKE=%s\n", config.cat2_handshake);
  fprintf(f, "\n");
  fprintf(f, "STATION_CALL=%s\n", config.station_call);
  fprintf(f, "OPERATOR_NAME=%s\n", config.operator_name);
  fprintf(f, "CONTEST_DEF_FILE=%s\n", config.contest_definition_path);
  fprintf(f, "CONTEST_TX_EXCHANGE=%s\n", config.contest_tx_exchange);
  fprintf(f, "CONTEST_TECHNIQUE=%s\n",
          contest_technique_to_text(config.contest_technique));
  fprintf(f, "\n");
  fprintf(f, "CW_DEVICE=%s\n", config.cw_device);
  fprintf(f, "CW_KEYER_LINE=%s\n", config.cw_keyer_line);
  fprintf(f, "CW_WPM=%d\n", config.cw_wpm);

  fclose(f);
  return 0;
}
