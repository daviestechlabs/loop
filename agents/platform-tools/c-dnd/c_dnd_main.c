/*
 * c-dnd — pure-C initiative CLI (no Go, no cgo).
 *
 * Line protocol on stdin (whitespace-separated; # comments):
 *   start <campaign> <encounter> <owner>
 *   seed_begin <campaign> <encounter> <owner> <version> <status> <round> <active_index>
 *   seed_part <id> <name> <initiative> <max_hp> <current_hp> [cond...]
 *   seed_commit
 *   add <id> <name> <initiative> <max_hp> <current_hp>
 *   remove <id>
 *   advance
 *   end
 *   damage <id> <amount>
 *   heal <id> <amount>
 *   condition_add <id> <condition>
 *   condition_remove <id> <condition>
 *   summary
 *   active
 *   dump
 *   validate
 *
 * On success prints OK lines; dump prints key=value state. Errors: ERR <code> <msg>
 * seed_* restores durable state without bumping version (for Go host CAS path).
 */
#define _POSIX_C_SOURCE 200809L

#include "dnd_initiative.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dnd_encounter_v1 g;
static int g_live;

static void err_rc(int rc, const char *msg) {
    fprintf(stdout, "ERR %d %s\n", rc, msg ? msg : "error");
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v;
    if (!s || !out) return -1;
    v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) return -1;
    *out = (int)v;
    return 0;
}

static void dump_state(void) {
    int i, c;
    char active[DND_ID_CAP];
    char summary[DND_SUMMARY_CAP];
    printf("DUMP campaign=%s encounter=%s owner=%s version=%lld status=%s round=%d n=%d active_index=%d\n",
           g.campaign_id, g.encounter_id, g.owner_user_id, (long long)g.version, g.status, g.round,
           g.n_participants, g.active_index);
    dnd_encounter_active_id_v1(&g, active, sizeof(active));
    dnd_encounter_summary_v1(&g, summary, sizeof(summary));
    printf("DUMP active_id=%s\n", active);
    printf("DUMP summary=%s\n", summary);
    for (i = 0; i < g.n_participants; i++) {
        const dnd_participant_v1 *p = &g.participants[i];
        printf("PART %s name=%s init=%d hp=%d/%d conds=%d", p->id, p->name, p->initiative,
               p->current_hp, p->max_hp, p->n_conditions);
        for (c = 0; c < p->n_conditions; c++) {
            printf(" %s", p->conditions[c]);
        }
        printf("\n");
    }
}

