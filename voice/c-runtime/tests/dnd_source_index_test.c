#include "dnd_source_fixture.h"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static source_fixture fixture;
static size_t checks;

static dnd_source_selection selected_result(const char *query, int reason, size_t count) {
    dnd_source_index index;
    dnd_source_selection selected;
    CHECK(dnd_source_index_admit(&fixture.data, &index) == 0);
    memset(&selected, 0xff, sizeof(selected));
    CHECK(dnd_source_index_select(&index, &fixture.scope, fixture.data.corpus_version,
        query, strlen(query), fixture.anchors, fixture.anchor_count, &selected) == 0);
    if (selected.reason != reason || selected.count != count)
        fprintf(stderr, "query=%s expected=%d/%zu actual=%d/%zu\n", query, reason, count, selected.reason, selected.count);
    CHECK(selected.reason == reason && selected.count == count);
    if (!count) CHECK(!selected.records[0] && !selected.replace_baseline);
    ++checks;
    return selected;
}

static void selection(const char *query, int reason, size_t count) {
    dnd_source_selection selected = selected_result(query, reason, count);
    if (count && strstr(query, "Warding Step")) {
        CHECK(selected.records[0] == 0 && selected.records[1] == 1 && !selected.replace_baseline);
        if (count == 3) CHECK(selected.records[2] == 2);
    }
    if (count && strstr(query, "iron badger")) CHECK(selected.replace_baseline && selected.records[0] == 3);
}

static void rejection(void) {
    dnd_source_index index = {&fixture.data};
    CHECK(dnd_source_index_admit(&fixture.data, &index) == -1 && !index.data);
    ++checks;
}

static void test_selection(void) {
    fixture_make(&fixture);
    selection("What does Warding Step require?", DND_SOURCE_SELECTED, 2);
    selection("How often can I use Warding Step in a round?", DND_SOURCE_SELECTED, 3);
    selection("What are the iron badger's AC, hit points and damage immunities?", DND_SOURCE_SELECTED, 1);
    selection("What is an iron badgerling's AC?", DND_SOURCE_NO_MATCH, 0);
    selection("What is the badger's AC?", DND_SOURCE_NO_MATCH, 0);
    fixture.scope.book_mask = 0;
    selection("What does Warding Step require?", DND_SOURCE_NO_MATCH, 0);
    selection("What is the iron badger's AC?", DND_SOURCE_NO_MATCH, 0);
    fixture_make(&fixture);
    fixture.anchors[0].page = 8;
    selection("What does Warding Step require?", DND_SOURCE_SELECTED, 2);
    fixture_make(&fixture);
    memset(&fixture.entries[0].body, 0, sizeof(fixture.entries[0].body));
    selection("What does Warding Step require?", DND_SOURCE_INCOMPLETE, 0);
    fixture_make(&fixture);
    memset(&fixture.entries[2].fields[4], 0, sizeof(fixture.entries[2].fields[4]));
    selection("What are the iron badger's AC and immunities?", DND_SOURCE_INCOMPLETE, 0);
    fixture_make(&fixture);
    fixture.data.entry_count = 1;
    selection("How often can I use Warding Step in a round?", DND_SOURCE_INCOMPLETE, 0);
    fixture_make(&fixture);
    fixture.entries[3] = fixture.entries[1]; fixture.data.entry_count = 4;
    selection("How often can I use Warding Step in a round?", DND_SOURCE_AMBIGUOUS, 0);
    fixture_make(&fixture);
    fixture.entries[3] = fixture.entries[2]; fixture.data.entry_count = 4;
    selection("What is the iron badger's AC?", DND_SOURCE_AMBIGUOUS, 0);
    fixture_make(&fixture);
    fixture.entries[3] = fixture.entries[0]; fixture.entries[3].body = fixture.entries[3].opening;
    fixture.data.entry_count = 4;
    selection("What does Warding Step require?", DND_SOURCE_AMBIGUOUS, 0);
}

