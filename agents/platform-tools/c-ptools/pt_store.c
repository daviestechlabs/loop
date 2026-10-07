#include "pt_store.h"

#include "cmp_json.h"
#include "../c-toolstore/toolstore.h"

#include <dirent.h>
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PT_SNAPSHOT_CAP (PT_MAX_CALLS * PT_RECORD_CAP + 1024u)

typedef struct {
    const char *name;
    size_t offset;
    size_t capacity;
} pt_store_field;

#define STORE_FIELD(name) {#name, offsetof(pt_call, name), sizeof(((pt_call *)0)->name)}
static const pt_store_field string_fields[] = {
    STORE_FIELD(tool_call_id), STORE_FIELD(idempotency_key),
    STORE_FIELD(parent_task_id), STORE_FIELD(parent_turn_id),
    STORE_FIELD(user_id), STORE_FIELD(session_id), STORE_FIELD(agent_id),
    STORE_FIELD(tool_id), STORE_FIELD(input_json), STORE_FIELD(output_json),
    STORE_FIELD(output_sha256), STORE_FIELD(output_artifact),
    STORE_FIELD(summary), STORE_FIELD(error), STORE_FIELD(approval_id),
};
#undef STORE_FIELD

const char *pt_state_name(int state) {
    switch (state) {
    case PT_ST_REQUESTED: return "requested";
    case PT_ST_AWAITING_APPROVAL: return "awaiting_approval";
    case PT_ST_APPROVED: return "approved";
    case PT_ST_REJECTED: return "rejected";
    case PT_ST_QUEUED: return "queued";
    case PT_ST_RUNNING: return "running";
    case PT_ST_COMPLETED: return "completed";
    case PT_ST_FAILED: return "failed";
    case PT_ST_CANCELED: return "canceled";
    default: return "unspecified";
    }
}

int pt_store_record_decode(const char *json, pt_call *call) {
    cmp_json_object object;
    char state[32];
    int64_t number = 0;
    size_t i;
    if (!json || !call || !cmp_json_object_parse(json, &object)) return -1;
    memset(call, 0, sizeof(*call));
    for (i = 0; i < object.field_count; ++i) {
        const cmp_json_field *member = &object.fields[i];
        size_t j;
        int known = 0;
        if (member->key_escaped) return -1;
        for (j = 0; j < sizeof(string_fields) / sizeof(string_fields[0]); ++j)
            if (strlen(string_fields[j].name) == member->key_len &&
                memcmp(string_fields[j].name, member->key, member->key_len) == 0)
                known = 1;
        if ((member->key_len == 5u && memcmp(member->key, "state", 5u) == 0) ||
            (member->key_len == 10u && memcmp(member->key, "created_at", 10u) == 0) ||
            (member->key_len == 10u && memcmp(member->key, "updated_at", 10u) == 0)) known = 1;
        if (!known) return -1;
    }
    for (i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); ++i) {
        const pt_store_field *field = &string_fields[i];
        int count = cmp_json_object_key_count(&object, field->name);
        if (count < 0 || count > 1 ||
            (count == 1 && !cmp_json_object_str(
                &object, field->name, (char *)call + field->offset, field->capacity)))
            return -1;
    }
    if (cmp_json_object_str(&object, "state", state, sizeof(state))) {
        for (call->state = PT_ST_REQUESTED; call->state <= PT_ST_CANCELED; ++call->state)
            if (strcmp(state, pt_state_name(call->state)) == 0) break;
    } else {
        if (!cmp_json_object_i64(&object, "state", &number) ||
            number < PT_ST_REQUESTED || number > PT_ST_CANCELED) return -1;
        call->state = (int)number;
    }
    if (call->state < PT_ST_REQUESTED || call->state > PT_ST_CANCELED ||
        !ts_valid_id(call->tool_call_id)) return -1;
    if (!cmp_json_object_i64(&object, "created_at", &call->created_at) ||
        !cmp_json_object_i64(&object, "updated_at", &call->updated_at) ||
        call->created_at < 0 || call->updated_at < call->created_at) return -1;
    call->in_use = 1;
    return 0;
}

