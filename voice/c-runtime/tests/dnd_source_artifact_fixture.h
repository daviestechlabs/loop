#ifndef DND_SOURCE_ARTIFACT_FIXTURE_H
#define DND_SOURCE_ARTIFACT_FIXTURE_H
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "dnd_source_artifact.h"
#include "dnd_source_fixture.h"
#include <assert.h>

/* Independent reference writer for the documented wire format. Test data only. */
typedef struct {
    unsigned char bytes[8192];
    size_t length, counts, docs[4], records[16], entries[8];
    dnd_source_artifact_expectation expected;
} artifact_fixture;

static inline void artifact_u32(artifact_fixture *a, uint32_t n) {
    assert(a->length + 4 <= sizeof(a->bytes));
    for (size_t i = 0; i < 4; ++i) a->bytes[a->length++] = (unsigned char)(n >> (i * 8));
}

static inline void artifact_bytes(artifact_fixture *a, const void *p, size_t n) {
    assert(n <= sizeof(a->bytes) - a->length);
    memcpy(a->bytes + a->length, p, n); a->length += n;
}

static inline void artifact_string(artifact_fixture *a, const char *s) {
    artifact_u32(a, (uint32_t)strlen(s)); artifact_bytes(a, s, strlen(s));
}

static inline void artifact_group(artifact_fixture *a, const dnd_source_group *g) {
    artifact_u32(a, g->begin); artifact_u32(a, g->end); artifact_u32(a, g->count);
    for (size_t i = 0; i < DND_SOURCE_SPANS_MAX; ++i) {
        artifact_u32(a, g->spans[i].record); artifact_u32(a, g->spans[i].begin); artifact_u32(a, g->spans[i].end);
    }
}

static inline void artifact_rehash(artifact_fixture *a) {
    fixture_hash((const char *)a->bytes, a->length, a->expected.artifact_sha256);
}

static inline void artifact_make(artifact_fixture *a, const source_fixture *f) {
    memset(a, 0, sizeof(*a));
    memset(a->expected.manifest_sha256, 'c', 64);
    memset(a->expected.compiler_sha256, 'd', 64);
    strcpy(a->expected.collection, f->data.collection);
    strcpy(a->expected.corpus_version, f->data.corpus_version);
    strcpy(a->expected.ruleset, f->data.ruleset);
    a->expected.documents = f->documents; a->expected.document_count = f->data.document_count;
    artifact_bytes(a, "DNDSIDX1", 8);
    for (size_t i = 0; i < 32; ++i) artifact_bytes(a, "\xcc", 1);
    for (size_t i = 0; i < 32; ++i) artifact_bytes(a, "\xdd", 1);
    artifact_string(a, f->data.collection); artifact_string(a, f->data.corpus_version); artifact_string(a, f->data.ruleset);
    a->counts = a->length;
    artifact_u32(a, (uint32_t)f->data.document_count);
    artifact_u32(a, (uint32_t)f->data.record_count);
    artifact_u32(a, (uint32_t)f->data.entry_count);
    for (size_t i = 0; i < f->data.document_count; ++i) {
        const dnd_source_document *d = &f->documents[i]; a->docs[i] = a->length;
        artifact_string(a, d->document_id); artifact_string(a, d->book_slug); artifact_string(a, d->source_sha256);
    }
    for (size_t i = 0; i < f->data.record_count; ++i) {
        const dnd_source_record *r = &f->records[i]; a->records[i] = a->length;
        artifact_string(a, r->record_id); artifact_string(a, r->content_hash);
        artifact_u32(a, r->document); artifact_u32(a, r->page); artifact_u32(a, r->chunk); artifact_u32(a, r->begin);
        artifact_u32(a, (uint32_t)r->content_len); artifact_bytes(a, r->content, r->content_len);
    }
    for (size_t i = 0; i < f->data.entry_count; ++i) {
        const dnd_source_entry *e = &f->entries[i]; a->entries[i] = a->length;
        artifact_string(a, e->name); artifact_string(a, e->alias);
        artifact_u32(a, e->document); artifact_u32(a, e->page); artifact_u32(a, e->kind); artifact_u32(a, e->flags);
        artifact_group(a, &e->heading); artifact_group(a, &e->body); artifact_group(a, &e->opening);
        for (size_t j = 0; j < DND_SOURCE_FIELDS; ++j) artifact_group(a, &e->fields[j]);
    }
    artifact_rehash(a);
}
#endif
