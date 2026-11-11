/*
 * Host-side smoke test for the local tools.
 *
 * Build (MinGW):
 *   gcc -std=c99 -O1 -w -Isrc -Isrc/core -Isrc/agent -Isrc/net \
 *       -Ithird_party/cjson -Ithird_party/bearssl/inc \
 *       tests/test_tools.c src/agent/tools.c src/agent/jsonutil.c \
 *       src/core/base.c src/core/config.c src/core/log.c src/core/path.c \
 *       src/core/str.c src/core/utf8.c third_party/cjson/cJSON.c \
 *       -lws2_32 -o build/host/test_tools.exe
 */

#include "agent/agent.h"
#include "agent/tools.h"
#include "core/config.h"
#include "core/str.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void run(Agent *a, const char *name, const char *args)
{
    Str out;
    char err[512];
    int rc;

    str_init(&out);
    err[0] = '\0';
    rc = tools_execute(a, name, args, &out, err, sizeof(err));
    printf("--- %s (rc=%d)\n", name, rc);
    if (err[0] != '\0') {
        printf("error: %s\n", err);
    }
    printf("%s\n", str_cstr(&out));
    if (rc != 0) {
        failures++;
    }
    str_free(&out);
}

int main(void)
{
    Config cfg;
    Agent a;

    memset(&cfg, 0, sizeof(cfg));
    config_defaults(&cfg, NULL);
    strcpy(cfg.workspace, ".");
    cfg.allow_shell = 1;

    memset(&a, 0, sizeof(a));
    a.cfg = &cfg;
    strcpy(a.workspace, ".");

    run(&a, "write_file",
        "{\"path\":\"build/host/tool_test.txt\",\"content\":\"hello\\nworld\\n\"}");
    run(&a, "read_file", "{\"path\":\"build/host/tool_test.txt\"}");
    run(&a, "list_dir", "{\"path\":\"build/host\"}");
    run(&a, "search_files",
        "{\"path\":\"build/host\",\"pattern\":\"world\"}");
    run(&a, "run_command",
        "{\"command\":\"echo stdout-line & echo stderr-line 1>&2\"}");

    if (failures != 0) {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nall tool tests passed\n");
    return 0;
}
