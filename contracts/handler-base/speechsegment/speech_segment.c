/* speech_segment.c — pure-C speakable boundary policy (no heap). */

#include "speech_segment.h"

#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#define SPEECH_SEG_HAS_SSE2 1
#else
#define SPEECH_SEG_HAS_SSE2 0
#endif

static int utf8_decode(const char *s, size_t len, size_t i, uint32_t *cp, size_t *adv) {
    unsigned char c0;
    if (i >= len) return -1;
    c0 = (unsigned char)s[i];
    if (c0 < 0x80u) {
        *cp = c0;
        *adv = 1;
        return 0;
    }
    if ((c0 & 0xe0u) == 0xc0u && i + 1 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        if ((c1 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x1fu) << 6) | (uint32_t)(c1 & 0x3fu);
        *adv = 2;
        return 0;
    }
    if ((c0 & 0xf0u) == 0xe0u && i + 2 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        unsigned char c2 = (unsigned char)s[i + 2];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x0fu) << 12) | ((uint32_t)(c1 & 0x3fu) << 6) |
              (uint32_t)(c2 & 0x3fu);
        *adv = 3;
        return 0;
    }
    if ((c0 & 0xf8u) == 0xf0u && i + 3 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        unsigned char c2 = (unsigned char)s[i + 2];
        unsigned char c3 = (unsigned char)s[i + 3];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u || (c3 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(c1 & 0x3fu) << 12) |
              ((uint32_t)(c2 & 0x3fu) << 6) | (uint32_t)(c3 & 0x3fu);
        *adv = 4;
        return 0;
    }
    *cp = 0xfffdu;
    *adv = 1;
    return 0;
}

size_t speech_seg_rune_len_v1(const char *text, size_t text_len) {
    size_t i = 0, n = 0;
    if (!text) return 0;
    while (i < text_len) {
        uint32_t cp;
        size_t adv;
        if (utf8_decode(text, text_len, i, &cp, &adv) != 0) break;
        i += adv;
        n++;
    }
    return n;
}

static int is_hard(uint32_t r) {
    return r == '.' || r == '!' || r == '?' || r == '\n';
}

static int is_soft(uint32_t r) {
    return r == ',' || r == ';' || r == ':';
}

static int is_space_ascii(uint32_t r) {
    return r == ' ' || r == '\n' || r == '\t' || r == '\r';
}

static int ascii_segment_word_has_special(size_t word) {
    const size_t ones = SIZE_MAX / 0xffu;
    const size_t highs = ones << 7;
    size_t controls = word & (ones * 0xe0u);
    size_t bangs = word ^ (ones * (size_t)'!');
    size_t comma_group =
        (word & (ones * 0xfcu)) ^ (ones * 0x2cu);
    size_t three_group =
        (word & (ones * 0xf0u)) ^ (ones * 0x30u);
    /* The broad groups may stop on digits, hyphens, or slashes. The scalar
     * step below makes the exact boundary decision for every stopped word. */
    return (word & highs) != 0u ||
        ((controls - ones) & ~controls & highs) != 0u ||
        ((bangs - ones) & ~bangs & highs) != 0u ||
        ((comma_group - ones) & ~comma_group & highs) != 0u ||
        ((three_group - ones) & ~three_group & highs) != 0u;
}

static int ascii_segment_byte_has_special(unsigned char c) {
    return c >= 0x80u || c == '<' || is_hard(c) || is_soft(c);
}

static size_t ascii_segment_tag_free_span(const char *text, size_t text_len) {
    size_t i = 0;
#if SPEECH_SEG_HAS_SSE2
    const __m128i angle_bytes = _mm_set1_epi8('<');
    while (text_len - i >= 16u) {
        __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)(text + i));
        unsigned int mask = (unsigned int)_mm_movemask_epi8(bytes) |
            (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(bytes, angle_bytes));
        if (mask != 0u) return i + (size_t)__builtin_ctz(mask);
        i += 16u;
    }
