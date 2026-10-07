/* Bounded protocol core for the pure-C WebTransport edge. */

#include "gateway_webtransport_core.h"

#include "utf8.h"
#include "voice_ascii.h"
#include "cmp_turn_metadata_policy.h"
#include "cmp_json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GW_WT_JSON_TOKENS 512u
#define GW_WT_JSON_DEPTH 8u
#define GW_WT_CONTROL_PREFIX "DTVA1:"

enum json_token_type {
    JSON_TOKEN_OBJECT = 1,
    JSON_TOKEN_ARRAY,
    JSON_TOKEN_STRING,
    JSON_TOKEN_STRING_ESCAPED,
    JSON_TOKEN_PRIMITIVE
};

typedef struct {
    size_t start;
    size_t end;
    enum json_token_type type;
    int parent;
    int first_child;
    int next_sibling;
} json_token;

typedef struct {
    json_token tokens[GW_WT_JSON_TOKENS];
    size_t count;
    int has_non_ascii;
} json_tokens;

static int json_hex(unsigned char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static int subject_lower_hex(unsigned char value) {
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f');
}

/* Repeated-byte masks make this test independent of host byte order. */
static int json_word_has_zero_byte(uint64_t value) {
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t high_bits = UINT64_C(0x8080808080808080);
    return ((value - ones) & ~value & high_bits) != 0u;
}

static int json_word_is_plain_ascii(uint64_t value) {
    const uint64_t high_bits = UINT64_C(0x8080808080808080);
    const uint64_t control_mask = UINT64_C(0xe0e0e0e0e0e0e0e0);
    const uint64_t quotes = UINT64_C(0x2222222222222222);
    const uint64_t backslashes = UINT64_C(0x5c5c5c5c5c5c5c5c);
    /* Without a high bit, a zero control-mask lane means a raw control byte. */
    return (value & high_bits) == 0u &&
           !json_word_has_zero_byte(value & control_mask) &&
           !json_word_has_zero_byte(value ^ quotes) &&
           !json_word_has_zero_byte(value ^ backslashes);
}

int gw_wt_response_subject_write(
    char *out,
    size_t out_cap,
    size_t slot,
    const char nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u]
) {
    static const char hex[] = "0123456789abcdef";
    size_t used = sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u;
    size_t i;
    if (!out || !nonce || out_cap <= GW_WT_EVENT_SUBJECT_LENGTH || slot > 255u)
        return GW_WT_ERR_ARGUMENT;
    for (i = 0; i < GW_WT_EVENT_NONCE_HEX_LEN; ++i)
        if (!subject_lower_hex((unsigned char)nonce[i]))
            return GW_WT_ERR_MALFORMED;
    if (nonce[GW_WT_EVENT_NONCE_HEX_LEN] != '\0') return GW_WT_ERR_MALFORMED;
    memcpy(out, GW_WT_EVENT_SUBJECT_PREFIX, used);
    out[used++] = hex[slot >> 4u];
    out[used++] = hex[slot & 15u];
    out[used++] = '.';
    memcpy(out + used, nonce, GW_WT_EVENT_NONCE_HEX_LEN + 1u);
    return GW_WT_OK;
}

int gw_wt_response_subject_slot(const char *subject, size_t *slot) {
    size_t prefix_len = sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u;
    size_t length = prefix_len;
    unsigned char high;
    unsigned char low;
    unsigned high_value;
    unsigned low_value;
    if (!subject || !slot) return GW_WT_ERR_ARGUMENT;
    if (strncmp(subject, GW_WT_EVENT_SUBJECT_PREFIX, prefix_len) != 0)
        return GW_WT_ERR_MALFORMED;
    high = (unsigned char)subject[prefix_len];
    if (!subject_lower_hex(high)) return GW_WT_ERR_MALFORMED;
    low = (unsigned char)subject[prefix_len + 1u];
    if (!subject_lower_hex(low) || subject[prefix_len + 2u] != '.')
        return GW_WT_ERR_MALFORMED;
    length += 3u;
    while (length <= GW_WT_EVENT_SUBJECT_LENGTH && subject[length] != '\0') length++;
    if (length != GW_WT_EVENT_SUBJECT_LENGTH) return GW_WT_ERR_MALFORMED;
    high_value = high <= (unsigned char)'9' ?
        (unsigned)(high - (unsigned char)'0') :
        (unsigned)(high - (unsigned char)'a') + 10u;
    low_value = low <= (unsigned char)'9' ?
        (unsigned)(low - (unsigned char)'0') :
        (unsigned)(low - (unsigned char)'a') + 10u;
    *slot = (size_t)high_value * 16u + (size_t)low_value;
    return GW_WT_OK;
}

static int token_add(
    json_tokens *parsed,
    enum json_token_type type,
    size_t start,
    int parent,
    unsigned depth,
    int *last_child,
    size_t *index
) {
    json_token *token;
    /* Callers own the pointers and linkage indexes. Input controls capacity and depth. */
    if (parsed->count >= GW_WT_JSON_TOKENS || depth > GW_WT_JSON_DEPTH)
        return -1;
    *index = parsed->count++;
    token = &parsed->tokens[*index];
    token->start = start;
    token->type = type;
    token->parent = parent;
    token->first_child = -1;
    token->next_sibling = -1;
    if (parent >= 0) {
        if (*last_child < 0)
            parsed->tokens[(size_t)parent].first_child = (int)*index;
        else
            parsed->tokens[(size_t)*last_child].next_sibling = (int)*index;
        *last_child = (int)*index;
    }
    return 0;
}

