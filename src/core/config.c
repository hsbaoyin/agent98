#include "config.h"

#include <windows.h>

#define DEFAULT_SYSTEM_PROMPT \
    "You are Agent98, a helpful AI assistant running on Windows 98. " \
    "You can call tools to inspect files, edit files, search text and run " \
    "commands in the user's workspace. Paths are relative to the workspace. " \
    "Use tools when they are needed, keep answers concise, and never claim a " \
    "tool result you did not receive. Destructive tools require user approval."

void config_trim(char *s)
{
    char *start;
    size_t len;

    if (s == NULL) {
        return;
    }
    start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
}

void config_defaults(Config *cfg, const char *exe_dir)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->api_base, "https://api.openai.com/v1");
    strcpy(cfg->model, "gpt-4o-mini");
    strcpy(cfg->system_prompt, DEFAULT_SYSTEM_PROMPT);
    if (exe_dir != NULL && exe_dir[0] != '\0') {
        a98_path_join(cfg->workspace, sizeof(cfg->workspace), exe_dir, "workspace");
    } else {
        strcpy(cfg->workspace, "workspace");
    }
    cfg->max_tokens = 2048;
    cfg->temperature = 0.7;
    cfg->stream = 1;
    cfg->tls_verify = 1;
    cfg->allow_outside = 0;
    cfg->allow_shell = 1;
    cfg->max_steps = 24;
    cfg->connect_timeout_sec = 30;
    cfg->request_timeout_sec = 180;
    cfg->log_level = 2;
}

static void read_str(const char *ini, const char *section, const char *key,
                     const char *def, char *out, size_t cap)
{
    DWORD n;

    if (out == NULL || cap == 0) {
        return;
    }
    n = GetPrivateProfileStringA(section, key, def ? def : "",
                                 out, (DWORD)cap, ini);
    if (n == 0 && def != NULL) {
        strncpy(out, def, cap - 1);
        out[cap - 1] = '\0';
    }
    config_trim(out);
}

static int read_int(const char *ini, const char *section, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(section, key, def, ini);
}

static void config_exe_dir(char *out, size_t cap)
{
    if (a98_path_exe_dir(out, cap) != 0) {
        strncpy(out, ".", cap - 1);
        out[cap - 1] = '\0';
    }
}

void config_default_path(char *out, size_t cap)
{
    char dir[A98_MAX_PATH];
    if (out == NULL || cap == 0) {
        return;
    }
    config_exe_dir(dir, sizeof(dir));
    a98_path_join(out, cap, dir, "agent98.ini");
}

