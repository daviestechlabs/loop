/* Offline evaluation of the production scene-question parser. No model or game effects. */
#define _POSIX_C_SOURCE 200809L
#include "dnd_tools.h"
#include "cmp_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_CASES 128
static char *read_json(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = calloc(1, 1048577);
    if (!text) { fclose(file); return NULL; }
    size_t n = fread(text, 1, 1048576, file);
    int invalid = ferror(file) || !n || n == 1048576 || memchr(text, 0, n);
    fclose(file);
    if (invalid) { free(text); return NULL; }
    return text;
}
static int identifier(const char *id) {
    if (!*id) return 0;
    for (; *id; ++id)
        if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
              (*id >= '0' && *id <= '9') || *id == '_' || *id == '-')) return 0;
    return 1;
}
int main(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], "--dataset") || strcmp(argv[3], "--profile")) return 2;
    char *data = read_json(argv[2]), *profile = read_json(argv[4]);
    cmp_json_object config, dataset;
    cmp_json_array cases;
    char schema[80], scope[80];
    int status = 2, intent_profile;
    if (!data || !profile || !cmp_json_object_parse(profile, &config) || config.field_count != 2 ||
        !cmp_json_object_str(&config, "schema", schema, sizeof(schema)) ||
        !cmp_json_object_str(&config, "scope", scope, sizeof(scope))) goto done;
    intent_profile = !strcmp(schema, "waterdeep-scene-profile/v2");
    if ((!intent_profile && strcmp(schema, "waterdeep-scene-profile/v1")) ||
        strcmp(scope, intent_profile ? "production-scene-intent-parser" : "production-scene-question-parser") ||
        !cmp_json_object_parse(data, &dataset) || dataset.field_count != 2 ||
        !cmp_json_object_str(&dataset, "schema", schema, sizeof(schema)) ||
        strcmp(schema, intent_profile ? "waterdeep-scene-cases/v2" : "waterdeep-scene-cases/v1") ||
        !cmp_json_field_array(cmp_json_object_field(&dataset, "cases"), &cases)) goto done;
    char ids[MAX_CASES][101];
    char *output = calloc(1, 262144);
    if (!output) goto done;
    size_t used = (size_t)snprintf(output, 262144, "{\"schema\":\"waterdeep-evaluation-result/v1\",\"cases\":[");
    cmp_json_field element;
    int next, count = 0;
    while ((next = cmp_json_array_next(&cases, &element)) == 1) {
        cmp_json_object row;
        char utterance[2048], expected[201], expected_spell[201] = {0};
        char escaped[1207], escaped_spell[1207], observed[2500], encoded[5001];
        dnd_scene_intent intent;
        int recognized;
        if (count == MAX_CASES || !cmp_json_field_object(&element, &row) || row.field_count != (intent_profile ? 5u : 4u) ||
            !cmp_json_object_str(&row, "id", ids[count], sizeof(ids[count])) || !identifier(ids[count]) ||
            !cmp_json_object_str(&row, "utterance", utterance, sizeof(utterance)) ||
            !cmp_json_object_bool(&row, "recognized", &recognized) ||
            !cmp_json_object_str(&row, "name", expected, sizeof(expected)) ||
            (intent_profile && !cmp_json_object_str(&row, "proposed_spell", expected_spell, sizeof(expected_spell))) ||
            (!recognized && (*expected || *expected_spell))) goto invalid;
        for (int i = 0; i < count; ++i) if (!strcmp(ids[i], ids[count])) goto invalid;
        int actual = dnd_scene_parse(utterance, &intent);
        if (cmp_json_escape_exact(intent.character_name, escaped, sizeof(escaped)) ||
            cmp_json_escape_exact(intent.proposed_spell, escaped_spell, sizeof(escaped_spell))) goto invalid;
        int n = intent_profile ? snprintf(observed, sizeof(observed),
            "{\"recognized\":%s,\"name\":\"%s\",\"proposed_spell\":\"%s\"}", actual ? "true" : "false", escaped, escaped_spell) :
            snprintf(observed, sizeof(observed), "{\"recognized\":%s,\"name\":\"%s\"}", actual ? "true" : "false", escaped);
        if (n < 0 || (size_t)n >= sizeof(observed) || cmp_json_escape_exact(observed, encoded, sizeof(encoded))) goto invalid;
        n = snprintf(output + used, 262144 - used, "%s{\"id\":\"%s\",\"passed\":%s,\"output\":\"%s\"}",
            count ? "," : "", ids[count], actual == recognized && !strcmp(intent.character_name, expected) &&
                (!intent_profile || !strcmp(intent.proposed_spell, expected_spell)) ? "true" : "false", encoded);
        if (n < 0 || (size_t)n >= 262144 - used) goto invalid;
        used += (size_t)n; ++count;
    }
    if (next < 0 || !count || used + 4 >= 262144) goto invalid;
    memcpy(output + used, "]}\n", 4);
    if (fputs(output, stdout) >= 0 && !fflush(stdout)) status = 0;
invalid:
    free(output);
done:
    free(data); free(profile);
    return status;
}
