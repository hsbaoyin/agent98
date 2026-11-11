#ifndef A98_MESSAGE_H
#define A98_MESSAGE_H

#include "../core/base.h"

typedef enum {
    MSG_SYSTEM = 0,
    MSG_USER,
    MSG_ASSISTANT,
    MSG_TOOL
} MsgRole;

typedef struct {
    MsgRole role;
    char *content;          /* UTF-8, may be NULL/empty */
    char *tool_calls_json;  /* raw OpenAI tool_calls array, or NULL */
    char *tool_call_id;     /* for MSG_TOOL */
    char *name;             /* tool name / optional name */
} Message;

typedef struct {
    Message *items;
    int count;
    int cap;
} MessageList;

void msglist_init(MessageList *list);
void msglist_free(MessageList *list);
void msglist_clear(MessageList *list);

Message *msglist_add(MessageList *list, MsgRole role, const char *content);
Message *msglist_add_assistant(MessageList *list, const char *content,
                               const char *tool_calls_json);
Message *msglist_add_tool(MessageList *list, const char *tool_call_id,
                          const char *name, const char *content);

const char *msg_role_name(MsgRole role);

#endif /* A98_MESSAGE_H */