static void test_temporal_wording(void) {
    static const char *const context[] = {
        "Can I use Warding Step on an opportunity attack?",
        "Can I use Warding Step for opportunity attacks?",
        "Can Warding Step happen as a reaction?",
        "Do reactions allow Warding Step?",
        "Can I use Warding Step on another turn?",
        "Can Warding Step work on someone else's turn?",
        "Can Warding Step work on an enemy's turn?",
        "Can Warding Step work on different turns?",
        "Can I use Warding Step each turn?",
        "Can I use Warding Step every turn?",
        "Can I use Warding Step twice?",
        "Can I use Warding Step on my turn and their turn?"
    };
    static const char *const ordinary[] = {
        "What does Warding Step require?",
        "What is Warding Step's range?",
        "Does Warding Step require sight?",
        "Explain Warding Step to a reactionary.",
        "What does Warding Step require of an opportunity attacker?",
        "Describe Warding Step per turnip.",
        "How do surroundings affect Warding Step?"
    };
    fixture_make(&fixture);
    for (size_t i = 0; i < sizeof(context) / sizeof(context[0]); ++i)
        selection(context[i], DND_SOURCE_SELECTED, 3);
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); ++i)
        selection(ordinary[i], DND_SOURCE_SELECTED, 2);
    selection("Can I take an opportunity attack?", DND_SOURCE_NO_MATCH, 0);
    selection("Can Warding Stepping happen as a reaction?", DND_SOURCE_NO_MATCH, 0);
    selection("Can Step happen as a reaction?", DND_SOURCE_NO_MATCH, 0);
    fixture.data.entry_count = 1;
    selection(context[0], DND_SOURCE_INCOMPLETE, 0);
    fixture_make(&fixture);
    fixture.entries[3] = fixture.entries[1]; fixture.data.entry_count = 4;
    selection(context[0], DND_SOURCE_AMBIGUOUS, 0);

    /* A temporal word cannot turn an ordinary rule into a once-per-turn rule. */
    static const char rule[] = "WARDING STEP You can step if you see the target.";
    fixture_make(&fixture);
    fixture_record(&fixture, 0, 0, 7, 0, 0, rule, sizeof(rule) - 1);
    fixture.entries[0].flags = 0;
    fixture.entries[0].body = fixture.entries[0].opening = fixture_group(0, 13, sizeof(rule) - 1);
    dnd_source_selection out = selected_result(context[0], DND_SOURCE_SELECTED, 1);
    CHECK(out.records[0] == 0 && !out.replace_baseline);
}

static void test_complete_turn_context(void) {
    fixture_make(&fixture);
    fixture_complete_reaction(&fixture, 4);
    dnd_source_selection out = selected_result(
        "How often can I use Warding Step in a round?", DND_SOURCE_SELECTED, 4);
    CHECK(out.records[0] == 0 && out.records[1] == 1 &&
        out.records[2] == 2 && out.records[3] == 4 && !out.replace_baseline);
    selection("What does Warding Step require?", DND_SOURCE_SELECTED, 2);

    /* A complete opening does not replace missing reaction requirements. */
    memset(&fixture.entries[1].body, 0, sizeof(fixture.entries[1].body));
    selection("Can Warding Step happen on another turn?", DND_SOURCE_INCOMPLETE, 0);
    selection("What does Warding Step require?", DND_SOURCE_SELECTED, 2);

    fixture_make(&fixture);
    fixture_complete_reaction(&fixture, 4);
    fixture.entries[3] = fixture.entries[1]; fixture.data.entry_count = 4;
    selection("Can Warding Step happen on another turn?", DND_SOURCE_AMBIGUOUS, 0);

    /* Two requested rules plus complete reaction context exceed four records. */
    fixture_make(&fixture);
    fixture_complete_reaction(&fixture, 4);
    static const char other[] = "GUARDING LEAP Once per turn, you can leap.";
    fixture_record(&fixture, 5, 0, 8, 0, 0, other, sizeof(other) - 1u);
    dnd_source_entry *e = &fixture.entries[3];
    *e = (dnd_source_entry){.page = 8, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_ONCE_PER_TURN};
    strcpy(e->name, "guardingleap");
    e->heading = fixture_group(5, 0, 13);
    e->body = e->opening = fixture_group(5, 14, sizeof(other) - 1u);
    fixture.data.record_count = 6; fixture.data.entry_count = 4;
    fixture.anchors[1] = (dnd_source_anchor){0, 8}; fixture.anchor_count = 2;
    selection("Can Warding Step and Guarding Leap happen twice in a round?", DND_SOURCE_CAPACITY, 0);
}

