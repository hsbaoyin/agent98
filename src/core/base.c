#include "base.h"

void *a98_alloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return malloc(n);
}

void *a98_realloc(void *p, size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return realloc(p, n);
}

void a98_free(void *p)
{
    if (p != NULL) {
        free(p);
    }
}

char *a98_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return a98_strndup(s, strlen(s));
}

char *a98_strndup(const char *s, size_t n)
{
    char *p;
    size_t len = 0;

    if (s == NULL) {
        return NULL;
    }
    while (len < n && s[len] != '\0') {
        len++;
    }
    p = (char *)a98_alloc(len + 1);
    if (p == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(p, s, len);
    }
    p[len] = '\0';
    return p;
}

int a98_strcasecmp(const char *a, const char *b)
{
    int ca;
    int cb;

    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    for (;;) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
        a++;
        b++;
    }
}

int a98_strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;
    int ca;
    int cb;

    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    for (i = 0; i < n; i++) {
        ca = (unsigned char)a[i];
        cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
    return 0;
}

char *a98_strfmt(const char *fmt, ...)
{
    char tmp[1024];
    char *out;
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        /* Older runtimes return -1 when the buffer is too small. */
        tmp[sizeof(tmp) - 1] = '\0';
        n = (int)strlen(tmp);
    }
    if ((size_t)n < sizeof(tmp)) {
        return a98_strndup(tmp, (size_t)n);
    }
    out = (char *)a98_alloc((size_t)n + 1);
    if (out == NULL) {
        return NULL;
    }
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

int a98_str_replace_all(char *s, size_t cap, const char *from, const char *to)
{
    size_t from_len;
    size_t to_len;
    size_t len;
    size_t pos;
    int replaced = 0;

    if (s == NULL || from == NULL || to == NULL) {
        return 0;
    }
    from_len = strlen(from);
    to_len = strlen(to);
    if (from_len == 0) {
        return 0;
    }
    len = strlen(s);
    pos = 0;
    while (pos + from_len <= len) {
        if (memcmp(s + pos, from, from_len) == 0) {
            size_t tail = len - pos - from_len;
            if (len - from_len + to_len >= cap) {
                break;
            }
            memmove(s + pos + to_len, s + pos + from_len, tail + 1);
            memcpy(s + pos, to, to_len);
            len = len - from_len + to_len;
            pos += to_len;
            replaced = 1;
        } else {
            pos++;
        }
    }
    return replaced;
}
