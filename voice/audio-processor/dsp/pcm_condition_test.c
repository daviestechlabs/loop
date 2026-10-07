#include "pcm_condition.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int16_t reference_quantize(double sample) {
    const double rounded = round(sample * 32767.0);
    if (rounded >= (double)INT16_MAX) return INT16_MAX;
    if (rounded <= (double)INT16_MIN) return INT16_MIN;
    return (int16_t)rounded;
}

static float reference_condition_pcm16(
    const void *input,
    int16_t *output,
    int sample_count,
    double highpass_alpha,
    int enable_highpass,
    int enable_noise_gate,
    double *highpass_previous_input,
    double *highpass_previous_output,
    double *noise_floor,
    double minimum_noise_floor,
    double noise_floor_smoothing,
    double noise_gate_multiplier,
    double noise_attenuation
) {
    const uint8_t *input_bytes = input;
    double floor = *noise_floor;
    double previous_input = *highpass_previous_input;
    double previous_output = *highpass_previous_output;
    double energy_sum = 0.0;
    int64_t output_energy_sum = 0;
    int index;
    if (floor <= 0.0) floor = minimum_noise_floor;
    const double threshold =
        fmax(minimum_noise_floor, floor * noise_gate_multiplier);
    const double inverse_input = 1.0 / 32768.0;

    for (index = 0; index < sample_count; ++index) {
        int16_t sample;
        double conditioned;
        memcpy(
            &sample,
            input_bytes + (size_t)index * sizeof(sample),
            sizeof(sample));
        conditioned = (double)sample * inverse_input;
        if (enable_highpass) {
            double next =
                conditioned - previous_input +
                highpass_alpha * previous_output;
            previous_input = conditioned;
            previous_output = next;
            conditioned = next;
        }
        if (enable_noise_gate) {
            double magnitude = fabs(conditioned);
            if (magnitude < threshold) {
                conditioned *= noise_attenuation;
            } else if (magnitude < threshold * 2.0) {
                double blend = (magnitude - threshold) / threshold;
                double attenuation =
                    noise_attenuation +
                    (1.0 - noise_attenuation) * blend;
                conditioned *= attenuation;
            }
        }
        energy_sum += conditioned * conditioned;
        output[index] = reference_quantize(conditioned);
        output_energy_sum +=
            (int64_t)output[index] * (int64_t)output[index];
    }

    {
        double energy = sqrt(energy_sum / (double)sample_count);
        bool speech_likely = energy >= fmax(
            minimum_noise_floor, floor * noise_gate_multiplier);
        if (!speech_likely) {
            floor =
                (1.0 - noise_floor_smoothing) * floor +
                noise_floor_smoothing * fmax(energy, minimum_noise_floor);
        } else {
            floor = fmax(
                minimum_noise_floor,
                floor * (1.0 - noise_floor_smoothing * 0.1));
        }
    }
    *noise_floor = floor;
    *highpass_previous_input = previous_input;
    *highpass_previous_output = previous_output;
    return (float)(
        sqrt((double)output_energy_sum / (double)sample_count) * inverse_input);
}

