/* cmp_json.c — minimal JSON field extraction for policy routes. */
#define _POSIX_C_SOURCE 200809L
#include "cmp_json.h"
#include "utf8.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_key(const char *json, const char *key);

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

static int json_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int skip_json_string(const char **cursor) {
    const char *p;
    if (!cursor || !*cursor || **cursor != '"') return 0;
    p = *cursor + 1;
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *cursor = p;
            return 1;
        }
        if (c < 0x20u) return 0;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                int i;
                for (i = 0; i < 4; ++i) {
                    if (!*p || !json_hex((unsigned char)*p)) return 0;
                    ++p;
                }
            } else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' &&
                       c != 'n' && c != 'r' && c != 't') {
                return 0;
            }
        }
    }
    return 0;
}

static int skip_json_number(const char **cursor) {
    const char *p;
    if (!cursor || !*cursor) return 0;
    p = *cursor;
    if (*p == '-') ++p;
    if (*p == '0') {
        ++p;
        if (*p >= '0' && *p <= '9') return 0;
    } else if (*p >= '1' && *p <= '9') {
        do {
            ++p;
        } while (*p >= '0' && *p <= '9');
    } else {
        return 0;
    }
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return 0;
        do {
            ++p;
        } while (*p >= '0' && *p <= '9');
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (*p < '0' || *p > '9') return 0;
        do {
            ++p;
        } while (*p >= '0' && *p <= '9');
    }
    *cursor = p;
    return 1;
}

static int skip_json_value(const char **cursor, unsigned depth);

static int skip_json_object(const char **cursor, unsigned depth) {
    const char *p;
    if (!cursor || !*cursor || **cursor != '{' || depth >= 32u) return 0;
    p = skip_ws(*cursor + 1);
    if (*p == '}') {
        *cursor = p + 1;
        return 1;
    }
    for (;;) {
        if (!skip_json_string(&p)) return 0;
        p = skip_ws(p);
        if (*p++ != ':') return 0;
        p = skip_ws(p);
        if (!skip_json_value(&p, depth + 1u)) return 0;
        p = skip_ws(p);
        if (*p == '}') {
            *cursor = p + 1;
            return 1;
        }
        if (*p++ != ',') return 0;
        p = skip_ws(p);
    }
}

static int skip_json_array(const char **cursor, unsigned depth) {
    const char *p;
    if (!cursor || !*cursor || **cursor != '[' || depth >= 32u) return 0;
    p = skip_ws(*cursor + 1);
    if (*p == ']') {
        *cursor = p + 1;
        return 1;
    }
    for (;;) {
        if (!skip_json_value(&p, depth + 1u)) return 0;
        p = skip_ws(p);
        if (*p == ']') {
            *cursor = p + 1;
            return 1;
        }
        if (*p++ != ',') return 0;
        p = skip_ws(p);
    }
}

static int skip_json_value(const char **cursor, unsigned depth) {
    const char *p;
    if (!cursor || !*cursor || depth >= 32u) return 0;
    p = *cursor;
    if (*p == '"') return skip_json_string(cursor);
    if (*p == '{') return skip_json_object(cursor, depth);
    if (*p == '[') return skip_json_array(cursor, depth);
    if (strncmp(p, "true", 4) == 0) {
        *cursor = p + 4;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *cursor = p + 5;
        return 1;
    }
    if (strncmp(p, "null", 4) == 0) {
        *cursor = p + 4;
        return 1;
    }
    return skip_json_number(cursor);
}

