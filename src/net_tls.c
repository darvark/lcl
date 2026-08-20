#include "net_tls.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef HAVE_OPENSSL
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

static void ignore_sigpipe_once(void) {
  static int initialized = 0;
  if (initialized)
    return;

  signal(SIGPIPE, SIG_IGN);
  initialized = 1;
}

static void set_ssl_error(char *error_text, size_t error_size,
                          const char *prefix) {
  if (!error_text || error_size < 2)
    return;

  unsigned long err = ERR_get_error();
  char ssl_error[128] = {0};
  if (err)
    ERR_error_string_n(err, ssl_error, sizeof(ssl_error));
  snprintf(error_text, error_size, "%s%s%s", prefix ? prefix : "tls error",
           err ? ": " : "", err ? ssl_error : "");
}

static int compute_fingerprint_from_cert(X509 *cert, char *out,
                                         size_t out_size) {
  if (!cert || !out || out_size < 95)
    return -1;

  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1)
    return -1;

  size_t used = 0;
  out[0] = 0;
  for (unsigned int i = 0; i < md_len; i++) {
    int n = snprintf(out + used, out_size - used, "%s%02X",
                     i == 0 ? "" : ":", md[i]);
    if (n <= 0 || (size_t)n >= out_size - used)
      return -1;
    used += (size_t)n;
  }

  return 0;
}

static int save_cert_and_key(const char *cert_file, const char *key_file,
                             X509 *cert, EVP_PKEY *pkey) {
  if (!cert_file || !cert_file[0] || !key_file || !key_file[0] || !cert ||
      !pkey)
    return -1;

  FILE *cert_fp = fopen(cert_file, "w");
  if (!cert_fp)
    return -1;

  if (PEM_write_X509(cert_fp, cert) != 1) {
    fclose(cert_fp);
    return -1;
  }
  fclose(cert_fp);

  FILE *key_fp = fopen(key_file, "w");
  if (!key_fp)
    return -1;

  if (PEM_write_PrivateKey(key_fp, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
    fclose(key_fp);
    return -1;
  }
  fclose(key_fp);
  return 0;
}

static int load_cert_and_key(SSL_CTX *ctx, const char *cert_file,
                             const char *key_file, char *local_fingerprint,
                             size_t local_fingerprint_size) {
  if (!ctx || !cert_file || !cert_file[0] || !key_file || !key_file[0])
    return -1;

  if (access(cert_file, R_OK) != 0 || access(key_file, R_OK) != 0)
    return -1;

  if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1)
    return -1;
  if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1)
    return -1;

  FILE *cert_fp = fopen(cert_file, "r");
  if (!cert_fp)
    return -1;

  X509 *cert = PEM_read_X509(cert_fp, NULL, NULL, NULL);
  fclose(cert_fp);
  if (!cert)
    return -1;

  int rc = compute_fingerprint_from_cert(cert, local_fingerprint,
                                         local_fingerprint_size);
  X509_free(cert);
  return rc;
}

