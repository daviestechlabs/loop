#include "base64.h"

#include <stdint.h>
#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <cpuid.h>
#include <immintrin.h>
#define BASE64_HAS_SSSE3 1
#else
#define BASE64_HAS_SSSE3 0
#endif

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_size_v1(size_t input_len) {
    if (input_len > (SIZE_MAX - 2u) / 3u) return 0;
    return ((input_len + 2u) / 3u) * 4u;
}

static size_t base64_encode_scalar(
    const uint8_t *input,
    size_t input_len,
    char *output
) {
    size_t input_pos = 0;
    size_t output_pos = 0;
    size_t remaining;
    while (input_len - input_pos >= 3u) {
        uint32_t value = (uint32_t)input[input_pos] << 16 |
            (uint32_t)input[input_pos + 1u] << 8 |
            (uint32_t)input[input_pos + 2u];
        input_pos += 3u;
        output[output_pos] = base64_table[(value >> 18) & 63u];
        output[output_pos + 1u] = base64_table[(value >> 12) & 63u];
        output[output_pos + 2u] = base64_table[(value >> 6) & 63u];
        output[output_pos + 3u] = base64_table[value & 63u];
        output_pos += 4u;
    }

    remaining = input_len - input_pos;
    if (remaining != 0u) {
        uint32_t value = (uint32_t)input[input_pos] << 16;
        if (remaining == 2u) value |= (uint32_t)input[input_pos + 1u] << 8;
        output[output_pos] = base64_table[(value >> 18) & 63u];
        output[output_pos + 1u] = base64_table[(value >> 12) & 63u];
        output[output_pos + 2u] = remaining == 2u ?
            base64_table[(value >> 6) & 63u] : '=';
        output[output_pos + 3u] = '=';
        output_pos += 4u;
    }
    output[output_pos] = '\0';
    return output_pos;
}

#if BASE64_HAS_SSSE3
static int base64_use_ssse3;
static int base64_use_avx2;

int base64_host_has_ssse3_v1(void) {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    return __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 &&
        (ecx & bit_SSSE3) != 0;
}

int base64_host_has_avx2_v1(void) {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    unsigned int xcr0_lo;
    unsigned int xcr0_hi;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0 ||
        (ecx & ((1u << 27) | (1u << 28))) !=
            ((1u << 27) | (1u << 28)))
        return 0;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6u) != 0x6u ||
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0)
        return 0;
    return (ebx & (1u << 5)) != 0u;
}

__attribute__((target("ssse3")))
static size_t base64_encode_ssse3(
    const uint8_t *input,
    size_t input_len,
    char *output
) {
    const __m128i shuffle = _mm_setr_epi8(
        1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10);
    const __m128i mask_a = _mm_set1_epi32(0x0fc0fc00);
    const __m128i mask_b = _mm_set1_epi32(0x003f03f0);
    const __m128i multiply_a = _mm_set1_epi32(0x04000040);
    const __m128i multiply_b = _mm_set1_epi32(0x01000010);
    const __m128i offset_lut = _mm_setr_epi8(
        65, 71, -4, -4, -4, -4, -4, -4,
        -4, -4, -4, -4, -19, -16, 0, 0);
    const __m128i value_25 = _mm_set1_epi8(25);
    const __m128i value_51 = _mm_set1_epi8(51);
    size_t input_pos = 0;
    size_t output_pos = 0;
    while (input_len - input_pos >= 16u) {
        __m128i packed = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos));
        __m128i sextets;
        __m128i offsets;
        __m128i indexes;
        packed = _mm_shuffle_epi8(packed, shuffle);
        sextets = _mm_or_si128(
            _mm_mulhi_epu16(_mm_and_si128(packed, mask_a), multiply_a),
            _mm_mullo_epi16(_mm_and_si128(packed, mask_b), multiply_b));
        indexes = _mm_subs_epu8(sextets, value_51);
        indexes = _mm_sub_epi8(indexes, _mm_cmpgt_epi8(sextets, value_25));
        offsets = _mm_shuffle_epi8(offset_lut, indexes);
        _mm_storeu_si128(
            (__m128i_u *)(output + output_pos), _mm_add_epi8(sextets, offsets));
        input_pos += 12u;
        output_pos += 16u;
    }
    output_pos += base64_encode_scalar(
        input + input_pos, input_len - input_pos, output + output_pos);
    return output_pos;
}

__attribute__((target("avx2")))
static inline __m256i base64_encode_block_avx2(__m256i packed) {
    const __m256i shuffle = _mm256_setr_epi8(
        1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10,
        1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10);
    const __m256i mask_a = _mm256_set1_epi32(0x0fc0fc00);
    const __m256i mask_b = _mm256_set1_epi32(0x003f03f0);
    const __m256i multiply_a = _mm256_set1_epi32(0x04000040);
    const __m256i multiply_b = _mm256_set1_epi32(0x01000010);
    const __m256i offset_lut = _mm256_setr_epi8(
        65, 71, -4, -4, -4, -4, -4, -4,
        -4, -4, -4, -4, -19, -16, 0, 0,
        65, 71, -4, -4, -4, -4, -4, -4,
        -4, -4, -4, -4, -19, -16, 0, 0);
    const __m256i value_25 = _mm256_set1_epi8(25);
    const __m256i value_51 = _mm256_set1_epi8(51);
    __m256i sextets;
    __m256i offsets;
    __m256i indexes;
    packed = _mm256_shuffle_epi8(packed, shuffle);
    sextets = _mm256_or_si256(
        _mm256_mulhi_epu16(_mm256_and_si256(packed, mask_a), multiply_a),
        _mm256_mullo_epi16(_mm256_and_si256(packed, mask_b), multiply_b));
    indexes = _mm256_subs_epu8(sextets, value_51);
    indexes = _mm256_sub_epi8(
        indexes, _mm256_cmpgt_epi8(sextets, value_25));
    offsets = _mm256_shuffle_epi8(offset_lut, indexes);
    return _mm256_add_epi8(sextets, offsets);
}

