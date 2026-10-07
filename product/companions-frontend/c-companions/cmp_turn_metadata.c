/* cmp_turn_metadata.c — fail-closed browser context for authenticated turns. */
#include "cmp_turn_metadata.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    CMP_META_TOKEN,
    CMP_META_BOOL,
    CMP_META_BUDGET_MS,
    CMP_META_DEADLINE_MS,
    CMP_META_MAX_TOKENS,
    CMP_META_HASH,
    CMP_META_LABEL,
    CMP_META_TURN_KIND,
    CMP_META_PROFILE,
    CMP_META_KNOWLEDGE_SCOPE,
    CMP_META_AUDIO_PROTOCOL,
    CMP_META_AUDIO_KERNEL,
    CMP_META_AUDIO_PACKET_MS
} cmp_meta_kind;

typedef struct {
    const char *key;
    cmp_meta_kind kind;
    size_t max_len;
} cmp_meta_rule;

static const cmp_meta_rule rules[] = {
    {"turn_source", CMP_META_TOKEN, 127u},
    {"turn_kind", CMP_META_TURN_KIND, 8u},
    {"client_trace_id", CMP_META_TOKEN, 127u},
    {"client_transport", CMP_META_TOKEN, 63u},
    {"client_surface", CMP_META_TOKEN, 63u},
    {"product", CMP_META_TOKEN, 63u},
    {"interaction_profile", CMP_META_PROFILE, 31u},
    {"voice_mode", CMP_META_TOKEN, 31u},
    {"turn_profile", CMP_META_TOKEN, 31u},
    {"retrieval_skip", CMP_META_BOOL, 5u},
    {"agent_id", CMP_META_TOKEN, 63u},
    {"task_intent", CMP_META_TOKEN, 63u},
    {"turn_budget_ms", CMP_META_BUDGET_MS, 6u},
    {"turn_deadline_unix_ms", CMP_META_DEADLINE_MS, 19u},
    {"turn_max_tokens", CMP_META_MAX_TOKENS, 7u},
    {"input_mode", CMP_META_TOKEN, 31u},
    {"client_audio_datagram_protocol", CMP_META_AUDIO_PROTOCOL, 5u},
    {"client_audio_kernel", CMP_META_AUDIO_KERNEL, 14u},
    {"client_audio_packet_ms", CMP_META_AUDIO_PACKET_MS, 2u},
    {"capability_id", CMP_META_TOKEN, 127u},
    {"parent_bundle", CMP_META_TOKEN, 127u},
    {"prompt_hash", CMP_META_HASH, 64u},
    {"product_session_id", CMP_META_TOKEN, 127u},
    {"campaign_id", CMP_META_TOKEN, 127u},
    {"scene_id", CMP_META_TOKEN, 64u},
    {"character_id", CMP_META_TOKEN, 127u},
    {"encounter_id", CMP_META_TOKEN, 127u},
    {"requested_npc_id", CMP_META_TOKEN, 127u},
    {"knowledge_scope", CMP_META_KNOWLEDGE_SCOPE, 31u},
    {"dnd_session_recap", CMP_META_BOOL, 5u},
    {"dnd_forget_session_recap", CMP_META_BOOL, 5u},
    {"recap_session_id", CMP_META_TOKEN, 127u},
    {"dnd_cold_open", CMP_META_BOOL, 5u},
    {"evaluation", CMP_META_BOOL, 5u},
    {"retrieval_force", CMP_META_BOOL, 5u},
    {"memory_context_version", CMP_META_TOKEN, 127u},
    {"audio_group_session", CMP_META_BOOL, 5u},
    {"audio_participant_id", CMP_META_TOKEN, 63u},
    {"audio_participant_label", CMP_META_LABEL, 240u},
};

typedef struct {
    const char *name;
    const char *voice_mode;
    const char *turn_profile;
    const char *agent_id;
    const char *task_intent;
    int routing_required;
} cmp_profile_rule;

