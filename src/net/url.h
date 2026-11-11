#ifndef A98_URL_H
#define A98_URL_H

#include "../core/base.h"

typedef struct {
    int tls;
    char host[256];
    int port;
    char path[1024];
} Url;

/* Parse http://host[:port][/path] or https://... . Returns 0 on success. */
int url_parse(const char *url, Url *out, char *err, size_t errcap);

/* Format host[:port] for the Host header. */
void url_host_header(const Url *u, char *out, size_t cap);

#endif /* A98_URL_H */
