#include "qtc.h"

#include "db.h"

#include <stdio.h>
#include <string.h>

/* In-memory QTC bundle store. */
QTCBundle qtc_bundles[QTC_MAX_BUNDLES];
int qtc_bundle_count = 0;

/*
 * Write an error message into a caller-supplied buffer.
 */
static void set_error(char *buf, size_t size, const char *msg) {
    if (!buf || size < 2)
        return;

    if (!msg)
        msg = "QTC error";

    snprintf(buf, size, "%s", msg);
}

/* ------------------------------------------------------------------ */

void qtc_init(void) {
    qtc_bundle_count = 0;
    memset(qtc_bundles, 0, sizeof(qtc_bundles));
    db_load_qtc_bundles(qtc_bundles, QTC_MAX_BUNDLES, &qtc_bundle_count);
}

int qtc_add_bundle(const QTCBundle *bundle) {
    if (!bundle)
        return -1;

    if (qtc_bundle_count >= QTC_MAX_BUNDLES)
        return -1;

    QTCBundle *dest = &qtc_bundles[qtc_bundle_count];
    *dest = *bundle;

    long long new_id = 0;
    if (db_insert_qtc_bundle(dest, &new_id) != 0)
        return -1;

    dest->db_id = new_id;
    return qtc_bundle_count++;
}

int qtc_total_records(void) {
    int total = 0;

    for (int i = 0; i < qtc_bundle_count; i++)
        total += qtc_bundles[i].record_count;

    return total;
}

int qtc_bundles_sent_to(const char *receiver_call) {
    if (!receiver_call || !receiver_call[0])
        return 0;

    int count = 0;
    for (int i = 0; i < qtc_bundle_count; i++) {
        if (qtc_bundles[i].sent &&
            strcmp(qtc_bundles[i].receiver_call, receiver_call) == 0)
            count++;
    }

    return count;
}

int qtc_qso_already_sent(const char *call, const char *date, const char *time) {
    if (!call || !call[0])
        return 0;

    for (int i = 0; i < qtc_bundle_count; i++) {
        const QTCBundle *b = &qtc_bundles[i];
        if (!b->sent)
            continue;

        for (int j = 0; j < b->record_count; j++) {
            const QTCRecord *r = &b->records[j];
            if (strcmp(r->call, call) == 0) {
                if ((!date || !date[0] || strcmp(r->date, date) == 0) &&
                    (!time || !time[0] || strcmp(r->time, time) == 0)) {
                    return 1;
                }
            }
        }
    }

    return 0;
}

void qtc_record_init(QTCRecord *r, const char *date, const char *time,
                     const char *call, const char *exch) {
    if (!r)
        return;

    memset(r, 0, sizeof(*r));
    if (date) snprintf(r->date, sizeof(r->date), "%s", date);
    if (time) snprintf(r->time, sizeof(r->time), "%s", time);
    if (call) snprintf(r->call, sizeof(r->call), "%s", call);
    if (exch) snprintf(r->exch, sizeof(r->exch), "%s", exch);
}

int qtc_bundle_validate(const QTCBundle *bundle, char *error_text,
                        size_t error_size) {
    if (!bundle) {
        set_error(error_text, error_size, "NULL bundle");
        return -1;
    }

    if (!bundle->sender_call[0]) {
        set_error(error_text, error_size, "Sender callsign is empty");
        return -1;
    }

    if (!bundle->receiver_call[0]) {
        set_error(error_text, error_size, "Receiver callsign is empty");
        return -1;
    }

    if (bundle->record_count < 1 ||
        bundle->record_count > QTC_MAX_RECORDS_PER_BUNDLE) {
        set_error(error_text, error_size, "record_count must be 1–10");
        return -1;
    }

    for (int i = 0; i < bundle->record_count; i++) {
        if (!bundle->records[i].call[0]) {
            set_error(error_text, error_size, "QTC record has empty callsign");
            return -1;
        }
    }

    if (error_text && error_size > 0)
        error_text[0] = 0;

    return 0;
}

int qtc_next_bundle_nr(const char *sender_call, const char *receiver_call) {
    int max_nr = 0;

    for (int i = 0; i < qtc_bundle_count; i++) {
        const QTCBundle *b = &qtc_bundles[i];
        if (!b->sent)
            continue;

        if (sender_call && sender_call[0] &&
            strcmp(b->sender_call, sender_call) != 0)
            continue;

        if (receiver_call && receiver_call[0] &&
            strcmp(b->receiver_call, receiver_call) != 0)
            continue;

        if (b->bundle_nr > max_nr)
            max_nr = b->bundle_nr;
    }

    return max_nr + 1;
}
