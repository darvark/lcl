#include "contest.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_in_place(char *text) {
  if (!text)
    return;

  size_t start = 0;
  while (text[start] && isspace((unsigned char)text[start]))
    start++;

  if (start > 0)
    memmove(text, text + start, strlen(text + start) + 1);

  size_t len = strlen(text);
  while (len > 0 && isspace((unsigned char)text[len - 1])) {
    text[len - 1] = 0;
    len--;
  }
}

static void uppercase_in_place(char *text) {
  if (!text)
    return;

  for (size_t i = 0; text[i]; i++)
    text[i] = (char)toupper((unsigned char)text[i]);
}

static int parse_truthy_flag(const char *value) {
  if (!value || !value[0])
    return 0;

  char upper[32] = {0};
  snprintf(upper, sizeof(upper), "%s", value);
  uppercase_in_place(upper);

  return strcmp(upper, "1") == 0 || strcmp(upper, "ON") == 0 ||
         strcmp(upper, "YES") == 0 || strcmp(upper, "TRUE") == 0 ||
         strcmp(upper, "Y") == 0 || strcmp(upper, "ENABLE") == 0 ||
         strcmp(upper, "ENABLED") == 0;
}

static void parse_score_rule(const char *value, ContestDefinition *out) {
  if (!value || !out || out->score_rule_count >= CONTEST_DEF_MAX_SCORE_RULES)
    return;

  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%s", value);
  char *points_text = strtok(buffer, ";");
  if (!points_text)
    return;

  trim_in_place(points_text);
  char *end = NULL;
  const long points = strtol(points_text, &end, 10);
  if (end == points_text || *end != 0 || points < 0 || points > 1000000)
    return;

  ContestScoreRule *rule = &out->score_rules[out->score_rule_count++];
  rule->points = (int)points;
  size_t used = 0;
  for (char *condition = strtok(NULL, ";"); condition;
       condition = strtok(NULL, ";")) {
    trim_in_place(condition);
    if (!condition[0] || !strchr(condition, '='))
      continue;
    const int written = snprintf(rule->conditions + used,
                                 sizeof(rule->conditions) - used,
                                 "%s%s", used ? ";" : "", condition);
    if (written < 0 || (size_t)written >= sizeof(rule->conditions) - used)
      break;
    used += (size_t)written;
  }
  out->points_configured = 1;
}

static void parse_region(const char *value, ContestDefinition *out) {
  if (!value || !out || out->region_count >= CONTEST_DEF_MAX_REGIONS)
    return;

  char buffer[384];
  snprintf(buffer, sizeof(buffer), "%s", value);
  char *save = NULL;
  char *name = strtok_r(buffer, ";", &save);
  if (!name)
    return;
  trim_in_place(name);
  if (!name[0])
    return;

  ContestRegionDef *region = &out->regions[out->region_count];
  memset(region, 0, sizeof(*region));
  snprintf(region->name, sizeof(region->name), "%s", name);
  uppercase_in_place(region->name);

  for (char *attribute = strtok_r(NULL, ";", &save); attribute;
       attribute = strtok_r(NULL, ";", &save)) {
    trim_in_place(attribute);
    char *separator = strchr(attribute, '=');
    if (!separator)
      continue;
    *separator++ = 0;
    trim_in_place(attribute);
    trim_in_place(separator);
    uppercase_in_place(attribute);
    if (strcmp(attribute, "CONTINENT") == 0) {
      snprintf(region->continent, sizeof(region->continent), "%s", separator);
      uppercase_in_place(region->continent);
    } else if (strcmp(attribute, "COUNTRIES") == 0) {
      snprintf(region->countries, sizeof(region->countries), "%s", separator);
      uppercase_in_place(region->countries);
    } else if (strcmp(attribute, "PREFIXES") == 0) {
      snprintf(region->prefixes, sizeof(region->prefixes), "%s", separator);
      uppercase_in_place(region->prefixes);
    } else if (strcmp(attribute, "EXCHANGES") == 0) {
      snprintf(region->exchanges, sizeof(region->exchanges), "%s", separator);
      uppercase_in_place(region->exchanges);
    }
  }
  out->region_count++;
}

static void parse_excluded_segment(const char *value, ContestDefinition *out) {
  if (!value || !out ||
      out->excluded_segment_count >= CONTEST_DEF_MAX_EXCLUDED_SEGMENTS)
    return;
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%s", value);
  char *save = NULL;
  char *mode = strtok_r(buffer, ",", &save);
  char *low = strtok_r(NULL, ",", &save);
  char *high = strtok_r(NULL, ",", &save);
  if (!mode || !low || !high)
    return;
  const int low_khz = atoi(low);
  const int high_khz = atoi(high);
  if (low_khz <= 0 || high_khz < low_khz)
    return;
  ContestExcludedSegment *segment =
      &out->excluded_segments[out->excluded_segment_count++];
  snprintf(segment->mode, sizeof(segment->mode), "%s", mode);
  uppercase_in_place(segment->mode);
  segment->low_khz = low_khz;
  segment->high_khz = high_khz;
}

static void set_multiplier_list(ContestDefinition *out, const char *value) {
  if (!out || !value)
    return;
  out->multiplier_count = 0;
  char copy[128];
  snprintf(copy, sizeof(copy), "%s", value);
  char *save = NULL;
  for (char *item = strtok_r(copy, ",", &save); item;
       item = strtok_r(NULL, ",", &save)) {
    trim_in_place(item);
    if (!item[0] || out->multiplier_count >= CONTEST_DEF_MAX_MULTIPLIERS)
      continue;
    out->multipliers[out->multiplier_count++] = contest_multiplier_from_text(item);
  }
  if (out->multiplier_count == 0) {
    out->multipliers[0] = contest_multiplier_from_text(value);
    out->multiplier_count = 1;
  }
  out->multiplier_type = out->multipliers[0];
}

static void set_error(char *error_text, size_t error_size, const char *text) {
  if (!error_text || error_size < 2)
    return;

  if (!text)
    text = "Unknown contest definition error";

  snprintf(error_text, error_size, "%s", text);
}