static const cmp_profile_rule profiles[] = {
    {"realtime_voice", "realtime", "realtime", NULL, NULL, 0},
    {"detail_voice", "detail", "detail", NULL, NULL, 0},
    {"text_chat", "text", "text", NULL, NULL, 0},
    {"dnd_app", "app", "dnd_app", "dnd-agent", "dnd_action", 0},
    {"khelben_image", "app", "khelben_image", "khelben-image", "generate_dnd_map", 1},
    {"khelben_session_image", "app", "khelben_session_image", "khelben-image",
     "generate_session_illustration", 1},
    {"khelben_video", "app", "khelben_video", "khelben-video", "generate_dnd_cinematic", 1},
};

static const cmp_profile_rule *find_profile(const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (strcmp(name, profiles[i].name) == 0) return &profiles[i];
    return NULL;
}

static int span_equal(const char *span, size_t span_len, const char *text) {
    size_t text_len = text ? strlen(text) : 0u;
    return span && text && span_len == text_len && memcmp(span, text, span_len) == 0;
}

static const cmp_meta_rule *find_rule(const char *key, size_t key_len) {
    size_t i;
    for (i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
        if (span_equal(key, key_len, rules[i].key)) return &rules[i];
    return NULL;
}

static int token_valid(const char *value, size_t max_len) {
    const unsigned char *p = (const unsigned char *)value;
    size_t len = value ? strlen(value) : 0u;
    if (len == 0u || len > max_len) return 0;
    if (!isalnum(*p)) return 0;
    while (*p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == ':'))
            return 0;
        ++p;
    }
    return 1;
}

static int uint_valid(const char *value, size_t max_len, uint64_t maximum) {
    size_t i;
    size_t len = value ? strlen(value) : 0u;
    uint64_t number = 0u;
    if (len == 0u || len > max_len || (len > 1u && value[0] == '0')) return 0;
    for (i = 0; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9') return 0;
        if (number > (maximum - (uint64_t)(value[i] - '0')) / 10u) return 0;
        number = number * 10u + (uint64_t)(value[i] - '0');
    }
    return number > 0u && number <= maximum;
}

static int label_valid(const char *value, size_t max_len) {
    const unsigned char *p = (const unsigned char *)value;
    size_t codepoints = 0u;
    size_t len = value ? strlen(value) : 0u;
    if (len == 0u || len > max_len || isspace(p[0]) || isspace(p[len - 1u])) return 0;
    while (*p) {
        if (*p < 0x20u || *p == 0x7fu) return 0;
        if ((*p & 0xc0u) != 0x80u) codepoints++;
        ++p;
    }
    return codepoints <= 80u;
}

static int value_in(const char *value, const char *const *allowed, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (strcmp(value, allowed[i]) == 0) return 1;
    return 0;
}

static int value_valid(const cmp_meta_rule *rule, const char *value) {
    static const char *const turn_kinds[] = {"voice", "chat"};
    static const char *const scopes[] = {
        "campaign_canon", "shared_rulebook", "owned_rulebook",
        "session_transcript", "character_memory"
    };
    size_t i;
    if (!rule || !value || strlen(value) > rule->max_len) return 0;
    switch (rule->kind) {
    case CMP_META_TOKEN:
        return token_valid(value, rule->max_len);
    case CMP_META_BOOL:
        return strcmp(value, "true") == 0 || strcmp(value, "false") == 0;
    case CMP_META_BUDGET_MS:
        return uint_valid(value, rule->max_len, UINT64_C(600000));
    case CMP_META_DEADLINE_MS:
        return uint_valid(value, rule->max_len, INT64_MAX);
    case CMP_META_MAX_TOKENS:
        return uint_valid(value, rule->max_len, UINT64_C(1000000));
    case CMP_META_HASH:
        if (strlen(value) != 64u) return 0;
        for (i = 0; i < 64u; ++i)
            if (!((value[i] >= '0' && value[i] <= '9') ||
                  (value[i] >= 'a' && value[i] <= 'f'))) return 0;
        return 1;
    case CMP_META_LABEL:
        return label_valid(value, rule->max_len);
    case CMP_META_TURN_KIND:
        return value_in(value, turn_kinds, sizeof(turn_kinds) / sizeof(turn_kinds[0]));
    case CMP_META_PROFILE:
        return find_profile(value) != NULL;
    case CMP_META_KNOWLEDGE_SCOPE:
        return value_in(value, scopes, sizeof(scopes) / sizeof(scopes[0]));
    case CMP_META_AUDIO_PROTOCOL:
        return strcmp(value, "dtvp1") == 0;
    case CMP_META_AUDIO_KERNEL:
        return strcmp(value, "wasm-simd") == 0 || strcmp(value, "scalar") == 0 ||
            strcmp(value, "recorded_pcm16") == 0 || strcmp(value, "live_c_worklet") == 0;
    case CMP_META_AUDIO_PACKET_MS:
        return strcmp(value, "20") == 0;
    }
    return 0;
}

