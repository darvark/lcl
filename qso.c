#include "qso.h"

#include "db.h"

#include <stdlib.h>

static int qso_debug_enabled(void) {
  const char *value = getenv("LOGGER_DEBUG");
  return value && value[0] && strcmp(value, "0") != 0;
}

QSO logbook[MAX_QSO];
int qso_count = 0;

/* ------------------------------------------------ */

/*
 * Validate a callsign for the QSO parser.
 *
 * @param call Callsign text.
 * @return 1 if the callsign is acceptable, otherwise 0.
 */
static int validate_callsign(const char *call) {
  if (!call)
    return 0;

  size_t len = strlen(call);

  if (len < 3 || len > 20)
    return 0;

  int has_digit = 0;

  for (size_t i = 0; i < len; i++) {
    char c = toupper((unsigned char)call[i]);

    if (isdigit((unsigned char)c))
      has_digit = 1;

    if (!(isalnum((unsigned char)c) || c == '/'))
      return 0;
  }

  return has_digit;
}

/* ------------------------------------------------ */

/*
 * Validate a frequency range for a QSO entry.
 *
 * @param freq Frequency in kHz.
 * @return 1 if the frequency is within range, otherwise 0.
 */
static int validate_frequency(int freq) {
  if (freq < 1000)
    return 0;

  if (freq > 500000)
    return 0;

  return 1;
}

/* ------------------------------------------------ */

/*
 * Validate the signal report field for a QSO entry.
 *
 * @param rst Signal report text.
 * @return 1 if the report is well formed, otherwise 0.
 */
static int validate_rst(const char *rst) {
  if (!rst)
    return 0;

  size_t len = strlen(rst);

  if (len < 2 || len > 3)
    return 0;

  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)rst[i]))
      return 0;
  }

  return 1;
}

/*
 * Copy comments text into the QSO record, filtering control characters.
 *
 * @param dst Destination comments buffer.
 * @param dst_size Destination buffer size.
 * @param src Source comments text.
 * @return Nothing.
 */
static void sanitize_comments(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size < 2)
    return;

  dst[0] = 0;
  if (!src)
    return;

  size_t out = 0;
  for (size_t i = 0; src[i] && out < dst_size - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 32 && c != 127)
      dst[out++] = (char)c;
  }

  dst[out] = 0;
}

/*
 * Copy free-text into one destination while filtering control characters.
 */
static void sanitize_text(char *dst, size_t dst_size, const char *src) {
  sanitize_comments(dst, dst_size, src);
}

/* ------------------------------------------------ */

/*
 * Map a frequency in kHz to a band label.
 *
 * @param freq Frequency in kHz.
 * @param band Destination buffer for the band label.
 * @return Nothing.
 */
void detect_band(int freq, char *band) {
  if (freq >= 1800 && freq <= 2000)
    strcpy(band, "160M");
  else if (freq >= 3500 && freq <= 4000)
    strcpy(band, "80M");
  else if (freq >= 7000 && freq <= 7300)
    strcpy(band, "40M");
  else if (freq >= 10100 && freq <= 10150)
    strcpy(band, "30M");
  else if (freq >= 14000 && freq <= 14350)
    strcpy(band, "20M");
  else if (freq >= 18068 && freq <= 18168)
    strcpy(band, "17M");
  else if (freq >= 21000 && freq <= 21450)
    strcpy(band, "15M");
  else if (freq >= 24890 && freq <= 24990)
    strcpy(band, "12M");
  else if (freq >= 28000 && freq <= 29700)
    strcpy(band, "10M");
  else if (freq >= 50000 && freq <= 54000)
    strcpy(band, "6M");
  else if (freq >= 144000 && freq <= 148000)
    strcpy(band, "2M");
  else
    strcpy(band, "?");
}

/* ------------------------------------------------ */

/*
 * Infer the operating mode from a frequency in kHz.
 *
 * @param freq Frequency in kHz.
 * @param mode Destination buffer for the mode label.
 * @return Nothing.
 */
