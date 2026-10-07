#define _POSIX_C_SOURCE 200809L
#include "iaevents.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

int main(void) {
    char subj[128];
    char err[192];
    char json[IA_JSON];
    char types[IA_MAX_TYPES][IA_STR];
    int n = 0;
    ia_schema s;

    expect(strcmp(ia_schema_version(), IA_VERSION) == 0, "version");
    expect(ia_type_known("turn") == 1, "turn known");
    expect(ia_type_known("session_start") == 1, "session_start");
    expect(ia_type_known("nope") == 0, "unknown");
    expect(ia_subject_for("tool_call", subj, sizeof(subj)) == IA_OK, "subject ok");
    expect(strcmp(subj, "analytics.events.interaction.tool_call") == 0, "subject val");
    expect(ia_subject_for("nope", subj, sizeof(subj)) == IA_ERR, "subject bad");

    expect(ia_validate_envelope("e1", "tool_call", IA_VERSION, 1234, NULL, err, sizeof(err)) == IA_OK,
           "validate ok");
    expect(ia_validate_envelope("", "tool_call", IA_VERSION, 1234, NULL, err, sizeof(err)) == IA_ERR,
           "need id");
    expect(ia_validate_envelope("e1", "tool_call", "old", 1234, NULL, err, sizeof(err)) == IA_ERR,
           "old version");
    expect(ia_validate_envelope("e1", "tool_call", IA_VERSION, 0, NULL, err, sizeof(err)) == IA_ERR,
           "need time");
    expect(ia_validate_envelope("e1", "bogus", IA_VERSION, 1, NULL, err, sizeof(err)) == IA_ERR,
           "bad type");

    expect(ia_ordered_types(types, &n, IA_MAX_TYPES) == IA_OK && n == 14, "14 types");
    expect(strcmp(types[0], "turn") == 0 && strcmp(types[13], "mode_switch") == 0, "order");

    expect(ia_schema_for("turn", &s) == IA_OK, "schema turn");
    expect(strcmp(s.ch_table, "turn_events") == 0, "ch table");
    expect(strcmp(s.iceberg_table, "interaction_analytics.turn_events") == 0, "iceberg");
    expect(s.n_partitions >= 1 && strcmp(s.partitions[0], "event_date") == 0, "part0");
    expect(s.n_required >= 4, "required");

    expect(ia_encode_types_json(json, sizeof(json)) == IA_OK, "types json");
    expect(strstr(json, "\"tool_call\"") != NULL, "types has tool_call");
    expect(ia_encode_schemas_json(json, sizeof(json)) == IA_OK, "schemas json");
    expect(strstr(json, "\"clickhouse_table\":\"route_events\"") != NULL, "route table");

    {
        char wire[4096];
        char eid[40];
        expect(ia_new_event_id(eid, sizeof(eid)) == IA_OK && strlen(eid) == 32, "event id");
        expect(ia_build_product_wire("page_view", NULL, "sess1", "", "anon-1", "daviestechlabs-homelab",
                                     1700000000000LL, "companions", "landing", "", "", "", "/", "", "", 0, 0,
                                     "{\"surface\":\"companions-frontend\"}", wire, sizeof(wire), err,
                                     sizeof(err)) == IA_OK,
               "build page_view");
        expect(strstr(wire, "\"type\":\"page_view\"") != NULL, "wire type");
        expect(strstr(wire, "\"path\":\"/\"") != NULL, "wire path");
        expect(ia_build_product_wire("ui_action", NULL, "s", "", "u", "repo", 1, "", "chat", "", "", "x",
                                     "", "", "", 0, 0, "{}", wire, sizeof(wire), err, sizeof(err)) == IA_ERR,
               "ui needs action");
        expect(ia_build_product_wire("mode_switch", NULL, "s", "", "u", "repo", 1, "p", "", "", "", "", "",
                                     "chat", "voice", 0, 0, "{}", wire, sizeof(wire), err, sizeof(err)) ==
                   IA_OK,
               "mode switch");
        expect(ia_build_product_wire("page_view", "quoted\"id\n", "s", "", "u", "repo", 1,
                                     "p", "m", "", "", "", "/", "", "", 0, 0, "{}", wire, sizeof(wire),
                                     err, sizeof(err)) == IA_OK, "escape event id");
        expect(strstr(wire, "quoted\\\"id\\u000a") != NULL, "event id remains JSON data");
        {
            char huge[4096];
            memset(huge, 'x', sizeof(huge) - 1u);
            huge[sizeof(huge) - 1u] = '\0';
            expect(ia_build_product_wire("page_view", "id", huge, "", "u", "repo", 1,
                                         "p", "m", "", "", "", "/", "", "", 0, 0, "{}", wire, sizeof(wire),
                                         err, sizeof(err)) == IA_ERR && !wire[0], "reject field truncation");
            expect(ia_build_product_wire("page_view", "id", "s", "", "u", "repo", 1,
                                         "p", "m", "", "", "", "/", "", "", 0, 0, huge, wire, sizeof(wire),
                                         err, sizeof(err)) == IA_ERR && !wire[0], "reject payload truncation");
        }
    }

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-iaevents unit\n");
    return 0;
}
