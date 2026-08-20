static int resolve_runtime_support_path(const char *path, char *resolved,
                                        size_t resolved_size) {
  if (!path || !path[0] || !resolved || resolved_size < 2)
    return -1;

  resolved[0] = 0;

  const char *candidates[] = {
      path,
      "logger.conf",
      "wl_cty.dat",
      "./logger.conf",
      "./wl_cty.dat",
      "../logger.conf",
      "../wl_cty.dat",
      LOGGER_BUILD_DIR "/logger.conf",
      LOGGER_BUILD_DIR "/wl_cty.dat",
      LOGGER_SOURCE_DIR "/logger.conf",
      LOGGER_SOURCE_DIR "/wl_cty.dat",
      NULL};

  for (size_t i = 0; candidates[i]; i++) {
    const char *candidate = candidates[i];
    if ((strcmp(path, "logger.conf") == 0 && strstr(candidate, "logger.conf")) ||
        (strcmp(path, "wl_cty.dat") == 0 && strstr(candidate, "wl_cty.dat")) ||
        strcmp(candidate, path) == 0) {
      if (access(candidate, R_OK) == 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
        return 0;
      }
    }
  }

  return -1;
}

static void render_sync_status(void) {
  int pending = 0;
  int failed = 0;

  if (db_sync_get_pending_outbox_count(&pending) != 0 ||
      db_sync_get_failed_outbox_count(&failed) != 0) {
    snprintf(status_text, sizeof(status_text), "SYNC status unavailable");
    return;
  }

  NetSyncStatus sync_state;
  memset(&sync_state, 0, sizeof(sync_state));
  net_sync_get_status(&sync_state);

  snprintf(status_text, sizeof(status_text),
           "SYNC pending=%d failed=%d connected=%d tls=%d retry=%d fs=%d",
           pending, failed, sync_state.connected ? 1 : 0,
           sync_state.tls_enabled ? 1 : 0, sync_state.reconnect_count,
           sync_state.failure_streak);
  snprintf(info_text, sizeof(info_text),
           "station=%.20s seq=%lld hb=%.20s ok=%.20s err=%.32s",
           sync_state.station_id[0] ? sync_state.station_id : "-",
           sync_state.last_pulled_global_seq,
           sync_state.last_heartbeat_utc[0] ? sync_state.last_heartbeat_utc : "-",
           sync_state.last_success_utc[0] ? sync_state.last_success_utc : "-",
           sync_state.last_error[0] ? sync_state.last_error : "-");
}

static int net_polling_running = 0;
static pthread_t net_poll_thread;

static void *net_poll_worker(void *arg) {
  (void)arg;

  while (net_polling_running) {
    (void)db_sync_expire_serial_reservations();
    (void)net_sync_poll_once();

    int sleep_ms = config.net_sync_interval_ms;
    if (sleep_ms < 100)
      sleep_ms = 100;
    if (sleep_ms > 60000)
      sleep_ms = 60000;

    usleep((useconds_t)sleep_ms * 1000U);
  }

  return NULL;
}

static void start_net_poll_loop(void) {
  if (net_polling_running)
    return;

  net_polling_running = 1;
  if (pthread_create(&net_poll_thread, NULL, net_poll_worker, NULL) != 0)
    net_polling_running = 0;
}

