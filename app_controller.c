#include "app_controller.h"

#include "cat.h"
#include "contest.h"
#include "config.h"
#include "db.h"
#include "cty.h"
#include "dxcluster.h"
#include "export.h"
#include "globals.h"
#include "qso.h"
#include "suggestion.h"
#include "stats.h"

#include <stdlib.h>
#include <unistd.h>

static char input_buffer[256];
static int input_len = 0;

enum {
  ENTRY_FIELD_CALL = 0,
  ENTRY_FIELD_RST = 1,
  ENTRY_FIELD_COMMENTS = 2,
  ENTRY_FIELD_COUNT = 3
};

static char entry_call_by_radio[2][32];
static char entry_rst_by_radio[2][8];
static char entry_comments_by_radio[2][128];
static int active_entry_field_by_radio[2] = {ENTRY_FIELD_CALL, ENTRY_FIELD_CALL};

static char status_text[128] = "Ready";
static char dxcc_text[128] = "";
static char info_text[128] = "";
static char display_info[128] = "";

int app_debug_enabled = 0;

int last_cq = 0;
int last_itu = 0;
int cty_update_in_progress = 0;
int call_suggestion_available = 0;
int call_suggestion_count = 0;
int call_suggestion_selected_index = 0;
char call_suggestion_matches[CALL_SUGGESTION_MAX][CALL_SUGGESTION_LEN] = {{0}};

#define MAX_CALL_HISTORY 20000

static char call_history[MAX_CALL_HISTORY][32];
static int call_history_count = 0;
static CallSuggestionList call_suggestions;

static bool cluster_view = true;
static int cluster_scroll = 0;
static bool export_prompt_mode = false;
static int manual_entry_freq_khz[2] = {14074, 7020};
static int active_radio_nr = 1;
static int radio_run_state[2] = {1, 0};
static ContestDefinition active_contest_def;
static int contest_definition_loaded = 0;
static char contest_exchange_label_text[64] = "Exchange";

#define NAMED_LOG_LIST_MAX 12

static void sync_callsign_suggestion_state(void);

static void update_composed_input_line(void);
static void refresh_callsign_suggestion(const char *input);
static void update_dxcc_from_input(const char *input);
static int is_digits_only(const char *s);
static int process_command(const char *cmd);

static const ContestFieldDef *active_exchange_field_def(void) {
  if (!contest_definition_loaded || active_contest_def.field_count <= 0)
    return NULL;

  for (int i = 0; i < active_contest_def.field_count; i++) {
    if (active_contest_def.fields[i].required)
      return &active_contest_def.fields[i];
  }

  return &active_contest_def.fields[0];
}

static int contest_exchange_is_numeric(const ContestFieldDef *field) {
  if (!field || !field->name[0])
    return 0;

  char upper_name[sizeof(field->name)];
  snprintf(upper_name, sizeof(upper_name), "%s", field->name);
  for (size_t i = 0; upper_name[i]; i++)
    upper_name[i] = (char)toupper((unsigned char)upper_name[i]);

  return strstr(upper_name, "SERIAL") != NULL || strstr(upper_name, "NR") != NULL ||
         strstr(upper_name, "NUMBER") != NULL || strstr(upper_name, "NUM") != NULL ||
         strstr(upper_name, "ZONE") != NULL;
}

static const char *contest_exchange_label(void) {
  const ContestFieldDef *field = active_exchange_field_def();

  if (field) {
    if (field->label[0])
      return field->label;
    if (field->name[0])
      return field->name;
  }

  return "Exchange";
}

static int validate_contest_exchange(const char *exchange, char *status,
                                     size_t status_size) {
  const ContestFieldDef *field = active_exchange_field_def();
  const char *label = contest_exchange_label();

  if (!exchange || !exchange[0]) {
    snprintf(status, status_size, "Need: CALL %s", label);
    return 0;
  }

  if (field && contest_exchange_is_numeric(field) && !is_digits_only(exchange)) {
    snprintf(status, status_size, "%s must be numeric", label);
    return 0;
  }

  return 1;
}

static int active_entry_field_count(void) {
  return contest_definition_loaded ? 2 : ENTRY_FIELD_COUNT;
}

static int active_radio_index(void) {
  return active_radio_nr == 2 ? 1 : 0;
}

static char *entry_call_for_idx(int radio_idx) {
  if (radio_idx < 0)
    radio_idx = 0;
  if (radio_idx > 1)
    radio_idx = 1;
  return entry_call_by_radio[radio_idx];
}

static char *entry_rst_for_idx(int radio_idx) {
  if (radio_idx < 0)
    radio_idx = 0;
  if (radio_idx > 1)
    radio_idx = 1;
  return entry_rst_by_radio[radio_idx];
}

static char *entry_comments_for_idx(int radio_idx) {
  if (radio_idx < 0)
    radio_idx = 0;
  if (radio_idx > 1)
    radio_idx = 1;
  return entry_comments_by_radio[radio_idx];
}

static int *active_entry_field_for_idx(int radio_idx) {
  if (radio_idx < 0)
    radio_idx = 0;
  if (radio_idx > 1)
    radio_idx = 1;
  return &active_entry_field_by_radio[radio_idx];
}

/*
 * Reset split QSO entry fields and move focus to CALL.
 *
 * @return Nothing.
 */
static void clear_entry_fields(void) {
  for (int i = 0; i < 2; i++) {
    entry_call_by_radio[i][0] = 0;
    entry_rst_by_radio[i][0] = 0;
    entry_comments_by_radio[i][0] = 0;
    active_entry_field_by_radio[i] = ENTRY_FIELD_CALL;
  }
  update_composed_input_line();
}

static void clear_active_entry_fields(void) {
  const int radio_idx = active_radio_index();
  entry_call_by_radio[radio_idx][0] = 0;
  entry_rst_by_radio[radio_idx][0] = 0;
  entry_comments_by_radio[radio_idx][0] = 0;
  active_entry_field_by_radio[radio_idx] = ENTRY_FIELD_CALL;
  update_composed_input_line();
}

static void clear_active_call_exchange_fields(void) {
  const int radio_idx = active_radio_index();
  entry_call_by_radio[radio_idx][0] = 0;
  entry_rst_by_radio[radio_idx][0] = 0;
  active_entry_field_by_radio[radio_idx] = ENTRY_FIELD_CALL;
  update_composed_input_line();
}

/*
 * Build a display-only composed input line from split entry fields.
 *
 * @return Nothing.
 */
static void update_composed_input_line(void) {
  const int radio_idx = active_radio_index();
  char *entry_call = entry_call_for_idx(radio_idx);
  char *entry_rst = entry_rst_for_idx(radio_idx);
  char *entry_comments = entry_comments_for_idx(radio_idx);

  if (contest_definition_loaded) {
    if (entry_call[0] || entry_rst[0]) {
      snprintf(input_buffer, sizeof(input_buffer), "%s %s", entry_call,
               entry_rst);
    } else {
      input_buffer[0] = 0;
    }
  } else {
    if (entry_call[0] || entry_rst[0] || entry_comments[0]) {
      snprintf(input_buffer, sizeof(input_buffer), "%s %s %s", entry_call,
               entry_rst, entry_comments);
    } else {
      input_buffer[0] = 0;
    }
  }

  input_len = (int)strlen(input_buffer);
}

/*
 * Return writable pointer and size for the currently active entry field.
 *
 * @param out_size Destination for selected field capacity.
 * @return Pointer to active field buffer.
 */
static char *active_field_buffer(size_t *out_size) {
  const int radio_idx = active_radio_index();
  const int active_entry_field = *active_entry_field_for_idx(radio_idx);

  if (out_size)
    *out_size = 0;

  switch (active_entry_field) {
  case ENTRY_FIELD_CALL:
    if (out_size)
      *out_size = sizeof(entry_call_by_radio[radio_idx]);
    return entry_call_for_idx(radio_idx);
  case ENTRY_FIELD_RST:
    if (out_size)
      *out_size = sizeof(entry_rst_by_radio[radio_idx]);
    return entry_rst_for_idx(radio_idx);
  case ENTRY_FIELD_COMMENTS:
  default:
    if (out_size)
      *out_size = sizeof(entry_comments_by_radio[radio_idx]);
    return entry_comments_for_idx(radio_idx);
  }
}

/*
 * Move editing focus to the next split entry field.
 *
 * @return Nothing.
 */
