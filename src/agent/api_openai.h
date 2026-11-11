#ifndef A98_API_OPENAI_H
#define A98_API_OPENAI_H

#include "../core/config.h"
#include "../core/str.h"
#include "message.h"

typedef struct {
    char *id;
    char *name;
    Str arguments;
    int index;
} ToolCall;

typedef struct {
    Str content;
    ToolCall *calls;
    int call_count;
    int call_cap;
    char finish_reason[48];
    int got_done;
    int http_status;
} ChatResult;

void chat_result_init(ChatResult *r);
void chat_result_free(ChatResult *r);

/* Called for each text delta (may be NULL). Return 0 to continue. */
typedef int (*ApiTextFn)(void *ctx, const char *text, size_t len);

/*
 * Perform a chat completion. Handles both streaming (SSE) and non-streaming
 * responses. `system_prompt` may be NULL. On failure returns -1 and fills
 * `err` with a user-presentable message.
 */
int api_chat(const Config *cfg, const char *system_prompt,
             const MessageList *messages, ChatResult *out,
             ApiTextFn on_text, void *ctx, volatile int *cancel,
             char *err, size_t errcap);

/* Build the HTTP header block for a request (including extra headers).
 * Returns a heap string owned by the caller (a98_free) or NULL on OOM. */
char *api_build_headers(const Config *cfg, int stream);

/* Exposed for unit tests: apply one OpenAI streaming JSON chunk. */
int openai_apply_chunk(ChatResult *out, const char *json, size_t len,
                       ApiTextFn on_text, void *ctx, char *err, size_t errcap);

/* Exposed for unit tests: parse a non-streaming response object. */
int openai_apply_full(ChatResult *out, const char *json, size_t len,
                      char *err, size_t errcap);

#endif /* A98_API_OPENAI_H */