__attribute__((target("avx2")))
static size_t base64_encode_avx2(
    const uint8_t *input,
    size_t input_len,
    char *output
) {
    size_t input_pos = 0;
    size_t output_pos = 0;
    while (input_len - input_pos >= 52u) {
        __m128i lower_a = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos));
        __m128i upper_a = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos + 12u));
        __m128i lower_b = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos + 24u));
        __m128i upper_b = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos + 36u));
        __m256i packed_a = _mm256_inserti128_si256(
            _mm256_castsi128_si256(lower_a), upper_a, 1);
        __m256i packed_b = _mm256_inserti128_si256(
            _mm256_castsi128_si256(lower_b), upper_b, 1);
        _mm256_storeu_si256(
            (__m256i_u *)(output + output_pos),
            base64_encode_block_avx2(packed_a));
        _mm256_storeu_si256(
            (__m256i_u *)(output + output_pos + 32u),
            base64_encode_block_avx2(packed_b));
        input_pos += 48u;
        output_pos += 64u;
    }
    while (input_len - input_pos >= 28u) {
        __m128i lower = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos));
        __m128i upper = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos + 12u));
        __m256i packed = _mm256_inserti128_si256(
            _mm256_castsi128_si256(lower), upper, 1);
        _mm256_storeu_si256(
            (__m256i_u *)(output + output_pos),
            base64_encode_block_avx2(packed));
        input_pos += 24u;
        output_pos += 32u;
    }
    if (input_len - input_pos >= 24u) {
        uint64_t upper_word = 0;
        __m128i lower = _mm_loadu_si128(
            (const __m128i_u *)(input + input_pos));
        __m128i upper = _mm_loadl_epi64(
            (const __m128i_u *)(input + input_pos + 12u));
        __m256i packed;
        memcpy(&upper_word, input + input_pos + 20u, 4u);
        upper = _mm_or_si128(
            upper, _mm_slli_si128(_mm_cvtsi64_si128((long long)upper_word), 8));
        packed = _mm256_inserti128_si256(
            _mm256_castsi128_si256(lower), upper, 1);
        _mm256_storeu_si256(
            (__m256i_u *)(output + output_pos),
            base64_encode_block_avx2(packed));
        input_pos += 24u;
        output_pos += 32u;
    }
    _mm256_zeroupper();
    if (input_pos == input_len) {
        output[output_pos] = '\0';
        return output_pos;
    }
    output_pos += base64_encode_ssse3(
        input + input_pos, input_len - input_pos, output + output_pos);
    return output_pos;
}

__attribute__((constructor))
static void base64_select(void) {
    base64_use_ssse3 = base64_host_has_ssse3_v1();
    base64_use_avx2 = base64_use_ssse3 && base64_host_has_avx2_v1();
}
#else
int base64_host_has_ssse3_v1(void) {
    return 0;
}

int base64_host_has_avx2_v1(void) {
    return 0;
}
#endif

size_t base64_encode_scalar_v1(
    const uint8_t *input,
    size_t input_len,
    char *output,
    size_t output_cap
) {
    size_t encoded_len;
    if ((!input && input_len != 0u) || !output || output_cap == 0u) return 0;
    encoded_len = base64_encoded_size_v1(input_len);
    if (encoded_len > SIZE_MAX - 1u || encoded_len + 1u > output_cap) return 0;
    return base64_encode_scalar(input, input_len, output);
}

int base64_using_ssse3_v1(void) {
#if BASE64_HAS_SSSE3
    return base64_use_ssse3;
#else
    return 0;
#endif
}

int base64_using_avx2_v1(void) {
#if BASE64_HAS_SSSE3
    return base64_use_avx2;
#else
    return 0;
#endif
}

size_t base64_encode_v1(
    const uint8_t *input,
    size_t input_len,
    char *output,
    size_t output_cap
) {
    size_t encoded_len;
    if ((!input && input_len != 0u) || !output || output_cap == 0u) return 0;
    encoded_len = base64_encoded_size_v1(input_len);
    if (encoded_len > SIZE_MAX - 1u || encoded_len + 1u > output_cap) return 0;
#if BASE64_HAS_SSSE3
    if (base64_use_avx2 && input_len >= 28u)
        return base64_encode_avx2(input, input_len, output);
    if (base64_use_ssse3 && input_len >= 16u)
        return base64_encode_ssse3(input, input_len, output);
#endif
    return base64_encode_scalar(input, input_len, output);
}
