#include "message.h"

void msglist_init(MessageList *list)
{
    if (list != NULL) {
        memset(list, 0, sizeof(*list));
    }
}

static void message_free(Message *m)
{
    if (m == NULL) {
        return;
    }
    a98_free(m->content);
    a98_free(m->tool_calls_json);
    a98_free(m->tool_call_id);
    a98_free(m->name);
    m->content = NULL;
    m->tool_calls_json = NULL;
    m->tool_call_id = NULL;
    m->name = NULL;
}

void msglist_free(MessageList *list)
{
    int i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        message_free(&list->items[i]);
    }
    a98_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void msglist_clear(MessageList *list)
{
    int i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        message_free(&list->items[i]);
    }
    list->count = 0;
}

static Message *msglist_new(MessageList *list)
{
    Message *m;

    if (list == NULL) {
        return NULL;
    }
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 16;
        Message *items = (Message *)a98_realloc(list->items,
                                                (size_t)cap * sizeof(Message));
        if (items == NULL) {
            return NULL;
        }
        list->items = items;
        list->cap = cap;
    }
    m = &list->items[list->count++];
    memset(m, 0, sizeof(*m));
    return m;
}

Message *msglist_add(MessageList *list, MsgRole role, const char *content)
{
    Message *m = msglist_new(list);
    if (m == NULL) {
        return NULL;
    }
    m->role = role;
    m->content = a98_strdup(content ? content : "");
    return m;
}

Message *msglist_add_assistant(MessageList *list, const char *content,
                               const char *tool_calls_json)
{
    Message *m = msglist_new(list);
    if (m == NULL) {
        return NULL;
    }
    m->role = MSG_ASSISTANT;
    m->content = a98_strdup(content ? content : "");
    if (tool_calls_json != NULL && tool_calls_json[0] != '\0') {
        m->tool_calls_json = a98_strdup(tool_calls_json);
    }
    return m;
}

Message *msglist_add_tool(MessageList *list, const char *tool_call_id,
                          const char *name, const char *content)
{
    Message *m = msglist_new(list);
    if (m == NULL) {
        return NULL;
    }
    m->role = MSG_TOOL;
    m->content = a98_strdup(content ? content : "");
    m->tool_call_id = a98_strdup(tool_call_id ? tool_call_id : "");
    m->name = a98_strdup(name ? name : "");
    return m;
}

const char *msg_role_name(MsgRole role)
{
    switch (role) {
    case MSG_SYSTEM:
        return "system";
    case MSG_USER:
        return "user";
    case MSG_ASSISTANT:
        return "assistant";
    case MSG_TOOL:
        return "tool";
    default:
        return "user";
    }
}
