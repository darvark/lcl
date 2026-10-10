#define _POSIX_C_SOURCE 200809L

#include "contest_rules.h"
#include "qso.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int reject(char *error, size_t error_size, const char *message) {
  if (error && error_size > 0)
    snprintf(error, error_size, "%s", message);
  return 0;
}

static int text_equal_ci(const char *left, const char *right) {
  return left && right && strcasecmp(left, right) == 0;
}

static int list_contains_ci(const char *list, const char *value) {
  if (!list || !list[0] || !value || !value[0])
    return 0;

  char copy[256];
  snprintf(copy, sizeof(copy), "%s", list);
    char *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item;
      item = strtok_r(NULL, ",", &save)) {
    while (*item && isspace((unsigned char)*item))
      item++;
    size_t length = strlen(item);
    while (length > 0 && isspace((unsigned char)item[length - 1]))
      item[--length] = 0;
    if (text_equal_ci(item, value))
      return 1;
  }
  return 0;
}

static int has_prefix_ci(const char *text, const char *prefix) {
  return text && prefix && prefix[0] &&
         strncasecmp(text, prefix, strlen(prefix)) == 0;
}

static int matches_prefix_list(const char *list, const char *call,
                               const char *cty_prefix) {
  if (!list || !list[0])
    return 0;
  char copy[256];
  snprintf(copy, sizeof(copy), "%s", list);
    char *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item;
      item = strtok_r(NULL, ",", &save)) {
    while (*item && isspace((unsigned char)*item))
      item++;
    size_t length = strlen(item);
    while (length > 0 && isspace((unsigned char)item[length - 1]))
      item[--length] = 0;
    if (has_prefix_ci(call, item) || text_equal_ci(cty_prefix, item))
      return 1;
  }
  return 0;
}

static int region_matches(const ContestDefinition *definition,
                          const char *region_name, const char *country,
                          const char *continent, const char *prefix,
                          const char *call, const char *exchange) {
  if (!region_name || !region_name[0])
    return 0;
  for (int i = 0; definition && i < definition->region_count; i++) {
    const ContestRegionDef *region = &definition->regions[i];
    if (!text_equal_ci(region->name, region_name))
      continue;
    return (region->continent[0] &&
            text_equal_ci(region->continent, continent)) ||
           (region->countries[0] &&
            list_contains_ci(region->countries, country)) ||
           (region->prefixes[0] &&
            matches_prefix_list(region->prefixes, call, prefix)) ||
           (region->exchanges[0] &&
            list_contains_ci(region->exchanges, exchange));
  }
  return 0;
}

int contest_definition_station_in_region(const ContestDefinition *definition,
                                        const char *region_name,
                                        const char *station_call,
                                        const char *station_exchange) {
  const CtyEntry *station = cty_lookup(station_call);
  return region_matches(definition, region_name,
                        station ? station->country : "",
                        station ? station->continent : "",
                        station ? station->prefix : "", station_call,
                        station_exchange);
}

static void exchange_class(const char *exchange, char *out, size_t out_size) {
  if (!exchange || !exchange[0]) {
    snprintf(out, out_size, "%s", "EMPTY");
    return;
  }
  int numeric = 1;
  int alphabetic = 1;
  for (size_t i = 0; exchange[i]; i++) {
    numeric &= isdigit((unsigned char)exchange[i]) != 0;
    alphabetic &= isalpha((unsigned char)exchange[i]) != 0;
  }
  snprintf(out, out_size, "%s", numeric ? "NUMERIC" :
           alphabetic ? "ALPHA" : "TEXT");
}

static int scalar_condition_matches(const char *actual, const char *expected) {
  if (!expected || !expected[0])
    return 0;
  const int negate = expected[0] == '!';
  if (negate)
    expected++;
  const int equal = text_equal_ci(actual, expected);
  return negate ? !equal : equal;
}

static int boolean_condition_matches(int actual, const char *expected) {
  if (!expected || !expected[0])
    return 0;
  const int negate = expected[0] == '!';
  if (negate)
    expected++;
  const int wanted = strcmp(expected, "1") == 0 ||
                     strcasecmp(expected, "YES") == 0 ||
                     strcasecmp(expected, "TRUE") == 0;
  return negate ? actual != wanted : actual == wanted;
}

