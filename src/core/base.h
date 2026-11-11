/*
 * Agent98 - base definitions and small helpers.
 *
 * Target: Windows 95/98/ME (Win32), 32-bit, built with OpenWatcom.
 * Keep this header free of Windows types so it can be reused by host tools.
 */
#ifndef A98_BASE_H
#define A98_BASE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#define A98_VERSION "0.1.0"
#define A98_NAME "Agent98"

/* Longest path we accept. Win9x MAX_PATH is 260. */
#define A98_MAX_PATH 260

#define A98_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define A98_MIN(a, b) ((a) < (b) ? (a) : (b))
#define A98_MAX(a, b) ((a) > (b) ? (a) : (b))
#define A98_UNUSED(x) ((void)(x))

void *a98_alloc(size_t n);
void *a98_realloc(void *p, size_t n);
void a98_free(void *p);
char *a98_strdup(const char *s);
char *a98_strndup(const char *s, size_t n);

int a98_strcasecmp(const char *a, const char *b);
int a98_strncasecmp(const char *a, const char *b, size_t n);

/* Format into a freshly allocated string, or NULL on failure. */
char *a98_strfmt(const char *fmt, ...);

/* Replace every occurrence of `from` in `s` with `to`, in place.
 * Returns 1 if at least one replacement happened. */
int a98_str_replace_all(char *s, size_t cap, const char *from, const char *to);

#endif /* A98_BASE_H */
