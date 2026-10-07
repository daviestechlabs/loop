#ifndef DTL_AUDIO_REFLEX_SESSION_H
#define DTL_AUDIO_REFLEX_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REFLEX_SESSION_ABI_V1 1u

typedef struct reflex_session_v1 reflex_session_v1;

typedef enum {
    REFLEX_OK = 0,
    REFLEX_ERR_ARGUMENT = -1,
    REFLEX_ERR_ABI = -2,
    REFLEX_ERR_FORMAT = -3,
    REFLEX_ERR_CAPACITY = -4,
    REFLEX_ERR_STATE = -5
} reflex_status_v1;

typedef enum {
    REFLEX_ENDPOINT_NONE = 0,
    REFLEX_ENDPOINT_SILENCE = 1,
    REFLEX_ENDPOINT_MAX_DURATION = 2,
    REFLEX_ENDPOINT_CLIENT_END = 3,
    REFLEX_ENDPOINT_CANCELLED = 4,
    REFLEX_ENDPOINT_CAPACITY = 5
} reflex_endpoint_v1;

enum {
    REFLEX_CONFIG_HIGH_PASS = 1u << 0,
    REFLEX_CONFIG_NOISE_GATE = 1u << 1,
    REFLEX_CONFIG_RETAIN_UTTERANCE = 1u << 2
};

enum {
    REFLEX_INPUT_ASSISTANT_SPEAKING = 1u << 0
};

enum {
    REFLEX_DECISION_SPEECH = 1u << 0,
    REFLEX_DECISION_VOICE_SEEN = 1u << 1,
    REFLEX_DECISION_ENDPOINT = 1u << 2,
    REFLEX_DECISION_INTERRUPT = 1u << 3,
    REFLEX_DECISION_COMPLETE = 1u << 4,
    REFLEX_DECISION_CANCELLED = 1u << 5,
    REFLEX_DECISION_OVERFLOW = 1u << 6
};

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;

    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;

    uint32_t max_datagram_bytes;
    uint32_t max_utterance_bytes;

    float energy_threshold;
    /* Retained layout slots from the pre-cutover internal ABI. */
    uint32_t reserved_vad;
    double high_pass_alpha;
    double min_noise_floor;
    double noise_floor_smoothing;
    double noise_gate_multiplier;
    double noise_attenuation;
    uint64_t reserved_conditioning;

    uint64_t min_speech_ns;
    uint64_t endpoint_silence_ns;
    uint64_t max_utterance_ns;
    uint64_t interrupt_hold_ns;
    float interrupt_energy_threshold;
    uint32_t reserved;
} reflex_config_v1;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    int32_t status;
    uint32_t flags;

    uint32_t endpoint_reason;
    uint32_t reserved;
    float energy;
    /* Retained layout slots from the pre-cutover internal ABI. */
    float reserved_features[2];
    float vad_confidence;
    float noise_floor;

    uint64_t utterance_bytes;
    uint64_t speech_started_ns;
    uint64_t silence_started_ns;
    uint64_t processed_frames;
    uint64_t rejected_frames;
} reflex_decision_v1;

void reflex_config_default_v1(reflex_config_v1 *config);

int reflex_session_init_v1(const reflex_config_v1 *config,
                           reflex_session_v1 **out_session);

/*
 * Process exactly one little-endian, mono PCM S16LE WebTransport datagram.
 * The session performs no allocation after init. now_ns must use a monotonic
 * clock. input_flags carries caller state such as assistant-speaking.
 */
int reflex_session_process_v1(reflex_session_v1 *session,
                              const uint8_t *pcm,
                              size_t pcm_len,
                              uint64_t now_ns,
                              uint32_t input_flags,
                              reflex_decision_v1 *out_decision);

/*
 * Copy the current conditioned STT payload into caller-owned memory. The
 * config must include REFLEX_CONFIG_RETAIN_UTTERANCE. On
 * REFLEX_ERR_CAPACITY, required_bytes is populated and session state is
 * unchanged. A NULL output with capacity zero is a size query.
 */
int reflex_session_snapshot_v1(const reflex_session_v1 *session,
                               uint8_t *out_pcm,
                               size_t out_capacity,
                               size_t *required_bytes,
                               reflex_decision_v1 *out_decision);

/*
 * Snapshot and atomically reset an utterance. The config must include
 * REFLEX_CONFIG_RETAIN_UTTERANCE. An undersized caller buffer leaves the
 * utterance intact. endpoint_reason is normally CLIENT_END when no earlier
 * endpoint decision was recorded.
 */
int reflex_session_finalize_v1(reflex_session_v1 *session,
                               uint8_t *out_pcm,
                               size_t out_capacity,
                               size_t *written_bytes,
                               uint32_t endpoint_reason,
                               reflex_decision_v1 *out_decision);

void reflex_session_cancel_v1(reflex_session_v1 *session,
                              reflex_decision_v1 *out_decision);

void reflex_session_reset_v1(reflex_session_v1 *session);
void reflex_session_destroy_v1(reflex_session_v1 *session);

#ifdef __cplusplus
}
#endif

#endif
