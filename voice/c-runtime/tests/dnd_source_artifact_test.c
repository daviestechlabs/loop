#define _POSIX_C_SOURCE 200809L
#include "dnd_source_artifact_fixture.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static source_fixture f;
static artifact_fixture a;
static size_t cases;

static void reset(void) { fixture_make(&f); artifact_make(&a, &f); }
static void reject(void) {
    dnd_source_artifact *loaded = NULL;
    assert(dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded) == -1);
    assert(!loaded); ++cases;
}
static dnd_source_artifact *decode(void) {
    dnd_source_artifact *loaded = NULL;
    assert(!dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded));
    assert(loaded); ++cases;
    return loaded;
}
static void change_number(size_t offset, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) a.bytes[offset + i] = (unsigned char)(n >> (i * 8));
    artifact_rehash(&a);
}
static void selected(const dnd_source_artifact *loaded) {
    const dnd_source_index *index = dnd_source_artifact_index(loaded);
    const char *q = "How often can I use Warding Step in a round?";
    dnd_source_selection s;
    assert(!dnd_source_index_select(index, &f.scope, f.data.corpus_version, q, strlen(q), f.anchors, f.anchor_count, &s));
    assert(s.reason == DND_SOURCE_SELECTED && s.count == 3 && !s.replace_baseline);
    assert(s.records[0] == 0 && s.records[1] == 1 && s.records[2] == 2);
    assert(!strcmp(index->data->records[1].record_id, f.records[1].record_id));
    assert(index->data->records[1].content_len == f.records[1].content_len);
    assert(!memcmp(index->data->records[1].content, f.records[1].content, f.records[1].content_len));
}

