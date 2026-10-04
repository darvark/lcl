#include "config.h"
#include "db.h"
#include "qso.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;

  fputs(text, f);
  fclose(f);
  return 0;
}

static int test_shared_log_identity_migration(const char *tmp_dir) {
  char server_path[512] = {0};
  char client_path[512] = {0};
  snprintf(server_path, sizeof(server_path), "%s/legacy_server.db", tmp_dir);
  snprintf(client_path, sizeof(client_path), "%s/client.db", tmp_dir);

  sqlite3 *legacy_db = NULL;
  if (sqlite3_open(server_path, &legacy_db) != SQLITE_OK) {
    fprintf(stderr, "cannot create legacy sync database\n");
    if (legacy_db)
      sqlite3_close(legacy_db);
    return 1;
  }

  const char *legacy_schema =
      "CREATE TABLE sync_identity ("
      "id INTEGER PRIMARY KEY CHECK (id = 1),"
      "station_id TEXT NOT NULL,"
      "station_name TEXT NOT NULL DEFAULT '',"
      "role TEXT NOT NULL DEFAULT 'client',"
      "created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
      "INSERT INTO sync_identity (id, station_id, station_name, role) "
      "VALUES (1, 'st-legacy', 'Legacy station', 'client');";
  const int schema_rc = sqlite3_exec(legacy_db, legacy_schema, NULL, NULL, NULL);
  sqlite3_close(legacy_db);
  if (schema_rc != SQLITE_OK) {
    fprintf(stderr, "cannot create legacy sync identity schema\n");
    return 1;
  }

  int result = 1;
  db_shutdown();
  if (setenv("LOGGER_DB_PATH", server_path, 1) != 0 || db_init() != 0) {
    fprintf(stderr, "cannot open legacy sync database for migration\n");
    goto cleanup;
  }

  char shared_log_id[36] = {0};
  char station_id[32] = {0};
  if (db_sync_get_shared_log_id(shared_log_id, sizeof(shared_log_id)) != 1) {
    fprintf(stderr, "legacy database should remain unpaired after migration\n");
    goto cleanup;
  }
  if (db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0 ||
      strcmp(station_id, "st-legacy") != 0) {
    fprintf(stderr, "legacy station identity was not preserved\n");
    goto cleanup;
  }
  char stable_id[36] = {0};
  if (db_sync_create_shared_log_id(shared_log_id, sizeof(shared_log_id)) != 0 ||
      strlen(shared_log_id) != 35 || strncmp(shared_log_id, "sl-", 3) != 0) {
    fprintf(stderr, "server shared-log ID was not created\n");
    goto cleanup;
  }

  db_shutdown();
  if (db_init() != 0 ||
      db_sync_get_shared_log_id(stable_id, sizeof(stable_id)) != 0 ||
      strcmp(stable_id, shared_log_id) != 0) {
    fprintf(stderr, "shared-log ID did not survive database restart\n");
    goto cleanup;
  }

  if (db_sync_create_shared_log_id(stable_id, sizeof(stable_id)) != 0 ||
      strcmp(stable_id, shared_log_id) != 0) {
    fprintf(stderr, "shared-log ID changed after it was created\n");
    goto cleanup;
  }

  char different_id[36] = {0};
  snprintf(different_id, sizeof(different_id), "%s", shared_log_id);
  different_id[34] = different_id[34] == '0' ? '1' : '0';
  if (db_sync_set_shared_log_id(different_id) == 0) {
    fprintf(stderr, "assigned shared-log ID was unexpectedly replaceable\n");
    goto cleanup;
  }

  db_shutdown();
  if (setenv("LOGGER_DB_PATH", client_path, 1) != 0 || db_init() != 0) {
    fprintf(stderr, "cannot open client database for pairing test\n");
    goto cleanup;
  }
  if (db_sync_get_shared_log_id(stable_id, sizeof(stable_id)) != 1 ||
      db_sync_set_shared_log_id(shared_log_id) != 0 ||
      db_sync_get_or_create_station_id(station_id, sizeof(station_id)) != 0 ||
      db_sync_get_shared_log_id(stable_id, sizeof(stable_id)) != 0 ||
      strcmp(stable_id, shared_log_id) != 0) {
    fprintf(stderr, "client pairing or station-ID update lost shared-log ID\n");
    goto cleanup;
  }

  result = 0;

cleanup:
  db_shutdown();
  if (setenv("LOGGER_DB_PATH", "", 1) != 0 || db_init() != 0) {
    fprintf(stderr, "cannot restore default database after identity test\n");
    return 1;
  }
  qso_init();
  return result;
}

