#include "db.h"

#include "qtc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_TRANSIENT ((void (*)(void *))-1)

extern int sqlite3_open(const char *filename, sqlite3 **ppDb);
extern int sqlite3_close(sqlite3 *db);
extern int sqlite3_busy_timeout(sqlite3 *db, int ms);
extern int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);
extern int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
extern int sqlite3_step(sqlite3_stmt *stmt);
extern int sqlite3_finalize(sqlite3_stmt *stmt);
extern int sqlite3_column_int(sqlite3_stmt *stmt, int iCol);
extern long long sqlite3_column_int64(sqlite3_stmt *stmt, int iCol);
extern const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol);
extern int sqlite3_bind_text(sqlite3_stmt *stmt, int idx, const char *value, int n, void (*destructor)(void *));
extern int sqlite3_bind_int(sqlite3_stmt *stmt, int idx, int value);
extern int sqlite3_bind_int64(sqlite3_stmt *stmt, int idx, long long value);
extern long long sqlite3_last_insert_rowid(sqlite3 *db);
extern void sqlite3_free(void *ptr);
extern int sqlite3_reset(sqlite3_stmt *stmt);
extern int sqlite3_clear_bindings(sqlite3_stmt *stmt);

static sqlite3 *db = NULL;
static char db_path[512] = {0};
static int db_initialized = 0;
static int db_is_default_path = 1;
static int db_bootstrap_import_done = 0;

static int table_is_empty(const char *table);
static int table_has_column(const char *table, const char *column);
static int meta_get_int(const char *key, int *value);
static int meta_set_int(const char *key, int value);
static int meta_get_previous_log_available(int *value);
static int copy_table(const char *src, const char *dst, const char *columns);
static int exec_sql_checked(const char *sql);
static int named_logbook_exists(long long id);
static int prepare_stmt(sqlite3_stmt **stmt, const char *sql);
static int get_current_logbook_id(int *out_id);
static int set_current_logbook_id(int id);
static int set_previous_logbook_id(int id);
static int get_previous_logbook_id(int *out_id);
static int ensure_logbook_context(void);
static int get_named_logbook_contest_path(int logbook_id, char *out, size_t out_size);
static int set_named_logbook_contest_path(int logbook_id, const char *path);

/*
 * Bind a text value or SQL NULL-equivalent empty string.
 *
 * @param stmt SQLite statement to bind into.
 * @param idx Parameter index.
 * @param value Text value to bind, or empty/NULL for an empty string.
 * @return SQLite status code from sqlite3_bind_text.
 */
static int bind_text_or_null(sqlite3_stmt *stmt, int idx, const char *value) {
  if (!value || !value[0])
    return sqlite3_bind_text(stmt, idx, "", -1, SQLITE_TRANSIENT);

  return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
}

/*
 * Check whether a table has a specific column.
 *
 * @param table Table name.
 * @param column Column name.
 * @return 1 if the column exists, otherwise 0.
 */
static int table_has_column(const char *table, const char *column) {
  if (!table || !column || !table[0] || !column[0])
    return 0;

  char sql[256];
  snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, sql) != SQLITE_OK)
    return 0;

  int found = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    if (name && strcmp(name, column) == 0) {
      found = 1;
      break;
    }
  }

  sqlite3_finalize(stmt);
  return found;
}

/*
 * Read the current logbook id from app metadata.
 *
 * @param out_id Destination for the logbook id.
 * @return 0 on success, or -1 on failure.
 */
static int get_current_logbook_id(int *out_id) {
  if (!out_id)
    return -1;

  return meta_get_int("current_logbook_id", out_id);
}

