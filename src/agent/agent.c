#include "agent.h"
#include "api_openai.h"
#include "tools.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../../third_party/cjson/cJSON.h"

#define MAX_TOOL_OUTPUT 16384

static void emit(Agent *a, AgentEventType type, const char *text)
{
    if (a->on_event != NULL) {
        a->on_event(a->event_ctx, type, text ? text : "");
    }
}

static void emit_delta(Agent *a, const char *text, size_t len)
{
    char *copy;

    if (a->on_event == NULL || len == 0) {
        return;
    }
    copy = a98_strndup(text, len);
    if (copy != NULL) {
        emit(a, AGENT_EV_TEXT, copy);
        a98_free(copy);
    }
}

static int text_cb(void *ctx, const char *text, size_t len)
{
    Agent *a = (Agent *)ctx;
    if (a->cancel != NULL && *a->cancel) {
        return -1;
    }
    emit_delta(a, text, len);
    return 0;
}

void agent_init(Agent *a, Config *cfg)
{
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->cfg = cfg;
    msglist_init(&a->messages);
    if (cfg != NULL) {
        strncpy(a->workspace, cfg->workspace, sizeof(a->workspace) - 1);
        a->workspace[sizeof(a->workspace) - 1] = '\0';
    }
    agent_ensure_dirs(a);
}

void agent_free(Agent *a)
{
    if (a == NULL) {
        return;
    }
    msglist_free(&a->messages);
}

void agent_new_conversation(Agent *a)
{
    if (a != NULL) {
        msglist_clear(&a->messages);
    }
}

void agent_ensure_dirs(Agent *a)
{
    char sessions[A98_MAX_PATH];

    if (a == NULL) {
        return;
    }
    if (a->workspace[0] != '\0') {
        a98_path_mkdirs(a->workspace);
    }
    if (a98_path_exe_dir(sessions, sizeof(sessions)) == 0) {
        char dir[A98_MAX_PATH];
        a98_path_join(dir, sizeof(dir), sessions, "sessions");
        a98_path_mkdirs(dir);
    }
}

void agent_last_session_path(char *out, size_t cap)
{
    char dir[A98_MAX_PATH];
    char sessions[A98_MAX_PATH];

    if (out == NULL || cap == 0) {
        return;
    }
    if (a98_path_exe_dir(dir, sizeof(dir)) != 0) {
        strcpy(dir, ".");
    }
    a98_path_join(sessions, sizeof(sessions), dir, "sessions");
    a98_path_join(out, cap, sessions, "last_session.json");
}

static char *calls_to_json(const ChatResult *res)
{
    cJSON *arr;
    int i;
    char *text;

    if (res->call_count == 0) {
        return NULL;
    }
    arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < res->call_count; i++) {
        const ToolCall *c = &res->calls[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        char idbuf[64];

        if (obj == NULL || fn == NULL) {
            cJSON_Delete(obj);
            cJSON_Delete(fn);
            continue;
        }
        if (c->id != NULL && c->id[0] != '\0') {
            cJSON_AddStringToObject(obj, "id", c->id);
        } else {
            sprintf(idbuf, "call_%d", i);
            cJSON_AddStringToObject(obj, "id", idbuf);
        }
        cJSON_AddStringToObject(obj, "type", "function");
        cJSON_AddStringToObject(fn, "name", c->name ? c->name : "");
        cJSON_AddStringToObject(fn, "arguments",
                                c->arguments.data ? c->arguments.data : "{}");
        cJSON_AddItemToObject(obj, "function", fn);
        cJSON_AddItemToArray(arr, obj);
    }
    text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return text;
}

static void truncate_output(Str *out)
{
    if (out->len > MAX_TOOL_OUTPUT) {
        out->len = MAX_TOOL_OUTPUT;
        if (out->data != NULL) {
            out->data[out->len] = '\0';
        }
        str_append_cstr(out, "\n[tool output truncated]");
    }
}

static int tool_needs_confirm(const char *name)
{
    const ToolDef *def = tools_find(name);
    return def != NULL && def->needs_confirm;
}

