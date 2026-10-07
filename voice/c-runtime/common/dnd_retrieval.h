#ifndef VOICE_C_DND_RETRIEVAL_H
#define VOICE_C_DND_RETRIEVAL_H

#include "cmp_json.h"
#include "dnd_retrieval_types.h"
#include <stddef.h>

enum {
    DND_RAG_SCOPE_COUNT = 5,
    DND_RAG_VECTOR_DIM_MAX = 1024,
    DND_RAG_VECTOR_JSON_CAP = 32768,
    DND_RAG_REPLY_CAP = 262144
};

/* Same scope order and semantics as workflows/argo-runtime/dnd_scope.py. */
typedef enum {
    DND_RAG_SHARED_RULEBOOK,
    DND_RAG_OWNED_RULEBOOK,
    DND_RAG_CAMPAIGN_CANON,
    DND_RAG_SESSION_TRANSCRIPT,
    DND_RAG_CHARACTER_MEMORY
} dnd_rag_scope_kind;

typedef struct {
    /* These names come from service configuration, never turn metadata. */
    char collections[DND_RAG_SCOPE_COUNT][128];
    char embedding_model[128];
    size_t embedding_dimensions;
    dnd_rag_score_metric shared_rulebook_metric;
    /* Optional server policy; empty retains the catalog-wide shared scope. */
    char shared_rulebook_ruleset[128];
    /* Optional server pin. Source-index fetching requires a nonempty value. */
    char shared_rulebook_corpus[128];
} dnd_rag_config;

typedef struct {
    /* user_id and entitled must come from authenticated server admission. */
    const char *user_id;
    const char *session_id;
    const char *campaign_id;
    const char *character_id;
    const char *knowledge_scope;
    int entitled;
    int premium;
} dnd_rag_identity;

typedef struct {
    dnd_rag_scope_kind kind;
    char collection[128];
    char authenticated_user_id[128];
    char owner_user_id[128];
    char campaign_id[128];
    char session_id[128];
    char character_id[128];
    int premium;
    uint64_t book_mask;
    char ruleset[128];
    char filter[4096];
} dnd_rag_scope;

/* All builders return 0 on success and -1 on rejection. Output clears on failure. */
int dnd_rag_scope_resolve(
    const dnd_rag_config *config, const dnd_rag_identity *identity, dnd_rag_scope *out);

/* Check scope fields in the ingestion record's metadata_json object. */
int dnd_rag_scope_matches(const dnd_rag_scope *scope, const cmp_json_object *metadata);

int dnd_rag_embedding_body(
    const dnd_rag_config *config, const char *query, char *out, size_t capacity);

/* Require CLS pooling and a confirmed complete input within the engine window.
 * Validate one model-bound embedding and retain its exact JSON numbers. */
int dnd_rag_embedding_extract(
    const dnd_rag_config *config, const char *json, char *out, size_t capacity);

/* Milvus REST v2 request with a server-derived filter and explicit output fields. */
int dnd_rag_search_body(
    const dnd_rag_config *config, const dnd_rag_scope *scope, const char *vector_json,
    size_t top_k, char *out, size_t capacity);

/* BM25 uses the full query and the same server-derived shared-book filter. */
int dnd_rag_lexical_search_body(
    const dnd_rag_config *config, const dnd_rag_scope *scope, const char *query,
    size_t top_k, char *out, size_t capacity);

/* Validate the entire Milvus reply before exposing any hit. The caller supplies
 * complete, NUL-terminated JSON and rejects embedded NUL bytes at the transport.
 * A malformed or unauthorized hit clears the whole result, including earlier hits.
 * Source SHA-256 is an ingestion assertion; content SHA-256 is recomputed here. */
int dnd_rag_search_extract(
    const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *json, size_t top_k, dnd_rag_result *out);

/* Exact shared-book fetches retain the canonical server filter and corpus pin.
 * IDs must be unique SHA-256 strings, with at most four records per group.
 * Query results have no ranking score. Missing, extra or duplicate records
 * reject the whole group. Successful output follows the requested ID order. */
int dnd_rag_record_query_body(
    const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *const *ids, size_t count, char *out, size_t capacity);
int dnd_rag_record_query_extract(
    const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *const *ids, size_t count, const char *json, dnd_rag_result *out);

/* Preserve every admitted excerpt and the complete question, or reject.
 * Recompute full-record hashes after the typed wire boundary, then apply
 * explicit record-local ranges. Invalid or partial UTF-8 ranges reject. */
int dnd_rag_grounded_prompt(
    const dnd_rag_result *result, const char *query, char *out, size_t capacity);

#endif
