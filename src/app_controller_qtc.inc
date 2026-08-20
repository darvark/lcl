int app_controller_qtc_enabled(void) {
  if (!contest_definition_loaded)
    return logbook_has_qtc_capable_contest_hint();

  const char *side = active_contest_def.qtc_sender_side;
  return side[0] && strcmp(side, "NONE") != 0;
}

int app_controller_qtc_can_send(void) {
  if (!app_controller_qtc_enabled())
    return 0;

  const char *side = active_contest_def.qtc_sender_side;
  if (strcmp(side, "BOTH") == 0)
    return 1;

  /* Determine own continent from CTY. */
  const CtyEntry *own = cty_lookup(config.station_call);
  if (!own)
    return 0;

  const int own_is_eu = own->continent[0] &&
                        strcmp(own->continent, "EU") == 0;

  if (strcmp(side, "EU") == 0)
    return own_is_eu;

  if (strcmp(side, "DX") == 0)
    return !own_is_eu;

  return 0;
}

static int qtc_call_looks_valid(const char *call) {
  if (!call || !call[0])
    return 0;

  size_t len = strlen(call);
  if (len < 3 || len > 20)
    return 0;

  int has_digit = 0;
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)call[i];
    if (isdigit(c))
      has_digit = 1;
    if (!(isalnum(c) || c == '/'))
      return 0;
  }

  return has_digit;
}

int app_controller_qtc_get_sendable(QTCRecord *out, int max_records) {
  if (!out || max_records <= 0)
    return 0;

  int count = 0;

  /*
   * Walk the logbook backwards (newest first) and collect QSOs that have
   * not yet been included in any sent QTC bundle.
   */
  for (int i = qso_count - 1; i >= 0 && count < max_records; i--) {
    const QSO *q = &logbook[i];

    if (q->invalid)
      continue;

    if (!qtc_call_looks_valid(q->call))
      continue;

    if (qtc_qso_already_sent(q->call, q->date, q->utc))
      continue;

    const char *qtc_exch = q->exchange_recv[0] ? q->exchange_recv
                                                : q->exchange_sent;

    QTCRecord *r = &out[count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->date, sizeof(r->date), "%s", q->date);
    snprintf(r->time, sizeof(r->time), "%s", q->utc);
    snprintf(r->call, sizeof(r->call), "%s", q->call);
    snprintf(r->exch, sizeof(r->exch), "%s", qtc_exch);
  }

  return count;
}

int app_controller_qtc_submit(const QTCBundle *bundle, char *status,
                              size_t status_size) {
  if (!bundle) {
    if (status && status_size > 0)
      snprintf(status, status_size, "NULL bundle");
    return -1;
  }

  char err[128] = {0};
  if (qtc_bundle_validate(bundle, err, sizeof(err)) != 0) {
    if (status && status_size > 0)
      snprintf(status, status_size, "QTC invalid: %s", err);
    return -1;
  }

  int idx = qtc_add_bundle(bundle);
  if (idx < 0) {
    if (status && status_size > 0)
      snprintf(status, status_size, "QTC save failed");
    return -1;
  }

  stats_update();

  if (status && status_size > 0) {
    snprintf(status, status_size, "QTC %d/%d saved (%d records)",
             bundle->bundle_nr, bundle->record_count, bundle->record_count);
  }

  return 0;
}

void app_controller_qtc_get_receiver_call(char *out, size_t out_size) {
  if (!out || out_size < 2)
    return;

  out[0] = 0;
  const int idx = active_radio_index();
  const char *call = entry_call_for_idx(idx);
  if (call && call[0])
    snprintf(out, out_size, "%s", call);
}
