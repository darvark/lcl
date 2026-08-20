#ifndef STATS_H
#define STATS_H

#include "contest.h"
#include "qso.h"

typedef struct {
  int total_qso;
  int total_dxcc;

  int cw;
  int ssb;
  int ft8;
  int ft4;
  int rtty;
  int psk31;

  int contest_qso_points;
  int contest_mults;
  int contest_score;

  /* QTC-specific counters (non-zero only when contest uses QTC). */
  int qtc_records;  /* total individual QTC records logged */
  int qtc_points;   /* qtc_records * points_per_qtc        */

} Statistics;

extern Statistics stats;

/*
 * Recalculate aggregate logbook statistics from the in-memory QSO list.
 *
 * @return Nothing.
 */
void stats_update(void);

/*
 * Set active contest scoring rules used by stats_update().
 */
void stats_set_contest_definition(const ContestDefinition *definition);

#endif
