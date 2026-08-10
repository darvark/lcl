#include "cw_keys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_inplace(char *s) {
  if (!s) return;
  char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t len = strlen(s);
  while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
}

static int fn_label_to_index(const char *label) {
  if (!label || toupper((unsigned char)label[0]) != 'F') return -1;
  int n = atoi(label + 1);
  if (n < 1 || n > CW_KEY_COUNT) return -1;
  return n - 1;
}

int cw_keys_load(CwKeyMap *map, const char *filename) {
  if (!map || !filename) return -1;
  memset(map, 0, sizeof(*map));

  FILE *f = fopen(filename, "r");
  if (!f) return -1;

  enum { SEC_NONE, SEC_RUN, SEC_SP } section = SEC_NONE;
  char line[256];

  while (fgets(line, sizeof(line), f)) {
    /* strip trailing newline/whitespace */
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                   line[len - 1] == ' '))
      line[--len] = 0;

    if (len == 0 || line[0] == ';' || line[0] == '#') continue;

    if (line[0] == '[') {
      if (strncmp(line, "[RUN]", 5) == 0)       section = SEC_RUN;
      else if (strncmp(line, "[S&P]", 5) == 0)  section = SEC_SP;
      else                                       section = SEC_NONE;
      continue;
    }

    if (section == SEC_NONE) continue;

    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;

    char *key   = line;
    char *value = eq + 1;
    trim_inplace(key);
    /* keep one leading space in value intentional — skip just whitespace */
    while (*value == ' ') value++;

    int idx = fn_label_to_index(key);
    if (idx < 0) continue;

    if (section == SEC_RUN)
      snprintf(map->run[idx], sizeof(map->run[idx]), "%s", value);
    else
      snprintf(map->sp[idx],  sizeof(map->sp[idx]),  "%s", value);
  }

  fclose(f);
  return 0;
}

void cw_keys_expand(const char *tpl, const char *mycall, const char *rst,
                    const char *exch, char *out, size_t out_size) {
  if (!tpl || !out || out_size < 2) return;
  if (!mycall) mycall = "";
  if (!rst)    rst    = "";
  if (!exch)   exch   = "";

  size_t pos = 0;
  const char *p = tpl;

  while (*p && pos + 1 < out_size) {
    if (*p == '{') {
      const char *close = strchr(p, '}');
      if (!close) { out[pos++] = *p++; continue; }

      size_t tag_len = (size_t)(close - p - 1);
      char tag[32] = {0};
      if (tag_len < sizeof(tag)) memcpy(tag, p + 1, tag_len);

      const char *subst = NULL;
      if      (strcmp(tag, "MYCALL") == 0) subst = mycall;
      else if (strcmp(tag, "RST")    == 0) subst = rst;
      else if (strcmp(tag, "EXCH")   == 0) subst = exch;

      if (subst) {
        while (*subst && pos + 1 < out_size) out[pos++] = *subst++;
        p = close + 1;
      } else {
        out[pos++] = *p++;
      }
    } else {
      out[pos++] = *p++;
    }
  }
  out[pos] = 0;
}

const char *cw_keys_get(const CwKeyMap *map, int fn_nr, int is_run) {
  if (!map || fn_nr < 1 || fn_nr > CW_KEY_COUNT) return NULL;
  return is_run ? map->run[fn_nr - 1] : map->sp[fn_nr - 1];
}
