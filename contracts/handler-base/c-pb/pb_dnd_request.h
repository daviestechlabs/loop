/* Shared bounded DndInitiativeRequest codec for both C turn implementations.
 * Header-only: the product and optimized runtime readers use different wire
 * types. This keeps the new submessage's admission identical without a host
 * library, allocation, or I/O dependency. */
#ifndef HANDLER_BASE_PB_DND_REQUEST_H
#define HANDLER_BASE_PB_DND_REQUEST_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DND_INITIATIVE_SELECTIONS_MAX 64u
#define DND_INITIATIVE_REQUEST_MAX 6144u
#define DND_TURN_START_WIRE_MAX 16384u
#define DND_EXACT_VERSION_MAX INT64_C(9007199254740991)
typedef struct {
    char character_id[65], expression[16];
} dnd_initiative_selection_c;
typedef struct {
    int64_t campaign_version, expected_version;
    char operation_id[65]; /* Stable across distinct transport attempts. */
    size_t count; /* Zero means no initiative request on the containing turn. */
    dnd_initiative_selection_c selections[DND_INITIATIVE_SELECTIONS_MAX];
} dnd_initiative_request_c;

static inline int dnd_request_id_valid(const char *id, size_t capacity) {
    const char *end = memchr(id, 0, capacity);
    if (!end || end == id || (size_t)(end - id) > 64u) return 0;
    for (const char *p = id; p != end; ++p) {
        unsigned char c = (unsigned char)*p;
        int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (p == id || (c != '.' && c != '_' && c != '-'))) return 0;
    }
    return 1;
}

/* Canonical d20 grammar; final modifiers are choices, never inferred stats.
 * keep is 0 for one die, 1 for highest, and -1 for lowest. */
static inline int dnd_request_expression(const char *text, size_t capacity,
                                          int *keep, int *modifier) {
    const char *end = memchr(text, 0, capacity), *p;
    int mode, amount = 0, negative = 0;
    if (!end) return 0;
    if ((size_t)(end - text) >= 4u && !memcmp(text, "1d20", 4u)) { mode = 0; p = text + 4u; }
    else if ((size_t)(end - text) >= 7u && !memcmp(text, "2d20kh1", 7u)) { mode = 1; p = text + 7u; }
    else if ((size_t)(end - text) >= 7u && !memcmp(text, "2d20kl1", 7u)) { mode = -1; p = text + 7u; }
    else return 0;
    if (p != end) {
        if (*p != '+' && *p != '-') return 0;
        negative = *p++ == '-';
        if (p == end || *p < '1' || *p > '9' || (size_t)(end - p) > 3u) return 0;
        while (p != end) {
            if (*p < '0' || *p > '9') return 0;
            amount = amount * 10 + (*p++ - '0');
        }
        if (amount > (negative ? 101 : 80)) return 0;
    }
    if (keep) *keep = mode;
    if (modifier) *modifier = negative ? -amount : amount;
    return 1;
}

static inline int dnd_initiative_request_valid(const dnd_initiative_request_c *request) {
    if (!request || !request->count || request->count > DND_INITIATIVE_SELECTIONS_MAX ||
        request->campaign_version < 1 || request->campaign_version > DND_EXACT_VERSION_MAX ||
        request->expected_version < 0 || request->expected_version >= DND_EXACT_VERSION_MAX ||
        !dnd_request_id_valid(request->operation_id, sizeof(request->operation_id))) return 0;
    for (size_t i = 0; i < request->count; ++i) {
        const dnd_initiative_selection_c *s = &request->selections[i];
        if (!dnd_request_id_valid(s->character_id, sizeof(s->character_id)) ||
            !dnd_request_expression(s->expression, sizeof(s->expression), NULL, NULL)) return 0;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(s->character_id, request->selections[j].character_id)) return 0;
    }
    return 1;
}

static inline int dnd_request_read_uint(const uint8_t *data, size_t length, size_t *pos, uint64_t *out) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift <= 63u; shift += 7u) {
        uint8_t byte;
        if (*pos >= length) return 0;
        byte = data[(*pos)++];
        if (shift == 63u && byte > 1u) return 0;
        value |= (uint64_t)(byte & 127u) << shift;
        if (!(byte & 128u)) {
            if (shift && !byte) return 0;
            *out = value;
            return 1;
        }
    }
    return 0;
}

static inline int dnd_request_read_span(const uint8_t *data, size_t length, size_t *pos,
                                         const uint8_t **span, size_t *size) {
    uint64_t count;
    if (!dnd_request_read_uint(data, length, pos, &count) || count > length - *pos) return 0;
    *span = data + *pos;
    *size = (size_t)count;
    *pos += *size;
    return 1;
}

