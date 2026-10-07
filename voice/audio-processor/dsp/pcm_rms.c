/* Exact PCM16 RMS and process-start CPU dispatch.
 *
 * Keep this code separate from the production conditioner. Binaries that do
 * not scan PCM for a second time can then omit the constructor and AVX2 body.
 */

#include "pcm_condition.h"

#include <math.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

double pcm16_rms_scalar(const int16_t *in, int n) {
    if (n <= 0 || !in) return 0.0;
    int64_t sum_sq = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)in[i];
        sum_sq += (int64_t)v * (int64_t)v;
    }
    return sqrt((double)sum_sq / (double)n) / 32768.0;
}

int pcm16_host_has_avx2(void) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    unsigned int xcr0_lo, xcr0_hi;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    /* OSXSAVE (27) + AVX (28) required before XGETBV / YMM use. */
    if ((ecx & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28)))
        return 0;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6u) != 0x6u)
        return 0;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return 0;
    return (ebx & (1u << 5)) != 0; /* AVX2 */
#else
    return 0;
#endif
}

typedef double (*pcm16_rms_fn)(const int16_t *, int);

static pcm16_rms_fn pcm16_rms_impl;
static int pcm16_rms_avx2_selected;

static void pcm16_rms_select(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (pcm16_host_has_avx2()) {
        pcm16_rms_impl = pcm16_rms_avx2;
        pcm16_rms_avx2_selected = 1;
        return;
    }
#endif
    pcm16_rms_impl = pcm16_rms_scalar;
    pcm16_rms_avx2_selected = 0;
}

#if defined(__GNUC__)
__attribute__((constructor))
#endif
static void pcm16_rms_ctor(void) {
    pcm16_rms_select();
}

int pcm16_rms_using_avx2(void) {
    if (!pcm16_rms_impl)
        pcm16_rms_select();
    return pcm16_rms_avx2_selected;
}

double pcm16_rms(const int16_t *in, int n) {
    if (!pcm16_rms_impl)
        pcm16_rms_select();
    return pcm16_rms_impl(in, n);
}
