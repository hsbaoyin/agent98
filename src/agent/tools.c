#include "tools.h"
#include "agent.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../core/utf8.h"
#include "../../third_party/cjson/cJSON.h"

#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

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

/*
 * The model always reads UTF-8.  Files created by Win9x applications are
 * usually in the local ANSI code page (GBK on a Simplified Chinese system),
 * so validate UTF-8 first and convert from CP_ACP when needed.  A UTF-8 BOM
 * is dropped.  `buf` is returned (possibly reallocated) and *len updated.
 */
static char *text_buffer_to_utf8(char *buf, size_t *len, const char *path)
{
    size_t n = *len;
    char *converted;

    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 2);
        n -= 3;
        *len = n;
    }
    if (a98_utf8_valid(buf, n)) {
        return buf;
    }
    converted = a98_acp_to_utf8(buf);
    if (converted == NULL || (converted[0] == '\0' && n > 0)) {
        /* Neither valid UTF-8 nor convertible: hand back the raw bytes
         * instead of losing the file. */
        a98_free(converted);
        log_warn("read_file: cannot convert %s from code page %u",
                 path, (unsigned)GetACP());
        return buf;
    }
    log_info("read_file: converted %s from code page %u to UTF-8",
             path, (unsigned)GetACP());
    a98_free(buf);
    *len = strlen(converted);
    return converted;
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
    buf = text_buffer_to_utf8(buf, &n, path);
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

/*
 * File types that must keep their exact bytes.  Everything else is treated
 * as text and gets DOS line endings.
 */
static int path_is_binary(const char *path)
{
    static const char *const binary_ext[] = {
        "exe", "com", "dll", "drv", "vxd", "386", "sys", "obj", "lib",
        "bin", "dat", "zip", "cab", "lzh", "arj", "rar", "gz", "png",
        "gif", "jpg", "jpeg", "bmp", "ico", "wav", "voc", "mid", "avi",
        "mp3", "hlp", "chm", "ttf", "fon", "pdr", NULL
    };
    const char *dot;
    const char *sep;
    int i;

    dot = strrchr(path, '.');
    if (dot == NULL || dot[1] == '\0') {
        return 0;
    }
    sep = strrchr(path, '\\');
    if (sep != NULL && dot < sep) {
        return 0;
    }
    sep = strrchr(path, '/');
    if (sep != NULL && dot < sep) {
        return 0;
    }
    for (i = 0; binary_ext[i] != NULL; i++) {
        if (a98_strcasecmp(dot + 1, binary_ext[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Windows 9x COMMAND.COM cannot cope with bare LF line endings: it reads the
 * whole .BAT as one very long command line and fails with "Bad command or
 * file name".  The model sends plain "\n", so convert lone LF to CRLF while
 * leaving existing CRLF pairs (and lone CR) untouched.
 */
static int write_text_dos(FILE *f, const char *content, size_t len,
                          size_t *written)
{
    char buf[4096];
    size_t o = 0;
    size_t i;
    size_t total = 0;
    int prev_cr = 0;

    for (i = 0; i < len; i++) {
        char c = content[i];

        if (o + 2 > sizeof(buf)) {
            if (fwrite(buf, 1, o, f) != o) {
                return -1;
            }
            total += o;
            o = 0;
        }
        if (c == '\n' && !prev_cr) {
            buf[o++] = '\r';
        }
        buf[o++] = c;
        prev_cr = (c == '\r');
    }
    if (o > 0) {
        if (fwrite(buf, 1, o, f) != o) {
            return -1;
        }
        total += o;
    }
    *written = total;
    return 0;
}

/*
 * Convert the model's UTF-8 content to the local ANSI code page so Notepad,
 * EDIT and COMMAND.COM can display it.  Returns NULL when the conversion is
 * not lossless (characters the code page cannot represent), in which case the
 * caller keeps the original UTF-8 bytes.
 */
static char *utf8_to_local_lossless(const char *text)
{
    char *local;
    char *roundtrip;

    local = a98_utf8_to_acp(text);
    if (local == NULL) {
        return NULL;
    }
    roundtrip = a98_acp_to_utf8(local);
    if (roundtrip == NULL || strcmp(roundtrip, text) != 0) {
        a98_free(roundtrip);
        a98_free(local);
        return NULL;
    }
    a98_free(roundtrip);
    return local;
}

static int tool_write_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    char dir[A98_MAX_PATH];
    const char *user;
    const char *content;
    const char *payload;
    char *local = NULL;
    size_t len;
    size_t payload_len;
    size_t written = 0;
    int text_mode;
    int converted_encoding = 0;
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
    text_mode = !path_is_binary(path);
    f = fopen(path, "wb");
    if (f == NULL) {
        set_err(err, errcap, "cannot write %s", user);
        return -1;
    }
    payload = content;
    payload_len = len;
    if (text_mode) {
        local = utf8_to_local_lossless(content);
        if (local != NULL) {
            payload = local;
            payload_len = strlen(local);
            converted_encoding = strcmp(local, content) != 0;
        }
    }
    if (text_mode) {
        if (write_text_dos(f, payload, payload_len, &written) != 0) {
            fclose(f);
            a98_free(local);
            set_err(err, errcap, "short write to %s", user);
            return -1;
        }
    } else {
        if (len > 0 && fwrite(content, 1, len, f) != len) {
            fclose(f);
            set_err(err, errcap, "short write to %s", user);
            return -1;
        }
        written = len;
    }
    fclose(f);
    if (converted_encoding) {
        log_info("write_file: converted UTF-8 to code page %u (%lu -> %lu bytes): %s",
                 (unsigned)GetACP(), (unsigned long)len,
                 (unsigned long)payload_len, user);
    }
    if (written != payload_len) {
        log_info("write_file: converted line endings to CRLF (%lu -> %lu bytes): %s",
                 (unsigned long)payload_len, (unsigned long)written, user);
    }
    a98_free(local);
    str_appendf(out, "Wrote %lu bytes to %s\n", (unsigned long)written, user);
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
    const char *cwd_user = arg_str(args, "cwd", NULL);
    int timeout = json_int(args, "timeout_sec", 30);
    char temp_dir[A98_MAX_PATH];
    char temp_file[A98_MAX_PATH];
    char cwd_buf[A98_MAX_PATH];
    char cmdline[4096];
    char *comspec;
    char *workdir;
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

    /* Block obviously destructive Win9x shell commands unless user overrides
     * via config (allow_shell is already required). */
    {
        static const char *const banned[] = {
            "format ", "format\t", "fdisk", "deltree ", "deltree\t",
            "rmdir /s", "rd /s", "erase /", "del /s", "del /f /s",
            "regedit /s", "debug ", "sys ", NULL
        };
        char lower[512];
        size_t i, n;
        int bi;
        n = strlen(command);
        if (n >= sizeof(lower)) n = sizeof(lower) - 1;
        for (i = 0; i < n; i++) {
            lower[i] = (char)tolower((unsigned char)command[i]);
        }
        lower[n] = '\0';
        for (bi = 0; banned[bi] != NULL; bi++) {
            if (strstr(lower, banned[bi]) != NULL) {
                set_err(err, errcap,
                        "blocked dangerous command pattern: %s (refusing to run)",
                        banned[bi]);
                return -1;
            }
        }
    }

    if (timeout < 1) {
        timeout = 1;
    }
    if (timeout > 300) {
        timeout = 300;
    }
    workdir = (a->workspace[0] ? a->workspace : NULL);
    if (cwd_user != NULL && cwd_user[0] != '\0') {
        if (resolve_path(a, cwd_user, cwd_buf, sizeof(cwd_buf), err, errcap) != 0) {
            return -1;
        }
        if (!a98_path_is_dir(cwd_buf)) {
            set_err(err, errcap, "cwd is not a directory: %s", cwd_user);
            return -1;
        }
        workdir = cwd_buf;
    }
    if (GetTempPathA(sizeof(temp_dir), temp_dir) == 0) {
        strcpy(temp_dir, ".");
    }
    /* GetTempPathA can return up to A98_MAX_PATH bytes, so the old sprintf()
     * could run past the end of temp_file. */
    if (snprintf(temp_file, sizeof(temp_file), "%sa98cmd_%lu_%lu.tmp", temp_dir,
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long)GetTickCount()) >= (int)sizeof(temp_file)) {
        set_err(err, errcap, "the temporary directory path is too long");
        return -1;
    }
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
                        workdir, &si, &pi)) {
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


/* Agent98 extended tools — included into tools.c */

static int glob_match(const char *pat, const char *name)
{
    const char *p = pat, *n = name, *star = NULL, *star_n = NULL;
    while (*n) {
        if (*p == '*') { star = p++; star_n = n; }
        else if (*p == '?' || (unsigned char)tolower((unsigned char)*p) ==
                 (unsigned char)tolower((unsigned char)*n)) { p++; n++; }
        else if (star) { p = star + 1; n = ++star_n; }
        else return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

static int tool_mkdir(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    if (a98_path_exists(path)) {
        if (a98_path_is_dir(path)) { str_appendf(out, "Directory already exists: %s\n", user); return 0; }
        set_err(err, errcap, "path exists and is not a directory: %s", user); return -1;
    }
    if (a98_path_mkdirs(path) != 0) { set_err(err, errcap, "cannot create directory: %s", user); return -1; }
    str_appendf(out, "Created directory: %s\n", user); return 0;
}

static int tool_delete_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    DWORD attrs;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "file not found: %s", user); return -1; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        set_err(err, errcap, "refusing to delete a directory (use rmdir): %s", user); return -1;
    }
    if (!DeleteFileA(path)) {
        set_err(err, errcap, "cannot delete %s (error %lu)", user, (unsigned long)GetLastError()); return -1;
    }
    str_appendf(out, "Deleted: %s\n", user); return 0;
}

static int tool_rmdir(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH], pattern[A98_MAX_PATH+4];
    const char *user = arg_str(args, "path", NULL);
    DWORD attrs; WIN32_FIND_DATAA fd; HANDLE h; int empty = 1;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "not found: %s", user); return -1; }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        set_err(err, errcap, "not a directory: %s", user); return -1;
    }
    a98_path_join(pattern, sizeof(pattern), path, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) { empty = 0; break; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!empty) { set_err(err, errcap, "directory is not empty: %s", user); return -1; }
    if (!RemoveDirectoryA(path)) {
        set_err(err, errcap, "cannot rmdir %s (error %lu)", user, (unsigned long)GetLastError()); return -1;
    }
    str_appendf(out, "Removed empty directory: %s\n", user); return 0;
}

