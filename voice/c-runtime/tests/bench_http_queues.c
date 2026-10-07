/* Compare HTTP response compaction with the shared circular edge queue. */
#define _POSIX_C_SOURCE 200809L

#include "byte_ring.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The deployed two-turn gate returned 1,982 events. A 640-byte PCM event
 * becomes about 1.1 KiB after base64 and the canonical NDJSON envelope. */
#define BENCH_EVENTS_PER_GATE 1982u
#define BENCH_EVENT_BYTES 1104u
#define BENCH_BACKLOG_BYTES (VOICE_BYTE_RING_CAPACITY / 2u)

typedef struct {
    uint8_t data[VOICE_BYTE_RING_CAPACITY];
    size_t start;
    size_t size;
    uint64_t compacted_bytes;
} linear_queue;

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int linear_write(linear_queue *queue, const void *data, size_t data_len) {
    if (!queue || (data_len != 0u && !data) ||
        data_len > VOICE_BYTE_RING_CAPACITY - queue->size)
        return 0;
    if (queue->start != 0u &&
        queue->start + queue->size + data_len > VOICE_BYTE_RING_CAPACITY) {
        memmove(queue->data, queue->data + queue->start, queue->size);
        queue->compacted_bytes += queue->size;
        queue->start = 0u;
    }
    if (data_len > VOICE_BYTE_RING_CAPACITY - queue->start - queue->size) return 0;
    memcpy(queue->data + queue->start + queue->size, data, data_len);
    queue->size += data_len;
    return 1;
}

static int linear_consume(linear_queue *queue, size_t data_len, uint64_t *checksum) {
    if (!queue || !checksum || data_len == 0u || data_len > queue->size) return 0;
    *checksum = *checksum * 131u + queue->data[queue->start];
    *checksum = *checksum * 131u + queue->data[queue->start + data_len - 1u];
    queue->start += data_len;
    queue->size -= data_len;
    if (queue->size == 0u) queue->start = 0u;
    return 1;
}

static int ring_consume(voice_byte_ring *ring, size_t data_len, uint64_t *checksum) {
    const uint8_t *span;
    size_t span_len;
    size_t remaining = data_len;
    size_t last;
    if (!ring || !checksum || data_len == 0u || data_len > ring->size) return 0;
    last = (ring->head + data_len - 1u) & (VOICE_BYTE_RING_CAPACITY - 1u);
    *checksum = *checksum * 131u + ring->data[ring->head];
    *checksum = *checksum * 131u + ring->data[last];
    while (remaining != 0u) {
        if (!voice_byte_ring_peek(ring, &span, &span_len)) return 0;
        (void)span;
        if (span_len > remaining) span_len = remaining;
        if (!voice_byte_ring_consume(ring, span_len)) return 0;
        remaining -= span_len;
    }
    return 1;
}

static int run_linear(
    size_t gates,
    uint64_t *elapsed,
    uint64_t *checksum,
    uint64_t *compacted,
    uint64_t *samples
) {
    linear_queue queue;
    uint8_t backlog[BENCH_BACKLOG_BYTES];
    uint8_t event[BENCH_EVENT_BYTES];
    uint64_t started;
    uint64_t gate_started;
    size_t gate;
    size_t i;
    memset(&queue, 0, sizeof(queue));
    memset(backlog, 0xa5, sizeof(backlog));
    memset(event, 0x5a, sizeof(event));
    *checksum = 0u;
    started = monotonic_ns();
    for (gate = 0u; gate < gates; ++gate) {
        gate_started = monotonic_ns();
        if (!linear_write(&queue, backlog, sizeof(backlog))) return 0;
        for (i = 0u; i < BENCH_EVENTS_PER_GATE; ++i) {
            event[0] = (uint8_t)(gate + i);
            event[sizeof(event) - 1u] = (uint8_t)(gate ^ i);
            if (!linear_write(&queue, event, sizeof(event)) ||
                !linear_consume(&queue, sizeof(event), checksum))
                return 0;
        }
        if (!linear_consume(&queue, sizeof(backlog), checksum)) return 0;
        samples[gate] = monotonic_ns() - gate_started;
    }
    *elapsed = monotonic_ns() - started;
    *compacted = queue.compacted_bytes;
    return queue.size == 0u;
}