static int get_named_logbook_contest_path(int logbook_id, char *out,
                                         size_t out_size) {
  if (!out || out_size == 0)
    return -1;

  out[0] = 0;
  if (logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT contest_definition_path FROM named_logbooks WHERE id = ? LIMIT 1;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);
  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *path = sqlite3_column_text(stmt, 0);
    if (path) {
      snprintf(out, out_size, "%s", (const char *)path);
      rc = 0;
    }
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int set_named_logbook_contest_path(int logbook_id, const char *path) {
  if (logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE named_logbooks SET contest_definition_path = ? WHERE id = ?;") != SQLITE_OK)
    return -1;

  bind_text_or_null(stmt, 1, path);
  sqlite3_bind_int(stmt, 2, logbook_id);
  int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(stmt);
  return rc;
}

/*
 * Store the current logbook id in app metadata.
 *
 * @param id Logbook id to store.
 * @return 0 on success, or -1 on failure.
 */
static int set_current_logbook_id(int id) {
  if (id <= 0)
    return -1;

  return meta_set_int("current_logbook_id", id);
}

/*
 * Read the previous logbook id from app metadata.
 *
 * @param out_id Destination for the logbook id.
 * @return 0 on success, or -1 on failure.
 */
static int get_previous_logbook_id(int *out_id) {
  if (!out_id)
    return -1;

  return meta_get_int("previous_logbook_id", out_id);
}

/*
 * Store the previous logbook id in app metadata.
 *
 * @param id Logbook id to store.
 * @return 0 on success, or -1 on failure.
 */
static int set_previous_logbook_id(int id) {
  if (id <= 0)
    return -1;

  return meta_set_int("previous_logbook_id", id);
}

/*
 * Ensure that a valid active logbook exists.
 *
 * @return 0 on success, or -1 on failure.
 */
static int ensure_logbook_context(void) {
  int current_id = 0;

  sqlite3_stmt *count_stmt = NULL;
  if (prepare_stmt(&count_stmt, "SELECT COUNT(*) FROM named_logbooks;") !=
      SQLITE_OK)
    return -1;

  int logs_count = 0;
  if (sqlite3_step(count_stmt) == SQLITE_ROW)
    logs_count = sqlite3_column_int(count_stmt, 0);
  sqlite3_finalize(count_stmt);

  if (logs_count <= 0) {
    sqlite3_stmt *insert_stmt = NULL;
    if (prepare_stmt(&insert_stmt,
                     "INSERT INTO named_logbooks (name, created_at) VALUES ('Default Log', CURRENT_TIMESTAMP);") !=
        SQLITE_OK)
      return -1;

    if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
      sqlite3_finalize(insert_stmt);
      return -1;
    }

    sqlite3_finalize(insert_stmt);
    current_id = (int)sqlite3_last_insert_rowid(db);
    if (set_current_logbook_id(current_id) != 0)
      return -1;
    return 0;
  }

  if (get_current_logbook_id(&current_id) == 0 && current_id > 0 &&
      named_logbook_exists(current_id))
    return 0;

  sqlite3_stmt *latest_stmt = NULL;
  if (prepare_stmt(&latest_stmt,
                   "SELECT id FROM named_logbooks ORDER BY id ASC LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(latest_stmt) == SQLITE_ROW)
    current_id = sqlite3_column_int(latest_stmt, 0);

  sqlite3_finalize(latest_stmt);

  if (current_id <= 0)
    return -1;

  return set_current_logbook_id(current_id);
}

/*
 * Execute a raw SQL statement against the active database.
 *
 * @param sql SQL text to execute.
 * @return SQLite status code.
 */
static int exec_sql(const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (err) {
    sqlite3_free(err);
  }
  return rc;
}

/*
 * Execute a SQL statement and normalize success to 0, failure to -1.
 *
 * @param sql SQL text to execute.
 * @return 0 on success, or -1 on failure.
 */
static int exec_sql_checked(const char *sql) {
  return exec_sql(sql) == SQLITE_OK ? 0 : -1;
}

/*
 * Prepare a SQLite statement against the active database.
 *
 * @param stmt Destination statement handle.
 * @param sql SQL text to prepare.
 * @return SQLite status code.
 */
static int prepare_stmt(sqlite3_stmt **stmt, const char *sql) {
  return sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
}

/*
 * Import call-history text lines into the active logbook.
 *
 * @param path Path to the call-history text file.
 * @return Number of imported entries, or -1 on failure.
 */
static int import_call_history_file_impl(const char *path) {
  int logbook_id = 1;

  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    logbook_id = 1;

  FILE *f = fopen(path, "r");
  if (!f)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO call_history (logbook_id, call) VALUES (?, ?);") !=
      SQLITE_OK) {
    fclose(f);
    return -1;
  }

  char line[128];
  int imported = 0;

  exec_sql("BEGIN;");

  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[n - 1] = 0;
      n--;
    }

    for (size_t i = 0; line[i]; i++)
      line[i] = (char)toupper((unsigned char)line[i]);

    if (!line[0])
      continue;

    sqlite3_bind_int(stmt, 1, logbook_id);
    sqlite3_bind_text(stmt, 2, line, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE)
      imported++;
    else if (rc != SQLITE_ROW && rc != SQLITE_DONE)
      break;

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3_finalize(stmt);
  fclose(f);
  exec_sql("COMMIT;");

  return imported;
}

/*
 * Open the SQLite database and create required schema on demand.
 *
 * @return 0 on success, or -1 on failure.
 */
static int ensure_open(void) {
  if (db)
    return 0;

  const char *env_path = getenv("LOGGER_DB_PATH");
  const char *path = (env_path && env_path[0]) ? env_path : "logger.db";

  snprintf(db_path, sizeof(db_path), "%s", path);
  db_is_default_path = !(env_path && env_path[0]);

  if (sqlite3_open(db_path, &db) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
      db = NULL;
    }
    return -1;
  }

  sqlite3_busy_timeout(db, 2000);
  exec_sql("PRAGMA foreign_keys = ON;");

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS qso ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "logbook_id INTEGER NOT NULL DEFAULT 1,"
          "date TEXT NOT NULL,"
          "utc TEXT NOT NULL,"
          "call TEXT NOT NULL,"
          "freq INTEGER NOT NULL,"
          "band TEXT NOT NULL,"
          "mode TEXT NOT NULL,"
          "rst TEXT NOT NULL,"
          "comments TEXT NOT NULL DEFAULT '',"
          "exchange_sent TEXT NOT NULL DEFAULT '',"
          "exchange_recv TEXT NOT NULL DEFAULT '',"
          "operator_mode TEXT NOT NULL DEFAULT '',"
          "contest_id TEXT NOT NULL DEFAULT '',"
          "radio_nr INTEGER NOT NULL DEFAULT 1,"
          "points INTEGER NOT NULL DEFAULT 1,"
          "country TEXT NOT NULL,"
          "cq_zone INTEGER NOT NULL,"
          "itu_zone INTEGER NOT NULL,"
          "invalid INTEGER NOT NULL DEFAULT 0"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS call_history ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "logbook_id INTEGER NOT NULL DEFAULT 1,"
          "call TEXT NOT NULL"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS previous_qso ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "date TEXT NOT NULL,"
          "utc TEXT NOT NULL,"
          "call TEXT NOT NULL,"
          "freq INTEGER NOT NULL,"
          "band TEXT NOT NULL,"
          "mode TEXT NOT NULL,"
          "rst TEXT NOT NULL,"
          "comments TEXT NOT NULL DEFAULT '',"
          "country TEXT NOT NULL,"
          "cq_zone INTEGER NOT NULL,"
          "itu_zone INTEGER NOT NULL,"
          "invalid INTEGER NOT NULL DEFAULT 0"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS previous_call_history ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "call TEXT NOT NULL"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS app_meta ("
          "key TEXT PRIMARY KEY,"
          "value INTEGER NOT NULL"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS named_logbooks ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "name TEXT NOT NULL,"
          "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS named_qso ("
          "logbook_id INTEGER NOT NULL,"
          "entry_order INTEGER NOT NULL,"
          "date TEXT NOT NULL,"
          "utc TEXT NOT NULL,"
          "call TEXT NOT NULL,"
          "freq INTEGER NOT NULL,"
          "band TEXT NOT NULL,"
          "mode TEXT NOT NULL,"
          "rst TEXT NOT NULL,"
          "comments TEXT NOT NULL DEFAULT '',"
          "country TEXT NOT NULL,"
          "cq_zone INTEGER NOT NULL,"
          "itu_zone INTEGER NOT NULL,"
          "invalid INTEGER NOT NULL DEFAULT 0"
          ");") != SQLITE_OK)
    return -1;

  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS named_call_history ("
          "logbook_id INTEGER NOT NULL,"
          "entry_order INTEGER NOT NULL,"
          "call TEXT NOT NULL"
          ");") != SQLITE_OK)
    return -1;

  /* QTC bundle header table. */
  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS qtc_bundles ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "logbook_id INTEGER NOT NULL DEFAULT 1,"
          "sender_call TEXT NOT NULL DEFAULT '',"
          "receiver_call TEXT NOT NULL DEFAULT '',"
          "bundle_nr INTEGER NOT NULL DEFAULT 1,"
          "record_count INTEGER NOT NULL DEFAULT 0,"
          "sent INTEGER NOT NULL DEFAULT 1"
          ");") != SQLITE_OK)
    return -1;

  /* QTC individual record table. */
  if (exec_sql(
          "CREATE TABLE IF NOT EXISTS qtc_records ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "bundle_id INTEGER NOT NULL,"
          "seq_nr INTEGER NOT NULL DEFAULT 0,"
          "date TEXT NOT NULL DEFAULT '',"
          "time TEXT NOT NULL DEFAULT '',"
          "call TEXT NOT NULL DEFAULT '',"
          "exch TEXT NOT NULL DEFAULT ''"
          ");") != SQLITE_OK)
    return -1;

  if (!table_has_column("named_logbooks", "contest_definition_path")) {
    if (exec_sql_checked("ALTER TABLE named_logbooks ADD COLUMN contest_definition_path TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "logbook_id")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN logbook_id INTEGER NOT NULL DEFAULT 1;") != 0)
      return -1;
  }

  if (!table_has_column("qso", "comments")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN comments TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "exchange_sent")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN exchange_sent TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "exchange_recv")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN exchange_recv TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "operator_mode")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN operator_mode TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "contest_id")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN contest_id TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "radio_nr")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN radio_nr INTEGER NOT NULL DEFAULT 1;") != 0)
      return -1;
  }

  if (!table_has_column("qso", "points")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN points INTEGER NOT NULL DEFAULT 1;") != 0)
      return -1;
  }

  if (!table_has_column("call_history", "logbook_id")) {
    if (exec_sql_checked("ALTER TABLE call_history ADD COLUMN logbook_id INTEGER NOT NULL DEFAULT 1;") != 0)
      return -1;
  }

  if (ensure_logbook_context() != 0)
    return -1;

  if (db_is_default_path && !db_bootstrap_import_done) {
    int imported_flag = 0;
    if (meta_get_int("call_history_bootstrap", &imported_flag) != 0 ||
        imported_flag == 0) {
      if (table_is_empty("call_history") &&
          import_call_history_file_impl("call_history.txt") >= 0) {
        meta_set_int("call_history_bootstrap", 1);
      }
    }

    db_bootstrap_import_done = 1;
  }

  return 0;
}

