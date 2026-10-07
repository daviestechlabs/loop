/* pcm_condition_avx2.c — AVX2 PCM span kernels. Compiled with -mavx2 only.
 * The RMS body runs only when pcm_rms.c's CPU probe selects it. */

#include "pcm_condition.h"

#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

void pcm16_to_f32_avx2(const int16_t *in, float *out, int n) {
    if (n <= 0 || !in || !out)
        return;
    const __m256 scale = _mm256_set1_ps(1.0f / 32768.0f);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i v16;
        memcpy(&v16, in + i, sizeof(v16));
        __m256i v32 = _mm256_cvtepi16_epi32(v16);
        __m256 vf = _mm256_cvtepi32_ps(v32);
        _mm256_storeu_ps(out + i, _mm256_mul_ps(vf, scale));
    }
    const float inv = 1.0f / 32768.0f;
    for (; i < n; i++)
        out[i] = (float)in[i] * inv;
}

/* Accumulate eight int16 samples into a 4×int64 sum-of-squares vector.
 * Widens to int32 before multiply so INT16_MIN^2 never overflows signed
 * int32 the way _mm256_madd_epi16 (pair-add of two 16×16 products) does. */
static inline __m256i pcm16_sqsum8_avx2(const int16_t *samples) {
    __m128i v16;
    memcpy(&v16, samples, sizeof(v16));
    const __m256i v32 = _mm256_cvtepi16_epi32(v16);
    const __m256i even_sq = _mm256_mul_epi32(v32, v32);
    const __m256i swapped = _mm256_shuffle_epi32(v32, _MM_SHUFFLE(2, 3, 0, 1));
    const __m256i odd_sq = _mm256_mul_epi32(swapped, swapped);
    return _mm256_add_epi64(even_sq, odd_sq);
}

double pcm16_rms_avx2(const int16_t *in, int n) {
    if (n <= 0 || !in)
        return 0.0;
    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        acc0 = _mm256_add_epi64(acc0, pcm16_sqsum8_avx2(in + i));
        acc1 = _mm256_add_epi64(acc1, pcm16_sqsum8_avx2(in + i + 8));
        acc0 = _mm256_add_epi64(acc0, pcm16_sqsum8_avx2(in + i + 16));
        acc1 = _mm256_add_epi64(acc1, pcm16_sqsum8_avx2(in + i + 24));
    }
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm256_add_epi64(acc0, pcm16_sqsum8_avx2(in + i));
        acc1 = _mm256_add_epi64(acc1, pcm16_sqsum8_avx2(in + i + 8));
    }
    if (i + 8 <= n) {
        acc0 = _mm256_add_epi64(acc0, pcm16_sqsum8_avx2(in + i));
        i += 8;
    }
    acc0 = _mm256_add_epi64(acc0, acc1);
    int64_t sum_sq = 0;
    {
        int64_t tmp[4];
        memcpy(tmp, &acc0, sizeof(tmp));
        sum_sq = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    }
    for (; i < n; i++) {
        int32_t v = (int32_t)in[i];
        sum_sq += (int64_t)v * (int64_t)v;
    }
    return sqrt((double)sum_sq / (double)n) / 32768.0;
}
