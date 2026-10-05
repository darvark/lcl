#include "db.h"

#include "config.h"
#include "qtc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct sqlite3_backup sqlite3_backup;

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
extern sqlite3_backup *sqlite3_backup_init(sqlite3 *dest, const char *dest_name,
                                          sqlite3 *src, const char *src_name);
extern int sqlite3_backup_step(sqlite3_backup *backup, int pages);
extern int sqlite3_backup_finish(sqlite3_backup *backup);

static sqlite3 *db = NULL;
static char db_path[512] = {0};
static char previous_db_path[512] = {0};
static char pending_logbook_name[64] = "GeneralLog";
static int db_initialized = 0;
static int db_is_default_path = 1;
static int db_bootstrap_import_done = 0;
static pthread_mutex_t db_operation_mutex;
static pthread_once_t db_operation_mutex_once = PTHREAD_ONCE_INIT;
static _Thread_local int db_transaction_lock_depth = 0;

#define DB_SYNC_MAX_RETRY 6

static int table_is_empty(const char *table);
static int table_has_column(const char *table, const char *column);
static int meta_get_int(const char *key, int *value);
static int meta_set_int(const char *key, int value);
static int meta_get_previous_log_available(int *value);
static int copy_table(const char *src, const char *dst, const char *columns);
static int exec_sql_checked(const char *sql);
static int named_logbook_exists(long long id);
static int has_suffix(const char *value, const char *suffix);
static void sanitize_log_name(const char *name, char *out, size_t out_size);
static void db_path_to_log_name(const char *path, char *out, size_t out_size);
static int copy_file_binary(const char *src, const char *dst);
static int migrate_legacy_default_db_if_needed(const char *new_default_path);
static int ensure_db_file_exists(const char *path);
static int resolve_logs_dir(char *out, size_t out_size);
static int build_log_db_path(const char *log_name, char *out, size_t out_size);
static void format_file_mtime(time_t t, char *out, size_t out_size);
static int count_qsos_for_file(const char *path, int *out_count);
static int list_log_db_files(char paths[][512], char names[][64],
                             struct stat stats[], int max_items,
                             int *out_count);
static int switch_to_db_file(const char *path, const char *log_name,
                             int remember_previous);
static int switch_to_named_log(const char *name, int fail_if_exists);
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

static void db_operation_mutex_init(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&db_operation_mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

static void db_operation_lock(void) {
  pthread_once(&db_operation_mutex_once, db_operation_mutex_init);
  pthread_mutex_lock(&db_operation_mutex);
}

static void db_operation_unlock(void) {
  pthread_mutex_unlock(&db_operation_mutex);
}

static int db_sqlite_step_locked(sqlite3_stmt *stmt) {
  db_operation_lock();
  int rc = sqlite3_step(stmt);
  db_operation_unlock();
  return rc;
}

#define sqlite3_step db_sqlite_step_locked

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

static int has_suffix(const char *value, const char *suffix) {
  if (!value || !suffix)
    return 0;

  size_t value_len = strlen(value);
  size_t suffix_len = strlen(suffix);
  if (suffix_len == 0 || value_len < suffix_len)
    return 0;

  return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static void sanitize_log_name(const char *name, char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  out[0] = 0;

  const char *src = (name && name[0]) ? name : "log";
  while (*src && isspace((unsigned char)*src))
    src++;

  size_t used = 0;
  for (size_t i = 0; src[i] && used < out_size - 1; i++) {
    unsigned char ch = (unsigned char)src[i];
    if (ch < 32)
      continue;
    if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
        ch == '"' || ch == '<' || ch == '>' || ch == '|')
      ch = '_';
    out[used++] = (char)ch;
  }

  while (used > 0 && isspace((unsigned char)out[used - 1]))
    used--;

  out[used] = 0;
  if (!out[0])
    snprintf(out, out_size, "%s", "log");
}

static void db_path_to_log_name(const char *path, char *out, size_t out_size) {
  if (!out || out_size < 2) {
    return;
  }

  out[0] = 0;
  if (!path || !path[0])
    return;

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  if (!name[0])
    return;

  snprintf(out, out_size, "%s", name);
  if (has_suffix(out, ".db")) {
    size_t len = strlen(out);
    if (len > 3)
      out[len - 3] = 0;
  }
}

static int copy_file_binary(const char *src, const char *dst) {
  if (!src || !src[0] || !dst || !dst[0])
    return -1;

  FILE *in = fopen(src, "rb");
  if (!in)
    return -1;

  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return -1;
  }

  char buffer[8192];
  size_t nread = 0;
  int rc = 0;

  while ((nread = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, nread, out) != nread) {
      rc = -1;
      break;
    }
  }

  if (ferror(in))
    rc = -1;

  fclose(out);
  fclose(in);
  return rc;
}

static int migrate_legacy_default_db_if_needed(const char *new_default_path) {
  if (!new_default_path || !new_default_path[0])
    return 0;

  if (access(new_default_path, F_OK) == 0)
    return 0;

  (void)config_ensure_runtime_layout();

  char candidates[2][512];
  memset(candidates, 0, sizeof(candidates));

  const char *runtime_dir = config_runtime_dir();
  if (runtime_dir && runtime_dir[0])
    snprintf(candidates[0], sizeof(candidates[0]), "%s/logger.db", runtime_dir);

  snprintf(candidates[1], sizeof(candidates[1]), "%s", "logger.db");

  for (size_t i = 0; i < 2; i++) {
    if (!candidates[i][0])
      continue;
    if (strcmp(candidates[i], new_default_path) == 0)
      continue;
    if (access(candidates[i], R_OK) != 0)
      continue;

    if (copy_file_binary(candidates[i], new_default_path) == 0)
      return 1;

    return -1;
  }

  return 0;
}

static int ensure_db_file_exists(const char *path) {
  if (!path || !path[0])
    return -1;

  FILE *f = fopen(path, "ab");
  if (!f)
    return -1;

  fclose(f);
  return 0;
}

static int resolve_logs_dir(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return -1;

  out[0] = 0;
  (void)config_ensure_runtime_layout();
  const char *runtime_dir = config_runtime_dir();

  if (!runtime_dir || !runtime_dir[0])
    return -1;

  snprintf(out, out_size, "%s/logs", runtime_dir);

  struct stat st;
  if (stat(out, &st) == 0)
    return S_ISDIR(st.st_mode) ? 0 : -1;

  if (mkdir(out, 0700) == 0)
    return 0;

  if (errno == EEXIST)
    return 0;

  return -1;
}

static int build_log_db_path(const char *log_name, char *out, size_t out_size) {
  if (!out || out_size < 2)
    return -1;

  char logs_dir[512] = {0};
  char safe_name[128] = {0};
  if (resolve_logs_dir(logs_dir, sizeof(logs_dir)) != 0)
    return -1;

  sanitize_log_name(log_name, safe_name, sizeof(safe_name));
  snprintf(out, out_size, "%s/%s.db", logs_dir, safe_name);
  return 0;
}

static void format_file_mtime(time_t t, char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_utc);
}

static int count_qsos_for_file(const char *path, int *out_count) {
  if (!path || !path[0] || !out_count)
    return -1;

  *out_count = 0;

  sqlite3 *tmp = NULL;
  if (sqlite3_open(path, &tmp) != SQLITE_OK) {
    if (tmp)
      sqlite3_close(tmp);
    return -1;
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(tmp, "SELECT COUNT(*) FROM qso;", -1, &stmt, NULL);
  if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);

  if (stmt)
    sqlite3_finalize(stmt);
  sqlite3_close(tmp);
  return 0;
}

static int stat_is_newer(const struct stat *left, const struct stat *right) {
  if (left->st_mtime != right->st_mtime)
    return left->st_mtime > right->st_mtime;
  return left->st_mtim.tv_nsec > right->st_mtim.tv_nsec;
}

static int list_log_db_files(char paths[][512], char names[][64],
                             struct stat stats[], int max_items,
                             int *out_count) {
  if (out_count)
    *out_count = 0;

  if (!paths || !names || !stats || max_items <= 0)
    return -1;

  char logs_dir[512] = {0};
  if (resolve_logs_dir(logs_dir, sizeof(logs_dir)) != 0)
    return -1;

  DIR *dir = opendir(logs_dir);
  if (!dir)
    return -1;

  struct dirent *entry = NULL;
  int count = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    if (!has_suffix(entry->d_name, ".db"))
      continue;

    char candidate_path[512] = {0};
    char candidate_name[64] = {0};
    struct stat candidate_stat;
    snprintf(candidate_path, sizeof(candidate_path), "%s/%s", logs_dir,
             entry->d_name);
    db_path_to_log_name(entry->d_name, candidate_name,
                        sizeof(candidate_name));

    if (stat(candidate_path, &candidate_stat) != 0)
      continue;

    int slot = count;
    if (count == max_items) {
      slot = 0;
      for (int i = 1; i < count; i++) {
        if (stat_is_newer(&stats[slot], &stats[i]))
          slot = i;
      }
      if (!stat_is_newer(&candidate_stat, &stats[slot]))
        continue;
    } else {
      count++;
    }

    snprintf(paths[slot], 512, "%s", candidate_path);
    snprintf(names[slot], 64, "%s", candidate_name);
    stats[slot] = candidate_stat;

  }

  closedir(dir);

  for (int i = 0; i < count; i++) {
    for (int j = i + 1; j < count; j++) {
      if (stat_is_newer(&stats[i], &stats[j]))
        continue;

      char tmp_path[512];
      char tmp_name[64];
      struct stat tmp_stat;

      snprintf(tmp_path, sizeof(tmp_path), "%s", paths[i]);
      snprintf(tmp_name, sizeof(tmp_name), "%s", names[i]);
      tmp_stat = stats[i];

      snprintf(paths[i], 512, "%s", paths[j]);
      snprintf(names[i], 64, "%s", names[j]);
      stats[i] = stats[j];

      snprintf(paths[j], 512, "%s", tmp_path);
      snprintf(names[j], 64, "%s", tmp_name);
      stats[j] = tmp_stat;
    }
  }

  if (out_count)
    *out_count = count;

  return 0;
}

