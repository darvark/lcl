#ifndef DB_H
#define DB_H

#include <stddef.h>

#include "qso.h"
#include "qtc.h"

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
 * Store the contest definition path associated with the active logbook.
 *
 * @param path Contest file path or empty string to clear the association.
 * @return 0 on success, or -1 on failure.
 */
int db_set_current_logbook_contest_path(const char *path);

/*
 * Read the contest definition path associated with the active logbook.
 *
 * @param out Destination buffer for the path.
 * @param out_size Destination buffer size.
 * @return 0 on success, or -1 on failure.
 */
int db_get_current_logbook_contest_path(char *out, size_t out_size);

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

/*
 * Insert a QTC bundle into the database for the current logbook.
 *
 * @param bundle  Fully populated bundle (db_id will be set on success).
 * @param out_id  Optional destination for the inserted row id.
 * @return 0 on success, or -1 on failure.
 */
int db_insert_qtc_bundle(const QTCBundle *bundle, long long *out_id);

/*
 * Load QTC bundles for the current logbook into memory.
 *
 * @param out        Destination array for loaded bundles.
 * @param max_items  Maximum number of bundles to load.
 * @param out_count  Optional output count of loaded items.
 * @return 0 on success, or -1 on failure.
 */
int db_load_qtc_bundles(QTCBundle *out, int max_items, int *out_count);

typedef struct {
	char op_id[96];
	long long station_seq;
	int logbook_id;
	char op_type[32];
	char entity_id[64];
	char payload_json[2048];
	char op_utc[32];
	int retry_count;
} SyncOutboxEntry;

typedef struct {
	long long global_seq;
	char op_id[96];
	char station_id[32];
	long long station_seq;
	int logbook_id;
	char op_type[32];
	char entity_id[64];
	char payload_json[2048];
	char op_utc[32];
} SyncLogOpEntry;

#define DB_SYNC_APPLY_CHANGED 1
#define DB_SYNC_APPLY_ALREADY_PRESENT 0
#define DB_SYNC_APPLY_ERR (-1)
#define DB_SYNC_APPLY_STATION_SEQ_CONFLICT (-2)
#define DB_SYNC_APPLY_QSO_UID_CONFLICT (-3)

#define DB_SYNC_COMMIT_OK 0
#define DB_SYNC_COMMIT_NOT_FOUND 1
#define DB_SYNC_COMMIT_ERR (-1)

/*
 * Ensure and return local station id used by synchronization layer.
 *
 * @param out Destination buffer for station id.
 * @param out_size Destination buffer size.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_get_or_create_station_id(char *out, size_t out_size);

/*
 * Persist explicit station id for this installation.
 */
int db_sync_set_station_id(const char *station_id);

/*
 * Allocate next local station sequence number for outbound operations.
 *
 * @param out_seq Destination for next sequence number.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_next_station_seq(long long *out_seq);

/*
 * Enqueue a synchronization operation into local outbox.
 *
 * @param op_id Stable unique operation identifier.
 * @param station_seq Local monotonic sequence number.
 * @param logbook_id Active logbook id.
 * @param op_type Operation type label.
 * @param entity_id Entity identifier (for example qso_uid).
 * @param payload_json JSON payload string.
 * @param op_utc UTC timestamp string.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_outbox_enqueue(const char *op_id, long long station_seq,
						  int logbook_id, const char *op_type,
						  const char *entity_id, const char *payload_json,
						  const char *op_utc);

/*
 * Load a batch of pending/sent but unacked outbox operations.
 *
 * @param out Destination array.
 * @param max_items Capacity of destination array.
 * @param out_count Number of loaded items.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_outbox_load_pending(SyncOutboxEntry *out, int max_items,
								int *out_count);

/*
 * Mark an outbox operation as sent.
 *
 * @param op_id Operation identifier.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_outbox_mark_sent(const char *op_id);

/*
 * Return operation to pending state and schedule next retry attempt.
 *
 * @param op_id Operation identifier.
 * @param delay_seconds Delay before next retry in seconds.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_outbox_mark_retry(const char *op_id, int delay_seconds);

/*
 * Mark an outbox operation as acknowledged.
 *
 * @param op_id Operation identifier.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_outbox_mark_acked(const char *op_id);

/*
 * Count pending outbox operations.
 *
 * @param out_count Destination for pending count.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_get_pending_outbox_count(int *out_count);

/*
 * Count outbox operations marked as permanently failed.
 *
 * @param out_count Destination for failed count.
 * @return 0 on success, or -1 on failure.
 */
int db_sync_get_failed_outbox_count(int *out_count);

/*
 * Read and persist last pulled global sequence cursor.
 */
int db_get_current_logbook_id(int *out_id);
int db_sync_get_last_global_seq(long long *out_seq);
int db_sync_set_last_global_seq(long long seq);

/*
 * Read max global sequence currently present in log_ops.
 */
int db_sync_get_max_global_seq(long long *out_seq);

/*
 * Read next expected station sequence on server side for given station.
 */
int db_sync_get_next_expected_station_seq(const char *station_id,
						      long long *out_seq);

/*
 * Apply one remote synchronization operation idempotently.
 *
 * @return DB_SYNC_APPLY_CHANGED when operation changed state,
 *         DB_SYNC_APPLY_ALREADY_PRESENT when already applied,
 *         DB_SYNC_APPLY_STATION_SEQ_CONFLICT when station_seq is already
 *         owned by a different op_id for the same station_id,
 *         or DB_SYNC_APPLY_ERR on failure.
 */
int db_sync_apply_remote_op(const char *op_id, const char *station_id,
						 long long station_seq, int logbook_id,
						 const char *op_type, const char *entity_id,
						 const char *payload_json,
						 const char *op_utc,
						 long long *out_global_seq);

/*
 * Load a batch of applied operations after a global sequence cursor.
 */
int db_sync_pull_ops(long long from_global_seq, int limit, SyncLogOpEntry *out,
				 int max_items, int *out_count,
				 long long *out_last_global_seq);

/*
 * Reserve and commit central serial numbers.
 */
int db_sync_reserve_serial(int logbook_id, const char *station_id,
			   const char *request_id, int ttl_sec,
			   char *out_reservation_id, size_t out_reservation_id_size,
			   int *out_serial, char *out_expires_utc,
			   size_t out_expires_utc_size);

/*
 * Commit reservation after QSO write on the client side.
 *
 * @return DB_SYNC_COMMIT_OK when committed,
 *         DB_SYNC_COMMIT_NOT_FOUND when reservation is missing/expired,
 *         or DB_SYNC_COMMIT_ERR on database error.
 */
int db_sync_commit_serial(const char *reservation_id, const char *qso_uid);
int db_sync_expire_serial_reservations(void);

#endif