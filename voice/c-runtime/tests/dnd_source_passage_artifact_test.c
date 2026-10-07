#include "dnd_source_encode.h"
#include "dnd_source_passage_fixture.h"
#include <stdlib.h>

static source_fixture f;
static artifact_fixture a, bad;
static dnd_source_passage_definition defs[2];
static size_t checks;
static void reject(artifact_fixture *b) {
    dnd_source_artifact *loaded = NULL;
    artifact_rehash(b); /* Test inner admission, independently of outer digest. */
    assert(dnd_source_artifact_decode(b->bytes, b->length, &b->expected, &loaded) == -1);
    assert(!loaded); ++checks;
}
static void set_number(artifact_fixture *b, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) b->bytes[at + i] = (unsigned char)(value >> (8u * i));
}
static void rebuild_reject(void) {
    dnd_source_passage out, zero = {0};
    memset(&out, 0x55, sizeof(out));
    assert(dnd_source_rebuild_passage(f.records, f.data.record_count, &defs[0], &out) != 0);
    assert(!memcmp(&out, &zero, sizeof(out))); ++checks;
}
static void capacity(void) {
    passage_fixture_make(&f, defs);
    size_t n = DND_SOURCE_PASSAGES_MAX;
    dnd_source_record *records = calloc(n + 4u, sizeof(*records));
    dnd_source_passage_definition *many = calloc(n, sizeof(*many));
    assert(records && many);
    memcpy(records, f.records, 4u * sizeof(*records));
    for (size_t i = 0; i < n; ++i) {
        records[i + 4u] = f.records[6];
        assert(snprintf(records[i + 4u].record_id, 65, "%064zu", i + 5u) == 64);
        records[i + 4u].page = 100u + (uint32_t)i;
        many[i] = defs[1]; many[i].first_page = many[i].last_page = records[i + 4u].page;
    }
    dnd_source_data data = f.data; data.records = records; data.record_count = n + 4u;
    artifact_make(&a, &f);
    unsigned char *wire = NULL; size_t length = 0;
    assert(!dnd_source_encode_passages(&data, many, n, a.expected.manifest_sha256,
        a.expected.compiler_sha256, &wire, &length));
    fixture_hash((const char *)wire, length, a.expected.artifact_sha256);
    dnd_source_artifact *loaded = NULL;
    assert(!dnd_source_artifact_decode(wire, length, &a.expected, &loaded));
    size_t count = 0;
    const dnd_source_passage *p = dnd_source_artifact_passages(loaded, &count);
    assert(count == n && p && p[n - 1u].page_start == 100u + n - 1u);
    for (size_t i = 0; i < n; ++i)
        assert(p[i].count == 1 && p[i].spans[0].record == i + 4u && !strcmp(p[i].name, "beacon"));
    printf("OK %zu passage capacity; wire=%zu cache=%zu bytes\n", n, length, n * sizeof(*p));
    dnd_source_artifact_free(loaded); free(wire); free(many); free(records);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--capacity")) { capacity(); return 0; }
    assert(argc == 1);
    passage_fixture_make(&f, defs);
    size_t start = passage_artifact_make(&a, &f, defs);
    dnd_source_artifact *loaded = NULL;
    assert(!dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded));
    size_t count = 0;
    const dnd_source_passage *p = dnd_source_artifact_passages(loaded, &count);
    assert(count == 2 && p && !strcmp(p[0].name, "aegis") && !strcmp(p[1].name, "beacon"));
    assert(!strcmp(p[0].text, passage_assembly) && p[0].count == 2 && p[0].page_start == 40 && p[0].page_end == 41);
    assert(p[0].spans[0].record == 4 && p[0].spans[1].record == 5);
    assert(p[0].spans[1].end == 20 && p[0].spans[1].begin == 0);
    const dnd_source_passage_definition *stored = dnd_source_artifact_passage_definitions(loaded, &count);
    assert(stored && count == 2 && !memcmp(stored, defs, sizeof(defs)));
    assert(dnd_source_artifact_passages(loaded, NULL) == p);
    assert(dnd_source_artifact_passage_definitions(loaded, NULL) == stored);
    ++checks;
    /* Failed replacement preserves the live artifact and all views. */
    assert(dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded) == -1);
    assert(dnd_source_artifact_passages(loaded, NULL) == p); ++checks;
    memset(f.contents, 0, sizeof(f.contents)); memset(a.bytes, 0, a.length);
    assert(!strcmp(p[0].text, passage_assembly));
    const dnd_source_data *data = dnd_source_artifact_index(loaded)->data;
    assert(!memcmp(data->records[4].content, passage_first, strlen(passage_first))); ++checks;
    dnd_source_artifact_free(loaded); loaded = NULL;
    count = 99; assert(!dnd_source_artifact_passages(NULL, &count) && !count);
    count = 99; assert(!dnd_source_artifact_passage_definitions(NULL, &count) && !count); ++checks;

    passage_fixture_make(&f, defs); start = passage_artifact_make(&a, &f, defs);
    unsigned char *wire = NULL; size_t length = 0;
    assert(!dnd_source_encode_passages(&f.data, defs, 2, a.expected.manifest_sha256,
        a.expected.compiler_sha256, &wire, &length));
    assert(length == a.length && !memcmp(wire, a.bytes, length)); ++checks;
    unsigned char *saved = wire;
    assert(dnd_source_encode_passages(&f.data, defs, 2, a.expected.manifest_sha256,
        a.expected.compiler_sha256, &wire, &length) == -1);
    assert(saved == wire && length == a.length); ++checks;
    free(wire); wire = NULL; length = 0;
    for (size_t n = 0; n <= DND_SOURCE_PASSAGES_MAX + 1u; n += DND_SOURCE_PASSAGES_MAX + 1u) {
        assert(dnd_source_encode_passages(&f.data, defs, n, a.expected.manifest_sha256,
            a.expected.compiler_sha256, &wire, &length) == -1);
        assert(!wire && !length); ++checks;
    }
    for (size_t which = 0; which < 3; ++which) {
        dnd_source_passage_definition changed[2] = {defs[1], defs[0]};
        if (which == 1) changed[0] = changed[1];
        if (which == 2) { changed[0] = defs[0]; changed[1] = defs[1]; changed[1].document = 2; }
        assert(dnd_source_encode_passages(&f.data, changed, 2, a.expected.manifest_sha256,
            a.expected.compiler_sha256, &wire, &length) == -1);
        assert(!wire && !length); ++checks;
    }
    assert(dnd_source_encode_passages(&f.data, NULL, 1, a.expected.manifest_sha256,
        a.expected.compiler_sha256, &wire, &length) == -1); ++checks;
    /* Every truncation must reject, including a truncated passage table. */
    for (size_t n = 0; n < a.length; ++n) { bad = a; bad.length = n; reject(&bad); }
    bad = a; bad.bytes[bad.length++] = 0; reject(&bad);
    bad = a; bad.bytes[7] = '3'; reject(&bad);
    bad = a; bad.bytes[start + 32] = bad.bytes[start + 32] == 'a' ? 'b' : 'a'; reject(&bad); /* assembly hash */
    bad = a; bad.bytes[start + 32] = 'X'; reject(&bad);
    bad = a; set_number(&bad, start + 28, 63); reject(&bad);
    for (uint32_t n = 0; n < 3; ++n) {
        bad = a; set_number(&bad, a.counts + 12u, n == 0 ? 0 : n == 1 ? 4097 : UINT32_MAX); reject(&bad);
    }
    for (size_t field = 0; field < 7; ++field) {
        bad = a; set_number(&bad, start + field * 4u, UINT32_MAX); reject(&bad);
    }
    bad = a; set_number(&bad, start, 1); reject(&bad); /* allowed book, wrong records */
    bad = a; set_number(&bad, start + 4, 39); reject(&bad); /* absent source page */
    bad = a; set_number(&bad, start + 12, 1); reject(&bad); /* wrong title boundary */
    bad = a; set_number(&bad, start + 24, 28); reject(&bad); /* shortened next title */
    bad = a; memcpy(bad.bytes + start + 96, bad.bytes + start, 96); reject(&bad); /* duplicate */
    bad = a; memcpy(bad.bytes + start, a.bytes + start + 96, 96);
    memcpy(bad.bytes + start + 96, a.bytes + start, 96); reject(&bad); /* reversed */
    bad = a; bad.bytes[a.records[4] + 72] ^= 1; reject(&bad); /* original content hash */
    bad = a; bad.bytes[8] ^= 1; reject(&bad);
    bad = a; bad.bytes[40] ^= 1; reject(&bad);
    bad = a; bad.expected.collection[0] = 'x'; reject(&bad);
    bad = a; bad.expected.document_count = 1; reject(&bad);

    /* V1 is still independently byte-identical, with empty passage views. */
    artifact_make(&a, &f);
    assert(!dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length));
    assert(length == a.length && !memcmp(wire, a.bytes, length)); free(wire);
    assert(!dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded));
    count = 99; assert(!dnd_source_artifact_passages(loaded, &count) && !count);
    count = 99; assert(!dnd_source_artifact_passage_definitions(loaded, &count) && !count);
    dnd_source_artifact_free(loaded); ++checks;

    /* Reconstruction checks lengths before touching bytes and rejects incomplete pages. */
    passage_fixture_make(&f, defs); f.records[4].content = NULL; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].content_len = SIZE_MAX; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].begin = UINT32_MAX; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].begin = 1; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].chunk = 1; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].page = 41; rebuild_reject(); /* duplicate chunk */
    passage_fixture_make(&f, defs); f.records[5].page = 42; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[5].document = 1; rebuild_reject();
    passage_fixture_make(&f, defs);
    fixture_record(&f, 7, 0, 40, 1, 10, "disagree", 8); f.data.record_count = 8; rebuild_reject();
    passage_fixture_make(&f, defs); f.records[4].content_hash[0] ^= 1; rebuild_reject();
    passage_fixture_make(&f, defs); defs[0].last_page = 43; rebuild_reject();
    printf("OK %zu passage artifact checks\n", checks);
    return 0;
}
