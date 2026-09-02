#ifndef NET_SYNC_H
#define NET_SYNC_H

#include <stddef.h>

typedef struct {
  int running;
  int connected;
  int tls_enabled;
  int reconnect_count;
  int failure_streak;
  long long last_pulled_global_seq;
  int pending_outbox;
  char station_id[32];
  char last_success_utc[32];
  char last_heartbeat_utc[32];
  char last_error[128];
} NetSyncStatus;

/* Start synchronization worker (foundation phase: state only). */
int net_sync_start(void);

/* Stop synchronization worker and release resources. */
void net_sync_stop(void);

/* Trigger one synchronization tick. */
int net_sync_poll_once(void);

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
