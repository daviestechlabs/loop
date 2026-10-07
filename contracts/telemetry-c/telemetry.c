#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

// telemetry.c
// Minimal pure-C implementation of the telemetry + hot micro-orchestration plane.
// Bounded process-local ring with allocation-free multi-producer publication.
// Designed for sub-microsecond emission in the 20 ms audio / token / SNAC hot path.
// Bounded process-local telemetry ring; audio ownership remains in audio_engine.c.

#include "telemetry.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <time.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#define TELEMETRY_RING_MAGIC 0x54454c52u /* "TELR" */
#define TELEMETRY_RING_LAYOUT_VERSION 5u

enum telemetry_slot_kind {
    TELEMETRY_SLOT_EMPTY = 0,
    TELEMETRY_SLOT_FRAME = 1,
    TELEMETRY_SLOT_STAGE = 2,
};

struct telemetry_ring {
    uint32_t magic;
    uint32_t layout_version;
    _Atomic uint32_t initialized;
    size_t capacity;            // power of 2
    size_t mapped_size;
    size_t sequences_offset;
    size_t kinds_offset;
    size_t frames_offset;
    size_t stages_offset;
    _Alignas(64)
    _Atomic size_t head;
    _Alignas(64)
    _Atomic size_t tail;
    _Atomic uint64_t emitted;
    _Atomic uint64_t full_events;     // producer events dropped because the ring was full
};

static inline void* ring_storage(telemetry_ring_t* r, size_t offset) {
    return (unsigned char*)r + offset;
}

static inline _Atomic size_t* ring_sequences(telemetry_ring_t* r) {
    return ring_storage(r, r->sequences_offset);
}

static inline uint8_t* ring_kinds(telemetry_ring_t* r) {
    return ring_storage(r, r->kinds_offset);
}

static inline frame_telemetry_t* ring_frames(telemetry_ring_t* r) {
    return ring_storage(r, r->frames_offset);
}

static inline stage_event_t* ring_stages(telemetry_ring_t* r) {
    return ring_storage(r, r->stages_offset);
}

static int size_add(size_t left, size_t right, size_t *out) {
    if (!out || right > SIZE_MAX - left) return -1;
    *out = left + right;
    return 0;
}

static int size_mul(size_t left, size_t right, size_t *out) {
    if (!out || (right != 0 && left > SIZE_MAX / right)) return -1;
    *out = left * right;
    return 0;
}

static int size_align_up(size_t value, size_t alignment, size_t *out) {
    size_t remainder;

    if (!out || alignment == 0) return -1;
    remainder = value % alignment;
    if (remainder == 0) {
        *out = value;
        return 0;
    }
    return size_add(value, alignment - remainder, out);
}

static int next_power_of_two(size_t requested, size_t *out) {
    size_t capacity = 1;

    if (!out || requested == 0) return -1;
    while (capacity < requested) {
        if (capacity > SIZE_MAX / 2) return -1;
        capacity *= 2;
    }
    *out = capacity;
    return 0;
}

telemetry_ring_t* telemetry_ring_new(size_t capacity) {
    size_t sequences_sz, kinds_sz, frames_sz, stages_sz;
    size_t sequences_offset, sequences_end, kinds_end, frames_end, total;
    size_t frames_offset, stages_offset;
    size_t i;

    if (next_power_of_two(capacity, &capacity) != 0 || capacity > (size_t)INT_MAX)
        return NULL;
    if (size_mul(capacity, sizeof(_Atomic size_t), &sequences_sz) != 0 ||
        size_mul(capacity, sizeof(uint8_t), &kinds_sz) != 0 ||
        size_mul(capacity, sizeof(frame_telemetry_t), &frames_sz) != 0 ||
        size_mul(capacity, sizeof(stage_event_t), &stages_sz) != 0)
        return NULL;
    if (size_align_up(sizeof(telemetry_ring_t), _Alignof(_Atomic size_t), &sequences_offset) != 0 ||
        size_add(sequences_offset, sequences_sz, &sequences_end) != 0 ||
        size_add(sequences_end, kinds_sz, &kinds_end) != 0 ||
        size_align_up(kinds_end, _Alignof(frame_telemetry_t), &frames_offset) != 0 ||
        size_add(frames_offset, frames_sz, &frames_end) != 0 ||
        size_align_up(frames_end, _Alignof(stage_event_t), &stages_offset) != 0 ||
        size_add(stages_offset, stages_sz, &total) != 0)
        return NULL;

    void *base = calloc(1, total);
    if (!base) return NULL;
    telemetry_ring_t* r = (telemetry_ring_t*)base;
    r->magic = TELEMETRY_RING_MAGIC;
    r->layout_version = TELEMETRY_RING_LAYOUT_VERSION;
    r->capacity = capacity;
    r->mapped_size = total;
    r->sequences_offset = sequences_offset;
    r->kinds_offset = sequences_end;
    r->frames_offset = frames_offset;
    r->stages_offset = stages_offset;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    atomic_store(&r->emitted, 0);
    atomic_store(&r->full_events, 0);
    for (i = 0; i < capacity; i++)
        atomic_init(&ring_sequences(r)[i], i);
    atomic_store_explicit(&r->initialized, 1, memory_order_release);
    return r;
}

// Global ring impl
static telemetry_ring_t* g_global_telemetry_ring = NULL;

