/*
 * Agent98 - incremental HTTP body decoding (Content-Length, chunked, EOF).
 */
#ifndef A98_BODY_H
#define A98_BODY_H

#include "../core/base.h"
#include "../core/str.h"

typedef int (*BodySink)(void *ctx, const char *data, size_t len);

typedef struct {
    int mode; /* 0 = content-length, 1 = chunked, 2 = until EOF */
    int state;
    int done;
    int saw_cr;
    unsigned long remaining;
    unsigned long total;
    Str line;
} BodyDecoder;

void body_decoder_init(BodyDecoder *d, int chunked, unsigned long content_length,
                       int until_eof);
void body_decoder_free(BodyDecoder *d);

/* Feed raw bytes. Returns 0 on success, -1 on framing error or sink error. */
int body_decoder_feed(BodyDecoder *d, const char *data, size_t len,
                      BodySink sink, void *ctx, char *err, size_t errcap);

/* Call when the network stream ends. Returns 0 when the body was complete. */
int body_decoder_finish(BodyDecoder *d, char *err, size_t errcap);

#endif /* A98_BODY_H */
