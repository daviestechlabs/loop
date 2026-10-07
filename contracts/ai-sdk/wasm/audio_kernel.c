#include "audio_kernel.h"
#include <stddef.h>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

#if defined(__wasm__)
#define DTL_EXPORT(name) __attribute__((export_name(name)))
#else
#define DTL_EXPORT(name)
#endif

static float input_samples[DTL_AUDIO_INPUT_MAX];
static float resampled_samples[DTL_AUDIO_OUTPUT_SAMPLES];
static int16_t output_samples[DTL_AUDIO_OUTPUT_SAMPLES];
static float playback_samples[DTL_AUDIO_PCM_BYTES_MAX / 2u];
_Static_assert(sizeof(float) == sizeof(uint32_t), "audio requires binary32 storage");

DTL_EXPORT("input_ptr") uintptr_t input_ptr(void) {
    return (uintptr_t)input_samples;
}

DTL_EXPORT("output_ptr") uintptr_t output_ptr(void) {
    return (uintptr_t)output_samples;
}

DTL_EXPORT("output_samples") uint32_t output_sample_count(void) {
    return DTL_AUDIO_OUTPUT_SAMPLES;
}

static float input_energy(const float *input, uint32_t length) {
#if defined(__wasm_simd128__)
    v128_t sum = wasm_f32x4_splat(0.0f);
    uint32_t index = 0u;
    for (; index + 4u <= length; index += 4u) {
        const v128_t samples = wasm_v128_load(input + index);
        sum = wasm_f32x4_add(sum, wasm_f32x4_mul(samples, samples));
    }
    float energy =
        wasm_f32x4_extract_lane(sum, 0) +
        wasm_f32x4_extract_lane(sum, 1) +
        wasm_f32x4_extract_lane(sum, 2) +
        wasm_f32x4_extract_lane(sum, 3);
#else
    /* Keep the SIMD reduction order in the portable C build. */
    float sums[4] = {0};
    uint32_t index = 0u;
    for (; index + 4u <= length; index += 4u)
        for (uint32_t lane = 0u; lane < 4u; lane++)
            sums[lane] += input[index + lane] * input[index + lane];
    float energy = sums[0] + sums[1] + sums[2] + sums[3];
#endif
    for (; index < length; index++) {
        energy += input[index] * input[index];
    }
    return length > 0 ? energy / (float)length : 0.0f;
}

static void box_resample(const float *input, uint32_t input_length) {
    for (uint32_t output_index = 0; output_index < DTL_AUDIO_OUTPUT_SAMPLES; output_index++) {
        uint32_t first = (output_index * input_length) / DTL_AUDIO_OUTPUT_SAMPLES;
        uint32_t last = ((output_index + 1) * input_length) / DTL_AUDIO_OUTPUT_SAMPLES;
        if (last <= first) {
            last = first + 1;
        }
        if (last > input_length) {
            last = input_length;
        }
        float sum = 0.0f;
        for (uint32_t input_index = first; input_index < last; input_index++) {
            sum += input[input_index];
        }
        resampled_samples[output_index] = sum / (float)(last - first);
    }
}

static void float_to_pcm16(void) {
#if defined(__wasm_simd128__)
    const v128_t minimum = wasm_f32x4_splat(-1.0f);
    const v128_t maximum = wasm_f32x4_splat(1.0f);
    const v128_t scale = wasm_f32x4_splat(32767.0f);
    uint32_t index = 0;
    for (; index + 8 <= DTL_AUDIO_OUTPUT_SAMPLES; index += 8) {
        v128_t lower = wasm_v128_load(resampled_samples + index);
        v128_t upper = wasm_v128_load(resampled_samples + index + 4);
        lower = wasm_f32x4_mul(wasm_f32x4_min(maximum, wasm_f32x4_max(minimum, lower)), scale);
        upper = wasm_f32x4_mul(wasm_f32x4_min(maximum, wasm_f32x4_max(minimum, upper)), scale);
        const v128_t packed = wasm_i16x8_narrow_i32x4(
            wasm_i32x4_trunc_sat_f32x4(lower),
            wasm_i32x4_trunc_sat_f32x4(upper));
        wasm_v128_store(output_samples + index, packed);
    }
#else
    for (uint32_t index = 0u; index < DTL_AUDIO_OUTPUT_SAMPLES; index++) {
        float sample = resampled_samples[index];
        if (sample < -1.0f) sample = -1.0f;
        if (sample > 1.0f) sample = 1.0f;
        output_samples[index] = (int16_t)(sample * 32767.0f);
    }
#endif
}

DTL_EXPORT("process") float process(uint32_t input_length) {
    if (input_length == 0u || input_length > DTL_AUDIO_INPUT_MAX) return -1.0f;
    for (uint32_t index = 0u; index < input_length; index++)
        if (!(input_samples[index] >= -1.0f && input_samples[index] <= 1.0f)) return -1.0f;
    const float energy = input_energy(input_samples, input_length);
    box_resample(input_samples, input_length);
    float_to_pcm16();
    return energy;
}

DTL_EXPORT("playback_ptr") uintptr_t playback_ptr(void) { return (uintptr_t)playback_samples; }

DTL_EXPORT("decode_pcm") uint32_t decode_pcm(uint32_t byte_length) {
    const uint8_t *bytes = (const uint8_t *)input_samples;
    if (byte_length == 0u || byte_length > DTL_AUDIO_PCM_BYTES_MAX || (byte_length & 1u)) return 0u;
    for (uint32_t index = 0u; index < byte_length / 2u; index++) {
        int32_t value = (int32_t)bytes[index * 2u] | ((int32_t)bytes[index * 2u + 1u] << 8u);
        if (value >= 32768) value -= 65536;
        playback_samples[index] = (float)value * (1.0f / 32768.0f);
    }
    return byte_length / 2u;
}

static void erase(void *data, size_t length) {
    volatile uint8_t *bytes = data;
    for (size_t index = 0u; index < length; index++) bytes[index] = 0u;
}

DTL_EXPORT("clear_audio") void clear_audio(void) {
    erase(input_samples, sizeof(input_samples));
    erase(resampled_samples, sizeof(resampled_samples));
    erase(output_samples, sizeof(output_samples));
    erase(playback_samples, sizeof(playback_samples));
}
