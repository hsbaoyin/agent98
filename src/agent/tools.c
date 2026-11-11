#include "tools.h"
#include "agent.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../core/utf8.h"
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

static const char *arg_str(cJSON *args, const char *name, const char *def)
{
    const char *v = json_string(args, name);
    return v != NULL ? v : def;
}

static int resolve_path(Agent *a, const char *user, char *out, size_t cap,
                        char *err, size_t errcap)
{
    char joined[A98_MAX_PATH * 2];
    char full[A98_MAX_PATH * 2];
    DWORD n;
    int rc;

    if (user == NULL || user[0] == '\0') {
        user = ".";
    }
    if (a == NULL || a->cfg == NULL) {
        set_err(err, errcap, "agent is not initialised");
        return -1;
    }
    if (!a->cfg->allow_outside) {
        rc = a98_path_resolve_in_root(a->workspace, user, out, cap);
        if (rc == 0) {
            set_err(err, errcap, "path escapes the workspace: %s", user);
            return -1;
        }
        if (rc < 0) {
            set_err(err, errcap, "invalid path: %s", user);
            return -1;
        }
        return 0;
    }
    if (a98_path_is_absolute(user)) {
        n = GetFullPathNameA(user, sizeof(full), full, NULL);
    } else {
        if (a98_path_join(joined, sizeof(joined), a->workspace, user) != 0) {
            set_err(err, errcap, "path is too long");
            return -1;
        }
        n = GetFullPathNameA(joined, sizeof(full), full, NULL);
    }
    if (n == 0 || n >= sizeof(full)) {
        set_err(err, errcap, "cannot resolve path: %s", user);
        return -1;
    }
    if (strlen(full) >= cap) {
        set_err(err, errcap, "path is too long");
        return -1;
    }
    strcpy(out, full);
    return 0;
}

static char *read_text_file(const char *path, size_t max_len, size_t *out_len,
                            int *truncated, char *err, size_t errcap)
{
    FILE *f;
    long size;
    char *buf;
    size_t n;
    size_t i;

    *truncated = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        set_err(err, errcap, "cannot open file: %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, errcap, "cannot seek file: %s", path);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        set_err(err, errcap, "cannot size file: %s", path);
        return NULL;
    }
    rewind(f);
    if ((size_t)size > max_len) {
        size = (long)max_len;
        *truncated = 1;
    }
    buf = (char *)a98_alloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    for (i = 0; i < n && i < 8192; i++) {
        if (buf[i] == '\0') {
            a98_free(buf);
            set_err(err, errcap, "file looks binary: %s", path);
            return NULL;
        }
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return buf;
}

static int tool_list_dir(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    char pattern[A98_MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int count = 0;

    if (resolve_path(a, arg_str(args, "path", "."), path, sizeof(path),
                     err, errcap) != 0) {
        return -1;
    }
    if (!a98_path_is_dir(path)) {
        set_err(err, errcap, "not a directory: %s", path);
        return -1;
    }
    a98_path_join(pattern, sizeof(pattern), path, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        set_err(err, errcap, "cannot list directory: %s", path);
        return -1;
    }
    str_appendf(out, "Directory: %s\n", path);
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            str_appendf(out, "[DIR]  %s\n", fd.cFileName);
        } else {
            str_appendf(out, "[FILE] %10lu  %s\n",
                        (unsigned long)fd.nFileSizeLow, fd.cFileName);
        }
        count++;
    } while (FindNextFileA(h, &fd) && count < 500);
    FindClose(h);
    if (count >= 500) {
        str_append_cstr(out, "[listing truncated at 500 entries]\n");
    }
    if (count == 0) {
        str_append_cstr(out, "(empty directory)\n");
    }
    return 0;
}

