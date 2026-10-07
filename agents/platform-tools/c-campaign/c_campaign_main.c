/*
 * c-campaign — pure-C campaign validation CLI (no Go, no cgo).
 *
 * Line protocol (whitespace tokens; # comments):
 *   id_ok <id>
 *   scope_ok <scope>
 *   pron_ok <token>
 *   meta_ok <name> <ruleset> <desc_len> <scene_len> <house_rules>
 *   char_ok <id> <name> <kind> <player|_> <level> <ac> <maxhp> \
 *           <persona_len> <voice|_> <style|_> <pron|_> \
 *           <abi:cha,con,dex,int,str,wis> <scopes:s1,s2> <safety_n> <sheets_n>
 *   scene_npc_ok  (same args as char_ok)
 *   recaps_ok <n> then n lines: <session_id> <turn_id> <summary_len> <created_ms>
 *     (use recaps_ok 0 for empty)
 *   director_ok <next_idx> <turn|_> <speaker|_> <lease_ms> <npc_ids:a,b|_> <known:a,b|_>
 *
 * Prints: OK <op>  or  ERR <code> <msg>
 */
#define _POSIX_C_SOURCE 200809L

#include "dnd_campaign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void ok(const char *op) { printf("OK %s\n", op); }
static void err(int code, const char *msg) { printf("ERR %d %s\n", code, msg ? msg : "error"); }

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v;
    if (!s || !out) return -1;
    v = strtol(s, &end, 10);
    if (end == s || (end && *end)) return -1;
    *out = (int)v;
    return 0;
}

static int parse_abilities(const char *s, int out[6]) {
    char buf[128];
    char *save = NULL, *tok;
    int i = 0;
    if (!s || strlen(s) >= sizeof(buf)) return -1;
    memcpy(buf, s, strlen(s) + 1);
    for (tok = strtok_r(buf, ",", &save); tok && i < 6; tok = strtok_r(NULL, ",", &save)) {
        if (parse_int(tok, &out[i]) != 0) return -1;
        i++;
    }
    return i == 6 ? 0 : -1;
}

static int parse_scopes(const char *s, dnd_camp_character_v1 *ch) {
    char buf[256];
    char *save = NULL, *tok;
    if (!s) {
        ch->n_scopes = 0;
        return 0;
    }
    if (strcmp(s, "_") == 0 || s[0] == '\0') {
        ch->n_scopes = 0;
        return 0;
    }
    if (strlen(s) >= sizeof(buf)) return -1;
    memcpy(buf, s, strlen(s) + 1);
    ch->n_scopes = 0;
    for (tok = strtok_r(buf, ",", &save); tok && ch->n_scopes < DND_CAMP_MAX_SCOPES;
         tok = strtok_r(NULL, ",", &save)) {
        snprintf(ch->scopes[ch->n_scopes], DND_CAMP_SCOPE_CAP, "%s", tok);
        ch->n_scopes++;
    }
    return 0;
}

static int fill_char(char **toks, int n, dnd_camp_character_v1 *ch) {
    int persona_len, safety_n, sheets_n;
    /* id name kind player level ac maxhp persona_len voice style pron abi scopes safety sheets = 15 */
    if (n < 15) return -1;
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->id, sizeof(ch->id), "%s", toks[0]);
    snprintf(ch->name, sizeof(ch->name), "%s", toks[1]);
    snprintf(ch->kind, sizeof(ch->kind), "%s", toks[2]);
    if (strcmp(toks[3], "_") != 0) snprintf(ch->player_user_id, sizeof(ch->player_user_id), "%s", toks[3]);
    if (parse_int(toks[4], &ch->level) || parse_int(toks[5], &ch->armor_class) ||
        parse_int(toks[6], &ch->max_hp) || parse_int(toks[7], &persona_len)) {
        return -1;
    }
    if (persona_len < 0 || persona_len > 4000) return -1;
    if (persona_len > 0) {
        /* synthetic persona of exact length for bound checks */
        memset(ch->persona, 'x', (size_t)persona_len);
        ch->persona[persona_len] = '\0';
    }
    if (strcmp(toks[8], "_") != 0) snprintf(ch->voice_id, sizeof(ch->voice_id), "%s", toks[8]);
    if (strcmp(toks[9], "_") != 0) snprintf(ch->speaking_style, sizeof(ch->speaking_style), "%s", toks[9]);
    if (strcmp(toks[10], "_") != 0) snprintf(ch->pronunciation, sizeof(ch->pronunciation), "%s", toks[10]);
    if (parse_abilities(toks[11], ch->ability) != 0) return -1;
    if (parse_scopes(toks[12], ch) != 0) return -1;
    if (parse_int(toks[13], &safety_n) || parse_int(toks[14], &sheets_n)) return -1;
    ch->n_safety = safety_n;
    ch->sheet_ref_count = sheets_n;
    if (safety_n < 0 || safety_n > DND_CAMP_MAX_SAFETY) return -1;
    /* synthetic unique sorted safety rules s00, s01, ... */
    {
        int i;
        for (i = 0; i < safety_n; i++) {
            snprintf(ch->safety[i], sizeof(ch->safety[i]), "s%02d", i);
        }
    }
    return 0;
}

