#ifndef VOICE_C_DND_SOURCE_PAGE_H
#define VOICE_C_DND_SOURCE_PAGE_H

#include <stddef.h>
#include <stdint.h>

enum { DND_SOURCE_PAGE_CHUNKS_MAX = 128, DND_SOURCE_PAGE_MATCHES_MAX = 128 };
enum {
    DND_SOURCE_PAGE_OK = 0,
    DND_SOURCE_PAGE_INVALID = -1,
    DND_SOURCE_PAGE_INCOMPLETE = 1,
    DND_SOURCE_PAGE_AMBIGUOUS = 2,
    DND_SOURCE_PAGE_CAPACITY = 3
};

typedef struct {
    const char *text;
    size_t length;
    uint32_t chunk;
} dnd_source_page_chunk;

typedef struct {
    uint32_t begin[DND_SOURCE_PAGE_CHUNKS_MAX];
    size_t count;
} dnd_source_page_alignment;

/* Offline compilation only. Match admitted record bytes against the exact
 * normalized source page, rather than guessing a join from suffix overlap.
 * Chunk indices start at zero and remain consecutive. A valid alignment starts
 * at page byte zero, covers the complete page, and advances monotonically.
 * Every chunk can have at most 128 exact occurrences. Count complete alignment
 * paths, saturating at two; publish offsets only for a unique complete path.
 * Page bytes are at most 65536; each chunk contains 1..8191 bytes of UTF-8.
 * The caller separately verifies PDF, extraction, document and record hashes.
 * Inputs stay immutable during the call. Bounded temporary storage is owned
 * and freed by this call. Any failure clears the complete output. */
int dnd_source_page_align(const char *page, size_t length,
    const dnd_source_page_chunk *chunks, size_t count, dnd_source_page_alignment *out);

#endif
