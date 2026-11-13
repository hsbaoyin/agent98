#include "path.h"

#include <windows.h>

int a98_path_exe_dir(char *out, size_t cap)
{
    char buf[A98_MAX_PATH * 2];
    DWORD n;
    DWORD i;

    if (out == NULL || cap == 0) {
        return -1;
    }
    n = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        out[0] = '\0';
        return -1;
    }
    i = n;
    while (i > 0) {
        char c = buf[i - 1];
        if (c == '\\' || c == '/') {
            break;
        }
        i--;
    }
    if (i == 0) {
        strncpy(out, ".", cap - 1);
        out[cap - 1] = '\0';
        return 0;
    }
    if ((size_t)i >= cap) {
        i = cap - 1;
    }
    memcpy(out, buf, (size_t)i);
    out[i] = '\0';
    return 0;
}

int a98_path_is_absolute(const char *p)
{
    if (p == NULL || p[0] == '\0') {
        return 0;
    }
    if ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) {
        if (p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
            return 1;
        }
    }
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
        return 1;
    }
    return 0;
}

int a98_path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t alen;
    size_t blen;
    int need_sep;

    if (out == NULL || cap == 0) {
        return -1;
    }
    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    alen = strlen(a);
    blen = strlen(b);
    need_sep = alen > 0 && a[alen - 1] != '\\' && a[alen - 1] != '/';
    if (alen + (size_t)need_sep + blen + 1 > cap) {
        out[0] = '\0';
        return -1;
    }
    memcpy(out, a, alen);
    if (need_sep) {
        out[alen] = '\\';
        alen++;
    }
    memcpy(out + alen, b, blen);
    out[alen + blen] = '\0';
    return 0;
}

static int is_sep(char c)
{
    return c == '\\' || c == '/';
}

int a98_path_normalize(char *out, size_t cap, const char *in)
{
    char tmp[A98_MAX_PATH * 2];
    char *parts[128];
    int count = 0;
    int absolute = 0;
    int drive = 0;
    char drive_char = 0;
    size_t i = 0;
    size_t o = 0;
    size_t len;

    if (out == NULL || cap == 0) {
        return -1;
    }
    if (in == NULL) {
        in = "";
    }
    if (a98_path_is_absolute(in)) {
        absolute = 1;
        if (in[1] == ':') {
            drive = 1;
            drive_char = in[0];
        }
    }
    if (strlen(in) >= sizeof(tmp)) {
        out[0] = '\0';
        return -1;
    }
    strcpy(tmp, in);

    /* Split on separators; resolve "." and ".." lexically. */
    while (tmp[i] != '\0') {
        char *start;
        while (tmp[i] != '\0' && is_sep(tmp[i])) {
            i++;
        }
        if (tmp[i] == '\0') {
            break;
        }
        start = &tmp[i];
        while (tmp[i] != '\0' && !is_sep(tmp[i])) {
            i++;
        }
        if (tmp[i] != '\0') {
            tmp[i] = '\0';
            i++;
        }
        if (strcmp(start, ".") == 0) {
            continue;
        }
        if (strcmp(start, "..") == 0) {
            if (count > 0 && strcmp(parts[count - 1], "..") != 0) {
                count--;
            } else if (!absolute) {
                if (count < A98_ARRAY_LEN(parts)) {
                    parts[count++] = start;
                }
            }
            continue;
        }
        if (count < A98_ARRAY_LEN(parts)) {
            parts[count++] = start;
        }
    }

    if (drive) {
        if (o + 3 > cap) {
            out[0] = '\0';
            return -1;
        }
        out[o++] = drive_char;
        out[o++] = ':';
    } else if (absolute) {
        if (o + 2 > cap) {
            out[0] = '\0';
            return -1;
        }
        out[o++] = '\\';
    }
    for (i = 0; i < (size_t)count; i++) {
        len = strlen(parts[i]);
        if (o > 0 && out[o - 1] != '\\' && out[o - 1] != ':') {
            if (o + 1 >= cap) {
                out[0] = '\0';
                return -1;
            }
            out[o++] = '\\';
        }
        if (o + len + 1 > cap) {
            out[0] = '\0';
            return -1;
        }
        memcpy(out + o, parts[i], len);
        o += len;
    }
    if (o == 0) {
        out[o++] = '.';
    }
    out[o] = '\0';
    return 0;
}

