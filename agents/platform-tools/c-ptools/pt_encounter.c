/* Strict encounter commands and retained state around the existing C kernel. */
#define _POSIX_C_SOURCE 200809L
#include "pt_encounter.h"
#include "pt_campaign.h"
#include "../c-dice/dice.h"
#include "../c-dnd/dnd_initiative.h"
#include "../c-toolstore/toolstore.h"
#include "cmp_json.h"
#include "utf8.h"
#include "pb_min.h"
#include <openssl/sha.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { START, GET, ADD, REMOVE, ADVANCE, END, DAMAGE, HEAL, CONDITION_ADD, CONDITION_REMOVE, ROLL_INITIATIVE };
static const char *const operations[] = {
    "start", "get", "add", "remove", "advance", "end", "damage", "heal",
    "condition_add", "condition_remove", "roll_initiative"
};
typedef struct {
    unsigned operation;
    char campaign[DND_ID_CAP], encounter[DND_ID_CAP], participant_id[DND_ID_CAP];
    char condition[32], damage_type[32];
    int64_t expected;
    int amount;
    int include_previous;
    dnd_participant_v1 participant;
    int64_t campaign_version;
    cmp_json_field initiative;
} encounter_command;

typedef struct {
    char ids[DND_TURN_PARTICIPANTS_MAX][DND_ID_CAP];
    dice_spec specs[DND_TURN_PARTICIPANTS_MAX];
    size_t count;
} initiative_requests;

/* Unknown, escaped, and duplicate keys fail. */
static int fields(const cmp_json_object *o, const char *const *names, size_t count,
                  unsigned allowed, unsigned required) {
    unsigned seen = 0;
    for (size_t i = 0; i < o->field_count; ++i) {
        const cmp_json_field *f = &o->fields[i];
        size_t j;
        if (f->key_escaped) return 0;
        for (j = 0; j < count; ++j)
            if (strlen(names[j]) == f->key_len && !memcmp(names[j], f->key, f->key_len)) break;
        if (j == count || j >= 32u || !(allowed & (1u << j)) || (seen & (1u << j))) return 0;
        seen |= 1u << j;
    }
    return (seen & required) == required;
}

static int integer(const cmp_json_object *o, const char *key, int low, int high, int *out) {
    int64_t value;
    if (!cmp_json_object_i64(o, key, &value) || value < low || value > high) return 0;
    *out = (int)value;
    return 1;
}

static int participant_decode(const cmp_json_object *o, dnd_participant_v1 *p, int retained) {
    static const char *const names[] = {"id", "name", "initiative", "max_hp", "current_hp", "conditions"};
    cmp_json_array array;
    cmp_json_field value;
    int rc;
    memset(p, 0, sizeof(*p));
    if (!fields(o, names, 6u, 63u, retained ? 63u : 15u) ||
        !cmp_json_object_str(o, "id", p->id, sizeof(p->id)) ||
        !cmp_json_object_str(o, "name", p->name, sizeof(p->name)) ||
        !integer(o, "initiative", -100, 100, &p->initiative) ||
        !integer(o, "max_hp", 0, 100000, &p->max_hp)) return 0;
    p->current_hp = p->max_hp;
    if (cmp_json_object_key_count(o, "current_hp") &&
        !integer(o, "current_hp", 0, p->max_hp, &p->current_hp)) return 0;
    if (cmp_json_object_key_count(o, "conditions")) {
        if (!cmp_json_field_array(cmp_json_object_field(o, "conditions"), &array)) return 0;
        while ((rc = cmp_json_array_next(&array, &value)) == 1) {
            if (p->n_conditions >= DND_MAX_CONDITIONS ||
                !cmp_json_field_str(&value, p->conditions[p->n_conditions], sizeof(p->conditions[0]))) return 0;
            ++p->n_conditions;
        }
        if (rc != 0) return 0;
    }
    return dnd_participant_ok_v1(p);
}