static int str_contains_upper(const char *haystack, const char *needle) {
  if (!haystack || !needle || !haystack[0] || !needle[0])
    return 0;

  char up_h[128];
  char up_n[64];
  snprintf(up_h, sizeof(up_h), "%s", haystack);
  snprintf(up_n, sizeof(up_n), "%s", needle);
  uppercase_in_place(up_h);
  uppercase_in_place(up_n);
  return strstr(up_h, up_n) != NULL;
}

static void first_token(const char *src, char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  out[0] = 0;
  if (!src || !src[0])
    return;

  size_t i = 0;
  while (src[i] && src[i] != ';' && src[i] != ',' && i < out_size - 1) {
    out[i] = src[i];
    i++;
  }
  out[i] = 0;
  trim_in_place(out);
}

static ContestMultiplierType map_dxlog_multiplier(const char *type,
                                                  const char *count) {
  char up_type[32] = {0};
  char up_count[32] = {0};
  snprintf(up_type, sizeof(up_type), "%s", type ? type : "");
  snprintf(up_count, sizeof(up_count), "%s", count ? count : "");
  uppercase_in_place(up_type);
  uppercase_in_place(up_count);

  const int per_band = strcmp(up_count, "PER_BAND") == 0;

  if (strcmp(up_type, "WPX") == 0)
    return per_band ? CONTEST_MULT_PREFIX_PER_BAND : CONTEST_MULT_PREFIX;
  if (strcmp(up_type, "DXCC") == 0)
    return per_band ? CONTEST_MULT_DXCC_PER_BAND : CONTEST_MULT_DXCC;
  if (strcmp(up_type, "CQZONE") == 0 || strcmp(up_type, "ZONE") == 0)
    return per_band ? CONTEST_MULT_ZONE_PER_BAND : CONTEST_MULT_ZONE;
  if (strcmp(up_type, "WWL") == 0 || strcmp(up_type, "GRID") == 0 ||
      strcmp(up_type, "LOCATOR") == 0)
    return CONTEST_MULT_GRID_PER_BAND;
  if (strcmp(up_type, "CUSTOM") == 0 || strcmp(up_type, "CUSTOM_LIST") == 0 ||
      strcmp(up_type, "SECTION") == 0 || strcmp(up_type, "AREA") == 0 ||
      strcmp(up_type, "PFX_AREA") == 0 || strcmp(up_type, "MULT3") == 0)
    return CONTEST_MULT_CUSTOM_LIST;

  return CONTEST_MULT_DXCC;
}

static int text_contains_token_ci(const char *text, const char *token) {
  if (!text || !text[0] || !token || !token[0])
    return 0;

  char up_text[192] = {0};
  char up_token[48] = {0};
  snprintf(up_text, sizeof(up_text), "%s", text);
  snprintf(up_token, sizeof(up_token), "%s", token);
  uppercase_in_place(up_text);
  uppercase_in_place(up_token);

  return strstr(up_text, up_token) != NULL;
}

static void apply_dxlog_received_field_type(const char *value,
                                            ContestDefinition *out) {
  if (!value || !value[0] || !out)
    return;

  out->field_count = 0;

  if ((text_contains_token_ci(value, "NR") ||
       text_contains_token_ci(value, "SERIAL") ||
       text_contains_token_ci(value, "QSONR") ||
       text_contains_token_ci(value, "#")) &&
      (text_contains_token_ci(value, "GRID") ||
       text_contains_token_ci(value, "LOC") ||
       text_contains_token_ci(value, "LOCATOR"))) {
    snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s",
             "SERIAL_GRID");
    snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
             "Serial + Grid");
    out->fields[0].required = 1;
    out->field_count = 1;
    snprintf(out->exchange_received_type,
         sizeof(out->exchange_received_type), "%s", "SERIAL_LOCATOR");
    return;
  }

  if (text_contains_token_ci(value, "NR") ||
      text_contains_token_ci(value, "SERIAL") ||
      text_contains_token_ci(value, "QSONR")) {
    snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s",
             "SERIAL");
    snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
             "Serial");
    out->fields[0].required = 1;
    out->field_count = 1;
    snprintf(out->exchange_received_type,
         sizeof(out->exchange_received_type), "%s", "SERIAL");
    return;
  }

  if (text_contains_token_ci(value, "CQZONE")) {
    snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s",
             "CQZONE");
    snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
             "CQ Zone");
    out->fields[0].required = 1;
    out->field_count = 1;
    snprintf(out->exchange_received_type,
         sizeof(out->exchange_received_type), "%s", "CQ_ZONE");
    return;
  }

  if (text_contains_token_ci(value, "ITUZONE") ||
      text_contains_token_ci(value, "ITU")) {
    snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s",
             "ITUZONE");
    snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
             "ITU Zone");
    out->fields[0].required = 1;
    out->field_count = 1;
    snprintf(out->exchange_received_type,
         sizeof(out->exchange_received_type), "%s", "ITU_ZONE_OR_HQ");
    return;
  }

  if (text_contains_token_ci(value, "GRID") ||
      text_contains_token_ci(value, "LOC")) {
    snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "GRID");
    snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
             "Grid");
    out->fields[0].required = 1;
    out->field_count = 1;
    snprintf(out->exchange_received_type,
             sizeof(out->exchange_received_type), "%s", "LOCATOR");
  }
}

static int is_dxlog_key_potentially_ignored(const char *key_upper) {
  if (!key_upper || !key_upper[0])
    return 0;

  return strcmp(key_upper, "POINTS_TYPE") == 0 ||
         strcmp(key_upper, "POINTS_FIELD_BAND_MODE") == 0 ||
         strcmp(key_upper, "POINTS_FIELD") == 0 ||
         strcmp(key_upper, "SCORE") == 0 ||
         strcmp(key_upper, "SCORE_DISPLAY") == 0 ||
         strcmp(key_upper, "SCORE_TOTAL_FX") == 0 ||
         strncmp(key_upper, "MULT", 4) == 0 ||
         strncmp(key_upper, "CFG_", 4) == 0 ||
         strncmp(key_upper, "WINDOWS_", 8) == 0 ||
         strncmp(key_upper, "CW_MESSAGE", 10) == 0 ||
         strcmp(key_upper, "DOUBLE_QSO") == 0 ||
         strcmp(key_upper, "ADIF_KEYS") == 0 ||
         strcmp(key_upper, "CABRILLO_LINE") == 0 ||
         strcmp(key_upper, "QSO_NUMBER_CATEGORY") == 0;
}