static int parse_json_object(const char **cursor, cmp_json_object *object) {
    const char *p;
    if (!object) return 0;
    memset(object, 0, sizeof(*object));
    if (!cursor || !*cursor) return 0;
    p = skip_ws(*cursor);
    if (*p++ != '{') return 0;
    p = skip_ws(p);
    if (*p == '}') {
        *cursor = p + 1;
        return 1;
    }
    for (;;) {
        const char *key_start;
        const char *key_end;
        const char *key_cursor;
        const char *value;
        cmp_json_field *field;
        if (object->field_count >= CMP_JSON_OBJECT_FIELDS_MAX) return 0;
        if (*p != '"') return 0;
        key_start = p + 1;
        key_cursor = p;
        if (!skip_json_string(&key_cursor)) return 0;
        key_end = key_cursor - 1;
        p = key_cursor;
        p = skip_ws(p);
        if (*p++ != ':') return 0;
        value = skip_ws(p);
        p = value;
        if (!skip_json_value(&p, 1u)) return 0;
        field = &object->fields[object->field_count++];
        field->key = key_start;
        field->key_len = (size_t)(key_end - key_start);
        field->key_escaped = memchr(key_start, '\\', field->key_len) != NULL;
        field->value_len = (size_t)(p - value);
        if (field->key_escaped) return 0;
        field->value = value;
        p = skip_ws(p);
        if (*p == '}') {
            *cursor = p + 1;
            return 1;
        }
        if (*p++ != ',') return 0;
        p = skip_ws(p);
    }
}

int cmp_json_object_parse(const char *json, cmp_json_object *object) {
    const char *p;
    if (!json || !utf8_validate_v1((const uint8_t *)json, strlen(json))) return 0;
    p = json;
    if (!parse_json_object(&p, object)) return 0;
    return *skip_ws(p) == '\0';
}

static const char *object_value(
    const cmp_json_object *object,
    const char *key,
    int *count_out
) {
    const char *value = NULL;
    size_t key_len;
    size_t i;
    int count = 0;
    if (count_out) *count_out = 0;
    if (!object || !key || !key[0]) return NULL;
    key_len = strlen(key);
    for (i = 0; i < object->field_count; ++i) {
        const cmp_json_field *field = &object->fields[i];
        if (!field->key_escaped && field->key_len == key_len &&
            memcmp(field->key, key, key_len) == 0) {
            if (count == 0) value = field->value;
            count++;
        }
    }
    if (count_out) *count_out = count;
    return value;
}

int cmp_json_object_key_count(const cmp_json_object *object, const char *key) {
    int count = 0;
    (void)object_value(object, key, &count);
    return count;
}

const cmp_json_field *cmp_json_object_field(const cmp_json_object *object, const char *key) {
    const cmp_json_field *found = NULL;
    size_t length, i;
    if (!object || !key || object->field_count > CMP_JSON_OBJECT_FIELDS_MAX) return NULL;
    length = strlen(key);
    for (i = 0; i < object->field_count; ++i) {
        const cmp_json_field *field = &object->fields[i];
        if (!field->key_escaped && field->key && field->key_len == length &&
            memcmp(field->key, key, length) == 0) {
            if (found) return NULL;
            found = field;
        }
    }
    return found;
}

int cmp_json_field_object(const cmp_json_field *field, cmp_json_object *out) {
    const char *cursor;
    if (!field || !field->value || !out || field->value_len < 2u) return 0;
    cursor = field->value;
    return parse_json_object(&cursor, out) &&
        cursor == field->value + field->value_len;
}

int cmp_json_field_array(const cmp_json_field *field, cmp_json_array *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!field || !field->value || field->value_len < 2u ||
        field->value[0] != '[' || field->value[field->value_len - 1u] != ']') return 0;
    out->cursor = field->value + 1u;
    out->end = field->value + field->value_len - 1u;
    return 1;
}

int cmp_json_array_parse(const char *json, cmp_json_array *out) {
    const char *start, *cursor;
    cmp_json_field field = {0};
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!json || !utf8_validate_v1((const uint8_t *)json, strlen(json))) return 0;
    start = skip_ws(json);
    cursor = start;
    if (!skip_json_array(&cursor, 0u) || *skip_ws(cursor) != '\0') return 0;
    field.value = start;
    field.value_len = (size_t)(cursor - start);
    return cmp_json_field_array(&field, out);
}

int cmp_json_array_next(cmp_json_array *array, cmp_json_field *element) {
    const char *start, *cursor;
    if (!array || !element || !array->cursor || !array->end ||
        array->cursor > array->end) return -1;
    memset(element, 0, sizeof(*element));
    start = skip_ws(array->cursor);
    if (start == array->end) return 0;
    if (start > array->end) return -1;
    cursor = start;
    if (!skip_json_value(&cursor, 1u) || cursor > array->end) return -1;
    element->value = start;
    element->value_len = (size_t)(cursor - start);
    cursor = skip_ws(cursor);
    if (cursor < array->end && *cursor == ',') {
        cursor = skip_ws(cursor + 1u);
        if (cursor >= array->end) return -1;
    } else if (cursor != array->end) return -1;
    array->cursor = cursor;
    return 1;
}

