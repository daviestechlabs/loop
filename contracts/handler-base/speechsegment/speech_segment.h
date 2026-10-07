/* speech_segment.h — pure-C speakable split (nextSegment / SplitSpeakable / first prefix).
 * Mirrors contracts/handler-base/speechsegment segmenter.go pure-CPU policy.
 * No heap in the core cut path; caller owns buffers.
 */
#ifndef HANDLER_BASE_SPEECH_SEGMENT_H
#define HANDLER_BASE_SPEECH_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SPEECH_SEG_MAX_TAG_SPAN = 24,
    SPEECH_SEG_MAX_PARTS = 32
};

enum {
    SPEECH_SEG_OK = 0,
    SPEECH_SEG_ERR_ARGUMENT = 1,
    SPEECH_SEG_NO_CUT = 2
};

typedef struct speech_seg_config_v1 {
    int first_segment_chars; /* 0 disables eager first-prefix */
    int min_segment_chars;
    int max_segment_chars;
} speech_seg_config_v1;

void speech_seg_config_default_v1(speech_seg_config_v1 *cfg);
void speech_seg_config_normalize_v1(speech_seg_config_v1 *cfg);

/*
 * nextSegment: find one speakable cut in text.
 * On SPEECH_SEG_OK: seg_off/seg_len and rest_off/rest_len are byte spans
 * into the original text (after left-trim of leading space for the cut body).
 * On SPEECH_SEG_NO_CUT: no boundary yet (rest covers full original).
 * At the maximum, prefer the last word separator after the minimum.
 * A word without an eligible separator still uses the bounded rune cut.
 */
int speech_seg_next_v1(
    const char *text,
    size_t text_len,
    const speech_seg_config_v1 *cfg,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
);

/*
 * SplitSpeakable: repeatedly nextSegment until no cut.
 * Writes up to max_parts (seg_off[i], seg_len[i]); out_n = count.
 * rem_off/rem_len = final remainder into original text.
 */
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
);

/*
 * Eager first-prefix cut at word/punct boundary (never mid-tag).
 * SPEECH_SEG_OK on success; SPEECH_SEG_NO_CUT if min not met or no boundary.
 */
int speech_seg_first_prefix_v1(
    const char *text,
    size_t text_len,
    int min_runes,
    size_t *seg_off,
    size_t *seg_len,
    size_t *rest_off,
    size_t *rest_len
);

/* UTF-8 rune count over [text, text+len). */
size_t speech_seg_rune_len_v1(const char *text, size_t text_len);

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_SPEECH_SEGMENT_H */
