#define _POSIX_C_SOURCE 200809L
#include "dnd_tools.h"
#include "cmp_json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); ++failures; }
}

static void *cancel_later(void *opaque) {
    struct timespec delay = {0, 100000000};
    (void)nanosleep(&delay, NULL);
    atomic_store_explicit((atomic_int *)opaque, 1, memory_order_relaxed);
    return NULL;
}

static int run_client(int argc, char **argv) {
    dnd_tool_result result;
    atomic_int cancel = 0;
    pthread_t thread;
    int threaded = 0, rc;
    int64_t deadline;
    dnd_initiative_request_c initiative;
    if (argc < 9 || argc > 12) return 2;
    if (argc == 12) {
        uint8_t wire[DND_INITIATIVE_REQUEST_MAX + 1u];
        FILE *file = fopen(argv[11], "rb");
        if (!file) return 2;
        size_t length = fread(wire, 1u, sizeof(wire), file);
        int failed = ferror(file);
        fclose(file);
        if (failed || !dnd_initiative_request_decode(wire, length, &initiative)) return 2;
    }
    deadline = strtoll(argv[7], NULL, 10);
    if (strcmp(argv[8], "before") == 0) atomic_store(&cancel, 1);
    if (strcmp(argv[8], "during") == 0) {
        if (pthread_create(&thread, NULL, cancel_later, &cancel) != 0) return 2;
        threaded = 1;
    }
    if (argc == 12) rc = dnd_initiative_execute(argv[2], getenv("DND_TOOLS_TEST_SECRET"), argv[3], argv[4],
        argv[5], argv[6], argv[9], argv[10], &initiative, deadline, 3000, &cancel, &result);
    else if (argc == 11) rc = dnd_encounter_execute(argv[2], getenv("DND_TOOLS_TEST_SECRET"), argv[3], argv[4],
        argv[5], argv[6], argv[9], argv[10], deadline, 3000, &cancel, &result);
    else if (argc == 10) rc = dnd_roster_execute(argv[2], getenv("DND_TOOLS_TEST_SECRET"), argv[3], argv[4],
        argv[5], argv[6], argv[9], deadline, 3000, &cancel, &result);
    else rc = dnd_tools_execute(argv[2], getenv("DND_TOOLS_TEST_SECRET"), argv[3], argv[4],
                           argv[5], argv[6], deadline, 3000, &cancel, &result);
    if (threaded) pthread_join(thread, NULL);
    if (rc != DND_TOOL_OK) {
        printf("{\"rc\":%d}\n", rc);
        return 1;
    }
    if (!strcmp(argv[1], "--encounter-wire"))
        return result.encounter_length && fwrite(result.encounter_wire, 1u, result.encounter_length, stdout) == result.encounter_length ? 0 : 2;
    if (!strcmp(argv[1], "--roster-wire"))
        return result.roster_length && fwrite(result.roster_wire, 1u, result.roster_length, stdout) == result.roster_length ? 0 : 2;
    if (!strcmp(argv[1], "--initiative-wire"))
        return result.initiative_length && fwrite(result.initiative_wire, 1u, result.initiative_length, stdout) == result.initiative_length ? 0 : 2;
    if (!strcmp(argv[1], "--turn-wire")) {
        uint8_t wire[DND_TOOL_EVENT_MAX];
        turn_tool_result_c tool = {.present = 1, .elapsed_ms = result.elapsed_ms};
        memcpy(tool.tool_id, result.tool_id, sizeof(result.tool_id));
        memcpy(tool.tool_call_id, result.call_id, sizeof(result.call_id));
        memcpy(tool.output_sha256, result.output_sha256, sizeof(result.output_sha256));
        size_t length = pb_encode_turn_text_event(wire, sizeof(wire), argv[3], "text_completed",
            result.text, result.text, result.text, 0, 1);
        length = pb_append_turn_tool_result(wire, sizeof(wire), length, &tool);
        if (result.encounter_length) {
            turn_encounter_c value = {result.encounter_wire, result.encounter_length};
            length = pb_append_turn_encounter(wire, sizeof(wire), length, &value);
        }
        if (result.roster_length) {
            turn_roster_c value = {result.roster_wire, result.roster_length};
            length = pb_append_turn_roster(wire, sizeof(wire), length, &value);
        }
        if (result.initiative_length) {
            turn_initiative_c value = {result.initiative_wire, result.initiative_length};
            length = pb_append_turn_initiative(wire, sizeof(wire), length, &value);
        }
        return length && fwrite(wire, 1u, length, stdout) == length ? 0 : 2;
    }
    char escaped[6u * sizeof(result.text)];
    if (cmp_json_escape_exact(result.text, escaped, sizeof(escaped))) return 2;
    printf("{\"rc\":0,\"call_id\":\"%s\",\"output_sha256\":\"%s\",\"text\":\"%s\"}\n",
             result.call_id, result.output_sha256, escaped);
    return 0;
}

