/* Offline observation of the production parser. No facts or game effects. */
#include "dnd_tools.h"
#include "cmp_json.h"
#include <stdio.h>

int main(int argc, char **argv) {
    dnd_scene_intent intent;
    char character[1201], spell[1201];
    if (argc != 2) return 2;
    int parsed = dnd_scene_parse(argv[1], &intent);
    if (cmp_json_escape_exact(intent.character_name, character, sizeof(character)) ||
        cmp_json_escape_exact(intent.proposed_spell, spell, sizeof(spell))) return 3;
    printf("{\"intent\":\"%s\",\"character_name\":\"%s\",\"proposed_spell\":\"%s\"}\n",
           parsed ? "scene_presence_question" : "other", character, spell);
    return 0;
}
