/* Offline research: add complete spell entries without replacing any retained
 * document, record, or entry. Both artifacts need the same reviewed manifest. */
#include "dnd_source_encode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put(char *out, size_t capacity, const char *text) {
    size_t n = strlen(text);
    if (!n || n >= capacity) return -1;
    memcpy(out, text, n + 1u);
    return 0;
}
static void remap(dnd_source_group *group, const uint32_t *mapping) {
    for (size_t i = 0; i < group->count; ++i) group->spans[i].record = mapping[group->spans[i].record];
}
int main(int argc, char **argv) {
    dnd_source_artifact *original = NULL, *candidate = NULL;
    dnd_source_artifact_expectation expected = {0};
    dnd_source_entry *entries = NULL;
    uint32_t *mapping = NULL, documents[DND_SOURCE_DOCUMENTS_MAX];
    unsigned char *wire = NULL;
    size_t length = 0, added = 0;
    int result = 1;
    if (argc != 13 ||
        put(expected.artifact_sha256, sizeof(expected.artifact_sha256), argv[2]) ||
        put(expected.compiler_sha256, sizeof(expected.compiler_sha256), argv[3]) ||
        put(expected.manifest_sha256, sizeof(expected.manifest_sha256), argv[8]) ||
        put(expected.collection, sizeof(expected.collection), argv[9]) ||
        put(expected.corpus_version, sizeof(expected.corpus_version), argv[10]) ||
        put(expected.ruleset, sizeof(expected.ruleset), argv[11]) ||
        dnd_source_artifact_load_reviewed(argv[1], argv[7], &expected, &original) ||
        put(expected.artifact_sha256, sizeof(expected.artifact_sha256), argv[5]) ||
        put(expected.compiler_sha256, sizeof(expected.compiler_sha256), argv[6]) ||
        dnd_source_artifact_load_reviewed(argv[4], argv[7], &expected, &candidate)) goto done;
    size_t passage_count = 0, extra_passages = 0;
    const dnd_source_passage_definition *defs = dnd_source_artifact_passage_definitions(original, &passage_count);
    (void)dnd_source_artifact_passage_definitions(candidate, &extra_passages);
    /* This tool only merges legacy entries. Never silently discard passages. */
    if (extra_passages) goto done;
    const dnd_source_data *base = dnd_source_artifact_index(original)->data;
    const dnd_source_data *extra = dnd_source_artifact_index(candidate)->data;
    if (extra->entry_count > DND_SOURCE_ENTRIES_MAX - base->entry_count) goto done;
    entries = calloc(base->entry_count + extra->entry_count, sizeof(*entries));
    mapping = calloc(extra->record_count, sizeof(*mapping));
    if (!entries || !mapping) goto done;
    memcpy(entries, base->entries, base->entry_count * sizeof(*entries));
    for (size_t i = 0; i < extra->document_count; ++i) {
        size_t j = 0;
        for (; j < base->document_count; ++j) {
            if (strcmp(extra->documents[i].document_id, base->documents[j].document_id)) continue;
            if (strcmp(extra->documents[i].book_slug, base->documents[j].book_slug) ||
                strcmp(extra->documents[i].source_sha256, base->documents[j].source_sha256)) goto done;
            documents[i] = (uint32_t)j;
            break;
        }
        if (j == base->document_count) goto done;
    }
    for (size_t i = 0; i < extra->record_count; ++i) {
        const dnd_source_record *r = &extra->records[i];
        size_t j = 0;
        for (; j < base->record_count; ++j) {
            const dnd_source_record *b = &base->records[j];
            if (strcmp(r->record_id, b->record_id)) continue;
            if (strcmp(r->content_hash, b->content_hash) || r->document >= extra->document_count ||
                documents[r->document] != b->document || r->page != b->page || r->chunk != b->chunk ||
                r->begin != b->begin || r->content_len != b->content_len ||
                memcmp(r->content, b->content, r->content_len)) goto done;
            mapping[i] = (uint32_t)j;
            break;
        }
        if (j == base->record_count) goto done;
    }
    for (size_t i = 0; i < extra->entry_count; ++i) {
        dnd_source_entry entry = extra->entries[i];
        if (entry.kind != DND_SOURCE_RULE || !(entry.flags & DND_SOURCE_SPELL_HEADER) || !entry.body.count) continue;
        entry.document = documents[entry.document];
        size_t j = 0;
        for (; j < base->entry_count; ++j) {
            const dnd_source_entry *old = &base->entries[j];
            if (old->document == entry.document && old->page == entry.page &&
                old->kind == entry.kind && !strcmp(old->name, entry.name)) break;
        }
        if (j < base->entry_count) continue;
        for (j = base->entry_count; j < base->entry_count + added; ++j)
            if (entries[j].document == entry.document && entries[j].page == entry.page &&
                entries[j].kind == entry.kind && !strcmp(entries[j].name, entry.name)) goto done;
        remap(&entry.heading, mapping); remap(&entry.body, mapping); remap(&entry.opening, mapping);
        for (size_t k = 0; k < DND_SOURCE_FIELDS; ++k) remap(&entry.fields[k], mapping);
        entries[base->entry_count + added++] = entry;
    }
    dnd_source_data merged = *base;
    merged.entries = entries;
    merged.entry_count += added;
    if (!added) goto done;
    int encoded = passage_count
        ? dnd_source_encode_passages(&merged, defs, passage_count, argv[8], argv[12], &wire, &length)
        : dnd_source_encode(&merged, argv[8], argv[12], &wire, &length);
    if (encoded) goto done;
    if (fwrite(wire, 1, length, stdout) != length || fflush(stdout)) goto done;
    fprintf(stderr, "preserved_documents=%zu preserved_records=%zu preserved_entries=%zu added_entries=%zu\n",
        base->document_count, base->record_count, base->entry_count, added);
    result = 0;
done:
    free(wire); free(mapping); free(entries);
    dnd_source_artifact_free(candidate); dnd_source_artifact_free(original);
    return result;
}
