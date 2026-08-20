
#include "net_protocol.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int json_get_string(const char *json, const char *key, char *out,
                           size_t out_size) {
  if (!json || !key || !out || out_size < 2)
    return -1;

  out[0] = 0;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  if (*p != '"')
    return -1;

  p++;
  size_t used = 0;
  while (*p && *p != '"' && used < out_size - 1) {
    if (*p == '\\' && p[1])
      p++;
    out[used++] = *p++;
  }
  out[used] = 0;
  return used > 0 ? 0 : -1;
}

static int json_get_i64(const char *json, const char *key, long long *out) {
  if (!json || !key || !out)
    return -1;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  char *endptr = NULL;
  long long value = strtoll(p, &endptr, 10);
  if (endptr == p)
    return -1;

  *out = value;
  return 0;
}

static int json_get_bool(const char *json, const char *key, int *out) {
  if (!json || !key || !out)
    return -1;

  char needle[96];
  snprintf(needle, sizeof(needle), "\"%s\":", key);

  const char *p = strstr(json, needle);
  if (!p)
    return -1;

  p += strlen(needle);
  while (*p == ' ' || *p == '\t')
    p++;

  if (strncmp(p, "true", 4) == 0) {
    *out = 1;
    return 0;
  }

  if (strncmp(p, "false", 5) == 0) {
    *out = 0;
    return 0;
  }

  return -1;
}

static int find_ops_array(const char *frame, const char **out_start,
                          const char **out_end) {
  if (!frame || !out_start || !out_end)
    return -1;

  const char *ops = strstr(frame, "\"ops\":[");
  if (!ops)
    return -1;

  const char *start = strchr(ops, '[');
  if (!start)
    return -1;
  start++;

  const char *end = strchr(start, ']');
  if (!end)
    return -1;

  *out_start = start;
  *out_end = end;
  return 0;
}

static int extract_object(const char **cursor, const char *limit, char *out,
                          size_t out_size) {
  if (!cursor || !*cursor || !limit || !out || out_size < 4)
    return -1;

  const char *p = *cursor;
  while (p < limit && *p != '{')
    p++;
  if (p >= limit)
    return -1;

  int depth = 0;
  const char *start = p;
  while (p < limit) {
    if (*p == '{')
      depth++;
    else if (*p == '}') {
      depth--;
      if (depth == 0) {
        size_t len = (size_t)(p - start + 1);
        if (len >= out_size)
          return -1;
        memcpy(out, start, len);
        out[len] = 0;
        *cursor = p + 1;
        return 0;
      }
    }
    p++;
  }

  return -1;
}

static void net_protocol_make_msg_id(char *out, size_t out_size) {
  static unsigned long long counter = 0;
  if (!out || out_size < 2)
    return;

  counter++;
  snprintf(out, out_size, "m-%llu-%lld", counter, (long long)time(NULL));
}