void telemetry_init_global_ring(size_t capacity) {
    if (g_global_telemetry_ring) return;
    g_global_telemetry_ring = telemetry_ring_new(capacity);
}

void telemetry_free_global_ring(void) {
    if (g_global_telemetry_ring) {
        telemetry_ring_free(g_global_telemetry_ring);
        g_global_telemetry_ring = NULL;
    }
}

telemetry_ring_t* telemetry_get_global_ring(void) {
    return g_global_telemetry_ring;
}

#ifdef TELEMETRY_NATIVE_TEST_API
int telemetry_get_ring_stats(telemetry_ring_t* r, ring_stats_t* out) {
    if (!r || !out) return -1;
    size_t head = atomic_load(&r->head);
    size_t tail = atomic_load(&r->tail);
    size_t cap = r->capacity;
    size_t occ = head - tail;
    out->capacity = cap;
    out->occupancy = occ;
    out->pressure = (float)occ / (float)cap;
    out->emitted_total = atomic_load(&r->emitted);
    out->full_events = atomic_load(&r->full_events);  // populated on pressure drop in push
    return 0;
}
#endif

void telemetry_ring_free(telemetry_ring_t* r) {
    if (!r) return;
    free(r);
}

static inline size_t mask(size_t v, size_t cap) { return v & (cap - 1); }

telemetry_tsc_t get_tsc(void) {
#ifdef __x86_64__
    uint32_t lo, hi;
    __asm__ volatile ("lfence\n\trdtsc" : "=a" (lo), "=d" (hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#else
    return telemetry_monotonic_ns();
#endif
}

uint64_t telemetry_monotonic_ns(void) {
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    const clockid_t clock_id = CLOCK_MONOTONIC_RAW;
#else
    const clockid_t clock_id = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clock_id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Turn ID gen (atomic for thread safety in C layers)
static _Atomic uint64_t g_next_turn_id = 1;

uint64_t telemetry_generate_turn_id(void) {
    return atomic_fetch_add(&g_next_turn_id, 1);
}

static int reserve_producer_slot(telemetry_ring_t *r, size_t *position_out) {
    size_t position;

    position = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (;;) {
        size_t idx = mask(position, r->capacity);
        size_t sequence =
            atomic_load_explicit(&ring_sequences(r)[idx], memory_order_acquire);
        if (sequence == position) {
            if (atomic_compare_exchange_weak_explicit(&r->head, &position, position + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *position_out = position;
                return 0;
            }
            continue;
        }
        if (sequence < position) {
            atomic_fetch_add_explicit(&r->full_events, 1, memory_order_relaxed);
            return -1;
        }
        position = atomic_load_explicit(&r->head, memory_order_relaxed);
    }
}

static void push_generic(void* dst_array, size_t elem_size, uint8_t slot_kind,
                         telemetry_ring_t* r, const void* ev, size_t ev_size) {
    size_t position;
    size_t idx;

    if (!r || !ev || reserve_producer_slot(r, &position) != 0) return;
    idx = mask(position, r->capacity);
    memcpy((char*)dst_array + (idx * elem_size), ev, ev_size);
    ring_kinds(r)[idx] = slot_kind;
    atomic_store_explicit(&ring_sequences(r)[idx], position + 1, memory_order_release);
    atomic_fetch_add_explicit(&r->emitted, 1, memory_order_relaxed);
}

void telemetry_emit_frame(telemetry_ring_t* r, const frame_telemetry_t* ev) {
    push_generic(ring_frames(r), sizeof(frame_telemetry_t), TELEMETRY_SLOT_FRAME, r, ev, sizeof(*ev));
}

void telemetry_emit_stage(telemetry_ring_t* r, const stage_event_t* ev) {
    push_generic(ring_stages(r), sizeof(stage_event_t), TELEMETRY_SLOT_STAGE, r, ev, sizeof(*ev));
}

uint64_t telemetry_emitted_count(const telemetry_ring_t* r) {
    if (!r) return 0;
    return atomic_load(&r->emitted);
}

#ifdef TELEMETRY_NATIVE_TEST_API
// Drain API for native tests and local benchmark tools.
int telemetry_consume(telemetry_ring_t* r, telemetry_consumer_fn cb, void* user) {
    size_t consumed = 0;
    size_t tail;
    size_t cap;

    if (!r || !cb) return -1;
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    cap = r->capacity;
    while (consumed < cap) {
        size_t idx = mask(tail, cap);
        size_t sequence =
            atomic_load_explicit(&ring_sequences(r)[idx], memory_order_acquire);
        if (sequence != tail + 1)
            break;
        switch (ring_kinds(r)[idx]) {
            case TELEMETRY_SLOT_FRAME:
                cb(1 /* is_frame */, &ring_frames(r)[idx], sizeof(frame_telemetry_t), user);
                break;
            case TELEMETRY_SLOT_STAGE:
                cb(0 /* is_stage */, &ring_stages(r)[idx], sizeof(stage_event_t), user);
                break;
            case TELEMETRY_SLOT_EMPTY:
            default:
                break;
        }
        ring_kinds(r)[idx] = TELEMETRY_SLOT_EMPTY;
        atomic_store_explicit(&ring_sequences(r)[idx], tail + cap, memory_order_release);
        tail++;
        consumed++;
    }
    atomic_store_explicit(&r->tail, tail, memory_order_relaxed);
    return (int)consumed;
}
#endif
