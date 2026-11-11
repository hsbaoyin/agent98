#include "api_openai.h"
#include "tools.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../net/http.h"
#include "../net/sse.h"
#include "../net/url.h"
#include "../../third_party/cjson/cJSON.h"

#include <windows.h>

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

void chat_result_init(ChatResult *r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    str_init(&r->content);
}

void chat_result_free(ChatResult *r)
{
    int i;

    if (r == NULL) {
        return;
    }
    for (i = 0; i < r->call_count; i++) {
        a98_free(r->calls[i].id);
        a98_free(r->calls[i].name);
        str_free(&r->calls[i].arguments);
    }
    a98_free(r->calls);
    r->calls = NULL;
    r->call_count = 0;
    r->call_cap = 0;
    str_free(&r->content);
}

static ToolCall *result_call_for_index(ChatResult *r, int index)
{
    int i;

    for (i = 0; i < r->call_count; i++) {
        if (r->calls[i].index == index) {
            return &r->calls[i];
        }
    }
    if (r->call_count == r->call_cap) {
        int cap = r->call_cap ? r->call_cap * 2 : 4;
        ToolCall *calls = (ToolCall *)a98_realloc(r->calls,
                                                  (size_t)cap * sizeof(ToolCall));
        if (calls == NULL) {
            return NULL;
        }
        r->calls = calls;
        r->call_cap = cap;
    }
    memset(&r->calls[r->call_count], 0, sizeof(ToolCall));
    r->calls[r->call_count].index = index;
    str_init(&r->calls[r->call_count].arguments);
    return &r->calls[r->call_count++];
}

static void append_dup(char **dst, const char *src)
{
    char *joined;

    if (src == NULL) {
        return;
    }
    if (*dst == NULL) {
        *dst = a98_strdup(src);
        return;
    }
    joined = a98_strfmt("%s%s", *dst, src);
    if (joined != NULL) {
        a98_free(*dst);
        *dst = joined;
    }
}

static int apply_error_object(cJSON *root, char *err, size_t errcap)
{
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const char *msg = NULL;

    if (error == NULL) {
        return 0;
    }
    if (cJSON_IsString(error)) {
        msg = error->valuestring;
    } else if (cJSON_IsObject(error)) {
        msg = json_string(error, "message");
        if (msg == NULL) {
            msg = json_string(error, "type");
        }
    }
    if (msg == NULL) {
        msg = "provider returned an unknown error";
    }
    set_err(err, errcap, "API error: %s", msg);
    return -1;
}

static int apply_tool_calls_array(ChatResult *out, cJSON *calls, int delta_mode,
                                  char *err, size_t errcap)
{
    cJSON *item;
    int i = 0;

    if (calls == NULL || !cJSON_IsArray(calls)) {
        return 0;
    }
    cJSON_ArrayForEach(item, calls) {
        int index = json_int(item, "index", i);
        const char *id = json_string(item, "id");
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(item, "function");
        const char *name = json_string(fn, "name");
        const char *args = json_string(fn, "arguments");
        ToolCall *call;

        if (out->call_count >= 32) {
            set_err(err, errcap, "model returned too many tool calls");
            return -1;
        }
        if (!delta_mode) {
            /* Complete calls: always create a new entry. */
            index = out->call_count;
        }
        call = result_call_for_index(out, index);
        if (call == NULL) {
            set_err(err, errcap, "out of memory");
            return -1;
        }
        if (id != NULL && id[0] != '\0') {
            if (call->id == NULL) {
                call->id = a98_strdup(id);
            } else if (delta_mode) {
                append_dup(&call->id, id);
            }
        }
        if (name != NULL && name[0] != '\0') {
            if (call->name == NULL) {
                call->name = a98_strdup(name);
            } else if (delta_mode) {
                append_dup(&call->name, name);
            }
        }
        if (args != NULL && args[0] != '\0') {
            str_append_cstr(&call->arguments, args);
        }
        i++;
    }
    return 0;
}