int agent_run(Agent *a, const char *user_text, char *err, size_t errcap)
{
    int step;

    if (a == NULL || a->cfg == NULL || user_text == NULL) {
        return -1;
    }
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (a->workspace[0] == '\0') {
        strncpy(a->workspace, a->cfg->workspace, sizeof(a->workspace) - 1);
        a->workspace[sizeof(a->workspace) - 1] = '\0';
    }
    agent_ensure_dirs(a);
    if (msglist_add(&a->messages, MSG_USER, user_text) == NULL) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }

    for (step = 0; step < a->cfg->max_steps; step++) {
        ChatResult res;
        char *calls_json = NULL;
        char roundbuf[64];
        char *calls_text = NULL;
        int rc;

        if (a->cancel != NULL && *a->cancel) {
            snprintf(err, errcap, "cancelled by user");
            return -1;
        }
        sprintf(roundbuf, "Thinking (round %d/%d)...", step + 1, a->cfg->max_steps);
        emit(a, AGENT_EV_ROUND, roundbuf);
        chat_result_init(&res);

        rc = api_chat(a->cfg, a->cfg->system_prompt, &a->messages, &res,
                      text_cb, a, a->cancel, err, errcap);
        if (rc != 0) {
            chat_result_free(&res);
            if (err != NULL && errcap > 0 && err[0] == '\0') {
                snprintf(err, errcap, "API request failed");
            }
            return -1;
        }

        calls_json = calls_to_json(&res);
        if (msglist_add_assistant(&a->messages,
                                  res.content.data ? res.content.data : "",
                                  calls_json) == NULL) {
            a98_free(calls_json);
            chat_result_free(&res);
            snprintf(err, errcap, "out of memory");
            return -1;
        }
        a98_free(calls_json);

        if (res.call_count == 0) {
            if (res.content.len == 0) {
                chat_result_free(&res);
                snprintf(err, errcap, "model returned an empty response");
                return -1;
            }
            chat_result_free(&res);
            return 0;
        }

        calls_text = calls_to_json(&res);
        if (calls_text != NULL) {
            log_debug("tool calls: %s", calls_text);
            cJSON_free(calls_text);
        }

        {
            int i;
            for (i = 0; i < res.call_count; i++) {
                ToolCall *c = &res.calls[i];
                const char *name = c->name ? c->name : "";
                const char *args = c->arguments.data ? c->arguments.data : "{}";
                Str out;
                char tool_err[512];
                char *summary;
                int allowed = 1;

                if (a->cancel != NULL && *a->cancel) {
                    chat_result_free(&res);
                    snprintf(err, errcap, "cancelled by user");
                    return -1;
                }
                summary = a98_strfmt("%s(%s)", name, args);
                emit(a, AGENT_EV_TOOL_START, summary ? summary : name);
                a98_free(summary);

                if (tool_needs_confirm(name) && a->on_confirm != NULL) {
                    allowed = a->on_confirm(a->confirm_ctx, name, args);
                }
                str_init(&out);
                tool_err[0] = '\0';
                if (!allowed) {
                    str_append_cstr(&out, "Tool execution denied by the user.");
                } else if (tools_execute(a, name, args, &out, tool_err,
                                         sizeof(tool_err)) != 0) {
                    str_appendf(&out, "Tool error: %s", tool_err);
                }
                truncate_output(&out);
                emit(a, AGENT_EV_TOOL_RESULT, str_cstr(&out));
                if (msglist_add_tool(&a->messages,
                                     c->id ? c->id : "",
                                     name, str_cstr(&out)) == NULL) {
                    str_free(&out);
                    chat_result_free(&res);
                    snprintf(err, errcap, "out of memory");
                    return -1;
                }
                str_free(&out);
            }
        }
        chat_result_free(&res);
    }

    snprintf(err, errcap, "stopped after %d tool rounds (max_steps)", a->cfg->max_steps);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