static void stop_net_poll_loop(void) {
  if (!net_polling_running)
    return;

  net_polling_running = 0;
  pthread_join(net_poll_thread, NULL);
}

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

  if (strcmp(command, "sync") == 0 || strcmp(command, "syncstatus") == 0) {
    if (strcmp(command, "sync") == 0)
      (void)net_sync_poll_once();
    render_sync_status();
    return 1;
  }

  if (strcmp(command, "net") == 0) {
    if (strcmp(arg, "on") == 0) {
      config.net_enabled = 1;
      if (net_sync_start() == 0) {
        start_net_poll_loop();
        snprintf(status_text, sizeof(status_text), "NET enabled");
      } else {
        snprintf(status_text, sizeof(status_text), "NET start failed");
      }
      return 1;
    }

    if (strcmp(arg, "off") == 0) {
      config.net_enabled = 0;
      stop_net_poll_loop();
      net_sync_stop();
      snprintf(status_text, sizeof(status_text), "NET disabled");
      return 1;
    }

    if (strcmp(arg, "status") == 0) {
      render_sync_status();
      return 1;
    }

    if (strcmp(arg, "sync") == 0) {
      (void)net_sync_poll_once();
      render_sync_status();
      return 1;
    }

    if (strncmp(arg, "role ", 5) == 0) {
      const char *role = arg + 5;
      if (strcmp(role, "client") == 0 || strcmp(role, "server") == 0) {
        if (config.net_enabled) {
          stop_net_poll_loop();
          net_sync_stop();
        }
        snprintf(config.net_role, sizeof(config.net_role), "%s", role);
        if (config.net_enabled) {
          if (net_sync_start() == 0) {
            start_net_poll_loop();
            snprintf(status_text, sizeof(status_text), "NET role=%s", role);
          } else {
            snprintf(status_text, sizeof(status_text),
                     "NET role switch failed");
          }
        } else {
          snprintf(status_text, sizeof(status_text), "NET role=%s", role);
        }
      } else {
        snprintf(status_text, sizeof(status_text),
                 "Usage: net role client|server");
      }
      return 1;
    }

    snprintf(status_text, sizeof(status_text),
             "Usage: net on|off|status|sync|role client|server");
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
               "%s", "EXCH");
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
           "EXCH");
  stats_set_contest_definition(&active_contest_def);
  clear_callsign_suggestion();

  char config_path[512] = {0};
  const char *runtime_logger_conf =
      resolve_runtime_support_path("logger.conf", config_path,
                                   sizeof(config_path)) == 0
          ? config_path
          : "logger.conf";

  if (config_load(runtime_logger_conf) != 0)
    fprintf(stderr, "Cannot load logger.conf\n");
  else if (app_debug_enabled) {
    fprintf(stderr,
            "[debug] app_controller_init: logger.conf loaded, CONTEST_DEF_FILE=%s\n",
            config.contest_definition_path[0] ? config.contest_definition_path : "<empty>");
  }

  if (load_configured_contest_definition() != 0 && app_debug_enabled) {
    fprintf(stderr,
            "[debug] app_controller_init: contest definition load failed for %s\n",
            config.contest_definition_path[0] ? config.contest_definition_path : "<empty>");
  } else if (app_debug_enabled) {
    fprintf(stderr,
            "[debug] app_controller_init: contest_definition_loaded=%d contest_name=%s\n",
            contest_definition_loaded,
            active_contest_def.name[0] ? active_contest_def.name : "<unset>");
  }

  stats_set_contest_definition(&active_contest_def);

  char cty_path[512] = {0};
  const char *runtime_cty_path =
      resolve_runtime_support_path("wl_cty.dat", cty_path,
                                   sizeof(cty_path)) == 0
          ? cty_path
          : "wl_cty.dat";

  cty_load(runtime_cty_path);
  qso_init();
  qtc_init();
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

  if (config.net_enabled) {
    if (net_sync_start() != 0) {
      snprintf(status_text, sizeof(status_text), "Net sync start failed");
      update_display_info();
      return -1;
    }

    start_net_poll_loop();
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
  stop_net_poll_loop();

  if (config.net_enabled)
    net_sync_stop();

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

  out->qtc_enabled = app_controller_qtc_enabled() != 0;
  out->qtc_records_total = qtc_total_records();
  out->contest_name = (contest_definition_loaded && active_contest_def.name[0])
                          ? active_contest_def.name
                          : "";
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
    char cty_path[512] = {0};
    const char *runtime_cty_path =
        resolve_runtime_support_path("wl_cty.dat", cty_path,
                                     sizeof(cty_path)) == 0
            ? cty_path
            : "wl_cty.dat";
    int loaded = cty_load(runtime_cty_path);
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
  char cty_path[512] = {0};
  const char *runtime_cty_path =
      resolve_runtime_support_path("wl_cty.dat", cty_path,
                                   sizeof(cty_path)) == 0
          ? cty_path
          : "wl_cty.dat";
  const int download_ok = cty_download_latest(runtime_cty_path) == 0;
  app_controller_complete_cty_update(download_ok);
}

/*
 * Handle a translated key code and update shared controller state.
 *
 * @param key One of the APP_KEY_* values.
 * @return The controller event to propagate to the UI.
 */
bool app_controller_export_prompt_active(void) {
  return export_prompt_mode;
}

const char *app_controller_export_filename_text(void) {
  return input_buffer;
}

void app_controller_set_export_filename_text(const char *text) {
  if (!text) {
    input_buffer[0] = 0;
    input_len = 0;
    return;
  }

  const size_t max_len = sizeof(input_buffer) - 1;
  const size_t copy_len = strnlen(text, max_len + 1);
  memmove(input_buffer, text, copy_len < max_len ? copy_len : max_len);
  input_buffer[copy_len < max_len ? copy_len : max_len] = 0;
  input_len = (int)(copy_len < max_len ? copy_len : max_len);
}

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

  if (key == APP_KEY_CTRL_L) {
    if (!app_controller_qtc_enabled()) {
      snprintf(status_text, sizeof(status_text),
               "QTC not enabled for current contest");
      return APP_CTRL_EVENT_NONE;
    }
    return APP_CTRL_EVENT_OPEN_QTC_WINDOW;
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
          app_controller_set_active_frequency_khz(freq_khz);
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
          char exchange_recv_storage[32] = {0};
          build_exchange_sent(exchange_sent, sizeof(exchange_sent));
          const int qso_points = resolve_contest_points(
            mode, qso_band, country, entry_call, active_contest_def.cabrillo_name);

          const char *qso_rst = contest_definition_loaded
                                    ? (strcmp(mode, "CW") == 0 ? "599" : "59")
                                    : entry_exchange;
          const char *qso_comments = contest_definition_loaded ? "" : entry_comments;
          const char *entered_exchange = contest_definition_loaded ? entry_exchange : "";
          if (entered_exchange && entered_exchange[0]) {
            snprintf(exchange_recv_storage, sizeof(exchange_recv_storage), "%s",
                     entered_exchange);
          }

          int idx = qso_add_contest_fields(
              entry_call, qso_freq_khz, qso_rst, mode, qso_comments,
              exchange_sent,
              exchange_recv_storage[0] ? exchange_recv_storage : entered_exchange,
              active_operator_mode_text(),
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

/* ------------------------------------------------------------------ */
/* QTC API                                                              */
/* ------------------------------------------------------------------ */

