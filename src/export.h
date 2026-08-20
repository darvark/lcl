#ifndef EXPORT_H
#define EXPORT_H

#include "contest.h"

/*
 * Export the current logbook to CSV, using the database layer when available.
 *
 * @param filename Output file path.
 * @return 0 on success, or -1 on failure.
 */
int export_csv(const char *filename);

/*
 * Export the current logbook to ADIF, using the database layer when available.
 *
 * @param filename Output file path.
 * @return 0 on success, or -1 on failure.
 */
int export_adif(const char *filename);

/*
 * Export the current logbook to Cabrillo format.
 *
 * @param filename Output file path.
 * @param definition Contest definition metadata.
 * @param station_call Station callsign used in headers and QSO lines.
 * @return 0 on success, or -1 on failure.
 */
int export_cabrillo(const char *filename, const ContestDefinition *definition,
					const char *station_call);

/*
 * Export the current logbook and QTC bundles to Cabrillo format.
 *
 * Includes QTC: lines when the contest definition enables QTC traffic.
 *
 * @param filename Output file path.
 * @param definition Contest definition metadata.
 * @param station_call Station callsign used in headers and QSO lines.
 * @return 0 on success, or -1 on failure.
 */
int export_cabrillo_with_qtc(const char *filename,
                             const ContestDefinition *definition,
                             const char *station_call);

#endif