static int parse_string_token(
    const uint8_t *json,
    size_t json_len,
    size_t *position,
    json_tokens *parsed,
    int parent,
    unsigned depth,
    int *last_child
) {
    unsigned char byte_or = 0u;
    size_t token_index;
    size_t cursor = *position + 1u;
    if (token_add(parsed, JSON_TOKEN_STRING, cursor, parent, depth,
                  last_child, &token_index) != 0)
        return -1;
    while (cursor < json_len) {
        while (json_len - cursor >= sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, json + cursor, sizeof(word));
            if (!json_word_is_plain_ascii(word)) break;
            cursor += sizeof(word);
        }
        while (cursor < json_len) {
            unsigned char value = json[cursor++];
            if (value == '"') {
                parsed->tokens[token_index].end = cursor - 1u;
                if ((byte_or & 0x80u) != 0u) parsed->has_non_ascii = 1;
                *position = cursor;
                return 0;
            }
            byte_or = (unsigned char)(byte_or | value);
            if (value < 0x20u) return -1;
            if (value == '\\') {
                unsigned char escaped;
                parsed->tokens[token_index].type = JSON_TOKEN_STRING_ESCAPED;
                if (cursor >= json_len) return -1;
                escaped = json[cursor++];
                if (escaped == 'u') {
                    unsigned i;
                    if (json_len - cursor < 4u) return -1;
                    for (i = 0; i < 4u; ++i)
                        if (!json_hex(json[cursor + i])) return -1;
                    cursor += 4u;
                } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                           escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                           escaped != 'r' && escaped != 't') {
                    return -1;
                }
                break;
            }
        }
    }
    return -1;
}

static int primitive_valid(const uint8_t *value, size_t len) {
    size_t cursor = 0;
    if ((len == 4u && memcmp(value, "true", 4u) == 0) ||
        (len == 4u && memcmp(value, "null", 4u) == 0) ||
        (len == 5u && memcmp(value, "false", 5u) == 0)) return 1;
    if (cursor < len && value[cursor] == '-') cursor++;
    if (cursor >= len) return 0;
    if (value[cursor] == '0') {
        cursor++;
        if (cursor < len && voice_ascii_is_digit(value[cursor])) return 0;
    } else if (value[cursor] >= '1' && value[cursor] <= '9') {
        do {
            cursor++;
        } while (cursor < len && voice_ascii_is_digit(value[cursor]));
    } else {
        return 0;
    }
    if (cursor < len && value[cursor] == '.') {
        cursor++;
        if (cursor >= len || !voice_ascii_is_digit(value[cursor])) return 0;
        while (cursor < len && voice_ascii_is_digit(value[cursor])) cursor++;
    }
    if (cursor < len && (value[cursor] == 'e' || value[cursor] == 'E')) {
        cursor++;
        if (cursor < len && (value[cursor] == '+' || value[cursor] == '-')) cursor++;
        if (cursor >= len || !voice_ascii_is_digit(value[cursor])) return 0;
        while (cursor < len && voice_ascii_is_digit(value[cursor])) cursor++;
    }
    return cursor == len;
}

static void syntax_skip_ws(const uint8_t *json, size_t json_len, size_t *cursor) {
    while (*cursor < json_len && (json[*cursor] == ' ' || json[*cursor] == '\t' ||
           json[*cursor] == '\r' || json[*cursor] == '\n')) (*cursor)++;
}

static int object_key_unique(
    const uint8_t *json,
    const json_tokens *parsed,
    size_t object_index,
    size_t key_index,
    uint64_t *seen_keys
) {
    const json_token *key;
    int previous_index;
    size_t key_len;
    unsigned fingerprint;
    uint64_t fingerprint_bit;
    if (!json || !parsed || object_index >= parsed->count ||
        key_index >= parsed->count || key_index <= object_index ||
        parsed->tokens[object_index].type != JSON_TOKEN_OBJECT || !seen_keys)
        return 0;
    key = &parsed->tokens[key_index];
    if (key->type != JSON_TOKEN_STRING || key->parent != (int)object_index ||
        key->start > key->end)
        return 0;
    key_len = key->end - key->start;
    fingerprint = (unsigned)(key_len & 63u);
    if (key_len != 0u) {
        fingerprint += (unsigned)json[key->start] * 3u;
        fingerprint += (unsigned)json[key->start + key_len / 2u] * 5u;
        fingerprint += (unsigned)json[key->end - 1u] * 7u;
    }
    fingerprint_bit = UINT64_C(1) << (fingerprint & 63u);
    if ((*seen_keys & fingerprint_bit) == 0u) {
        *seen_keys |= fingerprint_bit;
        return 1;
    }
    previous_index = parsed->tokens[object_index].first_child;
    while (previous_index >= 0 && (size_t)previous_index != key_index) {
        const json_token *previous;
        int previous_value;
        if ((size_t)previous_index >= parsed->count) return 0;
        previous = &parsed->tokens[(size_t)previous_index];
        previous_value = previous->next_sibling;
        if (previous->type != JSON_TOKEN_STRING || previous_value < 0 ||
            (size_t)previous_value >= parsed->count)
            return 0;
        if (previous->end - previous->start == key_len &&
            memcmp(json + previous->start, json + key->start, key_len) == 0)
            return 0;
        previous_index = parsed->tokens[(size_t)previous_value].next_sibling;
    }
    return previous_index >= 0;
}

