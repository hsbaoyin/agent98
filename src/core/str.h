/*
 * Agent98 - growable string buffer.
 */
#ifndef A98_STR_H
#define A98_STR_H

#include "base.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

void str_init(Str *s);
void str_free(Str *s);
void str_clear(Str *s);

int str_reserve(Str *s, size_t extra);
int str_append(Str *s, const void *data, size_t len);
int str_append_cstr(Str *s, const char *text);
int str_append_char(Str *s, char c);
int str_appendf(Str *s, const char *fmt, ...);
int str_set(Str *s, const char *text);
int str_setn(Str *s, const char *text, size_t len);

/* Detach the buffer; caller owns it and must a98_free() it. */
char *str_detach(Str *s);

/* NUL-terminated view, never NULL (empty string when empty). */
const char *str_cstr(const Str *s);

/* Remove trailing CR/LF characters. */
void str_trim_eol(Str *s);

#endif /* A98_STR_H */
