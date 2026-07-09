#ifndef DB_H
#define DB_H

#include "qso.h"

typedef struct {
	long long id;
	char name[64];
	char created_at[32];
	int qso_count;
} DBNamedLogbook;

/*
 * Initialize the SQLite database layer.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_init(void);

/*
 * Close the SQLite database layer and release any cached handles.
 *
 * @return Nothing.
 */
void db_shutdown(void);

/*
 * Load the current logbook QSO rows into memory.
 *
 * @param logbook Destination array for loaded QSOs.
 * @param max_qso Maximum number of rows to load.
 * @param ids Optional destination array for database row ids.
 * @param out_count Optional output count of loaded rows.
 * @return 0 on success, or -1 on failure.
 */
int db_load_qsos(QSO *logbook, int max_qso, long long *ids, int *out_count);

/*
 * Insert a QSO into the current logbook.
 *
 * @param qso QSO row to store.
 * @param out_id Optional destination for the inserted row id.
 * @return 0 on success, or -1 on failure.
 */
int db_insert_qso(const QSO *qso, long long *out_id);

/*
 * Update the invalid flag for a QSO row.
 *
 * @param id Database row id to update.
 * @param invalid Nonzero marks the row invalid, zero clears the flag.
 * @return 0 on success, or -1 on failure.
 */
int db_update_qso_invalid(long long id, int invalid);

/*
 * Update contest-specific fields for a QSO row.
 *
 * @param id Database row id to update.
 * @return 0 on success, or -1 on failure.
 */
int db_update_qso_contest_fields(long long id, const char *exchange_sent,
								 const char *exchange_recv,
								 const char *operator_mode,
								 const char *contest_id, int radio_nr,
								 int points);

/*
 * Load the current logbook call history into memory.
 *
 * @param history Destination buffer for call history entries.
 * @param max_history Maximum number of entries to read.
 * @param out_count Optional output count of loaded entries.
 * @return 0 on success, or -1 on failure.
 */
int db_load_call_history(char history[][32], int max_history, int *out_count);

/*
 * Append a callsign to the current logbook call history table.
 *
 * @param call Callsign to append.
 * @return 0 on success, or -1 on failure.
 */
int db_append_call_history(const char *call);

/*
 * Import call history entries from a text file.
 *
 * @param path Path to the file to import.
 * @return Number of imported entries, or -1 on failure.
 */
int db_import_call_history_file(const char *path);

/*
 * Clear the active logbook's QSO and call-history rows.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_clear_logbook(void);

/*
 * Save the current logbook as the previous logbook reference.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_archive_current_logbook(void);

/*
 * Open the previously archived logbook.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_open_previous_logbook(void);

/*
 * Archive the current logbook under a named entry and switch to a new one.
 *
 * @param name Name to assign to the archived logbook.
 * @return 0 on success, or -1 on failure.
 */
int db_archive_current_logbook_named(const char *name);

/*
 * List named logbooks in descending id order.
 *
 * @param out Destination array for named logbook metadata.
 * @param max_items Maximum number of items to return.
 * @param out_count Optional output count of returned items.
 * @return 0 on success, or -1 on failure.
 */
int db_list_named_logbooks(DBNamedLogbook *out, int max_items, int *out_count);

/*
 * Open a named logbook by database id.
 *
 * @param id Named logbook id to open.
 * @return 0 on success, or -1 on failure.
 */
int db_open_named_logbook_by_id(long long id);

/*
 * Open a named logbook by its stored name.
 *
 * @param name Logbook name to open.
 * @return 0 on success, or -1 on failure.
 */
int db_open_named_logbook_by_name(const char *name);

/*
 * Export the active logbook to CSV.
 *
 * @param filename Destination file path.
 * @return 0 on success, or -1 on failure.
 */
int db_export_csv(const char *filename);

/*
 * Export the active logbook to ADIF.
 *
 * @param filename Destination file path.
 * @return 0 on success, or -1 on failure.
 */
int db_export_adif(const char *filename);

#endif