static int tool_read_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user;
    char *data;
    size_t len = 0;
    int truncated = 0;
    int start_line = json_int(args, "start_line", 1);
    int max_lines = json_int(args, "max_lines", 400);
    int line_no = 1;
    int emitted = 0;
    size_t pos = 0;
    int stopped = 0;

    user = arg_str(args, "path", NULL);
    if (user == NULL || user[0] == '\0') {
        set_err(err, errcap, "path is required");
        return -1;
    }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) {
        return -1;
    }
    if (!a98_path_exists(path)) {
        set_err(err, errcap, "file does not exist: %s", user);
        return -1;
    }
    if (a98_path_is_dir(path)) {
        set_err(err, errcap, "%s is a directory; use list_dir", user);
        return -1;
    }
    if (start_line < 1) {
        start_line = 1;
    }
    if (max_lines < 1) {
        max_lines = 1;
    }
    if (max_lines > 2000) {
        max_lines = 2000;
    }
    data = read_text_file(path, 1024 * 1024, &len, &truncated, err, errcap);
    if (data == NULL) {
        return -1;
    }

    while (pos < len) {
        size_t eol = pos;
        size_t llen;
        while (eol < len && data[eol] != '\n') {
            eol++;
        }
        llen = eol - pos;
        if (llen > 0 && data[pos + llen - 1] == '\r') {
            llen--;
        }
        if (line_no >= start_line) {
            if (emitted >= max_lines) {
                stopped = 1;
                break;
            }
            str_append(out, data + pos, llen);
            str_append_char(out, '\n');
            emitted++;
        }
        line_no++;
        pos = eol < len ? eol + 1 : len;
    }
    if (stopped) {
        str_appendf(out, "[truncated after %d lines; file continues]\n", max_lines);
    } else if (truncated) {
        str_append_cstr(out, "[truncated: file is larger than 1 MiB]\n");
    }
    if (emitted == 0) {
        str_append_cstr(out, "(no content at the requested line range)\n");
    }
    a98_free(data);
    return 0;
}

static int tool_write_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    char dir[A98_MAX_PATH];
    const char *user;
    const char *content;
    size_t len;
    FILE *f;

    user = arg_str(args, "path", NULL);
    content = arg_str(args, "content", NULL);
    if (user == NULL || user[0] == '\0') {
        set_err(err, errcap, "path is required");
        return -1;
    }
    if (content == NULL) {
        set_err(err, errcap, "content is required");
        return -1;
    }
    len = strlen(content);
    if (len > 1024 * 1024) {
        set_err(err, errcap, "content is larger than 1 MiB");
        return -1;
    }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) {
        return -1;
    }
    if (a98_path_is_dir(path)) {
        set_err(err, errcap, "%s is a directory", user);
        return -1;
    }
    a98_path_dirname(path, dir, sizeof(dir));
    a98_path_mkdirs(dir);
    f = fopen(path, "wb");
    if (f == NULL) {
        set_err(err, errcap, "cannot write %s", user);
        return -1;
    }
    if (len > 0 && fwrite(content, 1, len, f) != len) {
        fclose(f);
        set_err(err, errcap, "short write to %s", user);
        return -1;
    }
    fclose(f);
    str_appendf(out, "Wrote %lu bytes to %s\n", (unsigned long)len, user);
    return 0;
}