static void test_rule_name_boundaries(void) {
    fixture_make(&fixture);
    selection("Explain wArDiNg-StEp.", DND_SOURCE_SELECTED, 2);
    selection("Explain Wardingstep.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain rewarding step.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain Warding Stepper.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain Warding Ste.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain Warding Stone.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain Warding St3p.", DND_SOURCE_NO_MATCH, 0);
    selection("Explain Warding\tStep.", DND_SOURCE_SELECTED, 2);
    static const char rule[] = "WARDING STEPS You can step.";
    fixture_record(&fixture, 0, 0, 7, 0, 0, rule, sizeof(rule) - 1);
    fixture.data.record_count = fixture.data.entry_count = 1;
    dnd_source_entry *e = &fixture.entries[0];
    strcpy(e->name, "wardingsteps"); e->flags = 0;
    e->heading = fixture_group(0, 0, 13);
    e->body = e->opening = fixture_group(0, 14, sizeof(rule) - 1);
    dnd_source_selection out = selected_result("Explain Warding Step.", DND_SOURCE_SELECTED, 1);
    CHECK(out.records[0] == 0);
    out = selected_result("Explain Warding Steps.", DND_SOURCE_SELECTED, 1);
    CHECK(out.records[0] == 0);
    selection("Explain Warding Stepside.", DND_SOURCE_NO_MATCH, 0);
}

static void copy_rule(uint32_t document, uint32_t page) {
    for (size_t i = 0; i < 2; ++i)
        fixture_record(&fixture, i + 4, document, page, (uint32_t)i, fixture.records[i].begin,
            fixture.contents[i], fixture.records[i].content_len);
    fixture.entries[3] = fixture.entries[0];
    dnd_source_entry *e = &fixture.entries[3];
    e->document = document; e->page = page;
    e->heading.spans[0].record = 4; e->opening.spans[0].record = 4;
    e->body.spans[0].record = 4; e->body.spans[1].record = 5;
    fixture.data.entry_count = 4; fixture.data.record_count = 6;
}

