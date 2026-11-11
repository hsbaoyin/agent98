/*
 * Agent98 - socket and TLS transport.
 *
 * The TLS backend is BearSSL with a fixed 16 KB record buffer and the
 * compiled-in Mozilla trust anchors. Only TLS 1.2 can be negotiated,
 * which is exactly what modern LLM APIs require.
 */
#ifndef A98_NET_H
#define A98_NET_H

#include "../core/base.h"
#include "url.h"

typedef struct NetConn NetConn;

int net_init(void);
void net_cleanup(void);

/* Open a TCP connection, optionally performing a TLS handshake lazily on
 * the first write. `sni` is the certificate name; pass NULL to disable
 * both SNI and host name verification. `verify` = 0 disables chain
 * validation as well (testing only). */
NetConn *net_open(const Url *url, int verify, int connect_timeout_sec,
                  int request_timeout_sec, char *err, size_t errcap);

void net_set_cancel(NetConn *conn, volatile int *flag);
void net_set_deadline(NetConn *conn, unsigned long timeout_sec);

int net_write_all(NetConn *conn, const void *data, size_t len);

/* Returns bytes read (>0), 0 on EOF, -1 on error/cancel/timeout. */
int net_read(NetConn *conn, void *buf, size_t len);

void net_close(NetConn *conn);

int net_is_tls(const NetConn *conn);
const char *net_tls_version(const NetConn *conn);
const char *net_tls_cipher(const NetConn *conn);
const char *net_error(const NetConn *conn);
int net_peer_addr(const NetConn *conn, char *out, size_t cap);

#endif /* A98_NET_H */