#endif
    while (text_len - i >= sizeof(size_t)) {
        const size_t ones = SIZE_MAX / 0xffu;
        const size_t highs = ones << 7;
        size_t word;
        size_t angle;
        memcpy(&word, text + i, sizeof(word));
        angle = word ^ (ones * (size_t)'<');
        if ((word & highs) != 0u ||
            ((angle - ones) & ~angle & highs) != 0u)
            break;
        i += sizeof(word);
    }
    while (i < text_len && (unsigned char)text[i] < 0x80u && text[i] != '<')
        i++;
    return i;
}

static size_t ascii_segment_plain_span(const char *text, size_t text_len) {
    size_t i = 0;
#if SPEECH_SEG_HAS_SSE2
    const __m128i newline = _mm_set1_epi8('\n');
    const __m128i bang = _mm_set1_epi8('!');
    const __m128i comma_mask = _mm_set1_epi8((char)0xfdu);
    const __m128i comma = _mm_set1_epi8(',');
    const __m128i colon_mask = _mm_set1_epi8((char)0xfeu);
    const __m128i colon = _mm_set1_epi8(':');
    const __m128i angle_mask = _mm_set1_epi8((char)0xfcu);
    const __m128i angle = _mm_set1_epi8('<');
    while (text_len - i >= 16u) {
        __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)(text + i));
        __m128i matches = _mm_or_si128(
            _mm_cmpeq_epi8(bytes, newline),
            _mm_cmpeq_epi8(bytes, bang));
        matches = _mm_or_si128(
            matches,
            _mm_cmpeq_epi8(_mm_and_si128(bytes, comma_mask), comma));
        matches = _mm_or_si128(
            matches,
            _mm_cmpeq_epi8(_mm_and_si128(bytes, colon_mask), colon));
        matches = _mm_or_si128(
            matches,
            _mm_cmpeq_epi8(_mm_and_si128(bytes, angle_mask), angle));
        {
            unsigned int mask = (unsigned int)_mm_movemask_epi8(bytes) |
                (unsigned int)_mm_movemask_epi8(matches);
            if (mask != 0u)
                return i + (size_t)__builtin_ctz(mask);
        }
        i += 16u;
    }
#endif
    while (text_len - i >= sizeof(size_t)) {
        size_t word;
        memcpy(&word, text + i, sizeof(word));
        if (ascii_segment_word_has_special(word)) break;
        i += sizeof(word);
    }
    while (i < text_len &&
           !ascii_segment_byte_has_special((unsigned char)text[i]))
        i++;
    return i;
}

static int set_ascii_segment(
    const char *text,
    size_t text_len,
    size_t start,
    size_t cut,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
) {
    size_t end = cut;
    size_t rest = cut;
    size_t length;

    while (end > start && is_space_ascii((unsigned char)text[end - 1u])) end--;
    while (rest < text_len && is_space_ascii((unsigned char)text[rest])) rest++;
    length = end > start ? end - start : 0;
    if (length == 0 || (length == 1u && text[start] == '>'))
        return SPEECH_SEG_NO_CUT;
    *seg_off = start;
    *seg_len = length;
    *rest_off = rest;
    *rest_len = text_len - rest;
    return SPEECH_SEG_OK;
}

/* At the cap, prefer a complete word after the minimum. A word with no
 * eligible separator still uses the bounded rune cut. */
static size_t ascii_word_cut(const char *text, size_t length, size_t start,
    size_t limit, int minimum) {
    if (limit < length && is_space_ascii((unsigned char)text[limit])) return limit;
    for (size_t i = limit; i > start + (size_t)minimum;) {
        if (is_space_ascii((unsigned char)text[--i])) return i;
    }
    return limit;
}

