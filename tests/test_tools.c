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

static int read_bytes(const char *path, char *buf, size_t cap, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        return -1;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    *out_len = n;
    return 0;
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

    /* Text files must come out with DOS line endings, even when the model
     * sends bare LF; existing CRLF pairs must not be doubled. */
    {
        static const char expect_text[] = "@echo off\r\necho hi\r\necho mixed\r\n";
        static const char expect_bin[] = "a\nb";
        char buf[128];
        size_t n = 0;

        run(&a, "write_file",
            "{\"path\":\"build/host/tool_crlf.bat\",\"content\":"
            "\"@echo off\\necho hi\\r\\necho mixed\\n\"}");
        if (read_bytes("build/host/tool_crlf.bat", buf, sizeof(buf), &n) != 0 ||
            n != strlen(expect_text) || memcmp(buf, expect_text, n) != 0) {
            printf("FAIL: text file did not use CRLF line endings\n");
            failures++;
        } else {
            printf("PASS: text file uses CRLF line endings\n");
        }

        run(&a, "write_file",
            "{\"path\":\"build/host/tool_bin.bin\",\"content\":\"a\\nb\"}");
        if (read_bytes("build/host/tool_bin.bin", buf, sizeof(buf), &n) != 0 ||
            n != strlen(expect_bin) || memcmp(buf, expect_bin, n) != 0) {
            printf("FAIL: binary file was rewritten\n");
            failures++;
        } else {
            printf("PASS: binary file keeps its bytes\n");
        }
    }

    if (failures != 0) {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nall tool tests passed\n");
    return 0;
}
