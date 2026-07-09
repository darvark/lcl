#ifndef CTY_H
#define CTY_H

#include "globals.h"

typedef struct {
  char prefix[16];
  char country[64];

  int cq_zone;
  int itu_zone;

  double lat;
  double lon;

} CtyEntry;

/*
 * Load the CTY prefix database from disk.
 *
 * @param filename Preferred path to the database file.
 * @return Number of parsed entries on success, or -1 on failure.
 */
int cty_load(const char *filename);

/*
 * Download the latest CTY database and save it locally.
 *
 * @param filename Destination filename. If empty or NULL, wl_cty.dat is used.
 * @return 0 on success, or -1 on failure.
 */
int cty_download_latest(const char *filename);

/*
 * Find the best CTY match for a callsign prefix.
 *
 * @param callsign Callsign or prefix to search for.
 * @return Pointer to the best matching entry, or NULL if none is found.
 */
const CtyEntry *cty_lookup(const char *callsign);

#endif