/*
 * Check whether a table contains any rows.
 *
 * @param table Table name.
 * @return 1 if the table is empty, otherwise 0.
 */
static int table_is_empty(const char *table) {
  char sql[128];
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", table);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, sql) != SQLITE_OK)
    return 0;

  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return count == 0;
}

/*
 * Read an integer value from the metadata table.
 *
 * @param key Metadata key.
 * @param value Destination for the stored integer.
 * @return 0 on success, or -1 on failure.
 */
static int meta_get_int(const char *key, int *value) {
  if (!key || !key[0] || !value)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, "SELECT value FROM app_meta WHERE key = ?;") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
  }

  sqlite3_finalize(stmt);
  return -1;
}

/*
 * Read the metadata flag that tracks whether a previous log is available.
 *
 * @param value Destination for the flag value.
 * @return 0 on success, or -1 on failure.
 */
static int meta_get_previous_log_available(int *value) {
  return meta_get_int("previous_log_available", value);
}

/*
 * Store an integer value in the metadata table.
 *
 * @param key Metadata key.
 * @param value Integer value to store.
 * @return 0 on success, or -1 on failure.
 */
static int meta_set_int(const char *key, int value) {
  if (!key || !key[0])
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO app_meta (key, value) VALUES (?, ?) "
                   "ON CONFLICT(key) DO UPDATE SET value = excluded.value;") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, value);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * Close the active database connection.
 *
 * @return Nothing.
 */