static int net_protocol_wrap(const char *type, const char *station_id,
                             const char *auth_token, const char *payload_json,
                             char *out, size_t out_size) {
  if (!type || !type[0] || !payload_json || !out || out_size < 32)
    return -1;

  char msg_id[64] = {0};
  net_protocol_make_msg_id(msg_id, sizeof(msg_id));

  const char *sid = station_id ? station_id : "";
  const char *token = auth_token ? auth_token : "";

  int n = snprintf(out, out_size,
                   "{\"protocol_ver\":1,\"msg_id\":\"%s\",\"type\":\"%s\",\"station_id\":\"%s\",\"auth_token\":\"%s\",\"payload\":%s}",
                   msg_id, type, sid, token, payload_json);
  return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

static ssize_t net_protocol_fd_read(void *ctx, void *buf, size_t len) {
  int fd = (int)(intptr_t)ctx;
  return recv(fd, buf, len, 0);
}

static ssize_t net_protocol_fd_write(void *ctx, const void *buf, size_t len) {
  int fd = (int)(intptr_t)ctx;
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  return send(fd, buf, len, flags);
}

int net_protocol_send_framed_io(void *ctx, NetProtocolWriteFn write_fn,
                                const char *json_frame) {
  if (!write_fn || !json_frame || !json_frame[0])
    return -1;

  size_t len = strlen(json_frame);
  if (len > 1024 * 1024)
    return -1;

  uint32_t be_len = htonl((uint32_t)len);
  size_t sent = 0;
  while (sent < sizeof(be_len)) {
    ssize_t n = write_fn(ctx, ((const char *)&be_len) + sent,
               sizeof(be_len) - sent);
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }

  sent = 0;
  while (sent < len) {
    ssize_t n = write_fn(ctx, json_frame + sent, len - sent);
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }

  return 0;
}

int net_protocol_recv_framed_io_limited(void *ctx, NetProtocolReadFn read_fn,
                                        char *out, size_t out_size,
                                        size_t max_frame_size) {
  if (!read_fn || !out || out_size < 2)
    return -1;

  out[0] = 0;

  uint32_t be_len = 0;
  size_t got = 0;
  while (got < sizeof(be_len)) {
    ssize_t n = read_fn(ctx, ((char *)&be_len) + got, sizeof(be_len) - got);
    if (n <= 0)
      return -1;
    got += (size_t)n;
  }

  uint32_t len = ntohl(be_len);
  if (len == 0 || len >= out_size ||
      (max_frame_size > 0 && len > max_frame_size))
    return -1;

  size_t recvd = 0;
  while (recvd < len) {
    ssize_t n = read_fn(ctx, out + recvd, len - recvd);
    if (n <= 0)
      return -1;
    recvd += (size_t)n;
  }

  out[len] = 0;
  return 0;
}

int net_protocol_recv_framed_io(void *ctx, NetProtocolReadFn read_fn,
                                char *out, size_t out_size) {
  return net_protocol_recv_framed_io_limited(ctx, read_fn, out, out_size,
                                             out_size > 0 ? out_size - 1 : 0);
}

int net_protocol_send_framed(int fd, const char *json_frame) {
  return net_protocol_send_framed_io((void *)(intptr_t)fd,
                                     net_protocol_fd_write, json_frame);
}

int net_protocol_recv_framed(int fd, char *out, size_t out_size) {
  return net_protocol_recv_framed_io((void *)(intptr_t)fd,
                                     net_protocol_fd_read, out, out_size);
}

int net_protocol_encode_hello(const char *station_id, const char *app_version,
                              const char *auth_token, char *out,
                              size_t out_size) {
  if (!station_id || !station_id[0] || !app_version || !app_version[0])
    return -1;

  char payload[256] = {0};
  int n = snprintf(payload, sizeof(payload), "{\"app\":\"%s\"}",
                   app_version);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("HELLO", station_id, auth_token, payload, out,
                           out_size);
}

int net_protocol_encode_hello_ack(int accepted,
                                  long long next_expected_station_seq,
                                  long long server_global_seq, char *out,
                                  size_t out_size) {
  char payload[256] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"accepted\":%s,\"next_expected_station_seq\":%lld,\"server_global_seq\":%lld}",
                   accepted ? "true" : "false", next_expected_station_seq,
                   server_global_seq);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("HELLO_ACK", "", "", payload, out, out_size);
}

int net_protocol_encode_pull_ops(long long from_global_seq, int limit,
                                 char *out, size_t out_size) {
  if (!out || out_size < 8 || from_global_seq < 0)
    return -1;

  if (limit < 1)
    limit = 1;
  if (limit > 1000)
    limit = 1000;

  char payload[128] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"from_global_seq\":%lld,\"limit\":%d}",
                   from_global_seq, limit);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("PULL_OPS", "", "", payload, out, out_size);
}

int net_protocol_encode_append_ops(const SyncOutboxEntry *ops, int op_count,
                                   char *out, size_t out_size) {
  if (!ops || op_count < 1 || !out || out_size < 24)
    return -1;

  char payload[16384] = {0};
  size_t used = 0;
  int n = snprintf(payload, sizeof(payload), "{\"ops\":[");
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;
  used = (size_t)n;

  for (int i = 0; i < op_count; i++) {
    const SyncOutboxEntry *e = &ops[i];
    n = snprintf(payload + used, sizeof(payload) - used,
           "%s{\"op_id\":\"%s\",\"station_seq\":%lld,\"logbook_id\":%d,\"op_type\":\"%s\",\"entity_id\":\"%s\",\"payload\":%s,\"op_utc\":\"%s\"}",
           i == 0 ? "" : ",", e->op_id, e->station_seq,
           e->logbook_id, e->op_type, e->entity_id,
           e->payload_json[0] ? e->payload_json : "{}", e->op_utc);
    if (n <= 0 || (size_t)n >= (sizeof(payload) - used))
      return -1;
    used += (size_t)n;
  }

  n = snprintf(payload + used, sizeof(payload) - used, "]}");
  if (n <= 0 || (size_t)n >= (sizeof(payload) - used))
    return -1;

  return net_protocol_wrap("APPEND_OPS", "", "", payload, out, out_size);
}

