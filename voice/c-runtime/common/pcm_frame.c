/* pcm_frame.c — multi-chunk progressive PCM framing for TTS path. */

#include "pcm_frame.h"

#include "pcm_parse.h"

#include <stdint.h>
#include <string.h>

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out || (a != 0 && b > SIZE_MAX / a)) return -1;
    *out = a * b;
    return 0;
}

size_t pcm_frame_bytes_v1(uint32_t sample_rate, uint16_t channels, uint16_t bit_depth, int frame_ms) {
    size_t bytes_per_sample;
    size_t bps;
    size_t bytes_per_second;
    size_t numerator;
    if (sample_rate == 0 || channels == 0 || bit_depth == 0 || frame_ms <= 0) return 0;
    bytes_per_sample = ((size_t)bit_depth + 7u) / 8u;
    if (bytes_per_sample == 0) return 0;
    if (checked_mul_size((size_t)channels, bytes_per_sample, &bps) != 0 ||
        checked_mul_size((size_t)sample_rate, bps, &bytes_per_second) != 0 ||
        checked_mul_size(bytes_per_second, (size_t)frame_ms, &numerator) != 0) return 0;
    /* frame_ms / 1000 * rate * bps — keep integer: (rate * bps * frame_ms) / 1000 */
    return numerator / 1000u;
}

static size_t ceil_div(size_t num, size_t den) {
    if (den == 0) return 0;
    return num / den + (num % den != 0);
}

int pcm_frame_plan_v1(const uint8_t *data, size_t len, int frame_ms, pcm_frame_plan *out) {
    pcm_header_c hdr;
    size_t wav_off = 0, wav_len = 0;
    int frame;

    if (!out) return PCM_FRAME_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!data || len == 0) return PCM_FRAME_ERR_ARGUMENT;
    frame = frame_ms > 0 ? frame_ms : PCM_FRAME_MS_DEFAULT;

    /* WAV */
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0) {
        if (pcm_parse_wav_v1(data, len, &hdr, &wav_off, &wav_len) != PCM_PARSE_OK) {
            return PCM_FRAME_ERR_PARSE;
        }
        out->body = data + wav_off;
        out->body_len = wav_len;
        out->header = NULL;
        out->header_len = 0;
        out->sample_rate = hdr.sample_rate;
        out->channels = hdr.channels;
        out->bit_depth = hdr.bit_depth;
    } else if (len >= 8 && pcm_parse_header_v1(data, len, &hdr) == PCM_PARSE_OK &&
               hdr.sample_rate >= 8000 && hdr.sample_rate <= 192000 &&
               hdr.channels >= 1 && hdr.channels <= 8 &&
               (hdr.bit_depth == 16 || hdr.bit_depth == 24 || hdr.bit_depth == 32 ||
                hdr.bit_depth == 8)) {
        /* Orpheus 8-byte LE header + body */
        out->body = data + 8;
        out->body_len = len - 8;
        out->header = data;
        out->header_len = 8;
        out->sample_rate = hdr.sample_rate;
        out->channels = hdr.channels;
        out->bit_depth = hdr.bit_depth;
    } else {
        /* Raw: assume product default 16k mono s16 */
        out->body = data;
        out->body_len = len;
        out->header = NULL;
        out->header_len = 0;
        out->sample_rate = 16000;
        out->channels = 1;
        out->bit_depth = 16;
    }

    {
        size_t sample_bytes = ((size_t)out->bit_depth + 7u) / 8u;
        size_t block_align;
        if (checked_mul_size((size_t)out->channels, sample_bytes, &block_align) != 0 ||
            block_align == 0 || out->body_len % block_align != 0) return PCM_FRAME_ERR_PARSE;
        out->frame_bytes = pcm_frame_bytes_v1(
            out->sample_rate, out->channels, out->bit_depth, frame);
        if (out->frame_bytes < block_align) return PCM_FRAME_ERR_ARGUMENT;
        /* Every emitted packet must contain complete interleaved sample frames. */
        out->frame_bytes -= out->frame_bytes % block_align;
        if (out->frame_bytes == 0) return PCM_FRAME_ERR_ARGUMENT;
    }
    out->frame_count = out->body_len ? ceil_div(out->body_len, out->frame_bytes) : 0;
    return PCM_FRAME_OK;
}

size_t pcm_frame_packet_v1(
    const pcm_frame_plan *plan,
    size_t frame_index,
    uint8_t *out,
    size_t out_cap
) {
    size_t off;
    size_t take;
    size_t need;
    if (!plan || !out || frame_index >= plan->frame_count) return 0;
    if (plan->frame_bytes == 0 || (!plan->body && plan->body_len != 0) ||
        (!plan->header && plan->header_len != 0) || frame_index > SIZE_MAX / plan->frame_bytes)
        return 0;
    off = frame_index * plan->frame_bytes;
    if (off >= plan->body_len) return 0;
    take = plan->body_len - off;
    if (take > plan->frame_bytes) take = plan->frame_bytes;
    if (take > SIZE_MAX - plan->header_len) return 0;
    need = plan->header_len + take;
    if (need > out_cap) return 0;
    if (plan->header_len && plan->header) {
        memcpy(out, plan->header, plan->header_len);
    }
    memcpy(out + plan->header_len, plan->body + off, take);
    return need;
}
