/* speech_seg_host.c — streaming segmenter buffer host. */

#include "speech_seg_host.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void speech_seg_host_init_v1(speech_seg_host_v1 *h, const speech_seg_config_v1 *cfg) {
    if (!h) return;
    if (cfg) {
        h->cfg = *cfg;
    } else {
        speech_seg_config_default_v1(&h->cfg);
    }
    speech_seg_config_normalize_v1(&h->cfg);
    h->overflow_buf = NULL;
    h->len = 0;
    h->overflow_cap = 0;
    h->emitted = 0;
    h->buffer_since_ns = 0;
    h->last_append_ns = 0;
    h->flush_timeout_ns = SPEECH_SEG_HOST_DEFAULT_FLUSH_NS;
}

void speech_seg_host_free_v1(speech_seg_host_v1 *h) {
    if (!h) return;
    free(h->overflow_buf);
    memset(&h->cfg, 0, sizeof(h->cfg));
    h->overflow_buf = NULL;
    h->len = 0;
    h->overflow_cap = 0;
    h->emitted = 0;
    h->buffer_since_ns = 0;
    h->last_append_ns = 0;
    h->flush_timeout_ns = 0;
}

void speech_seg_host_reset_v1(speech_seg_host_v1 *h) {
    if (!h) return;
    h->len = 0;
    h->emitted = 0;
    h->buffer_since_ns = 0;
    h->last_append_ns = 0;
}

static char *host_data(speech_seg_host_v1 *h) {
    return h->overflow_buf ? h->overflow_buf : h->inline_buf;
}

static const char *host_const_data(const speech_seg_host_v1 *h) {
    return h->overflow_buf ? h->overflow_buf : h->inline_buf;
}

static size_t host_capacity(const speech_seg_host_v1 *h) {
    return h->overflow_buf ? h->overflow_cap : sizeof(h->inline_buf);
}

static int host_reserve(speech_seg_host_v1 *h, size_t need) {
    char *nb;
    size_t ncap;
    size_t current_cap = host_capacity(h);
    if (need <= current_cap) return 0;
    ncap = current_cap;
    while (ncap < need) {
        if (ncap > (SIZE_MAX / 2)) return -1;
        ncap *= 2;
    }
    if (h->overflow_buf) {
        nb = (char *)realloc(h->overflow_buf, ncap);
    } else {
        nb = (char *)malloc(ncap);
        if (nb) memcpy(nb, h->inline_buf, h->len);
    }
    if (!nb) return -1;
    h->overflow_buf = nb;
    h->overflow_cap = ncap;
    return 0;
}

