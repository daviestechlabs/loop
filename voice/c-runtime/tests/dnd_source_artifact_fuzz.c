#include "dnd_source_passage_fixture.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void exercise_manifest(const uint8_t *data, size_t size) {
    source_fixture f;
    artifact_fixture a;
    char manifest[1024], identity[256], hash[65];
    fixture_make(&f);
    for (size_t i = 0; i < f.data.document_count; ++i) {
        int n = snprintf(identity, sizeof(identity), "%s:%zu.pdf", f.data.corpus_version, i);
        assert(n > 0 && (size_t)n < sizeof(identity));
        fixture_hash(identity, (size_t)n, hash);
        memcpy(f.documents[i].document_id, hash, 24); f.documents[i].document_id[24] = 0;
    }
    artifact_make(&a, &f);
    int n = snprintf(manifest, sizeof(manifest),
        "{\"schema_version\":\"dnd-corpus-manifest/v1\",\"corpus_id\":\"%s\",\"enabled_sources\":2,\"sources\":["
        "{\"enabled\":true,\"book_slug\":\"players-handbook\",\"source_sha256\":\"%s\",\"etag\":\"\",\"source_key\":\"0.pdf\"},"
        "{\"enabled\":true,\"book_slug\":\"monster-manual\",\"source_sha256\":\"%s\",\"etag\":\"\",\"source_key\":\"1.pdf\"}]}",
        f.data.corpus_version, f.documents[0].source_sha256, f.documents[1].source_sha256);
    assert(n > 0 && (size_t)n < sizeof(manifest));
    size_t length = (size_t)n;
    if (size >= 3) {
        size_t start = ((size_t)data[0] * 256u + data[1]) % length;
        for (size_t i = 2; i < size; ++i) manifest[(start + i - 2) % length] ^= (char)data[i];
    }
    const void *input = size && data[0] & 1 ? (const void *)data : manifest;
    size_t input_length = input == (const void *)data ? size : length;
    fixture_hash(input, input_length, a.expected.manifest_sha256);
    (void)SHA256(input, input_length, a.bytes + 8);
    artifact_rehash(&a);
    a.expected.documents = NULL; a.expected.document_count = 0;
    dnd_source_artifact *loaded = NULL;
    int rc = dnd_source_artifact_decode_reviewed(a.bytes, a.length, input, input_length, &a.expected, &loaded);
    if (rc) { if (loaded) abort(); }
    else {
        const dnd_source_index *index = dnd_source_artifact_index(loaded);
        if (!index || index->data->record_count != 4 || index->data->document_count != 2) abort();
        dnd_source_artifact_free(loaded);
    }
}

static void exercise(const void *bytes, size_t length, dnd_source_artifact_expectation expected, const source_fixture *f) {
    fixture_hash(bytes, length, expected.artifact_sha256);
    dnd_source_artifact *loaded = NULL;
    int result = dnd_source_artifact_decode(bytes, length, &expected, &loaded);
    if (result) { if (loaded) abort(); return; }
    const dnd_source_index *index = dnd_source_artifact_index(loaded);
    if (!index || !index->data || index->data->record_count > DND_SOURCE_RECORDS_MAX) abort();
    const char *q = "How often can Warding Step work per turn?";
    dnd_source_selection selection;
    if (dnd_source_index_select(index, &f->scope, f->data.corpus_version, q, strlen(q), f->anchors, f->anchor_count, &selection)) abort();
    if (selection.count > DND_RAG_HITS_MAX) abort();
    for (size_t i = 0; i < selection.count; ++i) {
        if (selection.records[i] >= index->data->record_count) abort();
        for (size_t j = 0; j < i; ++j) if (selection.records[i] == selection.records[j]) abort();
    }
    size_t count = 0;
    const dnd_source_passage *passages = dnd_source_artifact_passages(loaded, &count);
    if (count > DND_SOURCE_PASSAGES_MAX || (count && !passages)) abort();
    for (size_t i = 0; i < count; ++i) {
        const dnd_source_passage *p = &passages[i];
        char joined[DND_SOURCE_PASSAGE_TEXT_CAP] = {0}, hash[65];
        size_t used = 0; uint32_t page = 0;
        if (!p->count || p->count > DND_SOURCE_PASSAGE_SPANS_MAX) abort();
        for (size_t j = 0; j < p->count; ++j) {
            const dnd_source_passage_span *span = &p->spans[j];
            if (span->record >= index->data->record_count) abort();
            const dnd_source_record *record = &index->data->records[span->record];
            if (span->begin >= span->end || span->end > record->content_len || record->document != p->document) abort();
            if (j && page != record->page) { if (used + 1u >= sizeof(joined)) abort(); joined[used++] = '\n'; }
            size_t n = span->end - span->begin;
            if (n >= sizeof(joined) - used) abort();
            memcpy(joined + used, record->content + span->begin, n); used += n; page = record->page;
        }
        fixture_hash(joined, used, hash);
        if (used != p->length || memcmp(joined, p->text, used) || strcmp(hash, p->text_sha256)) abort();
    }
    dnd_source_artifact_free(loaded);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    source_fixture f;
    artifact_fixture a;
    fixture_make(&f); artifact_make(&a, &f);
    exercise(data, size, a.expected, &f);
    if (size >= 2) {
        size_t offset = ((size_t)data[0] * 256 + data[1]) % a.length;
        for (size_t i = 2; i < size; ++i) a.bytes[(offset + i - 2) % a.length] ^= data[i];
    }
    exercise(a.bytes, a.length, a.expected, &f);
    dnd_source_passage_definition defs[2];
    passage_fixture_make(&f, defs);
    size_t start = passage_artifact_make(&a, &f, defs);
    if (size >= 2) {
        size_t offset = data[0] & 1u ? start + data[1] % 192u
            : ((size_t)data[0] * 256u + data[1]) % a.length;
        for (size_t i = 2; i < size; ++i) a.bytes[(offset + i - 2) % a.length] ^= data[i];
    }
    exercise(a.bytes, a.length, a.expected, &f);
    exercise_manifest(data, size);
    return 0;
}