static char *read_file(const char *path, size_t maximum, int *missing) {
    struct stat st;
    char *buffer;
    size_t length, used = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    *missing = 0;
    if (fd < 0) {
        *missing = errno == ENOENT;
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size >= maximum) {
        close(fd);
        return NULL;
    }
    length = (size_t)st.st_size;
    buffer = malloc(length + 1u);
    if (!buffer) {
        close(fd);
        return NULL;
    }
    while (used < length) {
        ssize_t n = read(fd, buffer + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            free(buffer);
            close(fd);
            return NULL;
        }
        used += (size_t)n;
    }
    close(fd);
    if (memchr(buffer, '\0', length)) {
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
}

static int index_call(pt_store *store, const pt_call *call, const char *legacy, int replace) {
    sqlite3_stmt *statement = NULL;
    static const char insert[] = "INSERT INTO calls(id,owner,session,agent,tool,idem,legacy) VALUES(?,?,?,?,?,?,?)";
    static const char upsert[] = "INSERT INTO calls(id,owner,session,agent,tool,idem,legacy) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET owner=excluded.owner,session=excluded.session,agent=excluded.agent,"
        "tool=excluded.tool,idem=excluded.idem,legacy=excluded.legacy";
    const char *values[] = {call->tool_call_id, call->user_id, call->session_id,
                           call->agent_id, call->tool_id, call->idempotency_key};
    int result = -1;
    if (sqlite3_prepare_v2(store->index, replace ? upsert : insert, -1, &statement, NULL) != SQLITE_OK) goto done;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (sqlite3_bind_text(statement, (int)i + 1, values[i], -1, SQLITE_TRANSIENT) != SQLITE_OK) goto done;
    if ((legacy ? sqlite3_bind_text(statement, 7, legacy, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(statement, 7)) != SQLITE_OK)
        goto done;
    if (sqlite3_step(statement) == SQLITE_DONE) result = 0;
done:
    sqlite3_finalize(statement);
    return result;
}

static pt_call *cache_call(pt_store *store, const pt_call *call) {
    size_t slot = PT_MAX_CALLS;
    for (size_t i = 0; i < PT_MAX_CALLS; ++i) {
        if (store->calls[i].in_use && !strcmp(store->calls[i].tool_call_id, call->tool_call_id)) {
            slot = i;
            break;
        }
        if (!store->calls[i].in_use && slot == PT_MAX_CALLS) slot = i;
    }
    if (slot == PT_MAX_CALLS) {
        slot = store->next_slot;
        store->next_slot = (slot + 1u) % PT_MAX_CALLS;
    }
    store->calls[slot] = *call;
    return &store->calls[slot];
}

void pt_store_close(pt_store *store) {
    if (!store) return;
    if (store->index) sqlite3_close(store->index);
    store->index = NULL;
    if (store->index_path[0]) unlink(store->index_path);
    store->index_path[0] = '\0';
}

static int open_index(pt_store *store) {
    int fd, n;
    n = snprintf(store->index_path, sizeof(store->index_path), "%s.lookup-XXXXXX", store->path);
    if (n < 0 || (size_t)n >= sizeof(store->index_path)) return -1;
    fd = mkstemp(store->index_path);
    if (fd < 0) { store->index_path[0] = '\0'; return -1; }
    if (close(fd) != 0) return -1;
    if (sqlite3_open_v2(store->index_path, &store->index, SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, NULL) != SQLITE_OK)
        return -1;
    /* This private index is disposable. Synced JSON records remain authoritative. */
    return sqlite3_exec(store->index,
        "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF; PRAGMA cache_size=-1024; PRAGMA temp_store=FILE;"
        "CREATE TABLE calls(id TEXT PRIMARY KEY,owner TEXT NOT NULL,session TEXT NOT NULL,agent TEXT NOT NULL,"
        "tool TEXT NOT NULL,idem TEXT NOT NULL,legacy TEXT);"
        "CREATE UNIQUE INDEX scoped_retry ON calls(owner,session,agent,tool,idem) WHERE idem<>'';",
        NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/* The complete snapshot already passed the JSON grammar check. */
static const char *object_end(const char *p) {
    unsigned depth = 0;
    int quoted = 0;
    do {
        if (quoted) {
            if (*p == '\\' && p[1]) ++p;
            else if (*p == '"') quoted = 0;
        } else if (*p == '"') quoted = 1;
        else if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        ++p;
    } while (*p && depth);
    return depth == 0 ? p : NULL;
}

static int load_snapshot(pt_store *store, char *json) {
    cmp_json_object root;
    const char *members = NULL;
    size_t i;
    if (!cmp_json_object_parse(json, &root)) return -1;
    for (i = 0; i < root.field_count; ++i) {
        const cmp_json_field *field = &root.fields[i];
        if ((field->key_len == 5u && memcmp(field->key, "calls", 5u) == 0) ||
            (field->key_len == 5u && memcmp(field->key, "tasks", 5u) == 0)) {
            if (members || field->value[0] != '{') return -1;
            members = field->value + 1;
        }
    }
    if (!members) return -1;
    for (;;) {
        const char *end, *key_end;
        char id[PT_ID];
        char *record;
        pt_call call;
        size_t length;
        members = skip_space(members);
        if (*members == '}') return 0;
        if (*members != '"') return -1;
        key_end = strchr(++members, '"');
        if (!key_end || (size_t)(key_end - members) >= sizeof(id)) return -1;
        memcpy(id, members, (size_t)(key_end - members));
        id[key_end - members] = '\0';
        if (!ts_valid_id(id)) return -1;
        members = skip_space(key_end + 1);
        if (*members++ != ':') return -1;
        members = skip_space(members);
        if (*members != '{' || !(end = object_end(members))) return -1;
        length = (size_t)(end - members);
        if (length >= PT_RECORD_CAP) return -1;
        record = malloc(length + 1u);
        if (!record) return -1;
        memcpy(record, members, length);
        record[length] = '\0';
        i = pt_store_record_decode(record, &call) == 0 &&
            strcmp(id, call.tool_call_id) == 0 && index_call(store, &call, record, 0) == 0;
        free(record);
        if (!i) return -1;
        members = skip_space(end);
        if (*members == ',') ++members;
        else if (*members != '}') return -1;
    }
}

int pt_store_open(pt_store *store, const char *path) {
    char directory[TS_PATH], requested_path[512];
    char *json;
    DIR *dir;
    struct dirent *entry;
    int missing, result = 0;
    if (!store || !path || !path[0] || strlen(path) >= sizeof(store->path) ||
        ts_record_dir(path, directory, sizeof(directory)) != TS_OK) return -1;
    memcpy(requested_path, path, strlen(path) + 1u);
    path = requested_path;
    pt_store_close(store);
    memset(store, 0, sizeof(*store));
    memcpy(store->path, path, strlen(path) + 1u);
    json = read_file(path, PT_SNAPSHOT_CAP, &missing);
    if (!json && !missing) goto fail;
    if (!json && ts_atomic_write(path, "{\"calls\":{}}", sizeof("{\"calls\":{}}") - 1u, 0600) != TS_OK) goto fail;
    if (open_index(store) != 0) { free(json); goto fail; }
    if (json) {
        result = load_snapshot(store, json);
        free(json);
        if (result != 0) goto fail;
    }

    dir = opendir(directory);
    if (!dir) { if (errno == ENOENT) return 0; goto fail; }
    for (;;) {
        char filename[TS_PATH + TS_SHARD_NAME];
        pt_call call;
        int n;
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) result = -1;
            break;
        }
        if (entry->d_name[0] == '.') continue;
        n = snprintf(filename, sizeof(filename), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(filename)) { result = -1; break; }
        json = read_file(filename, PT_RECORD_CAP, &missing);
        if (!json) { result = -1; break; }
        result = pt_store_record_decode(json, &call);
        free(json);
        if (result != 0 || ts_check_shard_identity(call.tool_call_id, entry->d_name) != TS_OK ||
            index_call(store, &call, NULL, 1) != 0) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    if (result == 0) return 0;
fail:
    pt_store_close(store);
    store->failed = 1;
    return -1;
}

static int append_string(char *out, size_t cap, size_t *used, const char *text) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)text;
    if (*used >= cap - 1u) return -1;
    out[(*used)++] = '"';
    while (*p) {
        unsigned char ch = *p++;
        if (cap - *used < 7u) return -1;
        if (ch == '"' || ch == '\\') {
            out[(*used)++] = '\\';
            out[(*used)++] = (char)ch;
        } else if (ch < 0x20u) {
            memcpy(out + *used, "\\u00", 4u);
            *used += 4u;
            out[(*used)++] = hex[ch >> 4];
            out[(*used)++] = hex[ch & 15u];
        } else out[(*used)++] = (char)ch;
    }
    if (cap - *used < 2u) return -1;
    out[(*used)++] = '"';
    out[*used] = '\0';
    return 0;
}

size_t pt_store_record_encode(char *json, size_t capacity, const pt_call *call) {
    size_t i, used = 1u;
    int n;
    if (!json || capacity < 2u || !call) return 0;
    json[0] = '{';
    for (i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); ++i) {
        const pt_store_field *field = &string_fields[i];
        const char *value = (const char *)call + field->offset;
        if (strnlen(value, field->capacity) >= field->capacity) return 0;
        if (capacity - used < 2u) return 0;
        if (i) json[used++] = ',';
        if (append_string(json, capacity, &used, field->name) != 0) return 0;
        json[used++] = ':';
        if (append_string(json, capacity, &used, value) != 0) return 0;
    }
    n = snprintf(json + used, capacity - used,
                 ",\"state\":\"%s\",\"created_at\":%lld,\"updated_at\":%lld}",
                 pt_state_name(call->state), (long long)call->created_at,
                 (long long)call->updated_at);
    if (n < 0 || (size_t)n >= capacity - used) return 0;
    return used + (size_t)n;
}

