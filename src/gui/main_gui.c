/*
 * Agent98 - native Win32 GUI (Windows 95/98/ME compatible).
 *
 * The agent runs on a worker thread.  It reports streamed text and status
 * through PostMessage() to the UI thread; tool confirmations are handed to
 * the UI thread with a modal dialog and an event the worker waits on.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __WATCOMC__
/* The OpenWatcom runtime needs its thread bookkeeping (per-thread data,
 * heap arena, errno) before any C library call is made on a new thread.
 * CreateThread() skips that setup, so use _beginthreadex() instead. */
#include <process.h>
#endif

#include "resource.h"

#include "../core/base.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/path.h"
#include "../core/str.h"
#include "../core/utf8.h"
#include "../agent/agent.h"
#include "../agent/api_openai.h"
#include "../net/http.h"
#include "../net/net.h"
#include "../net/url.h"

#ifndef EM_EXLIMITTEXT
#define EM_EXLIMITTEXT (WM_USER + 37)
#endif
#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#endif

#define A98_WM_EVENT   (WM_APP + 1)
#define A98_WM_DONE    (WM_APP + 2)
#define A98_WM_CONFIRM (WM_APP + 3)

#define A98_CLASS_NAME "Agent98Window"

#define RICH_CLASS20 "RichEdit20A"
#define RICH_CLASS10 "RichEdit"

#define TRANSCRIPT_LIMIT 0x7FFFFFFEL

typedef struct {
    int type;
    char *text;
} UiEvent;

typedef struct {
    char tool[64];
    char *args;
    HANDLE event;
    int result;
} ConfirmReq;

typedef struct {
    HINSTANCE hinst;
    HWND hwnd;
    HWND transcript;
    HWND input;
    HWND send;
    HWND stop;
    HWND newbtn;
    HWND settings;
    HWND status;
    HFONT font;
    HACCEL accel;

    int rich;               /* 0 = EDIT, 1 = RichEdit 1.0, 2 = RichEdit 2.0 */
    HINSTANCE rich_dll;

    Config cfg;
    char ini_path[A98_MAX_PATH];
    char exe_dir[A98_MAX_PATH];
    Agent agent;

    HANDLE thread;
    DWORD thread_id;
    char *pending_input;
    volatile int cancel;
    volatile int running;
    int closing;
} App;

static App g_app;
static WNDPROC g_input_oldproc;
static App *g_dlg_app;
static const char *g_confirm_tool;
static const char *g_confirm_args;

typedef struct {
    const char *name;
    const char *base;
    const char *model;
} ProviderPreset;

static const ProviderPreset g_presets[] = {
    { "OpenAI",          "https://api.openai.com/v1",       "gpt-4o-mini" },
    { "DeepSeek",        "https://api.deepseek.com/v1",     "deepseek-chat" },
    { "Moonshot (Kimi)", "https://api.moonshot.cn/v1",      "moonshot-v1-8k" },
    { "SiliconFlow",     "https://api.siliconflow.cn/v1",   "Qwen/Qwen2.5-7B-Instruct" },
    { "Ollama (local)",  "http://127.0.0.1:11434/v1",       "llama3.2" },
    { "Custom",          "",                                "" }
};

#define PRESET_COUNT ((int)(sizeof(g_presets) / sizeof(g_presets[0])))

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static BOOL CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static BOOL CALLBACK confirm_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static BOOL CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI worker_proc(void *param);
/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int base_matches(const char *a, const char *b)
{
    char ta[512];
    char tb[512];
    size_t la;
    size_t lb;

    copy_str(ta, sizeof(ta), a);
    copy_str(tb, sizeof(tb), b);
    config_trim(ta);
    config_trim(tb);
    la = strlen(ta);
    lb = strlen(tb);
    while (la > 0 && ta[la - 1] == '/') {
        ta[--la] = '\0';
    }
    while (lb > 0 && tb[lb - 1] == '/') {
        tb[--lb] = '\0';
    }
    return a98_strcasecmp(ta, tb) == 0;
}

static int preset_from_base(const char *base)
{
    int i;

    for (i = 0; i + 1 < PRESET_COUNT; i++) {
        if (base_matches(base, g_presets[i].base)) {
            return i;
        }
    }
    return PRESET_COUNT - 1;
}

