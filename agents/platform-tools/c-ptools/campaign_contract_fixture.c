/* Portable process oracle for the production campaign transition and encoder.
 * This fixture does not replace the HTTP, journal, or filesystem fault gates. */
#include "pt_campaign.h"
#include "../c-toolstore/toolstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    pt_call *call = calloc(1, sizeof(*call));
    char *before = calloc(1, PT_OUT), *after = malloc(PT_OUT), *canonical = malloc(PT_OUT);
    char path[TS_PATH];
    pt_dnd_identity identity;
    size_t n;
    int create, read_only, scene_tool = argc == 5 && !strcmp(argv[4], "scene"), result = 1;
    FILE *file = NULL;
    if ((argc != 4 && !scene_tool) || !call || !before || !after || !canonical ||
        strlen(argv[2]) >= sizeof(call->user_id) || strlen(argv[3]) >= sizeof(call->tool_call_id)) goto done;
    if (strcmp(argv[1], "-")) {
        file = fopen(argv[1], "rb");
        if (!file) goto done;
        n = fread(before, 1u, PT_OUT, file);
        if (ferror(file) || !n || n == PT_OUT || memchr(before, '\0', n)) goto done;
    }
    n = fread(call->input_json, 1u, sizeof(call->input_json), stdin);
    if (ferror(stdin) || !n || n == sizeof(call->input_json) || memchr(call->input_json, '\0', n)) goto done;
    strcpy(call->user_id, argv[2]);
    strcpy(call->tool_call_id, argv[3]);
    if (!(scene_tool ? pt_scene_presence_path : pt_campaign_path)("fixture", call->input_json, path, sizeof(path), &create, &read_only) ||
        !(scene_tool ? pt_scene_presence_transition : pt_campaign_transition)(before[0] ? before : NULL, after, PT_OUT, call) ||
        !pt_campaign_canonical(after, canonical, PT_OUT, &identity) || strcmp(after, canonical)) goto done;
    printf("{\"read_only\":%s,\"state\":%s,\"output\":%s}\n", read_only ? "true" : "false", after, call->output_json);
    result = 0;
done:
    if (file) fclose(file);
    free(call);
    free(before);
    free(after);
    free(canonical);
    return result;
}