int main(void) {
    reset();
    dnd_source_artifact *loaded = decode();
    selected(loaded);
    memset(a.bytes, 0, a.length);
    selected(loaded); /* Source buffer lifetime is independent. */
    assert(dnd_source_artifact_decode(a.bytes, a.length, &a.expected, &loaded) == -1);
    selected(loaded); /* Failed replacement preserves an existing view. */
    dnd_source_artifact_free(loaded); ++cases;
    assert(!dnd_source_artifact_index(NULL)); dnd_source_artifact_free(NULL);
    reset(); loaded = NULL;
    assert(dnd_source_artifact_decode(NULL, a.length, &a.expected, &loaded) == -1);
    assert(dnd_source_artifact_decode(a.bytes, a.length, NULL, &loaded) == -1);
    assert(dnd_source_artifact_decode(a.bytes, a.length, &a.expected, NULL) == -1);
    assert(dnd_source_artifact_decode(a.bytes, DND_SOURCE_ARTIFACT_BYTES_MAX + 1u, &a.expected, &loaded) == -1);
    assert(!loaded); ++cases;

    reset(); a.bytes[0] ^= 1; reject(); /* Unchanged operator digest. */
    reset(); a.bytes[0] ^= 1; artifact_rehash(&a); reject(); /* Re-signed wrong magic. */
    reset(); a.bytes[7] = '2'; artifact_rehash(&a); reject();
    reset(); a.bytes[8] ^= 1; artifact_rehash(&a); reject();
    reset(); a.bytes[40] ^= 1; artifact_rehash(&a); reject();
    reset(); a.expected.collection[0] = 'x'; reject();
    reset(); a.expected.corpus_version[10] = 'f'; reject();
    reset(); a.expected.ruleset[0] = 'x'; reject();
    reset(); a.expected.manifest_sha256[0] = 'C'; reject();
    reset(); a.expected.compiler_sha256[64] = 'x'; reject();
    reset(); a.expected.artifact_sha256[3] = 0; reject();
    reset(); memset(a.expected.collection, 'x', sizeof(a.expected.collection)); reject();
    reset(); a.expected.documents = NULL; reject();
    reset(); a.expected.document_count = 0; reject();
    reset(); a.expected.document_count = DND_SOURCE_DOCUMENTS_MAX + 1u; reject();
    reset(); f.documents[1] = f.documents[0]; reject(); /* Invalid reviewed allowlist. */
    reset(); f.documents[0].source_sha256[0] = 'e'; reject();
    reset(); strcpy(f.documents[0].book_slug, "monster-manual"); reject();
    reset(); strcpy(f.documents[0].document_id, "unreviewed"); reject();
    reset(); f.documents[2] = f.documents[1]; strcpy(f.documents[2].document_id, "extra-reviewed");
    a.expected.document_count = 3; loaded = decode(); selected(loaded); dnd_source_artifact_free(loaded);
    reset(); dnd_source_document swap = f.documents[0]; f.documents[0] = f.documents[1]; f.documents[1] = swap;
    loaded = decode(); selected(loaded); dnd_source_artifact_free(loaded); /* Allowlist order is immaterial. */

    const uint32_t bad_counts[] = {0, UINT32_MAX};
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 2; ++j) {
        reset(); change_number(a.counts + i * 4, bad_counts[j]); reject();
    }
    reset(); change_number(a.counts + 4, DND_SOURCE_RECORDS_MAX); reject(); /* No giant table from a short file. */
    reset(); change_number(72, UINT32_MAX); reject();
    reset(); a.bytes[76] = 0; artifact_rehash(&a); reject();
    reset(); change_number(a.docs[0], 128); reject();
    reset(); a.bytes[a.docs[0] + 4] = 0; artifact_rehash(&a); reject();
    reset(); change_number(a.records[0], 65); reject();
    reset(); change_number(a.records[0] + 136, 99); reject(); /* Unknown document. */
    reset(); change_number(a.records[0] + 140, 0); reject(); /* Page zero. */
    reset(); change_number(a.records[0] + 152, DND_RAG_CONTENT_CAP); reject();
    reset(); a.bytes[a.records[0] + 156] ^= 1; artifact_rehash(&a); reject(); /* Inner content digest. */
    reset(); f.contents[0][2] = '\0'; fixture_hash(f.contents[0], f.records[0].content_len, f.records[0].content_hash);
    artifact_make(&a, &f); reject(); /* Hash-correct binary text. */
    reset(); f.contents[0][2] = (char)0xff; fixture_hash(f.contents[0], f.records[0].content_len, f.records[0].content_hash);
    artifact_make(&a, &f); reject();
    reset(); f.records[1] = f.records[0]; artifact_make(&a, &f); reject();
    reset(); f.entries[0].heading.spans[0].record = UINT32_MAX; artifact_make(&a, &f); reject();
    reset(); f.entries[0].body.spans[1].begin++; artifact_make(&a, &f); reject();
    reset(); f.entries[0].body.count = 3; artifact_make(&a, &f); reject();
    reset(); f.entries[2].fields[0].spans[1].end = 1; artifact_make(&a, &f); reject();
    reset(); strcpy(f.entries[0].name, "inventedrule"); artifact_make(&a, &f); reject();
    reset(); artifact_bytes(&a, "\0", 1); artifact_rehash(&a); reject();

    reset(); size_t complete_length = a.length;
    for (size_t n = 0; n < complete_length; ++n) {
        a.length = n; artifact_rehash(&a); reject();
    }
    a.length = complete_length; artifact_rehash(&a);
    loaded = decode(); selected(loaded); dnd_source_artifact_free(loaded);

    char directory[] = "/tmp/dnd-source-artifact.XXXXXX", path[128], link[128];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof(path), "%s/index", directory) > 0);
    assert(snprintf(link, sizeof(link), "%s/link", directory) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(write(fd, a.bytes, a.length) == (ssize_t)a.length); assert(!close(fd));
    loaded = NULL;
    assert(!dnd_source_artifact_load(path, &a.expected, &loaded)); selected(loaded); ++cases;
    dnd_source_artifact *rejected = NULL;
    assert(!symlink(path, link));
    assert(dnd_source_artifact_load(link, &a.expected, &rejected) == -1); ++cases;
    assert(!unlink(link));
    a.expected.artifact_sha256[0] = a.expected.artifact_sha256[0] == 'a' ? 'b' : 'a';
    assert(dnd_source_artifact_load(path, &a.expected, &rejected) == -1); ++cases;
    artifact_rehash(&a);
    assert(!unlink(path)); selected(loaded); dnd_source_artifact_free(loaded); loaded = NULL;
    assert(dnd_source_artifact_load(path, &a.expected, &loaded) == -1); ++cases;
    assert(!symlink(directory, link));
    assert(dnd_source_artifact_load(link, &a.expected, &loaded) == -1); ++cases;
    assert(dnd_source_artifact_load(directory, &a.expected, &loaded) == -1); ++cases;
    assert(!unlink(link)); assert(!mkfifo(path, 0600));
    assert(dnd_source_artifact_load(path, &a.expected, &loaded) == -1); ++cases;
    assert(!unlink(path)); fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(dnd_source_artifact_load(path, &a.expected, &loaded) == -1); ++cases;
    assert(!ftruncate(fd, DND_SOURCE_ARTIFACT_BYTES_MAX + 1));
    assert(dnd_source_artifact_load(path, &a.expected, &loaded) == -1); ++cases;
    assert(!ftruncate(fd, 1)); assert(!close(fd));
    assert(dnd_source_artifact_load(path, &a.expected, &loaded) == -1); ++cases;
    assert(!loaded); assert(!unlink(path)); assert(!rmdir(directory));
    printf("dnd_source_artifact: %zu checks passed (%zu truncation lengths)\n", cases, complete_length);
    return 0;
}
