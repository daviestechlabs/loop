#ifndef DND_SOURCE_PASSAGE_FIXTURE_H
#define DND_SOURCE_PASSAGE_FIXTURE_H
#include "dnd_source_artifact_fixture.h"

/* Independent wire writer and authored source text. No passage compiler calls. */
static const char passage_first[] =
    "AEGIS 1st-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round The ward";
static const char passage_last[] =
    " remains until dawn. LAMPLIGHT Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears.";
static const char passage_assembly[] =
    "AEGIS 1st-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round The ward\n remains until dawn.";
static const char passage_single[] =
    "BEACON Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears. LAMPLIGHT Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears.";

static inline void passage_fixture_make(source_fixture *f, dnd_source_passage_definition defs[2]) {
    fixture_make(f);
    fixture_record(f, 4, 0, 40, 0, 0, passage_first, strlen(passage_first));
    fixture_record(f, 5, 0, 41, 0, 0, passage_last, strlen(passage_last));
    fixture_record(f, 6, 0, 42, 0, 0, passage_single, strlen(passage_single));
    f->data.record_count = 7;
    defs[0] = (dnd_source_passage_definition){0, 40, 41, 0, 5, 21, 30};
    uint32_t next = (uint32_t)(strstr(passage_single, "LAMPLIGHT") - passage_single);
    defs[1] = (dnd_source_passage_definition){0, 42, 42, 0, 6, next, next + 9u};
}

/* Aegis needs five witnesses. Beacon adds a sixth distinct original record. */
static inline void passage_fixture_split(source_fixture *f, dnd_source_passage_definition defs[2]) {
    passage_fixture_make(f, defs);
    fixture_record(f, 4, 0, 40, 0, 0, passage_first, 32);
    fixture_record(f, 5, 0, 40, 1, 24, passage_first + 24, 48);
    fixture_record(f, 6, 0, 40, 2, 64, passage_first + 64, strlen(passage_first) - 64u);
    fixture_record(f, 7, 0, 41, 0, 0, passage_last, 12);
    fixture_record(f, 8, 0, 41, 1, 8, passage_last + 8, strlen(passage_last) - 8u);
    fixture_record(f, 9, 0, 42, 0, 0, passage_single, strlen(passage_single));
    f->data.record_count = 10;
}

static inline size_t passage_artifact_make(artifact_fixture *a, const source_fixture *f,
    const dnd_source_passage_definition defs[2]) {
    artifact_make(a, f);
    size_t at = a->counts + 12u, old = a->length;
    memmove(a->bytes + at + 4u, a->bytes + at, old - at);
    a->bytes[7] = '2'; a->length = at; artifact_u32(a, 2); a->length = old + 4u;
    for (size_t i = 0; i < f->data.document_count; ++i) a->docs[i] += 4u;
    for (size_t i = 0; i < f->data.record_count; ++i) a->records[i] += 4u;
    for (size_t i = 0; i < f->data.entry_count; ++i) a->entries[i] += 4u;
    size_t start = a->length;
    for (size_t i = 0; i < 2; ++i) {
        const dnd_source_passage_definition *d = &defs[i];
        artifact_u32(a, d->document); artifact_u32(a, d->first_page); artifact_u32(a, d->last_page);
        artifact_u32(a, d->heading_begin); artifact_u32(a, d->heading_end);
        artifact_u32(a, d->next_heading_begin); artifact_u32(a, d->next_heading_end);
        char hash[65];
        if (i) fixture_hash(passage_single, defs[1].next_heading_begin - 1u, hash);
        else fixture_hash(passage_assembly, strlen(passage_assembly), hash);
        artifact_string(a, hash);
    }
    artifact_rehash(a);
    return start;
}
#endif