void db_shutdown(void) {
  if (db) {
    sqlite3_close(db);
    db = NULL;
  }

  db_initialized = 0;
}

/*
 * Initialize the database layer and open the SQLite database.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_init(void) {
  if (db_initialized && db)
    return 0;

  if (ensure_open() != 0)
    return -1;

  db_initialized = 1;
  return 0;
}

/*
 * Load QSO rows from the active logbook.
 *
 * @param logbook Destination array for QSO rows.
 * @param max_qso Maximum number of rows to load.
 * @param ids Optional destination array for database ids.
 * @param out_count Optional output count of loaded rows.
 * @return 0 on success, or -1 on failure.
 */
int db_load_qsos(QSO *logbook, int max_qso, long long *ids, int *out_count) {
  if (out_count)
    *out_count = 0;

  if (!logbook || max_qso <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT id,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid "
                   "FROM qso WHERE logbook_id = ? ORDER BY id ASC;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_qso) {
    QSO *q = &logbook[count];
    memset(q, 0, sizeof(*q));

    const unsigned char *date_col = sqlite3_column_text(stmt, 1);
    const unsigned char *utc_col = sqlite3_column_text(stmt, 2);
    const unsigned char *call_col = sqlite3_column_text(stmt, 3);
    const unsigned char *band_col = sqlite3_column_text(stmt, 5);
    const unsigned char *mode_col = sqlite3_column_text(stmt, 6);
    const unsigned char *rst_col = sqlite3_column_text(stmt, 7);
    const unsigned char *comments_col = sqlite3_column_text(stmt, 8);
    const unsigned char *sent_col = sqlite3_column_text(stmt, 9);
    const unsigned char *recv_col = sqlite3_column_text(stmt, 10);
    const unsigned char *operator_mode_col = sqlite3_column_text(stmt, 11);
    const unsigned char *contest_id_col = sqlite3_column_text(stmt, 12);
    const unsigned char *country_col = sqlite3_column_text(stmt, 15);

    q->db_id = sqlite3_column_int64(stmt, 0);
    snprintf(q->date, sizeof(q->date), "%s", date_col ? (const char *)date_col : "");
    snprintf(q->utc, sizeof(q->utc), "%s", utc_col ? (const char *)utc_col : "");
    snprintf(q->call, sizeof(q->call), "%s", call_col ? (const char *)call_col : "");
    q->freq = sqlite3_column_int(stmt, 4);
    snprintf(q->band, sizeof(q->band), "%s", band_col ? (const char *)band_col : "");
    snprintf(q->mode, sizeof(q->mode), "%s", mode_col ? (const char *)mode_col : "");
    snprintf(q->rst, sizeof(q->rst), "%s", rst_col ? (const char *)rst_col : "");
    snprintf(q->comments, sizeof(q->comments), "%s", comments_col ? (const char *)comments_col : "");
    snprintf(q->exchange_sent, sizeof(q->exchange_sent), "%s",
             sent_col ? (const char *)sent_col : "");
    snprintf(q->exchange_recv, sizeof(q->exchange_recv), "%s",
             recv_col ? (const char *)recv_col : "");
    snprintf(q->operator_mode, sizeof(q->operator_mode), "%s",
             operator_mode_col ? (const char *)operator_mode_col : "");
    snprintf(q->contest_id, sizeof(q->contest_id), "%s",
             contest_id_col ? (const char *)contest_id_col : "");
    q->radio_nr = sqlite3_column_int(stmt, 13);
    q->points = sqlite3_column_int(stmt, 14);
    snprintf(q->country, sizeof(q->country), "%s",
             country_col ? (const char *)country_col : "");
    q->cq_zone = sqlite3_column_int(stmt, 16);
    q->itu_zone = sqlite3_column_int(stmt, 17);
    q->invalid = sqlite3_column_int(stmt, 18) != 0;

    if (ids)
      ids[count] = q->db_id;

    count++;
  }

  sqlite3_finalize(stmt);

  if (out_count)
    *out_count = count;

  return 0;
}

