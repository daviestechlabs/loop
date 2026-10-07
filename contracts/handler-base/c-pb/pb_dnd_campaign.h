/* Bounded first-time campaign setup. Both turn codecs share this admission.
 * Optional prose and NPC behavior stay empty until explicitly authored.
 * Adding a character never replaces an existing character record. */
#ifndef HANDLER_BASE_PB_DND_CAMPAIGN_H
#define HANDLER_BASE_PB_DND_CAMPAIGN_H
#include "pb_dnd_request.h"

#define DND_CAMPAIGN_REQUEST_MAX 1024u
enum { DND_CAMPAIGN_NONE = 0, DND_CAMPAIGN_CREATE = 1, DND_CAMPAIGN_ADD_CHARACTER = 2 };
typedef struct { char name[201], ruleset[121]; } dnd_campaign_create_c;
typedef struct {
    char id[65], name[121], species[121], class_name[121];
    uint32_t kind; /* 1 player, 2 NPC; player identity is server-derived. */
    uint32_t level, armor_class, max_hp;
    uint32_t abilities[6]; /* Strength, Dexterity, Constitution, Intelligence, Wisdom, Charisma. */
} dnd_character_create_c;
typedef struct {
    uint32_t operation; /* Zero means absent on the containing turn. */
    char operation_id[65];
    int64_t expected_version;
    union { dnd_campaign_create_c campaign; dnd_character_create_c character; } data;
} dnd_campaign_request_c;

/* Strict UTF-8 scalar strings with no control bytes or surrounding spaces. */
static inline int dnd_campaign_label(const char *text, size_t capacity, int required) {
    const char *end = memchr(text, 0, capacity);
    if (!end || (required && end == text)) return 0;
    size_t n = (size_t)(end - text);
    if (n && (text[0] == ' ' || text[n - 1u] == ' ')) return 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = (unsigned char)text[i++], minimum;
        unsigned rest;
        if (c < 128u) { if (c < 32u || c == 127u) return 0; continue; }
        if (c >= 194u && c <= 223u) { rest = 1; c &= 31u; minimum = 128u; }
        else if (c >= 224u && c <= 239u) { rest = 2; c &= 15u; minimum = 2048u; }
        else if (c >= 240u && c <= 244u) { rest = 3; c &= 7u; minimum = 65536u; }
        else return 0;
        while (rest--) {
            if (i >= n || ((unsigned char)text[i] & 192u) != 128u) return 0;
            c = (c << 6u) | ((unsigned char)text[i++] & 63u);
        }
        if (c < minimum || c > 0x10ffffu || (c >= 0xd800u && c <= 0xdfffu)) return 0;
    }
    return 1;
}

static inline int dnd_campaign_request_valid(const dnd_campaign_request_c *r) {
    if (!r || !dnd_request_id_valid(r->operation_id, sizeof(r->operation_id))) return 0;
    if (r->operation == DND_CAMPAIGN_CREATE)
        return r->expected_version == 0 &&
            dnd_campaign_label(r->data.campaign.name, sizeof(r->data.campaign.name), 1) &&
            dnd_campaign_label(r->data.campaign.ruleset, sizeof(r->data.campaign.ruleset), 1);
    if (r->operation != DND_CAMPAIGN_ADD_CHARACTER || r->expected_version < 1 ||
        r->expected_version >= DND_EXACT_VERSION_MAX) return 0;
    const dnd_character_create_c *c = &r->data.character;
    if (!dnd_request_id_valid(c->id, sizeof(c->id)) || (c->kind != 1u && c->kind != 2u) ||
        !dnd_campaign_label(c->name, sizeof(c->name), 1) ||
        !dnd_campaign_label(c->species, sizeof(c->species), 0) ||
        !dnd_campaign_label(c->class_name, sizeof(c->class_name), 0) ||
        c->level > 20u || c->armor_class > 100u || c->max_hp > 100000u) return 0;
    for (size_t i = 0; i < 6u; ++i) if (c->abilities[i] < 1u || c->abilities[i] > 30u) return 0;
    return 1;
}

static inline int dnd_campaign_read_string(const uint8_t *data, size_t length, size_t *pos,
                                            char *target, size_t capacity) {
    const uint8_t *span;
    size_t size;
    if (!dnd_request_read_span(data, length, pos, &span, &size) || size >= capacity || memchr(span, 0, size)) return 0;
    memcpy(target, span, size);
    target[size] = 0;
    return 1;
}