static int initiative_requests_decode(const cmp_json_field *field, initiative_requests *requests) {
    static const char *const names[] = {"character_id", "expression"};
    cmp_json_array array;
    cmp_json_field item;
    cmp_json_object object;
    char expression[DICE_EXPR], canonical[DICE_EXPR];
    int rc;
    memset(requests, 0, sizeof(*requests));
    if (!cmp_json_field_array(field, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        size_t i = requests->count;
        dice_spec *spec;
        if (i >= DND_TURN_PARTICIPANTS_MAX || !cmp_json_field_object(&item, &object) ||
            !fields(&object, names, 2u, 3u, 3u) ||
            !cmp_json_object_str(&object, "character_id", requests->ids[i], sizeof(requests->ids[i])) ||
            !dnd_id_ok_v1(requests->ids[i]) ||
            !cmp_json_object_str(&object, "expression", expression, sizeof(expression))) return 0;
        for (size_t j = 0; j < i; ++j) if (!strcmp(requests->ids[i], requests->ids[j])) return 0;
        spec = &requests->specs[i];
        if (dice_parse(expression, spec, NULL, 0) != DICE_OK ||
            spec->sides != 20 || spec->modifier < -101 || spec->modifier > 80 ||
            !((spec->count == 1 && !spec->keep[0]) ||
              (spec->count == 2 && (!strcmp(spec->keep, "kh1") || !strcmp(spec->keep, "kl1"))))) return 0;
        if (spec->modifier)
            (void)snprintf(canonical, sizeof(canonical), "%dd20%s%+d", spec->count, spec->keep, spec->modifier);
        else (void)snprintf(canonical, sizeof(canonical), "%dd20%s", spec->count, spec->keep);
        if (strcmp(expression, canonical)) return 0;
        ++requests->count;
    }
    return rc == 0 && requests->count > 0;
}

static int command_decode(const char *json, encounter_command *c) {
    static const char *const names[] = {
        "operation", "campaign_id", "encounter_id", "expected_version", "participant",
        "participant_id", "amount", "damage_type", "condition", "campaign_version", "initiative", "include_previous"
    };
    enum { BASE = 7u, VERSION = 8u, PARTICIPANT = 16u, TARGET = 32u,
           AMOUNT = 64u, DAMAGE_TYPE = 128u, CONDITION = 256u, CAMPAIGN_VERSION = 512u, INITIATIVE = 1024u,
           PREVIOUS = 2048u };
    /* Each operation admits only fields it consumes. */
    static const unsigned allowed[] = {
        BASE | VERSION, BASE, BASE | VERSION | PARTICIPANT, BASE | VERSION | TARGET,
        BASE | VERSION, BASE | VERSION, BASE | VERSION | TARGET | AMOUNT | DAMAGE_TYPE,
        BASE | VERSION | TARGET | AMOUNT, BASE | VERSION | TARGET | CONDITION,
        BASE | VERSION | TARGET | CONDITION, BASE | VERSION | CAMPAIGN_VERSION | INITIATIVE
    };
    cmp_json_object o, part;
    char op[32];
    unsigned required, admitted;
    memset(c, 0, sizeof(*c));
    if (!json || strnlen(json, PT_INPUT) >= PT_INPUT || !cmp_json_object_parse(json, &o) ||
        !cmp_json_object_str(&o, "operation", op, sizeof(op))) return 0;
    for (char *p = op; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    for (c->operation = 0; c->operation < sizeof(operations) / sizeof(operations[0]); ++c->operation)
        if (!strcmp(op, operations[c->operation])) break;
    if (c->operation == sizeof(operations) / sizeof(operations[0])) return 0;
    required = allowed[c->operation];
    admitted = required;
    if (c->operation == ADVANCE || c->operation == END || c->operation == DAMAGE ||
        c->operation == HEAL || c->operation == CONDITION_ADD || c->operation == CONDITION_REMOVE) admitted |= PREVIOUS;
    if (c->operation == DAMAGE) required &= ~(unsigned)DAMAGE_TYPE;
    if (!fields(&o, names, 12u, admitted, required) ||
        !cmp_json_object_str(&o, "campaign_id", c->campaign, sizeof(c->campaign)) ||
        !cmp_json_object_str(&o, "encounter_id", c->encounter, sizeof(c->encounter)) ||
        !dnd_id_ok_v1(c->campaign) || !dnd_id_ok_v1(c->encounter)) return 0;
    if (c->operation != GET && (!cmp_json_object_i64(&o, "expected_version", &c->expected) ||
        (c->operation == START ? c->expected != 0 : c->expected < (c->operation == ROLL_INITIATIVE ? 0 : 1)))) return 0;
    if (c->operation == ROLL_INITIATIVE) {
        initiative_requests *requests = calloc(1, sizeof(*requests));
        const cmp_json_field *field = cmp_json_object_field(&o, "initiative");
        int valid = requests && cmp_json_object_i64(&o, "campaign_version", &c->campaign_version) &&
            c->campaign_version > 0 && initiative_requests_decode(field, requests);
        free(requests);
        if (!valid) return 0;
        c->initiative = *field;
    }
    if ((allowed[c->operation] & TARGET) &&
        (!cmp_json_object_str(&o, "participant_id", c->participant_id, sizeof(c->participant_id)) ||
         !dnd_id_ok_v1(c->participant_id))) return 0;
    if ((allowed[c->operation] & AMOUNT) && !integer(&o, "amount", 1, INT_MAX, &c->amount)) return 0;
    if (cmp_json_object_key_count(&o, "damage_type") &&
        (!cmp_json_object_str(&o, "damage_type", c->damage_type, sizeof(c->damage_type)) ||
         !dnd_damage_type_ok_v1(c->damage_type))) return 0;
    if ((allowed[c->operation] & CONDITION) &&
        (!cmp_json_object_str(&o, "condition", c->condition, sizeof(c->condition)) ||
         !dnd_condition_ok_v1(c->condition))) return 0;
    if (cmp_json_object_key_count(&o, "include_previous") &&
        (!cmp_json_object_bool(&o, "include_previous", &c->include_previous) || !c->include_previous)) return 0;
    return c->operation != ADD || (cmp_json_object_object(&o, "participant", &part) &&
                                   participant_decode(&part, &c->participant, 0));
}

static int owner_ok(const char *owner) {
    size_t n = strnlen(owner, DND_ID_CAP);
    if (!n || n >= DND_ID_CAP || owner[0] == ' ' || owner[n - 1u] == ' ' ||
        !utf8_validate_v1((const uint8_t *)owner, n)) return 0;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)owner[i] < 32u || owner[i] == 127) return 0;
    return 1;
}

