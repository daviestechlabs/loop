/*
 * c-toolstore — pure-C tool-call CAS / path policy CLI.
 *   c-toolstore valid-id <id>
 *   c-toolstore shard-name <call_id>
 *   c-toolstore record-dir <store_path>
 *   c-toolstore idem-key <user> <session> <agent> <tool> <idem>
 *   c-toolstore same-request <idem> <task> <turn> <user> <session> <agent> <tool> <input>
 *                            <idem> <task> <turn> <user> <session> <agent> <tool> <input>
 *   c-toolstore terminal <state|name>
 *   c-toolstore check-shard <call_id> <filename>
 *   c-toolstore atomic-write <path> [mode_octal]   # body on stdin; default mode 0600
 *   c-toolstore bounded-path <root> <raw>
 *   c-toolstore edit-mode <0|1>
 *   c-toolstore artifact-ext <content_type>
 * Exit 0 ok, 1 error (message on stderr). For same-request/terminal/valid-id: stdout "1"/"0".
 */
#define _POSIX_C_SOURCE 200809L
#include "toolstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(void) {
    size_t cap = 8192, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        size_t r;
        if (n + 4096 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf)
                return NULL;
        }
        r = fread(buf + n, 1, cap - n - 1, stdin);
        n += r;
        if (r == 0)
            break;
    }
    buf[n] = '\0';
    return buf;
}

static int parse_mode(const char *s, unsigned *mode) {
    unsigned long v;
    char *end = NULL;
    if (!s || !s[0])
        return -1;
    v = strtoul(s, &end, 8);
    if (!end || *end != '\0' || v > 0777)
        return -1;
    *mode = (unsigned)v;
    return 0;
}

int main(int argc, char **argv) {
    char out[TS_IDEM];
    char shard[TS_SHARD_NAME];
    char dir[TS_PATH];
    unsigned mode = 0600;
    char *body;
    size_t body_len;

    if (argc < 2) {
        fprintf(stderr,
                "usage: c-toolstore valid-id|shard-name|record-dir|idem-key|same-request|terminal|"
                "check-shard|atomic-write|bounded-path|edit-mode|artifact-ext ...\n");
        return 2;
    }
    if (strcmp(argv[1], "valid-id") == 0) {
        if (argc < 3)
            return 2;
        puts(ts_valid_id(argv[2]) ? "1" : "0");
        return ts_valid_id(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "shard-name") == 0) {
        if (argc < 3)
            return 2;
        if (ts_shard_name(argv[2], shard, sizeof(shard)) != TS_OK) {
            fprintf(stderr, "invalid call id\n");
            return 1;
        }
        puts(shard);
        return 0;
    }
    if (strcmp(argv[1], "record-dir") == 0) {
        if (argc < 3)
            return 2;
        if (ts_record_dir(argv[2], dir, sizeof(dir)) != TS_OK) {
            fprintf(stderr, "record-dir failed\n");
            return 1;
        }
        puts(dir);
        return 0;
    }
    if (strcmp(argv[1], "idem-key") == 0) {
        if (argc < 7)
            return 2;
        if (ts_idempotency_key(argv[2], argv[3], argv[4], argv[5], argv[6], out, sizeof(out)) != TS_OK) {
            fprintf(stderr, "idem-key failed\n");
            return 1;
        }
        fputs(out, stdout);
        fputc('\n', stdout);
        return 0;
    }
    if (strcmp(argv[1], "same-request") == 0) {
        int eq;
        if (argc < 18)
            return 2;
        eq = ts_same_request(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9], argv[10],
                             argv[11], argv[12], argv[13], argv[14], argv[15], argv[16], argv[17]);
        puts(eq ? "1" : "0");
        return eq ? 0 : 1;
    }
    if (strcmp(argv[1], "terminal") == 0) {
        int term;
        if (argc < 3)
            return 2;
        if (argv[2][0] >= '0' && argv[2][0] <= '9' && argv[2][1] == '\0')
            term = ts_is_terminal(argv[2][0] - '0');
        else
            term = ts_is_terminal_name(argv[2]);
        puts(term ? "1" : "0");
        return term ? 0 : 1;
    }
    if (strcmp(argv[1], "check-shard") == 0) {
        if (argc < 4)
            return 2;
        if (ts_check_shard_identity(argv[2], argv[3]) != TS_OK) {
            fprintf(stderr, "shard identity mismatch\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "atomic-write") == 0) {
        if (argc < 3)
            return 2;
        if (argc >= 4 && parse_mode(argv[3], &mode) != 0) {
            fprintf(stderr, "bad mode\n");
            return 2;
        }
        body = read_all();
        if (!body) {
            fprintf(stderr, "read stdin failed\n");
            return 1;
        }
        body_len = strlen(body);
        /* Prefer binary length: re-read is awkward; for tool store JSON text is fine.
         * Use size excluding no embedded NUL for JSON payloads. */
        if (ts_atomic_write(argv[2], body, body_len, mode) != TS_OK) {
            fprintf(stderr, "atomic-write failed\n");
            free(body);
            return 1;
        }
        free(body);
        return 0;
    }
    if (strcmp(argv[1], "bounded-path") == 0) {
        char path[TS_PATH];
        if (argc < 4)
            return 2;
        if (ts_bounded_path(argv[2], argv[3], path, sizeof(path)) != TS_OK) {
            fprintf(stderr, "path outside workspace root\n");
            return 1;
        }
        puts(path);
        return 0;
    }
    if (strcmp(argv[1], "edit-mode") == 0) {
        char mode[16];
        int dry;
        if (argc < 3)
            return 2;
        dry = atoi(argv[2]) ? 1 : 0;
        if (ts_edit_mode(dry, mode, sizeof(mode)) != TS_OK)
            return 1;
        puts(mode);
        return 0;
    }
    if (strcmp(argv[1], "artifact-ext") == 0) {
        char ext[16];
        const char *ct = (argc >= 3) ? argv[2] : "";
        if (ts_artifact_extension(ct, ext, sizeof(ext)) != TS_OK)
            return 1;
        puts(ext);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