static int add_self_signed_certificate(SSL_CTX *ctx, const char *cert_file,
                                       const char *key_file,
                                       char *local_fingerprint,
                                       size_t local_fingerprint_size) {
  EVP_PKEY *pkey = NULL;
  RSA *rsa = NULL;
  BIGNUM *bn = NULL;
  X509 *cert = NULL;
  int rc = -1;

  pkey = EVP_PKEY_new();
  rsa = RSA_new();
  bn = BN_new();
  cert = X509_new();
  if (!pkey || !rsa || !bn || !cert)
    goto cleanup;

  if (BN_set_word(bn, RSA_F4) != 1)
    goto cleanup;
  if (RSA_generate_key_ex(rsa, 2048, bn, NULL) != 1)
    goto cleanup;
  if (EVP_PKEY_assign_RSA(pkey, rsa) != 1)
    goto cleanup;
  rsa = NULL;

  if (X509_set_version(cert, 2) != 1)
    goto cleanup;
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 31536000L);
  if (X509_set_pubkey(cert, pkey) != 1)
    goto cleanup;

  X509_NAME *name = X509_get_subject_name(cert);
  if (!name)
    goto cleanup;
  X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                             (const unsigned char *)"PL", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                             (const unsigned char *)"logger", -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             (const unsigned char *)"logger-net", -1, -1,
                             0);
  if (X509_set_issuer_name(cert, name) != 1)
    goto cleanup;
  if (X509_sign(cert, pkey, EVP_sha256()) <= 0)
    goto cleanup;

  if (SSL_CTX_use_certificate(ctx, cert) != 1)
    goto cleanup;
  if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1)
    goto cleanup;

  if (compute_fingerprint_from_cert(cert, local_fingerprint,
                                    local_fingerprint_size) != 0)
    goto cleanup;

  if (save_cert_and_key(cert_file, key_file, cert, pkey) != 0)
    goto cleanup;

  rc = 0;

cleanup:
  X509_free(cert);
  EVP_PKEY_free(pkey);
  RSA_free(rsa);
  BN_free(bn);
  return rc;
}

static int init_tls_common(NetTransport *transport, SSL_CTX *ctx, SSL *ssl,
                           char *error_text, size_t error_size) {
  if (!transport || !ctx || !ssl)
    return -1;

  SSL_set_fd(ssl, transport->fd);
  transport->ctx = ctx;
  transport->ssl = ssl;
  if (error_text && error_size > 0)
    error_text[0] = 0;
  return 0;
}
#endif

int net_transport_init_client(NetTransport *transport, int fd,
                              const char *server_host, int use_tls,
                              const char *expected_peer_fingerprint,
                              char *error_text, size_t error_size) {
  if (!transport || fd < 0)
    return -1;

  memset(transport, 0, sizeof(*transport));
  transport->fd = fd;
  transport->use_tls = use_tls ? 1 : 0;
  ignore_sigpipe_once();

#ifdef HAVE_OPENSSL
  if (transport->use_tls) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = NULL;
    if (!ctx) {
      set_ssl_error(error_text, error_size, "tls client ctx");
      return -1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    ssl = SSL_new(ctx);
    if (!ssl || init_tls_common(transport, ctx, ssl, error_text, error_size) !=
                    0) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      set_ssl_error(error_text, error_size, "tls client init");
      return -1;
    }

    if (server_host && server_host[0])
      SSL_set_tlsext_host_name(ssl, server_host);

    if (SSL_connect(ssl) != 1) {
      set_ssl_error(error_text, error_size, "tls handshake");
      net_transport_close(transport);
      return -1;
    }

    X509 *peer = SSL_get_peer_certificate(ssl);
    if (!peer) {
      if (error_text && error_size > 0)
        snprintf(error_text, error_size, "%s", "tls peer certificate missing");
      net_transport_close(transport);
      return -1;
    }

    if (compute_fingerprint_from_cert(peer, transport->peer_fingerprint,
                                      sizeof(transport->peer_fingerprint)) != 0) {
      X509_free(peer);
      if (error_text && error_size > 0)
        snprintf(error_text, error_size, "%s", "tls peer fingerprint failed");
      net_transport_close(transport);
      return -1;
    }
    X509_free(peer);

    if (expected_peer_fingerprint && expected_peer_fingerprint[0] &&
        strcmp(expected_peer_fingerprint, transport->peer_fingerprint) != 0) {
      if (error_text && error_size > 0)
        snprintf(error_text, error_size, "tls fingerprint mismatch");
      net_transport_close(transport);
      return -1;
    }
  }
  return 0;
#else
  (void)server_host;
  (void)expected_peer_fingerprint;
  if (transport->use_tls) {
    if (error_text && error_size > 0)
      snprintf(error_text, error_size, "%s", "TLS requested but OpenSSL is unavailable");
    return -1;
  }
  if (error_text && error_size > 0)
    error_text[0] = 0;
  return 0;
