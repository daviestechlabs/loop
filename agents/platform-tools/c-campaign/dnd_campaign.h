/* dnd_campaign.h — pure-C campaign registry validation (no heap I/O).
 * Mirrors pure-CPU rules in agents/platform-tools/dnd_campaign.go.
 */
#ifndef PLATFORM_TOOLS_DND_CAMPAIGN_H
#define PLATFORM_TOOLS_DND_CAMPAIGN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DND_CAMP_OK = 0,
    DND_CAMP_ERR_ARGUMENT = 1,
    DND_CAMP_ERR_VALIDATION = 2
};

enum {
    DND_CAMP_ID_CAP = 128,
    DND_CAMP_NAME_CAP = 201,
    DND_CAMP_MAX_SCOPES = 5,
    DND_CAMP_MAX_SAFETY = 20,
    DND_CAMP_SCOPE_CAP = 40
};

/* Safe id: same as initiative — [A-Za-z0-9][A-Za-z0-9._-]{0,63} subset of tool IDs. */
int dnd_camp_id_ok_v1(const char *id);

int dnd_camp_knowledge_scope_ok_v1(const char *scope);
int dnd_camp_pronunciation_ok_v1(const char *value);

typedef struct dnd_camp_metadata_v1 {
    char name[DND_CAMP_NAME_CAP];
    char ruleset[121];
    char description[4001];
    char current_scene[501];
    int house_rule_count; /* validated only as count bound */
} dnd_camp_metadata_v1;

int dnd_camp_metadata_ok_v1(const dnd_camp_metadata_v1 *m);

typedef struct dnd_camp_character_v1 {
    char id[DND_CAMP_ID_CAP];
    char name[DND_CAMP_NAME_CAP];
    char kind[16]; /* player | npc */
    char player_user_id[257];
    char species[121];
    char class_name[121];
    int level;
    int armor_class;
    int max_hp;
    char persona[4001];
    char voice_id[201];
    char speaking_style[501];
    char pronunciation[241];
    int ability[6]; /* cha,con,dex,int,str,wis order */
    int n_scopes;
    char scopes[DND_CAMP_MAX_SCOPES][DND_CAMP_SCOPE_CAP];
    int n_safety;
    char safety[DND_CAMP_MAX_SAFETY][64];
    int sheet_ref_count;
} dnd_camp_character_v1;

int dnd_camp_character_ok_v1(const dnd_camp_character_v1 *c);

/* Scene NPC requires persona, voice_id, speaking_style, and >=1 knowledge scope. */
int dnd_camp_scene_npc_ok_v1(const dnd_camp_character_v1 *c);

enum {
    DND_CAMP_MAX_RECAPS = 24,
    DND_CAMP_MAX_RECAP_RUNES = 1200,
    DND_CAMP_MAX_SCENE_CAST = 20
};

typedef struct dnd_camp_session_recap_v1 {
    char session_id[DND_CAMP_ID_CAP];
    char source_turn_id[DND_CAMP_ID_CAP];
    int summary_rune_count; /* ASCII/rune count bound (host may pass len) */
    int64_t created_at_unix_ms;
} dnd_camp_session_recap_v1;

/* n may be 0. Rejects >MAX, bad ids, empty summary, non-positive created_at,
 * duplicate session_id, or non-monotonic created_at. */
int dnd_camp_session_recaps_ok_v1(const dnd_camp_session_recap_v1 *recs, int n);

/* Upsert by session_id; appends; trims oldest when over MAX. Returns new count. */
int dnd_camp_upsert_session_recap_v1(dnd_camp_session_recap_v1 *arr, int n, int cap,
                                     const dnd_camp_session_recap_v1 *incoming);

typedef struct dnd_camp_scene_director_v1 {
    int n_active;
    char active_npc_ids[DND_CAMP_MAX_SCENE_CAST][DND_CAMP_ID_CAP];
    int next_speaker_index;
    char active_speaker_id[DND_CAMP_ID_CAP];
    char active_turn_id[DND_CAMP_ID_CAP];
    int64_t lease_expires_unix_ms;
} dnd_camp_scene_director_v1;

/* known_ids: character ids that exist and are valid scene NPCs (may be empty). */
int dnd_camp_scene_director_ok_v1(const dnd_camp_scene_director_v1 *d,
                                  const char known_ids[][DND_CAMP_ID_CAP], int n_known);

#ifdef __cplusplus
}
#endif

#endif
