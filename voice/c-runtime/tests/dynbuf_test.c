/*
 * Drive shipped dynbuf grow/shrink API: expand under append pressure, reclaim on idle.
 */
#include "dynbuf.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures;

static void expect(const char *name, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    } else {
        printf("PASS %s\n", name);
    }
}

int main(void) {
    dynbuf b;
    dynptr p;
    size_t cap_after_grow;
    char chunk[1024];
    int i;

    failures = 0;
    memset(chunk, 'A', sizeof(chunk));

    dynbuf_init(&b);
    expect("init empty", dynbuf_len(&b) == 0 && dynbuf_cap(&b) == 0);

    for (i = 0; i < 20; i++) {
        expect("append", dynbuf_append(&b, chunk, sizeof(chunk)) == 0);
    }
    expect("len after grow", dynbuf_len(&b) == 20 * 1024);
    cap_after_grow = dynbuf_cap(&b);
    expect("cap grew", cap_after_grow >= dynbuf_len(&b) && cap_after_grow > 0);

    dynbuf_clear(&b, 4096);
    expect("len cleared", dynbuf_len(&b) == 0);
    expect("cap reclaimed toward min", dynbuf_cap(&b) == 4096 || dynbuf_cap(&b) < cap_after_grow);

    dynbuf_reclaim(&b, 0);
    expect("full reclaim", dynbuf_cap(&b) == 0 && dynbuf_data(&b) == NULL);

    expect("bounded reserve", dynbuf_reserve_bounded(&b, 257, 300) == 0);
    expect("bounded reserve clamps growth", dynbuf_cap(&b) == 300);
    expect(
        "bounded reserve rejects limit",
        dynbuf_reserve_bounded(&b, 301, 300) != 0 && dynbuf_cap(&b) == 300);
    expect(
        "bounded append fills limit",
        dynbuf_append_bounded(&b, chunk, 300, 300) == 0 &&
        dynbuf_len(&b) == 300 && dynbuf_cap(&b) == 300);
    expect(
        "bounded append rejects limit without mutation",
        dynbuf_append_bounded(&b, "x", 1, 300) != 0 &&
        dynbuf_len(&b) == 300 && dynbuf_cap(&b) == 300);
    dynbuf_reclaim(&b, 0);

    {
        uint8_t *span = NULL;
        expect(
            "bounded extend returns appended span",
            dynbuf_extend_bounded(&b, 128, 256, &span) == 0 &&
            span == dynbuf_data(&b) && dynbuf_len(&b) == 128);
        if (span) memset(span, 0x5a, 128);
        expect(
            "bounded extend span is writable",
            dynbuf_data(&b) != NULL &&
            dynbuf_data(&b)[0] == 0x5a && dynbuf_data(&b)[127] == 0x5a);
        span = (uint8_t *)(uintptr_t)1u;
        expect(
            "bounded extend rejects limit without mutation",
            dynbuf_extend_bounded(&b, 129, 256, &span) != 0 &&
            span == NULL && dynbuf_len(&b) == 128);
        expect(
            "bounded extend rejects empty span",
            dynbuf_extend_bounded(&b, 0, 256, &span) != 0 &&
            span == NULL && dynbuf_len(&b) == 128);
        span = (uint8_t *)(uintptr_t)1u;
        expect(
            "bounded extend rejects NULL buffer",
            dynbuf_extend_bounded(NULL, 1, 256, &span) != 0 && span == NULL);
        expect(
            "bounded extend rejects NULL result",
            dynbuf_extend_bounded(&b, 1, 256, NULL) != 0 &&
            dynbuf_len(&b) == 128);
        {
            size_t saved_len = b.len;
            size_t saved_cap = b.cap;
            b.len = SIZE_MAX;
            b.cap = SIZE_MAX;
            span = (uint8_t *)(uintptr_t)1u;
            expect(
                "bounded extend rejects overflow",
                dynbuf_extend_bounded(&b, 1, SIZE_MAX, &span) != 0 &&
                span == NULL && b.len == SIZE_MAX);
            b.len = saved_len;
            b.cap = saved_cap;
        }
        dynbuf_reclaim(&b, 0);
    }

    /* Second grow after reclaim */
    expect("append after reclaim", dynbuf_append(&b, "hi", 2) == 0);
    expect("data ok", dynbuf_len(&b) == 2 && dynbuf_data(&b)[0] == 'h');
    expect("reject NULL append", dynbuf_append(&b, NULL, 1) != 0 && dynbuf_len(&b) == 2);
    size_t saved_cap = b.cap;
    b.len = SIZE_MAX;
    b.cap = SIZE_MAX;
    expect("reject append overflow", dynbuf_append(&b, "x", 1) != 0);
    b.len = 2;
    b.cap = saved_cap;
    dynbuf_free(&b);

    dynptr_init(&p);
    for (i = 0; i < 100; i++) {
        expect("push", dynptr_push(&p, (void *)(uintptr_t)(i + 1)) == 0);
    }
    expect("ptr cap grew", p.cap >= 100);
    dynptr_clear(&p, 0);
    expect("ptr reclaimed", p.cap == 0 && p.items == NULL);
    dynptr_free(&p);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ALL PASS dynbuf grow/shrink\n");
    return 0;
}
