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

  return "EXCH";
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
  qtc_init();
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
static int highest_numeric_exchange_sent(void) {
  int max_serial = 0;

  for (int i = 0; i < qso_count; i++) {
    const QSO *q = &logbook[i];
    if (!q || !q->exchange_sent[0])
      continue;

    char *end = NULL;
    errno = 0;
    const long value = strtol(q->exchange_sent, &end, 10);
    if (errno != 0 || end == q->exchange_sent || *end != '\0' || value < 0)
      continue;

    if (value > max_serial)
      max_serial = (int)value;
  }

  return max_serial;
}

static void build_exchange_sent(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  out[0] = 0;
  const char *tpl = active_contest_def.exchange_sent_template;
  if (!tpl || !tpl[0]) {
    const int largest_serial = highest_numeric_exchange_sent();
    const int next_serial = (qso_count > largest_serial ? qso_count : largest_serial) + 1;
    snprintf(out, out_size, "%d", next_serial);
    return;
  }

  // Contest rule: only '#' means serial; all other templates are static.
  if (strcmp(tpl, "#") == 0) {
    const int largest_serial = highest_numeric_exchange_sent();
    const int next_serial = (qso_count > largest_serial ? qso_count : largest_serial) + 1;
    snprintf(out, out_size, "%d", next_serial);
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

static int text_contains_token_ci(const char *text, const char *token) {
  if (!text || !text[0] || !token || !token[0])
    return 0;

  char up_text[96] = {0};
  char up_token[48] = {0};
  snprintf(up_text, sizeof(up_text), "%s", text);
  snprintf(up_token, sizeof(up_token), "%s", token);

  for (size_t i = 0; up_text[i]; i++)
    up_text[i] = (char)toupper((unsigned char)up_text[i]);
  for (size_t i = 0; up_token[i]; i++)
    up_token[i] = (char)toupper((unsigned char)up_token[i]);

  return strstr(up_text, up_token) != NULL;
}

static int logbook_has_qtc_capable_contest_hint(void) {
  if (qtc_bundle_count > 0)
    return 1;

  for (int i = 0; i < qso_count; i++) {
    if (text_contains_token_ci(logbook[i].contest_id, "WAEDC") ||
        text_contains_token_ci(logbook[i].contest_id, "WAE"))
      return 1;
  }

  return 0;
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

  /*
   * WAE DX scoring: only intercontinental EU↔DX QSOs score 1 point.
   * EU–EU or DX–DX QSOs score 0 points.
   * Match both the short name "WAE" and the Cabrillo token "WAEDC".
   */
  if (contest_name_contains("WAE") || contest_name_contains("WAEDC")) {
    const int src_is_eu = src_cont[0] && strcmp(src_cont, "EU") == 0;
    const int dst_is_eu = dst_cont[0] && strcmp(dst_cont, "EU") == 0;
    /* Both EU or both DX → 0 points */
    if (src_is_eu == dst_is_eu)
      return 0;
    return 1;
  }

  /*
   * IARU HF World Championship scoring.
   * Same zone/country → 1 pt, same continent → 3 pts, diff continent → 5 pts.
   * HQ station bonus (1 pt) requires a lookup table and is not implemented.
   */
  if (contest_name_contains("IARU")) {
    if (same_country)
      return 1;
    return same_cont ? 3 : 5;
  }

  /*
   * Scandinavian Activity Contest scoring.
   * Scandinavian DXCC (LA/OH/SM/OZ/OX/OY/JW/JX/OJ0/OH0/TF) detection uses
   * the main CTY prefix.
   */
  if (contest_name_contains("SAC")) {
    static const char * const scand[] = {
      "JW", "JX", "LA", "OH", "OH0", "OJ0", "OX", "OY", "OZ", "SM", "TF", NULL
    };
    int src_is_scand = 0, dst_is_scand = 0;
    if (src_cty) {
      char up_pfx[16];
      snprintf(up_pfx, sizeof(up_pfx), "%s", src_cty->prefix);
      for (size_t i = 0; up_pfx[i]; i++)
        up_pfx[i] = (char)toupper((unsigned char)up_pfx[i]);
      for (int i = 0; scand[i]; i++)
        if (strcmp(up_pfx, scand[i]) == 0) { src_is_scand = 1; break; }
    }
    if (dst_cty) {
      char up_pfx[16];
      snprintf(up_pfx, sizeof(up_pfx), "%s", dst_cty->prefix);
      for (size_t i = 0; up_pfx[i]; i++)
        up_pfx[i] = (char)toupper((unsigned char)up_pfx[i]);
      for (int i = 0; scand[i]; i++)
        if (strcmp(up_pfx, scand[i]) == 0) { dst_is_scand = 1; break; }
    }
    if (src_is_scand && dst_is_scand) return 0;  /* Scand-Scand not valid */
    if (src_is_scand) {
      return same_cont ? 2 : 3;  /* Scand→EU=2, Scand→non-EU=3 */
    }
    if (dst_is_scand) {
      /* non-EU→Scand differs by band; low bands=3, high bands=1.
       * EU→Scand is always 1 pt regardless of band. */
      if (strcmp(src_cont, "EU") == 0)
        return 1;
      return is_low_band(band) ? 3 : 1;
    }
    return 0;  /* non-Scand to non-Scand not valid */
  }

  /*
   * ARRL DX scoring: K/VE ↔ DX = 3 pts each way; same DXCC = 0.
   */
  if (contest_name_contains("ARRL DX") || contest_name_contains("ARRL-DX")) {
    const int src_is_na = src_cty &&
                          (strncmp(src_cty->prefix, "K",  1) == 0 ||
                           strncmp(src_cty->prefix, "VE", 2) == 0 ||
                           strcmp(src_cty->prefix,  "W")  == 0);
    const int dst_is_na = dst_cty &&
                          (strncmp(dst_cty->prefix, "K",  1) == 0 ||
                           strncmp(dst_cty->prefix, "VE", 2) == 0 ||
                           strcmp(dst_cty->prefix,  "W")  == 0);
    if (same_country) return 0;
    if (src_is_na == dst_is_na) return 0; /* both NA or both DX */
    return 3;
  }

  /*
   * Worked All Germany (WAG) scoring.
   * DL source: DL-DL=1, DL-EU=3, DL-non-EU=5.
   * non-DL source: works DL=3, else=0.
   */
  if (contest_name_contains("WAG")) {
    const int src_is_dl = src_cty && strcmp(src_cty->prefix, "DL") == 0;
    const int dst_is_dl = dst_cty && strcmp(dst_cty->prefix, "DL") == 0;
    if (src_is_dl) {
      if (dst_is_dl) return 1;
      return same_cont ? 3 : 5;
    }
    return dst_is_dl ? 3 : 0;
  }

  /*
   * Oceania DX Contest scoring.
   * OC ↔ non-OC QSOs only; points are band-dependent.
   * non-OC to non-OC = 0.
   */
  if (contest_name_contains("OCEANIA")) {
    const int src_is_oc = src_cont[0] && strcmp(src_cont, "OC") == 0;
    const int dst_is_oc = dst_cont[0] && strcmp(dst_cont, "OC") == 0;
    if (src_is_oc == dst_is_oc) return 0;

    if (!band || !band[0]) return 1;
    if (strcmp(band, "160M") == 0 || strcmp(band, "160") == 0) return 20;
    if (strcmp(band, "80M")  == 0 || strcmp(band, "80")  == 0) return 10;
    if (strcmp(band, "40M")  == 0 || strcmp(band, "40")  == 0) return  5;
    if (strcmp(band, "15M")  == 0 || strcmp(band, "15")  == 0) return  2;
    if (strcmp(band, "10M")  == 0 || strcmp(band, "10")  == 0) return  3;
    return 1; /* 20m default */
  }

  /*
   * Russian DX Contest (RDXC) scoring (simplified).
   * Working Russia = 10 pts; same DXCC = 2 pts; same cont = 3 pts; diff = 5 pts.
   */
  if (contest_name_contains("RDXC")) {
    if (dst_cty) {
      if (strncmp(dst_cty->prefix, "UA", 2) == 0 ||
          strncmp(dst_cty->prefix, "R1", 2) == 0)
        return 10;
    }
    if (same_country) return 2;
    return same_cont ? 3 : 5;
  }

  /*
   * Holyland DX Contest (simplified).
   * 4X working others: 4X-4X=1, 4X-AS=2, 4X-else=8.
   * DX working 4X: 8 pts.
   * /MM: 4 pts.
   * Same cont: 2 pts. Diff cont: 4 pts.
   */
  if (contest_name_contains("HOLYLAND")) {
    if (call_is_maritime_mobile(call)) return 4;
    const int src_is_4x = src_cty && strcmp(src_cty->prefix, "4X") == 0;
    const int dst_is_4x = dst_cty && strcmp(dst_cty->prefix, "4X") == 0;
    if (src_is_4x) {
      if (dst_is_4x) return 1;
      if (dst_cont[0] && strcmp(dst_cont, "AS") == 0) return 2;
      return 8;
    }
    if (dst_is_4x) return 8;
    return same_cont ? 2 : 4;
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