static void advance_entry_field(void) {
  const int radio_idx = active_radio_index();
  int *active_entry_field = active_entry_field_for_idx(radio_idx);
  *active_entry_field = (*active_entry_field + 1) % active_entry_field_count();
}

/*
 * Append one printable character to the active split entry field.
 *
 * @param key Printable key code.
 * @return 1 on success, or 0 if the field is full.
 */
static int append_to_active_field(int key) {
  const int radio_idx = active_radio_index();
  const int active_entry_field = *active_entry_field_for_idx(radio_idx);
  size_t size = 0;
  char *field = active_field_buffer(&size);

  if (!field || size < 2)
    return 0;

  size_t len = strlen(field);
  if (len >= size - 1)
    return 0;

  char ch = (char)key;
  if (active_entry_field == ENTRY_FIELD_CALL)
    ch = (char)toupper((unsigned char)ch);

  field[len] = ch;
  field[len + 1] = 0;
  return 1;
}

/*
 * Delete one character from the active split entry field.
 *
 * @return 1 if a character was removed, otherwise 0.
 */
static int backspace_active_field(void) {
  char *field = active_field_buffer(NULL);
  if (!field)
    return 0;

  size_t len = strlen(field);
  if (len == 0)
    return 0;

  field[len - 1] = 0;
  return 1;
}

/*
 * Build one command line from split fields.
 *
 * @param out Destination buffer.
 * @param out_size Destination size in bytes.
 * @return Nothing.
 */
static void compose_command_line(char *out, size_t out_size) {
  const int radio_idx = active_radio_index();
  char *entry_call = entry_call_for_idx(radio_idx);
  char *entry_rst = entry_rst_for_idx(radio_idx);
  char *entry_comments = entry_comments_for_idx(radio_idx);

  if (!out || out_size < 2)
    return;

  out[0] = 0;

  if (!entry_call[0])
    return;

  snprintf(out, out_size, "%s", entry_call);

  if (entry_rst[0]) {
    strncat(out, " ", out_size - strlen(out) - 1);
    strncat(out, entry_rst, out_size - strlen(out) - 1);
  }

  if (contest_definition_loaded)
    return;

  if (entry_comments[0]) {
    strncat(out, " ", out_size - strlen(out) - 1);
    strncat(out, entry_comments, out_size - strlen(out) - 1);
  }
}

/*
 * Extract the first callsign-like token from an input string.
 *
 * @param input Source text to scan.
 * @param out Destination buffer for the token.
 * @param out_size Size of the destination buffer.
 * @return 1 if a token was extracted, otherwise 0.
 */
static int extract_callsign_token(const char *input, char *out, size_t out_size) {
  if (!input || !out || out_size < 2)
    return 0;

  out[0] = 0;

  size_t len = strlen(input);
  size_t start = 0;

  while (start < len && isspace((unsigned char)input[start]))
    start++;

  if (start >= len)
    return 0;

  size_t end = start;
  while (end < len && !isspace((unsigned char)input[end]) && input[end] != ';')
    end++;

  size_t token_len = end - start;
  if (token_len < 2 || token_len >= out_size)
    return 0;

  for (size_t i = 0; i < token_len; i++)
    out[i] = (char)toupper((unsigned char)input[start + i]);

  out[token_len] = 0;

  return 1;
}

/*
 * Keep the in-memory call history buffer up to date.
 *
 * @param call Callsign to append.
 * @return Nothing.
 */
static void call_history_add_memory(const char *call) {
  if (!call || !call[0])
    return;

  if (call_history_count < MAX_CALL_HISTORY) {
    snprintf(call_history[call_history_count], sizeof(call_history[0]), "%s", call);
    call_history_count++;
    return;
  }

  memmove(call_history, call_history + 1,
          sizeof(call_history[0]) * (MAX_CALL_HISTORY - 1));
  snprintf(call_history[MAX_CALL_HISTORY - 1], sizeof(call_history[0]), "%s",
           call);
}

/*
 * Append a callsign to the persistent call-history store.
 *
 * @param call Callsign to append.
 * @return Nothing.
 */
static void call_history_append_file(const char *call) {
  db_append_call_history(call);
}

/*
 * Record a completed input line as call history when it contains a callsign.
 *
 * @param input Raw input line.
 * @return Nothing.
 */
static void call_history_record_from_input(const char *input) {
  char call[32];

  if (!extract_callsign_token(input, call, sizeof(call)))
    return;

  if (strcmp(call, "EXPORT") == 0 || strcmp(call, "INVALID") == 0 ||
      strcmp(call, "QUIT") == 0 || strcmp(call, "NEWLOG") == 0 ||
      strcmp(call, "CLEAR") == 0 || strcmp(call, "PREVLOG") == 0 ||
      strcmp(call, "OPENPREV") == 0 || strcmp(call, "PREVIOUS") == 0 ||
      strcmp(call, "OPENLOG") == 0 || strcmp(call, "LOGS") == 0 ||
      strcmp(call, "CONTEST") == 0 || strcmp(call, "EXPORTCAB") == 0 ||
      strcmp(call, "TECHNIQUE") == 0)
    return;

  call_history_add_memory(call);
  call_history_append_file(call);
}

/*
 * Reload call history from the database-backed store.
 *
 * @param path Unused compatibility parameter.
 * @return Nothing.
 */
static void call_history_load_file(const char *path) {
  (void)path;

  int count = 0;
  db_load_call_history(call_history, MAX_CALL_HISTORY, &count);
  call_history_count = count;
}

/*
 * Clear the active suggestion list and cached suggestion state.
 *
 * @return Nothing.
 */
static void clear_callsign_suggestion(void) {
  call_suggestion_list_clear(&call_suggestions);
  call_suggestion_available = 0;
  call_suggestion_count = 0;
  call_suggestion_selected_index = 0;

  for (int i = 0; i < CALL_SUGGESTION_MAX; i++)
    call_suggestion_matches[i][0] = 0;
}

/*
 * Reset all state associated with the currently loaded logbook.
 *
 * @return Nothing.
 */
static void reset_loaded_log_state(void) {
  qso_init();
  call_history_load_file("call_history.txt");
  clear_callsign_suggestion();
  clear_entry_fields();
  export_prompt_mode = false;
  dxcc_text[0] = 0;
  info_text[0] = 0;
  display_info[0] = 0;
  last_cq = 0;
  last_itu = 0;

  stats_update();
}

/*
 * Check whether a string contains only decimal digits.
 *
 * @param s Input string.
 * @return 1 if the string is non-empty and numeric, otherwise 0.
 */
static int is_digits_only(const char *s) {
  if (!s || !s[0])
    return 0;

  for (const char *p = s; *p; p++) {
    if (!isdigit((unsigned char)*p))
      return 0;
  }

  return 1;
}

/*
 * Validate frequency range used in QSO input.
 *
 * @param freq_khz Frequency in kHz.
 * @return 1 if valid, otherwise 0.
 */
static int is_valid_frequency_khz(int freq_khz) {
  if (freq_khz < 1000)
    return 0;

  if (freq_khz > 500000)
    return 0;

  return 1;
}

/*
 * Convert external radio number to internal 0-based index.
 */
static int radio_index_from_nr(int radio_nr) {
  if (radio_nr <= 1)
    return 0;

  return 1;
}

/*
 * Resolve CAT slot for one radio according to selected technique.
 */
static int cat_slot_for_radio(int radio_idx) {
  if (config.contest_technique == CONTEST_TECH_SO2R)
    return radio_idx == 1 ? CAT_SLOT_B : CAT_SLOT_A;

  return CAT_SLOT_A;
}

/*
 * Resolve CAT VFO mapping for one logical radio.
 */
static CatVfo cat_vfo_for_radio(int radio_idx) {
  if (config.contest_technique == CONTEST_TECH_SO2V)
    return radio_idx == 1 ? CAT_VFO_B : CAT_VFO_A;

  return CAT_VFO_CURR;
}

/*
 * Resolve QSO frequency, preferring live CAT over manual fallback.
 *
 * @return Frequency in kHz.
 */
static int resolve_qso_frequency_khz(void) {
  const int radio_idx = radio_index_from_nr(active_radio_nr);
  const int slot = cat_slot_for_radio(radio_idx);
  const CatVfo vfo = cat_vfo_for_radio(radio_idx);
  int cat_freq_khz = 0;

  if (cat_is_connected_slot(slot) &&
      cat_get_frequency_khz_slot_vfo(slot, vfo, &cat_freq_khz) == 0 &&
      is_valid_frequency_khz(cat_freq_khz)) {
    return cat_freq_khz;
  }

  return manual_entry_freq_khz[radio_idx];
}

static const char *resolve_qso_mode(char *mode_out, size_t mode_out_size) {
  if (!mode_out || mode_out_size < 2)
    return NULL;

  if (contest_definition_loaded && active_contest_def.mode[0] &&
      strcmp(active_contest_def.mode, "MIXED") != 0) {
    if (strcmp(active_contest_def.mode, "PHONE") == 0)
      snprintf(mode_out, mode_out_size, "%s", "SSB");
    else
      snprintf(mode_out, mode_out_size, "%s", active_contest_def.mode);
    return mode_out;
  }

  const int radio_idx = radio_index_from_nr(active_radio_nr);
  const int slot = cat_slot_for_radio(radio_idx);
  const CatVfo vfo = cat_vfo_for_radio(radio_idx);

  mode_out[0] = 0;

  if (cat_is_connected_slot(slot) && config.cat_mode_from_rig &&
      cat_get_mode_label_slot_vfo(slot, vfo, mode_out, mode_out_size) == 0 &&
      mode_out[0]) {
    return mode_out;
  }

  detect_mode(resolve_qso_frequency_khz(), mode_out);
  return mode_out;
}

/*
 * Build sent exchange text from the loaded contest template.
 */
static void build_exchange_sent(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  out[0] = 0;
  const char *tpl = active_contest_def.exchange_sent_template;
  if (!tpl || !tpl[0]) {
    snprintf(out, out_size, "%d", qso_count + 1);
    return;
  }

  // Contest rule: only '#' means serial; all other templates are static.
  if (strcmp(tpl, "#") == 0) {
    snprintf(out, out_size, "%d", qso_count + 1);
    return;
  }

  if (config.contest_tx_exchange[0]) {
    snprintf(out, out_size, "%s", config.contest_tx_exchange);
    return;
  }

  snprintf(out, out_size, "%s", tpl);
}

static const char *active_operator_mode_text(void) {
  int is_run = 0;
  app_controller_get_radio_state(active_radio_nr, NULL, NULL, 0, &is_run);
  return is_run ? "RUN" : "S&P";
}

static int is_phone_mode_label(const char *mode) {
  if (!mode || !mode[0])
    return 0;

  return strcmp(mode, "SSB") == 0 || strcmp(mode, "AM") == 0 ||
         strcmp(mode, "FM") == 0;
}

static int is_digi_mode_label(const char *mode) {
  if (!mode || !mode[0])
    return 0;

  return strcmp(mode, "FT8") == 0 || strcmp(mode, "FT4") == 0 ||
         strcmp(mode, "RTTY") == 0 || strcmp(mode, "PSK31") == 0;
}

static int qso_matches_active_contest(const QSO *q, const char *contest_id) {
  if (!q)
    return 0;

  if (!contest_id || !contest_id[0])
    return 1;

  if (!q->contest_id[0])
    return 0;

  return strcmp(q->contest_id, contest_id) == 0;
}

static int was_dxcc_worked(const char *country, const char *contest_id) {
  if (!country || !country[0] || strcmp(country, "UNKNOWN") == 0)
    return 0;

  for (int i = 0; i < qso_count; i++) {
    const QSO *q = &logbook[i];
    if (q->invalid)
      continue;
    if (!qso_matches_active_contest(q, contest_id))
      continue;
    if (strcmp(q->country, country) == 0)
      return 1;
  }

  return 0;
}

static int was_band_dxcc_worked(const char *band, const char *country,
                                const char *contest_id) {
  if (!band || !band[0] || !country || !country[0] ||
      strcmp(country, "UNKNOWN") == 0)
    return 0;

  for (int i = 0; i < qso_count; i++) {
    const QSO *q = &logbook[i];
    if (q->invalid)
      continue;
    if (!qso_matches_active_contest(q, contest_id))
      continue;
    if (strcmp(q->country, country) == 0 && strcmp(q->band, band) == 0)
      return 1;
  }

  return 0;
}

static int is_low_band(const char *band) {
  return band && (strcmp(band, "160") == 0 || strcmp(band, "80") == 0 ||
                  strcmp(band, "40") == 0);
}

static int call_is_maritime_mobile(const char *call) {
  if (!call || !call[0])
    return 0;
  const size_t len = strlen(call);
  return (len >= 3 && strcmp(call + len - 3, "/MM") == 0) ||
         (len >= 3 && strcmp(call + len - 3, "/AM") == 0);
}

static int contest_name_contains(const char *needle) {
  if (!needle || !needle[0])
    return 0;

  char up_name[96] = {0};
  char up_cab[96] = {0};
  char up_needle[48] = {0};
  snprintf(up_name, sizeof(up_name), "%s", active_contest_def.name);
  snprintf(up_cab, sizeof(up_cab), "%s", active_contest_def.cabrillo_name);
  snprintf(up_needle, sizeof(up_needle), "%s", needle);
  for (size_t i = 0; up_name[i]; i++)
    up_name[i] = (char)toupper((unsigned char)up_name[i]);
  for (size_t i = 0; up_cab[i]; i++)
    up_cab[i] = (char)toupper((unsigned char)up_cab[i]);
  for (size_t i = 0; up_needle[i]; i++)
    up_needle[i] = (char)toupper((unsigned char)up_needle[i]);

  return strstr(up_name, up_needle) != NULL || strstr(up_cab, up_needle) != NULL;
}

static int resolve_contest_points(const char *mode, const char *band,
                                  const char *country,
                                  const char *call,
                                  const char *contest_id) {
  const CtyEntry *src_cty = cty_lookup(config.station_call);
  const CtyEntry *dst_cty = cty_lookup(call);
  const char *src_country = src_cty ? src_cty->country : "";
  const char *dst_country = dst_cty ? dst_cty->country : country;
  const char *src_cont = src_cty ? src_cty->continent : "";
  const char *dst_cont = dst_cty ? dst_cty->continent : "";
  const int same_country = src_country[0] && dst_country &&
                           strcmp(src_country, dst_country) == 0;
  const int same_cont = src_cont[0] && dst_cont[0] && strcmp(src_cont, dst_cont) == 0;

  if (contest_name_contains("CQ WPX")) {
    if (same_country)
      return 1;

    if (same_cont) {
      if (strcmp(src_cont, "NA") == 0 && strcmp(dst_cont, "NA") == 0)
        return is_low_band(band) ? 4 : 2;
      return is_low_band(band) ? 2 : 1;
    }

    return is_low_band(band) ? 6 : 3;
  }

  if (contest_name_contains("CQ WW") || contest_name_contains("CQ WORLD WIDE")) {
    if (same_country)
      return 0;
    if (call_is_maritime_mobile(call))
      return 0;
    if (same_cont) {
      if (strcmp(src_cont, "NA") == 0 && strcmp(dst_cont, "NA") == 0)
        return 2;
      return 1;
    }
    return 3;
  }

  if (contest_name_contains("SP DX")) {
    const int src_is_sp = src_country[0] && strcmp(src_country, "Poland") == 0;
    const int dst_is_sp = dst_country && strcmp(dst_country, "Poland") == 0;
    if (src_is_sp && dst_is_sp)
      return 0;
    if (!src_is_sp && !dst_is_sp)
      return 0;
    if (!src_is_sp && dst_is_sp)
      return 3;
    return same_cont ? 1 : 3;
  }

  const int base_points =
      active_contest_def.points_per_qso > 0 ? active_contest_def.points_per_qso
                                            : 1;

  if (active_contest_def.points_new_band_dxcc > 0 ||
      active_contest_def.points_same_band_dxcc > 0) {
    if (was_band_dxcc_worked(band, country, contest_id)) {
      if (active_contest_def.points_same_band_dxcc > 0)
        return active_contest_def.points_same_band_dxcc;
    } else {
      if (active_contest_def.points_new_band_dxcc > 0)
        return active_contest_def.points_new_band_dxcc;
    }
  }

  if (active_contest_def.points_new_dxcc > 0 ||
      active_contest_def.points_same_dxcc > 0) {
    if (was_dxcc_worked(country, contest_id)) {
      if (active_contest_def.points_same_dxcc > 0)
        return active_contest_def.points_same_dxcc;
    } else {
      if (active_contest_def.points_new_dxcc > 0)
        return active_contest_def.points_new_dxcc;
    }
  }

  if (mode && mode[0]) {
    if (strcmp(mode, "CW") == 0 && active_contest_def.points_cw > 0)
      return active_contest_def.points_cw;
    if (is_phone_mode_label(mode) && active_contest_def.points_phone > 0)
      return active_contest_def.points_phone;
    if (is_digi_mode_label(mode) && active_contest_def.points_digi > 0)
      return active_contest_def.points_digi;
  }

  return base_points;
}

int app_controller_get_active_frequency_khz(void) {
  return resolve_qso_frequency_khz();
}

ContestTechnique app_controller_get_contest_technique(void) {
  return config.contest_technique;
}

void app_controller_set_contest_technique(ContestTechnique technique) {
  config.contest_technique = technique;

  if (config.contest_technique == CONTEST_TECH_SO1R)
    active_radio_nr = 1;

  app_controller_set_active_radio(active_radio_nr);

  config_save("logger.conf");
}

int app_controller_get_active_radio(void) { return active_radio_nr; }

void app_controller_set_active_radio(int radio_nr) {
  if (radio_nr < 1)
    radio_nr = 1;
  if (radio_nr > 2)
    radio_nr = 2;

  if (config.contest_technique == CONTEST_TECH_SO1R)
    radio_nr = 1;

  active_radio_nr = radio_nr;

  const int idx = radio_index_from_nr(active_radio_nr);
  const int slot = cat_slot_for_radio(idx);
  const CatVfo vfo = cat_vfo_for_radio(idx);
  if (cat_is_connected_slot(slot)) {
    if (config.contest_technique == CONTEST_TECH_SO2V)
      cat_set_active_vfo_slot(slot, vfo);
    cat_set_frequency_khz_slot_vfo(slot, vfo, manual_entry_freq_khz[idx]);
  }

  update_dxcc_from_input(entry_call_for_idx(idx));
  refresh_callsign_suggestion(entry_call_for_idx(idx));
  update_composed_input_line();
}

void app_controller_swap_radios(void) {
  int tmp_freq = manual_entry_freq_khz[0];
  manual_entry_freq_khz[0] = manual_entry_freq_khz[1];
  manual_entry_freq_khz[1] = tmp_freq;

  int tmp_run = radio_run_state[0];
  radio_run_state[0] = radio_run_state[1];
  radio_run_state[1] = tmp_run;

  char tmp_call[sizeof(entry_call_by_radio[0])];
  char tmp_rst[sizeof(entry_rst_by_radio[0])];
  char tmp_comments[sizeof(entry_comments_by_radio[0])];
  snprintf(tmp_call, sizeof(tmp_call), "%s", entry_call_by_radio[0]);
  snprintf(tmp_rst, sizeof(tmp_rst), "%s", entry_rst_by_radio[0]);
  snprintf(tmp_comments, sizeof(tmp_comments), "%s", entry_comments_by_radio[0]);
  snprintf(entry_call_by_radio[0], sizeof(entry_call_by_radio[0]), "%s",
           entry_call_by_radio[1]);
  snprintf(entry_rst_by_radio[0], sizeof(entry_rst_by_radio[0]), "%s",
           entry_rst_by_radio[1]);
  snprintf(entry_comments_by_radio[0], sizeof(entry_comments_by_radio[0]), "%s",
           entry_comments_by_radio[1]);
  snprintf(entry_call_by_radio[1], sizeof(entry_call_by_radio[1]), "%s", tmp_call);
  snprintf(entry_rst_by_radio[1], sizeof(entry_rst_by_radio[1]), "%s", tmp_rst);
  snprintf(entry_comments_by_radio[1], sizeof(entry_comments_by_radio[1]), "%s",
           tmp_comments);

  int tmp_field = active_entry_field_by_radio[0];
  active_entry_field_by_radio[0] = active_entry_field_by_radio[1];
  active_entry_field_by_radio[1] = tmp_field;

  if (config.contest_technique == CONTEST_TECH_SO2V ||
      config.contest_technique == CONTEST_TECH_SO2R) {
    active_radio_nr = active_radio_nr == 1 ? 2 : 1;
  }

  app_controller_set_active_radio(active_radio_nr);
}

void app_controller_toggle_run_sp(int radio_nr) {
  const int idx = radio_index_from_nr(radio_nr);
  radio_run_state[idx] = radio_run_state[idx] ? 0 : 1;
}

int app_controller_get_radio_state(int radio_nr, int *out_freq_khz,
                                   char *out_mode, size_t out_mode_size,
                                   int *out_is_run) {
  const int idx = radio_index_from_nr(radio_nr);
  const int slot = cat_slot_for_radio(idx);
  const CatVfo vfo = cat_vfo_for_radio(idx);

  int freq_khz = manual_entry_freq_khz[idx];
  if (cat_is_connected_slot(slot)) {
    int cat_freq_khz = 0;
    if (cat_get_frequency_khz_slot_vfo(slot, vfo, &cat_freq_khz) == 0 &&
        is_valid_frequency_khz(cat_freq_khz)) {
      freq_khz = cat_freq_khz;
    }
  }

  if (out_freq_khz)
    *out_freq_khz = freq_khz;

  if (out_mode && out_mode_size >= 2) {
    out_mode[0] = 0;

    if (contest_definition_loaded && active_contest_def.mode[0] &&
        strcmp(active_contest_def.mode, "MIXED") != 0) {
      if (strcmp(active_contest_def.mode, "PHONE") == 0)
        snprintf(out_mode, out_mode_size, "%s", "SSB");
      else
        snprintf(out_mode, out_mode_size, "%s", active_contest_def.mode);
    } else if (cat_is_connected_slot(slot) && config.cat_mode_from_rig) {
      if (cat_get_mode_label_slot_vfo(slot, vfo, out_mode, out_mode_size) != 0 ||
          !out_mode[0]) {
        detect_mode(freq_khz, out_mode);
      }
    } else {
      detect_mode(freq_khz, out_mode);
    }
  }

  if (out_is_run)
    *out_is_run = radio_run_state[idx];

  return 0;
}

/*
 * Trim leading and trailing whitespace from a string in place.
 *
 * @param s String to trim.
 * @return Nothing.
 */
static void trim_whitespace_in_place(char *s) {
  if (!s)
    return;

  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) {
    s[len - 1] = 0;
    len--;
  }

  size_t start = 0;
  while (s[start] && isspace((unsigned char)s[start]))
    start++;

  if (start > 0)
    memmove(s, s + start, strlen(s + start) + 1);
}

/*
 * Archive the current logbook, clear it, and reset UI state.
 *
 * @return Nothing.
 */
static void create_new_clean_log(void) {
  if (db_archive_current_logbook() != 0 || db_clear_logbook() != 0) {
    snprintf(status_text, sizeof(status_text), "New log failed");
    return;
  }

  reset_loaded_log_state();
  snprintf(status_text, sizeof(status_text), "New clean log created");
}

/*
 * Archive the current logbook under a name and reset UI state.
 *
 * @param name Optional logbook name.
 * @return Nothing.
 */
static void create_new_named_log(const char *name) {
  if (!name || !name[0]) {
    create_new_clean_log();
    return;
  }

  if (db_archive_current_logbook_named(name) != 0) {
    snprintf(status_text, sizeof(status_text), "New named log failed");
    return;
  }

  reset_loaded_log_state();
  snprintf(status_text, sizeof(status_text), "New log created: %s", name);
}

/*
 * Reopen the previously archived logbook.
 *
 * @return Nothing.
 */
static void open_previous_log(void) {
  if (db_open_previous_logbook() != 0) {
    snprintf(status_text, sizeof(status_text), "No previous log available");
    return;
  }

  reset_loaded_log_state();
  snprintf(status_text, sizeof(status_text), "Previous log opened");
}

/*
 * Format the list of named logbooks into the status area.
 *
 * @return Nothing.
 */
static void list_named_logs(void) {
  DBNamedLogbook items[NAMED_LOG_LIST_MAX];
  int count = 0;

  if (db_list_named_logbooks(items, NAMED_LOG_LIST_MAX, &count) != 0) {
    snprintf(status_text, sizeof(status_text), "Cannot read named logs");
    return;
  }

  if (count <= 0) {
    info_text[0] = 0;
    snprintf(status_text, sizeof(status_text), "No named logs in DB");
    return;
  }

  char line[sizeof(info_text)] = {0};
  size_t used = 0;

  for (int i = 0; i < count; i++) {
    char piece[96];
    snprintf(piece, sizeof(piece), "%lld:%s(%d)%s", items[i].id, items[i].name,
             items[i].qso_count, (i + 1 < count) ? " | " : "");

    size_t piece_len = strlen(piece);
    if (used + piece_len >= sizeof(line) - 1)
      break;

    memcpy(line + used, piece, piece_len);
    used += piece_len;
    line[used] = 0;
  }

  snprintf(info_text, sizeof(info_text), "%s", line);
  snprintf(status_text, sizeof(status_text),
           "Named logs: %d (use openlog <id|name>)", count);
}

/*
 * Open a named logbook by id or name selector.
 *
 * @param selector Logbook id or name.
 * @return Nothing.
 */
static void open_named_log_selector(const char *selector) {
  if (!selector || !selector[0]) {
    snprintf(status_text, sizeof(status_text), "Usage: openlog <id|name>");
    return;
  }

  int rc = -1;
  if (is_digits_only(selector)) {
    rc = db_open_named_logbook_by_id(atoll(selector));
  } else {
    rc = db_open_named_logbook_by_name(selector);
  }

  if (rc != 0) {
    snprintf(status_text, sizeof(status_text), "Named log not found: %s",
             selector);
    return;
  }

  reset_loaded_log_state();
  snprintf(status_text, sizeof(status_text), "Log opened: %s", selector);
}

/*
 * Refresh call suggestions for the current input buffer.
 *
 * @param input Input buffer to analyze.
 * @return Nothing.
 */
static void refresh_callsign_suggestion(const char *input) {
  clear_callsign_suggestion();

  call_suggestion_refresh(&call_suggestions, input, call_history,
                          call_history_count);
  sync_callsign_suggestion_state();
}

/*
 * Apply the currently selected callsign suggestion to the input buffer.
 *
 * @param input Input buffer to modify.
 * @param len Current input length, updated on success.
 * @return 1 if a suggestion was applied, otherwise 0.
 */
static int apply_callsign_suggestion(char *input, int *len) {
  return call_suggestion_apply(&call_suggestions, input, len,
                               sizeof(input_buffer));
}

/*
 * Synchronize public suggestion state from the internal suggestion list.
 *
 * @return Nothing.
 */
static void sync_callsign_suggestion_state(void) {
  call_suggestion_count = call_suggestions.count;
  call_suggestion_selected_index = call_suggestions.selected;
  call_suggestion_available = call_suggestion_count > 0;

  for (int i = 0; i < CALL_SUGGESTION_MAX; i++) {
    if (i < call_suggestion_count)
      snprintf(call_suggestion_matches[i], CALL_SUGGESTION_LEN, "%s",
               call_suggestions.matches[i]);
    else
      call_suggestion_matches[i][0] = 0;
  }
}

/*
 * Export the current logbook to ADIF using an explicit filename.
 *
 * @param adif_file Destination ADIF filename.
 * @return 0 on success, or -1 on failure.
 */
static int export_with_adif_filename(const char *adif_file) {
  const char *default_csv = "log.csv";

  if (!adif_file || !adif_file[0])
    return -1;

  if (export_csv(default_csv) != 0)
    return -1;

  if (export_adif(adif_file) != 0)
    return -1;

  snprintf(status_text, sizeof(status_text), "Exported ADIF: %s", adif_file);

  return 0;
}

/*
 * Export the current logbook to Cabrillo using loaded contest definition.
 */
static int export_cabrillo_with_filename(const char *cab_file) {
  if (!cab_file || !cab_file[0])
    return -1;

  if (!contest_definition_loaded)
    contest_definition_init_defaults(&active_contest_def);

  if (export_cabrillo(cab_file, &active_contest_def, config.station_call) != 0)
    return -1;

  snprintf(status_text, sizeof(status_text), "Exported Cabrillo: %s", cab_file);
  return 0;
}

/*
 * Load one DXLog-like contest definition and store it in controller state.
 */
static int load_contest_definition_file(const char *path) {
  if (!path || !path[0])
    return -1;

  char err[128] = {0};
  if (contest_definition_load(path, &active_contest_def, err, sizeof(err)) != 0) {
    const char *msg = err[0] ? err : path;
    snprintf(status_text, sizeof(status_text), "Contest load failed: %.96s",
             msg ? msg : "unknown");
    return -1;
  }

  contest_definition_loaded = 1;
  clear_entry_fields();
  snprintf(contest_exchange_label_text, sizeof(contest_exchange_label_text), "%s",
           contest_exchange_label());
  stats_set_contest_definition(&active_contest_def);
  stats_update();
  snprintf(status_text, sizeof(status_text), "Contest loaded: %s",
           active_contest_def.name);
  return 0;
}

static int resolve_contest_definition_path(const char *path, char *resolved,
                                          size_t resolved_size) {
  if (!path || !path[0] || !resolved || resolved_size == 0)
    return -1;

  if (access(path, R_OK) == 0) {
    snprintf(resolved, resolved_size, "%s", path);
    return 0;
  }

  if (path[0] == '/')
    return -1;

  const char *prefixes[] = {"../", "../../"};
  char candidate[512];

  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    snprintf(candidate, sizeof(candidate), "%s%s", prefixes[i], path);
    if (access(candidate, R_OK) == 0) {
      snprintf(resolved, resolved_size, "%s", candidate);
      return 0;
    }
  }

  if (!strchr(path, '/')) {
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
      snprintf(candidate, sizeof(candidate), "%scontest_defs/%s", prefixes[i],
               path);
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }
    }
  }

  return -1;
}

/*
 * Load contest definition path from config with fallback relative locations.
 */
static int load_configured_contest_definition(void) {
  if (!config.contest_definition_path[0])
    return 0;

  char resolved[512];
  if (resolve_contest_definition_path(config.contest_definition_path, resolved,
                                      sizeof(resolved)) != 0)
    return -1;

  return load_contest_definition_file(resolved);
}

/*
 * Parse an export command and choose the ADIF destination filename.
 *
 * @param cmd Raw command line.
 * @return 0 on success, or -1 on failure.
 */
static int export_with_optional_adif(const char *cmd) {
  const char *default_adif = "log.adi";
  const char *adif_file = default_adif;

  if (cmd) {
    const char *p = cmd;

    while (*p && isspace((unsigned char)*p))
      p++;

    if (strncmp(p, "export", 6) != 0)
      return -1;

    p += 6;

    while (*p && isspace((unsigned char)*p))
      p++;

    if (*p)
      adif_file = p;
  }

  return export_with_adif_filename(adif_file);
}

/*
 * Handle a typed command line entered into the controller.
 *
 * @param cmd Raw command text.
 * @return 1 if the input was treated as a command, otherwise 0.
 */
static int process_command(const char *cmd) {
  char command[32] = {0};
  char arg[192] = {0};

  if (!cmd || !cmd[0])
    return 0;

  const char *p = cmd;
  while (*p && isspace((unsigned char)*p))
    p++;

  int i = 0;
  while (*p && !isspace((unsigned char)*p) && i < (int)sizeof(command) - 1) {
    command[i++] = (char)tolower((unsigned char)*p);
    p++;
  }
  command[i] = 0;

  while (*p && isspace((unsigned char)*p))
    p++;

  snprintf(arg, sizeof(arg), "%s", p);
  trim_whitespace_in_place(arg);

  if (strcmp(command, "export") == 0) {
    if (export_with_optional_adif(cmd) != 0)
      snprintf(status_text, sizeof(status_text), "Export failed");

    return 1;
  }

  if (strcmp(command, "newlog") == 0 || strcmp(command, "clear") == 0) {
    if (arg[0])
      create_new_named_log(arg);
    else
      create_new_clean_log();

    return 1;
  }

  if (strcmp(command, "prevlog") == 0 || strcmp(command, "openprev") == 0 ||
      strcmp(command, "previous") == 0) {
    open_previous_log();
    return 1;
  }

  if (strcmp(command, "logs") == 0) {
    list_named_logs();
    return 1;
  }

  if (strcmp(command, "openlog") == 0) {
    open_named_log_selector(arg);
    return 1;
  }

  if (strcmp(command, "invalid") == 0) {
    if (qso_count > 0) {
      qso_mark_invalid(qso_count - 1);

      snprintf(status_text, sizeof(status_text), "Last QSO toggled INVALID");
    }

    return 1;
  }

  if (strcmp(command, "contest") == 0) {
    if (!arg[0]) {
      snprintf(status_text, sizeof(status_text), "Usage: contest <file>");
    } else if ((strncmp(arg, "import", 6) == 0 &&
                (arg[6] == 0 || isspace((unsigned char)arg[6]))) ||
               (strncmp(arg, "import-only", 11) == 0 &&
                (arg[11] == 0 || isspace((unsigned char)arg[11])))) {
      char source[256] = {0};
      char dest[256] = "contest.conf";
      const int import_only = strncmp(arg, "import-only", 11) == 0;
      const char *pimp = arg + (import_only ? 11 : 6);
      while (*pimp && isspace((unsigned char)*pimp))
        pimp++;

      if (!*pimp) {
        snprintf(status_text, sizeof(status_text),
                 import_only
                   ? "Usage: contest import-only <dxlog_file> [output_conf]"
                   : "Usage: contest import <dxlog_file> [output_conf]");
        return 1;
      }

      int n = sscanf(pimp, "%255s %255s", source, dest);
      if (n < 1 || !source[0]) {
        snprintf(status_text, sizeof(status_text),
                 import_only
                   ? "Usage: contest import-only <dxlog_file> [output_conf]"
                   : "Usage: contest import <dxlog_file> [output_conf]");
        return 1;
      }

      char resolved_source[512] = {0};
      const char *src_path = source;
      if (resolve_contest_definition_path(source, resolved_source,
                                          sizeof(resolved_source)) == 0)
        src_path = resolved_source;

      char err[128] = {0};
      char warn[256] = {0};
      if (contest_definition_import_dxlog(src_path, dest, err, sizeof(err),
                                          warn, sizeof(warn)) != 0) {
        snprintf(status_text, sizeof(status_text), "Contest import failed: %.96s",
                 err[0] ? err : "unknown");
        info_text[0] = 0;
        return 1;
      }

      if (warn[0])
        snprintf(info_text, sizeof(info_text), "%.127s", warn);
      else
        info_text[0] = 0;

      if (import_only) {
        snprintf(status_text, sizeof(status_text),
                 "Contest imported (not loaded)");
        return 1;
      }

      snprintf(config.contest_definition_path,
               sizeof(config.contest_definition_path), "%s", dest);
      config_save("logger.conf");

      if (load_contest_definition_file(dest) == 0) {
        if (warn[0])
          snprintf(status_text, sizeof(status_text),
                   "Contest imported and loaded (with warnings)");
        else
          snprintf(status_text, sizeof(status_text),
                   "Contest imported and loaded");
      }
    } else if (strcmp(arg, "none") == 0 || strcmp(arg, "off") == 0 ||
               strcmp(arg, "clear") == 0) {
      contest_definition_loaded = 0;
      contest_definition_init_defaults(&active_contest_def);
      snprintf(contest_exchange_label_text, sizeof(contest_exchange_label_text),
               "%s", "Exchange");
      clear_entry_fields();
      stats_set_contest_definition(&active_contest_def);
      stats_update();
      snprintf(status_text, sizeof(status_text), "Contest cleared");
    } else {
      char resolved[512];
      if (resolve_contest_definition_path(arg, resolved, sizeof(resolved)) == 0)
        load_contest_definition_file(resolved);
      else
        load_contest_definition_file(arg);
    }
    return 1;
  }

  if (strcmp(command, "exportcab") == 0) {
    const char *cab_file = arg[0] ? arg : "log.cbr";
    if (export_cabrillo_with_filename(cab_file) != 0)
      snprintf(status_text, sizeof(status_text), "Cabrillo export failed");
    return 1;
  }

  if (strcmp(command, "technique") == 0) {
    if (!arg[0]) {
      snprintf(status_text, sizeof(status_text), "Technique: %s",
               contest_technique_to_text(config.contest_technique));
    } else {
      app_controller_set_contest_technique(contest_technique_from_text(arg));
      snprintf(status_text, sizeof(status_text), "Technique set: %s",
               contest_technique_to_text(config.contest_technique));
    }
    return 1;
  }

  return 0;
}

/*
 * Update the DXCC display state based on the current input text.
 *
 * @param input Current input buffer.
 * @return Nothing.
 */
static void update_dxcc_from_input(const char *input) {
  dxcc_text[0] = 0;
  last_cq = 0;
  last_itu = 0;

  if (!input || !input[0])
    return;

  char call[32] = {0};
  size_t len = 0;
  const char *p = input;

  while (*p && isspace((unsigned char)*p))
    p++;

  while (*p && *p != ';' && !isspace((unsigned char)*p) &&
         len < sizeof(call) - 1) {
    char c = (char)toupper((unsigned char)*p);
    if (c == '/')
      break;

    call[len++] = c;
    p++;
  }

  call[len] = 0;

  if (len < 2)
    return;

  const CtyEntry *cty = cty_lookup(call);

  if (cty) {
    snprintf(dxcc_text, sizeof(dxcc_text), "%s", cty->country);

    last_cq = cty->cq_zone;
    last_itu = cty->itu_zone;
  } else {
    snprintf(dxcc_text, sizeof(dxcc_text), "Unknown");
  }
}

/*
 * Finalize a parsed QSO line and refresh derived UI state.
 *
 * @param line QSO input line.
 * @return Nothing.
 */
static void process_qso(const char *line) {
  int idx = qso_add(line, status_text, sizeof(status_text));

  if (idx < 0)
    return;

  QSO *q = &logbook[idx];
  snprintf(info_text, sizeof(info_text), "%s %s", q->band, q->mode);

  if (q->country[0] && strcmp(q->country, "UNKNOWN") != 0) {
    snprintf(dxcc_text, sizeof(dxcc_text), "%s", q->country);
    last_cq = q->cq_zone;
    last_itu = q->itu_zone;
  } else {
    snprintf(dxcc_text, sizeof(dxcc_text), "Unknown");
    last_cq = 0;
    last_itu = 0;
  }

  stats_update();
}

/*
 * Apply shared UI/stat state updates after inserting a QSO.
 *
 * @param idx Index of the inserted QSO in the in-memory logbook.
 * @return 1 on success, otherwise 0.
 */
static int apply_saved_qso_state(int idx) {
  if (idx < 0 || idx >= qso_count)
    return 0;

  QSO *q = &logbook[idx];

  snprintf(info_text, sizeof(info_text), "%s %s", q->band, q->mode);

  if (q->country[0] && strcmp(q->country, "UNKNOWN") != 0) {
    snprintf(dxcc_text, sizeof(dxcc_text), "%s", q->country);

    last_cq = q->cq_zone;
    last_itu = q->itu_zone;
  } else {
    snprintf(dxcc_text, sizeof(dxcc_text), "Unknown");
    last_cq = 0;
    last_itu = 0;
  }

  stats_update();
  return 1;
}

/*
 * Refresh the rendered info line based on suggestions and current input.
 *
 * @return Nothing.
 */
static void update_display_info(void) {
  if (call_suggestion_available && !export_prompt_mode) {
    const char *selected = call_suggestion_selected(&call_suggestions);
    if (selected && call_suggestion_count > 1) {
      snprintf(display_info, sizeof(display_info), "Suggest: %s (+%d more) [Tab]",
               selected, call_suggestion_count - 1);
    } else if (selected) {
      snprintf(display_info, sizeof(display_info), "Suggest: %s [Tab]", selected);
    } else {
      snprintf(display_info, sizeof(display_info), "%s", info_text);
    }
  } else {
    snprintf(display_info, sizeof(display_info), "%s", info_text);
  }
}

/*
 * Initialize shared application state and background services.
 *
 * @return 0 on success.
 */
int app_controller_init(void) {
  memset(input_buffer, 0, sizeof(input_buffer));
  input_len = 0;
  clear_entry_fields();
  memset(call_history, 0, sizeof(call_history));
  call_history_count = 0;
  snprintf(status_text, sizeof(status_text), "Ready");
  dxcc_text[0] = 0;
  info_text[0] = 0;
  display_info[0] = 0;
  cluster_view = true;
  cluster_scroll = 0;
  export_prompt_mode = false;
  manual_entry_freq_khz[0] = 14074;
  manual_entry_freq_khz[1] = 7020;
  active_radio_nr = 1;
  radio_run_state[0] = 1;
  radio_run_state[1] = 0;
  cty_update_in_progress = 0;
  last_cq = 0;
  last_itu = 0;
  contest_definition_loaded = 0;
  contest_definition_init_defaults(&active_contest_def);
  snprintf(contest_exchange_label_text, sizeof(contest_exchange_label_text), "%s",
           "Exchange");
  stats_set_contest_definition(&active_contest_def);
  clear_callsign_suggestion();

  if (config_load("logger.conf") != 0)
    fprintf(stderr, "Cannot load logger.conf\n");

  load_configured_contest_definition();

  stats_set_contest_definition(&active_contest_def);

  cty_load("wl_cty.dat");
  qso_init();
  call_history_load_file("call_history.txt");

  if (app_debug_enabled) {
    fprintf(stderr,
            "[debug] app_controller_init: cluster_view=%d, starting DXCluster\n",
            cluster_view ? 1 : 0);
  }

  if (dxcluster_start() != 0) {
    snprintf(status_text, sizeof(status_text), "DXCluster start failed");
    update_display_info();
    return -1;
  }
  update_display_info();

  return 0;
}

/*
 * Shut down shared application state and stop background services.
 *
 * @return Nothing.
 */
void app_controller_shutdown(void) {
  dxcluster_stop();
  db_shutdown();
}

/*
 * Copy the current render state into out for UI consumers.
 *
 * @param out Destination structure to fill.
 * @return Nothing.
 */
void app_controller_get_render_state(AppRenderState *out) {
  if (!out)
    return;

  update_display_info();

  const int active_idx = active_radio_index();
  const char *active_call = entry_call_for_idx(active_idx);
  const char *active_rst = entry_rst_for_idx(active_idx);
  const char *active_comments = entry_comments_for_idx(active_idx);

  out->input = input_buffer;
  out->input_call = active_call;
  out->input_rst = active_rst;
  out->input_comments = active_comments;
  out->active_input_field = *active_entry_field_for_idx(active_idx);
  out->input_call_r1 = entry_call_for_idx(0);
  out->input_rst_r1 = entry_rst_for_idx(0);
  out->input_comments_r1 = entry_comments_for_idx(0);
  out->active_input_field_r1 = *active_entry_field_for_idx(0);
  out->input_call_r2 = entry_call_for_idx(1);
  out->input_rst_r2 = entry_rst_for_idx(1);
  out->input_comments_r2 = entry_comments_for_idx(1);
  out->active_input_field_r2 = *active_entry_field_for_idx(1);
  out->status = status_text;
  out->dxcc = dxcc_text;
  out->info = display_info;
  out->contest_exchange_label = contest_exchange_label_text;
  static char contest_exchange_sent_text[32];
  contest_exchange_sent_text[0] = 0;
  if (contest_definition_loaded)
    build_exchange_sent(contest_exchange_sent_text,
                        sizeof(contest_exchange_sent_text));
  out->contest_exchange_sent = contest_exchange_sent_text;
  out->contest_entry_mode = contest_definition_loaded != 0;
  out->cluster_view = cluster_view;
  out->cluster_scroll = cluster_scroll;

  static char radio_mode_1[16];
  static char radio_mode_2[16];
  int radio1_freq = 0;
  int radio2_freq = 0;
  int radio1_run = 0;
  int radio2_run = 0;

  app_controller_get_radio_state(1, &radio1_freq, radio_mode_1,
                                 sizeof(radio_mode_1), &radio1_run);
  app_controller_get_radio_state(2, &radio2_freq, radio_mode_2,
                                 sizeof(radio_mode_2), &radio2_run);

  out->technique = contest_technique_to_text(config.contest_technique);
  out->active_radio = active_radio_nr;
  out->radio1_freq_khz = radio1_freq;
  out->radio2_freq_khz = radio2_freq;
  out->radio1_mode = radio_mode_1;
  out->radio2_mode = radio_mode_2;
  out->radio1_run = radio1_run != 0;
  out->radio2_run = radio2_run != 0;
}

AppControllerEvent app_controller_submit_command_text(const char *command_text) {
  char command_line[256] = {0};

  if (!command_text)
    return APP_CTRL_EVENT_NONE;

  snprintf(command_line, sizeof(command_line), "%s", command_text);
  trim_whitespace_in_place(command_line);
  if (!command_line[0])
    return APP_CTRL_EVENT_NONE;

  clear_active_entry_fields();
  clear_callsign_suggestion();
  export_prompt_mode = false;
  input_buffer[0] = 0;
  input_len = 0;

  if (strcmp(command_line, "quit") == 0)
    return APP_CTRL_EVENT_EXIT;

  process_command(command_line);
  update_composed_input_line();
  return APP_CTRL_EVENT_NONE;
}

void app_controller_complete_cty_update(int download_ok) {
  if (download_ok) {
    int loaded = cty_load("wl_cty.dat");
    if (loaded >= 0)
      snprintf(status_text, sizeof(status_text), "CTY updated (%d entries)",
               loaded);
    else
      snprintf(status_text, sizeof(status_text), "Downloaded CTY, reload failed");
  } else {
    snprintf(status_text, sizeof(status_text), "CTY download failed");
  }

  cty_update_in_progress = 0;
}

/*
 * Download and reload the latest CTY database.
 *
 * @return Nothing.
 */
void app_controller_perform_cty_update(void) {
  const int download_ok = cty_download_latest("wl_cty.dat") == 0;
  app_controller_complete_cty_update(download_ok);
}

/*
 * Handle a translated key code and update shared controller state.
 *
 * @param key One of the APP_KEY_* values.
 * @return The controller event to propagate to the UI.
 */
AppControllerEvent app_controller_handle_key(int key) {
  if (key == APP_KEY_NONE || key == APP_KEY_RESIZE)
    return APP_CTRL_EVENT_NONE;

  if (key == APP_KEY_F10) {
    return APP_CTRL_EVENT_EXIT;
  }

  if (cty_update_in_progress)
    return APP_CTRL_EVENT_NONE;

  if (key == APP_KEY_F2) {
    create_new_clean_log();
    return APP_CTRL_EVENT_NONE;
  }

  if (key == APP_KEY_F3) {
    open_previous_log();
    return APP_CTRL_EVENT_NONE;
  }

  if (key == APP_KEY_F4) {
    export_prompt_mode = true;
    clear_callsign_suggestion();
    input_buffer[0] = 0;
    input_len = 0;
    snprintf(status_text, sizeof(status_text),
             "Enter ADIF filename and press Enter (Esc to cancel)");
    return APP_CTRL_EVENT_NONE;
  }

  if (export_prompt_mode && key == APP_KEY_ESC) {
    export_prompt_mode = false;
    input_buffer[0] = 0;
    input_len = 0;
    snprintf(status_text, sizeof(status_text), "Export cancelled");
    return APP_CTRL_EVENT_NONE;
  }

  if (!export_prompt_mode && key == APP_KEY_ESC) {
    clear_entry_fields();
    clear_callsign_suggestion();
    dxcc_text[0] = 0;
    return APP_CTRL_EVENT_NONE;
  }

  if (!export_prompt_mode && key == APP_KEY_ALT_W) {
    clear_active_call_exchange_fields();
    clear_callsign_suggestion();
    dxcc_text[0] = 0;
    snprintf(status_text, sizeof(status_text), "CALL and Exchange cleared");
    return APP_CTRL_EVENT_NONE;
  }

  if (!export_prompt_mode &&
      (config.contest_technique == CONTEST_TECH_SO2R ||
       config.contest_technique == CONTEST_TECH_SO2V) &&
      (key == APP_KEY_LEFT || key == APP_KEY_RIGHT)) {
    app_controller_set_active_radio(key == APP_KEY_LEFT ? 1 : 2);
    return APP_CTRL_EVENT_NONE;
  }

  if (key == APP_KEY_F6) {
    stats_update();
    snprintf(status_text, sizeof(status_text), "STATS updated");
  }

  if (key == APP_KEY_F5) {
    cluster_view = !cluster_view;
    snprintf(status_text, sizeof(status_text),
             cluster_view ? "DXCluster window shown"
                          : "DXCluster window hidden");
    if (app_debug_enabled) {
      fprintf(stderr, "[debug] APP_KEY_F5: cluster_view=%d\n",
              cluster_view ? 1 : 0);
    }
  }

  if (key == APP_KEY_F7) {
    cty_update_in_progress = 1;
    snprintf(status_text, sizeof(status_text),
             "Downloading wl_cty.dat... keyboard locked");
    return APP_CTRL_EVENT_REQUEST_CTY_UPDATE;
  }

  if (key == APP_KEY_F1) {
    if (contest_definition_loaded) {
      snprintf(status_text, sizeof(status_text), "CALL %s | Space: next field | F2 new | F3 previous",
               contest_exchange_label());
    } else {
      snprintf(status_text, sizeof(status_text),
               "CALL RST COMMENTS | Space: next field | F2 new | F3 previous");
    }
    return APP_CTRL_EVENT_NONE;
  }

  if (!export_prompt_mode && key == APP_KEY_TAB) {
    if (*active_entry_field_for_idx(active_radio_index()) == ENTRY_FIELD_CALL) {
      const char *selected = call_suggestion_selected(&call_suggestions);
      if (selected) {
        char *entry_call = entry_call_for_idx(active_radio_index());
        snprintf(entry_call, sizeof(entry_call_by_radio[0]), "%s", selected);
        update_dxcc_from_input(entry_call);
        refresh_callsign_suggestion(entry_call);
      }
    }
    update_composed_input_line();
    return APP_CTRL_EVENT_NONE;
  }

    if (!export_prompt_mode &&
      *active_entry_field_for_idx(active_radio_index()) == ENTRY_FIELD_CALL &&
      call_suggestion_available && key == APP_KEY_UP) {
    call_suggestion_select_prev(&call_suggestions);
    sync_callsign_suggestion_state();
    return APP_CTRL_EVENT_NONE;
  }

    if (!export_prompt_mode &&
      *active_entry_field_for_idx(active_radio_index()) == ENTRY_FIELD_CALL &&
      call_suggestion_available && key == APP_KEY_DOWN) {
    call_suggestion_select_next(&call_suggestions);
    sync_callsign_suggestion_state();
    return APP_CTRL_EVENT_NONE;
  }

  if (!export_prompt_mode && key == APP_KEY_SPACE) {
    if (*active_entry_field_for_idx(active_radio_index()) == ENTRY_FIELD_CALL &&
        call_suggestion_available) {
      const char *selected = call_suggestion_selected(&call_suggestions);
      if (selected)
        snprintf(entry_call_for_idx(active_radio_index()),
                 sizeof(entry_call_by_radio[0]), "%s", selected);
    }

    advance_entry_field();
    update_dxcc_from_input(entry_call_for_idx(active_radio_index()));
    refresh_callsign_suggestion(entry_call_for_idx(active_radio_index()));
    update_composed_input_line();
    return APP_CTRL_EVENT_NONE;
  }

  if (key == APP_KEY_BACKSPACE) {
    if (export_prompt_mode) {
      if (input_len > 0)
        input_buffer[--input_len] = 0;
      return APP_CTRL_EVENT_NONE;
    }

    backspace_active_field();
    update_composed_input_line();
    update_dxcc_from_input(entry_call_for_idx(active_radio_index()));
    refresh_callsign_suggestion(entry_call_for_idx(active_radio_index()));
    return APP_CTRL_EVENT_NONE;
  }

  if (key == APP_KEY_ENTER) {
    if (export_prompt_mode) {
      if (strlen(input_buffer)) {
        if (export_with_adif_filename(input_buffer) != 0)
          snprintf(status_text, sizeof(status_text), "Export failed");
        export_prompt_mode = false;
        input_buffer[0] = 0;
        input_len = 0;
        clear_callsign_suggestion();
      } else {
        snprintf(status_text, sizeof(status_text),
                 "Please enter ADIF filename (Esc to cancel)");
      }

      return APP_CTRL_EVENT_NONE;
    }

    update_composed_input_line();

    char *entry_call = entry_call_for_idx(active_radio_index());
    char *entry_exchange = entry_rst_for_idx(active_radio_index());
    char *entry_comments = entry_comments_for_idx(active_radio_index());

    const int has_entry_input = contest_definition_loaded
                                    ? (entry_call[0] || entry_exchange[0])
                                    : (entry_call[0] || entry_exchange[0] ||
                                       entry_comments[0]);

    if (has_entry_input) {
      char command_line[256] = {0};
      compose_command_line(command_line, sizeof(command_line));

      if (strcmp(command_line, "quit") == 0) {
        return APP_CTRL_EVENT_EXIT;
      }

      if (!process_command(command_line)) {
        if (entry_call[0] && !entry_exchange[0] && !entry_comments[0] &&
            is_digits_only(entry_call)) {
          const int freq_khz = atoi(entry_call);
          const int radio_idx = radio_index_from_nr(active_radio_nr);
          const int slot = cat_slot_for_radio(radio_idx);

          if (!is_valid_frequency_khz(freq_khz)) {
            snprintf(status_text, sizeof(status_text), "Invalid frequency");
          } else {
            manual_entry_freq_khz[radio_idx] = freq_khz;
            const CatVfo vfo = cat_vfo_for_radio(radio_idx);

            if (cat_is_connected_slot(slot)) {
              if (cat_set_frequency_khz_slot_vfo(slot, vfo, freq_khz) == 0) {
                snprintf(status_text, sizeof(status_text),
                         "Frequency set to %d kHz (manual + CAT)", freq_khz);
              } else {
                snprintf(status_text, sizeof(status_text),
                         "Frequency set to %d kHz (manual)", freq_khz);
              }
            } else {
              snprintf(status_text, sizeof(status_text),
                       "Frequency set to %d kHz (manual)", freq_khz);
            }
          }
        } else if (!contest_definition_loaded && entry_call[0] && entry_exchange[0] &&
                   entry_comments[0] &&
            strlen(entry_exchange) >= 4 && is_digits_only(entry_exchange) &&
            is_digits_only(entry_comments)) {
          process_qso(command_line);
          call_history_record_from_input(command_line);
        } else if (entry_call[0] && entry_exchange[0]) {
          if (contest_definition_loaded &&
              !validate_contest_exchange(entry_exchange, status_text,
                                         sizeof(status_text))) {
            clear_active_entry_fields();
            clear_callsign_suggestion();
            return APP_CTRL_EVENT_NONE;
          }

          const int qso_freq_khz = resolve_qso_frequency_khz();
          char qso_mode[16] = {0};
          const char *mode = resolve_qso_mode(qso_mode, sizeof(qso_mode));
          char qso_band[8] = {0};
          detect_band(qso_freq_khz, qso_band);

          const CtyEntry *cty = cty_lookup(entry_call);
          const char *country = cty ? cty->country : "UNKNOWN";

          char exchange_sent[32] = {0};
          build_exchange_sent(exchange_sent, sizeof(exchange_sent));
          const int qso_points = resolve_contest_points(
            mode, qso_band, country, entry_call, active_contest_def.cabrillo_name);

          const char *qso_rst = contest_definition_loaded
                                    ? (strcmp(mode, "CW") == 0 ? "599" : "59")
                                    : entry_exchange;
          const char *qso_comments = contest_definition_loaded ? "" : entry_comments;
          const char *exchange_recv = contest_definition_loaded ? entry_exchange : "";

          int idx = qso_add_contest_fields(
              entry_call, qso_freq_khz, qso_rst, mode, qso_comments,
              exchange_sent, exchange_recv, active_operator_mode_text(),
            active_contest_def.cabrillo_name, active_radio_nr, qso_points,
              status_text, sizeof(status_text));
          if (apply_saved_qso_state(idx)) {
            call_history_record_from_input(entry_call);
          }
        } else {
          if (contest_definition_loaded)
            snprintf(status_text, sizeof(status_text), "Need: CALL %s",
                     contest_exchange_label());
          else
            snprintf(status_text, sizeof(status_text), "Bad format");
        }
      }
    }

    clear_active_entry_fields();
    clear_callsign_suggestion();
    return APP_CTRL_EVENT_NONE;
  }

  if (key >= 0 && key <= 255 && isprint(key)) {
    if (export_prompt_mode) {
      if (input_len < (int)sizeof(input_buffer) - 1) {
        input_buffer[input_len++] = (char)key;
        input_buffer[input_len] = 0;
      }
      return APP_CTRL_EVENT_NONE;
    }

    if (append_to_active_field(key)) {
      update_composed_input_line();
      update_dxcc_from_input(entry_call_for_idx(active_radio_index()));
      refresh_callsign_suggestion(entry_call_for_idx(active_radio_index()));
    }
  }

  if (!export_prompt_mode)
    update_composed_input_line();

  return APP_CTRL_EVENT_NONE;
}