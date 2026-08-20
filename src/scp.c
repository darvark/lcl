#include "scp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* ---------------------------------------------------------- */

static char scp_db[SCP_MAX_CALLS][SCP_CALL_LEN];
static int  scp_loaded = 0;

static const char *SCP_URL =
    "https://www.supercheckpartial.com/downloads/MASTER.SCP";

/* ---------------------------------------------------------- */

static int command_ok(int status) {
  if (status == -1)
    return 0;
  if (!WIFEXITED(status))
    return 0;
  return WEXITSTATUS(status) == 0;
}

/* ---------------------------------------------------------- */

int scp_load(const char *path) {
  const char *target = (path && path[0]) ? path : "MASTER.SCP";

  FILE *f = fopen(target, "r");
  if (!f)
    return -1;

  int count = 0;
  char line[64];

  while (count < SCP_MAX_CALLS && fgets(line, sizeof(line), f)) {
    /* Strip CR/LF */
    char *p = line;
    while (*p && *p != '\r' && *p != '\n')
      p++;
    *p = '\0';

    /* Skip blank lines and header lines starting with ! or # */
    if (line[0] == '\0' || line[0] == '!' || line[0] == '#')
      continue;

    /* Store uppercase; ignore oversized entries */
    int len = (int)strlen(line);
    if (len == 0 || len >= SCP_CALL_LEN)
      continue;

    for (int i = 0; i <= len; i++)
      scp_db[count][i] = (char)toupper((unsigned char)line[i]);

    count++;
  }

  fclose(f);
  scp_loaded = count;
  return count;
}

/* ---------------------------------------------------------- */

int scp_count(void) {
  return scp_loaded;
}

/* ---------------------------------------------------------- */

int scp_search(const char *partial, char results[][SCP_CALL_LEN], int max) {
  if (!partial || partial[0] == '\0')
    return 0;

  /* Upper-case the search term */
  char upper[SCP_CALL_LEN];
  int plen = 0;
  for (; partial[plen] && plen < SCP_CALL_LEN - 1; plen++)
    upper[plen] = (char)toupper((unsigned char)partial[plen]);
  upper[plen] = '\0';

  if (max > SCP_SEARCH_MAX)
    max = SCP_SEARCH_MAX;

  int found = 0;
  for (int i = 0; i < scp_loaded; i++) {
    if (strstr(scp_db[i], upper)) {
      if (found < max)
        memcpy(results[found], scp_db[i], SCP_CALL_LEN);
      found++;
    }
  }
  return found;
}

/* ---------------------------------------------------------- */

int scp_download_latest(const char *filename) {
  const char *target  = (filename && filename[0]) ? filename : "MASTER.SCP";
  const char *tmp     = "MASTER.SCP.tmp";

  char cmd[512];

  snprintf(cmd, sizeof(cmd),
           "curl -fsSLk --connect-timeout 10 --max-time 90 \"%s\" -o \"%s\"",
           SCP_URL, tmp);
  int status = system(cmd);

  if (!command_ok(status)) {
    snprintf(cmd, sizeof(cmd),
             "wget -q -T 90 -O \"%s\" \"%s\"", tmp, SCP_URL);
    status = system(cmd);
  }

  if (!command_ok(status)) {
    remove(tmp);
    return -1;
  }

  FILE *f = fopen(tmp, "r");
  if (!f) {
    remove(tmp);
    return -1;
  }
  fclose(f);

  if (rename(tmp, target) != 0) {
    remove(tmp);
    return -1;
  }

  return 0;
}
