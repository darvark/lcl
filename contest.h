#ifndef CONTEST_H
#define CONTEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONTEST_DEF_MAX_FIELDS 16

typedef enum {
  CONTEST_TECH_SO1R = 0,
  CONTEST_TECH_SO2V = 1,
  CONTEST_TECH_SO2R = 2
} ContestTechnique;

typedef enum {
  CONTEST_MULT_NONE = 0,
  CONTEST_MULT_DXCC = 1,
  CONTEST_MULT_BAND_DXCC = 2,
  CONTEST_MULT_MODE_DXCC = 3
} ContestMultiplierType;

typedef struct {
  char name[32];
  char label[64];
  int required;
} ContestFieldDef;

typedef struct {
  char name[64];
  char cabrillo_name[64];
  char mode[16];
  char category_operator[16];
  char category_band[16];
  char category_power[16];
  char category_overlay[16];
  char station_location[32];
  char operators[32];
  char exchange_sent_template[64];
  int points_per_qso;
  int points_cw;
  int points_phone;
  int points_digi;
  int points_new_dxcc;
  int points_same_dxcc;
  int points_new_band_dxcc;
  int points_same_band_dxcc;
  ContestMultiplierType multiplier_type;
  int bonus_points;

  ContestFieldDef fields[CONTEST_DEF_MAX_FIELDS];
  int field_count;
} ContestDefinition;

/*
 * Initialize one contest definition with practical defaults.
 */
void contest_definition_init_defaults(ContestDefinition *out);

/*
 * Load a DXLog-like key/value contest definition from disk.
 * Supported examples:
 * NAME=CQ-WW-CW
 * CABRILLO_NAME=CQ-WW-CW
 * EXCHANGE_SENT=#
 * FIELD=SERIAL,Serial Number,required
 */
int contest_definition_load(const char *path, ContestDefinition *out,
                           char *error_text, size_t error_size);

/*
 * Parse a textual operating technique (SO1R, SO2V, SO2R).
 */
ContestTechnique contest_technique_from_text(const char *text);

/*
 * Convert an operating technique enum to text.
 */
const char *contest_technique_to_text(ContestTechnique technique);

/*
 * Parse one textual multiplier mode.
 */
ContestMultiplierType contest_multiplier_from_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif
