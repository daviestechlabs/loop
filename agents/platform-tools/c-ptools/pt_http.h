#ifndef PT_HTTP_H
#define PT_HTTP_H

#include <signal.h>

typedef struct pt_manager pt_manager;

/* Authenticated HTTP server on the supplied IPv4 host and port. */
int pt_http_serve(pt_manager *m, const char *host, int port,
                  volatile sig_atomic_t *stop, const char *secret);

#endif
