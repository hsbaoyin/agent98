#include "body.h"

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        strncpy(err, msg, cap - 1);
        err[cap - 1] = '\0';
    }
}

void body_decoder_init(BodyDecoder *d, int chunked, unsigned long content_length,
                       int until_eof)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
    str_init(&d->line);
    if (chunked) {
        d->mode = 1;
    } else if (until_eof) {
        d->mode = 2;
    } else {
        d->mode = 0;
        d->remaining = content_length;
        if (content_length == 0) {
            d->done = 1;
        }
    }
}

void body_decoder_free(BodyDecoder *d)
{
    if (d != NULL) {
        str_free(&d->line);
    }
}

static int parse_chunk_size(Str *line, unsigned long *out)
{
    size_t i = 0;
    unsigned long v = 0;
    int digits = 0;

    /* Skip leading whitespace */
    while (i < line->len && (line->data[i] == ' ' || line->data[i] == '\t')) {
        i++;
    }
    if (i < line->len && line->data[i] == '-') {
        /* Disallow negative chunk sizes */
        return -1;
    }

    while (i < line->len) {
        char c = line->data[i];
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        }
        if (d < 0) {
            break;
        }
        /* Keep the running value inside 28 bits so the shift below cannot
         * overflow on a 32-bit host (Win9x).  Chunk data itself is streamed
         * to the sink, so this bounds the number, not the memory. */
        if (v > 0x00FFFFFFUL) {
            return -1;
        }
        v = (v << 4) | (unsigned long)d;
        digits++;
        i++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

int body_decoder_feed(BodyDecoder *d, const char *data, size_t len,
                      BodySink sink, void *ctx, char *err, size_t errcap)
{
    size_t i = 0;

    if (d == NULL) {
        return -1;
    }
    if (d->done) {
        return 0;
    }
    if (d->mode == 0) {
        size_t n = len;
        if (n > (size_t)d->remaining) {
            n = (size_t)d->remaining;
        }
        if (n > 0 && sink != NULL) {
            if (sink(ctx, data, n) != 0) {
                set_err(err, errcap, "body sink failed");
                return -1;
            }
        }
        d->remaining -= (unsigned long)n;
        d->total += (unsigned long)n;
        if (d->remaining == 0) {
            d->done = 1;
        }
        return 0;
    }
    if (d->mode == 2) {
        if (len > 0 && sink != NULL) {
            if (sink(ctx, data, len) != 0) {
                set_err(err, errcap, "body sink failed");
                return -1;
            }
        }
        d->total += (unsigned long)len;
        return 0;
    }

    while (i < len && !d->done) {
        char c = data[i];

        if (d->state == 0) {
            /* Chunk size line. */
            i++;
            if (c == '\n') {
                if (d->line.len > 0 && d->line.data[d->line.len - 1] == '\r') {
                    d->line.len--;
                    d->line.data[d->line.len] = '\0';
                }
                {
                    unsigned long size = 0;
                    if (parse_chunk_size(&d->line, &size) != 0) {
                        set_err(err, errcap, "invalid chunk size");
                        return -1;
                    }
                    str_clear(&d->line);
                    if (size == 0) {
                        d->state = 3;
                    } else {
                        d->remaining = size;
                        d->state = 1;
                    }
                }
            } else {
                if (d->line.len > 128) {
                    set_err(err, errcap, "chunk size line too long");
                    return -1;
                }
                str_append_char(&d->line, c);
            }
        } else if (d->state == 1) {
            /* Chunk data. */
            size_t avail = len - i;
            size_t n = avail;
            if (n > (size_t)d->remaining) {
                n = (size_t)d->remaining;
            }
            if (n > 0 && sink != NULL) {
                if (sink(ctx, data + i, n) != 0) {
                    set_err(err, errcap, "body sink failed");
                    return -1;
                }
            }
            d->total += (unsigned long)n;
            d->remaining -= (unsigned long)n;
            i += n;
            if (d->remaining == 0) {
                d->state = 2;
                d->saw_cr = 0;
            }
        } else if (d->state == 2) {
            /* CRLF after chunk data (tolerate bare LF). */
            i++;
            if (c == '\r') {
                d->saw_cr = 1;
            } else if (c == '\n') {
                d->state = 0;
                d->saw_cr = 0;
            } else {
                set_err(err, errcap, "missing CRLF after chunk");
                return -1;
            }
        } else if (d->state == 3) {
            /* Trailer section; an empty line ends the body. */
            i++;
            if (c == '\n') {
                if (d->line.len > 0 && d->line.data[d->line.len - 1] == '\r') {
                    d->line.len--;
                    d->line.data[d->line.len] = '\0';
                }
                if (d->line.len == 0) {
                    d->done = 1;
                } else {
                    str_clear(&d->line);
                }
            } else {
                if (d->line.len > 4096) {
                    set_err(err, errcap, "trailer line too long");
                    return -1;
                }
                str_append_char(&d->line, c);
            }
        }
    }
    return 0;
}

int body_decoder_finish(BodyDecoder *d, char *err, size_t errcap)
{
    if (d == NULL) {
        return -1;
    }
    if (d->done) {
        return 0;
    }
    if (d->mode == 2) {
        d->done = 1;
        return 0;
    }
    if (d->mode == 0 && d->remaining == 0) {
        d->done = 1;
        return 0;
    }
    set_err(err, errcap, "connection closed before the HTTP body was complete");
    return -1;
}
