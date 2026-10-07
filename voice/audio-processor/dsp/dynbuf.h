/*
 * dynbuf — amortized grow + idle shrink for variable-size pure-C spans.
 * Pattern mirrors audio_engine utterance growth: double until fit, reclaim
 * when empty/idle so capacity can track load without fixed hard caps only.
 */
#ifndef VOICE_C_DYNBUF_H
#define VOICE_C_DYNBUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dynbuf {
    uint8_t *data;
    size_t len;
    size_t cap;
} dynbuf;

void dynbuf_init(dynbuf *b);
void dynbuf_free(dynbuf *b);

/* Ensure capacity >= need; amortized doubling. Returns 0 on success, -1 OOM. */
int dynbuf_reserve(dynbuf *b, size_t need);

/* Reserve without growing beyond max_capacity. */
int dynbuf_reserve_bounded(dynbuf *b, size_t need, size_t max_capacity);

/* Append bytes; grows as needed. Returns 0 on success, -1 OOM. */
int dynbuf_append(dynbuf *b, const void *src, size_t n);

/* Append without growing beyond max_capacity. */
int dynbuf_append_bounded(dynbuf *b, const void *src, size_t n, size_t max_capacity);

/* Extend by n bytes and return the writable appended span.
 * Failure leaves the length unchanged and sets span to NULL. */
int dynbuf_extend_bounded(
    dynbuf *b,
    size_t n,
    size_t max_capacity,
    uint8_t **span);

/* Clear length; if cap > min_keep and len==0, shrink cap toward min_keep. */
void dynbuf_clear(dynbuf *b, size_t min_keep);

/* Force reclaim to min_keep (or free if min_keep==0). Len set to 0. */
void dynbuf_reclaim(dynbuf *b, size_t min_keep);

size_t dynbuf_len(const dynbuf *b);
size_t dynbuf_cap(const dynbuf *b);
uint8_t *dynbuf_data(dynbuf *b);

/* Grow/shrink pointer table (sessions, etc.). */
typedef struct dynptr {
    void **items;
    size_t len;
    size_t cap;
} dynptr;

void dynptr_init(dynptr *p);
void dynptr_free(dynptr *p);
int dynptr_reserve(dynptr *p, size_t need);
int dynptr_push(dynptr *p, void *item);
void dynptr_clear(dynptr *p, size_t min_keep);

#ifdef __cplusplus
}
#endif

#endif
