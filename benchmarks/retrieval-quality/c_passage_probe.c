/* Offline passage diagnostic. Only hashes, source references and ranges leave
 * this executable. No retrieval publication, backend call, or generated text. */
#include "dnd_source_artifact.h"
#include "dnd_source_passage.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put(char *out, size_t capacity, const char *value) {
    size_t n = strlen(value); if (!n || n >= capacity) return -1;
    memcpy(out, value, n + 1u); return 0;
}
static int number(const char *text, uint32_t *out) {
    if (!text[0]) return -1;
    for (size_t i = 0; text[i]; ++i) if (text[i] < '0' || text[i] > '9') return -1;
    char *end = NULL; errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value > INT32_MAX) return -1;
    *out = (uint32_t)value; return 0;
}
int main(int argc, char **argv) {
    dnd_source_artifact_expectation expected = {0};
    dnd_source_artifact *artifact = NULL;
    dnd_source_passage_definition in = {0};
    dnd_source_passage out;
    uint32_t first, last;
    int result = 1;
    if (argc != 16 || put(expected.artifact_sha256, sizeof(expected.artifact_sha256), argv[2]) ||
        put(expected.compiler_sha256, sizeof(expected.compiler_sha256), argv[3]) ||
        put(expected.manifest_sha256, sizeof(expected.manifest_sha256), argv[5]) ||
        put(expected.collection, sizeof(expected.collection), argv[6]) ||
        put(expected.corpus_version, sizeof(expected.corpus_version), argv[7]) ||
        put(expected.ruleset, sizeof(expected.ruleset), argv[8]) ||
        number(argv[10], &first) || number(argv[11], &last) || !first || first > last ||
        last - first >= DND_SOURCE_PASSAGE_PAGES_MAX ||
        number(argv[12], &in.heading_begin) || number(argv[13], &in.heading_end) ||
        number(argv[14], &in.next_heading_begin) || number(argv[15], &in.next_heading_end) ||
        dnd_source_artifact_load_reviewed(argv[1], argv[4], &expected, &artifact)) goto done;
    const dnd_source_data *data = dnd_source_artifact_index(artifact)->data;
    size_t document = 0;
    for (; document < data->document_count; ++document)
        if (!strcmp(data->documents[document].document_id, argv[9])) break;
    if (document == data->document_count) goto done;
    in.document = (uint32_t)document; in.first_page = first; in.last_page = last;
    int status = dnd_source_rebuild_passage(data->records, data->record_count, &in, &out);
    if (status != DND_SOURCE_PAGE_OK) { fprintf(stderr, "passage_status=%d\n", status); goto done; }
    printf("{\"name\":\"%s\",\"page_start\":%u,\"page_end\":%u,\"bytes\":%zu,\"sha256\":\"%s\",\"spans\":[",
        out.name, out.page_start, out.page_end, out.length, out.text_sha256);
    for (size_t i = 0; i < out.count; ++i) {
        const dnd_source_passage_span *s = &out.spans[i]; const dnd_source_record *r = &data->records[s->record];
        printf("%s{\"record_id\":\"%s\",\"content_hash\":\"%s\",\"page\":%u,\"chunk\":%u,\"begin\":%u,\"end\":%u}",
            i ? "," : "", r->record_id, r->content_hash, r->page, r->chunk, s->begin, s->end);
    }
    if (printf("]}\n") < 0 || fflush(stdout)) goto done;
    result = 0;
done:
    dnd_source_artifact_free(artifact);
    return result;
}