static void copy_part(char out[SPEECH_SEG_HOST_MAX_PART], const char *src, size_t n) {
    if (n >= SPEECH_SEG_HOST_MAX_PART) n = SPEECH_SEG_HOST_MAX_PART - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

static int emit_from_buffer(
    speech_seg_host_v1 *h,
    char out_parts[][SPEECH_SEG_HOST_MAX_PART],
    size_t max_parts,
    size_t *out_n
) {
    char *buf;
    int split_rc;
    size_t offs[SPEECH_SEG_MAX_PARTS];
    size_t lens[SPEECH_SEG_MAX_PARTS];
    size_t nparts = 0, rem_off = 0, rem_len = 0;
    size_t i, n = 0;

    *out_n = 0;
    if (h->len == 0) return SPEECH_SEG_OK;
    if (max_parts == 0) return SPEECH_SEG_OK;
    buf = host_data(h);

    split_rc = speech_seg_split_speakable_v1(
        buf, h->len, &h->cfg, offs, lens, max_parts, &nparts, &rem_off, &rem_len);
    if (split_rc != SPEECH_SEG_OK) return split_rc;

    /* Eager first-prefix once, only when no tag and no clause cut yet. */
    if (nparts == 0 && !h->emitted && h->cfg.first_segment_chars > 0) {
        int has_lt = 0;
        size_t j;
        for (j = 0; j < h->len; ++j) {
            if (buf[j] == '<') {
                has_lt = 1;
                break;
            }
        }
        if (!has_lt) {
            size_t so, sl, ro, rl;
            if (speech_seg_first_prefix_v1(
                    buf, h->len, h->cfg.first_segment_chars, &so, &sl, &ro, &rl
                ) == SPEECH_SEG_OK) {
                offs[0] = so;
                lens[0] = sl;
                nparts = 1;
                rem_off = ro;
                rem_len = rl;
            }
        }
    }

    if (nparts == 0) return SPEECH_SEG_OK;
    for (i = 0; i < nparts; ++i) {
        if (lens[i] >= SPEECH_SEG_HOST_MAX_PART)
            return SPEECH_SEG_ERR_ARGUMENT;
    }

    for (i = 0; i < nparts; ++i) {
        copy_part(out_parts[n], buf + offs[i], lens[i]);
        n++;
    }
    h->emitted = 1;

    /* Keep remainder. */
    if (rem_len > 0 && rem_off + rem_len <= h->len) {
        memmove(buf, buf + rem_off, rem_len);
        h->len = rem_len;
    } else {
        h->len = 0;
        h->buffer_since_ns = 0;
    }
    *out_n = n;
    return SPEECH_SEG_OK;
}

int speech_seg_host_add_v1(
    speech_seg_host_v1 *h,
    uint64_t now_ns,
    const char *chunk,
    size_t chunk_len,
    char out_parts[][SPEECH_SEG_HOST_MAX_PART],
    size_t max_parts,
    size_t *out_n
) {
    char *buf;
    if (!h || !out_n) return SPEECH_SEG_ERR_ARGUMENT;
    *out_n = 0;
    if (chunk_len == 0) return SPEECH_SEG_OK;
    if (!chunk) return SPEECH_SEG_ERR_ARGUMENT;
    if (max_parts > 0 && !out_parts) return SPEECH_SEG_ERR_ARGUMENT;
    if (h->len == SIZE_MAX || chunk_len > SIZE_MAX - h->len - 1u)
        return SPEECH_SEG_ERR_ARGUMENT;

    if (h->len == 0) h->buffer_since_ns = now_ns;
    if (host_reserve(h, h->len + chunk_len + 1) != 0) return SPEECH_SEG_ERR_ARGUMENT;
    buf = host_data(h);
    memcpy(buf + h->len, chunk, chunk_len);
    h->len += chunk_len;
    buf[h->len] = '\0';
    h->last_append_ns = now_ns;

    return emit_from_buffer(h, out_parts, max_parts, out_n);
}

static int trim_copy(
    char out[SPEECH_SEG_HOST_MAX_PART],
    const char *src,
    size_t n
) {
    size_t a = 0, b = n;
    while (a < b && (src[a] == ' ' || src[a] == '\t' || src[a] == '\n' || src[a] == '\r')) a++;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t' || src[b - 1] == '\n' || src[b - 1] == '\r'))
        b--;
    if (b - a >= SPEECH_SEG_HOST_MAX_PART) return SPEECH_SEG_ERR_ARGUMENT;
    copy_part(out, src + a, b - a);
    return SPEECH_SEG_OK;
}

int speech_seg_host_flush_if_idle_v1(
    speech_seg_host_v1 *h,
    uint64_t now_ns,
    char out_part[SPEECH_SEG_HOST_MAX_PART],
    int *emitted
) {
    if (!h || !emitted) return SPEECH_SEG_ERR_ARGUMENT;
    *emitted = 0;
    if (!out_part) return SPEECH_SEG_ERR_ARGUMENT;
    out_part[0] = '\0';
    if (h->len == 0 || h->last_append_ns == 0) return SPEECH_SEG_OK;
    if (now_ns < h->last_append_ns ||
        now_ns - h->last_append_ns < h->flush_timeout_ns)
        return SPEECH_SEG_OK;
    return speech_seg_host_flush_all_v1(h, out_part, emitted);
}

int speech_seg_host_flush_all_v1(
    speech_seg_host_v1 *h,
    char out_part[SPEECH_SEG_HOST_MAX_PART],
    int *emitted
) {
    if (!h || !emitted || !out_part) return SPEECH_SEG_ERR_ARGUMENT;
    *emitted = 0;
    out_part[0] = '\0';
    if (h->len == 0) return SPEECH_SEG_OK;
    if (trim_copy(out_part, host_const_data(h), h->len) != SPEECH_SEG_OK)
        return SPEECH_SEG_ERR_ARGUMENT;
    h->len = 0;
    h->buffer_since_ns = 0;
    if (out_part[0]) {
        *emitted = 1;
        h->emitted = 1;
    }
    return SPEECH_SEG_OK;
}
