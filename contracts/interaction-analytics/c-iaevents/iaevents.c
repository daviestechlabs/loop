#define _POSIX_C_SOURCE 200809L
#include "iaevents.h"

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct ia_row {
    const char *type;
    const char *table;
    const char *parts[IA_PART];
    int n_parts;
    const char *pkeys[IA_PART];
    int n_pkeys;
    const char *req[IA_REQ];
    int n_req;
} ia_row;

/* Canonical catalog matching contracts/interaction-analytics/events/schema.go */
static const ia_row k_rows[] = {
    {"turn", "turn_events", {"event_date", "profile"}, 2, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id", "turn_id"}, 6},
    {"route", "route_events", {"event_date", "profile", "route_id"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "turn_id", "policy_version"}, 6},
    {"model", "model_events", {"event_date", "model_id", "hardware_id"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "model_id"}, 5},
    {"tts_segment", "tts_segment_events", {"event_date", "voice_id", "hardware_id"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "turn_id"}, 5},
    {"stt_segment", "stt_segment_events", {"event_date", "model_id", "hardware_id"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
    {"tool_call", "tool_call_events", {"event_date", "tool_id", "state"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "tool_call_id"}, 5},
    {"agent_task", "agent_task_events", {"event_date", "agent_id", "state"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "task_id"}, 5},
    {"eval_result", "eval_result_events", {"event_date", "eval_suite", "policy_version"}, 3, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "policy_version", "commit_sha"}, 6},
    {"hardware_capacity_sample", "hardware_capacity_samples", {"event_date", "node", "device_kind"}, 3,
     {"event_id"}, 1, {"event_id", "type", "schema_version", "occurred_at", "hardware_id"}, 5},
    {"session_start", "events_all", {"event_date", "profile"}, 2, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
    {"session_end", "events_all", {"event_date", "profile"}, 2, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
    {"ui_action", "events_all", {"event_date"}, 1, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
    {"page_view", "events_all", {"event_date"}, 1, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
    {"mode_switch", "events_all", {"event_date", "profile"}, 2, {"event_id"}, 1,
     {"event_id", "type", "schema_version", "occurred_at", "session_id"}, 5},
};

static const int k_n_rows = (int)(sizeof(k_rows) / sizeof(k_rows[0]));

static const ia_row *find_row(const char *type) {
    int i;
    if (!type || !type[0])
        return NULL;
    for (i = 0; i < k_n_rows; i++) {
        if (strcmp(k_rows[i].type, type) == 0)
            return &k_rows[i];
    }
    return NULL;
}

const char *ia_schema_version(void) {
    return IA_VERSION;
}

const char *ia_subject_prefix(void) {
    return IA_SUBJECT_PREFIX;
}

int ia_type_known(const char *type) {
    return find_row(type) != NULL;
}

int ia_subject_for(const char *type, char *out, size_t cap) {
    if (!out || cap < 8 || !ia_type_known(type))
        return IA_ERR;
    if ((size_t)snprintf(out, cap, "%s.%s", IA_SUBJECT_PREFIX, type) >= cap)
        return IA_ERR;
    return IA_OK;
}

int ia_schema_for(const char *type, ia_schema *out) {
    const ia_row *r;
    int i;
    if (!out)
        return IA_ERR;
    memset(out, 0, sizeof(*out));
    r = find_row(type);
    if (!r)
        return IA_ERR;
    snprintf(out->type, sizeof(out->type), "%s", r->type);
    snprintf(out->ch_table, sizeof(out->ch_table), "%s", r->table);
    snprintf(out->iceberg_table, sizeof(out->iceberg_table), "interaction_analytics.%s", r->table);
    out->n_partitions = r->n_parts;
    for (i = 0; i < r->n_parts && i < IA_PART; i++)
        snprintf(out->partitions[i], sizeof(out->partitions[i]), "%s", r->parts[i]);
    out->n_primary_key = r->n_pkeys;
    for (i = 0; i < r->n_pkeys && i < IA_PART; i++)
        snprintf(out->primary_key[i], sizeof(out->primary_key[i]), "%s", r->pkeys[i]);
    out->n_required = r->n_req;
    for (i = 0; i < r->n_req && i < IA_REQ; i++)
        snprintf(out->required[i], sizeof(out->required[i]), "%s", r->req[i]);
    return IA_OK;
}

int ia_ordered_types(char out[][IA_STR], int *n, int max) {
    int i;
    if (!out || !n || max < 1)
        return IA_ERR;
    *n = 0;
    for (i = 0; i < k_n_rows && *n < max; i++) {
        snprintf(out[*n], IA_STR, "%s", k_rows[i].type);
        (*n)++;
    }
    return IA_OK;
}

int ia_validate_envelope(const char *event_id, const char *type, const char *schema_version,
                         long long occurred_at_unix_ms, const char *occurred_at_str, char *err,
                         size_t err_cap) {
    if (!event_id || !event_id[0]) {
        if (err && err_cap)
            snprintf(err, err_cap, "event_id is required");
        return IA_ERR;
    }
    if (!type || !type[0]) {
        if (err && err_cap)
            snprintf(err, err_cap, "type is required");
        return IA_ERR;
    }
    if (!schema_version || strcmp(schema_version, IA_VERSION) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "schema_version = \"%s\", want \"%s\"",
                     schema_version ? schema_version : "", IA_VERSION);
        return IA_ERR;
    }
    if (occurred_at_unix_ms <= 0 && (!occurred_at_str || !occurred_at_str[0])) {
        if (err && err_cap)
            snprintf(err, err_cap, "occurred_at is required");
        return IA_ERR;
    }
    if (!ia_type_known(type)) {
        if (err && err_cap)
            snprintf(err, err_cap, "unknown event type \"%s\"", type);
        return IA_ERR;
    }
    if (err && err_cap)
        err[0] = '\0';
    return IA_OK;
}

static int append_str_array(char *out, size_t cap, size_t *pos, const char *const *items, int n) {
    int i, w;
    if (*pos + 1 >= cap)
        return -1;
    out[(*pos)++] = '[';
    for (i = 0; i < n; i++) {
        if (i) {
            if (*pos + 1 >= cap)
                return -1;
            out[(*pos)++] = ',';
        }
        w = snprintf(out + *pos, cap - *pos, "\"%s\"", items[i] ? items[i] : "");
        if (w < 0 || (size_t)w >= cap - *pos)
            return -1;
        *pos += (size_t)w;
    }
    if (*pos + 1 >= cap)
        return -1;
    out[(*pos)++] = ']';
    out[*pos] = '\0';
    return 0;
}

int ia_encode_types_json(char *out, size_t cap) {
    size_t pos = 0;
    int i, w;
    if (!out || cap < 4)
        return IA_ERR;
    out[pos++] = '[';
    out[pos] = '\0';
    for (i = 0; i < k_n_rows; i++) {
        if (i) {
            if (pos + 1 >= cap)
                return IA_ERR;
            out[pos++] = ',';
        }
        w = snprintf(out + pos, cap - pos, "\"%s\"", k_rows[i].type);
        if (w < 0 || (size_t)w >= cap - pos)
            return IA_ERR;
        pos += (size_t)w;
    }
    if (pos + 2 >= cap)
        return IA_ERR;
    out[pos++] = ']';
    out[pos] = '\0';
    return IA_OK;
}

int ia_encode_schemas_json(char *out, size_t cap) {
    size_t pos = 0;
    int i, j, w;
    if (!out || cap < 8)
        return IA_ERR;
    out[pos++] = '[';
    out[pos] = '\0';
    for (i = 0; i < k_n_rows; i++) {
        const ia_row *r = &k_rows[i];
        char parts_json[512];
        char pkey_json[256];
        char req_json[1024];
        size_t ppos;
        const char *tmp[IA_REQ];

        if (i) {
            if (pos + 1 >= cap)
                return IA_ERR;
            out[pos++] = ',';
        }
        ppos = 0;
        for (j = 0; j < r->n_parts; j++)
            tmp[j] = r->parts[j];
        if (append_str_array(parts_json, sizeof(parts_json), &ppos, tmp, r->n_parts) != 0)
            return IA_ERR;
        ppos = 0;
        for (j = 0; j < r->n_pkeys; j++)
            tmp[j] = r->pkeys[j];
        if (append_str_array(pkey_json, sizeof(pkey_json), &ppos, tmp, r->n_pkeys) != 0)
            return IA_ERR;
        ppos = 0;
        for (j = 0; j < r->n_req; j++)
            tmp[j] = r->req[j];
        if (append_str_array(req_json, sizeof(req_json), &ppos, tmp, r->n_req) != 0)
            return IA_ERR;

        w = snprintf(out + pos, cap - pos,
                     "{\"type\":\"%s\",\"version\":\"%s\",\"clickhouse_table\":\"%s\","
                     "\"iceberg_table\":\"interaction_analytics.%s\",\"partition_columns\":%s,"
                     "\"primary_key\":%s,\"required_fields\":%s}",
                     r->type, IA_VERSION, r->table, r->table, parts_json, pkey_json, req_json);
        if (w < 0 || (size_t)w >= cap - pos)
            return IA_ERR;
        pos += (size_t)w;
    }
    if (pos + 2 >= cap)
        return IA_ERR;
    out[pos++] = ']';
    out[pos] = '\0';
    return IA_OK;
}

int ia_new_event_id(char *out, size_t cap) {
    unsigned char rnd[16];
    int fd, i;
    size_t used = 0;
    if (!out || cap < 33)
        return IA_ERR;
    out[0] = '\0';
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return IA_ERR;
    while (used < sizeof(rnd)) {
        ssize_t n = read(fd, rnd + used, sizeof(rnd) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return IA_ERR;
        }
        used += (size_t)n;
    }
    close(fd);
    for (i = 0; i < 16; i++)
        sprintf(out + i * 2, "%02x", rnd[i]);
    out[32] = '\0';
    return IA_OK;
}

static int jesc(const char *in, char *out, size_t cap) {
    size_t i = 0, j = 0;
    if (!out || cap == 0)
        return IA_ERR;
    if (!in)
        in = "";
    while (in[i]) {
        unsigned char c = (unsigned char)in[i++];
        size_t need = c < 0x20 ? 6u : (c == '"' || c == '\\') ? 2u : 1u;
        if (need >= cap - j) {
            out[0] = '\0';
            return IA_ERR;
        }
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            memcpy(out + j, "\\u00", 4u);
            j += 4u;
            out[j++] = hex[c >> 4u];
            out[j++] = hex[c & 15u];
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return IA_OK;
}

/* RFC3339 UTC from unix ms (seconds only precision). */
static void fmt_time(long long ms, char *out, size_t cap) {
    time_t sec = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&sec, &tm);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int ia_build_product_wire(const char *type, const char *event_id, const char *session_id,
                          const char *turn_id, const char *user_id, const char *source_repo,
                          long long occurred_unix_ms, const char *profile, const char *mode,
                          const char *campaign, const char *action, const char *target,
                          const char *path, const char *from_mode, const char *to_mode,
                          long long duration_ms, long long turn_count, const char *labels_json,
                          char *out, size_t cap, char *err, size_t err_cap) {
    char eid[40], tstr[40];
    char e_id[256], e_type[64], e_sess[256], e_turn[256], e_user[256], e_repo[256];
    char e_prof[128], e_mode[128], e_camp[128], e_act[128], e_tgt[128], e_path[512];
    char e_from[128], e_to[128];
    char payload[2048];
    const char *lab = labels_json && labels_json[0] ? labels_json : "{}";
    int w;

    if (err && err_cap)
        err[0] = '\0';
    if (out && cap) out[0] = '\0';
    if (!type || !out || cap == 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "bad args");
        return IA_ERR;
    }
    if (!ia_type_known(type)) {
        if (err && err_cap)
            snprintf(err, err_cap, "unknown type");
        return IA_ERR;
    }
    if (!session_id || !session_id[0]) {
        if (err && err_cap)
            snprintf(err, err_cap, "session_id required");
        return IA_ERR;
    }
    if (!event_id || !event_id[0]) {
        if (ia_new_event_id(eid, sizeof(eid)) != IA_OK)
            return IA_ERR;
        event_id = eid;
    }
    if (occurred_unix_ms <= 0)
        occurred_unix_ms = 1;
    fmt_time(occurred_unix_ms, tstr, sizeof(tstr));
    if (jesc(event_id, e_id, sizeof(e_id)) || jesc(type, e_type, sizeof(e_type)) ||
        jesc(session_id, e_sess, sizeof(e_sess)) || jesc(turn_id, e_turn, sizeof(e_turn)) ||
        jesc(user_id, e_user, sizeof(e_user)) || jesc(source_repo, e_repo, sizeof(e_repo)) ||
        jesc(profile, e_prof, sizeof(e_prof)) || jesc(mode, e_mode, sizeof(e_mode)) ||
        jesc(campaign, e_camp, sizeof(e_camp)) || jesc(action, e_act, sizeof(e_act)) ||
        jesc(target, e_tgt, sizeof(e_tgt)) || jesc(path, e_path, sizeof(e_path)) ||
        jesc(from_mode, e_from, sizeof(e_from)) || jesc(to_mode, e_to, sizeof(e_to))) {
        if (err && err_cap) snprintf(err, err_cap, "field overflow");
        return IA_ERR;
    }

    if (strcmp(type, "session_start") == 0) {
        w = snprintf(payload, sizeof(payload),
                 "{\"profile\":\"%s\",\"mode\":\"%s\",\"campaign\":\"%s\",\"labels\":%s}", e_prof,
                 e_mode, e_camp, lab);
    } else if (strcmp(type, "session_end") == 0) {
        w = snprintf(payload, sizeof(payload),
                 "{\"profile\":\"%s\",\"mode\":\"%s\",\"duration_ms\":%lld,\"turn_count\":%lld,"
                 "\"labels\":%s}",
                 e_prof, e_mode, (long long)duration_ms, (long long)turn_count, lab);
    } else if (strcmp(type, "ui_action") == 0) {
        if (!action || !action[0]) {
            if (err && err_cap)
                snprintf(err, err_cap, "action is required");
            return IA_ERR;
        }
        w = snprintf(payload, sizeof(payload),
                 "{\"action\":\"%s\",\"target\":\"%s\",\"mode\":\"%s\",\"labels\":%s}", e_act, e_tgt,
                 e_mode, lab);
    } else if (strcmp(type, "page_view") == 0) {
        if (!path || !path[0]) {
            if (err && err_cap)
                snprintf(err, err_cap, "path is required");
            return IA_ERR;
        }
        w = snprintf(payload, sizeof(payload), "{\"path\":\"%s\",\"mode\":\"%s\",\"labels\":%s}", e_path,
                 e_mode, lab);
    } else if (strcmp(type, "mode_switch") == 0) {
        if (!from_mode || !from_mode[0] || !to_mode || !to_mode[0]) {
            if (err && err_cap)
                snprintf(err, err_cap, "from_mode and to_mode are required");
            return IA_ERR;
        }
        w = snprintf(payload, sizeof(payload),
                 "{\"from_mode\":\"%s\",\"to_mode\":\"%s\",\"profile\":\"%s\",\"labels\":%s}", e_from,
                 e_to, e_prof, lab);
    } else {
        if (err && err_cap)
            snprintf(err, err_cap, "not a product surface type");
        return IA_ERR;
    }

    if (w < 0 || (size_t)w >= sizeof(payload)) {
        if (err && err_cap) snprintf(err, err_cap, "payload overflow");
        return IA_ERR;
    }
    w = snprintf(out, cap,
                 "{\"event_id\":\"%s\",\"type\":\"%s\",\"schema_version\":\"%s\","
                 "\"occurred_at\":\"%s\",\"session_id\":\"%s\",\"turn_id\":\"%s\","
                 "\"user_id\":\"%s\",\"source_repo\":\"%s\",\"labels\":%s,\"payload\":%s}",
                 e_id, e_type, IA_VERSION, tstr, e_sess, e_turn, e_user, e_repo, lab, payload);
    if (w < 0 || (size_t)w >= cap) {
        out[0] = '\0';
        if (err && err_cap)
            snprintf(err, err_cap, "output overflow");
        return IA_ERR;
    }
    return IA_OK;
}
