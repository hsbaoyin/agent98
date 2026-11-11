/*
 * Agent98 - path helpers for Win32 (Win9x safe).
 */
#ifndef A98_PATH_H
#define A98_PATH_H

#include "base.h"

int a98_path_exe_dir(char *out, size_t cap);
int a98_path_join(char *out, size_t cap, const char *a, const char *b);
int a98_path_normalize(char *out, size_t cap, const char *in);
int a98_path_is_absolute(const char *p);
int a98_path_exists(const char *p);
int a98_path_is_dir(const char *p);
int a98_path_mkdir(const char *p);
int a98_path_mkdirs(const char *p);
const char *a98_path_basename(const char *p);
void a98_path_dirname(const char *p, char *out, size_t cap);

/*
 * Resolve `user_path` against `root` and refuse anything that escapes root
 * after lexical normalization. Returns 1 when allowed, 0 when outside,
 * -1 on error. `resolved` receives the absolute path when allowed.
 */
int a98_path_resolve_in_root(const char *root, const char *user_path,
                             char *resolved, size_t cap);

#endif /* A98_PATH_H */
