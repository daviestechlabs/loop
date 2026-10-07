#include "dnd_source_compile.h"
#include "dnd_source_fixture.h"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static source_fixture f;
static dnd_source_compiled_page compiled;
static dnd_source_section_hint hint;
static size_t checks;
static uint32_t expected_records[4] = {1, 2, 4, 5};
static const char page_text[] =
    "CONCENTRATION You can hold a spell. "
    "SCHOOLS OF MAGIC You can classify spells. "
    "Taking damage. When a pulse hits, you must make a check. The threshold is 13. "
    "Being dazed. You must stop.";
static const dnd_source_heading headings[] = {{"concentration"}, {"schools of magic"}};

static int compile(void) {
    return dnd_source_compile_page_sections(page_text, sizeof(page_text) - 1, 0, 7,
        DND_SOURCE_RULE, f.records, 3, headings, 2, &hint, 1, &compiled);
}

static void reset(void) {
    fixture_make(&f);
    expected_records[0] = 1; expected_records[1] = 2; expected_records[2] = 4; expected_records[3] = 5;
    uint32_t child = (uint32_t)(strstr(page_text, "Taking damage.") - page_text);
    uint32_t end = (uint32_t)(strstr(page_text, "Being dazed.") - page_text - 1);
    hint = (dnd_source_section_hint){0, 13, child, child + 14, end};
    uint32_t cuts[][2] = {{0, child - 5}, {child - 15, child + 47}, {child + 35, sizeof(page_text) - 1}};
    for (size_t i = 0; i < 3; ++i)
        fixture_record(&f, i, 0, 7, (uint32_t)i, cuts[i][0], page_text + cuts[i][0], cuts[i][1] - cuts[i][0]);
    f.data.record_count = 3;
}

static void admit(void) {
    dnd_source_index index;
    CHECK(dnd_source_index_admit(&f.data, &index) == 0);
    ++checks;
}

static void load_fixture(void) {
    reset();
    CHECK(compile() == DND_SOURCE_PAGE_OK);
    CHECK(compiled.count == 3);
    for (size_t i = 0; i < 3; ++i) f.records[i].begin = compiled.alignment.begin[i];
    memcpy(f.entries, compiled.entries, compiled.count * sizeof(f.entries[0]));
    f.data.entry_count = compiled.count;
    admit();
}

static void select_case(const char *query, int reason, size_t count) {
    dnd_source_index index;
    dnd_source_selection out;
    CHECK(dnd_source_index_admit(&f.data, &index) == 0);
    CHECK(dnd_source_index_select(&index, &f.scope, f.data.corpus_version, query,
        strlen(query), NULL, 0, &out) == 0);
    CHECK(out.reason == reason && out.count == count);
    if (count) {
        CHECK(out.replace_baseline);
        for (size_t i = 0; i < count; ++i) CHECK(out.records[i] == expected_records[i]);
    } else CHECK(!out.replace_baseline && !out.records[0]);
    ++checks;
}

static void test_compile(void) {
    load_fixture();
    const dnd_source_entry *e = &f.entries[2];
    CHECK(e->kind == DND_SOURCE_SECTION && !strcmp(e->name, "takingdamage"));
    CHECK(!strcmp(e->alias, "concentration") && !e->flags);
    CHECK(e->fields[0].begin == 0 && e->fields[0].end == 13);
    CHECK(e->heading.begin == hint.heading_begin && e->heading.end == hint.heading_end);
    CHECK(e->body.begin == hint.heading_end + 1 && e->body.end == hint.body_end);
    CHECK(e->body.count == 2 && e->body.spans[0].record == 1 && e->body.spans[1].record == 2);
    CHECK(e->opening.end < e->body.end);
    for (size_t i = 0; i < 11; ++i) {
        reset();
        if (i == 0) hint.parent_end = UINT32_MAX;
        if (i == 1) hint.parent_end = 0;
        if (i == 2) hint.parent_begin = hint.heading_begin;
        if (i == 3) hint.heading_begin = UINT32_MAX;
        if (i == 4) --hint.heading_end;
        if (i == 5) hint.body_end = hint.heading_end;
        if (i == 6) --hint.body_end;
        if (i == 7) hint.body_end = UINT32_MAX;
        if (i == 8) hint.parent_begin = 1;
        if (i == 9) hint.parent_end = 4;
        if (i == 10) hint.heading_begin += 7;
        memset(&compiled, 0xa5, sizeof(compiled));
        CHECK(compile() == DND_SOURCE_PAGE_INVALID);
        const unsigned char *bytes = (const unsigned char *)&compiled;
        for (size_t j = 0; j < sizeof(compiled); ++j) CHECK(!bytes[j]);
        ++checks;
    }
    reset();
    dnd_source_section_hint duplicate[] = {hint, hint};
    CHECK(dnd_source_compile_page_sections(page_text, sizeof(page_text) - 1, 0, 7,
        DND_SOURCE_RULE, f.records, 3, headings, 2, duplicate, 2, &compiled) == DND_SOURCE_PAGE_INVALID);
    CHECK(dnd_source_compile_page_sections(page_text, sizeof(page_text) - 1, 0, 7,
        DND_SOURCE_CREATURE, f.records, 3, headings, 2, &hint, 1, &compiled) == DND_SOURCE_PAGE_INVALID);
    ++checks;
}

