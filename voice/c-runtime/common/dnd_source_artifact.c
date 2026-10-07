#define _POSIX_C_SOURCE 200809L
#include "dnd_source_artifact.h"
#include "dnd_source_select_internal.h"
#include "utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct dnd_source_artifact {
    dnd_source_data data;
    dnd_source_index index;
    dnd_source_document *documents;
    dnd_source_record *records;
    dnd_source_entry *entries;
    dnd_source_document *reviewed;
    size_t reviewed_count, passage_count;
    dnd_source_passage_definition *definitions;
    dnd_source_passage *passages;
    unsigned char *bytes;
};

typedef struct { const unsigned char *next; size_t left; } cursor;

static int take(cursor *c, size_t n, const unsigned char **out) {
    if (n > c->left) return -1;
    *out = c->next;
    c->next += n; c->left -= n;
    return 0;
}

static int number(cursor *c, uint32_t *out) {
    const unsigned char *p;
    if (take(c, 4, &p)) return -1;
    *out = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return 0;
}

static int string(cursor *c, char *out, size_t capacity) {
    uint32_t n;
    const unsigned char *p;
    if (number(c, &n) || n >= capacity || take(c, n, &p) || memchr(p, 0, n)) return -1;
    memcpy(out, p, n); out[n] = '\0';
    return 0;
}

static int bounded(const char *s, size_t capacity) {
    return s[0] && memchr(s, 0, capacity) != NULL;
}

static int hash_bytes(const char *hex, unsigned char out[32]) {
    if (hex[64]) return -1;
    for (size_t i = 0; i < 64; ++i) {
        unsigned value;
        if (hex[i] >= '0' && hex[i] <= '9') value = (unsigned)(hex[i] - '0');
        else if (hex[i] >= 'a' && hex[i] <= 'f') value = (unsigned)(hex[i] - 'a' + 10);
        else return -1;
        if (!(i & 1)) out[i / 2] = (unsigned char)(value << 4);
        else out[i / 2] |= (unsigned char)value;
    }
    return 0;
}

static int metadata_valid(const dnd_source_artifact_expectation *e) {
    unsigned char hash[32];
    return e && !hash_bytes(e->artifact_sha256, hash) && !hash_bytes(e->manifest_sha256, hash) &&
        !hash_bytes(e->compiler_sha256, hash) && bounded(e->collection, sizeof(e->collection)) &&
        bounded(e->corpus_version, sizeof(e->corpus_version)) && bounded(e->ruleset, sizeof(e->ruleset));
}

static int expectation_valid(const dnd_source_artifact_expectation *e) {
    unsigned char hash[32];
    if (!metadata_valid(e) ||
        !e->documents || !e->document_count || e->document_count > DND_SOURCE_DOCUMENTS_MAX) return 0;
    for (size_t i = 0; i < e->document_count; ++i) {
        const dnd_source_document *d = &e->documents[i];
        if (!bounded(d->document_id, sizeof(d->document_id)) || !bounded(d->book_slug, sizeof(d->book_slug)) ||
            hash_bytes(d->source_sha256, hash)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(d->document_id, e->documents[j].document_id)) return 0;
    }
    return 1;
}

static int header(cursor *c, const dnd_source_artifact_expectation *e, dnd_source_data *d, size_t *passage_count) {
    const unsigned char *p;
    unsigned char hash[32];
    uint32_t documents, records, entries, passages = 0;
    if (take(c, 8, &p) || (memcmp(p, "DNDSIDX1", 8) && memcmp(p, "DNDSIDX2", 8))) return -1;
    int version2 = p[7] == '2';
    if (hash_bytes(e->manifest_sha256, hash) || take(c, 32, &p) || memcmp(p, hash, 32) ||
        hash_bytes(e->compiler_sha256, hash) || take(c, 32, &p) || memcmp(p, hash, 32) ||
        string(c, d->collection, sizeof(d->collection)) || strcmp(d->collection, e->collection) ||
        string(c, d->corpus_version, sizeof(d->corpus_version)) || strcmp(d->corpus_version, e->corpus_version) ||
        string(c, d->ruleset, sizeof(d->ruleset)) || strcmp(d->ruleset, e->ruleset) ||
        number(c, &documents) || !documents || documents > DND_SOURCE_DOCUMENTS_MAX ||
        number(c, &records) || !records || records > DND_SOURCE_RECORDS_MAX ||
        number(c, &entries) || !entries || entries > DND_SOURCE_ENTRIES_MAX) return -1;
    if (version2 && (number(c, &passages) || !passages || passages > DND_SOURCE_PASSAGES_MAX)) return -1;
    *passage_count = passages;
    d->document_count = documents; d->record_count = records; d->entry_count = entries;
    return 0;
}

