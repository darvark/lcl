#include "cty.h"

#include <strings.h>
#include <sys/wait.h>

#define MAX_CTY 50000

static CtyEntry cty_db[MAX_CTY];
static int cty_count = 0;

static const char *CTY_URL = "https://www.country-files.com/cty/wl_cty.dat";

/* ------------------------------------------------ */

/*
 * Check whether a system command exited successfully.
 *
 * @param status Return value from system().
 * @return 1 if the command exited with status 0, otherwise 0.
 */
static int command_ok(int status) {
  if (status == -1)
    return 0;

  if (!WIFEXITED(status))
    return 0;

  return WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------ */

/*
 * Download the latest CTY database and save it locally.
 *
 * @param filename Destination filename. If empty or NULL, wl_cty.dat is used.
 * @return 0 on success, or -1 on failure.
 */
int cty_download_latest(const char *filename) {
  const char *target = (filename && filename[0]) ? filename : "wl_cty.dat";
  const char *tmp_file = "wl_cty.dat.tmp";

  char cmd[512];

  snprintf(cmd, sizeof(cmd),
           "curl -fsSL --connect-timeout 10 --max-time 90 \"%s\" -o \"%s\"",
           CTY_URL, tmp_file);

  int status = system(cmd);

  if (!command_ok(status)) {
    snprintf(cmd, sizeof(cmd),
             "wget -q -T 90 -O \"%s\" \"%s\"", tmp_file, CTY_URL);
    status = system(cmd);
  }

  if (!command_ok(status)) {
    remove(tmp_file);
    return -1;
  }

  FILE *f = fopen(tmp_file, "r");
  if (!f) {
    remove(tmp_file);
    return -1;
  }

  fclose(f);

  if (rename(tmp_file, target) != 0) {
    remove(tmp_file);
    return -1;
  }

  return 0;
}

/* ------------------------------------------------ */

/*
 * Trim leading and trailing whitespace from a token in place.
 *
 * @param s String to trim.
 * @return Nothing.
 */
static void trim(char *s) {
  if (!s)
    return;

  char *p = s;
  while (*p && isspace((unsigned char)*p))
    p++;

  if (p != s)
    memmove(s, p, strlen(p) + 1);

  for (int i = (int)strlen(s) - 1; i >= 0; i--) {
    if (isspace((unsigned char)s[i]))
      s[i] = 0;
    else
      break;
  }
}

/* ------------------------------------------------ */

/*
 * Check whether a callsign begins with a given prefix.
 *
 * @param call Callsign to test.
 * @param prefix Prefix to match.
 * @return 1 if the prefix matches, otherwise 0.
 */
/*
 * Check whether a callsign begins with a given prefix.
 *
 * @param call Callsign to test.
 * @param prefix Prefix to match.
 * @return 1 if the prefix matches, otherwise 0.
 */
static int match_prefix(const char *call, const char *prefix) {
  return strncasecmp(call, prefix, strlen(prefix)) == 0;
}

/* ------------------------------------------------ */

/*
 * Add one country/prefix record to the in-memory CTY database.
 *
 * @param country Country name.
 * @param prefix Prefix to store.
 * @param cq CQ zone.
 * @param itu ITU zone.
 * @param lat Latitude.
 * @param lon Longitude.
 * @return Nothing.
 */
/*
 * Add one country/prefix record to the in-memory CTY database.
 *
 * @param country Country name.
 * @param prefix Prefix to store.
 * @param cq CQ zone.
 * @param itu ITU zone.
 * @param lat Latitude.
 * @param lon Longitude.
 * @return Nothing.
 */
static void add_country_entry(const char *country, const char *prefix, int cq,
                              int itu, double lat, double lon) {
  if (!country || !prefix || !prefix[0])
    return;

  if (cty_count >= MAX_CTY)
/*
 * Load the CTY prefix database from disk.
 *
 * @param filename Preferred path to the database file.
 * @return Number of parsed entries on success, or -1 on failure.
 */
    return;

  CtyEntry *e = &cty_db[cty_count++];
/*
 * Find the best CTY match for a callsign prefix.
 *
 * @param callsign Callsign or prefix to search for.
 * @return Pointer to the best matching entry, or NULL if none is found.
 */
  memset(e, 0, sizeof(*e));

  strncpy(e->prefix, prefix, sizeof(e->prefix) - 1);
  strncpy(e->country, country, sizeof(e->country) - 1);
  e->cq_zone = cq;
  e->itu_zone = itu;
  e->lat = lat;
  e->lon = lon;
}

/* ------------------------------------------------ */

/*
 * Load the CTY prefix database from disk.
 *
 * @param filename Preferred path to the database file.
 * @return Number of parsed entries on success, or -1 on failure.
 */
int cty_load(const char *filename) {
  const char *candidates[] = {filename, "wl_cty.dat", "build/wl_cty.dat",
                              "./wl_cty.dat", "../wl_cty.dat"};

  FILE *f = NULL;
  const char *used_path = NULL;

  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    if (!candidates[i] || !candidates[i][0])
      continue;

    f = fopen(candidates[i], "r");
    if (f) {
      used_path = candidates[i];
      break;
    }
  }

  if (!f) {
    fprintf(stderr, "Cannot open CTY database\n");
    return -1;
  }

  cty_count = 0;

  char line[512];
  char country[64] = {0};
  int cq_zone = 0;
  int itu_zone = 0;
  double lat = 0.0;
  double lon = 0.0;

  while (fgets(line, sizeof(line), f)) {
    char *trimmed = line;
    while (*trimmed && isspace((unsigned char)*trimmed))
      trimmed++;

    if (*trimmed == 0 || *trimmed == '#')
      continue;

    char *save = NULL;
    char *tokens[10];
    int count = 0;

    for (char *tok = strtok_r(trimmed, ":", &save); tok && count < 10;
         tok = strtok_r(NULL, ":", &save)) {
      trim(tok);
      if (tok[0])
        tokens[count++] = tok;
    }

    if (count >= 3 && isdigit((unsigned char)tokens[1][0]) &&
        isdigit((unsigned char)tokens[2][0])) {
      snprintf(country, sizeof(country), "%s", tokens[0]);
      cq_zone = atoi(tokens[1]);
      itu_zone = atoi(tokens[2]);

      if (count >= 6) {
        lat = strtod(tokens[4], NULL);
        lon = strtod(tokens[5], NULL);
      } else {
        lat = 0.0;
        lon = 0.0;
      }

      if (count >= 8) {
        add_country_entry(country, tokens[count - 1], cq_zone, itu_zone, lat,
                          lon);
      }

      continue;
    }

    if (!country[0])
      continue;

    char *prefix_save = NULL;
    for (char *tok = strtok_r(trimmed, ",; \t\r\n", &prefix_save); tok;
         tok = strtok_r(NULL, ",; \t\r\n", &prefix_save)) {
      trim(tok);
      if (tok[0])
        add_country_entry(country, tok, cq_zone, itu_zone, lat, lon);
    }
  }

  fclose(f);
  return cty_count;
}

/* ------------------------------------------------ */

/*
 * Find the best CTY match for a callsign prefix.
 *
 * @param callsign Callsign or prefix to search for.
 * @return Pointer to the best matching entry, or NULL if none is found.
 */
const CtyEntry *cty_lookup(const char *callsign) {
  if (!callsign)
    return NULL;

  static char tmp[32];
  strncpy(tmp, callsign, sizeof(tmp));
  tmp[sizeof(tmp) - 1] = 0;

  for (int i = 0; tmp[i]; i++)
    tmp[i] = toupper(tmp[i]);

  const CtyEntry *best = NULL;
  size_t best_len = 0;

  for (int i = 0; i < cty_count; i++) {
    size_t len = strlen(cty_db[i].prefix);

    if (len < 1)
      continue;

    if (len > best_len && match_prefix(tmp, cty_db[i].prefix)) {
      best = &cty_db[i];
      best_len = len;
    }
  }

  return best;
}