/*
 * Insert a QSO into the active logbook.
 *
 * @param qso QSO row to insert.
 * @param out_id Optional destination for the inserted row id.
 * @return 0 on success, or -1 on failure.
 */
int db_insert_qso(const QSO *qso, long long *out_id) {
  if (out_id)
    *out_id = 0;

  if (!qso)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO qso (logbook_id,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);
  sqlite3_bind_text(stmt, 2, qso->date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, qso->utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, qso->call, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, qso->freq);
  sqlite3_bind_text(stmt, 6, qso->band, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, qso->mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, qso->rst, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, qso->comments, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, qso->exchange_sent, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, qso->exchange_recv, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12, qso->operator_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13, qso->contest_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 14, qso->radio_nr);
  sqlite3_bind_int(stmt, 15, qso->points);
  sqlite3_bind_text(stmt, 16, qso->country, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 17, qso->cq_zone);
  sqlite3_bind_int(stmt, 18, qso->itu_zone);
  sqlite3_bind_int(stmt, 19, qso->invalid ? 1 : 0);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }

  if (out_id)
    *out_id = sqlite3_last_insert_rowid(db);

  sqlite3_finalize(stmt);
  return 0;
}

/*
 * Update the invalid flag for a stored QSO row.
 *
 * @param id Row id to update.
 * @param invalid Nonzero marks the row invalid.
 * @return 0 on success, or -1 on failure.
 */
