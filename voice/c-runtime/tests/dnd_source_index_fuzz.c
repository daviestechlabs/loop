#include "dnd_source_fixture.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    source_fixture f;
    dnd_source_index index;
    dnd_source_selection result;
    char query[DND_SOURCE_QUERY_CAP];
    if (!size || size >= sizeof(query)) return 0;
    fixture_make(&f);
    int spell = (data[0] & 128u) != 0;
    if (spell) {
        static const char text[] = "AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self "
            "Components: V Duration: 1 round A shell appears. It adds protection.";
        fixture_record(&f, 0, 0, 7, 0, 0, text, sizeof(text) - 1u);
        dnd_source_entry *e = &f.entries[0];
        *e = (dnd_source_entry){.page = 7, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
        strcpy(e->name, "aegis");
        e->heading = fixture_group(0, 0, 5);
        e->opening = fixture_group(0, 6, (uint32_t)(strchr(text, '.') - text + 1));
        e->body = fixture_group(0, 6, sizeof(text) - 1u);
    }
    size_t entry = size > 1 ? data[1] % 3u : 0;
    size_t record = size > 2 ? data[2] % 4u : 0;
    uint32_t value = size > 3 ? data[3] : 0;
    switch (data[0] % 20u) {
    case 0: f.entries[entry].body.count = value; break;
    case 1: f.entries[entry].heading.spans[0].record = value; break;
    case 2: f.entries[entry].opening.begin = value; break;
    case 3: f.entries[entry].document = value; break;
    case 4: f.entries[entry].page = value; break;
    case 5: f.entries[entry].flags = value; break;
    case 6: f.records[record].document = value; break;
    case 7: f.records[record].begin = value; break;
    case 8: f.records[record].chunk = value; break;
    case 9: f.contents[record][value % f.records[record].content_len] ^= 1; break;
    case 10: f.records[record].content_hash[value % 64u] ^= 1; break;
    case 11: f.entries[entry].name[value % 255u] ^= 1; break;
    case 12: f.entries[entry].alias[value % 255u] = 'x'; break;
    case 13: f.records[record].content_len = 0; break;
    case 14: f.entries[entry].fields[0].end = value; break;
    case 15: f.scope.kind = (dnd_rag_scope_kind)value; break;
    case 16: f.scope.book_mask = UINT64_C(1) << (value % 64u); break;
    case 17: f.anchors[0].page = value; break;
    case 18: f.data.corpus_version[value % 71u] ^= 1; break;
    default: break;
    }
    if (dnd_source_index_admit(&f.data, &index)) {
        if (index.data) abort();
        return 0;
    }
    memcpy(query, data, size);
    int rc = dnd_source_index_select(&index, &f.scope, f.data.corpus_version,
        query, size, f.anchors, 1, &result);
    if (rc || result.reason != DND_SOURCE_SELECTED) {
        if (result.count || result.replace_baseline) abort();
        for (size_t i = 0; i < DND_RAG_HITS_MAX; ++i) if (result.records[i]) abort();
    } else {
        if (!result.count || result.count > DND_RAG_HITS_MAX) abort();
        for (size_t i = 0; i < result.count; ++i) {
            if (result.records[i] >= f.data.record_count) abort();
            const dnd_source_record *r = &f.records[result.records[i]];
            if (!dnd_rag_excerpt_valid(&result.excerpts[i], r->content_len)) abort();
            if (!ent_book_allowed(f.scope.book_mask, f.documents[r->document].book_slug)) abort();
            for (size_t j = 0; j < i; ++j) if (result.records[i] == result.records[j]) abort();
        }
    }
    /* Always exercise a valid named rule as well as the arbitrary query bytes. */
    static const char *const queries[] = {
        "How often can I use Warding Step in a round?",
        "Can Warding Step happen on an opportunity attack?"
    };
    const char *focus = spell ? "Can I cast Aegis on Mira?" : queries[data[0] & 1u];
    rc = dnd_source_index_select(&index, &f.scope, f.data.corpus_version,
        focus, strlen(focus), f.anchors, 1, &result);
    if ((rc || result.reason != DND_SOURCE_SELECTED) && (result.count || result.replace_baseline)) abort();
    return 0;
}