static int state_decode(const char *json, dnd_encounter_v1 *e) {
    static const char *const names[] = {"campaign_id", "encounter_id", "owner_user_id", "version",
                                        "status", "round", "active_index", "participants"};
    cmp_json_object o, participant;
    cmp_json_array array;
    cmp_json_field value;
    int rc;
    memset(e, 0, sizeof(*e));
    if (!json || strnlen(json, PT_OUT) >= PT_OUT || !cmp_json_object_parse(json, &o) ||
        !fields(&o, names, 8u, 255u, 255u) ||
        !cmp_json_object_str(&o, "campaign_id", e->campaign_id, sizeof(e->campaign_id)) ||
        !cmp_json_object_str(&o, "encounter_id", e->encounter_id, sizeof(e->encounter_id)) ||
        !cmp_json_object_str(&o, "owner_user_id", e->owner_user_id, sizeof(e->owner_user_id)) ||
        !owner_ok(e->owner_user_id) || !cmp_json_object_i64(&o, "version", &e->version) ||
        !cmp_json_object_str(&o, "status", e->status, sizeof(e->status)) ||
        !integer(&o, "round", 1, INT_MAX, &e->round) ||
        !integer(&o, "active_index", -1, DND_MAX_PARTICIPANTS - 1, &e->active_index) ||
        !cmp_json_field_array(cmp_json_object_field(&o, "participants"), &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &value)) == 1) {
        if (e->n_participants >= DND_MAX_PARTICIPANTS || !cmp_json_field_object(&value, &participant) ||
            !participant_decode(&participant, &e->participants[e->n_participants], 1)) return 0;
        ++e->n_participants;
    }
    return rc == 0 && dnd_encounter_validate_v1(e) == DND_OK;
}

