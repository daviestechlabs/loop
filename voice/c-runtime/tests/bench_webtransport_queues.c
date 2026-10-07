/* Compare the retired compacting response queue with the circular queue. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_webtransport_core.h"

#include <openssl/crypto.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_FRAME_BYTES 4096u
#define BENCH_HEADER_BYTES 5u
#define BENCH_PAYLOAD_BYTES (BENCH_FRAME_BYTES - BENCH_HEADER_BYTES)
#define BENCH_DISPATCH_SESSIONS 64u
#define BENCH_DISPATCH_ITERATIONS 5000000u
#define BENCH_REQUEST_PARSE_ITERATIONS 200000u

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

typedef struct {
    uint8_t data[GW_WT_RESPONSE_QUEUE_CAP];
    size_t offset;
    size_t length;
    uint64_t compacted_bytes;
} linear_queue;

typedef struct {
    int active;
    char subject[GW_WT_EVENT_SUBJECT_LENGTH + 1u];
} dispatch_session;

static const uint8_t request_fixture[] =
    "{\"request_id\":\"wt-benchmark-request\","
    "\"session_id\":\"wt-benchmark-session\","
    "\"text\":\"local WebTransport benchmark\","
    "\"identity_token\":\"vat1.payload.signature\","
    "\"enable_rag\":false,\"enable_tts\":false,"
    "\"metadata\":{\"input_mode\":\"text\","
    "\"client_surface\":\"c-local-benchmark\"}}";

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

static int linear_write(linear_queue *queue, const uint8_t *data, size_t data_len) {
    if (!queue || !data || data_len > sizeof(queue->data)) return 0;
    if (queue->offset == queue->length) {
        queue->offset = 0;
        queue->length = 0;
    } else if (queue->offset != 0u &&
               data_len <= sizeof(queue->data) - (queue->length - queue->offset)) {
        size_t remaining = queue->length - queue->offset;
        memmove(queue->data, queue->data + queue->offset, remaining);
        queue->compacted_bytes += remaining;
        queue->length = remaining;
        queue->offset = 0;
    }
    if (data_len > sizeof(queue->data) - queue->length) return 0;
    memcpy(queue->data + queue->length, data, data_len);
    queue->length += data_len;
    return 1;
}

static int linear_consume(linear_queue *queue, size_t data_len, uint64_t *checksum) {
    if (!queue || !checksum || data_len > queue->length - queue->offset) return 0;
    if (data_len != 0u) {
        *checksum = *checksum * 131u + queue->data[queue->offset];
        *checksum = *checksum * 131u + queue->data[queue->offset + data_len - 1u];
    }
    queue->offset += data_len;
    return 1;
}

static BENCH_NOINLINE void linear_encode_frame(
    uint8_t *frame,
    const uint8_t *header,
    const uint8_t *payload
) {
    memcpy(frame, header, BENCH_HEADER_BYTES);
    memcpy(frame + BENCH_HEADER_BYTES, payload, BENCH_PAYLOAD_BYTES);
}

static int ring_consume(gw_wt_byte_ring *ring, size_t data_len, uint64_t *checksum) {
    size_t last;
    if (!ring || !checksum || data_len == 0u || data_len > ring->size) return 0;
    last = (ring->head + data_len - 1u) & (GW_WT_RESPONSE_QUEUE_CAP - 1u);
    *checksum = *checksum * 131u + ring->data[ring->head];
    *checksum = *checksum * 131u + ring->data[last];
    return gw_wt_byte_ring_consume(ring, data_len);
}

static int run_linear(size_t iterations, uint64_t *elapsed, uint64_t *checksum,
                      uint64_t *compacted) {
    linear_queue queue;
    uint8_t header[BENCH_HEADER_BYTES] = {2u, 0u, 0u, 0x0fu, 0xfbu};
    uint8_t payload[BENCH_PAYLOAD_BYTES];
    uint8_t frame[BENCH_FRAME_BYTES];
    uint64_t started;
    size_t i;
    memset(&queue, 0, sizeof(queue));
    memset(payload, 0x5a, sizeof(payload));
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < iterations; ++i) {
        payload[0] = (uint8_t)i;
        linear_encode_frame(frame, header, payload);
        if (!linear_write(&queue, frame, sizeof(frame)) ||
            !linear_consume(&queue, i == 0u ? 3072u : BENCH_FRAME_BYTES, checksum))
            return 0;
    }
    if (!linear_consume(&queue, 1024u, checksum)) return 0;
    *elapsed = monotonic_ns() - started;
    *compacted = queue.compacted_bytes;
    return 1;
}

static int run_ring(size_t iterations, uint64_t *elapsed, uint64_t *checksum) {
    gw_wt_byte_ring ring;
    uint8_t header[BENCH_HEADER_BYTES] = {2u, 0u, 0u, 0x0fu, 0xfbu};
    uint8_t payload[BENCH_PAYLOAD_BYTES];
    uint64_t started;
    size_t i;
    memset(&ring, 0, sizeof(ring));
    memset(payload, 0x5a, sizeof(payload));
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < iterations; ++i) {
        payload[0] = (uint8_t)i;
        if (!gw_wt_byte_ring_write_pair(&ring, header, sizeof(header),
                                         payload, sizeof(payload)) ||
            !ring_consume(&ring, i == 0u ? 3072u : BENCH_FRAME_BYTES, checksum))
            return 0;
    }
    if (!ring_consume(&ring, 1024u, checksum)) return 0;
    *elapsed = monotonic_ns() - started;
    return ring.size == 0u;
}

static BENCH_NOINLINE size_t dispatch_scan(
    const dispatch_session sessions[BENCH_DISPATCH_SESSIONS],
    const char *subject
) {
    size_t slot;
    for (slot = 0; slot < BENCH_DISPATCH_SESSIONS; ++slot)
        if (sessions[slot].active && strcmp(sessions[slot].subject, subject) == 0)
            return slot;
    return BENCH_DISPATCH_SESSIONS;
}

static BENCH_NOINLINE size_t dispatch_slot(
    const dispatch_session sessions[BENCH_DISPATCH_SESSIONS],
    const char *subject
) {
    size_t slot;
    if (gw_wt_response_subject_slot(subject, &slot) != GW_WT_OK ||
        slot >= BENCH_DISPATCH_SESSIONS || !sessions[slot].active ||
        CRYPTO_memcmp(sessions[slot].subject, subject,
                      GW_WT_EVENT_SUBJECT_LENGTH + 1u) != 0)
        return BENCH_DISPATCH_SESSIONS;
    return slot;
}

static int initialize_dispatch(dispatch_session sessions[BENCH_DISPATCH_SESSIONS]) {
    static const char nonce_template[] = "0123456789abcdef0123456789abcdef";
    char nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u];
    static const char hex[] = "0123456789abcdef";
    size_t slot;
    for (slot = 0; slot < BENCH_DISPATCH_SESSIONS; ++slot) {
        memcpy(nonce, nonce_template, sizeof(nonce));
        nonce[GW_WT_EVENT_NONCE_HEX_LEN - 2u] = hex[slot >> 4u];
        nonce[GW_WT_EVENT_NONCE_HEX_LEN - 1u] = hex[slot & 15u];
        sessions[slot].active = 1;
        if (gw_wt_response_subject_write(
                sessions[slot].subject, sizeof(sessions[slot].subject),
                slot, nonce) != GW_WT_OK)
            return 0;
    }
    return 1;
}

static int run_dispatch(
    size_t iterations,
    int use_slot,
    uint64_t *elapsed,
    uint64_t *checksum
) {
    dispatch_session sessions[BENCH_DISPATCH_SESSIONS];
    uint64_t started;
    size_t i;
    if (!initialize_dispatch(sessions)) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0; i < iterations; ++i) {
        size_t expected = i & (BENCH_DISPATCH_SESSIONS - 1u);
        size_t found = use_slot ? dispatch_slot(sessions, sessions[expected].subject) :
                                  dispatch_scan(sessions, sessions[expected].subject);
        if (found != expected) return 0;
        *checksum = *checksum * 131u + found;
    }
    *elapsed = monotonic_ns() - started;
    return 1;
}

static int run_request_parse(
    size_t iterations,
    int use_active,
    uint64_t *elapsed,
    uint64_t *checksum
) {
    gw_wt_turn_request request;
    uint64_t started;
    size_t i;
    if (!elapsed || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < iterations; ++i) {
        int status = use_active ?
            gw_wt_turn_request_parse_active(
                request_fixture, sizeof(request_fixture) - 1u, &request) :
            gw_wt_turn_request_parse(
                request_fixture, sizeof(request_fixture) - 1u, &request);
        if (status != GW_WT_OK)
            return 0;
        *checksum = *checksum * 131u + (unsigned char)request.request_id[i & 7u];
    }
    *elapsed = monotonic_ns() - started;
    return strcmp(request.request_id, "wt-benchmark-request") == 0 &&
           strcmp(request.session_id, "wt-benchmark-session") == 0 &&
           strcmp(request.text, "local WebTransport benchmark") == 0 &&
           strcmp(request.identity_token, "vat1.payload.signature") == 0 &&
           request.enable_rag == 0 && request.enable_tts == 0 &&
           request.audio_first == 0;
}

int main(int argc, char **argv) {
    size_t iterations = argc > 1 && strcmp(argv[1], "--verify") == 0 ? 20000u : 500000u;
    uint64_t linear_elapsed;
    uint64_t ring_elapsed;
    uint64_t linear_checksum;
    uint64_t ring_checksum;
    uint64_t compacted;
    uint64_t dispatch_scan_elapsed;
    uint64_t dispatch_slot_elapsed;
    uint64_t dispatch_scan_checksum;
    uint64_t dispatch_slot_checksum;
    uint64_t request_parse_elapsed;
    uint64_t request_parse_checksum;
    size_t dispatch_iterations = argc > 1 && strcmp(argv[1], "--verify") == 0 ?
        20000u : BENCH_DISPATCH_ITERATIONS;
    size_t request_parse_iterations = argc > 1 && strcmp(argv[1], "--verify") == 0 ?
        10000u : BENCH_REQUEST_PARSE_ITERATIONS;
    uint64_t saved_frame_copy_bytes = (uint64_t)iterations * BENCH_PAYLOAD_BYTES;
    uint64_t speedup_x100;
    uint64_t dispatch_speedup_x100;
    if (argc > 1 && (strcmp(argv[1], "--request-parse") == 0 ||
                     strcmp(argv[1], "--request-parse-active") == 0)) {
        int use_active = strcmp(argv[1], "--request-parse-active") == 0;
        if (!run_request_parse(BENCH_REQUEST_PARSE_ITERATIONS, use_active,
                               &request_parse_elapsed,
                               &request_parse_checksum)) {
            fprintf(stderr, "FAIL WebTransport request parser\n");
            return 1;
        }
        printf("BenchmarkWebTransportRequestParse mode=%s iterations=%u ns=%" PRIu64
               " checksum=%" PRIu64 "\n",
               use_active ? "active" : "checked",
               BENCH_REQUEST_PARSE_ITERATIONS, request_parse_elapsed,
               request_parse_checksum);
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "--dispatch-scan") == 0 ||
                     strcmp(argv[1], "--dispatch-slot") == 0)) {
        int use_slot = strcmp(argv[1], "--dispatch-slot") == 0;
        if (!run_dispatch(BENCH_DISPATCH_ITERATIONS, use_slot,
                          &dispatch_scan_elapsed, &dispatch_scan_checksum)) {
            fprintf(stderr, "FAIL WebTransport response dispatch\n");
            return 1;
        }
        printf("BenchmarkWebTransportDispatch mode=%s iterations=%u ns=%" PRIu64
               " checksum=%" PRIu64 "\n",
               use_slot ? "slot" : "scan", BENCH_DISPATCH_ITERATIONS,
               dispatch_scan_elapsed, dispatch_scan_checksum);
        return 0;
    }
    if (!run_linear(iterations, &linear_elapsed, &linear_checksum, &compacted) ||
        !run_ring(iterations, &ring_elapsed, &ring_checksum) ||
        !run_dispatch(dispatch_iterations, 0, &dispatch_scan_elapsed,
                      &dispatch_scan_checksum) ||
        !run_dispatch(dispatch_iterations, 1, &dispatch_slot_elapsed,
                      &dispatch_slot_checksum) ||
        !run_request_parse(request_parse_iterations, 1, &request_parse_elapsed,
                           &request_parse_checksum) ||
        linear_checksum != ring_checksum ||
        dispatch_scan_checksum != dispatch_slot_checksum ||
        compacted != (uint64_t)(iterations - 1u) * 1024u) {
        fprintf(stderr, "FAIL WebTransport response queue equivalence\n");
        return 1;
    }
    speedup_x100 = ring_elapsed == 0u ? 0u : linear_elapsed * 100u / ring_elapsed;
    dispatch_speedup_x100 = dispatch_slot_elapsed == 0u ? 0u :
        dispatch_scan_elapsed * 100u / dispatch_slot_elapsed;
    printf("BenchmarkWebTransportQueue iterations=%zu linear_ns=%" PRIu64
           " ring_ns=%" PRIu64 " compacted_bytes=%" PRIu64
           " frame_copy_saved_bytes=%" PRIu64 " speedup_x100=%" PRIu64
           " checksum=%" PRIu64 "\n",
           iterations, linear_elapsed, ring_elapsed, compacted,
           saved_frame_copy_bytes, speedup_x100, ring_checksum);
    printf("BenchmarkWebTransportDispatch iterations=%zu scan_ns=%" PRIu64
           " slot_ns=%" PRIu64 " speedup_x100=%" PRIu64
           " checksum=%" PRIu64 "\n",
           dispatch_iterations, dispatch_scan_elapsed, dispatch_slot_elapsed,
           dispatch_speedup_x100, dispatch_slot_checksum);
    printf("BenchmarkWebTransportRequestParse mode=active iterations=%zu ns=%" PRIu64
           " checksum=%" PRIu64 "\n",
           request_parse_iterations, request_parse_elapsed,
           request_parse_checksum);
    return 0;
}
