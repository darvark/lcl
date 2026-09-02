#ifndef SCP_H
#define SCP_H

#define SCP_MAX_CALLS  65000
#define SCP_CALL_LEN   16
#define SCP_SEARCH_MAX 60

/*
 * Load the Super Check Partial database from a .scp file.
 *
 * @param path  Path to MASTER.SCP (or NULL / empty to use "MASTER.SCP").
 * @return Number of callsigns loaded, or -1 on failure.
 */
int scp_load(const char *path);

/*
 * Return the number of callsigns currently loaded in the database.
 */
int scp_count(void);

/*
 * Search for callsigns that contain @p partial as a substring (case-insensitive).
 *
 * @param partial   Partial callsign to search for (must be non-NULL).
 * @param results   Output array; each entry is a NUL-terminated string.
 * @param max       Maximum number of results to return (capped at SCP_SEARCH_MAX).
 * @return Number of matches found (may be larger than @p max if results were truncated).
 */
int scp_search(const char *partial, char results[][SCP_CALL_LEN], int max);

/*
 * Download the latest MASTER.SCP from supercheckpartial.com and save locally.
 *
 * @param filename  Destination filename (NULL or empty → "MASTER.SCP").
 * @return 0 on success, -1 on failure.
 */
int scp_download_latest(const char *filename);

#endif /* SCP_H */
