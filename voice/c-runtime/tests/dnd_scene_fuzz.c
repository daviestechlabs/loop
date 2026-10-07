#include "dnd_tools.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char input[8193], name[201], command[2048];
    dnd_scene_intent intent;
    dnd_tool_result *result;
    if (!size || size > 8192u || memchr(data, '\0', size)) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    int parsed = dnd_scene_parse(input, &intent);
    if (parsed != dnd_scene_question(input, name) || strcmp(name, intent.character_name)) abort();
    if (!parsed && (intent.character_name[0] || intent.proposed_spell[0])) abort();
    if (intent.proposed_spell[0] && (!parsed || !intent.character_name[0])) abort();
    (void)dnd_scene_input(input, "table", "hall", command, sizeof(command));
    result = calloc(1, sizeof(*result));
    if (!result) return 0;
    strcpy(result->call_id, "scene-call");
    (void)dnd_scene_output(input, "I cast fireball, no wait is Mira still in the room?", "table", "hall", result);
    free(result);
    return 0;
}
