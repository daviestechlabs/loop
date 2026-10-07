/* Unit tests for pure-C dnd initiative kernel. */
#include "dnd_initiative.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *name, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    } else {
        printf("PASS %s\n", name);
    }
}

static dnd_participant_v1 make_p(const char *id, const char *name, int init, int hp) {
    dnd_participant_v1 p;
    memset(&p, 0, sizeof(p));
    snprintf(p.id, sizeof(p.id), "%s", id);
    snprintf(p.name, sizeof(p.name), "%s", name);
    p.initiative = init;
    p.max_hp = hp;
    p.current_hp = hp;
    return p;
}

int main(void) {
    dnd_encounter_v1 e;
    dnd_participant_v1 goblin, rogue;
    char summary[DND_SUMMARY_CAP];
    char active[DND_ID_CAP];

    expect("id ok", dnd_id_ok_v1("campaign-1"));
    expect("id bad space", !dnd_id_ok_v1("bad id"));
    expect("id empty", !dnd_id_ok_v1(""));
    expect("cond prone", dnd_condition_ok_v1("prone"));
    expect("cond sigh no", !dnd_condition_ok_v1("sigh"));
    expect("dmg slash", dnd_damage_type_ok_v1("slashing"));
    expect("dmg laser no", !dnd_damage_type_ok_v1("laser"));

    expect("start", dnd_encounter_start_v1(&e, "campaign-1", "ambush-1", "user-1") == DND_OK);
    expect("start ver", e.version == 1);
    expect("start active -1", e.active_index == -1);
    expect("start empty", e.n_participants == 0);

    goblin = make_p("goblin-1", "Goblin", 12, 15);
    rogue = make_p("rogue-1", "Rogue", 18, 20);
    expect("add goblin", dnd_encounter_add_v1(&e, &goblin) == DND_OK);
    expect("add rogue", dnd_encounter_add_v1(&e, &rogue) == DND_OK);
    expect("sorted n", e.n_participants == 2);
    expect("sorted0 rogue", strcmp(e.participants[0].id, "rogue-1") == 0);
    expect("sorted1 goblin", strcmp(e.participants[1].id, "goblin-1") == 0);
    expect("ver after adds", e.version == 3);

    expect("advance1", dnd_encounter_advance_v1(&e) == DND_OK);
    dnd_encounter_active_id_v1(&e, active, sizeof(active));
    expect("active rogue", strcmp(active, "rogue-1") == 0);
    expect("round1", e.round == 1);

    expect("advance2", dnd_encounter_advance_v1(&e) == DND_OK);
    dnd_encounter_active_id_v1(&e, active, sizeof(active));
    expect("active goblin", strcmp(active, "goblin-1") == 0);

    expect("advance3 wrap", dnd_encounter_advance_v1(&e) == DND_OK);
    dnd_encounter_active_id_v1(&e, active, sizeof(active));
    expect("wrap rogue", strcmp(active, "rogue-1") == 0);
    expect("round2", e.round == 2);

    expect("damage", dnd_encounter_damage_v1(&e, "goblin-1", 7) == DND_OK);
    expect("hp after dmg", e.participants[1].current_hp == 8);
    expect("heal cap", dnd_encounter_heal_v1(&e, "goblin-1", 99) == DND_OK);
    expect("hp full", e.participants[1].current_hp == 15);

    expect("cond add", dnd_encounter_condition_add_v1(&e, "goblin-1", "prone") == DND_OK);
    expect("cond n", e.participants[1].n_conditions == 1);
    expect("cond val", strcmp(e.participants[1].conditions[0], "prone") == 0);
    expect("cond add2 sorted", dnd_encounter_condition_add_v1(&e, "goblin-1", "blinded") == DND_OK);
    expect("cond0 blinded", strcmp(e.participants[1].conditions[0], "blinded") == 0);
    expect("cond1 prone", strcmp(e.participants[1].conditions[1], "prone") == 0);
    expect("cond rm", dnd_encounter_condition_remove_v1(&e, "goblin-1", "prone") == DND_OK);
    expect("cond left", e.participants[1].n_conditions == 1);

    expect("remove active", dnd_encounter_remove_v1(&e, "rogue-1") == DND_OK);
    dnd_encounter_active_id_v1(&e, active, sizeof(active));
    expect("active after rm", strcmp(active, "goblin-1") == 0 || e.n_participants == 1);

    expect("end", dnd_encounter_end_v1(&e) == DND_OK);
    expect("ended status", strcmp(e.status, "ended") == 0);
    expect("ended active", e.active_index == -1);
    expect("summary", dnd_encounter_summary_v1(&e, summary, sizeof(summary)) == DND_OK);
    expect("summary ended", strstr(summary, "Ended encounter") != NULL);

    /* validate rejects unsorted */
    {
        dnd_encounter_v1 bad;
        dnd_encounter_start_v1(&bad, "c1", "e1", "u1");
        bad.n_participants = 2;
        bad.participants[0] = make_p("a", "A", 1, 1);
        bad.participants[1] = make_p("b", "B", 10, 1); /* higher init after lower — unsorted */
        bad.version = 1;
        expect("unsorted bad", dnd_encounter_validate_v1(&bad) == DND_ERR_VALIDATION);
    }

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ALL PASS c-dnd\n");
    return 0;
}