static int document(cursor *c, const dnd_source_artifact_expectation *e, dnd_source_document *d) {
    if (string(c, d->document_id, sizeof(d->document_id)) || string(c, d->book_slug, sizeof(d->book_slug)) ||
        string(c, d->source_sha256, sizeof(d->source_sha256))) return -1;
    for (size_t i = 0; i < e->document_count; ++i) {
        const dnd_source_document *allowed = &e->documents[i];
        if (!strcmp(d->document_id, allowed->document_id) && !strcmp(d->book_slug, allowed->book_slug) &&
            !strcmp(d->source_sha256, allowed->source_sha256)) return 0;
    }
    return -1;
}

static int record(cursor *c, dnd_source_record *r) {
    uint32_t n;
    const unsigned char *p;
    if (string(c, r->record_id, sizeof(r->record_id)) || string(c, r->content_hash, sizeof(r->content_hash)) ||
        number(c, &r->document) || number(c, &r->page) || number(c, &r->chunk) || number(c, &r->begin) ||
        number(c, &n) || !n || n >= DND_RAG_CONTENT_CAP || take(c, n, &p)) return -1;
    r->content = (const char *)p; r->content_len = n;
    return 0;
}

static int group(cursor *c, dnd_source_group *g) {
    if (number(c, &g->begin) || number(c, &g->end) || number(c, &g->count) ||
        g->count > DND_SOURCE_SPANS_MAX) return -1;
    for (size_t i = 0; i < DND_SOURCE_SPANS_MAX; ++i)
        if (number(c, &g->spans[i].record) || number(c, &g->spans[i].begin) || number(c, &g->spans[i].end)) return -1;
    return 0;
}

static int entry(cursor *c, dnd_source_entry *e) {
    if (string(c, e->name, sizeof(e->name)) || string(c, e->alias, sizeof(e->alias)) ||
        number(c, &e->document) || number(c, &e->page) || number(c, &e->kind) || number(c, &e->flags) ||
        group(c, &e->heading) || group(c, &e->body) || group(c, &e->opening)) return -1;
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) if (group(c, &e->fields[i])) return -1;
    return 0;
}

static int passage(cursor *c, dnd_source_passage_definition *d, char assembly[65]) {
    unsigned char hash[32];
    return number(c, &d->document) || number(c, &d->first_page) || number(c, &d->last_page) ||
        number(c, &d->heading_begin) || number(c, &d->heading_end) ||
        number(c, &d->next_heading_begin) || number(c, &d->next_heading_end) ||
        string(c, assembly, 65) || hash_bytes(assembly, hash) ? -1 : 0;
}

/* The first walk rejects truncated or impossible tables before allocating them.
 * The second fills owned arrays. No native structure is read from wire bytes. */
static int tables(cursor c, const dnd_source_artifact_expectation *e, dnd_source_artifact *a, int fill) {
    for (size_t i = 0; i < a->data.document_count; ++i) {
        dnd_source_document d = {0};
        if (document(&c, e, &d)) return -1;
        if (fill) a->documents[i] = d;
    }
    for (size_t i = 0; i < a->data.record_count; ++i) {
        dnd_source_record r = {0};
        if (record(&c, &r)) return -1;
        if (fill) a->records[i] = r;
    }
    for (size_t i = 0; i < a->data.entry_count; ++i) {
        dnd_source_entry n = {0};
        if (entry(&c, &n)) return -1;
        if (fill) a->entries[i] = n;
    }
    for (size_t i = 0; i < a->passage_count; ++i) {
        dnd_source_passage_definition d = {0};
        char assembly[65] = {0};
        if (passage(&c, &d, assembly)) return -1;
        if (fill) { a->definitions[i] = d; memcpy(a->passages[i].text_sha256, assembly, 65); }
    }
    return c.left ? -1 : 0;
}

void dnd_source_artifact_free(dnd_source_artifact *a) {
    if (!a) return;
    free(a->definitions); free(a->passages);
    free(a->documents); free(a->records); free(a->entries); free(a->reviewed); free(a->bytes); free(a);
}

const dnd_source_index *dnd_source_artifact_index(const dnd_source_artifact *a) {
    return a ? &a->index : NULL;
}