static void test_document_fallback(void) {
    static const char query[] = "What does Warding Step require?";
    fixture_make(&fixture);
    fixture.anchors[0].page = 19;
    selection(query, DND_SOURCE_SELECTED, 2);
    selection("Can I use Warding Step on an opportunity attack?", DND_SOURCE_SELECTED, 3);
    fixture.anchors[0].document = 1;
    selection(query, DND_SOURCE_NO_MATCH, 0);
    fixture.anchors[0] = (dnd_source_anchor){UINT32_MAX, 7};
    selection(query, DND_SOURCE_NO_MATCH, 0);
    fixture.anchor_count = 0;
    selection(query, DND_SOURCE_NO_MATCH, 0);

    fixture_make(&fixture);
    copy_rule(0, 8);
    fixture.anchors[0].page = 19;
    /* Equal byte offsets on different pages are different source passages. */
    selection(query, DND_SOURCE_AMBIGUOUS, 0);
    fixture.anchors[0].page = 7;
    selection(query, DND_SOURCE_SELECTED, 2);
    fixture.anchors[0].page = 8;
    dnd_source_selection out = selected_result(query, DND_SOURCE_SELECTED, 2);
    CHECK(out.records[0] == 4 && out.records[1] == 5 && !out.replace_baseline);

    fixture_make(&fixture);
    copy_rule(1, 7);
    fixture.anchors[0] = (dnd_source_anchor){1, 19};
    fixture.anchors[1] = (dnd_source_anchor){0, 7}; fixture.anchor_count = 2;
    /* An exact page beats an earlier hit elsewhere in another document. */
    selection(query, DND_SOURCE_SELECTED, 2);
    fixture.anchors[1].page = 19;
    out = selected_result(query, DND_SOURCE_SELECTED, 2);
    CHECK(out.records[0] == 4 && out.records[1] == 5);
    /* Context cannot come from the other document, even with identical text. */
    selection("Can Warding Step happen as a reaction?", DND_SOURCE_INCOMPLETE, 0);
    dnd_source_entry swap = fixture.entries[0];
    fixture.entries[0] = fixture.entries[3]; fixture.entries[3] = swap;
    out = selected_result(query, DND_SOURCE_SELECTED, 2);
    CHECK(out.records[0] == 4 && out.records[1] == 5);
    fixture.scope.book_mask = 0;
    selection(query, DND_SOURCE_NO_MATCH, 0);

    fixture_make(&fixture);
    copy_rule(0, 8);
    memset(&fixture.entries[0].body, 0, sizeof(fixture.entries[0].body));
    /* Do not hide missing evidence by falling through to a different passage. */
    selection(query, DND_SOURCE_INCOMPLETE, 0);

    /* Incidental rule names elsewhere in a book cannot expand an exact match. */
    fixture_make(&fixture);
    static const char other[] = "HIT POINTS You can track health with points.";
    fixture_record(&fixture, 4, 0, 19, 0, 0, other, sizeof(other) - 1);
    fixture.entries[3] = (dnd_source_entry){.kind = DND_SOURCE_RULE, .page = 19};
    strcpy(fixture.entries[3].name, "hitpoints");
    fixture.entries[3].heading = fixture_group(4, 0, 10);
    fixture.entries[3].body = fixture.entries[3].opening = fixture_group(4, 11, sizeof(other) - 1);
    fixture.data.entry_count = 4; fixture.data.record_count = 5;
    selection("What does Warding Step do to hit points?", DND_SOURCE_SELECTED, 2);
}

static void test_admission(void) {
    fixture_make(&fixture); fixture.contents[1][50] ^= 1; rejection();
    fixture_make(&fixture); fixture.records[1].content_hash[0] = 'f'; rejection();
    fixture_make(&fixture); strcpy(fixture.records[1].record_id, fixture.records[0].record_id); rejection();
    fixture_make(&fixture); fixture.records[1].document = 1; rejection();
    fixture_make(&fixture); fixture.records[1].page = 8; rejection();
    fixture_make(&fixture); fixture.records[1].chunk = 2; rejection();
    fixture_make(&fixture); fixture.entries[0].body.spans[1].begin++; rejection();
    fixture_make(&fixture); fixture.entries[0].body.spans[1].record = 16; rejection();
    fixture_make(&fixture); fixture.entries[0].body.count = 3; rejection();
    fixture_make(&fixture); fixture.entries[0].body.end--; fixture.entries[0].body.spans[1].end--; rejection();
    fixture_make(&fixture); fixture.entries[0].flags = 0; rejection();
    fixture_make(&fixture); fixture.entries[0].name[0] = 'x'; rejection();
    fixture_make(&fixture); strcpy(fixture.entries[2].alias, "invented name"); rejection();
    fixture_make(&fixture); fixture.entries[2].fields[0].begin = 0; rejection();
    fixture_make(&fixture); fixture.entries[2].fields[0] = fixture.entries[2].fields[1]; rejection();
    fixture_make(&fixture); fixture.documents[0].source_sha256[0] = 'X'; rejection();
    fixture_make(&fixture); strcpy(fixture.documents[1].document_id, "rules-a"); rejection();
    fixture_make(&fixture); strcpy(fixture.documents[1].book_slug, "unreviewed-book"); rejection();
    fixture_make(&fixture); fixture.records[0].content_len = DND_RAG_CONTENT_CAP; rejection();
    fixture_make(&fixture); fixture.records[0].begin = DND_SOURCE_PAGE_CAP; rejection();
    fixture_make(&fixture); fixture.data.record_count = DND_SOURCE_RECORDS_MAX + 1u; rejection();
    fixture_make(&fixture); fixture.data.entry_count = DND_SOURCE_ENTRIES_MAX + 1u; rejection();
    fixture_make(&fixture); fixture.data.document_count = DND_SOURCE_DOCUMENTS_MAX + 1u; rejection();
    fixture_make(&fixture); memset(fixture.data.collection, 'a', sizeof(fixture.data.collection)); rejection();
    fixture_make(&fixture); fixture.contents[0][2] = '\0'; rejection();
    fixture_make(&fixture); fixture.contents[0][2] = (char)0xff;
    fixture_hash(fixture.contents[0], fixture.records[0].content_len, fixture.records[0].content_hash); rejection();
    fixture_make(&fixture); fixture.contents[1][0] ^= 1;
    fixture_hash(fixture.contents[1], fixture.records[1].content_len, fixture.records[1].content_hash); rejection();
}