static int rmdir_tree(const char *dir, char *err, size_t errcap)
{
    char pattern[A98_MAX_PATH+4], full[A98_MAX_PATH];
    WIN32_FIND_DATAA fd; HANDLE h;
    a98_path_join(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (rmdir_tree(full, err, errcap) != 0) { FindClose(h); return -1; }
            } else {
                if (!DeleteFileA(full)) {
                    set_err(err, errcap, "cannot delete %s (error %lu)", full, (unsigned long)GetLastError());
                    FindClose(h); return -1;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryA(dir)) {
        set_err(err, errcap, "cannot rmdir %s (error %lu)", dir, (unsigned long)GetLastError()); return -1;
    }
    return 0;
}

static int tool_rmdir_force(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    int confirm = json_bool(args, "confirm", 0);
    DWORD attrs;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (!confirm) {
        set_err(err, errcap, "rmdir_force requires confirm=true (recursive delete)"); return -1;
    }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "not found: %s", user); return -1; }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        set_err(err, errcap, "not a directory: %s", user); return -1;
    }
    /* Refuse deleting the workspace root.  `path` is fully resolved, but
     * a->workspace comes straight from the INI and may be relative or carry a
     * trailing separator, so normalise it the same way before comparing. */
    {
        char root[A98_MAX_PATH * 2];
        char norm[A98_MAX_PATH];
        DWORD rn;
        size_t rl, nl;

        if (a->workspace[0] == '\0') {
            set_err(err, errcap, "refusing to delete a directory without a workspace");
            return -1;
        }
        rn = GetFullPathNameA(a->workspace, sizeof(root), root, NULL);
        if (rn == 0 || rn >= (DWORD)sizeof(root)) {
            set_err(err, errcap, "cannot resolve the workspace root");
            return -1;
        }
        strncpy(norm, path, sizeof(norm) - 1);
        norm[sizeof(norm) - 1] = '\0';
        rl = strlen(root);
        while (rl > 0 && (root[rl - 1] == '\\' || root[rl - 1] == '/')) {
            root[--rl] = '\0';
        }
        nl = strlen(norm);
        while (nl > 0 && (norm[nl - 1] == '\\' || norm[nl - 1] == '/')) {
            norm[--nl] = '\0';
        }
        if (rl > 0 && nl == rl && a98_strncasecmp(norm, root, rl) == 0) {
            set_err(err, errcap, "refusing to delete the workspace root");
            return -1;
        }
    }
    if (rmdir_tree(path, err, errcap) != 0) return -1;
    str_appendf(out, "Recursively removed: %s\n", user); return 0;
}

static int tool_touch(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    HANDLE h; FILETIME ft; SYSTEMTIME st; DWORD attrs;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        a98_path_dirname(path, dir, sizeof(dir));
        if (dir[0]) a98_path_mkdirs(dir);
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            set_err(err, errcap, "cannot create %s (error %lu)", user, (unsigned long)GetLastError()); return -1;
        }
        CloseHandle(h); str_appendf(out, "Created empty file: %s\n", user); return 0;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) { set_err(err, errcap, "path is a directory: %s", user); return -1; }
    h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_err(err, errcap, "cannot open %s (error %lu)", user, (unsigned long)GetLastError()); return -1;
    }
    GetSystemTime(&st); SystemTimeToFileTime(&st, &ft);
    if (!SetFileTime(h, NULL, NULL, &ft)) {
        CloseHandle(h); set_err(err, errcap, "cannot set time on %s", user); return -1;
    }
    CloseHandle(h); str_appendf(out, "Updated timestamp: %s\n", user); return 0;
}

static int tool_move_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char src[A98_MAX_PATH], dst[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *from_user = arg_str(args, "from", NULL);
    const char *to_user = arg_str(args, "to", NULL);
    DWORD attrs;
    if (!from_user || !from_user[0] || !to_user || !to_user[0]) {
        set_err(err, errcap, "from and to are required"); return -1;
    }
    if (resolve_path(a, from_user, src, sizeof(src), err, errcap) != 0) return -1;
    if (resolve_path(a, to_user, dst, sizeof(dst), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(src);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "source not found: %s", from_user); return -1; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        set_err(err, errcap, "refusing to move a directory: %s", from_user); return -1;
    }
    a98_path_dirname(dst, dir, sizeof(dir));
    if (dir[0] && !a98_path_is_dir(dir) && a98_path_mkdirs(dir) != 0) {
        set_err(err, errcap, "cannot create destination directory"); return -1;
    }
    if (!MoveFileA(src, dst)) {
        set_err(err, errcap, "cannot move %s -> %s (error %lu)", from_user, to_user, (unsigned long)GetLastError());
        return -1;
    }
    str_appendf(out, "Moved %s -> %s\n", from_user, to_user); return 0;
}