const dnd_source_passage *dnd_source_artifact_passages(const dnd_source_artifact *a, size_t *count) {
    if (count) *count = a ? a->passage_count : 0;
    return a ? a->passages : NULL;
}
const dnd_source_passage_definition *dnd_source_artifact_passage_definitions(
    const dnd_source_artifact *a, size_t *count) {
    if (count) *count = a ? a->passage_count : 0;
    return a ? a->definitions : NULL;
}

enum { PASSAGE_RANK_NONE = DND_RAG_HITS_MAX * 2 };
static size_t passage_rank(uint32_t document, uint32_t first, uint32_t last,
    const dnd_source_anchor *anchors, size_t count) {
    size_t rank = PASSAGE_RANK_NONE;
    for (size_t i = 0; i < count; ++i) {
        if (anchors[i].document != document) continue;
        if (anchors[i].page >= first && anchors[i].page <= last) return i;
        if (rank == PASSAGE_RANK_NONE) rank = DND_RAG_HITS_MAX + i;
    }
    return rank;
}
/* Match a complete catalog name at this word boundary. Catalog names omit
 * spaces; the query keeps them. Prefer the longest source-backed name. */
static size_t spell_prefix(const char *query, const char *name) {
    size_t used = 0, at = 0;
    while (query[at] && name[used]) {
        if (query[at] == ' ') { ++at; continue; }
        if (query[at++] != name[used++]) return 0;
    }
    return !name[used] && (!query[at] || query[at] == ' ') ? at : 0;
}
static const char *cast_catalog_name(const dnd_source_artifact *a, const uint8_t *allowed,
    const dnd_source_anchor *anchors, size_t count, const char *query, size_t *length) {
    const char *name = NULL; *length = 0;
    for (size_t i = 0; i < a->passage_count; ++i) {
        const dnd_source_passage *p = &a->passages[i];
        if (!allowed[p->document] || passage_rank(p->document, p->page_start, p->page_end, anchors, count) == PASSAGE_RANK_NONE)
            continue;
        size_t n = spell_prefix(query, p->name);
        if (n > *length) { *length = n; name = p->name; }
    }
    for (size_t i = 0; i < a->data.entry_count; ++i) {
        const dnd_source_entry *e = &a->entries[i];
        if (!(e->flags & DND_SOURCE_SPELL_HEADER) || !allowed[e->document] ||
            passage_rank(e->document, e->page, e->page, anchors, count) == PASSAGE_RANK_NONE) continue;
        size_t n = spell_prefix(query, e->name);
        if (n > *length) { *length = n; name = e->name; }
    }
    return name;
}
static int cast_list_name(const char *name, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(name, names[i])) return 1;
    return 0;
}
static size_t cast_list_names(const dnd_source_artifact *a, const uint8_t *allowed,
    const dnd_source_anchor *anchors, size_t count, const char *query,
    const char *names[DND_RAG_HITS_MAX + 1u]) {
    size_t found = 0;
    for (size_t start = 0; query[start]; ++start) {
        if (start && query[start - 1u] != ' ') continue;
        const char *at = query + start;
        if (!strncmp(at, "cast ", 5)) at += 5;
        else if (!strncmp(at, "casts ", 6)) at += 6;
        else if (!strncmp(at, "casting ", 8)) at += 8;
        else continue;
        const char *chain[DND_RAG_HITS_MAX + 1u]; size_t chain_count = 0;
        while (chain_count < DND_RAG_HITS_MAX + 1u) {
            if (!strncmp(at, "the ", 4)) at += 4;
            size_t length;
            const char *name = cast_catalog_name(a, allowed, anchors, count, at, &length);
            if (!name) break;
            at += length;
            if (!strncmp(at, " spell", 6) && (!at[6] || at[6] == ' ')) at += 6;
            /* A conjunction must connect catalog names directly. Do not carry
             * casting context through 'and carry a shield' or unrelated prose. */
            chain[chain_count++] = name;
            if (!strncmp(at, " and ", 5)) at += 5;
            else if (!strncmp(at, " or ", 4)) at += 4;
            else break;
        }
        if (chain_count < 2u) continue;
        for (size_t i = 0; i < chain_count; ++i) {
            if (cast_list_name(chain[i], names, found)) continue;
            names[found++] = chain[i];
            if (found == DND_RAG_HITS_MAX + 1u) return found;
        }
    }
    return found;
}
int dnd_source_artifact_select_passages(const dnd_source_artifact *a,
    const dnd_rag_scope *scope, const char *corpus, const char *query, size_t query_len,
    const dnd_source_anchor *anchors, size_t count, dnd_source_passage_selection *out) {
    char normalized[DND_SOURCE_QUERY_CAP];
    uint8_t allowed[DND_SOURCE_DOCUMENTS_MAX];
    dnd_source_passage_selection selected = {0};
    size_t ranks[DND_RAG_HITS_MAX] = {0};
    const char *cast_names[DND_RAG_HITS_MAX + 1u];
    int ambiguous[DND_RAG_HITS_MAX] = {0};
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!a || dnd_source_query_prepare(&a->index, scope, corpus, query, query_len,
        anchors, count, normalized, allowed)) return -1;
    size_t cast_count = cast_list_names(a, allowed, anchors, count, normalized, cast_names);
    for (size_t i = 0; i < a->passage_count; ++i) {
        const dnd_source_passage *p = &a->passages[i];
        if (!allowed[p->document] || (!dnd_source_spell_name_matches(normalized, p->name) &&
            !cast_list_name(p->name, cast_names, cast_count))) continue;
        size_t rank = passage_rank(p->document, p->page_start, p->page_end, anchors, count);
        if (rank == PASSAGE_RANK_NONE) continue;
        size_t at = 0;
        for (; at < selected.count; ++at)
            if (!strcmp(p->name, a->passages[selected.passages[at]].name)) break;
        if (at == selected.count) {
            if (selected.count == DND_RAG_HITS_MAX) { out->reason = DND_SOURCE_CAPACITY; return 0; }
            selected.passages[at] = (uint32_t)i; ranks[at] = rank; ++selected.count;
        } else if (rank < ranks[at]) {
            selected.passages[at] = (uint32_t)i; ranks[at] = rank; ambiguous[at] = 0;
        } else if (rank == ranks[at]) ambiguous[at] = 1;
    }
    if (!selected.count) return 0;
    for (size_t i = 0; i < selected.count; ++i)
        if (ambiguous[i]) { out->reason = DND_SOURCE_AMBIGUOUS; return 0; }
    /* A partial table cannot silently drop another source-backed named spell. */
    for (size_t i = 0; i < a->data.entry_count; ++i) {
        const dnd_source_entry *e = &a->entries[i];
        if (!(e->flags & DND_SOURCE_SPELL_HEADER) || !allowed[e->document] ||
            passage_rank(e->document, e->page, e->page, anchors, count) == PASSAGE_RANK_NONE ||
            (!dnd_source_spell_name_matches(normalized, e->name) &&
            !cast_list_name(e->name, cast_names, cast_count))) continue;
        size_t at = 0;
        for (; at < selected.count; ++at)
            if (!strcmp(e->name, a->passages[selected.passages[at]].name)) break;
        if (at == selected.count) { out->reason = DND_SOURCE_INCOMPLETE; return 0; }
        const dnd_source_passage *p = &a->passages[selected.passages[at]];
        const dnd_source_passage_definition *def = &a->definitions[selected.passages[at]];
        size_t rank = passage_rank(e->document, e->page, e->page, anchors, count);
        if (e->document == p->document && e->page == p->page_start && e->heading.begin == def->heading_begin)
            continue;
        if (rank < ranks[at]) { out->reason = DND_SOURCE_INCOMPLETE; return 0; }
        if (rank == ranks[at]) { out->reason = DND_SOURCE_AMBIGUOUS; return 0; }
    }
    for (size_t i = 0; i < selected.count; ++i) {
        const dnd_source_passage *p = &a->passages[selected.passages[i]];
        for (size_t j = 0; j < p->count; ++j) {
            size_t at = 0;
            for (; at < selected.record_count; ++at) if (selected.records[at] == p->spans[j].record) break;
            if (at == selected.record_count) selected.records[selected.record_count++] = p->spans[j].record;
        }
    }
    selected.reason = DND_SOURCE_SELECTED; *out = selected;
    return 0;
}