static int try_next_ascii_plain(
    const char *text,
    size_t text_len,
    const speech_seg_config_v1 *cfg,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len,
    int *handled
) {
    size_t start = 0;
    size_t cut = (size_t)-1;
    size_t i;
    int runes = 0;

    *handled = 1;
    while (start < text_len) {
        unsigned char c = (unsigned char)text[start];
        if (c >= 0x80u || c == '<') {
            *handled = 0;
            return SPEECH_SEG_NO_CUT;
        }
        if (!is_space_ascii(c)) break;
        start++;
    }
    for (i = start; i < text_len;) {
        size_t remaining;
        size_t span;
        unsigned char c;
        remaining = (size_t)(cfg->max_segment_chars - runes);
        if (remaining > text_len - i) remaining = text_len - i;
        span = ascii_segment_plain_span(text + i, remaining);
        i += span;
        runes += (int)span;
        if (span == remaining) {
            if (runes < cfg->max_segment_chars) break;
            if (cut == (size_t)-1)
                cut = ascii_word_cut(text, text_len, start, i, cfg->min_segment_chars);
            break;
        }
        c = (unsigned char)text[i];
        if (c >= 0x80u || c == '<') {
            *handled = 0;
            return SPEECH_SEG_NO_CUT;
        }
        runes++;
        if (is_hard(c) || (is_soft(c) && runes >= cfg->min_segment_chars))
            cut = i + 1u;
        if (runes >= cfg->max_segment_chars) {
            if (cut == (size_t)-1)
                cut = ascii_word_cut(text, text_len, start, i + 1u, cfg->min_segment_chars);
            break;
        }
        i++;
    }
    if (runes < cfg->min_segment_chars || cut == (size_t)-1)
        return SPEECH_SEG_NO_CUT;
    return set_ascii_segment(
        text, text_len, start, cut, seg_off, seg_len, rest_off, rest_len);
}

static int try_first_prefix_ascii_plain(
    const char *text,
    size_t text_len,
    int min_runes,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len,
    int *handled
) {
    size_t start = 0;
    size_t i;
    int runes = 0;

    *handled = 1;
    while (start < text_len) {
        unsigned char c = (unsigned char)text[start];
        if (c >= 0x80u || c == '<') {
            *handled = 0;
            return SPEECH_SEG_NO_CUT;
        }
        if (!is_space_ascii(c)) break;
        start++;
    }
    if ((size_t)min_runes > text_len - start) return SPEECH_SEG_NO_CUT;
    i = start + (size_t)min_runes - 1u;
    if (ascii_segment_tag_free_span(text + start, i - start) != i - start) {
        *handled = 0;
        return SPEECH_SEG_NO_CUT;
    }
    runes = min_runes - 1;
    for (; i < text_len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80u || c == '<') {
            *handled = 0;
            return SPEECH_SEG_NO_CUT;
        }
        runes++;
        if (runes >= min_runes &&
            (is_space_ascii(c) || is_hard(c) || is_soft(c)))
            return set_ascii_segment(
                text,
                text_len,
                start,
                i + 1u,
                seg_off,
                seg_len,
                rest_off,
                rest_len);
    }
    return SPEECH_SEG_NO_CUT;
}

static size_t trim_left_off(const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t adv;
        if (utf8_decode(text, len, i, &cp, &adv) != 0) break;
        if (!is_space_ascii(cp)) break;
        i += adv;
    }
    return i;
}

static size_t trim_right_len(const char *text, size_t off, size_t end) {
    /* end is exclusive byte index into text; return trimmed exclusive end. */
    size_t e = end;
    while (e > off) {
        /* walk back one UTF-8 char */
        size_t i = e - 1;
        while (i > off && ((unsigned char)text[i] & 0xc0u) == 0x80u) i--;
        {
            uint32_t cp;
            size_t adv;
            if (utf8_decode(text, end, i, &cp, &adv) != 0) break;
            if (i + adv != e) break; /* not aligned */
            if (!is_space_ascii(cp)) break;
            e = i;
        }
    }
    return e;
}

void speech_seg_config_default_v1(speech_seg_config_v1 *cfg) {
    if (!cfg) return;
    cfg->first_segment_chars = 0;
    cfg->min_segment_chars = 24;
    cfg->max_segment_chars = 220;
}

