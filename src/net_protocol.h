#ifndef LOGGER_NET_PROTOCOL_H
#define LOGGER_NET_PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>

#include "db.h"

typedef enum {
  NET_MSG_UNKNOWN = 0,
  NET_MSG_HELLO = 1,
  NET_MSG_HELLO_ACK = 2,
  NET_MSG_HEARTBEAT = 3,
  NET_MSG_APPEND_OPS = 4,
  NET_MSG_PULL_OPS = 5,
  NET_MSG_ACK = 6,
  NET_MSG_ERROR = 7,
  NET_MSG_PULL_OPS_RESP = 8,
  NET_MSG_RESERVE_SERIAL = 9,
  NET_MSG_RESERVE_SERIAL_ACK = 10,
  NET_MSG_COMMIT_SERIAL = 11,
  NET_MSG_APPEND_ACK = 12
} NetMessageType;

typedef struct {
  char station_id[32];
  char op_id[96];
  long long station_seq;
  int logbook_id;
  char op_type[32];
  char entity_id[64];
  char payload_json[2048];
  char op_utc[32];
} NetAppendOp;

typedef struct {
  char station_id[32];
  char auth_token[128];
} NetSessionMeta;

typedef ssize_t (*NetProtocolReadFn)(void *ctx, void *buf, size_t len);
typedef ssize_t (*NetProtocolWriteFn)(void *ctx, const void *buf, size_t len);

int net_protocol_encode_hello(const char *station_id, const char *app_version,
                              const char *auth_token, char *out,
                              size_t out_size);
int net_protocol_encode_hello_ack(int accepted,
                                  long long next_expected_station_seq,
                                  long long server_global_seq, char *out,
                                  size_t out_size);
int net_protocol_encode_pull_ops(long long from_global_seq, int limit,
                                 char *out, size_t out_size);
int net_protocol_encode_append_ops(const SyncOutboxEntry *ops, int op_count,
                                   char *out, size_t out_size);
int net_protocol_encode_heartbeat(char *out, size_t out_size);
int net_protocol_encode_ack_empty(long long last_global_seq, char *out,
                                  size_t out_size);
int net_protocol_encode_append_ack(const char *accepted_json,
                                   const char *rejected_json,
                                   long long last_acked_station_seq,
                                   long long server_global_seq, char *out,
                                   size_t out_size);
int net_protocol_encode_pull_ops_resp(const SyncLogOpEntry *ops, int op_count,
                                      long long last_global_seq, int has_more,
                                      char *out, size_t out_size);
int net_protocol_encode_reserve_serial(const char *request_id, int ttl_sec,
                                       char *out, size_t out_size);
int net_protocol_encode_reserve_serial_ack(const char *request_id,
                                           const char *reservation_id,
                                           int serial,
                                           const char *expires_utc,
                                           char *out, size_t out_size);

int net_protocol_parse_append_ops(const char *frame, NetAppendOp *out,
                                  int max_items, int *out_count);
int net_protocol_parse_pull_ops_resp(const char *frame, SyncLogOpEntry *out,
                                     int max_items, int *out_count,
                                     long long *out_last_global_seq);
int net_protocol_parse_reserve_serial(const char *frame, char *request_id,
                                      size_t request_id_size, int *ttl_sec);
int net_protocol_parse_commit_serial(const char *frame, char *reservation_id,
                                     size_t reservation_id_size,
                                     char *qso_uid, size_t qso_uid_size);
int net_protocol_parse_hello_meta(const char *frame, NetSessionMeta *out);
int net_protocol_parse_hello_ack(const char *frame, int *out_accepted,
                                 long long *out_server_global_seq);
int net_protocol_parse_station_meta(const char *frame, NetSessionMeta *out);

int net_protocol_send_framed_io(void *ctx, NetProtocolWriteFn write_fn,
                                const char *json_frame);
int net_protocol_recv_framed_io_limited(void *ctx, NetProtocolReadFn read_fn,
                                        char *out, size_t out_size,
                                        size_t max_frame_size);
int net_protocol_recv_framed_io(void *ctx, NetProtocolReadFn read_fn,
                                char *out, size_t out_size);
int net_protocol_send_framed(int fd, const char *json_frame);
int net_protocol_recv_framed(int fd, char *out, size_t out_size);

int net_protocol_detect_type(const char *frame, NetMessageType *out_type);

#endif