static int tool_copy_file_ex(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char src[A98_MAX_PATH], dst[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *from_user = arg_str(args, "from", NULL);
    const char *to_user = arg_str(args, "to", NULL);
    int overwrite = json_bool(args, "overwrite", 0);
    DWORD attrs; BOOL ok;
    if (!from_user || !from_user[0] || !to_user || !to_user[0]) {
        set_err(err, errcap, "from and to are required"); return -1;
    }
    if (resolve_path(a, from_user, src, sizeof(src), err, errcap) != 0) return -1;
    if (resolve_path(a, to_user, dst, sizeof(dst), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(src);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "source not found: %s", from_user); return -1; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        set_err(err, errcap, "refusing to copy a directory: %s", from_user); return -1;
    }
    a98_path_dirname(dst, dir, sizeof(dir));
    if (dir[0] && !a98_path_is_dir(dir) && a98_path_mkdirs(dir) != 0) {
        set_err(err, errcap, "cannot create destination directory"); return -1;
    }
    ok = CopyFileA(src, dst, overwrite ? FALSE : TRUE);
    if (!ok) {
        DWORD last = GetLastError();
        if (!overwrite && (last == ERROR_FILE_EXISTS || last == ERROR_ALREADY_EXISTS))
            set_err(err, errcap, "destination exists (pass overwrite=true): %s", to_user);
        else
            set_err(err, errcap, "cannot copy (error %lu)", (unsigned long)last);
        return -1;
    }
    str_appendf(out, "Copied %s -> %s%s\n", from_user, to_user, overwrite ? " (overwrote)" : "");
    return 0;
}

static int tool_file_info(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    WIN32_FIND_DATAA fd; HANDLE h; SYSTEMTIME st; FILETIME ft; char time_buf[64];
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) { set_err(err, errcap, "not found: %s", user); return -1; }
    FindClose(h);
    str_appendf(out, "path: %s\ntype: %s\n", user,
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file");
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        str_appendf(out, "size: %lu bytes\n", (unsigned long)fd.nFileSizeLow);
    if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &ft) && FileTimeToSystemTime(&ft, &st))
        snprintf(time_buf, sizeof(time_buf), "%04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)st.wYear,(unsigned)st.wMonth,(unsigned)st.wDay,
                 (unsigned)st.wHour,(unsigned)st.wMinute,(unsigned)st.wSecond);
    else strcpy(time_buf, "(unknown)");
    str_appendf(out, "modified: %s\nattributes:", time_buf);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) str_append_cstr(out, " readonly");
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) str_append_cstr(out, " hidden");
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) str_append_cstr(out, " system");
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) str_append_cstr(out, " archive");
    str_append_cstr(out, "\n"); return 0;
}