static int name_in_list(const char *name, const char *const *list)
{
    int i;
    for (i = 0; list[i] != NULL; i++) {
        if (a98_strcasecmp(name, list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *stristr_local(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return haystack;
    }
    while (*haystack != '\0') {
        if (a98_strncasecmp(haystack, needle, nlen) == 0) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

static void search_file(const char *full, const char *rel, const char *pattern,
                        int case_sensitive, int *remaining, Str *out)
{
    size_t len = 0;
    int truncated = 0;
    char err[128];
    char *data;
    size_t pos = 0;
    int line_no = 1;

    data = read_text_file(full, 1024 * 1024, &len, &truncated, err, sizeof(err));
    A98_UNUSED(truncated);
    if (data == NULL) {
        return;
    }
    while (pos < len && *remaining > 0) {
        size_t eol = pos;
        size_t llen;
        const char *hit;
        while (eol < len && data[eol] != '\n') {
            eol++;
        }
        llen = eol - pos;
        if (llen > 0 && data[pos + llen - 1] == '\r') {
            llen--;
        }
        if (llen > 0) {
            char saved = data[pos + llen];
            char linebuf[240];
            data[pos + llen] = '\0';
            if (case_sensitive) {
                hit = strstr(data + pos, pattern);
            } else {
                hit = stristr_local(data + pos, pattern);
            }
            if (hit != NULL) {
                size_t n = llen;
                if (n >= sizeof(linebuf)) {
                    n = sizeof(linebuf) - 1;
                }
                memcpy(linebuf, data + pos, n);
                linebuf[n] = '\0';
                str_appendf(out, "%s:%d: %s\n", rel, line_no, linebuf);
                (*remaining)--;
            }
            data[pos + llen] = saved;
        }
        line_no++;
        pos = eol < len ? eol + 1 : len;
    }
    a98_free(data);
}

static void search_dir(const char *dir, const char *rel_prefix, const char *pattern,
                       int case_sensitive, int *remaining, Str *out, int depth)
{
    static const char *const skip_dirs[] = {
        ".git", "node_modules", "windows", "system", "recycled", "temp", NULL
    };
    char pattern_path[A98_MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (depth > 8 || *remaining <= 0) {
        return;
    }
    a98_path_join(pattern_path, sizeof(pattern_path), dir, "*");
    h = FindFirstFileA(pattern_path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char full[A98_MAX_PATH];
        char rel[A98_MAX_PATH];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (name_in_list(fd.cFileName, skip_dirs)) {
                continue;
            }
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            if (rel_prefix[0] != '\0') {
                snprintf(rel, sizeof(rel), "%s\\%s", rel_prefix, fd.cFileName);
            } else {
                strncpy(rel, fd.cFileName, sizeof(rel) - 1);
                rel[sizeof(rel) - 1] = '\0';
            }
            search_dir(full, rel, pattern, case_sensitive, remaining, out, depth + 1);
        } else if (fd.nFileSizeLow <= 1024 * 1024) {
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            if (rel_prefix[0] != '\0') {
                snprintf(rel, sizeof(rel), "%s\\%s", rel_prefix, fd.cFileName);
            } else {
                strncpy(rel, fd.cFileName, sizeof(rel) - 1);
                rel[sizeof(rel) - 1] = '\0';
            }
            search_file(full, rel, pattern, case_sensitive, remaining, out);
        }
    } while (FindNextFileA(h, &fd) && *remaining > 0);
    FindClose(h);
}

static int tool_search_files(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *pattern = arg_str(args, "pattern", NULL);
    int max_results = json_int(args, "max_results", 50);
    int case_sensitive = json_bool(args, "case_sensitive", 0);
    int remaining;
    const char *prefix = "";

    if (pattern == NULL || pattern[0] == '\0') {
        set_err(err, errcap, "pattern is required");
        return -1;
    }
    if (max_results < 1) {
        max_results = 1;
    }
    if (max_results > 200) {
        max_results = 200;
    }
    if (resolve_path(a, arg_str(args, "path", "."), path, sizeof(path),
                     err, errcap) != 0) {
        return -1;
    }
    if (!a98_path_is_dir(path)) {
        set_err(err, errcap, "not a directory: %s", path);
        return -1;
    }
    if (a98_strncasecmp(path, a->workspace, strlen(a->workspace)) == 0) {
        size_t wlen = strlen(a->workspace);
        if (path[wlen] == '\\' || path[wlen] == '/') {
            prefix = path + wlen + 1;
        }
    }
    remaining = max_results;
    search_dir(path, prefix, pattern, case_sensitive, &remaining, out, 0);
    if (remaining == max_results) {
        str_append_cstr(out, "(no matches)\n");
    } else if (remaining == 0) {
        str_append_cstr(out, "[results truncated]\n");
    }
    return 0;
}

static int tool_run_command(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *command = arg_str(args, "command", NULL);
    int timeout = json_int(args, "timeout_sec", 30);
    char temp_dir[A98_MAX_PATH];
    char temp_file[A98_MAX_PATH];
    char cmdline[4096];
    char *comspec;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hout = INVALID_HANDLE_VALUE;
    HANDLE hnul = INVALID_HANDLE_VALUE;
    DWORD start;
    DWORD waited;
    int timed_out = 0;
    int cancelled = 0;
    DWORD exit_code = (DWORD)-1;
    FILE *f;
    size_t n;
    char *raw;
    char *utf8;

    if (command == NULL || command[0] == '\0') {
        set_err(err, errcap, "command is required");
        return -1;
    }
    if (a->cfg == NULL || !a->cfg->allow_shell) {
        set_err(err, errcap, "shell commands are disabled in the configuration");
        return -1;
    }
    if (timeout < 1) {
        timeout = 1;
    }
    if (timeout > 300) {
        timeout = 300;
    }
    if (GetTempPathA(sizeof(temp_dir), temp_dir) == 0) {
        strcpy(temp_dir, ".");
    }
    sprintf(temp_file, "%sa98cmd_%lu_%lu.tmp", temp_dir,
            (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    DeleteFileA(temp_file);

    /* Capture output with inherited file handles instead of shell
     * redirection: Win9x COMMAND.COM does not understand "2>&1". */
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    hout = CreateFileA(temp_file, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) {
        set_err(err, errcap, "cannot create the output file (error %lu)",
                (unsigned long)GetLastError());
        return -1;
    }
    hnul = CreateFileA("NUL", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    comspec = getenv("COMSPEC");
    if (comspec == NULL || comspec[0] == '\0') {
        comspec = "command.com";
    }
    if (snprintf(cmdline, sizeof(cmdline), "\"%s\" /c %s",
                 comspec, command) >= (int)sizeof(cmdline)) {
        CloseHandle(hout);
        if (hnul != INVALID_HANDLE_VALUE) {
            CloseHandle(hnul);
        }
        DeleteFileA(temp_file);
        set_err(err, errcap, "command line is too long");
        return -1;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hnul != INVALID_HANDLE_VALUE ? hnul : hout;
    si.hStdOutput = hout;
    si.hStdError = hout;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                        a->workspace[0] ? a->workspace : NULL, &si, &pi)) {
        DWORD last = GetLastError();
        CloseHandle(hout);
        if (hnul != INVALID_HANDLE_VALUE) {
            CloseHandle(hnul);
        }
        DeleteFileA(temp_file);
        set_err(err, errcap, "cannot start the shell (error %lu)",
                (unsigned long)last);
        return -1;
    }
    CloseHandle(hout);
    if (hnul != INVALID_HANDLE_VALUE) {
        CloseHandle(hnul);
    }

    start = GetTickCount();
    for (;;) {
        waited = WaitForSingleObject(pi.hProcess, 100);
        if (waited == WAIT_OBJECT_0) {
            break;
        }
        if (a->cancel != NULL && *a->cancel) {
            cancelled = 1;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if ((long)(GetTickCount() - start) >= (long)timeout * 1000L) {
            timed_out = 1;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, 2000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (cancelled) {
        DeleteFileA(temp_file);
        set_err(err, errcap, "command cancelled");
        return -1;
    }

    n = 0;
    raw = NULL;
    f = fopen(temp_file, "rb");
    if (f != NULL) {
        long size;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        rewind(f);
        if (size > 32768) {
            size = 32768;
        }
        if (size > 0) {
            raw = (char *)a98_alloc((size_t)size + 1);
            if (raw != NULL) {
                n = fread(raw, 1, (size_t)size, f);
                raw[n] = '\0';
            }
        }
        fclose(f);
    }
    DeleteFileA(temp_file);

    utf8 = raw != NULL ? a98_acp_to_utf8(raw) : a98_strdup("");
    a98_free(raw);
    if (timed_out) {
        str_appendf(out, "Command timed out after %d seconds.\n", timeout);
    }
    str_appendf(out, "exit code: %lu\n", (unsigned long)exit_code);
    if (utf8 != NULL && utf8[0] != '\0') {
        str_append_cstr(out, utf8);
        if (out->len > 0 && out->data[out->len - 1] != '\n') {
            str_append_char(out, '\n');
        }
    } else {
        str_append_cstr(out, "(no output)\n");
    }
    a98_free(utf8);
    return 0;
}

static const char schema_list_dir[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory path relative to the workspace. Defaults to .\"}},"
    "\"required\":[]}";

static const char schema_read_file[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path relative to the workspace.\"},"
    "\"start_line\":{\"type\":\"integer\",\"description\":\"First line (1-based).\"},"
    "\"max_lines\":{\"type\":\"integer\",\"description\":\"Maximum number of lines to return (default 400).\"}},"
    "\"required\":[\"path\"]}";

static const char schema_write_file[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path relative to the workspace.\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Full UTF-8 content to write.\"}},"
    "\"required\":[\"path\",\"content\"]}";

static const char schema_search[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"Plain text to find (no regular expressions).\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search, relative to the workspace. Defaults to .\"},"
    "\"max_results\":{\"type\":\"integer\",\"description\":\"Maximum matches (default 50, max 200).\"},"
    "\"case_sensitive\":{\"type\":\"boolean\",\"description\":\"Match case when true.\"}},"
    "\"required\":[\"pattern\"]}";

static const char schema_run_command[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Command line to run through COMMAND.COM in the workspace.\"},"
    "\"timeout_sec\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30, max 300).\"}},"
    "\"required\":[\"command\"]}";

static const ToolDef g_tools[] = {
    {"list_dir", "List files and directories inside the workspace.",
     schema_list_dir, 0, tool_list_dir},
    {"read_file", "Read a UTF-8 text file from the workspace, optionally a line range.",
     schema_read_file, 0, tool_read_file},
    {"write_file", "Write a UTF-8 text file in the workspace. Requires user approval.",
     schema_write_file, 1, tool_write_file},
    {"search_files", "Search text files under a workspace directory for a literal string.",
     schema_search, 0, tool_search_files},
    {"run_command", "Run a shell command in the workspace and capture its output. Requires user approval.",
     schema_run_command, 1, tool_run_command}
};

const ToolDef *tools_all(int *count)
{
    if (count != NULL) {
        *count = A98_ARRAY_LEN(g_tools);
    }
    return g_tools;
}

const ToolDef *tools_find(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < A98_ARRAY_LEN(g_tools); i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            return &g_tools[i];
        }
    }
    return NULL;
}

int tools_execute(Agent *a, const char *name, const char *args_json,
                  Str *out, char *err, size_t errcap)
{
    const ToolDef *def;
    cJSON *args = NULL;
    int rc;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (out == NULL) {
        return -1;
    }
    def = tools_find(name);
    if (def == NULL) {
        set_err(err, errcap, "unknown tool: %s", name ? name : "");
        return -1;
    }
    if (args_json != NULL && args_json[0] != '\0') {
        args = cJSON_Parse(args_json);
        if (args == NULL) {
            set_err(err, errcap, "tool arguments are not valid JSON");
            return -1;
        }
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            set_err(err, errcap, "tool arguments must be a JSON object");
            return -1;
        }
    } else {
        args = cJSON_CreateObject();
    }
    if (args == NULL) {
        set_err(err, errcap, "out of memory");
        return -1;
    }
    log_info("tool %s args=%s", def->name, args_json ? args_json : "{}");
    rc = def->run(a, args, out, err, errcap);
    cJSON_Delete(args);
    return rc;
}
