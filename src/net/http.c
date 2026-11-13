#include "http.h"
#include "body.h"
#include "../core/log.h"

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int is_tchar(char c)
{
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    return c == '-' || c == '_' || c == '.' || c == '!' || c == '#' ||
           c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
           c == '+' || c == '^' || c == '`' || c == '|' || c == '~';
}

int http_parse_response_head(const char *data, size_t len, HttpResponse *out,
                             char *err, size_t errcap)
{
    size_t i;
    size_t head_end = 0;
    size_t line_start;
    int found_end = 0;
    int line_no = 0;

    if (out == NULL || data == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (i + 1 < len && data[i + 1] == '\n') {
                head_end = i + 2;
                found_end = 1;
                break;
            }
            if (i >= 2 && data[i - 1] == '\r' && data[i - 2] == '\n') {
                head_end = i + 1;
                found_end = 1;
                break;
            }
        }
    }
    if (!found_end) {
        set_err(err, errcap, "incomplete HTTP response head");
        return -1;
    }
    out->head_len = head_end;

    line_start = 0;
    for (i = 0; i <= head_end && line_start < head_end; i++) {
        if (i == head_end || data[i] == '\n') {
            size_t end = i;
            if (end > line_start && data[end - 1] == '\r') {
                end--;
            }
            if (line_no == 0) {
                /* Status line: HTTP/1.x SP status SP reason */
                size_t p = line_start;
                int status = 0;
                if (end - line_start < 12 ||
                    memcmp(data + line_start, "HTTP/", 5) != 0) {
                    set_err(err, errcap, "not an HTTP response");
                    return -1;
                }
                while (p < end && data[p] != ' ') {
                    p++;
                }
                if (p >= end || end - p < 4) {
                    set_err(err, errcap, "malformed HTTP status line");
                    return -1;
                }
                p++;
                if (!(data[p] >= '0' && data[p] <= '9') ||
                    !(data[p + 1] >= '0' && data[p + 1] <= '9') ||
                    !(data[p + 2] >= '0' && data[p + 2] <= '9')) {
                    set_err(err, errcap, "malformed HTTP status code");
                    return -1;
                }
                status = (data[p] - '0') * 100 + (data[p + 1] - '0') * 10 +
                         (data[p + 2] - '0');
                out->status = status;
                p += 3;
                while (p < end && data[p] == ' ') {
                    p++;
                }
                {
                    size_t n = end - p;
                    if (n >= sizeof(out->reason)) {
                        n = sizeof(out->reason) - 1;
                    }
                    memcpy(out->reason, data + p, n);
                    out->reason[n] = '\0';
                }
            } else if (end > line_start) {
                size_t colon = line_start;
                char name[64];
                char value[1024];
                size_t nlen;
                size_t vstart;
                size_t vlen;

                while (colon < end && data[colon] != ':') {
                    colon++;
                }
                if (colon >= end) {
                    set_err(err, errcap, "malformed HTTP header line");
                    return -1;
                }
                nlen = colon - line_start;
                if (nlen == 0 || nlen >= sizeof(name)) {
                    set_err(err, errcap, "invalid HTTP header name");
                    return -1;
                }
                memcpy(name, data + line_start, nlen);
                name[nlen] = '\0';
                vstart = colon + 1;
                while (vstart < end && (data[vstart] == ' ' || data[vstart] == '\t')) {
                    vstart++;
                }
                vlen = end - vstart;
                if (vlen >= sizeof(value)) {
                    /* Truncating used to turn a long Content-Length or
                     * Transfer-Encoding into a confusing parse error. */
                    set_err(err, errcap, "HTTP header value is too long");
                    return -1;
                }
                memcpy(value, data + vstart, vlen);
                value[vlen] = '\0';
                while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
                    value[--vlen] = '\0';
                }

                if (a98_strcasecmp(name, "Content-Length") == 0) {
                    char *endptr = NULL;
                    unsigned long cl = strtoul(value, &endptr, 10);
                    if (endptr == value || *endptr != '\0') {
                        set_err(err, errcap, "invalid Content-Length");
                        return -1;
                    }
                    out->content_length = cl;
                    out->has_length = 1;
                } else if (a98_strcasecmp(name, "Transfer-Encoding") == 0) {
                    size_t vlen = strlen(value);
                    size_t k;
                    for (k = 0; k + 7 <= vlen; k++) {
                        if (a98_strncasecmp(value + k, "chunked", 7) == 0) {
                            out->chunked = 1;
                            break;
                        }
                    }
                } else if (a98_strcasecmp(name, "Content-Type") == 0) {
                    strncpy(out->content_type, value, sizeof(out->content_type) - 1);
                    out->content_type[sizeof(out->content_type) - 1] = '\0';
                } else if (a98_strcasecmp(name, "Location") == 0) {
                    strncpy(out->location, value, sizeof(out->location) - 1);
                    out->location[sizeof(out->location) - 1] = '\0';
                } else if (a98_strcasecmp(name, "Connection") == 0) {
                    if (a98_strcasecmp(value, "close") == 0) {
                        out->connection_close = 1;
                    }
                }
            }
            line_no++;
            line_start = i + 1;
        }
    }
    if (out->status < 100 || out->status > 599) {
        set_err(err, errcap, "invalid HTTP status code");
        return -1;
    }
    return 0;
}

/* Is `name` already present in a "Name: value\r\n" block?  Used to avoid
 * sending two Host or Accept headers when the caller supplies its own. */
