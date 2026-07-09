#include "stats.h"

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
static char mult_list[MAX_QSO][96];
static int mult_count = 0;

/*
 * Check whether a DXCC country already exists in the current set.
 *
 * @param country Country name to search for.
 * @return 1 if present, otherwise 0.
 */
static int dxcc_exists(const char *country) {
  for (int i = 0; i < dxcc_count; i++) {
    if (strcmp(dxcc_list[i], country) == 0)
      return 1;
  }
  return 0;
}

/*
 * Add a DXCC country to the current set if it is valid and new.
 *
 * @param country Country name to add.
 * @return Nothing.
 */
static void dxcc_add(const char *country) {
  if (!country || !country[0])
    return;
  if (strcmp(country, "UNKNOWN") == 0)
    return;

  for (int i = 0; i < dxcc_count; i++)
    if (strcmp(dxcc_list[i], country) == 0)
      return;

  strcpy(dxcc_list[dxcc_count++], country);
}

static int mult_exists(const char *key) {
  for (int i = 0; i < mult_count; i++) {
    if (strcmp(mult_list[i], key) == 0)
      return 1;
  }

  return 0;
}

static void maybe_add_multiplier(const QSO *q) {
  if (!q || !q->country[0] || strcmp(q->country, "UNKNOWN") == 0)
    return;

  char key[96] = {0};
  switch (scoring_def.multiplier_type) {
  case CONTEST_MULT_NONE:
    return;
  case CONTEST_MULT_BAND_DXCC:
    snprintf(key, sizeof(key), "%s|%s", q->band, q->country);
    break;
  case CONTEST_MULT_MODE_DXCC:
    snprintf(key, sizeof(key), "%s|%s", q->mode, q->country);
    break;
  case CONTEST_MULT_DXCC:
  default:
    snprintf(key, sizeof(key), "%s", q->country);
    break;
  }

  if (mult_count >= MAX_QSO)
    return;

  if (!mult_exists(key))
    snprintf(mult_list[mult_count++], sizeof(mult_list[0]), "%s", key);
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

    maybe_add_multiplier(q);
  }

  stats.total_dxcc = dxcc_count;

  stats.contest_mults = mult_count;
  if (scoring_def.multiplier_type == CONTEST_MULT_NONE) {
    stats.contest_score = stats.contest_qso_points + scoring_def.bonus_points;
  } else {
    const int mult = stats.contest_mults > 0 ? stats.contest_mults : 1;
    stats.contest_score =
        stats.contest_qso_points * mult + scoring_def.bonus_points;
  }
}