int cmp_json_field_members(const cmp_json_field *field, cmp_json_members *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!field || !field->value || field->value_len < 2u ||
        field->value[0] != '{' || field->value[field->value_len - 1u] != '}') return 0;
    out->cursor = field->value + 1u;
    out->end = field->value + field->value_len - 1u;
    return 1;
}

int cmp_json_members_next(cmp_json_members *members, cmp_json_field *field) {
    const char *start, *cursor;
    if (!members || !field || !members->cursor || !members->end ||
        members->cursor > members->end) return -1;
    memset(field, 0, sizeof(*field));
    start = skip_ws(members->cursor);
    if (start == members->end) return 0;
    if (start > members->end) return -1;
    cursor = start;
    if (!skip_json_string(&cursor) || cursor > members->end) return -1;
    field->key = start + 1u;
    field->key_len = (size_t)(cursor - start) - 2u;
    field->key_escaped = memchr(field->key, '\\', field->key_len) != NULL;
    cursor = skip_ws(cursor);
    if (cursor >= members->end || *cursor++ != ':') return -1;
    field->value = skip_ws(cursor);
    cursor = field->value;
    if (!skip_json_value(&cursor, 1u) || cursor > members->end) return -1;
    field->value_len = (size_t)(cursor - field->value);
    cursor = skip_ws(cursor);
    if (cursor < members->end && *cursor == ',') {
        cursor = skip_ws(cursor + 1u);
        if (cursor >= members->end) return -1;
    } else if (cursor != members->end) return -1;
    members->cursor = cursor;
    return 1;
}

