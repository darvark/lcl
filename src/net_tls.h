#ifndef NET_TLS_H
#define NET_TLS_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
  int fd;
  int use_tls;
  char peer_fingerprint[128];
  char local_fingerprint[128];
#ifdef HAVE_OPENSSL
  void *ctx;
  void *ssl;
#endif
} NetTransport;

int net_transport_init_client(NetTransport *transport, int fd,
                              const char *server_host, int use_tls,
                              const char *expected_peer_fingerprint,
                              char *error_text, size_t error_size);
int net_transport_init_server(NetTransport *transport, int fd, int use_tls,
                              const char *cert_file,
                              const char *key_file,
                              char *error_text, size_t error_size);
ssize_t net_transport_read_cb(void *ctx, void *buf, size_t len);
ssize_t net_transport_write_cb(void *ctx, const void *buf, size_t len);
void net_transport_close(NetTransport *transport);
const char *net_transport_peer_fingerprint(const NetTransport *transport);
const char *net_transport_local_fingerprint(const NetTransport *transport);

#endif