#endif
}

int net_transport_init_server(NetTransport *transport, int fd, int use_tls,
                              const char *cert_file,
                              const char *key_file,
                              char *error_text, size_t error_size) {
  if (!transport || fd < 0)
    return -1;

  memset(transport, 0, sizeof(*transport));
  transport->fd = fd;
  transport->use_tls = use_tls ? 1 : 0;
  ignore_sigpipe_once();

#ifdef HAVE_OPENSSL
  if (transport->use_tls) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    SSL *ssl = NULL;
    if (!ctx) {
      set_ssl_error(error_text, error_size, "tls server ctx");
      return -1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (load_cert_and_key(ctx, cert_file, key_file, transport->local_fingerprint,
                          sizeof(transport->local_fingerprint)) != 0) {
      if (add_self_signed_certificate(ctx, cert_file, key_file,
                                      transport->local_fingerprint,
                                      sizeof(transport->local_fingerprint)) != 0) {
        set_ssl_error(error_text, error_size, "tls certificate");
        SSL_CTX_free(ctx);
        return -1;
      }
    }

    ssl = SSL_new(ctx);
    if (!ssl || init_tls_common(transport, ctx, ssl, error_text, error_size) !=
                    0) {
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      set_ssl_error(error_text, error_size, "tls server init");
      return -1;
    }

    if (SSL_accept(ssl) != 1) {
      set_ssl_error(error_text, error_size, "tls accept");
      net_transport_close(transport);
      return -1;
    }
  }
  return 0;
#else
  (void)cert_file;
  (void)key_file;
  if (transport->use_tls) {
    if (error_text && error_size > 0)
      snprintf(error_text, error_size, "%s", "TLS requested but OpenSSL is unavailable");
    return -1;
  }
  if (error_text && error_size > 0)
    error_text[0] = 0;
  return 0;
#endif
}

ssize_t net_transport_read_cb(void *ctx, void *buf, size_t len) {
  NetTransport *transport = (NetTransport *)ctx;
  if (!transport || transport->fd < 0 || !buf || len == 0)
    return -1;

#ifdef HAVE_OPENSSL
  if (transport->use_tls && transport->ssl)
    return (ssize_t)SSL_read((SSL *)transport->ssl, buf, (int)len);
#endif

  return recv(transport->fd, buf, len, 0);
}

ssize_t net_transport_write_cb(void *ctx, const void *buf, size_t len) {
  NetTransport *transport = (NetTransport *)ctx;
  if (!transport || transport->fd < 0 || !buf || len == 0)
    return -1;

#ifdef HAVE_OPENSSL
  if (transport->use_tls && transport->ssl)
    return (ssize_t)SSL_write((SSL *)transport->ssl, buf, (int)len);
#endif

  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  return send(transport->fd, buf, len, flags);
}

void net_transport_close(NetTransport *transport) {
  if (!transport)
    return;

#ifdef HAVE_OPENSSL
  if (transport->ssl) {
    SSL_shutdown((SSL *)transport->ssl);
    SSL_free((SSL *)transport->ssl);
    transport->ssl = NULL;
  }
  if (transport->ctx) {
    SSL_CTX_free((SSL_CTX *)transport->ctx);
    transport->ctx = NULL;
  }
#endif

  if (transport->fd >= 0) {
    close(transport->fd);
    transport->fd = -1;
  }
}

const char *net_transport_peer_fingerprint(const NetTransport *transport) {
  if (!transport || !transport->peer_fingerprint[0])
    return NULL;
  return transport->peer_fingerprint;
}

const char *net_transport_local_fingerprint(const NetTransport *transport) {
  if (!transport || !transport->local_fingerprint[0])
    return NULL;
  return transport->local_fingerprint;
}