int cmp_json_field_double(const cmp_json_field *field, double *out) {
    char number[64], *end;
    const char *cursor;
    double value;
    if (!field || !field->value || !out || !field->value_len ||
        field->value_len >= sizeof(number)) return 0;
    memcpy(number, field->value, field->value_len);
    number[field->value_len] = '\0';
    cursor = number;
    if (!skip_json_number(&cursor) || *cursor != '\0') return 0;
    errno = 0;
    value = strtod(number, &end);
    if (errno || *end != '\0' || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

static int scan_top_level_key(
    const char *json,
    const char *key,
    const char **value_out,
    int *count_out
) {
    cmp_json_object object;
    const char *value;
    int count = 0;
    if (value_out) *value_out = NULL;
    if (count_out) *count_out = 0;
    if (!cmp_json_object_parse(json, &object)) return 0;
    value = object_value(&object, key, &count);
    if (value_out) *value_out = value;
    if (count_out) *count_out = count;
    return 1;
}

static const char *find_key(const char *json, const char *key) {
    const char *value = NULL;
    int count = 0;
    if (!scan_top_level_key(json, key, &value, &count) || count != 1) return NULL;
    return value;
}

int cmp_json_flat_object_valid(const char *json) {
    const char *p;
    if (!json) return 0;
    p = skip_ws(json);
    if (!p || !utf8_validate_v1((const uint8_t *)json, strlen(json)) || *p++ != '{') return 0;
    p = skip_ws(p);
    if (!p) return 0;
    if (*p == '}') return *skip_ws(p + 1) == '\0';
    for (;;) {
        if (!skip_json_string(&p)) return 0;
        p = skip_ws(p);
        if (!p) return 0;
        if (*p++ != ':') return 0;
        p = skip_ws(p);
        if (!p) return 0;
        if (*p == '"') {
            if (!skip_json_string(&p)) return 0;
        } else if (strncmp(p, "true", 4) == 0) {
            p += 4;
        } else if (strncmp(p, "false", 5) == 0) {
            p += 5;
        } else if (strncmp(p, "null", 4) == 0) {
            p += 4;
        } else if (!skip_json_number(&p)) {
            return 0;
        }
        p = skip_ws(p);
        if (!p) return 0;
        if (*p == ',') {
            p = skip_ws(p + 1);
            if (!p) return 0;
            continue;
        }
        if (*p == '}') return *skip_ws(p + 1) == '\0';
        return 0;
    }
}

int cmp_json_top_level_key_count(const char *json, const char *key) {
    int count = 0;
    return scan_top_level_key(json, key, NULL, &count) ? count : -1;
}

static int json_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int json_code_unit(const char *input, uint32_t *value) {
    uint32_t result = 0;
    int i;
    if (!input || !value) return 0;
    for (i = 0; i < 4; ++i) {
        int digit = json_hex_value((unsigned char)input[i]);
        if (digit < 0) return 0;
        result = result * 16u + (uint32_t)digit;
    }
    *value = result;
    return 1;
}

static int append_codepoint(uint32_t codepoint, char *out, size_t cap, size_t *used) {
    unsigned char encoded[4];
    size_t length;
    if (codepoint == 0) return 0;
    if (codepoint <= 0x7fu) {
        encoded[0] = (unsigned char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ffu) {
        encoded[0] = (unsigned char)(0xc0u | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 2;
    } else if (codepoint <= 0xffffu) {
        if (codepoint >= 0xd800u && codepoint <= 0xdfffu) return 0;
        encoded[0] = (unsigned char)(0xe0u | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 3;
    } else if (codepoint <= 0x10ffffu) {
        encoded[0] = (unsigned char)(0xf0u | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[3] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 4;
    } else {
        return 0;
    }
    if (*used > cap - 1u || length > cap - 1u - *used) return 0;
    memcpy(out + *used, encoded, length);
    *used += length;
    return 1;
}

static int decode_string_value(const char *p, char *out, size_t cap) {
    size_t i = 0;
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (*p != '"')
        return 0;
    p++;
    while (*p && *p != '"') {
        uint32_t codepoint;
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20u) return 0;
        if (c != '\\') {
            if (i + 1u >= cap) return 0;
            out[i++] = (char)c;
            continue;
        }
        c = (unsigned char)*p++;
        if (c == '"' || c == '\\' || c == '/') codepoint = c;
        else if (c == 'b') codepoint = '\b';
        else if (c == 'f') codepoint = '\f';
        else if (c == 'n') codepoint = '\n';
        else if (c == 'r') codepoint = '\r';
        else if (c == 't') codepoint = '\t';
        else if (c == 'u') {
            uint32_t first;
            if (!json_code_unit(p, &first)) return 0;
            p += 4;
            if (first >= 0xd800u && first <= 0xdbffu) {
                uint32_t second;
                if (*p != '\\') return 0;
                ++p;
                if (*p != 'u') return 0;
                ++p;
                if (!json_code_unit(p, &second) ||
                    second < 0xdc00u || second > 0xdfffu) return 0;
                p += 4;
                codepoint = UINT32_C(0x10000) + ((first - UINT32_C(0xd800)) << 10) +
                    (second - UINT32_C(0xdc00));
            } else if (first >= 0xdc00u && first <= 0xdfffu) {
                return 0;
            } else {
                codepoint = first;
            }
        } else {
            return 0;
        }
        if (!append_codepoint(codepoint, out, cap, &i)) return 0;
    }
    out[i] = '\0';
    return (*p == '"') ? 1 : 0;
}

int cmp_json_str(const char *json, const char *key, char *out, size_t cap) {
    const char *value = find_key(json, key);
    if (!value) {
        if (out && cap != 0) out[0] = '\0';
        return 0;
    }
    return decode_string_value(value, out, cap);
}

int cmp_json_object_str(
    const cmp_json_object *object,
    const char *key,
    char *out,
    size_t cap
) {
    int count = 0;
    const char *value = object_value(object, key, &count);
    if (count != 1 || !value) {
        if (out && cap != 0) out[0] = '\0';
        return 0;
    }
    return decode_string_value(value, out, cap);
}

static int scalar_ends(const char *p) {
    p = skip_ws(p);
    return *p == ',' || *p == '}';
}

static int decode_i64_value(const char *p, int64_t *out) {
    uint64_t magnitude = 0;
    uint64_t limit;
    int negative = 0;
    if (!out)
        return 0;
    if (*p == '-') {
        negative = 1;
        ++p;
    }
    if (*p < '0' || *p > '9') return 0;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return 0;
    limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (magnitude > (limit - digit) / 10u) return 0;
        magnitude = magnitude * 10u + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    if (!scalar_ends(p)) return 0;
    if (negative && magnitude == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
    else if (negative) *out = -(int64_t)magnitude;
    else *out = (int64_t)magnitude;
    return 1;
}

int cmp_json_i64(const char *json, const char *key, int64_t *out) {
    const char *value = find_key(json, key);
    return value ? decode_i64_value(value, out) : 0;
}

int cmp_json_object_i64(const cmp_json_object *object, const char *key, int64_t *out) {
    int count = 0;
    const char *value = object_value(object, key, &count);
    return count == 1 && value ? decode_i64_value(value, out) : 0;
}

static int decode_bool_value(const char *p, int *out) {
    if (!out)
        return 0;
    if (strncmp(p, "true", 4) == 0 && scalar_ends(p + 4)) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0 && scalar_ends(p + 5)) {
        *out = 0;
        return 1;
    }
    return 0;
}

int cmp_json_bool(const char *json, const char *key, int *out) {
    const char *value = find_key(json, key);
    return value ? decode_bool_value(value, out) : 0;
}

int cmp_json_object_bool(const cmp_json_object *object, const char *key, int *out) {
    int count = 0;
    const char *value = object_value(object, key, &count);
    return count == 1 && value ? decode_bool_value(value, out) : 0;
}

int cmp_json_field_str(const cmp_json_field *field, char *out, size_t cap) {
    if (!field || !field->value || field->value_len < 2u ||
        field->value[0] != '"' || field->value[field->value_len - 1u] != '"') {
        if (out && cap != 0) out[0] = '\0';
        return 0;
    }
    return decode_string_value(field->value, out, cap);
}

int cmp_json_object_object(
    const cmp_json_object *object,
    const char *key,
    cmp_json_object *out
) {
    const cmp_json_field *field = NULL;
    const char *end;
    size_t key_len;
    size_t i;
    int count = 0;
    if (!object || !key || !key[0] || !out) return 0;
    key_len = strlen(key);
    for (i = 0; i < object->field_count; ++i) {
        const cmp_json_field *candidate = &object->fields[i];
        if (!candidate->key_escaped && candidate->key_len == key_len &&
            memcmp(candidate->key, key, key_len) == 0) {
            field = candidate;
            count++;
        }
    }
    if (count != 1 || !field || field->value_len < 2u || field->value[0] != '{')
        return 0;
    end = field->value;
    if (!parse_json_object(&end, out)) return 0;
    return (size_t)(end - field->value) == field->value_len;
}

int cmp_json_escape_exact(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t i, used = 0;
    if (!out || !cap) return -1;
    out[0] = '\0';
    if (!in || !utf8_validate_v1((const uint8_t *)in, strlen(in))) return -1;
    for (i = 0; in[i]; ++i) {
        unsigned char c = (unsigned char)in[i];
        size_t needed = c < 0x20u ? 6u : (c == '"' || c == '\\') ? 2u : 1u;
        if (needed > cap - 1u - used) { out[0] = '\0'; return -1; }
        if (c < 0x20u) {
            memcpy(out + used, "\\u00", 4u);
            out[used + 4u] = hex[c >> 4u];
            out[used + 5u] = hex[c & 15u];
            used += 6u;
        } else {
            if (needed == 2u) out[used++] = '\\';
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    return 0;
}

int cmp_json_escape(const char *in, char *out, size_t cap) {
    size_t i = 0, j = 0;
    if (!out || cap == 0)
        return -1;
    if (!in)
        in = "";
    while (in[i] && j + 2 < cap) {
        unsigned char c = (unsigned char)in[i++];
        if (c == '"' || c == '\\') {
            if (j + 3 >= cap)
                break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 3 >= cap)
                break;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c < 0x20) {
            /* skip controls */
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return 0;
}