static uint64_t book_mask(const char *book) {
    size_t count;
    const ent_book *books = ent_books(&count);
    for (size_t i = 0; i < count; ++i) if (!strcmp(books[i].slug, book)) return UINT64_C(1) << i;
    CHECK(0); return 0;
}

static void test_scope_before_matching(void) {
    fixture_make(&fixture);
    fixture_record(&fixture, 4, 0, 12, 0, 0, fixture.contents[3], fixture.records[3].content_len);
    fixture.entries[3] = fixture.entries[2];
    dnd_source_entry *copy = &fixture.entries[3];
    copy->document = 0; copy->heading.spans[0].record = 4;
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) copy->fields[i].spans[0].record = 4;
    fixture.data.entry_count = 4; fixture.data.record_count = 5;
    selection("What is the iron badger's AC?", DND_SOURCE_AMBIGUOUS, 0);
    fixture.scope.book_mask = book_mask("monster-manual");
    selection("What is the iron badger's AC?", DND_SOURCE_SELECTED, 1);

    fixture_make(&fixture);
    for (size_t i = 0; i < 2; ++i)
        fixture_record(&fixture, i + 4, 1, 7, (uint32_t)i, fixture.records[i].begin,
            fixture.contents[i], fixture.records[i].content_len);
    fixture.entries[3] = fixture.entries[0]; copy = &fixture.entries[3];
    copy->document = 1; copy->heading.spans[0].record = 4; copy->opening.spans[0].record = 4;
    copy->body.spans[0].record = 4; copy->body.spans[1].record = 5;
    fixture.data.entry_count = 4; fixture.data.record_count = 6;
    fixture.anchors[0] = (dnd_source_anchor){1, 7};
    fixture.anchors[1] = (dnd_source_anchor){0, 7}; fixture.anchor_count = 2;
    fixture.scope.book_mask = book_mask("players-handbook");
    selection("What does Warding Step require?", DND_SOURCE_SELECTED, 2);
    fixture.scope.book_mask = 0;
    selection("What does Warding Step require?", DND_SOURCE_NO_MATCH, 0);
}

static void test_scope_and_arguments(void) {
    dnd_source_index index;
    dnd_source_selection out;
    fixture_make(&fixture);
    CHECK(dnd_source_index_admit(&fixture.data, &index) == 0);
    for (size_t i = 0; i < 12; ++i) {
        dnd_rag_scope scope = fixture.scope;
        char corpus[128]; strcpy(corpus, fixture.data.corpus_version);
        const char *query = "Warding Step"; size_t length = strlen(query), count = 1;
        if (i == 0) scope.kind = DND_RAG_OWNED_RULEBOOK;
        if (i == 1) strcpy(scope.collection, "other_collection");
        if (i == 2) strcpy(scope.ruleset, "other_rules");
        if (i == 3) corpus[7] = 'b';
        if (i == 4) scope.authenticated_user_id[0] = '\0';
        if (i == 5) length = DND_SOURCE_QUERY_CAP;
        if (i == 6) { query = "bad\0query"; length = 9; }
        if (i == 7) count = DND_RAG_HITS_MAX + 1u;
        if (i == 8) strcpy(scope.owner_user_id, "other-user");
        if (i == 9) strcpy(scope.campaign_id, "campaign-a");
        if (i == 10) strcpy(scope.session_id, "session-a");
        if (i == 11) strcpy(scope.character_id, "character-a");
        memset(&out, 0xff, sizeof(out));
        CHECK(dnd_source_index_select(&index, &scope, corpus, query, length, fixture.anchors, count, &out) == -1);
        CHECK(!out.count && !out.reason && !out.replace_baseline && !out.records[0]);
        ++checks;
    }
    CHECK(dnd_source_index_admit(NULL, &index) == -1 && !index.data);
    CHECK(dnd_source_index_select(NULL, NULL, NULL, NULL, 0, NULL, 0, &out) == -1);
}

