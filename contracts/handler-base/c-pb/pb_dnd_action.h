/* Explicit encounter changes. No allocation, I/O, or inferred game choices. */
#ifndef HANDLER_BASE_PB_DND_ACTION_H
#define HANDLER_BASE_PB_DND_ACTION_H
#include "pb_dnd_request.h"

#define DND_ENCOUNTER_ACTION_MAX 256u
enum { DND_ACTION_NONE, DND_ACTION_ADVANCE, DND_ACTION_DAMAGE, DND_ACTION_HEAL,
       DND_ACTION_CONDITION_ADD, DND_ACTION_CONDITION_REMOVE, DND_ACTION_END };
typedef struct {
    uint32_t operation;
    char operation_id[65], participant_id[65];
    int64_t expected_version;
    uint32_t amount, condition, damage_type;
} dnd_encounter_action_c;

static inline const char *dnd_action_name(uint32_t operation) {
    static const char *const names[] = {"", "advance", "damage", "heal", "condition_add", "condition_remove", "end"};
    return operation < sizeof(names) / sizeof(names[0]) ? names[operation] : "";
}
static inline const char *dnd_condition_name(uint32_t condition) {
    static const char *const names[] = {"", "blinded", "charmed", "deafened", "frightened", "grappled", "incapacitated",
        "invisible", "paralyzed", "petrified", "poisoned", "prone", "restrained", "stunned", "unconscious"};
    return condition < sizeof(names) / sizeof(names[0]) ? names[condition] : "";
}
static inline const char *dnd_damage_name(uint32_t type) {
    static const char *const names[] = {"", "acid", "bludgeoning", "cold", "fire", "force", "lightning", "necrotic",
        "piercing", "poison", "psychic", "radiant", "slashing", "thunder"};
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "";
}
static inline uint32_t dnd_condition_number(const char *name) {
    for (uint32_t i = 1; i <= 14u; ++i) if (!strcmp(name, dnd_condition_name(i))) return i;
    return 0;
}
static inline uint32_t dnd_damage_number(const char *name) {
    for (uint32_t i = 1; i <= 13u; ++i) if (!strcmp(name, dnd_damage_name(i))) return i;
    return 0;
}
static inline int dnd_encounter_action_valid(const dnd_encounter_action_c *r) {
    if (!r || !r->operation || !dnd_action_name(r->operation)[0] ||
        !dnd_request_id_valid(r->operation_id, sizeof(r->operation_id)) ||
        r->expected_version < 1 || r->expected_version >= DND_EXACT_VERSION_MAX) return 0;
    if (r->operation == DND_ACTION_ADVANCE || r->operation == DND_ACTION_END)
        return !r->participant_id[0] && !r->amount && !r->condition && !r->damage_type;
    if (!dnd_request_id_valid(r->participant_id, sizeof(r->participant_id))) return 0;
    if (r->operation == DND_ACTION_DAMAGE || r->operation == DND_ACTION_HEAL)
        return r->amount >= 1u && r->amount <= 100000u && !r->condition &&
            (r->operation == DND_ACTION_DAMAGE ? r->damage_type <= 13u : !r->damage_type);
    return !r->amount && !r->damage_type && r->condition >= 1u && r->condition <= 14u;
}
static inline int dnd_encounter_action_decode(const uint8_t *data, size_t length, dnd_encounter_action_c *out) {
    size_t pos = 0;
    unsigned seen = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!data || !length || length > DND_ENCOUNTER_ACTION_MAX) return 0;
    while (pos < length) {
        uint64_t tag, value;
        if (!dnd_request_read_uint(data, length, &pos, &tag) || (tag >> 3u) < 1u || (tag >> 3u) > 7u) return 0;
        unsigned field = (unsigned)(tag >> 3u), wire = (unsigned)(tag & 7u);
        if (seen & (1u << field)) return 0;
        seen |= 1u << field;
        if (field == 2u || field == 4u) {
            const uint8_t *span;
            size_t size;
            char *target = field == 2u ? out->operation_id : out->participant_id;
            if (wire != 2u || !dnd_request_read_span(data, length, &pos, &span, &size) || size >= 65u || memchr(span, 0, size)) return 0;
            memcpy(target, span, size);
            target[size] = 0;
        } else {
            if (wire || !dnd_request_read_uint(data, length, &pos, &value)) return 0;
            if (field == 3u) {
                if (value >= (uint64_t)DND_EXACT_VERSION_MAX) return 0;
                out->expected_version = (int64_t)value;
            } else {
                if (value > UINT32_MAX) return 0;
                if (field == 1u) out->operation = (uint32_t)value;
                else if (field == 5u) out->amount = (uint32_t)value;
                else if (field == 6u) out->condition = (uint32_t)value;
                else out->damage_type = (uint32_t)value;
            }
        }
    }
    return dnd_encounter_action_valid(out);
}
static inline size_t dnd_encounter_action_encode(uint8_t *data, size_t capacity, const dnd_encounter_action_c *r) {
    size_t pos = 0;
    if (!data || !dnd_encounter_action_valid(r)) return 0;
    for (unsigned field = 1; field <= 7u; ++field) {
        if (field == 2u || field == 4u) {
            const char *value = field == 2u ? r->operation_id : r->participant_id;
            if (value[0] && !dnd_request_write_span(data, capacity, &pos, ((uint64_t)field << 3u) | 2u,
                (const uint8_t *)value, strlen(value))) return 0;
        } else {
            uint64_t value = field == 1u ? r->operation : field == 3u ? (uint64_t)r->expected_version :
                field == 5u ? r->amount : field == 6u ? r->condition : r->damage_type;
            if (value && (!dnd_request_write_uint(data, capacity, &pos, (uint64_t)field << 3u) ||
                !dnd_request_write_uint(data, capacity, &pos, value))) return 0;
        }
    }
    return pos <= DND_ENCOUNTER_ACTION_MAX ? pos : 0;
}
#endif