static inline int dnd_request_selection_decode(const uint8_t *data, size_t length,
                                                dnd_initiative_selection_c *out) {
    size_t pos = 0;
    unsigned seen = 0;
    memset(out, 0, sizeof(*out));
    while (pos < length) {
        uint64_t tag;
        const uint8_t *value;
        size_t size, capacity;
        char *target;
        if (!dnd_request_read_uint(data, length, &pos, &tag) || (tag != 10u && tag != 18u) ||
            (seen & (tag == 10u ? 1u : 2u)) ||
            !dnd_request_read_span(data, length, &pos, &value, &size)) return 0;
        seen |= tag == 10u ? 1u : 2u;
        target = tag == 10u ? out->character_id : out->expression;
        capacity = tag == 10u ? sizeof(out->character_id) : sizeof(out->expression);
        if (!size || size >= capacity || memchr(value, 0, size)) return 0;
        memcpy(target, value, size);
    }
    return seen == 3u;
}

static inline int dnd_initiative_request_decode(const uint8_t *data, size_t length,
                                                dnd_initiative_request_c *out) {
    size_t pos = 0;
    unsigned seen = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!data || !length || length > DND_INITIATIVE_REQUEST_MAX) return 0;
    while (pos < length) {
        uint64_t tag, value;
        const uint8_t *span;
        size_t size;
        if (!dnd_request_read_uint(data, length, &pos, &tag)) return 0;
        if (tag == 8u || tag == 16u) {
            unsigned bit = tag == 8u ? 1u : 2u;
            if ((seen & bit) || !dnd_request_read_uint(data, length, &pos, &value) ||
                value > (uint64_t)DND_EXACT_VERSION_MAX) return 0;
            seen |= bit;
            if (tag == 8u) out->campaign_version = (int64_t)value;
            else out->expected_version = (int64_t)value;
        } else if (tag == 26u) {
            if (out->count >= DND_INITIATIVE_SELECTIONS_MAX ||
                !dnd_request_read_span(data, length, &pos, &span, &size) ||
                !dnd_request_selection_decode(span, size, &out->selections[out->count])) return 0;
            ++out->count;
        } else if (tag == 34u) {
            if ((seen & 4u) || !dnd_request_read_span(data, length, &pos, &span, &size) ||
                !size || size >= sizeof(out->operation_id) || memchr(span, 0, size)) return 0;
            seen |= 4u;
            memcpy(out->operation_id, span, size);
        } else return 0;
    }
    return (seen & 5u) == 5u && dnd_initiative_request_valid(out);
}

static inline int dnd_request_write_uint(uint8_t *data, size_t capacity, size_t *pos, uint64_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 127u);
        value >>= 7u;
        if (*pos >= capacity) return 0;
        data[(*pos)++] = (uint8_t)(byte | (value ? 128u : 0u));
    } while (value);
    return 1;
}

static inline int dnd_request_write_span(uint8_t *data, size_t capacity, size_t *pos,
                                          uint64_t tag, const uint8_t *value, size_t length) {
    if (!dnd_request_write_uint(data, capacity, pos, tag) ||
        !dnd_request_write_uint(data, capacity, pos, length) || length > capacity - *pos) return 0;
    memcpy(data + *pos, value, length);
    *pos += length;
    return 1;
}

static inline size_t dnd_initiative_request_encode(uint8_t *data, size_t capacity,
                                                   const dnd_initiative_request_c *request) {
    size_t pos = 0;
    if (!data || !dnd_initiative_request_valid(request) ||
        !dnd_request_write_uint(data, capacity, &pos, 8u) ||
        !dnd_request_write_uint(data, capacity, &pos, (uint64_t)request->campaign_version)) return 0;
    if (request->expected_version &&
        (!dnd_request_write_uint(data, capacity, &pos, 16u) ||
         !dnd_request_write_uint(data, capacity, &pos, (uint64_t)request->expected_version))) return 0;
    for (size_t i = 0; i < request->count; ++i) {
        uint8_t selection[96];
        size_t used = 0;
        const dnd_initiative_selection_c *s = &request->selections[i];
        if (!dnd_request_write_span(selection, sizeof(selection), &used, 10u,
                (const uint8_t *)s->character_id, strlen(s->character_id)) ||
            !dnd_request_write_span(selection, sizeof(selection), &used, 18u,
                (const uint8_t *)s->expression, strlen(s->expression)) ||
            !dnd_request_write_span(data, capacity, &pos, 26u, selection, used)) return 0;
    }
    if (!dnd_request_write_span(data, capacity, &pos, 34u,
            (const uint8_t *)request->operation_id, strlen(request->operation_id))) return 0;
    return pos <= DND_INITIATIVE_REQUEST_MAX ? pos : 0;
}
#endif