int main(void) {
    char line[1024];
    g_live = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char *toks[16];
        int n = 0;
        char *save = NULL;
        char *p = line;
        /* strip comment */
        {
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
        }
        for (p = strtok_r(line, " \t\r\n", &save); p && n < 16; p = strtok_r(NULL, " \t\r\n", &save)) {
            toks[n++] = p;
        }
        if (n == 0) continue;
        if (strcmp(toks[0], "start") == 0) {
            int rc;
            if (n < 4) { err_rc(DND_ERR_ARGUMENT, "start needs campaign encounter owner"); continue; }
            rc = dnd_encounter_start_v1(&g, toks[1], toks[2], toks[3]);
            g_live = (rc == DND_OK);
            if (rc != DND_OK) err_rc(rc, "start failed");
            else printf("OK start version=%lld\n", (long long)g.version);
        } else if (strcmp(toks[0], "seed_begin") == 0) {
            int version, round, active_index;
            if (n < 8) {
                err_rc(DND_ERR_ARGUMENT, "seed_begin needs campaign encounter owner version status round active_index");
                continue;
            }
            if (parse_int(toks[4], &version) || parse_int(toks[6], &round) || parse_int(toks[7], &active_index)) {
                err_rc(DND_ERR_ARGUMENT, "bad seed integers");
                continue;
            }
            if (!dnd_id_ok_v1(toks[1]) || !dnd_id_ok_v1(toks[2]) || !toks[3][0]) {
                err_rc(DND_ERR_ARGUMENT, "bad seed identity");
                continue;
            }
            if (strcmp(toks[5], "active") != 0 && strcmp(toks[5], "ended") != 0) {
                err_rc(DND_ERR_ARGUMENT, "bad seed status");
                continue;
            }
            memset(&g, 0, sizeof(g));
            snprintf(g.campaign_id, sizeof(g.campaign_id), "%s", toks[1]);
            snprintf(g.encounter_id, sizeof(g.encounter_id), "%s", toks[2]);
            snprintf(g.owner_user_id, sizeof(g.owner_user_id), "%s", toks[3]);
            g.version = version;
            snprintf(g.status, sizeof(g.status), "%s", toks[5]);
            g.round = round;
            g.active_index = active_index;
            g.n_participants = 0;
            g_live = 1;
            printf("OK seed_begin\n");
        } else if (strcmp(toks[0], "seed_part") == 0) {
            dnd_participant_v1 part;
            int init, maxhp, curhp, i;
            if (!g_live) { err_rc(DND_ERR_STATE, "no seed"); continue; }
            if (n < 6) { err_rc(DND_ERR_ARGUMENT, "seed_part needs id name init max cur"); continue; }
            if (g.n_participants >= DND_MAX_PARTICIPANTS) { err_rc(DND_ERR_CAPACITY, "full"); continue; }
            memset(&part, 0, sizeof(part));
            snprintf(part.id, sizeof(part.id), "%s", toks[1]);
            snprintf(part.name, sizeof(part.name), "%s", toks[2]);
            if (parse_int(toks[3], &init) || parse_int(toks[4], &maxhp) || parse_int(toks[5], &curhp)) {
                err_rc(DND_ERR_ARGUMENT, "bad seed_part ints");
                continue;
            }
            part.initiative = init;
            part.max_hp = maxhp;
            part.current_hp = curhp;
            for (i = 6; i < n && part.n_conditions < DND_MAX_CONDITIONS; i++) {
                snprintf(part.conditions[part.n_conditions], 32, "%s", toks[i]);
                part.n_conditions++;
            }
            g.participants[g.n_participants++] = part;
            printf("OK seed_part n=%d\n", g.n_participants);
        } else if (strcmp(toks[0], "seed_commit") == 0) {
            int rc;
            if (!g_live) { err_rc(DND_ERR_STATE, "no seed"); continue; }
            rc = dnd_encounter_validate_v1(&g);
            if (rc != DND_OK) {
                g_live = 0;
                err_rc(rc, "seed_commit validate failed");
            } else {
                printf("OK seed_commit version=%lld n=%d\n", (long long)g.version, g.n_participants);
            }
        } else if (strcmp(toks[0], "add") == 0) {
            dnd_participant_v1 part;
            int init, maxhp, curhp, rc;
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            if (n < 6) { err_rc(DND_ERR_ARGUMENT, "add needs id name initiative max_hp current_hp"); continue; }
            memset(&part, 0, sizeof(part));
            snprintf(part.id, sizeof(part.id), "%s", toks[1]);
            snprintf(part.name, sizeof(part.name), "%s", toks[2]);
            if (parse_int(toks[3], &init) || parse_int(toks[4], &maxhp) || parse_int(toks[5], &curhp)) {
                err_rc(DND_ERR_ARGUMENT, "bad integers");
                continue;
            }
            part.initiative = init;
            part.max_hp = maxhp;
            part.current_hp = curhp;
            rc = dnd_encounter_add_v1(&g, &part);
            if (rc != DND_OK) err_rc(rc, "add failed");
            else printf("OK add version=%lld n=%d\n", (long long)g.version, g.n_participants);
        } else if (strcmp(toks[0], "remove") == 0) {
            int rc;
            if (!g_live || n < 2) { err_rc(DND_ERR_ARGUMENT, "remove needs id"); continue; }
            rc = dnd_encounter_remove_v1(&g, toks[1]);
            if (rc != DND_OK) err_rc(rc, "remove failed");
            else printf("OK remove version=%lld\n", (long long)g.version);
        } else if (strcmp(toks[0], "advance") == 0) {
            int rc;
            char active[DND_ID_CAP];
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            rc = dnd_encounter_advance_v1(&g);
            dnd_encounter_active_id_v1(&g, active, sizeof(active));
            if (rc != DND_OK) err_rc(rc, "advance failed");
            else printf("OK advance version=%lld active=%s round=%d\n", (long long)g.version, active, g.round);
        } else if (strcmp(toks[0], "end") == 0) {
            int rc;
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            rc = dnd_encounter_end_v1(&g);
            if (rc != DND_OK) err_rc(rc, "end failed");
            else printf("OK end version=%lld\n", (long long)g.version);
        } else if (strcmp(toks[0], "damage") == 0 || strcmp(toks[0], "heal") == 0) {
            int amount, rc;
            if (!g_live || n < 3) { err_rc(DND_ERR_ARGUMENT, "damage|heal needs id amount"); continue; }
            if (parse_int(toks[2], &amount)) { err_rc(DND_ERR_ARGUMENT, "bad amount"); continue; }
            rc = (toks[0][0] == 'd') ? dnd_encounter_damage_v1(&g, toks[1], amount)
                                     : dnd_encounter_heal_v1(&g, toks[1], amount);
            if (rc != DND_OK) err_rc(rc, "hp op failed");
            else printf("OK %s version=%lld\n", toks[0], (long long)g.version);
        } else if (strcmp(toks[0], "condition_add") == 0 || strcmp(toks[0], "condition_remove") == 0) {
            int rc;
            if (!g_live || n < 3) { err_rc(DND_ERR_ARGUMENT, "condition op needs id condition"); continue; }
            rc = (strstr(toks[0], "add")) ? dnd_encounter_condition_add_v1(&g, toks[1], toks[2])
                                          : dnd_encounter_condition_remove_v1(&g, toks[1], toks[2]);
            if (rc != DND_OK) err_rc(rc, "condition op failed");
            else printf("OK %s version=%lld\n", toks[0], (long long)g.version);
        } else if (strcmp(toks[0], "summary") == 0) {
            char summary[DND_SUMMARY_CAP];
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            dnd_encounter_summary_v1(&g, summary, sizeof(summary));
            printf("OK summary %s\n", summary);
        } else if (strcmp(toks[0], "active") == 0) {
            char active[DND_ID_CAP];
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            dnd_encounter_active_id_v1(&g, active, sizeof(active));
            printf("OK active %s\n", active);
        } else if (strcmp(toks[0], "dump") == 0) {
            if (!g_live) { err_rc(DND_ERR_STATE, "no encounter"); continue; }
            dump_state();
            printf("OK dump\n");
        } else if (strcmp(toks[0], "validate") == 0) {
            int rc = g_live ? dnd_encounter_validate_v1(&g) : DND_ERR_STATE;
            if (rc != DND_OK) err_rc(rc, "validate failed");
            else printf("OK validate\n");
        } else {
            err_rc(DND_ERR_ARGUMENT, "unknown op");
        }
    }
    return 0;
}