int openai_apply_chunk(ChatResult *out, const char *json, size_t len,
                       ApiTextFn on_text, void *ctx, char *err, size_t errcap)
{
    cJSON *root;
    cJSON *choices;
    cJSON *choice;
    cJSON *delta;
    cJSON *content;
    cJSON *calls;
    const char *finish;

    if (out == NULL) {
        return -1;
    }
    root = json_parse(json, len);
    if (root == NULL) {
        set_err(err, errcap, "provider sent invalid JSON in the stream");
        return -1;
    }
    if (apply_error_object(root, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return 0;
    }
    choice = cJSON_GetArrayItem(choices, 0);
    delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    if (delta != NULL) {
        content = cJSON_GetObjectItemCaseSensitive(delta, "content");
        if (cJSON_IsString(content) && content->valuestring != NULL &&
            content->valuestring[0] != '\0') {
            size_t n = strlen(content->valuestring);
            if (str_append(&out->content, content->valuestring, n) != 0) {
                cJSON_Delete(root);
                set_err(err, errcap, "out of memory");
                return -1;
            }
            if (on_text != NULL && on_text(ctx, content->valuestring, n) != 0) {
                cJSON_Delete(root);
                set_err(err, errcap, "response display aborted");
                return -1;
            }
        }
        calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
        if (apply_tool_calls_array(out, calls, 1, err, errcap) != 0) {
            cJSON_Delete(root);
            return -1;
        }
    }
    finish = json_string(choice, "finish_reason");
    if (finish != NULL) {
        strncpy(out->finish_reason, finish, sizeof(out->finish_reason) - 1);
        out->finish_reason[sizeof(out->finish_reason) - 1] = '\0';
    }
    cJSON_Delete(root);
    return 0;
}

int openai_apply_full(ChatResult *out, const char *json, size_t len,
                      char *err, size_t errcap)
{
    cJSON *root;
    cJSON *choices;
    cJSON *choice;
    cJSON *message;
    cJSON *content;
    cJSON *calls;
    const char *finish;
    const char *text;

    if (out == NULL) {
        return -1;
    }
    root = json_parse(json, len);
    if (root == NULL) {
        set_err(err, errcap, "provider sent invalid JSON");
        return -1;
    }
    if (apply_error_object(root, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        set_err(err, errcap, "provider response has no choices");
        return -1;
    }
    choice = cJSON_GetArrayItem(choices, 0);
    message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (message == NULL) {
        cJSON_Delete(root);
        set_err(err, errcap, "provider response has no message");
        return -1;
    }
    content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        str_append_cstr(&out->content, content->valuestring);
    }
    calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (apply_tool_calls_array(out, calls, 0, err, errcap) != 0) {
        cJSON_Delete(root);
        return -1;
    }
    finish = json_string(choice, "finish_reason");
    if (finish != NULL) {
        strncpy(out->finish_reason, finish, sizeof(out->finish_reason) - 1);
        out->finish_reason[sizeof(out->finish_reason) - 1] = '\0';
    }
    text = json_string(root, "model");
    if (text != NULL) {
        log_debug("response model: %s", text);
    }
    cJSON_Delete(root);
    return 0;
}

static cJSON *message_to_json(const Message *m)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *content_item;

    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "role", msg_role_name(m->role));
    if (m->role == MSG_ASSISTANT) {
        if (m->content != NULL && m->content[0] != '\0') {
            cJSON_AddStringToObject(obj, "content", m->content);
        } else {
            cJSON_AddNullToObject(obj, "content");
        }
        if (m->tool_calls_json != NULL && m->tool_calls_json[0] != '\0') {
            cJSON *calls = cJSON_Parse(m->tool_calls_json);
            if (calls != NULL) {
                cJSON_AddItemToObject(obj, "tool_calls", calls);
            }
        }
    } else if (m->role == MSG_TOOL) {
        cJSON_AddStringToObject(obj, "tool_call_id",
                                m->tool_call_id ? m->tool_call_id : "");
        cJSON_AddStringToObject(obj, "content", m->content ? m->content : "");
        if (m->name != NULL && m->name[0] != '\0') {
            cJSON_AddStringToObject(obj, "name", m->name);
        }
    } else {
        content_item = cJSON_CreateString(m->content ? m->content : "");
        if (content_item != NULL) {
            cJSON_AddItemToObject(obj, "content", content_item);
        }
    }
    return obj;
}