typedef struct { char *out; size_t cap, used; int failed; } json_writer;
static void append(json_writer *w, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void append(json_writer *w, const char *format, ...) {
    va_list args;
    int n;
    if (w->failed) return;
    va_start(args, format);
    n = vsnprintf(w->out + w->used, w->cap - w->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->cap - w->used) { w->failed = 1; return; }
    w->used += (size_t)n;
}

static void string(json_writer *w, const char *value) {
    char escaped[6u * PT_ID + 1u];
    if (cmp_json_escape_exact(value, escaped, sizeof(escaped))) { w->failed = 1; return; }
    append(w, "\"%s\"", escaped);
}

static void state_write(json_writer *w, const dnd_encounter_v1 *e, int owner) {
    append(w, "\"campaign_id\":"); string(w, e->campaign_id);
    append(w, ",\"encounter_id\":"); string(w, e->encounter_id);
    if (owner) { append(w, ",\"owner_user_id\":"); string(w, e->owner_user_id); }
    append(w, ",\"version\":%lld,\"status\":", (long long)e->version); string(w, e->status);
    append(w, ",\"round\":%d,\"active_index\":%d,\"participants\":[", e->round, e->active_index);
    for (int i = 0; i < e->n_participants; ++i) {
        const dnd_participant_v1 *p = &e->participants[i];
        append(w, "%s{\"id\":", i ? "," : ""); string(w, p->id);
        append(w, ",\"name\":"); string(w, p->name);
        append(w, ",\"initiative\":%d,\"max_hp\":%d,\"current_hp\":%d,\"conditions\":[",
               p->initiative, p->max_hp, p->current_hp);
        for (int j = 0; j < p->n_conditions; ++j) {
            append(w, "%s", j ? "," : ""); string(w, p->conditions[j]);
        }
        append(w, "]}");
    }
    append(w, "]");
}

/* Both serializers clear incomplete output; callers persist only after encoding. */
static size_t state_encode(char *out, size_t cap, const dnd_encounter_v1 *e) {
    json_writer w = {out, cap, 0, cap == 0};
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (dnd_encounter_validate_v1(e) != DND_OK || !owner_ok(e->owner_user_id)) return 0;
    append(&w, "{"); state_write(&w, e, 1); append(&w, "}");
    if (w.failed) { out[0] = '\0'; return 0; }
    return w.used;
}

static int output_encode(const dnd_encounter_v1 *e, const encounter_command *c,
                         const dnd_encounter_v1 *before, pt_call *call) {
    json_writer w = {call->output_json, sizeof(call->output_json), 0, 0};
    char active[DND_ID_CAP];
    if (dnd_encounter_active_id_v1(e, active, sizeof(active)) != DND_OK) return 0;
    append(&w, "{\"operation\":"); string(&w, operations[c->operation]);
    append(&w, ",\"operation_id\":"); string(&w, call->tool_call_id);
    append(&w, ","); state_write(&w, e, 0);
    append(&w, ",\"active_participant_id\":"); string(&w, active);
    if (c->participant_id[0]) { append(&w, ",\"participant_id\":"); string(&w, c->participant_id); }
    if (c->amount) append(&w, ",\"amount\":%d", c->amount);
    if (c->damage_type[0]) { append(&w, ",\"damage_type\":"); string(&w, c->damage_type); }
    if (c->condition[0]) { append(&w, ",\"condition\":"); string(&w, c->condition); }
    /* Opt-in preserves replay of existing journal records byte for byte. */
    if (c->include_previous) {
        if (!before) return 0;
        append(&w, ",\"previous_state\":{"); state_write(&w, before, 0); append(&w, "}");
    }
    append(&w, "}");
    if (w.failed) { call->output_json[0] = '\0'; return 0; }
    return 1;
}

/* Pure transition: no persistence or artifact operation occurs here. */
static int transition(const encounter_command *command, const dnd_encounter_v1 *before,
                      dnd_encounter_v1 *after, pt_call *call) {
    encounter_command c = *command;
    const char *error = "invalid encounter command";
    int rc = DND_ERR_ARGUMENT;
    call->output_json[0] = call->summary[0] = call->error[0] = '\0';
    if (!owner_ok(call->user_id) ||
        strnlen(call->tool_call_id, sizeof(call->tool_call_id)) >= sizeof(call->tool_call_id) ||
        !ts_valid_id(call->tool_call_id)) goto fail;
    if (c.operation == START) {
        if (before) goto fail;
        rc = dnd_encounter_start_v1(after, c.campaign, c.encounter, call->user_id);
    } else {
        error = "encounter state is unavailable or invalid";
        if (!before || dnd_encounter_validate_v1(before) != DND_OK ||
            strcmp(before->campaign_id, c.campaign) || strcmp(before->encounter_id, c.encounter)) goto fail;
        error = "encounter belongs to a different governed user";
        if (strcmp(before->owner_user_id, call->user_id)) goto fail;
        if (c.include_previous && strcmp(before->status, "active")) goto fail;
        *after = *before;
        if (c.operation == GET) rc = DND_OK;
        else {
            error = "encounter version conflict or counter exhausted";
            if (c.expected != before->version || before->version == INT64_MAX ||
                (c.operation == ADVANCE && before->round == INT_MAX &&
                 before->active_index == before->n_participants - 1)) goto fail;
            switch (c.operation) {
            case ADD:
                rc = dnd_encounter_add_v1(after, &c.participant);
                memcpy(c.participant_id, c.participant.id, sizeof(c.participant_id));
                break;
            case REMOVE: rc = dnd_encounter_remove_v1(after, c.participant_id); break;
            case ADVANCE: rc = dnd_encounter_advance_v1(after); break;
            case END: rc = dnd_encounter_end_v1(after); break;
            case DAMAGE: rc = dnd_encounter_damage_v1(after, c.participant_id, c.amount); break;
            case HEAL: rc = dnd_encounter_heal_v1(after, c.participant_id, c.amount); break;
            case CONDITION_ADD: rc = dnd_encounter_condition_add_v1(after, c.participant_id, c.condition); break;
            case CONDITION_REMOVE: rc = dnd_encounter_condition_remove_v1(after, c.participant_id, c.condition); break;
            default: break;
            }
        }
    }
    error = "encounter operation rejected";
    if (rc != DND_OK || dnd_encounter_validate_v1(after) != DND_OK) goto fail;
    error = "encounter output exceeds capacity";
    if (!output_encode(after, &c, before, call)) goto fail;
    (void)dnd_encounter_summary_v1(after, call->summary, sizeof(call->summary));
    return 1;
fail:
    call->output_json[0] = call->summary[0] = '\0';
    (void)snprintf(call->error, sizeof(call->error), "%s", error);
    return 0;
}


int pt_encounter_path(const char *directory, const char *input, char *path, size_t cap,
                       int *create, int *read_only) {
    encounter_command command;
    int n;
    if (!directory || !directory[0] || !command_decode(input, &command)) return 0;
    *create = command.operation == START || (command.operation == ROLL_INITIATIVE && command.expected == 0);
    *read_only = command.operation == GET;
    n = snprintf(path, cap, "%s/encounters/%s/%s.json", directory, command.campaign, command.encounter);
    return n > 0 && (size_t)n < cap;
}

int pt_encounter_transition(const char *before_json, char *after_json, size_t cap, pt_call *call) {
    encounter_command command;
    dnd_encounter_v1 before, after;
    if (!command_decode(call->input_json, &command) ||
        (before_json && !state_decode(before_json, &before)) ||
        !transition(&command, before_json ? &before : NULL, &after, call)) return 0;
    return state_encode(after_json, cap, &after) != 0;
}

int pt_encounter_canonical(const char *json, char *out, size_t cap, pt_dnd_identity *identity) {
    dnd_encounter_v1 state;
    if (!state_decode(json, &state) || !state_encode(out, cap, &state)) return 0;
    memset(identity, 0, sizeof(*identity));
    strcpy(identity->owner, state.owner_user_id);
    strcpy(identity->campaign, state.campaign_id);
    strcpy(identity->object, state.encounter_id);
    identity->version = state.version;
    return 1;
}

typedef struct {
    encounter_command command;
    initiative_requests requests;
    dnd_participant_v1 selected[DND_TURN_PARTICIPANTS_MAX];
    dnd_encounter_v1 after;
} initiative_plan;

int pt_encounter_is_initiative(const char *input) {
    encounter_command command;
    return command_decode(input, &command) && command.operation == ROLL_INITIATIVE;
}

static int initiative_plan_build(const char *before, const char *campaign, const pt_call *call,
                                  initiative_plan *plan) {
    encounter_command *c = &plan->command;
    dnd_encounter_v1 *state = &plan->after;
    if (!call || !owner_ok(call->user_id) ||
        strnlen(call->tool_call_id, sizeof(call->tool_call_id)) >= sizeof(call->tool_call_id) || !ts_valid_id(call->tool_call_id) ||
        !command_decode(call->input_json, c) || c->operation != ROLL_INITIATIVE ||
        !initiative_requests_decode(&c->initiative, &plan->requests) ||
        !pt_campaign_select(campaign, c->campaign, call->user_id, c->campaign_version,
            (const char (*)[DND_ID_CAP])plan->requests.ids, plan->requests.count, plan->selected)) return 0;
    if (!before) {
        if (c->expected != 0 || dnd_encounter_start_v1(state, c->campaign, c->encounter, call->user_id) != DND_OK) return 0;
    } else {
        if (!state_decode(before, state) || strcmp(state->campaign_id, c->campaign) ||
            strcmp(state->encounter_id, c->encounter) || strcmp(state->owner_user_id, call->user_id) ||
            strcmp(state->status, "active") || state->version != c->expected || state->version >= INT64_C(9007199254740991)) return 0;
        ++state->version;
    }
    for (size_t i = 0; i < plan->requests.count; ++i) {
        if (dnd_participant_index_v1(state->participants, state->n_participants, plan->selected[i].id) >= 0) continue;
        if (state->n_participants >= (int)DND_TURN_PARTICIPANTS_MAX) return 0;
        state->participants[state->n_participants++] = plan->selected[i];
    }
    return state->n_participants <= (int)DND_TURN_PARTICIPANTS_MAX;
}

/* Verify the saved state can return through the existing typed GET projection.
 * A future GET supplies its own fixed-size call identity and artifact hash. */
static int initiative_refresh_fits(const dnd_encounter_v1 *state) {
    turn_encounter_state_c view = {0};
    turn_encounter_participant_c *participants = NULL;
    uint8_t wire[DND_TURN_ENCOUNTER_MAX];
    int result = 0;
    if (state->n_participants < 1 || state->n_participants > (int)DND_TURN_PARTICIPANTS_MAX) return 0;
    participants = calloc((size_t)state->n_participants, sizeof(*participants));
    if (!participants) return 0;
    strcpy(view.campaign_id, state->campaign_id);
    strcpy(view.encounter_id, state->encounter_id);
    strcpy(view.status, state->status);
    strcpy(view.operation, "get");
    memcpy(view.tool_call_id, "encounter-", 10u);
    memset(view.tool_call_id + 10u, '0', 64u);
    memset(view.output_sha256, '0', 64u);
    view.version = state->version;
    view.round = state->round;
    view.active_index = state->active_index;
    view.participant_count = (size_t)state->n_participants;
    if (state->active_index >= 0) strcpy(view.active_participant_id, state->participants[state->active_index].id);
    for (int i = 0; i < state->n_participants; ++i) {
        const dnd_participant_v1 *p = &state->participants[i];
        turn_encounter_participant_c *v = &participants[i];
        if (!utf8_validate_v1((const uint8_t *)p->name, strlen(p->name))) goto done;
        for (const unsigned char *s = (const unsigned char *)p->name; *s; ++s)
            if (*s < 32u || *s == 127u) goto done;
        strcpy(v->id, p->id);
        strcpy(v->name, p->name);
        v->initiative = p->initiative;
        v->max_hp = p->max_hp;
        v->current_hp = p->current_hp;
        v->condition_count = (size_t)p->n_conditions;
        memcpy(v->conditions, p->conditions, sizeof(v->conditions));
    }
    result = pb_encode_turn_encounter(wire, sizeof(wire), &view, participants) != 0;
done:
    free(participants);
    return result;
}

int pt_encounter_prepare_initiative(const char *before, const char *campaign, const pt_call *call,
                                    char draws[PT_INITIATIVE_DRAWS_CAP]) {
    initiative_plan *plan = NULL;
    json_writer w = {draws, PT_INITIATIVE_DRAWS_CAP, 0, 0};
    int emitted = 0, result = 0;
    if (!draws) return 0;
    draws[0] = '\0';
    plan = calloc(1, sizeof(*plan));
    if (!plan || !initiative_plan_build(before, campaign, call, plan)) goto done;
    append(&w, "[");
    for (size_t i = 0; i < plan->requests.count; ++i) {
        int rolls[2];
        const dice_spec *spec = &plan->requests.specs[i];
        if (dice_secure_roll(spec, rolls, 2) != DICE_OK) goto done;
        for (int j = 0; j < spec->count; ++j) {
            append(&w, "%s%d", emitted ? "," : "", rolls[j]);
            ++emitted;
        }
    }
    append(&w, "]");
    result = !w.failed;
done:
    if (!result) draws[0] = '\0';
    free(plan);
    return result;
}

static int initiative_draws_decode(const char *json, int rolls[2u * DND_TURN_PARTICIPANTS_MAX], size_t *count) {
    cmp_json_array array;
    cmp_json_field item;
    int rc;
    *count = 0;
    if (!json || strnlen(json, PT_INITIATIVE_DRAWS_CAP) >= PT_INITIATIVE_DRAWS_CAP ||
        !cmp_json_array_parse(json, &array)) return 0;
    while ((rc = cmp_json_array_next(&array, &item)) == 1) {
        int value;
        if (*count >= 2u * DND_TURN_PARTICIPANTS_MAX) return 0;
        if (item.value_len == 1u && item.value[0] >= '1' && item.value[0] <= '9') value = item.value[0] - '0';
        else if (item.value_len == 2u && item.value[0] == '1' && item.value[1] >= '0' && item.value[1] <= '9')
            value = 10 + item.value[1] - '0';
        else if (item.value_len == 2u && !memcmp(item.value, "20", 2u)) value = 20;
        else return 0;
        rolls[(*count)++] = value;
    }
    return rc == 0;
}

int pt_encounter_initiative_transition(const char *before, const char *campaign, const char *draws,
                                       char *after, size_t cap, pt_call *call) {
    initiative_plan *plan = NULL;
    int rolls[2u * DND_TURN_PARTICIPANTS_MAX], result = 0;
    size_t count, used = 0;
    char active[DND_ID_CAP] = "", hash[65];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    static const char hex[] = "0123456789abcdef";
    if (!call || !after || !cap) return 0;
    after[0] = call->output_json[0] = call->summary[0] = call->error[0] = '\0';
    plan = calloc(1, sizeof(*plan));
    if (!plan || !initiative_plan_build(before, campaign, call, plan) || !initiative_draws_decode(draws, rolls, &count)) goto done;
    if (plan->after.active_index >= 0) strcpy(active, plan->after.participants[plan->after.active_index].id);
    for (size_t i = 0; i < plan->requests.count; ++i) {
        const dice_spec *spec = &plan->requests.specs[i];
        int kept[2], n_kept, index;
        if (used + (size_t)spec->count > count ||
            dice_select_indexes(rolls + used, spec->count, spec->keep, kept, &n_kept) != DICE_OK || n_kept != 1) goto done;
        index = dnd_participant_index_v1(plan->after.participants, plan->after.n_participants, plan->requests.ids[i]);
        if (index < 0) goto done;
        plan->after.participants[index].initiative = rolls[used + (size_t)kept[0]] + spec->modifier;
        used += (size_t)spec->count;
    }
    if (used != count) goto done;
    dnd_sort_participants_v1(plan->after.participants, plan->after.n_participants);
    plan->after.active_index = active[0] ? dnd_participant_index_v1(plan->after.participants, plan->after.n_participants, active) : -1;
    if (dnd_encounter_validate_v1(&plan->after) != DND_OK || !initiative_refresh_fits(&plan->after) ||
        !output_encode(&plan->after, &plan->command, NULL, call) ||
        !SHA256((const unsigned char *)campaign, strlen(campaign), digest)) goto done;
    for (size_t i = 0; i < sizeof(digest); ++i) { hash[2u * i] = hex[digest[i] >> 4u]; hash[2u * i + 1u] = hex[digest[i] & 15u]; }
    hash[64] = '\0';
    json_writer w = {call->output_json, sizeof(call->output_json), strlen(call->output_json) - 1u, 0};
    append(&w, ",\"campaign_version\":%lld,\"campaign_sha256\":\"%s\",\"initiative_rolls\":[",
           (long long)plan->command.campaign_version, hash);
    used = 0;
    for (size_t i = 0; i < plan->requests.count; ++i) {
        const dice_spec *spec = &plan->requests.specs[i];
        int kept[2], n_kept;
        if (dice_select_indexes(rolls + used, spec->count, spec->keep, kept, &n_kept) != DICE_OK) goto done;
        append(&w, "%s{\"character_id\":", i ? "," : ""); string(&w, plan->requests.ids[i]);
        append(&w, ",\"expression\":"); string(&w, spec->expression);
        append(&w, ",\"rolls\":[");
        for (int j = 0; j < spec->count; ++j) append(&w, "%s%d", j ? "," : "", rolls[used + (size_t)j]);
        append(&w, "],\"kept_indices\":[%d],\"total\":%d,\"entropy_source\":\"getrandom\"}",
               kept[0], rolls[used + (size_t)kept[0]] + spec->modifier);
        used += (size_t)spec->count;
    }
    append(&w, "]}");
    if (w.failed || !state_encode(after, cap, &plan->after)) goto done;
    (void)snprintf(call->summary, sizeof(call->summary), "Rolled initiative for %zu characters in encounter %s.",
                   plan->requests.count, plan->command.encounter);
    result = 1;
done:
    if (!result) {
        after[0] = call->output_json[0] = call->summary[0] = '\0';
        (void)snprintf(call->error, sizeof(call->error), "initiative state, selection, rolls, or output is invalid");
    }
    free(plan);
    return result;
}
