#ifndef CONFIG_H
#define CONFIG_H

#include "contest.h"
#include "globals.h"

typedef struct {
  double lat;
  double lon;

  char locator[16];

  char dxc_host[128];
  int dxc_port;
  char dxc_call[32];

  int cat_model;
  char cat_device[128];
  int cat_baud;
  int cat_data_bits;
  int cat_stop_bits;
  char cat_parity[16];
  char cat_handshake[16];
  int cat_mode_from_rig;
  int cat_auto_connect;

  int cat2_model;
  char cat2_device[128];
  int cat2_baud;
  int cat2_data_bits;
  int cat2_stop_bits;
  char cat2_parity[16];
  char cat2_handshake[16];

  char station_call[32];
  char operator_name[64];
  char contest_definition_path[256];
  char contest_tx_exchange[32];
  ContestTechnique contest_technique;

  char cw_device[128];
  char cw_keyer_line[8];  /* "DTR" or "RTS" */
  int cw_wpm;
  int cw_auto_connect;

} Config;

extern Config config;

/*
 * Load the logger configuration file into the global config structure.
 *
 * @param filename Path to the configuration file to read.
 * @return 0 on success, or -1 if the file cannot be opened.
 */
int config_load(const char *filename);

/*
 * Persist the current global config structure to a logger.conf-style file.
 *
 * @param filename Path to the destination configuration file.
 * @return 0 on success, or -1 if the file cannot be written.
 */
int config_save(const char *filename);

#endif
