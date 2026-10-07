// telemetry.h
// Process-local C reflex facts and hot micro-orchestration primitives.
//
// The bounded ring exists inside the process that owns the reflex session. It
// carries frame and decision facts needed by C snapshots and local benchmarks;
// it is not a shared-memory transport, product event bus, or telemetry exporter.
// The C turn owner emits durable product telemetry after a decision.
//
// Pure libc + optional guarded perf headers. No other dependencies.

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  // size_t for bounded ring capacities

#ifdef __cplusplus
extern "C" {
#endif

// Raw cycle counter for same-process ordering/attribution only. On non-x86 this
// falls back to monotonic nanoseconds. Never convert it to elapsed time without
// an explicit frequency calibration; use telemetry_monotonic_ns for durations.
typedef uint64_t telemetry_tsc_t;

// Compact feature snapshot from the retained audio reflex.
typedef struct {
    float energy;
    float noise_floor;          // C-computed from smoothing; low = clean audio
} reflex_features_t;

// Hardware snapshot (sampled; cheap fields only in hot path).
typedef struct {
    uint8_t node_id;            // small index into known fleet
    uint8_t npu_util;           // 0-100 or 0 if not on NPU this stage
    uint8_t gpu_util;           // 0-100 (Strix or GT15)
    uint16_t mem_bw_mbs;        // approximate MB/s or 0
    uint32_t est_power_mw;      // mW or 0
} hw_snapshot_t;

// Per-frame fact emitted on every 20 ms audio chunk.
typedef struct {
    telemetry_tsc_t tsc_start;
    telemetry_tsc_t tsc_end;
    uint64_t turn_id_hi;        // process-local qh (hi/lo), not durable turn_id
    uint64_t turn_id_lo;
    uint32_t frame_seq;         // per-turn monotonic
    reflex_features_t features;
    hw_snapshot_t hw;
    uint32_t bytes_processed;
    float rtf_this_frame;       // process time / audio time for this micro-stage
} frame_telemetry_t;

// Higher-granularity process-local reflex fact.
typedef struct {
    telemetry_tsc_t tsc;
    uint64_t turn_id_hi;
    uint64_t turn_id_lo;
    uint16_t stage_id;          // STAGE_KIND_* enum below
    uint16_t flags;             // stage-specific bitmask
    reflex_features_t features;
    hw_snapshot_t hw;
    float latency_ms;
    uint32_t extra_u32;         // stage-specific compact value
} stage_event_t;

// C-native stage kinds emitted by the retained process-local reflex.
enum {
    STAGE_KIND_VAD = 1,
    // Add only measured, product-consumed reflex stages.
};

// Bounded process-local ring for hot emission and snapshot inspection.
typedef struct telemetry_ring telemetry_ring_t;

// Create/destroy ring (power-of-2 capacity in events).
telemetry_ring_t* telemetry_ring_new(size_t capacity);
void telemetry_ring_free(telemetry_ring_t* r);

// Process-global ring for C sites embedded in one owner process.
void telemetry_init_global_ring(size_t capacity);

void telemetry_free_global_ring(void);
telemetry_ring_t* telemetry_get_global_ring(void);

// Emit with lock-free best effort in the hot path. A full ring drops the new event.
void telemetry_emit_frame(telemetry_ring_t* r, const frame_telemetry_t* ev);
void telemetry_emit_stage(telemetry_ring_t* r, const stage_event_t* ev);

uint64_t telemetry_emitted_count(const telemetry_ring_t* r);

// Duration clock: CLOCK_MONOTONIC_RAW nanoseconds on Linux, CLOCK_MONOTONIC
// otherwise. This is the only clock used for latency and RTF calculations.
uint64_t telemetry_monotonic_ns(void);

// Cycle counter for low-overhead event attribution. Units are CPU cycles on
// x86_64 and monotonic nanoseconds on other architectures.
telemetry_tsc_t get_tsc(void);

// Process-local turn ID generation for the C reflex session.
uint64_t telemetry_generate_turn_id(void);

// Native proof inspection is excluded from production objects. Test and
// benchmark builds opt in explicitly with -DTELEMETRY_NATIVE_TEST_API=1.
#ifdef TELEMETRY_NATIVE_TEST_API
typedef struct {
    size_t capacity;
    size_t occupancy;
    float pressure;      // 0.0 - 1.0 (head-tail distance / cap)
    uint64_t emitted_total;
    uint64_t full_events;  // new events dropped because the ring was full
} ring_stats_t;

int telemetry_get_ring_stats(telemetry_ring_t* r, ring_stats_t* out);

typedef void (*telemetry_consumer_fn)(int is_frame, const void* data, size_t size, void* user);

int telemetry_consume(telemetry_ring_t* r, telemetry_consumer_fn cb, void* user);
#endif

#ifdef __cplusplus
}
#endif

#endif // TELEMETRY_H
