#include "dnd_source_passage.h"
#include "dnd_source_spell.h"
#include "utf8.h"
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>

static int hash(const char *text, size_t length, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[32];
    if (!SHA256((const unsigned char *)text, length, bytes)) return 0;
    for (size_t i = 0; i < 32u; ++i) {
        out[2u * i] = hex[bytes[i] >> 4]; out[2u * i + 1u] = hex[bytes[i] & 15];
    }
    out[64] = 0;
    return 1;
}
static int hex64(const char *text) {
    if (text[64]) return 0;
    for (size_t i = 0; i < 64u; ++i)
        if (!((text[i] >= 'a' && text[i] <= 'f') || (text[i] >= '0' && text[i] <= '9'))) return 0;
    return 1;
}
static int letter(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int title(const char *text, size_t length, uint32_t begin, uint32_t end, char *name) {
    size_t used = 0, caps = 0, words = 0, singles = 0;
    if (begin >= end || end >= length || (begin && letter((unsigned char)text[begin - 1u])) ||
        letter((unsigned char)text[end]) ||
        !utf8_validate_v1((const unsigned char *)text + begin, end - begin)) return 0;
    for (size_t i = begin; i < end;) {
        if (!letter((unsigned char)text[i])) { ++i; continue; }
        size_t count = 0;
        while (i < end && letter((unsigned char)text[i])) {
            if (used + 1u == DND_SOURCE_NAME_CAP) return 0;
            unsigned char c = (unsigned char)text[i++];
            caps += c >= 'A' && c <= 'Z';
            name[used++] = (char)dnd_spell_lower(c); ++count;
        }
        ++words; singles += count == 1u;
    }
    name[used] = 0;
    return used >= 4u && (caps * 4u >= used * 3u || (caps && singles * 4u >= words * 3u));
}
static size_t skip_spaces(const char *text, size_t begin, size_t end) {
    while (begin < end && text[begin] == ' ') ++begin;
    return begin;
}
static int page_valid(const dnd_source_passage_input *in, size_t page_index) {
    const dnd_source_passage_page *p = &in->pages[page_index];
    dnd_source_page_chunk chunks[DND_SOURCE_PAGE_CHUNKS_MAX];
    dnd_source_page_alignment alignment;
    if (!p->page || p->page > INT32_MAX || !p->text || !p->length || p->length > DND_SOURCE_PAGE_CAP ||
        !p->count || p->count > DND_SOURCE_PAGE_CHUNKS_MAX ||
        (page_index && p->page != in->pages[page_index - 1u].page + 1u)) return DND_SOURCE_PAGE_INVALID;
    for (size_t i = 0; i < p->count; ++i) {
        char actual[65];
        if (p->records[i] >= in->record_count) return DND_SOURCE_PAGE_INVALID;
        const dnd_source_record *r = &in->records[p->records[i]];
        if (r->document != in->document || r->page != p->page || r->chunk != i ||
            !r->content || !r->content_len || r->content_len >= DND_RAG_CONTENT_CAP ||
            !hex64(r->record_id) || !hex64(r->content_hash) ||
            !hash(r->content, r->content_len, actual) || strcmp(actual, r->content_hash)) return DND_SOURCE_PAGE_INVALID;
        chunks[i] = (dnd_source_page_chunk){r->content, r->content_len, r->chunk};
    }
    int result = dnd_source_page_align(p->text, p->length, chunks, p->count, &alignment);
    if (result != DND_SOURCE_PAGE_OK) return result;
    for (size_t i = 0; i < p->count; ++i)
        if (in->records[p->records[i]].begin != alignment.begin[i]) return DND_SOURCE_PAGE_INVALID;
    return DND_SOURCE_PAGE_OK;
}
static int cover(const dnd_source_passage_input *in, size_t page_index,
    size_t begin, size_t end, dnd_source_passage *out) {
    const dnd_source_passage_page *p = &in->pages[page_index];
    size_t cursor = begin;
    while (cursor < end) {
        size_t best = p->count, reach = cursor;
        for (size_t i = 0; i < p->count; ++i) {
            const dnd_source_record *r = &in->records[p->records[i]];
            size_t stop = r->begin + r->content_len;
            if (r->begin > cursor || stop <= cursor) continue;
            if (stop > reach || (stop == reach && best < p->count &&
                strcmp(r->record_id, in->records[p->records[best]].record_id) < 0)) {
                best = i; reach = stop;
            }
        }
        if (best == p->count) return DND_SOURCE_PAGE_INCOMPLETE;
        if (out->count == DND_SOURCE_PASSAGE_SPANS_MAX) return DND_SOURCE_PAGE_CAPACITY;
        const dnd_source_record *r = &in->records[p->records[best]];
        if (reach > end) reach = end;
        out->spans[out->count++] = (dnd_source_passage_span){p->records[best],
            (uint32_t)(cursor - r->begin), (uint32_t)(reach - r->begin)};
        cursor = reach;
    }
    return DND_SOURCE_PAGE_OK;
}
static int passage_identity(const dnd_source_passage_input *in, dnd_source_passage *out) {
    unsigned char bytes[2048]; size_t used = 0;
    static const char domain[] = "dnd-complete-passage/v1";
    memcpy(bytes, domain, sizeof(domain)); used += sizeof(domain);
    memcpy(bytes + used, out->text_sha256, 64); used += 64;
    for (size_t i = 0; i < out->count; ++i) {
        const dnd_source_passage_span *s = &out->spans[i];
        const dnd_source_record *r = &in->records[s->record];
        if (sizeof(bytes) - used < 80u) return 0;
        memcpy(bytes + used, r->record_id, 64); used += 64;
        const uint32_t values[] = {r->page, r->chunk, s->begin, s->end};
        for (size_t j = 0; j < 4; ++j)
            for (size_t k = 0; k < 4; ++k) bytes[used++] = (unsigned char)(values[j] >> (k * 8u));
    }
    return hash((const char *)bytes, used, out->passage_id);
}

int dnd_source_compile_passage(const dnd_source_passage_input *in, dnd_source_passage *out) {
    dnd_source_passage result = {0};
    char next_name[DND_SOURCE_NAME_CAP];
    if (!out) return DND_SOURCE_PAGE_INVALID;
    memset(out, 0, sizeof(*out));
    if (!in || in->document >= DND_SOURCE_DOCUMENTS_MAX || !in->records || !in->record_count ||
        in->record_count > DND_SOURCE_RECORDS_MAX || !in->pages || !in->page_count ||
        in->page_count > DND_SOURCE_PASSAGE_PAGES_MAX) return DND_SOURCE_PAGE_INVALID;
    for (size_t i = 0; i < in->record_count; ++i)
        if (!hex64(in->records[i].record_id) || (i && strcmp(in->records[i - 1u].record_id,
            in->records[i].record_id) >= 0)) return DND_SOURCE_PAGE_INVALID;
    for (size_t i = 0; i < in->page_count; ++i) {
        int status = page_valid(in, i);
        if (status != DND_SOURCE_PAGE_OK) return status;
    }
    const dnd_source_passage_page *first = &in->pages[0], *last = &in->pages[in->page_count - 1u];
    if (!title(first->text, first->length, in->heading_begin, in->heading_end, result.name) ||
        !title(last->text, last->length, in->next_heading_begin, in->next_heading_end, next_name) ||
        (in->page_count == 1u && in->heading_end >= in->next_heading_begin)) return DND_SOURCE_PAGE_INVALID;
    size_t next_header = skip_spaces(last->text, in->next_heading_end, last->length);
    if (!dnd_source_spell_header(last->text + next_header, last->length - next_header)) return DND_SOURCE_PAGE_INCOMPLETE;
    for (size_t i = 0; i < in->page_count; ++i) {
        const dnd_source_passage_page *p = &in->pages[i];
        size_t begin = i ? 0 : in->heading_begin;
        size_t end = i + 1u == in->page_count ? in->next_heading_begin : p->length;
        while (end > begin && p->text[end - 1u] == ' ') --end;
        if (begin == end && i && i + 1u == in->page_count) continue;
        if (begin >= end) return DND_SOURCE_PAGE_INCOMPLETE;
        size_t length = end - begin;
        if (length + (i != 0) >= sizeof(result.text) - result.length) return DND_SOURCE_PAGE_CAPACITY;
        if (!utf8_validate_v1((const unsigned char *)p->text + begin, length)) return DND_SOURCE_PAGE_INVALID;
        int status = cover(in, i, begin, end, &result);
        if (status != DND_SOURCE_PAGE_OK) return status;
        if (i) result.text[result.length++] = '\n';
        memcpy(result.text + result.length, p->text + begin, length); result.length += length;
    }
    size_t header = skip_spaces(result.text, in->heading_end - in->heading_begin, result.length);
    if (!dnd_source_spell_header(result.text + header, result.length - header) ||
        !strchr(".!?", result.text[result.length - 1u])) return DND_SOURCE_PAGE_INCOMPLETE;
    for (size_t i = header + 1u; i < result.length; ++i)
        if ((result.text[i - 1u] == ' ' || result.text[i - 1u] == '\n') &&
            dnd_source_spell_header(result.text + i, result.length - i)) return DND_SOURCE_PAGE_AMBIGUOUS;
    result.document = in->document; result.page_start = first->page; result.page_end = in->records[result.spans[result.count - 1u].record].page;
    if (!hash(result.text, result.length, result.text_sha256) || !passage_identity(in, &result)) return DND_SOURCE_PAGE_INVALID;
    *out = result;
    return DND_SOURCE_PAGE_OK;
}

int dnd_source_passage_definitions_valid(const dnd_source_passage_definition *defs,
    size_t count, size_t document_count) {
    if (!defs || !count || count > DND_SOURCE_PASSAGES_MAX ||
        !document_count || document_count > DND_SOURCE_DOCUMENTS_MAX) return 0;
    for (size_t i = 0; i < count; ++i) {
        const dnd_source_passage_definition *d = &defs[i];
        if (d->document >= document_count || !d->first_page || d->first_page > d->last_page ||
            d->last_page > INT32_MAX || d->last_page - d->first_page >= DND_SOURCE_PASSAGE_PAGES_MAX ||
            d->heading_begin >= d->heading_end || d->heading_end >= DND_SOURCE_PAGE_CAP ||
            d->next_heading_begin >= d->next_heading_end || d->next_heading_end >= DND_SOURCE_PAGE_CAP ||
            (d->first_page == d->last_page && d->heading_end >= d->next_heading_begin)) return 0;
        if (i) {
            const dnd_source_passage_definition *prev = &defs[i - 1u];
            if (d->document < prev->document || (d->document == prev->document &&
                (d->first_page < prev->first_page || (d->first_page == prev->first_page &&
                d->heading_begin <= prev->heading_begin)))) return 0;
        }
    }
    return 1;
}

typedef struct {
    dnd_source_passage_page pages[DND_SOURCE_PASSAGE_PAGES_MAX];
    char text[DND_SOURCE_PASSAGE_PAGES_MAX][DND_SOURCE_PAGE_CAP];
    unsigned char seen[DND_SOURCE_PASSAGE_PAGES_MAX][DND_SOURCE_PAGE_CAP];
} passage_workspace;

int dnd_source_rebuild_passage(const dnd_source_record *records, size_t record_count,
    const dnd_source_passage_definition *d, dnd_source_passage *out) {
    if (!out) return DND_SOURCE_PAGE_INVALID;
    memset(out, 0, sizeof(*out));
    if (!records || !record_count || record_count > DND_SOURCE_RECORDS_MAX ||
        !dnd_source_passage_definitions_valid(d, 1, DND_SOURCE_DOCUMENTS_MAX)) return DND_SOURCE_PAGE_INVALID;
    passage_workspace *work = calloc(1, sizeof(*work));
    if (!work) return DND_SOURCE_PAGE_CAPACITY;
    int status = DND_SOURCE_PAGE_INVALID;
    dnd_source_passage_input in = {
        .document = d->document, .records = records, .record_count = record_count,
        .pages = work->pages, .page_count = d->last_page - d->first_page + 1u,
        .heading_begin = d->heading_begin, .heading_end = d->heading_end,
        .next_heading_begin = d->next_heading_begin, .next_heading_end = d->next_heading_end
    };
    for (size_t i = 0; i < in.page_count; ++i) {
        work->pages[i].page = d->first_page + (uint32_t)i; work->pages[i].text = work->text[i];
        for (size_t j = 0; j < DND_SOURCE_PAGE_CHUNKS_MAX; ++j) work->pages[i].records[j] = UINT32_MAX;
    }
    for (size_t i = 0; i < record_count; ++i) {
        const dnd_source_record *r = &records[i];
        if (r->document != d->document || r->page < d->first_page || r->page > d->last_page) continue;
        size_t page = r->page - d->first_page;
        dnd_source_passage_page *p = &work->pages[page];
        if (!r->content || !r->content_len || r->content_len >= DND_RAG_CONTENT_CAP ||
            r->chunk >= DND_SOURCE_PAGE_CHUNKS_MAX || p->records[r->chunk] != UINT32_MAX ||
            r->begin >= DND_SOURCE_PAGE_CAP || r->content_len > DND_SOURCE_PAGE_CAP - r->begin) goto done;
        p->records[r->chunk] = (uint32_t)i; ++p->count;
        size_t end = r->begin + r->content_len; if (end > p->length) p->length = end;
        for (size_t at = r->begin; at < end; ++at) {
            char value = r->content[at - r->begin];
            if (work->seen[page][at] && work->text[page][at] != value) goto done;
            work->seen[page][at] = 1; work->text[page][at] = value;
        }
    }
    for (size_t i = 0; i < in.page_count; ++i) {
        const dnd_source_passage_page *p = &work->pages[i];
        if (!p->count || !p->length || memchr(work->seen[i], 0, p->length)) goto done;
        for (size_t j = 0; j < p->count; ++j) if (p->records[j] == UINT32_MAX) goto done;
    }
    status = dnd_source_compile_passage(&in, out);
done:
    free(work);
    return status;
}
