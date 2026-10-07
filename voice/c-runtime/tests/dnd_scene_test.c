#include "dnd_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int valid, const char *label) {
    if (!valid) { fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

int main(int argc, char **argv) {
    dnd_scene_intent intent;
    char name[201], input[2048];
    dnd_tool_result *result = calloc(1, sizeof(*result));
    if (!result) return 2;
    if (argc == 5 && !strcmp(argv[1], "input")) {
        int n = dnd_scene_input(argv[2], argv[3], argv[4], input, sizeof(input));
        if (n > 0) puts(input);
        free(result);
        return n > 0 ? 0 : 1;
    }
    if (argc == 7 && !strcmp(argv[1], "output")) {
        if (strlen(argv[6]) >= sizeof(result->call_id)) { free(result); return 2; }
        strcpy(result->call_id, argv[6]);
        int rc = dnd_scene_output(argv[2], argv[3], argv[4], argv[5], result);
        if (!rc) puts(result->text);
        free(result);
        return rc ? 1 : 0;
    }
    const char *positive[] = {
        "Is Mira still in the room?", "is Mira in the room", "Please is Mira here?",
        "I cast fireball, no wait is Mira still in the room?",
        "I cast fireball. No, wait, is Mira still in this room?",
        "I cast Fireball. No, wait. Is Mira still in the room?",
        "I cast fireball. No wait. Is Mira still in the room?",
        "Is Eli here? Hold on. Is Mira still here?",
        "Wait. Is Mira here?", "Wait, is Mira here?", "Hold on -- is Mira in the scene.",
        "Is Eli here, no wait is Mira still here?", "  Is   Mira  in the room?  ",
        "Can you tell me whether Mira is still here?",
        "Check whether Mira is in this room.", "Do we know if Mira is still here?",
        "Can you check if Mira is in the room?", "Tell me whether Mira is here.",
        "Please can you check if Mira is still in this room?",
        "I cast Fireball. Actually, wait—is Mira here?",
        "I cast Fireball, actually wait—can you check if Mira is here?",
        "Check whether Eli is here. No wait, tell me whether Mira is here.",
        "Before I cast Fireball, is Mira still here?",
        "Before I cast Fireball is Mira still here?",
        "Before I cast Moon Ward can you check if Mira is in the room?",
        "Before I cast Moon Ward, can you check if Mira is in the room?",
        "Before I cast Fireball, is Eli here, no wait is Mira here?"
    };
    const char *negative[] = {
        "I cast fireball. No way is Miral still in the room?",
        "Is Ela here? Know it? Is Mira still here?",
        "I cast fireball. No wait... Is Mira here?",
        "I cast fireball. No wait? Is Mira here?",
        "She said no wait. Is Mira here?",
        "If I cast fireball. No wait. Is Mira here?",
        "I cast fireball. No wait. Is Mira here? I cast fireball.",
        "I cast fireball.", "Is Mira within the blast radius?", "Was Mira in the room?",
        "Is Mira in the room if I open the door?", "Mira is in the room.",
        "Is Mira still in the room? I cast fireball.", "Is <Mira> here?", "Is Mira [secret] here?",
        "Is Mira here??", "Is here?", "Wait", "", "No wait",
        "Is she out of its area over here?", "Is she still in the room?",
        "Is he here?", "Is it in the room?", "Is someone here?",
        "Is that person in the room?", "Please is SHE here?",
        "Is Mira outside the blast area here?", "Is Elira within its radius here?",
        "Is Mira safe in this room?", "Is Mira behind me here?",
        "Can you check if she is here?", "Do we know if that person is here?",
        "Can you check if Mira was in the room?", "Tell me whether Mira will be here.",
        "Check whether Mira is within the blast radius.", "Tell me whether Mira is safe here.",
        "Do we know if Mira is not in the room?", "Check whether Mira is no longer here.",
        "Can you check if Mira is here if I open the door?",
        "Mira asked, can you check if Eli is here?",
        "I wrote: I cast Fireball. Actually, wait—is Mira here?",
        "If I cast Fireball, actually wait—can you check if Mira is here?",
        "Can you check if Mira is here? Cast Fireball.",
        "Tell me whether Mira is here??", "Can you check if Mira—is here?",
        "Before I cast Fireball if needed, is Mira here?",
        "Before I cast Fireball and move, is Mira here?",
        "Before I cast , is Mira here?", "Before I cast is Mira here?",
        "Before I cast Fireball if needed is Mira here?",
        "Before I cast Fireball and move is Mira here?",
        "Before I cast Fireball is Mira here? Cast it.",
        "Before I cast Fireball, will Mira be here?",
        "Before I cast Fireball, is Mira safe here?",
        "Before I cast Fireball, is she here?",
        "Mira said before I cast Fireball, is Eli here?",
        "Before I cast Fireball, is Mira here? Cast it."
    };
    for (size_t i = 0; i < sizeof(positive) / sizeof(positive[0]); ++i)
        check(dnd_scene_question(positive[i], name) && !strcmp(name, "Mira"), positive[i]);
    for (size_t i = 0; i < sizeof(negative) / sizeof(negative[0]); ++i)
        check(!dnd_scene_question(negative[i], name) && !name[0], negative[i]);
    check(dnd_scene_question("Is Éowyn in the room?", name) && !strcmp(name, "Éowyn"), "UTF-8 name");
    check(dnd_scene_question("Is Sheena here?", name) && !strcmp(name, "Sheena"), "pronoun prefix is still a name");
    check(dnd_scene_question("Is Itzel here?", name) && !strcmp(name, "Itzel"), "it prefix is still a name");
    check(dnd_scene_question("Is Mira Outlander here?", name) && !strcmp(name, "Mira Outlander"), "spatial prefix is still a name");
    check(dnd_scene_parse("Before I cast fireball is Rowan still here?", &intent) &&
        !intent.proposed_spell[0] && !strcmp(intent.character_name, "Rowan"),
        "actual STT omission preserves question without cast authorization");
    check(dnd_scene_parse("Before I cast Fireball, is Mira here?", &intent) &&
        !intent.proposed_spell[0] && !strcmp(intent.character_name, "Mira"),
        "future plan is not an interrupted cast proposal");
    check(dnd_scene_parse("Before I cast Fireball, is Mira here, no wait is Eli here?", &intent) &&
        !intent.proposed_spell[0] && !strcmp(intent.character_name, "Eli"),
        "entity repair does not promote future plan to proposal");
    check(dnd_scene_parse("I cast Fireball. No wait. Is Mira here?", &intent) &&
        !strcmp(intent.proposed_spell, "Fireball") && !strcmp(intent.character_name, "Mira"),
        "repaired proposal remains separate from scene question");
    check(dnd_scene_parse("I cast Fireball, no wait is Mira here, no wait is Eli here?", &intent) &&
        !strcmp(intent.proposed_spell, "Fireball") && !strcmp(intent.character_name, "Eli"),
        "later entity repair preserves initial proposal");
    check(dnd_scene_parse("Is Mira here?", &intent) && !intent.proposed_spell[0],
        "plain question does not invent a proposal");
    check(dnd_scene_parse("I cast Moon Ward. Actually, wait—can you check if Elowen is here?", &intent) &&
        !strcmp(intent.proposed_spell, "Moon Ward") && !strcmp(intent.character_name, "Elowen"),
        "request paraphrase preserves interrupted proposal");
    check(dnd_scene_question("Please tell me whether Aster Vale is still in this room?", name) &&
        !strcmp(name, "Aster Vale"), "fresh multiword label through request form");
    check(!dnd_scene_parse("Mira said I cast Fireball, no wait is Mira here?", &intent) &&
        !intent.proposed_spell[0] && !intent.character_name[0], "reported action is not a proposal");
    check(!dnd_scene_parse("I cast Fireball, no wait is Mira here? Cast it.", &intent) &&
        !intent.proposed_spell[0] && !intent.character_name[0], "invalid suffix discards entire parse");
    check(!dnd_scene_parse(NULL, &intent) && !intent.proposed_spell[0] &&
        !intent.character_name[0] && !dnd_scene_parse("Is Mira here?", NULL), "invalid parse outputs clear");
    check(dnd_scene_input(positive[0], "table", "hall", input, sizeof(input)) > 0 &&
        !strcmp(input, "{\"operation\":\"resolve_scene_presence\",\"campaign_id\":\"table\",\"scene_id\":\"hall\",\"character_name\":\"Mira\"}"), "bounded command");
    check(dnd_scene_input(positive[0], "../table", "hall", input, sizeof(input)) < 0, "campaign traversal rejected");
    check(dnd_scene_input(positive[0], "table", "hall", input, 2u) < 0, "small output rejected");
    free(result);
    if (failures) return 1;
    puts("ALL PASS C scene question contract");
    return 0;
}