static int is_dxlog_key_supported_for_import(const char *key_upper) {
  if (!key_upper || !key_upper[0])
    return 0;

  return strcmp(key_upper, "NAME") == 0 ||
         strcmp(key_upper, "CONTESTNAME") == 0 ||
         strcmp(key_upper, "CABRILLO_NAME") == 0 ||
         strcmp(key_upper, "CABRILLO_CONTEST_NAME") == 0 ||
         strcmp(key_upper, "CABRILLO-CONTEST") == 0 ||
         strcmp(key_upper, "MODE") == 0 ||
         strcmp(key_upper, "MODES") == 0 ||
         strcmp(key_upper, "CATEGORY_OPERATOR") == 0 ||
         strcmp(key_upper, "CATEGORY_BAND") == 0 ||
         strcmp(key_upper, "CATEGORY_POWER") == 0 ||
         strcmp(key_upper, "CATEGORY_OVERLAY") == 0 ||
         strcmp(key_upper, "STATION_LOCATION") == 0 ||
         strcmp(key_upper, "OPERATORS") == 0 ||
         strcmp(key_upper, "EXCHANGE_SENT") == 0 ||
         strcmp(key_upper, "POINTS_PER_QSO") == 0 ||
         strcmp(key_upper, "POINTS_CW") == 0 ||
         strcmp(key_upper, "POINTS_PHONE") == 0 ||
         strcmp(key_upper, "POINTS_DIGI") == 0 ||
         strcmp(key_upper, "POINTS_NEW_DXCC") == 0 ||
         strcmp(key_upper, "POINTS_SAME_DXCC") == 0 ||
         strcmp(key_upper, "POINTS_NEW_BAND_DXCC") == 0 ||
         strcmp(key_upper, "POINTS_SAME_BAND_DXCC") == 0 ||
         strcmp(key_upper, "MULTIPLIER") == 0 ||
         strcmp(key_upper, "MULT1_TYPE") == 0 ||
         strcmp(key_upper, "MULT1_COUNT") == 0 ||
         strcmp(key_upper, "MULT2_TYPE") == 0 ||
         strcmp(key_upper, "MULT2_COUNT") == 0 ||
         strcmp(key_upper, "MULT3_TYPE") == 0 ||
         strcmp(key_upper, "MULT3_COUNT") == 0 ||
         strcmp(key_upper, "MULT3_FIELD") == 0 ||
         strcmp(key_upper, "CUSTOM_MULT_LIST") == 0 ||
         strcmp(key_upper, "SECTION") == 0 ||
         strcmp(key_upper, "AREA") == 0 ||
         strcmp(key_upper, "PFX_AREA") == 0 ||
         strcmp(key_upper, "FIELD") == 0 ||
         strcmp(key_upper, "FIELD_RCVD_TYPE") == 0 ||
         strcmp(key_upper, "BONUS_POINTS") == 0 ||
         strcmp(key_upper, "QTC_SENDER") == 0 ||
         strcmp(key_upper, "POINTS_PER_QTC") == 0;
}

static void build_dxlog_import_warnings(const char *source_path,
                                        char *warning_text,
                                        size_t warning_size) {
  if (!warning_text || warning_size < 2)
    return;

  warning_text[0] = 0;
  if (!source_path || !source_path[0])
    return;

  FILE *f = fopen(source_path, "r");
  if (!f)
    return;

  char ignored_keys[16][40];
  int ignored_count = 0;
  int ignored_total = 0;
  char line[256];

  while (fgets(line, sizeof(line), f)) {
    trim_in_place(line);
    if (!line[0] || line[0] == '#')
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    *eq = 0;
    trim_in_place(line);
    uppercase_in_place(line);

    if (!line[0] || is_dxlog_key_supported_for_import(line))
      continue;

    if (!is_dxlog_key_potentially_ignored(line))
      continue;

    ignored_total++;

    int exists = 0;
    for (int i = 0; i < ignored_count; i++) {
      if (strcmp(ignored_keys[i], line) == 0) {
        exists = 1;
        break;
      }
    }

    if (!exists && ignored_count < (int)(sizeof(ignored_keys) / sizeof(ignored_keys[0]))) {
      strncpy(ignored_keys[ignored_count], line,
              sizeof(ignored_keys[ignored_count]) - 1);
      ignored_keys[ignored_count][sizeof(ignored_keys[ignored_count]) - 1] = 0;
      ignored_count++;
    }
  }

  fclose(f);

  if (ignored_count <= 0)
    return;

  size_t used = 0;
  int written = snprintf(warning_text, warning_size,
                         "Ignored DXLog rules: ");
  if (written < 0)
    return;
  used = (size_t)written;

  for (int i = 0; i < ignored_count; i++) {
    if (used + 4 >= warning_size)
      break;
    written = snprintf(warning_text + used, warning_size - used,
                       "%s%s", ignored_keys[i],
                       (i + 1 < ignored_count) ? ", " : "");
    if (written < 0)
      break;
    used += (size_t)written;
  }

  if (ignored_total > ignored_count && used + 20 < warning_size) {
    snprintf(warning_text + used, warning_size - used,
             " (+%d more)", ignored_total - ignored_count);
  }
}

