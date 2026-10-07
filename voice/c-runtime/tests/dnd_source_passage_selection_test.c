#include "dnd_source_encode.h"
#include "dnd_source_passage_fixture.h"
#include <stdlib.h>

static source_fixture f;
static artifact_fixture fixture;
static dnd_source_passage_definition defs[5];
static dnd_source_artifact *artifact;
static size_t checks;
static void load(size_t count) {
    artifact_make(&fixture, &f);
    unsigned char *wire = NULL; size_t length = 0;
    int encoded = dnd_source_encode_passages(&f.data, defs, count, fixture.expected.manifest_sha256,
        fixture.expected.compiler_sha256, &wire, &length);
    if (encoded) fprintf(stderr, "fixture admission failed after %zu checks\n", checks);
    assert(!encoded);
    fixture_hash((const char *)wire, length, fixture.expected.artifact_sha256);
    dnd_source_artifact_free(artifact); artifact = NULL;
    assert(!dnd_source_artifact_decode(wire, length, &fixture.expected, &artifact)); free(wire);
}
static dnd_source_passage_selection select(const char *q, int rc, int reason, size_t count, size_t records) {
    dnd_source_passage_selection out;
    memset(&out, 0x55, sizeof(out));
    assert(dnd_source_artifact_select_passages(artifact, &f.scope, f.data.corpus_version,
        q, strlen(q), f.anchors, f.anchor_count, &out) == rc);
    assert(out.reason == reason && out.count == count && out.record_count == records);
    if (!count) {
        dnd_source_passage_selection zero = {0}; zero.reason = reason;
        assert(!memcmp(&out, &zero, sizeof(out)));
    }
    ++checks; return out;
}
static void simple_passages(const char *const *names, size_t count) {
    fixture_make(&f);
    for (size_t i = 0; i < count; ++i) {
        char text[512];
        int n = snprintf(text, sizeof(text), "%s Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears. %s", names[i],
            "LAMPLIGHT Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears.");
        assert(n > 0 && (size_t)n < sizeof(text));
        uint32_t page = 40u + (uint32_t)i;
        fixture_record(&f, i + 4u, 0, page, 0, 0, text, (size_t)n);
        uint32_t next = (uint32_t)(strstr(text, "LAMPLIGHT") - text);
        defs[i] = (dnd_source_passage_definition){0, page, page, 0, (uint32_t)strlen(names[i]), next, next + 9u};
    }
    f.data.record_count = count + 4u; load(count);
}
int main(void) {
    passage_fixture_split(&f, defs); load(2); f.anchors[0] = (dnd_source_anchor){0, 40};
    dnd_source_passage_selection out = select("Can I cast Aegis?", 0, DND_SOURCE_SELECTED, 1, 5);
    for (size_t i = 0; i < 5; ++i) assert(out.records[i] == 4u + i);
    select("Compare the Aegis spell and the Beacon spell.", 0, DND_SOURCE_SELECTED, 2, 6);
    select("Can I cast Aegis or Beacon?", 0, DND_SOURCE_SELECTED, 2, 6);
    select("Can I cast the Aegis spell and the Beacon spell?", 0, DND_SOURCE_SELECTED, 2, 6);
    select("Can I cast Aegis and carry a Beacon?", 0, DND_SOURCE_SELECTED, 1, 5);
    select("Can I cast Aegis or travel to Beacon?", 0, DND_SOURCE_SELECTED, 1, 5);
    select("Are Aegis and Beacon in the room?", 0, DND_SOURCE_NO_MATCH, 0, 0);
    f.anchors[0].page = 7; select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 5);
    f.anchors[0].page = 41; select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 5);
    select("Aegisian spell", 0, DND_SOURCE_NO_MATCH, 0, 0);
    select("Aegis", 0, DND_SOURCE_NO_MATCH, 0, 0);
    f.anchor_count = 0; select("Aegis spell", 0, DND_SOURCE_NO_MATCH, 0, 0);
    f.anchor_count = 1; f.anchors[0].document = UINT32_MAX; select("Aegis spell", 0, DND_SOURCE_NO_MATCH, 0, 0);
    f.anchors[0].document = 1; select("Aegis spell", 0, DND_SOURCE_NO_MATCH, 0, 0);
    f.anchors[0].document = 2; select("Aegis spell", -1, 0, 0, 0);
    f.anchors[0].document = 0; f.anchors[0].page = 0; select("Aegis spell", -1, 0, 0, 0);
    f.anchors[0].page = 40; f.scope.book_mask = 0; select("Aegis spell", 0, 0, 0, 0);
    f.scope.book_mask = ent_book_access_mask(1);
    f.scope.kind = DND_RAG_OWNED_RULEBOOK; select("Aegis spell", -1, 0, 0, 0);
    f.scope.kind = DND_RAG_SHARED_RULEBOOK; strcpy(f.scope.owner_user_id, "owner"); select("Aegis spell", -1, 0, 0, 0);
    f.scope.owner_user_id[0] = 0; f.scope.collection[0] = 'x'; select("Aegis spell", -1, 0, 0, 0);
    strcpy(f.scope.collection, f.data.collection); f.scope.ruleset[0] = 'x'; select("Aegis spell", -1, 0, 0, 0);
    strcpy(f.scope.ruleset, f.data.ruleset); f.data.corpus_version[8] = 'x'; select("Aegis spell", -1, 0, 0, 0);
    f.data.corpus_version[8] = 'a'; select("\xff", -1, 0, 0, 0);
    f.scope.authenticated_user_id[0] = 0; select("Aegis spell", -1, 0, 0, 0);

    const char *const same[] = {"AEGIS", "AEGIS"}; simple_passages(same, 2);
    f.anchors[0] = (dnd_source_anchor){0, 7}; select("Aegis spell", 0, DND_SOURCE_AMBIGUOUS, 0, 0);
    f.anchors[0].page = 40; out = select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1); assert(out.passages[0] == 0);
    f.anchors[0].page = 41; out = select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1); assert(out.passages[0] == 1);
    f.records[5].document = 1; defs[1].document = 1; load(2);
    f.anchors[0] = (dnd_source_anchor){1, 7}; f.anchors[1] = (dnd_source_anchor){0, 7}; f.anchor_count = 2;
    out = select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1); assert(out.passages[0] == 1);
    f.anchors[1].page = 40; out = select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1); assert(out.passages[0] == 0);

    const char *const many[] = {"AEGIS", "BEACON", "CHARM", "DREAM", "EMBER"}; simple_passages(many, 5);
    f.anchors[0] = (dnd_source_anchor){0, 40};
    select("Aegis spell, Beacon spell, Charm spell, Dream spell", 0, DND_SOURCE_SELECTED, 4, 4);
    select("Aegis spell, Beacon spell, Charm spell, Dream spell, Ember spell", 0, DND_SOURCE_CAPACITY, 0, 0);
    /* A legacy source-backed spell absent from the passage table blocks partial answers. */
    dnd_source_entry *e = &f.entries[3];
    *e = (dnd_source_entry){.page = 44, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
    strcpy(e->name, "ember"); e->heading = fixture_group(8, 0, 5);
    e->opening = fixture_group(8, 6, (uint32_t)(strchr(f.contents[8], '.') - f.contents[8] + 1));
    f.data.entry_count = 4; load(4);
    select("Aegis spell and Ember spell", 0, DND_SOURCE_INCOMPLETE, 0, 0);
    select("Can I cast Aegis or Ember?", 0, DND_SOURCE_INCOMPLETE, 0, 0);

    /* A reviewed passage cannot override a better or equally ranked legacy location. */
    simple_passages(same, 2);
    e = &f.entries[3];
    *e = (dnd_source_entry){.page = 41, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
    strcpy(e->name, "aegis"); e->heading = fixture_group(5, 0, 5);
    e->opening = fixture_group(5, 6, (uint32_t)(strchr(f.contents[5], '.') - f.contents[5] + 1));
    f.data.entry_count = 4; load(1);
    f.anchors[0] = (dnd_source_anchor){0, 41};
    select("Aegis spell", 0, DND_SOURCE_INCOMPLETE, 0, 0);
    f.anchors[0].page = 7;
    select("Aegis spell", 0, DND_SOURCE_AMBIGUOUS, 0, 0);
    f.anchors[0].page = 40;
    select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1);
    load(2); f.anchors[0].page = 41;
    select("Aegis spell", 0, DND_SOURCE_SELECTED, 1, 1);
    dnd_source_artifact_free(artifact);
    printf("OK %zu complete-passage selection checks\n", checks);
    return 0;
}