static void center_window(HWND hwnd)
{
    RECT r;
    RECT ro;
    HWND owner;
    int w;
    int h;

    GetWindowRect(hwnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    owner = GetParent(hwnd);
    if (owner == NULL) {
        owner = GetDesktopWindow();
    }
    GetWindowRect(owner, &ro);
    MoveWindow(hwnd,
               ro.left + ((ro.right - ro.left) - w) / 2,
               ro.top + ((ro.bottom - ro.top) - h) / 2,
               w, h, FALSE);
}

static void msgbox_utf8(HWND owner, const char *utf8, UINT flags)
{
    char *acp = a98_utf8_to_acp(utf8 ? utf8 : "");

    MessageBoxA(owner, acp != NULL ? acp : "", "Agent98", flags);
    a98_free(acp);
}

static char *window_text_utf8(HWND h)
{
    int len;
    char *buf;
    char *utf8;

    len = GetWindowTextLengthA(h);
    if (len < 0) {
        len = 0;
    }
    buf = (char *)a98_alloc((size_t)len + 1);
    if (buf == NULL) {
        return NULL;
    }
    GetWindowTextA(h, buf, len + 1);
    if (a98_utf8_valid(buf, strlen(buf))) {
        utf8 = a98_strdup(buf);
    } else {
        utf8 = a98_acp_to_utf8(buf);
    }
    a98_free(buf);
    return utf8;
}

static char *dlg_text_utf8(HWND dlg, int id)
{
    return window_text_utf8(GetDlgItem(dlg, id));
}

static void dlg_set_utf8(HWND dlg, int id, const char *utf8)
{
    char *acp = a98_utf8_to_acp(utf8 ? utf8 : "");

    SetDlgItemTextA(dlg, id, acp != NULL ? acp : "");
    a98_free(acp);
}

static char *absolute_path_utf8(const char *utf8)
{
    char *acp;
    char full[A98_MAX_PATH];
    DWORD n;

    acp = a98_utf8_to_acp(utf8 ? utf8 : "");
    if (acp == NULL) {
        return NULL;
    }
    n = GetFullPathNameA(acp, sizeof(full), full, NULL);
    a98_free(acp);
    if (n == 0 || n >= sizeof(full)) {
        return a98_strdup(utf8 ? utf8 : "");
    }
    return a98_acp_to_utf8(full);
}

/* ------------------------------------------------------------------ */
/* Transcript                                                          */
/* ------------------------------------------------------------------ */

static void transcript_append(App *app, const char *utf8)
{
    Str s;
    size_t i;
    char *acp;
    int len;

    if (app == NULL || app->transcript == NULL || utf8 == NULL) {
        return;
    }
    str_init(&s);
    for (i = 0; utf8[i] != '\0'; i++) {
        if (utf8[i] == '\r') {
            continue;
        }
        if (utf8[i] == '\n') {
            str_append_cstr(&s, "\r\n");
        } else {
            str_append_char(&s, utf8[i]);
        }
    }
    acp = a98_utf8_to_acp(str_cstr(&s));
    str_free(&s);
    if (acp == NULL) {
        return;
    }
    len = GetWindowTextLengthA(app->transcript);
    SendMessageA(app->transcript, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(app->transcript, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)acp);
    SendMessageA(app->transcript, EM_SCROLLCARET, 0, 0);
    a98_free(acp);
}

static void transcript_line(App *app, const char *prefix, const char *text)
{
    Str s;

    str_init(&s);
    str_append_cstr(&s, prefix != NULL ? prefix : "");
    str_append_cstr(&s, text != NULL ? text : "");
    str_append_cstr(&s, "\r\n");
    transcript_append(app, str_cstr(&s));
    str_free(&s);
}

static void transcript_clear(App *app)
{
    if (app != NULL && app->transcript != NULL) {
        SetWindowTextA(app->transcript, "");
    }
}

static void render_history(App *app)
{
    int i;

    if (app == NULL) {
        return;
    }
    transcript_clear(app);
    for (i = 0; i < app->agent.messages.count; i++) {
        Message *m = &app->agent.messages.items[i];
        if (m->role == MSG_USER) {
            transcript_line(app, "You: ", m->content != NULL ? m->content : "");
        } else if (m->role == MSG_ASSISTANT) {
            if (m->content != NULL && m->content[0] != '\0') {
                transcript_line(app, "Assistant: ",
                                m->content);
            }
        } else if (m->role == MSG_TOOL) {
            transcript_line(app, "[tool result] ",
                            m->content != NULL ? m->content : "");
        }
        transcript_append(app, "\r\n");
    }
}

/* ------------------------------------------------------------------ */
/* Status and control state                                            */
/* ------------------------------------------------------------------ */

static void update_status(App *app)
{
    Str s;
    char *acp;
    char title[256];

    if (app == NULL || app->status == NULL) {
        return;
    }
    str_init(&s);
    str_appendf(&s, "%s  |  %s  |  workspace: %s",
                app->cfg.model, app->cfg.api_base, app->cfg.workspace);
    acp = a98_utf8_to_acp(str_cstr(&s));
    str_free(&s);
    SetWindowTextA(app->status, acp != NULL ? acp : "");
    a98_free(acp);

    {
        char *m = a98_utf8_to_acp(app->cfg.model);
        snprintf(title, sizeof(title), "Agent98 - %s", m != NULL ? m : "");
        a98_free(m);
        SetWindowTextA(app->hwnd, title);
    }
}

static void set_running(App *app, int running)
{
    if (app == NULL) {
        return;
    }
    app->running = running;
    EnableWindow(app->send, !running);
    EnableWindow(app->input, !running);
    EnableWindow(app->stop, running);
    EnableWindow(app->newbtn, !running);
    EnableWindow(app->settings, !running);
    if (!running) {
        SetFocus(app->input);
    }
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static void gui_on_event(void *ctx, AgentEventType type, const char *text)
{
    App *app = (App *)ctx;
    UiEvent *ev;

    if (app == NULL || app->hwnd == NULL) {
        return;
    }
    ev = (UiEvent *)a98_alloc(sizeof(*ev));
    if (ev == NULL) {
        return;
    }
    ev->type = (int)type;
    ev->text = a98_strdup(text != NULL ? text : "");
    if (ev->text == NULL) {
        a98_free(ev);
        return;
    }
    if (!PostMessageA(app->hwnd, A98_WM_EVENT, 0, (LPARAM)ev)) {
        a98_free(ev->text);
        a98_free(ev);
    }
}

static int gui_confirm(void *ctx, const char *tool, const char *args_json)
{
    App *app = (App *)ctx;
    ConfirmReq req;

    if (app == NULL) {
        return 0;
    }
    memset(&req, 0, sizeof(req));
    copy_str(req.tool, sizeof(req.tool), tool != NULL ? tool : "");
    req.args = a98_strdup(args_json != NULL ? args_json : "{}");
    if (req.args == NULL) {
        return 0;
    }
    req.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (req.event == NULL) {
        a98_free(req.args);
        return 0;
    }
    if (!PostMessageA(app->hwnd, A98_WM_CONFIRM, 0, (LPARAM)&req)) {
        CloseHandle(req.event);
        a98_free(req.args);
        return 0;
    }
    WaitForSingleObject(req.event, INFINITE);
    CloseHandle(req.event);
    a98_free(req.args);
    return req.result;
}

static DWORD WINAPI worker_proc(void *param)
{
    App *app = (App *)param;
    char err[512];
    int rc;
    char *msg;

    err[0] = '\0';
    rc = agent_run(&app->agent, app->pending_input, err, sizeof(err));
    a98_free(app->pending_input);
    app->pending_input = NULL;

    msg = a98_strdup(err);
    if (!PostMessageA(app->hwnd, A98_WM_DONE, (WPARAM)(rc == 0),
                      (LPARAM)msg)) {
        a98_free(msg);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void do_send(App *app)
{
    char *text;
    char *trimmed;
    DWORD tid;

    if (app == NULL || app->running) {
        return;
    }
    text = window_text_utf8(app->input);
    if (text == NULL) {
        return;
    }
    trimmed = text;
    while (*trimmed == ' ' || *trimmed == '\t' ||
           *trimmed == '\r' || *trimmed == '\n') {
        trimmed++;
    }
    if (trimmed != text) {
        memmove(text, trimmed, strlen(trimmed) + 1);
    }
    {
        size_t n = strlen(text);
        while (n > 0) {
            char c = text[n - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                text[--n] = '\0';
            } else {
                break;
            }
        }
    }
    if (text[0] == '\0') {
        a98_free(text);
        return;
    }
    if (app->cfg.api_key[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No API key is configured.\n\n"
                    "Open Tools > Settings and enter one.",
                    MB_OK | MB_ICONWARNING);
        a98_free(text);
        return;
    }

    SetWindowTextA(app->input, "");
    transcript_append(app, "\r\n");
    transcript_line(app, "You: ", text);
    transcript_append(app, "\r\n");

    app->pending_input = text;
    app->cancel = 0;
    app->closing = 0;
    set_running(app, 1);
    SetWindowTextA(app->status, "Connecting...");

#ifdef __WATCOMC__
    {
        unsigned crt_tid = 0;
        unsigned long h = _beginthreadex(NULL, 0,
                                         (unsigned (__stdcall *)(void *))
                                         worker_proc,
                                         app, 0, &crt_tid);
        app->thread = (HANDLE)h;
        tid = (DWORD)crt_tid;
    }
#else
    app->thread = CreateThread(NULL, 0, worker_proc, app, 0, &tid);
#endif
    if (app->thread == NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot start the worker thread (error %lu).",
                 (unsigned long)GetLastError());
        a98_free(app->pending_input);
        app->pending_input = NULL;
        set_running(app, 0);
        update_status(app);
        msgbox_utf8(app->hwnd, buf, MB_OK | MB_ICONERROR);
        return;
    }
    app->thread_id = tid;
}

static void do_stop(App *app)
{
    if (app == NULL || !app->running) {
        return;
    }
    app->cancel = 1;
    SetWindowTextA(app->status, "Cancelling...");
}

static void do_new(App *app)
{
    if (app == NULL) {
        return;
    }
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    agent_new_conversation(&app->agent);
    transcript_clear(app);
    update_status(app);
}

static int choose_session_file(App *app, int save, char *out, size_t cap)
{
    OPENFILENAMEA ofn;
    char file[A98_MAX_PATH];

    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    file[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter =
        "Agent98 sessions (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = save ? "Save session" : "Load session";
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_PATHMUSTEXIST |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) {
        copy_str(out, cap, file);
        return 1;
    }
    return 0;
}

static void do_save(App *app)
{
    char path[A98_MAX_PATH];
    char err[512];

    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!choose_session_file(app, 1, path, sizeof(path))) {
        return;
    }
    err[0] = '\0';
    if (agent_save_session(&app->agent, path, err, sizeof(err)) == 0) {
        transcript_line(app, "[info] Session saved: ", path);
    } else {
        char *u = a98_acp_to_utf8(err[0] ? err : "unknown error");
        transcript_line(app, "[error] Save failed: ", u != NULL ? u : "");
        a98_free(u);
    }
}

static void do_load(App *app)
{
    char path[A98_MAX_PATH];
    char err[512];

    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!choose_session_file(app, 0, path, sizeof(path))) {
        return;
    }
    err[0] = '\0';
    if (agent_load_session(&app->agent, path, err, sizeof(err)) == 0) {
        render_history(app);
        transcript_line(app, "[info] Session loaded: ", path);
    } else {
        char *u = a98_acp_to_utf8(err[0] ? err : "unknown error");
        transcript_line(app, "[error] Load failed: ", u != NULL ? u : "");
        a98_free(u);
    }
}

static void settings_test_connection(HWND dlg, App *app)
{
    Config tmpcfg;
    char *base;
    char *key;
    char *extra;
    char *headers;
    char urlbuf[900];
    char err[512];
    char result[320];
    Url url;
    HttpResponse resp;
    NetConn *conn;
    size_t len;
    int verify;
    int rc;

    (void)app;
    base = dlg_text_utf8(dlg, IDC_API_BASE);
    key = dlg_text_utf8(dlg, IDC_API_KEY);
    extra = dlg_text_utf8(dlg, IDC_EXTRA_HEADERS);
    if (base == NULL || key == NULL || extra == NULL) {
        a98_free(base);
        a98_free(key);
        a98_free(extra);
        msgbox_utf8(dlg, "Out of memory.", MB_OK | MB_ICONERROR);
        return;
    }
    config_trim(base);
    config_trim(key);
    config_trim(extra);
    if (base[0] == '\0') {
        dlg_set_utf8(dlg, IDC_TEST_RESULT,
                     "Enter an API base URL first.");
        goto done;
    }

    len = strlen(base);
    if (len >= sizeof(urlbuf)) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "API base URL is too long.");
        goto done;
    }
    memcpy(urlbuf, base, len + 1);
    {
        static const char cc[] = "/chat/completions";
        size_t cl = sizeof(cc) - 1;
        if (len >= cl && a98_strcasecmp(urlbuf + len - cl, cc) == 0) {
            len -= cl;
            urlbuf[len] = '\0';
        }
    }
    while (len > 0 && urlbuf[len - 1] == '/') {
        urlbuf[--len] = '\0';
    }
    if (len + 8 > sizeof(urlbuf)) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "API base URL is too long.");
        goto done;
    }
    strcat(urlbuf, "/models");

    err[0] = '\0';
    if (url_parse(urlbuf, &url, err, sizeof(err)) != 0) {
        snprintf(result, sizeof(result), "Invalid URL: %s", err);
        dlg_set_utf8(dlg, IDC_TEST_RESULT, result);
        goto done;
    }

    memset(&tmpcfg, 0, sizeof(tmpcfg));
    copy_str(tmpcfg.api_key, sizeof(tmpcfg.api_key), key);
    copy_str(tmpcfg.extra_headers, sizeof(tmpcfg.extra_headers), extra);
    headers = api_build_headers(&tmpcfg, 0);
    if (headers == NULL) {
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "Out of memory.");
        goto done;
    }

    verify = IsDlgButtonChecked(dlg, IDC_TLS_VERIFY) == BST_CHECKED;
    dlg_set_utf8(dlg, IDC_TEST_RESULT, "Testing connection...");
    UpdateWindow(dlg);
    SetCursor(LoadCursorA(NULL, IDC_WAIT));

    conn = net_open(&url, verify, 10, 20, err, sizeof(err));
    if (conn == NULL) {
        snprintf(result, sizeof(result), "Failed: %s",
                 err[0] != '\0' ? err : "cannot connect");
    } else {
        memset(&resp, 0, sizeof(resp));
        rc = http_request(conn, "GET", &url, headers, NULL, 0, &resp,
                          NULL, NULL, err, sizeof(err));
        net_close(conn);
        if (rc != 0) {
            snprintf(result, sizeof(result), "Failed: %s",
                     err[0] != '\0' ? err : "request failed");
        } else if (resp.status == 200) {
            strcpy(result, "Connected successfully (HTTP 200).");
        } else if (resp.status == 401 || resp.status == 403) {
            snprintf(result, sizeof(result),
                     "Authentication failed (HTTP %d). Check the API key.",
                     resp.status);
        } else if (resp.status == 404) {
            strcpy(result,
                   "Endpoint not found (HTTP 404). Check the API base URL.");
        } else if (resp.status == 429) {
            strcpy(result, "Reachable, but rate limited (HTTP 429).");
        } else if (resp.status >= 500) {
            snprintf(result, sizeof(result),
                     "Server error (HTTP %d). The endpoint is reachable.",
                     resp.status);
        } else {
            snprintf(result, sizeof(result), "HTTP %d %s", resp.status,
                     resp.reason);
        }
    }
    a98_free(headers);
    SetCursor(LoadCursorA(NULL, IDC_ARROW));
    dlg_set_utf8(dlg, IDC_TEST_RESULT, result);