int config_load(Config *cfg, const char *ini_path)
{
    char def_workspace[A98_MAX_PATH];
    char exe_dir[A98_MAX_PATH];
    char tmp[64];

    if (cfg == NULL || ini_path == NULL) {
        return -1;
    }
    config_exe_dir(exe_dir, sizeof(exe_dir));
    config_defaults(cfg, exe_dir);

    read_str(ini_path, "api", "base", "https://api.openai.com/v1",
             cfg->api_base, sizeof(cfg->api_base));
    read_str(ini_path, "api", "key", "", cfg->api_key, sizeof(cfg->api_key));
    read_str(ini_path, "api", "model", "gpt-4o-mini",
             cfg->model, sizeof(cfg->model));
    read_str(ini_path, "api", "system_prompt", DEFAULT_SYSTEM_PROMPT,
             cfg->system_prompt, sizeof(cfg->system_prompt));
    read_str(ini_path, "api", "extra_headers", "", cfg->extra_headers,
             sizeof(cfg->extra_headers));
    cfg->max_tokens = read_int(ini_path, "api", "max_tokens", cfg->max_tokens);
    read_str(ini_path, "api", "temperature", "0.7", tmp, sizeof(tmp));
    cfg->temperature = atof(tmp);
    cfg->stream = read_int(ini_path, "api", "stream", cfg->stream);

    read_str(ini_path, "agent", "workspace", cfg->workspace,
             cfg->workspace, sizeof(cfg->workspace));
    cfg->allow_outside = read_int(ini_path, "agent", "allow_outside", cfg->allow_outside);
    cfg->allow_shell = read_int(ini_path, "agent", "allow_shell", cfg->allow_shell);
    cfg->max_steps = read_int(ini_path, "agent", "max_steps", cfg->max_steps);
    cfg->connect_timeout_sec = read_int(ini_path, "agent", "connect_timeout",
                                        cfg->connect_timeout_sec);
    cfg->request_timeout_sec = read_int(ini_path, "agent", "request_timeout",
                                        cfg->request_timeout_sec);

    read_str(ini_path, "tls", "verify", "1", tmp, sizeof(tmp));
    cfg->tls_verify = atoi(tmp) != 0;

    cfg->log_level = read_int(ini_path, "log", "level", cfg->log_level);

    if (cfg->workspace[0] == '\0') {
        config_exe_dir(def_workspace, sizeof(def_workspace));
        a98_path_join(cfg->workspace, sizeof(cfg->workspace),
                      def_workspace, "workspace");
    }
    if (cfg->max_tokens < 1) {
        cfg->max_tokens = 1;
    }
    if (cfg->max_tokens > 32768) {
        cfg->max_tokens = 32768;
    }
    if (cfg->max_steps < 1) {
        cfg->max_steps = 1;
    }
    if (cfg->max_steps > 32) {
        cfg->max_steps = 32;
    }
    if (cfg->temperature < 0.0) {
        cfg->temperature = 0.0;
    }
    if (cfg->temperature > 2.0) {
        cfg->temperature = 2.0;
    }
    if (cfg->connect_timeout_sec < 1) {
        cfg->connect_timeout_sec = 1;
    }
    if (cfg->request_timeout_sec < 1) {
        cfg->request_timeout_sec = 1;
    }
    return 0;
}

static void write_str(const char *ini, const char *section, const char *key,
                      const char *value)
{
    WritePrivateProfileStringA(section, key, value ? value : "", ini);
}

int config_save(const Config *cfg, const char *ini_path)
{
    char tmp[64];

    if (cfg == NULL || ini_path == NULL) {
        return -1;
    }
    write_str(ini_path, "api", "base", cfg->api_base);
    write_str(ini_path, "api", "key", cfg->api_key);
    write_str(ini_path, "api", "model", cfg->model);
    write_str(ini_path, "api", "system_prompt", cfg->system_prompt);
    write_str(ini_path, "api", "extra_headers", cfg->extra_headers);
    sprintf(tmp, "%d", cfg->max_tokens);
    write_str(ini_path, "api", "max_tokens", tmp);
    sprintf(tmp, "%.2f", cfg->temperature);
    write_str(ini_path, "api", "temperature", tmp);
    sprintf(tmp, "%d", cfg->stream ? 1 : 0);
    write_str(ini_path, "api", "stream", tmp);

    write_str(ini_path, "agent", "workspace", cfg->workspace);
    sprintf(tmp, "%d", cfg->allow_outside ? 1 : 0);
    write_str(ini_path, "agent", "allow_outside", tmp);
    sprintf(tmp, "%d", cfg->allow_shell ? 1 : 0);
    write_str(ini_path, "agent", "allow_shell", tmp);
    sprintf(tmp, "%d", cfg->max_steps);
    write_str(ini_path, "agent", "max_steps", tmp);
    sprintf(tmp, "%d", cfg->connect_timeout_sec);
    write_str(ini_path, "agent", "connect_timeout", tmp);
    sprintf(tmp, "%d", cfg->request_timeout_sec);
    write_str(ini_path, "agent", "request_timeout", tmp);

    sprintf(tmp, "%d", cfg->tls_verify ? 1 : 0);
    write_str(ini_path, "tls", "verify", tmp);

    sprintf(tmp, "%d", cfg->log_level);
    write_str(ini_path, "log", "level", tmp);
    return 0;
}

int config_ensure(Config *cfg, const char *ini_path)
{
    char exe_dir[A98_MAX_PATH];

    if (a98_path_exists(ini_path)) {
        return config_load(cfg, ini_path);
    }
    config_exe_dir(exe_dir, sizeof(exe_dir));
    config_defaults(cfg, exe_dir);
    return config_save(cfg, ini_path);
}
