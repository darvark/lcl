#include "stats.h"

#include "config.h"
#include "contest_rules.h"
#include "cty.h"
#include "maidenhead.h"
#include "qtc.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
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

static int extract_locator_fragment(const char *text, char *out,
                                    size_t out_size) {
  if (!text || !text[0] || !out || out_size < 5)
    return 0;

  out[0] = 0;
  size_t len = strlen(text);
  for (size_t width = 6; width >= 4; width -= 2) {
    if (len < width)
      continue;

    for (size_t start = 0; start + width <= len; start++) {
      char candidate[16] = {0};
      memcpy(candidate, text + start, width);
      candidate[width] = 0;
      if (locator_is_valid(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 1;
      }
    }

    if (width == 4)
      break;
  }

  return 0;
}

Statistics stats;

/* prosta lista DXCC (bez hashmapy – Etap 3) */
static char dxcc_list[MAX_QSO][64];
static int dxcc_count = 0;
static ContestDefinition scoring_def;

/*
 * Cache for multiplier keys (e.g. DXCC, BAND+DXCC).
 */
static char mult_list[MAX_QSO * 2][96];
static int mult_count = 0;
static int active_multiplier_bucket = 0;

/*
 * Add a DXCC country candidate to the current set.
 * Deduplication is handled in a batch at the end of stats_update.
 *
 * @param country Country name to add.
 * @return Nothing.
 */
static void dxcc_add(const char *country) {
  if (!country || !country[0])
    return;
  if (strcmp(country, "UNKNOWN") == 0)
    return;
  if (dxcc_count >= MAX_QSO)
    return;

  snprintf(dxcc_list[dxcc_count++], sizeof(dxcc_list[0]), "%s", country);
}

/* qsort comparator for fixed-size string buffers. */
static int compare_fixed_strings(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

/*
 * Sort and deduplicate a list of fixed-size keys in-place.
 *
 * @param list List of fixed-size key buffers.
 * @param count Number of valid entries.
 * @return Number of unique keys after deduplication.
 */
static int unique_sorted_string_count(char list[][96], int count) {
  if (count <= 0)
    return 0;

  qsort(list, (size_t)count, sizeof(list[0]), compare_fixed_strings);

  int write = 1;
  for (int i = 1; i < count; i++) {
    if (strcmp(list[i], list[write - 1]) != 0) {
      if (write != i)
        snprintf(list[write], sizeof(list[0]), "%s", list[i]);
      write++;
    }
  }

  return write;
}

/*
 * Sort and deduplicate DXCC list collected during stats scan.
 *
 * @return Number of unique DXCC entities.
 */
static int unique_sorted_dxcc_count(void) {
  if (dxcc_count <= 0)
    return 0;

  qsort(dxcc_list, (size_t)dxcc_count, sizeof(dxcc_list[0]),
        compare_fixed_strings);

  int write = 1;
  for (int i = 1; i < dxcc_count; i++) {
    if (strcmp(dxcc_list[i], dxcc_list[write - 1]) != 0) {
      if (write != i)
        snprintf(dxcc_list[write], sizeof(dxcc_list[0]), "%s", dxcc_list[i]);
      write++;
    }
  }

  return write;
}

/* Append one multiplier candidate key (dedup happens in batch later). */
static void mult_add(const char *key) {
  if (!key || !key[0])
    return;
  if (mult_count >= (int)(sizeof(mult_list) / sizeof(mult_list[0])))
    return;

  snprintf(mult_list[mult_count++], sizeof(mult_list[0]), "F%02d|%s",
           active_multiplier_bucket, key);
}

static int is_sp_callsign(const char *call) {
  const CtyEntry *cty = cty_lookup(call);
  if (!cty || !cty->country[0])
    return 0;
  return strcmp(cty->country, "Poland") == 0;
}

static int station_exchange_is_sp_province(void) {
  static const char provinces[] = "BCDFGJKLMOPRSUWZ";
  return strlen(config.station_exchange) == 1 &&
         strchr(provinces,
                toupper((unsigned char)config.station_exchange[0])) != NULL;
}

static int wag_station_is_german(void) {
  if (scoring_def.station_exchange_region[0])
    return contest_definition_station_in_region(
        &scoring_def, scoring_def.station_exchange_region, config.station_call,
        config.station_exchange);
  if (config.station_exchange[0])
    return 1;
  const CtyEntry *station = cty_lookup(config.station_call);
  return station && strcmp(station->prefix, "DL") == 0;
}

static int wag_qso_is_german(const QSO *q) {
  const CtyEntry *worked = cty_lookup(q->call);
  if (worked && strcmp(worked->prefix, "DL") == 0)
    return 1;

  char country[sizeof(q->country)] = {0};
  snprintf(country, sizeof(country), "%s", q->country);
  for (size_t i = 0; country[i]; i++)
    country[i] = (char)toupper((unsigned char)country[i]);
  return strstr(country, "GERMANY") != NULL;
}

static const char *wag_special_dxcc_prefix(const char *call) {
  if (!call)
    return NULL;
  if (strncasecmp(call, "IG9", 3) == 0)
    return "IG9";
  if (strncasecmp(call, "IH9", 3) == 0)
    return "IH9";
  const CtyEntry *worked = cty_lookup(call);
  if (!worked)
    return NULL;
  if (strcmp(worked->prefix, "IG9") == 0 || strcmp(worked->prefix, "IH9") == 0)
    return worked->prefix;
  return NULL;
}

static int build_callsign_prefix(const char *call, char *out,
                                 size_t out_size) {
  if (!out || out_size < 2)
    return 0;

  out[0] = 0;
  if (!call || !call[0])
    return 0;

  char parts[4][32] = {{0}};
  int part_count = 0;
  size_t part_len = 0;
  for (size_t i = 0;; i++) {
    const unsigned char ch = (unsigned char)call[i];
    if (ch == '/' || ch == 0) {
      if (part_len > 0 && part_count < 4) {
        parts[part_count][part_len] = 0;
        part_count++;
      }
      part_len = 0;
      if (ch == 0)
        break;
      continue;
    }
    if (isalnum(ch) && part_len + 1 < sizeof(parts[0]))
      parts[part_count < 4 ? part_count : 3][part_len++] =
          (char)toupper(ch);
  }
  if (part_count == 0)
    return 0;

  static const char *ignored_suffixes[] = {
      "A", "E", "J", "M", "MM", "AM", "P", "QRP", NULL};
  int base_part = -1;
  for (int i = 0; i < part_count && base_part < 0; i++) {
    for (size_t j = 0; parts[i][j]; j++) {
      if (isdigit((unsigned char)parts[i][j])) {
        base_part = i;
        break;
      }
    }
  }
  if (base_part < 0)
    base_part = 0;

  int prefix_part = base_part;
  if (base_part > 0) {
    prefix_part = 0;
    for (int i = 0; i < base_part; i++) {
      int ignored = 0;
      for (size_t j = 0; ignored_suffixes[j]; j++)
        if (strcmp(parts[i], ignored_suffixes[j]) == 0)
          ignored = 1;
      if (!ignored && parts[i][0])
        prefix_part = i;
    }
  } else {
    size_t shortest_portable_len = strlen(parts[base_part]);
    for (int i = base_part + 1; i < part_count; i++) {
      int ignored = 0;
      for (size_t j = 0; ignored_suffixes[j]; j++)
        if (strcmp(parts[i], ignored_suffixes[j]) == 0)
          ignored = 1;
      const size_t candidate_len = strlen(parts[i]);
      if (!ignored && parts[i][0] && candidate_len < shortest_portable_len) {
        prefix_part = i;
        shortest_portable_len = candidate_len;
      }
    }
  }

  char prefix[32] = {0};
  size_t prefix_len = 0;
  size_t i = 0;
  if (isdigit((unsigned char)parts[prefix_part][0])) {
    while (isdigit((unsigned char)parts[prefix_part][i]))
      prefix[prefix_len++] = parts[prefix_part][i++];
    while (isalpha((unsigned char)parts[prefix_part][i]))
      prefix[prefix_len++] = parts[prefix_part][i++];
    while (isdigit((unsigned char)parts[prefix_part][i]))
      prefix[prefix_len++] = parts[prefix_part][i++];
  } else {
    while (isalpha((unsigned char)parts[prefix_part][i]))
      prefix[prefix_len++] = parts[prefix_part][i++];
    while (isdigit((unsigned char)parts[prefix_part][i]))
      prefix[prefix_len++] = parts[prefix_part][i++];
  }
  if (prefix_len == 0)
    return 0;

  int has_digit = 0;
  for (size_t i = 0; i < prefix_len; i++)
    has_digit |= isdigit((unsigned char)prefix[i]) != 0;
  if (!has_digit) {
    size_t letters = 0;
    while (letters < prefix_len && isalpha((unsigned char)prefix[letters]))
      letters++;
    prefix_len = letters < 2 ? letters : 2;
    prefix[prefix_len++] = '0';
  }

  if (prefix_len >= out_size)
    prefix_len = out_size - 1;
  memcpy(out, prefix, prefix_len);
  out[prefix_len] = 0;
  return out[0] != 0;
}

static int spdx_province_letter(const char *exchange, char *out,
                                size_t out_size) {
  static const char provinces[] = "BCDFGJKLMOPRSUWZ";
  if (!exchange || !exchange[0] || !out || out_size < 2)
    return 0;
  for (size_t i = 0; exchange[i]; i++) {
    const char ch = (char)toupper((unsigned char)exchange[i]);
    if (strchr(provinces, ch)) {
      out[0] = ch;
      out[1] = 0;
      return 1;
    }
  }
  return 0;
}

static const char *cqww_special_entity(const char *call) {
  if (!call)
    return NULL;
  if (strncasecmp(call, "IG9", 3) == 0)
    return "IG9";
  if (strncasecmp(call, "IH9", 3) == 0)
    return "IH9";
  const CtyEntry *worked = cty_lookup(call);
  if (worked && (strcmp(worked->prefix, "IG9") == 0 ||
                 strcmp(worked->prefix, "IH9") == 0))
    return worked->prefix;
  return NULL;
}

static int qso_exchange_zone(const QSO *q) {
  if (!q)
    return 0;
  char *end = NULL;
  const long zone = strtol(q->exchange_recv, &end, 10);
  if (q->exchange_recv[0] && end != q->exchange_recv && *end == 0 &&
      zone >= 1 && zone <= 90)
    return (int)zone;
  return q->cq_zone;
}

static int call_has_configured_suffix(const char *call, const char *suffixes) {
  if (!call || !suffixes || !suffixes[0])
    return 0;
  char copy[96];
  snprintf(copy, sizeof(copy), "%s", suffixes);
  char *save = NULL;
  const size_t call_length = strlen(call);
  for (char *suffix = strtok_r(copy, ",", &save); suffix;
       suffix = strtok_r(NULL, ",", &save)) {
    trim_in_place(suffix);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length <= call_length &&
        strcasecmp(call + call_length - suffix_length, suffix) == 0)
      return 1;
  }
  return 0;
}

static int configured_multiplier_prefix(const QSO *q, int family, char *out,
                                       size_t out_size) {
  if (!q || family < 0 || family >= CONTEST_DEF_MAX_MULTIPLIERS)
    return 0;
  const char *prefixes = scoring_def.multiplier_special_prefixes[family];
  if (!prefixes[0])
    return 0;
  char copy[128];
  snprintf(copy, sizeof(copy), "%s", prefixes);
  char *save = NULL;
  for (char *prefix = strtok_r(copy, ",", &save); prefix;
       prefix = strtok_r(NULL, ",", &save)) {
    trim_in_place(prefix);
    if (strncasecmp(q->call, prefix, strlen(prefix)) == 0) {
      snprintf(out, out_size, "%s", prefix);
      return 1;
    }
  }
  return 0;
}

/*
 * Build multiplier key(s) for one QSO according to active contest rules.
 * Keys are appended as candidates and deduplicated after full scan.
 */
static void maybe_add_multiplier(const QSO *q, int own_is_sp,
                                 ContestMultiplierType multiplier_type,
                                 int family_index) {
  if (!q)
    return;

  if (family_index >= 0 && family_index < CONTEST_DEF_MAX_MULTIPLIERS &&
      call_has_configured_suffix(
          q->call, scoring_def.multiplier_excluded_suffixes[family_index]))
    return;

  active_multiplier_bucket = family_index * 2;
  char key[96] = {0};
  char special_prefix[32] = {0};
  const int has_special_prefix = configured_multiplier_prefix(
      q, family_index, special_prefix, sizeof(special_prefix));
  switch (multiplier_type) {
  case CONTEST_MULT_NONE:
    return;
  case CONTEST_MULT_DXCC_PER_BAND:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "%s|%s", q->band,
             has_special_prefix ? special_prefix : q->country);
    break;
  case CONTEST_MULT_ZONE_PER_BAND:
    if (qso_exchange_zone(q) <= 0)
      return;
    snprintf(key, sizeof(key), "%s|%d", q->band, qso_exchange_zone(q));
    break;
  case CONTEST_MULT_ZONE:
    if (qso_exchange_zone(q) <= 0)
      return;
    snprintf(key, sizeof(key), "%d", qso_exchange_zone(q));
    break;
  case CONTEST_MULT_PREFIX: {
    char prefix[32] = {0};
    if (!build_callsign_prefix(q->call, prefix, sizeof(prefix)))
      return;
    snprintf(key, sizeof(key), "%s", prefix);
    break;
  }
  case CONTEST_MULT_PREFIX_PER_BAND: {
    char prefix[32] = {0};
    if (!build_callsign_prefix(q->call, prefix, sizeof(prefix)))
      return;
    snprintf(key, sizeof(key), "%s|%s", q->band, prefix);
    break;
  }
  case CONTEST_MULT_MODE_DXCC:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "%s|%s", q->mode, q->country);
    break;
  case CONTEST_MULT_CQWW: {
    char call_upper[sizeof(q->call)] = {0};
    snprintf(call_upper, sizeof(call_upper), "%s", q->call);
    for (size_t i = 0; call_upper[i]; i++)
      call_upper[i] = (char)toupper((unsigned char)call_upper[i]);
    const size_t call_len = strlen(call_upper);
    const int maritime_mobile =
        (call_len >= 3 && strcmp(call_upper + call_len - 3, "/MM") == 0) ||
        (call_len >= 3 && strcmp(call_upper + call_len - 3, "/AM") == 0);
    const char *special_entity = has_special_prefix
                     ? special_prefix
                     : cqww_special_entity(q->call);
    if (!maritime_mobile && q->country[0] &&
        strcmp(q->country, "UNKNOWN") != 0) {
      snprintf(key, sizeof(key), "C|%s|%s", q->band,
               special_entity ? special_entity : q->country);
      mult_add(key);
    }
    const int zone = qso_exchange_zone(q);
    if (zone > 0) {
      snprintf(key, sizeof(key), "Z|%s|%d", q->band, zone);
      active_multiplier_bucket = family_index * 2 + 1;
      mult_add(key);
    }
    return;
  }
  case CONTEST_MULT_IARU: {
    char exchange[sizeof(q->exchange_recv)] = {0};
    snprintf(exchange, sizeof(exchange), "%s", q->exchange_recv);
    for (size_t i = 0; exchange[i]; i++)
      exchange[i] = (char)toupper((unsigned char)exchange[i]);
    char *end = NULL;
    const long zone = strtol(exchange, &end, 10);
    if (exchange[0] && end != exchange && *end == 0 && zone >= 1 && zone <= 90) {
      snprintf(key, sizeof(key), "Z|%s|%ld", q->band, zone);
    } else if (!exchange[0] && q->itu_zone > 0) {
      snprintf(key, sizeof(key), "Z|%s|%d", q->band, q->itu_zone);
    } else if (exchange[0] && isalpha((unsigned char)exchange[0])) {
      snprintf(key, sizeof(key), "H|%s|%s", q->band, exchange);
    } else {
      return;
    }
    break;
  }
  case CONTEST_MULT_WAG:
    if (wag_station_is_german()) {
      if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
        return;
      const char *special_prefix = wag_special_dxcc_prefix(q->call);
      snprintf(key, sizeof(key), "DX|%s|%s|%s", q->band, q->mode,
               special_prefix ? special_prefix : q->country);
    } else {
      if (!wag_qso_is_german(q) || !q->exchange_recv[0])
        return;

      char exchange[sizeof(q->exchange_recv)] = {0};
      snprintf(exchange, sizeof(exchange), "%s", q->exchange_recv);
      for (size_t i = 0; exchange[i]; i++)
        exchange[i] = (char)toupper((unsigned char)exchange[i]);
      if (strcmp(exchange, "NM") == 0)
        return;

      snprintf(key, sizeof(key), "DOK|%s|%s|%c", q->band, q->mode,
               exchange[0]);
    }
    break;
  case CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "D|%s|%s", q->band, q->country);
    mult_add(key);
    if (qso_exchange_zone(q) > 0) {
      snprintf(key, sizeof(key), "Z|%s|%d", q->band, qso_exchange_zone(q));
      active_multiplier_bucket = family_index * 2 + 1;
      mult_add(key);
    }
    return;
  case CONTEST_MULT_SPDX: {
    const int qso_is_sp = strcmp(q->country, "Poland") == 0;

    if (own_is_sp) {
      if (qso_is_sp || !q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
        return;
      snprintf(key, sizeof(key), "D|%s|%s", q->band, q->country);
    } else {
      if (!qso_is_sp || !q->exchange_recv[0])
        return;
      char province[4] = {0};
      if (!spdx_province_letter(q->exchange_recv, province,
                                sizeof(province)))
        return;
      snprintf(key, sizeof(key), "V|%s|%s", q->band, province);
    }
    break;
  }
  case CONTEST_MULT_GRID_PER_BAND: {
    char locator[16] = {0};
    if (!extract_locator_fragment(q->exchange_recv, locator, sizeof(locator)))
      return;
    snprintf(key, sizeof(key), "%s|%s", q->band, locator);
    break;
  }
  case CONTEST_MULT_CUSTOM_LIST: {
    char source[64] = {0};
    char list_copy[256] = {0};
    char *token = NULL;

    if (q->exchange_recv[0]) {
      snprintf(source, sizeof(source), "%s", q->exchange_recv);
    } else if (q->country[0] && strcmp(q->country, "UNKNOWN") != 0) {
      snprintf(source, sizeof(source), "%s", q->country);
    } else {
      return;
    }

    for (size_t i = 0; source[i]; i++)
      source[i] = (char)toupper((unsigned char)source[i]);

    if (scoring_def.custom_mult_list[0]) {
      snprintf(list_copy, sizeof(list_copy), "%s", scoring_def.custom_mult_list);
      token = strtok(list_copy, ",");
      while (token) {
        char entry[32] = {0};
        snprintf(entry, sizeof(entry), "%s", token);
        for (size_t i = 0; entry[i]; i++)
          entry[i] = (char)toupper((unsigned char)entry[i]);
        trim_in_place(entry);
        if (strcmp(entry, source) == 0) {
          snprintf(key, sizeof(key), "%s", entry);
          break;
        }
        token = strtok(NULL, ",");
      }
      if (!key[0])
        return;
      break;
    }

    if (scoring_def.mult3_field[0]) {
      snprintf(key, sizeof(key), "%s", source);
      break;
    }

    if (q->country[0] && strcmp(q->country, "UNKNOWN") != 0) {
      snprintf(key, sizeof(key), "%s", q->country);
      break;
    }
    return;
  }
  case CONTEST_MULT_DXCC:
  default:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "%s",
         has_special_prefix ? special_prefix : q->country);
    break;
  }

  mult_add(key);
}

