#include "net.h"
#include "../core/log.h"
#include "../trust_anchors.h"

#include <winsock2.h>
#include <windows.h>
#include <time.h>

#include "bearssl.h"

#define NET_IOBUF_SIZE 16384
#define NET_SELECT_MS 200

struct NetConn {
    SOCKET sock;
    int tls;
    int closed;
    int handshake_done;
    int verify;
    volatile int *cancel;
    unsigned long deadline; /* GetTickCount deadline, 0 = none */
    char err[320];
    char peer[64];
    char version[16];
    char cipher[32];

    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char *iobuf;
};

static int g_wsa_started = 0;

int net_init(void)
{
    WSADATA wsa;
    int rc;

    if (g_wsa_started) {
        return 0;
    }
    rc = WSAStartup(MAKEWORD(2, 0), &wsa);
    if (rc != 0) {
        rc = WSAStartup(MAKEWORD(1, 1), &wsa);
        if (rc != 0) {
            return -1;
        }
    }
    g_wsa_started = 1;
    return 0;
}

void net_cleanup(void)
{
    if (g_wsa_started) {
        WSACleanup();
        g_wsa_started = 0;
    }
}

static void set_err(NetConn *c, const char *fmt, ...)
{
    va_list ap;

    if (c == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
}

static int deadline_expired(NetConn *c)
{
    if (c->deadline == 0) {
        return 0;
    }
    return (long)(GetTickCount() - c->deadline) >= 0;
}

/* Wait for readability/writability with cancellation checks. */
static int sock_wait(NetConn *c, int for_write)
{
    fd_set set;
    struct timeval tv;
    int rc;

    for (;;) {
        if (c->cancel != NULL && *c->cancel) {
            set_err(c, "cancelled by user");
            return -1;
        }
        if (deadline_expired(c)) {
            set_err(c, "network timeout");
            return -1;
        }
        FD_ZERO(&set);
        FD_SET(c->sock, &set);
        tv.tv_sec = 0;
        tv.tv_usec = NET_SELECT_MS * 1000;
        if (for_write) {
            rc = select((int)c->sock + 1, NULL, &set, NULL, &tv);
        } else {
            rc = select((int)c->sock + 1, &set, NULL, NULL, &tv);
        }
        if (rc > 0) {
            return 0;
        }
        if (rc == 0) {
            continue;
        }
        if (WSAGetLastError() == WSAEINTR) {
            continue;
        }
        set_err(c, "select failed (Winsock error %d)", WSAGetLastError());
        return -1;
    }
}

static int sock_read_cb(void *ctx, unsigned char *buf, size_t len)
{
    NetConn *c = (NetConn *)ctx;
    int n;

    for (;;) {
        if (sock_wait(c, 0) != 0) {
            return -1;
        }
        n = recv(c->sock, (char *)buf, (int)len, 0);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            continue;
        }
        if (WSAGetLastError() == WSAEINTR) {
            continue;
        }
        set_err(c, "recv failed (Winsock error %d)", WSAGetLastError());
        return -1;
    }
}

static int sock_write_cb(void *ctx, const unsigned char *buf, size_t len)
{
    NetConn *c = (NetConn *)ctx;
    int n;

    for (;;) {
        if (sock_wait(c, 1) != 0) {
            return -1;
        }
        n = send(c->sock, (const char *)buf, (int)len, 0);
        if (n >= 0) {
            return n;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            continue;
        }
        if (WSAGetLastError() == WSAEINTR) {
            continue;
        }
        set_err(c, "send failed (Winsock error %d)", WSAGetLastError());
        return -1;
    }
}