static int tool_append_file(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    const char *content = arg_str(args, "content", NULL);
    const char *payload; char *local = NULL; size_t len, payload_len, written = 0;
    int text_mode; FILE *f;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (!content) { set_err(err, errcap, "content is required"); return -1; }
    len = strlen(content);
    if (len > 512*1024) { set_err(err, errcap, "content larger than 512 KiB"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    if (a98_path_is_dir(path)) { set_err(err, errcap, "is a directory"); return -1; }
    a98_path_dirname(path, dir, sizeof(dir)); a98_path_mkdirs(dir);
    f = fopen(path, "ab");
    if (!f) { set_err(err, errcap, "cannot open for append"); return -1; }
    text_mode = !path_is_binary(path);
    payload = content; payload_len = len;
    if (text_mode) {
        local = utf8_to_local_lossless(content);
        if (local) { payload = local; payload_len = strlen(local); }
        if (write_text_dos(f, payload, payload_len, &written) != 0) {
            fclose(f); a98_free(local); set_err(err, errcap, "short write"); return -1;
        }
    } else {
        if (payload_len && fwrite(payload,1,payload_len,f) != payload_len) {
            fclose(f); a98_free(local); set_err(err, errcap, "short write"); return -1;
        }
        written = payload_len;
    }
    fclose(f); a98_free(local);
    str_appendf(out, "Appended %lu bytes to %s\n", (unsigned long)written, user); return 0;
}

static int tool_edit(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    const char *old_s = arg_str(args, "old_string", NULL);
    const char *new_s = arg_str(args, "new_string", NULL);
    int replace_all = json_bool(args, "replace_all", 0);
    char *data, *result, *local = NULL; size_t len = 0, old_len, new_len, rcap, rlen = 0, written = 0;
    int truncated = 0, replacements = 0, text_mode; const char *p; FILE *f;
    if (!user || !user[0] || !old_s || !new_s) { set_err(err, errcap, "path, old_string, new_string required"); return -1; }
    old_len = strlen(old_s); new_len = strlen(new_s);
    if (!old_len) { set_err(err, errcap, "old_string empty"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    data = read_text_file(path, 2*1024*1024, &len, &truncated, err, errcap);
    if (!data) return -1;
    if (truncated) { a98_free(data); set_err(err, errcap, "file > 2 MiB"); return -1; }
    for (p = data; (p = strstr(p, old_s)); p += old_len) { replacements++; if (!replace_all) break; }
    if (!replacements) { a98_free(data); set_err(err, errcap, "old_string not found"); return -1; }
    if (!replace_all) {
        int total = 0; for (p = data; (p = strstr(p, old_s)); p += old_len) total++;
        if (total > 1) { a98_free(data); set_err(err, errcap, "found %d times; use replace_all", total); return -1; }
    }
    rcap = len + 64 + replacements * (new_len > old_len ? new_len - old_len : 0);
    result = (char *)a98_alloc(rcap); if (!result) { a98_free(data); set_err(err, errcap, "oom"); return -1; }
    p = data; rlen = 0; replacements = 0;
    while (*p) {
        const char *hit = strstr(p, old_s); size_t chunk;
        if (!hit) { chunk = strlen(p); memcpy(result+rlen, p, chunk); rlen += chunk; break; }
        chunk = (size_t)(hit - p);
        if (rlen + chunk + new_len + 1 > rcap) {
            size_t ncap = rcap * 2 + chunk + new_len; char *nb = (char *)a98_alloc(ncap);
            if (!nb) { a98_free(result); a98_free(data); set_err(err, errcap, "oom"); return -1; }
            memcpy(nb, result, rlen); a98_free(result); result = nb; rcap = ncap;
        }
        memcpy(result+rlen, p, chunk); rlen += chunk;
        memcpy(result+rlen, new_s, new_len); rlen += new_len;
        replacements++; p = hit + old_len;
        if (!replace_all) { chunk = strlen(p); memcpy(result+rlen, p, chunk); rlen += chunk; break; }
    }
    result[rlen] = '\0'; a98_free(data);
    a98_path_dirname(path, dir, sizeof(dir)); a98_path_mkdirs(dir);
    text_mode = !path_is_binary(path);
    f = fopen(path, "wb");
    if (!f) { a98_free(result); set_err(err, errcap, "cannot write"); return -1; }
    if (text_mode) {
        local = utf8_to_local_lossless(result);
        if (write_text_dos(f, local ? local : result, local ? strlen(local) : rlen, &written) != 0) {
            fclose(f); a98_free(local); a98_free(result); set_err(err, errcap, "short write"); return -1;
        }
        a98_free(local);
    } else {
        if (rlen && fwrite(result,1,rlen,f) != rlen) {
            fclose(f); a98_free(result); set_err(err, errcap, "short write"); return -1;
        }
        written = rlen;
    }
    fclose(f); a98_free(result);
    str_appendf(out, "Edited %s (%d replacement%s)\n", user, replacements, replacements==1?"":"s");
    return 0;
}

static void tree_walk(const char *dir, const char *prefix, Str *out, int depth, int max_depth, int *remaining)
{
    char pattern[A98_MAX_PATH], full[A98_MAX_PATH];
    WIN32_FIND_DATAA fd; HANDLE h;
    if (depth > max_depth || *remaining <= 0) return;
    a98_path_join(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        str_appendf(out, "%s%s%s\n", prefix,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "[D] " : "    ",
                    fd.cFileName);
        (*remaining)--;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char np[128];
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            snprintf(np, sizeof(np), "%s  ", prefix);
            tree_walk(full, np, out, depth+1, max_depth, remaining);
        }
    } while (FindNextFileA(h, &fd) && *remaining > 0);
    FindClose(h);
}

static int tool_tree(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", ".");
    int max_depth = json_int(args, "max_depth", 3);
    int max_entries = json_int(args, "max_entries", 200);
    int remaining;
    if (max_depth < 1) max_depth = 1; if (max_depth > 8) max_depth = 8;
    if (max_entries < 1) max_entries = 1; if (max_entries > 500) max_entries = 500;
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    if (!a98_path_is_dir(path)) { set_err(err, errcap, "not a directory"); return -1; }
    str_appendf(out, "Tree of %s (depth<=%d):\n", user, max_depth);
    remaining = max_entries;
    tree_walk(path, "", out, 0, max_depth, &remaining);
    if (remaining == 0) str_append_cstr(out, "[truncated]\n");
    return 0;
}

static int tool_wc(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    char *data; size_t len = 0, i; int truncated = 0;
    unsigned long lines = 0, words = 0, bytes = 0; int in_word = 0;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    data = read_text_file(path, 4*1024*1024, &len, &truncated, err, errcap);
    if (!data) return -1;
    bytes = (unsigned long)len;
    for (i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (len && data[len-1] != '\n') lines++;
    a98_free(data);
    str_appendf(out, "path: %s\nlines: %lu\nwords: %lu\nbytes: %lu\n", user, lines, words, bytes);
    if (truncated) str_append_cstr(out, "[truncated at 4 MiB]\n");
    return 0;
}

static int tool_which(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *name = arg_str(args, "name", NULL);
    char pathbuf[A98_MAX_PATH];
    char *path_env; char *tok; char *save = NULL;
    char try_path[A98_MAX_PATH*2];
    A98_UNUSED(a);
    if (!name || !name[0]) { set_err(err, errcap, "name is required"); return -1; }
    /* If absolute and exists */
    if (a98_path_is_absolute(name) && a98_path_exists(name)) {
        str_appendf(out, "%s\n", name); return 0;
    }
    path_env = getenv("PATH");
    if (!path_env) { set_err(err, errcap, "PATH not set"); return -1; }
    strncpy(pathbuf, path_env, sizeof(pathbuf)-1); pathbuf[sizeof(pathbuf)-1] = 0;
    for (tok = strtok(pathbuf, ";"); tok; tok = strtok(NULL, ";")) {
        static const char *exts[] = { "", ".exe", ".com", ".bat", ".cmd", NULL };
        int i;
        for (i = 0; exts[i]; i++) {
            snprintf(try_path, sizeof(try_path), "%s\\%s%s", tok, name, exts[i]);
            if (a98_path_exists(try_path) && !a98_path_is_dir(try_path)) {
                str_appendf(out, "%s\n", try_path); return 0;
            }
        }
    }
    set_err(err, errcap, "not found on PATH: %s", name); return -1;
}

static int tool_disk_info(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH], root[8];
    const char *user = arg_str(args, "path", ".");
    DWORD spc, bps, free_c, total_c; char vol[64], fs[32]; DWORD serial, maxc, flags;
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    if (path[0] && path[1]==':') { root[0]=path[0]; root[1]=':'; root[2]='\\'; root[3]=0; }
    else { set_err(err, errcap, "cannot determine drive"); return -1; }
    if (!GetDiskFreeSpaceA(root, &spc, &bps, &free_c, &total_c)) {
        set_err(err, errcap, "GetDiskFreeSpace failed"); return -1;
    }
    vol[0]=0; fs[0]=0;
    GetVolumeInformationA(root, vol, sizeof(vol), &serial, &maxc, &flags, fs, sizeof(fs));
    str_appendf(out, "drive: %s\n", root);
    if (vol[0]) str_appendf(out, "volume: %s\n", vol);
    if (fs[0]) str_appendf(out, "filesystem: %s\n", fs);
    str_appendf(out, "total: %.1f MiB\nfree: %.1f MiB\n",
                ((double)total_c*spc*bps)/(1024.0*1024.0),
                ((double)free_c*spc*bps)/(1024.0*1024.0));
    return 0;
}

static int tool_env(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *name = arg_str(args, "name", NULL);
    char buf[4096]; DWORD n; A98_UNUSED(a);
    if (!name || !name[0]) {
        static const char *keys[] = {"PATH","TEMP","TMP","COMSPEC","WINDIR","PROMPT","USERNAME","COMPUTERNAME",NULL};
        int i; str_append_cstr(out, "Common environment variables:\n");
        for (i=0; keys[i]; i++) {
            n = GetEnvironmentVariableA(keys[i], buf, sizeof(buf));
            if (n > 0 && n < sizeof(buf)) str_appendf(out, "%s=%s\n", keys[i], buf);
        }
        return 0;
    }
    n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n) { set_err(err, errcap, "not set: %s", name); return -1; }
    if (n >= sizeof(buf)) { set_err(err, errcap, "value too long"); return -1; }
    str_appendf(out, "%s=%s\n", name, buf); return 0;
}

static int tool_session_info(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    OSVERSIONINFOA vi; A98_UNUSED(args);
    memset(&vi, 0, sizeof(vi)); vi.dwOSVersionInfoSize = sizeof(vi);
    GetVersionExA(&vi);
    str_appendf(out, "os: Windows %lu.%lu build %lu %s\n",
                (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
                (unsigned long)vi.dwBuildNumber, vi.szCSDVersion);
    str_appendf(out, "workspace: %s\n", a->workspace[0] ? a->workspace : "(none)");
    str_append_cstr(out, "tools: list_dir read_file write_file search_files run_command "
                   "mkdir delete_file rmdir rmdir_force move_file copy_file append_file "
                   "edit file_info touch set_attr tree wc which glob grep head tail "
                   "diff checksum disk_info env session_info todo_read todo_write apply_patch\n");
    return 0;
}

static int tool_set_attr(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    DWORD attrs, next; cJSON *j;
    int has = 0;
    if (!user || !user[0]) { set_err(err, errcap, "path is required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { set_err(err, errcap, "not found"); return -1; }
    next = attrs;
    j = cJSON_GetObjectItem(args, "readonly");
    if (j && (cJSON_IsBool(j) || cJSON_IsNumber(j))) {
        has = 1;
        if (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint)) next |= FILE_ATTRIBUTE_READONLY;
        else next &= ~FILE_ATTRIBUTE_READONLY;
    }
    j = cJSON_GetObjectItem(args, "hidden");
    if (j && (cJSON_IsBool(j) || cJSON_IsNumber(j))) {
        has = 1;
        if (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint)) next |= FILE_ATTRIBUTE_HIDDEN;
        else next &= ~FILE_ATTRIBUTE_HIDDEN;
    }
    j = cJSON_GetObjectItem(args, "archive");
    if (j && (cJSON_IsBool(j) || cJSON_IsNumber(j))) {
        has = 1;
        if (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint)) next |= FILE_ATTRIBUTE_ARCHIVE;
        else next &= ~FILE_ATTRIBUTE_ARCHIVE;
    }
    j = cJSON_GetObjectItem(args, "system");
    if (j && (cJSON_IsBool(j) || cJSON_IsNumber(j))) {
        has = 1;
        if (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint)) next |= FILE_ATTRIBUTE_SYSTEM;
        else next &= ~FILE_ATTRIBUTE_SYSTEM;
    }
    if (!has) { set_err(err, errcap, "specify readonly/hidden/archive/system"); return -1; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) next |= FILE_ATTRIBUTE_DIRECTORY;
    if (!SetFileAttributesA(path, next)) {
        set_err(err, errcap, "SetFileAttributes failed"); return -1;
    }
    str_appendf(out, "Attributes updated: %s\n", user); return 0;
}

static void glob_walk(const char *dir, const char *rel, const char *pat, int *rem, Str *out, int depth)
{
    char pattern[A98_MAX_PATH], full[A98_MAX_PATH], r[A98_MAX_PATH];
    WIN32_FIND_DATAA fd; HANDLE h;
    if (depth > 10 || *rem <= 0) return;
    a98_path_join(pattern, sizeof(pattern), dir, "*");
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName,".")||!strcmp(fd.cFileName,"..")) continue;
        if (rel[0]) snprintf(r,sizeof(r),"%s\\%s", rel, fd.cFileName);
        else { strncpy(r, fd.cFileName, sizeof(r)-1); r[sizeof(r)-1]=0; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (glob_match(pat, fd.cFileName) || glob_match(pat, r)) {
                str_appendf(out, "[DIR]  %s\n", r); (*rem)--;
            }
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            glob_walk(full, r, pat, rem, out, depth+1);
        } else if (glob_match(pat, fd.cFileName) || glob_match(pat, r)) {
            str_appendf(out, "[FILE] %s\n", r); (*rem)--;
        }
    } while (FindNextFileA(h,&fd) && *rem > 0);
    FindClose(h);
}

