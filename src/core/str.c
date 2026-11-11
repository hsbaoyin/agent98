#include "str.h"

void str_init(Str *s)
{
    if (s == NULL) {
        return;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_free(Str *s)
{
    if (s == NULL) {
        return;
    }
    a98_free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_clear(Str *s)
{
    if (s == NULL) {
        return;
    }
    s->len = 0;
    if (s->data != NULL) {
        s->data[0] = '\0';
    }
}

int str_reserve(Str *s, size_t extra)
{
    size_t need;
    size_t cap;
    char *p;

    if (s == NULL) {
        return -1;
    }
    need = s->len + extra + 1;
    if (need <= s->cap) {
        return 0;
    }
    cap = s->cap ? s->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = (char *)a98_realloc(s->data, cap);
    if (p == NULL) {
        return -1;
    }
    s->data = p;
    s->cap = cap;
    if (s->len == 0) {
        s->data[0] = '\0';
    }
    return 0;
}

int str_append(Str *s, const void *data, size_t len)
{
    if (s == NULL) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (str_reserve(s, len) != 0) {
        return -1;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
    return 0;
}

int str_append_cstr(Str *s, const char *text)
{
    if (text == NULL) {
        return 0;
    }
    return str_append(s, text, strlen(text));
}

int str_append_char(Str *s, char c)
{
    return str_append(s, &c, 1);
}

int str_appendf(Str *s, const char *fmt, ...)
{
    char tmp[1024];
    char *big;
    int n;
    int rc;
    va_list ap;

    if (s == NULL || fmt == NULL) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        tmp[sizeof(tmp) - 1] = '\0';
        n = (int)strlen(tmp);
    }
    if ((size_t)n < sizeof(tmp)) {
        return str_append(s, tmp, (size_t)n);
    }
    big = (char *)a98_alloc((size_t)n + 1);
    if (big == NULL) {
        return -1;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    rc = str_append(s, big, (size_t)n);
    a98_free(big);
    return rc;
}

int str_set(Str *s, const char *text)
{
    if (s == NULL) {
        return -1;
    }
    str_clear(s);
    return str_append_cstr(s, text);
}

int str_setn(Str *s, const char *text, size_t len)
{
    if (s == NULL) {
        return -1;
    }
    str_clear(s);
    return str_append(s, text, len);
}

char *str_detach(Str *s)
{
    char *p;

    if (s == NULL) {
        return NULL;
    }
    if (s->data == NULL) {
        p = a98_strdup("");
    } else {
        p = s->data;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return p;
}

const char *str_cstr(const Str *s)
{
    if (s == NULL || s->data == NULL) {
        return "";
    }
    return s->data;
}

void str_trim_eol(Str *s)
{
    if (s == NULL || s->data == NULL) {
        return;
    }
    while (s->len > 0) {
        char c = s->data[s->len - 1];
        if (c == '\r' || c == '\n') {
            s->len--;
        } else {
            break;
        }
    }
    s->data[s->len] = '\0';
}