static int condition_matches(const ContestDefinition *definition,
                             const ContestScoreContext *context,
                             const char *condition) {
  char token[128];
  snprintf(token, sizeof(token), "%s", condition ? condition : "");
  char *separator = strchr(token, '=');
  if (!separator)
    return 0;
  *separator++ = 0;
  while (*token && isspace((unsigned char)*token))
    memmove(token, token + 1, strlen(token));
  char *value = separator;
  while (*value && isspace((unsigned char)*value))
    value++;

  char actual[32] = {0};
  if (strcmp(token, "SAME_COUNTRY") == 0) {
    snprintf(actual, sizeof(actual), "%d", context->source_country &&
             context->source_country[0] && context->destination_country &&
             context->destination_country[0] &&
             text_equal_ci(context->source_country, context->destination_country));
  } else if (strcmp(token, "SAME_CONTINENT") == 0) {
    snprintf(actual, sizeof(actual), "%d", context->source_continent &&
             context->source_continent[0] && context->destination_continent &&
             context->destination_continent[0] &&
             text_equal_ci(context->source_continent,
                           context->destination_continent));
  } else if (strcmp(token, "SAME_ITU_ZONE") == 0) {
    snprintf(actual, sizeof(actual), "%d", context->same_itu_zone != 0);
  } else if (strcmp(token, "NEW_DXCC") == 0) {
    snprintf(actual, sizeof(actual), "%d", context->new_dxcc != 0);
  } else if (strcmp(token, "NEW_BAND_DXCC") == 0) {
    snprintf(actual, sizeof(actual), "%d", context->new_band_dxcc != 0);
  } else if (strcmp(token, "BAND_CLASS") == 0) {
    const char *band = context->band ? context->band : "";
    const int low = strcmp(band, "160") == 0 || strcmp(band, "160M") == 0 ||
                    strcmp(band, "80") == 0 || strcmp(band, "80M") == 0 ||
                    strcmp(band, "40") == 0 || strcmp(band, "40M") == 0;
    snprintf(actual, sizeof(actual), "%s", low ? "LOW" : "HIGH");
  } else if (strcmp(token, "EXCHANGE_CLASS") == 0) {
    exchange_class(context->exchange_received, actual, sizeof(actual));
  } else if (strcmp(token, "EXCHANGE_VALUE") == 0) {
    return boolean_condition_matches(
        list_contains_ci(value, context->exchange_received), "1");
  } else if (strcmp(token, "STATION_EXCHANGE_CLASS") == 0) {
    exchange_class(context->station_exchange, actual, sizeof(actual));
  } else if (strcmp(token, "SOURCE_REGION") == 0) {
    const int negate = value[0] == '!';
    const char *region = negate ? value + 1 : value;
    const int matches = region_matches(definition, region,
        context->source_country, context->source_continent,
      context->source_prefix, NULL, context->station_exchange);
    return boolean_condition_matches(matches, negate ? "!1" : "1");
  } else if (strcmp(token, "DESTINATION_REGION") == 0) {
    const int negate = value[0] == '!';
    const char *region = negate ? value + 1 : value;
    const int matches = region_matches(definition, region,
        context->destination_country, context->destination_continent,
      context->destination_prefix, context->call,
      context->exchange_received);
    return boolean_condition_matches(matches, negate ? "!1" : "1");
  } else if (strcmp(token, "CALL_PREFIX") == 0) {
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", value);
    int matches = 0;
    int has_positive = 0;
    int rejected = 0;
    char *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item;
         item = strtok_r(NULL, ",", &save)) {
      while (*item && isspace((unsigned char)*item))
        item++;
      if (item[0] == '!')
        rejected |= has_prefix_ci(context->call, item + 1);
      else {
        has_positive = 1;
        matches |= has_prefix_ci(context->call, item);
      }
    }
    matches = !rejected && (!has_positive || matches);
    return boolean_condition_matches(matches, "1");
  } else if (strcmp(token, "CALL_SUFFIX") == 0) {
    const char *call = context->call ? context->call : "";
    const size_t call_length = strlen(call);
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", value);
    int matches = 0;
        char *save = NULL;
        for (char *item = strtok_r(copy, ",", &save); item;
          item = strtok_r(NULL, ",", &save)) {
      const size_t suffix_length = strlen(item);
      matches |= suffix_length <= call_length &&
                 strcasecmp(call + call_length - suffix_length, item) == 0;
    }
    return boolean_condition_matches(matches, "1");
  } else if (strcmp(token, "BAND") == 0) {
    snprintf(actual, sizeof(actual), "%s", context->band ? context->band : "");
  } else if (strcmp(token, "MODE") == 0) {
    snprintf(actual, sizeof(actual), "%s", context->mode ? context->mode : "");
  } else if (strcmp(token, "SOURCE_COUNTRY") == 0) {
    return scalar_condition_matches(context->source_country, value);
  } else if (strcmp(token, "SOURCE_CONTINENT") == 0) {
    return scalar_condition_matches(context->source_continent, value);
  } else if (strcmp(token, "DESTINATION_COUNTRY") == 0) {
    return scalar_condition_matches(context->destination_country, value);
  } else if (strcmp(token, "DESTINATION_CONTINENT") == 0) {
    return scalar_condition_matches(context->destination_continent, value);
  } else if (strcmp(token, "SOURCE_PREFIX") == 0) {
    const int negate = value[0] == '!';
    const char *prefixes = negate ? value + 1 : value;
    const int matches = matches_prefix_list(prefixes, NULL,
                                            context->source_prefix);
    return boolean_condition_matches(matches, negate ? "!1" : "1");
  } else if (strcmp(token, "DESTINATION_PREFIX") == 0) {
    const int negate = value[0] == '!';
    const char *prefixes = negate ? value + 1 : value;
    const int matches = matches_prefix_list(prefixes, context->call,
                                            context->destination_prefix);
    return boolean_condition_matches(matches, negate ? "!1" : "1");
  } else {
    return 0;
  }
  return scalar_condition_matches(actual, value);
}

