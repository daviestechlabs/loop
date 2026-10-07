#include "pcm_convert.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int pcm_s16le_bytes_to_f32(const void *in, int nbytes, float *out, int out_cap,
                           int *out_n)
{
    if (!out_n)
        return -1;
    if (nbytes == 0) {
        *out_n = 0;
        return 0;
    }
    if (nbytes < 0 || (nbytes & 1) != 0 || !in || !out || out_cap < 0)
        return -1;
    int n = nbytes / 2;
    if (n > out_cap)
        return -1;

    const uint8_t *bytes = (const uint8_t *)in;
    const float inv = 1.0f / 32768.0f;
    for (int i = 0; i < n; i++) {
        int16_t sample;
        memcpy(&sample, bytes + (size_t)i * sizeof(sample), sizeof(sample));
        out[i] = (float)sample * inv;
    }
    *out_n = n;
    return 0;
}

int pcm_f32_to_s16_clip(const float *in, int16_t *out, int n)
{
    if (n < 0)
        return -1;
    if (n == 0)
        return 0;
    if (!in || !out)
        return -1;
    for (int i = 0; i < n; i++) {
        float s = in[i];
        if (s > 1.0f)
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
        else if (!isfinite(s))
            s = 0.0f;
        out[i] = (int16_t)(s * 32767.0f);
    }
    return 0;
}