static cJSON *build_tools(void)
{
    const ToolDef *defs;
    int count = 0;
    int i;
    cJSON *tools;

    if (tools_all(&count) == NULL || count == 0) {
        return NULL;
    }
    defs = tools_all(&count);
    tools = cJSON_CreateArray();
    if (tools == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON *fn = cJSON_CreateObject();
        cJSON *params;

        if (tool == NULL || fn == NULL) {
            cJSON_Delete(tool);
            cJSON_Delete(fn);
            continue;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(fn, "name", defs[i].name);
        cJSON_AddStringToObject(fn, "description", defs[i].description);
        params = cJSON_Parse(defs[i].params_json ? defs[i].params_json : "{}");
        if (params == NULL) {
            params = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(fn, "parameters", params);
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(tools, tool);
    }
    return tools;
}

static char *build_request_json(const Config *cfg, const char *system_prompt,
                                const MessageList *messages,
                                char *err, size_t errcap)
{
    cJSON *root;
    cJSON *arr;
    cJSON *tools;
    int i;
    char *text;

    root = cJSON_CreateObject();
    if (root == NULL) {
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", cfg->model);
    cJSON_AddNumberToObject(root, "max_tokens", (double)cfg->max_tokens);
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    cJSON_AddBoolToObject(root, "stream", cfg->stream ? 1 : 0);
    if (cfg->stream) {
        cJSON *opts = cJSON_AddObjectToObject(root, "stream_options");
        if (opts != NULL) {
            cJSON_AddBoolToObject(opts, "include_usage", 0);
        }
    }

    arr = cJSON_AddArrayToObject(root, "messages");
    if (arr == NULL) {
        cJSON_Delete(root);
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(arr, sys);
    }
    for (i = 0; i < messages->count; i++) {
        cJSON *obj = message_to_json(&messages->items[i]);
        if (obj != NULL) {
            cJSON_AddItemToArray(arr, obj);
        }
    }

    tools = build_tools();
    if (tools != NULL) {
        cJSON_AddItemToObject(root, "tools", tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        set_err(err, errcap, "out of memory while encoding the request");
    }
    return text;
}

char *api_build_headers(const Config *cfg, int stream)
{
    Str h;
    const char *p;

    str_init(&h);
    if (cfg->api_key[0] != '\0') {
        str_appendf(&h, "Authorization: Bearer %s\r\n", cfg->api_key);
    }
    str_append_cstr(&h, "Content-Type: application/json\r\n");
    str_appendf(&h, "Accept: %s\r\n",
                stream ? "text/event-stream" : "application/json");
    if (cfg->extra_headers[0] != '\0') {
        p = cfg->extra_headers;
        while (*p != '\0') {
            const char *start = p;
            size_t len;
            while (*p != '\0' && *p != '|' && *p != '\n') {
                p++;
            }
            len = (size_t)(p - start);
            while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == ' ')) {
                len--;
            }
            while (len > 0 && (*start == ' ' || *start == '\t')) {
                start++;
                len--;
            }
            if (len > 0) {
                str_append(&h, start, len);
                str_append_cstr(&h, "\r\n");
            }
            if (*p == '|' || *p == '\n') {
                p++;
            }
        }
    }
    return str_detach(&h);
}

static int make_endpoint(const char *base, Url *url, char *err, size_t errcap)
{
    char tmp[900];
    size_t len;
    const char *suffix = "/chat/completions";

    if (base == NULL || base[0] == '\0') {
        set_err(err, errcap, "API base URL is empty");
        return -1;
    }
    if (strlen(base) >= sizeof(tmp)) {
        set_err(err, errcap, "API base URL is too long");
        return -1;
    }
    strcpy(tmp, base);
    len = strlen(tmp);
    while (len > 9 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    if (len < strlen(suffix) || a98_strcasecmp(tmp + len - strlen(suffix), suffix) != 0) {
        if (len + strlen(suffix) + 1 > sizeof(tmp)) {
            set_err(err, errcap, "API base URL is too long");
            return -1;
        }
        strcat(tmp, suffix);
    }
    if (url_parse(tmp, url, err, errcap) != 0) {
        return -1;
    }
    return 0;
}

typedef struct {
    NetConn *conn;
    int timeout_sec;
    const HttpResponse *resp;
    int streaming;
    ChatResult *result;
    ApiTextFn on_text;
    void *user;
    SseParser sse;
    Str raw;
    Str error_body;
    int failed;
    char err[320];
} SinkCtx;

static int sse_callback(void *vctx, const char *event, const char *data)
{
    SinkCtx *s = (SinkCtx *)vctx;
    size_t len;

    A98_UNUSED(event);
    if (s->failed) {
        return -1;
    }
    if (data == NULL) {
        return 0;
    }
    len = strlen(data);
    if (strcmp(data, "[DONE]") == 0) {
        s->result->got_done = 1;
        return 0;
    }
    if (openai_apply_chunk(s->result, data, len, s->on_text, s->user,
                           s->err, sizeof(s->err)) != 0) {
        s->failed = 1;
        return -1;
    }
    return 0;
}

static int body_sink(void *vctx, const char *data, size_t len)
{
    SinkCtx *s = (SinkCtx *)vctx;

    if (s->conn != NULL && s->timeout_sec > 0) {
        net_set_deadline(s->conn, (unsigned long)s->timeout_sec);
    }
    if (s->resp != NULL && s->resp->status != 200) {
        if (s->error_body.len < 64 * 1024) {
            return str_append(&s->error_body, data, len);
        }
        return 0;
    }
    if (s->streaming) {
        if (sse_feed(&s->sse, data, len, sse_callback, s) != 0) {
            if (!s->failed) {
                set_err(s->err, sizeof(s->err), "invalid SSE stream");
                s->failed = 1;
            }
            return -1;
        }
        return 0;
    }
    if (s->raw.len + len > 4 * 1024 * 1024) {
        set_err(s->err, sizeof(s->err), "response body is too large");
        s->failed = 1;
        return -1;
    }
    return str_append(&s->raw, data, len);
}

static void api_error_from_body(int status, const char *body, size_t len,
                                char *err, size_t errcap)
{
    char detail[256];
    const char *msg = NULL;
    cJSON *root;

    detail[0] = '\0';
    if (len > 0) {
        root = json_parse(body, len);
        if (root != NULL) {
            cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (cJSON_IsObject(e)) {
                msg = json_string(e, "message");
            } else if (cJSON_IsString(e)) {
                msg = e->valuestring;
            }
            if (msg == NULL) {
                msg = json_string(root, "message");
            }
            if (msg != NULL) {
                strncpy(detail, msg, sizeof(detail) - 1);
                detail[sizeof(detail) - 1] = '\0';
            }
            cJSON_Delete(root);
        }
        if (detail[0] == '\0') {
            size_t n = len;
            size_t i;
            if (n >= sizeof(detail)) {
                n = sizeof(detail) - 1;
            }
            for (i = 0; i < n; i++) {
                char c = body[i];
                if (c == '\r' || c == '\n' || c == '\t') {
                    c = ' ';
                }
                detail[i] = c;
            }
            detail[n] = '\0';
            config_trim(detail);
        }
    }
    if (status == 401) {
        set_err(err, errcap, "HTTP 401 Unauthorized - check the API key%s%s",
                detail[0] ? ": " : "", detail);
    } else if (status == 404) {
        set_err(err, errcap, "HTTP 404 Not Found - check the API base URL and model%s%s",
                detail[0] ? ": " : "", detail);
    } else if (status == 429) {
        set_err(err, errcap, "HTTP 429 rate limited%s%s",
                detail[0] ? ": " : "", detail);
    } else {
        set_err(err, errcap, "HTTP %d%s%s", status,
                detail[0] ? ": " : "", detail);
    }
}

int api_chat(const Config *cfg, const char *system_prompt,
             const MessageList *messages, ChatResult *out,
             ApiTextFn on_text, void *ctx, volatile int *cancel,
             char *err, size_t errcap)
{
    Url url;
    char headers[2048];
    char *req = NULL;
    char *extra = NULL;
    NetConn *conn = NULL;
    HttpResponse resp;
    SinkCtx sink;
    int rc = -1;
    int stream;

    if (cfg == NULL || messages == NULL || out == NULL) {
        return -1;
    }
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (make_endpoint(cfg->api_base, &url, err, errcap) != 0) {
        return -1;
    }
    if (cfg->api_key[0] == '\0') {
        log_warn("API key is empty; only local/unauthorised endpoints will work");
    }
    req = build_request_json(cfg, system_prompt, messages, err, errcap);
    if (req == NULL) {
        return -1;
    }
    extra = api_build_headers(cfg, cfg->stream);
    if (extra == NULL) {
        set_err(err, errcap, "out of memory");
        goto done;
    }
    snprintf(headers, sizeof(headers), "%s", extra);

    if (cancel != NULL && *cancel) {
        set_err(err, errcap, "cancelled by user");
        goto done;
    }
    conn = net_open(&url, cfg->tls_verify, cfg->connect_timeout_sec,
                    cfg->request_timeout_sec, err, errcap);
    if (conn == NULL) {
        goto done;
    }
    net_set_cancel(conn, cancel);

    stream = cfg->stream;
    memset(&sink, 0, sizeof(sink));
    sink.conn = conn;
    sink.timeout_sec = cfg->request_timeout_sec;
    sink.resp = &resp;
    sink.streaming = stream;
    sink.result = out;
    sink.on_text = on_text;
    sink.user = ctx;
    str_init(&sink.sse.line);
    str_init(&sink.sse.data);
    str_init(&sink.sse.event);
    str_init(&sink.raw);
    str_init(&sink.error_body);
    memset(&resp, 0, sizeof(resp));

    if (http_request(conn, "POST", &url, headers, req, strlen(req), &resp,
                     body_sink, &sink, err, errcap) != 0) {
        if (sink.failed) {
            set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
        } else if (err != NULL && errcap > 0 && err[0] == '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        }
        goto done_sink;
    }
    out->http_status = resp.status;
    if (resp.status != 200) {
        api_error_from_body(resp.status, sink.error_body.data,
                            sink.error_body.len, err, errcap);
        goto done_sink;
    }
    if (stream) {
        if (sse_flush(&sink.sse, sse_callback, &sink) != 0) {
            if (sink.failed) {
                set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
            }
            goto done_sink;
        }
        if (sink.failed) {
            set_err(err, errcap, "%s", sink.err[0] ? sink.err : "stream failed");
            goto done_sink;
        }
        if (out->content.len == 0 && out->call_count == 0) {
            set_err(err, errcap, "stream ended without any content or tool calls");
            goto done_sink;
        }
    } else {
        if (sink.raw.len == 0) {
            set_err(err, errcap, "provider returned an empty response");
            goto done_sink;
        }
        if (openai_apply_full(out, sink.raw.data, sink.raw.len, err, errcap) != 0) {
            goto done_sink;
        }
        /* Non-streaming responses are parsed in one go, so deliver the
         * text here just like the SSE path does. */
        if (on_text != NULL && out->content.len > 0) {
            if (on_text(ctx, out->content.data, out->content.len) != 0) {
                set_err(err, errcap, "response display aborted");
                goto done_sink;
            }
        }
    }
    rc = 0;

done_sink:
    sse_free(&sink.sse);
    str_free(&sink.raw);
    str_free(&sink.error_body);
done:
    if (conn != NULL) {
        net_close(conn);
    }
    a98_free(extra);
    if (req != NULL) {
        cJSON_free(req);
    }
    return rc;
}
