// pcm_condition.c
// Pure C implementation of the audio conditioning hot path (high-pass filter,
// noise gate) plus PCM16-to-float conversion.
//
// pcm16_to_f32 stays scalar after A/B showed no material AVX win. The IIR
// high-pass remains scalar because each sample depends on the previous.

#include "pcm_condition.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline int16_t round_scaled_pcm16(double scaled) {
    /* C conversion truncates toward zero, so this matches round-away at ties. */
    return (int16_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

static inline int16_t quantize_pcm16(double sample) {
    const double scaled = sample * 32767.0;
    if (scaled >= 32767.0) return INT16_MAX;
    if (scaled <= -32768.0) return INT16_MIN;
    return round_scaled_pcm16(scaled);
}

static inline int16_t quantize_scaled_pcm16_unit_range(double sample) {
    return round_scaled_pcm16(sample * (32767.0 / 32768.0));
}

static inline int16_t quantize_scaled_pcm16(double sample) {
    const double scaled = sample * (32767.0 / 32768.0);
    if (scaled >= 32767.0) return INT16_MAX;
    if (scaled <= -32768.0) return INT16_MIN;
    return round_scaled_pcm16(scaled);
}

float condition_pcm16(const void* in, int16_t* out, int n,
                      double highpass_alpha,
                      int enable_hp,
                      int enable_ng,
                      double* hp_prev_in,
                      double* hp_prev_out,
                      double* noise_floor,
                      double min_noise_floor,
                      double noise_floor_smoothing,
                      double noise_gate_multiplier,
                      double noise_attenuation)
{
    if (n <= 0 || !in || !out || !hp_prev_in || !hp_prev_out || !noise_floor)
        return 0.0f;
    if (!isfinite(highpass_alpha) || highpass_alpha < 0.0 || highpass_alpha > 1.0 ||
        !isfinite(*hp_prev_in) || !isfinite(*hp_prev_out) ||
        !isfinite(*noise_floor) ||
        !isfinite(min_noise_floor) || min_noise_floor <= 0.0 ||
        !isfinite(noise_floor_smoothing) || noise_floor_smoothing < 0.0 ||
        noise_floor_smoothing > 1.0 ||
        !isfinite(noise_gate_multiplier) || noise_gate_multiplier <= 0.0 ||
        !isfinite(noise_attenuation) || noise_attenuation < 0.0 ||
        noise_attenuation > 1.0) {
        memset(out, 0, (size_t)n * sizeof(*out));
        return 0.0f;
    }

    const uint8_t* input_bytes = (const uint8_t*)in;

    double nf = *noise_floor;
    if (nf <= 0.0) nf = min_noise_floor;

    /* Validation above excludes NaN and nonpositive floor values. Ordered
     * comparisons therefore preserve fmax semantics in this finite domain. */
    const double adaptive_thresh = nf * noise_gate_multiplier;
    const double thresh = adaptive_thresh > min_noise_floor
        ? adaptive_thresh : min_noise_floor;
    const double transition_limit = thresh * 2.0;
    const double inv_in = 1.0 / 32768.0;

    double hp_in = *hp_prev_in;
    double hp_out = *hp_prev_out;

    double sum_sq = 0.0;
    int64_t output_sum_sq = 0;

    /* The product path keeps the recurrence in signed PCM units. Scaling by
     * 2^15 is exact for finite doubles, and removes one multiply per sample.
     * Extreme external state keeps the normalized compatibility path. */
    const double max_scaled_input = DBL_MAX / (32768.0 * 4.0);
    if (enable_hp && enable_ng &&
        fabs(hp_in) <= max_scaled_input &&
        fabs(hp_out) <= max_scaled_input &&
        transition_limit <= max_scaled_input) {
        double scaled_sum_sq = 0.0;
        double scaled_hp_in = hp_in * 32768.0;
        double scaled_hp_out = hp_out * 32768.0;
        const double scaled_thresh = thresh * 32768.0;
        const double scaled_transition_limit = transition_limit * 32768.0;
        /* The macro keeps one source body while forcing a portable two-sample
         * unroll. memcpy preserves unaligned input loads. */
#define CONDITION_PRODUCT_SAMPLE(condition_index) do {                         \
            int16_t sample;                                                    \
            double conditioned;                                               \
            double next;                                                      \
            double magnitude;                                                 \
            int16_t quantized;                                                \
            memcpy(                                                           \
                &sample,                                                       \
                input_bytes + (size_t)(condition_index) * sizeof(sample),      \
                sizeof(sample));                                              \
            conditioned = (double)sample;                                     \
            next = conditioned - scaled_hp_in +                               \
                highpass_alpha * scaled_hp_out;                               \
            scaled_hp_in = conditioned;                                       \
            scaled_hp_out = next;                                             \
            conditioned = next;                                               \
            magnitude = fabs(conditioned);                                    \
            if (magnitude < scaled_transition_limit) {                         \
                if (magnitude < scaled_thresh) {                               \
                    conditioned *= noise_attenuation;                          \
                } else {                                                       \
                    double blend =                                            \
                        (magnitude - scaled_thresh) / scaled_thresh;            \
                    double attenuation = noise_attenuation +                   \
                        (1.0 - noise_attenuation) * blend;                     \
                    conditioned *= attenuation;                               \
                }                                                              \
            }                                                                  \
            scaled_sum_sq += conditioned * conditioned;                       \
            /* Gate attenuation cannot increase the pre-gate magnitude. */    \
            quantized = magnitude <= 32768.0                                  \
                ? quantize_scaled_pcm16_unit_range(conditioned)                \
                : quantize_scaled_pcm16(conditioned);                          \
            out[condition_index] = quantized;                                  \
            output_sum_sq += (int64_t)quantized * (int64_t)quantized;          \
        } while (0)
        int i = 0;
        /* The recurrence stays ordered. Two samples share one loop branch. */
        for (; i + 1 < n; i += 2) {
            CONDITION_PRODUCT_SAMPLE(i);
            CONDITION_PRODUCT_SAMPLE(i + 1);
        }
        if (i < n) CONDITION_PRODUCT_SAMPLE(i);
#undef CONDITION_PRODUCT_SAMPLE
        sum_sq = scaled_sum_sq * (1.0 / (32768.0 * 32768.0));
        hp_in = scaled_hp_in * inv_in;
        hp_out = scaled_hp_out * inv_in;
    } else {
        for (int i = 0; i < n; i++) {
            int16_t sample;
            memcpy(
                &sample,
                input_bytes + (size_t)i * sizeof(sample),
                sizeof(sample));
            double conditioned = (double)sample * inv_in;

            if (enable_hp) {
                double next = conditioned - hp_in + highpass_alpha * hp_out;
                hp_in = conditioned;
                hp_out = next;
                conditioned = next;
            }

            if (enable_ng) {
                double magnitude = fabs(conditioned);
                if (magnitude < transition_limit) {
                    if (magnitude < thresh) {
                        conditioned *= noise_attenuation;
                    } else {
                        double blend = (magnitude - thresh) / thresh;
                        double attenuation =
                            noise_attenuation +
                            (1.0 - noise_attenuation) * blend;
                        conditioned *= attenuation;
                    }
                }
            }

            sum_sq += conditioned * conditioned;
            const int16_t quantized = quantize_pcm16(conditioned);
            out[i] = quantized;
            output_sum_sq += (int64_t)quantized * (int64_t)quantized;
        }
    }

    // Noise-floor adaptation deliberately retains the pre-quantization energy.
    double loop_rms = sqrt(sum_sq / (double)n);
    double effective_energy = loop_rms;
    bool speech_likely = effective_energy >= thresh;
    if (!speech_likely) {
        const double bounded_energy = effective_energy > min_noise_floor
            ? effective_energy : min_noise_floor;
        nf = (1.0 - noise_floor_smoothing) * nf +
            noise_floor_smoothing * bounded_energy;
    } else {
        const double decayed_floor =
            nf * (1.0 - noise_floor_smoothing * 0.1);
        nf = decayed_floor > min_noise_floor
            ? decayed_floor : min_noise_floor;
    }
    *noise_floor = nf;

    *hp_prev_in = hp_in;
    *hp_prev_out = hp_out;

    // Exact RMS from the canonical quantized PCM energy.
    return (float)(sqrt((double)output_sum_sq / (double)n) * inv_in);
}

void pcm16_to_f32_scalar(const int16_t* in, float* out, int n) {
    if (n <= 0 || !in || !out) return;
    const float inv = 1.0f / 32768.0f;
    for (int i = 0; i < n; i++) {
        out[i] = (float)in[i] * inv;
    }
}

void pcm16_to_f32(const int16_t* in, float* out, int n) {
    /* Scalar default: paired A/B showed AVX2 convert ~0.75–1.2× (no win). */
    pcm16_to_f32_scalar(in, out, n);
}
