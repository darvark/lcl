#ifndef LIVE_UPLOAD_H
#define LIVE_UPLOAD_H

#include "qso.h"
#include "stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Publish one real-time update containing last QSO details and current score.
 *
 * Returns 0 on success (or when upload is disabled), -1 on transport error.
 */
int live_upload_publish_qso_and_stats(const QSO *q, const Statistics *stats,
                                      const char *contest_name);

#ifdef __cplusplus
}
#endif

#endif