/*
 * Reset the cached statistics and DXCC set.
 *
 * @return Nothing.
 */
static void reset_stats(void) {
  memset(&stats, 0, sizeof(stats));
  dxcc_count = 0;
  mult_count = 0;
  active_multiplier_bucket = 0;
}

void stats_set_contest_definition(const ContestDefinition *definition) {
  contest_definition_init_defaults(&scoring_def);
  if (!definition)
    return;

  scoring_def = *definition;
  if (scoring_def.multipliers[0] != scoring_def.multiplier_type) {
    scoring_def.multipliers[0] = scoring_def.multiplier_type;
    scoring_def.multiplier_count = 1;
  }
}

/*
 * Recalculate aggregate logbook statistics from the in-memory QSO list.
 *
 * @return Nothing.
 */
void stats_update(void) {
  reset_stats();
  int has_spdx_multiplier = scoring_def.multiplier_type == CONTEST_MULT_SPDX;
  for (int i = 0; i < scoring_def.multiplier_count; i++)
    has_spdx_multiplier |= scoring_def.multipliers[i] == CONTEST_MULT_SPDX;
  const int own_is_sp = has_spdx_multiplier
      ? (scoring_def.station_exchange_region[0]
             ? contest_definition_station_in_region(
                   &scoring_def, scoring_def.station_exchange_region,
                   config.station_call, config.station_exchange)
             : station_exchange_is_sp_province() ||
                   is_sp_callsign(config.station_call))
      : 0;
  const int family_count = scoring_def.multiplier_count > 0
                               ? scoring_def.multiplier_count : 1;

  for (int i = 0; i < qso_count; i++) {
    QSO *q = &logbook[i];

    if (q->invalid)
      continue;

    stats.total_qso++;

    if (q->country[0])
      dxcc_add(q->country);

    if (strcmp(q->mode, "CW") == 0)
      stats.cw++;
    else if (strcmp(q->mode, "SSB") == 0)
      stats.ssb++;
    else if (strcmp(q->mode, "FT8") == 0)
      stats.ft8++;
    else if (strcmp(q->mode, "FT4") == 0)
      stats.ft4++;
    else if (strcmp(q->mode, "RTTY") == 0)
      stats.rtty++;
    else if (strcmp(q->mode, "PSK31") == 0)
      stats.psk31++;

    stats.contest_qso_points += q->points;

    for (int family = 0; family < family_count; family++) {
      const ContestMultiplierType type = scoring_def.multiplier_count > 0
          ? scoring_def.multipliers[family] : scoring_def.multiplier_type;
      maybe_add_multiplier(q, own_is_sp, type, family);
    }
  }

  dxcc_count = unique_sorted_dxcc_count();
  mult_count = unique_sorted_string_count(mult_list, mult_count);

  stats.total_dxcc = dxcc_count;

  stats.contest_mults = mult_count;

  /* QTC points (WAE and similar contests). */
  if (scoring_def.points_per_qtc > 0) {
    stats.qtc_records = qtc_total_records();
    stats.qtc_points  = stats.qtc_records * scoring_def.points_per_qtc;
  } else {
    stats.qtc_records = 0;
    stats.qtc_points  = 0;
  }

  int has_multiplier = 0;
  long long score_multiplier = 1;
  for (int family = 0; family < family_count; family++) {
    const ContestMultiplierType type = scoring_def.multiplier_count > 0
        ? scoring_def.multipliers[family] : scoring_def.multiplier_type;
    if (type == CONTEST_MULT_NONE)
      continue;
    has_multiplier = 1;
    const int bucket_count = (type == CONTEST_MULT_CQWW ||
                              type == CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND)
                                 ? 2 : 1;
    for (int offset = 0; offset < bucket_count; offset++) {
      const int bucket = family * 2 + offset;
      char prefix[8];
      snprintf(prefix, sizeof(prefix), "F%02d|", bucket);
      int unique_count = 0;
      for (int i = 0; i < mult_count; i++)
        unique_count += strncmp(mult_list[i], prefix, strlen(prefix)) == 0;
      if (unique_count > 0) {
        if (score_multiplier > INT_MAX / unique_count)
          score_multiplier = INT_MAX;
        else
          score_multiplier *= unique_count;
      }
    }
  }

  if (!has_multiplier) {
    stats.contest_score = stats.contest_qso_points + stats.qtc_points +
                          scoring_def.bonus_points;
  } else {
    const long long points =
      (long long)stats.contest_qso_points + stats.qtc_points;
    const long long score =
      points > INT_MAX / score_multiplier
        ? INT_MAX
        : points * score_multiplier + scoring_def.bonus_points;
    stats.contest_score = score > INT_MAX ? INT_MAX : (int)score;
  }
}
