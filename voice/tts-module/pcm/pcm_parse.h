/* pcm_parse.h — pure-C Orpheus PCM header + WAV PCM extract (no heap). */
#ifndef VOICE_TTS_PCM_PARSE_H
#define VOICE_TTS_PCM_PARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcm_header_c {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bit_depth;
} pcm_header_c;

enum {
    PCM_PARSE_OK = 0,
    PCM_PARSE_ERR_ARGUMENT = 1,
    PCM_PARSE_ERR_TOO_SHORT = 2,
    PCM_PARSE_ERR_INVALID = 3,
    PCM_PARSE_ERR_UNSUPPORTED = 4,
    PCM_PARSE_ERR_TRUNCATED = 5
};

/* 8-byte little-endian Orpheus stream header. */
int pcm_parse_header_v1(const uint8_t *data, size_t len, pcm_header_c *out);

/* Product TTS contract: 8-byte little-endian mono-s16 header at 8-48 kHz. */
int pcm_parse_mono_s16_header_v1(
    const uint8_t *data,
    size_t len,
    pcm_header_c *out
);

/*
 * Parse a RIFF/WAVE PCM payload. On success, *pcm_off and *pcm_len point into
 * data (no copy). Caller must keep data live for the duration of use.
 */
int pcm_parse_wav_v1(
    const uint8_t *data,
    size_t len,
    pcm_header_c *out_hdr,
    size_t *pcm_off,
    size_t *pcm_len
);

#ifdef __cplusplus
}
#endif

#endif
