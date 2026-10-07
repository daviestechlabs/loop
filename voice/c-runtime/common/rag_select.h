/* rag_select — pure-C collection selector strip/split (no heap). */
#ifndef VOICE_C_RAG_SELECT_H
#define VOICE_C_RAG_SELECT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RAG_SELECT_MAX_PARTS = 16,
    RAG_SELECT_MAX_NAME = 128
};

/*
 * Strip collection selector options: cut at first '?' or whitespace.
 * Trims leading/trailing ASCII space/tab. Writes NUL-terminated name to out.
 * Returns 0 on success, -1 on bad args.
 */
int rag_select_strip_options(const char *in, size_t in_len, char *out, size_t out_cap);

/*
 * Comma-split a selector into up to RAG_SELECT_MAX_PARTS names.
 * Each part is strip_options'ed. *out_n set to count written.
 * parts[][RAG_SELECT_MAX_NAME] caller-owned. Returns 0 on success.
 */
int rag_select_split(
    const char *in,
    size_t in_len,
    char parts[][RAG_SELECT_MAX_NAME],
    size_t parts_cap,
    size_t *out_n
);

/*
 * Extract grounded context from rag-gateway JSON (or raw hits).
 * Pulls up to max_hits values of "text"/"content"/"snippet" string fields,
 * joined by " | ". Falls back to a trimmed copy of in if no fields found.
 * Returns 0 on success.
 */
int rag_hits_excerpt_v1(
    const char *in,
    size_t in_len,
    size_t max_hits,
    char *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif

#endif
