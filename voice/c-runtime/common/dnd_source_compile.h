#ifndef VOICE_C_DND_SOURCE_COMPILE_H
#define VOICE_C_DND_SOURCE_COMPILE_H

#include "dnd_source_index.h"
#include "dnd_source_page.h"

enum { DND_SOURCE_HEADINGS_MAX = 128 };
typedef struct { char text[DND_SOURCE_NAME_CAP]; } dnd_source_heading;
typedef struct {
    uint32_t parent_begin, parent_end, heading_begin, heading_end, body_end;
} dnd_source_section_hint;
typedef struct {
    dnd_source_page_alignment alignment;
    dnd_source_entry entries[DND_SOURCE_HEADINGS_MAX];
    size_t count;
} dnd_source_compiled_page;

/* Offline operation. Records arrive in chunk order. Their begin fields are
 * ignored: unique alignment to the original normalized page determines them.
 * The caller separately pins the source PDF, extractor and heading hints.
 * Record hashes and exact page bytes are checked here. Heading hints only
 * locate source text; they cannot supply answer text or numeric fields.
 * All records must belong to the supplied document/page. Output spans refer
 * to local record indices; the producer remaps them when sorting its table.
 * Returns a DND_SOURCE_PAGE_* status. Failure clears the complete output.
 * Success can contain zero entries. This is a bounded heuristic compiler,
 * not complete book coverage or a semantic correctness certificate. */
int dnd_source_compile_page(const char *text, size_t length,
    uint32_t document, uint32_t page, uint32_t kind,
    const dnd_source_record *records, size_t record_count,
    const dnd_source_heading *headings, size_t heading_count,
    dnd_source_compiled_page *out);

/* Reviewed byte ranges identify a parent heading and an inline subsection.
 * The body starts after the child heading and ends at body_end. Names, flags,
 * answer text and record spans are derived from the pinned page bytes only.
 * Ranges validate source identity, not semantic hierarchy: the operator must
 * review the relationship, including PDF sidebars and reading-order changes.
 * Invalid or duplicate descriptors reject the whole page and clear output. */
int dnd_source_compile_page_sections(const char *text, size_t length,
    uint32_t document, uint32_t page, uint32_t kind,
    const dnd_source_record *records, size_t record_count,
    const dnd_source_heading *headings, size_t heading_count,
    const dnd_source_section_hint *sections, size_t section_count,
    dnd_source_compiled_page *out);

#endif
