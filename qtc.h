#ifndef QTC_H
#define QTC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum QSO records in one QTC bundle (WAE rule: max 10). */
#define QTC_MAX_RECORDS_PER_BUNDLE 10

/* Maximum number of QTC bundles held in memory. */
#define QTC_MAX_BUNDLES 500

/*
 * One QSO item transmitted inside a QTC bundle.
 *
 * Each record carries the date, time, callsign, and exchange of a previously
 * worked QSO, formatted according to WAE exchange rules.
 */
typedef struct {
    char date[9];   /* YYYYMMDD */
    char time[5];   /* HHMM     */
    char call[32];
    char exch[32];  /* serial number or other received exchange */
} QTCRecord;

/*
 * A complete QTC bundle exchanged between two stations.
 *
 * In WAE, one bundle carries up to 10 QSO records.  A bundle is either
 * sent by the local station (sent=1) or received from the other station
 * (sent=0).
 */
typedef struct {
    long long db_id;
    char sender_call[32];    /* callsign of the transmitting station */
    char receiver_call[32];  /* callsign of the receiving station   */
    int  bundle_nr;          /* sequential 1-based bundle number    */
    int  record_count;       /* number of valid records (1–10)      */
    QTCRecord records[QTC_MAX_RECORDS_PER_BUNDLE];
    int  sent;               /* 1 = we sent this bundle, 0 = received */
} QTCBundle;

/* In-memory QTC bundle store. */
extern QTCBundle qtc_bundles[QTC_MAX_BUNDLES];
extern int qtc_bundle_count;

/*
 * Load existing QTC bundles from the database into memory.
 *
 * Should be called once after db_init().
 */
void qtc_init(void);

/*
 * Append a new QTC bundle to the in-memory store and persist it.
 *
 * @param bundle  Fully populated bundle to store.
 * @return Index of the inserted bundle on success, or -1 on error.
 */
int qtc_add_bundle(const QTCBundle *bundle);

/*
 * Count the total number of individual QTC records across all bundles.
 *
 * Used for scoring: each record is worth points_per_qtc points.
 */
int qtc_total_records(void);

/*
 * Count the number of bundles sent to a specific receiver.
 *
 * @param receiver_call  Receiver callsign to search for.
 * @return Number of matching bundles.
 */
int qtc_bundles_sent_to(const char *receiver_call);

/*
 * Check if a QSO identified by call/date/time is already part of any bundle.
 *
 * Prevents the same QSO from being transmitted twice.
 *
 * @return 1 if already included, 0 otherwise.
 */
int qtc_qso_already_sent(const char *call, const char *date, const char *time);

/*
 * Populate a QTCRecord from individual field strings.
 *
 * @param r     Destination record to fill.
 */
void qtc_record_init(QTCRecord *r, const char *date, const char *time,
                     const char *call, const char *exch);

/*
 * Validate a QTC bundle before submission.
 *
 * Checks that record_count is in [1, QTC_MAX_RECORDS_PER_BUNDLE], that
 * sender and receiver calls are non-empty, and that each record contains
 * a non-empty callsign.
 *
 * @param bundle      Bundle to validate.
 * @param error_text  Destination for a human-readable error message.
 * @param error_size  Size of error_text buffer.
 * @return 0 if valid, -1 on failure.
 */
int qtc_bundle_validate(const QTCBundle *bundle, char *error_text,
                        size_t error_size);

/*
 * Return the next available bundle number for a given sender/receiver pair.
 *
 * Scans existing bundles and returns max_bundle_nr + 1 (minimum 1).
 */
int qtc_next_bundle_nr(const char *sender_call, const char *receiver_call);

#ifdef __cplusplus
}
#endif

#endif /* QTC_H */