int net_protocol_encode_heartbeat(char *out, size_t out_size) {
  return net_protocol_wrap("HEARTBEAT", "", "", "{}", out, out_size);
}

int net_protocol_encode_ack_empty(long long last_global_seq, char *out,
                                  size_t out_size) {
  char payload[128] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"acked\":[],\"last_global_seq\":%lld}",
                   last_global_seq);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("ACK", "", "", payload, out, out_size);
}

int net_protocol_encode_append_ack(const char *accepted_json,
                                   const char *rejected_json,
                                   long long last_acked_station_seq,
                                   long long server_global_seq, char *out,
                                   size_t out_size) {
  char payload[4096] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"accepted_ops\":%s,\"rejected_ops\":%s,\"last_acked_station_seq\":%lld,\"server_global_seq\":%lld}",
                   accepted_json ? accepted_json : "[]",
                   rejected_json ? rejected_json : "[]",
                   last_acked_station_seq, server_global_seq);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("APPEND_ACK", "", "", payload, out, out_size);
}

int net_protocol_encode_pull_ops_resp(const SyncLogOpEntry *ops, int op_count,
                                      long long last_global_seq, int has_more,
                                      char *out, size_t out_size) {
  if (!out || out_size < 24)
    return -1;

  if (!ops || op_count < 0)
    op_count = 0;

  char payload[16384] = {0};
  int n = snprintf(payload, sizeof(payload), "{\"ops\":[");
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  size_t used = (size_t)n;
  for (int i = 0; i < op_count; i++) {
    const SyncLogOpEntry *e = &ops[i];
    n = snprintf(payload + used, sizeof(payload) - used,
                 "%s{\"global_seq\":%lld,\"op_id\":\"%s\",\"station_id\":\"%s\",\"station_seq\":%lld,\"logbook_id\":%d,\"op_type\":\"%s\",\"entity_id\":\"%s\",\"payload\":%s,\"op_utc\":\"%s\"}",
                 i == 0 ? "" : ",", e->global_seq, e->op_id, e->station_id,
                 e->station_seq, e->logbook_id, e->op_type, e->entity_id,
                 e->payload_json[0] ? e->payload_json : "{}", e->op_utc);
    if (n <= 0 || (size_t)n >= sizeof(payload) - used)
      return -1;
    used += (size_t)n;
  }

  n = snprintf(payload + used, sizeof(payload) - used,
               "],\"last_global_seq\":%lld,\"has_more\":%s}",
               last_global_seq, has_more ? "true" : "false");
  if (n <= 0 || (size_t)n >= sizeof(payload) - used)
    return -1;

  return net_protocol_wrap("PULL_OPS_RESP", "", "", payload, out, out_size);
}

int net_protocol_encode_reserve_serial(const char *request_id, int ttl_sec,
                                       char *out, size_t out_size) {
  if (!request_id || !request_id[0] || !out || out_size < 24)
    return -1;

  if (ttl_sec < 1)
    ttl_sec = 1;

  char payload[256] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"request_id\":\"%s\",\"ttl_sec\":%d}", request_id,
                   ttl_sec);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("RESERVE_SERIAL", "", "", payload, out, out_size);
}

int net_protocol_encode_reserve_serial_ack(const char *request_id,
                                           const char *reservation_id,
                                           int serial,
                                           const char *expires_utc,
                                           char *out, size_t out_size) {
  if (!request_id || !request_id[0] || !reservation_id || !reservation_id[0] ||
      !expires_utc || !expires_utc[0] || !out || out_size < 24)
    return -1;

  char payload[512] = {0};
  int n = snprintf(payload, sizeof(payload),
                   "{\"request_id\":\"%s\",\"reservation_id\":\"%s\",\"serial\":%d,\"expires_utc\":\"%s\"}",
                   request_id, reservation_id, serial, expires_utc);
  if (n <= 0 || (size_t)n >= sizeof(payload))
    return -1;

  return net_protocol_wrap("RESERVE_SERIAL_ACK", "", "", payload, out,
                           out_size);
}

int net_protocol_parse_station_meta(const char *frame, NetSessionMeta *out) {
  if (!frame || !out)
    return -1;

  memset(out, 0, sizeof(*out));
  (void)json_get_string(frame, "station_id", out->station_id,
                        sizeof(out->station_id));
  (void)json_get_string(frame, "auth_token", out->auth_token,
                        sizeof(out->auth_token));
  return 0;
}