int db_update_qso_invalid(long long id, int invalid) {
  if (id <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE qso SET invalid = ? WHERE id = ? AND logbook_id = ?;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, invalid ? 1 : 0);
  sqlite3_bind_int64(stmt, 2, id);
  sqlite3_bind_int(stmt, 3, logbook_id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE ? 0 : -1;
}

int db_update_qso_contest_fields(long long id, const char *exchange_sent,
                                 const char *exchange_recv,
                                 const char *operator_mode,
                                 const char *contest_id, int radio_nr,
                                 int points) {
  if (id <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  if (radio_nr < 1)
    radio_nr = 1;
  if (points < 0)
    points = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE qso SET exchange_sent = ?, exchange_recv = ?, operator_mode = ?, contest_id = ?, radio_nr = ?, points = ? WHERE id = ? AND logbook_id = ?;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, exchange_sent ? exchange_sent : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, exchange_recv ? exchange_recv : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, operator_mode ? operator_mode : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, contest_id ? contest_id : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, radio_nr);
  sqlite3_bind_int(stmt, 6, points);
  sqlite3_bind_int64(stmt, 7, id);
  sqlite3_bind_int(stmt, 8, logbook_id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * Load call-history rows from the active logbook.
 *
 * @param history Destination array for call strings.
 * @param max_history Maximum number of entries to load.
 * @param out_count Optional output count of loaded entries.
 * @return 0 on success, or -1 on failure.
 */
int db_load_call_history(char history[][32], int max_history, int *out_count) {
  if (out_count)
    *out_count = 0;

  if (!history || max_history <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT call FROM call_history WHERE logbook_id = ? ORDER BY id ASC;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_history) {
    const unsigned char *call = sqlite3_column_text(stmt, 0);
    if (call && call[0]) {
      snprintf(history[count], 32, "%s", call);
      count++;
    }
  }

  sqlite3_finalize(stmt);

  if (out_count)
    *out_count = count;

  return 0;
}

/*
 * Append a callsign to the call-history table for the active logbook.
 *
 * @param call Callsign to append.
 * @return 0 on success, or -1 on failure.
 */
int db_append_call_history(const char *call) {
  if (!call || !call[0])
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO call_history (logbook_id, call) VALUES (?, ?);") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);
  sqlite3_bind_text(stmt, 2, call, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE ? 0 : -1;
}

int db_set_current_logbook_contest_path(const char *path) {
  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  return set_named_logbook_contest_path(logbook_id, path);
}

int db_get_current_logbook_contest_path(char *out, size_t out_size) {
  if (!out || out_size == 0)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  return get_named_logbook_contest_path(logbook_id, out, out_size);
}

/*
 * Import call history from a file into the active logbook.
 *
 * @param path File path to import.
 * @return Number of imported entries, or -1 on failure.
 */
int db_import_call_history_file(const char *path) {
  if (!path || !path[0])
    return -1;

  if (db_init() != 0)
    return -1;

  return import_call_history_file_impl(path);
}

/*
 * Clear the active logbook's QSO and call-history rows.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_clear_logbook(void) {
  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0)
    return -1;

  if (exec_sql("BEGIN;") != SQLITE_OK)
    return -1;

  sqlite3_stmt *del_qso = NULL;
  sqlite3_stmt *del_hist = NULL;
  int ok = 0;

  if (prepare_stmt(&del_qso, "DELETE FROM qso WHERE logbook_id = ?;") == SQLITE_OK &&
      prepare_stmt(&del_hist,
                   "DELETE FROM call_history WHERE logbook_id = ?;") == SQLITE_OK) {
    sqlite3_bind_int(del_qso, 1, logbook_id);
    sqlite3_bind_int(del_hist, 1, logbook_id);
    ok = sqlite3_step(del_qso) == SQLITE_DONE && sqlite3_step(del_hist) == SQLITE_DONE;
  }

  if (del_qso)
    sqlite3_finalize(del_qso);
  if (del_hist)
    sqlite3_finalize(del_hist);

  if (exec_sql(ok ? "COMMIT;" : "ROLLBACK;") != SQLITE_OK)
    return -1;

  return ok ? 0 : -1;
}

/*
 * Copy rows from one table into another.
 *
 * @param src Source table.
 * @param dst Destination table.
 * @param columns Comma-separated column list to copy.
 * @return 0 on success, or -1 on failure.
 */
static int copy_table(const char *src, const char *dst, const char *columns) {
  char sql[512];

  if (!src || !dst || !columns)
    return -1;

  snprintf(sql, sizeof(sql), "DELETE FROM %s;", dst);
  if (exec_sql_checked(sql) != 0)
    return -1;

  snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) SELECT %s FROM %s;", dst,
           columns, columns, src);
  return exec_sql_checked(sql);
}

/*
 * Archive the current logbook as the previous logbook reference.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_archive_current_logbook(void) {
  if (db_init() != 0)
    return -1;

  int current_id = 0;
  if (get_current_logbook_id(&current_id) != 0 || current_id <= 0)
    return -1;

  return set_previous_logbook_id(current_id);
}

/*
 * Open the previously archived named logbook.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_open_previous_logbook(void) {
  if (db_init() != 0)
    return -1;

  int current_id = 0;
  int previous_id = 0;

  if (get_current_logbook_id(&current_id) != 0 || current_id <= 0)
    return -1;

  if (get_previous_logbook_id(&previous_id) != 0 || previous_id <= 0 ||
      !named_logbook_exists(previous_id))
    return -1;

  if (set_current_logbook_id(previous_id) != 0)
    return -1;

  if (set_previous_logbook_id(current_id) != 0)
    return -1;

  return 0;
}

/*
 * Check whether a named logbook id exists.
 *
 * @param id Logbook id to check.
 * @return 1 if the named logbook exists, otherwise 0.
 */
static int named_logbook_exists(long long id) {
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, "SELECT 1 FROM named_logbooks WHERE id = ?;") !=
      SQLITE_OK)
    return 0;

  sqlite3_bind_int64(stmt, 1, id);
  int found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

/*
 * Archive the current logbook under a new named logbook entry.
 *
 * @param name Name to assign to the archived logbook.
 * @return 0 on success, or -1 on failure.
 */
int db_archive_current_logbook_named(const char *name) {
  if (!name || !name[0])
    return -1;

  if (db_init() != 0)
    return -1;

  int current_id = 0;
  if (get_current_logbook_id(&current_id) != 0 || current_id <= 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO named_logbooks (name, created_at) VALUES (?, CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return -1;

  bind_text_or_null(stmt, 1, name);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }

  int new_id = (int)sqlite3_last_insert_rowid(db);
  sqlite3_finalize(stmt);

  if (new_id <= 0)
    return -1;

  if (set_previous_logbook_id(current_id) != 0)
    return -1;

  if (set_current_logbook_id(new_id) != 0)
    return -1;

  return 0;
}

/*
 * List named logbooks and their QSO counts.
 *
 * @param out Destination array for logbook metadata.
 * @param max_items Maximum number of items to return.
 * @param out_count Optional output count of returned items.
 * @return 0 on success, or -1 on failure.
 */
int db_list_named_logbooks(DBNamedLogbook *out, int max_items, int *out_count) {
  if (out_count)
    *out_count = 0;

  if (!out || max_items <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT nl.id,nl.name,nl.created_at,"
                   "(SELECT COUNT(*) FROM qso nq WHERE nq.logbook_id = nl.id) "
                   "FROM named_logbooks nl ORDER BY nl.id DESC;") != SQLITE_OK)
    return -1;

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_items) {
    DBNamedLogbook *item = &out[count];
    memset(item, 0, sizeof(*item));

    item->id = sqlite3_column_int64(stmt, 0);
    snprintf(item->name, sizeof(item->name), "%s",
             (const char *)sqlite3_column_text(stmt, 1));
    snprintf(item->created_at, sizeof(item->created_at), "%s",
             (const char *)sqlite3_column_text(stmt, 2));
    item->qso_count = sqlite3_column_int(stmt, 3);

    count++;
  }

  sqlite3_finalize(stmt);

  if (out_count)
    *out_count = count;

  return 0;
}

/*
 * Open a named logbook by id.
 *
 * @param id Named logbook id to open.
 * @return 0 on success, or -1 on failure.
 */
