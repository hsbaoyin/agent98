#include "utf8.h"
#include "str.h"

#ifdef _WIN32
#include <windows.h>
#endif

int a98_utf8_decode(const char *s, size_t len, unsigned int *cp)
{
    unsigned char c;
    unsigned int v;
    int need;
    int i;

    if (s == NULL || len == 0) {
        return -1;
    }
    c = (unsigned char)s[0];
    if (c < 0x80) {
        if (cp != NULL) {
            *cp = c;
        }
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        v = c & 0x1F;
        need = 1;
    } else if ((c & 0xF0) == 0xE0) {
        v = c & 0x0F;
        need = 2;
    } else if ((c & 0xF8) == 0xF0) {
        v = c & 0x07;
        need = 3;
    } else {
        return -1;
    }
    if (len < (size_t)(need + 1)) {
        return -1;
    }
    for (i = 1; i <= need; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) {
            return -1;
        }
        v = (v << 6) | (unsigned int)(cc & 0x3F);
    }
    /* Reject overlong encodings, surrogates and out-of-range values. */
    if ((need == 1 && v < 0x80) ||
        (need == 2 && v < 0x800) ||
        (need == 3 && v < 0x10000) ||
        v > 0x10FFFF ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        return -1;
    }
    if (cp != NULL) {
        *cp = v;
    }
    return need + 1;
}

int a98_utf8_encode(unsigned int cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return -1;
        }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return -1;
}

int a98_utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    int n;

    if (s == NULL) {
        return 0;
    }
    while (i < len) {
        n = a98_utf8_decode(s + i, len - i, NULL);
        if (n <= 0) {
            return 0;
        }
        i += (size_t)n;
    }
    return 1;
}

unsigned short *a98_utf8_to_utf16(const char *s, size_t *out_units)
{
    size_t cap;
    size_t len;
    size_t i;
    size_t o;
    unsigned short *w;
    int n;
    unsigned int cp;

    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    cap = len + 1;
    w = (unsigned short *)a98_alloc((cap + 1) * sizeof(unsigned short));
    if (w == NULL) {
        return NULL;
    }
    i = 0;
    o = 0;
    while (i < len) {
        if (o + 3 > cap) {
            unsigned short *bigger;
            cap = cap * 2 + 16;
            bigger = (unsigned short *)a98_realloc(w, (cap + 1) * sizeof(unsigned short));
            if (bigger == NULL) {
                a98_free(w);
                return NULL;
            }
            w = bigger;
        }
        n = a98_utf8_decode(s + i, len - i, &cp);
        if (n <= 0) {
            w[o++] = (unsigned short)'?';
            i++;
            continue;
        }
        i += (size_t)n;
        if (cp < 0x10000) {
            w[o++] = (unsigned short)cp;
        } else {
            unsigned int v = cp - 0x10000;
            w[o++] = (unsigned short)(0xD800 + (v >> 10));
            w[o++] = (unsigned short)(0xDC00 + (v & 0x3FF));
        }
    }
    w[o] = 0;
    if (out_units != NULL) {
        *out_units = o;
    }
    return w;
}

char *a98_utf16_to_utf8(const unsigned short *w, size_t units)
{
    Str out;
    size_t i;
    char tmp[4];
    int n;

    str_init(&out);
    if (w == NULL) {
        return a98_strdup("");
    }
    for (i = 0; i < units; i++) {
        unsigned int cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
        }
        n = a98_utf8_encode(cp, tmp);
        if (n > 0) {
            str_append(&out, tmp, (size_t)n);
        }
    }
    return str_detach(&out);
}

void a98_utf8_sanitize(char *s)
{
    size_t len;
    size_t i;
    int n;

    if (s == NULL) {
        return;
    }
    len = strlen(s);
    i = 0;
    while (i < len) {
        n = a98_utf8_decode(s + i, len - i, NULL);
        if (n <= 0) {
            s[i] = '?';
            i++;
        } else {
            i += (size_t)n;
        }
    }
}

#ifdef _WIN32
char *a98_utf8_to_acp(const char *s)
{
    unsigned short *w;
    size_t units = 0;
    int need;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    w = a98_utf8_to_utf16(s, &units);
    if (w == NULL) {
        return NULL;
    }
    need = WideCharToMultiByte(CP_ACP, 0, (const wchar_t *)w, (int)units,
                               NULL, 0, NULL, NULL);
    if (need <= 0) {
        a98_free(w);
        return a98_strdup("");
    }
    out = (char *)a98_alloc((size_t)need + 1);
    if (out == NULL) {
        a98_free(w);
        return NULL;
    }
    WideCharToMultiByte(CP_ACP, 0, (const wchar_t *)w, (int)units,
                        out, need, NULL, NULL);
    out[need] = '\0';
    a98_free(w);
    return out;
}

char *a98_acp_to_utf8(const char *s)
{
    int need;
    unsigned short *w;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    need = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (need <= 0) {
        return a98_strdup("");
    }
    w = (unsigned short *)a98_alloc(((size_t)need + 1) * sizeof(unsigned short));
    if (w == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, s, -1, (wchar_t *)w, need);
    out = a98_utf16_to_utf8(w, (size_t)(need - 1));
    a98_free(w);
    return out;
}
#endif
