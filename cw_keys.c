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
                    const char *exch, const char *hiscall,
                    char *out, size_t out_size) {
  if (!tpl || !out || out_size < 2) return;
  if (!mycall) mycall = "";
  if (!rst)    rst    = "";
  if (!exch)   exch   = "";
  if (!hiscall) hiscall = "";

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
      else if (strcmp(tag, "HISCALL") == 0 || strcmp(tag, "CALL") == 0) subst = hiscall;

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

int cw_qtc_load(CwQtcMap *qtc_map, const char *filename) {
  if (!qtc_map || !filename) return -1;

  memset(qtc_map, 0, sizeof(*qtc_map));

  /* Set reasonable defaults so the map is usable even without a [QTC] section. */
  snprintf(qtc_map->preamble,  sizeof(qtc_map->preamble),  "%s",
           "QTC {QTC_NR}/{QTC_COUNT}");
  snprintf(qtc_map->record,    sizeof(qtc_map->record),    "%s",
           "{QTC_TIME} {QTC_CALL} {QTC_EXCH}");
  snprintf(qtc_map->confirm,   sizeof(qtc_map->confirm),   "%s", "QSL");
  snprintf(qtc_map->request,   sizeof(qtc_map->request),   "%s", "QRV");

  FILE *f = fopen(filename, "r");
  if (!f) return -1;

  int in_qtc_section = 0;
  char line[256];

  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                   line[len - 1] == ' '))
      line[--len] = 0;

    if (len == 0 || line[0] == ';' || line[0] == '#') continue;

    if (line[0] == '[') {
      in_qtc_section = (strncmp(line, "[QTC]", 5) == 0);
      continue;
    }

    if (!in_qtc_section) continue;

    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;

    char *key   = line;
    char *value = eq + 1;
    trim_inplace(key);
    while (*value == ' ') value++;

    if (strcmp(key, "PREAMBLE") == 0)
      snprintf(qtc_map->preamble, sizeof(qtc_map->preamble), "%s", value);
    else if (strcmp(key, "RECORD") == 0)
      snprintf(qtc_map->record, sizeof(qtc_map->record), "%s", value);
    else if (strcmp(key, "CONFIRM") == 0)
      snprintf(qtc_map->confirm, sizeof(qtc_map->confirm), "%s", value);
    else if (strcmp(key, "REQUEST") == 0)
      snprintf(qtc_map->request, sizeof(qtc_map->request), "%s", value);
  }

  fclose(f);
  return 0;
}

void cw_qtc_expand(const char *tpl, const char *mycall, const char *hiscall,
                   int bundle_nr, int bundle_count,
                   const char *qtc_time, const char *qtc_call,
                   const char *qtc_exch,
                   char *out, size_t out_size) {
  if (!tpl || !out || out_size < 2) return;
  if (!mycall)   mycall   = "";
  if (!hiscall)  hiscall  = "";
  if (!qtc_time) qtc_time = "";
  if (!qtc_call) qtc_call = "";
  if (!qtc_exch) qtc_exch = "";

  char nr_buf[16];
  char cnt_buf[16];
  snprintf(nr_buf,  sizeof(nr_buf),  "%d", bundle_nr);
  snprintf(cnt_buf, sizeof(cnt_buf), "%d", bundle_count);

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
      if      (strcmp(tag, "MYCALL")    == 0) subst = mycall;
      else if (strcmp(tag, "HISCALL")   == 0 ||
               strcmp(tag, "CALL")      == 0) subst = hiscall;
      else if (strcmp(tag, "QTC_NR")    == 0) subst = nr_buf;
      else if (strcmp(tag, "QTC_COUNT") == 0) subst = cnt_buf;
      else if (strcmp(tag, "QTC_TIME")  == 0) subst = qtc_time;
      else if (strcmp(tag, "QTC_CALL")  == 0) subst = qtc_call;
      else if (strcmp(tag, "QTC_EXCH")  == 0) subst = qtc_exch;

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