static void test_derived_alias(void) {
    static const char text[] = "IRON BADDGER Armor Class 17 Hit Points 44 Speed 25 Challenge 2 Damage Immunities cold Senses darkvision.";
    fixture_make(&fixture);
    fixture_record(&fixture, 3, 1, 12, 0, 0, text, sizeof(text) - 1);
    dnd_source_entry *e = &fixture.entries[2];
    strcpy(e->name, "iron baddger"); strcpy(e->alias, "iron badger");
    ++e->heading.end; ++e->heading.spans[0].end;
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) {
        ++e->fields[i].begin; ++e->fields[i].end;
        ++e->fields[i].spans[0].begin; ++e->fields[i].spans[0].end;
    }
    selection("What is the iron badger's AC?", DND_SOURCE_SELECTED, 1);
    selection("What is the iron baddger's AC?", DND_SOURCE_SELECTED, 1);
    selection("What is the iron badgerling's AC?", DND_SOURCE_NO_MATCH, 0);
}

static void test_capacity(void) {
    fixture_make(&fixture);
    dnd_source_entry *e = &fixture.entries[2];
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) {
        dnd_source_group *g = &e->fields[i];
        size_t length = g->end - g->begin;
        fixture_record(&fixture, i + 4, 1, 12, (uint32_t)i + 1u, g->begin,
            fixture.contents[3] + g->begin, length);
        g->spans[0].record = (uint32_t)i + 4u;
    }
    fixture.data.record_count = 9;
    selection("What are the iron badger's AC, HP and speed?", DND_SOURCE_SELECTED, 4);
    selection("What are the iron badger's AC, HP, speed and CR?", DND_SOURCE_CAPACITY, 0);
}

