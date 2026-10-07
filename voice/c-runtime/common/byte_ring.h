/* Fixed-capacity byte ring shared by the public voice edges. */
#ifndef VOICE_C_COMMON_BYTE_RING_H
#define VOICE_C_COMMON_BYTE_RING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef VOICE_BYTE_RING_CAPACITY
#define VOICE_BYTE_RING_CAPACITY (64u * 1024u)
#endif

#if (VOICE_BYTE_RING_CAPACITY & (VOICE_BYTE_RING_CAPACITY - 1u)) != 0
#error "VOICE_BYTE_RING_CAPACITY must be a power of two"
#endif

typedef struct {
    uint8_t data[VOICE_BYTE_RING_CAPACITY];
    size_t head;
    size_t size;
} voice_byte_ring;

static inline size_t voice_byte_ring_dirty_after_write_bounded(
    size_t dirty_bytes,
    size_t head,
    size_t size,
    size_t written_bytes,
    size_t capacity
) {
    size_t tail;
    size_t end;
    if (capacity == 0u) return 0u;
    if ((capacity & (capacity - 1u)) != 0u || dirty_bytes > capacity ||
        head >= capacity || size > capacity || written_bytes > capacity - size)
        return capacity;
    if (written_bytes == 0u || dirty_bytes == capacity) return dirty_bytes;
    tail = (head + size) & (capacity - 1u);
    if (written_bytes > capacity - tail) return capacity;
    end = tail + written_bytes;
    return dirty_bytes > end ? dirty_bytes : end;
}

static inline void voice_byte_ring_reset(voice_byte_ring *ring) {
    if (!ring) return;
    ring->head = 0u;
    ring->size = 0u;
}

/* Return the scrub prefix after a successful write from the current tail. */
static inline size_t voice_byte_ring_dirty_after_write(
    const voice_byte_ring *ring,
    size_t dirty_bytes,
    size_t written_bytes
) {
    return ring ? voice_byte_ring_dirty_after_write_bounded(
        dirty_bytes, ring->head, ring->size, written_bytes,
        VOICE_BYTE_RING_CAPACITY) : VOICE_BYTE_RING_CAPACITY;
}

/* Erase the tracked prefix and restore the empty-ring metadata. */
static inline void voice_byte_ring_scrub(
    voice_byte_ring *ring,
    size_t dirty_bytes
) {
    if (!ring) return;
    if (dirty_bytes > VOICE_BYTE_RING_CAPACITY)
        dirty_bytes = VOICE_BYTE_RING_CAPACITY;
    if (dirty_bytes != 0u) memset(ring->data, 0, dirty_bytes);
    voice_byte_ring_reset(ring);
}

static inline void voice_byte_ring_copy_unchecked_bounded(
    uint8_t *storage,
    size_t capacity,
    size_t *head,
    size_t *size,
    const uint8_t *data,
    size_t data_len
) {
    size_t tail = (*head + *size) & (capacity - 1u);
    size_t first_len = capacity - tail;
    if (first_len > data_len) first_len = data_len;
    if (first_len != 0u) memcpy(storage + tail, data, first_len);
    if (data_len > first_len)
        memcpy(storage, data + first_len, data_len - first_len);
    *size += data_len;
}

/* Append two spans to power-of-two storage as one atomic capacity decision. */
static inline int voice_byte_ring_write_pair_bounded(
    uint8_t *storage,
    size_t capacity,
    size_t *head,
    size_t *size,
    const void *first,
    size_t first_len,
    const void *second,
    size_t second_len
) {
    size_t total;
    if (!storage || !head || !size || capacity == 0u ||
        (capacity & (capacity - 1u)) != 0u ||
        (first_len != 0u && !first) || (second_len != 0u && !second) ||
        first_len > capacity || second_len > capacity - first_len)
        return 0;
    total = first_len + second_len;
    if (*head >= capacity || *size > capacity || total > capacity - *size)
        return 0;
    if (first_len != 0u)
        voice_byte_ring_copy_unchecked_bounded(
            storage, capacity, head, size, (const uint8_t *)first, first_len);
    if (second_len != 0u)
        voice_byte_ring_copy_unchecked_bounded(
            storage, capacity, head, size, (const uint8_t *)second, second_len);
    return 1;
}

/* Append two spans as one atomic capacity decision. */
static inline int voice_byte_ring_write_pair(
    voice_byte_ring *ring,
    const void *first,
    size_t first_len,
    const void *second,
    size_t second_len
) {
    return ring && voice_byte_ring_write_pair_bounded(
        ring->data, VOICE_BYTE_RING_CAPACITY, &ring->head, &ring->size,
        first, first_len, second, second_len);
}

