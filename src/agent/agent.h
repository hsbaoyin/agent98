#ifndef A98_AGENT_H
#define A98_AGENT_H

#include "../core/config.h"
#include "message.h"

typedef enum {
    AGENT_EV_TEXT = 0,      /* streamed assistant text delta */
    AGENT_EV_ROUND,         /* new model round */
    AGENT_EV_TOOL_START,    /* before a tool runs */
    AGENT_EV_TOOL_RESULT,   /* tool output preview */
    AGENT_EV_INFO           /* status/info line */
} AgentEventType;

typedef void (*AgentEventFn)(void *ctx, AgentEventType type, const char *text);

/* Return 1 to allow the tool call, 0 to deny it. */
typedef int (*AgentConfirmFn)(void *ctx, const char *tool, const char *args_json);

typedef struct Agent {
    Config *cfg;
    MessageList messages;
    AgentEventFn on_event;
    void *event_ctx;
    AgentConfirmFn on_confirm;
    void *confirm_ctx;
    volatile int *cancel;
    char workspace[A98_MAX_PATH];
} Agent;

void agent_init(Agent *a, Config *cfg);
void agent_free(Agent *a);
void agent_new_conversation(Agent *a);

int agent_run(Agent *a, const char *user_text, char *err, size_t errcap);

/* Session files are JSON. Returns 0 on success. */
int agent_save_session(Agent *a, const char *path, char *err, size_t errcap);
int agent_load_session(Agent *a, const char *path, char *err, size_t errcap);

/* <exe dir>\sessions\last_session.json */
void agent_last_session_path(char *out, size_t cap);
void agent_ensure_dirs(Agent *a);

#endif /* A98_AGENT_H */