void speech_seg_config_normalize_v1(speech_seg_config_v1 *cfg) {
    if (!cfg) return;
    if (cfg->min_segment_chars <= 0) cfg->min_segment_chars = 24;
    if (cfg->max_segment_chars <= 0) cfg->max_segment_chars = 220;
    if (cfg->max_segment_chars < cfg->min_segment_chars)
        cfg->max_segment_chars = cfg->min_segment_chars;
    if (cfg->first_segment_chars < 0) cfg->first_segment_chars = 0;
    if (cfg->first_segment_chars > cfg->max_segment_chars)
        cfg->first_segment_chars = cfg->max_segment_chars;
}

int speech_seg_next_v1(
    const char *text,
    size_t text_len,
    const speech_seg_config_v1 *cfg,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
) {
    speech_seg_config_v1 local;
    size_t trim0;
    const char *t;
    size_t tlen;
    size_t cut_index = (size_t)-1;
    size_t word_cut = (size_t)-1;
    int runes = 0;
    int in_tag = 0;
    int tag_runes = 0;
    int ascii_handled;
    int ascii_result;
    size_t i;

    if (!text || !cfg || !seg_off || !seg_len || !rest_off || !rest_len)
        return SPEECH_SEG_ERR_ARGUMENT;
    local = *cfg;
    speech_seg_config_normalize_v1(&local);

    *seg_off = 0;
    *seg_len = 0;
    *rest_off = 0;
    *rest_len = text_len;

    ascii_result = try_next_ascii_plain(
        text,
        text_len,
        &local,
        seg_off,
        seg_len,
        rest_off,
        rest_len,
        &ascii_handled);
    if (ascii_handled) return ascii_result;

    trim0 = trim_left_off(text, text_len);
    t = text + trim0;
    tlen = text_len - trim0;

    for (i = 0; i < tlen;) {
        uint32_t r;
        size_t adv;
        size_t byte_end;
        if (utf8_decode(t, tlen, i, &r, &adv) != 0) break;
        byte_end = i + adv;
        runes++;

        if (r == '<') {
            in_tag = 1;
            tag_runes = 1;
        } else if (in_tag) {
            tag_runes++;
            if (r == '>') {
                in_tag = 0;
                tag_runes = 0;
            } else if (tag_runes >= SPEECH_SEG_MAX_TAG_SPAN) {
                in_tag = 0;
                tag_runes = 0;
            }
        }

        if (!in_tag &&
            (is_hard(r) || (is_soft(r) && runes >= local.min_segment_chars))) {
            cut_index = byte_end;
        }
        if (!in_tag && is_space_ascii(r) && runes > local.min_segment_chars)
            word_cut = i;

        if (runes >= local.max_segment_chars) {
            if (in_tag) {
                if (r == '>') {
                    cut_index = byte_end;
                    break;
                }
                i = byte_end;
                continue;
            }
            if (cut_index == (size_t)-1) {
                int boundary = byte_end < tlen && is_space_ascii((unsigned char)t[byte_end]);
                cut_index = !boundary && word_cut != (size_t)-1 ? word_cut : byte_end;
            }
            break;
        }
        i = byte_end;
    }

    if (runes < local.min_segment_chars) return SPEECH_SEG_NO_CUT;
    if (cut_index == (size_t)-1) {
        return SPEECH_SEG_NO_CUT;
    }

    {
        size_t abs_start = trim0;
        size_t abs_cut = trim0 + cut_index;
        size_t trim_end = trim_right_len(text, abs_start, abs_cut);
        size_t rest_start = abs_cut + trim_left_off(text + abs_cut, text_len - abs_cut);
        size_t slen = trim_end > abs_start ? trim_end - abs_start : 0;

        if (slen == 1 && text[abs_start] == '>') {
            return SPEECH_SEG_NO_CUT;
        }
        if (slen == 0) {
            return SPEECH_SEG_NO_CUT;
        }
        *seg_off = abs_start;
        *seg_len = slen;
        *rest_off = rest_start;
        *rest_len = text_len - rest_start;
        return SPEECH_SEG_OK;
    }
}

