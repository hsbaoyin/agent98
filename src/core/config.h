/*
 * Agent98 - INI-backed configuration.
 */
#ifndef A98_CONFIG_H
#define A98_CONFIG_H

#include "base.h"
#include "path.h"

typedef struct {
    char api_base[512];
    char api_key[512];
    char model[128];
    char system_prompt[4096];
    char workspace[A98_MAX_PATH];
    char extra_headers[512];
    int max_tokens;
    double temperature;
    int stream;
    int tls_verify;
    int allow_outside;
    int allow_shell;
    int max_steps;
    int connect_timeout_sec;
    int request_timeout_sec;
    int log_level;
} Config;

/* Fill in defaults. `exe_dir` may be NULL; workspace then defaults to
 * "workspace" relative to the current directory. */
void config_defaults(Config *cfg, const char *exe_dir);

/* Returns 0 on success, -1 on I/O error. Missing keys keep defaults. */
int config_load(Config *cfg, const char *ini_path);
int config_save(const Config *cfg, const char *ini_path);
int config_ensure(Config *cfg, const char *ini_path);

/* Resolve the default INI path next to the executable. */
void config_default_path(char *out, size_t cap);

void config_trim(char *s);

#endif /* A98_CONFIG_H */