static int headers_contain(const char *block, const char *name)
{
    const char *p;
    size_t nlen;

    if (block == NULL || name == NULL) {
        return 0;
    }
    nlen = strlen(name);
    p = block;
    while (*p != '\0') {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        if ((size_t)(eol - p) > nlen &&
            a98_strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            return 1;
        }
        p = (*eol == '\0') ? eol : eol + 1;
    }
    return 0;
}

static int build_request(Str *req, const char *method, const Url *url,
                         const char *extra_headers, size_t body_len)
{
    char host[300];

    url_host_header(url, host, sizeof(host));
    str_appendf(req, "%s %s HTTP/1.1\r\n", method, url->path);
    if (!headers_contain(extra_headers, "Host")) {
        str_appendf(req, "Host: %s\r\n", host);
    }
    str_appendf(req, "User-Agent: Agent98/%s\r\n", A98_VERSION);
    if (!headers_contain(extra_headers, "Accept")) {
        str_appendf(req, "Accept: */*\r\n");
    }
    str_appendf(req, "Connection: close\r\n");
    if (extra_headers != NULL && extra_headers[0] != '\0') {
        str_append_cstr(req, extra_headers);
        if (req->len >= 2 && !(req->data[req->len - 2] == '\r' && req->data[req->len - 1] == '\n')) {
            str_append_cstr(req, "\r\n");
        }
    }
    if (body_len > 0) {
        str_appendf(req, "Content-Length: %lu\r\n", (unsigned long)body_len);
    }
    str_append_cstr(req, "\r\n");
    return 0;
}

int http_request(NetConn *conn, const char *method, const Url *url,
                 const char *extra_headers, const char *body, size_t body_len,
                 HttpResponse *resp, HttpSink sink, void *sink_ctx,
                 char *err, size_t errcap)
{
    Str req;
    Str head;
    char buf[4096];
    BodyDecoder decoder;
    int got_head = 0;
    int rc = -1;
    int read_err = 0;
    int saw_eof = 0;

    if (conn == NULL || url == NULL || method == NULL || resp == NULL) {
        set_err(err, errcap, "invalid HTTP request arguments");
        return -1;
    }
    str_init(&req);
    str_init(&head);
    memset(&decoder, 0, sizeof(decoder));
    str_init(&decoder.line);

    if (body == NULL) {
        body_len = 0;
    }

    build_request(&req, method, url, extra_headers, body_len);
    log_debug("HTTP %s %s (%lu body bytes)", method, url->path, (unsigned long)body_len);
    if (net_write_all(conn, str_cstr(&req), req.len) != 0) {
        set_err(err, errcap, "%s", net_error(conn));
        goto done;
    }
    if (body_len > 0 && body != NULL) {
        if (net_write_all(conn, body, body_len) != 0) {
            set_err(err, errcap, "%s", net_error(conn));
            goto done;
        }
    }

    for (;;) {
        int n = net_read(conn, buf, sizeof(buf));
        if (n == 0) {
            saw_eof = 1;
            break;
        }
        if (n < 0) {
            read_err = 1;
            break;
        }
        if (!got_head) {
            str_append(&head, buf, (size_t)n);
            if (head.len > 65536) {
                set_err(err, errcap, "HTTP response head too large");
                goto done;
            }
            if (http_parse_response_head(head.data, head.len, resp, err, errcap) == 0) {
                size_t body_off = resp->head_len;
                got_head = 1;
                if (resp->status == 204 || resp->status == 304 ||
                    a98_strcasecmp(method, "HEAD") == 0) {
                    body_decoder_init(&decoder, 0, 0, 0);
                } else if (resp->chunked) {
                    body_decoder_init(&decoder, 1, 0, 0);
                } else if (resp->has_length) {
                    body_decoder_init(&decoder, 0, resp->content_length, 0);
                } else {
                    body_decoder_init(&decoder, 0, 0, 1);
                }
                if (body_off < head.len) {
                    if (body_decoder_feed(&decoder, head.data + body_off,
                                          head.len - body_off, sink, sink_ctx,
                                          err, errcap) != 0) {
                        goto done;
                    }
                }
                str_free(&head);
                str_init(&head);
            } else {
                /* Parsing fails until the blank line arrives; only report
                 * real format errors once the head looks complete. */
                if (strstr(str_cstr(&head), "\r\n\r\n") != NULL ||
                    strstr(str_cstr(&head), "\n\n") != NULL) {
                    goto done;
                }
            }
        } else {
            if (body_decoder_feed(&decoder, buf, (size_t)n, sink, sink_ctx,
                                  err, errcap) != 0) {
                goto done;
            }
            if (decoder.done) {
                break;
            }
        }
        if (decoder.done) {
            break;
        }
    }

    if (!got_head) {
        if (read_err && net_error(conn)[0] != '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        } else {
            set_err(err, errcap, "server closed the connection before sending a response");
        }
        goto done;
    }
    if (decoder.mode == 2) {
        /* EOF-delimited body: only a clean EOF completes the body. */
        if (!saw_eof) {
            if (read_err && net_error(conn)[0] != '\0') {
                set_err(err, errcap, "%s", net_error(conn));
            } else {
                set_err(err, errcap, "connection closed before the HTTP body was complete");
            }
            goto done;
        }
        if (body_decoder_finish(&decoder, err, errcap) != 0) {
            goto done;
        }
    } else if (!decoder.done) {
        if (read_err && net_error(conn)[0] != '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        } else if (body_decoder_finish(&decoder, err, errcap) != 0) {
            /* error already set */
        }
        goto done;
    }
    rc = 0;
    goto done;

done:
    body_decoder_free(&decoder);
    str_free(&req);
    str_free(&head);
    if (rc != 0 && err != NULL && errcap > 0 && err[0] == '\0') {
        set_err(err, errcap, "HTTP request failed");
    }
    return rc;
}
