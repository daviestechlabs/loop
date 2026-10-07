/*
 * c-pb — pure-C protobuf CLI for residual diagnostics / standalone ops.
 * Product residual host uses c-companions /api/v1/policy/pb/encode|decode (no dual-run).
 *
 *   c-pb encode <kind>  < key=value lines  > binary
 *   c-pb decode <kind>  < binary           > key=value lines
 *   c-pb selftest
 */
#define _POSIX_C_SOURCE 200809L
#include "pb_cli_codec.h"
#include "pb_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *f, size_t *out_len) {
    size_t cap = 65536, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t r;
        if (n + 4096 > cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        r = fread(buf + n, 1, 4096, f);
        n += r;
        if (r < 4096) break;
    }
    if (n + 1 > cap) {
        char *nb = (char *)realloc(buf, n + 1);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static int cmd_encode(const char *kind) {
    size_t n = 0, on = 0;
    char *in = read_all(stdin, &n);
    uint8_t out[256 * 1024];
    int rc;
    if (!in) return 1;
    rc = pb_cli_encode(kind, in, out, sizeof(out), &on);
    free(in);
    if (rc == PB_CLI_UNKNOWN) {
        fprintf(stderr, "unknown encode kind: %s\n", kind);
        return 2;
    }
    if (rc != PB_CLI_OK || !on) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    if (fwrite(out, 1, on, stdout) != on) return 1;
    return 0;
}

static int cmd_decode(const char *kind) {
    size_t n = 0;
    char *in = read_all(stdin, &n);
    char out[65536];
    int rc;
    if (!in) return 1;
    rc = pb_cli_decode(kind, (const uint8_t *)in, n, out, sizeof(out));
    free(in);
    if (rc == PB_CLI_UNKNOWN) {
        fprintf(stderr, "unknown decode kind: %s\n", kind);
        return 2;
    }
    if (rc != PB_CLI_OK) return 1;
    fputs(out, stdout);
    return 0;
}

static int cmd_selftest(void) {
    uint8_t buf[8192];
    char kv[4096], lines[8192];
    size_t n = 0;
    int fails = 0;
    pb_stt_lifecycle sl, sl2;

    snprintf(kv, sizeof(kv),
             "request_id=r1\nuser_id=u1\nsession_id=s1\ntext=hello\nresponse_subject=ev\n"
             "premium=1\nenable_rag=0\nenable_tts=1\n");
    if (pb_cli_encode("turn-start", kv, buf, sizeof(buf), &n) != PB_CLI_OK || !n) {
        fprintf(stderr, "FAIL encode turn-start\n");
        fails++;
    } else if (pb_cli_decode("turn-start", buf, n, lines, sizeof(lines)) != PB_CLI_OK ||
               !strstr(lines, "request_id=r1")) {
        fprintf(stderr, "FAIL decode turn-start\n");
        fails++;
    }

    memset(&sl, 0, sizeof(sl));
    snprintf(sl.session_id, sizeof(sl.session_id), "sess");
    snprintf(sl.type, sizeof(sl.type), "stream_ended");
    sl.timestamp_ms = 12345;
    n = pb_enc_stt_lifecycle(buf, sizeof(buf), &sl);
    if (!n || pb_dec_stt_lifecycle(buf, n, &sl2) != 0 || sl2.timestamp_ms != 12345) {
        fprintf(stderr, "FAIL stt-lifecycle roundtrip\n");
        fails++;
    }

    if (fails) {
        fprintf(stderr, "%d fails\n", fails);
        return 1;
    }
    printf("ALL PASS c-pb\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: c-pb encode|decode|selftest [kind]\n");
        return 2;
    }
    if (strcmp(argv[1], "selftest") == 0) return cmd_selftest();
    if (argc < 3) {
        fprintf(stderr, "kind required\n");
        return 2;
    }
    if (strcmp(argv[1], "encode") == 0) return cmd_encode(argv[2]);
    if (strcmp(argv[1], "decode") == 0) return cmd_decode(argv[2]);
    fprintf(stderr, "unknown command\n");
    return 2;
}
