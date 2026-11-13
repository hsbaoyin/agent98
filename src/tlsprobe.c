/*
 * Agent98 - TLS probe.
 *
 * Small standalone gate: performs a real HTTPS request with BearSSL and
 * reports the negotiated TLS version/cipher. Run this on the target Win9x
 * machine before trusting the full agent build.
 */

#include "core/base.h"
#include "core/log.h"
#include "net/http.h"
#include "net/net.h"
#include "net/url.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct {
    unsigned long bytes;
    int printed;
} ProbeSink;

static int probe_sink(void *ctx, const char *data, size_t len)
{
    ProbeSink *s = (ProbeSink *)ctx;

    (void)data;
    s->bytes += (unsigned long)len;
    return 0;
}

static void usage(void)
{
    printf("Agent98 TLS probe\n");
    printf("Usage: tlsprobe [url] [--insecure]\n");
    printf("  url        https://host[:port][/path] (default https://api.openai.com/v1/models)\n");
    printf("  --insecure skip certificate validation (diagnostics only)\n");
}

int main(int argc, char **argv)
{
    const char *target = "https://api.openai.com/v1/models";
    char target_buf[1200];
    char err[512];
    char peer[128];
    Url url;
    NetConn *conn;
    HttpResponse resp;
    ProbeSink sink;
    char headers[512];
    DWORD started;
    DWORD elapsed;
    int verify = 1;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--insecure") == 0 || strcmp(argv[i], "-k") == 0) {
            verify = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            target = argv[i];
        }
    }

    if (strstr(target, "://") == NULL) {
        snprintf(target_buf, sizeof(target_buf), "https://%s", target);
        target = target_buf;
    }

    err[0] = '\0';
    if (url_parse(target, &url, err, sizeof(err)) != 0) {
        printf("URL error: %s\n", err);
        return 2;
    }
    if (!url.tls) {
        printf("This probe expects an https:// URL.\n");
        return 2;
    }

    log_init(NULL, LOG_LEVEL_WARN);
    if (net_init() != 0) {
        printf("Failed to initialize Winsock.\n");
        return 2;
    }

    printf("Agent98 TLS probe\n");
    printf("Target: %s\n", target);
    printf("Verify: %s\n", verify ? "on (compiled-in Mozilla roots)" : "OFF");

    conn = net_open(&url, verify, 30, 60, err, sizeof(err));
    if (conn == NULL) {
        printf("Connect failed: %s\n", err[0] ? err : "unknown error");
        net_cleanup();
        return 2;
    }
    if (net_peer_addr(conn, peer, sizeof(peer)) == 0) {
        printf("Peer: %s\n", peer);
    }

    /* Only the headers the transport does not add itself: build_request()
     * always sends Host, User-Agent and Connection, and http_request() skips
     * its default Accept when this block supplies one.  Sending Host here too
     * used to put two Host headers on the wire. */
    strcpy(headers, "Accept: application/json\r\n");

    memset(&resp, 0, sizeof(resp));
    memset(&sink, 0, sizeof(sink));
    started = GetTickCount();
    rc = http_request(conn, "GET", &url, headers, NULL, 0, &resp,
                      probe_sink, &sink, err, sizeof(err));
    elapsed = GetTickCount() - started;

    if (rc != 0) {
        printf("TLS/HTTP failed after %lu ms: %s\n",
               (unsigned long)elapsed,
               err[0] ? err : net_error(conn));
        printf("Negotiated: %s / %s\n",
               net_tls_version(conn) ? net_tls_version(conn) : "(none)",
               net_tls_cipher(conn) ? net_tls_cipher(conn) : "(none)");
        net_close(conn);
        net_cleanup();
        return 2;
    }

    printf("TLS: %s / %s\n",
           net_tls_version(conn) ? net_tls_version(conn) : "(none)",
           net_tls_cipher(conn) ? net_tls_cipher(conn) : "(none)");
    printf("HTTP: %d %s (%lu bytes, %lu ms)\n",
           resp.status, resp.reason, sink.bytes, (unsigned long)elapsed);
    printf("RESULT: OK\n");

    net_close(conn);
    net_cleanup();
    log_shutdown();
    return 0;
}
