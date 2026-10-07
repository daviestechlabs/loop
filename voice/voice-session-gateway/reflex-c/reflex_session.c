#include "reflex_session.h"

#include "../../audio-processor/dsp/pcm_condition.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "reflex_session_v1 currently requires a little-endian host"
#endif

#define REFLEX_DEFAULT_SAMPLE_RATE 16000u
#define REFLEX_DEFAULT_MAX_DATAGRAM 8192u
#define REFLEX_DEFAULT_MAX_UTTERANCE (REFLEX_DEFAULT_SAMPLE_RATE * 2u * 30u)

struct reflex_session_v1 {
    reflex_config_v1 config;
    int16_t *scratch;
    uint8_t *utterance;
    size_t utterance_len;

    double hp_prev_in;
    double hp_prev_out;
    double noise_floor;

    uint64_t speech_started_ns;
    uint64_t silence_started_ns;
    uint64_t silence_audio_ns;
    uint64_t interrupt_started_ns;
    int interrupt_emitted;
    uint64_t processed_frames;
    uint64_t rejected_frames;

    float energy;
    float vad_confidence;
    uint32_t flags;
    uint32_t endpoint_reason;
};

static bool elapsed_at_least(uint64_t now_ns, uint64_t started_ns, uint64_t duration_ns) {
    return started_ns != 0u && now_ns >= started_ns && now_ns - started_ns >= duration_ns;
}

static void fill_decision(const reflex_session_v1 *session,
                          int status,
                          reflex_decision_v1 *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->abi_version = REFLEX_SESSION_ABI_V1;
    out->struct_size = (uint32_t)sizeof(*out);
    out->status = status;
    if (session == NULL) {
        return;
    }
    out->flags = session->flags;
    out->endpoint_reason = session->endpoint_reason;
    out->energy = session->energy;
    out->vad_confidence = session->vad_confidence;
    out->noise_floor = (float)session->noise_floor;
    out->utterance_bytes = (uint64_t)session->utterance_len;
    out->speech_started_ns = session->speech_started_ns;
    out->silence_started_ns = session->silence_started_ns;
    out->processed_frames = session->processed_frames;
    out->rejected_frames = session->rejected_frames;
}

static void clear_utterance(reflex_session_v1 *session) {
    session->utterance_len = 0u;
    session->hp_prev_in = 0.0;
    session->hp_prev_out = 0.0;
    session->noise_floor = session->config.min_noise_floor;
    session->speech_started_ns = 0u;
    session->silence_started_ns = 0u;
    session->silence_audio_ns = 0u;
    session->interrupt_started_ns = 0u;
    session->interrupt_emitted = 0;
    session->energy = 0.0f;
    session->vad_confidence = 0.0f;
    session->flags = 0u;
    session->endpoint_reason = REFLEX_ENDPOINT_NONE;
}

void reflex_config_default_v1(reflex_config_v1 *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->abi_version = REFLEX_SESSION_ABI_V1;
    config->struct_size = (uint32_t)sizeof(*config);
    config->sample_rate_hz = REFLEX_DEFAULT_SAMPLE_RATE;
    config->channels = 1u;
    config->bits_per_sample = 16u;
    config->flags = REFLEX_CONFIG_HIGH_PASS | REFLEX_CONFIG_NOISE_GATE |
        REFLEX_CONFIG_RETAIN_UTTERANCE;
    config->max_datagram_bytes = REFLEX_DEFAULT_MAX_DATAGRAM;
    config->max_utterance_bytes = REFLEX_DEFAULT_MAX_UTTERANCE;
    config->energy_threshold = 0.01f;
    config->high_pass_alpha = 0.985;
    config->min_noise_floor = 0.0025;
    config->noise_floor_smoothing = 0.2;
    config->noise_gate_multiplier = 1.75;
    config->noise_attenuation = 0.12;
    config->min_speech_ns = 180000000u;
    config->endpoint_silence_ns = 800000000u;
    config->max_utterance_ns = 30000000000u;
    /* Require sustained qualifying speech before interrupting playback. */
    config->interrupt_hold_ns = 500000000u;
    config->interrupt_energy_threshold = 0.02f;
}