static const char *meta_value(const pb_turn_start_req *request, const char *key) {
    int i;
    if (!request || !key) return NULL;
    for (i = 0; i < request->n_meta; ++i)
        if (strcmp(request->meta[i].key, key) == 0) return request->meta[i].val;
    return NULL;
}

static int profile_field_matches(const char *value, const char *expected, int required) {
    if (!expected) return !value;
    return value ? strcmp(value, expected) == 0 : !required;
}

static int profile_consistent(const pb_turn_start_req *request) {
    const char *profile = meta_value(request, "interaction_profile");
    const cmp_profile_rule *rule = find_profile(profile);
    const char *voice_mode = meta_value(request, "voice_mode");
    const char *turn_profile = meta_value(request, "turn_profile");
    const char *agent_id = meta_value(request, "agent_id");
    const char *task_intent = meta_value(request, "task_intent");
    const char *scope = meta_value(request, "knowledge_scope");
    const char *force = meta_value(request, "retrieval_force");
    const char *campaign = meta_value(request, "campaign_id");
    const char *scene = meta_value(request, "scene_id");
    const char *character = meta_value(request, "character_id");
    const char *dnd_recap = meta_value(request, "dnd_session_recap");
    const char *dnd_forget = meta_value(request, "dnd_forget_session_recap");
    const char *dnd_cold_open = meta_value(request, "dnd_cold_open");
    const char *recap_session = meta_value(request, "recap_session_id");
    const char *encounter = meta_value(request, "encounter_id");
    const char *requested_npc = meta_value(request, "requested_npc_id");
    const char *group = meta_value(request, "audio_group_session");
    const char *participant_id = meta_value(request, "audio_participant_id");
    const char *participant_label = meta_value(request, "audio_participant_label");
    int is_dnd = profile && strcmp(profile, "dnd_app") == 0;
    int has_table = scope || campaign || scene || character || encounter || requested_npc || dnd_recap || dnd_forget ||
        dnd_cold_open || recap_session;
    if ((scene || character || encounter || requested_npc || scope || dnd_recap || dnd_forget || dnd_cold_open ||
         recap_session) && !campaign) return 0;
    if (scope && strcmp(scope, "character_memory") == 0 && !character) return 0;
    if ((dnd_forget && strcmp(dnd_forget, "true") == 0) != (recap_session != NULL)) return 0;
    if (dnd_recap && strcmp(dnd_recap, "true") == 0 && dnd_forget &&
        strcmp(dnd_forget, "true") == 0) return 0;
    if (group && strcmp(group, "true") == 0) {
        if (!participant_id || !participant_label) return 0;
    } else if (participant_id || participant_label) {
        return 0;
    }
    if (!profile)
        return !has_table && !force && !agent_id && !task_intent &&
            !voice_mode && !turn_profile;
    if (!rule) return 0;
    if (force) {
        if (!is_dnd) return 0;
        if (strcmp(force, "true") == 0 &&
            (!scope || (strcmp(scope, "shared_rulebook") != 0 &&
                        strcmp(scope, "owned_rulebook") != 0))) return 0;
    }
    if (has_table && !is_dnd && strcmp(profile, "realtime_voice") != 0) return 0;
    return profile_field_matches(voice_mode, rule->voice_mode, rule->routing_required) &&
        profile_field_matches(turn_profile, rule->turn_profile, rule->routing_required) &&
        profile_field_matches(agent_id, rule->agent_id, rule->routing_required) &&
        profile_field_matches(task_intent, rule->task_intent, rule->routing_required);
}

