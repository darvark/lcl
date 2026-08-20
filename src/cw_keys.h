#ifndef CW_KEYS_H
#define CW_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CW_KEY_COUNT 10

typedef struct {
  char run[CW_KEY_COUNT][128];
  char sp[CW_KEY_COUNT][128];
} CwKeyMap;

/*
 * CW messages used during a QTC exchange.
 *
 * preamble  – sent once before the bundle, e.g. "QTC {QTC_NR}/{QTC_COUNT}"
 * record    – sent for each QSO record, e.g. "{QTC_TIME} {QTC_CALL} {QTC_EXCH}"
 * confirm   – sent after each received acknowledgement, e.g. "QSL"
 * request   – used to request QTC from a DX station, e.g. "QRV"
 */
typedef struct {
  char preamble[128];
  char record[128];
  char confirm[128];
  char request[128];
} CwQtcMap;

/* Load RUN and S&P sections from an INI file into map. Returns 0 or -1. */
int cw_keys_load(CwKeyMap *map, const char *filename);

/* Load [QTC] section from an INI file into qtc_map. Returns 0 or -1. */
int cw_qtc_load(CwQtcMap *qtc_map, const char *filename);

/* Expand {MYCALL}, {RST}, {EXCH}, {HISCALL}/{CALL} placeholders in tpl into out. */
void cw_keys_expand(const char *tpl, const char *mycall, const char *rst,
                    const char *exch, const char *hiscall,
                    char *out, size_t out_size);

/*
 * Expand QTC-specific placeholders in a template.
 *
 * Recognised tokens:
 *   {QTC_NR}    – bundle number
 *   {QTC_COUNT} – number of records in bundle
 *   {QTC_TIME}  – QSO time (HHMM)
 *   {QTC_CALL}  – QSO callsign
 *   {QTC_EXCH}  – QSO exchange (serial)
 *   {MYCALL}    – local station callsign
 *   {HISCALL}   – remote station callsign
 */
void cw_qtc_expand(const char *tpl, const char *mycall, const char *hiscall,
                   int bundle_nr, int bundle_count,
                   const char *qtc_time, const char *qtc_call,
                   const char *qtc_exch,
                   char *out, size_t out_size);

/* Return the raw template for fn_nr (1-based) in RUN (is_run=1) or S&P. */
const char *cw_keys_get(const CwKeyMap *map, int fn_nr, int is_run);

#ifdef __cplusplus
}
#endif

#endif