static void condition_differential(void) {
    static const int lengths[] = {1, 2, 3, 31, 32, 127, 319, 320};
    uint8_t input_storage[sizeof(int16_t) * 320u + 1u];
    int16_t product_output[320];
    int16_t reference_output[320];
    uint32_t random_state = UINT32_C(0x6d2b79f5);
    unsigned mode;
    unsigned batch;

    for (mode = 0; mode < 4u; ++mode) {
        double product_previous_input = 0.125;
        double product_previous_output = -0.25;
        double product_noise_floor = 0.0025;
        double reference_previous_input = product_previous_input;
        double reference_previous_output = product_previous_output;
        double reference_noise_floor = product_noise_floor;
        for (batch = 0; batch < sizeof(lengths) / sizeof(lengths[0]); ++batch) {
            int sample_count = lengths[batch];
            int index;
            float product_energy;
            float reference_energy;
            for (index = 0; index < sample_count; ++index) {
                int16_t sample;
                random_state =
                    random_state * UINT32_C(1664525) + UINT32_C(1013904223);
                sample = (int16_t)(random_state >> 16);
                if (index == 0) sample = INT16_MIN;
                if (index == sample_count - 1) sample = INT16_MAX;
                memcpy(
                    input_storage + 1u + (size_t)index * sizeof(sample),
                    &sample,
                    sizeof(sample));
            }
            product_energy = condition_pcm16(
                input_storage + 1u,
                product_output,
                sample_count,
                0.985,
                (mode & 1u) != 0u,
                (mode & 2u) != 0u,
                &product_previous_input,
                &product_previous_output,
                &product_noise_floor,
                0.0025,
                0.2,
                1.75,
                0.12);
            reference_energy = reference_condition_pcm16(
                input_storage + 1u,
                reference_output,
                sample_count,
                0.985,
                (mode & 1u) != 0u,
                (mode & 2u) != 0u,
                &reference_previous_input,
                &reference_previous_output,
                &reference_noise_floor,
                0.0025,
                0.2,
                1.75,
                0.12);
            assert(memcmp(
                product_output,
                reference_output,
                (size_t)sample_count * sizeof(product_output[0])) == 0);
            assert(memcmp(
                &product_energy,
                &reference_energy,
                sizeof(product_energy)) == 0);
            assert(memcmp(
                &product_previous_input,
                &reference_previous_input,
                sizeof(product_previous_input)) == 0);
            assert(memcmp(
                &product_previous_output,
                &reference_previous_output,
                sizeof(product_previous_output)) == 0);
            assert(memcmp(
                &product_noise_floor,
                &reference_noise_floor,
                sizeof(product_noise_floor)) == 0);
        }
    }
}

static void condition_noise_gate_boundaries(void) {
    static const int16_t input[] = {
        -2049, -2048, -2047, -1025, -1024, -1023,
        0,
        1023, 1024, 1025, 2047, 2048, 2049
    };
    int16_t product_output[sizeof(input) / sizeof(input[0])];
    int16_t reference_output[sizeof(input) / sizeof(input[0])];
    double product_previous_input = 0.0;
    double product_previous_output = 0.0;
    double product_noise_floor = 1024.0 / 32768.0;
    double reference_previous_input = product_previous_input;
    double reference_previous_output = product_previous_output;
    double reference_noise_floor = product_noise_floor;
    float product_energy;
    float reference_energy;

    product_energy = condition_pcm16(
        input,
        product_output,
        (int)(sizeof(input) / sizeof(input[0])),
        0.985,
        0,
        1,
        &product_previous_input,
        &product_previous_output,
        &product_noise_floor,
        1024.0 / 32768.0,
        0.2,
        1.0,
        0.25);
    reference_energy = reference_condition_pcm16(
        input,
        reference_output,
        (int)(sizeof(input) / sizeof(input[0])),
        0.985,
        0,
        1,
        &reference_previous_input,
        &reference_previous_output,
        &reference_noise_floor,
        1024.0 / 32768.0,
        0.2,
        1.0,
        0.25);

    assert(memcmp(product_output, reference_output, sizeof(product_output)) == 0);
    assert(memcmp(&product_energy, &reference_energy, sizeof(product_energy)) == 0);
    assert(memcmp(
        &product_previous_input,
        &reference_previous_input,
        sizeof(product_previous_input)) == 0);
    assert(memcmp(
        &product_previous_output,
        &reference_previous_output,
        sizeof(product_previous_output)) == 0);
    assert(memcmp(
        &product_noise_floor,
        &reference_noise_floor,
        sizeof(product_noise_floor)) == 0);
}