void detect_mode(int freq, char *mode) {
  if (freq == 14074 || freq == 7074 || freq == 3573 || freq == 21074) {
    strcpy(mode, "FT8");
  } else if (freq == 14080 || freq == 7047 || freq == 3575) {
    strcpy(mode, "FT4");
  } else if (freq >= 14070 && freq <= 14071) {
    strcpy(mode, "PSK31");
  } else if (freq >= 14080 && freq <= 14099) {
    strcpy(mode, "RTTY");
  } else if (freq < 14100) {
    strcpy(mode, "CW");
  } else {
    strcpy(mode, "SSB");
  }
}

/* ------------------------------------------------ */

/*
 * Load the current logbook from the database into memory.
 *
 * @return Nothing.
 */
void qso_init(void) {
  memset(logbook, 0, sizeof(logbook));
  qso_count = 0;

  if (db_init() != 0)
    return;

  int loaded = 0;
  if (db_load_qsos(logbook, MAX_QSO, NULL, &loaded) == 0)
    qso_count = loaded;
}

/* ------------------------------------------------ */

/*
 * Fill a QSO record with the current UTC date and time.
 *
 * @param q QSO record to update.
 * @return Nothing.
 */
static void fill_utc(QSO *q) {
  time_t t = time(NULL);

  struct tm *utc = gmtime(&t);

  strftime(q->date, sizeof(q->date), "%Y%m%d", utc);
  strftime(q->utc, sizeof(q->utc), "%H%M", utc);
}

/* ------------------------------------------------ */

/*
 * Parse, validate, and store one QSO entry.
 *
 * @param line Input line containing call, frequency, and RST fields.
 * @param status Destination buffer for a status message.
 * @param status_size Size of the status buffer in bytes.
 * @return Index of the inserted QSO, or -1 on failure.
 */
int qso_add(const char *line, char *status, size_t status_size) {
  if (!line)
    return -1;

  char buf[256];
  strncpy(buf, line, sizeof(buf));
  buf[sizeof(buf) - 1] = 0;

  char *call = strtok(buf, " ");
  char *freq = strtok(NULL, " ");
  char *rst = strtok(NULL, " ");
  (void)strtok(NULL, " ");

  if (!call || !freq || !rst) {
    snprintf(status, status_size, "Bad format");
    return -1;
  }

  for (int i = 0; call[i]; i++)
    call[i] = toupper((unsigned char)call[i]);

  char *p = strchr(call, '/');
  if (p)
    *p = 0;

  if (!validate_callsign(call)) {
    snprintf(status, status_size, "Invalid callsign");
    return -1;
  }

  int f = atoi(freq);

  if (qso_count >= MAX_QSO) {
    snprintf(status, status_size, "Logbook full");
    return -1;
  }

  QSO q;
  memset(&q, 0, sizeof(q));

  strcpy(q.call, call);
  strcpy(q.rst, rst);
  q.comments[0] = 0;
  q.exchange_sent[0] = 0;
  q.exchange_recv[0] = 0;
  q.operator_mode[0] = 0;
  q.contest_id[0] = 0;
  q.radio_nr = 1;
  q.points = 1;
  q.freq = f;

  detect_band(f, q.band);
  detect_mode(f, q.mode);

  fill_utc(&q);

  const CtyEntry *cty = cty_lookup(call);

  if (cty) {
    strncpy(q.country, cty->country, sizeof(q.country) - 1);
    q.cq_zone = cty->cq_zone;
    q.itu_zone = cty->itu_zone;
  } else {
    strcpy(q.country, "UNKNOWN");
  }

  long long db_id = 0;
  if (db_insert_qso(&q, &db_id) == 0)
    q.db_id = db_id;

  logbook[qso_count] = q;
  qso_count++;

  snprintf(status, status_size, "QSO OK");
  return qso_count - 1;
}

/* ------------------------------------------------ */

/*
 * Parse, validate, and store one QSO from split entry fields.
 *
 * @param call Callsign field.
 * @param rst Signal report field.
 * @param comments Free-form comments field.
 * @param status Destination buffer for a status message.
 * @param status_size Size of the status buffer in bytes.
 * @return Index of the inserted QSO, or -1 on failure.
 */
