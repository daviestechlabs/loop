#include "dnd_source_encode.h"
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *bytes; size_t used; int failed; } writer;
static void bytes(writer *w, const void *p, size_t n) {
    if (w->failed) return;
    if (n > DND_SOURCE_ARTIFACT_BYTES_MAX - w->used) { w->failed = 1; return; }
    if (w->bytes) memcpy(w->bytes + w->used, p, n);
    w->used += n;
}
static void number(writer *w, uint32_t n) {
    unsigned char p[4];
    for (size_t i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8u));
    bytes(w, p, sizeof(p));
}
static void string(writer *w, const char *p, size_t n) {
    number(w, (uint32_t)n); bytes(w, p, n);
}
static void group(writer *w, const dnd_source_group *g) {
    number(w, g->begin); number(w, g->end); number(w, g->count);
    for (size_t i = 0; i < DND_SOURCE_SPANS_MAX; ++i) {
        number(w, g->spans[i].record); number(w, g->spans[i].begin); number(w, g->spans[i].end);
    }
}
static int hash_bytes(const char *s, unsigned char out[32]) {
    if (!s || s[64]) return -1;
    for (size_t i = 0; i < 64; ++i) {
        unsigned value;
        if (s[i] >= '0' && s[i] <= '9') value = (unsigned)(s[i] - '0');
        else if (s[i] >= 'a' && s[i] <= 'f') value = (unsigned)(s[i] - 'a' + 10);
        else return -1;
        if (i & 1u) out[i / 2u] |= (unsigned char)value;
        else out[i / 2u] = (unsigned char)(value << 4);
    }
    return 0;
}
static void encode(writer *w, const dnd_source_data *d, const unsigned char hashes[64],
    const dnd_source_passage_definition *defs, size_t count, const char (*assemblies)[65]) {
    bytes(w, count ? "DNDSIDX2" : "DNDSIDX1", 8); bytes(w, hashes, 64);
    string(w, d->collection, strlen(d->collection));
    string(w, d->corpus_version, strlen(d->corpus_version));
    string(w, d->ruleset, strlen(d->ruleset));
    number(w, (uint32_t)d->document_count); number(w, (uint32_t)d->record_count); number(w, (uint32_t)d->entry_count);
    if (count) number(w, (uint32_t)count);
    for (size_t i = 0; i < d->document_count; ++i) {
        const dnd_source_document *doc = &d->documents[i];
        string(w, doc->document_id, strlen(doc->document_id));
        string(w, doc->book_slug, strlen(doc->book_slug));
        string(w, doc->source_sha256, 64);
    }
    for (size_t i = 0; i < d->record_count; ++i) {
        const dnd_source_record *r = &d->records[i];
        string(w, r->record_id, 64); string(w, r->content_hash, 64);
        number(w, r->document); number(w, r->page); number(w, r->chunk); number(w, r->begin);
        string(w, r->content, r->content_len);
    }
    for (size_t i = 0; i < d->entry_count; ++i) {
        const dnd_source_entry *e = &d->entries[i];
        string(w, e->name, strlen(e->name)); string(w, e->alias, strlen(e->alias));
        number(w, e->document); number(w, e->page); number(w, e->kind); number(w, e->flags);
        group(w, &e->heading); group(w, &e->body); group(w, &e->opening);
        for (size_t j = 0; j < DND_SOURCE_FIELDS; ++j) group(w, &e->fields[j]);
    }
    for (size_t i = 0; i < count; ++i) {
        const dnd_source_passage_definition *p = &defs[i];
        number(w, p->document); number(w, p->first_page); number(w, p->last_page);
        number(w, p->heading_begin); number(w, p->heading_end);
        number(w, p->next_heading_begin); number(w, p->next_heading_end);
        string(w, assemblies[i], 64);
    }
}
static int encode_all(const dnd_source_data *d,
    const dnd_source_passage_definition *defs, size_t count, const char manifest_sha256[65],
    const char compiler_sha256[65], unsigned char **out, size_t *length) {
    unsigned char hashes[64];
    dnd_source_index index;
    if (!out || *out || !length || *length || hash_bytes(manifest_sha256, hashes) ||
        hash_bytes(compiler_sha256, hashes + 32) || dnd_source_index_admit(d, &index)) return -1;
    char (*assemblies)[65] = NULL;
    if (count) {
        if (!dnd_source_passage_definitions_valid(defs, count, d->document_count)) return -1;
        assemblies = calloc(count, sizeof(*assemblies));
        if (!assemblies) return -1;
        for (size_t i = 0; i < count; ++i) {
            dnd_source_passage passage;
            if (dnd_source_rebuild_passage(d->records, d->record_count, &defs[i], &passage)) {
                free(assemblies); return -1;
            }
            memcpy(assemblies[i], passage.text_sha256, 65);
        }
    }
    writer w = {0};
    encode(&w, d, hashes, defs, count, (const char (*)[65])assemblies);
    if (w.failed) { free(assemblies); return -1; }
    size_t n = w.used;
    w.bytes = malloc(n);
    if (!w.bytes) { free(assemblies); return -1; }
    w.used = 0;
    encode(&w, d, hashes, defs, count, (const char (*)[65])assemblies);
    free(assemblies);
    if (w.failed || w.used != n) { free(w.bytes); return -1; }
    *out = w.bytes; *length = n;
    return 0;
}
int dnd_source_encode(const dnd_source_data *d, const char manifest_sha256[65],
    const char compiler_sha256[65], unsigned char **out, size_t *length) {
    return encode_all(d, NULL, 0, manifest_sha256, compiler_sha256, out, length);
}
int dnd_source_encode_passages(const dnd_source_data *d,
    const dnd_source_passage_definition *defs, size_t count, const char manifest_sha256[65],
    const char compiler_sha256[65], unsigned char **out, size_t *length) {
    if (!count) return -1;
    return encode_all(d, defs, count, manifest_sha256, compiler_sha256, out, length);
}