int main(int argc, char **argv) {
    static const struct { const char *prompt; int admitted; const char *expression; } cases[] = {
        {"Roll 2d20 with advantage and add 5 for my Perception check. Tell me the actual dice and total.", 1, "2d20kh1+5"},
        {"Please roll d20 with disadvantage minus 2.", 1, "2d20kl1-2"},
        {"Can you roll 4d6+3?", 1, "4d6+3"},
        {"Roll 100d1000-10000.", 1, "100d1000-10000"},
        {"Roll 2d20kl1+5", 1, "2d20kl1+5"},
        {"How does advantage work?", 0, ""},
        {"Roll initiative for the party.", -1, ""},
        {"Roll 1d0", -1, ""},
        {"Roll 3d20 with advantage", -1, ""},
        {"Roll 2d20 with advantage with disadvantage", -1, ""},
        {"Roll 2d20kh1 with disadvantage", -1, ""},
        {"Roll 2d20+2 and add 5", -1, ""},
        {"Roll 2d20 and add 500000", -1, ""},
        {"Roll 2d20 or 2d6", -1, ""},
    };
    static const char output[] =
        "{\"expression\":\"2d20kh1+5\",\"rolls\":[10,18],\"kept_indexes\":[1],"
        "\"kept_rolls\":[18],\"modifier\":5,\"total\":23,\"mode\":\"advantage\","
        "\"entropy_source\":\"getrandom\",\"roll_id\":\"dice-test\",\"visibility\":\"public\"}";
    static const struct { const char *before; const char *after; } corruptions[] = {
        {"\"total\":23", "\"total\":24"}, {"[10,18]", "[10,21]"},
        {"[10,18]", "[10,-1]"}, {"[10,18]", "[10,1e1]"},
        {"\"kept_indexes\":[1]", "\"kept_indexes\":[0]"},
        {"\"kept_indexes\":[1]", "\"kept_indexes\":[1000]"},
        {"\"kept_indexes\":[1]", "\"kept_indexes\":[-1]"},
        {"\"kept_indexes\":[1]", "\"kept_indexes\":[1,1]"},
        {"\"kept_rolls\":[18]", "\"kept_rolls\":[10]"},
        {"\"modifier\":5", "\"modifier\":6"},
        {"\"mode\":\"advantage\"", "\"mode\":\"disadvantage\""},
        {"\"entropy_source\":\"getrandom\"", "\"entropy_source\":\"model\""},
        {"\"roll_id\":\"dice-test\"", "\"roll_id\":\"another-call\""},
        {"\"total\":23", "\"total\":23,\"total\":23"},
    };
    size_t i;
    char expression[64], text[2048], mutated[1024];
    if (argc > 1) return run_client(argc, argv);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int rc = dnd_dice_intent(cases[i].prompt, expression);
        expect(rc == cases[i].admitted, cases[i].prompt);
        if (rc == 1) expect(strcmp(expression, cases[i].expression) == 0, "canonical expression");
    }
    expect(dnd_dice_output_text(output, "2d20kh1+5", "dice-test", text, sizeof(text)) == 0 &&
        strcmp(text, "You rolled 2d20kh1+5 with advantage: 10, 18, kept 18, with modifier +5, for a total of 23.") == 0,
        "speech contains only the audited dice and total");
    expect(dnd_dice_output_text(output, "2d20kh1+5", "dice-test", text, 20u) != 0, "speech capacity fails closed");
    for (i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        const char *start = strstr(output, corruptions[i].before);
        size_t prefix = (size_t)(start - output);
        memcpy(mutated, output, prefix);
        snprintf(mutated + prefix, sizeof(mutated) - prefix, "%s%s", corruptions[i].after,
                   start + strlen(corruptions[i].before));
        expect(dnd_dice_output_text(mutated, "2d20kh1+5", "dice-test", text, sizeof(text)) != 0,
               corruptions[i].after);
    }
    if (!failures) puts("ALL PASS authenticated D&D dice client contracts");
    return failures ? 1 : 0;
}
