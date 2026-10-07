#ifndef VOICE_C_DND_RETRIEVAL_TYPES_H
#define VOICE_C_DND_RETRIEVAL_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

enum {
    DND_RAG_HITS_MAX = 4,
    DND_RAG_EXCERPT_SPANS_MAX = 8,
    DND_RAG_PASSAGE_WITNESSES_MAX = 16,
    DND_RAG_PASSAGE_BYTES_MAX = 4096,
    DND_RAG_CONTENT_CAP = 8192,
    DND_RAG_WIRE_CAP = 65536,
    DND_RAG_PROMPT_CAP = 40960,
    DND_GROUNDING_TEXT_CAP = 4096,
    DND_RAG_CITATIONS_WIRE_CAP = 33024,
    DND_RAG_PUBLIC_JSON_CAP = 131072
};

#define DND_GROUNDING_PROMPT_ID "rag.answer.grounded_response"

typedef enum {
    DND_RAG_SCORE_COSINE = 0,
    DND_RAG_SCORE_BM25 = 1,
    /* Exact record fetching has no ranking score. Zero is a wire placeholder. */
    DND_RAG_SCORE_NONE = 2
} dnd_rag_score_metric;

static inline int dnd_rag_score_valid(dnd_rag_score_metric metric, double score) {
    if (!isfinite(score)) return 0;
    if (metric == DND_RAG_SCORE_COSINE) return score >= -1.000001 && score <= 1.000001;
    if (metric == DND_RAG_SCORE_BM25) return score >= 0.0;
    return metric == DND_RAG_SCORE_NONE && score == 0.0;
}

static inline const char *dnd_rag_score_metric_name(dnd_rag_score_metric metric) {
    switch (metric) {
    case DND_RAG_SCORE_COSINE: return "cosine";
    case DND_RAG_SCORE_BM25: return "bm25";
    case DND_RAG_SCORE_NONE: return "none";
    default: return NULL;
    }
}

typedef struct {
    char id[64];
    char version[32];
    char sha256[65];
} dnd_grounding_identity;

typedef struct { uint32_t begin, end; } dnd_rag_byte_span;
typedef struct {
    dnd_rag_byte_span spans[DND_RAG_EXCERPT_SPANS_MAX];
    size_t count;
} dnd_rag_excerpt;

/* Zero spans retains the full record. Explicit spans are sorted, disjoint,
 * nonadjacent half-open byte ranges. The prompt also validates UTF-8 cuts. */
static inline int dnd_rag_excerpt_valid(const dnd_rag_excerpt *value, size_t length) {
    if (value->count > DND_RAG_EXCERPT_SPANS_MAX) return 0;
    for (size_t i = 0; i < value->count; ++i) {
        if (value->spans[i].begin >= value->spans[i].end || value->spans[i].end > length ||
            (i && value->spans[i - 1u].end >= value->spans[i].begin)) return 0;
    }
    return 1;
}

typedef struct {
    char record_id[65], content_hash[65];
    uint32_t page, chunk, begin, end, record_length;
} dnd_rag_passage_witness;

typedef struct {
    char source[2048];
    char book_slug[128];
    char collection[128];
    char corpus_version[128];
    char embedding_model[128];
    char record_id[65];
    char document_id[128];
    char section[1024];
    char content_hash[65];
    char source_sha256[65];
    int32_t page_start;
    int32_t page_end;
    int32_t chunk_index;
    double score;
    dnd_rag_score_metric score_metric;
    /* Record citations hash the full original record. Passage citations hash
     * the exact assembly, and each witness retains its original record hash.
     * A passage has a passage_id, no record_id, no excerpt ranges, no score,
     * and chunk_index zero (unused). Original chunk indices are in witnesses. */
    dnd_rag_excerpt excerpt;
    char passage_id[65];
    dnd_rag_passage_witness witnesses[DND_RAG_PASSAGE_WITNESSES_MAX];
    size_t witness_count;
} dnd_rag_citation;

static inline int dnd_rag_hash_valid(const char value[65]) {
    if (value[64]) return 0;
    for (size_t i = 0; i < 64u; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}
static inline const char *dnd_rag_citation_id(const dnd_rag_citation *c) {
    return c->witness_count ? c->passage_id : c->record_id;
}
/* Returns the exact assembled byte length, including one LF at page changes.
 * Zero means no passage or malformed provenance. No content or hash I/O. */
static inline size_t dnd_rag_passage_length(const dnd_rag_citation *c) {
    if (!c->witness_count || c->witness_count > DND_RAG_PASSAGE_WITNESSES_MAX ||
        !dnd_rag_hash_valid(c->passage_id) || c->record_id[0] || c->excerpt.count ||
        c->score_metric != DND_RAG_SCORE_NONE || c->score != 0 || c->chunk_index ||
        c->page_start < 1 || c->page_end < c->page_start || c->page_end - c->page_start >= 3) return 0;
    size_t length = 0;
    for (size_t i = 0; i < c->witness_count; ++i) {
        const dnd_rag_passage_witness *w = &c->witnesses[i];
        if (!dnd_rag_hash_valid(w->record_id) || !dnd_rag_hash_valid(w->content_hash) ||
            !w->page || w->page > INT32_MAX || w->chunk > INT32_MAX ||
            !w->record_length || w->record_length >= DND_RAG_CONTENT_CAP ||
            w->begin >= w->end || w->end > w->record_length) return 0;
        if (!i && w->page != (uint32_t)c->page_start) return 0;
        if (i) {
            const dnd_rag_passage_witness *prev = &c->witnesses[i - 1u];
            if (w->page == prev->page) { if (w->chunk <= prev->chunk) return 0; }
            else { if (w->page != prev->page + 1u) return 0; ++length; }
        }
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(w->record_id, c->witnesses[j].record_id)) return 0;
        length += w->end - w->begin;
        if (length > DND_RAG_PASSAGE_BYTES_MAX) return 0;
    }
    return c->witnesses[c->witness_count - 1u].page == (uint32_t)c->page_end ? length : 0;
}
static inline int dnd_rag_citation_shape_valid(const dnd_rag_citation *c) {
    if (c->witness_count) return dnd_rag_passage_length(c) != 0;
    return !c->passage_id[0] && dnd_rag_hash_valid(c->record_id) && c->chunk_index >= 0 &&
        dnd_rag_excerpt_valid(&c->excerpt, DND_RAG_CONTENT_CAP - 1u);
}

typedef struct {
    char content[DND_RAG_CONTENT_CAP];
    dnd_rag_citation citation;
} dnd_rag_hit;

typedef struct {
    dnd_rag_hit hits[DND_RAG_HITS_MAX];
    size_t count;
} dnd_rag_result;

#endif
