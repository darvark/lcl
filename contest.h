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
  CONTEST_MULT_DXCC_PER_BAND = 2,
  CONTEST_MULT_ZONE_PER_BAND = 3,
  CONTEST_MULT_ZONE = 4,
  CONTEST_MULT_PREFIX = 5,
  CONTEST_MULT_PREFIX_PER_BAND = 6,
  CONTEST_MULT_MODE_DXCC = 7,
  CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND = 8,
  CONTEST_MULT_SPDX = 9,
  CONTEST_MULT_BAND_DXCC = CONTEST_MULT_DXCC_PER_BAND,
  CONTEST_MULT_ZONE_BAND = CONTEST_MULT_ZONE_PER_BAND
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

/*
 * Import a raw DXLog contest file and write a normalized local contest
 * definition file that this logger can load directly.
 *
 * @param source_path Path to raw DXLog .txt file.
 * @param dest_path Output normalized config path (for example contest.conf).
 * @param error_text Optional output buffer for error text.
 * @param error_size Size of error_text buffer.
 * @param warning_text Optional output buffer for import warnings.
 * @param warning_size Size of warning_text buffer.
 * @return 0 on success, or -1 on failure.
 */
int contest_definition_import_dxlog(const char *source_path,
                                    const char *dest_path,
                                    char *error_text,
                                    size_t error_size,
                                    char *warning_text,
                                    size_t warning_size);

#ifdef __cplusplus
}
#endif

#endif
