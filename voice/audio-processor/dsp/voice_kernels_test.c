#include "embedding_post.h"
#include "pcm_convert.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_s16le_empty(void)
{
    float out[1];
    int out_n = -1;
    out[0] = 99.0f;
    assert(pcm_s16le_bytes_to_f32(NULL, 0, out, 1, &out_n) == 0);
    assert(out_n == 0);
    assert(out[0] == 99.0f);
}

static void test_s16le_odd_length(void)
{
    const unsigned char odd[] = {0x00};
    float out[4];
    int out_n = 7;
    memset(out, 0x5a, sizeof(out));
    assert(pcm_s16le_bytes_to_f32(odd, 1, out, 4, &out_n) == -1);
    assert(pcm_s16le_bytes_to_f32(odd, 3, out, 4, &out_n) == -1);
}

static void test_s16le_scale(void)
{
    int16_t samples[] = {0, 32767, -32768, 16384};
    float out[4];
    int out_n = 0;
    assert(pcm_s16le_bytes_to_f32(samples, (int)sizeof(samples), out, 4, &out_n) == 0);
    assert(out_n == 4);
    assert(out[0] == 0.0f);
    assert(out[1] == (float)32767 * (1.0f / 32768.0f));
    assert(out[2] == -1.0f);
    assert(out[3] == 0.5f);
}

static void test_s16le_unaligned(void)
{
    int16_t samples[] = {0, 32767, -32768, 16384};
    unsigned char storage[sizeof(samples) + 2u];
    float aligned[4];
    float unaligned[4];
    int aligned_n = 0;
    int unaligned_n = 0;
    unsigned char* in = storage;
    if (((uintptr_t)in % sizeof(int16_t)) == 0)
        in += 1;
    memcpy(in, samples, sizeof(samples));
    assert(((uintptr_t)in % sizeof(int16_t)) != 0);
    assert(pcm_s16le_bytes_to_f32(
               samples, (int)sizeof(samples), aligned, 4, &aligned_n)
           == 0);
    assert(pcm_s16le_bytes_to_f32(
               in, (int)sizeof(samples), unaligned, 4, &unaligned_n)
           == 0);
    assert(aligned_n == 4);
    assert(unaligned_n == 4);
    assert(memcmp(aligned, unaligned, sizeof(aligned)) == 0);
}

static void test_s16le_cap_and_null(void)
{
    int16_t samples[] = {1, 2};
    float out[1];
    int out_n = 0;
    assert(pcm_s16le_bytes_to_f32(samples, 4, out, 1, &out_n) == -1);
    assert(pcm_s16le_bytes_to_f32(samples, 4, out, 2, NULL) == -1);
    assert(pcm_s16le_bytes_to_f32(NULL, 4, out, 2, &out_n) == -1);
    assert(pcm_s16le_bytes_to_f32(samples, -2, out, 2, &out_n) == -1);
}

static void test_f32_to_s16_empty(void)
{
    int16_t out[1];
    out[0] = 9;
    assert(pcm_f32_to_s16_clip(NULL, out, 0) == 0);
    assert(out[0] == 9);
}

static void test_f32_to_s16_clip(void)
{
    const float in[] = {
        0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f, INFINITY, -INFINITY, NAN
    };
    int16_t out[9];
    assert(pcm_f32_to_s16_clip(in, out, 9) == 0);
    assert(out[0] == 0);
    assert(out[1] == 32767);
    assert(out[2] == -32767);
    assert(out[3] == 32767);
    assert(out[4] == -32767);
    assert(out[5] == (int16_t)(0.5f * 32767.0f));
    assert(out[6] == 32767);
    assert(out[7] == -32767);
    assert(out[8] == 0);
}

static void test_f32_to_s16_null(void)
{
    float in[] = {0.0f};
    int16_t out[1];
    assert(pcm_f32_to_s16_clip(NULL, out, 1) == -1);
    assert(pcm_f32_to_s16_clip(in, NULL, 1) == -1);
    assert(pcm_f32_to_s16_clip(in, out, -1) == -1);
}

static void test_cls_l2_contract(void)
{
    /* (3,4) L2 is (0.6, 0.8). Second token is ignored. */
    const float hidden[] = {3.0f, 4.0f, -3.0f, 4.0f};
    float out[2];
    assert(embedding_cls_pool(hidden, 1, 2, 2, 1, out) == 0);
    assert(fabsf(out[0] - 0.6f) < 1e-6f);
    assert(fabsf(out[1] - 0.8f) < 1e-6f);
}

static void test_cls_without_normalize(void)
{
    const float hidden[] = {3.0f, 4.0f, 9.0f, 9.0f};
    float out[2];
    assert(embedding_cls_pool(hidden, 1, 2, 2, 0, out) == 0);
    assert(out[0] == 3.0f);
    assert(out[1] == 4.0f);
}

static void test_cls_zero_vector(void)
{
    const float hidden[] = {0.0f, 0.0f};
    float out[2];
    assert(embedding_cls_pool(hidden, 1, 1, 2, 1, out) == -1);
}

static void test_cls_nonfinite(void)
{
    float nan_row[] = {NAN, 1.0f};
    float inf_row[] = {INFINITY, 1.0f};
    float out[2];
    assert(embedding_cls_pool(nan_row, 1, 1, 2, 1, out) == -1);
    assert(embedding_cls_pool(inf_row, 1, 1, 2, 1, out) == -1);
}

static void test_cls_bad_shape(void)
{
    const float hidden[] = {1.0f, 2.0f};
    float out[2];
    assert(embedding_cls_pool(hidden, 0, 1, 2, 1, out) == -1);
    assert(embedding_cls_pool(hidden, 1, 0, 2, 1, out) == -1);
    assert(embedding_cls_pool(hidden, 1, 1, 0, 1, out) == -1);
    assert(embedding_cls_pool(NULL, 1, 1, 2, 1, out) == -1);
    assert(embedding_cls_pool(hidden, 1, 1, 2, 1, NULL) == -1);
}

static void test_cls_tiny_norm_floor(void)
{
    const float hidden[] = {1e-20f, 0.0f};
    float out[2];
    assert(embedding_cls_pool(hidden, 1, 1, 2, 1, out) == 0);
}

int main(void)
{
    test_s16le_empty();
    test_s16le_odd_length();
    test_s16le_scale();
    test_s16le_unaligned();
    test_s16le_cap_and_null();
    puts("PASS S16LE empty/odd/scale=1/32768/unaligned");
    test_f32_to_s16_empty();
    test_f32_to_s16_clip();
    test_f32_to_s16_null();
    puts("PASS f32->S16 clip*32767 empty/clip/nonfinite");
    test_cls_l2_contract();
    test_cls_without_normalize();
    test_cls_zero_vector();
    test_cls_nonfinite();
    test_cls_bad_shape();
    test_cls_tiny_norm_floor();
    puts("PASS CLS+L2 (3,4)->(0.6,0.8) zero/nonfinite/tiny");
    return 0;
}