static int initiative_parse(const cmp_json_object *body, pb_turn_start_req *request) {
    cmp_json_object object, selection;
    cmp_json_array array;
    cmp_json_field item;
    dnd_initiative_request_c *out = &request->dnd_initiative;
    int count = cmp_json_object_key_count(body, "dnd_initiative"), rc;
    const char *profile = meta_value(request, "interaction_profile");
    const char *campaign = meta_value(request, "campaign_id");
    const char *encounter = meta_value(request, "encounter_id");
    out->count = 0;
    if (!count) return 0;
    if (count != 1 || !profile || strcmp(profile, "dnd_app") || !campaign || !encounter ||
        !dnd_request_id_valid(campaign, 128u) || !dnd_request_id_valid(encounter, 128u) ||
        !cmp_json_object_object(body, "dnd_initiative", &object) || object.field_count != 4u ||
        !cmp_json_object_str(&object, "operation_id", out->operation_id, sizeof(out->operation_id)) ||
        !cmp_json_object_i64(&object, "campaign_version", &out->campaign_version) ||
        !cmp_json_object_i64(&object, "expected_version", &out->expected_version) ||
        !cmp_json_field_array(cmp_json_object_field(&object, "selections"), &array)) return -1;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        dnd_initiative_selection_c *s;
        if (out->count >= DND_INITIATIVE_SELECTIONS_MAX ||
            !cmp_json_field_object(&item, &selection) || selection.field_count != 2u) return -1;
        s = &out->selections[out->count++];
        if (!cmp_json_object_str(&selection, "character_id", s->character_id, sizeof(s->character_id)) ||
            !cmp_json_object_str(&selection, "expression", s->expression, sizeof(s->expression))) return -1;
    }
    return rc == 0 && dnd_initiative_request_valid(out) ? 0 : -1;
}

static int campaign_parse(const cmp_json_object *body, pb_turn_start_req *request) {
    static const char *const abilities[] = {"strength", "dexterity", "constitution", "intelligence", "wisdom", "charisma"};
    cmp_json_object object, data, scores;
    dnd_campaign_request_c *out = &request->dnd_campaign;
    char operation[32], kind[16];
    int count = cmp_json_object_key_count(body, "dnd_campaign");
    const char *profile = meta_value(request, "interaction_profile"), *campaign = meta_value(request, "campaign_id");
    out->operation = 0;
    if (!count) return 0;
    if (count != 1 || request->dnd_initiative.count || !profile || strcmp(profile, "dnd_app") || !campaign ||
        !dnd_request_id_valid(campaign, 128u) ||
        !cmp_json_object_object(body, "dnd_campaign", &object) || object.field_count != 4u ||
        !cmp_json_object_str(&object, "operation", operation, sizeof(operation)) ||
        !cmp_json_object_str(&object, "operation_id", out->operation_id, sizeof(out->operation_id)) ||
        !cmp_json_object_i64(&object, "expected_version", &out->expected_version)) return -1;
    if (!strcmp(operation, "create")) {
        out->operation = DND_CAMPAIGN_CREATE;
        if (!cmp_json_object_object(&object, "campaign", &data) || data.field_count != 2u ||
            !cmp_json_object_str(&data, "name", out->data.campaign.name, sizeof(out->data.campaign.name)) ||
            !cmp_json_object_str(&data, "ruleset", out->data.campaign.ruleset, sizeof(out->data.campaign.ruleset))) return -1;
    } else if (!strcmp(operation, "add_character")) {
        dnd_character_create_c *c = &out->data.character;
        int64_t value;
        out->operation = DND_CAMPAIGN_ADD_CHARACTER;
        if (!cmp_json_object_object(&object, "character", &data) || data.field_count != 9u ||
            !cmp_json_object_str(&data, "id", c->id, sizeof(c->id)) ||
            !cmp_json_object_str(&data, "name", c->name, sizeof(c->name)) ||
            !cmp_json_object_str(&data, "kind", kind, sizeof(kind)) ||
            !cmp_json_object_str(&data, "species", c->species, sizeof(c->species)) ||
            !cmp_json_object_str(&data, "class_name", c->class_name, sizeof(c->class_name)) ||
            !cmp_json_object_object(&data, "ability_scores", &scores) || scores.field_count != 6u) return -1;
        c->kind = !strcmp(kind, "player") ? 1u : !strcmp(kind, "npc") ? 2u : 0u;
#define NUMBER(key, target, maximum) do { \
    if (!cmp_json_object_i64(&data, key, &value) || value < 0 || value > maximum) return -1; \
    target = (uint32_t)value; } while (0)
        NUMBER("level", c->level, 20);
        NUMBER("armor_class", c->armor_class, 100);
        NUMBER("max_hp", c->max_hp, 100000);
#undef NUMBER
        for (size_t i = 0; i < 6u; ++i) {
            if (!cmp_json_object_i64(&scores, abilities[i], &value) || value < 1 || value > 30) return -1;
            c->abilities[i] = (uint32_t)value;
        }
    } else return -1;
    return dnd_campaign_request_valid(out) ? 0 : -1;
}