static int score_rule_matches(const ContestDefinition *definition,
                              const ContestScoreContext *context,
                              const ContestScoreRule *rule) {
  if (!rule->conditions[0])
    return 1;
  char conditions[sizeof(rule->conditions)];
  snprintf(conditions, sizeof(conditions), "%s", rule->conditions);
    char *save = NULL;
    for (char *condition = strtok_r(conditions, ";", &save); condition;
      condition = strtok_r(NULL, ";", &save)) {
    while (*condition && isspace((unsigned char)*condition))
      condition++;
    if (!condition_matches(definition, context, condition))
      return 0;
  }
  return 1;
}

int contest_definition_score_qso(const ContestDefinition *definition,
                                 const ContestScoreContext *context,
                                 int *out_points) {
  if (!definition || !context || !out_points)
    return 0;
  if (definition->distance_scoring && context->distance_km > 0) {
    *out_points = context->distance_km;
    return 1;
  }
  if (definition->score_rule_count <= 0)
    return 0;
  for (int i = 0; i < definition->score_rule_count; i++) {
    if (score_rule_matches(definition, context, &definition->score_rules[i])) {
      *out_points = definition->score_rules[i].points;
      return 1;
    }
  }
  *out_points = definition->points_per_qso > 0 ? definition->points_per_qso : 1;
  return 1;
}

static int parse_utc_timestamp(const char *text, time_t *out_time) {
  if (!text || strlen(text) != 20 || !out_time || text[4] != '-' ||
      text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[19] != 'Z')
    return 0;

  const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9,
                                 11, 12, 14, 15, 17, 18};
  for (size_t i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); i++)
    if (!isdigit((unsigned char)text[digit_positions[i]]))
      return 0;

  const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
                   (text[2] - '0') * 10 + text[3] - '0';
  const int month = (text[5] - '0') * 10 + text[6] - '0';
  const int day = (text[8] - '0') * 10 + text[9] - '0';
  const int hour = (text[11] - '0') * 10 + text[12] - '0';
  const int minute = (text[14] - '0') * 10 + text[15] - '0';
  const int second = (text[17] - '0') * 10 + text[18] - '0';
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 59)
    return 0;

  static const int month_days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  const int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int max_day = month_days[month - 1] + (month == 2 && leap);
  if (day > max_day)
    return 0;

  int adjusted_year = year - (month <= 2);
  const int era = adjusted_year / 400;
  const unsigned year_of_era = (unsigned)(adjusted_year - era * 400);
  const unsigned adjusted_month = (unsigned)(month + (month > 2 ? -3 : 9));
  const unsigned day_of_year = (153 * adjusted_month + 2) / 5 +
                               (unsigned)day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                              year_of_era / 100 + day_of_year;
  const long long days = (long long)era * 146097 + day_of_era - 719468;
  *out_time = (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
  return 1;
}

