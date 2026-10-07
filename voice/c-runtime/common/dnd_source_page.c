#include "dnd_source_page.h"
#include "dnd_source_index.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t begin;
    uint16_t previous;
    uint8_t paths;
} position;

static int valid_bytes(const char *text, size_t length, size_t maximum) {
    return text && length && length <= maximum && !memchr(text, 0, length) &&
        utf8_validate_v1((const uint8_t *)text, length);
}

int dnd_source_page_align(const char *page, size_t length,
    const dnd_source_page_chunk *chunks, size_t count, dnd_source_page_alignment *out) {
    size_t sizes[DND_SOURCE_PAGE_CHUNKS_MAX] = {0};
    position *nodes = NULL;
    int result = DND_SOURCE_PAGE_INVALID;
    if (!out) return result;
    memset(out, 0, sizeof(*out));
    if (!valid_bytes(page, length, DND_SOURCE_PAGE_CAP) || !chunks || !count ||
        count > DND_SOURCE_PAGE_CHUNKS_MAX) return result;
    for (size_t i = 0; i < count; ++i) {
        if (!valid_bytes(chunks[i].text, chunks[i].length, DND_RAG_CONTENT_CAP - 1u)) return result;
        if (chunks[i].chunk != i) return DND_SOURCE_PAGE_INCOMPLETE;
    }
    nodes = calloc(count * DND_SOURCE_PAGE_MATCHES_MAX, sizeof(*nodes));
    if (!nodes) return DND_SOURCE_PAGE_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        const dnd_source_page_chunk *chunk = &chunks[i];
        position *row = nodes + i * DND_SOURCE_PAGE_MATCHES_MAX;
        if (chunk->length > length) { result = DND_SOURCE_PAGE_INCOMPLETE; goto done; }
        size_t cursor = 0, last = length - chunk->length;
        while (cursor <= last) {
            const char *found = memchr(page + cursor, (unsigned char)chunk->text[0], last - cursor + 1u);
            if (!found) break;
            size_t at = (size_t)(found - page);
            cursor = at + 1u;
            if (memcmp(found, chunk->text, chunk->length)) continue;
            if (sizes[i] == DND_SOURCE_PAGE_MATCHES_MAX) { result = DND_SOURCE_PAGE_CAPACITY; goto done; }
            position *node = &row[sizes[i]++];
            node->begin = (uint32_t)at;
            if (!i) {
                node->paths = (uint8_t)(at == 0);
                continue;
            }
            const position *previous = nodes + (i - 1u) * DND_SOURCE_PAGE_MATCHES_MAX;
            for (size_t j = 0; j < sizes[i - 1u]; ++j) {
                const position *p = &previous[j];
                size_t end = p->begin + chunks[i - 1u].length;
                if (!p->paths || at < p->begin || at > end || at + chunk->length < end) continue;
                unsigned paths = (unsigned)node->paths + p->paths;
                node->paths = (uint8_t)(paths > 1u ? 2u : paths);
                node->previous = (uint16_t)j;
            }
        }
        if (!sizes[i]) { result = DND_SOURCE_PAGE_INCOMPLETE; goto done; }
    }
    const position *last_row = nodes + (count - 1u) * DND_SOURCE_PAGE_MATCHES_MAX;
    unsigned paths = 0;
    size_t selected = 0;
    for (size_t i = 0; i < sizes[count - 1u]; ++i) {
        if (last_row[i].begin + chunks[count - 1u].length != length) continue;
        paths += last_row[i].paths;
        if (paths > 1u) { result = DND_SOURCE_PAGE_AMBIGUOUS; goto done; }
        if (last_row[i].paths) selected = i;
    }
    if (!paths) { result = DND_SOURCE_PAGE_INCOMPLETE; goto done; }
    for (size_t i = count; i-- > 0;) {
        const position *node = nodes + i * DND_SOURCE_PAGE_MATCHES_MAX + selected;
        out->begin[i] = node->begin;
        selected = node->previous;
    }
    out->count = count;
    result = DND_SOURCE_PAGE_OK;
done:
    free(nodes);
    return result;
}