static int run_ring(
    size_t gates,
    uint64_t *elapsed,
    uint64_t *checksum,
    uint64_t *samples
) {
    voice_byte_ring ring;
    uint8_t backlog[BENCH_BACKLOG_BYTES];
    uint8_t event[BENCH_EVENT_BYTES];
    uint64_t started;
    uint64_t gate_started;
    size_t gate;
    size_t i;
    memset(&ring, 0, sizeof(ring));
    memset(backlog, 0xa5, sizeof(backlog));
    memset(event, 0x5a, sizeof(event));
    *checksum = 0u;
    started = monotonic_ns();
    for (gate = 0u; gate < gates; ++gate) {
        gate_started = monotonic_ns();
        if (!voice_byte_ring_write(&ring, backlog, sizeof(backlog))) return 0;
        for (i = 0u; i < BENCH_EVENTS_PER_GATE; ++i) {
            event[0] = (uint8_t)(gate + i);
            event[sizeof(event) - 1u] = (uint8_t)(gate ^ i);
            if (!voice_byte_ring_write(&ring, event, sizeof(event)) ||
                !ring_consume(&ring, sizeof(event), checksum))
                return 0;
        }
        if (!ring_consume(&ring, sizeof(backlog), checksum)) return 0;
        samples[gate] = monotonic_ns() - gate_started;
    }
    *elapsed = monotonic_ns() - started;
    return ring.size == 0u;
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t percentile(uint64_t *samples, size_t count, size_t percent) {
    size_t index;
    if (!samples || count == 0u || percent > 100u) return 0u;
    qsort(samples, count, sizeof(samples[0]), compare_u64);
    index = (count * percent + 99u) / 100u;
    if (index == 0u) return samples[0];
    if (index > count) index = count;
    return samples[index - 1u];
}

int main(int argc, char **argv) {
    size_t gates = argc > 1 && strcmp(argv[1], "--verify") == 0 ? 8u : 1000u;
    uint64_t linear_samples[1000];
    uint64_t ring_samples[1000];
    uint64_t linear_elapsed;
    uint64_t ring_elapsed;
    uint64_t linear_checksum;
    uint64_t ring_checksum;
    uint64_t compacted;
    uint64_t response_bytes = (uint64_t)gates * BENCH_EVENTS_PER_GATE * BENCH_EVENT_BYTES;
    uint64_t amplification_x100;
    uint64_t speedup_x100;
    uint64_t linear_mib_s_x100;
    uint64_t ring_mib_s_x100;
    uint64_t linear_p50;
    uint64_t linear_p99;
    uint64_t linear_max;
    uint64_t ring_p50;
    uint64_t ring_p99;
    uint64_t ring_max;
    if (!run_linear(
            gates, &linear_elapsed, &linear_checksum, &compacted, linear_samples) ||
        !run_ring(gates, &ring_elapsed, &ring_checksum, ring_samples) ||
        linear_checksum != ring_checksum || compacted < response_bytes) {
        fprintf(stderr, "FAIL HTTP response queue equivalence\n");
        return 1;
    }
    amplification_x100 = response_bytes == 0u
        ? 0u : (response_bytes + compacted) * 100u / response_bytes;
    speedup_x100 = ring_elapsed == 0u ? 0u : linear_elapsed * 100u / ring_elapsed;
    /* 100 * 1e9 / 2^20 rounds to 95,367. */
    linear_mib_s_x100 = linear_elapsed == 0u
        ? 0u : response_bytes * UINT64_C(95367) / linear_elapsed;
    ring_mib_s_x100 = ring_elapsed == 0u
        ? 0u : response_bytes * UINT64_C(95367) / ring_elapsed;
    linear_p50 = percentile(linear_samples, gates, 50u);
    linear_p99 = percentile(linear_samples, gates, 99u);
    linear_max = linear_samples[gates - 1u];
    ring_p50 = percentile(ring_samples, gates, 50u);
    ring_p99 = percentile(ring_samples, gates, 99u);
    ring_max = ring_samples[gates - 1u];
    printf("BenchmarkHttpResponseQueue gates=%zu events_per_gate=%u"
           " response_bytes=%" PRIu64 " linear_ns=%" PRIu64
           " ring_ns=%" PRIu64 " compacted_bytes=%" PRIu64
           " copy_amplification_x100=%" PRIu64 " speedup_x100=%" PRIu64
           " linear_mib_s_x100=%" PRIu64 " ring_mib_s_x100=%" PRIu64
           " linear_p50_ns=%" PRIu64 " linear_p99_ns=%" PRIu64
           " linear_max_ns=%" PRIu64 " ring_p50_ns=%" PRIu64
           " ring_p99_ns=%" PRIu64 " ring_max_ns=%" PRIu64
           " queue_bytes=%zu linear_max_compaction_bytes=%u"
           " ring_max_compaction_bytes=0"
           " checksum=%" PRIu64 "\n",
           gates, BENCH_EVENTS_PER_GATE, response_bytes, linear_elapsed,
           ring_elapsed, compacted, amplification_x100, speedup_x100,
           linear_mib_s_x100, ring_mib_s_x100,
           linear_p50, linear_p99, linear_max, ring_p50, ring_p99, ring_max,
           sizeof(voice_byte_ring), BENCH_BACKLOG_BYTES,
           ring_checksum);
    return 0;
}