static const char *tls_error_name(int err)
{
    switch (err) {
    case 0:
        return "no error";
    case BR_ERR_BAD_PARAM:
        return "bad parameter";
    case BR_ERR_BAD_STATE:
        return "bad state";
    case BR_ERR_UNSUPPORTED_VERSION:
        return "unsupported protocol version (server may require TLS 1.3 only)";
    case BR_ERR_BAD_VERSION:
        return "bad protocol version";
    case BR_ERR_BAD_LENGTH:
        return "bad record length";
    case BR_ERR_TOO_LARGE:
        return "record too large";
    case BR_ERR_BAD_MAC:
        return "bad MAC";
    case BR_ERR_NO_RANDOM:
        return "no entropy available";
    case BR_ERR_UNEXPECTED:
        return "unexpected message";
    case BR_ERR_X509_INVALID_VALUE:
        return "bad certificate";
    case BR_ERR_IO:
        return "I/O error";
    case BR_ERR_X509_NOT_TRUSTED:
        return "certificate is not trusted (check the system clock and CA bundle)";
    case BR_ERR_X509_EXPIRED:
        return "certificate expired (Win9x clock may be wrong)";
    case BR_ERR_X509_BAD_TIME:
        return "certificate time is invalid (Win9x clock may be wrong)";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "certificate does not match the server name";
    case BR_ERR_X509_TIME_UNKNOWN:
        return "certificate time cannot be validated";
    case BR_ERR_X509_BAD_SIGNATURE:
        return "certificate signature is invalid";
    case BR_ERR_X509_UNSUPPORTED:
        return "unsupported certificate feature";
    case BR_ERR_X509_WEAK_PUBLIC_KEY:
        return "certificate public key is too weak";
    case BR_ERR_X509_NOT_CA:
        return "issuer is not a CA";
    case BR_ERR_X509_CRITICAL_EXTENSION:
        return "unsupported critical certificate extension";
    default:
        return "TLS error";
    }
}

static void entropy_seed(unsigned char *seed, size_t len)
{
    br_sha256_context sha;
    SYSTEMTIME st;
    LARGE_INTEGER qpc;
    POINT pt;
    DWORD tick;
    DWORD pid;
    DWORD tid;
    int i;
    unsigned int mix;
    unsigned char digest[32];
    size_t off = 0;

    memset(seed, 0, len);
    mix = 0x9E3779B9UL;

    GetSystemTime(&st);
    GetCursorPos(&pt);
    tick = GetTickCount();
    pid = GetCurrentProcessId();
    tid = GetCurrentThreadId();
    qpc.QuadPart = 0;
    QueryPerformanceCounter(&qpc);

    br_sha256_init(&sha);
    br_sha256_update(&sha, &st, sizeof(st));
    br_sha256_update(&sha, &pt, sizeof(pt));
    br_sha256_update(&sha, &tick, sizeof(tick));
    br_sha256_update(&sha, &pid, sizeof(pid));
    br_sha256_update(&sha, &tid, sizeof(tid));
    br_sha256_update(&sha, &qpc, sizeof(qpc));
    {
        void *stack = &st;
        br_sha256_update(&sha, &stack, sizeof(stack));
    }
    br_sha256_out(&sha, digest);

    /* Mix a few clock-derived words so repeated calls differ. */
    for (i = 0; i < 64; i++) {
        mix ^= (unsigned int)(tick + (unsigned int)i * 2654435761UL);
        mix = (mix << 13) | (mix >> 19);
        mix *= 0x5bd1e995UL;
        mix ^= (unsigned int)qpc.LowPart;
    }
    br_sha256_init(&sha);
    br_sha256_update(&sha, digest, sizeof(digest));
    br_sha256_update(&sha, &mix, sizeof(mix));
    br_sha256_out(&sha, digest);

    while (off < len) {
        size_t n = len - off;
        if (n > sizeof(digest)) {
            n = sizeof(digest);
        }
        memcpy(seed + off, digest, n);
        off += n;
        br_sha256_init(&sha);
        br_sha256_update(&sha, digest, sizeof(digest));
        br_sha256_out(&sha, digest);
    }
    memset(digest, 0, sizeof(digest));
}

static NetConn *conn_new(void)
{
    NetConn *c = (NetConn *)a98_alloc(sizeof(NetConn));
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->sock = INVALID_SOCKET;
    return c;
}

