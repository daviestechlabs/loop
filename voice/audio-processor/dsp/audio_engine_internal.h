#ifndef AUDIO_ENGINE_INTERNAL_H
#define AUDIO_ENGINE_INTERNAL_H

#include "audio_engine.h"
#include "dynbuf.h"
#include "telemetry.h"

struct audio_engine_session {
    float energy_thresh;

    telemetry_ring_t *telemetry;
    int owns_telemetry;

    uint64_t utterance_started_ns;
    uint64_t silence_started_ns;
    int current_is_speech;
    int utterance_has_voice;
    uint64_t last_chunk_ns;
    uint64_t interrupt_started_ns;
    int interrupt_fired;

    double hp_prev_in;
    double hp_prev_out;
    double noise_floor;

    double highpass_alpha;
    int enable_hp;
    int enable_ng;
    double min_noise_floor;
    double noise_floor_smoothing;
    double noise_gate_mult;
    double noise_attenuation;

    dynbuf utterance;
    size_t first_speech_offset;
    size_t speech_end_offset;
    int speech_window_valid;

    uint32_t frame_seq;

    uint64_t complete_marked_ns;
    int is_complete_flag;

    uint64_t turn_id_hi;
    uint64_t turn_id_lo;

    uint64_t last_partial_ns;
    size_t last_partial_bytes;

    uint64_t vision_sequence;
    uint64_t vision_observed_ns;
    uint64_t vision_received_ns;
    uint64_t vision_stale_after_ns;
    uint32_t group_enter_count;
    uint32_t group_exit_count;
    int group_deliberating;
};

/* The caller supplies zeroed storage and retains its ownership. */
audio_engine_session *audio_engine_session_init_zeroed(
    audio_engine_session *s,
    float energy_thresh,
    double highpass_alpha,
    int enable_hp,
    int enable_ng,
    double min_noise_floor,
    double noise_floor_smoothing,
    double noise_gate_mult,
    double noise_attenuation);

/* Release owned buffers without releasing the caller-owned session storage. */
void audio_engine_session_deinit(audio_engine_session *s);

#endif