int main(void) {
  char tmp_dir[] = "/tmp/lnx_logger_newlog_file_XXXXXX";
  if (!mkdtemp(tmp_dir)) {
    fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
    return 2;
  }

  if (setenv("HOME", tmp_dir, 1) != 0) {
    fprintf(stderr, "setenv HOME failed: %s\n", strerror(errno));
    return 2;
  }

  if (setenv("LOGGER_DB_PATH", "", 1) != 0) {
    fprintf(stderr, "setenv LOGGER_DB_PATH failed: %s\n", strerror(errno));
    return 2;
  }

  if (chdir(tmp_dir) != 0) {
    fprintf(stderr, "chdir failed: %s\n", strerror(errno));
    return 2;
  }

  if (write_text_file("logger.conf", "CONTEST_DEF_FILE=\n") != 0) {
    fprintf(stderr, "cannot write logger.conf\n");
    return 2;
  }

  db_shutdown();
  if (db_init() != 0) {
    fprintf(stderr, "db_init failed\n");
    return 1;
  }

  char status[64] = {0};
  qso_count = 0;
  if (qso_add("OM/DL7AUP 14074 599", status, sizeof(status)) < 0 ||
      strcmp(logbook[0].call, "OM/DL7AUP") != 0) {
    fprintf(stderr, "portable callsign text entry failed: %s\n", status);
    db_shutdown();
    return 1;
  }

  if (qso_add_fields("om/dl7aup", 14150, "59", "SSB", "", status,
                     sizeof(status)) < 0 ||
      strcmp(logbook[1].call, "OM/DL7AUP") != 0) {
    fprintf(stderr, "portable callsign field entry failed: %s\n", status);
    db_shutdown();
    return 1;
  }

  const char *runtime_dir = config_runtime_dir();
  if (!runtime_dir || !runtime_dir[0]) {
    fprintf(stderr, "config_runtime_dir unavailable\n");
    db_shutdown();
    return 1;
  }

  char expected_db_path[1024] = {0};
  snprintf(expected_db_path, sizeof(expected_db_path),
           "%s/logs/UnitNewLogFile.db", runtime_dir);

  struct stat st;
  (void)unlink(expected_db_path);

  if (db_archive_current_logbook_named("UnitNewLogFile") != 0) {
    fprintf(stderr, "db_archive_current_logbook_named failed\n");
    db_shutdown();
    return 1;
  }

  qso_init();
  if (qso_count != 0) {
    fprintf(stderr, "new log was not loaded after archive\n");
    db_shutdown();
    return 1;
  }

  if (qso_add_fields("DL1ABC", 14074, "59", "CW", "", status,
                     sizeof(status)) < 0) {
    fprintf(stderr, "cannot add QSO to new log: %s\n", status);
    db_shutdown();
    return 1;
  }

  if (db_archive_current_logbook_named("UnitOtherLog") != 0) {
    fprintf(stderr, "cannot create second named log\n");
    db_shutdown();
    return 1;
  }

  qso_init();
  if (qso_count != 0) {
    fprintf(stderr, "second new log was not loaded\n");
    db_shutdown();
    return 1;
  }

  if (db_open_named_logbook_by_name("UnitNewLogFile") != 0) {
    fprintf(stderr, "cannot reopen named log\n");
    db_shutdown();
    return 1;
  }

  qso_init();
  if (qso_count != 1 || strcmp(logbook[0].call, "DL1ABC") != 0) {
    fprintf(stderr, "named log was not loaded after reopening\n");
    db_shutdown();
    return 1;
  }

  int rc = stat(expected_db_path, &st);

  db_shutdown();

  if (rc != 0) {
    fprintf(stderr, "expected DB file missing: %s\n", expected_db_path);
    return 1;
  }

  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "expected DB path is not a file: %s\n", expected_db_path);
    return 1;
  }

  if (test_shared_log_identity_migration(tmp_dir) != 0)
    return 1;
  db_shutdown();

  printf("PASS: created DB file %s\n", expected_db_path);
  return 0;
}
