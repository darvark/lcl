#ifndef NET_SYNC_H
#define NET_SYNC_H

#include <stddef.h>

#define NET_SYNC_REMOTE_REJECTED (-2)

typedef struct {
  int running;
  int connected;
  int tls_enabled;
  int reconnect_count;
  int failure_streak;
  long long last_pulled_global_seq;
  int pending_outbox;
  int failed_outbox;
  char station_id[32];
  char last_success_utc[32];
  char last_heartbeat_utc[32];
  char last_error[128];
} NetSyncStatus;

/* Start the configured server or client synchronization worker. */
int net_sync_start(void);
int net_sync_validate_config(char *error, size_t error_size);
int net_sync_token_is_strong(const char *token);

/* Stop synchronization worker and release resources. */
void net_sync_stop(void);

/* Trigger one periodic poll using a short-lived client TCP session. */
int net_sync_poll_once(void);
/* Request a poll without waiting; intended for UI commands. */
int net_sync_request_poll(void);
int net_sync_peek_serial_reservation(int *out_serial);
int net_sync_reserve_serial_for_qso(int *out_serial,
                char *out_reservation_id,
                size_t out_reservation_id_size,
                int *out_commit_remote);
void net_sync_set_serial_prefetch_enabled(int enabled);

/* Reserve serial from central server (client mode). */
int net_sync_reserve_serial_remote(int *out_serial);
int net_sync_reserve_serial_remote_ex(int *out_serial,
                                      char *out_reservation_id,
                                      size_t out_reservation_id_size);

/* Commit already reserved serial after QSO is persisted locally. */
int net_sync_commit_serial_remote(const char *reservation_id,
                                  const char *qso_uid);

/* Read current synchronization status snapshot. */
void net_sync_get_status(NetSyncStatus *out);

#endif
