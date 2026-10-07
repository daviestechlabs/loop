#ifndef PCM_CONVERT_H
#define PCM_CONVERT_H

#include <stdint.h>

/* Canonical STT hop: little-endian S16 bytes to float32 at 1/32768.
 * Caller owns `out`. Empty input succeeds with *out_n = 0.
 * Odd `nbytes` or a bad span returns -1. `in` may be unaligned. */
int pcm_s16le_bytes_to_f32(const void *in, int nbytes, float *out, int out_cap,
                           int *out_n);

/* Post-SNAC hop: clip to [-1, 1], scale by 32767, store S16 (trunc toward 0).
 * Caller owns `out`. n == 0 succeeds. */
int pcm_f32_to_s16_clip(const float *in, int16_t *out, int n);

#endif