int qso_add_fields(const char *call, int freq_khz, const char *rst,
                   const char *mode, const char *comments, char *status,
                   size_t status_size) {
  if (!call || !rst) {
    if (status && status_size)
      snprintf(status, status_size, "Bad format");
    return -1;
  }

  char call_buf[32] = {0};
  snprintf(call_buf, sizeof(call_buf), "%s", call);

  for (int i = 0; call_buf[i]; i++)
    call_buf[i] = toupper((unsigned char)call_buf[i]);

  char *slash = strchr(call_buf, '/');
  if (slash)
    *slash = 0;

  if (!validate_callsign(call_buf)) {
    snprintf(status, status_size, "Invalid callsign");
    return -1;
  }

  if (!validate_rst(rst)) {
    snprintf(status, status_size, "Invalid RST");
    return -1;
  }

  if (!validate_frequency(freq_khz)) {
    snprintf(status, status_size, "Invalid frequency");
    return -1;
  }

  if (qso_count >= MAX_QSO) {
    snprintf(status, status_size, "Logbook full");
    return -1;
  }

  QSO q;
  memset(&q, 0, sizeof(q));

  strcpy(q.call, call_buf);
  strcpy(q.rst, rst);
  sanitize_comments(q.comments, sizeof(q.comments), comments);
  q.exchange_sent[0] = 0;
  q.exchange_recv[0] = 0;
  q.operator_mode[0] = 0;
  q.contest_id[0] = 0;
  q.radio_nr = 1;
  q.points = 1;

  q.freq = freq_khz;

  detect_band(q.freq, q.band);
  if (mode && mode[0]) {
    snprintf(q.mode, sizeof(q.mode), "%s", mode);
  } else {
    detect_mode(q.freq, q.mode);
  }

  fill_utc(&q);

  const CtyEntry *cty = cty_lookup(call_buf);
  if (cty) {
    strncpy(q.country, cty->country, sizeof(q.country) - 1);
    q.cq_zone = cty->cq_zone;
    q.itu_zone = cty->itu_zone;
  } else {
    strcpy(q.country, "UNKNOWN");
  }

  long long db_id = 0;
  if (db_insert_qso(&q, &db_id) == 0)
    q.db_id = db_id;

  logbook[qso_count] = q;
  qso_count++;

  snprintf(status, status_size, "QSO OK");
  return qso_count - 1;
}

int qso_add_contest_fields(const char *call, int freq_khz, const char *rst,
                           const char *mode, const char *comments,
                           const char *exchange_sent,
                           const char *exchange_recv,
                           const char *operator_mode,
                           const char *contest_id, int radio_nr, int points,
                           char *status, size_t status_size) {
  int idx = qso_add_fields(call, freq_khz, rst, mode, comments, status,
                           status_size);
  if (idx < 0)
    return idx;

  QSO *q = &logbook[idx];
  sanitize_text(q->exchange_sent, sizeof(q->exchange_sent), exchange_sent);
  sanitize_text(q->exchange_recv, sizeof(q->exchange_recv), exchange_recv);
  sanitize_text(q->operator_mode, sizeof(q->operator_mode), operator_mode);
  sanitize_text(q->contest_id, sizeof(q->contest_id), contest_id);

  if (qso_debug_enabled()) {
    fprintf(stderr,
            "[debug] qso_add_contest_fields: call=%s sent=%s recv=%s mode=%s contest=%s\n",
            call ? call : "<null>",
            q->exchange_sent[0] ? q->exchange_sent : "<empty>",
            q->exchange_recv[0] ? q->exchange_recv : "<empty>",
            mode ? mode : "<null>",
            contest_id ? contest_id : "<null>");
  }

  if (radio_nr < 1)
    radio_nr = 1;
  q->radio_nr = radio_nr;

  if (points < 0)
    points = 0;
  q->points = points;

  db_update_qso_contest_fields(q->db_id, q->exchange_sent, q->exchange_recv,
                               q->operator_mode, q->contest_id, q->radio_nr,
                               q->points);

  snprintf(status, status_size, "QSO OK TX:%s RX:%s",
           q->exchange_sent[0] ? q->exchange_sent : "-",
           q->exchange_recv[0] ? q->exchange_recv : "-");
  return idx;
}

/* ------------------------------------------------ */

/*
 * Toggle the invalid flag for a QSO row.
 *
 * @param index Zero-based index into the in-memory logbook.
 * @return Nothing.
 */
void qso_mark_invalid(int index) {
  if (index < 0)
    return;

  if (index >= qso_count)
    return;

  logbook[index].invalid = !logbook[index].invalid;
  db_update_qso_invalid(logbook[index].db_id, logbook[index].invalid);
}