int dnd_source_artifact_matches_source(const dnd_source_artifact *a, const dnd_rag_citation *c) {
    if (!a || !c || !bounded(c->document_id, sizeof(c->document_id)) ||
        !bounded(c->book_slug, sizeof(c->book_slug)) || !bounded(c->source_sha256, sizeof(c->source_sha256)) ||
        !bounded(c->collection, sizeof(c->collection)) || !bounded(c->corpus_version, sizeof(c->corpus_version)) ||
        strcmp(c->collection, a->data.collection) || strcmp(c->corpus_version, a->data.corpus_version)) return 0;
    for (size_t i = 0; i < a->reviewed_count; ++i) {
        const dnd_source_document *d = &a->reviewed[i];
        if (!strcmp(c->document_id, d->document_id) && !strcmp(c->book_slug, d->book_slug) &&
            !strcmp(c->source_sha256, d->source_sha256)) return 1;
    }
    return 0;
}

/* Takes ownership of bytes, including on failure. */
static int admit(unsigned char *bytes, size_t length, const dnd_source_artifact_expectation *e,
    dnd_source_artifact **out) {
    unsigned char hash[32], expected_hash[32];
    dnd_source_artifact *a = NULL;
    if (hash_bytes(e->artifact_sha256, expected_hash) || !SHA256(bytes, length, hash) ||
        memcmp(hash, expected_hash, sizeof(hash))) goto fail;
    a = calloc(1, sizeof(*a));
    if (!a) goto fail;
    a->bytes = bytes;
    cursor c = {bytes, length};
    if (header(&c, e, &a->data, &a->passage_count) || tables(c, e, a, 0)) goto fail;
    a->documents = calloc(a->data.document_count, sizeof(*a->documents));
    a->records = calloc(a->data.record_count, sizeof(*a->records));
    a->entries = calloc(a->data.entry_count, sizeof(*a->entries));
    a->reviewed = calloc(e->document_count, sizeof(*a->reviewed));
    if (a->passage_count) {
        a->definitions = calloc(a->passage_count, sizeof(*a->definitions));
        a->passages = calloc(a->passage_count, sizeof(*a->passages));
        if (!a->definitions || !a->passages) goto fail;
    }
    if (!a->documents || !a->records || !a->entries || !a->reviewed || tables(c, e, a, 1)) goto fail;
    memcpy(a->reviewed, e->documents, e->document_count * sizeof(*a->reviewed));
    a->reviewed_count = e->document_count;
    a->data.documents = a->documents; a->data.records = a->records; a->data.entries = a->entries;
    if (dnd_source_index_admit(&a->data, &a->index)) goto fail;
    if (a->passage_count && !dnd_source_passage_definitions_valid(a->definitions,
        a->passage_count, a->data.document_count)) goto fail;
    for (size_t i = 0; i < a->passage_count; ++i) {
        char assembly[65];
        memcpy(assembly, a->passages[i].text_sha256, 65);
        if (dnd_source_rebuild_passage(a->records, a->data.record_count, &a->definitions[i], &a->passages[i]) ||
            strcmp(assembly, a->passages[i].text_sha256)) goto fail;
    }
    *out = a;
    return 0;
fail:
    if (a) dnd_source_artifact_free(a);
    else free(bytes);
    return -1;
}