static void test_selection(void) {
    static const char *const positive[] = {
        "For concentration checks, how do I calculate the DC after damage?",
        "After taking damage while concentrating, what is the saving throw?",
        "What concentration save do I make when I take damage?",
        "How does damage affect a concentration check?"
    };
    static const char *const negative[] = {
        "What is concentration?", "Does concentration increase my spell damage?",
        "How much damage does this spell deal?", "How do I concentrate at work?",
        "What does a damage concentrationary do?",
        "Does War Caster give advantage on concentration checks after damage?",
        "Can proficiency improve my concentration save after damage?"
    };
    load_fixture();
    for (size_t i = 0; i < sizeof(positive) / sizeof(positive[0]); ++i)
        select_case(positive[i], DND_SOURCE_SELECTED, 2);
    for (size_t i = 0; i < sizeof(negative) / sizeof(negative[0]); ++i)
        select_case(negative[i], DND_SOURCE_NO_MATCH, 0);
    f.scope.book_mask = 0;
    select_case(positive[0], DND_SOURCE_NO_MATCH, 0);
}

static void remap(dnd_source_group *g, uint32_t offset) {
    for (size_t i = 0; i < g->count; ++i) g->spans[i].record += offset;
}

static void test_variants(void) {
    load_fixture();
    strcpy(f.documents[1].document_id, "rules-b");
    strcpy(f.documents[1].book_slug, "players-handbook");
    for (size_t i = 0; i < 3; ++i)
        fixture_record(&f, i + 3, 1, 7, (uint32_t)i, f.records[i].begin, f.records[i].content, f.records[i].content_len);
    f.entries[3] = f.entries[2]; f.entries[3].document = 1;
    remap(&f.entries[3].heading, 3); remap(&f.entries[3].body, 3);
    remap(&f.entries[3].opening, 3); remap(&f.entries[3].fields[0], 3);
    f.data.entry_count = 4; f.data.record_count = 6;
    select_case("What concentration save follows taking damage?", DND_SOURCE_SELECTED, 4);
    char *threshold = strstr(f.contents[5], "13"); CHECK(threshold);
    memcpy(threshold, "21", 2);
    fixture_hash(f.contents[5], f.records[5].content_len, f.records[5].content_hash);
    select_case("What concentration save follows taking damage?", DND_SOURCE_SELECTED, 4);
    /* The earlier retrieved page does not silently select one printing. */
    dnd_source_index index;
    dnd_source_selection out;
    dnd_source_anchor anchor = {1, 7};
    CHECK(!dnd_source_index_admit(&f.data, &index));
    const char query[] = "What concentration save follows taking damage?";
    CHECK(!dnd_source_index_select(&index, &f.scope, f.data.corpus_version, query,
        sizeof(query) - 1, &anchor, 1, &out));
    CHECK(out.count == 4 && out.records[0] == 1 && out.records[1] == 2 && out.records[2] == 4 && out.records[3] == 5);
    strcpy(f.documents[1].book_slug, "tashas-cauldron-of-everything");
    f.scope.book_mask = ent_book_access_mask(0);
    select_case(query, DND_SOURCE_SELECTED, 2);
    /* Two distinct places in the same document are ambiguous. */
    f.scope.book_mask = ent_book_access_mask(1);
    for (size_t i = 3; i < 6; ++i) { f.records[i].document = 0; f.records[i].page = 8; }
    f.entries[3].document = 0; f.entries[3].page = 8;
    select_case(query, DND_SOURCE_AMBIGUOUS, 0);
}

static void test_capacity(void) {
    load_fixture();
    f.data.document_count = 3;
    for (size_t d = 1; d < 3; ++d) {
        f.documents[d] = f.documents[0];
        f.documents[d].document_id[6] = (char)('a' + d);
        for (size_t i = 0; i < 3; ++i)
            fixture_record(&f, d * 3 + i, (uint32_t)d, 7, (uint32_t)i,
                f.records[i].begin, f.records[i].content, f.records[i].content_len);
        dnd_source_entry *e = &f.entries[d + 2];
        *e = f.entries[2]; e->document = (uint32_t)d;
        remap(&e->heading, (uint32_t)d * 3); remap(&e->body, (uint32_t)d * 3);
        remap(&e->opening, (uint32_t)d * 3); remap(&e->fields[0], (uint32_t)d * 3);
    }
    f.data.entry_count = 5; f.data.record_count = 9;
    select_case("What concentration save follows taking damage?", DND_SOURCE_CAPACITY, 0);
}