int net_protocol_parse_hello_meta(const char *frame, NetSessionMeta *out) {
  return net_protocol_parse_station_meta(frame, out);
}

int net_protocol_parse_hello_ack(const char *frame, int *out_accepted,
                                 long long *out_server_global_seq) {
  if (!frame || !out_accepted || !out_server_global_seq)
    return -1;

  int accepted = 0;
  long long server_seq = 0;

  (void)json_get_bool(frame, "accepted", &accepted);
  (void)json_get_i64(frame, "server_global_seq", &server_seq);

  *out_accepted = accepted;
  *out_server_global_seq = server_seq;
  return 0;
}

int net_protocol_parse_append_ops(const char *frame, NetAppendOp *out,
                                  int max_items, int *out_count) {
  if (!frame || !out || max_items <= 0 || !out_count)
    return -1;

  *out_count = 0;

  const char *start = NULL;
  const char *end = NULL;
  if (find_ops_array(frame, &start, &end) != 0)
    return 0;

  const char *cursor = start;
  int count = 0;
  while (cursor < end && count < max_items) {
    char obj[4096] = {0};
    if (extract_object(&cursor, end, obj, sizeof(obj)) != 0)
      break;

    NetAppendOp *dst = &out[count];
    memset(dst, 0, sizeof(*dst));

    long long seq = 0;
    long long logbook_id = 1;
    if (json_get_string(obj, "op_id", dst->op_id, sizeof(dst->op_id)) != 0)
      continue;
    (void)json_get_string(obj, "station_id", dst->station_id,
                          sizeof(dst->station_id));
    (void)json_get_i64(obj, "station_seq", &seq);
    (void)json_get_i64(obj, "logbook_id", &logbook_id);
    (void)json_get_string(obj, "op_type", dst->op_type, sizeof(dst->op_type));
    (void)json_get_string(obj, "entity_id", dst->entity_id,
                          sizeof(dst->entity_id));
    (void)json_get_string(obj, "op_utc", dst->op_utc, sizeof(dst->op_utc));

    dst->station_seq = seq;
    dst->logbook_id = (int)logbook_id;

    const char *payload = strstr(obj, "\"payload\":");
    if (payload) {
      payload += strlen("\"payload\":");
      while (*payload == ' ' || *payload == '\t')
        payload++;
      if (*payload == '{') {
        int depth = 0;
        const char *p = payload;
        while (*p) {
          if (*p == '{')
            depth++;
          else if (*p == '}') {
            depth--;
            if (depth == 0) {
              size_t len = (size_t)(p - payload + 1);
              if (len >= sizeof(dst->payload_json))
                len = sizeof(dst->payload_json) - 1;
              memcpy(dst->payload_json, payload, len);
              dst->payload_json[len] = 0;
              break;
            }
          }
          p++;
        }
      }
    }
    if (!dst->payload_json[0])
      snprintf(dst->payload_json, sizeof(dst->payload_json), "%s", "{}");

    if (!dst->op_type[0])
      snprintf(dst->op_type, sizeof(dst->op_type), "%s", "QSO_INSERT");
    if (!dst->entity_id[0])
      snprintf(dst->entity_id, sizeof(dst->entity_id), "%s", "unknown");
    if (!dst->op_utc[0])
      snprintf(dst->op_utc, sizeof(dst->op_utc), "%s", "1970-01-01T00:00:00Z");

    count++;
  }

  *out_count = count;
  return 0;
}

