/*
 * Agent98 - minimal CLI for Windows 98 / modern Windows.
 * Pure C, Win32 console.
 */

#include "agent/agent.h"
#include "agent/tools.h"
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
        size_t tail = g_pending.len - good;
        char rest[8];

        /* Copy the incomplete multi-byte sequence *before* writing the NUL:
         * a98_utf8_to_acp() stops at that NUL, and the old code then moved the
         * tail from data + good -- one byte past where the NUL had just been
         * written.  The first byte of the split character became 0, so the
         * whole pending tail (and the next chunk) was silently dropped. */
        if (tail >= sizeof(rest)) {
            tail = sizeof(rest) - 1;   /* unreachable: at most 3 bytes */
        }
        if (tail > 0) {
            memcpy(rest, g_pending.data + good, tail);
        }
        g_pending.data[good] = '\0';
        acp = a98_utf8_to_acp(g_pending.data);
        if (acp != NULL) {
            fputs(acp, stdout);
            a98_free(acp);
        }
        fflush(stdout);
        if (tail > 0) {
            memmove(g_pending.data, rest, tail);
            g_pending.len = tail;
            g_pending.data[tail] = '\0';
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

/* Write UTF-8 text as the local ANSI code page with DOS CRLF line endings, so
 * /export produces the same bytes as write_file() and the GUI exporter. */
static void export_write_utf8(FILE *f, const char *text)
{
    char *acp;
    const char *src;
    const char *p;
    int prev_cr = 0;

    if (f == NULL) {
        return;
    }
    acp = a98_utf8_to_acp(text != NULL ? text : "");
    src = (acp != NULL) ? acp : (text != NULL ? text : "");
    for (p = src; *p != '\0'; p++) {
        if (*p == '\r') {
            fputc('\r', f);
            prev_cr = 1;
        } else if (*p == '\n') {
            if (!prev_cr) {
                fputc('\r', f);
            }
            fputc('\n', f);
            prev_cr = 0;
        } else {
            fputc(*p, f);
            prev_cr = 0;
        }
    }
    a98_free(acp);
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

static int g_auto_yes = 0;

static int on_confirm(void *ctx, const char *tool, const char *args_json)
{
    char line[32];
    char summary[1024];
    int danger = 0;
    (void)ctx;
    if (g_auto_yes) {
        return 1;
    }
    tools_format_confirm(tool, args_json, summary, sizeof(summary), &danger);
    console_flush_utf8();
    fputs("\n========== CONFIRM ==========\n", stdout);
    if (danger >= 2) {
        fputs("*** HIGH RISK ***\n", stdout);
    } else if (danger == 1) {
        fputs("*** WRITE OPERATION ***\n", stdout);
    }
    console_write_utf8(summary);
    fputs("\n=============================\n", stdout);
    fputs(danger >= 2 ? "Allow? type YES to confirm: " : "Allow? [y/N]: ", stdout);
    fflush(stdout);
    if (fgets(line, (int)sizeof(line), stdin) == NULL) {
        return 0;
    }
    if (danger >= 2) {
        return (strncmp(line, "YES", 3) == 0 || strncmp(line, "yes", 3) == 0);
    }
    return (line[0] == 'y' || line[0] == 'Y');
}

static void mask_key(const char *key, char *out, size_t cap)
{
    size_t n;
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (key == NULL || key[0] == '\0') {
        strncpy(out, "(empty)", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    n = strlen(key);
    if (n <= 8) {
        snprintf(out, cap, "****(%u chars)", (unsigned)n);
    } else {
        snprintf(out, cap, "%.4s***%s", key, key + n - 4);
    }
}

static void print_usage_argv(void)
{
    printf("Usage: agent98_cli [options]\n");
    printf("  --ini PATH         Config INI path\n");
    printf("  --workspace PATH   Override workspace\n");
    printf("  --message TEXT     Send one message and exit\n");
    printf("  --yes, -y          Auto-allow tool confirms (DANGEROUS)\n");
    printf("  --no-shell         Disable shell commands\n");
    printf("  -h, --help         Show this help\n");
}


static void ensure_workspace(const char *path)
{
    if (path == NULL || path[0] == '\0') return;
    CreateDirectoryA(path, NULL);
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  /help              Show this help\n");
    printf("  /new               Start a new conversation\n");
    printf("  /stop              Cancel current request (also Ctrl+C)\n");
    printf("  /save [path]       Save session\n");
    printf("  /load [path]       Load session\n");
    printf("  /export [path]     Export transcript as text\n");
    printf("  /config            Show config (API key masked)\n");
    printf("  /tools             List available tools\n");
    printf("  /workspace [path]  Show or set workspace for this process\n");
    printf("  /model [name]      Show or set model for this process\n");
    printf("  /quit              Exit\n");
}

int main(int argc, char **argv)
{
    char exe_dir[A98_MAX_PATH];
    char ini_path[A98_MAX_PATH];
    char log_path[A98_MAX_PATH];
    char sess_path[A98_MAX_PATH];
    char input[4096];
    char err[512];
    char key_masked[128];
    Config cfg;
    Agent agent;
    int have_key;
    int i;
    const char *opt_ini = NULL;
    const char *opt_workspace = NULL;
    const char *opt_message = NULL;
    int opt_no_shell = 0;
    int oneshot = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage_argv();
            return 0;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            g_auto_yes = 1;
        } else if (strcmp(argv[i], "--no-shell") == 0) {
            opt_no_shell = 1;
        } else if (strcmp(argv[i], "--ini") == 0 && i + 1 < argc) {
            opt_ini = argv[++i];
        } else if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc) {
            opt_workspace = argv[++i];
        } else if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) {
            opt_message = argv[++i];
            oneshot = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage_argv();
            return 1;
        }
    }

    SetConsoleCtrlHandler(console_ctrl, TRUE);

    if (a98_path_exe_dir(exe_dir, sizeof(exe_dir)) != 0) {
        strcpy(exe_dir, ".");
    }

    a98_path_join(log_path, sizeof(log_path), exe_dir, "agent98.log");
    /* Start at INFO so config_load() warnings are captured; the configured
     * level is applied once the INI has been read. */
    log_init(log_path, LOG_LEVEL_INFO);

    if (opt_ini != NULL) {
        strncpy(ini_path, opt_ini, sizeof(ini_path) - 1);
        ini_path[sizeof(ini_path) - 1] = '\0';
    } else {
        a98_path_join(ini_path, sizeof(ini_path), exe_dir, "AGENT98.INI");
    }

    memset(&cfg, 0, sizeof(cfg));
    config_load(&cfg, ini_path);
    if (cfg.log_level < LOG_LEVEL_ERROR) {
        cfg.log_level = LOG_LEVEL_ERROR;
    } else if (cfg.log_level > LOG_LEVEL_TRACE) {
        cfg.log_level = LOG_LEVEL_TRACE;
    }
    log_set_level(cfg.log_level);
    if (!a98_path_exists(ini_path)) {
        config_save(&cfg, ini_path);
    }

    if (opt_workspace != NULL && opt_workspace[0] != '\0') {
        strncpy(cfg.workspace, opt_workspace, sizeof(cfg.workspace) - 1);
        cfg.workspace[sizeof(cfg.workspace) - 1] = '\0';
    }
    if (opt_no_shell) {
        cfg.allow_shell = 0;
    }

    log_info("Workspace: %s", cfg.workspace);
    ensure_workspace(cfg.workspace);

    if (net_init() != 0) {
        fprintf(stderr, "Failed to initialize network (Winsock).\n");
        return 1;
    }

    agent_init(&agent, &cfg);
    agent.on_event = on_event;
    agent.event_ctx = NULL;
    agent.on_confirm = on_confirm;
    agent.confirm_ctx = NULL;
    agent.cancel = &g_cancel;

    have_key = cfg.api_key[0] != '\0';
    printf("Agent98 CLI\n");
    printf("Config: %s\n", ini_path);
    printf("Log: %s\n", log_path);
    printf("Workspace: %s\n", cfg.workspace);
    printf("Model: %s\n", cfg.model);
    printf("API base: %s\n", cfg.api_base);
    if (g_auto_yes) {
        printf("WARNING: --yes is set; tool confirms are auto-allowed.\n");
    }
    if (!have_key) {
        printf("\nWARNING: api.key is empty in %s\n", ini_path);
        printf("Edit [api] key=... then restart.\n");
    }
    if (!oneshot) {
        printf("\nType /help for commands. Ctrl+C cancels current request.\n\n");
    }

    if (oneshot) {
        char *cmd;
        if (!have_key) {
            fprintf(stderr, "api.key is not set.\n");
            agent_free(&agent);
            net_cleanup();
            return 1;
        }
        g_cancel = 0;
        cmd = input_to_utf8(opt_message);
        if (cmd == NULL) {
            cmd = a98_strdup(opt_message);
        }
        err[0] = '\0';
        if (agent_run(&agent, cmd, err, sizeof(err)) != 0) {
            if (err[0]) console_line_utf8("[error] ", err);
            else printf("\n[error] agent_run failed\n");
            a98_free(cmd);
            agent_free(&agent);
            net_cleanup();
            return 1;
        }
        console_flush_utf8();
        printf("\n");
        a98_free(cmd);
        agent_free(&agent);
        net_cleanup();
        return 0;
    }

    for (;;) {
        char *line;
        char *cmd;

        g_cancel = 0;
        printf("> ");
        fflush(stdout);
        line = fgets(input, (int)sizeof(input), stdin);
        if (line == NULL) {
            break;
        }
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

        if (cmd[0] == '/') {
            if (strcmp(cmd, "/help") == 0) {
                print_help();
            } else if (strcmp(cmd, "/quit") == 0 || strcmp(cmd, "/exit") == 0) {
                a98_free(cmd);
                break;
            } else if (strcmp(cmd, "/new") == 0) {
                agent_new_conversation(&agent);
                printf("[info] New conversation started.\n");
            } else if (strcmp(cmd, "/stop") == 0) {
                g_cancel = 1;
                printf("[info] Cancel requested.\n");
            } else if (strcmp(cmd, "/tools") == 0) {
                int count = 0, ti;
                const ToolDef *all = tools_all(&count);
                printf("Available tools (%d):\n", count);
                for (ti = 0; ti < count; ti++) {
                    printf("  %-14s %s%s\n", all[ti].name,
                           all[ti].description ? all[ti].description : "",
                           all[ti].needs_confirm ? " [confirm]" : "");
                }
            } else if (strcmp(cmd, "/config") == 0) {
                mask_key(cfg.api_key, key_masked, sizeof(key_masked));
                printf("Config path: %s\n", ini_path);
                printf("api.base=%s\napi.model=%s\napi.key=%s\nworkspace=%s\n"
                       "tls.verify=%d\nallow_shell=%d\n",
                       cfg.api_base, cfg.model, key_masked,
                       cfg.workspace, cfg.tls_verify, cfg.allow_shell);
            } else if (strncmp(cmd, "/workspace", 10) == 0) {
                const char *arg = cmd + 10;
                while (*arg == ' ') arg++;
                if (*arg == '\0') {
                    printf("workspace=%s\n", cfg.workspace);
                } else {
                    strncpy(cfg.workspace, arg, sizeof(cfg.workspace) - 1);
                    cfg.workspace[sizeof(cfg.workspace) - 1] = '\0';
                    ensure_workspace(cfg.workspace);
                    strncpy(agent.workspace, cfg.workspace, sizeof(agent.workspace) - 1);
                    agent.workspace[sizeof(agent.workspace) - 1] = '\0';
                    printf("[info] workspace set to %s\n", cfg.workspace);
                }
            } else if (strncmp(cmd, "/model", 6) == 0) {
                const char *arg = cmd + 6;
                while (*arg == ' ') arg++;
                if (*arg == '\0') {
                    printf("model=%s\n", cfg.model);
                } else {
                    strncpy(cfg.model, arg, sizeof(cfg.model) - 1);
                    cfg.model[sizeof(cfg.model) - 1] = '\0';
                    printf("[info] model set to %s\n", cfg.model);
                }
            } else if (strncmp(cmd, "/save", 5) == 0) {
                const char *arg = cmd + 5;
                const char *sp;
                while (*arg == ' ') arg++;
                if (*arg == '\0') {
                    agent_last_session_path(sess_path, sizeof(sess_path));
                    sp = sess_path;
                } else {
                    sp = arg;
                }
                err[0] = '\0';
                if (agent_save_session(&agent, sp, err, sizeof(err)) == 0) {
                    printf("[info] Session saved: %s\n", sp);
                } else {
                    printf("[error] Save failed: %s\n", err[0] ? err : "unknown");
                }
            } else if (strncmp(cmd, "/load", 5) == 0) {
                const char *arg = cmd + 5;
                const char *sp;
                while (*arg == ' ') arg++;
                if (*arg == '\0') {
                    agent_last_session_path(sess_path, sizeof(sess_path));
                    sp = sess_path;
                } else {
                    sp = arg;
                }
                err[0] = '\0';
                if (agent_load_session(&agent, sp, err, sizeof(err)) == 0) {
                    printf("[info] Session loaded: %s\n", sp);
                } else {
                    printf("[error] Load failed: %s\n", err[0] ? err : "unknown");
                }
            } else if (strncmp(cmd, "/export", 7) == 0) {
                const char *arg = cmd + 7;
                FILE *ef;
                int mi;
                while (*arg == ' ') arg++;
                if (*arg == '\0') {
                    printf("usage: /export path.txt\n");
                } else {
                    ef = fopen(arg, "wb");
                    if (ef == NULL) {
                        printf("[error] cannot write %s\n", arg);
                    } else {
                        for (mi = 0; mi < agent.messages.count; mi++) {
                            Message *msg = &agent.messages.items[mi];
                            fprintf(ef, "### %s\r\n", msg_role_name(msg->role));
                            if (msg->content != NULL) {
                                export_write_utf8(ef, msg->content);
                            }
                            fputs("\r\n", ef);
                        }
                        fclose(ef);
                        printf("[info] Exported to %s\n", arg);
                    }
                }
            } else {
                printf("Unknown command. Type /help.\n");
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
    log_shutdown();
    return 0;
}