static void condition_extreme_state_fallback(void) {
    static const int16_t input[] = {0, INT16_MAX};
    int16_t product_output[2];
    int16_t reference_output[2];
    double product_previous_input = DBL_MAX / 2.0;
    double product_previous_output = -DBL_MAX / 4.0;
    double product_noise_floor = 0.0025;
    double reference_previous_input = product_previous_input;
    double reference_previous_output = product_previous_output;
    double reference_noise_floor = product_noise_floor;
    float product_energy;
    float reference_energy;

    product_energy = condition_pcm16(
        input, product_output, 2, 0.5, 1, 1,
        &product_previous_input, &product_previous_output,
        &product_noise_floor, 0.0025, 0.2, 1.75, 0.12);
    reference_energy = reference_condition_pcm16(
        input, reference_output, 2, 0.5, 1, 1,
        &reference_previous_input, &reference_previous_output,
        &reference_noise_floor, 0.0025, 0.2, 1.75, 0.12);

    assert(memcmp(product_output, reference_output, sizeof(product_output)) == 0);
    assert(memcmp(&product_energy, &reference_energy, sizeof(product_energy)) == 0);
    assert(memcmp(
        &product_previous_input,
        &reference_previous_input,
        sizeof(product_previous_input)) == 0);
    assert(memcmp(
        &product_previous_output,
        &reference_previous_output,
        sizeof(product_previous_output)) == 0);
    assert(memcmp(
        &product_noise_floor,
        &reference_noise_floor,
        sizeof(product_noise_floor)) == 0);

    product_previous_input = 0.0;
    product_previous_output = 0.0;
    product_noise_floor = DBL_MAX / 2.0;
    reference_previous_input = product_previous_input;
    reference_previous_output = product_previous_output;
    reference_noise_floor = product_noise_floor;
    product_energy = condition_pcm16(
        input, product_output, 2, 0.985, 1, 1,
        &product_previous_input, &product_previous_output,
        &product_noise_floor, 0.0025, 0.2, 1.75, 0.12);
    reference_energy = reference_condition_pcm16(
        input, reference_output, 2, 0.985, 1, 1,
        &reference_previous_input, &reference_previous_output,
        &reference_noise_floor, 0.0025, 0.2, 1.75, 0.12);

    assert(memcmp(product_output, reference_output, sizeof(product_output)) == 0);
    assert(memcmp(&product_energy, &reference_energy, sizeof(product_energy)) == 0);
    assert(memcmp(
        &product_previous_input,
        &reference_previous_input,
        sizeof(product_previous_input)) == 0);
    assert(memcmp(
        &product_previous_output,
        &reference_previous_output,
        sizeof(product_previous_output)) == 0);
    assert(memcmp(
        &product_noise_floor,
        &reference_noise_floor,
        sizeof(product_noise_floor)) == 0);
}

