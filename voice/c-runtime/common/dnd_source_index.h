#ifndef VOICE_C_DND_SOURCE_INDEX_H
#define VOICE_C_DND_SOURCE_INDEX_H

#include "dnd_retrieval.h"
#include <stddef.h>
#include <stdint.h>

enum {
    DND_SOURCE_DOCUMENTS_MAX = 128,
    DND_SOURCE_RECORDS_MAX = 32768,
    DND_SOURCE_ENTRIES_MAX = 16384,
    DND_SOURCE_NAME_CAP = 256,
    DND_SOURCE_PAGE_CAP = 65536,
    DND_SOURCE_SPANS_MAX = 2,
    DND_SOURCE_FIELDS = 5,
    DND_SOURCE_QUERY_CAP = 2048
};

enum { DND_SOURCE_RULE = 1, DND_SOURCE_CREATURE = 2, DND_SOURCE_SECTION = 3 };
enum { DND_SOURCE_ONCE_PER_TURN = 1, DND_SOURCE_OTHER_TURN_REACTION = 2,
    DND_SOURCE_SPELL_HEADER = 4 };
enum {
    DND_SOURCE_NO_MATCH,
    DND_SOURCE_SELECTED,
    DND_SOURCE_AMBIGUOUS,
    DND_SOURCE_INCOMPLETE,
    DND_SOURCE_CAPACITY
};

typedef struct {
    char document_id[128], book_slug[128], source_sha256[65];
} dnd_source_document;

typedef struct {
    char record_id[65], content_hash[65];
    uint32_t document, page, chunk, begin;
    const char *content;
    size_t content_len;
} dnd_source_record;

typedef struct { uint32_t record, begin, end; } dnd_source_span;
typedef struct {
    uint32_t begin, end, count;
    dnd_source_span spans[DND_SOURCE_SPANS_MAX];
} dnd_source_group;

typedef struct {
    /* Rules and sections use compact ASCII heading letters. Creatures retain
     * word spaces. A section alias is its source-derived parent heading name.
     * The optional creature alias only collapses repeated adjacent letters. */
    char name[DND_SOURCE_NAME_CAP], alias[DND_SOURCE_NAME_CAP];
    uint32_t document, page, kind, flags;
    dnd_source_group heading, body, opening;
    /* Creatures: Armor Class, Hit Points, speed, Challenge Rating, immunities.
     * Sections: fields[0] is the parent heading; all other fields are empty. */
    dnd_source_group fields[DND_SOURCE_FIELDS];
} dnd_source_entry;

typedef struct {
    char collection[128], corpus_version[128], ruleset[128];
    const dnd_source_document *documents;
    size_t document_count;
    /* Records are strictly sorted by record_id; entries retain compiler order. */
    const dnd_source_record *records;
    size_t record_count;
    const dnd_source_entry *entries;
    size_t entry_count;
} dnd_source_data;

typedef struct { const dnd_source_data *data; } dnd_source_index;

/* An anchor comes from an already authorized hit, matched by exact document
 * and source identity. UINT32_MAX denotes a document outside this index. */
typedef struct { uint32_t document, page; } dnd_source_anchor;
typedef struct {
    uint32_t records[DND_RAG_HITS_MAX];
    dnd_rag_excerpt excerpts[DND_RAG_HITS_MAX];
    size_t count;
    int reason, replace_baseline;
} dnd_source_selection;

/* Admission runs once, outside the query kernel. It recomputes content hashes
 * and validates names and complete spans against the original record bytes.
 * Source-file hashes and page offsets remain compiler assertions. The caller
 * must separately bind that compiler artifact to its reviewed source manifest.
 * All arrays and content bytes stay immutable and alive while the index is used.
 * The caller owns all index storage. Returns zero on success; rejection clears out. */
int dnd_source_index_admit(const dnd_source_data *data, dnd_source_index *out);

/* The scope is server-derived. Only shared-rulebook scope is supported.
 * Corpus, collection, ruleset and book access must match before name selection.
 * Named rules use exact anchored pages. Only when no name matches those pages
 * does selection recover matching rules from other pages in those documents.
 * Hit order breaks ties between documents; distinct passages tied within one
 * document remain ambiguous. No rule fallback reaches an unanchored document.
 * Qualified sections can match without anchors. They retain every allowed
 * printing in document-ID order, or reject the whole union on citation overflow.
 * Distinct sections and repeated locations within one document remain ambiguous.
 * Returns original record indices and optional record-local byte ranges.
 * Named spells include their exact heading and body, excluding neighboring text.
 * The caller fetches and revalidates those records through the normal scoped
 * retrieval boundary. replace_baseline selects only those records; otherwise
 * the caller fills spare slots with its admitted baseline hits.
 * Ambiguity, missing required spans and capacity limits yield no additions.
 * Invalid arguments return -1 and clear out. No allocation, hashing or I/O. */
int dnd_source_index_select(
    const dnd_source_index *index, const dnd_rag_scope *scope, const char *corpus_version,
    const char *query, size_t query_len, const dnd_source_anchor *anchors,
    size_t anchor_count, dnd_source_selection *out);

#endif
