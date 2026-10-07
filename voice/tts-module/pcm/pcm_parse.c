#include "pcm_parse.h"

#include <string.h>

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int pcm_parse_header_v1(const uint8_t *data, size_t len, pcm_header_c *out) {
    if (!out) {
        return PCM_PARSE_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!data || len < 8) {
        return PCM_PARSE_ERR_TOO_SHORT;
    }
    out->sample_rate = rd_u32_le(data);
    out->channels = rd_u16_le(data + 4);
    out->bit_depth = rd_u16_le(data + 6);
    if (out->sample_rate == 0 || out->channels == 0 ||
        (out->bit_depth != 8 && out->bit_depth != 16 && out->bit_depth != 24 &&
         out->bit_depth != 32)) {
        memset(out, 0, sizeof(*out));
        return PCM_PARSE_ERR_INVALID;
    }
    return PCM_PARSE_OK;
}

int pcm_parse_mono_s16_header_v1(
    const uint8_t *data,
    size_t len,
    pcm_header_c *out
) {
    uint32_t sample_rate;
    if (!out) return PCM_PARSE_ERR_ARGUMENT;
    if (!data || len < 8u) {
        memset(out, 0, sizeof(*out));
        return PCM_PARSE_ERR_TOO_SHORT;
    }
    sample_rate = rd_u32_le(data);
    if (sample_rate < 8000u || sample_rate > 48000u ||
        data[4] != 1u || data[5] != 0u || data[6] != 16u || data[7] != 0u) {
        memset(out, 0, sizeof(*out));
        return PCM_PARSE_ERR_INVALID;
    }
    out->sample_rate = sample_rate;
    out->channels = 1u;
    out->bit_depth = 16u;
    return PCM_PARSE_OK;
}

int pcm_parse_wav_v1(
    const uint8_t *data,
    size_t len,
    pcm_header_c *out_hdr,
    size_t *pcm_off,
    size_t *pcm_len
) {
    size_t offset;
    size_t riff_end;
    int have_fmt = 0;

    if (!out_hdr || !pcm_off || !pcm_len) {
        return PCM_PARSE_ERR_ARGUMENT;
    }
    *pcm_off = 0;
    *pcm_len = 0;
    memset(out_hdr, 0, sizeof(*out_hdr));

    if (!data || len < 44) {
        return PCM_PARSE_ERR_TOO_SHORT;
    }
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return PCM_PARSE_ERR_INVALID;
    }
    {
        uint32_t riff_size = rd_u32_le(data + 4);
        if (riff_size < 4) return PCM_PARSE_ERR_INVALID;
        if ((uint64_t)riff_size + UINT64_C(8) > (uint64_t)len)
            return PCM_PARSE_ERR_TRUNCATED;
        riff_end = (size_t)riff_size + 8u;
    }

    for (offset = 12; offset <= riff_end && riff_end - offset >= 8;) {
        const char *chunk_id = (const char *)(data + offset);
        uint32_t chunk_size_u = rd_u32_le(data + offset + 4);
        size_t chunk_size = (size_t)chunk_size_u;
        size_t chunk_start = offset + 8;
        size_t chunk_end;

        if (chunk_size > riff_end - chunk_start) {
            return PCM_PARSE_ERR_TRUNCATED;
        }
        chunk_end = chunk_start + chunk_size;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t format;
            uint16_t channels;
            uint16_t bits;
            uint16_t block_align;
            uint32_t sample_rate;
            uint32_t byte_rate;
            uint64_t expected_align;
            uint64_t expected_rate;
            if (chunk_size < 16) {
                return PCM_PARSE_ERR_INVALID;
            }
            format = rd_u16_le(data + chunk_start);
            if (format != 1) {
                return PCM_PARSE_ERR_UNSUPPORTED;
            }
            channels = rd_u16_le(data + chunk_start + 2);
            sample_rate = rd_u32_le(data + chunk_start + 4);
            byte_rate = rd_u32_le(data + chunk_start + 8);
            block_align = rd_u16_le(data + chunk_start + 12);
            bits = rd_u16_le(data + chunk_start + 14);
            if (channels == 0 || sample_rate == 0 ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32))
                return PCM_PARSE_ERR_INVALID;
            expected_align = (uint64_t)channels * (uint64_t)(bits / 8u);
            expected_rate = (uint64_t)sample_rate * expected_align;
            if (expected_align > UINT16_MAX || expected_rate > UINT32_MAX ||
                block_align != (uint16_t)expected_align || byte_rate != (uint32_t)expected_rate)
                return PCM_PARSE_ERR_INVALID;
            out_hdr->channels = channels;
            out_hdr->sample_rate = sample_rate;
            out_hdr->bit_depth = bits;
            have_fmt = 1;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (!have_fmt || out_hdr->sample_rate == 0 || out_hdr->channels == 0 ||
                out_hdr->bit_depth == 0) {
                return PCM_PARSE_ERR_INVALID;
            }
            *pcm_off = chunk_start;
            *pcm_len = chunk_end - chunk_start;
            if (*pcm_len % ((size_t)out_hdr->channels * (out_hdr->bit_depth / 8u)) != 0) {
                *pcm_off = 0;
                *pcm_len = 0;
                return PCM_PARSE_ERR_INVALID;
            }
            return PCM_PARSE_OK;
        }

        offset = chunk_end;
        if ((offset & 1u) != 0u) {
            if (offset == riff_end) return PCM_PARSE_ERR_TRUNCATED;
            offset++;
        }
    }
    return PCM_PARSE_ERR_INVALID;
}