int a98_path_exists(const char *p)
{
    DWORD attr;
    if (p == NULL || p[0] == '\0') {
        return 0;
    }
    attr = GetFileAttributesA(p);
    return attr != 0xFFFFFFFFUL;
}

int a98_path_is_dir(const char *p)
{
    DWORD attr;
    if (p == NULL || p[0] == '\0') {
        return 0;
    }
    attr = GetFileAttributesA(p);
    if (attr == 0xFFFFFFFFUL) {
        return 0;
    }
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int a98_path_mkdir(const char *p)
{
    if (p == NULL || p[0] == '\0') {
        return -1;
    }
    if (CreateDirectoryA(p, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS && a98_path_is_dir(p)) {
        return 0;
    }
    return -1;
}

int a98_path_mkdirs(const char *p)
{
    char tmp[A98_MAX_PATH];
    size_t i;
    size_t len;

    if (p == NULL || p[0] == '\0') {
        return -1;
    }
    if (strlen(p) >= sizeof(tmp)) {
        return -1;
    }
    strcpy(tmp, p);

    /* Create every intermediate directory. Skip the drive prefix. */
    len = strlen(tmp);
    for (i = 0; i < len; i++) {
        if (is_sep(tmp[i]) && i > 0) {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[i - 1] != ':' && tmp[0] != '\0') {
                a98_path_mkdir(tmp);
            }
            tmp[i] = saved;
        }
    }
    return a98_path_mkdir(tmp);
}

const char *a98_path_basename(const char *p)
{
    const char *base;
    const char *q;

    if (p == NULL) {
        return "";
    }
    base = p;
    for (q = p; *q != '\0'; q++) {
        if (is_sep(*q)) {
            base = q + 1;
        }
    }
    return base;
}

void a98_path_dirname(const char *p, char *out, size_t cap)
{
    const char *last = NULL;
    const char *q;
    size_t n;

    if (out == NULL || cap == 0) {
        return;
    }
    if (p == NULL) {
        p = "";
    }
    for (q = p; *q != '\0'; q++) {
        if (is_sep(*q)) {
            last = q;
        }
    }
    if (last == NULL) {
        strncpy(out, ".", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    n = (size_t)(last - p);
    if (n == 0) {
        n = 1;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

static void lower_ascii(char *s)
{
    while (*s != '\0') {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
        s++;
    }
}

int a98_path_resolve_in_root(const char *root, const char *user_path,
                             char *resolved, size_t cap)
{
    char joined[A98_MAX_PATH * 2];
    char full[A98_MAX_PATH * 2];
    char full_lc[A98_MAX_PATH * 2];
    char root_full[A98_MAX_PATH * 2];
    char root_lc[A98_MAX_PATH * 2];
    DWORD n;
    size_t rlen;

    if (root == NULL || user_path == NULL || resolved == NULL || cap == 0) {
        return -1;
    }
    if (a98_path_is_absolute(user_path)) {
        if (strlen(user_path) >= sizeof(joined)) {
            return -1;
        }
        strcpy(joined, user_path);
    } else {
        if (a98_path_join(joined, sizeof(joined), root, user_path) != 0) {
            return -1;
        }
    }
    n = GetFullPathNameA(joined, sizeof(full), full, NULL);
    if (n == 0 || n >= sizeof(full)) {
        return -1;
    }
    n = GetFullPathNameA(root, sizeof(root_full), root_full, NULL);
    if (n == 0 || n >= sizeof(root_full)) {
        return -1;
    }
    /* Compare case-insensitively on copies: the resolved path handed back to
     * the caller keeps the on-disk spelling.  Lower-casing `full` in place
     * made every tool report paths in all lower case. */
    strcpy(full_lc, full);
    strcpy(root_lc, root_full);
    lower_ascii(full_lc);
    lower_ascii(root_lc);
    rlen = strlen(root_lc);
    while (rlen > 0 && root_lc[rlen - 1] == '\\') {
        root_lc[--rlen] = '\0';
    }
    if (rlen == 0) {
        return -1;
    }
    if (strncmp(full_lc, root_lc, rlen) != 0) {
        return 0;
    }
    if (full_lc[rlen] != '\0' && full_lc[rlen] != '\\') {
        return 0;
    }
    if (strlen(full) >= cap) {
        return -1;
    }
    strcpy(resolved, full);
    return 1;
}