static int switch_to_db_file_impl(const char *path, const char *log_name,
                                  int remember_previous) {
  if (!path || !path[0])
    return -1;

  char target_path[512] = {0};
  snprintf(target_path, sizeof(target_path), "%s", path);

  char target_name[64] = {0};
  if (log_name && log_name[0])
    sanitize_log_name(log_name, target_name, sizeof(target_name));
  else
    db_path_to_log_name(target_path, target_name, sizeof(target_name));

  if (!target_name[0])
    snprintf(target_name, sizeof(target_name), "%s", "GeneralLog");

  if (ensure_db_file_exists(target_path) != 0)
    return -1;

  char old_path[512] = {0};
  if (db_path[0])
    snprintf(old_path, sizeof(old_path), "%s", db_path);

  if (remember_previous && old_path[0] && strcmp(old_path, target_path) != 0)
    snprintf(previous_db_path, sizeof(previous_db_path), "%s", old_path);

  char saved_previous_path[sizeof(previous_db_path)] = {0};
  snprintf(saved_previous_path, sizeof(saved_previous_path), "%s",
           previous_db_path);
  if (db) {
    db_shutdown();
    snprintf(previous_db_path, sizeof(previous_db_path), "%s",
             saved_previous_path);
  }

  snprintf(db_path, sizeof(db_path), "%s", target_path);
  snprintf(pending_logbook_name, sizeof(pending_logbook_name), "%s",
           target_name);
  db_is_default_path = 0;
  db_bootstrap_import_done = 0;
  db_initialized = 0;

  if (db_init() != 0) {
    if (old_path[0]) {
      snprintf(db_path, sizeof(db_path), "%s", old_path);
      db_initialized = 0;
      (void)db_init();
    }
    return -1;
  }

  struct stat st;
  if (stat(target_path, &st) != 0 || !S_ISREG(st.st_mode)) {
    db_shutdown();
    if (old_path[0]) {
      snprintf(db_path, sizeof(db_path), "%s", old_path);
      db_initialized = 0;
      (void)db_init();
    }
    return -1;
  }

  return 0;
}

static int switch_to_db_file(const char *path, const char *log_name,
                             int remember_previous) {
  if (config.net_enabled)
    return DB_ERR_LOG_CHANGE_WHILE_NET_ACTIVE;
  db_operation_lock();
  int rc = switch_to_db_file_impl(path, log_name, remember_previous);
  db_operation_unlock();
  return rc;
}

static int switch_to_named_log_impl(const char *name, int fail_if_exists) {
  char db_file[512] = {0};
  char safe_name[64] = {0};

  sanitize_log_name(name, safe_name, sizeof(safe_name));
  if (!safe_name[0])
    return -1;

  if (build_log_db_path(safe_name, db_file, sizeof(db_file)) != 0)
    return -1;

  if (fail_if_exists && access(db_file, F_OK) == 0)
    return -1;

  if (!fail_if_exists && access(db_file, F_OK) != 0)
    return -1;

  return switch_to_db_file(db_file, safe_name, 1);
}

static int switch_to_named_log(const char *name, int fail_if_exists) {
  db_operation_lock();
  int rc = switch_to_named_log_impl(name, fail_if_exists);
  db_operation_unlock();
  return rc;
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

static int sync_shared_log_id_is_valid(const char *shared_log_id) {
  if (!shared_log_id || strlen(shared_log_id) != 35 ||
      strncmp(shared_log_id, "sl-", 3) != 0)
    return 0;

  for (size_t i = 3; i < 35; i++) {
    const char ch = shared_log_id[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return 0;
  }

  return 1;
}

int db_sync_validate_shared_log_id(const char *shared_log_id) {
  return sync_shared_log_id_is_valid(shared_log_id);
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
                     "INSERT INTO named_logbooks (name, created_at) VALUES (?, CURRENT_TIMESTAMP);") !=
        SQLITE_OK)
      return -1;

    bind_text_or_null(insert_stmt, 1,
                      pending_logbook_name[0] ? pending_logbook_name
                                              : "GeneralLog");

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
  if (!sql)
    return -1;

  const char *command = sql;
  while (*command == ' ' || *command == '\t' || *command == '\r' ||
         *command == '\n')
    command++;
  int begins_transaction = strncmp(command, "BEGIN", 5) == 0;
  int ends_transaction = strncmp(command, "COMMIT", 6) == 0 ||
                         strncmp(command, "ROLLBACK", 8) == 0;
  if (begins_transaction) {
    db_operation_lock();
    db_transaction_lock_depth++;
  }
  db_operation_lock();
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (err) {
    sqlite3_free(err);
  }
  db_operation_unlock();

  if (begins_transaction && rc != SQLITE_OK) {
    db_transaction_lock_depth--;
    db_operation_unlock();
  } else if (ends_transaction && rc == SQLITE_OK &&
             db_transaction_lock_depth > 0) {
    db_transaction_lock_depth--;
    db_operation_unlock();
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

static void adif_format_freq_mhz(int freq_khz, char *out, size_t out_size) {
  if (!out || out_size == 0)
    return;

  snprintf(out, out_size, "%.6f", freq_khz / 1000.0);
  for (size_t i = 0; out[i]; i++) {
    if (out[i] == ',')
      out[i] = '.';
  }
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
  const char *env_path = getenv("LOGGER_DB_PATH");
  const char *target_path = NULL;
  int using_default_path = 0;

  if (db_path[0]) {
    target_path = db_path;
  } else if (env_path && env_path[0]) {
    target_path = env_path;
  } else {
    if (build_log_db_path("GeneralLog", db_path, sizeof(db_path)) == 0)
      target_path = db_path;
    else
      target_path = "logger.db";
    using_default_path = 1;
  }

  if (db && (!target_path || strcmp(db_path, target_path) != 0)) {
    sqlite3_close(db);
    db = NULL;
    db_initialized = 0;
    db_path[0] = 0;
    previous_db_path[0] = 0;
    pending_logbook_name[0] = 0;
  }

  if (db)
    return 0;

  if (target_path != db_path)
    snprintf(db_path, sizeof(db_path), "%s", target_path);
  db_is_default_path = using_default_path;

  if (using_default_path) {
    int migrate_rc = migrate_legacy_default_db_if_needed(db_path);
    if (migrate_rc < 0)
      return -1;
  }

  if (ensure_db_file_exists(db_path) != 0)
    return -1;

  if (!pending_logbook_name[0]) {
    db_path_to_log_name(db_path, pending_logbook_name,
                        sizeof(pending_logbook_name));
    if (!pending_logbook_name[0])
      snprintf(pending_logbook_name, sizeof(pending_logbook_name), "%s",
               "GeneralLog");
  }

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
              "shared_log_id TEXT NOT NULL DEFAULT '',"
              "created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
              ");") != SQLITE_OK)
            return -1;

          if (exec_sql(
              "CREATE TABLE IF NOT EXISTS sync_cursors ("
              "id INTEGER PRIMARY KEY CHECK (id = 1),"
              "last_pulled_global_seq INTEGER NOT NULL DEFAULT 0,"
              "last_acked_local_seq INTEGER NOT NULL DEFAULT 0,"
              "last_server_epoch TEXT NOT NULL DEFAULT '',"
              "pending_serial_request_id TEXT NOT NULL DEFAULT ''"
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
              "request_id TEXT NOT NULL DEFAULT '',"
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

  if (!table_has_column("sync_cursors", "pending_serial_request_id")) {
    if (exec_sql_checked("ALTER TABLE sync_cursors ADD COLUMN pending_serial_request_id TEXT NOT NULL DEFAULT '';"))
      return -1;
  }

  if (!table_has_column("serial_reservations", "request_id")) {
    if (exec_sql_checked("ALTER TABLE serial_reservations ADD COLUMN request_id TEXT NOT NULL DEFAULT '';"))
      return -1;
  }
  if (exec_sql_checked(
          "UPDATE serial_reservations SET request_id = substr(reservation_id, 5) "
          "WHERE request_id = '' AND reservation_id LIKE 'rsv-%';") != 0)
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

  if (!table_has_column("sync_identity", "shared_log_id")) {
    if (exec_sql_checked("ALTER TABLE sync_identity ADD COLUMN shared_log_id TEXT NOT NULL DEFAULT '';") != 0)
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

  if (exec_sql_checked("UPDATE qso SET qso_uid = 'legacy-' || id WHERE qso_uid = '';") != 0)
    return -1;
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_qso_qso_uid ON qso(qso_uid);") != 0)
    return -1;
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_qso_origin_station ON qso(origin_station_id, origin_station_seq);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_qso_last_modified ON qso(last_modified_utc);");
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_log_outbox_status_retry ON log_outbox(status, next_retry_utc);");
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_log_outbox_op_id_unique ON log_outbox(op_id);") != 0)
    return -1;
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_log_outbox_station_seq_unique ON log_outbox(station_seq);") != 0)
    return -1;
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_log_ops_station_seq ON log_ops(station_id, station_seq);");
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_log_ops_op_id_unique ON log_ops(op_id);") != 0)
    return -1;
  if (exec_sql_checked("CREATE UNIQUE INDEX IF NOT EXISTS idx_log_ops_station_seq_unique ON log_ops(station_id, station_seq);") != 0)
    return -1;
  exec_sql_checked("CREATE INDEX IF NOT EXISTS idx_serial_reservations_lookup ON serial_reservations(logbook_id, station_id, status);");
  if (exec_sql_checked(
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_serial_reservations_request_unique "
          "ON serial_reservations(station_id, request_id) WHERE request_id != '';") != 0)
    return -1;

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
static void db_shutdown_impl(void) {
  if (db) {
    sqlite3_close(db);
    db = NULL;
  }

  db_initialized = 0;
  db_path[0] = 0;
  previous_db_path[0] = 0;
  pending_logbook_name[0] = 0;
  db_is_default_path = 1;
  db_bootstrap_import_done = 0;
}

void db_shutdown(void) {
  db_operation_lock();
  db_shutdown_impl();
  db_operation_unlock();
}

/*
 * Initialize the database layer and open the SQLite database.
 *
 * @return 0 on success, or -1 on failure.
 */
static int db_init_impl(void) {
  const char *env_path = getenv("LOGGER_DB_PATH");
  const char *target_path = db_path[0] ? db_path
                                       : env_path && env_path[0] ? env_path : NULL;

  if (db_initialized && db && db_path[0] && target_path &&
      strcmp(db_path, target_path) == 0)
    return 0;

  if (db) {
    if (!target_path || !db_path[0] || strcmp(db_path, target_path) != 0) {
      sqlite3_close(db);
      db = NULL;
      db_initialized = 0;
      db_path[0] = 0;
      previous_db_path[0] = 0;
      pending_logbook_name[0] = 0;
    }
  }

  if (ensure_open() != 0)
    return -1;

  db_initialized = 1;
  return 0;
}

int db_init(void) {
  db_operation_lock();
  int rc = db_init_impl();
  db_operation_unlock();
  return rc;
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
static int db_insert_qso_impl(QSO *qso, long long *out_id) {
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

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  long long station_seq = qso->origin_station_seq;
  if (station_seq <= 0 && db_sync_next_station_seq(&station_seq) != 0)
    goto rollback;

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
      goto rollback;
    char short_station[9] = {0};
    size_t sid_len = strlen(station_id);
    const char *tail = sid_len > 8 ? station_id + sid_len - 8 : station_id;
    snprintf(short_station, sizeof(short_station), "%.8s", tail);
    snprintf(qso_uid, sizeof(qso_uid), "q-%s-%.12s", short_station, uid_token);
  }

  char op_id[96] = {0};
  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    goto rollback;
  snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
           op_token);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT INTO qso (logbook_id,qso_uid,origin_station_id,origin_station_seq,last_op_id,last_modified_utc,version,date,utc,call,freq,band,mode,rst,comments,exchange_sent,exchange_recv,operator_mode,contest_id,radio_nr,points,country,cq_zone,itu_zone,invalid) "
                   "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);") != SQLITE_OK)
    goto rollback;

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
    goto rollback;
  }

  long long inserted_id = sqlite3_last_insert_rowid(db);

  sqlite3_finalize(stmt);

  char payload[2048] = {0};
  if (sync_build_qso_payload_from_row(inserted_id, logbook_id, payload,
                                      sizeof(payload)) != 0 || !payload[0])
    goto rollback;
  if (db_sync_outbox_enqueue(op_id, station_seq, logbook_id, "QSO_INSERT",
                             qso_uid, payload, modified_utc) != 0)
    goto rollback;
  if (exec_sql_checked("COMMIT;") != 0)
    goto rollback;

  if (out_id)
    *out_id = inserted_id;
  snprintf(qso->origin_station_id, sizeof(qso->origin_station_id), "%s",
           station_id);
  qso->origin_station_seq = station_seq;
  snprintf(qso->qso_uid, sizeof(qso->qso_uid), "%s", qso_uid);
  snprintf(qso->last_modified_utc, sizeof(qso->last_modified_utc), "%s",
           modified_utc);
  qso->version = version;
  qso->db_id = inserted_id;

  return 0;

rollback:
  (void)exec_sql_checked("ROLLBACK;");
  return -1;
}

