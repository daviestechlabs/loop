/* speech_seg_host.h — stateful streaming segmenter (buffer + idle flush).
 * The normal window stays inline. Exceptional deltas use a retained heap buffer.
 * Pure C; mutex/time host is the caller's responsibility.
 */
#ifndef HANDLER_BASE_SPEECH_SEG_HOST_H
#define HANDLER_BASE_SPEECH_SEG_HOST_H

#include "speech_segment.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SPEECH_SEG_HOST_MAX_PART = 512,
    SPEECH_SEG_HOST_INLINE_CAP = 256,
    SPEECH_SEG_HOST_DEFAULT_FLUSH_NS = 1200000000ull /* 1.2s */
};

typedef struct speech_seg_host_v1 {
    speech_seg_config_v1 cfg;
    char *overflow_buf;
    size_t len;
    size_t overflow_cap;
    int emitted;
    uint64_t buffer_since_ns;
    uint64_t last_append_ns;
    uint64_t flush_timeout_ns;
    char inline_buf[SPEECH_SEG_HOST_INLINE_CAP];
} speech_seg_host_v1;

void speech_seg_host_init_v1(speech_seg_host_v1 *h, const speech_seg_config_v1 *cfg);
void speech_seg_host_free_v1(speech_seg_host_v1 *h);
void speech_seg_host_reset_v1(speech_seg_host_v1 *h);

/*
 * Append chunk at now_ns. Writes up to max_parts NUL-terminated segments into
 * out_parts[i] (each SPEECH_SEG_HOST_MAX_PART). *out_n = count emitted.
 * The host retains segments that exceed max_parts for a later call.
 * Returns SPEECH_SEG_OK or SPEECH_SEG_ERR_ARGUMENT / OOM as ERR_ARGUMENT.
 */
int speech_seg_host_add_v1(
    speech_seg_host_v1 *h,
    uint64_t now_ns,
    const char *chunk,
    size_t chunk_len,
    char out_parts[][SPEECH_SEG_HOST_MAX_PART],
    size_t max_parts,
    size_t *out_n
);

/* Idle flush if last_append + flush_timeout <= now. 0 or 1 segment. */
int speech_seg_host_flush_if_idle_v1(
    speech_seg_host_v1 *h,
    uint64_t now_ns,
    char out_part[SPEECH_SEG_HOST_MAX_PART],
    int *emitted
);

/* Flush remaining buffer as one segment (trimmed). */
int speech_seg_host_flush_all_v1(
    speech_seg_host_v1 *h,
    char out_part[SPEECH_SEG_HOST_MAX_PART],
    int *emitted
);

#ifdef __cplusplus
}
#endif

#endif
