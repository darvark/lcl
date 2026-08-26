#ifndef QSO_H
#define QSO_H

#include "cty.h"
#include "globals.h"

#define MAX_QSO 1000

typedef struct {
  long long db_id;

  char qso_uid[40];
  char origin_station_id[32];
  long long origin_station_seq;
  int version;
  char last_modified_utc[32];

  char date[9];
  char utc[5];

  char call[32];

  int freq;

  char band[8];
  char mode[16];
  char rst[8];
  char comments[128];

  char exchange_sent[32];
  char exchange_recv[32];
  char operator_mode[8];
  char contest_id[64];
  int radio_nr;
  int points;

  char country[64];
  int cq_zone;
  int itu_zone;

  bool invalid;

} QSO;

extern QSO logbook[MAX_QSO];
extern int qso_count;

/*
 * Load the current logbook from the database into memory.
 *
 * @return Nothing.
 */
void qso_init(void);

/*
 * Parse, validate, and store one QSO entry.
 *
 * @param line Input line containing call, frequency, and RST fields.
 * @param status Destination buffer for a status message.
 * @param status_size Size of the status buffer in bytes.
 * @return Index of the inserted QSO, or -1 on failure.
 */
int qso_add(const char *line, char *status, size_t status_size);

/*
 * Parse, validate, and store one QSO from split entry fields.
 *
 * @param call Callsign field.
 * @param freq_khz Frequency in kHz.
 * @param rst Signal report field.
 * @param mode Optional mode label. If empty, the mode is derived from frequency.
 * @param comments Free-form comments field.
 * @param status Destination buffer for a status message.
 * @param status_size Size of the status buffer in bytes.
 * @return Index of the inserted QSO, or -1 on failure.
 */
int qso_add_fields(const char *call, int freq_khz, const char *rst,
                   const char *mode, const char *comments, char *status,
                   size_t status_size);

/*
 * Parse, validate, and store one contest QSO from split entry fields.
 *
 * @param exchange_sent Sent exchange field (for example serial).
 * @param exchange_recv Received exchange field.
 * @param operator_mode Operator mode label (RUN or S&P).
 * @param contest_id Contest identifier used for Cabrillo headers.
 * @param radio_nr Radio number used in SO2R logs.
 * @param points Claimed points for this QSO.
 */
int qso_add_contest_fields(const char *call, int freq_khz, const char *rst,
                           const char *mode, const char *comments,
                           const char *exchange_sent,
                           const char *exchange_recv,
                           const char *operator_mode,
                           const char *contest_id, int radio_nr, int points,
                           char *status, size_t status_size);

/*
 * Toggle the invalid flag for a QSO row.
 *
 * @param index Zero-based index into the in-memory logbook.
 * @return Nothing.
 */
void qso_mark_invalid(int index);

/*
 * Map a frequency in kHz to a band label.
 *
 * @param freq Frequency in kHz.
 * @param band Destination buffer for the band label.
 * @return Nothing.
 */
void detect_band(int freq, char *band);

/*
 * Infer the operating mode from a frequency in kHz.
 *
 * @param freq Frequency in kHz.
 * @param mode Destination buffer for the mode label.
 * @return Nothing.
 */
void detect_mode(int freq, char *mode);

#endif