int dnd_source_artifact_decode(const void *bytes, size_t length,
    const dnd_source_artifact_expectation *e, dnd_source_artifact **out) {
    if (!out || *out || !bytes || !length || length > DND_SOURCE_ARTIFACT_BYTES_MAX || !expectation_valid(e)) return -1;
    unsigned char *copy = malloc(length);
    if (!copy) return -1;
    memcpy(copy, bytes, length);
    return admit(copy, length, e, out);
}

static unsigned char *read_file(const char *path, size_t maximum, size_t *length_out) {
    if (!path || !*path) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    unsigned char *bytes = NULL;
    size_t used = 0, length = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uintmax_t)st.st_size > maximum) goto fail;
    length = (size_t)st.st_size;
    bytes = malloc(length + 1u);
    if (!bytes) goto fail;
    while (used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        used += (size_t)n;
    }
    unsigned char extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0) goto fail;
    (void)close(fd);
    bytes[length] = 0;
    *length_out = length;
    return bytes;
fail:
    (void)close(fd);
    free(bytes);
    return NULL;
}

int dnd_source_artifact_load(const char *path, const dnd_source_artifact_expectation *e, dnd_source_artifact **out) {
    size_t length = 0;
    if (!out || *out || !expectation_valid(e)) return -1;
    unsigned char *bytes = read_file(path, DND_SOURCE_ARTIFACT_BYTES_MAX, &length);
    return bytes ? admit(bytes, length, e, out) : -1;
}