int net_protocol_parse_pull_ops_resp(const char *frame, SyncLogOpEntry *out,
                                     int max_items, int *out_count,
                                     long long *out_last_global_seq) {
  if (!frame || !out || max_items <= 0 || !out_count || !out_last_global_seq)
    return -1;

  *out_count = 0;
  *out_last_global_seq = 0;

  (void)json_get_i64(frame, "last_global_seq", out_last_global_seq);

  const char *start = NULL;
  const char *end = NULL;
  if (find_ops_array(frame, &start, &end) != 0)
    return 0;

  const char *cursor = start;
  int count = 0;
  while (cursor < end && count < max_items) {
    char obj[4096] = {0};
    if (extract_object(&cursor, end, obj, sizeof(obj)) != 0)
      break;

    SyncLogOpEntry *dst = &out[count];
    memset(dst, 0, sizeof(*dst));

    long long logbook_id = 1;
    (void)json_get_i64(obj, "global_seq", &dst->global_seq);
    (void)json_get_string(obj, "op_id", dst->op_id, sizeof(dst->op_id));
    (void)json_get_string(obj, "station_id", dst->station_id,
                          sizeof(dst->station_id));
    (void)json_get_i64(obj, "station_seq", &dst->station_seq);
    (void)json_get_i64(obj, "logbook_id", &logbook_id);
    (void)json_get_string(obj, "op_type", dst->op_type, sizeof(dst->op_type));
    (void)json_get_string(obj, "entity_id", dst->entity_id,
                          sizeof(dst->entity_id));
    (void)json_get_string(obj, "op_utc", dst->op_utc, sizeof(dst->op_utc));
    dst->logbook_id = (int)logbook_id;

    const char *payload = strstr(obj, "\"payload\":");
    if (payload) {
      payload += strlen("\"payload\":");
      while (*payload == ' ' || *payload == '\t')
        payload++;
      if (*payload == '{') {
        int depth = 0;
        const char *p = payload;
        while (*p) {
          if (*p == '{')
            depth++;
          else if (*p == '}') {
            depth--;
            if (depth == 0) {
              size_t len = (size_t)(p - payload + 1);
              if (len >= sizeof(dst->payload_json))
                len = sizeof(dst->payload_json) - 1;
              memcpy(dst->payload_json, payload, len);
              dst->payload_json[len] = 0;
              break;
            }
          }
          p++;
        }
      }
    }

    if (dst->op_id[0])
      count++;
  }

  *out_count = count;
  return 0;
}

int net_protocol_parse_reserve_serial(const char *frame, char *request_id,
                                      size_t request_id_size, int *ttl_sec) {
  if (!frame || !request_id || request_id_size < 2 || !ttl_sec)
    return -1;

  request_id[0] = 0;
  *ttl_sec = 0;

  if (json_get_string(frame, "request_id", request_id, request_id_size) != 0)
    return -1;

  long long ttl = 0;
  if (json_get_i64(frame, "ttl_sec", &ttl) != 0)
    ttl = 120;

  *ttl_sec = (int)ttl;
  return 0;
}

int net_protocol_parse_commit_serial(const char *frame, char *reservation_id,
                                     size_t reservation_id_size,
                                     char *qso_uid, size_t qso_uid_size) {
  if (!frame || !reservation_id || reservation_id_size < 2 || !qso_uid ||
      qso_uid_size < 2)
    return -1;

  reservation_id[0] = 0;
  qso_uid[0] = 0;

  if (json_get_string(frame, "reservation_id", reservation_id,
                      reservation_id_size) != 0)
    return -1;

  (void)json_get_string(frame, "qso_uid", qso_uid, qso_uid_size);
  return 0;
}

int net_protocol_detect_type(const char *frame, NetMessageType *out_type) {
  if (!frame || !out_type)
    return -1;

  *out_type = NET_MSG_UNKNOWN;

  if (strstr(frame, "\"type\":\"HELLO\""))
    *out_type = NET_MSG_HELLO;
  else if (strstr(frame, "\"type\":\"HELLO_ACK\""))
    *out_type = NET_MSG_HELLO_ACK;
  else if (strstr(frame, "\"type\":\"HEARTBEAT\""))
    *out_type = NET_MSG_HEARTBEAT;
  else if (strstr(frame, "\"type\":\"APPEND_OPS\""))
    *out_type = NET_MSG_APPEND_OPS;
  else if (strstr(frame, "\"type\":\"PULL_OPS\""))
    *out_type = NET_MSG_PULL_OPS;
  else if (strstr(frame, "\"type\":\"PULL_OPS_RESP\""))
    *out_type = NET_MSG_PULL_OPS_RESP;
  else if (strstr(frame, "\"type\":\"APPEND_ACK\""))
    *out_type = NET_MSG_APPEND_ACK;
  else if (strstr(frame, "\"type\":\"ACK\""))
    *out_type = NET_MSG_ACK;
  else if (strstr(frame, "\"type\":\"RESERVE_SERIAL\""))
    *out_type = NET_MSG_RESERVE_SERIAL;
  else if (strstr(frame, "\"type\":\"RESERVE_SERIAL_ACK\""))
    *out_type = NET_MSG_RESERVE_SERIAL_ACK;
  else if (strstr(frame, "\"type\":\"COMMIT_SERIAL\""))
    *out_type = NET_MSG_COMMIT_SERIAL;
  else if (strstr(frame, "\"type\":\"ERROR\""))
    *out_type = NET_MSG_ERROR;

  return 0;
}
