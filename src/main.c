/*
 * Agent98 - minimal CLI for Windows 98 / modern Windows.
 * Pure C, Win32 console.
 */

#include "agent/agent.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/utf8.h"
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static volatile int g_cancel = 0;
static Str g_pending;

static void console_flush_utf8(void)
{
    char *acp;

    if (g_pending.data == NULL || g_pending.len == 0) {
        return;
    }
    acp = a98_utf8_to_acp(g_pending.data);
    if (acp != NULL) {
        fputs(acp, stdout);
        a98_free(acp);
    }
    g_pending.len = 0;
    g_pending.data[0] = '\0';
    fflush(stdout);
}

/* Write a UTF-8 chunk to the ANSI console, holding back an incomplete
 * multi-byte sequence until the next chunk arrives. */
static void console_write_utf8(const char *s)
{
    size_t i;
    size_t good;
    size_t len;
    char *acp;

    if (s == NULL) {
        return;
    }
    if (g_pending.data == NULL) {
        str_init(&g_pending);
    }
    str_append_cstr(&g_pending, s);
    len = g_pending.len;
    i = 0;
    good = 0;
    while (i < len) {
        unsigned char c = (unsigned char)g_pending.data[i];
        int need;
        if (c < 0x80) {
            need = 0;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            i++;
            good = i;
            continue;
        }
        if (i + (size_t)need >= len) {
            break;
        }
        i += (size_t)need + 1;
        good = i;
    }
    if (good > 0) {
        g_pending.data[good] = '\0';
        acp = a98_utf8_to_acp(g_pending.data);
        if (acp != NULL) {
            fputs(acp, stdout);
            a98_free(acp);
        }
        fflush(stdout);
        if (good < g_pending.len) {
            memmove(g_pending.data, g_pending.data + good,
                    g_pending.len - good);
            g_pending.len -= good;
            g_pending.data[g_pending.len] = '\0';
        } else {
            g_pending.len = 0;
            g_pending.data[0] = '\0';
        }
    }
}

static void console_puts_utf8(const char *s)
{
    console_flush_utf8();
    if (s != NULL) {
        console_write_utf8(s);
    }
    console_flush_utf8();
}

static char *input_to_utf8(const char *s)
{
    size_t len;

    if (s == NULL) {
        return a98_strdup("");
    }
    len = strlen(s);
    if (a98_utf8_valid(s, len)) {
        return a98_strdup(s);
    }
    return a98_acp_to_utf8(s);
}

static BOOL WINAPI console_ctrl(DWORD type)
{
    (void)type;
    g_cancel = 1;
    return TRUE;
}

static void console_line_utf8(const char *prefix, const char *text)
{
    console_flush_utf8();
    fputs(prefix, stdout);
    console_write_utf8(text != NULL ? text : "");
    fputc('\n', stdout);
    fflush(stdout);
}

static void on_event(void *ctx, AgentEventType type, const char *text)
{
    (void)ctx;
    switch (type) {
    case AGENT_EV_TEXT:
        console_write_utf8(text);
        break;
    case AGENT_EV_ROUND:
        console_line_utf8("\n[round ", text);
        break;
    case AGENT_EV_TOOL_START:
        console_line_utf8("\n[tool] ", text);
        break;
    case AGENT_EV_TOOL_RESULT:
        console_line_utf8("[result] ", text);
        break;
    case AGENT_EV_INFO:
        console_line_utf8("[info] ", text);
        break;
    default:
        break;
    }
}

static int on_confirm(void *ctx, const char *tool, const char *args_json)
{
    char line[32];
    (void)ctx;
    console_flush_utf8();
    fprintf(stdout, "\n[confirm] tool '%s' requested.\nargs: ",
            tool ? tool : "");
    console_write_utf8(args_json ? args_json : "{}");
    fputs("\nAllow? [y/N]: ", stdout);
    fflush(stdout);
    if (fgets(line, (int)sizeof(line), stdin) == NULL) {
        return 0;
    }
    return (line[0] == 'y' || line[0] == 'Y');
}

static void ensure_workspace(const char *path)
{
    if (path == NULL || path[0] == '\0') return;
    CreateDirectoryA(path, NULL);
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  /help            Show this help\n");
    printf("  /new             Start a new conversation\n");
    printf("  /save [path]     Save session (default: sessions\\last_session.json)\n");
    printf("  /load [path]     Load session\n");
    printf("  /config          Show config path and key settings\n");
    printf("  /quit            Exit\n");
}