int db_open_named_logbook_by_id(long long id) {
  if (id <= 0)
    return -1;

  if (db_init() != 0)
    return -1;

  if (!named_logbook_exists(id))
    return -1;

  int current_id = 0;
  if (get_current_logbook_id(&current_id) != 0 || current_id <= 0)
    return -1;

  if (set_previous_logbook_id(current_id) != 0)
    return -1;

  return set_current_logbook_id((int)id);
}

/*
 * Open a named logbook by name.
 *
 * @param name Logbook name to open.
 * @return 0 on success, or -1 on failure.
 */
int db_open_named_logbook_by_name(const char *name) {
  if (!name || !name[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT id FROM named_logbooks WHERE name = ? ORDER BY id DESC LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  bind_text_or_null(stmt, 1, name);
  long long id = 0;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    id = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);

  if (id <= 0)
    return -1;

  return db_open_named_logbook_by_id(id);
}

/*
 * Export query rows to either CSV or ADIF format.
 *
 * @param sql Query that selects QSO fields in export order.
 * @param f Open output stream.
 * @param adif_mode Nonzero to write ADIF, zero to write CSV.
 * @return 0 on success, or -1 on failure.
 */
static int export_qso_rows(const char *sql, FILE *f, int adif_mode) {
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, sql) != SQLITE_OK)
    return -1;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *date = (const char *)sqlite3_column_text(stmt, 0);
    const char *utc = (const char *)sqlite3_column_text(stmt, 1);
    const char *call = (const char *)sqlite3_column_text(stmt, 2);
    int freq = sqlite3_column_int(stmt, 3);
    const char *band = (const char *)sqlite3_column_text(stmt, 4);
    const char *mode = (const char *)sqlite3_column_text(stmt, 5);
    const char *rst = (const char *)sqlite3_column_text(stmt, 6);
    const char *comments = (const char *)sqlite3_column_text(stmt, 7);
    const char *country = (const char *)sqlite3_column_text(stmt, 8);

    if (!adif_mode) {
      fprintf(f, "%s,%s,%s,%d,%s,%s,%s,%s,%s\n", date, utc, call, freq, band,
              mode, rst, comments ? comments : "", country);
    } else {
      fprintf(f, "<CALL:%zu>%s", strlen(call), call);
      fprintf(f, "<QSO_DATE:8>%s", date);
      fprintf(f, "<TIME_ON:4>%s", utc);
      fprintf(f, "<FREQ:9>%.6f", freq / 1000.0);
      fprintf(f, "<BAND:%zu>%s", strlen(band), band);
      fprintf(f, "<MODE:%zu>%s", strlen(mode), mode);
      fprintf(f, "<RST_SENT:%zu>%s", strlen(rst), rst);
      fprintf(f, "<RST_RCVD:%zu>%s", strlen(rst), rst);
      if (comments && comments[0])
        fprintf(f, "<COMMENT:%zu>%s", strlen(comments), comments);
      if (country && country[0])
        fprintf(f, "<COUNTRY:%zu>%s", strlen(country), country);
      fprintf(f, "<EOR>\n");
    }
  }

  sqlite3_finalize(stmt);
  return 0;
}

/*
 * Export the active logbook to CSV.
 *
 * @param filename Destination file path.
 * @return 0 on success, or -1 on failure.
 */
int db_export_csv(const char *filename) {
  FILE *f = fopen(filename, "w");
  if (!f)
    return -1;

  if (db_init() != 0) {
    fclose(f);
    return -1;
  }

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0) {
    fclose(f);
    return -1;
  }

  fprintf(f, "DATE,UTC,CALL,FREQ,BAND,MODE,RST,COMMENTS,COUNTRY\n");

  char sql[256];
  snprintf(sql, sizeof(sql),
           "SELECT date,utc,call,freq,band,mode,rst,comments,country FROM qso "
           "WHERE logbook_id = %d AND invalid = 0 ORDER BY id ASC;",
           logbook_id);

  int rc = export_qso_rows(sql, f, 0);

  fclose(f);
  return rc;
}

/*
 * Export the active logbook to ADIF.
 *
 * @param filename Destination file path.
 * @return 0 on success, or -1 on failure.
 */
int db_export_adif(const char *filename) {
  FILE *f = fopen(filename, "w");
  if (!f)
    return -1;

  if (db_init() != 0) {
    fclose(f);
    return -1;
  }

  int logbook_id = 0;
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0) {
    fclose(f);
    return -1;
  }

  fprintf(f, "Generated by Logger\n");
  fprintf(f, "<EOH>\n");

  char sql[256];
  snprintf(sql, sizeof(sql),
           "SELECT date,utc,call,freq,band,mode,rst,comments,country FROM qso "
           "WHERE logbook_id = %d AND invalid = 0 ORDER BY id ASC;",
           logbook_id);

  int rc = export_qso_rows(sql, f, 1);

  fclose(f);
  return rc;
}

/* ------------------------------------------------------------------ */
/* QTC persistence                                                      */
/* ------------------------------------------------------------------ */

