static int resolve_contest_definition_path(const char *path, char *resolved,
                                          size_t resolved_size);
static int restore_current_log_contest_definition(void);

int app_controller_get_active_frequency_khz(void) {
  return resolve_qso_frequency_khz();
}

int app_controller_set_active_frequency_khz(int freq_khz) {
  if (!is_valid_frequency_khz(freq_khz)) {
    snprintf(status_text, sizeof(status_text), "Invalid frequency");
    return -1;
  }

  const int radio_idx = radio_index_from_nr(active_radio_nr);
  const int slot = cat_slot_for_radio(radio_idx);
  const CatVfo vfo = cat_vfo_for_radio(radio_idx);

  manual_entry_freq_khz[radio_idx] = freq_khz;

  if (cat_is_connected_slot(slot)) {
    if (config.contest_technique == CONTEST_TECH_SO2V)
      cat_set_active_vfo_slot(slot, vfo);

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

  return 0;
}

void app_controller_set_active_call(const char *call_text) {
  const int idx = active_radio_index();
  char *entry_call = entry_call_for_idx(idx);

  if (!call_text)
    call_text = "";

  snprintf(entry_call, sizeof(entry_call_by_radio[0]), "%s", call_text);
  update_dxcc_from_input(entry_call);
  refresh_callsign_suggestion(entry_call);
  update_composed_input_line();
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

  if (db_clear_logbook() != 0) {
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
  restore_current_log_contest_definition();
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
  restore_current_log_contest_definition();
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

static int export_cabrillo_with_filename(const char *cab_file);

static void derive_cabrillo_filename_from_adif(const char *adif_file,
                                              char *cabrillo_file,
                                              size_t cabrillo_size) {
  if (!adif_file || !adif_file[0] || !cabrillo_file || cabrillo_size == 0)
    return;

  const char *ext = strrchr(adif_file, '.');
  if (ext && (strcasecmp(ext, ".adi") == 0 || strcasecmp(ext, ".adif") == 0)) {
    const size_t prefix_len = (size_t)(ext - adif_file);
    if (prefix_len + 5 < cabrillo_size) {
      memcpy(cabrillo_file, adif_file, prefix_len);
      cabrillo_file[prefix_len] = 0;
      snprintf(cabrillo_file + prefix_len, cabrillo_size - prefix_len, ".cbr");
      return;
    }
  }

  snprintf(cabrillo_file, cabrillo_size, "%s", adif_file);
  if (strrchr(cabrillo_file, '.') == NULL) {
    const size_t len = strlen(cabrillo_file);
    if (len + 4 < cabrillo_size) {
      snprintf(cabrillo_file + len, cabrillo_size - len, ".cbr");
      return;
    }
  }

  char *dot = strrchr(cabrillo_file, '.');
  if (dot) {
    *dot = 0;
    const size_t len = strlen(cabrillo_file);
    if (len + 4 < cabrillo_size) {
      snprintf(cabrillo_file + len, cabrillo_size - len, ".cbr");
    }
  }
}

/*
 * Export the current logbook to ADIF and Cabrillo using an explicit ADIF filename.
 *
 * @param adif_file Destination ADIF filename.
 * @return 0 on success, or -1 on failure.
 */
static int export_with_adif_filename(const char *adif_file) {
  const char *default_csv = "log.csv";
  char cabrillo_file[512] = {0};

  if (!adif_file || !adif_file[0])
    return -1;

  if (export_csv(default_csv) != 0)
    return -1;

  if (export_adif(adif_file) != 0)
    return -1;

  derive_cabrillo_filename_from_adif(adif_file, cabrillo_file, sizeof(cabrillo_file));
  if (export_cabrillo_with_filename(cabrillo_file) != 0)
    return -1;

  snprintf(status_text, sizeof(status_text),
           "Exported ADIF: %.80s | Cabrillo: %.60s",
           adif_file ? adif_file : "",
           cabrillo_file ? cabrillo_file : "");

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

  /* Use the QTC-aware exporter so QTC: lines are included when applicable. */
  if (export_cabrillo_with_qtc(cab_file, &active_contest_def, config.station_call) != 0)
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
           "EXCH");
  snprintf(config.contest_definition_path, sizeof(config.contest_definition_path), "%s",
           path);
  config_save("logger.conf");
  db_set_current_logbook_contest_path(path);
  stats_set_contest_definition(&active_contest_def);
  stats_update();
  if (app_debug_enabled) {
    fprintf(stderr,
            "[debug] load_contest_definition_file: loaded=%s field_count=%d exchange_label=%s\n",
            path,
            active_contest_def.field_count,
            contest_exchange_label_text);
  }
  snprintf(status_text, sizeof(status_text), "Contest loaded: %s",
           active_contest_def.name);
  return 0;
}

static int restore_current_log_contest_definition(void) {
  char saved_path[512] = {0};
  if (db_get_current_logbook_contest_path(saved_path, sizeof(saved_path)) != 0 || !saved_path[0])
    return 0;

  contest_definition_loaded = 0;
  contest_definition_init_defaults(&active_contest_def);
  snprintf(contest_exchange_label_text, sizeof(contest_exchange_label_text), "%s",
           "EXCH");
  clear_entry_fields();

  char resolved[512] = {0};
  if (resolve_contest_definition_path(saved_path, resolved, sizeof(resolved)) == 0)
    return load_contest_definition_file(resolved);

  return load_contest_definition_file(saved_path);
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

  const char *prefixes[] = {"", "./", "../", "../../", "../../../",
                           "../../../../", "../../../../../"};
  const char *roots[] = {LOGGER_SOURCE_DIR, LOGGER_BUILD_DIR,
                         LOGGER_BUILD_DIR "/..", NULL};
  char candidate[1024];
  char cwd[1024] = {0};

  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    snprintf(candidate, sizeof(candidate), "%s%s", prefixes[i], path);
    if (access(candidate, R_OK) == 0) {
      snprintf(resolved, resolved_size, "%s", candidate);
      return 0;
    }
  }

  for (size_t root_idx = 0; roots[root_idx]; root_idx++) {
    snprintf(candidate, sizeof(candidate), "%s/%s", roots[root_idx], path);
    if (access(candidate, R_OK) == 0) {
      snprintf(resolved, resolved_size, "%s", candidate);
      return 0;
    }
  }

  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
      snprintf(candidate, sizeof(candidate), "%s/%s%s", cwd, prefixes[i], path);
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }
    }

    char trial[1024];
    snprintf(trial, sizeof(trial), "%s", cwd);
    for (int depth = 0; depth < 12; depth++) {
      snprintf(candidate, sizeof(candidate), "%s/%s", trial, path);
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }

      char *slash = strrchr(trial, '/');
      if (!slash || strcmp(trial, "/") == 0)
        break;
      if (slash == trial)
        *slash = 0;
      else
        *slash = 0;
      if (trial[0] == 0)
        snprintf(trial, sizeof(trial), "/");
    }
  }

  char direct_path[1024];
  snprintf(direct_path, sizeof(direct_path), "%s", path);
  const char *contest_defs_prefix = "contest_defs/";
  if (strncmp(direct_path, contest_defs_prefix, strlen(contest_defs_prefix)) != 0) {
    snprintf(direct_path, sizeof(direct_path), "%s%s", contest_defs_prefix, path);
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
      snprintf(candidate, sizeof(candidate), "%s%s", prefixes[i], direct_path);
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }
    }

    for (size_t root_idx = 0; roots[root_idx]; root_idx++) {
      snprintf(candidate, sizeof(candidate), "%s/%s", roots[root_idx],
               direct_path);
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }
    }

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(candidate, sizeof(candidate), "%s/%s%s", cwd, prefixes[i], direct_path);
        if (access(candidate, R_OK) == 0) {
          snprintf(resolved, resolved_size, "%s", candidate);
          return 0;
        }
      }

      char trial[1024];
      snprintf(trial, sizeof(trial), "%s", cwd);
      for (int depth = 0; depth < 12; depth++) {
        snprintf(candidate, sizeof(candidate), "%s/%s", trial, direct_path);
        if (access(candidate, R_OK) == 0) {
          snprintf(resolved, resolved_size, "%s", candidate);
          return 0;
        }

        char *slash = strrchr(trial, '/');
        if (!slash || strcmp(trial, "/") == 0)
          break;
        *slash = 0;
        if (trial[0] == 0)
          snprintf(trial, sizeof(trial), "/");
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
                                      sizeof(resolved)) != 0) {
    /* Default repo config may point to a local contest.conf that simply does not
     * exist yet. In that case we should keep the app operating in non-contest
     * mode instead of treating startup as a fatal error. */
    if (strcmp(config.contest_definition_path, "contest.conf") == 0)
      return 0;
    return -1;
  }

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

  if (export_with_adif_filename(adif_file) != 0)
    return -1;

  char cabrillo_file[512] = {0};
  derive_cabrillo_filename_from_adif(adif_file, cabrillo_file, sizeof(cabrillo_file));

  if (export_cabrillo_with_filename(cabrillo_file) != 0)
    return -1;

  snprintf(status_text, sizeof(status_text),
           "Exported ADIF: %.80s | Cabrillo: %.60s",
           adif_file ? adif_file : "",
           cabrillo_file ? cabrillo_file : "");

  return 0;
}

/*
 * Handle a typed command line entered into the controller.
 *
 * @param cmd Raw command text.
 * @return 1 if the input was treated as a command, otherwise 0.
 */