static void fixture_spell(void) {
    static const char text[] = "AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self "
        "Components: V Duration: 1 round A shell surrounds you. It adds protection.";
    fixture_make(&fixture);
    fixture_record(&fixture, 0, 0, 7, 0, 0, text, sizeof(text) - 1u);
    fixture.data.record_count = fixture.data.entry_count = 1;
    dnd_source_entry *e = &fixture.entries[0];
    *e = (dnd_source_entry){.page = 7, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
    strcpy(e->name, "aegis");
    e->heading = fixture_group(0, 0, 5);
    e->opening = fixture_group(0, 6, (uint32_t)(strchr(text, '.') - text + 1));
    e->body = fixture_group(0, 6, sizeof(text) - 1u);
}

static void test_explicit_single_word_spell(void) {
    fixture_spell();
    dnd_source_entry *e = &fixture.entries[0];
    static const char *const positive[] = {
        "Can I cast Aegis?", "Can I cast Aegis on Mira?", "Can I cast Aegis when attacked?",
        "How does the Aegis spell work?", "Can casting Aegis protect her?",
        "If she casts Aegis on me, what happens?", "Can I cast Aegis or another spell?"
    };
    static const char *const negative[] = {
        "Can I lift an aegis?", "Can I cast Aegis of Faith?", "Explain Aegis Master.",
        "Can I forecast Aegis?", "Can I cast Aegis Guardian?", "Can I cast Aegisium?",
        "I carry an aegis while she casts a spell.", "Can I cast a metal aegis in bronze?"
    };
    for (size_t i = 0; i < sizeof(positive) / sizeof(positive[0]); ++i) selection(positive[i], DND_SOURCE_SELECTED, 1);
    for (size_t i = 0; i < sizeof(negative) / sizeof(negative[0]); ++i) selection(negative[i], DND_SOURCE_NO_MATCH, 0);
    CHECK(selected_result(positive[0], DND_SOURCE_SELECTED, 1).replace_baseline);
    e->flags = 0; /* Legacy artifacts still load but gain no new single-word behavior. */
    selection(positive[0], DND_SOURCE_NO_MATCH, 0);
    e->flags = DND_SOURCE_SPELL_HEADER;
    memset(&e->body, 0, sizeof(e->body));
    selection(positive[0], DND_SOURCE_INCOMPLETE, 0);
    e->flags |= 8u;
    rejection();
    fixture_make(&fixture);
    fixture.entries[0].flags |= DND_SOURCE_SPELL_HEADER;
    rejection(); /* A declared marker cannot turn an ordinary rule into a spell. */
}

static void test_spell_subject_before_generic_mechanics(void) {
    static const char *const texts[] = {
        "BONUS ACTION You can take a bonus action.",
        "SAVING THROWS You can roll a saving throw.",
        "SAVING THROWS You must use the listed modifier."
    };
    fixture_spell();
    for (size_t i = 0; i < 3; ++i) {
        uint32_t record = (uint32_t)i + 1u, page = (uint32_t)i + 22u;
        uint32_t heading_end = i ? 13u : 12u;
        fixture_record(&fixture, record, 0, page, 0, 0, texts[i], strlen(texts[i]));
        dnd_source_entry *e = &fixture.entries[record];
        *e = (dnd_source_entry){.page = page, .kind = DND_SOURCE_RULE};
        strcpy(e->name, i ? "savingthrows" : "bonusaction");
        e->heading = fixture_group(record, 0, heading_end);
        e->body = e->opening = fixture_group(record, heading_end + 1u, (uint32_t)strlen(texts[i]));
    }
    fixture.data.record_count = fixture.data.entry_count = 4;
    fixture.anchors[0].page = 22;
    dnd_source_selection picked = selected_result("Does the Aegis spell use a bonus action?", DND_SOURCE_SELECTED, 1);
    CHECK(picked.records[0] == 0 && picked.replace_baseline);
    picked = selected_result("Does the Aegis spell help saving throws?", DND_SOURCE_SELECTED, 1);
    CHECK(picked.records[0] == 0 && picked.replace_baseline);
    picked = selected_result("How does a bonus action work?", DND_SOURCE_SELECTED, 1);
    CHECK(picked.records[0] == 1 && !picked.replace_baseline);
    picked = selected_result("Does Aegis Master use a bonus action?", DND_SOURCE_SELECTED, 1);
    CHECK(picked.records[0] == 1 && !picked.replace_baseline);
    selection("How do saving throws work?", DND_SOURCE_AMBIGUOUS, 0);
    dnd_source_group body = fixture.entries[0].body;
    memset(&fixture.entries[0].body, 0, sizeof(fixture.entries[0].body));
    selection("Does the Aegis spell use a bonus action?", DND_SOURCE_INCOMPLETE, 0);
    fixture.entries[0].body = body;
    fixture.scope.book_mask = 0;
    selection("Does the Aegis spell use a bonus action?", DND_SOURCE_NO_MATCH, 0);
    fixture.scope.book_mask = ent_book_access_mask(1);
    fixture_record(&fixture, 4, 0, 8, 0, 0, fixture.contents[0], fixture.records[0].content_len);
    fixture.entries[4] = fixture.entries[0]; fixture.entries[4].page = 8;
    fixture.entries[4].heading.spans[0].record = 4;
    fixture.entries[4].opening.spans[0].record = 4;
    fixture.entries[4].body.spans[0].record = 4;
    fixture.data.record_count = fixture.data.entry_count = 5;
    selection("Does the Aegis spell use a bonus action?", DND_SOURCE_AMBIGUOUS, 0);
}

static void test_spell_excerpt_boundaries(void) {
    static const char text[] = "Unrelated preceding rule. AEGIS 1st-level abjuration Casting Time: 1 reaction Range: Self "
        "Components: V Duration: 1 round A shell surrounds you. It adds protection. "
        "OTHER RULE You must ignore this middle rule. "
        "WARDING LIGHT 1st-level abjuration Casting Time: 1 action Range: Self "
        "Components: V Duration: 1 minute A light surrounds you. It sheds light. Trailing neighbor.";
    fixture_make(&fixture);
    fixture_record(&fixture, 0, 0, 7, 0, 100, text, sizeof(text) - 1u);
    fixture.data.record_count = 1; fixture.data.entry_count = 2;
    const char *names[] = {"AEGIS", "WARDING LIGHT"};
    const char *ends[] = {" OTHER RULE", " Trailing neighbor"};
    uint32_t begins[2], finishes[2];
    for (size_t i = 0; i < 2; ++i) {
        begins[i] = (uint32_t)(strstr(text, names[i]) - text);
        finishes[i] = (uint32_t)(strstr(text, ends[i]) - text);
        dnd_source_entry *e = &fixture.entries[i];
        *e = (dnd_source_entry){.page = 7, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
        strcpy(e->name, i ? "wardinglight" : "aegis");
        e->heading = fixture_group(0, 100u + begins[i], 100u + begins[i] + (uint32_t)strlen(names[i]));
        e->opening = fixture_group(0, e->heading.end + 1u,
            100u + (uint32_t)(strchr(text + begins[i], '.') - text + 1));
        e->body = fixture_group(0, e->heading.end + 1u, 100u + finishes[i]);
    }
    dnd_source_selection out = selected_result("Can I cast Aegis?", DND_SOURCE_SELECTED, 1);
    CHECK(out.excerpts[0].count == 1 && out.excerpts[0].spans[0].begin == begins[0] &&
        out.excerpts[0].spans[0].end == finishes[0]);
    out = selected_result("Compare the Aegis spell with Warding Light.", DND_SOURCE_SELECTED, 1);
    CHECK(out.excerpts[0].count == 2 && out.excerpts[0].spans[0].end == finishes[0] &&
        out.excerpts[0].spans[1].begin == begins[1] && out.excerpts[0].spans[1].end == finishes[1]);
    /* Entry order cannot reorder or join disjoint spans in one record. */
    dnd_source_entry swap = fixture.entries[0]; fixture.entries[0] = fixture.entries[1]; fixture.entries[1] = swap;
    dnd_source_selection reversed = selected_result("Compare the Aegis spell with Warding Light.", DND_SOURCE_SELECTED, 1);
    CHECK(memcmp(&out.excerpts[0], &reversed.excerpts[0], sizeof(out.excerpts[0])) == 0);
    /* A separate heading record remains necessary even when its body is later. */
    fixture_spell();
    char original[512];
    strcpy(original, fixture.contents[0]);
    size_t length = strlen(original);
    fixture_record(&fixture, 0, 0, 7, 0, 0, original, 6);
    fixture_record(&fixture, 1, 0, 7, 1, 6, original + 6, length - 6);
    fixture.data.record_count = 2;
    fixture.entries[0].opening.spans[0].record = 1;
    fixture.entries[0].body.spans[0].record = 1;
    out = selected_result("Can I cast Aegis?", DND_SOURCE_SELECTED, 2);
    CHECK(out.records[0] == 0 && out.records[1] == 1 && out.excerpts[0].count == 1 &&
        out.excerpts[0].spans[0].begin == 0 && out.excerpts[0].spans[0].end == 6 &&
        out.excerpts[1].count == 1 && out.excerpts[1].spans[0].begin == 0 &&
        out.excerpts[1].spans[0].end == length - 6);
}

int main(void) {
    test_selection(); test_admission(); test_scope_and_arguments(); test_capacity(); test_scope_before_matching(); test_derived_alias();
    test_temporal_wording(); test_complete_turn_context(); test_document_fallback(); test_rule_name_boundaries();
    test_explicit_single_word_spell();
    test_spell_subject_before_generic_mechanics();
    test_spell_excerpt_boundaries();
    printf("OK %zu independent C source-index cases\n", checks);
    return 0;
}