static int write_call(const pt_store *store, const pt_call *call) {
    char directory[TS_PATH], shard[TS_SHARD_NAME], path[TS_PATH + TS_SHARD_NAME];
    char *json = malloc(PT_RECORD_CAP);
    size_t used;
    int n, result = -1;
    if (!json) return -1;
    used = pt_store_record_encode(json, PT_RECORD_CAP, call);
    if (!used) goto done;
    if (ts_record_dir(store->path, directory, sizeof(directory)) != TS_OK ||
        ts_shard_name(call->tool_call_id, shard, sizeof(shard)) != TS_OK) goto done;
    n = snprintf(path, sizeof(path), "%s/%s", directory, shard);
    if (n < 0 || (size_t)n >= sizeof(path)) goto done;
    if (ts_atomic_write(path, json, used, 0600) == TS_OK) result = 0;
done:
    free(json);
    return result;
}

int pt_store_save(pt_store *store) {
    if (!store || store->failed || !store->index) return -1;
    for (size_t i = 0; i < PT_MAX_CALLS; ++i)
        if (store->calls[i].in_use && pt_store_put(store, &store->calls[i]) != 0) return -1;
    return 0;
}

pt_call *pt_store_get(pt_store *store, const char *id) {
    sqlite3_stmt *statement = NULL;
    pt_call call, *result = NULL;
    char directory[TS_PATH], name[TS_SHARD_NAME], filename[TS_PATH + TS_SHARD_NAME];
    char *json = NULL;
    int missing, status;
    if (!store || store->failed || !store->index || !ts_valid_id(id)) return NULL;
    for (size_t i = 0; i < PT_MAX_CALLS; ++i)
        if (store->calls[i].in_use && !strcmp(store->calls[i].tool_call_id, id)) return &store->calls[i];
    if (sqlite3_prepare_v2(store->index, "SELECT legacy FROM calls WHERE id=?", -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto corrupt;
    status = sqlite3_step(statement);
    if (status == SQLITE_DONE) goto done;
    if (status != SQLITE_ROW) goto corrupt;
    if (sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        const char *legacy = (const char *)sqlite3_column_text(statement, 0);
        if (!legacy || pt_store_record_decode(legacy, &call) != 0) goto corrupt;
    } else {
        if (ts_record_dir(store->path, directory, sizeof(directory)) != TS_OK ||
            ts_shard_name(id, name, sizeof(name)) != TS_OK) goto corrupt;
        status = snprintf(filename, sizeof(filename), "%s/%s", directory, name);
        if (status < 0 || (size_t)status >= sizeof(filename)) goto corrupt;
        json = read_file(filename, PT_RECORD_CAP, &missing);
        if (!json || pt_store_record_decode(json, &call) != 0) goto corrupt;
    }
    if (strcmp(call.tool_call_id, id)) goto corrupt;
    result = cache_call(store, &call);
    goto done;
corrupt:
    store->failed = 1;
done:
    free(json);
    sqlite3_finalize(statement);
    return result;
}

pt_call *pt_store_by_idem(pt_store *store, const char *user, const char *session,
                          const char *agent, const char *tool, const char *idem) {
    sqlite3_stmt *statement = NULL;
    const char *values[] = {user, session, agent, tool, idem};
    char id[PT_ID];
    pt_call *call = NULL;
    int status;
    if (!store || store->failed || !store->index || !user || !session || !agent || !tool || !idem || !idem[0]) return NULL;
    if (sqlite3_prepare_v2(store->index, "SELECT id FROM calls WHERE owner=? AND session=? AND agent=? AND tool=? AND idem=? AND idem<>''",
                          -1, &statement, NULL) != SQLITE_OK) goto corrupt;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (sqlite3_bind_text(statement, (int)i + 1, values[i], -1, SQLITE_TRANSIENT) != SQLITE_OK) goto corrupt;
    status = sqlite3_step(statement);
    if (status == SQLITE_DONE) goto done;
    if (status != SQLITE_ROW || sqlite3_column_bytes(statement, 0) >= (int)sizeof(id)) goto corrupt;
    const char *value = (const char *)sqlite3_column_text(statement, 0);
    if (!value) goto corrupt;
    memcpy(id, value, (size_t)sqlite3_column_bytes(statement, 0) + 1u);
    call = pt_store_get(store, id);
    if (!call || strcmp(call->user_id, user) || strcmp(call->session_id, session) ||
        strcmp(call->agent_id, agent) || strcmp(call->tool_id, tool) || strcmp(call->idempotency_key, idem)) goto corrupt;
    goto done;
corrupt:
    store->failed = 1;
    call = NULL;
done:
    sqlite3_finalize(statement);
    return call;
}

int pt_store_put(pt_store *store, const pt_call *call) {
    if (!store || store->failed || !store->index || !call || !call->in_use || !ts_valid_id(call->tool_call_id) ||
        call->state < PT_ST_REQUESTED || call->state > PT_ST_CANCELED ||
        call->created_at < 0 || call->updated_at < call->created_at) return -1;
    if (sqlite3_exec(store->index, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) goto corrupt;
    if (index_call(store, call, NULL, 1) != 0) {
        int error = sqlite3_errcode(store->index);
        if (sqlite3_exec(store->index, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK || error != SQLITE_CONSTRAINT) goto corrupt;
        return -1;
    }
    if (write_call(store, call) != 0 || sqlite3_exec(store->index, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto corrupt;
    (void)cache_call(store, call);
    return 0;
corrupt:
    /* A record rename can precede a failed sync. Reopen and validate before admitting more work. */
    store->failed = 1;
    (void)sqlite3_exec(store->index, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int pt_store_visit(pt_store *store, int (*visit)(const pt_call *, void *), void *context) {
    sqlite3_stmt *statement = NULL;
    int status, result = -1;
    if (!store || store->failed || !store->index || !visit) return -1;
    if (sqlite3_prepare_v2(store->index, "SELECT id FROM calls ORDER BY id", -1, &statement, NULL) != SQLITE_OK) goto done;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(statement, 0);
        pt_call *call = id ? pt_store_get(store, id) : NULL;
        if (!call || visit(call, context) != 0) goto done;
    }
    if (status == SQLITE_DONE) result = 0;
done:
    sqlite3_finalize(statement);
    return result;
}