int db_insert_qso(QSO *qso, long long *out_id) {
  db_operation_lock();
  int rc = db_insert_qso_impl(qso, out_id);
  db_operation_unlock();
  return rc;
}

/*
 * Update the invalid flag for a stored QSO row.
 *
 * @param id Row id to update.
 * @param invalid Nonzero marks the row invalid.
 * @return 0 on success, or -1 on failure.
 */
static int db_update_qso_invalid_impl(long long id, int invalid) {
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

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  long long station_seq = 0;
  if (db_sync_next_station_seq(&station_seq) != 0)
    goto invalid_rollback;

  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    goto invalid_rollback;
  snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
           op_token);

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE qso SET invalid = ?, version = version + 1, last_modified_utc = ?, last_op_id = ? WHERE id = ? AND logbook_id = ?;") !=
      SQLITE_OK)
    goto invalid_rollback;

  sqlite3_bind_int(stmt, 1, invalid ? 1 : 0);
  sqlite3_bind_text(stmt, 2, modified_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, id);
  sqlite3_bind_int(stmt, 5, logbook_id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE)
    goto invalid_rollback;

  char qso_uid[40] = {0};
  char origin_station_id[32] = {0};
  long long origin_seq = 0;
  int version = 0;
  char payload[2048] = {0};
  if (sync_fetch_qso_meta(id, logbook_id, qso_uid, sizeof(qso_uid),
                          origin_station_id, sizeof(origin_station_id),
                          &origin_seq, &version) != 0 || !qso_uid[0] ||
      sync_build_qso_payload_from_row(id, logbook_id, payload,
                                      sizeof(payload)) != 0 || !payload[0] ||
      db_sync_outbox_enqueue(op_id, station_seq, logbook_id, "QSO_INVALID",
                             qso_uid, payload, modified_utc) != 0 ||
      exec_sql_checked("COMMIT;") != 0)
    goto invalid_rollback;

  return 0;

invalid_rollback:
  (void)exec_sql_checked("ROLLBACK;");
  return -1;
}

int db_update_qso_invalid(long long id, int invalid) {
  db_operation_lock();
  int rc = db_update_qso_invalid_impl(id, invalid);
  db_operation_unlock();
  return rc;
}

