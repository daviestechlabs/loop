/* pcm_frame — split Orpheus/WAV PCM into fixed-duration frames (no heap). */
#ifndef VOICE_C_PCM_FRAME_H
#define VOICE_C_PCM_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default progressive TTS frame duration (product 20ms path). */
#ifndef PCM_FRAME_MS_DEFAULT
#define PCM_FRAME_MS_DEFAULT 20
#endif

/* Bytes for one frame of interleaved PCM. 0 on bad args. */
size_t pcm_frame_bytes_v1(uint32_t sample_rate, uint16_t channels, uint16_t bit_depth, int frame_ms);

/*
 * Describe how to walk a provider payload into multi-chunk publishes.
 * - orpheus: 8-byte header + raw PCM body; each published packet = header + frame
 * - wav:     body is pcm after RIFF parse; packets are raw PCM frames (no RIFF rewrap)
 * - raw:     whole buffer is body; packets are raw slices
 */
typedef struct pcm_frame_plan {
    const uint8_t *body;   /* points into original payload */
    size_t body_len;
    const uint8_t *header; /* 8 bytes or NULL */
    size_t header_len;     /* 0 or 8 */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bit_depth;
    size_t frame_bytes;
    size_t frame_count; /* ceil(body_len / frame_bytes), 0 if empty */
} pcm_frame_plan;

enum {
    PCM_FRAME_OK = 0,
    PCM_FRAME_ERR_ARGUMENT = 1,
    PCM_FRAME_ERR_PARSE = 2
};

/*
 * Build a plan from provider bytes (Orpheus 8-byte header, RIFF/WAVE, or raw).
 * Defaults: 16k mono s16, frame_ms=20 if header missing.
 */
int pcm_frame_plan_v1(
    const uint8_t *data,
    size_t len,
    int frame_ms,
    pcm_frame_plan *out
);

/*
 * Fill packet for frame index i into out (cap).
 * Packet = optional header + body slice. Returns packet length, 0 if done/err.
 */
size_t pcm_frame_packet_v1(
    const pcm_frame_plan *plan,
    size_t frame_index,
    uint8_t *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif

#endif