static int tokenize_value(
    const uint8_t *json,
    size_t json_len,
    size_t *cursor,
    json_tokens *parsed,
    int parent,
    unsigned depth,
    int *last_child
) {
    unsigned char value;
    size_t token_index;
    if (depth > GW_WT_JSON_DEPTH) return -1;
    syntax_skip_ws(json, json_len, cursor);
    if (*cursor >= json_len) return -1;
    value = json[*cursor];
    if (value == '"')
        return parse_string_token(json, json_len, cursor, parsed, parent,
                                  depth, last_child);
    if (value == '{') {
        int object_last_child = -1;
        uint64_t object_seen_keys = 0u;
        if (depth >= GW_WT_JSON_DEPTH ||
            token_add(parsed, JSON_TOKEN_OBJECT, *cursor, parent, depth,
                      last_child, &token_index) != 0)
            return -1;
        (*cursor)++;
        syntax_skip_ws(json, json_len, cursor);
        if (*cursor < json_len && json[*cursor] == '}') {
            (*cursor)++;
            parsed->tokens[token_index].end = *cursor;
            return 0;
        }
        for (;;) {
            size_t key_index = parsed->count;
            if (*cursor >= json_len || json[*cursor] != '"' ||
                parse_string_token(
                    json, json_len, cursor, parsed, (int)token_index,
                    depth + 1u, &object_last_child) != 0 ||
                !object_key_unique(
                    json, parsed, token_index, key_index, &object_seen_keys))
                return -1;
            syntax_skip_ws(json, json_len, cursor);
            if (*cursor >= json_len || json[(*cursor)++] != ':') return -1;
            if (tokenize_value(
                    json, json_len, cursor, parsed, (int)token_index,
                    depth + 1u, &object_last_child) != 0)
                return -1;
            syntax_skip_ws(json, json_len, cursor);
            if (*cursor < json_len && json[*cursor] == '}') {
                (*cursor)++;
                parsed->tokens[token_index].end = *cursor;
                return 0;
            }
            if (*cursor >= json_len || json[(*cursor)++] != ',') return -1;
            syntax_skip_ws(json, json_len, cursor);
        }
    }
    if (value == '[') {
        int array_last_child = -1;
        if (depth >= GW_WT_JSON_DEPTH ||
            token_add(parsed, JSON_TOKEN_ARRAY, *cursor, parent, depth,
                      last_child, &token_index) != 0)
            return -1;
        (*cursor)++;
        syntax_skip_ws(json, json_len, cursor);
        if (*cursor < json_len && json[*cursor] == ']') {
            (*cursor)++;
            parsed->tokens[token_index].end = *cursor;
            return 0;
        }
        for (;;) {
            if (tokenize_value(
                    json, json_len, cursor, parsed, (int)token_index,
                    depth + 1u, &array_last_child) != 0)
                return -1;
            syntax_skip_ws(json, json_len, cursor);
            if (*cursor < json_len && json[*cursor] == ']') {
                (*cursor)++;
                parsed->tokens[token_index].end = *cursor;
                return 0;
            }
            if (*cursor >= json_len || json[(*cursor)++] != ',') return -1;
            syntax_skip_ws(json, json_len, cursor);
        }
    }
    {
        size_t start = *cursor;
        while (*cursor < json_len && json[*cursor] != ',' && json[*cursor] != ']' &&
               json[*cursor] != '}' && json[*cursor] != ' ' && json[*cursor] != '\t' &&
               json[*cursor] != '\r' && json[*cursor] != '\n') (*cursor)++;
        if (*cursor == start || !primitive_valid(json + start, *cursor - start) ||
            token_add(parsed, JSON_TOKEN_PRIMITIVE, start, parent, depth,
                      last_child, &token_index) != 0)
            return -1;
        parsed->tokens[token_index].end = *cursor;
        return 0;
    }
}

static int json_tokenize(const uint8_t *json, size_t json_len, json_tokens *parsed) {
    size_t cursor = 0u;
    int root_last_child = -1;
    if (!json || !parsed || json_len == 0 || json_len > GW_WT_CONTROL_FRAME_CAP)
        return -1;
    /* token_add initializes every token that count makes live. */
    parsed->count = 0u;
    parsed->has_non_ascii = 0;
    if (tokenize_value(json, json_len, &cursor, parsed, -1, 0u,
                       &root_last_child) != 0)
        return -1;
    syntax_skip_ws(json, json_len, &cursor);
    if (cursor != json_len || parsed->count == 0u ||
        parsed->tokens[0].type != JSON_TOKEN_OBJECT)
        return -1;
    return parsed->has_non_ascii && !utf8_validate_v1(json, json_len) ? -1 : 0;
}

static int object_value_token(
    const uint8_t *json,
    const json_tokens *parsed,
    size_t object_index,
    const char *key,
    size_t *value_index
) {
    int name_index;
    size_t visited = 0u;
    size_t key_len;
    if (!json || !parsed || object_index >= parsed->count || !key || !value_index ||
        parsed->tokens[object_index].type != JSON_TOKEN_OBJECT) return 0;
    key_len = strlen(key);
    name_index = parsed->tokens[object_index].first_child;
    while (name_index >= 0 && visited + 1u < parsed->count) {
        const json_token *name;
        int found_value;
        if ((size_t)name_index >= parsed->count) return 0;
        name = &parsed->tokens[(size_t)name_index];
        found_value = name->next_sibling;
        if (found_value < 0 || (size_t)found_value >= parsed->count) return 0;
        if (name->type == JSON_TOKEN_STRING && name->end - name->start == key_len &&
            memcmp(json + name->start, key, key_len) == 0) {
            *value_index = (size_t)found_value;
            return 1;
        }
        name_index = parsed->tokens[(size_t)found_value].next_sibling;
        visited += 2u;
    }
    return 0;
}

enum turn_request_field {
    TURN_REQUEST_FIELD_REQUEST_ID = 0,
    TURN_REQUEST_FIELD_SESSION_ID,
    TURN_REQUEST_FIELD_TEXT,
    TURN_REQUEST_FIELD_IDENTITY_TOKEN,
    TURN_REQUEST_FIELD_ENABLE_RAG,
    TURN_REQUEST_FIELD_ENABLE_TTS,
    TURN_REQUEST_FIELD_METADATA,
    TURN_REQUEST_FIELD_CLIENT_TRANSPORT,
    TURN_REQUEST_FIELD_DND_INITIATIVE,
    TURN_REQUEST_FIELD_DND_CAMPAIGN,
    TURN_REQUEST_FIELD_DND_ENCOUNTER_ACTION,
    TURN_REQUEST_FIELD_COUNT
};

static int turn_request_field(const uint8_t *name, size_t name_len) {
    if (name_len == 20u && memcmp(name, "dnd_encounter_action", 20u) == 0)
        return TURN_REQUEST_FIELD_DND_ENCOUNTER_ACTION;
    if (name_len == 12u && memcmp(name, "dnd_campaign", 12u) == 0)
        return TURN_REQUEST_FIELD_DND_CAMPAIGN;
    if (name_len == 14u && memcmp(name, "dnd_initiative", 14u) == 0)
        return TURN_REQUEST_FIELD_DND_INITIATIVE;
    if (name_len == 4u && memcmp(name, "text", 4u) == 0)
        return TURN_REQUEST_FIELD_TEXT;
    if (name_len == 8u && memcmp(name, "metadata", 8u) == 0)
        return TURN_REQUEST_FIELD_METADATA;
    if (name_len == 10u) {
        if (memcmp(name, "request_id", 10u) == 0)
            return TURN_REQUEST_FIELD_REQUEST_ID;
        if (memcmp(name, "session_id", 10u) == 0)
            return TURN_REQUEST_FIELD_SESSION_ID;
        if (memcmp(name, "enable_rag", 10u) == 0)
            return TURN_REQUEST_FIELD_ENABLE_RAG;
        if (memcmp(name, "enable_tts", 10u) == 0)
            return TURN_REQUEST_FIELD_ENABLE_TTS;
    }
    if (name_len == 14u && memcmp(name, "identity_token", 14u) == 0)
        return TURN_REQUEST_FIELD_IDENTITY_TOKEN;
    if (name_len == 16u && memcmp(name, "client_transport", 16u) == 0)
        return TURN_REQUEST_FIELD_CLIENT_TRANSPORT;
    return -1;
}

