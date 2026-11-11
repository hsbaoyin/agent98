#include "jsonutil.h"
#include "../core/str.h"

cJSON *json_parse(const char *text, size_t len)
{
    if (text == NULL) {
        return NULL;
    }
    return cJSON_ParseWithLength(text, len);
}

const char *json_string(cJSON *obj, const char *name)
{
    cJSON *item;
    if (obj == NULL) {
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return NULL;
}

int json_int(cJSON *obj, const char *name, int def)
{
    cJSON *item;
    if (obj == NULL) {
        return def;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        return (int)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return atoi(item->valuestring);
    }
    return def;
}

int json_bool(cJSON *obj, const char *name, int def)
{
    cJSON *item;
    if (obj == NULL) {
        return def;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0.0;
    }
    return def;
}

char *json_escape_dup(const char *s)
{
    Str out;
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    str_init(&out);
    while (*p != '\0') {
        switch (*p) {
        case '"':
            str_append_cstr(&out, "\\\"");
            break;
        case '\\':
            str_append_cstr(&out, "\\\\");
            break;
        case '\b':
            str_append_cstr(&out, "\\b");
            break;
        case '\f':
            str_append_cstr(&out, "\\f");
            break;
        case '\n':
            str_append_cstr(&out, "\\n");
            break;
        case '\r':
            str_append_cstr(&out, "\\r");
            break;
        case '\t':
            str_append_cstr(&out, "\\t");
            break;
        default:
            if (*p < 0x20) {
                str_appendf(&out, "\\u%04x", (unsigned)*p);
            } else {
                str_append_char(&out, (char)*p);
            }
            break;
        }
        p++;
    }
    return str_detach(&out);
}