int main(void) {
    int16_t input[320];
    int16_t output[320];
    int16_t reference_output[320];
    double hp_prev_in = 0.0;
    double hp_prev_out = 0.0;
    double noise_floor = 0.0025;
    double combined_product_previous_input = 0.125;
    double combined_product_previous_output = -0.25;
    double combined_product_noise_floor = 0.0025;
    double combined_reference_previous_input =
        combined_product_previous_input;
    double combined_reference_previous_output =
        combined_product_previous_output;
    double combined_reference_noise_floor = combined_product_noise_floor;

    condition_differential();
    condition_noise_gate_boundaries();
    condition_extreme_state_fallback();

    for (size_t index = 0; index < 320u; ++index) {
        input[index] = (int16_t)((int)index * 193 - 30000);
    }
    input[0] = INT16_MIN;
    input[319] = INT16_MAX;

    const float energy = condition_pcm16(
        input,
        output,
        320,
        0.985,
        1,
        1,
        &hp_prev_in,
        &hp_prev_out,
        &noise_floor,
        0.0025,
        0.2,
        1.75,
        0.12);
    const float scanned_energy = (float)pcm16_rms_scalar(output, 320);

    assert(isfinite(energy));
    assert(energy == scanned_energy);

    for (int start = INT16_MIN; start <= INT16_MAX; start += 320) {
        int count = INT16_MAX - start + 1;
        if (count > 320) count = 320;
        for (int index = 0; index < count; ++index)
            input[index] = (int16_t)(start + index);
        hp_prev_in = 0.0;
        hp_prev_out = 0.0;
        noise_floor = 0.0025;
        (void)condition_pcm16(
            input, output, count, 0.985, 0, 0,
            &hp_prev_in, &hp_prev_out, &noise_floor,
            0.0025, 0.2, 1.75, 0.12);
        for (int index = 0; index < count; ++index) {
            const double normalized = (double)input[index] / 32768.0;
            assert(output[index] == reference_quantize(normalized));
        }
        {
            double product_previous_input = 0.0;
            double product_previous_output = 0.0;
            double product_noise_floor = 1024.0 / 32768.0;
            double reference_previous_input = 0.0;
            double reference_previous_output = 0.0;
            double reference_noise_floor = product_noise_floor;
            float product_energy = condition_pcm16(
                input, output, count, 0.985, 0, 1,
                &product_previous_input,
                &product_previous_output,
                &product_noise_floor,
                1024.0 / 32768.0, 0.2, 1.0, 0.25);
            float reference_energy = reference_condition_pcm16(
                input, reference_output, count, 0.985, 0, 1,
                &reference_previous_input,
                &reference_previous_output,
                &reference_noise_floor,
                1024.0 / 32768.0, 0.2, 1.0, 0.25);
            assert(memcmp(
                output,
                reference_output,
                (size_t)count * sizeof(output[0])) == 0);
            assert(memcmp(
                &product_energy,
                &reference_energy,
                sizeof(product_energy)) == 0);
            assert(memcmp(
                &product_noise_floor,
                &reference_noise_floor,
                sizeof(product_noise_floor)) == 0);
        }
        {
            float product_energy = condition_pcm16(
                input, output, count, 0.985, 1, 1,
                &combined_product_previous_input,
                &combined_product_previous_output,
                &combined_product_noise_floor,
                0.0025, 0.2, 1.75, 0.12);
            float reference_energy = reference_condition_pcm16(
                input, reference_output, count, 0.985, 1, 1,
                &combined_reference_previous_input,
                &combined_reference_previous_output,
                &combined_reference_noise_floor,
                0.0025, 0.2, 1.75, 0.12);
            assert(memcmp(
                output,
                reference_output,
                (size_t)count * sizeof(output[0])) == 0);
            assert(memcmp(
                &product_energy,
                &reference_energy,
                sizeof(product_energy)) == 0);
            assert(memcmp(
                &combined_product_previous_input,
                &combined_reference_previous_input,
                sizeof(combined_product_previous_input)) == 0);
            assert(memcmp(
                &combined_product_previous_output,
                &combined_reference_previous_output,
                sizeof(combined_product_previous_output)) == 0);
            assert(memcmp(
                &combined_product_noise_floor,
                &combined_reference_noise_floor,
                sizeof(combined_product_noise_floor)) == 0);
        }
    }

    {
        static const double conditioned[] = {
            -2.0, -1.0001, -1.0, -1000.5 / 32767.0, -0.5 / 32767.0,
            0.0, 0.5 / 32767.0, 1000.5 / 32767.0, 1.0, 1.0001, 2.0
        };
        input[0] = 0;
        for (size_t index = 0; index < sizeof(conditioned) / sizeof(conditioned[0]);
             ++index) {
            hp_prev_in = -conditioned[index];
            hp_prev_out = 0.0;
            noise_floor = 0.0025;
            (void)condition_pcm16(
                input, output, 1, 0.0, 1, 0,
                &hp_prev_in, &hp_prev_out, &noise_floor,
                0.0025, 0.2, 1.75, 0.12);
            assert(output[0] == reference_quantize(conditioned[index]));
            {
                double product_previous_input = -conditioned[index];
                double product_previous_output = 0.0;
                double product_noise_floor = 1e-12;
                double reference_previous_input = product_previous_input;
                double reference_previous_output = product_previous_output;
                double reference_noise_floor = product_noise_floor;
                float product_energy = condition_pcm16(
                    input, output, 1, 0.0, 1, 1,
                    &product_previous_input,
                    &product_previous_output,
                    &product_noise_floor,
                    1e-12, 0.2, 1.0, 0.25);
                float reference_energy = reference_condition_pcm16(
                    input, reference_output, 1, 0.0, 1, 1,
                    &reference_previous_input,
                    &reference_previous_output,
                    &reference_noise_floor,
                    1e-12, 0.2, 1.0, 0.25);
                assert(output[0] == reference_output[0]);
                assert(memcmp(
                    &product_energy,
                    &reference_energy,
                    sizeof(product_energy)) == 0);
                assert(memcmp(
                    &product_previous_output,
                    &reference_previous_output,
                    sizeof(product_previous_output)) == 0);
                assert(memcmp(
                    &product_noise_floor,
                    &reference_noise_floor,
                    sizeof(product_noise_floor)) == 0);
            }
        }
    }

    output[0] = 1;
    assert(condition_pcm16(
               input,
               output,
               320,
               NAN,
               1,
               1,
               &hp_prev_in,
               &hp_prev_out,
               &noise_floor,
               0.0025,
               0.2,
               1.75,
               0.12) == 0.0f);
    assert(output[0] == 0);
    return 0;
}
