/*
 * c-toolpolicy — pure-C platform-tools policy validation CLI.
 *   c-toolpolicy parse-risk <raw>          → risk name
 *   c-toolpolicy validate-tool             # stdin tool JSON
 *   c-toolpolicy validate-policy           # stdin policy JSON {version,tools,agents}
 * Exit 0 ok, 1 error (message on stderr).
 */
#define _POSIX_C_SOURCE 200809L
#include "toolpolicy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(void) {
    size_t cap = 8192, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t r;
        if (n + 4096 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) return NULL;
        }
        r = fread(buf + n, 1, cap - n - 1, stdin);
        n += r;
        if (r == 0) break;
    }
    buf[n] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    char err[TP_MSG];
    char *body;

    if (argc < 2) {
        fprintf(stderr, "usage: c-toolpolicy parse-risk|validate-tool|validate-policy\n");
        return 2;
    }
    if (strcmp(argv[1], "parse-risk") == 0) {
        if (argc < 3) return 2;
        puts(tp_parse_risk(argv[2]));
        return 0;
    }
    if (strcmp(argv[1], "validate-tool") == 0) {
        body = read_all();
        if (!body) return 1;
        if (tp_validate_tool_json(body, err, sizeof(err)) != TP_OK) {
            fprintf(stderr, "%s\n", err);
            free(body);
            return 1;
        }
        free(body);
        return 0;
    }
    if (strcmp(argv[1], "validate-policy") == 0) {
        body = read_all();
        if (!body) return 1;
        if (tp_validate_policy_json(body, err, sizeof(err)) != TP_OK) {
            fprintf(stderr, "%s\n", err);
            free(body);
            return 1;
        }
        free(body);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