int main(void) {
    char line[2048];
    while (fgets(line, sizeof(line), stdin)) {
        char *toks[32];
        int n = 0;
        char *save = NULL, *p;
        {
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
        }
        for (p = strtok_r(line, " \t\r\n", &save); p && n < 32; p = strtok_r(NULL, " \t\r\n", &save)) {
            toks[n++] = p;
        }
        if (n == 0) continue;
        if (strcmp(toks[0], "id_ok") == 0) {
            if (n < 2) { err(DND_CAMP_ERR_ARGUMENT, "id_ok needs id"); continue; }
            if (dnd_camp_id_ok_v1(toks[1])) ok("id_ok");
            else err(DND_CAMP_ERR_VALIDATION, "bad id");
        } else if (strcmp(toks[0], "scope_ok") == 0) {
            if (n < 2) { err(DND_CAMP_ERR_ARGUMENT, "scope_ok needs scope"); continue; }
            if (dnd_camp_knowledge_scope_ok_v1(toks[1])) ok("scope_ok");
            else err(DND_CAMP_ERR_VALIDATION, "bad scope");
        } else if (strcmp(toks[0], "pron_ok") == 0) {
            /* Allow spaces: rebuild from tokens[1..] */
            char pron[256];
            size_t pos = 0;
            int i;
            if (n < 2) { err(DND_CAMP_ERR_ARGUMENT, "pron_ok needs text"); continue; }
            pron[0] = '\0';
            for (i = 1; i < n; i++) {
                size_t ln = strlen(toks[i]);
                if (pos && pos + 1 < sizeof(pron)) pron[pos++] = ' ';
                if (pos + ln >= sizeof(pron)) { err(DND_CAMP_ERR_VALIDATION, "pron too long"); goto next_line; }
                memcpy(pron + pos, toks[i], ln);
                pos += ln;
                pron[pos] = '\0';
            }
            if (dnd_camp_pronunciation_ok_v1(pron)) ok("pron_ok");
            else err(DND_CAMP_ERR_VALIDATION, "bad pron");
        } else if (strcmp(toks[0], "meta_ok") == 0) {
            dnd_camp_metadata_v1 m;
            int desc_len, scene_len, house_n, rc;
            if (n < 6) { err(DND_CAMP_ERR_ARGUMENT, "meta_ok args"); continue; }
            memset(&m, 0, sizeof(m));
            snprintf(m.name, sizeof(m.name), "%s", toks[1]);
            snprintf(m.ruleset, sizeof(m.ruleset), "%s", toks[2]);
            if (parse_int(toks[3], &desc_len) || parse_int(toks[4], &scene_len) ||
                parse_int(toks[5], &house_n)) {
                err(DND_CAMP_ERR_ARGUMENT, "bad meta ints");
                continue;
            }
            if (desc_len < 0 || desc_len > 4000 || scene_len < 0 || scene_len > 500) {
                err(DND_CAMP_ERR_VALIDATION, "meta lens");
                continue;
            }
            if (desc_len) memset(m.description, 'd', (size_t)desc_len);
            if (scene_len) memset(m.current_scene, 's', (size_t)scene_len);
            m.house_rule_count = house_n;
            rc = dnd_camp_metadata_ok_v1(&m);
            if (rc == DND_CAMP_OK) ok("meta_ok");
            else err(rc, "meta invalid");
        } else if (strcmp(toks[0], "char_ok") == 0 || strcmp(toks[0], "scene_npc_ok") == 0) {
            dnd_camp_character_v1 ch;
            int rc;
            if (fill_char(toks + 1, n - 1, &ch) != 0) {
                err(DND_CAMP_ERR_ARGUMENT, "char args");
                continue;
            }
            if (strcmp(toks[0], "scene_npc_ok") == 0) rc = dnd_camp_scene_npc_ok_v1(&ch);
            else rc = dnd_camp_character_ok_v1(&ch);
            if (rc == DND_CAMP_OK) ok(toks[0]);
            else err(rc, "char invalid");
        } else if (strcmp(toks[0], "recaps_ok") == 0) {
            int count, i, rc;
            dnd_camp_session_recap_v1 recs[DND_CAMP_MAX_RECAPS + 1];
            if (n < 2 || parse_int(toks[1], &count) != 0 || count < 0 ||
                count > DND_CAMP_MAX_RECAPS + 1) {
                err(DND_CAMP_ERR_ARGUMENT, "recaps_ok count");
                continue;
            }
            memset(recs, 0, sizeof(recs));
            for (i = 0; i < count; i++) {
                char rline[512];
                char *rtoks[8];
                int rn = 0;
                char *rsave = NULL, *rp;
                long long created;
                if (!fgets(rline, sizeof(rline), stdin)) {
                    err(DND_CAMP_ERR_ARGUMENT, "recaps missing lines");
                    goto next_line;
                }
                for (rp = strtok_r(rline, " \t\r\n", &rsave); rp && rn < 8;
                     rp = strtok_r(NULL, " \t\r\n", &rsave)) {
                    rtoks[rn++] = rp;
                }
                if (rn < 4) {
                    err(DND_CAMP_ERR_ARGUMENT, "recap line args");
                    goto next_line;
                }
                snprintf(recs[i].session_id, sizeof(recs[i].session_id), "%s", rtoks[0]);
                snprintf(recs[i].source_turn_id, sizeof(recs[i].source_turn_id), "%s", rtoks[1]);
                if (parse_int(rtoks[2], &recs[i].summary_rune_count) != 0) {
                    err(DND_CAMP_ERR_ARGUMENT, "recap summary_len");
                    goto next_line;
                }
                {
                    char *end = NULL;
                    created = strtoll(rtoks[3], &end, 10);
                    if (end == rtoks[3] || (end && *end)) {
                        err(DND_CAMP_ERR_ARGUMENT, "recap created");
                        goto next_line;
                    }
                    recs[i].created_at_unix_ms = (int64_t)created;
                }
            }
            rc = dnd_camp_session_recaps_ok_v1(count ? recs : NULL, count);
            if (rc == DND_CAMP_OK) ok("recaps_ok");
            else err(rc, "recaps invalid");
        } else if (strcmp(toks[0], "director_ok") == 0) {
            /* director_ok next turn speaker lease npcs known */
            dnd_camp_scene_director_v1 d;
            char known[DND_CAMP_MAX_SCENE_CAST + 8][DND_CAMP_ID_CAP];
            int n_known = 0, rc, next_idx;
            long long lease;
            if (n < 7) {
                err(DND_CAMP_ERR_ARGUMENT, "director_ok args");
                continue;
            }
            memset(&d, 0, sizeof(d));
            memset(known, 0, sizeof(known));
            if (parse_int(toks[1], &next_idx) != 0) {
                err(DND_CAMP_ERR_ARGUMENT, "director next");
                continue;
            }
            d.next_speaker_index = next_idx;
            if (strcmp(toks[2], "_") != 0)
                snprintf(d.active_turn_id, sizeof(d.active_turn_id), "%s", toks[2]);
            if (strcmp(toks[3], "_") != 0)
                snprintf(d.active_speaker_id, sizeof(d.active_speaker_id), "%s", toks[3]);
            {
                char *end = NULL;
                lease = strtoll(toks[4], &end, 10);
                if (end == toks[4] || (end && *end)) {
                    err(DND_CAMP_ERR_ARGUMENT, "director lease");
                    continue;
                }
                d.lease_expires_unix_ms = (int64_t)lease;
            }
            if (strcmp(toks[5], "_") != 0 && toks[5][0]) {
                char buf[1024];
                char *save = NULL, *tok;
                if (strlen(toks[5]) >= sizeof(buf)) {
                    err(DND_CAMP_ERR_ARGUMENT, "npc list");
                    continue;
                }
                memcpy(buf, toks[5], strlen(toks[5]) + 1);
                for (tok = strtok_r(buf, ",", &save); tok && d.n_active < DND_CAMP_MAX_SCENE_CAST;
                     tok = strtok_r(NULL, ",", &save)) {
                    snprintf(d.active_npc_ids[d.n_active], sizeof(d.active_npc_ids[0]), "%s", tok);
                    d.n_active++;
                }
            }
            if (strcmp(toks[6], "_") != 0 && toks[6][0]) {
                char buf[1024];
                char *save = NULL, *tok;
                if (strlen(toks[6]) >= sizeof(buf)) {
                    err(DND_CAMP_ERR_ARGUMENT, "known list");
                    continue;
                }
                memcpy(buf, toks[6], strlen(toks[6]) + 1);
                for (tok = strtok_r(buf, ",", &save);
                     tok && n_known < (int)(sizeof(known) / sizeof(known[0]));
                     tok = strtok_r(NULL, ",", &save)) {
                    snprintf(known[n_known], sizeof(known[0]), "%s", tok);
                    n_known++;
                }
            }
            rc = dnd_camp_scene_director_ok_v1(&d, known, n_known);
            if (rc == DND_CAMP_OK) ok("director_ok");
            else err(rc, "director invalid");
        } else {
            err(DND_CAMP_ERR_ARGUMENT, "unknown op");
        }
    next_line:
        continue;
    }
    return 0;
}
