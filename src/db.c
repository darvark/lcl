#include "db.h"

#include "qtc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
extern int sqlite3_changes(sqlite3 *db);
extern void sqlite3_free(void *ptr);
extern int sqlite3_reset(sqlite3_stmt *stmt);
extern int sqlite3_clear_bindings(sqlite3_stmt *stmt);

static sqlite3 *db = NULL;
static char db_path[512] = {0};
static int db_initialized = 0;
static int db_is_default_path = 1;
static int db_bootstrap_import_done = 0;

#define DB_SYNC_MAX_RETRY 6

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
static void utc_now_iso(char *out, size_t out_size);
static int sync_generate_hex_token(int bytes, char *out, size_t out_size);
static int sync_fetch_qso_meta(long long id, int logbook_id, char *qso_uid,
                               size_t qso_uid_size, char *origin_station_id,
                               size_t station_id_size, long long *origin_seq,
                               int *version);
static void utc_plus_seconds_iso(int delta_seconds, char *out,
                                 size_t out_size);
static int sync_build_qso_payload_from_row(long long id, int logbook_id,
                                           char *out, size_t out_size);
static int sync_json_get_string(const char *json, const char *key, char *out,
                                size_t out_size);
static int sync_json_get_int(const char *json, const char *key, int *out);
static int sync_json_get_i64(const char *json, const char *key,
                             long long *out);
static int sync_json_get_bool(const char *json, const char *key, int *out);
static int sync_qso_upsert_from_payload(const char *op_id,
                                        const char *origin_station_id,
                                        long long origin_station_seq,
                                        int logbook_id,
                                        const char *payload_json,
                                        int *out_changed);

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
 * Format current UTC timestamp into ISO-8601 form.
 *
 * @param out Destination buffer.
 * @param out_size Destination size.
 * @return Nothing.
 */
static void utc_now_iso(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  time_t now = time(NULL);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/*
 * Generate a lowercase hex token using SQLite randomblob.
 *
 * @param bytes Number of random bytes.
 * @param out Destination buffer.
 * @param out_size Destination size.
 * @return 0 on success, or -1 on failure.
 */
static int sync_generate_hex_token(int bytes, char *out, size_t out_size) {
  if (!out || out_size < 2 || bytes <= 0)
    return -1;

  out[0] = 0;

  char sql[96];
  snprintf(sql, sizeof(sql), "SELECT lower(hex(randomblob(%d)));", bytes);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt, sql) != SQLITE_OK)
    return -1;

  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *token = sqlite3_column_text(stmt, 0);
    if (token && token[0]) {
      snprintf(out, out_size, "%s", (const char *)token);
      rc = 0;
    }
  }

  sqlite3_finalize(stmt);
  return rc;
}

/*
 * Load synchronization metadata for a single QSO row.
 */
static int sync_fetch_qso_meta(long long id, int logbook_id, char *qso_uid,
                               size_t qso_uid_size, char *origin_station_id,
                               size_t station_id_size, long long *origin_seq,
                               int *version) {
  if (id <= 0 || logbook_id <= 0 || !qso_uid || qso_uid_size < 2 ||
      !origin_station_id || station_id_size < 2 || !origin_seq || !version)
    return -1;

  qso_uid[0] = 0;
  origin_station_id[0] = 0;
  *origin_seq = 0;
  *version = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT qso_uid, origin_station_id, origin_station_seq, version "
                   "FROM qso WHERE id = ? AND logbook_id = ? LIMIT 1;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_int(stmt, 2, logbook_id);

  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *uid_col = sqlite3_column_text(stmt, 0);
    const unsigned char *station_col = sqlite3_column_text(stmt, 1);

    snprintf(qso_uid, qso_uid_size, "%s", uid_col ? (const char *)uid_col : "");
    snprintf(origin_station_id, station_id_size, "%s",
             station_col ? (const char *)station_col : "");
    *origin_seq = sqlite3_column_int64(stmt, 2);
    *version = sqlite3_column_int(stmt, 3);

    rc = qso_uid[0] ? 0 : -1;
  }

  sqlite3_finalize(stmt);
  return rc;
}

