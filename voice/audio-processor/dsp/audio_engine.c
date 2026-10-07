#include "audio_engine_internal.h"
#include "pcm_condition.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/* Idle utterance capacity kept after discard/finalize-reset so the next
 * utterance can grow without a cold realloc storm; larger caps shrink back. */
#define AUDIO_ENGINE_UTTERANCE_MIN_KEEP 4096

static float elapsed_ms(uint64_t started_ns, uint64_t ended_ns) {
    if (started_ns == 0 || ended_ns < started_ns) return 0.0f;
    return (float)((double)(ended_ns - started_ns) / 1000000.0);
}

static int elapsed_at_least(uint64_t now_ns, uint64_t started_ns, uint64_t duration_ns) {
    return started_ns != 0 && now_ns >= started_ns && now_ns - started_ns >= duration_ns;
}

static double audio_engine_rtf(double process_ms, double audio_ms);
static int audio_engine_is_complete(const audio_engine_session* s);
static int audio_engine_has_voice(const audio_engine_session* s);
static int audio_engine_has_pending_audio(const audio_engine_session* s);
static int audio_engine_should_do_live_partial(const audio_engine_session* s);
static size_t audio_engine_utterance_bytes(const audio_engine_session* s);
static int audio_engine_vad_endpoint(audio_engine_session* s, int has_voice,
                                     uint64_t now_ns, uint64_t min_speech_ns,
                                     uint64_t silence_ns, uint64_t max_utt_ns);
static int audio_engine_decide_process_reason(
    audio_engine_session* s, size_t total_bytes, size_t buffer_size,
    size_t max_buffer_size, float chunk_timeout_s, uint64_t now_ns,
    uint64_t complete_settle_ns, int endpointing_enabled,
    uint64_t min_speech_ns, uint64_t silence_ns, uint64_t max_utt_ns,
    int has_voice_activity, int utterance_has_voice, int has_utterance_id);
static int audio_engine_should_trigger_live_partial(const audio_engine_session* s, int enabled);
static uint64_t audio_engine_suggest_monitor_interval_ns(const audio_engine_session* s,
                                                         uint64_t base_ns,
                                                         uint64_t short_ns);
static int audio_engine_adjudicate_interrupt(
    audio_engine_session* s, int speech, float audio_level, float threshold,
    float duration_s, uint64_t now_ns);
static int audio_engine_refresh_group_policy(audio_engine_session* s,
                                             uint64_t now_ns);

static int audio_engine_config_valid(float energy_thresh,
                                     double highpass_alpha,
                                     double min_noise_floor,
                                     double noise_floor_smoothing,
                                     double noise_gate_mult,
                                     double noise_attenuation) {
    return isfinite(energy_thresh) &&
        isfinite(highpass_alpha) && highpass_alpha >= 0.0 &&
        highpass_alpha <= 1.0 &&
        isfinite(min_noise_floor) && min_noise_floor > 0.0 &&
        isfinite(noise_floor_smoothing) && noise_floor_smoothing >= 0.0 &&
        noise_floor_smoothing <= 1.0 &&
        isfinite(noise_gate_mult) && noise_gate_mult > 0.0 &&
        isfinite(noise_attenuation) && noise_attenuation >= 0.0 &&
        noise_attenuation <= 1.0;
}

static audio_engine_session *audio_engine_configure_zeroed(
    audio_engine_session *s,
    float energy_thresh,
    double highpass_alpha,
    int enable_hp,
    int enable_ng,
    double min_noise_floor,
    double noise_floor_smoothing,
    double noise_gate_mult,
    double noise_attenuation) {

    s->energy_thresh = energy_thresh > 0 ? energy_thresh : 0.01f;

    s->highpass_alpha          = highpass_alpha;
    s->enable_hp               = enable_hp;
    s->enable_ng               = enable_ng;
    s->min_noise_floor         = min_noise_floor;
    s->noise_floor_smoothing   = noise_floor_smoothing;
    s->noise_gate_mult         = noise_gate_mult;
    s->noise_attenuation       = noise_attenuation;

    // Contiguous utterance via dynbuf (amortized grow + idle shrink).
    dynbuf_init(&s->utterance);

    s->frame_seq = 0;

    // Reuse the process-global ring initialized by the service. It remains
    // process-local and its capacity is independent of individual sessions.
    s->telemetry = telemetry_get_global_ring();
    s->owns_telemetry = 0;
    if (!s->telemetry) {
        // Standalone tests and tools may create an engine without initializing
        // the process-global ring. Give those sessions a private fallback.
        s->telemetry = telemetry_ring_new(1024);
        s->owns_telemetry = s->telemetry != NULL;
    }

    // C endpoint state (zeroed for "no utterance yet").
    s->utterance_started_ns = 0;
    s->silence_started_ns = 0;
    s->utterance_has_voice = 0;
    s->last_chunk_ns = 0;


    s->complete_marked_ns = 0;
    s->is_complete_flag = 0;

    // Process-local correlation key. C generates it on the first frame and
    // rotates it when the utterance state is cleared.
    s->turn_id_hi = 0;
    s->turn_id_lo = 0;

    // Last partial for C rate limit on live partials (delta/interval).
    s->last_partial_ns = 0;
    s->last_partial_bytes = 0;

    return s;
}