void contest_definition_init_defaults(ContestDefinition *out) {
  if (!out)
    return;

  memset(out, 0, sizeof(*out));

  snprintf(out->name, sizeof(out->name), "%s", "GENERAL");
  snprintf(out->cabrillo_name, sizeof(out->cabrillo_name), "%s", "GENERAL");
  snprintf(out->mode, sizeof(out->mode), "%s", "MIXED");
  snprintf(out->category_operator, sizeof(out->category_operator), "%s",
           "SINGLE-OP");
  snprintf(out->category_band, sizeof(out->category_band), "%s", "ALL");
  snprintf(out->category_power, sizeof(out->category_power), "%s", "LOW");
  snprintf(out->category_overlay, sizeof(out->category_overlay), "%s", "");
  snprintf(out->station_location, sizeof(out->station_location), "%s", "DX");
  snprintf(out->operators, sizeof(out->operators), "%s", "");
  snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template),
           "%s", "#");
  out->exchange_received_type[0] = 0;
  out->station_exchange_region[0] = 0;
  out->serial_locator_separator[0] = 0;
  out->start_utc[0] = 0;
  out->end_utc[0] = 0;
  out->enforce_time_window = 0;
  out->distance_scoring = 0;
  out->serial_width = 0;
  out->duplicate_mode_sensitive = 1;
  out->points_per_qso = 1;
  out->points_cw = 0;
  out->points_phone = 0;
  out->points_digi = 0;
  out->points_new_dxcc = 0;
  out->points_same_dxcc = 0;
  out->points_new_band_dxcc = 0;
  out->points_same_band_dxcc = 0;
  out->points_configured = 0;
  out->multiplier_type = CONTEST_MULT_DXCC;
  out->multipliers[0] = CONTEST_MULT_DXCC;
  out->multiplier_count = 1;
  out->custom_mult_list[0] = 0;
  out->mult3_type[0] = 0;
  out->mult3_field[0] = 0;
  out->section_name[0] = 0;
  out->area_name[0] = 0;
  out->pfx_area[0] = 0;
  out->bonus_points = 0;
  snprintf(out->qtc_sender_side, sizeof(out->qtc_sender_side), "%s", "NONE");
  out->points_per_qtc = 0;
  out->duplicate_qso = 0;
  out->field_count = 0;
}

ContestTechnique contest_technique_from_text(const char *text) {
  if (!text || !text[0])
    return CONTEST_TECH_SO1R;

  char upper[16];
  snprintf(upper, sizeof(upper), "%s", text);
  uppercase_in_place(upper);

  if (strcmp(upper, "SO2R") == 0)
    return CONTEST_TECH_SO2R;

  if (strcmp(upper, "SO2V") == 0)
    return CONTEST_TECH_SO2V;

  return CONTEST_TECH_SO1R;
}

const char *contest_technique_to_text(ContestTechnique technique) {
  switch (technique) {
  case CONTEST_TECH_SO2R:
    return "SO2R";
  case CONTEST_TECH_SO2V:
    return "SO2V";
  case CONTEST_TECH_SO1R:
  default:
    return "SO1R";
  }
}

ContestMultiplierType contest_multiplier_from_text(const char *text) {
  if (!text || !text[0])
    return CONTEST_MULT_DXCC;

  char upper[32];
  snprintf(upper, sizeof(upper), "%s", text);
  uppercase_in_place(upper);

  if (strcmp(upper, "NONE") == 0)
    return CONTEST_MULT_NONE;
  if (strcmp(upper, "DXCC") == 0)
    return CONTEST_MULT_DXCC;
  if (strcmp(upper, "DXCC_PER_BAND") == 0 ||
      strcmp(upper, "DXCC-PER-BAND") == 0)
    return CONTEST_MULT_DXCC_PER_BAND;
  if (strcmp(upper, "ZONE_PER_BAND") == 0 ||
      strcmp(upper, "ZONE-PER-BAND") == 0)
    return CONTEST_MULT_ZONE_PER_BAND;
  if (strcmp(upper, "ZONE") == 0)
    return CONTEST_MULT_ZONE;
  if (strcmp(upper, "PREFIX") == 0)
    return CONTEST_MULT_PREFIX;
  if (strcmp(upper, "PREFIX_PER_BAND") == 0 ||
      strcmp(upper, "PREFIX-PER-BAND") == 0)
    return CONTEST_MULT_PREFIX_PER_BAND;
  if (strcmp(upper, "DXCC_PLUS_ZONE_PER_BAND") == 0 ||
      strcmp(upper, "DXCC+ZONE_PER_BAND") == 0)
    return CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND;
  if (strcmp(upper, "SPDX") == 0)
    return CONTEST_MULT_SPDX;
  if (strcmp(upper, "WAG") == 0)
    return CONTEST_MULT_WAG;
  if (strcmp(upper, "IARU") == 0)
    return CONTEST_MULT_IARU;
  if (strcmp(upper, "CQWW") == 0)
    return CONTEST_MULT_CQWW;
  if (strcmp(upper, "GRID_PER_BAND") == 0 ||
      strcmp(upper, "GRID-PER-BAND") == 0 ||
      strcmp(upper, "WWL_PER_BAND") == 0 ||
      strcmp(upper, "LOCATOR_PER_BAND") == 0)
    return CONTEST_MULT_GRID_PER_BAND;
  if (strcmp(upper, "CUSTOM") == 0 || strcmp(upper, "CUSTOM_LIST") == 0 ||
      strcmp(upper, "CUSTOM-LIST") == 0)
    return CONTEST_MULT_CUSTOM_LIST;

  /* Backward-compatible aliases. */
  if (strcmp(upper, "BAND_DXCC") == 0 || strcmp(upper, "BAND-DXCC") == 0)
    return CONTEST_MULT_DXCC_PER_BAND;
  if (strcmp(upper, "MODE_DXCC") == 0 || strcmp(upper, "MODE-DXCC") == 0)
    return CONTEST_MULT_MODE_DXCC;

  return CONTEST_MULT_DXCC;
}

static const char *contest_multiplier_to_text(ContestMultiplierType type) {
  switch (type) {
  case CONTEST_MULT_NONE:
    return "NONE";
  case CONTEST_MULT_DXCC:
    return "DXCC";
  case CONTEST_MULT_DXCC_PER_BAND:
    return "DXCC_PER_BAND";
  case CONTEST_MULT_ZONE_PER_BAND:
    return "ZONE_PER_BAND";
  case CONTEST_MULT_ZONE:
    return "ZONE";
  case CONTEST_MULT_PREFIX:
    return "PREFIX";
  case CONTEST_MULT_PREFIX_PER_BAND:
    return "PREFIX_PER_BAND";
  case CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND:
    return "DXCC_PLUS_ZONE_PER_BAND";
  case CONTEST_MULT_SPDX:
    return "SPDX";
  case CONTEST_MULT_GRID_PER_BAND:
    return "GRID_PER_BAND";
  case CONTEST_MULT_MODE_DXCC:
    return "MODE_DXCC";
  case CONTEST_MULT_CUSTOM_LIST:
    return "CUSTOM_LIST";
  case CONTEST_MULT_WAG:
    return "WAG";
  case CONTEST_MULT_IARU:
    return "IARU";
  case CONTEST_MULT_CQWW:
    return "CQWW";
  default:
    return "DXCC";
  }
}