int speech_seg_split_speakable_v1(
    const char *text,
    size_t text_len,
    const speech_seg_config_v1 *cfg,
    size_t *seg_off,
    size_t *seg_len,
    size_t max_parts,
    size_t *out_n,
    size_t *rem_off,
    size_t *rem_len
) {
    size_t cur_off = 0;
    size_t cur_len = text_len;
    size_t n = 0;

    if (!out_n || !rem_off || !rem_len) return SPEECH_SEG_ERR_ARGUMENT;
    *out_n = 0;
    *rem_off = 0;
    *rem_len = text_len;
    if (!text || !cfg) return SPEECH_SEG_ERR_ARGUMENT;
    if (max_parts > 0 && (!seg_off || !seg_len)) return SPEECH_SEG_ERR_ARGUMENT;

    while (n < max_parts && n < SPEECH_SEG_MAX_PARTS) {
        size_t so, sl, ro, rl;
        int rc = speech_seg_next_v1(text + cur_off, cur_len, cfg, &so, &sl, &ro, &rl);
        if (rc == SPEECH_SEG_NO_CUT) {
            *rem_off = cur_off;
            *rem_len = cur_len;
            *out_n = n;
            return SPEECH_SEG_OK;
        }
        if (rc != SPEECH_SEG_OK) return rc;
        seg_off[n] = cur_off + so;
        seg_len[n] = sl;
        n++;
        cur_off = cur_off + ro;
        cur_len = rl;
    }
    *rem_off = cur_off;
    *rem_len = cur_len;
    *out_n = n;
    return SPEECH_SEG_OK;
}

int speech_seg_first_prefix_v1(
    const char *text,
    size_t text_len,
    int min_runes,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
) {
    size_t trim0;
    const char *t;
    size_t tlen;
    int runes = 0;
    int in_tag = 0;
    int tag_runes = 0;
    int ascii_handled;
    int ascii_result;
    size_t i;

    if (!text || !seg_off || !seg_len || !rest_off || !rest_len)
        return SPEECH_SEG_ERR_ARGUMENT;
    *seg_off = 0;
    *seg_len = 0;
    *rest_off = 0;
    *rest_len = text_len;

    if (min_runes <= 0) return SPEECH_SEG_NO_CUT;
    ascii_result = try_first_prefix_ascii_plain(
        text,
        text_len,
        min_runes,
        seg_off,
        seg_len,
        rest_off,
        rest_len,
        &ascii_handled);
    if (ascii_handled) return ascii_result;

    trim0 = trim_left_off(text, text_len);
    t = text + trim0;
    tlen = text_len - trim0;

    for (i = 0; i < tlen;) {
        uint32_t r;
        size_t adv;
        size_t byte_end;
        if (utf8_decode(t, tlen, i, &r, &adv) != 0) break;
        byte_end = i + adv;
        runes++;

        if (r == '<') {
            in_tag = 1;
            tag_runes = 1;
        } else if (in_tag) {
            tag_runes++;
            if (r == '>') {
                in_tag = 0;
                tag_runes = 0;
            } else if (tag_runes >= SPEECH_SEG_MAX_TAG_SPAN) {
                in_tag = 0;
                tag_runes = 0;
            }
        }

        if (!in_tag && runes >= min_runes &&
            (is_space_ascii(r) || is_hard(r) || is_soft(r))) {
            size_t abs_start = trim0;
            size_t abs_cut = trim0 + byte_end;
            size_t trim_end = trim_right_len(text, abs_start, abs_cut);
            size_t rest_start = abs_cut + trim_left_off(text + abs_cut, text_len - abs_cut);
            size_t slen = trim_end > abs_start ? trim_end - abs_start : 0;
            if (slen == 1 && text[abs_start] == '>') {
                return SPEECH_SEG_NO_CUT;
            }
            if (slen == 0) {
                return SPEECH_SEG_NO_CUT;
            }
            *seg_off = abs_start;
            *seg_len = slen;
            *rest_off = rest_start;
            *rest_len = text_len - rest_start;
            return SPEECH_SEG_OK;
        }
        i = byte_end;
    }
    return SPEECH_SEG_NO_CUT;
}
