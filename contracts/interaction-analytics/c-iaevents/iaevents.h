/* iaevents.h — pure-C interaction-analytics schema catalog + validate (no Go). */
#ifndef INTERACTION_ANALYTICS_IAEVENTS_H
#define INTERACTION_ANALYTICS_IAEVENTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    IA_OK = 0,
    IA_ERR = 1
};

#define IA_VERSION "interaction.analytics.events.v1"
#define IA_SUBJECT_PREFIX "analytics.events.interaction"
#define IA_MAX_TYPES 16
#define IA_STR 64
#define IA_PATH 128
#define IA_REQ 16
#define IA_PART 8
#define IA_MSG 192
#define IA_JSON 8192

typedef struct ia_schema {
    char type[IA_STR];
    char ch_table[IA_STR];
    char iceberg_table[IA_PATH];
    char partitions[IA_PART][IA_STR];
    int n_partitions;
    char primary_key[IA_PART][IA_STR];
    int n_primary_key;
    char required[IA_REQ][IA_STR];
    int n_required;
} ia_schema;

const char *ia_schema_version(void);
const char *ia_subject_prefix(void);

/* 1 if type is a known event type. */
int ia_type_known(const char *type);

/* subject = prefix + "." + type; returns IA_OK if type known. */
int ia_subject_for(const char *type, char *out, size_t cap);

/* Fill *out for known type; IA_ERR if unknown. */
int ia_schema_for(const char *type, ia_schema *out);

/* Ordered type names into out[][IA_STR]; *n set. */
int ia_ordered_types(char out[][IA_STR], int *n, int max);

/*
 * Validate envelope fields:
 * event_id non-empty, type known, schema_version == IA_VERSION, occurred_at non-zero
 * (unix millis > 0 OR non-empty RFC3339-ish string).
 */
int ia_validate_envelope(const char *event_id, const char *type, const char *schema_version,
                         long long occurred_at_unix_ms, const char *occurred_at_str, char *err,
                         size_t err_cap);

/* Encode all schemas as JSON array (for Go Schemas() rebuild). */
int ia_encode_schemas_json(char *out, size_t cap);

/* Encode ordered types as JSON string array. */
int ia_encode_types_json(char *out, size_t cap);

/*
 * Build product wire event JSON (envelope + payload) for companions analytics.
 * type: session_start|session_end|ui_action|page_view|mode_switch
 * labels_json: optional object string e.g. {"surface":"companions-frontend"}
 * occurred_unix_ms: 0 → use 1 as placeholder (host should pass now).
 * Returns IA_OK and writes wire JSON to out.
 */
int ia_build_product_wire(const char *type, const char *event_id, const char *session_id,
                          const char *turn_id, const char *user_id, const char *source_repo,
                          long long occurred_unix_ms, const char *profile, const char *mode,
                          const char *campaign, const char *action, const char *target,
                          const char *path, const char *from_mode, const char *to_mode,
                          long long duration_ms, long long turn_count, const char *labels_json,
                          char *out, size_t cap, char *err, size_t err_cap);

/* Fill 32-char hex event id (16 random bytes). */
int ia_new_event_id(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