static void test_section_turn_context(void) {
    static const char text[] = "WARDING You can guard. Quick step. Once per turn, you can step. You must remain nearby.";
    fixture_make(&f);
    dnd_source_entry reaction = f.entries[1];
    fixture_record(&f, 0, 0, 7, 0, 0, text, sizeof(text) - 1);
    uint32_t begin = (uint32_t)(strstr(text, "Quick step.") - text);
    dnd_source_section_hint h = {0, 7, begin, begin + sizeof("Quick step.") - 1, sizeof(text) - 1};
    CHECK(!dnd_source_compile_page_sections(text, sizeof(text) - 1, 0, 7,
        DND_SOURCE_RULE, f.records, 1, NULL, 0, &h, 1, &compiled));
    CHECK(compiled.count == 1 && compiled.entries[0].flags == DND_SOURCE_ONCE_PER_TURN);
    f.entries[0] = compiled.entries[0]; f.entries[1] = reaction;
    f.data.entry_count = 2; f.data.record_count = 3;
    expected_records[0] = 0; expected_records[1] = 2;
    select_case("What are Warding Quick Step requirements?", DND_SOURCE_SELECTED, 1);
    select_case("How often can I use Warding Quick Step?", DND_SOURCE_SELECTED, 2);
    f.data.entry_count = 1;
    select_case("Can I use Warding Quick Step on an opportunity attack?", DND_SOURCE_INCOMPLETE, 0);
    f.entries[2] = reaction; f.data.entry_count = 3;
    select_case("How often can I use Warding Quick Step?", DND_SOURCE_AMBIGUOUS, 0);
}

static void test_grapple_queries(void) {
    static const char text[] = "GRAPPLING You can hold a foe. Escaping a Grapple. You can spend an action on a contest. Its threshold is 9.";
    static const dnd_source_heading parent[] = {{"grappling"}};
    fixture_make(&f);
    fixture_record(&f, 0, 0, 7, 0, 0, text, sizeof(text) - 1);
    uint32_t begin = (uint32_t)(strstr(text, "Escaping a Grapple.") - text);
    dnd_source_section_hint h = {0, 9, begin, begin + sizeof("Escaping a Grapple.") - 1, sizeof(text) - 1};
    CHECK(!dnd_source_compile_page_sections(text, sizeof(text) - 1, 0, 7,
        DND_SOURCE_RULE, f.records, 1, parent, 1, &h, 1, &compiled));
    memcpy(f.entries, compiled.entries, compiled.count * sizeof(f.entries[0]));
    f.data.entry_count = compiled.count; f.data.record_count = 1;
    expected_records[0] = 0;
    static const char *const positives[] = {
        "How does a creature get out of a grapple?", "What action lets me escape a grapple?",
        "Can I break free from a grapple?", "How can I escape being grappled?",
        "Explain escaping the grapple."
    };
    static const char *const negatives[] = {
        "How do I start a grapple?", "Can a grappled creature escape the room?",
        "Does the Grappler feat let me escape a grapple?", "Can I use a spell to escape a grapple?"
    };
    for (size_t i = 0; i < sizeof(positives) / sizeof(positives[0]); ++i)
        select_case(positives[i], DND_SOURCE_SELECTED, 1);
    for (size_t i = 0; i < sizeof(negatives) / sizeof(negatives[0]); ++i)
        select_case(negatives[i], DND_SOURCE_NO_MATCH, 0);
}

static void test_admission(void) {
    for (size_t i = 0; i < 6; ++i) {
        load_fixture();
        dnd_source_entry *e = &f.entries[2];
        if (i == 0) e->alias[0] = 0;
        if (i == 1) strcpy(e->alias, "schoolsofmagic");
        if (i == 2) memset(&e->fields[0], 0, sizeof(e->fields[0]));
        if (i == 3) e->fields[1] = e->fields[0];
        if (i == 4) e->fields[0] = e->body;
        if (i == 5) e->heading.spans[0].record = 0;
        dnd_source_index index;
        CHECK(dnd_source_index_admit(&f.data, &index) == -1 && !index.data);
        ++checks;
    }
}

int main(void) {
    test_compile(); test_selection(); test_variants(); test_capacity();
    test_admission(); test_section_turn_context(); test_grapple_queries();
    printf("OK %zu source subsection cases\n", checks);
    return 0;
}