static int manifest_documents(const unsigned char *bytes, size_t length,
    const dnd_source_artifact_expectation *expected, dnd_source_document *documents, size_t *count) {
    cmp_json_object root, source;
    cmp_json_array sources;
    cmp_json_field element;
    char value[128], etag[2048], key[2048], identity[2177];
    unsigned char digest[32], pinned[32];
    static const char hex[] = "0123456789abcdef";
    int64_t enabled_sources;
    size_t total = 0;
    int rc;
    *count = 0;
    if (hash_bytes(expected->manifest_sha256, pinned) || !SHA256(bytes, length, digest) ||
        memcmp(digest, pinned, sizeof(digest)) || memchr(bytes, 0, length) || !utf8_validate_v1(bytes, length) ||
        !cmp_json_object_parse((const char *)bytes, &root) ||
        !cmp_json_object_str(&root, "schema_version", value, sizeof(value)) || strcmp(value, "dnd-corpus-manifest/v1") ||
        !cmp_json_object_str(&root, "corpus_id", value, sizeof(value)) || strcmp(value, expected->corpus_version) ||
        !cmp_json_object_i64(&root, "enabled_sources", &enabled_sources) ||
        enabled_sources < 1 || enabled_sources > DND_SOURCE_DOCUMENTS_MAX ||
        !cmp_json_field_array(cmp_json_object_field(&root, "sources"), &sources)) return -1;
    while ((rc = cmp_json_array_next(&sources, &element)) == 1) {
        int enabled;
        if (++total > DND_SOURCE_DOCUMENTS_MAX || !cmp_json_field_object(&element, &source) ||
            !cmp_json_object_bool(&source, "enabled", &enabled)) return -1;
        if (!enabled) continue;
        dnd_source_document *d = &documents[*count];
        if (!cmp_json_object_str(&source, "book_slug", d->book_slug, sizeof(d->book_slug)) || !d->book_slug[0] ||
            !cmp_json_object_str(&source, "source_sha256", d->source_sha256, sizeof(d->source_sha256)) ||
            hash_bytes(d->source_sha256, digest) ||
            !cmp_json_object_str(&source, "etag", etag, sizeof(etag)) ||
            !cmp_json_object_str(&source, "source_key", key, sizeof(key)) || !key[0]) return -1;
        size_t corpus_len = strlen(expected->corpus_version), key_len = strlen(etag[0] ? etag : key);
        if (corpus_len + 1u + key_len > sizeof(identity)) return -1;
        memcpy(identity, expected->corpus_version, corpus_len); identity[corpus_len] = ':';
        memcpy(identity + corpus_len + 1u, etag[0] ? etag : key, key_len);
        if (!SHA256((const unsigned char *)identity, corpus_len + 1u + key_len, digest)) return -1;
        for (size_t i = 0; i < 12; ++i) {
            d->document_id[i * 2] = hex[digest[i] >> 4]; d->document_id[i * 2 + 1] = hex[digest[i] & 15];
        }
        d->document_id[24] = '\0';
        ++*count;
    }
    return !rc && *count == (size_t)enabled_sources ? 0 : -1;
}

int dnd_source_artifact_load_reviewed(const char *path, const char *manifest_path,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out) {
    if (!out || *out || !metadata_valid(expected) || expected->documents || expected->document_count) return -1;
    dnd_source_document documents[DND_SOURCE_DOCUMENTS_MAX] = {0};
    dnd_source_artifact_expectation e = *expected;
    size_t length = 0;
    unsigned char *bytes = read_file(manifest_path, 256u * 1024u, &length);
    if (!bytes) return -1;
    int rc = manifest_documents(bytes, length, expected, documents, &e.document_count);
    free(bytes);
    if (rc) return -1;
    e.documents = documents;
    return dnd_source_artifact_load(path, &e, out);
}

int dnd_source_artifact_decode_reviewed(const void *bytes, size_t length,
    const void *manifest, size_t manifest_length,
    const dnd_source_artifact_expectation *expected, dnd_source_artifact **out) {
    if (!out || *out || !metadata_valid(expected) || expected->documents || expected->document_count ||
        !bytes || !length || length > DND_SOURCE_ARTIFACT_BYTES_MAX ||
        !manifest || !manifest_length || manifest_length > 256u * 1024u) return -1;
    dnd_source_document documents[DND_SOURCE_DOCUMENTS_MAX] = {0};
    dnd_source_artifact_expectation e = *expected;
    unsigned char *copy = malloc(manifest_length + 1u);
    if (!copy) return -1;
    memcpy(copy, manifest, manifest_length); copy[manifest_length] = 0;
    int rc = manifest_documents(copy, manifest_length, expected, documents, &e.document_count);
    free(copy);
    if (rc) return -1;
    e.documents = documents;
    return dnd_source_artifact_decode(bytes, length, &e, out);
}
