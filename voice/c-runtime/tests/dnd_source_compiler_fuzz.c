#include "dnd_source_compile.h"
#include "dnd_source_encode.h"
#include "ent_books.h"
#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *bytes, size_t length);
static void digest(const void *bytes, size_t length, char out[65]) {
    unsigned char hash[32];
    static const char hex[] = "0123456789abcdef";
    assert(SHA256(bytes, length, hash));
    for (size_t i = 0; i < 32; ++i) { out[2u * i] = hex[hash[i] >> 4]; out[2u * i + 1u] = hex[hash[i] & 15]; }
    out[64] = 0;
}
int LLVMFuzzerTestOneInput(const unsigned char *bytes, size_t length) {
    if (length < 4 || length > 7000) return 0;
    char text[8192];
    int subsection = (bytes[0] & 4u) != 0;
    int spell = !subsection && (bytes[0] & 8u);
    const char *prefix = subsection ? "CONCENTRATION You can hold a spell. Taking damage. You must check. Its threshold is 13. " :
        spell ? "AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self Components: V Duration: 1 round A shell appears. " :
        (bytes[0] & 1u) ? "WARDING STEP You can move. " :
        "OWL Tiny beast Armor Class 11 Hit Points 1 Speed 5 Challenge 0 ";
    size_t n = strlen(prefix), count = 1u + bytes[1] % 4u;
    memcpy(text, prefix, n); memcpy(text + n, bytes + 4, length - 4u); n += length - 4u;
    if (spell && (bytes[2] & 16u)) text[bytes[3] % strlen(prefix)] = (char)bytes[1];
    static const char suffix[] = " REACTIONS A reaction can occur on your turn or someone else's turn.";
    memcpy(text + n, suffix, sizeof(suffix));
    n += sizeof(suffix) - 1u;
    dnd_source_heading headings[] = {{"warding step"}, {"reactions"}};
    uint32_t kind = (subsection || spell || (bytes[0] & 1u)) ? DND_SOURCE_RULE : DND_SOURCE_CREATURE;
    if (subsection) strcpy(headings[0].text, "concentration");
    if (spell) strcpy(headings[0].text, "aegis");
    if (kind == DND_SOURCE_CREATURE) strcpy(headings[0].text, "owl");
    if (bytes[2] & 1u) memcpy(headings[0].text, bytes + 4, length - 4u < 256u ? length - 4u : 256u);
    dnd_source_record records[4] = {0};
    size_t previous_end = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t begin = i ? previous_end - (bytes[3] % (previous_end + 1u)) : 0;
        size_t end = i + 1u == count ? n : (i + 1u) * n / count;
        records[i].content = text + begin; records[i].content_len = end - begin;
        records[i].document = 0; records[i].page = 1; records[i].chunk = (uint32_t)i;
        assert(snprintf(records[i].record_id, 65, "%064zu", i + 1u) == 64);
        digest(records[i].content, records[i].content_len, records[i].content_hash);
        previous_end = end;
    }
    if (bytes[2] & 2u) records[count - 1u].chunk++;
    if (bytes[2] & 4u) records[0].content_hash[0] = 'x';
    dnd_source_compiled_page *out = malloc(sizeof(*out));
    assert(out); memset(out, 0xa5, sizeof(*out));
    uint32_t at = (uint32_t)strlen("CONCENTRATION You can hold a spell. ");
    dnd_source_section_hint section = {0, 13, at, at + 14u, (uint32_t)strlen(prefix) - 1u};
    if (bytes[2] & 8u) section.body_end += bytes[3];
    int rc = dnd_source_compile_page_sections(text, n, 0, 1, kind, records, count, headings, 2,
        subsection ? &section : NULL, subsection ? 1u : 0u, out);
    if (rc) {
        assert(rc == DND_SOURCE_PAGE_INVALID || rc == DND_SOURCE_PAGE_INCOMPLETE ||
            rc == DND_SOURCE_PAGE_AMBIGUOUS || rc == DND_SOURCE_PAGE_CAPACITY);
        const unsigned char *p = (const unsigned char *)out;
        for (size_t i = 0; i < sizeof(*out); ++i) assert(!p[i]);
    } else if (out->count) {
        for (size_t i = 0; i < count; ++i) records[i].begin = out->alignment.begin[i];
        dnd_source_document doc = {.document_id = "fixture", .book_slug = "players-handbook"};
        memset(doc.source_sha256, '1', 64);
        dnd_source_data data = {.collection = "fixture", .ruleset = "dnd-5e-2014",
            .documents = &doc, .document_count = 1, .records = records, .record_count = count,
            .entries = out->entries, .entry_count = out->count};
        memcpy(data.corpus_version, "sha256:", 7); memset(data.corpus_version + 7, '1', 64);
        unsigned char *wire = NULL;
        size_t wire_length = 0;
        assert(!dnd_source_encode(&data, doc.source_sha256, doc.source_sha256, &wire, &wire_length));
        dnd_source_artifact_expectation e = {.documents = &doc, .document_count = 1};
        memcpy(e.manifest_sha256, doc.source_sha256, 65); memcpy(e.compiler_sha256, doc.source_sha256, 65);
        strcpy(e.collection, data.collection); strcpy(e.corpus_version, data.corpus_version); strcpy(e.ruleset, data.ruleset);
        digest(wire, wire_length, e.artifact_sha256);
        dnd_source_artifact *loaded = NULL;
        assert(!dnd_source_artifact_decode(wire, wire_length, &e, &loaded));
        assert(dnd_source_artifact_index(loaded)->data->entry_count == out->count);
        dnd_rag_scope scope = {.kind = DND_RAG_SHARED_RULEBOOK, .authenticated_user_id = "fixture-user"};
        strcpy(scope.collection, data.collection); strcpy(scope.ruleset, data.ruleset);
        scope.book_mask = ent_book_access_mask(1);
        const char *query = spell ? "Can I cast Aegis on Mira?" : "For concentration checks, what DC follows taking damage?";
        dnd_source_anchor anchor = {0, 1};
        dnd_source_selection selected;
        assert(!dnd_source_index_select(dnd_source_artifact_index(loaded), &scope, data.corpus_version,
            query, strlen(query), spell ? &anchor : NULL, spell ? 1u : 0u, &selected));
        if (selected.reason == DND_SOURCE_SELECTED) {
            assert(selected.count && selected.count <= DND_RAG_HITS_MAX);
            for (size_t i = 0; i < selected.count; ++i) assert(selected.records[i] < count);
        } else assert(!selected.count && !selected.replace_baseline);
        dnd_source_artifact_free(loaded); free(wire);
    }
    free(out);
    return 0;
}
