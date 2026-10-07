#ifndef DND_SOURCE_FIXTURE_H
#define DND_SOURCE_FIXTURE_H
#include "dnd_source_index.h"
#include "ent_books.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    dnd_source_data data;
    dnd_source_document documents[4];
    dnd_source_record records[16];
    dnd_source_entry entries[8];
    char contents[16][512];
    dnd_rag_scope scope;
    dnd_source_anchor anchors[4];
    size_t anchor_count;
} source_fixture;

static inline void fixture_hash(const char *text, size_t length, char hash[65]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    (void)SHA256((const unsigned char *)text, length, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) (void)snprintf(hash + i * 2, 3, "%02x", (unsigned)digest[i]);
}

static inline void fixture_record(source_fixture *f, size_t i, uint32_t doc, uint32_t page,
    uint32_t chunk, uint32_t begin, const char *text, size_t length) {
    dnd_source_record *r = &f->records[i];
    (void)snprintf(r->record_id, sizeof(r->record_id), "%064zu", i + 1);
    memcpy(f->contents[i], text, length);
    f->contents[i][length] = '\0';
    r->document = doc; r->page = page; r->chunk = chunk; r->begin = begin;
    r->content = f->contents[i]; r->content_len = length;
    fixture_hash(text, length, r->content_hash);
}

static inline dnd_source_group fixture_group(uint32_t record, uint32_t begin, uint32_t end) {
    dnd_source_group g = {begin, end, 1, {{record, begin, end}, {0, 0, 0}}};
    return g;
}

static inline void fixture_complete_reaction(source_fixture *f, uint32_t second_record) {
    static const char text[] = "REACTIONS A reaction is an instant response that can occur on your turn or on someone else's turn. A counter-strike uses a reaction. After a reaction, you must wait until your next turn.";
    uint32_t opening_end = (uint32_t)(strchr(text, '.') - text + 1);
    uint32_t split = (uint32_t)(strstr(text, "uses a reaction") - text);
    uint32_t second_begin = opening_end - 12u;
    fixture_record(f, 2, 0, 22, 0, 0, text, split);
    fixture_record(f, second_record, 0, 22, 1, second_begin,
        text + second_begin, strlen(text) - second_begin);
    dnd_source_entry *e = &f->entries[1];
    e->opening = fixture_group(2, 10, opening_end);
    e->body = fixture_group(2, 10, (uint32_t)strlen(text));
    e->body.count = 2;
    e->body.spans[0].end = split;
    e->body.spans[1] = (dnd_source_span){second_record, split, (uint32_t)strlen(text)};
    if (f->data.record_count <= second_record) f->data.record_count = (size_t)second_record + 1u;
}

static inline void fixture_make(source_fixture *f) {
    static const char rule[] = "WARDING STEP Once per turn, you can step if you see the target. You must remain within 15 feet.";
    static const char reaction[] = "REACTIONS A reaction is an instant response that can occur on your turn or on someone else's turn.";
    static const char creature[] = "IRON BADGER Armor Class 17 Hit Points 44 Speed 25 Challenge 2 Damage Immunities cold Senses darkvision.";
    static const char *const markers[] = {"Armor Class", "Hit Points", "Speed", "Challenge", "Damage Immunities", "Senses"};
    memset(f, 0, sizeof(*f));
    strcpy(f->data.collection, "reviewed_books");
    strcpy(f->data.corpus_version, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    strcpy(f->data.ruleset, "dnd-5e-2014");
    f->data.documents = f->documents; f->data.document_count = 2;
    f->data.records = f->records; f->data.record_count = 4;
    f->data.entries = f->entries; f->data.entry_count = 3;
    strcpy(f->documents[0].document_id, "rules-a");
    strcpy(f->documents[0].book_slug, "players-handbook");
    strcpy(f->documents[1].document_id, "creatures-a");
    strcpy(f->documents[1].book_slug, "monster-manual");
    for (size_t i = 0; i < 2; ++i) memset(f->documents[i].source_sha256, 'b', 64);
    uint32_t split = (uint32_t)(strstr(rule, "15 feet") - rule);
    uint32_t first_end = (uint32_t)(strchr(rule, '.') - rule + 1);
    fixture_record(f, 0, 0, 7, 0, 0, rule, split);
    fixture_record(f, 1, 0, 7, 1, 24, rule + 24, strlen(rule) - 24);
    fixture_record(f, 2, 0, 22, 0, 0, reaction, strlen(reaction));
    fixture_record(f, 3, 1, 12, 0, 0, creature, strlen(creature));
    dnd_source_entry *e = &f->entries[0];
    strcpy(e->name, "wardingstep"); e->kind = DND_SOURCE_RULE; e->page = 7;
    e->flags = DND_SOURCE_ONCE_PER_TURN;
    e->heading = fixture_group(0, 0, 12);
    e->opening = fixture_group(0, 13, first_end);
    e->body = fixture_group(0, 13, (uint32_t)strlen(rule));
    e->body.count = 2;
    e->body.spans[0].end = split;
    e->body.spans[1] = (dnd_source_span){1, split, (uint32_t)strlen(rule)};
    e = &f->entries[1];
    strcpy(e->name, "reactions"); e->kind = DND_SOURCE_RULE; e->page = 22;
    e->flags = DND_SOURCE_OTHER_TURN_REACTION;
    e->heading = fixture_group(2, 0, 9);
    e->body = e->opening = fixture_group(2, 10, (uint32_t)strlen(reaction));
    e = &f->entries[2];
    strcpy(e->name, "iron badger"); e->kind = DND_SOURCE_CREATURE; e->document = 1; e->page = 12;
    e->heading = fixture_group(3, 0, 11);
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) {
        uint32_t begin = (uint32_t)(strstr(creature, markers[i]) - creature);
        uint32_t end = (uint32_t)(strstr(creature, markers[i + 1]) - creature - 1);
        e->fields[i] = fixture_group(3, begin, end);
    }
    f->scope.kind = DND_RAG_SHARED_RULEBOOK;
    strcpy(f->scope.authenticated_user_id, "fixture-user");
    strcpy(f->scope.collection, f->data.collection);
    strcpy(f->scope.ruleset, f->data.ruleset);
    f->scope.book_mask = ent_book_access_mask(1);
    f->anchors[0] = (dnd_source_anchor){0, 7};
    f->anchor_count = 1;
}
#endif