int contest_rules_validate_target_qso(const ContestDefinition *definition,
                                      const char *contest_name, time_t utc_time,
                                      int frequency_khz, const char *mode,
                                      int power_watts, char *error_text,
                                      size_t error_size) {
  if (error_text && error_size > 0)
    error_text[0] = 0;
  (void)contest_name;
  if (definition && definition->enforce_time_window) {
    time_t start_time = 0;
    time_t end_time = 0;
    if (!parse_utc_timestamp(definition->start_utc, &start_time) ||
        !parse_utc_timestamp(definition->end_utc, &end_time) ||
        end_time <= start_time)
      return reject(error_text, error_size,
                    "Contest UTC time window is not configured correctly");
    if (utc_time < start_time || utc_time > end_time)
      return reject(error_text, error_size,
                    "QSO time is outside the configured contest window");
  }

  if (!definition || !definition->validate_operating_rules)
    return 1;

  if (definition->allowed_modes[0] &&
      !list_contains_ci(definition->allowed_modes, mode))
    return reject(error_text, error_size, "Mode is not allowed by contest rules");

  char band[8] = {0};
  detect_band(frequency_khz, band);
  if (definition->allowed_bands[0] &&
      !list_contains_ci(definition->allowed_bands, band))
    return reject(error_text, error_size, "Band is not allowed by contest rules");

  for (int i = 0; i < definition->excluded_segment_count; i++) {
    const ContestExcludedSegment *segment = &definition->excluded_segments[i];
    if (frequency_khz >= segment->low_khz &&
        frequency_khz <= segment->high_khz &&
        (segment->mode[0] == 0 || strcasecmp(segment->mode, mode) == 0))
      return reject(error_text, error_size,
                    "Frequency is in a contest-excluded segment");
  }

  if (definition->category_band[0] &&
      strcasecmp(definition->category_band, "ALL") != 0) {
    char category_band[8] = {0};
    snprintf(category_band, sizeof(category_band), "%s",
             definition->category_band);
    for (size_t i = 0; category_band[i]; i++)
      category_band[i] = (char)toupper((unsigned char)category_band[i]);
    if (strcasecmp(category_band, band) != 0)
      return reject(error_text, error_size,
                    "QSO is outside the selected single-band category");
  }
  if (definition->category_power[0]) {
    const char *power = definition->category_power;
    if (power_watts <= 0)
      return reject(error_text, error_size,
                    "Set STATION_TX_POWER_WATTS before logging this contest");
    if (strcasecmp(power, "QRP") != 0 && strcasecmp(power, "LOW") != 0 &&
        strcasecmp(power, "HIGH") != 0)
      return reject(error_text, error_size,
                    "Unknown power category; use QRP, LOW or HIGH");
    if ((strcasecmp(power, "QRP") == 0 && power_watts > 5) ||
        (strcasecmp(power, "LOW") == 0 && power_watts > 100) ||
        (strcasecmp(power, "LOW") == 0 && power_watts <= 0) ||
        (strcasecmp(power, "HIGH") == 0 && power_watts <= 100) ||
        (definition->max_power_watts > 0 &&
         power_watts > definition->max_power_watts))
      return reject(error_text, error_size,
                    "TX power does not match the selected contest category");
  }
  return 1;
}

#ifdef LOGGER_TESTING
static time_t test_time_utc = (time_t)-1;

void contest_rules_set_test_time(time_t utc_time) { test_time_utc = utc_time; }
#endif

time_t contest_rules_now_utc(void) {
#ifdef LOGGER_TESTING
  if (test_time_utc != (time_t)-1)
    return test_time_utc;
#endif
  return time(NULL);
}