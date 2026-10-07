/* Offline local-retrieval experiment. Does not grant corpus or training rights. */
#include "dnd_source_artifact.h"
#include "cmp_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put(char *out, size_t capacity, const char *text) {
    size_t n = strlen(text);
    if (!n || n >= capacity) return -1;
    memcpy(out, text, n + 1u);
    return 0;
}

static int selected(const dnd_source_entry *entry, const dnd_source_data *data) {
    if (strcmp(data->documents[entry->document].book_slug, "players-handbook")) return 0;
    return !strcmp(entry->name, "shield") || !strcmp(entry->name, "mistystep") ||
        !strcmp(entry->name, "sanctuary") || !strcmp(entry->name, "shieldoffaith");
}

int main(int argc, char **argv) {
    dnd_source_artifact_expectation expected = {0};
    dnd_source_artifact *artifact = NULL;
    static unsigned char selected_records[DND_SOURCE_RECORDS_MAX];
    if ((argc != 9 && !(argc == 10 && !strcmp(argv[9], "--all-phb"))) || put(expected.artifact_sha256, sizeof(expected.artifact_sha256), argv[3]) ||
        put(expected.manifest_sha256, sizeof(expected.manifest_sha256), argv[4]) ||
        put(expected.compiler_sha256, sizeof(expected.compiler_sha256), argv[5]) ||
        put(expected.collection, sizeof(expected.collection), argv[6]) ||
        put(expected.corpus_version, sizeof(expected.corpus_version), argv[7]) ||
        put(expected.ruleset, sizeof(expected.ruleset), argv[8]) ||
        dnd_source_artifact_load_reviewed(argv[1], argv[2], &expected, &artifact)) return 1;
    const dnd_source_data *data = dnd_source_artifact_index(artifact)->data;
    printf("{\"entries\":[");
    size_t count = 0;
    for (size_t i = 0; i < data->entry_count; ++i) {
        const dnd_source_entry *entry = &data->entries[i];
        if (!selected(entry, data)) continue;
        printf("%s{\"name\":\"%s\",\"document_id\":\"%s\",\"page\":%u,\"record_ids\":[",
            count++ ? "," : "", entry->name, data->documents[entry->document].document_id, entry->page);
        for (size_t j = 0; j < entry->body.count; ++j) {
            uint32_t r = entry->body.spans[j].record;
            selected_records[r] = 1;
            printf("%s\"%s\"", j ? "," : "", data->records[r].record_id);
        }
        printf("]}");
    }
    printf("],\"records\":[");
    size_t records = 0;
    for (size_t i = 0; i < data->record_count; ++i) {
        const dnd_source_record *r = &data->records[i];
        const dnd_source_document *d = &data->documents[r->document];
        if (!selected_records[i] && !(argc == 10 && !strcmp(d->book_slug, "players-handbook"))) continue;
        size_t cap = r->content_len * 6u + 1u;
        char *escaped = malloc(cap);
        char *terminated = malloc(r->content_len + 1u);
        if (!escaped || !terminated) {
            free(escaped); free(terminated); dnd_source_artifact_free(artifact); return 1;
        }
        memcpy(terminated, r->content, r->content_len);
        terminated[r->content_len] = '\0';
        if (cmp_json_escape_exact(terminated, escaped, cap)) {
            free(escaped); free(terminated); dnd_source_artifact_free(artifact); return 1;
        }
        printf("%s{\"record_id\":\"%s\",\"document_id\":\"%s\",\"book_slug\":\"%s\","
            "\"content_hash\":\"%s\",\"source_sha256\":\"%s\",\"page\":%u,\"chunk\":%u,\"content\":\"%s\"}",
            records++ ? "," : "", r->record_id, d->document_id, d->book_slug, r->content_hash,
            d->source_sha256, r->page, r->chunk, escaped);
        free(escaped);
        free(terminated);
    }
    printf("]}\n");
    dnd_source_artifact_free(artifact);
    return ferror(stdout) || (argc == 9 && count == 0) || records == 0 ? 1 : 0;
}
