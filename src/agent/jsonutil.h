#ifndef A98_JSONUTIL_H
#define A98_JSONUTIL_H

#include "../core/base.h"
#include "../../third_party/cjson/cJSON.h"

cJSON *json_parse(const char *text, size_t len);
const char *json_string(cJSON *obj, const char *name);
int json_int(cJSON *obj, const char *name, int def);
int json_bool(cJSON *obj, const char *name, int def);
char *json_escape_dup(const char *s);

#endif /* A98_JSONUTIL_H */
