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

  return CONTEST_MULT_DXCC;
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
         strcmp(key_upper, "MULT3_TYPE") == 0 ||
         strcmp(key_upper, "MULT3_COUNT") == 0 ||
         strcmp(key_upper, "MULT3_FIELD") == 0 ||
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
  out->points_per_qso = 1;
  out->points_cw = 0;
  out->points_phone = 0;
  out->points_digi = 0;
  out->points_new_dxcc = 0;
  out->points_same_dxcc = 0;
  out->points_new_band_dxcc = 0;
  out->points_same_band_dxcc = 0;
  out->multiplier_type = CONTEST_MULT_DXCC;
  out->bonus_points = 0;
  snprintf(out->qtc_sender_side, sizeof(out->qtc_sender_side), "%s", "NONE");
  out->points_per_qtc = 0;
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
  case CONTEST_MULT_MODE_DXCC:
    return "MODE_DXCC";
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
  fprintf(f, "POINTS_PER_QSO=%d\n", def.points_per_qso > 0 ? def.points_per_qso : 1);
  fprintf(f, "POINTS_CW=%d\n", def.points_cw);
  fprintf(f, "POINTS_PHONE=%d\n", def.points_phone);
  fprintf(f, "POINTS_DIGI=%d\n", def.points_digi);
  fprintf(f, "POINTS_NEW_DXCC=%d\n", def.points_new_dxcc);
  fprintf(f, "POINTS_SAME_DXCC=%d\n", def.points_same_dxcc);
  fprintf(f, "POINTS_NEW_BAND_DXCC=%d\n", def.points_new_band_dxcc);
  fprintf(f, "POINTS_SAME_BAND_DXCC=%d\n", def.points_same_band_dxcc);
  fprintf(f, "MULTIPLIER=%s\n", contest_multiplier_to_text(def.multiplier_type));
  fprintf(f, "BONUS_POINTS=%d\n", def.bonus_points);
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
    } else if (strcmp(key, "POINTS_PER_QSO") == 0) {
      out->points_per_qso = atoi(value);
      if (out->points_per_qso <= 0)
        out->points_per_qso = 1;
    } else if (strcmp(key, "POINTS_CW") == 0) {
      out->points_cw = atoi(value);
      if (out->points_cw < 0)
        out->points_cw = 0;
    } else if (strcmp(key, "POINTS_PHONE") == 0) {
      out->points_phone = atoi(value);
      if (out->points_phone < 0)
        out->points_phone = 0;
    } else if (strcmp(key, "POINTS_DIGI") == 0) {
      out->points_digi = atoi(value);
      if (out->points_digi < 0)
        out->points_digi = 0;
    } else if (strcmp(key, "POINTS_NEW_DXCC") == 0) {
      out->points_new_dxcc = atoi(value);
      if (out->points_new_dxcc < 0)
        out->points_new_dxcc = 0;
    } else if (strcmp(key, "POINTS_SAME_DXCC") == 0) {
      out->points_same_dxcc = atoi(value);
      if (out->points_same_dxcc < 0)
        out->points_same_dxcc = 0;
    } else if (strcmp(key, "POINTS_NEW_BAND_DXCC") == 0) {
      out->points_new_band_dxcc = atoi(value);
      if (out->points_new_band_dxcc < 0)
        out->points_new_band_dxcc = 0;
    } else if (strcmp(key, "POINTS_SAME_BAND_DXCC") == 0) {
      out->points_same_band_dxcc = atoi(value);
      if (out->points_same_band_dxcc < 0)
        out->points_same_band_dxcc = 0;
    } else if (strcmp(key, "MULTIPLIER") == 0) {
      out->multiplier_type = contest_multiplier_from_text(value);
    } else if (strcmp(key, "MULT1_TYPE") == 0) {
      snprintf(dxlog_mult1_type, sizeof(dxlog_mult1_type), "%s", value);
    } else if (strcmp(key, "MULT1_COUNT") == 0) {
      snprintf(dxlog_mult1_count, sizeof(dxlog_mult1_count), "%s", value);
    } else if (strcmp(key, "MULT2_TYPE") == 0) {
      snprintf(dxlog_mult2_type, sizeof(dxlog_mult2_type), "%s", value);
    } else if (strcmp(key, "MULT2_COUNT") == 0) {
      snprintf(dxlog_mult2_count, sizeof(dxlog_mult2_count), "%s", value);
    } else if (strcmp(key, "FIELD_RCVD_TYPE") == 0) {
      out->field_count = 0;
      if (strstr(value, "NR")) {
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "SERIAL");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "Serial");
        out->fields[0].required = 1;
        out->field_count = 1;
      } else if (strstr(value, "CQZONE")) {
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "CQZONE");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "CQ Zone");
        out->fields[0].required = 1;
        out->field_count = 1;
      }
      dxlog_field_set = out->field_count > 0;
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
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "SERIAL");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "Serial");
        out->fields[0].required = 1;
      }
    } else if (str_contains_upper(out->name, "CQ WORLD WIDE") ||
               str_contains_upper(out->name, "CQ WW")) {
      snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template), "%s", "CQZONE");
      out->multiplier_type = CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND;
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "CQZONE");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "CQ Zone");
        out->fields[0].required = 1;
      }
    } else if (str_contains_upper(out->name, "SP DX")) {
      snprintf(out->exchange_sent_template, sizeof(out->exchange_sent_template), "%s", "#");
      out->multiplier_type = CONTEST_MULT_SPDX;
      if (!dxlog_field_set) {
        out->field_count = 1;
        snprintf(out->fields[0].name, sizeof(out->fields[0].name), "%s", "EXCHANGE");
        snprintf(out->fields[0].label, sizeof(out->fields[0].label), "%s", "Exchange");
        out->fields[0].required = 1;
      }
    } else if (dxlog_mult1_type[0]) {
      out->multiplier_type = map_dxlog_multiplier(dxlog_mult1_type, dxlog_mult1_count);
      if (dxlog_mult2_type[0] &&
          map_dxlog_multiplier(dxlog_mult1_type, dxlog_mult1_count) == CONTEST_MULT_DXCC_PER_BAND &&
          map_dxlog_multiplier(dxlog_mult2_type, dxlog_mult2_count) == CONTEST_MULT_ZONE_PER_BAND) {
        out->multiplier_type = CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND;
      }
    }
  }

  set_error(error_text, error_size, "");
  return 0;
}