static void utc_plus_seconds_iso(int delta_seconds, char *out,
                                 size_t out_size) {
  if (!out || out_size < 2)
    return;

  time_t now = time(NULL);
  now += delta_seconds;

  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static int sync_json_get_string(const char *json, const char *key, char *out,
                                size_t out_size) {
  if (!json || !key || !out || out_size < 2)
    return -1;

  out[0] = 0;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  if (*p != '"')
    return -1;
  p++;

  size_t used = 0;
  while (*p && *p != '"' && used < out_size - 1) {
    if (*p == '\\' && p[1])
      p++;
    out[used++] = *p++;
  }
  out[used] = 0;

  return used > 0 ? 0 : -1;
}

static int sync_json_get_i64(const char *json, const char *key,
                             long long *out) {
  if (!json || !key || !out)
    return -1;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  char *endptr = NULL;
  long long value = strtoll(p, &endptr, 10);
  if (endptr == p)
    return -1;

  *out = value;
  return 0;
}

static int sync_json_get_int(const char *json, const char *key, int *out) {
  long long v = 0;
  if (!out || sync_json_get_i64(json, key, &v) != 0)
    return -1;
  *out = (int)v;
  return 0;
}

static int sync_json_get_bool(const char *json, const char *key, int *out) {
  if (!json || !key || !out)
    return -1;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  if (strncmp(p, "true", 4) == 0) {
    *out = 1;
    return 0;
  }

  if (strncmp(p, "false", 5) == 0) {
    *out = 0;
    return 0;
  }

  return -1;
}

static int sync_build_qso_payload_from_row(long long id, int logbook_id,
                                           char *out, size_t out_size) {
  if (id <= 0 || logbook_id <= 0 || !out || out_size < 8)
    return -1;

  out[0] = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT qso_uid,origin_station_id,origin_station_seq,last_modified_utc,version,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid "
                   "FROM qso WHERE id = ? AND logbook_id = ? LIMIT 1;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_int(stmt, 2, logbook_id);

  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *qso_uid = (const char *)sqlite3_column_text(stmt, 0);
    const char *origin_station_id = (const char *)sqlite3_column_text(stmt, 1);
    long long origin_station_seq = sqlite3_column_int64(stmt, 2);
    const char *last_modified_utc = (const char *)sqlite3_column_text(stmt, 3);
    int version = sqlite3_column_int(stmt, 4);
    const char *date = (const char *)sqlite3_column_text(stmt, 5);
    const char *utc = (const char *)sqlite3_column_text(stmt, 6);
    const char *call = (const char *)sqlite3_column_text(stmt, 7);
    int freq = sqlite3_column_int(stmt, 8);
    const char *band = (const char *)sqlite3_column_text(stmt, 9);
    const char *mode = (const char *)sqlite3_column_text(stmt, 10);
    const char *rst = (const char *)sqlite3_column_text(stmt, 11);
    const char *comments = (const char *)sqlite3_column_text(stmt, 12);
    const char *exchange_sent = (const char *)sqlite3_column_text(stmt, 13);
    const char *exchange_recv = (const char *)sqlite3_column_text(stmt, 14);
    const char *operator_mode = (const char *)sqlite3_column_text(stmt, 15);
    const char *contest_id = (const char *)sqlite3_column_text(stmt, 16);
    int radio_nr = sqlite3_column_int(stmt, 17);
    int points = sqlite3_column_int(stmt, 18);
    const char *country = (const char *)sqlite3_column_text(stmt, 19);
    int cq_zone = sqlite3_column_int(stmt, 20);
    int itu_zone = sqlite3_column_int(stmt, 21);
    int invalid = sqlite3_column_int(stmt, 22) != 0;

    int n = snprintf(out, out_size,
                     "{\"kind\":\"qso_full\",\"qso_uid\":\"%s\",\"origin_station_id\":\"%s\",\"origin_station_seq\":%lld,\"last_modified_utc\":\"%s\",\"version\":%d,\"date\":\"%s\",\"utc\":\"%s\",\"call\":\"%s\",\"freq\":%d,\"band\":\"%s\",\"mode\":\"%s\",\"rst\":\"%s\",\"comments\":\"%s\",\"exchange_sent\":\"%s\",\"exchange_recv\":\"%s\",\"operator_mode\":\"%s\",\"contest_id\":\"%s\",\"radio_nr\":%d,\"points\":%d,\"country\":\"%s\",\"cq_zone\":%d,\"itu_zone\":%d,\"invalid\":%s}",
                     qso_uid ? qso_uid : "", origin_station_id ? origin_station_id : "",
                     origin_station_seq, last_modified_utc ? last_modified_utc : "",
                     version, date ? date : "", utc ? utc : "", call ? call : "",
                     freq, band ? band : "", mode ? mode : "", rst ? rst : "",
                     comments ? comments : "", exchange_sent ? exchange_sent : "",
                     exchange_recv ? exchange_recv : "",
                     operator_mode ? operator_mode : "", contest_id ? contest_id : "",
                     radio_nr, points, country ? country : "", cq_zone, itu_zone,
                     invalid ? "true" : "false");
    if (n > 0 && (size_t)n < out_size)
      rc = 0;
  }

  sqlite3_finalize(stmt);
  return rc;
}

