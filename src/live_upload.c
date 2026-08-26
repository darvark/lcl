#include "live_upload.h"

#include "config.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  char host[128];
  int port;
  char payload[1536];
} LiveUploadTask;

static void json_escape(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size < 2)
    return;

  dst[0] = 0;
  if (!src)
    return;

  size_t out = 0;
  for (size_t i = 0; src[i] && out < dst_size - 1; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c < 32)
      continue;

    if ((c == '"' || c == '\\') && out + 2 < dst_size) {
      dst[out++] = '\\';
      dst[out++] = (char)c;
    } else if (out + 1 < dst_size) {
      dst[out++] = (char)c;
    } else {
      break;
    }
  }

  dst[out] = 0;
}

static void *live_upload_worker(void *arg) {
  LiveUploadTask *task = (LiveUploadTask *)arg;
  if (!task)
    return NULL;

  char port_text[16] = {0};
  snprintf(port_text, sizeof(port_text), "%d", task->port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(task->host, port_text, &hints, &res) == 0) {
    for (struct addrinfo *it = res; it; it = it->ai_next) {
      int sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
      if (sock < 0)
        continue;

      (void)sendto(sock, task->payload, strlen(task->payload), 0, it->ai_addr,
                   it->ai_addrlen);
      close(sock);
      break;
    }

    freeaddrinfo(res);
  }

  free(task);
  return NULL;
}

int live_upload_publish_qso_and_stats(const QSO *q, const Statistics *stats,
                                      const char *contest_name) {
  if (!config.live_upload_enabled || !config.live_upload_host[0] ||
      config.live_upload_port < 1)
    return 0;

  if (!q || !stats)
    return -1;

  char call[64] = {0};
  char mode[32] = {0};
  char band[16] = {0};
  char country[96] = {0};
  char sent[48] = {0};
  char recv[48] = {0};
  char contest[96] = {0};
  char token[160] = {0};

  json_escape(call, sizeof(call), q->call);
  json_escape(mode, sizeof(mode), q->mode);
  json_escape(band, sizeof(band), q->band);
  json_escape(country, sizeof(country), q->country);
  json_escape(sent, sizeof(sent), q->exchange_sent);
  json_escape(recv, sizeof(recv), q->exchange_recv);
  json_escape(contest, sizeof(contest),
              contest_name && contest_name[0] ? contest_name : q->contest_id);
  json_escape(token, sizeof(token), config.live_upload_token);

  char payload[1536] = {0};
  snprintf(payload, sizeof(payload),
           "{"
           "\"type\":\"qso_score_update\","
           "\"station\":\"%s\","
           "\"operator\":\"%s\","
           "\"token\":\"%s\","
           "\"contest\":\"%s\","
           "\"qso\":{"
           "\"date\":\"%s\",\"utc\":\"%s\",\"call\":\"%s\","
           "\"freq\":%d,\"band\":\"%s\",\"mode\":\"%s\","
           "\"points\":%d,\"exch_sent\":\"%s\",\"exch_recv\":\"%s\","
           "\"country\":\"%s\"},"
           "\"score\":{"
           "\"total_qso\":%d,\"contest_qso_points\":%d,"
           "\"contest_mults\":%d,\"contest_score\":%d,"
           "\"total_dxcc\":%d,\"qtc_records\":%d,\"qtc_points\":%d}"
           "}",
           config.station_call, config_effective_operator_call(), token,
           contest, q->date, q->utc, call, q->freq, band, mode, q->points,
           sent, recv, country, stats->total_qso, stats->contest_qso_points,
           stats->contest_mults, stats->contest_score, stats->total_dxcc,
           stats->qtc_records, stats->qtc_points);

  LiveUploadTask *task = (LiveUploadTask *)calloc(1, sizeof(*task));
  if (!task)
    return -1;

  snprintf(task->host, sizeof(task->host), "%s", config.live_upload_host);
  task->port = config.live_upload_port;
  snprintf(task->payload, sizeof(task->payload), "%s", payload);

  pthread_t tid;
  if (pthread_create(&tid, NULL, live_upload_worker, task) != 0) {
    free(task);
    return -1;
  }

  pthread_detach(tid);
  return 0;
}
