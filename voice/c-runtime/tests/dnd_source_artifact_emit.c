#include "dnd_source_passage_fixture.h"
#include <stdlib.h>

static void write_file(const char *directory, const char *name, const void *bytes, size_t length) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *file = fopen(path, "wbx"); assert(file);
    assert(fwrite(bytes, 1, length, file) == length && !fclose(file));
}

int main(int argc, char **argv) {
    static source_fixture f;
    static artifact_fixture a;
    static char manifest[4096], rows[16384];
    static const char *const keys[] = {"corpus/source/dnd-books/rules.pdf", "corpus/source/dnd-books/creatures.pdf"};
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    assert(argc == 2 || (argc == 3 && (!strcmp(argv[2], "--complete-reaction") || !strcmp(argv[2], "--spell") || !strcmp(argv[2], "--passages"))));
    dnd_source_passage_definition defs[2] = {0};
    fixture_make(&f);
    static const char *const section_text[] = {
        "CONCENTRATION Taking damage. When a pulse hits, you must make a check. Its threshold is 13.",
        "CONCENTRATION Taking damage. When a pulse hits, you must make a check. Its threshold is 17."
    };
    for (size_t i = 0; i < 2; ++i) {
        uint32_t record = (uint32_t)i + 4u;
        fixture_record(&f, record, (uint32_t)i, 33, 0, 0, section_text[i], strlen(section_text[i]));
        dnd_source_entry *e = &f.entries[i + 3];
        *e = (dnd_source_entry){.document = (uint32_t)i, .page = 33, .kind = DND_SOURCE_SECTION};
        strcpy(e->name, "takingdamage"); strcpy(e->alias, "concentration");
        e->fields[0] = fixture_group(record, 0, 13);
        e->heading = fixture_group(record, 14, 28);
        e->body = fixture_group(record, 29, (uint32_t)strlen(section_text[i]));
        e->opening = fixture_group(record, 29, (uint32_t)(strchr(section_text[i] + 29, '.') - section_text[i] + 1));
    }
    f.data.record_count = 6; f.data.entry_count = 5;
    if (argc == 3 && !strcmp(argv[2], "--complete-reaction")) fixture_complete_reaction(&f, 6);
    if (argc == 3 && !strcmp(argv[2], "--spell")) {
        static const char text[] = "Neighbor grants 99 AC. AEGIS 1st-level abjuration Casting Time: 1 action Range: Self "
            "Components: V Duration: 1 round A shell surrounds you. It adds protection. NEXT RULE grants immunity.";
        uint32_t begin = (uint32_t)(strstr(text, "AEGIS") - text);
        uint32_t end = (uint32_t)(strstr(text, " NEXT RULE") - text);
        fixture_record(&f, 0, 0, 7, 0, 0, text, sizeof(text) - 1u);
        f.data.record_count = f.data.entry_count = 1;
        dnd_source_entry *e = &f.entries[0];
        *e = (dnd_source_entry){.page = 7, .kind = DND_SOURCE_RULE, .flags = DND_SOURCE_SPELL_HEADER};
        strcpy(e->name, "aegis");
        e->heading = fixture_group(0, begin, begin + 5u);
        e->opening = fixture_group(0, begin + 6u, (uint32_t)(strchr(text + begin, '.') - text + 1));
        e->body = fixture_group(0, begin + 6u, end);
    }
    if (argc == 3 && !strcmp(argv[2], "--passages")) passage_fixture_split(&f, defs);
    strcpy(f.data.collection, "dnd_text_chunks_c_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    strcpy(f.documents[1].book_slug, "tashas-cauldron-of-everything");
    for (size_t i = 0; i < 2; ++i) {
        char identity[256];
        int n = snprintf(identity, sizeof(identity), "%s:%s", f.data.corpus_version, keys[i]);
        assert(n > 0 && (size_t)n < sizeof(identity));
        assert(SHA256((const unsigned char *)identity, (size_t)n, digest));
        for (size_t j = 0; j < 12; ++j) {
            f.documents[i].document_id[j * 2] = hex[digest[j] >> 4];
            f.documents[i].document_id[j * 2 + 1] = hex[digest[j] & 15];
        }
        f.documents[i].document_id[24] = 0;
    }
    int n = snprintf(manifest, sizeof(manifest),
        "{\"schema_version\":\"dnd-corpus-manifest/v1\",\"corpus_id\":\"%s\",\"enabled_sources\":3,\"sources\":["
        "{\"enabled\":true,\"book_slug\":\"%s\",\"source_sha256\":\"%s\",\"etag\":\"\",\"source_key\":\"%s\"},"
        "{\"enabled\":true,\"book_slug\":\"%s\",\"source_sha256\":\"%s\",\"etag\":\"\",\"source_key\":\"%s\"},"
        "{\"enabled\":true,\"book_slug\":\"players-handbook\",\"source_sha256\":\"%s\",\"etag\":\"appendix-etag\",\"source_key\":\"appendix.pdf\"}]}",
        f.data.corpus_version, f.documents[0].book_slug, f.documents[0].source_sha256, keys[0],
        f.documents[1].book_slug, f.documents[1].source_sha256, keys[1], f.documents[0].source_sha256);
    assert(n > 0 && (size_t)n < sizeof(manifest));
    write_file(argv[1], "manifest.json", manifest, (size_t)n);
    if (argc == 3 && !strcmp(argv[2], "--passages")) (void)passage_artifact_make(&a, &f, defs);
    else artifact_make(&a, &f);
    assert(SHA256((const unsigned char *)manifest, (size_t)n, digest));
    memcpy(a.bytes + 8, digest, sizeof(digest));
    write_file(argv[1], "index.dndsidx", a.bytes, a.length);
    size_t used = 0;
    rows[used++] = '[';
    for (size_t i = 0; i < f.data.record_count; ++i) {
        const dnd_source_record *r = &f.records[i];
        const dnd_source_document *d = &f.documents[r->document];
        char escaped[4096];
        assert(!cmp_json_escape_exact(r->content, escaped, sizeof(escaped)));
        n = snprintf(rows + used, sizeof(rows) - used,
            "%s{\"record_id\":\"%s\",\"document_id\":\"%s\",\"book_slug\":\"%s\",\"content_hash\":\"%s\","
            "\"source_sha256\":\"%s\",\"page\":%u,\"chunk\":%u,\"content\":\"%s\"}",
            i ? "," : "", r->record_id, d->document_id, d->book_slug, r->content_hash, d->source_sha256,
            r->page, r->chunk, escaped);
        assert(n > 0 && (size_t)n < sizeof(rows) - used); used += (size_t)n;
    }
    rows[used++] = ']';
    write_file(argv[1], "records.json", rows, used);
    return 0;
}