static int sync_qso_upsert_from_payload(const char *op_id,
                                        const char *origin_station_id,
                                        long long origin_station_seq,
                                        int logbook_id,
                                        const char *payload_json,
                                        int *out_changed) {
  if (!op_id || !op_id[0] || !payload_json || !payload_json[0] ||
      !origin_station_id || !origin_station_id[0] || origin_station_seq <= 0 ||
      logbook_id <= 0)
    return -1;

  if (out_changed)
    *out_changed = 0;

  char qso_uid[40] = {0};
  char last_modified_utc[32] = {0};
  char date[9] = {0};
  char utc[5] = {0};
  char call[32] = {0};
  char band[8] = {0};
  char mode[16] = {0};
  char rst[8] = {0};
  char comments[128] = {0};
  char exchange_sent[32] = {0};
  char exchange_recv[32] = {0};
  char operator_mode[8] = {0};
  char contest_id[64] = {0};
  char country[64] = {0};
  int freq = 0;
  int version = 1;
  int radio_nr = 1;
  int points = 1;
  int cq_zone = 0;
  int itu_zone = 0;
  int invalid = 0;

  if (sync_json_get_string(payload_json, "qso_uid", qso_uid,
                           sizeof(qso_uid)) != 0)
    return -1;

  (void)sync_json_get_string(payload_json, "last_modified_utc",
                             last_modified_utc, sizeof(last_modified_utc));
  (void)sync_json_get_string(payload_json, "date", date, sizeof(date));
  (void)sync_json_get_string(payload_json, "utc", utc, sizeof(utc));
  (void)sync_json_get_string(payload_json, "call", call, sizeof(call));
  {
    long long freq_ll = 0;
    if (sync_json_get_i64(payload_json, "freq", &freq_ll) == 0)
      freq = (int)freq_ll;
  }
  (void)sync_json_get_string(payload_json, "band", band, sizeof(band));
  (void)sync_json_get_string(payload_json, "mode", mode, sizeof(mode));
  (void)sync_json_get_string(payload_json, "rst", rst, sizeof(rst));
  (void)sync_json_get_string(payload_json, "comments", comments,
                             sizeof(comments));
  (void)sync_json_get_string(payload_json, "exchange_sent", exchange_sent,
                             sizeof(exchange_sent));
  (void)sync_json_get_string(payload_json, "exchange_recv", exchange_recv,
                             sizeof(exchange_recv));
  (void)sync_json_get_string(payload_json, "operator_mode", operator_mode,
                             sizeof(operator_mode));
  (void)sync_json_get_string(payload_json, "contest_id", contest_id,
                             sizeof(contest_id));
  (void)sync_json_get_string(payload_json, "country", country,
                             sizeof(country));
  (void)sync_json_get_int(payload_json, "version", &version);
  (void)sync_json_get_int(payload_json, "radio_nr", &radio_nr);
  (void)sync_json_get_int(payload_json, "points", &points);
  (void)sync_json_get_int(payload_json, "cq_zone", &cq_zone);
  (void)sync_json_get_int(payload_json, "itu_zone", &itu_zone);
  (void)sync_json_get_bool(payload_json, "invalid", &invalid);

  sqlite3_stmt *sel = NULL;
  if (prepare_stmt(&sel,
                   "SELECT id, version, last_modified_utc, origin_station_id "
                   "FROM qso WHERE qso_uid = ? AND logbook_id = ? LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(sel, 1, qso_uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(sel, 2, logbook_id);

  long long existing_id = 0;
  int existing_version = 0;
  char existing_modified[32] = {0};
  char existing_origin_station[32] = {0};
  int exists = 0;

  if (sqlite3_step(sel) == SQLITE_ROW) {
    exists = 1;
    existing_id = sqlite3_column_int64(sel, 0);
    existing_version = sqlite3_column_int(sel, 1);
    const unsigned char *mod_col = sqlite3_column_text(sel, 2);
    const unsigned char *origin_col = sqlite3_column_text(sel, 3);
    snprintf(existing_modified, sizeof(existing_modified), "%s",
             mod_col ? (const char *)mod_col : "");
    snprintf(existing_origin_station, sizeof(existing_origin_station), "%s",
             origin_col ? (const char *)origin_col : "");
  }
  sqlite3_finalize(sel);

  if (!exists) {
    sqlite3_stmt *ins = NULL;
    if (prepare_stmt(&ins,
                     "INSERT INTO qso "
                     "(logbook_id,qso_uid,origin_station_id,origin_station_seq,last_op_id,last_modified_utc,version,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid) "
                     "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);") !=
        SQLITE_OK)
      return -1;

    sqlite3_bind_int(ins, 1, logbook_id);
    sqlite3_bind_text(ins, 2, qso_uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, origin_station_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 4, origin_station_seq);
    sqlite3_bind_text(ins, 5, op_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 6, last_modified_utc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 7, version > 0 ? version : 1);
    sqlite3_bind_text(ins, 8, date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 9, utc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 10, call, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 11, freq);
    sqlite3_bind_text(ins, 12, band, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 13, mode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 14, rst, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 15, comments, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 16, exchange_sent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 17, exchange_recv, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 18, operator_mode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 19, contest_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 20, radio_nr < 1 ? 1 : radio_nr);
    sqlite3_bind_int(ins, 21, points < 0 ? 0 : points);
    sqlite3_bind_text(ins, 22, country, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 23, cq_zone);
    sqlite3_bind_int(ins, 24, itu_zone);
    sqlite3_bind_int(ins, 25, invalid ? 1 : 0);

    int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE)
      return -1;

    if (out_changed)
      *out_changed = 1;
    return 0;
  }

  int should_update = 0;
  if (version > existing_version)
    should_update = 1;
  else if (version == existing_version && last_modified_utc[0] &&
           strcmp(last_modified_utc, existing_modified) > 0)
    should_update = 1;
  else if (version == existing_version &&
           strcmp(last_modified_utc, existing_modified) == 0 &&
           strcmp(origin_station_id, existing_origin_station) > 0)
    should_update = 1;

  if (!should_update)
    return 0;

  sqlite3_stmt *upd = NULL;
  if (prepare_stmt(&upd,
                   "UPDATE qso SET "
                   "origin_station_id = ?, origin_station_seq = ?, last_op_id = ?, "
                   "last_modified_utc = ?, version = ?, date = ?, utc = ?, call = ?, freq = ?, "
                   "band = ?, mode = ?, rst = ?, comments = ?, exchange_sent = ?, exchange_recv = ?, "
                   "operator_mode = ?, contest_id = ?, radio_nr = ?, points = ?, country = ?, cq_zone = ?, "
                   "itu_zone = ?, invalid = ? "
                   "WHERE id = ? AND logbook_id = ?;") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(upd, 1, origin_station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(upd, 2, origin_station_seq);
  sqlite3_bind_text(upd, 3, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 4, last_modified_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(upd, 5, version);
  sqlite3_bind_text(upd, 6, date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 7, utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 8, call, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(upd, 9, freq);
  sqlite3_bind_text(upd, 10, band, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 11, mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 12, rst, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 13, comments, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 14, exchange_sent, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 15, exchange_recv, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 16, operator_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 17, contest_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(upd, 18, radio_nr < 1 ? 1 : radio_nr);
  sqlite3_bind_int(upd, 19, points < 0 ? 0 : points);
  sqlite3_bind_text(upd, 20, country, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(upd, 21, cq_zone);
  sqlite3_bind_int(upd, 22, itu_zone);
  sqlite3_bind_int(upd, 23, invalid ? 1 : 0);
  sqlite3_bind_int64(upd, 24, existing_id);
  sqlite3_bind_int(upd, 25, logbook_id);

  int rc = sqlite3_step(upd);
  sqlite3_finalize(upd);
  if (rc != SQLITE_DONE)
    return -1;

  if (out_changed)
    *out_changed = 1;
  return 0;
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
        "qso_uid TEXT NOT NULL DEFAULT '',"
        "origin_station_id TEXT NOT NULL DEFAULT '',"
        "origin_station_seq INTEGER NOT NULL DEFAULT 0,"
        "last_op_id TEXT NOT NULL DEFAULT '',"
        "last_modified_utc TEXT NOT NULL DEFAULT '',"
        "version INTEGER NOT NULL DEFAULT 1,"
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
              "CREATE TABLE IF NOT EXISTS sync_identity ("
              "id INTEGER PRIMARY KEY CHECK (id = 1),"
              "station_id TEXT NOT NULL,"
              "station_name TEXT NOT NULL DEFAULT '',"
              "role TEXT NOT NULL DEFAULT 'client',"
              "created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS sync_cursors ("
              "id INTEGER PRIMARY KEY CHECK (id = 1),"
              "last_pulled_global_seq INTEGER NOT NULL DEFAULT 0,"
              "last_acked_local_seq INTEGER NOT NULL DEFAULT 0,"
              "last_server_epoch TEXT NOT NULL DEFAULT ''"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS log_outbox ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "op_id TEXT NOT NULL UNIQUE,"
              "station_seq INTEGER NOT NULL,"
              "logbook_id INTEGER NOT NULL,"
              "op_type TEXT NOT NULL,"
              "entity_id TEXT NOT NULL,"
              "payload_json TEXT NOT NULL,"
              "op_utc TEXT NOT NULL,"
              "status TEXT NOT NULL DEFAULT 'pending',"
              "retry_count INTEGER NOT NULL DEFAULT 0,"
              "next_retry_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS log_ops ("
              "global_seq INTEGER PRIMARY KEY AUTOINCREMENT,"
              "op_id TEXT NOT NULL UNIQUE,"
              "station_id TEXT NOT NULL,"
              "station_seq INTEGER NOT NULL,"
              "logbook_id INTEGER NOT NULL,"
              "op_type TEXT NOT NULL,"
              "entity_id TEXT NOT NULL,"
              "payload_json TEXT NOT NULL,"
              "op_utc TEXT NOT NULL,"
              "applied_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS serial_alloc ("
              "logbook_id INTEGER PRIMARY KEY,"
              "next_serial INTEGER NOT NULL DEFAULT 1,"
              "updated_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS serial_reservations ("
              "reservation_id TEXT PRIMARY KEY,"
              "logbook_id INTEGER NOT NULL,"
              "station_id TEXT NOT NULL,"
              "reserved_serial INTEGER NOT NULL,"
              "status TEXT NOT NULL DEFAULT 'reserved',"
              "reserved_utc TEXT NOT NULL,"
              "expires_utc TEXT NOT NULL,"
              "consumed_utc TEXT NOT NULL DEFAULT '',"
              "consumed_qso_uid TEXT NOT NULL DEFAULT ''"
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

  if (!table_has_column("qso", "qso_uid")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN qso_uid TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "origin_station_id")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN origin_station_id TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "origin_station_seq")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN origin_station_seq INTEGER NOT NULL DEFAULT 0;") != 0)
      return -1;
  }

  if (!table_has_column("qso", "last_op_id")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN last_op_id TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "last_modified_utc")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN last_modified_utc TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("qso", "version")) {
    if (exec_sql_checked("ALTER TABLE qso ADD COLUMN version INTEGER NOT NULL DEFAULT 1;") != 0)
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

  if (!table_has_column("serial_reservations", "consumed_qso_uid")) {
    if (exec_sql_checked("ALTER TABLE serial_reservations ADD COLUMN consumed_qso_uid TEXT NOT NULL DEFAULT '';") != 0)
      return -1;
  }

  if (!table_has_column("call_history", "logbook_id")) {
    if (exec_sql_checked("ALTER TABLE call_history ADD COLUMN logbook_id INTEGER NOT NULL DEFAULT 1;") != 0)
      return -1;
  }

  if (ensure_logbook_context() != 0)
    return -1;

  exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_qso_qso_uid ON qso(qso_uid);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_qso_origin_station ON qso(origin_station_id, origin_station_seq);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_qso_last_modified ON qso(last_modified_utc);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_log_outbox_status_retry ON log_outbox(status, next_retry_utc);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_log_ops_station_seq ON log_ops(station_id, station_seq);");
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_log_ops_station_seq_unique ON log_ops(station_id, station_seq);") != 0)
    return -1;
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_serial_reservations_lookup ON serial_reservations(logbook_id, station_id, status);");

  if (exec_sql_checked(
          "INSERT OR IGNORE INTO sync_cursors (id, last_pulled_global_seq, last_acked_local_seq, last_server_epoch) "
          "VALUES (1, 0, 0, '');") != 0)
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
                   "SELECT id,qso_uid,origin_station_id,origin_station_seq,last_modified_utc,version,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid "
                   "FROM qso WHERE logbook_id = ? ORDER BY id ASC;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_qso) {
    QSO *q = &logbook[count];
    memset(q, 0, sizeof(*q));

    const unsigned char *qso_uid_col = sqlite3_column_text(stmt, 1);
    const unsigned char *origin_station_col = sqlite3_column_text(stmt, 2);
    const unsigned char *last_modified_col = sqlite3_column_text(stmt, 4);
    const unsigned char *date_col = sqlite3_column_text(stmt, 6);
    const unsigned char *utc_col = sqlite3_column_text(stmt, 7);
    const unsigned char *call_col = sqlite3_column_text(stmt, 8);
    const unsigned char *band_col = sqlite3_column_text(stmt, 10);
    const unsigned char *mode_col = sqlite3_column_text(stmt, 11);
    const unsigned char *rst_col = sqlite3_column_text(stmt, 12);
    const unsigned char *comments_col = sqlite3_column_text(stmt, 13);
    const unsigned char *sent_col = sqlite3_column_text(stmt, 14);
    const unsigned char *recv_col = sqlite3_column_text(stmt, 15);
    const unsigned char *operator_mode_col = sqlite3_column_text(stmt, 16);
    const unsigned char *contest_id_col = sqlite3_column_text(stmt, 17);
    const unsigned char *country_col = sqlite3_column_text(stmt, 20);

    q->db_id = sqlite3_column_int64(stmt, 0);
    snprintf(q->qso_uid, sizeof(q->qso_uid), "%s",
         qso_uid_col ? (const char *)qso_uid_col : "");
    snprintf(q->origin_station_id, sizeof(q->origin_station_id), "%s",
         origin_station_col ? (const char *)origin_station_col : "");
    q->origin_station_seq = sqlite3_column_int64(stmt, 3);
    snprintf(q->last_modified_utc, sizeof(q->last_modified_utc), "%s",
         last_modified_col ? (const char *)last_modified_col : "");
    q->version = sqlite3_column_int(stmt, 5);
    snprintf(q->date, sizeof(q->date), "%s", date_col ? (const char *)date_col : "");
    snprintf(q->utc, sizeof(q->utc), "%s", utc_col ? (const char *)utc_col : "");
    snprintf(q->call, sizeof(q->call), "%s", call_col ? (const char *)call_col : "");
    q->freq = sqlite3_column_int(stmt, 9);
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
    q->radio_nr = sqlite3_column_int(stmt, 18);
    q->points = sqlite3_column_int(stmt, 19);
    snprintf(q->country, sizeof(q->country), "%s",
             country_col ? (const char *)country_col : "");
    q->cq_zone = sqlite3_column_int(stmt, 21);
    q->itu_zone = sqlite3_column_int(stmt, 22);
    q->invalid = sqlite3_column_int(stmt, 23) != 0;

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

  char station_id[32] = {0};
  if (qso->origin_station_id[0]) {
    snprintf(station_id, sizeof(station_id), "%s", qso->origin_station_id);
  } else if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0) {
    return -1;
  }

  long long station_seq = qso->origin_station_seq;
  if (station_seq <= 0 && db_sync_next_station_seq(&station_seq) != 0)
    return -1;

  int version = qso->version > 0 ? qso->version : 1;

  char modified_utc[32] = {0};
  if (qso->last_modified_utc[0])
    snprintf(modified_utc, sizeof(modified_utc), "%s", qso->last_modified_utc);
  else
    utc_now_iso(modified_utc, sizeof(modified_utc));

  char qso_uid[40] = {0};
  if (qso->qso_uid[0]) {
    snprintf(qso_uid, sizeof(qso_uid), "%s", qso->qso_uid);
  } else {
    char uid_token[25] = {0};
    if (sync_generate_hex_token(12, uid_token, sizeof(uid_token)) != 0)
      return -1;
    char short_station[9] = {0};
    size_t sid_len = strlen(station_id);
    const char *tail = sid_len > 8 ? station_id + sid_len - 8 : station_id;
    snprintf(short_station, sizeof(short_station), "%.8s", tail);
    snprintf(qso_uid, sizeof(qso_uid), "q-%s-%.12s", short_station, uid_token);
  }

  char op_id[96] = {0};
  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    return -1;
  snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
           op_token);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO qso (logbook_id,qso_uid,origin_station_id,origin_station_seq,last_op_id,last_modified_utc,version,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, logbook_id);
  sqlite3_bind_text(stmt, 2, qso_uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, station_seq);
  sqlite3_bind_text(stmt, 5, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, modified_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 7, version);
  sqlite3_bind_text(stmt, 8, qso->date, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, qso->utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, qso->call, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 11, qso->freq);
  sqlite3_bind_text(stmt, 12, qso->band, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13, qso->mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 14, qso->rst, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 15, qso->comments, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 16, qso->exchange_sent, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 17, qso->exchange_recv, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 18, qso->operator_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 19, qso->contest_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 20, qso->radio_nr);
  sqlite3_bind_int(stmt, 21, qso->points);
  sqlite3_bind_text(stmt, 22, qso->country, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 23, qso->cq_zone);
  sqlite3_bind_int(stmt, 24, qso->itu_zone);
  sqlite3_bind_int(stmt, 25, qso->invalid ? 1 : 0);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return -1;
  }

  if (out_id)
    *out_id = sqlite3_last_insert_rowid(db);

  sqlite3_finalize(stmt);

  char payload[2048] = {0};
  if (out_id && *out_id > 0)
    (void)sync_build_qso_payload_from_row(*out_id, logbook_id, payload,
                                          sizeof(payload));
  if (!payload[0]) {
    snprintf(payload, sizeof(payload),
             "{\"kind\":\"qso_full\",\"qso_uid\":\"%s\",\"version\":%d}",
             qso_uid, version);
  }
  (void)db_sync_outbox_enqueue(op_id, station_seq, logbook_id, "QSO_INSERT",
                               qso_uid, payload, modified_utc);

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

  char modified_utc[32] = {0};
  utc_now_iso(modified_utc, sizeof(modified_utc));

  char op_id[96] = {0};
  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  long long station_seq = 0;
  if (db_sync_next_station_seq(&station_seq) != 0)
    return -1;

  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    return -1;
  snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
           op_token);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE qso SET invalid = ?, version = version + 1, last_modified_utc = ?, last_op_id = ? WHERE id = ? AND logbook_id = ?;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, invalid ? 1 : 0);
  sqlite3_bind_text(stmt, 2, modified_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, id);
  sqlite3_bind_int(stmt, 5, logbook_id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_DONE) {
    char qso_uid[40] = {0};
    char origin_station_id[32] = {0};
    long long origin_seq = 0;
    int version = 0;
    if (sync_fetch_qso_meta(id, logbook_id, qso_uid, sizeof(qso_uid),
                            origin_station_id, sizeof(origin_station_id),
                            &origin_seq, &version) == 0) {
      char payload[2048] = {0};
      (void)sync_build_qso_payload_from_row(id, logbook_id, payload,
                                            sizeof(payload));
      if (!payload[0]) {
        snprintf(payload, sizeof(payload),
                 "{\"kind\":\"qso_full\",\"qso_uid\":\"%s\",\"invalid\":%s,\"version\":%d}",
                 qso_uid, invalid ? "true" : "false", version);
      }
      (void)db_sync_outbox_enqueue(op_id, station_seq, logbook_id,
                                   "QSO_INVALID", qso_uid, payload,
                                   modified_utc);
    }
  }

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

  char modified_utc[32] = {0};
  utc_now_iso(modified_utc, sizeof(modified_utc));

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  long long station_seq = 0;
  if (db_sync_next_station_seq(&station_seq) != 0)
    return -1;

  char op_id[96] = {0};
  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    return -1;
  snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
           op_token);

  if (radio_nr < 1)
    radio_nr = 1;
  if (points < 0)
    points = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE qso SET exchange_sent = ?, exchange_recv = ?, operator_mode = ?, contest_id = ?, radio_nr = ?, points = ?, version = version + 1, last_modified_utc = ?, last_op_id = ? WHERE id = ? AND logbook_id = ?;") !=
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
  sqlite3_bind_text(stmt, 7, modified_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 9, id);
  sqlite3_bind_int(stmt, 10, logbook_id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_DONE) {
    char qso_uid[40] = {0};
    char origin_station_id[32] = {0};
    long long origin_seq = 0;
    int version = 0;
    if (sync_fetch_qso_meta(id, logbook_id, qso_uid, sizeof(qso_uid),
                            origin_station_id, sizeof(origin_station_id),
                            &origin_seq, &version) == 0) {
      char payload[2048] = {0};
      (void)sync_build_qso_payload_from_row(id, logbook_id, payload,
                                            sizeof(payload));
      if (!payload[0]) {
        snprintf(payload, sizeof(payload),
                 "{\"kind\":\"qso_full\",\"qso_uid\":\"%s\",\"version\":%d,\"radio_nr\":%d,\"points\":%d}",
                 qso_uid, version, radio_nr, points);
      }
      (void)db_sync_outbox_enqueue(op_id, station_seq, logbook_id,
                                   "QSO_CONTEST", qso_uid, payload,
                                   modified_utc);
    }
  }

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

