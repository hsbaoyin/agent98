#include "sse.h"

#define SSE_MAX_LINE (256 * 1024)
#define SSE_MAX_DATA (1024 * 1024)

void sse_init(SseParser *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    str_init(&p->line);
    str_init(&p->data);
    str_init(&p->event);
}

void sse_free(SseParser *p)
{
    if (p == NULL) {
        return;
    }
    str_free(&p->line);
    str_free(&p->data);
    str_free(&p->event);
}

static int dispatch(SseParser *p, SseEventFn fn, void *ctx)
{
    int rc = 0;

    if (p->data.len > 0 || p->has_event) {
        if (p->data.len > 0 && p->data.data[p->data.len - 1] == '\n') {
            p->data.len--;
            p->data.data[p->data.len] = '\0';
        }
        rc = fn(ctx, p->has_event ? str_cstr(&p->event) : "message",
                 str_cstr(&p->data));
    }
    str_clear(&p->data);
    str_clear(&p->event);
    p->has_event = 0;
    return rc;
}

static int process_line(SseParser *p, SseEventFn fn, void *ctx)
{
    char *line;
    char *colon;
    size_t len;

    if (p->line.len > 0 && p->line.data[p->line.len - 1] == '\r') {
        p->line.len--;
        p->line.data[p->line.len] = '\0';
    }
    len = p->line.len;
    line = p->line.data;
    if (line == NULL || len == 0) {
        return dispatch(p, fn, ctx);
    }
    if (line[0] == ':') {
        return 0;
    }
    colon = (char *)memchr(line, ':', len);
    if (colon == NULL) {
        /* Field with no value; only "data" is interesting, and it is empty. */
        return 0;
    }
    *colon = '\0';
    {
        const char *value = colon + 1;
        if (*value == ' ') {
            value++;
        }
        if (strcmp(line, "data") == 0) {
            if (p->data.len + strlen(value) + 2 > SSE_MAX_DATA) {
                p->aborted = 1;
                return -1;
            }
            str_append_cstr(&p->data, value);
            str_append_char(&p->data, '\n');
        } else if (strcmp(line, "event") == 0) {
            str_set(&p->event, value);
            p->has_event = 1;
        } else if (strcmp(line, "id") == 0 || strcmp(line, "retry") == 0) {
            /* Not needed for chat streaming. */
        }
    }
    return 0;
}

int sse_feed(SseParser *p, const char *data, size_t len, SseEventFn fn, void *ctx)
{
    size_t i = 0;

    if (p == NULL || fn == NULL) {
        return -1;
    }
    if (p->aborted) {
        return -1;
    }
    while (i < len) {
        char c = data[i++];
        if (c == '\n') {
            int rc = process_line(p, fn, ctx);
            str_clear(&p->line);
            if (rc != 0) {
                p->aborted = 1;
                return -1;
            }
        } else {
            if (p->line.len >= SSE_MAX_LINE) {
                p->aborted = 1;
                return -1;
            }
            str_append_char(&p->line, c);
        }
    }
    return 0;
}

int sse_flush(SseParser *p, SseEventFn fn, void *ctx)
{
    int rc = 0;

    if (p == NULL || fn == NULL) {
        return -1;
    }
    if (p->line.len > 0) {
        rc = process_line(p, fn, ctx);
        str_clear(&p->line);
        if (rc != 0) {
            return -1;
        }
    }
    if (p->data.len > 0 || p->has_event) {
        rc = dispatch(p, fn, ctx);
    }
    return rc;
}
