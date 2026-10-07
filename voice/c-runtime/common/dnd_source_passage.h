#ifndef VOICE_C_DND_SOURCE_PASSAGE_H
#define VOICE_C_DND_SOURCE_PASSAGE_H

#include "dnd_source_index.h"
#include "dnd_source_page.h"

enum {
    DND_SOURCE_PASSAGES_MAX = 4096,
    DND_SOURCE_PASSAGE_PAGES_MAX = 3,
    DND_SOURCE_PASSAGE_SPANS_MAX = 16,
    DND_SOURCE_PASSAGE_TEXT_CAP = 4097
};

/* Source coordinates only; no caller-supplied answer text or record hashes. */
typedef struct {
    uint32_t document, first_page, last_page;
    uint32_t heading_begin, heading_end, next_heading_begin, next_heading_end;
} dnd_source_passage_definition;

/* Definitions must be strictly ordered by document, first page, heading begin.
 * A duplicate start is rejected even when its closing boundary differs. */
int dnd_source_passage_definitions_valid(const dnd_source_passage_definition *definitions,
    size_t count, size_t document_count);

typedef struct {
    uint32_t page;
    const char *text;
    size_t length, count;
    /* Every original page record, in consecutive chunk order. */
    uint32_t records[DND_SOURCE_PAGE_CHUNKS_MAX];
} dnd_source_passage_page;

typedef struct {
    uint32_t document;
    /* Same strict record-ID ordering as the source index. */
    const dnd_source_record *records;
    size_t record_count;
    const dnd_source_passage_page *pages;
    size_t page_count;
    /* Reviewed title ranges on the first and last supplied pages. The next
     * title bounds the passage; a page edge alone never proves completion. */
    uint32_t heading_begin, heading_end, next_heading_begin, next_heading_end;
} dnd_source_passage_input;

typedef struct {
    /* Half-open offsets within the complete original record. */
    uint32_t record, begin, end;
} dnd_source_passage_span;

typedef struct {
    uint32_t document, page_start, page_end;
    char name[DND_SOURCE_NAME_CAP];
    dnd_source_passage_span spans[DND_SOURCE_PASSAGE_SPANS_MAX];
    size_t count, length;
    /* Exact source slices in page order, with one newline between pages.
     * This hash identifies that assembly, not any original backend record. */
    char text[DND_SOURCE_PASSAGE_TEXT_CAP], text_sha256[65];
    /* Domain-separated hash of assembly hash and original record/range witnesses. */
    char passage_id[65];
} dnd_source_passage;

/* Offline source compiler foundation. It checks full original record hashes,
 * unique page alignment, declared offsets, consecutive pages, source-shaped
 * spell headers, complete endings, intervening spell headers, and byte limits.
 * Original page text and record bytes stay unchanged. Boundary hints remain
 * operator-reviewed source assertions, not a proof of PDF reading order.
 * Output record indices refer to input.records. Minimum coverage uses furthest
 * reach, with record ID tie-breaking. Unused output fields stay zero.
 * Returns DND_SOURCE_PAGE_*; every failure clears the complete output.
 * No file, network, or model access. This API does not publish an index or
 * change runtime citation limits. Existing DNDSIDX1 has no passage table. */
int dnd_source_compile_passage(const dnd_source_passage_input *input,
    dnd_source_passage *out);

/* Reconstruct consecutive pages from their complete original records, then
 * compile. Startup/offline only: bounded workspace allocation; no I/O.
 * Rejects missing chunks, gaps, disagreeing overlaps, and invalid lengths.
 * Failure clears out. Input storage stays caller-owned and immutable. */
int dnd_source_rebuild_passage(const dnd_source_record *records, size_t record_count,
    const dnd_source_passage_definition *definition, dnd_source_passage *out);

#endif
