/*
 * Agent98 - incremental Server-Sent Events parser.
 */
#ifndef A98_SSE_H
#define A98_SSE_H

#include "../core/base.h"
#include "../core/str.h"

/* Return 0 to continue, non-zero to abort parsing. */
typedef int (*SseEventFn)(void *ctx, const char *event, const char *data);

typedef struct {
    Str line;
    Str data;
    Str event;
    int has_event;
    int aborted;
} SseParser;

void sse_init(SseParser *p);
void sse_free(SseParser *p);
int sse_feed(SseParser *p, const char *data, size_t len, SseEventFn fn, void *ctx);
int sse_flush(SseParser *p, SseEventFn fn, void *ctx);

#endif /* A98_SSE_H */
