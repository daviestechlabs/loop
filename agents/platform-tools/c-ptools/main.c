/*
 * c-ptools — pure-C platform-tools service host (no Go).
 *
 * Env:
 *   TOOL_CALL_STORE_PATH   default /tmp/c-ptools-calls.json
 *   TOOL_HTTP_ADDR         e.g. :8081 or 127.0.0.1:8081 (IPv4 HTTP listener)
 *   TOOL_HTTP_AUTH_SECRET  required; dedicated 32-255 byte shared key
 *   TOOL_WORKSPACE_ROOT    default /tmp/platform-tools-ws
 *   TOOL_ARTIFACT_DIR      default /tmp/platform-tools-artifacts
 *   TOOL_DND_STATE_DIR     default /tmp/platform-tools-dnd
 *
 * Transport is authenticated HTTP only. There is no NATS client.
 */
#define _POSIX_C_SOURCE 200809L
#include "pt_http.h"
#include "pt_manager.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

static int http_address(const char *text, char host[16], int *port) {
    const char *colon = strchr(text, ':');
    const unsigned char *p;
    struct in_addr address;
    size_t length;
    unsigned value = 0;
    if (!colon || !colon[1]) return -1;
    length = (size_t)(colon - text);
    if (length >= 16u) return -1;
    if (length) {
        memcpy(host, text, length);
        host[length] = '\0';
    } else memcpy(host, "0.0.0.0", sizeof("0.0.0.0"));
    if (inet_pton(AF_INET, host, &address) != 1) return -1;
    for (p = (const unsigned char *)colon + 1u; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10u + (unsigned)(*p - '0');
        if (value > 65535u) return -1;
    }
    if (!value) return -1;
    *port = (int)value;
    return 0;
}

int main(void) {
    pt_manager *m;
    const char *store = getenv("TOOL_CALL_STORE_PATH");
    const char *ws = getenv("TOOL_WORKSPACE_ROOT");
    const char *art = getenv("TOOL_ARTIFACT_DIR");
    const char *dnd = getenv("TOOL_DND_STATE_DIR");
    const char *http_addr = getenv("TOOL_HTTP_ADDR");
    const char *secret = getenv("TOOL_HTTP_AUTH_SECRET");
    char host[16];
    int port = 0, result = 0;

    if (getenv("NATS_URL") && getenv("NATS_URL")[0]) {
        fprintf(stderr, "NATS_URL is not supported; set TOOL_HTTP_ADDR\n");
        return 1;
    }
    if (!http_addr || !http_addr[0]) {
        fprintf(stderr, "TOOL_HTTP_ADDR is required\n");
        return 1;
    }
    if (http_address(http_addr, host, &port) != 0 || !secret ||
        strlen(secret) < 32u || strlen(secret) > 255u) {
        fprintf(stderr, "HTTP requires a valid IPv4 address and a dedicated 32-255 byte TOOL_HTTP_AUTH_SECRET\n");
        return 1;
    }

    m = calloc(1, sizeof(*m));
    if (!m)
        return 1;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (pt_manager_init(m, store && store[0] ? store : "/tmp/c-ptools-calls.json", ws, art, dnd) != 0) {
        fprintf(stderr, "store open failed\n");
        free(m);
        return 1;
    }

    result = pt_http_serve(m, host, port, &g_stop, secret) == 0 ? 0 : 1;
    if (result) fprintf(stderr, "HTTP listener failed\n");

    g_stop = 1;
    pt_manager_destroy(m);
    free(m);
    return result;
}
