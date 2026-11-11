#ifndef A98_TOOLS_H
#define A98_TOOLS_H

#include "../core/base.h"
#include "../core/str.h"
#include "../../third_party/cjson/cJSON.h"

struct Agent;

typedef struct ToolDef {
    const char *name;
    const char *description;
    const char *params_json;
    int needs_confirm;
    int (*run)(struct Agent *agent, cJSON *args, Str *out, char *err, size_t errcap);
} ToolDef;

const ToolDef *tools_all(int *count);
const ToolDef *tools_find(const char *name);

/* Execute a tool by name. `args_json` may be empty. Returns 0 on success;
 * on failure returns -1 and puts a model-readable message in `err`. */
int tools_execute(struct Agent *agent, const char *name, const char *args_json,
                  Str *out, char *err, size_t errcap);

#endif /* A98_TOOLS_H */