static int encounter_action_parse(const cmp_json_object *body, pb_turn_start_req *request) {
    cmp_json_object object;
    dnd_encounter_action_c *out = &request->dnd_encounter_action;
    char operation[32], label[32];
    const char *profile = meta_value(request, "interaction_profile"), *campaign = meta_value(request, "campaign_id");
    const char *encounter = meta_value(request, "encounter_id");
    int count = cmp_json_object_key_count(body, "dnd_encounter_action");
    if (!count) return 0;
    if (count != 1 || request->dnd_initiative.count || request->dnd_campaign.operation ||
        !profile || strcmp(profile, "dnd_app") || !campaign || !encounter ||
        !dnd_request_id_valid(campaign, 128u) || !dnd_request_id_valid(encounter, 128u) ||
        !cmp_json_object_object(body, "dnd_encounter_action", &object) ||
        !cmp_json_object_str(&object, "operation", operation, sizeof(operation)) ||
        !cmp_json_object_str(&object, "operation_id", out->operation_id, sizeof(out->operation_id)) ||
        !cmp_json_object_i64(&object, "expected_version", &out->expected_version)) return -1;
    for (uint32_t i = 1; i <= DND_ACTION_END; ++i) if (!strcmp(operation, dnd_action_name(i))) out->operation = i;
    if (out->operation == DND_ACTION_ADVANCE || out->operation == DND_ACTION_END) {
        if (object.field_count != 3u) return -1;
    } else {
        if (!cmp_json_object_str(&object, "participant_id", out->participant_id, sizeof(out->participant_id))) return -1;
        if (out->operation == DND_ACTION_DAMAGE || out->operation == DND_ACTION_HEAL) {
            int64_t amount;
            if (object.field_count != (out->operation == DND_ACTION_DAMAGE ? 6u : 5u) ||
                !cmp_json_object_i64(&object, "amount", &amount) || amount < 1 || amount > 100000) return -1;
            out->amount = (uint32_t)amount;
            if (out->operation == DND_ACTION_DAMAGE) {
                if (!cmp_json_object_str(&object, "damage_type", label, sizeof(label))) return -1;
                out->damage_type = dnd_damage_number(label);
                if (label[0] && !out->damage_type) return -1;
            }
        } else {
            if (object.field_count != 5u || !cmp_json_object_str(&object, "condition", label, sizeof(label))) return -1;
            out->condition = dnd_condition_number(label);
        }
    }
    return dnd_encounter_action_valid(out) ? 0 : -1;
}