static inline int voice_byte_ring_write(
    voice_byte_ring *ring,
    const void *data,
    size_t data_len
) {
    return voice_byte_ring_write_pair(ring, data, data_len, NULL, 0u);
}

/*
 * Borrow the complete contiguous write span without changing ring metadata.
 * Keep the ring metadata unchanged until the matching commit completes.
 */
static inline int voice_byte_ring_write_span_bounded(
    uint8_t *storage,
    size_t capacity,
    size_t head,
    size_t size,
    uint8_t **data,
    size_t *data_len
) {
    size_t available;
    size_t contiguous;
    size_t tail;
    if (data) *data = NULL;
    if (data_len) *data_len = 0u;
    if (!storage || !data || !data_len || capacity == 0u ||
        (capacity & (capacity - 1u)) != 0u || head >= capacity ||
        size >= capacity)
        return 0;
    tail = (head + size) & (capacity - 1u);
    available = capacity - size;
    contiguous = capacity - tail;
    if (contiguous > available) contiguous = available;
    *data = storage + tail;
    *data_len = contiguous;
    return 1;
}

/* Borrow the complete contiguous write span without changing ring metadata. */
static inline int voice_byte_ring_write_span(
    voice_byte_ring *ring,
    uint8_t **data,
    size_t *data_len
) {
    if (!ring) {
        if (data) *data = NULL;
        if (data_len) *data_len = 0u;
        return 0;
    }
    return voice_byte_ring_write_span_bounded(
        ring->data, VOICE_BYTE_RING_CAPACITY, ring->head, ring->size,
        data, data_len);
}

/* Commit bytes written into a span borrowed from unchanged ring metadata. */
static inline int voice_byte_ring_write_commit_bounded(
    size_t capacity,
    size_t head,
    size_t *size,
    size_t data_len
) {
    size_t available;
    size_t contiguous;
    size_t tail;
    if (!size || capacity == 0u ||
        (capacity & (capacity - 1u)) != 0u || head >= capacity ||
        *size > capacity)
        return 0;
    tail = (head + *size) & (capacity - 1u);
    available = capacity - *size;
    contiguous = capacity - tail;
    if (contiguous > available) contiguous = available;
    if (data_len > contiguous) return 0;
    *size += data_len;
    return 1;
}

/* Commit bytes written into a span borrowed from unchanged ring metadata. */
static inline int voice_byte_ring_write_commit(
    voice_byte_ring *ring,
    size_t data_len
) {
    return ring && voice_byte_ring_write_commit_bounded(
        VOICE_BYTE_RING_CAPACITY, ring->head, &ring->size, data_len);
}

/* Return the next contiguous readable span from power-of-two storage. */
static inline int voice_byte_ring_peek_bounded(
    const uint8_t *storage,
    size_t capacity,
    size_t head,
    size_t size,
    const uint8_t **data,
    size_t *data_len
) {
    size_t contiguous;
    if (!storage || capacity == 0u || (capacity & (capacity - 1u)) != 0u ||
        !data || !data_len || head >= capacity || size > capacity || size == 0u)
        return 0;
    contiguous = capacity - head;
    if (contiguous > size) contiguous = size;
    *data = storage + head;
    *data_len = contiguous;
    return 1;
}

/* Return the next contiguous readable span. */
static inline int voice_byte_ring_peek(
    const voice_byte_ring *ring,
    const uint8_t **data,
    size_t *data_len
) {
    return ring && voice_byte_ring_peek_bounded(
        ring->data, VOICE_BYTE_RING_CAPACITY, ring->head, ring->size,
        data, data_len);
}

/* Consume bytes from power-of-two storage. */
static inline int voice_byte_ring_consume_bounded(
    size_t capacity,
    size_t *head,
    size_t *size,
    size_t data_len
) {
    if (!head || !size || capacity == 0u || (capacity & (capacity - 1u)) != 0u ||
        *head >= capacity || *size > capacity || data_len == 0u || data_len > *size)
        return 0;
    *head = (*head + data_len) & (capacity - 1u);
    *size -= data_len;
    if (*size == 0u) *head = 0u;
    return 1;
}

/* Consume a bounded number of queued bytes. */
static inline int voice_byte_ring_consume(voice_byte_ring *ring, size_t data_len) {
    return ring && voice_byte_ring_consume_bounded(
        VOICE_BYTE_RING_CAPACITY, &ring->head, &ring->size, data_len);
}

#endif