static int validate_config(const reflex_config_v1 *config) {
    if (config == NULL) {
        return REFLEX_ERR_ARGUMENT;
    }
    if (config->abi_version != REFLEX_SESSION_ABI_V1 ||
        config->struct_size != sizeof(*config)) {
        return REFLEX_ERR_ABI;
    }
    if ((config->flags & ~(uint32_t)(
            REFLEX_CONFIG_HIGH_PASS | REFLEX_CONFIG_NOISE_GATE |
            REFLEX_CONFIG_RETAIN_UTTERANCE)) != 0u)
        return REFLEX_ERR_ARGUMENT;
    if (config->sample_rate_hz != REFLEX_DEFAULT_SAMPLE_RATE ||
        config->channels != 1u ||
        config->bits_per_sample != 16u) {
        return REFLEX_ERR_FORMAT;
    }
    if (config->max_datagram_bytes < 2u ||
        (config->max_datagram_bytes & 1u) != 0u ||
        config->max_utterance_bytes < config->max_datagram_bytes ||
        (config->max_utterance_bytes & 1u) != 0u) {
        return REFLEX_ERR_CAPACITY;
    }
    if (!isfinite(config->energy_threshold) ||
        !isfinite(config->high_pass_alpha) ||
        !isfinite(config->min_noise_floor) ||
        !isfinite(config->noise_floor_smoothing) ||
        !isfinite(config->noise_gate_multiplier) ||
        !isfinite(config->noise_attenuation) ||
        !isfinite(config->interrupt_energy_threshold) ||
        config->energy_threshold <= 0.0f ||
        config->high_pass_alpha < 0.0 || config->high_pass_alpha > 1.0 ||
        config->min_noise_floor <= 0.0 ||
        config->noise_floor_smoothing < 0.0 ||
        config->noise_floor_smoothing > 1.0 ||
        config->noise_gate_multiplier <= 0.0 ||
        config->noise_attenuation < 0.0 ||
        config->noise_attenuation > 1.0 ||
        config->min_speech_ns == 0u ||
        config->endpoint_silence_ns == 0u ||
        config->max_utterance_ns == 0u) {
        return REFLEX_ERR_ARGUMENT;
    }
    return REFLEX_OK;
}

int reflex_session_init_v1(const reflex_config_v1 *config,
                           reflex_session_v1 **out_session) {
    if (out_session == NULL) {
        return REFLEX_ERR_ARGUMENT;
    }
    *out_session = NULL;
    int status = validate_config(config);
    if (status != REFLEX_OK) {
        return status;
    }

    reflex_session_v1 *session = calloc(1u, sizeof(*session));
    if (session == NULL) {
        return REFLEX_ERR_CAPACITY;
    }
    session->scratch = malloc(config->max_datagram_bytes);
    if ((config->flags & REFLEX_CONFIG_RETAIN_UTTERANCE) != 0u)
        session->utterance = malloc(config->max_utterance_bytes);
    if (session->scratch == NULL ||
        ((config->flags & REFLEX_CONFIG_RETAIN_UTTERANCE) != 0u &&
         session->utterance == NULL)) {
        free(session->utterance);
        free(session->scratch);
        free(session);
        return REFLEX_ERR_CAPACITY;
    }
    session->config = *config;
    clear_utterance(session);
    *out_session = session;
    return REFLEX_OK;
}

static int reject_frame(reflex_session_v1 *session,
                        int status,
                        reflex_decision_v1 *out_decision) {
    if (session != NULL) {
        session->rejected_frames++;
    }
    fill_decision(session, status, out_decision);
    return status;
}

