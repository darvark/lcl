#ifndef CONTEST_H
#define CONTEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONTEST_DEF_MAX_FIELDS 16
#define CONTEST_DEF_MAX_SCORE_RULES 64
#define CONTEST_DEF_MAX_REGIONS 16
#define CONTEST_DEF_MAX_EXCLUDED_SEGMENTS 32
#define CONTEST_DEF_MAX_MULTIPLIERS 4

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
  CONTEST_MULT_GRID_PER_BAND = 10,
  CONTEST_MULT_CUSTOM_LIST = 11,
  CONTEST_MULT_WAG = 12,
  CONTEST_MULT_IARU = 13,
  CONTEST_MULT_CQWW = 14,
  CONTEST_MULT_BAND_DXCC = CONTEST_MULT_DXCC_PER_BAND,
  CONTEST_MULT_ZONE_BAND = CONTEST_MULT_ZONE_PER_BAND
} ContestMultiplierType;

typedef struct {
  char name[32];
  char label[64];
  int required;
} ContestFieldDef;

typedef struct {
  int points;
  char conditions[192];
} ContestScoreRule;

typedef struct {
  char name[24];
  char continent[8];
  char countries[128];
  char prefixes[128];
  char exchanges[128];
} ContestRegionDef;

typedef struct {
  char mode[8];
  int low_khz;
  int high_khz;
} ContestExcludedSegment;

typedef struct {
  const char *call;
  const char *mode;
  const char *band;
  const char *source_country;
  const char *source_continent;
  const char *source_prefix;
  const char *destination_country;
  const char *destination_continent;
  const char *destination_prefix;
  const char *exchange_received;
  const char *station_exchange;
  int same_itu_zone;
  int new_dxcc;
  int new_band_dxcc;
  int distance_km;
} ContestScoreContext;

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
  char exchange_received_type[32];
  char station_exchange_region[24];
  char serial_locator_separator[8];
  char start_utc[24];
  char end_utc[24];
  int enforce_time_window;
  int distance_scoring;
  int serial_width;
  int duplicate_mode_sensitive;
  int validate_operating_rules;
  char allowed_modes[96];
  char allowed_bands[128];
  int max_power_watts;
  ContestExcludedSegment excluded_segments[CONTEST_DEF_MAX_EXCLUDED_SEGMENTS];
  int excluded_segment_count;
  ContestScoreRule score_rules[CONTEST_DEF_MAX_SCORE_RULES];
  int score_rule_count;
  ContestRegionDef regions[CONTEST_DEF_MAX_REGIONS];
  int region_count;
  int points_per_qso;
  int points_cw;
  int points_phone;
  int points_digi;
  int points_new_dxcc;
  int points_same_dxcc;
  int points_new_band_dxcc;
  int points_same_band_dxcc;
  int points_configured;
  ContestMultiplierType multiplier_type;
  ContestMultiplierType multipliers[CONTEST_DEF_MAX_MULTIPLIERS];
  int multiplier_count;
  char multiplier_excluded_suffixes[CONTEST_DEF_MAX_MULTIPLIERS][64];
  char multiplier_special_prefixes[CONTEST_DEF_MAX_MULTIPLIERS][96];
  char custom_mult_list[256];
  char mult3_type[32];
  char mult3_field[32];
  char section_name[32];
  char area_name[32];
  char pfx_area[32];
  int bonus_points;

  /*
   * QTC traffic exchange configuration (WAE and similar contests).
   *
   * qtc_sender_side identifies which geographic side originates QTC traffic:
   *   "NONE"  – QTC not supported (default)
   *   "EU"    – European stations send QTC to DX
   *   "DX"    – DX stations send QTC to EU
   *   "BOTH"  – either side may send QTC
   *
   * points_per_qtc is added to the total score for each individual QTC
   * record successfully exchanged (default 1 when QTC is enabled).
   */
  char qtc_sender_side[8];
  int  points_per_qtc;
  int  duplicate_qso;

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

int contest_definition_score_qso(const ContestDefinition *definition,
                                 const ContestScoreContext *context,
                                 int *out_points);

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
