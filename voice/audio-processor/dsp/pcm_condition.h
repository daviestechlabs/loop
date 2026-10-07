#ifndef PCM_CONDITION_H
#define PCM_CONDITION_H

#include <stdint.h>

// Pure C audio conditioning for the hot path (high-pass + noise gate).
// The caller provides the input and output spans. The kernel does not allocate.
//
// State (hp_prev_in/out, noise_floor) is passed by pointer. The returned RMS is
// measured from the exact quantized PCM written to out, so VAD does not rescan
// the frame after conditioning.
//
// The same implementation is reused by the embedded gateway reflex session.

/* `in` is a byte-addressable PCM16 span and may be unaligned (for example,
 * when it points into a protobuf frame). `out` must be int16_t-aligned. */
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
                      double noise_attenuation);

// Legacy float32 STT rollback conversion. Canonical STT accepts S16LE.
// Span-shaped: converts n samples. Product entry stays scalar (A/B: no win).
void pcm16_to_f32(const int16_t* in, float* out, int n);

// Scalar / AVX2 entry points exposed for A/B measurement only.
void pcm16_to_f32_scalar(const int16_t* in, float* out, int n);
#if defined(__x86_64__) || defined(__i386__)
void pcm16_to_f32_avx2(const int16_t* in, float* out, int n);
#endif

// Exact PCM16 RMS over a sample span. Caller-buffered; no allocation.
// pcm16_rms picks AVX2 at process start when CPUID+XGETBV report usable AVX2.
// It otherwise uses the scalar implementation.
// Double precision preserves the integer sum-of-squares reference result.
double pcm16_rms(const int16_t* in, int n);
double pcm16_rms_scalar(const int16_t* in, int n);
#if defined(__x86_64__) || defined(__i386__)
double pcm16_rms_avx2(const int16_t* in, int n);
#endif

/* 1 if this CPU can run the AVX2 RMS kernel (OSXSAVE+AVX+YMM+AVX2). */
int pcm16_host_has_avx2(void);
/* 1 if pcm16_rms selected the AVX2 kernel at process init. */
int pcm16_rms_using_avx2(void);
#endif