int reflex_session_process_v1(reflex_session_v1 *session,
                              const uint8_t *pcm,
                              size_t pcm_len,
                              uint64_t now_ns,
                              uint32_t input_flags,
                              reflex_decision_v1 *out_decision) {
    if (session == NULL || out_decision == NULL) {
        return REFLEX_ERR_ARGUMENT;
    }
    if (pcm == NULL || pcm_len == 0u || (pcm_len & 1u) != 0u ||
        pcm_len > session->config.max_datagram_bytes || now_ns == 0u) {
        return reject_frame(session, REFLEX_ERR_FORMAT, out_decision);
    }
    if ((session->flags & (REFLEX_DECISION_COMPLETE | REFLEX_DECISION_CANCELLED)) != 0u) {
        return reject_frame(session, REFLEX_ERR_STATE, out_decision);
    }
    if (pcm_len > session->config.max_utterance_bytes - session->utterance_len) {
        session->flags |= REFLEX_DECISION_ENDPOINT |
                          REFLEX_DECISION_COMPLETE |
                          REFLEX_DECISION_OVERFLOW;
        session->endpoint_reason = REFLEX_ENDPOINT_CAPACITY;
        return reject_frame(session, REFLEX_ERR_CAPACITY, out_decision);
    }

    /*
     * Copy once into aligned session scratch, then condition in place. This
     * makes unaligned QUIC buffers safe without allocating or copying again.
     */
    memcpy(session->scratch, pcm, pcm_len);
    const int samples = (int)(pcm_len / sizeof(int16_t));
    session->energy = condition_pcm16(
        session->scratch,
        session->scratch,
        samples,
        session->config.high_pass_alpha,
        (session->config.flags & REFLEX_CONFIG_HIGH_PASS) != 0u,
        (session->config.flags & REFLEX_CONFIG_NOISE_GATE) != 0u,
        &session->hp_prev_in,
        &session->hp_prev_out,
        &session->noise_floor,
        session->config.min_noise_floor,
        session->config.noise_floor_smoothing,
        session->config.noise_gate_multiplier,
        session->config.noise_attenuation);

    const bool speech = session->energy >= session->config.energy_threshold;
    session->vad_confidence = session->energy * 10.0f;
    if (session->vad_confidence > 1.0f) {
        session->vad_confidence = 1.0f;
    }

    if ((session->config.flags & REFLEX_CONFIG_RETAIN_UTTERANCE) != 0u)
        memcpy(session->utterance + session->utterance_len, session->scratch, pcm_len);
    session->utterance_len += pcm_len;
    session->processed_frames++;
    session->flags &= ~(uint32_t)(REFLEX_DECISION_SPEECH | REFLEX_DECISION_INTERRUPT);

    if (speech) {
        session->flags |= REFLEX_DECISION_SPEECH | REFLEX_DECISION_VOICE_SEEN;
        if (session->speech_started_ns == 0u) {
            session->speech_started_ns = now_ns;
        }
        session->silence_started_ns = 0u;
        session->silence_audio_ns = 0u;

        if ((input_flags & REFLEX_INPUT_ASSISTANT_SPEAKING) != 0u &&
            session->energy >= session->config.interrupt_energy_threshold) {
            if (session->interrupt_started_ns == 0u) {
                session->interrupt_started_ns = now_ns;
            }
            if (elapsed_at_least(now_ns,
                                 session->interrupt_started_ns,
                                 session->config.interrupt_hold_ns) &&
                !session->interrupt_emitted) {
                session->flags |= REFLEX_DECISION_INTERRUPT;
                session->interrupt_emitted = 1;
            }
        } else {
            session->interrupt_started_ns = 0u;
            session->interrupt_emitted = 0;
        }
    } else {
        session->interrupt_started_ns = 0u;
        session->interrupt_emitted = 0;
        /* Delivery stalls do not establish acoustic silence. The utterance
         * capacity bounds this sum; each PCM sample contributes once. */
        session->silence_audio_ns +=
            (uint64_t)(pcm_len / sizeof(int16_t)) * UINT64_C(1000000000) /
            session->config.sample_rate_hz;
        if ((session->flags & REFLEX_DECISION_VOICE_SEEN) != 0u &&
            session->silence_started_ns == 0u) {
            session->silence_started_ns = now_ns;
        }
    }

    if ((session->flags & REFLEX_DECISION_VOICE_SEEN) != 0u &&
        elapsed_at_least(now_ns,
                         session->speech_started_ns,
                         session->config.max_utterance_ns)) {
        session->flags |= REFLEX_DECISION_ENDPOINT | REFLEX_DECISION_COMPLETE;
        session->endpoint_reason = REFLEX_ENDPOINT_MAX_DURATION;
    } else if ((session->flags & REFLEX_DECISION_VOICE_SEEN) != 0u &&
               elapsed_at_least(now_ns,
                                session->speech_started_ns,
                                session->config.min_speech_ns) &&
               elapsed_at_least(now_ns,
                                session->silence_started_ns,
                                session->config.endpoint_silence_ns) &&
               session->silence_audio_ns >= session->config.endpoint_silence_ns) {
        session->flags |= REFLEX_DECISION_ENDPOINT | REFLEX_DECISION_COMPLETE;
        session->endpoint_reason = REFLEX_ENDPOINT_SILENCE;
    }

    fill_decision(session, REFLEX_OK, out_decision);
    return REFLEX_OK;
}

