#include "dynbuf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void dynbuf_init(dynbuf *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void dynbuf_free(dynbuf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int dynbuf_reserve_bounded(dynbuf *b, size_t need, size_t max_capacity) {
    size_t newcap;
    uint8_t *nb;
    if (!b) return -1;
    if (need <= b->cap) return 0;
    if (need > max_capacity) return -1;
    newcap = b->cap ? b->cap : 256u;
    if (newcap > max_capacity) newcap = max_capacity;
    while (newcap < need) {
        if (newcap > max_capacity / 2u) {
            newcap = max_capacity;
            break;
        }
        newcap *= 2u;
    }
    nb = (uint8_t *)realloc(b->data, newcap);
    if (!nb) return -1;
    b->data = nb;
    b->cap = newcap;
    return 0;
}

int dynbuf_reserve(dynbuf *b, size_t need) {
    return dynbuf_reserve_bounded(b, need, SIZE_MAX);
}

int dynbuf_append_bounded(dynbuf *b, const void *src, size_t n, size_t max_capacity) {
    if (!b) return -1;
    if (n == 0) return 0;
    if (!src || n > SIZE_MAX - b->len) return -1;
    if (dynbuf_reserve_bounded(b, b->len + n, max_capacity) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int dynbuf_extend_bounded(
    dynbuf *b,
    size_t n,
    size_t max_capacity,
    uint8_t **span
) {
    size_t offset;
    if (span) *span = NULL;
    if (!b || !span || n == 0 || n > SIZE_MAX - b->len) return -1;
    offset = b->len;
    if (dynbuf_reserve_bounded(b, offset + n, max_capacity) != 0) return -1;
    *span = b->data + offset;
    b->len = offset + n;
    return 0;
}

int dynbuf_append(dynbuf *b, const void *src, size_t n) {
    return dynbuf_append_bounded(b, src, n, SIZE_MAX);
}

void dynbuf_clear(dynbuf *b, size_t min_keep) {
    if (!b) return;
    b->len = 0;
    if (b->cap > min_keep && min_keep > 0) {
        uint8_t *nb = (uint8_t *)realloc(b->data, min_keep);
        if (nb || min_keep == 0) {
            b->data = nb;
            b->cap = min_keep;
        }
    } else if (min_keep == 0) {
        free(b->data);
        b->data = NULL;
        b->cap = 0;
    }
}

void dynbuf_reclaim(dynbuf *b, size_t min_keep) {
    dynbuf_clear(b, min_keep);
}

size_t dynbuf_len(const dynbuf *b) {
    return b ? b->len : 0;
}

size_t dynbuf_cap(const dynbuf *b) {
    return b ? b->cap : 0;
}

uint8_t *dynbuf_data(dynbuf *b) {
    return b ? b->data : NULL;
}

void dynptr_init(dynptr *p) {
    if (!p) return;
    p->items = NULL;
    p->len = 0;
    p->cap = 0;
}

void dynptr_free(dynptr *p) {
    if (!p) return;
    free(p->items);
    p->items = NULL;
    p->len = 0;
    p->cap = 0;
}

int dynptr_reserve(dynptr *p, size_t need) {
    size_t newcap;
    void **nb;
    if (!p) return -1;
    if (need <= p->cap) return 0;
    if (need > SIZE_MAX / sizeof(void *)) return -1;
    newcap = p->cap ? p->cap : 8;
    while (newcap < need) {
        if (newcap > (SIZE_MAX / 2 / sizeof(void *))) {
            newcap = need;
            break;
        }
        newcap *= 2;
    }
    nb = (void **)realloc(p->items, newcap * sizeof(void *));
    if (!nb) return -1;
    p->items = nb;
    p->cap = newcap;
    return 0;
}

int dynptr_push(dynptr *p, void *item) {
    if (dynptr_reserve(p, p->len + 1) != 0) return -1;
    p->items[p->len++] = item;
    return 0;
}

void dynptr_clear(dynptr *p, size_t min_keep) {
    if (!p) return;
    p->len = 0;
    if (p->cap > min_keep) {
        if (min_keep == 0) {
            free(p->items);
            p->items = NULL;
            p->cap = 0;
        } else {
            void **nb = (void **)realloc(p->items, min_keep * sizeof(void *));
            if (nb) {
                p->items = nb;
                p->cap = min_keep;
            }
        }
    }
}