int db_sync_get_or_create_station_id(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return -1;

  out[0] = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT station_id FROM sync_identity WHERE id = 1 LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  int found = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *station_col = sqlite3_column_text(stmt, 0);
    if (station_col && station_col[0]) {
      snprintf(out, out_size, "%s", (const char *)station_col);
      found = 1;
    }
  }
  sqlite3_finalize(stmt);

  if (found)
    return 0;

  char token[25] = {0};
  if (sync_generate_hex_token(12, token, sizeof(token)) != 0)
    return -1;

  char station_id[32] = {0};
  snprintf(station_id, sizeof(station_id), "st-%s", token);

  sqlite3_stmt *insert_stmt = NULL;
  if (prepare_stmt(&insert_stmt,
                   "INSERT OR REPLACE INTO sync_identity "
                   "(id, station_id, station_name, role, created_utc) "
                   "VALUES (1, ?, '', 'client', CURRENT_TIMESTAMP);") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(insert_stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(insert_stmt);
  sqlite3_finalize(insert_stmt);

  if (rc != SQLITE_DONE)
    return -1;

  snprintf(out, out_size, "%s", station_id);
  return 0;
}

int db_sync_set_station_id(const char *station_id) {
  if (!station_id || !station_id[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT OR REPLACE INTO sync_identity "
                   "(id, station_id, station_name, role, created_utc) "
                   "VALUES (1, ?, '', 'client', CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_next_station_seq(long long *out_seq) {
  if (!out_seq)
    return -1;

  *out_seq = 0;

  if (db_init() != 0)
    return -1;

  char station_id[32] = {0};
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT MAX(v) FROM ("
                   "  SELECT IFNULL(MAX(station_seq), 0) AS v FROM log_outbox "
                   "  UNION ALL "
                   "  SELECT IFNULL(MAX(origin_station_seq), 0) AS v FROM qso WHERE origin_station_id = ? "
                   "  UNION ALL "
                   "  SELECT IFNULL(last_acked_local_seq, 0) AS v FROM sync_cursors WHERE id = 1"
                   ");") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);

  long long max_seq = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    max_seq = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);

  long long next_seq = max_seq + 1;
  sqlite3_stmt *update_stmt = NULL;
  if (prepare_stmt(&update_stmt,
                   "UPDATE sync_cursors SET last_acked_local_seq = ? WHERE id = 1;") ==
      SQLITE_OK) {
    sqlite3_bind_int64(update_stmt, 1, next_seq);
    sqlite3_step(update_stmt);
    sqlite3_finalize(update_stmt);
  }

  *out_seq = next_seq;
  return 0;
}

int db_sync_outbox_enqueue(const char *op_id, long long station_seq,
                          int logbook_id, const char *op_type,
                          const char *entity_id, const char *payload_json,
                          const char *op_utc) {
  if (!op_id || !op_id[0] || station_seq <= 0 || logbook_id <= 0 || !op_type ||
      !op_type[0] || !entity_id || !entity_id[0] || !payload_json ||
      !payload_json[0] || !op_utc || !op_utc[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT OR IGNORE INTO log_outbox "
                   "(op_id, station_seq, logbook_id, op_type, entity_id, payload_json, op_utc, status, retry_count, next_retry_utc) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', 0, CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, station_seq);
  sqlite3_bind_int(stmt, 3, logbook_id);
  sqlite3_bind_text(stmt, 4, op_type, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, entity_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, payload_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, op_utc, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_outbox_load_pending(SyncOutboxEntry *out, int max_items,
                                int *out_count) {
  if (!out || max_items <= 0)
    return -1;

  if (out_count)
    *out_count = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT op_id, station_seq, logbook_id, op_type, entity_id, payload_json, op_utc, retry_count "
                   "FROM log_outbox "
                   "WHERE (status = 'pending' OR status = 'sent') "
                   "AND next_retry_utc <= CURRENT_TIMESTAMP "
                   "ORDER BY station_seq ASC LIMIT ?;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, max_items);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_items) {
    SyncOutboxEntry *e = &out[count];
    memset(e, 0, sizeof(*e));

    const unsigned char *op_id_col = sqlite3_column_text(stmt, 0);
    const unsigned char *op_type_col = sqlite3_column_text(stmt, 3);
    const unsigned char *entity_col = sqlite3_column_text(stmt, 4);
    const unsigned char *payload_col = sqlite3_column_text(stmt, 5);
    const unsigned char *utc_col = sqlite3_column_text(stmt, 6);

    snprintf(e->op_id, sizeof(e->op_id), "%s",
             op_id_col ? (const char *)op_id_col : "");
    e->station_seq = sqlite3_column_int64(stmt, 1);
    e->logbook_id = sqlite3_column_int(stmt, 2);
    snprintf(e->op_type, sizeof(e->op_type), "%s",
             op_type_col ? (const char *)op_type_col : "");
    snprintf(e->entity_id, sizeof(e->entity_id), "%s",
             entity_col ? (const char *)entity_col : "");
    snprintf(e->payload_json, sizeof(e->payload_json), "%s",
             payload_col ? (const char *)payload_col : "");
    snprintf(e->op_utc, sizeof(e->op_utc), "%s",
             utc_col ? (const char *)utc_col : "");
    e->retry_count = sqlite3_column_int(stmt, 7);

    count++;
  }

  sqlite3_finalize(stmt);

  if (out_count)
    *out_count = count;

  return 0;
}

int db_sync_outbox_mark_sent(const char *op_id) {
  if (!op_id || !op_id[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE log_outbox "
                   "SET status = 'sent', next_retry_utc = CURRENT_TIMESTAMP "
                   "WHERE op_id = ?;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, op_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_outbox_mark_retry(const char *op_id, int delay_seconds) {
  if (!op_id || !op_id[0])
    return -1;

  if (delay_seconds < 1)
    delay_seconds = 1;

  if (db_init() != 0)
    return -1;

  char delay_sql[32] = {0};
  snprintf(delay_sql, sizeof(delay_sql), "+%d seconds", delay_seconds);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE log_outbox "
                   "SET retry_count = retry_count + 1, "
                   "status = CASE "
                   "  WHEN retry_count + 1 >= ? THEN 'failed' "
                   "  ELSE 'pending' "
                   "END, "
                   "next_retry_utc = CASE "
                   "  WHEN retry_count + 1 >= ? THEN next_retry_utc "
                   "  ELSE datetime('now', ?) "
                   "END "
                   "WHERE op_id = ? AND status != 'acked';") != SQLITE_OK)
    return -1;

  sqlite3_bind_int(stmt, 1, DB_SYNC_MAX_RETRY);
  sqlite3_bind_int(stmt, 2, DB_SYNC_MAX_RETRY);
  sqlite3_bind_text(stmt, 3, delay_sql, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, op_id, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_outbox_mark_acked(const char *op_id) {
  if (!op_id || !op_id[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE log_outbox SET status = 'acked' WHERE op_id = ?;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, op_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_get_pending_outbox_count(int *out_count) {
  if (!out_count)
    return -1;

  *out_count = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT COUNT(*) FROM log_outbox WHERE status = 'pending' OR status = 'sent';") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_get_failed_outbox_count(int *out_count) {
  if (!out_count)
    return -1;

  *out_count = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT COUNT(*) FROM log_outbox WHERE status = 'failed';") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_get_current_logbook_id(int *out_id) {
  if (!out_id)
    return -1;

  *out_id = 0;
  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT value FROM app_metadata WHERE key = 'current_logbook_id' LIMIT 1;") != SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_id = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_get_last_global_seq(long long *out_seq) {
  if (!out_seq)
    return -1;

  *out_seq = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT last_pulled_global_seq FROM sync_cursors WHERE id = 1 LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_seq = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_set_last_global_seq(long long seq) {
  if (seq < 0)
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE sync_cursors SET last_pulled_global_seq = ? WHERE id = 1;") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_int64(stmt, 1, seq);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_get_max_global_seq(long long *out_seq) {
  if (!out_seq)
    return -1;

  *out_seq = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT IFNULL(MAX(global_seq), 0) FROM log_ops;") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_seq = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_get_next_expected_station_seq(const char *station_id,
                                          long long *out_seq) {
  if (!station_id || !station_id[0] || !out_seq)
    return -1;

  *out_seq = 1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT IFNULL(MAX(station_seq), 0) FROM log_ops "
                   "WHERE station_id = ?;") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  long long max_seq = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    max_seq = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);
  *out_seq = max_seq + 1;
  return 0;
}

int db_sync_apply_remote_op(const char *op_id, const char *station_id,
                            long long station_seq, int logbook_id,
                            const char *op_type, const char *entity_id,
                            const char *payload_json, const char *op_utc,
                            long long *out_global_seq) {
  if (out_global_seq)
    *out_global_seq = 0;

  if (!op_id || !op_id[0] || !station_id || !station_id[0] || station_seq <= 0 ||
      logbook_id <= 0 || !op_type || !op_type[0] || !entity_id ||
      !entity_id[0] || !payload_json || !payload_json[0] || !op_utc ||
      !op_utc[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *exists = NULL;
  if (prepare_stmt(&exists,
                   "SELECT global_seq FROM log_ops WHERE op_id = ? LIMIT 1;") !=
      SQLITE_OK)
    return DB_SYNC_APPLY_ERR;
  sqlite3_bind_text(exists, 1, op_id, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(exists) == SQLITE_ROW) {
    if (out_global_seq)
      *out_global_seq = sqlite3_column_int64(exists, 0);
    sqlite3_finalize(exists);
    return DB_SYNC_APPLY_ALREADY_PRESENT;
  }
  sqlite3_finalize(exists);

  sqlite3_stmt *conflict = NULL;
  if (prepare_stmt(&conflict,
                   "SELECT op_id FROM log_ops WHERE station_id = ? AND station_seq = ? LIMIT 1;") !=
      SQLITE_OK)
    return DB_SYNC_APPLY_ERR;

  sqlite3_bind_text(conflict, 1, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(conflict, 2, station_seq);

  if (sqlite3_step(conflict) == SQLITE_ROW) {
    const unsigned char *existing_op = sqlite3_column_text(conflict, 0);
    if (!existing_op || strcmp((const char *)existing_op, op_id) != 0) {
      sqlite3_finalize(conflict);
      return DB_SYNC_APPLY_STATION_SEQ_CONFLICT;
    }
  }
  sqlite3_finalize(conflict);

  if (strncmp(op_type, "QSO_", 4) == 0) {
    char qso_uid_buf[40] = {0};
    if (sync_json_get_string(payload_json, "qso_uid", qso_uid_buf,
                             sizeof(qso_uid_buf)) == 0 && qso_uid_buf[0]) {
      sqlite3_stmt *uid_conflict = NULL;
      if (prepare_stmt(&uid_conflict,
                       "SELECT id FROM qso WHERE qso_uid = ? AND logbook_id = ? AND id NOT IN (SELECT id FROM qso WHERE qso_uid = ? AND logbook_id = ? AND origin_station_id = ? AND origin_station_seq = ?);") != SQLITE_OK)
        return DB_SYNC_APPLY_ERR;

      sqlite3_bind_text(uid_conflict, 1, qso_uid_buf, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(uid_conflict, 2, logbook_id);
      sqlite3_bind_text(uid_conflict, 3, qso_uid_buf, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(uid_conflict, 4, logbook_id);
      sqlite3_bind_text(uid_conflict, 5, station_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(uid_conflict, 6, station_seq);

      if (sqlite3_step(uid_conflict) == SQLITE_ROW) {
        sqlite3_finalize(uid_conflict);
        return DB_SYNC_APPLY_QSO_UID_CONFLICT;
      }
      sqlite3_finalize(uid_conflict);
    }
  }

  int changed = 0;
  if (strncmp(op_type, "QSO_", 4) == 0) {
    if (sync_qso_upsert_from_payload(op_id, station_id, station_seq, logbook_id,
                                     payload_json, &changed) != 0)
      return DB_SYNC_APPLY_ERR;
  }

  sqlite3_stmt *ins = NULL;
  if (prepare_stmt(&ins,
                   "INSERT INTO log_ops "
                   "(op_id, station_id, station_seq, logbook_id, op_type, entity_id, payload_json, op_utc) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?);") != SQLITE_OK)
    return DB_SYNC_APPLY_ERR;

  sqlite3_bind_text(ins, 1, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 2, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(ins, 3, station_seq);
  sqlite3_bind_int(ins, 4, logbook_id);
  sqlite3_bind_text(ins, 5, op_type, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 6, entity_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 7, payload_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 8, op_utc, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(ins);
  sqlite3_finalize(ins);
  if (rc != SQLITE_DONE)
    return DB_SYNC_APPLY_ERR;

  sqlite3_stmt *sel = NULL;
  if (prepare_stmt(&sel,
                   "SELECT global_seq FROM log_ops WHERE op_id = ? LIMIT 1;") ==
      SQLITE_OK) {
    sqlite3_bind_text(sel, 1, op_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) == SQLITE_ROW && out_global_seq)
      *out_global_seq = sqlite3_column_int64(sel, 0);
    sqlite3_finalize(sel);
  }

  return changed ? DB_SYNC_APPLY_CHANGED : DB_SYNC_APPLY_ALREADY_PRESENT;
}

int db_sync_pull_ops(long long from_global_seq, int limit, SyncLogOpEntry *out,
                     int max_items, int *out_count,
                     long long *out_last_global_seq) {
  if (!out || max_items <= 0 || !out_count || !out_last_global_seq ||
      from_global_seq < 0)
    return -1;

  *out_count = 0;
  *out_last_global_seq = from_global_seq;

  if (limit < 1)
    limit = 1;
  if (limit > max_items)
    limit = max_items;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT global_seq, op_id, station_id, station_seq, logbook_id, "
                   "op_type, entity_id, payload_json, op_utc "
                   "FROM log_ops WHERE global_seq > ? "
                   "ORDER BY global_seq ASC LIMIT ?;") != SQLITE_OK)
    return -1;

  sqlite3_bind_int64(stmt, 1, from_global_seq);
  sqlite3_bind_int(stmt, 2, limit);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_items) {
    SyncLogOpEntry *e = &out[count];
    memset(e, 0, sizeof(*e));

    e->global_seq = sqlite3_column_int64(stmt, 0);
    const unsigned char *op_id_col = sqlite3_column_text(stmt, 1);
    const unsigned char *station_col = sqlite3_column_text(stmt, 2);
    e->station_seq = sqlite3_column_int64(stmt, 3);
    e->logbook_id = sqlite3_column_int(stmt, 4);
    const unsigned char *op_type_col = sqlite3_column_text(stmt, 5);
    const unsigned char *entity_col = sqlite3_column_text(stmt, 6);
    const unsigned char *payload_col = sqlite3_column_text(stmt, 7);
    const unsigned char *utc_col = sqlite3_column_text(stmt, 8);

    snprintf(e->op_id, sizeof(e->op_id), "%s",
             op_id_col ? (const char *)op_id_col : "");
    snprintf(e->station_id, sizeof(e->station_id), "%s",
             station_col ? (const char *)station_col : "");
    snprintf(e->op_type, sizeof(e->op_type), "%s",
             op_type_col ? (const char *)op_type_col : "");
    snprintf(e->entity_id, sizeof(e->entity_id), "%s",
             entity_col ? (const char *)entity_col : "");
    snprintf(e->payload_json, sizeof(e->payload_json), "%s",
             payload_col ? (const char *)payload_col : "{}");
    snprintf(e->op_utc, sizeof(e->op_utc), "%s",
             utc_col ? (const char *)utc_col : "");

    *out_last_global_seq = e->global_seq;
    count++;
  }

  sqlite3_finalize(stmt);
  *out_count = count;
  return 0;
}

int db_sync_reserve_serial(int logbook_id, const char *station_id,
                           const char *request_id, int ttl_sec,
                           char *out_reservation_id,
                           size_t out_reservation_id_size, int *out_serial,
                           char *out_expires_utc,
                           size_t out_expires_utc_size) {
  if (logbook_id <= 0 || !station_id || !station_id[0] || !out_reservation_id ||
      out_reservation_id_size < 2 || !out_serial || !out_expires_utc ||
      out_expires_utc_size < 2)
    return -1;

  if (ttl_sec < 10)
    ttl_sec = 10;
  if (ttl_sec > 3600)
    ttl_sec = 3600;

  if (db_init() != 0)
    return -1;

  (void)db_sync_expire_serial_reservations();

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  sqlite3_stmt *init = NULL;
  if (prepare_stmt(&init,
                   "INSERT OR IGNORE INTO serial_alloc "
                   "(logbook_id, next_serial, updated_utc) "
                   "VALUES (?, 1, CURRENT_TIMESTAMP);") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_int(init, 1, logbook_id);
  if (sqlite3_step(init) != SQLITE_DONE) {
    sqlite3_finalize(init);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_finalize(init);

  sqlite3_stmt *sel = NULL;
  if (prepare_stmt(&sel,
                   "SELECT next_serial FROM serial_alloc WHERE logbook_id = ? LIMIT 1;") !=
      SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_int(sel, 1, logbook_id);
  if (sqlite3_step(sel) != SQLITE_ROW) {
    sqlite3_finalize(sel);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  int serial = sqlite3_column_int(sel, 0);
  sqlite3_finalize(sel);

  char token[25] = {0};
  if (sync_generate_hex_token(8, token, sizeof(token)) != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  char reservation_id[64] = {0};
  if (request_id && request_id[0])
    snprintf(reservation_id, sizeof(reservation_id), "rsv-%s", request_id);
  else
    snprintf(reservation_id, sizeof(reservation_id), "rsv-%s", token);

  char reserved_utc[32] = {0};
  char expires_utc[32] = {0};
  utc_now_iso(reserved_utc, sizeof(reserved_utc));
  utc_plus_seconds_iso(ttl_sec, expires_utc, sizeof(expires_utc));

  sqlite3_stmt *ins = NULL;
  if (prepare_stmt(&ins,
                   "INSERT INTO serial_reservations "
                   "(reservation_id, logbook_id, station_id, reserved_serial, status, reserved_utc, expires_utc, consumed_utc, consumed_qso_uid) "
                   "VALUES (?, ?, ?, ?, 'reserved', ?, ?, '', '');") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  sqlite3_bind_text(ins, 1, reservation_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins, 2, logbook_id);
  sqlite3_bind_text(ins, 3, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins, 4, serial);
  sqlite3_bind_text(ins, 5, reserved_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 6, expires_utc, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(ins) != SQLITE_DONE) {
    sqlite3_finalize(ins);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_finalize(ins);

  sqlite3_stmt *upd = NULL;
  if (prepare_stmt(&upd,
                   "UPDATE serial_alloc "
                   "SET next_serial = ?, updated_utc = CURRENT_TIMESTAMP "
                   "WHERE logbook_id = ?;") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_int(upd, 1, serial + 1);
  sqlite3_bind_int(upd, 2, logbook_id);
  if (sqlite3_step(upd) != SQLITE_DONE) {
    sqlite3_finalize(upd);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_finalize(upd);

  if (exec_sql_checked("COMMIT;") != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  snprintf(out_reservation_id, out_reservation_id_size, "%s", reservation_id);
  *out_serial = serial;
  snprintf(out_expires_utc, out_expires_utc_size, "%s", expires_utc);
  return 0;
}

int db_sync_commit_serial(const char *reservation_id, const char *qso_uid) {
  if (!reservation_id || !reservation_id[0] || !qso_uid || !qso_uid[0])
    return DB_SYNC_COMMIT_ERR;

  if (db_init() != 0)
    return DB_SYNC_COMMIT_ERR;

  (void)db_sync_expire_serial_reservations();

  char consumed_utc[32] = {0};
  utc_now_iso(consumed_utc, sizeof(consumed_utc));

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations "
                   "SET status = 'consumed', consumed_utc = ?, consumed_qso_uid = ? "
                   "WHERE reservation_id = ? AND status = 'reserved' "
                   "AND (expires_utc = '' OR expires_utc >= CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return DB_SYNC_COMMIT_ERR;

  sqlite3_bind_text(stmt, 1, consumed_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, qso_uid ? qso_uid : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, reservation_id, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    return DB_SYNC_COMMIT_ERR;

  return sqlite3_changes(db) > 0 ? DB_SYNC_COMMIT_OK : DB_SYNC_COMMIT_NOT_FOUND;
}

int db_sync_expire_serial_reservations(void) {
  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations "
                   "SET status = 'expired' "
                   "WHERE status = 'reserved' "
                   "AND expires_utc != '' AND expires_utc < CURRENT_TIMESTAMP;") !=
      SQLITE_OK)
    return -1;

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}