static int turn_request_field_tokens(
    const uint8_t *json,
    const json_tokens *parsed,
    size_t values[TURN_REQUEST_FIELD_COUNT]
) {
    int name_index;
    size_t i;
    size_t visited = 0u;
    if (!json || !parsed || !values || parsed->count == 0u ||
        parsed->tokens[0].type != JSON_TOKEN_OBJECT)
        return 0;
    for (i = 0u; i < TURN_REQUEST_FIELD_COUNT; ++i) values[i] = SIZE_MAX;
    name_index = parsed->tokens[0].first_child;
    while (name_index >= 0 && visited + 1u < parsed->count) {
        const json_token *name;
        int value_index;
        int field;
        if ((size_t)name_index >= parsed->count) return 0;
        name = &parsed->tokens[(size_t)name_index];
        value_index = name->next_sibling;
        if (name->type != JSON_TOKEN_STRING || value_index < 0 ||
            (size_t)value_index >= parsed->count)
            return 0;
        field = turn_request_field(json + name->start, name->end - name->start);
        if (field >= 0) values[(size_t)field] = (size_t)value_index;
        name_index = parsed->tokens[(size_t)value_index].next_sibling;
        visited += 2u;
    }
    return name_index < 0;
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static int read_hex4(const uint8_t *input, uint32_t *out) {
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < 4u; ++i) {
        int digit = hex_value(input[i]);
        if (digit < 0) return -1;
        value = value << 4u | (unsigned)digit;
    }
    *out = value;
    return 0;
}

static int append_codepoint(uint32_t codepoint, char *out, size_t out_cap, size_t *used) {
    uint8_t encoded[4];
    size_t encoded_len;
    if (codepoint == 0) return -1;
    if (codepoint <= 0x7fu) {
        encoded[0] = (uint8_t)codepoint;
        encoded_len = 1;
    } else if (codepoint <= 0x7ffu) {
        encoded[0] = (uint8_t)(0xc0u | codepoint >> 6u);
        encoded[1] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        encoded_len = 2;
    } else if (codepoint <= 0xffffu && (codepoint < 0xd800u || codepoint > 0xdfffu)) {
        encoded[0] = (uint8_t)(0xe0u | codepoint >> 12u);
        encoded[1] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        encoded_len = 3;
    } else if (codepoint <= 0x10ffffu) {
        encoded[0] = (uint8_t)(0xf0u | codepoint >> 18u);
        encoded[1] = (uint8_t)(0x80u | ((codepoint >> 12u) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
        encoded[3] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        encoded_len = 4;
    } else {
        return -1;
    }
    if (*used >= out_cap || encoded_len >= out_cap - *used) return -1;
    memcpy(out + *used, encoded, encoded_len);
    *used += encoded_len;
    return 0;
}

static int token_string(
    const uint8_t *json,
    const json_token *token,
    char *out,
    size_t out_cap,
    int allow_empty
) {
    size_t cursor;
    size_t used = 0;
    size_t input_len;
    if (!json || !token ||
        (token->type != JSON_TOKEN_STRING &&
         token->type != JSON_TOKEN_STRING_ESCAPED) ||
        !out || out_cap == 0 || token->start > token->end)
        return 0;
    input_len = token->end - token->start;
    if (token->type == JSON_TOKEN_STRING) {
        if (input_len >= out_cap || (!allow_empty && input_len == 0u)) return 0;
        memcpy(out, json + token->start, input_len);
        out[input_len] = '\0';
        return 1;
    }
    out[0] = '\0';
    for (cursor = token->start; cursor < token->end;) {
        unsigned char value = json[cursor++];
        if (value == '\\') {
            unsigned char escaped;
            if (cursor >= token->end) return 0;
            escaped = json[cursor++];
            if (escaped == 'u') {
                uint32_t codepoint;
                if (token->end - cursor < 4u || read_hex4(json + cursor, &codepoint) != 0)
                    return 0;
                cursor += 4u;
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                    uint32_t low;
                    if (token->end - cursor < 6u || json[cursor] != '\\' ||
                        json[cursor + 1u] != 'u' || read_hex4(json + cursor + 2u, &low) != 0 ||
                        low < 0xdc00u || low > 0xdfffu) return 0;
                    codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) + low - 0xdc00u;
                    cursor += 6u;
                } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                    return 0;
                }
                if (append_codepoint(codepoint, out, out_cap, &used) != 0) return 0;
                continue;
            }
            if (escaped == 'n') value = '\n';
            else if (escaped == 'r') value = '\r';
            else if (escaped == 't') value = '\t';
            else if (escaped == 'b') value = '\b';
            else if (escaped == 'f') value = '\f';
            else if (escaped == '"' || escaped == '\\' || escaped == '/') value = escaped;
            else return 0;
        }
        if (used + 1u >= out_cap) return 0;
        out[used++] = (char)value;
    }
    out[used] = '\0';
    return allow_empty || used != 0;
}

