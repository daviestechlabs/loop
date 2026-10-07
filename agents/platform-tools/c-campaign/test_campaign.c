#include "dnd_campaign.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(const char *n, int c) {
    if (!c) { fprintf(stderr, "FAIL %s\n", n); failures++; }
    else printf("PASS %s\n", n);
}

int main(void) {
    dnd_camp_metadata_v1 m;
    dnd_camp_character_v1 ch;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "Curse of Strahd");
    snprintf(m.ruleset, sizeof(m.ruleset), "5e");
    expect("meta ok", dnd_camp_metadata_ok_v1(&m) == DND_CAMP_OK);
    m.house_rule_count = 101;
    expect("meta house bound", dnd_camp_metadata_ok_v1(&m) == DND_CAMP_ERR_VALIDATION);

    expect("scope ok", dnd_camp_knowledge_scope_ok_v1("campaign_canon"));
    expect("scope bad", !dnd_camp_knowledge_scope_ok_v1("wiki"));
    expect("pron ok", dnd_camp_pronunciation_ok_v1("El-MIN-ster"));
    expect("pron bad", !dnd_camp_pronunciation_ok_v1("El@minster"));

    memset(&ch, 0, sizeof(ch));
    snprintf(ch.id, sizeof(ch.id), "char-1");
    snprintf(ch.name, sizeof(ch.name), "Ireena");
    snprintf(ch.kind, sizeof(ch.kind), "npc");
    snprintf(ch.persona, sizeof(ch.persona), "brave");
    snprintf(ch.voice_id, sizeof(ch.voice_id), "voice-1");
    snprintf(ch.speaking_style, sizeof(ch.speaking_style), "clear");
    {
        int i;
        for (i = 0; i < 6; i++) ch.ability[i] = 10;
    }
    ch.n_scopes = 1;
    snprintf(ch.scopes[0], sizeof(ch.scopes[0]), "campaign_canon");
    expect("char ok", dnd_camp_character_ok_v1(&ch) == DND_CAMP_OK);
    expect("scene npc", dnd_camp_scene_npc_ok_v1(&ch) == DND_CAMP_OK);
    ch.ability[0] = 0;
    expect("char ability", dnd_camp_character_ok_v1(&ch) == DND_CAMP_ERR_VALIDATION);
    ch.ability[0] = 10;
    snprintf(ch.kind, sizeof(ch.kind), "player");
    expect("player needs user", dnd_camp_character_ok_v1(&ch) == DND_CAMP_ERR_VALIDATION);
    snprintf(ch.player_user_id, sizeof(ch.player_user_id), "user-1");
    expect("player ok", dnd_camp_character_ok_v1(&ch) == DND_CAMP_OK);

    {
        dnd_camp_session_recap_v1 recs[4];
        dnd_camp_session_recap_v1 arr[DND_CAMP_MAX_RECAPS];
        dnd_camp_session_recap_v1 incoming;
        int n;
        memset(recs, 0, sizeof(recs));
        snprintf(recs[0].session_id, sizeof(recs[0].session_id), "session-1");
        snprintf(recs[0].source_turn_id, sizeof(recs[0].source_turn_id), "turn-1");
        recs[0].summary_rune_count = 10;
        recs[0].created_at_unix_ms = 1000;
        snprintf(recs[1].session_id, sizeof(recs[1].session_id), "session-2");
        snprintf(recs[1].source_turn_id, sizeof(recs[1].source_turn_id), "turn-2");
        recs[1].summary_rune_count = 5;
        recs[1].created_at_unix_ms = 2000;
        expect("recaps ok", dnd_camp_session_recaps_ok_v1(recs, 2) == DND_CAMP_OK);
        recs[1].created_at_unix_ms = 500;
        expect("recaps unsorted", dnd_camp_session_recaps_ok_v1(recs, 2) == DND_CAMP_ERR_VALIDATION);
        recs[1].created_at_unix_ms = 2000;
        snprintf(recs[1].session_id, sizeof(recs[1].session_id), "session-1");
        expect("recaps dup", dnd_camp_session_recaps_ok_v1(recs, 2) == DND_CAMP_ERR_VALIDATION);
        expect("recaps empty ok", dnd_camp_session_recaps_ok_v1(NULL, 0) == DND_CAMP_OK);
        recs[0].summary_rune_count = DND_CAMP_MAX_RECAP_RUNES + 1;
        snprintf(recs[0].session_id, sizeof(recs[0].session_id), "session-1");
        expect("recaps summary bound",
               dnd_camp_session_recaps_ok_v1(recs, 1) == DND_CAMP_ERR_VALIDATION);

        memset(arr, 0, sizeof(arr));
        n = 0;
        {
        int i;
        for (i = 0; i <= DND_CAMP_MAX_RECAPS; i++) {
            memset(&incoming, 0, sizeof(incoming));
            snprintf(incoming.session_id, sizeof(incoming.session_id), "session-%d", i);
            snprintf(incoming.source_turn_id, sizeof(incoming.source_turn_id), "turn-%d", i);
            incoming.summary_rune_count = 3;
            incoming.created_at_unix_ms = 1000 + i;
            n = dnd_camp_upsert_session_recap_v1(arr, n, DND_CAMP_MAX_RECAPS, &incoming);
        }
        }
        expect("upsert cap", n == DND_CAMP_MAX_RECAPS);
        expect("upsert dropped oldest", strcmp(arr[0].session_id, "session-1") == 0);
        expect("upsert last",
               strcmp(arr[n - 1].session_id, "session-24") == 0);
        expect("upsert recaps ok", dnd_camp_session_recaps_ok_v1(arr, n) == DND_CAMP_OK);
    }

    {
        dnd_camp_scene_director_v1 d;
        char known[2][DND_CAMP_ID_CAP];
        memset(&d, 0, sizeof(d));
        snprintf(known[0], sizeof(known[0]), "ireena");
        snprintf(known[1], sizeof(known[1]), "ismark");
        d.n_active = 2;
        snprintf(d.active_npc_ids[0], sizeof(d.active_npc_ids[0]), "ireena");
        snprintf(d.active_npc_ids[1], sizeof(d.active_npc_ids[1]), "ismark");
        d.next_speaker_index = 0;
        expect("director idle ok", dnd_camp_scene_director_ok_v1(&d, known, 2) == DND_CAMP_OK);
        snprintf(d.active_turn_id, sizeof(d.active_turn_id), "turn-1");
        snprintf(d.active_speaker_id, sizeof(d.active_speaker_id), "ireena");
        d.lease_expires_unix_ms = 9999;
        expect("director lease ok", dnd_camp_scene_director_ok_v1(&d, known, 2) == DND_CAMP_OK);
        d.lease_expires_unix_ms = 0;
        expect("director partial lease",
               dnd_camp_scene_director_ok_v1(&d, known, 2) == DND_CAMP_ERR_VALIDATION);
        d.lease_expires_unix_ms = 9999;
        snprintf(d.active_npc_ids[0], sizeof(d.active_npc_ids[0]), "strahd");
        expect("director missing npc",
               dnd_camp_scene_director_ok_v1(&d, known, 2) == DND_CAMP_ERR_VALIDATION);
        d.n_active = 0;
        d.next_speaker_index = 1;
        d.active_turn_id[0] = '\0';
        d.active_speaker_id[0] = '\0';
        d.lease_expires_unix_ms = 0;
        expect("director empty cursor",
               dnd_camp_scene_director_ok_v1(&d, known, 2) == DND_CAMP_ERR_VALIDATION);
    }

    if (failures) return 1;
    printf("ALL PASS c-campaign\n");
    return 0;
}
