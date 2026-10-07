#ifndef VOICE_C_DND_SOURCE_ARTIFACT_H
#define VOICE_C_DND_SOURCE_ARTIFACT_H

#include "dnd_source_index.h"
#include "dnd_source_passage.h"

enum { DND_SOURCE_ARTIFACT_BYTES_MAX = 64 * 1024 * 1024 };

typedef struct {
    char artifact_sha256[65], manifest_sha256[65], compiler_sha256[65];
    char collection[128], corpus_version[128], ruleset[128];
    /* These identities come from the reviewed manifest, outside the artifact.
     * The artifact may index a subset, but every indexed document must match. */
    const dnd_source_document *documents;
    size_t document_count;
} dnd_source_artifact_expectation;

typedef struct dnd_source_artifact dnd_source_artifact;

/* Startup operations only. Both calls own a private copy and publish only
 * after all identity, wire and source-index admission checks pass. The input
 * expectation is trusted operator configuration, never request metadata.
 * `out` must point to NULL. Failure preserves it and frees temporary storage.
 * File loading accepts only a bounded regular file, without following a final
 * symlink. The byte digest is authoritative even if the file changes on disk. */
int dnd_source_artifact_decode(const void *bytes, size_t length,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out);
int dnd_source_artifact_load(const char *path,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out);

/* Read the exact pinned dnd-corpus-manifest/v1 in C, then load the artifact.
 * expected supplies the trusted pins and scope, with documents/count empty.
 * Manifest bytes are bounded to 256 KiB and sources to 128. Document IDs use
 * the ingestion derivation: SHA256(corpus_id + ':' + (etag or source_key)),
 * truncated to the first 24 lowercase hex digits. No source object is fetched. */
int dnd_source_artifact_load_reviewed(const char *path, const char *manifest_path,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out);
/* Equivalent admission for caller-provided buffers; both are copied privately. */
int dnd_source_artifact_decode_reviewed(const void *bytes, size_t length,
    const void *manifest, size_t manifest_length,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out);

/* Match an admitted citation against the complete reviewed document allowlist,
 * including documents outside the indexed subset. This checks source identity
 * and exact collection/corpus; normal retrieval still owns user authorization. */
int dnd_source_artifact_matches_source(const dnd_source_artifact *artifact,
    const dnd_rag_citation *citation);

enum { DND_SOURCE_PASSAGE_RECORDS_MAX = DND_RAG_HITS_MAX * DND_SOURCE_PASSAGE_SPANS_MAX };
typedef struct {
    uint32_t passages[DND_RAG_HITS_MAX];
    uint32_t records[DND_SOURCE_PASSAGE_RECORDS_MAX];
    size_t count, record_count;
    int reason;
} dnd_source_passage_selection;

/* Query kernel: no allocation, hashing, or I/O. Every passage requires an
 * authorized document anchor. Exact pages win per named spell, then document
 * recovery applies. Anchor order ranks printings. Equal-ranked locations are
 * ambiguous. Matching legacy spell names without a complete selected passage
 * reject the whole selection. The caller must fetch and revalidate every
 * distinct original record before using the precompiled text. Failure clears
 * selections; reason distinguishes no match, ambiguity, and capacity. */
int dnd_source_artifact_select_passages(const dnd_source_artifact *artifact,
    const dnd_rag_scope *scope, const char *corpus, const char *query, size_t query_len,
    const dnd_source_anchor *anchors, size_t anchor_count, dnd_source_passage_selection *out);

/* The returned view and all its storage remain immutable until free.
 * Free only after every reader has stopped using the view. NULL is allowed. */
const dnd_source_index *dnd_source_artifact_index(const dnd_source_artifact *artifact);
/* V1 has no passages. V2 owns rebuilt, hash-checked passages and definitions.
 * NULL artifact yields NULL and count zero. Count itself may be NULL. */
const dnd_source_passage *dnd_source_artifact_passages(const dnd_source_artifact *artifact, size_t *count);
const dnd_source_passage_definition *dnd_source_artifact_passage_definitions(
    const dnd_source_artifact *artifact, size_t *count);
void dnd_source_artifact_free(dnd_source_artifact *artifact);

#endif
