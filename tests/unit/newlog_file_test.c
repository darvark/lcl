#include "config.h"
#include "db.h"

#include <errno.h>
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

  printf("PASS: created DB file %s\n", expected_db_path);
  return 0;
}