/*
 * Insert a QTC bundle header and its records into the database.
 *
 * @param bundle  Bundle to persist.
 * @param out_id  Optional destination for the inserted bundle row id.
 * @return 0 on success, or -1 on failure.
 */
int db_insert_qtc_bundle(const QTCBundle *bundle, long long *out_id) {
  if (!bundle)
    return -1;

  if (db_init() != 0)
    return -1;

  int logbook_id = 1;
  get_current_logbook_id(&logbook_id);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO qtc_bundles "
                   "(logbook_id, sender_call, receiver_call, bundle_nr, "
                   " record_count, sent) "
                   "VALUES (?, ?, ?, ?, ?, ?);") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);
  bind_text_or_null(stmt, 2, bundle->sender_call);
  bind_text_or_null(stmt, 3, bundle->receiver_call);
  sqlite3_bind_int(stmt, 4, bundle->bundle_nr);
  sqlite3_bind_int(stmt, 5, bundle->record_count);
  sqlite3_bind_int(stmt, 6, bundle->sent ? 1 : 0);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE)
    return -1;

  long long bundle_id = sqlite3_last_insert_rowid(db);
  if (out_id)
    *out_id = bundle_id;

  /* Insert individual QTC records. */
  for (int i = 0; i < bundle->record_count; i++) {
    const QTCRecord *r = &bundle->records[i];
    sqlite3_stmt *rstmt = NULL;
    if (prepare_stmt(&rstmt,
                     "INSERT INTO qtc_records "
                     "(bundle_id, seq_nr, date, time, call, exch) "
                     "VALUES (?, ?, ?, ?, ?, ?);") != SQLITE_OK)
      return -1;

    sqlite3_bind_int64(rstmt, 1, bundle_id);
    sqlite3_bind_int(rstmt, 2, i);
    bind_text_or_null(rstmt, 3, r->date);
    bind_text_or_null(rstmt, 4, r->time);
    bind_text_or_null(rstmt, 5, r->call);
    bind_text_or_null(rstmt, 6, r->exch);

    int rrc = sqlite3_step(rstmt);
    sqlite3_finalize(rstmt);

    if (rrc != SQLITE_DONE)
      return -1;
  }

  return 0;
}

/*
 * Load QTC bundles for the current logbook from the database.
 *
 * @param out        Destination array.
 * @param max_items  Maximum items to load.
 * @param out_count  Optional output count.
 * @return 0 on success, or -1 on failure.
 */
int db_load_qtc_bundles(QTCBundle *out, int max_items, int *out_count) {
  if (!out || max_items <= 0)
    return -1;

  if (out_count)
    *out_count = 0;

  if (db_init() != 0)
    return -1;

  int logbook_id = 1;
  get_current_logbook_id(&logbook_id);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT id, sender_call, receiver_call, bundle_nr, "
                   "       record_count, sent "
                   "FROM qtc_bundles "
                   "WHERE logbook_id = ? "
                   "ORDER BY id ASC;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_items) {
    QTCBundle *b = &out[count];
    memset(b, 0, sizeof(*b));

    b->db_id = sqlite3_column_int64(stmt, 0);
    const char *sender  = (const char *)sqlite3_column_text(stmt, 1);
    const char *recvr   = (const char *)sqlite3_column_text(stmt, 2);
    b->bundle_nr        = sqlite3_column_int(stmt, 3);
    b->record_count     = sqlite3_column_int(stmt, 4);
    b->sent             = sqlite3_column_int(stmt, 5);

    if (sender)
      snprintf(b->sender_call, sizeof(b->sender_call), "%s", sender);
    if (recvr)
      snprintf(b->receiver_call, sizeof(b->receiver_call), "%s", recvr);

    /* Load per-record rows. */
    sqlite3_stmt *rstmt = NULL;
    if (prepare_stmt(&rstmt,
                     "SELECT seq_nr, date, time, call, exch "
                     "FROM qtc_records WHERE bundle_id = ? "
                     "ORDER BY seq_nr ASC;") == SQLITE_OK) {
      sqlite3_bind_int64(rstmt, 1, b->db_id);

      while (sqlite3_step(rstmt) == SQLITE_ROW) {
        int seq = sqlite3_column_int(rstmt, 0);
        if (seq < 0 || seq >= QTC_MAX_RECORDS_PER_BUNDLE)
          continue;

        QTCRecord *r = &b->records[seq];
        const char *d = (const char *)sqlite3_column_text(rstmt, 1);
        const char *t = (const char *)sqlite3_column_text(rstmt, 2);
        const char *c = (const char *)sqlite3_column_text(rstmt, 3);
        const char *e = (const char *)sqlite3_column_text(rstmt, 4);

        if (d) snprintf(r->date, sizeof(r->date), "%s", d);
        if (t) snprintf(r->time, sizeof(r->time), "%s", t);
        if (c) snprintf(r->call, sizeof(r->call), "%s", c);
        if (e) snprintf(r->exch, sizeof(r->exch), "%s", e);
      }

      sqlite3_finalize(rstmt);
    }

    count++;
  }

  sqlite3_finalize(stmt);

  if (out_count)
    *out_count = count;

  return 0;
}