audio_engine_session *audio_engine_session_init_zeroed(
    audio_engine_session *s,
    float energy_thresh,
    double highpass_alpha,
    int enable_hp,
    int enable_ng,
    double min_noise_floor,
    double noise_floor_smoothing,
    double noise_gate_mult,
    double noise_attenuation) {
    if (!s || !audio_engine_config_valid(
            energy_thresh,
            highpass_alpha,
            min_noise_floor,
            noise_floor_smoothing,
            noise_gate_mult,
            noise_attenuation)) return NULL;
    return audio_engine_configure_zeroed(
        s,
        energy_thresh,
        highpass_alpha,
        enable_hp,
        enable_ng,
        min_noise_floor,
        noise_floor_smoothing,
        noise_gate_mult,
        noise_attenuation);
}

audio_engine_session* audio_engine_create(float energy_thresh,
                                          double highpass_alpha,
                                          int enable_hp,
                                          int enable_ng,
                                          double min_noise_floor,
                                          double noise_floor_smoothing,
                                          double noise_gate_mult,
                                          double noise_attenuation)
{
    audio_engine_session *s;
    if (!audio_engine_config_valid(
            energy_thresh,
            highpass_alpha,
            min_noise_floor,
            noise_floor_smoothing,
            noise_gate_mult,
            noise_attenuation)) return NULL;
    s = (audio_engine_session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    return audio_engine_configure_zeroed(
        s,
        energy_thresh,
        highpass_alpha,
        enable_hp,
        enable_ng,
        min_noise_floor,
        noise_floor_smoothing,
        noise_gate_mult,
        noise_attenuation);
}

static int audio_engine_refresh_group_policy(audio_engine_session* s,
                                             uint64_t now_ns) {
    if (!s || s->vision_received_ns == 0 || s->vision_stale_after_ns == 0) {
        return 0;
    }
    if (now_ns == 0) {
        now_ns = telemetry_monotonic_ns();
    }
    if (now_ns < s->vision_received_ns ||
        now_ns - s->vision_received_ns > s->vision_stale_after_ns) {
        s->group_deliberating = 0;
        s->group_enter_count = 0;
        s->group_exit_count = 0;
        return 0;
    }
    return 1;
}

audio_group_policy_decision audio_engine_apply_vision_event(
    audio_engine_session* s,
    audio_vision_signal signal,
    audio_group_policy_config config) {
    audio_group_policy_decision decision = {0};
    if (!s ||
        signal.sequence == 0 ||
        signal.observed_at_mono_ns == 0 ||
        signal.received_at_ns == 0 ||
        signal.person_count > 32u ||
        !isfinite(signal.motion_energy) ||
        signal.motion_energy < 0.0f ||
        signal.motion_energy > 1.0f ||
        signal.gaze_cluster > AUDIO_VISION_GAZE_SHARED_OFFTABLE ||
        !isfinite(config.motion_threshold) ||
        config.motion_threshold < 0.0f ||
        config.motion_threshold > 1.0f ||
        config.enter_samples == 0 ||
        config.exit_samples == 0 ||
        config.stale_after_ns == 0) {
        return decision;
    }
    if (s->vision_sequence != 0 &&
        (signal.sequence <= s->vision_sequence ||
         signal.observed_at_mono_ns <= s->vision_observed_ns)) {
        return decision;
    }

    s->vision_sequence = signal.sequence;
    s->vision_observed_ns = signal.observed_at_mono_ns;
    s->vision_received_ns = signal.received_at_ns;
    s->vision_stale_after_ns = config.stale_after_ns;
    decision.accepted = 1;

    if ((signal.flags & AUDIO_VISION_SCENE_CHANGED) != 0u) {
        s->group_deliberating = 0;
        s->group_enter_count = 0;
        s->group_exit_count = 0;
    } else {
        const int shared_attention =
            signal.gaze_cluster == AUDIO_VISION_GAZE_SHARED_TABLETOP ||
            signal.gaze_cluster == AUDIO_VISION_GAZE_SHARED_OFFTABLE;
        const int audio_evidence =
            s->current_is_speech || s->utterance_has_voice;
        const int qualifying =
            signal.person_count >= 2u &&
            signal.motion_energy >= config.motion_threshold &&
            shared_attention &&
            audio_evidence;
        decision.qualifying = qualifying;
        if (qualifying) {
            s->group_exit_count = 0;
            if (s->group_enter_count < config.enter_samples) {
                s->group_enter_count++;
            }
            if (s->group_enter_count >= config.enter_samples) {
                s->group_deliberating = 1;
            }
        } else {
            s->group_enter_count = 0;
            if (s->group_exit_count < config.exit_samples) {
                s->group_exit_count++;
            }
            if (s->group_exit_count >= config.exit_samples) {
                s->group_deliberating = 0;
            }
        }
    }

    decision.group_deliberating = s->group_deliberating;
    decision.vision_fresh =
        audio_engine_refresh_group_policy(s, signal.received_at_ns);
    decision.enter_count = s->group_enter_count;
    decision.exit_count = s->group_exit_count;
    return decision;
}

static void audio_engine_process_internal(audio_engine_session* s,
                                          const uint8_t* pcm,
                                          size_t len,
                                          uint64_t frame_now_ns,
                                          audio_process_options options,
                                          size_t max_utterance_capacity,
                                          audio_decision* decision)
{
    if (!s || !pcm || len == 0 || (len & 1u) != 0u ||
        len / sizeof(int16_t) > (size_t)INT_MAX || !decision) {
        if (decision) {
            memset(decision, 0, sizeof(*decision));
            decision->flags = AUDIO_DECISION_ERROR;
        }
        return;
    }

    const uint64_t process_started_ns = telemetry_monotonic_ns();
    const telemetry_tsc_t process_started_cycles = get_tsc();
    frame_telemetry_t frame_event = {0};
    int frame_event_pending = 0;

    // 1. Reserve the final utterance span and condition PCM into it.
    int n = (int)(len / sizeof(int16_t));
    uint8_t* conditioned_bytes = NULL;
    if (dynbuf_extend_bounded(
            &s->utterance,
            len,
            max_utterance_capacity,
            &conditioned_bytes) != 0) {
        memset(decision, 0, sizeof(*decision));
        decision->flags = AUDIO_DECISION_ERROR;
        return;
    }
    int16_t* out16 = (int16_t*)(void*)conditioned_bytes;

    float energy = condition_pcm16(pcm, out16, n,
                                   s->highpass_alpha,
                                   s->enable_hp,
                                   s->enable_ng,
                                   &s->hp_prev_in,
                                   &s->hp_prev_out,
                                   &s->noise_floor,
                                   s->min_noise_floor,
                                   s->noise_floor_smoothing,
                                   s->noise_gate_mult,
                                   s->noise_attenuation);

    // 2. Energy VAD uses the exact conditioned PCM RMS returned by the same
    // pass. Deleted auxiliary features did not feed a product consumer.
    int is_speech = energy >= s->energy_thresh;

    if (is_speech) {
        const size_t chunk_end = dynbuf_len(&s->utterance);
        const size_t chunk_start = chunk_end - len;
        if (!s->speech_window_valid) {
            s->first_speech_offset = chunk_start;
            s->speech_window_valid = 1;
        }
        s->speech_end_offset = chunk_end;
    }

    // Preserve the established conditioned-energy behavior used by VAD.
    {
        float cleanliness = 1.0f - (float)s->noise_floor;
        /* The conditioner keeps its noise state finite. */
        const float bounded_cleanliness = cleanliness > 0.0f
            ? cleanliness : 0.0f;
        energy *= (0.8f + 0.2f * bounded_cleanliness);  // boost if low noise
        if (energy > 1.0f) energy = 1.0f;
    }


    s->current_is_speech = is_speech;
    const uint64_t vad_ended_ns = telemetry_monotonic_ns();

    // The process call owns all per-frame C state. Callers pass the same
    // timestamp used by their lifecycle snapshots, eliminating the former
    // UpdateLastChunk/VADEndpoint/SetHasVoice/UpdateSilence cgo tail.
    if (frame_now_ns == 0) {
        frame_now_ns = process_started_ns;
    }
    const int vision_fresh =
        audio_engine_refresh_group_policy(s, frame_now_ns);
    s->last_chunk_ns = frame_now_ns;
    (void)audio_engine_vad_endpoint(
        s, is_speech, frame_now_ns, 0, 0, 0);
    const int interrupt =
        (options.flags & AUDIO_PROCESS_INTERRUPT_ENABLED) != 0u &&
        audio_engine_adjudicate_interrupt(
            s,
            is_speech,
            energy,
            options.interrupt_threshold,
            options.interrupt_duration_s,
            frame_now_ns);

    // === C Telemetry + Hot Micro-Orchestration emission (first integration point) ===
    // Build the frame now and emit it at function exit after real elapsed time
    // is known. TSC is only a cycle counter; monotonic raw ns owns latency/RTF.

    if (s->telemetry) {
        frame_event_pending = 1;
        frame_event.tsc_start = process_started_cycles;
        frame_event.turn_id_hi = s->turn_id_hi;
        frame_event.turn_id_lo = s->turn_id_lo;
        if (frame_event.turn_id_lo == 0) {
            // The bounded ring is local to the C reflex. The C service owns
            // durable product turn correlation outside the DSP kernel.
            uint64_t tid = telemetry_generate_turn_id();
            s->turn_id_hi = 0;
            s->turn_id_lo = tid;
            frame_event.turn_id_hi = 0;
            frame_event.turn_id_lo = tid;
        }
        frame_event.frame_seq = s->frame_seq++;
        frame_event.features.energy = energy;
        frame_event.features.noise_floor = (float)s->noise_floor;
        frame_event.bytes_processed = (uint32_t)len;

        stage_event_t st = {0};
        st.tsc        = get_tsc();
        st.turn_id_hi = s->turn_id_hi;
        st.turn_id_lo = s->turn_id_lo;
        st.stage_id   = STAGE_KIND_VAD;
        st.flags      = (uint16_t)(is_speech ? 1 : 0);
        st.features   = frame_event.features;
        st.latency_ms = elapsed_ms(process_started_ns, vad_ended_ns);
        telemetry_emit_stage(s->telemetry, &st);

        // Reflex facts remain in the C process-local ring. The C service
        // emits product telemetry and durable events at turn boundaries.
    }

    // 3. Fill the smallest decision consumed by the service.
    decision->utterance_bytes = audio_engine_utterance_bytes(s);
    decision->energy = energy;
    decision->vad_confidence = energy * 10.0f; // match previous scaling
    if (decision->vad_confidence > 1.0f) decision->vad_confidence = 1.0f;
    decision->flags = 0u;
    if (is_speech) decision->flags |= AUDIO_DECISION_SPEECH;
    if (audio_engine_has_pending_audio(s)) {
        decision->flags |= AUDIO_DECISION_PENDING_AUDIO;
    }
    if (audio_engine_should_do_live_partial(s)) {
        decision->flags |= AUDIO_DECISION_LIVE_PARTIAL;
    }
    if (audio_engine_is_complete(s)) {
        decision->flags |= AUDIO_DECISION_COMPLETE;
    }
    if (audio_engine_has_voice(s)) {
        decision->flags |= AUDIO_DECISION_VOICE_SEEN;
    }
    if (interrupt) {
        decision->flags |= AUDIO_DECISION_INTERRUPT;
    }
    if (s->group_deliberating) {
        decision->flags |= AUDIO_DECISION_GROUP_DELIBERATING;
    }
    if (vision_fresh) {
        decision->flags |= AUDIO_DECISION_VISION_FRESH;
    }

    if (frame_event_pending) {
        const uint64_t process_ended_ns = telemetry_monotonic_ns();
        frame_event.tsc_end = get_tsc();
        frame_event.features.energy = decision->energy;
        frame_event.features.noise_floor = (float)s->noise_floor;
        const double process_ms = (double)elapsed_ms(process_started_ns, process_ended_ns);
        const double audio_ms = (double)n / 16000.0 * 1000.0;
        frame_event.rtf_this_frame = (float)audio_engine_rtf(process_ms, audio_ms);
        telemetry_emit_frame(s->telemetry, &frame_event);
    }
}

void audio_engine_process(audio_engine_session* s,
                          const uint8_t* pcm,
                          size_t len,
                          uint64_t now_ns,
                          audio_process_options options,
                          audio_decision* decision)
{
    audio_engine_process_internal(s, pcm, len, now_ns, options, SIZE_MAX, decision);
}

void audio_engine_process_bounded(audio_engine_session* s,
                                  const uint8_t* pcm,
                                  size_t len,
                                  uint64_t now_ns,
                                  audio_process_options options,
                                  size_t max_utterance_capacity,
                                  audio_decision* decision)
{
    audio_engine_process_internal(
        s, pcm, len, now_ns, options, max_utterance_capacity, decision);
}

static audio_finalized_utterance audio_engine_finalize_internal(
    audio_engine_session* s,
    size_t leading_context_bytes,
    size_t trailing_context_bytes,
    int trim_speech_window)
{
    audio_finalized_utterance finalized = {0};
    if (!s || dynbuf_len(&s->utterance) == 0 || dynbuf_data(&s->utterance) == NULL) {
        return finalized;
    }

    finalized.captured_length = dynbuf_len(&s->utterance);
    finalized.buffer = dynbuf_data(&s->utterance);
    finalized.length = finalized.captured_length;
    if (trim_speech_window && s->speech_window_valid &&
        s->first_speech_offset <= s->speech_end_offset &&
        s->speech_end_offset <= finalized.captured_length) {
        size_t start;
        size_t end;

        /* PCM16 offsets stay sample-aligned even when a caller supplies an
         * odd context byte count. */
        leading_context_bytes &= ~(size_t)1u;
        trailing_context_bytes &= ~(size_t)1u;
        start = s->first_speech_offset > leading_context_bytes
            ? s->first_speech_offset - leading_context_bytes
            : 0u;
        end = s->speech_end_offset;
        if (trailing_context_bytes >= finalized.captured_length - end)
            end = finalized.captured_length;
        else
            end += trailing_context_bytes;

        finalized.trimmed_prefix_length = start;
        finalized.trimmed_suffix_length = finalized.captured_length - end;
        finalized.length = end - start;
        if (start != 0u)
            memmove(finalized.buffer, finalized.buffer + start, finalized.length);
    }
    if (audio_engine_has_pending_audio(s)) {
        finalized.flags |= AUDIO_DECISION_PENDING_AUDIO;
    }
    if (audio_engine_should_do_live_partial(s)) {
        finalized.flags |= AUDIO_DECISION_LIVE_PARTIAL;
    }
    if (audio_engine_is_complete(s)) {
        finalized.flags |= AUDIO_DECISION_COMPLETE;
    }
    if (audio_engine_has_voice(s)) {
        finalized.flags |= AUDIO_DECISION_VOICE_SEEN;
    }
    if (s->group_deliberating) {
        finalized.flags |= AUDIO_DECISION_GROUP_DELIBERATING;
    }

    // Reset after hand-off: caller owns the pointer; dynbuf no longer does.
    dynbuf_init(&s->utterance);
    s->first_speech_offset = 0u;
    s->speech_end_offset = 0u;
    s->speech_window_valid = 0;

    return finalized;
}

audio_finalized_utterance audio_engine_finalize(audio_engine_session* s)
{
    return audio_engine_finalize_internal(s, 0u, 0u, 0);
}

audio_finalized_utterance audio_engine_finalize_speech_window(
    audio_engine_session* s,
    size_t leading_context_bytes,
    size_t trailing_context_bytes)
{
    return audio_engine_finalize_internal(
        s,
        leading_context_bytes,
        trailing_context_bytes,
        1);
}

size_t audio_engine_utterance_capacity(const audio_engine_session* s)
{
    return s ? dynbuf_cap(&s->utterance) : 0u;
}

int audio_engine_copy_utterance(audio_engine_session* s,
                                uint8_t** out_buf,
                                size_t* out_len)
{
    if (!s || !out_buf || !out_len) return 0;

    *out_buf = NULL;
    *out_len = 0;

    size_t n = dynbuf_len(&s->utterance);
    uint8_t* src = dynbuf_data(&s->utterance);
    if (n == 0 || src == NULL) {
        return 0;
    }

    uint8_t* copy = (uint8_t*)malloc(n);
    if (!copy) {
        return 0;
    }
    memcpy(copy, src, n);
    *out_buf = copy;
    *out_len = n;
    return 1;
}

static void audio_engine_discard_utterance(audio_engine_session* s)
{
    if (!s) return;
    /* Clear length and shrink capacity toward min-keep when idle. */
    dynbuf_clear(&s->utterance, AUDIO_ENGINE_UTTERANCE_MIN_KEEP);
}

void audio_engine_free_buffer(uint8_t* buf) {
    free(buf);
}

void audio_engine_session_deinit(audio_engine_session *s) {
    if (!s) return;
    // Free any un-finalized utterance buffer (e.g. aborted turn).
    dynbuf_free(&s->utterance);
    if (s->telemetry && s->owns_telemetry) {
        telemetry_ring_free(s->telemetry);
    }
}

void audio_engine_destroy(audio_engine_session* s) {
    if (!s) return;
    audio_engine_session_deinit(s);
    free(s);
}

static double audio_engine_rtf(double process_ms, double audio_ms) {
    if (audio_ms <= 0.0) return 0.0;
    return process_ms / audio_ms;
}

static int audio_engine_adjudicate_interrupt(
    audio_engine_session* s, int speech, float audio_level, float threshold,
    float duration_s, uint64_t now_ns) {
    if (!s) return 0;
    if (!speech || audio_level < threshold) {
        s->interrupt_started_ns = 0;
        s->interrupt_fired = 0;
        return 0;
    }
    if (s->interrupt_fired) return 0;

    if (now_ns == 0) {
        now_ns = telemetry_monotonic_ns();
    }
    if (s->interrupt_started_ns == 0) {
        s->interrupt_started_ns = now_ns;
    }

    const double duration_ns_f64 = (double)duration_s * 1000000000.0;
    const uint64_t duration_ns = duration_ns_f64 > 0.0
        ? (uint64_t)duration_ns_f64
        : 0;
    const uint64_t elapsed_ns = now_ns >= s->interrupt_started_ns
        ? now_ns - s->interrupt_started_ns
        : 0;
    if (elapsed_ns < duration_ns) return 0;

    s->interrupt_fired = 1;
    return 1;
}

void audio_engine_reset_interrupt(audio_engine_session* s) {
    if (!s) return;
    s->interrupt_started_ns = 0;
    s->interrupt_fired = 0;
}

static int audio_engine_vad_endpoint(audio_engine_session* s, int has_voice, uint64_t now_ns,
                                     uint64_t min_speech_ns, uint64_t silence_ns,
                                     uint64_t max_utt_ns) {
    if (!s) return 0;

    if (has_voice) {
        if (s->utterance_started_ns == 0) s->utterance_started_ns = now_ns;
        s->silence_started_ns = 0;
        s->utterance_has_voice = 1;
        if (max_utt_ns > 0 && elapsed_at_least(now_ns, s->utterance_started_ns, max_utt_ns)) {
            return 5; // endpoint (max utterance)
        }
        return 0;
    }

    // no voice / silence side
    if (s->utterance_started_ns != 0 && s->silence_started_ns == 0) {
        s->silence_started_ns = now_ns;
    }
    // Utterance evidence is sticky. Trailing silence ends speech activity but
    // must not erase the fact that this utterance contained speech; the
    // explicit clear_utterance_state boundary owns that reset.

    // min speech elapsed + silence duration -> vad endpoint
    if (min_speech_ns > 0 &&
        elapsed_at_least(now_ns, s->utterance_started_ns, min_speech_ns) &&
        silence_ns > 0 && elapsed_at_least(now_ns, s->silence_started_ns, silence_ns)) {
        return 5;
    }
    if (max_utt_ns > 0 && elapsed_at_least(now_ns, s->utterance_started_ns, max_utt_ns)) {
        return 5;
    }
    return 0;
}

// Extended reason: C byte/feature + timers (Go passes ns from steady clock for wall/config).
// Ported more Go logic: threshold + feature based early return + timer consult (vad_endpoint style).
static int audio_engine_extended_process_reason(audio_engine_session* s, size_t total_bytes,
                                                size_t buffer_size, float chunk_timeout_s,
                                                uint64_t now_ns, uint64_t min_speech_ns,
                                                uint64_t silence_ns) {
    (void)chunk_timeout_s;
    if (!s) return 0;
    if (total_bytes >= buffer_size) return 1; // threshold
    // Feature + timer: if we have started state and silence conditions met (caller may have reset via has_voice in process)
    if (min_speech_ns > 0 && silence_ns > 0 &&
        elapsed_at_least(now_ns, s->utterance_started_ns, min_speech_ns) &&
        elapsed_at_least(now_ns, s->silence_started_ns, silence_ns)) {
        return 5; // vad endpoint like
    }
    // The process boundary already owns energy VAD; this monitor only needs
    // byte and timer state.
    return 0;
}

static int audio_engine_should_finalize(audio_engine_session* s, size_t buffer_size,
                                        size_t max_buffer_size, float chunk_timeout_s) {
    (void)chunk_timeout_s;
    if (!s) return 0;
    size_t utt = dynbuf_len(&s->utterance);
    if (utt >= max_buffer_size) return 1;
    if (utt >= buffer_size) return 1;
    // Enhanced deeper: if C complete flag set, allow finalize (settle decided by caller or check_complete).
    if (s->is_complete_flag) return 1;
    return 0;
}

// Consolidated process reason in C: removes Go time.Since, complete checks, etc from hot path.
// Uses passed Go state + internal (timers, noise_floor, utterance_len) + boosted features.
// Deeper: noise quality (low noise) can bias toward endpoint/threshold when voice present.
// Now owns complete settle too: if marked complete, C decides based on its complete_marked_ns / last_chunk vs settle.
static int audio_engine_decide_process_reason(
  audio_engine_session* s,
  size_t total_bytes,
  size_t buffer_size,
  size_t max_buffer_size,
  float chunk_timeout_s,
  uint64_t now_ns,
  uint64_t complete_settle_ns,
  int endpointing_enabled,
  uint64_t min_speech_ns,
  uint64_t silence_ns,
  uint64_t max_utt_ns,
  int has_voice_activity,
  int utterance_has_voice,
  int has_utterance_id
) {
    if (!s) return 0;

    if (!has_utterance_id && !has_voice_activity && !utterance_has_voice) {
        return 0;
    }

    if (total_bytes >= max_buffer_size) {
        return 4; // max buffer
    }

    if (s->is_complete_flag && (total_bytes > 0 || utterance_has_voice)) {
        uint64_t base = s->complete_marked_ns ? s->complete_marked_ns : s->last_chunk_ns;
        if (base > 0 && complete_settle_ns > 0) {
            uint64_t elapsed = (now_ns > base) ? (now_ns - base) : 0;
            if (elapsed >= complete_settle_ns) {
                return 3; // complete
            }
            return 0; // still settling per C
        }
        // no timer yet; signal caller can check or treat as ready if bytes
        return 3;
    }

    if (endpointing_enabled && has_utterance_id && (total_bytes > 0 || utterance_has_voice)) {
        int er = audio_engine_vad_endpoint(s, has_voice_activity, now_ns, min_speech_ns, silence_ns, max_utt_ns);
        if (er == 5) {
            return 5;
        }
    }


    if (s->last_chunk_ns > 0) {
        uint64_t elapsed_ns = (now_ns > s->last_chunk_ns) ? (now_ns - s->last_chunk_ns) : 0;
        double elapsed_s = (double)elapsed_ns / 1000000000.0;

        double eff_timeout = (double)chunk_timeout_s;
        if (s->noise_floor > 0.01) {
            eff_timeout *= (1.0 + fmin(0.8, (s->noise_floor - 0.01) * 40.0)); // up to ~1.8x
        }
        if (!has_voice_activity && total_bytes < buffer_size && elapsed_s < eff_timeout) {
            return 0;
        }
        if (elapsed_s > eff_timeout && total_bytes > 0) {
            return 2; // timeout
        }
    }
    // Use extended for timer (byte check only if !voice to match Go expectation for active speech)
    if (total_bytes > 0) {
        int r = audio_engine_extended_process_reason(s, total_bytes, buffer_size, chunk_timeout_s, now_ns, min_speech_ns, silence_ns);
        if (r != 0 && !utterance_has_voice) return 1; // threshold only if no voice
    }
    if (audio_engine_should_finalize(s, buffer_size, max_buffer_size, chunk_timeout_s) && !utterance_has_voice) {
        return 1;
    }

    return 0;
}

static int audio_engine_should_trigger_live_partial(const audio_engine_session* s, int enabled) {
	if (!s || !enabled) return 0;
	return audio_engine_should_do_live_partial(s);
}

// Deeper: mark complete in C (called from Go markComplete with now_ns). Owns the settle timer base.
audio_monitor_state audio_engine_mark_complete(audio_engine_session* s, uint64_t now_ns) {
    audio_monitor_state state = {0};
    if (!s) return state;
    (void)audio_engine_refresh_group_policy(s, now_ns);
    s->is_complete_flag = 1;
    s->complete_marked_ns = now_ns ? now_ns : s->last_chunk_ns;
    // If no prior last_chunk, seed it so settle can progress
    if (s->complete_marked_ns == 0) s->complete_marked_ns = now_ns;
    state.pending_audio = audio_engine_has_pending_audio(s);
    state.live_partial_eligible = audio_engine_should_do_live_partial(s);
    state.utterance_bytes = audio_engine_utterance_bytes(s);
    state.is_complete = audio_engine_is_complete(s);
    state.has_voice = audio_engine_has_voice(s);
    state.group_deliberating = s->group_deliberating;
    return state;
}

// Clear utterance + complete state inside the atomic finish-processing ABI.
static void audio_engine_clear_utterance_state(audio_engine_session* s) {
    if (!s) return;
    s->utterance_started_ns = 0;
    s->silence_started_ns = 0;
    s->current_is_speech = 0;
    s->utterance_has_voice = 0;
    s->last_chunk_ns = 0;
    s->interrupt_started_ns = 0;
    s->interrupt_fired = 0;
    s->complete_marked_ns = 0;
    s->is_complete_flag = 0;
    s->first_speech_offset = 0u;
    s->speech_end_offset = 0u;
    s->speech_window_valid = 0;
    // Reset last partial so new utterance starts fresh for live partial rate limits.
    s->last_partial_ns = 0;
    s->last_partial_bytes = 0;
    s->turn_id_hi = 0;
    s->turn_id_lo = 0;
    s->frame_seq = 0;
    // Note: we do NOT clear utterance buffer here; caller coordinates handoff or discard.
}

int audio_engine_finish_processing(audio_engine_session* s, int force_final) {
    if (!s) return 0;
    const int final = force_final || s->is_complete_flag;
    if (!final) return 0;

    // Finalized PCM may already have been detached for STT, while frames that
    // arrived during inference may have started a new buffer. Final cleanup
    // owns both the discard and lifecycle reset in one C critical section.
    audio_engine_discard_utterance(s);
    audio_engine_clear_utterance_state(s);
    return 1;
}

int audio_engine_check_complete_settle(audio_engine_session* s, uint64_t now_ns, uint64_t complete_settle_ns) {
    if (!s || !s->is_complete_flag || dynbuf_len(&s->utterance) == 0) return 0;
    uint64_t base = s->complete_marked_ns ? s->complete_marked_ns : s->last_chunk_ns;
    if (base == 0) return 1; // no timer: signal ready
    uint64_t elapsed = (now_ns > base) ? (now_ns - base) : 0;
    return (elapsed >= complete_settle_ns) ? 1 : 0;
}

static int audio_engine_is_complete(const audio_engine_session* s) {
    return s ? s->is_complete_flag : 0;
}

static int audio_engine_has_voice(const audio_engine_session* s) {
    return s ? s->utterance_has_voice : 0;
}

static size_t audio_engine_utterance_bytes(const audio_engine_session* s) {
    return s ? dynbuf_len(&s->utterance) : 0;
}

static int audio_engine_has_pending_audio(const audio_engine_session* s) {
    if (!s) return 0;
    return (dynbuf_len(&s->utterance) > 0) ? 1 : 0;
}

static int audio_engine_should_do_live_partial(const audio_engine_session* s) {
    if (!s) return 0;
    if (audio_engine_is_complete(s) || !s->current_is_speech) {
        return 0;
    }
    return audio_engine_has_pending_audio(s);
}

static uint64_t audio_engine_suggest_monitor_interval_ns(const audio_engine_session* s,
                                                         uint64_t base_ns,
                                                         uint64_t short_ns) {
    if (!s) return base_ns;
    if (audio_engine_should_do_live_partial(s)) {
        return short_ns;
    }
    return base_ns;
}

void audio_engine_record_partial(audio_engine_session* s, uint64_t now_ns, size_t bytes) {
    if (!s) return;
    s->last_partial_ns = now_ns ? now_ns : s->last_chunk_ns;
    s->last_partial_bytes = bytes;
}

int audio_engine_should_do_partial(audio_engine_session* s, uint64_t now_ns, size_t current_bytes, int min_bytes, int min_delta_bytes, uint64_t min_interval_ns) {
    if (!s) return 0;
    if (current_bytes == 0) return 0;
    if (min_bytes > 0 && (int)current_bytes < min_bytes) return 0;
    if (s->last_partial_ns == 0 || s->last_partial_bytes == 0) return 1; // first partial in utterance
    size_t delta = current_bytes > s->last_partial_bytes ? current_bytes - s->last_partial_bytes : 0;
    if (min_delta_bytes > 0 && (int)delta >= min_delta_bytes) return 1;
    uint64_t elapsed = (now_ns > s->last_partial_ns) ? (now_ns - s->last_partial_ns) : 0;
    if (min_interval_ns > 0 && elapsed >= min_interval_ns) return 1;
    return 0;
}

void audio_engine_get_monitor_state(const audio_engine_session* s, audio_monitor_state* out) {
	if (!s || !out) {
		if (out) memset(out, 0, sizeof(*out));
		return;
	}
	out->pending_audio = audio_engine_has_pending_audio(s);
	out->live_partial_eligible = audio_engine_should_do_live_partial(s);
	out->utterance_bytes = audio_engine_utterance_bytes(s);
	out->is_complete = audio_engine_is_complete(s);
	out->has_voice = audio_engine_has_voice(s);
	out->group_deliberating = s->group_deliberating;
}

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
                              audio_monitor_tick* out)
{
    if (!s || !out) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    memset(out, 0, sizeof(*out));
    (void)audio_engine_refresh_group_policy(s, now_ns);

    int pending = audio_engine_has_pending_audio(s);
    int live_eligible = audio_engine_should_do_live_partial(s);
    size_t bytes = audio_engine_utterance_bytes(s);
    int complete = audio_engine_is_complete(s);
    int has_voice = audio_engine_has_voice(s);
    int current_is_speech = s->current_is_speech;
    int reason = 0;
    if (bytes > 0 || has_voice) {
        reason = audio_engine_decide_process_reason(
            s,
            bytes,
            buffer_size,
            max_buffer_size,
            chunk_timeout_s,
            now_ns,
            complete_settle_ns,
            endpointing_enabled,
            min_speech_ns,
            silence_ns,
            max_utt_ns,
            current_is_speech,
            has_voice,
            has_utterance_id || pending || has_voice
        );
    }

    int trigger = audio_engine_should_trigger_live_partial(s, live_partial_enabled);
    uint64_t interval_ns = audio_engine_suggest_monitor_interval_ns(s, base_interval_ns, short_interval_ns);

    out->reason = reason;
    out->live_partial_trigger = trigger;
    out->monitor_interval_ns = interval_ns;

    out->pending_audio = pending;
    out->live_partial_eligible = live_eligible;
    out->utterance_bytes = bytes;
    out->is_complete = complete;
    out->has_voice = has_voice;
    out->group_deliberating = s->group_deliberating;
    return 1;
}