static inline int dnd_campaign_data_decode(const uint8_t *data, size_t length,
                                            dnd_campaign_request_c *out, int character) {
    size_t pos = 0;
    unsigned seen = 0;
    while (pos < length) {
        uint64_t tag, value;
        if (!dnd_request_read_uint(data, length, &pos, &tag) || (tag >> 3u) < 1u ||
            (tag >> 3u) > (character ? 9u : 2u)) return 0;
        unsigned field = (unsigned)(tag >> 3u), wire = (unsigned)(tag & 7u);
        if (seen & (1u << field)) return 0;
        seen |= 1u << field;
        dnd_character_create_c *c = &out->data.character;
        if (!character) {
            if (wire != 2u || !dnd_campaign_read_string(data, length, &pos,
                    field == 1u ? out->data.campaign.name : out->data.campaign.ruleset,
                    field == 1u ? sizeof(out->data.campaign.name) : sizeof(out->data.campaign.ruleset))) return 0;
        } else if (field == 1u || field == 2u || field == 4u || field == 5u) {
            char *target = field == 1u ? c->id : field == 2u ? c->name : field == 4u ? c->species : c->class_name;
            size_t cap = field == 1u ? sizeof(c->id) : sizeof(c->name);
            if (wire != 2u || !dnd_campaign_read_string(data, length, &pos, target, cap)) return 0;
        } else if (field == 9u) {
            const uint8_t *span;
            size_t size, offset = 0;
            unsigned scores = 0;
            if (wire != 2u || !dnd_request_read_span(data, length, &pos, &span, &size)) return 0;
            while (offset < size) {
                uint64_t key;
                if (!dnd_request_read_uint(span, size, &offset, &key) || (key & 7u) ||
                    key < 8u || key > 48u || (scores & (1u << (key >> 3u))) ||
                    !dnd_request_read_uint(span, size, &offset, &value) || value < 1u || value > 30u) return 0;
                scores |= 1u << (key >> 3u);
                c->abilities[(key >> 3u) - 1u] = (uint32_t)value;
            }
            if (scores != 126u) return 0;
        } else {
            if (wire != 0u || !dnd_request_read_uint(data, length, &pos, &value) || value > UINT32_MAX) return 0;
            if (field == 3u) c->kind = (uint32_t)value;
            else if (field == 6u) c->level = (uint32_t)value;
            else if (field == 7u) c->armor_class = (uint32_t)value;
            else c->max_hp = (uint32_t)value;
        }
    }
    return 1;
}

static inline int dnd_campaign_request_decode(const uint8_t *data, size_t length, dnd_campaign_request_c *out) {
    size_t pos = 0;
    unsigned seen = 0, payload = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!data || !length || length > DND_CAMPAIGN_REQUEST_MAX) return 0;
    while (pos < length) {
        uint64_t tag, value;
        if (!dnd_request_read_uint(data, length, &pos, &tag) || (tag >> 3u) < 1u || (tag >> 3u) > 5u) return 0;
        unsigned field = (unsigned)(tag >> 3u), wire = (unsigned)(tag & 7u);
        if (seen & (1u << field)) return 0;
        seen |= 1u << field;
        if (field == 1u || field == 3u) {
            if (wire != 0u || !dnd_request_read_uint(data, length, &pos, &value) || value > (uint64_t)DND_EXACT_VERSION_MAX) return 0;
            if (field == 1u) { if (value > 2u) return 0; out->operation = (uint32_t)value; }
            else out->expected_version = (int64_t)value;
        } else if (field == 2u) {
            if (wire != 2u || !dnd_campaign_read_string(data, length, &pos, out->operation_id, sizeof(out->operation_id))) return 0;
        } else {
            const uint8_t *span;
            size_t size;
            if (payload || wire != 2u || !dnd_request_read_span(data, length, &pos, &span, &size) ||
                !dnd_campaign_data_decode(span, size, out, field == 5u)) return 0;
            payload = field - 3u;
        }
    }
    return payload == out->operation && dnd_campaign_request_valid(out);
}

static inline size_t dnd_campaign_request_encode(uint8_t *data, size_t capacity, const dnd_campaign_request_c *r) {
    uint8_t nested[768], scores[24];
    size_t pos = 0, n = 0, s = 0;
    if (!data || !dnd_campaign_request_valid(r)) return 0;
#define UINT(dst, cap, used, field, value) do { \
    if (!dnd_request_write_uint(dst, cap, used, (uint64_t)(field) << 3u) || \
        !dnd_request_write_uint(dst, cap, used, value)) return 0; } while (0)
#define TEXT(dst, cap, used, field, value) do { \
    if (!dnd_request_write_span(dst, cap, used, ((uint64_t)(field) << 3u) | 2u, \
        (const uint8_t *)(value), strlen(value))) return 0; } while (0)
    UINT(data, capacity, &pos, 1u, r->operation);
    TEXT(data, capacity, &pos, 2u, r->operation_id);
    if (r->expected_version) { UINT(data, capacity, &pos, 3u, (uint64_t)r->expected_version); }
    if (r->operation == DND_CAMPAIGN_CREATE) {
        TEXT(nested, sizeof(nested), &n, 1u, r->data.campaign.name);
        TEXT(nested, sizeof(nested), &n, 2u, r->data.campaign.ruleset);
    } else {
        const dnd_character_create_c *c = &r->data.character;
        TEXT(nested, sizeof(nested), &n, 1u, c->id);
        TEXT(nested, sizeof(nested), &n, 2u, c->name);
        UINT(nested, sizeof(nested), &n, 3u, c->kind);
        if (c->species[0]) { TEXT(nested, sizeof(nested), &n, 4u, c->species); }
        if (c->class_name[0]) { TEXT(nested, sizeof(nested), &n, 5u, c->class_name); }
        if (c->level) { UINT(nested, sizeof(nested), &n, 6u, c->level); }
        if (c->armor_class) { UINT(nested, sizeof(nested), &n, 7u, c->armor_class); }
        if (c->max_hp) { UINT(nested, sizeof(nested), &n, 8u, c->max_hp); }
        for (size_t i = 0; i < 6u; ++i) { UINT(scores, sizeof(scores), &s, i + 1u, c->abilities[i]); }
        if (!dnd_request_write_span(nested, sizeof(nested), &n, 74u, scores, s)) return 0;
    }
#undef UINT
#undef TEXT
    if (!dnd_request_write_span(data, capacity, &pos, r->operation == DND_CAMPAIGN_CREATE ? 34u : 42u, nested, n)) return 0;
    return pos <= DND_CAMPAIGN_REQUEST_MAX ? pos : 0;
}
#endif
