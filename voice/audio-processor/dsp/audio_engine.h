#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdint.h>
#include <stddef.h>

typedef struct audio_engine_session audio_engine_session;

enum {
    AUDIO_DECISION_SPEECH = 1u << 0,
    AUDIO_DECISION_PENDING_AUDIO = 1u << 1,
    AUDIO_DECISION_LIVE_PARTIAL = 1u << 2,
    AUDIO_DECISION_COMPLETE = 1u << 3,
    AUDIO_DECISION_VOICE_SEEN = 1u << 4,
    AUDIO_DECISION_INTERRUPT = 1u << 5,
    AUDIO_DECISION_GROUP_DELIBERATING = 1u << 6,
    AUDIO_DECISION_VISION_FRESH = 1u << 7,
    /* The frame was rejected or could not be retained. Callers must not
     * mistake the all-zero payload fields for a valid silence decision. */
    AUDIO_DECISION_ERROR = 1u << 8
};

enum {
    AUDIO_PROCESS_INTERRUPT_ENABLED = 1u << 0
};

typedef struct {
    float    interrupt_threshold;
    float    interrupt_duration_s;
    uint32_t flags;
} audio_process_options;

typedef struct {
    uint64_t utterance_bytes;
    float    energy;
    float    vad_confidence;
    uint32_t flags;
} audio_decision;

enum {
    AUDIO_VISION_GAZE_UNSPECIFIED = 0,
    AUDIO_VISION_GAZE_DISPERSED = 1,
    AUDIO_VISION_GAZE_SHARED_TABLETOP = 2,
    AUDIO_VISION_GAZE_SHARED_OFFTABLE = 3
};

enum {
    AUDIO_VISION_SCENE_CHANGED = 1u << 0
};

typedef struct {
    uint64_t sequence;
    uint64_t observed_at_mono_ns;
    uint64_t received_at_ns;
    uint32_t person_count;
    float    motion_energy;
    uint32_t gaze_cluster;
    uint32_t flags;
} audio_vision_signal;

typedef struct {
    float    motion_threshold;
    uint32_t enter_samples;
    uint32_t exit_samples;
    uint64_t stale_after_ns;
} audio_group_policy_config;

typedef struct {
    int      accepted;
    int      qualifying;
    int      group_deliberating;
    int      vision_fresh;
    uint32_t enter_count;
    uint32_t exit_count;
} audio_group_policy_decision;

audio_engine_session* audio_engine_create(float energy_thresh,
                                          double highpass_alpha,
                                          int enable_hp,
                                          int enable_ng,
                                          double min_noise_floor,
                                          double noise_floor_smoothing,
                                          double noise_gate_mult,
                                          double noise_attenuation);

void audio_engine_process(audio_engine_session* s,
                          const uint8_t* pcm,
                          size_t len,
                          uint64_t now_ns,
                          audio_process_options options,
                          audio_decision* decision);

void audio_engine_process_bounded(audio_engine_session* s,
                                  const uint8_t* pcm,
                                  size_t len,
                                  uint64_t now_ns,
                                  audio_process_options options,
                                  size_t max_utterance_capacity,
                                  audio_decision* decision);

typedef struct {
    uint8_t* buffer;
    size_t   length;
    size_t   captured_length;
    size_t   trimmed_prefix_length;
    size_t   trimmed_suffix_length;
    uint32_t flags;
} audio_finalized_utterance;

typedef struct {
    int      pending_audio;
    int      live_partial_eligible;
    uint64_t utterance_bytes;
    int      is_complete;
    int      has_voice;
    int      group_deliberating;
} audio_monitor_state;

// Snapshot the utterance lifecycle flags and detach its contiguous PCM buffer.
// The caller owns buffer and must release it with
// audio_engine_free_buffer.
audio_finalized_utterance audio_engine_finalize(audio_engine_session* s);

// Detach only the span around frames classified as speech. Interior pauses
// remain intact. Context values are rounded down to complete PCM16 samples.
// If no speech span exists, this function returns the complete utterance.
audio_finalized_utterance audio_engine_finalize_speech_window(
    audio_engine_session* s,
    size_t leading_context_bytes,
    size_t trailing_context_bytes);

size_t audio_engine_utterance_capacity(const audio_engine_session* s);

int audio_engine_copy_utterance(audio_engine_session* s,
                                uint8_t** out_buf,
                                size_t* out_len);

void audio_engine_free_buffer(uint8_t* buf);

void audio_engine_destroy(audio_engine_session* s);

// Explicit response-state transitions re-arm the one-shot interrupt decision.
// Per-frame interrupt adjudication stays fused into audio_engine_process.
void audio_engine_reset_interrupt(audio_engine_session* s);

// Mark the client stream complete and return the resulting lifecycle state
// from that same C observation.
audio_monitor_state audio_engine_mark_complete(audio_engine_session* s, uint64_t now_ns);

// Atomically reconcile the C-owned completion flag and reset a final
// utterance. Returns one when force_final or the session complete flag makes
// this a final cleanup; non-final processing leaves the session unchanged.
int audio_engine_finish_processing(audio_engine_session* s, int force_final);

int audio_engine_check_complete_settle(audio_engine_session* s, uint64_t now_ns, uint64_t complete_settle_ns);

void audio_engine_record_partial(audio_engine_session* s, uint64_t now_ns, size_t bytes);
int audio_engine_should_do_partial(audio_engine_session* s, uint64_t now_ns, size_t current_bytes, int min_bytes, int min_delta_bytes, uint64_t min_interval_ns);

void audio_engine_get_monitor_state(const audio_engine_session* s, audio_monitor_state* out);

typedef struct {
    int      reason;
    int      live_partial_trigger;
    uint64_t monitor_interval_ns;

    int      pending_audio;
    int      live_partial_eligible;
    uint64_t utterance_bytes;
    int      is_complete;
    int      has_voice;
    int      group_deliberating;
} audio_monitor_tick;

// Apply one validated, non-identifying structured vision event to the
// session-local C policy. Sequence/order, staleness, and hysteresis remain
// native state; raw frames never cross this ABI.
audio_group_policy_decision audio_engine_apply_vision_event(
    audio_engine_session* s,
    audio_vision_signal signal,
    audio_group_policy_config config);

int audio_engine_monitor_tick(audio_engine_session* s,
                              size_t buffer_size,
                              size_t max_buffer_size,
                              float chunk_timeout_s,
                              uint64_t now_ns,
                              uint64_t complete_settle_ns,
                              int endpointing_enabled,
                              uint64_t min_speech_ns,
                              uint64_t silence_ns,
                              uint64_t max_utt_ns,
                              int has_utterance_id,
                              int live_partial_enabled,
                              uint64_t base_interval_ns,
                              uint64_t short_interval_ns,
                              audio_monitor_tick* out);

// Cross-service note: gateway/clients can call equivalent framing/assembly.

#endif
