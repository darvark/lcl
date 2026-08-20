#include "stats.h"

#include "config.h"
#include "qtc.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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

  snprintf(mult_list[mult_count++], sizeof(mult_list[0]), "%s", key);
}

static int is_sp_callsign(const char *call) {
  const CtyEntry *cty = cty_lookup(call);
  if (!cty || !cty->country[0])
    return 0;
  return strcmp(cty->country, "Poland") == 0;
}

static int build_callsign_prefix(const char *call, char *out,
                                 size_t out_size) {
  if (!out || out_size < 2)
    return 0;

  out[0] = 0;
  if (!call || !call[0])
    return 0;

  char clean[32] = {0};
  size_t clean_len = 0;
  for (size_t i = 0; call[i] && clean_len < sizeof(clean) - 1; i++) {
    unsigned char ch = (unsigned char)call[i];
    if (isalnum(ch))
      clean[clean_len++] = (char)toupper(ch);
  }

  if (clean_len == 0)
    return 0;

  size_t end = 0;
  for (size_t i = 0; i < clean_len; i++) {
    end = i + 1;
    if (isdigit((unsigned char)clean[i]))
      break;
  }

  if (end == 0)
    return 0;

  if (end >= out_size)
    end = out_size - 1;

  memcpy(out, clean, end);
  out[end] = 0;
  return out[0] != 0;
}

/*
 * Build multiplier key(s) for one QSO according to active contest rules.
 * Keys are appended as candidates and deduplicated after full scan.
 */
static void maybe_add_multiplier(const QSO *q, int own_is_sp) {
  if (!q)
    return;

  char key[96] = {0};
  switch (scoring_def.multiplier_type) {
  case CONTEST_MULT_NONE:
    return;
  case CONTEST_MULT_DXCC_PER_BAND:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "%s|%s", q->band, q->country);
    break;
  case CONTEST_MULT_ZONE_PER_BAND:
    if (q->cq_zone <= 0)
      return;
    snprintf(key, sizeof(key), "%s|%d", q->band, q->cq_zone);
    break;
  case CONTEST_MULT_ZONE:
    if (q->cq_zone <= 0)
      return;
    snprintf(key, sizeof(key), "%d", q->cq_zone);
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
  case CONTEST_MULT_DXCC_PLUS_ZONE_PER_BAND:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "D|%s|%s", q->band, q->country);
    mult_add(key);
    if (q->cq_zone > 0) {
      snprintf(key, sizeof(key), "Z|%s|%d", q->band, q->cq_zone);
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

      char exch[32] = {0};
      snprintf(exch, sizeof(exch), "%s", q->exchange_recv);
      for (size_t i = 0; exch[i]; i++)
        exch[i] = (char)toupper((unsigned char)exch[i]);

      snprintf(key, sizeof(key), "V|%s|%s", q->band, exch);
    }
    break;
  }
  case CONTEST_MULT_DXCC:
  default:
    if (!q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
      return;
    snprintf(key, sizeof(key), "%s", q->country);
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
}

void stats_set_contest_definition(const ContestDefinition *definition) {
  contest_definition_init_defaults(&scoring_def);
  if (!definition)
    return;

  scoring_def = *definition;
}

/*
 * Recalculate aggregate logbook statistics from the in-memory QSO list.
 *
 * @return Nothing.
 */
void stats_update(void) {
  reset_stats();
  const int own_is_sp = (scoring_def.multiplier_type == CONTEST_MULT_SPDX)
                            ? is_sp_callsign(config.station_call)
                            : 0;

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

    if (q->points > 0)
      stats.contest_qso_points += q->points;
    else
      stats.contest_qso_points += scoring_def.points_per_qso;

    maybe_add_multiplier(q, own_is_sp);
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

  if (scoring_def.multiplier_type == CONTEST_MULT_NONE) {
    stats.contest_score = stats.contest_qso_points + stats.qtc_points +
                          scoring_def.bonus_points;
  } else {
    const int mult = stats.contest_mults > 0 ? stats.contest_mults : 1;
    stats.contest_score = (stats.contest_qso_points + stats.qtc_points) * mult +
                          scoring_def.bonus_points;
  }
}