static void conn_free(NetConn *c)
{
    if (c == NULL) {
        return;
    }
    a98_free(c->iobuf);
    a98_free(c);
}

static const char *version_name(unsigned v, char *buf, size_t cap)
{
    switch (v) {
    case 0x0301:
        strncpy(buf, "TLS 1.0", cap - 1);
        break;
    case 0x0302:
        strncpy(buf, "TLS 1.1", cap - 1);
        break;
    case 0x0303:
        strncpy(buf, "TLS 1.2", cap - 1);
        break;
    case 0x0304:
        strncpy(buf, "TLS 1.3", cap - 1);
        break;
    default:
        snprintf(buf, cap, "0x%04X", v);
        break;
    }
    buf[cap - 1] = '\0';
    return buf;
}

static const char *cipher_name(unsigned suite)
{
    switch (suite) {
    case 0xC02B: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case 0xC02C: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case 0xC02F: return "ECDHE-RSA-AES128-GCM-SHA256";
    case 0xC030: return "ECDHE-RSA-AES256-GCM-SHA384";
    case 0xCCA8: return "ECDHE-RSA-CHACHA20-POLY1305";
    case 0xCCA9: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case 0xC013: return "ECDHE-RSA-AES128-SHA";
    case 0xC014: return "ECDHE-RSA-AES256-SHA";
    case 0xC027: return "ECDHE-RSA-AES128-SHA256";
    case 0xC028: return "ECDHE-RSA-AES256-SHA384";
    case 0xC009: return "ECDHE-ECDSA-AES128-SHA";
    case 0xC00A: return "ECDHE-ECDSA-AES256-SHA";
    case 0xC023: return "ECDHE-ECDSA-AES128-SHA256";
    case 0xC024: return "ECDHE-ECDSA-AES256-SHA384";
    case 0x009C: return "AES128-GCM-SHA256";
    case 0x009D: return "AES256-GCM-SHA384";
    case 0x002F: return "AES128-SHA";
    case 0x0035: return "AES256-SHA";
    case 0x003C: return "AES128-SHA256";
    case 0x003D: return "AES256-SHA256";
    case 0x000A: return "DES-CBC3-SHA";
    default: return NULL;
    }
}

static void update_session_info(NetConn *c)
{
    unsigned v = br_ssl_engine_get_version(&c->cc.eng);
    unsigned suite = (unsigned)c->cc.eng.session.cipher_suite;
    const char *name = cipher_name(suite);

    version_name(v, c->version, sizeof(c->version));
    if (name != NULL) {
        snprintf(c->cipher, sizeof(c->cipher), "%s", name);
    } else {
        snprintf(c->cipher, sizeof(c->cipher), "0x%04X", suite);
    }
    c->handshake_done = 1;
}