static char *read_file_all(const char *path, size_t max_len, size_t *out_len)
{
    FILE *f;
    long size;
    char *buf;
    size_t n;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if ((size_t)size > max_len) {
        size = (long)max_len;
    }
    rewind(f);
    buf = (char *)a98_alloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

int agent_save_session(Agent *a, const char *path, char *err, size_t errcap)
{
    cJSON *root;
    cJSON *arr;
    char *text;
    char tmp[A98_MAX_PATH + 8];
    FILE *f;
    int i;
    int ok = 1;

    if (a == NULL || path == NULL) {
        return -1;
    }
    root = cJSON_CreateObject();
    if (root == NULL) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }
    cJSON_AddNumberToObject(root, "version", 1);
    {
        char timebuf[64];
        sprintf(timebuf, "%ld", (long)time(NULL));
        cJSON_AddStringToObject(root, "saved_at", timebuf);
    }
    arr = cJSON_AddArrayToObject(root, "messages");
    for (i = 0; i < a->messages.count; i++) {
        Message *m = &a->messages.items[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON *tc;
        if (obj == NULL) {
            continue;
        }
        cJSON_AddStringToObject(obj, "role", msg_role_name(m->role));
        if (m->content != NULL) {
            cJSON_AddStringToObject(obj, "content", m->content);
        }
        if (m->tool_calls_json != NULL) {
            tc = cJSON_Parse(m->tool_calls_json);
            if (tc != NULL) {
                cJSON_AddItemToObject(obj, "tool_calls", tc);
            }
        }
        if (m->tool_call_id != NULL) {
            cJSON_AddStringToObject(obj, "tool_call_id", m->tool_call_id);
        }
        if (m->name != NULL) {
            cJSON_AddStringToObject(obj, "name", m->name);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        snprintf(err, errcap, "could not encode the session");
        return -1;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) {
        cJSON_free(text);
        snprintf(err, errcap, "cannot write %s", tmp);
        return -1;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        ok = 0;
    }
    fclose(f);
    cJSON_free(text);
    if (!ok) {
        remove(tmp);
        snprintf(err, errcap, "failed to write the session file");
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errcap, "cannot replace %s", path);
        return -1;
    }
    log_info("session saved: %s (%d messages)", path, a->messages.count);
    return 0;
}

int agent_load_session(Agent *a, const char *path, char *err, size_t errcap)
{
    size_t len = 0;
    char *text;
    cJSON *root;
    cJSON *arr;
    cJSON *item;

    if (a == NULL || path == NULL) {
        return -1;
    }
    text = read_file_all(path, 8 * 1024 * 1024, &len);
    if (text == NULL) {
        snprintf(err, errcap, "cannot read %s", path);
        return -1;
    }
    root = json_parse(text, len);
    a98_free(text);
    if (root == NULL) {
        snprintf(err, errcap, "session file is not valid JSON");
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        snprintf(err, errcap, "session file has no messages array");
        return -1;
    }
    msglist_clear(&a->messages);
    cJSON_ArrayForEach(item, arr) {
        const char *role = json_string(item, "role");
        const char *content = json_string(item, "content");
        const char *tc_id = json_string(item, "tool_call_id");
        const char *name = json_string(item, "name");
        cJSON *tc = cJSON_GetObjectItemCaseSensitive(item, "tool_calls");
        char *tc_text = NULL;

        if (tc != NULL) {
            tc_text = cJSON_PrintUnformatted(tc);
        }
        if (role == NULL) {
            a98_free(tc_text);
            continue;
        }
        if (strcmp(role, "user") == 0) {
            msglist_add(&a->messages, MSG_USER, content ? content : "");
        } else if (strcmp(role, "assistant") == 0) {
            msglist_add_assistant(&a->messages, content ? content : "", tc_text);
        } else if (strcmp(role, "tool") == 0) {
            msglist_add_tool(&a->messages, tc_id ? tc_id : "",
                             name ? name : "", content ? content : "");
        } else if (strcmp(role, "system") == 0) {
            /* The system prompt lives in the config, not in history. */
        }
        a98_free(tc_text);
    }
    cJSON_Delete(root);
    log_info("session loaded: %s (%d messages)", path, a->messages.count);
    return 0;
}
