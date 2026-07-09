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
  if (strcmp(upper, "BAND_DXCC") == 0 || strcmp(upper, "BAND-DXCC") == 0)
    return CONTEST_MULT_BAND_DXCC;
  if (strcmp(upper, "MODE_DXCC") == 0 || strcmp(upper, "MODE-DXCC") == 0)
    return CONTEST_MULT_MODE_DXCC;

  return CONTEST_MULT_DXCC;
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

    if (strcmp(key, "NAME") == 0) {
      snprintf(out->name, sizeof(out->name), "%s", value);
    } else if (strcmp(key, "CABRILLO_NAME") == 0 ||
               strcmp(key, "CABRILLO-CONTEST") == 0) {
      snprintf(out->cabrillo_name, sizeof(out->cabrillo_name), "%s", value);
    } else if (strcmp(key, "MODE") == 0) {
      snprintf(out->mode, sizeof(out->mode), "%s", value);
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
    } else if (strcmp(key, "BONUS_POINTS") == 0) {
      out->bonus_points = atoi(value);
      if (out->bonus_points < 0)
        out->bonus_points = 0;
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

  set_error(error_text, error_size, "");
  return 0;
}