static int tls_start(NetConn *c, const Url *url, int verify)
{
    unsigned char seed[48];
    time_t now;
    unsigned long days;
    unsigned long secs;

    c->tls = 1;
    c->verify = verify;
    c->iobuf = (unsigned char *)a98_alloc(NET_IOBUF_SIZE);
    if (c->iobuf == NULL) {
        set_err(c, "out of memory");
        return -1;
    }

    br_ssl_client_init_full(&c->cc, &c->xc, TAs, TAs_NUM);
    br_ssl_engine_set_buffer(&c->cc.eng, c->iobuf, NET_IOBUF_SIZE, 1);

    now = time(NULL);
    if (now > 0) {
        days = 719528UL + (unsigned long)(now / 86400L);
        secs = (unsigned long)(now % 86400L);
        br_x509_minimal_set_time(&c->xc, days, secs);
    }
    if (!verify) {
        br_ssl_engine_set_x509(&c->cc.eng, NULL);
    }

    entropy_seed(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&c->cc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    if (!br_ssl_client_reset(&c->cc, url->host, 0)) {
        set_err(c, "TLS initialisation failed: %s",
                tls_error_name(br_ssl_engine_last_error(&c->cc.eng)));
        return -1;
    }
    br_sslio_init(&c->ioc, &c->cc.eng, sock_read_cb, c, sock_write_cb, c);
    log_debug("TLS context ready for %s (verify=%d)", url->host, verify);
    return 0;
}

static int tcp_connect(NetConn *c, const Url *url, int timeout_sec, char *err, size_t errcap)
{
    struct hostent *he;
    struct sockaddr_in sa;
    unsigned long addr;
    u_long nonblock = 1;
    fd_set wset;
    struct timeval tv;
    int rc;
    int so_error = 0;
    int slen = sizeof(so_error);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)url->port);

    addr = inet_addr(url->host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname(url->host);
        if (he == NULL || he->h_addrtype != AF_INET) {
            snprintf(err, errcap, "DNS lookup failed for %s (Winsock error %d)",
                     url->host, WSAGetLastError());
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock == INVALID_SOCKET) {
        snprintf(err, errcap, "socket() failed (Winsock error %d)", WSAGetLastError());
        return -1;
    }
    ioctlsocket(c->sock, FIONBIO, &nonblock);

    rc = connect(c->sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0) {
        int werr = WSAGetLastError();
        if (werr != WSAEWOULDBLOCK && werr != WSAEINPROGRESS) {
            snprintf(err, errcap, "connect to %s:%d failed (Winsock error %d)",
                     url->host, url->port, werr);
            return -1;
        }
        FD_ZERO(&wset);
        FD_SET(c->sock, &wset);
        tv.tv_sec = timeout_sec > 0 ? timeout_sec : 30;
        tv.tv_usec = 0;
        rc = select((int)c->sock + 1, NULL, &wset, NULL, &tv);
        if (rc <= 0) {
            snprintf(err, errcap, "connect to %s:%d timed out",
                     url->host, url->port);
            return -1;
        }
        if (getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &slen) != 0 ||
            so_error != 0) {
            snprintf(err, errcap, "connect to %s:%d failed (error %d)",
                     url->host, url->port, so_error);
            return -1;
        }
    }
    {
        int one = 1;
        setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    }
    {
        struct sockaddr_in peer;
        int plen = sizeof(peer);
        if (getpeername(c->sock, (struct sockaddr *)&peer, &plen) == 0) {
            strncpy(c->peer, inet_ntoa(peer.sin_addr), sizeof(c->peer) - 1);
            c->peer[sizeof(c->peer) - 1] = '\0';
        }
    }
    return 0;
}

NetConn *net_open(const Url *url, int verify, int connect_timeout_sec,
                  int request_timeout_sec, char *err, size_t errcap)
{
    NetConn *c;

    if (url == NULL) {
        return NULL;
    }
    if (net_init() != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "Winsock initialisation failed");
        }
        return NULL;
    }
    c = conn_new();
    if (c == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return NULL;
    }
    if (tcp_connect(c, url, connect_timeout_sec, err, errcap) != 0) {
        if (c->sock != INVALID_SOCKET) {
            closesocket(c->sock);
        }
        conn_free(c);
        return NULL;
    }
    if (url->tls) {
        if (tls_start(c, url, verify) != 0) {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap, "%s", c->err);
            }
            closesocket(c->sock);
            conn_free(c);
            return NULL;
        }
    }
    if (request_timeout_sec > 0) {
        c->deadline = GetTickCount() + (unsigned long)request_timeout_sec * 1000UL;
    }
    log_debug("connected to %s:%d%s%s%s", url->host, url->port,
              url->tls ? " via TLS" : "", c->peer[0] ? " peer " : "", c->peer);
    return c;
}

void net_set_cancel(NetConn *conn, volatile int *flag)
{
    if (conn != NULL) {
        conn->cancel = flag;
    }
}