done:
    a98_free(base);
    a98_free(key);
    a98_free(extra);
}

static void do_settings(App *app)
{
    if (app->running) {
        msgbox_utf8(app->hwnd, "Stop the current request first.",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_dlg_app = app;
    DialogBoxParamA(app->hinst, MAKEINTRESOURCEA(IDD_SETTINGS),
                    app->hwnd, settings_proc, 0);
    g_dlg_app = NULL;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    App *app = g_dlg_app;

    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int i;
        int idx;
        char tmp[64];

        if (app == NULL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        for (i = 0; i < PRESET_COUNT; i++) {
            SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_ADDSTRING, 0,
                                (LPARAM)g_presets[i].name);
        }
        idx = preset_from_base(app->cfg.api_base);
        SendDlgItemMessageA(dlg, IDC_PROVIDER, CB_SETCURSEL, (WPARAM)idx, 0);
        dlg_set_utf8(dlg, IDC_API_BASE, app->cfg.api_base);
        dlg_set_utf8(dlg, IDC_API_KEY, app->cfg.api_key);
        dlg_set_utf8(dlg, IDC_MODEL, app->cfg.model);
        dlg_set_utf8(dlg, IDC_SYSTEM_PROMPT, app->cfg.system_prompt);
        dlg_set_utf8(dlg, IDC_EXTRA_HEADERS, app->cfg.extra_headers);
        sprintf(tmp, "%.2f", app->cfg.temperature);
        SetDlgItemTextA(dlg, IDC_TEMPERATURE, tmp);
        dlg_set_utf8(dlg, IDC_WORKSPACE, app->cfg.workspace);
        SetDlgItemInt(dlg, IDC_MAX_STEPS, (UINT)app->cfg.max_steps, FALSE);
        SetDlgItemInt(dlg, IDC_MAX_TOKENS, (UINT)app->cfg.max_tokens, FALSE);
        CheckDlgButton(dlg, IDC_TLS_VERIFY,
                       app->cfg.tls_verify ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ALLOW_SHELL,
                       app->cfg.allow_shell ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_STREAM,
                       app->cfg.stream ? BST_CHECKED : BST_UNCHECKED);
        dlg_set_utf8(dlg, IDC_TEST_RESULT, "");
        center_window(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PROVIDER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)SendDlgItemMessageA(dlg, IDC_PROVIDER,
                                                   CB_GETCURSEL, 0, 0);
                if (idx >= 0 && idx < PRESET_COUNT - 1) {
                    dlg_set_utf8(dlg, IDC_API_BASE, g_presets[idx].base);
                    dlg_set_utf8(dlg, IDC_MODEL, g_presets[idx].model);
                    dlg_set_utf8(dlg, IDC_TEST_RESULT, "");
                }
            }
            return TRUE;
        case IDC_TEST:
            settings_test_connection(dlg, app);
            return TRUE;
        case IDOK: {
            char *base;
            char *key;
            char *model;
            char *workspace;
            char *prompt;
            char *headers;
            char *temp;
            char *abs_ws;
            BOOL ok;
            double t;

            base = dlg_text_utf8(dlg, IDC_API_BASE);
            key = dlg_text_utf8(dlg, IDC_API_KEY);
            model = dlg_text_utf8(dlg, IDC_MODEL);
            workspace = dlg_text_utf8(dlg, IDC_WORKSPACE);
            prompt = dlg_text_utf8(dlg, IDC_SYSTEM_PROMPT);
            headers = dlg_text_utf8(dlg, IDC_EXTRA_HEADERS);
            temp = dlg_text_utf8(dlg, IDC_TEMPERATURE);
            if (base == NULL || key == NULL || model == NULL ||
                workspace == NULL || prompt == NULL || headers == NULL ||
                temp == NULL) {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "Out of memory.", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            config_trim(base);
            config_trim(key);
            config_trim(model);
            config_trim(prompt);
            config_trim(headers);
            config_trim(temp);
            if (base[0] == '\0') {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "The API base URL cannot be empty.",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (workspace[0] == '\0') {
                a98_free(base);
                a98_free(key);
                a98_free(model);
                a98_free(workspace);
                a98_free(prompt);
                a98_free(headers);
                a98_free(temp);
                msgbox_utf8(dlg, "The workspace path cannot be empty.",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            abs_ws = absolute_path_utf8(workspace);
            if (abs_ws != NULL) {
                a98_free(workspace);
                workspace = abs_ws;
            }

            copy_str(app->cfg.api_base, sizeof(app->cfg.api_base), base);
            copy_str(app->cfg.api_key, sizeof(app->cfg.api_key), key);
            copy_str(app->cfg.model, sizeof(app->cfg.model), model);
            copy_str(app->cfg.system_prompt, sizeof(app->cfg.system_prompt),
                     prompt);
            copy_str(app->cfg.extra_headers, sizeof(app->cfg.extra_headers),
                     headers);
            copy_str(app->cfg.workspace, sizeof(app->cfg.workspace),
                     workspace);
            t = temp[0] != '\0' ? atof(temp) : app->cfg.temperature;
            if (t < 0.0) {
                t = 0.0;
            }
            if (t > 2.0) {
                t = 2.0;
            }
            app->cfg.temperature = t;
            app->cfg.max_steps =
                (int)GetDlgItemInt(dlg, IDC_MAX_STEPS, &ok, FALSE);
            if (!ok || app->cfg.max_steps < 1) {
                app->cfg.max_steps = 1;
            }
            if (app->cfg.max_steps > 32) {
                app->cfg.max_steps = 32;
            }
            app->cfg.max_tokens =
                (int)GetDlgItemInt(dlg, IDC_MAX_TOKENS, &ok, FALSE);
            if (!ok || app->cfg.max_tokens < 1) {
                app->cfg.max_tokens = 1;
            }
            if (app->cfg.max_tokens > 32768) {
                app->cfg.max_tokens = 32768;
            }
            app->cfg.tls_verify =
                IsDlgButtonChecked(dlg, IDC_TLS_VERIFY) == BST_CHECKED;
            app->cfg.allow_shell =
                IsDlgButtonChecked(dlg, IDC_ALLOW_SHELL) == BST_CHECKED;
            app->cfg.stream =
                IsDlgButtonChecked(dlg, IDC_STREAM) == BST_CHECKED;

            copy_str(app->agent.workspace, sizeof(app->agent.workspace),
                     app->cfg.workspace);
            a98_path_mkdirs(app->cfg.workspace);
            if (config_save(&app->cfg, app->ini_path) != 0) {
                msgbox_utf8(dlg, "Settings could not be saved to the INI file.",
                            MB_OK | MB_ICONWARNING);
            }
            update_status(app);

            a98_free(base);
            a98_free(key);
            a98_free(model);
            a98_free(workspace);
            a98_free(prompt);
            a98_free(headers);
            a98_free(temp);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

static BOOL CALLBACK confirm_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        dlg_set_utf8(dlg, IDC_CONFIRM_TOOL,
                     g_confirm_tool != NULL ? g_confirm_tool : "");
        dlg_set_utf8(dlg, IDC_CONFIRM_ARGS,
                     g_confirm_args != NULL ? g_confirm_args : "{}");
        center_window(dlg);
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

static BOOL CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        center_window(dlg);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dlg, IDOK);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Input subclass: Enter sends, Ctrl+Enter inserts a newline           */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR && wp == '\r') {
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            PostMessageA(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(IDC_SEND, BN_CLICKED),
                         (LPARAM)GetDlgItem(GetParent(hwnd), IDC_SEND));
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            return 0;
        }
    }
    return CallWindowProcA(g_input_oldproc, hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Window creation and layout                                          */
/* ------------------------------------------------------------------ */

static void layout(App *app)
{
    RECT rc;
    int w;
    int h;
    int y;
    int iy;
    const int margin = 6;
    const int btn_h = 24;
    const int input_h = 84;

    if (app == NULL || app->hwnd == NULL) {
        return;
    }
    GetClientRect(app->hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    MoveWindow(app->status, margin, h - 20 - margin, w - 2 * margin, 16, TRUE);

    y = h - 20 - margin - btn_h - 4;
    MoveWindow(app->send, margin, y, 70, btn_h, TRUE);
    MoveWindow(app->stop, margin + 74, y, 70, btn_h, TRUE);
    MoveWindow(app->newbtn, margin + 148, y, 70, btn_h, TRUE);
    MoveWindow(app->settings, margin + 222, y, 70, btn_h, TRUE);

    iy = y - 4 - input_h;
    if (iy < margin + 40) {
        iy = margin + 40;
    }
    MoveWindow(app->input, margin, iy, w - 2 * margin, input_h, TRUE);
    MoveWindow(app->transcript, margin, margin,
               w - 2 * margin, iy - 2 * margin, TRUE);
}

static int load_rich_edit(App *app)
{
    app->rich_dll = LoadLibraryA("riched20.dll");
    if (app->rich_dll != NULL) {
        app->rich = 2;
        return 2;
    }
    app->rich_dll = LoadLibraryA("riched32.dll");
    if (app->rich_dll != NULL) {
        app->rich = 1;
        return 1;
    }
    app->rich = 0;
    return 0;
}

static void create_controls(App *app, HWND hwnd)
{
    const char *transcript_class;
    DWORD transcript_style;
    DWORD input_style;

    transcript_class = app->rich == 2 ? RICH_CLASS20 :
                       (app->rich == 1 ? RICH_CLASS10 : "EDIT");

    transcript_style = WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                       ES_NOHIDESEL;
    app->transcript = CreateWindowExA(WS_EX_CLIENTEDGE, transcript_class, "",
                                      transcript_style,
                                      0, 0, 100, 100, hwnd, (HMENU)IDC_TRANSCRIPT,
                                      app->hinst, NULL);

    input_style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                  ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    app->input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 input_style, 0, 0, 100, 100, hwnd,
                                 (HMENU)IDC_INPUT, app->hinst, NULL);

    app->send = CreateWindowExA(0, "BUTTON", "Send",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                BS_PUSHBUTTON, 0, 0, 70, 24, hwnd,
                                (HMENU)IDC_SEND, app->hinst, NULL);
    app->stop = CreateWindowExA(0, "BUTTON", "Stop",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                BS_PUSHBUTTON, 0, 0, 70, 24, hwnd,
                                (HMENU)IDC_STOP, app->hinst, NULL);
    app->newbtn = CreateWindowExA(0, "BUTTON", "New",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_PUSHBUTTON, 0, 0, 70, 24, hwnd,
                                  (HMENU)IDC_NEW, app->hinst, NULL);
    app->settings = CreateWindowExA(0, "BUTTON", "Settings",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON, 0, 0, 70, 24, hwnd,
                                    (HMENU)IDC_SETTINGS, app->hinst, NULL);

    app->status = CreateWindowExA(0, "STATIC", "",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT |
                                  SS_NOPREFIX, 0, 0, 100, 16, hwnd,
                                  (HMENU)IDC_STATUS, app->hinst, NULL);

    if (app->font != NULL) {
        SendMessageA(app->transcript, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->input, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->send, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->stop, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->newbtn, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->settings, WM_SETFONT, (WPARAM)app->font, TRUE);
        SendMessageA(app->status, WM_SETFONT, (WPARAM)app->font, TRUE);
    }

    if (app->rich == 2) {
        SendMessageA(app->transcript, EM_EXLIMITTEXT, 0, (LPARAM)TRANSCRIPT_LIMIT);
        SendMessageA(app->transcript, EM_SETBKGNDCOLOR, 0,
                     (LPARAM)GetSysColor(COLOR_WINDOW));
    } else {
        SendMessageA(app->transcript, EM_SETLIMITTEXT, 0, (LPARAM)0x7FFFFFFE);
    }

    g_input_oldproc = (WNDPROC)SetWindowLongA(app->input, GWL_WNDPROC,
                                             (LONG)input_proc);
}

/* ------------------------------------------------------------------ */
/* Main window procedure                                               */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App *app = &g_app;

    switch (msg) {
    case WM_CREATE:
        app = (App *)((CREATESTRUCT *)lp)->lpCreateParams;
        app->hwnd = hwnd;
        create_controls(app, hwnd);
        layout(app);
        set_running(app, 0);
        update_status(app);
        SetFocus(app->input);
        return 0;

    case WM_SIZE:
        layout(app);
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 520;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 380;
        return 0;

    case WM_SETFOCUS:
        if (!app->running) {
            SetFocus(app->input);
        }
        return 0;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SEND:
            do_send(app);
            return 0;
        case IDC_STOP:
            do_stop(app);
            return 0;
        case IDC_NEW:
        case IDM_NEW:
            do_new(app);
            return 0;
        case IDC_SETTINGS:
        case IDM_SETTINGS:
            do_settings(app);
            return 0;
        case IDM_SAVE:
            do_save(app);
            return 0;
        case IDM_LOAD:
            do_load(app);
            return 0;
        case IDM_ABOUT:
            DialogBoxParamA(app->hinst, MAKEINTRESOURCEA(IDD_ABOUT),
                            hwnd, about_proc, 0);
            return 0;
        case IDM_EXIT:
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;

    case A98_WM_EVENT: {
        UiEvent *ev = (UiEvent *)lp;
        if (ev != NULL) {
            switch (ev->type) {
            case AGENT_EV_TEXT:
                transcript_append(app, ev->text);
                break;
            case AGENT_EV_ROUND:
                transcript_line(app, "\r\n[round] ", ev->text);
                break;
            case AGENT_EV_TOOL_START:
                transcript_line(app, "\r\n[tool] ", ev->text);
                break;
            case AGENT_EV_TOOL_RESULT:
                transcript_line(app, "[result] ", ev->text);
                break;
            case AGENT_EV_INFO:
                transcript_line(app, "[info] ", ev->text);
                break;
            default:
                transcript_line(app, "", ev->text);
                break;
            }
            a98_free(ev->text);
            a98_free(ev);
        }
        return 0;
    }

    case A98_WM_DONE: {
        char *err = (char *)lp;
        int ok = (int)wp;

        if (app->thread != NULL) {
            CloseHandle(app->thread);
            app->thread = NULL;
        }
        if (!ok) {
            if (err != NULL && err[0] != '\0') {
                transcript_line(app, "\r\n[error] ", err);
            } else {
                transcript_append(app, "\r\n[error] request failed\r\n");
            }
        } else {
            transcript_append(app, "\r\n");
        }
        a98_free(err);
        set_running(app, 0);
        update_status(app);
        if (app->closing) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case A98_WM_CONFIRM: {
        ConfirmReq *req = (ConfirmReq *)lp;
        if (req != NULL) {
            g_confirm_tool = req->tool;
            g_confirm_args = req->args;
            req->result = (int)DialogBoxParamA(app->hinst,
                                               MAKEINTRESOURCEA(IDD_CONFIRM),
                                               hwnd, confirm_proc, 0);
            g_confirm_tool = NULL;
            g_confirm_args = NULL;
            SetEvent(req->event);
        }
        return 0;
    }

    case WM_CLOSE:
        if (app->running) {
            if (MessageBoxA(hwnd,
                            "A request is still running.\n\n"
                            "Stop it and exit?",
                            "Agent98",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 0;
            }
            app->closing = 1;
            app->cancel = 1;
            SetWindowTextA(app->status, "Cancelling...");
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        app->cancel = 1;
        if (app->thread != NULL) {
            WaitForSingleObject(app->thread, 3000);
            CloseHandle(app->thread);
            app->thread = NULL;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static HFONT make_ui_font(void)
{
    NONCLIENTMETRICSA ncm;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return CreateFontIndirectA(&ncm.lfMessageFont);
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    App *app = &g_app;
    WNDCLASSEXA wc;
    MSG msg;
    char log_path[A98_MAX_PATH];
    int rich;

    (void)prev;
    (void)cmdline;

    memset(app, 0, sizeof(*app));
    app->hinst = hinst;

    if (a98_path_exe_dir(app->exe_dir, sizeof(app->exe_dir)) != 0) {
        strcpy(app->exe_dir, ".");
    }

    /* Open the log before touching the configuration.  If the process dies
     * during startup, this file is the only clue we get on Windows 9x. */
    a98_path_join(log_path, sizeof(log_path), app->exe_dir, "agent98.log");
    log_init(log_path, LOG_LEVEL_DEBUG);
    log_info("Agent98 GUI starting (version %s)", A98_VERSION);
    log_info("Executable directory: %s", app->exe_dir);

    config_default_path(app->ini_path, sizeof(app->ini_path));
    config_defaults(&app->cfg, app->exe_dir);
    if (config_ensure(&app->cfg, app->ini_path) != 0) {
        log_warn("Cannot load or create %s", app->ini_path);
    }
    log_set_level(app->cfg.log_level);
    log_info("Configuration loaded from %s", app->ini_path);

    if (net_init() != 0) {
        msgbox_utf8(NULL, "Failed to initialise Winsock.",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    agent_init(&app->agent, &app->cfg);
    agent_new_conversation(&app->agent);
    app->agent.on_event = gui_on_event;
    app->agent.event_ctx = app;
    app->agent.on_confirm = gui_confirm;
    app->agent.confirm_ctx = app;
    app->agent.cancel = &app->cancel;

    rich = load_rich_edit(app);
    log_info("RichEdit mode: %d", rich);
    app->font = make_ui_font();
    app->accel = LoadAcceleratorsA(hinst, MAKEINTRESOURCEA(IDR_ACCEL));

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(IDI_APP));
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    }
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = A98_CLASS_NAME;

    if (!RegisterClassExA(&wc)) {
        msgbox_utf8(NULL, "Cannot register the main window class.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }

    app->hwnd = CreateWindowExA(0, A98_CLASS_NAME, "Agent98",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 620,
                                NULL,
                                LoadMenuA(hinst, MAKEINTRESOURCEA(IDR_MAINMENU)),
                                hinst, app);
    if (app->hwnd == NULL) {
        msgbox_utf8(NULL, "Cannot create the main window.",
                    MB_OK | MB_ICONERROR);
        net_cleanup();
        return 1;
    }

    ShowWindow(app->hwnd, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(app->hwnd);

    if (app->cfg.api_key[0] == '\0') {
        msgbox_utf8(app->hwnd,
                    "No API key is configured yet.\n\n"
                    "Open Tools > Settings, enter your API key and model, "
                    "then press OK.",
                    MB_OK | MB_ICONINFORMATION);
    }

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (app->accel != NULL &&
            TranslateAcceleratorA(app->hwnd, app->accel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    app->cancel = 1;
    if (app->thread != NULL) {
        WaitForSingleObject(app->thread, 3000);
        CloseHandle(app->thread);
        app->thread = NULL;
    }
    agent_free(&app->agent);
    net_cleanup();
    log_info("Agent98 GUI stopped");
    log_shutdown();
    if (app->font != NULL) {
        DeleteObject(app->font);
    }
    if (app->rich_dll != NULL) {
        FreeLibrary(app->rich_dll);
    }
    return (int)msg.wParam;
}