static int tool_glob(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *pattern = arg_str(args, "pattern", NULL);
    int max_results = json_int(args, "max_results", 100);
    int remaining; const char *prefix = "";
    if (!pattern || !pattern[0]) { set_err(err, errcap, "pattern required"); return -1; }
    if (max_results < 1) max_results = 1; if (max_results > 500) max_results = 500;
    if (resolve_path(a, arg_str(args,"path","."), path, sizeof(path), err, errcap) != 0) return -1;
    if (!a98_path_is_dir(path)) { set_err(err, errcap, "not a directory"); return -1; }
    remaining = max_results;
    glob_walk(path, prefix, pattern, &remaining, out, 0);
    if (remaining == max_results) str_append_cstr(out, "(no matches)\n");
    else if (remaining == 0) str_append_cstr(out, "[truncated]\n");
    return 0;
}

static void grep_walk(const char *dir, const char *rel_prefix, const char *pattern,
                     const char *include, int case_sensitive, int *remaining,
                     Str *out, int depth)
{
    static const char *const skip_dirs[] = {
        ".git", "node_modules", "windows", "system", "recycled", "temp", NULL
    };
    char pattern_path[A98_MAX_PATH];
    char full[A98_MAX_PATH];
    char rel[A98_MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i, skip;

    if (depth > 8 || *remaining <= 0) {
        return;
    }
    a98_path_join(pattern_path, sizeof(pattern_path), dir, "*");
    h = FindFirstFileA(pattern_path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            skip = 0;
            for (i = 0; skip_dirs[i] != NULL; i++) {
                if (strcmp(fd.cFileName, skip_dirs[i]) == 0) {
                    skip = 1;
                    break;
                }
            }
            if (skip) {
                continue;
            }
            a98_path_join(full, sizeof(full), dir, fd.cFileName);
            if (rel_prefix[0] != '\0') {
                snprintf(rel, sizeof(rel), "%s\\%s", rel_prefix, fd.cFileName);
            } else {
                strncpy(rel, fd.cFileName, sizeof(rel) - 1);
                rel[sizeof(rel) - 1] = '\0';
            }
            grep_walk(full, rel, pattern, include, case_sensitive, remaining, out, depth + 1);
        } else if (fd.nFileSizeLow <= 1024 * 1024) {
            if (include != NULL && include[0] != '\0' &&
                !glob_match(include, fd.cFileName)) {
                continue;
            }
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

static int tool_grep(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *pattern = arg_str(args, "pattern", NULL);
    const char *include = arg_str(args, "include", NULL);
    int max_results = json_int(args, "max_results", 50);
    int case_sensitive = json_bool(args, "case_sensitive", 0);
    int remaining;
    const char *prefix = "";

    if (pattern == NULL || pattern[0] == '\0') {
        set_err(err, errcap, "pattern is required");
        return -1;
    }
    if (max_results < 1) max_results = 1;
    if (max_results > 200) max_results = 200;
    if (resolve_path(a, arg_str(args, "path", "."), path, sizeof(path),
                     err, errcap) != 0) {
        return -1;
    }
    if (!a98_path_is_dir(path)) {
        set_err(err, errcap, "not a directory");
        return -1;
    }
    if (a98_strncasecmp(path, a->workspace, strlen(a->workspace)) == 0) {
        size_t wlen = strlen(a->workspace);
        if (path[wlen] == '\\' || path[wlen] == '/') {
            prefix = path + wlen + 1;
        }
    }
    remaining = max_results;
    if (include != NULL && include[0] != '\0') {
        str_appendf(out, "include filter: %s\n", include);
    }
    grep_walk(path, prefix, pattern, include, case_sensitive, &remaining, out, 0);
    if (remaining == max_results) {
        str_append_cstr(out, "(no matches)\n");
    } else if (remaining == 0) {
        str_append_cstr(out, "[truncated]\n");
    }
    return 0;
}


static int tool_head_tail(Agent *a, cJSON *args, Str *out, char *err, size_t errcap, int from_end)
{
    char path[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    int max_lines = json_int(args, "lines", 20);
    char *data; size_t len = 0, pos; int truncated = 0, line_count = 0, start_line, cur;
    if (!user || !user[0]) { set_err(err, errcap, "path required"); return -1; }
    if (max_lines < 1) max_lines = 1; if (max_lines > 500) max_lines = 500;
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    data = read_text_file(path, 2*1024*1024, &len, &truncated, err, errcap);
    if (!data) return -1;
    for (pos = 0; pos < len; pos++) if (data[pos]=='\n') line_count++;
    if (len && data[len-1] != '\n') line_count++;
    start_line = from_end ? (line_count - max_lines + 1) : 1;
    if (start_line < 1) start_line = 1;
    cur = 1; pos = 0;
    while (pos < len) {
        size_t eol = pos, llen;
        while (eol < len && data[eol] != '\n') eol++;
        llen = eol - pos;
        if (llen && data[pos+llen-1]=='\r') llen--;
        if (cur >= start_line && cur < start_line + max_lines) {
            str_appendf(out, "%6d| ", cur);
            if (llen) str_append(out, data+pos, llen);
            str_append_char(out, '\n');
        }
        cur++; pos = eol < len ? eol + 1 : len;
        if (!from_end && cur >= start_line + max_lines) break;
    }
    a98_free(data);
    str_appendf(out, "(%s of %d lines)\n", from_end?"tail":"head", line_count);
    return 0;
}
static int tool_head(Agent *a, cJSON *args, Str *out, char *err, size_t errcap) {
    return tool_head_tail(a, args, out, err, errcap, 0);
}
static int tool_tail(Agent *a, cJSON *args, Str *out, char *err, size_t errcap) {
    return tool_head_tail(a, args, out, err, errcap, 1);
}

static int tool_diff(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char pa[A98_MAX_PATH], pb[A98_MAX_PATH];
    const char *ua = arg_str(args,"a",NULL), *ub = arg_str(args,"b",NULL);
    char *da, *db; size_t la=0,lb=0,posa=0,posb=0; int ta=0,tb=0,line=1,diffs=0;
    int max_diffs = json_int(args, "max_diffs", 50);
    if (!ua||!ub) { set_err(err,errcap,"a and b required"); return -1; }
    if (max_diffs<1) max_diffs=1; if (max_diffs>200) max_diffs=200;
    if (resolve_path(a,ua,pa,sizeof(pa),err,errcap)!=0) return -1;
    if (resolve_path(a,ub,pb,sizeof(pb),err,errcap)!=0) return -1;
    da = read_text_file(pa,1024*1024,&la,&ta,err,errcap); if (!da) return -1;
    db = read_text_file(pb,1024*1024,&lb,&tb,err,errcap);
    if (!db) { a98_free(da); return -1; }
    str_appendf(out, "--- %s\n+++ %s\n", ua, ub);
    while ((posa<la||posb<lb) && diffs<max_diffs) {
        size_t ea=posa,eb=posb,lla,llb; int same;
        while (ea<la && da[ea]!='\n') ea++;
        while (eb<lb && db[eb]!='\n') eb++;
        lla=ea-posa; llb=eb-posb;
        if (lla&&da[posa+lla-1]=='\r') lla--;
        if (llb&&db[posb+llb-1]=='\r') llb--;
        same = (lla==llb && (lla==0 || memcmp(da+posa,db+posb,lla)==0));
        if (!same) {
            diffs++; str_appendf(out, "@@ line %d @@\n- ", line);
            if (posa<la) str_append(out, da+posa, lla); else str_append_cstr(out,"(EOF)");
            str_append_cstr(out, "\n+ ");
            if (posb<lb) str_append(out, db+posb, llb); else str_append_cstr(out,"(EOF)");
            str_append_char(out, '\n');
        }
        if (posa<la) posa = ea<la ? ea+1 : la;
        if (posb<lb) posb = eb<lb ? eb+1 : lb;
        line++;
    }
    if (!diffs) str_append_cstr(out, "(identical)\n");
    else str_appendf(out, "(%d differing line(s))\n", diffs);
    a98_free(da); a98_free(db); return 0;
}

static int tool_checksum(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH]; const char *user = arg_str(args,"path",NULL);
    FILE *f; unsigned char buf[4096]; size_t n,i; unsigned long crc, total=0;
    static unsigned long table[256]; static int ready=0;
    if (!user||!user[0]) { set_err(err,errcap,"path required"); return -1; }
    if (resolve_path(a,user,path,sizeof(path),err,errcap)!=0) return -1;
    if (a98_path_is_dir(path)) { set_err(err,errcap,"is directory"); return -1; }
    f = fopen(path,"rb"); if (!f) { set_err(err,errcap,"cannot open"); return -1; }
    if (!ready) {
        for (i=0;i<256;i++) {
            unsigned long c=(unsigned long)i; int j;
            for (j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
            table[i]=c;
        }
        ready=1;
    }
    crc=0xFFFFFFFFu;
    while ((n=fread(buf,1,sizeof(buf),f))>0) {
        for (i=0;i<n;i++) crc = table[(crc^buf[i])&0xFFu]^(crc>>8);
        total += (unsigned long)n;
    }
    fclose(f); crc^=0xFFFFFFFFu;
    str_appendf(out,"path: %s\nsize: %lu\ncrc32: %08lX\n", user, total, crc); return 0;
}

#define A98_TODO_FILE ".agent98_todos.txt"

static int tool_todo_read(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH]; char *data; size_t len=0; int trunc=0;
    A98_UNUSED(args);
    if (a98_path_join(path, sizeof(path), a->workspace, A98_TODO_FILE) != 0) {
        set_err(err, errcap, "path too long"); return -1;
    }
    if (!a98_path_exists(path)) { str_append_cstr(out, "(no todos)\n"); return 0; }
    data = read_text_file(path, 256*1024, &len, &trunc, err, errcap);
    if (!data) return -1;
    str_append(out, data, len);
    if (len && data[len-1]!='\n') str_append_char(out, '\n');
    a98_free(data); return 0;
}

static int tool_todo_write(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    char path[A98_MAX_PATH];
    const char *content = arg_str(args, "content", NULL);
    FILE *f; char *local = NULL; size_t written=0;
    if (!content) { set_err(err, errcap, "content required"); return -1; }
    if (a98_path_join(path, sizeof(path), a->workspace, A98_TODO_FILE) != 0) {
        set_err(err, errcap, "path too long"); return -1;
    }
    f = fopen(path, "wb");
    if (!f) { set_err(err, errcap, "cannot write todos"); return -1; }
    local = utf8_to_local_lossless(content);
    if (write_text_dos(f, local ? local : content, local ? strlen(local) : strlen(content), &written) != 0) {
        fclose(f); a98_free(local); set_err(err, errcap, "short write"); return -1;
    }
    fclose(f); a98_free(local);
    str_appendf(out, "Wrote todos (%lu bytes)\n", (unsigned long)written); return 0;
}

static int tool_apply_patch(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    /* Multiple sequential search-replace hunks on one file.
     * args: path, patch as text with blocks:
     * *** SEARCH
     * old lines
     * *** REPLACE
     * new lines
     * *** END
     */
    char path[A98_MAX_PATH], dir[A98_MAX_PATH];
    const char *user = arg_str(args, "path", NULL);
    const char *patch = arg_str(args, "patch", NULL);
    char *data, *result = NULL, *local = NULL;
    size_t len=0, rlen=0, rcap=0, written=0;
    int truncated=0, hunks=0, text_mode;
    const char *p; FILE *f;
    if (!user||!user[0]||!patch) { set_err(err,errcap,"path and patch required"); return -1; }
    if (resolve_path(a, user, path, sizeof(path), err, errcap) != 0) return -1;
    data = read_text_file(path, 2*1024*1024, &len, &truncated, err, errcap);
    if (!data) return -1;
    if (truncated) { a98_free(data); set_err(err,errcap,"file too large"); return -1; }
    result = (char *)a98_alloc(len + strlen(patch) + 64);
    if (!result) { a98_free(data); set_err(err,errcap,"oom"); return -1; }
    memcpy(result, data, len); rlen = len; rcap = len + strlen(patch) + 64; result[rlen]=0;
    a98_free(data);
    p = patch;
    while (*p) {
        const char *s_mark, *r_mark, *e_mark, *old_s, *new_s, *hit;
        size_t old_len, new_len, prefix, suffix;
        char *nbuf;
        while (*p=='\r'||*p=='\n'||*p==' ') p++;
        if (!*p) break;
        s_mark = strstr(p, "*** SEARCH");
        if (!s_mark) break;
        s_mark += 10;
        while (*s_mark=='\r'||*s_mark=='\n') s_mark++;
        r_mark = strstr(s_mark, "*** REPLACE");
        if (!r_mark) { a98_free(result); set_err(err,errcap,"patch missing *** REPLACE"); return -1; }
        e_mark = strstr(r_mark, "*** END");
        if (!e_mark) { a98_free(result); set_err(err,errcap,"patch missing *** END"); return -1; }
        old_s = s_mark; old_len = (size_t)(r_mark - s_mark);
        while (old_len && (old_s[old_len-1]=='\n'||old_s[old_len-1]=='\r')) old_len--;
        new_s = r_mark + 11;
        while (*new_s=='\r'||*new_s=='\n') new_s++;
        new_len = (size_t)(e_mark - new_s);
        while (new_len && (new_s[new_len-1]=='\n'||new_s[new_len-1]=='\r')) new_len--;
        /* find old in result */
        {
            char *old_tmp = (char *)a98_alloc(old_len + 1);
            if (!old_tmp) { a98_free(result); set_err(err,errcap,"oom"); return -1; }
            memcpy(old_tmp, old_s, old_len); old_tmp[old_len]=0;
            hit = strstr(result, old_tmp);
            a98_free(old_tmp);
        }
        if (!hit) { a98_free(result); set_err(err,errcap,"hunk %d SEARCH not found", hunks+1); return -1; }
        prefix = (size_t)(hit - result);
        suffix = rlen - prefix - old_len;
        nbuf = (char *)a98_alloc(prefix + new_len + suffix + 1);
        if (!nbuf) { a98_free(result); set_err(err,errcap,"oom"); return -1; }
        memcpy(nbuf, result, prefix);
        memcpy(nbuf+prefix, new_s, new_len);
        memcpy(nbuf+prefix+new_len, hit+old_len, suffix);
        rlen = prefix + new_len + suffix;
        nbuf[rlen]=0;
        a98_free(result); result = nbuf;
        hunks++;
        p = e_mark + 7;
    }
    if (!hunks) { a98_free(result); set_err(err,errcap,"no hunks (need *** SEARCH / *** REPLACE / *** END)"); return -1; }
    a98_path_dirname(path, dir, sizeof(dir)); a98_path_mkdirs(dir);
    text_mode = !path_is_binary(path);
    /* Backup original for recovery if something goes wrong mid-write */
    {
        char bak[A98_MAX_PATH + 8];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        if (!CopyFileA(path, bak, TRUE)) {
            a98_free(result);
            set_err(err, errcap, "cannot create backup %s (error %lu)",
                    bak, (unsigned long)GetLastError());
            return -1;
        }
    }
    f = fopen(path, "wb");
    if (!f) { a98_free(result); set_err(err,errcap,"cannot write"); return -1; }
    if (text_mode) {
        local = utf8_to_local_lossless(result);
        if (write_text_dos(f, local?local:result, local?strlen(local):rlen, &written)!=0) {
            fclose(f); a98_free(local); a98_free(result); set_err(err,errcap,"short write"); return -1;
        }
        a98_free(local);
    } else {
        if (rlen && fwrite(result,1,rlen,f)!=rlen) {
            fclose(f); a98_free(result); set_err(err,errcap,"short write"); return -1;
        }
        written = rlen;
    }
    fclose(f); a98_free(result);
    str_appendf(out, "Applied %d hunks to %s (%lu bytes)\n", hunks, user, (unsigned long)written);
    return 0;
}


static const char schema_mkdir[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_delete_file[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_rmdir[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_rmdir_force[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"confirm\":{\"type\":\"boolean\",\"description\":\"Must be true\"}},\"required\":[\"path\",\"confirm\"]}";
static const char schema_touch[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_move_file[] =
    "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}";
static const char schema_copy_file[] =
    "{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"}},\"required\":[\"from\",\"to\"]}";
static const char schema_file_info[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_append_file[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}";
static const char schema_edit[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_string\",\"new_string\"]}";
static const char schema_tree[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"max_depth\":{\"type\":\"integer\"},\"max_entries\":{\"type\":\"integer\"}},\"required\":[]}";
static const char schema_wc[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_which[] =
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}";
static const char schema_set_attr[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"readonly\":{\"type\":\"boolean\"},\"hidden\":{\"type\":\"boolean\"},\"archive\":{\"type\":\"boolean\"},\"system\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}";
static const char schema_glob[] =
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}";
static const char schema_grep[] =
    "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"include\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"},\"case_sensitive\":{\"type\":\"boolean\"}},\"required\":[\"pattern\"]}";
static const char schema_head[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"lines\":{\"type\":\"integer\"}},\"required\":[\"path\"]}";
static const char schema_tail[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"lines\":{\"type\":\"integer\"}},\"required\":[\"path\"]}";
static const char schema_diff[] =
    "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"},\"b\":{\"type\":\"string\"},\"max_diffs\":{\"type\":\"integer\"}},\"required\":[\"a\",\"b\"]}";
static const char schema_checksum[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
static const char schema_disk_info[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[]}";
static const char schema_env[] =
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[]}";
static const char schema_session_info[] =
    "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
static const char schema_todo_read[] =
    "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
static const char schema_todo_write[] =
    "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"}},\"required\":[\"content\"]}";
static const char schema_apply_patch[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"patch\":{\"type\":\"string\",\"description\":\"Blocks of *** SEARCH / *** REPLACE / *** END\"}},\"required\":[\"path\",\"patch\"]}";


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
    "\"content\":{\"type\":\"string\",\"description\":\"Full UTF-8 content to write. Text files are stored in the local ANSI code page with DOS CRLF line endings.\"}},"
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
    {"read_file", "Read a text file from the workspace (converted to UTF-8), optionally a line range.",
     schema_read_file, 0, tool_read_file},
    {"write_file", "Write a text file in the workspace using the local ANSI code page and DOS line endings. Requires user approval.",
     schema_write_file, 1, tool_write_file},
    {"search_files", "Search text files under a workspace directory for a literal string.",
     schema_search, 0, tool_search_files},
    {"run_command", "Run a shell command (optional cwd). Requires user approval.",
     schema_run_command, 1, tool_run_command},
    {"mkdir", "Create a directory (and parents).", schema_mkdir, 0, tool_mkdir},
    {"delete_file", "Delete a file. Requires approval.", schema_delete_file, 1, tool_delete_file},
    {"rmdir", "Remove an empty directory. Requires approval.", schema_rmdir, 1, tool_rmdir},
    {"rmdir_force", "Recursively delete a directory; requires confirm=true. Requires approval.", schema_rmdir_force, 1, tool_rmdir_force},
    {"move_file", "Move or rename a file. Requires approval.", schema_move_file, 1, tool_move_file},
    {"copy_file", "Copy a file; overwrite=true to replace. Requires approval.", schema_copy_file, 1, tool_copy_file_ex},
    {"append_file", "Append text to a file. Requires approval.", schema_append_file, 1, tool_append_file},
    {"edit", "Exact string replacement (OpenCode-style). Requires approval.", schema_edit, 1, tool_edit},
    {"apply_patch", "Apply multi-hunk SEARCH/REPLACE patch. Requires approval.", schema_apply_patch, 1, tool_apply_patch},
    {"touch", "Create empty file or update timestamp.", schema_touch, 0, tool_touch},
    {"file_info", "File type, size, mtime, attributes.", schema_file_info, 0, tool_file_info},
    {"set_attr", "Set readonly/hidden/archive/system. Requires approval.", schema_set_attr, 1, tool_set_attr},
    {"tree", "Directory tree listing.", schema_tree, 0, tool_tree},
    {"wc", "Count lines, words, bytes.", schema_wc, 0, tool_wc},
    {"which", "Find executable on PATH.", schema_which, 0, tool_which},
    {"glob", "Find paths by * and ? pattern.", schema_glob, 0, tool_glob},
    {"grep", "Search file contents (literal).", schema_grep, 0, tool_grep},
    {"head", "First N lines of a file.", schema_head, 0, tool_head},
    {"tail", "Last N lines of a file.", schema_tail, 0, tool_tail},
    {"diff", "Line-by-line file comparison.", schema_diff, 0, tool_diff},
    {"checksum", "CRC32 and size of a file.", schema_checksum, 0, tool_checksum},
    {"disk_info", "Drive free/total space.", schema_disk_info, 0, tool_disk_info},
    {"env", "Read environment variable(s). Requires approval.", schema_env, 1, tool_env},
    {"session_info", "OS, workspace, tool list.", schema_session_info, 0, tool_session_info},
    {"todo_read", "Read workspace todo list.", schema_todo_read, 0, tool_todo_read},
    {"todo_write", "Write workspace todo list. Requires approval.", schema_todo_write, 1, tool_todo_write}
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


/* Redact secrets before logging tool args or HTTP headers */
static void redact_secrets(char *buf)
{
    static const char *keys[] = {
        "api_key", "apikey", "api-key", "authorization", "password", "passwd",
        "token", "secret", "access_key", "private_key", NULL
    };
    char *p;
    int i;
    if (buf == NULL) {
        return;
    }
    for (i = 0; keys[i] != NULL; i++) {
        p = buf;
        while ((p = strstr(p, keys[i])) != NULL) {
            char *v = p + strlen(keys[i]);
            /* skip spaces quotes colon */
            while (*v == ' ' || *v == '\"' || *v == '\'' || *v == ':' || *v == '=') {
                v++;
            }
            while (*v && *v != '\"' && *v != '\'' && *v != ',' && *v != ' ' &&
                   *v != '}' && *v != '\r' && *v != '\n' && *v != '&') {
                *v++ = '*';
            }
            p = v;
        }
    }
}



void tools_format_confirm(const char *name, const char *args_json,
                          char *out, size_t outcap, int *danger)
{
    cJSON *args = NULL;
    const char *n = name != NULL ? name : "";
    int d = 0;
    char line[512];

    if (out == NULL || outcap == 0) {
        return;
    }
    out[0] = '\0';
    if (danger != NULL) {
        *danger = 0;
    }

    if (args_json != NULL && args_json[0] != '\0') {
        args = cJSON_Parse(args_json);
    }

    if (strcmp(n, "run_command") == 0) {
        const char *cmd = args ? json_string(args, "command") : NULL;
        const char *cwd = args ? json_string(args, "cwd") : NULL;
        d = 2;
        snprintf(line, sizeof(line), "Run shell command%s%s\n  cmd: %s",
                 (cwd && cwd[0]) ? " in " : "",
                 (cwd && cwd[0]) ? cwd : "",
                 cmd ? cmd : "(empty)");
    } else if (strcmp(n, "rmdir_force") == 0) {
        const char *p = args ? json_string(args, "path") : NULL;
        d = 2;
        snprintf(line, sizeof(line), "RECURSIVE DELETE directory\n  path: %s",
                 p ? p : "?");
    } else if (strcmp(n, "delete_file") == 0 || strcmp(n, "rmdir") == 0) {
        const char *p = args ? json_string(args, "path") : NULL;
        d = 2;
        snprintf(line, sizeof(line), "Delete %s\n  path: %s",
                 strcmp(n, "rmdir") == 0 ? "empty directory" : "file",
                 p ? p : "?");
    } else if (strcmp(n, "write_file") == 0 || strcmp(n, "edit") == 0 ||
               strcmp(n, "apply_patch") == 0 || strcmp(n, "append_file") == 0) {
        const char *p = args ? json_string(args, "path") : NULL;
        d = 1;
        if (strcmp(n, "edit") == 0) {
            const char *o = args ? json_string(args, "old_string") : NULL;
            const char *nw = args ? json_string(args, "new_string") : NULL;
            snprintf(line, sizeof(line),
                     "Edit file (exact replace)\n  path: %s\n  old: %.80s%s\n  new: %.80s%s",
                     p ? p : "?",
                     o ? o : "", o && strlen(o) > 80 ? "..." : "",
                     nw ? nw : "", nw && strlen(nw) > 80 ? "..." : "");
        } else if (strcmp(n, "apply_patch") == 0) {
            snprintf(line, sizeof(line), "Apply multi-hunk patch\n  path: %s",
                     p ? p : "?");
        } else if (strcmp(n, "append_file") == 0) {
            snprintf(line, sizeof(line), "Append to file\n  path: %s", p ? p : "?");
        } else {
            snprintf(line, sizeof(line), "Overwrite file\n  path: %s", p ? p : "?");
        }
    } else if (strcmp(n, "move_file") == 0 || strcmp(n, "copy_file") == 0) {
        const char *fr = args ? json_string(args, "from") : NULL;
        const char *to = args ? json_string(args, "to") : NULL;
        d = 1;
        snprintf(line, sizeof(line), "%s\n  from: %s\n  to: %s",
                 strcmp(n, "copy_file") == 0 ? "Copy file" : "Move/rename file",
                 fr ? fr : "?", to ? to : "?");
    } else if (strcmp(n, "set_attr") == 0) {
        const char *p = args ? json_string(args, "path") : NULL;
        d = 1;
        snprintf(line, sizeof(line), "Change file attributes\n  path: %s", p ? p : "?");
    } else if (strcmp(n, "todo_write") == 0) {
        d = 0;
        snprintf(line, sizeof(line), "Update workspace todo list");
    } else if (strcmp(n, "env") == 0) {
        const char *p = args ? json_string(args, "name") : NULL;
        d = 1;
        snprintf(line, sizeof(line), "Read environment variable\n  name: %s",
                 (p && p[0]) ? p : "(common list)");
    } else {
        d = 0;
        snprintf(line, sizeof(line), "Tool: %s", n);
    }

    {
        const char *tag = d >= 2 ? "[HIGH RISK] " : (d == 1 ? "[WRITE] " : "");
        snprintf(out, outcap, "%s%s\n\nRaw args:\n%s",
                 tag, line, args_json ? args_json : "{}");
    }
    if (danger != NULL) {
        *danger = d;
    }
    if (args != NULL) {
        cJSON_Delete(args);
    }
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
    {
        char logbuf[2048];
        const char *src = args_json ? args_json : "{}";
        size_t n = strlen(src);
        if (n >= sizeof(logbuf)) n = sizeof(logbuf) - 1;
        memcpy(logbuf, src, n);
        logbuf[n] = '\0';
        redact_secrets(logbuf);
        log_info("tool %s args=%s", def->name, logbuf);
    }
    rc = def->run(a, args, out, err, errcap);
    cJSON_Delete(args);
    return rc;
}