int contest_definition_import_dxlog(const char *source_path,
                                    const char *dest_path,
                                    char *error_text,
                                    size_t error_size,
                                    char *warning_text,
                                    size_t warning_size) {
  if (!source_path || !source_path[0] || !dest_path || !dest_path[0]) {
    set_error(error_text, error_size, "Usage: source and destination paths are required");
    if (warning_text && warning_size > 0)
      warning_text[0] = 0;
    return -1;
  }

  if (warning_text && warning_size > 0)
    warning_text[0] = 0;

  ContestDefinition def;
  char load_err[128] = {0};
  if (contest_definition_load(source_path, &def, load_err, sizeof(load_err)) != 0) {
    set_error(error_text, error_size, load_err[0] ? load_err : "DXLog parse failed");
    return -1;
  }

  build_dxlog_import_warnings(source_path, warning_text, warning_size);

  FILE *f = fopen(dest_path, "w");
  if (!f) {
    set_error(error_text, error_size, "Cannot open destination contest file");
    return -1;
  }

  fprintf(f, "# Normalized from DXLog-compatible definition\n");
  fprintf(f, "NAME=%s\n", def.name[0] ? def.name : "GENERAL");
  fprintf(f, "CABRILLO_NAME=%s\n", def.cabrillo_name[0] ? def.cabrillo_name : def.name);
  fprintf(f, "MODE=%s\n", def.mode[0] ? def.mode : "MIXED");
  fprintf(f, "CATEGORY_OPERATOR=%s\n", def.category_operator[0] ? def.category_operator : "SINGLE-OP");
  fprintf(f, "CATEGORY_BAND=%s\n", def.category_band[0] ? def.category_band : "ALL");
  fprintf(f, "CATEGORY_POWER=%s\n", def.category_power[0] ? def.category_power : "LOW");
  fprintf(f, "EXCHANGE_SENT=%s\n", def.exchange_sent_template[0] ? def.exchange_sent_template : "#");
  if (def.exchange_received_type[0])
    fprintf(f, "EXCHANGE_RECEIVED_TYPE=%s\n", def.exchange_received_type);
  if (def.station_exchange_region[0])
    fprintf(f, "STATION_EXCHANGE_REGION=%s\n", def.station_exchange_region);
  if (def.serial_width > 0)
    fprintf(f, "SERIAL_WIDTH=%d\n", def.serial_width);
  if (def.serial_locator_separator[0])
    fprintf(f, "SERIAL_LOCATOR_SEPARATOR=%s\n", def.serial_locator_separator);
  if (!def.duplicate_mode_sensitive)
    fprintf(f, "DUPLICATE_MODE_SENSITIVE=0\n");
  if (def.validate_operating_rules)
    fprintf(f, "VALIDATE_OPERATING_RULES=1\n");
  if (def.allowed_modes[0])
    fprintf(f, "ALLOWED_MODES=%s\n", def.allowed_modes);
  if (def.allowed_bands[0])
    fprintf(f, "ALLOWED_BANDS=%s\n", def.allowed_bands);
  if (def.max_power_watts > 0)
    fprintf(f, "MAX_POWER_WATTS=%d\n", def.max_power_watts);
  if (def.distance_scoring)
    fprintf(f, "DISTANCE_SCORING=1\n");
  for (int i = 0; i < def.excluded_segment_count; i++)
    fprintf(f, "EXCLUDED_SEGMENT=%s,%d,%d\n",
            def.excluded_segments[i].mode, def.excluded_segments[i].low_khz,
            def.excluded_segments[i].high_khz);
  if (def.start_utc[0])
    fprintf(f, "START_UTC=%s\n", def.start_utc);
  if (def.end_utc[0])
    fprintf(f, "END_UTC=%s\n", def.end_utc);
  if (def.enforce_time_window)
    fprintf(f, "ENFORCE_TIME_WINDOW=1\n");
  if (def.points_configured) {
    fprintf(f, "POINTS_PER_QSO=%d\n", def.points_per_qso > 0 ? def.points_per_qso : 1);
    fprintf(f, "POINTS_CW=%d\n", def.points_cw);
    fprintf(f, "POINTS_PHONE=%d\n", def.points_phone);
    fprintf(f, "POINTS_DIGI=%d\n", def.points_digi);
    fprintf(f, "POINTS_NEW_DXCC=%d\n", def.points_new_dxcc);
    fprintf(f, "POINTS_SAME_DXCC=%d\n", def.points_same_dxcc);
    fprintf(f, "POINTS_NEW_BAND_DXCC=%d\n", def.points_new_band_dxcc);
    fprintf(f, "POINTS_SAME_BAND_DXCC=%d\n", def.points_same_band_dxcc);
  }
  for (int i = 0; i < def.score_rule_count; i++)
    fprintf(f, "SCORING_RULE=%d;%s\n", def.score_rules[i].points,
            def.score_rules[i].conditions);
  for (int i = 0; i < def.multiplier_count; i++) {
    if (def.multiplier_excluded_suffixes[i][0])
      fprintf(f, "MULTIPLIER%d_EXCLUDED_SUFFIXES=%s\n", i + 1,
              def.multiplier_excluded_suffixes[i]);
    if (def.multiplier_special_prefixes[i][0])
      fprintf(f, "MULTIPLIER%d_SPECIAL_PREFIXES=%s\n", i + 1,
              def.multiplier_special_prefixes[i]);
  }
  for (int i = 0; i < def.region_count; i++) {
    const ContestRegionDef *region = &def.regions[i];
    fprintf(f, "REGION=%s", region->name);
    if (region->continent[0])
      fprintf(f, ";CONTINENT=%s", region->continent);
    if (region->countries[0])
      fprintf(f, ";COUNTRIES=%s", region->countries);
    if (region->prefixes[0])
      fprintf(f, ";PREFIXES=%s", region->prefixes);
    if (region->exchanges[0])
      fprintf(f, ";EXCHANGES=%s", region->exchanges);
    fputc('\n', f);
  }
  fprintf(f, "MULTIPLIER=");
  for (int i = 0; i < def.multiplier_count; i++)
    fprintf(f, "%s%s", i ? "," : "",
            contest_multiplier_to_text(def.multipliers[i]));
  fputc('\n', f);
  if (def.custom_mult_list[0])
    fprintf(f, "CUSTOM_MULT_LIST=%s\n", def.custom_mult_list);
  if (def.mult3_type[0])
    fprintf(f, "MULT3_TYPE=%s\n", def.mult3_type);
  if (def.mult3_field[0])
    fprintf(f, "MULT3_FIELD=%s\n", def.mult3_field);
  if (def.section_name[0])
    fprintf(f, "SECTION=%s\n", def.section_name);
  if (def.area_name[0])
    fprintf(f, "AREA=%s\n", def.area_name);
  if (def.pfx_area[0])
    fprintf(f, "PFX_AREA=%s\n", def.pfx_area);
  fprintf(f, "BONUS_POINTS=%d\n", def.bonus_points);
  if (def.duplicate_qso)
    fprintf(f, "DOUBLE_QSO=1\n");
  if (def.qtc_sender_side[0] && strcmp(def.qtc_sender_side, "NONE") != 0) {
    fprintf(f, "QTC_SENDER=%s\n", def.qtc_sender_side);
    fprintf(f, "POINTS_PER_QTC=%d\n", def.points_per_qtc > 0 ? def.points_per_qtc : 1);
  }

  for (int i = 0; i < def.field_count; i++) {
    const ContestFieldDef *field = &def.fields[i];
    if (!field->name[0])
      continue;
    fprintf(f, "FIELD=%s,%s,%s\n",
            field->name,
            field->label[0] ? field->label : field->name,
            field->required ? "required" : "optional");
  }

  fclose(f);
  set_error(error_text, error_size, "");
  return 0;
}