int reflex_session_snapshot_v1(const reflex_session_v1 *session,
                               uint8_t *out_pcm,
                               size_t out_capacity,
                               size_t *required_bytes,
                               reflex_decision_v1 *out_decision) {
    if (session == NULL || required_bytes == NULL) {
        return REFLEX_ERR_ARGUMENT;
    }
    *required_bytes = session->utterance_len;
    if ((session->config.flags & REFLEX_CONFIG_RETAIN_UTTERANCE) == 0u) {
        fill_decision(session, REFLEX_ERR_STATE, out_decision);
        return REFLEX_ERR_STATE;
    }
    if (out_capacity < session->utterance_len ||
        (session->utterance_len != 0u && out_pcm == NULL)) {
        fill_decision(session, REFLEX_ERR_CAPACITY, out_decision);
        return REFLEX_ERR_CAPACITY;
    }
    if (session->utterance_len != 0u) {
        memcpy(out_pcm, session->utterance, session->utterance_len);
    }
    fill_decision(session, REFLEX_OK, out_decision);
    return REFLEX_OK;
}

int reflex_session_finalize_v1(reflex_session_v1 *session,
                               uint8_t *out_pcm,
                               size_t out_capacity,
                               size_t *written_bytes,
                               uint32_t endpoint_reason,
                               reflex_decision_v1 *out_decision) {
    if (session == NULL || written_bytes == NULL) {
        return REFLEX_ERR_ARGUMENT;
    }
    *written_bytes = session->utterance_len;
    if ((session->config.flags & REFLEX_CONFIG_RETAIN_UTTERANCE) == 0u) {
        fill_decision(session, REFLEX_ERR_STATE, out_decision);
        return REFLEX_ERR_STATE;
    }
    if (out_capacity < session->utterance_len ||
        (session->utterance_len != 0u && out_pcm == NULL)) {
        fill_decision(session, REFLEX_ERR_CAPACITY, out_decision);
        return REFLEX_ERR_CAPACITY;
    }
    if (session->utterance_len != 0u) {
        memcpy(out_pcm, session->utterance, session->utterance_len);
    }
    if (session->endpoint_reason == REFLEX_ENDPOINT_NONE) {
        session->endpoint_reason = endpoint_reason == REFLEX_ENDPOINT_NONE
                                       ? REFLEX_ENDPOINT_CLIENT_END
                                       : endpoint_reason;
    }
    session->flags |= REFLEX_DECISION_ENDPOINT | REFLEX_DECISION_COMPLETE;
    fill_decision(session, REFLEX_OK, out_decision);
    clear_utterance(session);
    return REFLEX_OK;
}

void reflex_session_cancel_v1(reflex_session_v1 *session,
                              reflex_decision_v1 *out_decision) {
    if (session == NULL) {
        fill_decision(NULL, REFLEX_ERR_ARGUMENT, out_decision);
        return;
    }
    session->utterance_len = 0u;
    session->flags |= REFLEX_DECISION_ENDPOINT |
                      REFLEX_DECISION_COMPLETE |
                      REFLEX_DECISION_CANCELLED;
    session->endpoint_reason = REFLEX_ENDPOINT_CANCELLED;
    fill_decision(session, REFLEX_OK, out_decision);
}

void reflex_session_reset_v1(reflex_session_v1 *session) {
    if (session == NULL) {
        return;
    }
    clear_utterance(session);
}

void reflex_session_destroy_v1(reflex_session_v1 *session) {
    if (session == NULL) {
        return;
    }
    free(session->utterance);
    free(session->scratch);
    free(session);
}