static int db_update_qso_contest_fields_with_reservation_impl(
  long long id, const char *exchange_sent, const char *exchange_recv,
  const char *operator_mode, const char *contest_id, int radio_nr,
  int points, const char *reservation_id, int commit_remote) {
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

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  long long station_seq = 0;
  if (db_sync_next_station_seq(&station_seq) != 0)
    goto contest_rollback;

  char op_id[96] = {0};
  char op_token[17] = {0};
  if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
    goto contest_rollback;
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
    goto contest_rollback;

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

  if (rc != SQLITE_DONE)
    goto contest_rollback;

  char qso_uid[40] = {0};
  char origin_station_id[32] = {0};
  long long origin_seq = 0;
  int version = 0;
  char payload[2048] = {0};
  if (sync_fetch_qso_meta(id, logbook_id, qso_uid, sizeof(qso_uid),
                          origin_station_id, sizeof(origin_station_id),
                          &origin_seq, &version) != 0 || !qso_uid[0] ||
      sync_build_qso_payload_from_row(id, logbook_id, payload,
                                      sizeof(payload)) != 0 || !payload[0] ||
      db_sync_outbox_enqueue(op_id, station_seq, logbook_id, "QSO_CONTEST",
                             qso_uid, payload, modified_utc) != 0)
    goto contest_rollback;

  if (reservation_id && reservation_id[0]) {
    sqlite3_stmt *reservation = NULL;
    if (prepare_stmt(&reservation,
                     "UPDATE serial_reservations SET status = ?, "
                     "consumed_utc = ?, consumed_qso_uid = ? "
                     "WHERE reservation_id = ? AND status IN ('claimed', "
                     "'reserved') AND datetime(expires_utc) >= CURRENT_TIMESTAMP;") !=
        SQLITE_OK)
      goto contest_rollback;
    sqlite3_bind_text(reservation, 1,
                       commit_remote ? "commit_pending" : "consumed", -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_text(reservation, 2, modified_utc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(reservation, 3, qso_uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(reservation, 4, reservation_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(reservation);
    sqlite3_finalize(reservation);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
      goto contest_rollback;
  }

  if (exec_sql_checked("COMMIT;") != 0)
    goto contest_rollback;

  return 0;

contest_rollback:
  (void)exec_sql_checked("ROLLBACK;");
  return -1;
}

int db_update_qso_contest_fields_with_reservation(
    long long id, const char *exchange_sent, const char *exchange_recv,
    const char *operator_mode, const char *contest_id, int radio_nr,
    int points, const char *reservation_id, int commit_remote) {
  db_operation_lock();
  int rc = db_update_qso_contest_fields_with_reservation_impl(
      id, exchange_sent, exchange_recv, operator_mode, contest_id, radio_nr,
      points, reservation_id, commit_remote);
  db_operation_unlock();
  return rc;
}

int db_update_qso_contest_fields(long long id, const char *exchange_sent,
                                 const char *exchange_recv,
                                 const char *operator_mode,
                                 const char *contest_id, int radio_nr,
                                 int points) {
  return db_update_qso_contest_fields_with_reservation(
      id, exchange_sent, exchange_recv, operator_mode, contest_id, radio_nr,
      points, NULL, 0);
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
  if (config.net_enabled)
    return DB_ERR_LOG_CHANGE_WHILE_NET_ACTIVE;

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
  time_t now = time(NULL);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);

  char base_name[64] = {0};
  strftime(base_name, sizeof(base_name), "log_%Y%m%d_%H%M%S", &tm_utc);

  if (db_archive_current_logbook_named(base_name) == 0)
    return 0;

  for (int i = 2; i <= 99; i++) {
    char candidate[64] = {0};
    snprintf(candidate, sizeof(candidate), "%s_%d", base_name, i);
    if (db_archive_current_logbook_named(candidate) == 0)
      return 0;
  }

  return -1;
}

/*
 * Open the previously archived named logbook.
 *
 * @return 0 on success, or -1 on failure.
 */
int db_open_previous_logbook(void) {
  if (!previous_db_path[0])
    return -1;

  if (db_init() != 0)
    return -1;

  char current_path[512] = {0};
  if (db_path[0])
    snprintf(current_path, sizeof(current_path), "%s", db_path);
  else
    return -1;

  char target_path[512] = {0};
  snprintf(target_path, sizeof(target_path), "%s", previous_db_path);

  char target_name[64] = {0};
  db_path_to_log_name(target_path, target_name, sizeof(target_name));

  if (switch_to_db_file(target_path, target_name, 0) != 0)
    return -1;

  snprintf(previous_db_path, sizeof(previous_db_path), "%s", current_path);

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

  char db_file[512] = {0};
  char safe_name[64] = {0};

  sanitize_log_name(name, safe_name, sizeof(safe_name));
  if (!safe_name[0])
    return -1;

  if (build_log_db_path(safe_name, db_file, sizeof(db_file)) != 0)
    return -1;

  db_operation_lock();
  int rc = -1;
  int created = 0;
  sqlite3 *archive_db = NULL;
  sqlite3_backup *backup = NULL;
  if (access(db_file, F_OK) == 0 || db_init() != 0 || !db)
    goto cleanup;

  int create_flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_NOFOLLOW
  create_flags |= O_NOFOLLOW;
#endif
  int fd = open(db_file, create_flags, S_IRUSR | S_IWUSR);
  if (fd < 0)
    goto cleanup;
  created = 1;
  close(fd);

  if (sqlite3_open(db_file, &archive_db) != SQLITE_OK || !archive_db)
    goto cleanup;
  (void)sqlite3_busy_timeout(archive_db, 3000);
  backup = sqlite3_backup_init(archive_db, "main", db, "main");
  if (!backup)
    goto cleanup;
  if (sqlite3_backup_step(backup, -1) != SQLITE_DONE)
    goto cleanup;
  if (sqlite3_backup_finish(backup) != SQLITE_OK) {
    backup = NULL;
    goto cleanup;
  }
  backup = NULL;
  if (sqlite3_close(archive_db) != SQLITE_OK)
    goto cleanup;
  archive_db = NULL;

  snprintf(previous_db_path, sizeof(previous_db_path), "%s", db_file);
  rc = 0;

cleanup:
  if (backup)
    (void)sqlite3_backup_finish(backup);
  if (archive_db)
    (void)sqlite3_close(archive_db);
  if (rc != 0 && created)
    (void)unlink(db_file);
  db_operation_unlock();
  return rc;
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

  char paths[128][512];
  char names[128][64];
  struct stat stats[128];
  int listed = 0;

  if (max_items > 128)
    max_items = 128;

  if (list_log_db_files(paths, names, stats, max_items, &listed) != 0)
    return -1;

  int count = 0;
  for (int i = 0; i < listed && count < max_items; i++) {
    DBNamedLogbook *item = &out[count];
    memset(item, 0, sizeof(*item));

    item->id = i + 1;
    strncpy(item->name, names[i], sizeof(item->name) - 1);
    item->name[sizeof(item->name) - 1] = 0;
    format_file_mtime(stats[i].st_mtime, item->created_at,
                      sizeof(item->created_at));

    int qso_count = 0;
    if (count_qsos_for_file(paths[i], &qso_count) == 0)
      item->qso_count = qso_count;
    else
      item->qso_count = 0;

    count++;
  }

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

  char paths[256][512];
  char names[256][64];
  struct stat stats[256];
  int listed = 0;

  if (list_log_db_files(paths, names, stats, 256, &listed) != 0)
    return -1;

  if (id > listed)
    return -1;

  return switch_to_db_file(paths[id - 1], names[id - 1], 1);
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

  return switch_to_named_log(name, 0);
}

int db_open_logbook_file(const char *path) {
  if (!path || !path[0])
    return -1;

  if (access(path, R_OK) != 0)
    return -1;

  char log_name[64] = {0};
  db_path_to_log_name(path, log_name, sizeof(log_name));
  if (!log_name[0])
    snprintf(log_name, sizeof(log_name), "%s", "log");

  return switch_to_db_file(path, log_name, 1);
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
      char freq_text[32] = {0};
      adif_format_freq_mhz(freq, freq_text, sizeof(freq_text));

      fprintf(f, "<CALL:%zu>%s", strlen(call), call);
      fprintf(f, "<QSO_DATE:8>%s", date);
      fprintf(f, "<TIME_ON:4>%s", utc);
      fprintf(f, "<FREQ:%zu>%s", strlen(freq_text), freq_text);
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
           "WHERE logbook_id = %d ORDER BY id ASC;",
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
           "WHERE logbook_id = %d ORDER BY id ASC;",
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

int db_sync_get_shared_log_id(char *out, size_t out_size) {
  if (!out || out_size < 36)
    return -1;

  out[0] = 0;
  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT shared_log_id FROM sync_identity WHERE id = 1 LIMIT 1;") !=
      SQLITE_OK)
    return -1;

  int result = 1;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *id = sqlite3_column_text(stmt, 0);
    if (id && id[0]) {
      if (!sync_shared_log_id_is_valid((const char *)id)) {
        result = -1;
      } else {
        snprintf(out, out_size, "%s", (const char *)id);
        result = 0;
      }
    }
  } else if (rc != SQLITE_DONE) {
    result = -1;
  }

  sqlite3_finalize(stmt);
  return result;
}

int db_sync_set_shared_log_id(const char *shared_log_id) {
  if (!sync_shared_log_id_is_valid(shared_log_id) || db_init() != 0 ||
      exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT shared_log_id FROM sync_identity WHERE id = 1 LIMIT 1;") !=
      SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  int row_exists = 0;
  char existing[36] = {0};
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    row_exists = 1;
    const unsigned char *id = sqlite3_column_text(stmt, 0);
    if (id)
      snprintf(existing, sizeof(existing), "%s", (const char *)id);
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_finalize(stmt);

  if (existing[0]) {
    if (!sync_shared_log_id_is_valid(existing) ||
        strcmp(existing, shared_log_id) != 0) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    return exec_sql_checked("COMMIT;") == 0 ? 0 : -1;
  }

  if (row_exists) {
    if (prepare_stmt(&stmt,
                     "UPDATE sync_identity SET shared_log_id = ? WHERE id = 1 AND shared_log_id = '';") !=
        SQLITE_OK) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    sqlite3_bind_text(stmt, 1, shared_log_id, -1, SQLITE_TRANSIENT);
  } else {
    if (prepare_stmt(&stmt,
                     "INSERT INTO sync_identity (id, station_id, station_name, role, shared_log_id, created_utc) "
                     "VALUES (1, '', '', 'client', ?, CURRENT_TIMESTAMP);") !=
        SQLITE_OK) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    sqlite3_bind_text(stmt, 1, shared_log_id, -1, SQLITE_TRANSIENT);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE || sqlite3_changes(db) != 1 ||
      exec_sql_checked("COMMIT;") != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  return 0;
}

int db_sync_create_shared_log_id(char *out, size_t out_size) {
  if (!out || out_size < 36)
    return -1;

  out[0] = 0;
  if (db_init() != 0 || exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT shared_log_id FROM sync_identity WHERE id = 1 LIMIT 1;") !=
      SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  int row_exists = 0;
  char existing[36] = {0};
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    row_exists = 1;
    const unsigned char *id = sqlite3_column_text(stmt, 0);
    if (id)
      snprintf(existing, sizeof(existing), "%s", (const char *)id);
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_finalize(stmt);

  if (existing[0]) {
    if (!sync_shared_log_id_is_valid(existing) ||
        exec_sql_checked("COMMIT;") != 0) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    snprintf(out, out_size, "%s", existing);
    return 0;
  }

  char token[33] = {0};
  if (sync_generate_hex_token(16, token, sizeof(token)) != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  char shared_log_id[36] = {0};
  snprintf(shared_log_id, sizeof(shared_log_id), "sl-%s", token);
  if (row_exists) {
    if (prepare_stmt(&stmt,
                     "UPDATE sync_identity SET shared_log_id = ?, role = 'server' WHERE id = 1 AND shared_log_id = '';") !=
        SQLITE_OK) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    sqlite3_bind_text(stmt, 1, shared_log_id, -1, SQLITE_TRANSIENT);
  } else {
    if (prepare_stmt(&stmt,
                     "INSERT INTO sync_identity (id, station_id, station_name, role, shared_log_id, created_utc) "
                     "VALUES (1, '', '', 'server', ?, CURRENT_TIMESTAMP);") !=
        SQLITE_OK) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    sqlite3_bind_text(stmt, 1, shared_log_id, -1, SQLITE_TRANSIENT);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE || sqlite3_changes(db) != 1 ||
      exec_sql_checked("COMMIT;") != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  snprintf(out, out_size, "%s", shared_log_id);
  return 0;
}

static int db_sync_get_or_create_station_id_impl(char *out, size_t out_size) {
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
                   "INSERT OR IGNORE INTO sync_identity "
                   "(id, station_id, station_name, role, created_utc) "
                   "VALUES (1, ?, '', 'client', CURRENT_TIMESTAMP);") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(insert_stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(insert_stmt);
  sqlite3_finalize(insert_stmt);

  if (rc != SQLITE_DONE)
    return -1;

  if (prepare_stmt(&insert_stmt,
                   "UPDATE sync_identity SET station_id = ? WHERE id = 1;") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(insert_stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(insert_stmt);
  sqlite3_finalize(insert_stmt);
  if (rc != SQLITE_DONE)
    return -1;

  snprintf(out, out_size, "%s", station_id);
  return 0;
}

int db_sync_get_or_create_station_id(char *out, size_t out_size) {
  db_operation_lock();
  int rc = db_sync_get_or_create_station_id_impl(out, out_size);
  db_operation_unlock();
  return rc;
}

int db_sync_set_station_id(const char *station_id) {
  if (!station_id || !station_id[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT OR IGNORE INTO sync_identity "
                   "(id, station_id, station_name, role, created_utc) "
                   "VALUES (1, ?, '', 'client', CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    return -1;

  if (prepare_stmt(&stmt,
                   "UPDATE sync_identity SET station_id = ? WHERE id = 1;") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

static int db_sync_next_station_seq_impl(long long *out_seq) {
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
                   "  SELECT IFNULL(MAX(station_seq), 0) AS v FROM log_ops WHERE station_id = ? "
                   "  UNION ALL "
                   "  SELECT IFNULL(last_acked_local_seq, 0) AS v FROM sync_cursors WHERE id = 1"
                   ");") != SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, station_id, -1, SQLITE_TRANSIENT);

  long long max_seq = 0;
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  max_seq = sqlite3_column_int64(stmt, 0);

  sqlite3_finalize(stmt);

  long long next_seq = max_seq + 1;
  sqlite3_stmt *update_stmt = NULL;
  if (prepare_stmt(&update_stmt,
                   "UPDATE sync_cursors SET last_acked_local_seq = ? WHERE id = 1;") ==
      SQLITE_OK) {
    sqlite3_bind_int64(update_stmt, 1, next_seq);
    rc = sqlite3_step(update_stmt);
    sqlite3_finalize(update_stmt);
  } else {
    return -1;
  }
  if (rc != SQLITE_DONE)
    return -1;

  *out_seq = next_seq;
  return 0;
}

int db_sync_next_station_seq(long long *out_seq) {
  db_operation_lock();
  int rc = db_sync_next_station_seq_impl(out_seq);
  db_operation_unlock();
  return rc;
}

static int db_sync_outbox_enqueue_impl(const char *op_id, long long station_seq,
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
                   "INSERT INTO log_outbox "
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

int db_sync_outbox_enqueue(const char *op_id, long long station_seq,
                          int logbook_id, const char *op_type,
                          const char *entity_id, const char *payload_json,
                          const char *op_utc) {
  db_operation_lock();
  int rc = db_sync_outbox_enqueue_impl(op_id, station_seq, logbook_id, op_type,
                                       entity_id, payload_json, op_utc);
  db_operation_unlock();
  return rc;
}

static int db_sync_outbox_load_pending_impl(SyncOutboxEntry *out, int max_items,
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

int db_sync_outbox_load_pending(SyncOutboxEntry *out, int max_items,
                                int *out_count) {
  db_operation_lock();
  int rc = db_sync_outbox_load_pending_impl(out, max_items, out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_load_failed_outbox_impl(SyncOutboxEntry *out,
                                           int max_items, int *out_count) {
  if (!out || max_items <= 0 || !out_count || db_init() != 0)
    return -1;
  *out_count = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT op_id, station_seq, logbook_id, op_type, entity_id, "
                   "payload_json, op_utc, retry_count FROM log_outbox "
                   "WHERE status = 'failed' ORDER BY station_seq ASC LIMIT ?;") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, max_items);

  int count = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_items) {
    SyncOutboxEntry *entry = &out[count];
    memset(entry, 0, sizeof(*entry));
    const unsigned char *op_id = sqlite3_column_text(stmt, 0);
    const unsigned char *op_type = sqlite3_column_text(stmt, 3);
    const unsigned char *entity_id = sqlite3_column_text(stmt, 4);
    const unsigned char *payload = sqlite3_column_text(stmt, 5);
    const unsigned char *op_utc = sqlite3_column_text(stmt, 6);
    snprintf(entry->op_id, sizeof(entry->op_id), "%s",
             op_id ? (const char *)op_id : "");
    entry->station_seq = sqlite3_column_int64(stmt, 1);
    entry->logbook_id = sqlite3_column_int(stmt, 2);
    snprintf(entry->op_type, sizeof(entry->op_type), "%s",
             op_type ? (const char *)op_type : "");
    snprintf(entry->entity_id, sizeof(entry->entity_id), "%s",
             entity_id ? (const char *)entity_id : "");
    snprintf(entry->payload_json, sizeof(entry->payload_json), "%s",
             payload ? (const char *)payload : "");
    snprintf(entry->op_utc, sizeof(entry->op_utc), "%s",
             op_utc ? (const char *)op_utc : "");
    entry->retry_count = sqlite3_column_int(stmt, 7);
    count++;
  }
  sqlite3_finalize(stmt);
  *out_count = count;
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_load_failed_outbox(SyncOutboxEntry *out, int max_items,
                               int *out_count) {
  db_operation_lock();
  int rc = db_sync_load_failed_outbox_impl(out, max_items, out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_retry_failed_outbox_impl(const char *op_id) {
  if (!op_id || !op_id[0] || db_init() != 0)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE log_outbox SET status = 'pending', retry_count = 0, "
                   "next_retry_utc = CURRENT_TIMESTAMP "
                   "WHERE op_id = ? AND status = 'failed';") != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, op_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int db_sync_retry_failed_outbox(const char *op_id) {
  db_operation_lock();
  int rc = db_sync_retry_failed_outbox_impl(op_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_outbox_mark_sent_impl(const char *op_id) {
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

int db_sync_outbox_mark_sent(const char *op_id) {
  db_operation_lock();
  int rc = db_sync_outbox_mark_sent_impl(op_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_outbox_mark_retry_impl(const char *op_id,
                                          int delay_seconds) {
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
                   "  WHEN retry_count + 1 >= ? THEN CURRENT_TIMESTAMP "
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

int db_sync_outbox_mark_retry(const char *op_id, int delay_seconds) {
  db_operation_lock();
  int rc = db_sync_outbox_mark_retry_impl(op_id, delay_seconds);
  db_operation_unlock();
  return rc;
}

static int db_sync_outbox_mark_acked_impl(const char *op_id) {
  if (!op_id || !op_id[0])
    return -1;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE log_outbox "
                   "SET status = 'acked', next_retry_utc = CURRENT_TIMESTAMP "
                   "WHERE op_id = ? AND status != 'acked';") !=
      SQLITE_OK)
    return -1;

  sqlite3_bind_text(stmt, 1, op_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_outbox_mark_acked(const char *op_id) {
  db_operation_lock();
  int rc = db_sync_outbox_mark_acked_impl(op_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_get_pending_outbox_count_impl(int *out_count) {
  if (!out_count)
    return -1;

  *out_count = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT (SELECT COUNT(*) FROM log_outbox "
                   "WHERE status = 'pending' OR status = 'sent') + "
                   "(SELECT COUNT(*) FROM serial_reservations "
                   "WHERE status = 'commit_pending');") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_get_pending_outbox_count(int *out_count) {
  db_operation_lock();
  int rc = db_sync_get_pending_outbox_count_impl(out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_get_failed_outbox_count_impl(int *out_count) {
  if (!out_count)
    return -1;

  *out_count = 0;

  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT (SELECT COUNT(*) FROM log_outbox WHERE status = 'failed') + "
                   "(SELECT COUNT(*) FROM serial_reservations "
                   "WHERE status = 'commit_failed');") !=
      SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_sync_get_failed_outbox_count(int *out_count) {
  db_operation_lock();
  int rc = db_sync_get_failed_outbox_count_impl(out_count);
  db_operation_unlock();
  return rc;
}

static int db_get_current_logbook_id_impl(int *out_id) {
  if (!out_id)
    return -1;

  *out_id = 0;
  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT value FROM app_meta WHERE key = 'current_logbook_id' LIMIT 1;") != SQLITE_OK)
    return -1;

  if (sqlite3_step(stmt) == SQLITE_ROW)
    *out_id = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  return 0;
}

int db_get_current_logbook_id(int *out_id) {
  db_operation_lock();
  int rc = db_get_current_logbook_id_impl(out_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_get_last_global_seq_impl(long long *out_seq) {
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

int db_sync_get_last_global_seq(long long *out_seq) {
  db_operation_lock();
  int rc = db_sync_get_last_global_seq_impl(out_seq);
  db_operation_unlock();
  return rc;
}

static int db_sync_set_last_global_seq_impl(long long seq) {
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

int db_sync_set_last_global_seq(long long seq) {
  db_operation_lock();
  int rc = db_sync_set_last_global_seq_impl(seq);
  db_operation_unlock();
  return rc;
}

static int db_sync_get_max_global_seq_impl(long long *out_seq) {
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

int db_sync_get_max_global_seq(long long *out_seq) {
  db_operation_lock();
  int rc = db_sync_get_max_global_seq_impl(out_seq);
  db_operation_unlock();
  return rc;
}

static int db_sync_get_next_expected_station_seq_impl(const char *station_id,
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

int db_sync_get_next_expected_station_seq(const char *station_id,
                                          long long *out_seq) {
  db_operation_lock();
  int rc = db_sync_get_next_expected_station_seq_impl(station_id, out_seq);
  db_operation_unlock();
  return rc;
}

static int db_sync_apply_remote_op_with_cursor_impl(
  const char *op_id, const char *station_id, long long station_seq,
  int logbook_id, const char *op_type, const char *entity_id,
  const char *payload_json, const char *op_utc, long long global_seq,
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

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return DB_SYNC_APPLY_ERR;

  int apply_result = DB_SYNC_APPLY_ERR;
  int changed = 0;
  int rc = SQLITE_OK;

  sqlite3_stmt *exists = NULL;
  if (prepare_stmt(&exists,
                   "SELECT global_seq FROM log_ops WHERE op_id = ? LIMIT 1;") !=
      SQLITE_OK)
    goto apply_rollback;
  sqlite3_bind_text(exists, 1, op_id, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(exists);
  if (rc == SQLITE_ROW) {
    if (out_global_seq)
      *out_global_seq = sqlite3_column_int64(exists, 0);
    sqlite3_finalize(exists);
    apply_result = DB_SYNC_APPLY_ALREADY_PRESENT;
    goto apply_commit;
  }
  sqlite3_finalize(exists);
  if (rc != SQLITE_DONE)
    goto apply_rollback;

  sqlite3_stmt *conflict = NULL;
  if (prepare_stmt(&conflict,
                   "SELECT op_id FROM log_ops WHERE station_id = ? AND station_seq = ? LIMIT 1;") !=
      SQLITE_OK)
    goto apply_rollback;

  sqlite3_bind_text(conflict, 1, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(conflict, 2, station_seq);

  rc = sqlite3_step(conflict);
  if (rc == SQLITE_ROW) {
    const unsigned char *existing_op = sqlite3_column_text(conflict, 0);
    if (!existing_op || strcmp((const char *)existing_op, op_id) != 0) {
      sqlite3_finalize(conflict);
      apply_result = DB_SYNC_APPLY_STATION_SEQ_CONFLICT;
      goto apply_rollback_result;
    }
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(conflict);
    goto apply_rollback;
  }
  sqlite3_finalize(conflict);

  if (strncmp(op_type, "QSO_", 4) == 0) {
    char qso_uid_buf[40] = {0};
    if (sync_json_get_string(payload_json, "qso_uid", qso_uid_buf,
                             sizeof(qso_uid_buf)) == 0 && qso_uid_buf[0]) {
      char qso_origin_station_id[32] = {0};
      long long qso_origin_station_seq = station_seq;
      if (sync_json_get_string(payload_json, "origin_station_id",
                               qso_origin_station_id,
                               sizeof(qso_origin_station_id)) != 0 ||
          !qso_origin_station_id[0])
        snprintf(qso_origin_station_id, sizeof(qso_origin_station_id), "%s",
                 station_id);
      if (sync_json_get_i64(payload_json, "origin_station_seq",
                            &qso_origin_station_seq) != 0 ||
          qso_origin_station_seq <= 0)
        qso_origin_station_seq = station_seq;

      sqlite3_stmt *uid_conflict = NULL;
      if (prepare_stmt(&uid_conflict,
                       "SELECT id FROM qso WHERE qso_uid = ? AND logbook_id = ? AND id NOT IN (SELECT id FROM qso WHERE qso_uid = ? AND logbook_id = ? AND origin_station_id = ? AND origin_station_seq = ?);") != SQLITE_OK)
        goto apply_rollback;

      sqlite3_bind_text(uid_conflict, 1, qso_uid_buf, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(uid_conflict, 2, logbook_id);
      sqlite3_bind_text(uid_conflict, 3, qso_uid_buf, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(uid_conflict, 4, logbook_id);
      sqlite3_bind_text(uid_conflict, 5, qso_origin_station_id, -1,
            SQLITE_TRANSIENT);
      sqlite3_bind_int64(uid_conflict, 6, qso_origin_station_seq);

      rc = sqlite3_step(uid_conflict);
      if (rc == SQLITE_ROW) {
        sqlite3_finalize(uid_conflict);
        apply_result = DB_SYNC_APPLY_QSO_UID_CONFLICT;
        goto apply_rollback_result;
      }
      sqlite3_finalize(uid_conflict);
      if (rc != SQLITE_DONE)
        goto apply_rollback;
    }
  }

  if (strncmp(op_type, "QSO_", 4) == 0) {
    if (sync_qso_upsert_from_payload(op_id, station_id, station_seq, logbook_id,
                                     payload_json, &changed) != 0)
      goto apply_rollback;
  }

  sqlite3_stmt *ins = NULL;
  if (prepare_stmt(&ins,
                   "INSERT INTO log_ops "
                   "(op_id, station_id, station_seq, logbook_id, op_type, entity_id, payload_json, op_utc) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?);") != SQLITE_OK)
          goto apply_rollback;

  sqlite3_bind_text(ins, 1, op_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 2, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(ins, 3, station_seq);
  sqlite3_bind_int(ins, 4, logbook_id);
  sqlite3_bind_text(ins, 5, op_type, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 6, entity_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 7, payload_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 8, op_utc, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(ins);
  sqlite3_finalize(ins);
  if (rc != SQLITE_DONE)
    goto apply_rollback;

  sqlite3_stmt *sel = NULL;
  if (prepare_stmt(&sel,
                   "SELECT global_seq FROM log_ops WHERE op_id = ? LIMIT 1;") !=
      SQLITE_OK)
    goto apply_rollback;
  sqlite3_bind_text(sel, 1, op_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(sel);
  if (rc == SQLITE_ROW && out_global_seq)
    *out_global_seq = sqlite3_column_int64(sel, 0);
  sqlite3_finalize(sel);
  if (rc != SQLITE_ROW)
    goto apply_rollback;

  apply_result = changed ? DB_SYNC_APPLY_CHANGED : DB_SYNC_APPLY_ALREADY_PRESENT;

apply_commit:
  if (global_seq > 0) {
    sqlite3_stmt *cursor = NULL;
    if (prepare_stmt(&cursor,
                     "UPDATE sync_cursors SET last_pulled_global_seq = "
                     "CASE WHEN last_pulled_global_seq < ? THEN ? "
                     "ELSE last_pulled_global_seq END WHERE id = 1;") !=
        SQLITE_OK)
      goto apply_rollback;
    sqlite3_bind_int64(cursor, 1, global_seq);
    sqlite3_bind_int64(cursor, 2, global_seq);
    rc = sqlite3_step(cursor);
    sqlite3_finalize(cursor);
    if (rc != SQLITE_DONE)
      goto apply_rollback;
  }

  if (exec_sql_checked("COMMIT;") != 0)
    goto apply_rollback;

  if (changed)
    qso_init();

  return apply_result;

apply_rollback_result:
  (void)exec_sql_checked("ROLLBACK;");
  return apply_result;

apply_rollback:
  (void)exec_sql_checked("ROLLBACK;");
  return DB_SYNC_APPLY_ERR;
}

int db_sync_apply_remote_op_with_cursor(
    const char *op_id, const char *station_id, long long station_seq,
    int logbook_id, const char *op_type, const char *entity_id,
    const char *payload_json, const char *op_utc, long long global_seq,
    long long *out_global_seq) {
  db_operation_lock();
  int rc = db_sync_apply_remote_op_with_cursor_impl(
      op_id, station_id, station_seq, logbook_id, op_type, entity_id,
      payload_json, op_utc, global_seq, out_global_seq);
  db_operation_unlock();
  return rc;
}

int db_sync_apply_remote_op(const char *op_id, const char *station_id,
                            long long station_seq, int logbook_id,
                            const char *op_type, const char *entity_id,
                            const char *payload_json, const char *op_utc,
                            long long *out_global_seq) {
  return db_sync_apply_remote_op_with_cursor(
      op_id, station_id, station_seq, logbook_id, op_type, entity_id,
      payload_json, op_utc, 0, out_global_seq);
}

static int db_sync_pull_ops_impl(long long from_global_seq, int limit,
                                 SyncLogOpEntry *out, int max_items,
                                 int *out_count,
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

static int db_sync_backfill_local_qsos_impl(int logbook_id,
                                            const char *station_id) {
  if (logbook_id <= 0 || !station_id || !station_id[0])
    return -1;

  for (;;) {
    sqlite3_stmt *select = NULL;
    if (prepare_stmt(&select,
                     "SELECT id, qso_uid, origin_station_id, origin_station_seq, "
                     "last_modified_utc FROM qso AS q "
                     "WHERE logbook_id = ? "
                     "AND (origin_station_id = '' OR origin_station_id = ?) "
                     "AND NOT EXISTS (SELECT 1 FROM log_outbox AS o "
                     "WHERE o.op_id = q.last_op_id) "
                     "AND NOT EXISTS (SELECT 1 FROM log_ops AS p "
                     "WHERE p.op_id = q.last_op_id) "
                     "ORDER BY id LIMIT 1;") != SQLITE_OK)
      return -1;
    sqlite3_bind_int(select, 1, logbook_id);
    sqlite3_bind_text(select, 2, station_id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(select);
    if (rc == SQLITE_DONE) {
      sqlite3_finalize(select);
      return 0;
    }
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(select);
      return -1;
    }

    long long qso_id = sqlite3_column_int64(select, 0);
    char qso_uid[40] = {0};
    char origin_station_id[32] = {0};
    long long origin_station_seq = sqlite3_column_int64(select, 3);
    char op_utc[32] = {0};
    const unsigned char *uid_col = sqlite3_column_text(select, 1);
    const unsigned char *origin_col = sqlite3_column_text(select, 2);
    const unsigned char *utc_col = sqlite3_column_text(select, 4);
    snprintf(qso_uid, sizeof(qso_uid), "%s", uid_col ? (const char *)uid_col : "");
    snprintf(origin_station_id, sizeof(origin_station_id), "%s",
             origin_col ? (const char *)origin_col : "");
    snprintf(op_utc, sizeof(op_utc), "%s", utc_col ? (const char *)utc_col : "");
    sqlite3_finalize(select);

    if (qso_id <= 0 || !qso_uid[0])
      return -1;

    long long station_seq = 0;
    if (db_sync_next_station_seq_impl(&station_seq) != 0)
      return -1;
    if (origin_station_seq <= 0)
      origin_station_seq = station_seq;
    if (!origin_station_id[0])
      snprintf(origin_station_id, sizeof(origin_station_id), "%s", station_id);
    if (!op_utc[0])
      utc_now_iso(op_utc, sizeof(op_utc));

    char op_token[17] = {0};
    char op_id[96] = {0};
    if (sync_generate_hex_token(8, op_token, sizeof(op_token)) != 0)
      return -1;
    snprintf(op_id, sizeof(op_id), "op-%s-%lld-%s", station_id, station_seq,
             op_token);

    sqlite3_stmt *update = NULL;
    if (prepare_stmt(&update,
                     "UPDATE qso SET origin_station_id = ?, origin_station_seq = ?, "
                     "last_op_id = ?, last_modified_utc = CASE "
                     "WHEN last_modified_utc = '' THEN ? ELSE last_modified_utc END, "
                     "version = CASE WHEN version < 1 THEN 1 ELSE version END "
                     "WHERE id = ? AND logbook_id = ?;") != SQLITE_OK)
      return -1;
    sqlite3_bind_text(update, 1, origin_station_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update, 2, origin_station_seq);
    sqlite3_bind_text(update, 3, op_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update, 4, op_utc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update, 5, qso_id);
    sqlite3_bind_int(update, 6, logbook_id);
    rc = sqlite3_step(update);
    sqlite3_finalize(update);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
      return -1;

    char payload[2048] = {0};
    if (sync_build_qso_payload_from_row(qso_id, logbook_id, payload,
                                        sizeof(payload)) != 0 ||
        !payload[0] ||
        db_sync_outbox_enqueue_impl(op_id, station_seq, logbook_id,
                                    "QSO_INSERT", qso_uid, payload,
                                    op_utc) != 0)
      return -1;
  }
}

static int db_sync_publish_local_logbook_ops_impl(void) {
  if (db_init() != 0)
    return -1;

  int logbook_id = 0;
  char station_id[32] = {0};
  if (get_current_logbook_id(&logbook_id) != 0 || logbook_id <= 0 ||
      db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0)
    return -1;

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  if (db_sync_backfill_local_qsos_impl(logbook_id, station_id) != 0)
    goto publish_rollback;

  sqlite3_stmt *publish = NULL;
  if (prepare_stmt(&publish,
                   "INSERT INTO log_ops "
                   "(op_id, station_id, station_seq, logbook_id, op_type, "
                   "entity_id, payload_json, op_utc) "
                   "SELECT o.op_id, ?, o.station_seq, o.logbook_id, o.op_type, "
                   "o.entity_id, o.payload_json, o.op_utc "
                   "FROM log_outbox AS o WHERE o.logbook_id = ? "
                   "AND NOT EXISTS (SELECT 1 FROM log_ops AS p "
                   "WHERE p.op_id = o.op_id) "
                   "ORDER BY o.station_seq, o.id;") != SQLITE_OK)
    goto publish_rollback;
  sqlite3_bind_text(publish, 1, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(publish, 2, logbook_id);
  int rc = sqlite3_step(publish);
  sqlite3_finalize(publish);
  if (rc != SQLITE_DONE)
    goto publish_rollback;

  sqlite3_stmt *ack = NULL;
  if (prepare_stmt(&ack,
                   "UPDATE log_outbox SET status = 'acked', "
                   "next_retry_utc = CURRENT_TIMESTAMP "
                   "WHERE logbook_id = ? AND EXISTS ("
                   "SELECT 1 FROM log_ops WHERE log_ops.op_id = log_outbox.op_id "
                   "AND log_ops.station_id = ?);") != SQLITE_OK)
    goto publish_rollback;
  sqlite3_bind_int(ack, 1, logbook_id);
  sqlite3_bind_text(ack, 2, station_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(ack);
  sqlite3_finalize(ack);
  if (rc != SQLITE_DONE)
    goto publish_rollback;

  if (exec_sql_checked("COMMIT;") != 0)
    goto publish_rollback;
  return 0;

publish_rollback:
  (void)exec_sql_checked("ROLLBACK;");
  return -1;
}

int db_sync_publish_local_logbook_ops(void) {
  db_operation_lock();
  int rc = db_sync_publish_local_logbook_ops_impl();
  db_operation_unlock();
  return rc;
}

int db_sync_pull_ops(long long from_global_seq, int limit, SyncLogOpEntry *out,
                     int max_items, int *out_count,
                     long long *out_last_global_seq) {
  db_operation_lock();
  int rc = db_sync_pull_ops_impl(from_global_seq, limit, out, max_items,
                                 out_count, out_last_global_seq);
  db_operation_unlock();
  return rc;
}

static int sync_existing_serial_max(int logbook_id, int *out_max_serial) {
  if (logbook_id <= 0 || !out_max_serial)
    return -1;
  *out_max_serial = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT IFNULL(MAX(CAST(exchange_sent AS INTEGER)), 0) "
                   "FROM qso WHERE logbook_id = ? "
                   "AND exchange_sent GLOB '[0-9]*';") != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, logbook_id);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    *out_max_serial = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 0 : -1;
}

int db_sync_get_or_create_serial_request_id(const char *candidate_request_id,
                                             char *out_request_id,
                                             size_t out_request_id_size) {
  if (!candidate_request_id || !candidate_request_id[0] ||
      strlen(candidate_request_id) >= 64 || !out_request_id ||
      out_request_id_size < 2 || db_init() != 0)
    return -1;
  out_request_id[0] = 0;
  db_operation_lock();

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0) {
    db_operation_unlock();
    return -1;
  }

  sqlite3_stmt *select_stmt = NULL;
  if (prepare_stmt(&select_stmt,
                   "SELECT pending_serial_request_id FROM sync_cursors "
                   "WHERE id = 1 LIMIT 1;") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    db_operation_unlock();
    return -1;
  }

  int rc = sqlite3_step(select_stmt);
  char pending_id[64] = {0};
  if (rc == SQLITE_ROW) {
    const unsigned char *stored = sqlite3_column_text(select_stmt, 0);
    if (stored)
      snprintf(pending_id, sizeof(pending_id), "%s", (const char *)stored);
  }
  sqlite3_finalize(select_stmt);
  if (rc != SQLITE_ROW) {
    (void)exec_sql_checked("ROLLBACK;");
    db_operation_unlock();
    return -1;
  }

  if (!pending_id[0]) {
    sqlite3_stmt *update_stmt = NULL;
    if (prepare_stmt(&update_stmt,
                     "UPDATE sync_cursors SET pending_serial_request_id = ? "
                     "WHERE id = 1 AND pending_serial_request_id = '';") !=
        SQLITE_OK) {
      (void)exec_sql_checked("ROLLBACK;");
      db_operation_unlock();
      return -1;
    }
    sqlite3_bind_text(update_stmt, 1, candidate_request_id, -1,
                      SQLITE_TRANSIENT);
    rc = sqlite3_step(update_stmt);
    sqlite3_finalize(update_stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1) {
      (void)exec_sql_checked("ROLLBACK;");
      db_operation_unlock();
      return -1;
    }
    snprintf(pending_id, sizeof(pending_id), "%s", candidate_request_id);
  }

  if (strlen(pending_id) >= out_request_id_size ||
      exec_sql_checked("COMMIT;") != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    db_operation_unlock();
    return -1;
  }
  snprintf(out_request_id, out_request_id_size, "%s", pending_id);
  db_operation_unlock();
  return 0;
}

int db_sync_clear_serial_request_id(const char *request_id) {
  if (!request_id || !request_id[0] || db_init() != 0)
    return -1;
  db_operation_lock();
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE sync_cursors SET pending_serial_request_id = '' "
                   "WHERE id = 1 AND pending_serial_request_id = ?;") !=
      SQLITE_OK) {
    db_operation_unlock();
    return -1;
  }
  sqlite3_bind_text(stmt, 1, request_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  db_operation_unlock();
  return rc == SQLITE_DONE ? 0 : -1;
}

static int db_sync_peek_next_serial_impl(int logbook_id, int *out_serial) {
  if (logbook_id <= 0 || !out_serial || db_init() != 0)
    return -1;

  int max_existing = 0;
  if (sync_existing_serial_max(logbook_id, &max_existing) != 0)
    return -1;

  int next_serial = 1;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT next_serial FROM serial_alloc WHERE logbook_id = ? LIMIT 1;") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, logbook_id);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    next_serial = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (next_serial <= max_existing)
    next_serial = max_existing + 1;
  *out_serial = next_serial;
  return 0;
}

int db_sync_peek_next_serial(int logbook_id, int *out_serial) {
  db_operation_lock();
  int rc = db_sync_peek_next_serial_impl(logbook_id, out_serial);
  db_operation_unlock();
  return rc;
}

static int db_sync_reserve_serial_impl(int logbook_id, const char *station_id,
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

  char generated_request_id[64] = {0};
  if (!request_id || !request_id[0]) {
    char request_token[25] = {0};
    if (sync_generate_hex_token(12, request_token, sizeof(request_token)) != 0)
      return -1;
    snprintf(generated_request_id, sizeof(generated_request_id), "req-%s",
             request_token);
    request_id = generated_request_id;
  }
  if (strlen(request_id) >= sizeof(generated_request_id))
    return -1;

  (void)db_sync_expire_serial_reservations();

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  sqlite3_stmt *existing_stmt = NULL;
  if (prepare_stmt(&existing_stmt,
                   "SELECT reservation_id,reserved_serial,logbook_id,expires_utc "
                   "FROM serial_reservations WHERE station_id = ? "
                   "AND request_id = ? LIMIT 1;") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_text(existing_stmt, 1, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(existing_stmt, 2, request_id, -1, SQLITE_TRANSIENT);
  int existing_rc = sqlite3_step(existing_stmt);
  if (existing_rc == SQLITE_ROW) {
    const unsigned char *existing_reservation =
        sqlite3_column_text(existing_stmt, 0);
    const unsigned char *existing_expiry = sqlite3_column_text(existing_stmt, 3);
    int existing_serial = sqlite3_column_int(existing_stmt, 1);
    int existing_logbook_id = sqlite3_column_int(existing_stmt, 2);
    char saved_reservation_id[64] = {0};
    char saved_expires_utc[32] = {0};
    snprintf(saved_reservation_id, sizeof(saved_reservation_id), "%s",
             existing_reservation ? (const char *)existing_reservation : "");
    snprintf(saved_expires_utc, sizeof(saved_expires_utc), "%s",
             existing_expiry ? (const char *)existing_expiry : "");
    sqlite3_finalize(existing_stmt);
    if (existing_logbook_id != logbook_id || !saved_reservation_id[0] ||
        strlen(saved_reservation_id) >= out_reservation_id_size ||
        strlen(saved_expires_utc) >= out_expires_utc_size ||
        exec_sql_checked("COMMIT;") != 0) {
      (void)exec_sql_checked("ROLLBACK;");
      return -1;
    }
    snprintf(out_reservation_id, out_reservation_id_size, "%s",
             saved_reservation_id);
    snprintf(out_expires_utc, out_expires_utc_size, "%s", saved_expires_utc);
    *out_serial = existing_serial;
    return 0;
  }
  sqlite3_finalize(existing_stmt);
  if (existing_rc != SQLITE_DONE) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

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

  int max_existing_serial = 0;
  if (sync_existing_serial_max(logbook_id, &max_existing_serial) != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  if (serial <= max_existing_serial)
    serial = max_existing_serial + 1;

  char token[25] = {0};
  if (sync_generate_hex_token(8, token, sizeof(token)) != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  char reservation_id[64] = {0};
  snprintf(reservation_id, sizeof(reservation_id), "rsv-%s", token);

  char reserved_utc[32] = {0};
  char expires_utc[32] = {0};
  utc_now_iso(reserved_utc, sizeof(reserved_utc));
  utc_plus_seconds_iso(ttl_sec, expires_utc, sizeof(expires_utc));

  sqlite3_stmt *ins = NULL;
  if (prepare_stmt(&ins,
                   "INSERT INTO serial_reservations "
                   "(reservation_id, request_id, logbook_id, station_id, reserved_serial, status, reserved_utc, expires_utc, consumed_utc, consumed_qso_uid) "
                   "VALUES (?, ?, ?, ?, ?, 'reserved', ?, ?, '', '');") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }

  sqlite3_bind_text(ins, 1, reservation_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 2, request_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins, 3, logbook_id);
  sqlite3_bind_text(ins, 4, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins, 5, serial);
  sqlite3_bind_text(ins, 6, reserved_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins, 7, expires_utc, -1, SQLITE_TRANSIENT);

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

int db_sync_reserve_serial(int logbook_id, const char *station_id,
                           const char *request_id, int ttl_sec,
                           char *out_reservation_id,
                           size_t out_reservation_id_size, int *out_serial,
                           char *out_expires_utc,
                           size_t out_expires_utc_size) {
  db_operation_lock();
  int rc = db_sync_reserve_serial_impl(
      logbook_id, station_id, request_id, ttl_sec, out_reservation_id,
      out_reservation_id_size, out_serial, out_expires_utc,
      out_expires_utc_size);
  db_operation_unlock();
  return rc;
}

static int db_sync_commit_serial_impl(const char *reservation_id,
                                      const char *qso_uid) {
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
                   "AND (expires_utc = '' OR datetime(expires_utc) >= CURRENT_TIMESTAMP);") !=
      SQLITE_OK)
    return DB_SYNC_COMMIT_ERR;

  sqlite3_bind_text(stmt, 1, consumed_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, qso_uid ? qso_uid : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, reservation_id, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    return DB_SYNC_COMMIT_ERR;

  if (sqlite3_changes(db) > 0)
    return DB_SYNC_COMMIT_OK;

  sqlite3_stmt *existing = NULL;
  if (prepare_stmt(&existing,
                   "SELECT consumed_qso_uid FROM serial_reservations "
                   "WHERE reservation_id = ? AND status = 'consumed' LIMIT 1;") !=
      SQLITE_OK)
    return DB_SYNC_COMMIT_ERR;
  sqlite3_bind_text(existing, 1, reservation_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(existing);
  const unsigned char *consumed_uid =
      rc == SQLITE_ROW ? sqlite3_column_text(existing, 0) : NULL;
  int already_committed =
      consumed_uid && strcmp((const char *)consumed_uid, qso_uid) == 0;
  sqlite3_finalize(existing);
  return already_committed ? DB_SYNC_COMMIT_OK : DB_SYNC_COMMIT_NOT_FOUND;
}

int db_sync_commit_serial(const char *reservation_id, const char *qso_uid) {
  db_operation_lock();
  int rc = db_sync_commit_serial_impl(reservation_id, qso_uid);
  db_operation_unlock();
  return rc;
}

static int db_sync_expire_serial_reservations_impl(void) {
  if (db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations "
                   "SET status = 'expired' "
                   "WHERE status IN ('reserved', 'available', 'claimed') "
                   "AND expires_utc != '' "
                   "AND datetime(expires_utc) < CURRENT_TIMESTAMP;") !=
      SQLITE_OK)
    return -1;

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_expire_serial_reservations(void) {
  db_operation_lock();
  int rc = db_sync_expire_serial_reservations_impl();
  db_operation_unlock();
  return rc;
}

static int db_sync_cache_serial_reservation_impl(const char *reservation_id,
                                     int logbook_id, const char *station_id,
                                     int serial, const char *expires_utc) {
  if (!reservation_id || !reservation_id[0] || logbook_id <= 0 || !station_id ||
      !station_id[0] || serial <= 0 || !expires_utc || !expires_utc[0] ||
      db_init() != 0)
    return -1;

  char reserved_utc[32] = {0};
  utc_now_iso(reserved_utc, sizeof(reserved_utc));
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "INSERT OR IGNORE INTO serial_reservations "
                   "(reservation_id,logbook_id,station_id,reserved_serial,status,"
                   "reserved_utc,expires_utc,consumed_utc,consumed_qso_uid) "
                   "VALUES (?,?,?,?,'available',?,?, '', '');") != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, reservation_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, logbook_id);
  sqlite3_bind_text(stmt, 3, station_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, serial);
  sqlite3_bind_text(stmt, 5, reserved_utc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, expires_utc, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_cache_serial_reservation(const char *reservation_id,
                                     int logbook_id, const char *station_id,
                                     int serial, const char *expires_utc) {
  db_operation_lock();
  int rc = db_sync_cache_serial_reservation_impl(
      reservation_id, logbook_id, station_id, serial, expires_utc);
  db_operation_unlock();
  return rc;
}

static int db_sync_count_available_serial_reservations_impl(int logbook_id,
                                                const char *station_id,
                                                int *out_count) {
  if (!out_count || logbook_id <= 0 || !station_id || !station_id[0] ||
      db_init() != 0)
    return -1;
  *out_count = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT COUNT(*) FROM serial_reservations "
                   "WHERE logbook_id = ? AND station_id = ? "
                   "AND status = 'available' "
                   "AND datetime(expires_utc) >= CURRENT_TIMESTAMP;") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, logbook_id);
  sqlite3_bind_text(stmt, 2, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    *out_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 0 : -1;
}

int db_sync_count_available_serial_reservations(int logbook_id,
                                                const char *station_id,
                                                int *out_count) {
  db_operation_lock();
  int rc = db_sync_count_available_serial_reservations_impl(
      logbook_id, station_id, out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_peek_available_serial_reservation_impl(int logbook_id,
                                              const char *station_id,
                                              int *out_serial) {
  if (!out_serial || logbook_id <= 0 || !station_id || !station_id[0] ||
      db_init() != 0)
    return -1;
  *out_serial = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT reserved_serial FROM serial_reservations "
                   "WHERE logbook_id = ? AND station_id = ? "
                   "AND status = 'available' "
                   "AND datetime(expires_utc) >= CURRENT_TIMESTAMP "
                   "ORDER BY reserved_serial ASC LIMIT 1;") != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, logbook_id);
  sqlite3_bind_text(stmt, 2, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    *out_serial = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 0 : rc == SQLITE_DONE ? 1 : -1;
}

int db_sync_peek_available_serial_reservation(int logbook_id,
                                              const char *station_id,
                                              int *out_serial) {
  db_operation_lock();
  int rc = db_sync_peek_available_serial_reservation_impl(
      logbook_id, station_id, out_serial);
  db_operation_unlock();
  return rc;
}

static int db_sync_claim_available_serial_reservation_impl(
    int logbook_id, const char *station_id, char *out_reservation_id,
    size_t out_reservation_id_size, int *out_serial) {
  if (logbook_id <= 0 || !station_id || !station_id[0] ||
      !out_reservation_id || out_reservation_id_size < 2 || !out_serial ||
      db_init() != 0)
    return -1;
  out_reservation_id[0] = 0;
  *out_serial = 0;

  if (exec_sql_checked("BEGIN IMMEDIATE;") != 0)
    return -1;

  sqlite3_stmt *select_stmt = NULL;
  if (prepare_stmt(&select_stmt,
                   "SELECT reservation_id,reserved_serial "
                   "FROM serial_reservations WHERE logbook_id = ? "
                   "AND station_id = ? AND status = 'available' "
                   "AND datetime(expires_utc) >= CURRENT_TIMESTAMP "
                   "ORDER BY reserved_serial ASC LIMIT 1;") != SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_int(select_stmt, 1, logbook_id);
  sqlite3_bind_text(select_stmt, 2, station_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(select_stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(select_stmt);
    (void)exec_sql_checked("ROLLBACK;");
    return rc == SQLITE_DONE ? 1 : -1;
  }

  const unsigned char *reservation = sqlite3_column_text(select_stmt, 0);
  if (!reservation || !reservation[0] ||
      strlen((const char *)reservation) >= out_reservation_id_size) {
    sqlite3_finalize(select_stmt);
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  snprintf(out_reservation_id, out_reservation_id_size, "%s",
           (const char *)reservation);
  *out_serial = sqlite3_column_int(select_stmt, 1);
  sqlite3_finalize(select_stmt);

  sqlite3_stmt *update_stmt = NULL;
  if (prepare_stmt(&update_stmt,
                   "UPDATE serial_reservations SET status = 'claimed' "
                   "WHERE reservation_id = ? AND status = 'available';") !=
      SQLITE_OK) {
    (void)exec_sql_checked("ROLLBACK;");
    return -1;
  }
  sqlite3_bind_text(update_stmt, 1, out_reservation_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(update_stmt);
  sqlite3_finalize(update_stmt);
  if (rc != SQLITE_DONE || sqlite3_changes(db) != 1 ||
      exec_sql_checked("COMMIT;") != 0) {
    (void)exec_sql_checked("ROLLBACK;");
    out_reservation_id[0] = 0;
    *out_serial = 0;
    return -1;
  }
  return 0;
}

int db_sync_claim_available_serial_reservation(
    int logbook_id, const char *station_id, char *out_reservation_id,
    size_t out_reservation_id_size, int *out_serial) {
  db_operation_lock();
  int rc = db_sync_claim_available_serial_reservation_impl(
      logbook_id, station_id, out_reservation_id, out_reservation_id_size,
      out_serial);
  db_operation_unlock();
  return rc;
}

static int db_sync_release_serial_reservation_impl(const char *reservation_id,
                                       int reusable) {
  if (!reservation_id || !reservation_id[0] || db_init() != 0)
    return -1;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations SET status = CASE "
                   "WHEN ? != 0 AND datetime(expires_utc) >= CURRENT_TIMESTAMP "
                   "THEN 'available' ELSE 'expired' END "
                   "WHERE reservation_id = ? AND status IN ('claimed','reserved');") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, reusable ? 1 : 0);
  sqlite3_bind_text(stmt, 2, reservation_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_release_serial_reservation(const char *reservation_id,
                                       int reusable) {
  db_operation_lock();
  int rc = db_sync_release_serial_reservation_impl(reservation_id, reusable);
  db_operation_unlock();
  return rc;
}

static int db_sync_recover_serial_claims_impl(void) {
  if (db_init() != 0)
    return -1;
  return exec_sql_checked(
      "UPDATE serial_reservations SET status = CASE "
      "WHEN datetime(expires_utc) >= CURRENT_TIMESTAMP THEN 'available' "
      "ELSE 'expired' END WHERE status = 'claimed';");
}

int db_sync_recover_serial_claims(void) {
  db_operation_lock();
  int rc = db_sync_recover_serial_claims_impl();
  db_operation_unlock();
  return rc;
}

static int db_sync_load_pending_serial_commits_impl(SyncSerialCommitEntry *out,
                                        int max_items, int *out_count) {
  if (!out || max_items <= 0 || !out_count || db_init() != 0)
    return -1;
  *out_count = 0;

  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT reservation_id,consumed_qso_uid "
                   "FROM serial_reservations WHERE status = 'commit_pending' "
                   "ORDER BY reserved_serial ASC LIMIT ?;") != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, max_items);
  int count = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_items) {
    const unsigned char *reservation = sqlite3_column_text(stmt, 0);
    const unsigned char *qso_uid = sqlite3_column_text(stmt, 1);
    snprintf(out[count].reservation_id, sizeof(out[count].reservation_id),
             "%s", reservation ? (const char *)reservation : "");
    snprintf(out[count].qso_uid, sizeof(out[count].qso_uid), "%s",
             qso_uid ? (const char *)qso_uid : "");
    count++;
  }
  sqlite3_finalize(stmt);
  *out_count = count;
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_load_pending_serial_commits(SyncSerialCommitEntry *out,
                                        int max_items, int *out_count) {
  db_operation_lock();
  int rc = db_sync_load_pending_serial_commits_impl(out, max_items, out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_load_failed_serial_commits_impl(
    SyncFailedSerialCommitEntry *out, int max_items, int *out_count) {
  if (!out || max_items <= 0 || !out_count || db_init() != 0)
    return -1;
  *out_count = 0;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "SELECT reservation_id, consumed_qso_uid, reserved_serial "
                   "FROM serial_reservations WHERE status = 'commit_failed' "
                   "ORDER BY reserved_serial ASC LIMIT ?;") != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, max_items);

  int count = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_items) {
    SyncFailedSerialCommitEntry *entry = &out[count];
    memset(entry, 0, sizeof(*entry));
    const unsigned char *reservation_id = sqlite3_column_text(stmt, 0);
    const unsigned char *qso_uid = sqlite3_column_text(stmt, 1);
    snprintf(entry->reservation_id, sizeof(entry->reservation_id), "%s",
             reservation_id ? (const char *)reservation_id : "");
    snprintf(entry->qso_uid, sizeof(entry->qso_uid), "%s",
             qso_uid ? (const char *)qso_uid : "");
    entry->reserved_serial = sqlite3_column_int(stmt, 2);
    count++;
  }
  sqlite3_finalize(stmt);
  *out_count = count;
  return rc == SQLITE_DONE ? 0 : -1;
}

int db_sync_load_failed_serial_commits(SyncFailedSerialCommitEntry *out,
                                       int max_items, int *out_count) {
  db_operation_lock();
  int rc = db_sync_load_failed_serial_commits_impl(out, max_items, out_count);
  db_operation_unlock();
  return rc;
}

static int db_sync_retry_failed_serial_commit_impl(const char *reservation_id) {
  if (!reservation_id || !reservation_id[0] || db_init() != 0)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations SET status = 'commit_pending' "
                   "WHERE reservation_id = ? AND status = 'commit_failed' "
                   "AND consumed_qso_uid != '';") != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, reservation_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  int changed = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && changed == 1 ? 0 : -1;
}

int db_sync_retry_failed_serial_commit(const char *reservation_id) {
  db_operation_lock();
  int rc = db_sync_retry_failed_serial_commit_impl(reservation_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_mark_serial_commit_acked_impl(const char *reservation_id) {
  if (!reservation_id || !reservation_id[0] || db_init() != 0)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations SET status = 'consumed' "
                   "WHERE reservation_id = ? AND status = 'commit_pending';") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, reservation_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

int db_sync_mark_serial_commit_acked(const char *reservation_id) {
  db_operation_lock();
  int rc = db_sync_mark_serial_commit_acked_impl(reservation_id);
  db_operation_unlock();
  return rc;
}

static int db_sync_mark_serial_commit_failed_impl(const char *reservation_id) {
  if (!reservation_id || !reservation_id[0] || db_init() != 0)
    return -1;
  sqlite3_stmt *stmt = NULL;
  if (prepare_stmt(&stmt,
                   "UPDATE serial_reservations SET status = 'commit_failed' "
                   "WHERE reservation_id = ? AND status = 'commit_pending';") !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, reservation_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

int db_sync_mark_serial_commit_failed(const char *reservation_id) {
  db_operation_lock();
  int rc = db_sync_mark_serial_commit_failed_impl(reservation_id);
  db_operation_unlock();
  return rc;
}