static void parse_field_line(const char *value, ContestDefinition *out) {
  if (!value || !out)
    return;

  if (out->field_count >= CONTEST_DEF_MAX_FIELDS)
    return;

  char buffer[192];
  snprintf(buffer, sizeof(buffer), "%s", value);

  char *name = strtok(buffer, ",");
  char *label = strtok(NULL, ",");
  char *required = strtok(NULL, ",");

  if (!name)
    return;

  trim_in_place(name);
  if (!name[0])
    return;

  ContestFieldDef *f = &out->fields[out->field_count++];
  memset(f, 0, sizeof(*f));

  snprintf(f->name, sizeof(f->name), "%s", name);

  if (label) {
    trim_in_place(label);
    snprintf(f->label, sizeof(f->label), "%s", label);
  } else {
    snprintf(f->label, sizeof(f->label), "%s", name);
  }

  if (required) {
    trim_in_place(required);
    uppercase_in_place(required);
    f->required = (strcmp(required, "REQUIRED") == 0 ||
                   strcmp(required, "1") == 0 ||
                   strcmp(required, "YES") == 0);
  } else {
    f->required = 0;
  }
}

int contest_definition_load(const char *path, ContestDefinition *out,
                           char *error_text, size_t error_size) {
  if (!path || !path[0] || !out) {
    set_error(error_text, error_size, "Missing contest definition path");
    return -1;
  }

  FILE *f = fopen(path, "r");
  if (!f) {
    set_error(error_text, error_size, "Cannot open contest definition file");
    return -1;
  }

  contest_definition_init_defaults(out);

  int saw_dxlog_name = 0;
  char dxlog_mult1_type[32] = {0};
  char dxlog_mult1_count[32] = {0};
  char dxlog_mult2_type[32] = {0};
  char dxlog_mult2_count[32] = {0};
  int dxlog_field_set = 0;

  char line[256];
  int line_no = 0;
  while (fgets(line, sizeof(line), f)) {
    line_no++;

    trim_in_place(line);
    if (!line[0] || line[0] == '#')
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;

    *eq = 0;
    char *key = line;
    char *value = eq + 1;

    trim_in_place(key);
    trim_in_place(value);
    uppercase_in_place(key);

    if (!key[0])
      continue;

    if (strcmp(key, "NAME") == 0 || strcmp(key, "CONTESTNAME") == 0) {
      snprintf(out->name, sizeof(out->name), "%s", value);
      if (strcmp(key, "CONTESTNAME") == 0)
        saw_dxlog_name = 1;
    } else if (strcmp(key, "CABRILLO_NAME") == 0 ||
               strcmp(key, "CABRILLO-CONTEST") == 0 ||
               strcmp(key, "CABRILLO_CONTEST_NAME") == 0) {
      snprintf(out->cabrillo_name, sizeof(out->cabrillo_name), "%s", value);
    } else if (strcmp(key, "MODE") == 0) {
      snprintf(out->mode, sizeof(out->mode), "%s", value);
      uppercase_in_place(out->mode);
    } else if (strcmp(key, "MODES") == 0) {
      char mode_token[16] = {0};
      first_token(value, mode_token, sizeof(mode_token));
      if (strchr(value, ';'))
        snprintf(out->mode, sizeof(out->mode), "%s", "MIXED");
      else
        snprintf(out->mode, sizeof(out->mode), "%s", mode_token[0] ? mode_token : "MIXED");
      uppercase_in_place(out->mode);
    } else if (strcmp(key, "CATEGORY_OPERATOR") == 0) {
      snprintf(out->category_operator, sizeof(out->category_operator), "%s",
               value);
      uppercase_in_place(out->category_operator);
    } else if (strcmp(key, "CATEGORY_BAND") == 0) {
      snprintf(out->category_band, sizeof(out->category_band), "%s", value);
      uppercase_in_place(out->category_band);
    } else if (strcmp(key, "CATEGORY_POWER") == 0) {
      snprintf(out->category_power, sizeof(out->category_power), "%s", value);
      uppercase_in_place(out->category_power);
    } else if (strcmp(key, "CATEGORY_OVERLAY") == 0) {
      snprintf(out->category_overlay, sizeof(out->category_overlay), "%s",
               value);
      uppercase_in_place(out->category_overlay);
    } else if (strcmp(key, "STATION_LOCATION") == 0) {
      snprintf(out->station_location, sizeof(out->station_location), "%s",
               value);
      uppercase_in_place(out->station_location);
    } else if (strcmp(key, "OPERATORS") == 0) {
      snprintf(out->operators, sizeof(out->operators), "%s", value);
      uppercase_in_place(out->operators);
    } else if (strcmp(key, "EXCHANGE_SENT") == 0) {
      snprintf(out->exchange_sent_template,
               sizeof(out->exchange_sent_template), "%s", value);
    } else if (strcmp(key, "EXCHANGE_RECEIVED_TYPE") == 0) {
      snprintf(out->exchange_received_type,
               sizeof(out->exchange_received_type), "%s", value);
      uppercase_in_place(out->exchange_received_type);
    } else if (strcmp(key, "STATION_EXCHANGE_REGION") == 0) {
      snprintf(out->station_exchange_region,
               sizeof(out->station_exchange_region), "%s", value);
      uppercase_in_place(out->station_exchange_region);
    } else if (strcmp(key, "SERIAL_WIDTH") == 0) {
      out->serial_width = atoi(value);
      if (out->serial_width < 0 || out->serial_width > 8)
        out->serial_width = 0;
    } else if (strcmp(key, "SERIAL_LOCATOR_SEPARATOR") == 0) {
      snprintf(out->serial_locator_separator,
               sizeof(out->serial_locator_separator), "%s", value);
      uppercase_in_place(out->serial_locator_separator);
    } else if (strcmp(key, "DUPLICATE_MODE_SENSITIVE") == 0) {
      out->duplicate_mode_sensitive = parse_truthy_flag(value);
    } else if (strcmp(key, "VALIDATE_OPERATING_RULES") == 0) {
      out->validate_operating_rules = parse_truthy_flag(value);
    } else if (strcmp(key, "ALLOWED_MODES") == 0) {
      snprintf(out->allowed_modes, sizeof(out->allowed_modes), "%s", value);
      uppercase_in_place(out->allowed_modes);
    } else if (strcmp(key, "ALLOWED_BANDS") == 0) {
      snprintf(out->allowed_bands, sizeof(out->allowed_bands), "%s", value);
      uppercase_in_place(out->allowed_bands);
    } else if (strcmp(key, "MAX_POWER_WATTS") == 0) {
      out->max_power_watts = atoi(value);
      if (out->max_power_watts < 0)
        out->max_power_watts = 0;
    } else if (strcmp(key, "EXCLUDED_SEGMENT") == 0) {
      parse_excluded_segment(value, out);
    } else if (strcmp(key, "START_UTC") == 0) {
      snprintf(out->start_utc, sizeof(out->start_utc), "%s", value);
    } else if (strcmp(key, "END_UTC") == 0) {
      snprintf(out->end_utc, sizeof(out->end_utc), "%s", value);
    } else if (strcmp(key, "ENFORCE_TIME_WINDOW") == 0) {
      out->enforce_time_window = parse_truthy_flag(value);
    } else if (strcmp(key, "DISTANCE_SCORING") == 0) {
      out->distance_scoring = parse_truthy_flag(value);
    } else if (strcmp(key, "SCORING_RULE") == 0) {
      parse_score_rule(value, out);
    } else if (strcmp(key, "REGION") == 0) {
      parse_region(value, out);
    } else if (strcmp(key, "POINTS_PER_QSO") == 0) {
      out->points_per_qso = atoi(value);
      if (out->points_per_qso <= 0)
        out->points_per_qso = 1;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_CW") == 0) {
      out->points_cw = atoi(value);
      if (out->points_cw < 0)
        out->points_cw = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_PHONE") == 0) {
      out->points_phone = atoi(value);
      if (out->points_phone < 0)
        out->points_phone = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_DIGI") == 0) {
      out->points_digi = atoi(value);
      if (out->points_digi < 0)
        out->points_digi = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_NEW_DXCC") == 0) {
      out->points_new_dxcc = atoi(value);
      if (out->points_new_dxcc < 0)
        out->points_new_dxcc = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_SAME_DXCC") == 0) {
      out->points_same_dxcc = atoi(value);
      if (out->points_same_dxcc < 0)
        out->points_same_dxcc = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_NEW_BAND_DXCC") == 0) {
      out->points_new_band_dxcc = atoi(value);
      if (out->points_new_band_dxcc < 0)
        out->points_new_band_dxcc = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_SAME_BAND_DXCC") == 0) {
      out->points_same_band_dxcc = atoi(value);
      if (out->points_same_band_dxcc < 0)
        out->points_same_band_dxcc = 0;
      out->points_configured = 1;
    } else if (strcmp(key, "POINTS_TYPE") == 0) {
      /* The DXLog type alone does not define point values in this logger. */
    } else if (strcmp(key, "MULTIPLIER") == 0) {
      set_multiplier_list(out, value);
    } else if (strncmp(key, "MULTIPLIER", 10) == 0 &&
               isdigit((unsigned char)key[10])) {
      const int family = atoi(key + 10) - 1;
      if (family >= 0 && family < CONTEST_DEF_MAX_MULTIPLIERS) {
        char *attribute = strchr(key + 10, '_');
        if (attribute && strcmp(attribute, "_EXCLUDED_SUFFIXES") == 0) {
          snprintf(out->multiplier_excluded_suffixes[family],
                   sizeof(out->multiplier_excluded_suffixes[family]), "%s", value);
          uppercase_in_place(out->multiplier_excluded_suffixes[family]);
        } else if (attribute && strcmp(attribute, "_SPECIAL_PREFIXES") == 0) {
          snprintf(out->multiplier_special_prefixes[family],
                   sizeof(out->multiplier_special_prefixes[family]), "%s", value);
          uppercase_in_place(out->multiplier_special_prefixes[family]);
        }
      }
    } else if (strcmp(key, "CUSTOM_MULT_LIST") == 0) {
      snprintf(out->custom_mult_list, sizeof(out->custom_mult_list), "%s", value);
    } else if (strcmp(key, "MULT3_TYPE") == 0) {
      snprintf(out->mult3_type, sizeof(out->mult3_type), "%s", value);
      uppercase_in_place(out->mult3_type);
    } else if (strcmp(key, "MULT3_FIELD") == 0) {
      snprintf(out->mult3_field, sizeof(out->mult3_field), "%s", value);
      uppercase_in_place(out->mult3_field);
    } else if (strcmp(key, "SECTION") == 0) {
      snprintf(out->section_name, sizeof(out->section_name), "%s", value);
    } else if (strcmp(key, "AREA") == 0) {
      snprintf(out->area_name, sizeof(out->area_name), "%s", value);
    } else if (strcmp(key, "PFX_AREA") == 0) {
      snprintf(out->pfx_area, sizeof(out->pfx_area), "%s", value);
    } else if (strcmp(key, "MULT1_TYPE") == 0) {
      snprintf(dxlog_mult1_type, sizeof(dxlog_mult1_type), "%s", value);
    } else if (strcmp(key, "MULT1_COUNT") == 0) {
      snprintf(dxlog_mult1_count, sizeof(dxlog_mult1_count), "%s", value);
    } else if (strcmp(key, "MULT2_TYPE") == 0) {
      snprintf(dxlog_mult2_type, sizeof(dxlog_mult2_type), "%s", value);
    } else if (strcmp(key, "MULT2_COUNT") == 0) {
      snprintf(dxlog_mult2_count, sizeof(dxlog_mult2_count), "%s", value);
    } else if (strcmp(key, "FIELD_RCVD_TYPE") == 0) {
      apply_dxlog_received_field_type(value, out);
      dxlog_field_set = out->field_count > 0;
    } else if (strcmp(key, "QSO_NUMBER_CATEGORY") == 0) {
      if (!text_contains_token_ci(value, "NONE")) {
        snprintf(out->exchange_sent_template,
                 sizeof(out->exchange_sent_template), "%s", "#");
        if (!dxlog_field_set) {
          out->field_count = 1;
          snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s",
                   "SERIAL");
          snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s",
                   "Serial");
          out->fields[0].required = 1;
          dxlog_field_set = 1;
        }
      }
    } else if (strcmp(key, "BONUS_POINTS") == 0) {
      out->bonus_points = atoi(value);
      if (out->bonus_points < 0)
        out->bonus_points = 0;
    } else if (strcmp(key, "QTC_SENDER") == 0) {
      snprintf(out->qtc_sender_side, sizeof(out->qtc_sender_side), "%s", value);
      uppercase_in_place(out->qtc_sender_side);
    } else if (strcmp(key, "POINTS_PER_QTC") == 0) {
      out->points_per_qtc = atoi(value);
      if (out->points_per_qtc < 0)
        out->points_per_qtc = 0;
    } else if (strcmp(key, "DOUBLE_QSO") == 0) {
      out->duplicate_qso = parse_truthy_flag(value);
    } else if (strcmp(key, "FIELD") == 0) {
      parse_field_line(value, out);
    } else {
      (void)line_no;
    }
  }

  fclose(f);

  if (!out->name[0])
    snprintf(out->name, sizeof(out->name), "%s", "GENERAL");

  if (!out->cabrillo_name[0])
    snprintf(out->cabrillo_name, sizeof(out->cabrillo_name), "%s", out->name);

  if (saw_dxlog_name) {
    if (str_contains_upper(out->name, "CQ WPX")) {
      snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template), "%s", "#");
      out->multiplier_type = CONTEST_MULT_PREFIX;
      out->serial_width = 3;
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "SERIAL");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "Serial");
        out->fields[0].required = 1;
      }
    } else if (str_contains_upper(out->name, "CQ WORLD WIDE") ||
               str_contains_upper(out->name, "CQ WW")) {
      snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template), "%s", "CQZONE");
      out->exchange_received_type[0] = 0;
      snprintf(out->exchange_received_type,
           sizeof(out->exchange_received_type), "%s", "CQ_ZONE");
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "CQZONE");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "CQ Zone");
        out->fields[0].required = 1;
      }
    } else if (str_contains_upper(out->name, "SP DX")) {
      snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template), "%s", "#");
      out->multiplier_type = CONTEST_MULT_SPDX;
      snprintf(out->exchange_received_type,
           sizeof(out->exchange_received_type), "%s",
           "SP_PROVINCE_OR_SERIAL");
      out->serial_width = 3;
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "EXCHANGE");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "Exchange");
        out->fields[0].required = 1;
      }
    } else if (dxlog_mult1_type[0]) {
      out->multipliers[0] = map_dxlog_multiplier(dxlog_mult1_type,
                                                 dxlog_mult1_count);
      out->multiplier_count = 1;
      if (dxlog_mult2_type[0] &&
          out->multiplier_count < CONTEST_DEF_MAX_MULTIPLIERS)
        out->multipliers[out->multiplier_count++] =
            map_dxlog_multiplier(dxlog_mult2_type, dxlog_mult2_count);
      out->multiplier_type = out->multipliers[0];
    }
  }

  if (saw_dxlog_name) {
    if (str_contains_upper(out->name, "CQ WORLD WIDE") ||
        str_contains_upper(out->name, "CQ WW")) {
      out->multipliers[0] = CONTEST_MULT_DXCC_PER_BAND;
      out->multipliers[1] = CONTEST_MULT_ZONE_PER_BAND;
      out->multiplier_count = 2;
      out->multiplier_type = out->multipliers[0];
      snprintf(out->exchange_received_type,
               sizeof(out->exchange_received_type), "%s", "CQ_ZONE");
      snprintf(out->multiplier_excluded_suffixes[0],
               sizeof(out->multiplier_excluded_suffixes[0]), "%s", "/MM,/AM");
      snprintf(out->multiplier_special_prefixes[0],
               sizeof(out->multiplier_special_prefixes[0]), "%s", "IG9,IH9");
    } else if (out->multiplier_count <= 1) {
      out->multipliers[0] = out->multiplier_type;
      out->multiplier_count = 1;
    }
  }

  set_error(error_text, error_size, "");
  return 0;
}