int main(int argc, char **argv)
{
    char exe_dir[A98_MAX_PATH];
    char ini_path[A98_MAX_PATH];
    char log_path[A98_MAX_PATH];
    char sess_path[A98_MAX_PATH];
    char input[4096];
    char err[512];
    Config cfg;
    Agent agent;
    int have_key;

    (void)argc;
    (void)argv;

    SetConsoleCtrlHandler(console_ctrl, TRUE);

    if (a98_path_exe_dir(exe_dir, sizeof(exe_dir)) != 0) {
        strcpy(exe_dir, ".");
    }

    /* Log from the very first statement: if the process dies during startup
     * this file is the only evidence available on Windows 9x. */
    a98_path_join(log_path, sizeof(log_path), exe_dir, "agent98.log");
    log_init(log_path, LOG_LEVEL_DEBUG);
    log_info("Agent98 CLI starting (version %s)", A98_VERSION);
    log_info("Executable directory: %s", exe_dir);

    config_default_path(ini_path, sizeof(ini_path));
    config_defaults(&cfg, exe_dir);
    if (config_ensure(&cfg, ini_path) != 0) {
        /* If ensure not fully implemented, try load/save best-effort */
        config_load(&cfg, ini_path);
    }

    /* If config_ensure only loaded when exists, create default file if missing */
    if (!a98_path_exists(ini_path)) {
        config_save(&cfg, ini_path);
    }

    log_set_level(cfg.log_level);
    log_info("Configuration: %s", ini_path);
    log_info("Workspace: %s", cfg.workspace);
    ensure_workspace(cfg.workspace);
    agent_ensure_dirs(NULL);

    if (net_init() != 0) {
        fprintf(stderr, "Failed to initialize network (Winsock).\n");
        return 1;
    }

    agent_init(&agent, &cfg);
    agent_new_conversation(&agent);
    agent.on_event = on_event;
    agent.event_ctx = NULL;
    agent.on_confirm = on_confirm;
    agent.confirm_ctx = NULL;
    agent.cancel = &g_cancel;

    have_key = (cfg.api_key[0] != '\0');

    printf("Agent98 CLI\n");
    printf("Config: %s\n", ini_path);
    printf("Workspace: %s\n", cfg.workspace);
    printf("Model: %s\n", cfg.model);
    printf("API base: %s\n", cfg.api_base);
    if (!have_key) {
        printf("\nWARNING: api.key is empty in %s\n", ini_path);
        printf("Edit [api] key=... then restart (or set it and continue if supported).\n");
    }
    printf("\nType /help for commands. Ctrl+C cancels current request.\n\n");

    for (;;) {
        char *line;
        char *cmd;
        g_cancel = 0;

        console_flush_utf8();
        printf("> ");
        fflush(stdout);
        line = fgets(input, (int)sizeof(input), stdin);
        if (line == NULL) {
            break;
        }
        /* trim newline */
        {
            size_t n = strlen(input);
            while (n > 0 && (input[n - 1] == '\n' || input[n - 1] == '\r')) {
                input[--n] = '\0';
            }
        }
        if (input[0] == '\0') {
            continue;
        }
        cmd = input_to_utf8(input);
        if (cmd == NULL) {
            continue;
        }
        if (cmd[0] == '\0') {
            a98_free(cmd);
            continue;
        }

        if (strcmp(cmd, "/quit") == 0 || strcmp(cmd, "/exit") == 0) {
            a98_free(cmd);
            break;
        } else if (strcmp(cmd, "/help") == 0) {
            print_help();
            a98_free(cmd);
            continue;
        } else if (strcmp(cmd, "/new") == 0) {
            agent_new_conversation(&agent);
            printf("[info] New conversation started.\n");
            a98_free(cmd);
            continue;
        } else if (strcmp(cmd, "/config") == 0) {
            printf("Config path: %s\n", ini_path);
            printf("api.base=%s\napi.model=%s\napi.key=%s\nworkspace=%s\ntls.verify=%d\n",
                   cfg.api_base, cfg.model,
                   cfg.api_key[0] ? "(set)" : "(empty)",
                   cfg.workspace, cfg.tls_verify);
            a98_free(cmd);
            continue;
        } else if (strncmp(cmd, "/save", 5) == 0) {
            const char *p = cmd + 5;
            while (*p == ' ') p++;
            if (*p == '\0') {
                agent_last_session_path(sess_path, sizeof(sess_path));
                p = sess_path;
            }
            err[0] = '\0';
            if (agent_save_session(&agent, p, err, sizeof(err)) == 0) {
                printf("[info] Session saved: %s\n", p);
            } else {
                printf("[error] Save failed: %s\n", err[0] ? err : "unknown error");
            }
            a98_free(cmd);
            continue;
        } else if (strncmp(cmd, "/load", 5) == 0) {
            const char *p = cmd + 5;
            while (*p == ' ') p++;
            if (*p == '\0') {
                agent_last_session_path(sess_path, sizeof(sess_path));
                p = sess_path;
            }
            err[0] = '\0';
            if (agent_load_session(&agent, p, err, sizeof(err)) == 0) {
                printf("[info] Session loaded: %s\n", p);
            } else {
                printf("[error] Load failed: %s\n", err[0] ? err : "unknown error");
            }
            a98_free(cmd);
            continue;
        }

        err[0] = '\0';
        if (cfg.api_key[0] == '\0') {
            printf("[error] api.key is not set. Edit %s and restart.\n", ini_path);
            a98_free(cmd);
            continue;
        }

        printf("\n");
        fflush(stdout);
        if (agent_run(&agent, cmd, err, sizeof(err)) != 0) {
            if (err[0] != '\0') {
                console_line_utf8("[error] ", err);
            } else {
                printf("\n[error] agent_run failed\n");
            }
        }
        console_flush_utf8();
        printf("\n\n");
        a98_free(cmd);
    }

    console_flush_utf8();
    str_free(&g_pending);
    agent_free(&agent);
    net_cleanup();
    return 0;
}
