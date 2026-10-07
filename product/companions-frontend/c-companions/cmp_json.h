/* cmp_json.h — tiny JSON field helpers for c-companions (no external JSON lib). */
#ifndef C_COMPANIONS_CMP_JSON_H
#define C_COMPANIONS_CMP_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMP_JSON_OBJECT_FIELDS_MAX 32u

typedef struct {
    const char *key;
    const char *value;
    size_t key_len;
    size_t value_len;
    int key_escaped;
} cmp_json_field;

typedef struct {
    cmp_json_field fields[CMP_JSON_OBJECT_FIELDS_MAX];
    size_t field_count;
} cmp_json_object;

typedef struct {
    const char *cursor;
    const char *end;
} cmp_json_array;

typedef struct {
    const char *cursor;
    const char *end;
} cmp_json_members;

/* Build a bounded directory of one object's top-level fields. */
int cmp_json_object_parse(const char *json, cmp_json_object *object);

/* Directory getters reject duplicate keys and type mismatches. */
int cmp_json_object_key_count(const cmp_json_object *object, const char *key);
const cmp_json_field *cmp_json_object_field(const cmp_json_object *object, const char *key);
int cmp_json_object_str(
    const cmp_json_object *object,
    const char *key,
    char *out,
    size_t cap
);
int cmp_json_object_i64(const cmp_json_object *object, const char *key, int64_t *out);
int cmp_json_object_bool(const cmp_json_object *object, const char *key, int *out);

/* Decode one parsed string field. The field must come from a parsed object. */
int cmp_json_field_str(const cmp_json_field *field, char *out, size_t cap);
int cmp_json_field_object(const cmp_json_field *field, cmp_json_object *out);
int cmp_json_field_double(const cmp_json_field *field, double *out);

/* Arrays borrow validated JSON spans. No element directory or heap is needed.
 * next returns 1 for an element, 0 at the end, and -1 for invalid state. */
int cmp_json_array_parse(const char *json, cmp_json_array *out);
int cmp_json_field_array(const cmp_json_field *field, cmp_json_array *out);
int cmp_json_array_next(cmp_json_array *array, cmp_json_field *element);

/* Iterate a parsed object span without enlarging the fixed field directory.
 * next returns 1, 0 at the end, or -1 on invalid state. Keys retain escapes;
 * the caller must decode and check key uniqueness for its map policy. */
int cmp_json_field_members(const cmp_json_field *field, cmp_json_members *out);
int cmp_json_members_next(cmp_json_members *members, cmp_json_field *field);

/* Parse one unique object-valued member without copying its bounded span. */
int cmp_json_object_object(
    const cmp_json_object *object,
    const char *key,
    cmp_json_object *out
);

/* Extract JSON string value for "key" into out (unescaped simple; no \u). Returns 1 if found. */
int cmp_json_str(const char *json, const char *key, char *out, size_t cap);

/* Extract JSON integer (signed) for "key". Returns 1 if found. */
int cmp_json_i64(const char *json, const char *key, int64_t *out);

/* Extract JSON bool true/false for "key". Returns 1 if found. */
int cmp_json_bool(const char *json, const char *key, int *out);

/* Count exact top-level object keys. Returns -1 for malformed structure. */
int cmp_json_top_level_key_count(const char *json, const char *key);

/* Validate one complete JSON object whose values are scalar JSON values. */
int cmp_json_flat_object_valid(const char *json);

/* Escape string into JSON string content (no surrounding quotes). */
int cmp_json_escape(const char *in, char *out, size_t cap);

/* Preserve every UTF-8 input byte, including controls, or clear out and fail.
 * Returns 0 on success and -1 on invalid input or insufficient capacity. */
int cmp_json_escape_exact(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
