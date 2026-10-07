/*
 * c-iaevents — pure-C interaction-analytics schema CLI.
 *   c-iaevents version
 *   c-iaevents subject <type>
 *   c-iaevents known <type>          → 1|0
 *   c-iaevents types                 → JSON array
 *   c-iaevents schemas               → JSON array of schema rows
 *   c-iaevents schema <type>         → one schema JSON
 *   c-iaevents validate-envelope <event_id> <type> <schema_version> <unix_ms>
 * Exit 0 ok, 1 error (message on stderr).
 */
#define _POSIX_C_SOURCE 200809L
#include "iaevents.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char out[IA_JSON];
    char err[IA_MSG];
    char subj[256];
    ia_schema s;
    long long ms;
    int i, w;

    if (argc < 2) {
        fprintf(stderr, "usage: c-iaevents version|subject|known|types|schemas|schema|validate-envelope\n");
        return 2;
    }
    if (strcmp(argv[1], "version") == 0) {
        puts(ia_schema_version());
        return 0;
    }
    if (strcmp(argv[1], "subject") == 0) {
        if (argc < 3)
            return 2;
        if (ia_subject_for(argv[2], subj, sizeof(subj)) != IA_OK) {
            fprintf(stderr, "unknown event type\n");
            return 1;
        }
        puts(subj);
        return 0;
    }
    if (strcmp(argv[1], "known") == 0) {
        if (argc < 3)
            return 2;
        puts(ia_type_known(argv[2]) ? "1" : "0");
        return ia_type_known(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "types") == 0) {
        if (ia_encode_types_json(out, sizeof(out)) != IA_OK)
            return 1;
        puts(out);
        return 0;
    }
    if (strcmp(argv[1], "schemas") == 0) {
        if (ia_encode_schemas_json(out, sizeof(out)) != IA_OK)
            return 1;
        puts(out);
        return 0;
    }
    if (strcmp(argv[1], "schema") == 0) {
        if (argc < 3)
            return 2;
        if (ia_schema_for(argv[2], &s) != IA_OK) {
            fprintf(stderr, "unknown event type\n");
            return 1;
        }
        /* compact one-object JSON */
        w = snprintf(out, sizeof(out),
                     "{\"type\":\"%s\",\"version\":\"%s\",\"clickhouse_table\":\"%s\","
                     "\"iceberg_table\":\"%s\",\"partition_columns\":[",
                     s.type, IA_VERSION, s.ch_table, s.iceberg_table);
        if (w < 0 || (size_t)w >= sizeof(out))
            return 1;
        for (i = 0; i < s.n_partitions; i++) {
            w += snprintf(out + w, sizeof(out) - (size_t)w, "%s\"%s\"", i ? "," : "", s.partitions[i]);
        }
        w += snprintf(out + w, sizeof(out) - (size_t)w, "],\"primary_key\":[");
        for (i = 0; i < s.n_primary_key; i++) {
            w += snprintf(out + w, sizeof(out) - (size_t)w, "%s\"%s\"", i ? "," : "", s.primary_key[i]);
        }
        w += snprintf(out + w, sizeof(out) - (size_t)w, "],\"required_fields\":[");
        for (i = 0; i < s.n_required; i++) {
            w += snprintf(out + w, sizeof(out) - (size_t)w, "%s\"%s\"", i ? "," : "", s.required[i]);
        }
        snprintf(out + w, sizeof(out) - (size_t)w, "]}");
        puts(out);
        return 0;
    }
    if (strcmp(argv[1], "validate-envelope") == 0) {
        if (argc < 6)
            return 2;
        ms = atoll(argv[5]);
        if (ia_validate_envelope(argv[2], argv[3], argv[4], ms, NULL, err, sizeof(err)) != IA_OK) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        return 0;
    }
    /*
     * build-product <type> <session_id> <user_id> <source_repo> <unix_ms>
     *   [profile] [mode] [campaign] [action] [target] [path] [from_mode] [to_mode]
     *   [duration_ms] [turn_count] [labels_json]
     */
    if (strcmp(argv[1], "build-product") == 0) {
        const char *type, *session, *user, *repo;
        long long ums = 0, dur = 0, turns = 0;
        const char *profile = "", *mode = "", *campaign = "", *action = "", *target = "";
        const char *path = "", *from_mode = "", *to_mode = "", *labels = "{}";
        if (argc < 7)
            return 2;
        type = argv[2];
        session = argv[3];
        user = argv[4];
        repo = argv[5];
        ums = atoll(argv[6]);
        if (argc > 7)
            profile = argv[7];
        if (argc > 8)
            mode = argv[8];
        if (argc > 9)
            campaign = argv[9];
        if (argc > 10)
            action = argv[10];
        if (argc > 11)
            target = argv[11];
        if (argc > 12)
            path = argv[12];
        if (argc > 13)
            from_mode = argv[13];
        if (argc > 14)
            to_mode = argv[14];
        if (argc > 15)
            dur = atoll(argv[15]);
        if (argc > 16)
            turns = atoll(argv[16]);
        if (argc > 17)
            labels = argv[17];
        if (ia_build_product_wire(type, NULL, session, "", user, repo, ums, profile, mode, campaign,
                                  action, target, path, from_mode, to_mode, dur, turns, labels, out,
                                  sizeof(out), err, sizeof(err)) != IA_OK) {
            fprintf(stderr, "%s\n", err[0] ? err : "build failed");
            return 1;
        }
        puts(out);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