static int token_bool(const uint8_t *json, const json_token *token, int *out) {
    size_t len;
    if (!json || !token || token->type != JSON_TOKEN_PRIMITIVE || !out) return 0;
    len = token->end - token->start;
    if (len == 4u && memcmp(json + token->start, "true", 4u) == 0) {
        *out = 1;
        return 1;
    }
    if (len == 5u && memcmp(json + token->start, "false", 5u) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int token_u32(const uint8_t *json, const json_token *token, uint32_t *out) {
    uint32_t value = 0;
    size_t cursor;
    if (!json || !token || token->type != JSON_TOKEN_PRIMITIVE || !out ||
        token->start >= token->end) return 0;
    for (cursor = token->start; cursor < token->end; ++cursor) {
        uint32_t digit;
        if (json[cursor] < '0' || json[cursor] > '9') return 0;
        if (cursor != token->start && json[token->start] == '0') return 0;
        digit = (uint32_t)(json[cursor] - '0');
        if (value > (UINT32_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    *out = value;
    return 1;
}

static int safe_identifier(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    size_t len = 0;
    if (!cursor || !*cursor) return 0;
    while (*cursor) {
        if (!(voice_ascii_is_alnum(*cursor) || *cursor == '-' || *cursor == '_' || *cursor == '.' ||
              *cursor == ':')) return 0;
        cursor++;
        if (++len > 127u) return 0;
    }
    return 1;
}

static void turn_request_reset_active(gw_wt_turn_request *out) {
    out->request_id[0] = '\0';
    out->session_id[0] = '\0';
    out->text[0] = '\0';
    out->identity_token[0] = '\0';
    out->enable_rag = 0;
    out->enable_tts = 0;
    out->audio_first = 0;
    out->audio_stream = 0;
    memset(&out->metadata, 0, sizeof(out->metadata));
    out->dnd_initiative.count = 0;
    out->dnd_campaign.operation = 0;
    out->dnd_encounter_action.operation = 0;
    out->meta_budget_ms[0] = '\0';
    out->meta_deadline_unix_ms[0] = '\0';
}

static int turn_request_metadata_field(const char *key, const char *value, void *user) {
    gw_wt_turn_request *out = user;
    size_t length = strlen(value);
    if (strcmp(key, "turn_budget_ms") == 0) {
        if (length >= sizeof(out->meta_budget_ms)) return -1;
        memcpy(out->meta_budget_ms, value, length + 1u);
    } else if (strcmp(key, "turn_deadline_unix_ms") == 0) {
        if (length >= sizeof(out->meta_deadline_unix_ms)) return -1;
        memcpy(out->meta_deadline_unix_ms, value, length + 1u);
    } else if (pb_turn_client_metadata_set(&out->metadata, key, value) < 0) return -1;
    return 0;
}

/* The tokenizer already validates these borrowed spans. Reuse the product
 * policy without copying the control frame or admitting a second key list. */
static int turn_request_metadata(
    const uint8_t *json, const json_tokens *parsed,
    const size_t fields[TURN_REQUEST_FIELD_COUNT], gw_wt_turn_request *out
) {
    cmp_json_object body = {0};
    cmp_turn_metadata_policy policy;
    size_t i;
    static const struct { size_t field; const char *key; } selected[] = {
        {TURN_REQUEST_FIELD_METADATA, "metadata"},
        {TURN_REQUEST_FIELD_CLIENT_TRANSPORT, "client_transport"},
        {TURN_REQUEST_FIELD_DND_INITIATIVE, "dnd_initiative"},
        {TURN_REQUEST_FIELD_DND_CAMPAIGN, "dnd_campaign"},
        {TURN_REQUEST_FIELD_DND_ENCOUNTER_ACTION, "dnd_encounter_action"}
    };
    for (i = 0; i < sizeof(selected) / sizeof(selected[0]); ++i) {
        const json_token *token;
        cmp_json_field *field;
        size_t start, end;
        if (fields[selected[i].field] == SIZE_MAX) continue;
        token = &parsed->tokens[fields[selected[i].field]];
        start = token->start; end = token->end;
        if (token->type == JSON_TOKEN_STRING || token->type == JSON_TOKEN_STRING_ESCAPED) {
            start--; end++;
        }
        field = &body.fields[body.field_count++];
        field->key = selected[i].key;
        field->key_len = strlen(field->key);
        field->value = (const char *)json + start;
        field->value_len = end - start;
    }
    return cmp_turn_metadata_visit(&body, turn_request_metadata_field, out, &policy,
        &out->dnd_initiative, &out->dnd_campaign, &out->dnd_encounter_action) == 0 ?
        GW_WT_OK : GW_WT_ERR_MALFORMED;
}

static int turn_request_parse_impl(
    const uint8_t *json,
    size_t json_len,
    gw_wt_turn_request *out
) {
    json_tokens parsed;
    size_t fields[TURN_REQUEST_FIELD_COUNT];
    size_t metadata;
    size_t token;
    char input_mode[64];
    char audio_protocol[32];
    if (!json || json_len == 0 || json_len > GW_WT_CONTROL_FRAME_CAP)
        return GW_WT_ERR_ARGUMENT;
    if (json_tokenize(json, json_len, &parsed) != 0 ||
        !turn_request_field_tokens(json, &parsed, fields))
        return GW_WT_ERR_MALFORMED;
    if ((fields[TURN_REQUEST_FIELD_METADATA] != SIZE_MAX ||
         fields[TURN_REQUEST_FIELD_CLIENT_TRANSPORT] != SIZE_MAX ||
         fields[TURN_REQUEST_FIELD_DND_INITIATIVE] != SIZE_MAX || fields[TURN_REQUEST_FIELD_DND_CAMPAIGN] != SIZE_MAX ||
         fields[TURN_REQUEST_FIELD_DND_ENCOUNTER_ACTION] != SIZE_MAX) &&
        turn_request_metadata(json, &parsed, fields, out) != GW_WT_OK)
        return GW_WT_ERR_MALFORMED;
    token = fields[TURN_REQUEST_FIELD_REQUEST_ID];
    if (token == SIZE_MAX ||
        !token_string(json, &parsed.tokens[token], out->request_id, sizeof(out->request_id), 0) ||
        !safe_identifier(out->request_id) ||
        fields[TURN_REQUEST_FIELD_IDENTITY_TOKEN] == SIZE_MAX)
        return GW_WT_ERR_MALFORMED;
    token = fields[TURN_REQUEST_FIELD_IDENTITY_TOKEN];
    if (!token_string(json, &parsed.tokens[token], out->identity_token,
                      sizeof(out->identity_token), 0)) return GW_WT_ERR_MALFORMED;
    token = fields[TURN_REQUEST_FIELD_SESSION_ID];
    if (token != SIZE_MAX) {
        if (!token_string(json, &parsed.tokens[token], out->session_id,
                          sizeof(out->session_id), 0) || !safe_identifier(out->session_id))
            return GW_WT_ERR_MALFORMED;
    } else {
        memcpy(out->session_id, out->request_id, strlen(out->request_id) + 1u);
    }
    token = fields[TURN_REQUEST_FIELD_TEXT];
    if (token != SIZE_MAX &&
        !token_string(json, &parsed.tokens[token], out->text, sizeof(out->text), 1))
        return GW_WT_ERR_MALFORMED;
    token = fields[TURN_REQUEST_FIELD_ENABLE_RAG];
    if (token != SIZE_MAX &&
        !token_bool(json, &parsed.tokens[token], &out->enable_rag)) return GW_WT_ERR_MALFORMED;
    token = fields[TURN_REQUEST_FIELD_ENABLE_TTS];
    if (token != SIZE_MAX &&
        !token_bool(json, &parsed.tokens[token], &out->enable_tts)) return GW_WT_ERR_MALFORMED;
    input_mode[0] = '\0';
    audio_protocol[0] = '\0';
    metadata = fields[TURN_REQUEST_FIELD_METADATA];
    if (metadata != SIZE_MAX) {
        if (parsed.tokens[metadata].type != JSON_TOKEN_OBJECT) return GW_WT_ERR_MALFORMED;
        if (object_value_token(json, &parsed, metadata, "input_mode", &token) &&
            !token_string(json, &parsed.tokens[token], input_mode, sizeof(input_mode), 0))
            return GW_WT_ERR_MALFORMED;
        if (object_value_token(json, &parsed, metadata,
                               "client_audio_datagram_protocol", &token) &&
            !token_string(json, &parsed.tokens[token], audio_protocol,
                          sizeof(audio_protocol), 0))
            return GW_WT_ERR_MALFORMED;
    }
    out->audio_stream = strcmp(input_mode, "webtransport_audio_stream_v1") == 0;
    out->audio_first = out->text[0] == '\0' &&
        (strcmp(input_mode, "webtransport_audio") == 0 || out->audio_stream);
    if (out->audio_stream && !out->audio_first) return GW_WT_ERR_MALFORMED;
    if (out->text[0] == '\0' && !out->audio_first) return GW_WT_ERR_MALFORMED;
    if (out->audio_first && strcmp(audio_protocol, "dtvp1") != 0)
        return GW_WT_ERR_MALFORMED;
    return GW_WT_OK;
}

int gw_wt_turn_request_parse(
    const uint8_t *json,
    size_t json_len,
    gw_wt_turn_request *out
) {
    if (!out) return GW_WT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    return turn_request_parse_impl(json, json_len, out);
}

int gw_wt_turn_request_parse_active(
    const uint8_t *json,
    size_t json_len,
    gw_wt_turn_request *out
) {
    if (!out) return GW_WT_ERR_ARGUMENT;
    turn_request_reset_active(out);
    return turn_request_parse_impl(json, json_len, out);
}

int gw_wt_quic_varint_decode(
    const uint8_t *input,
    size_t input_len,
    uint64_t *value,
    size_t *consumed
) {
    size_t width;
    size_t i;
    uint64_t decoded;
    uint64_t minimum;
    if (!input || !value || !consumed || input_len == 0) return GW_WT_ERR_ARGUMENT;
    width = (size_t)1u << (input[0] >> 6u);
    if (width > input_len) return GW_WT_ERR_MALFORMED;
    decoded = input[0] & 0x3fu;
    for (i = 1; i < width; ++i) decoded = decoded << 8u | input[i];
    minimum = width == 1u ? 0u : width == 2u ? 64u : width == 4u ? 16384u : 1073741824u;
    if (decoded < minimum) return GW_WT_ERR_MALFORMED;
    *value = decoded;
    *consumed = width;
    return GW_WT_OK;
}

static int control_parse(const uint8_t *json, size_t json_len, gw_wt_control *out) {
    json_tokens parsed;
    size_t token;
    char type[32];
    if (!out) return GW_WT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (json_tokenize(json, json_len, &parsed) != 0 ||
        !object_value_token(json, &parsed, 0, "type", &token) ||
        !token_string(json, &parsed.tokens[token], type, sizeof(type), 0))
        return GW_WT_ERR_MALFORMED;
    if (strcmp(type, "end") == 0) out->kind = GW_WT_CONTROL_END;
    else if (strcmp(type, "cancel") == 0) out->kind = GW_WT_CONTROL_CANCEL;
    else if (strcmp(type, "interrupt") == 0) out->kind = GW_WT_CONTROL_INTERRUPT;
    else return GW_WT_ERR_MALFORMED;
    if (object_value_token(json, &parsed, 0, "request_id", &token) &&
        (!token_string(json, &parsed.tokens[token], out->request_id,
                       sizeof(out->request_id), 0) || !safe_identifier(out->request_id)))
        return GW_WT_ERR_MALFORMED;
    if (object_value_token(json, &parsed, 0, "reason", &token) &&
        !token_string(json, &parsed.tokens[token], out->reason, sizeof(out->reason), 0))
        return GW_WT_ERR_MALFORMED;
    if (out->kind == GW_WT_CONTROL_END &&
        (!object_value_token(json, &parsed, 0, "packet_count", &token) ||
         !token_u32(json, &parsed.tokens[token], &out->packet_count) ||
         out->packet_count == 0u ||
         out->packet_count > GW_WT_MAX_AUDIO_BYTES / 2u ||
         !object_value_token(json, &parsed, 0, "audio_bytes", &token) ||
         !token_u32(json, &parsed.tokens[token], &out->audio_bytes) ||
         out->audio_bytes == 0u || out->audio_bytes > GW_WT_MAX_AUDIO_BYTES ||
         out->audio_bytes % 2u != 0u ||
         (uint64_t)out->audio_bytes < (uint64_t)out->packet_count * 2u ||
         (uint64_t)out->audio_bytes >
             (uint64_t)out->packet_count * GW_WT_AUDIO_PAYLOAD_CAP))
        return GW_WT_ERR_MALFORMED;
    return GW_WT_OK;
}

static int audio_packet_parse(const uint8_t *payload, size_t payload_len,
                             gw_wt_datagram *out) {
    if (!payload || payload_len <= GW_WT_AUDIO_HEADER_BYTES ||
        payload_len > GW_WT_DATAGRAM_CAP ||
        memcmp(payload, GW_WT_AUDIO_PREFIX, sizeof(GW_WT_AUDIO_PREFIX) - 1u) != 0)
        return GW_WT_ERR_MALFORMED;
    out->sequence = ((uint32_t)payload[5] << 24u) |
                    ((uint32_t)payload[6] << 16u) |
                    ((uint32_t)payload[7] << 8u) | (uint32_t)payload[8];
    out->payload = payload + GW_WT_AUDIO_HEADER_BYTES;
    out->payload_len = payload_len - GW_WT_AUDIO_HEADER_BYTES;
    if (out->payload_len == 0u || out->payload_len > GW_WT_AUDIO_PAYLOAD_CAP ||
        out->payload_len % 2u != 0u) return GW_WT_ERR_MALFORMED;
    out->is_audio = 1;
    return GW_WT_OK;
}

int gw_wt_datagram_parse(const uint8_t *input, size_t input_len, gw_wt_datagram *out) {
    uint64_t quarter_stream_id;
    size_t context_len;
    const uint8_t *payload;
    size_t payload_len;
    if (!out) return GW_WT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!input || input_len == 0 || input_len > GW_WT_DATAGRAM_CAP + 8u ||
        gw_wt_quic_varint_decode(input, input_len, &quarter_stream_id, &context_len) != GW_WT_OK ||
        quarter_stream_id > (UINT64_MAX >> 2u)) return GW_WT_ERR_MALFORMED;
    payload = input + context_len;
    payload_len = input_len - context_len;
    if (payload_len == 0 || payload_len > GW_WT_DATAGRAM_CAP) return GW_WT_ERR_CAPACITY;
    out->session_stream_id = quarter_stream_id << 2u;
    if (payload_len >= sizeof(GW_WT_CONTROL_PREFIX) - 1u &&
        memcmp(payload, GW_WT_CONTROL_PREFIX, sizeof(GW_WT_CONTROL_PREFIX) - 1u) == 0) {
        size_t control_len = payload_len - (sizeof(GW_WT_CONTROL_PREFIX) - 1u);
        if (control_len == 0 || control_len > 512u ||
            control_parse(payload + sizeof(GW_WT_CONTROL_PREFIX) - 1u,
                          control_len, &out->control) != GW_WT_OK)
            return GW_WT_ERR_MALFORMED;
        out->is_control = 1;
    } else {
        return audio_packet_parse(payload, payload_len, out);
    }
    return GW_WT_OK;
}

int gw_wt_stream_input_parse(uint8_t frame_type, const uint8_t *payload,
                           size_t payload_len, gw_wt_datagram *out) {
    if (!out) return GW_WT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (frame_type == GW_WT_FRAME_AUDIO_PACKET)
        return audio_packet_parse(payload, payload_len, out);
    if (frame_type != 1u || !payload || !payload_len || payload_len > 512u ||
        control_parse(payload, payload_len, &out->control) != GW_WT_OK ||
        !out->control.request_id[0]) return GW_WT_ERR_MALFORMED;
    out->is_control = 1;
    return GW_WT_OK;
}

int gw_wt_audio_reorder_insert(
    gw_wt_audio_reorder *window,
    uint32_t sequence,
    const uint8_t *payload,
    size_t payload_len
) {
    gw_wt_audio_slot *slot;
    uint32_t distance;
    if (!window || !payload || payload_len == 0u ||
        payload_len > GW_WT_AUDIO_PAYLOAD_CAP || payload_len % 2u != 0u ||
        sequence >= GW_WT_MAX_AUDIO_BYTES / 2u)
        return GW_WT_AUDIO_INVALID;
    if (sequence < window->next_sequence) {
        window->duplicate_datagrams++;
        return GW_WT_AUDIO_DUPLICATE;
    }
    distance = sequence - window->next_sequence;
    if (distance >= GW_WT_AUDIO_REORDER_WINDOW)
        return GW_WT_AUDIO_OUTSIDE_WINDOW;
    slot = &window->slots[sequence % GW_WT_AUDIO_REORDER_WINDOW];
    if (slot->present) {
        if (slot->sequence != sequence || slot->payload_len != payload_len ||
            memcmp(slot->payload, payload, payload_len) != 0)
            return GW_WT_AUDIO_CONFLICT;
        window->duplicate_datagrams++;
        return GW_WT_AUDIO_DUPLICATE;
    }
    if (payload_len > GW_WT_MAX_AUDIO_BYTES - window->unique_audio_bytes)
        return GW_WT_AUDIO_LIMIT;
    slot->sequence = sequence;
    slot->payload_len = (uint16_t)payload_len;
    memcpy(slot->payload, payload, payload_len);
    slot->present = 1u;
    window->buffered_datagrams++;
    window->unique_datagrams++;
    window->unique_audio_bytes += payload_len;
    if (distance != 0u) window->reordered_datagrams++;
    if (sequence + 1u > window->highest_sequence_plus_one)
        window->highest_sequence_plus_one = sequence + 1u;
    return GW_WT_AUDIO_INSERTED;
}

int gw_wt_audio_reorder_peek(
    const gw_wt_audio_reorder *window,
    const uint8_t **payload,
    size_t *payload_len
) {
    const gw_wt_audio_slot *slot;
    if (!window || !payload || !payload_len) return 0;
    slot = &window->slots[window->next_sequence % GW_WT_AUDIO_REORDER_WINDOW];
    if (!slot->present || slot->sequence != window->next_sequence) return 0;
    *payload = slot->payload;
    *payload_len = slot->payload_len;
    return 1;
}

int gw_wt_audio_reorder_pop(gw_wt_audio_reorder *window) {
    gw_wt_audio_slot *slot;
    if (!window) return 0;
    slot = &window->slots[window->next_sequence % GW_WT_AUDIO_REORDER_WINDOW];
    if (!slot->present || slot->sequence != window->next_sequence) return 0;
    memset(slot, 0, sizeof(*slot));
    window->buffered_datagrams--;
    window->next_sequence++;
    return 1;
}

uint64_t gw_wt_audio_gap_deadline(
    uint64_t deadline_ms,
    uint32_t previous_sequence,
    uint32_t next_sequence,
    int has_gap,
    uint64_t now_ms
) {
    if (!has_gap) return 0u;
    if (deadline_ms != 0u && next_sequence <= previous_sequence) return deadline_ms;
    return now_ms > UINT64_MAX - GW_WT_AUDIO_GAP_TIMEOUT_MS ?
        UINT64_MAX : now_ms + GW_WT_AUDIO_GAP_TIMEOUT_MS;
}

int gw_wt_audio_reorder_commit_state(
    const gw_wt_audio_reorder *window,
    uint32_t expected_datagrams,
    uint32_t expected_audio_bytes
) {
    if (!window || expected_datagrams == 0u || expected_audio_bytes == 0u ||
        expected_datagrams > GW_WT_MAX_AUDIO_BYTES / 2u ||
        expected_audio_bytes > GW_WT_MAX_AUDIO_BYTES ||
        window->highest_sequence_plus_one > expected_datagrams ||
        window->unique_datagrams > expected_datagrams ||
        window->unique_audio_bytes > expected_audio_bytes)
        return GW_WT_AUDIO_COMMIT_MISMATCH;
    if (window->next_sequence != expected_datagrams ||
        window->buffered_datagrams != 0u)
        return GW_WT_AUDIO_COMMIT_WAIT;
    return window->unique_datagrams == expected_datagrams &&
                   window->unique_audio_bytes == expected_audio_bytes
               ? GW_WT_AUDIO_COMMIT_READY
               : GW_WT_AUDIO_COMMIT_MISMATCH;
}

int gw_wt_audio_delivery_mark_endpoint(gw_wt_audio_delivery *delivery) {
    if (!delivery) return -1;
    if (delivery->endpoint_seen) return 0;
    delivery->endpoint_seen = 1;
    return 1;
}

int gw_wt_audio_delivery_should_forward(const gw_wt_audio_delivery *delivery) {
    return delivery && !delivery->endpoint_seen;
}

int gw_wt_audio_delivery_record(
    gw_wt_audio_delivery *delivery,
    int forwarded,
    size_t audio_bytes
) {
    uint32_t *datagrams;
    size_t *bytes;
    if (!delivery || (forwarded != 0 && forwarded != 1) || audio_bytes == 0u ||
        audio_bytes > GW_WT_AUDIO_PAYLOAD_CAP || audio_bytes % 2u != 0u)
        return GW_WT_ERR_ARGUMENT;
    if (forwarded != gw_wt_audio_delivery_should_forward(delivery))
        return GW_WT_ERR_MALFORMED;
    datagrams = forwarded ? &delivery->forwarded_datagrams :
                            &delivery->drained_datagrams;
    bytes = forwarded ? &delivery->forwarded_audio_bytes :
                        &delivery->drained_audio_bytes;
    if (*datagrams == UINT32_MAX || audio_bytes > GW_WT_MAX_AUDIO_BYTES - *bytes)
        return GW_WT_ERR_CAPACITY;
    (*datagrams)++;
    *bytes += audio_bytes;
    return GW_WT_OK;
}

int gw_wt_audio_delivery_matches(
    const gw_wt_audio_delivery *delivery,
    uint32_t received_datagrams,
    size_t received_audio_bytes
) {
    uint64_t delivered_datagrams;
    size_t delivered_audio_bytes;
    if (!delivery || received_datagrams == 0u || received_audio_bytes == 0u ||
        received_audio_bytes > GW_WT_MAX_AUDIO_BYTES ||
        delivery->forwarded_audio_bytes > GW_WT_MAX_AUDIO_BYTES ||
        delivery->drained_audio_bytes >
            GW_WT_MAX_AUDIO_BYTES - delivery->forwarded_audio_bytes)
        return 0;
    delivered_datagrams = (uint64_t)delivery->forwarded_datagrams +
                          (uint64_t)delivery->drained_datagrams;
    delivered_audio_bytes = delivery->forwarded_audio_bytes +
                            delivery->drained_audio_bytes;
    return delivered_datagrams == received_datagrams &&
           delivered_audio_bytes == received_audio_bytes;
}

int gw_wt_transcript_control(
    const turn_start_c *turn, const char *request_id, const char *session_id,
    const char *user_id, char *out, size_t out_cap, size_t *out_len
) {
    char escaped[sizeof(turn->text) * 6u];
    size_t text_len;
    int written;
    if (!out_len) return GW_WT_ERR_ARGUMENT;
    *out_len = 0u;
    if (!turn || !request_id || !session_id || !user_id || !out || !out_cap)
        return GW_WT_ERR_ARGUMENT;
    out[0] = '\0';
    if (!memchr(turn->request_id, '\0', sizeof(turn->request_id)) ||
        !memchr(turn->session_id, '\0', sizeof(turn->session_id)) ||
        !memchr(turn->user_id, '\0', sizeof(turn->user_id)) ||
        !memchr(turn->text, '\0', sizeof(turn->text)) ||
        !safe_identifier(turn->request_id) || !turn->session_id[0] || !turn->user_id[0] ||
        strcmp(turn->request_id, request_id) != 0 ||
        strcmp(turn->session_id, session_id) != 0 || strcmp(turn->user_id, user_id) != 0)
        return GW_WT_ERR_MALFORMED;
    text_len = strlen(turn->text);
    if (!text_len || strspn(turn->text, " \t\r\n") == text_len ||
        !utf8_validate_v1((const uint8_t *)turn->text, text_len) ||
        cmp_json_escape_exact(turn->text, escaped, sizeof(escaped)) < 0)
        return GW_WT_ERR_MALFORMED;
    written = snprintf(out, out_cap,
        "{\"type\":\"transcript\",\"protocol_version\":\"turnstream.v1alpha1\","
        "\"request_id\":\"%s\",\"text\":\"%s\",\"is_final\":true}",
        request_id, escaped);
    if (written <= 0 || (size_t)written >= out_cap) {
        out[0] = '\0';
        return GW_WT_ERR_CAPACITY;
    }
    *out_len = (size_t)written;
    return GW_WT_OK;
}