void net_set_deadline(NetConn *conn, unsigned long timeout_sec)
{
    if (conn != NULL) {
        conn->deadline = timeout_sec ? GetTickCount() + timeout_sec * 1000UL : 0;
    }
}

int net_write_all(NetConn *conn, const void *data, size_t len)
{
    const char *p = (const char *)data;

    if (conn == NULL || conn->closed) {
        return -1;
    }
    if (conn->tls) {
        if (br_sslio_write_all(&conn->ioc, data, len) < 0) {
            set_err(conn, "TLS write failed: %s",
                    tls_error_name(br_ssl_engine_last_error(&conn->cc.eng)));
            return -1;
        }
        if (br_sslio_flush(&conn->ioc) < 0) {
            set_err(conn, "TLS flush/handshake failed: %s",
                    tls_error_name(br_ssl_engine_last_error(&conn->cc.eng)));
            return -1;
        }
        if (!conn->handshake_done) {
            update_session_info(conn);
            log_info("TLS %s, cipher %s, peer %s",
                     conn->version, conn->cipher, conn->peer);
        }
        return 0;
    }
    while (len > 0) {
        int n;
        if (sock_wait(conn, 1) != 0) {
            return -1;
        }
        n = send(conn->sock, p, (int)len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && (WSAGetLastError() == WSAEWOULDBLOCK ||
                      WSAGetLastError() == WSAEINTR)) {
            continue;
        }
        set_err(conn, "send failed (Winsock error %d)", WSAGetLastError());
        return -1;
    }
    return 0;
}

int net_read(NetConn *conn, void *buf, size_t len)
{
    if (conn == NULL || conn->closed || len == 0) {
        return -1;
    }
    if (conn->tls) {
        int n = br_sslio_read(&conn->ioc, buf, len);
        if (n < 0) {
            int err = br_ssl_engine_last_error(&conn->cc.eng);
            if (err != 0) {
                set_err(conn, "TLS read failed: %s", tls_error_name(err));
            } else if (conn->cancel != NULL && *conn->cancel) {
                set_err(conn, "cancelled by user");
            } else if (deadline_expired(conn)) {
                set_err(conn, "network timeout");
            } else {
                set_err(conn, "TLS read failed");
            }
            return -1;
        }
        return n;
    } else {
        int n;
        for (;;) {
            if (sock_wait(conn, 0) != 0) {
                return -1;
            }
            n = recv(conn->sock, (char *)buf, (int)len, 0);
            if (n >= 0) {
                return n;
            }
            if (WSAGetLastError() == WSAEWOULDBLOCK ||
                WSAGetLastError() == WSAEINTR) {
                continue;
            }
            set_err(conn, "recv failed (Winsock error %d)", WSAGetLastError());
            return -1;
        }
    }
}

void net_close(NetConn *conn)
{
    if (conn == NULL) {
        return;
    }
    if (!conn->closed) {
        conn->closed = 1;
        if (conn->sock != INVALID_SOCKET) {
            /* Do not linger on close_notify: a broken link would stall. */
            closesocket(conn->sock);
            conn->sock = INVALID_SOCKET;
        }
    }
    conn_free(conn);
}

int net_is_tls(const NetConn *conn)
{
    return conn != NULL && conn->tls;
}

const char *net_tls_version(const NetConn *conn)
{
    if (conn == NULL || !conn->tls) {
        return "";
    }
    return conn->version[0] ? conn->version : "TLS (handshake pending)";
}

const char *net_tls_cipher(const NetConn *conn)
{
    if (conn == NULL || !conn->tls) {
        return "";
    }
    return conn->cipher;
}

const char *net_error(const NetConn *conn)
{
    if (conn == NULL) {
        return "no connection";
    }
    return conn->err;
}

int net_peer_addr(const NetConn *conn, char *out, size_t cap)
{
    if (conn == NULL || out == NULL || cap == 0) {
        return -1;
    }
    strncpy(out, conn->peer, cap - 1);
    out[cap - 1] = '\0';
    return 0;
}