int cmp_turn_metadata_parse(
    const cmp_json_object *body,
    pb_turn_start_req *request,
    cmp_turn_metadata_policy *policy
) {
    cmp_json_object metadata;
    char top_transport[64];
    int metadata_count;
    int transport_count;
    size_t i;
    if (!body || !request || !policy) return -1;
    memset(policy, 0, sizeof(*policy));
    request->n_meta = 0;
    memset(&request->dnd_initiative, 0, sizeof(request->dnd_initiative));
    memset(&request->dnd_campaign, 0, sizeof(request->dnd_campaign));
    memset(&request->dnd_encounter_action, 0, sizeof(request->dnd_encounter_action));
    metadata_count = cmp_json_object_key_count(body, "metadata");
    transport_count = cmp_json_object_key_count(body, "client_transport");
    if (metadata_count < 0 || metadata_count > 1 ||
        transport_count < 0 || transport_count > 1) return -1;
    top_transport[0] = '\0';
    if (transport_count == 1 &&
        (!cmp_json_object_str(body, "client_transport", top_transport,
                              sizeof(top_transport)) ||
         !token_valid(top_transport, sizeof(top_transport) - 1u))) return -1;
    if (metadata_count == 1) {
        if (!cmp_json_object_object(body, "metadata", &metadata) ||
            metadata.field_count > PB_TURN_META_PAIRS) return -1;
        for (i = 0; i < metadata.field_count; ++i) {
            const cmp_json_field *field = &metadata.fields[i];
            const cmp_meta_rule *rule = find_rule(field->key, field->key_len);
            pb_meta_pair *pair;
            int prior;
            if (!rule || request->n_meta >= PB_TURN_META_PAIRS) return -1;
            pair = &request->meta[request->n_meta];
            if (field->key_len >= sizeof(pair->key) ||
                !cmp_json_field_str(field, pair->val, sizeof(pair->val)) ||
                !value_valid(rule, pair->val)) return -1;
            memcpy(pair->key, field->key, field->key_len);
            pair->key[field->key_len] = '\0';
            for (prior = 0; prior < request->n_meta; ++prior)
                if (strcmp(request->meta[prior].key, pair->key) == 0) return -1;
            request->n_meta++;
        }
    }
    {
        const char *nested_transport = meta_value(request, "client_transport");
        if (nested_transport && top_transport[0] &&
            strcmp(nested_transport, top_transport) != 0) return -1;
        if (!nested_transport && top_transport[0]) {
            pb_meta_pair *pair;
            if (request->n_meta >= PB_TURN_META_PAIRS) return -1;
            pair = &request->meta[request->n_meta++];
            memcpy(pair->key, "client_transport", sizeof("client_transport"));
            memcpy(pair->val, top_transport, strlen(top_transport) + 1u);
        }
    }
    if (!profile_consistent(request) || initiative_parse(body, request) != 0 || campaign_parse(body, request) != 0 ||
        encounter_action_parse(body, request) != 0) return -1;
    {
        const char *force = meta_value(request, "retrieval_force");
        policy->retrieval_force = force && strcmp(force, "true") == 0;
    }
    return 0;
}

int cmp_turn_metadata_visit(const cmp_json_object *body, cmp_turn_metadata_writer writer,
                            void *user, cmp_turn_metadata_policy *policy,
                            dnd_initiative_request_c *initiative, dnd_campaign_request_c *campaign,
                            dnd_encounter_action_c *action) {
    pb_turn_start_req request;
    int i;
    if (!writer || cmp_turn_metadata_parse(body, &request, policy) != 0) return -1;
    for (i = 0; i < request.n_meta; ++i)
        if (writer(request.meta[i].key, request.meta[i].val, user) != 0) return -1;
    if (initiative) *initiative = request.dnd_initiative;
    if (campaign) *campaign = request.dnd_campaign;
    if (action) *action = request.dnd_encounter_action;
    return 0;
}
