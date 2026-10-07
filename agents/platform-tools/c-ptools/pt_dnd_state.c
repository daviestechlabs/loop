/* Shared write-ahead recovery for the fixed encounter and campaign tools. */
#define _POSIX_C_SOURCE 200809L
#include "pt_dnd_state.h"
#include "pt_encounter.h"
#include "pt_campaign.h"
#include "pt_artifact.h"
#include "../c-toolstore/toolstore.h"
#include "cmp_json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DND_JOURNAL_CAP (PT_RECORD_CAP + 3u * PT_OUT + PT_INITIATIVE_DRAWS_CAP + 256u)
#define INITIATIVE_SCHEMA "dnd-encounter-transaction/v2"
typedef struct {
    char before[PT_OUT], after[PT_OUT];
    pt_call completed;
    char campaign[PT_OUT], draws[PT_INITIATIVE_DRAWS_CAP];
} state_transaction;

typedef struct {
    const char *tool, *schema, *journal;
    int (*path)(const char *, const char *, char *, size_t, int *, int *);
    int (*transition)(const char *, char *, size_t, pt_call *);
    int (*canonical)(const char *, char *, size_t, pt_dnd_identity *);
} state_ops;
/* Fixed state owners, plus a read-only projection of campaign scene evidence. */
static const state_ops adapters[] = {
    {"dnd-encounter-state", "dnd-encounter-transaction/v1", "encounter-transaction-v1.json",
     pt_encounter_path, pt_encounter_transition, pt_encounter_canonical},
    {"dnd-campaign-state", "dnd-campaign-transaction/v1", "campaign-transaction-v1.json",
     pt_campaign_path, pt_campaign_transition, pt_campaign_canonical},
    {"dnd-scene-presence", "dnd-scene-query-transaction/v1", "scene-query-transaction-v1.json",
     pt_scene_presence_path, pt_scene_presence_transition, pt_campaign_canonical}
};
static const state_ops *find_tool(const char *tool) {
    if (tool) for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); ++i)
        if (!strcmp(tool, adapters[i].tool)) return &adapters[i];
    return NULL;
}
int pt_dnd_state_tool(const char *tool_id) { return find_tool(tool_id) != NULL; }

static int command_campaign(const char *tool, const char *input, char campaign[128], int *create) {
    const state_ops *ops = find_tool(tool);
    cmp_json_object object;
    char path[TS_PATH];
    int read_only;
    return ops && ops->path(".", input, path, sizeof(path), create, &read_only) &&
        cmp_json_object_parse(input, &object) &&
        cmp_json_object_str(&object, "campaign_id", campaign, 128u);
}
int pt_dnd_command_valid(const char *tool_id, const char *input) {
    char campaign[128];
    int create;
    return command_campaign(tool_id, input, campaign, &create);
}

static int journal_path(const state_ops *ops, const char *directory, char path[TS_PATH]) {
    int n;
    if (!directory || !directory[0]) return 0;
    n = snprintf(path, TS_PATH, "%s/%s", directory, ops->journal);
    return n > 0 && n < TS_PATH;
}
static int copy_span(const cmp_json_field *field, char *out, size_t cap) {
    if (!field || !field->value_len || field->value_len >= cap) return 0;
    memcpy(out, field->value, field->value_len);
    out[field->value_len] = '\0';
    return 1;
}
static int transaction_decode(const state_ops *ops, const char *json, state_transaction *tx) {
    cmp_json_object object;
    const cmp_json_field *before;
    char schema[48], *record = NULL;
    int result = 0, initiative;
    memset(tx, 0, sizeof(*tx));
    if (!json || strnlen(json, DND_JOURNAL_CAP) >= DND_JOURNAL_CAP ||
        !cmp_json_object_parse(json, &object) ||
        !cmp_json_object_str(&object, "schema", schema, sizeof(schema))) return 0;
    initiative = !strcmp(ops->tool, "dnd-encounter-state") && !strcmp(schema, INITIATIVE_SCHEMA);
    if ((!initiative && strcmp(schema, ops->schema)) || object.field_count != (initiative ? 6u : 4u)) return 0;
    if (initiative && (!copy_span(cmp_json_object_field(&object, "campaign"), tx->campaign, sizeof(tx->campaign)) ||
        !copy_span(cmp_json_object_field(&object, "draws"), tx->draws, sizeof(tx->draws)))) return 0;
    before = cmp_json_object_field(&object, "before");
    if (!before || ((before->value_len != 4u || memcmp(before->value, "null", 4u)) &&
        !copy_span(before, tx->before, sizeof(tx->before)))) return 0;
    if (!copy_span(cmp_json_object_field(&object, "after"), tx->after, sizeof(tx->after))) return 0;
    record = malloc(PT_RECORD_CAP);
    if (record && copy_span(cmp_json_object_field(&object, "call"), record, PT_RECORD_CAP) &&
        pt_store_record_decode(record, &tx->completed) == 0 && !strcmp(tx->completed.tool_id, ops->tool) &&
        initiative == (!strcmp(ops->tool, "dnd-encounter-state") && pt_encounter_is_initiative(tx->completed.input_json))) result = 1;
    free(record);
    return result;
}
static size_t transaction_encode(const state_ops *ops, char *out, size_t cap, const state_transaction *tx) {
    char *record;
    int n;
    if (!out || !cap) return 0;
    out[0] = '\0';
    record = malloc(PT_RECORD_CAP);
    if (!record) return 0;
    if (!pt_store_record_encode(record, PT_RECORD_CAP, &tx->completed)) { free(record); return 0; }
    if (tx->campaign[0])
        n = snprintf(out, cap, "{\"schema\":\"%s\",\"before\":%s,\"after\":%s,\"call\":%s,\"campaign\":%s,\"draws\":%s}",
                     INITIATIVE_SCHEMA, tx->before[0] ? tx->before : "null", tx->after, record, tx->campaign, tx->draws);
    else n = snprintf(out, cap, "{\"schema\":\"%s\",\"before\":%s,\"after\":%s,\"call\":%s}",
                      ops->schema, tx->before[0] ? tx->before : "null", tx->after, record);
    free(record);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

/* Reconstruct the complete result from the original request and retained before
 * state. The artifact verifier independently checks the hash and exact bytes. */
static int transaction_valid(const state_ops *ops, const state_transaction *tx,
                             const pt_call *original, pt_dnd_identity *after_identity) {
    pt_call *expected = NULL;
    char *encoded = NULL;
    int result = 0;
    if (!original || strcmp(original->tool_id, ops->tool) ||
        (original->state != PT_ST_QUEUED && original->state != PT_ST_COMPLETED) ||
        tx->completed.state != PT_ST_COMPLETED || tx->completed.updated_at < original->updated_at) return 0;
    if (original->state == PT_ST_COMPLETED && memcmp(original, &tx->completed, sizeof(*original))) return 0;
    if (original->state == PT_ST_QUEUED && (original->output_json[0] || original->output_sha256[0] ||
        original->output_artifact[0] || original->summary[0] || original->error[0])) return 0;
    expected = malloc(sizeof(*expected));
    encoded = malloc(PT_OUT);
    if (!expected || !encoded) goto done;
    *expected = *original;
    if (tx->campaign[0]) {
        pt_dnd_identity parent;
        if (strcmp(ops->tool, "dnd-encounter-state") ||
            !pt_campaign_canonical(tx->campaign, encoded, PT_OUT, &parent) || strcmp(encoded, tx->campaign) ||
            !pt_encounter_initiative_transition(tx->before[0] ? tx->before : NULL, tx->campaign, tx->draws,
                                                encoded, PT_OUT, expected)) goto done;
    } else if (!ops->transition(tx->before[0] ? tx->before : NULL, encoded, PT_OUT, expected)) goto done;
    if (strcmp(encoded, tx->after) || !ops->canonical(tx->after, encoded, PT_OUT, after_identity) ||
        strcmp(encoded, tx->after)) goto done;
    expected->state = PT_ST_COMPLETED;
    expected->updated_at = tx->completed.updated_at;
    memcpy(expected->output_sha256, tx->completed.output_sha256, sizeof(expected->output_sha256));
    memcpy(expected->output_artifact, tx->completed.output_artifact, sizeof(expected->output_artifact));
    result = memcmp(expected, &tx->completed, sizeof(*expected)) == 0;
done:
    free(expected);
    free(encoded);
    return result;
}
static char *read_document(const char *path, size_t maximum, int *missing) {
    struct stat st;
    char *out = NULL, extra;
    size_t used = 0, length;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    *missing = fd < 0 && errno == ENOENT;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 ||
        (uint64_t)st.st_size >= maximum) goto fail;
    length = (size_t)st.st_size;
    out = malloc(length + 1u);
    if (!out) goto fail;
    while (used < length) {
        ssize_t n = read(fd, out + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        used += (size_t)n;
    }
    if (read(fd, &extra, 1u) != 0 || memchr(out, '\0', length)) goto fail;
    out[length] = '\0';
    if (close(fd)) { free(out); return NULL; }
    return out;
fail:
    close(fd);
    free(out);
    return NULL;
}

int pt_dnd_authorized(const char *state_dir, const char *tool_id, const char *input,
                       const char *user, int allow_create) {
    pt_dnd_identity identity;
    char campaign[128], path[TS_PATH], *document = NULL, *canonical = NULL;
    int create, missing = 0, n, result = 0;
    if (!state_dir || !state_dir[0] || !user || !user[0] || strnlen(user, PT_ID) >= PT_ID ||
        !command_campaign(tool_id, input, campaign, &create)) return 0;
    n = snprintf(path, sizeof(path), "%s/campaigns/%s.json", state_dir, campaign);
    if (n <= 0 || (size_t)n >= sizeof(path)) return 0;
    document = read_document(path, PT_OUT, &missing);
    if (!document) return missing && allow_create && create && !strcmp(tool_id, "dnd-campaign-state");
    canonical = malloc(PT_OUT);
    if (canonical && pt_campaign_canonical(document, canonical, PT_OUT, &identity))
        result = !strcmp(identity.campaign, campaign) && !strcmp(identity.owner, user);
    free(canonical);
    free(document);
    return result;
}

static int clear_journal(const char *state_dir, const char *path) {
    int fd, result;
    fd = open(state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    result = unlink(path) == 0 && fsync(fd) == 0;
    if (close(fd)) result = 0;
    return result;
}


static int prepare(const state_ops *ops, const char *directory, const pt_call *call, state_transaction *tx) {
    char path[TS_PATH], *document = NULL;
    pt_dnd_identity identity;
    int create, read_only, missing = 0, result = 0;
    memset(tx, 0, sizeof(*tx));
    tx->completed = *call;
    if (!ops->path(directory, call->input_json, path, sizeof(path), &create, &read_only)) return 0;
    document = read_document(path, PT_OUT, &missing);
    if (create) {
        if (!missing) goto done;
    } else if (!document || !ops->canonical(document, tx->before, sizeof(tx->before), &identity)) goto done;
    if (!strcmp(ops->tool, "dnd-encounter-state") && pt_encounter_is_initiative(call->input_json)) {
        char campaign[128];
        int ignored, n;
        if (!command_campaign(ops->tool, call->input_json, campaign, &ignored)) goto done;
        n = snprintf(path, sizeof(path), "%s/campaigns/%s.json", directory, campaign);
        if (n <= 0 || (size_t)n >= sizeof(path)) goto done;
        free(document);
        document = read_document(path, PT_OUT, &missing);
        if (!document || !pt_campaign_canonical(document, tx->campaign, sizeof(tx->campaign), &identity) ||
            !pt_encounter_prepare_initiative(tx->before[0] ? tx->before : NULL, tx->campaign, call, tx->draws) ||
            !pt_encounter_initiative_transition(tx->before[0] ? tx->before : NULL, tx->campaign, tx->draws,
                                                tx->after, sizeof(tx->after), &tx->completed)) goto done;
    } else if (!ops->transition(tx->before[0] ? tx->before : NULL, tx->after, sizeof(tx->after), &tx->completed)) goto done;
    tx->completed.state = PT_ST_COMPLETED;
    result = 1;
done:
    free(document);
    return result;
}
static int same_identity(const pt_dnd_identity *a, const pt_dnd_identity *b) {
    return !strcmp(a->owner, b->owner) && !strcmp(a->campaign, b->campaign) && !strcmp(a->object, b->object);
}

static int initiative_campaign_current(const char *directory, const state_transaction *tx, const pt_call *original) {
    pt_dnd_identity pinned, current;
    char path[TS_PATH], *document = NULL, *canonical = NULL;
    int n, missing = 0, result = 0;
    if (!tx->campaign[0]) return 1;
    canonical = malloc(PT_OUT);
    if (!canonical || !pt_campaign_canonical(tx->campaign, canonical, PT_OUT, &pinned) ||
        strcmp(canonical, tx->campaign)) goto done;
    n = snprintf(path, sizeof(path), "%s/campaigns/%s.json", directory, pinned.campaign);
    if (n <= 0 || (size_t)n >= sizeof(path)) goto done;
    document = read_document(path, PT_OUT, &missing);
    if (!document || !pt_campaign_canonical(document, canonical, PT_OUT, &current) ||
        !same_identity(&pinned, &current) || current.version < pinned.version) goto done;
    /* A completed cleanup journal cannot rewind later authorized roster edits. */
    result = current.version == pinned.version ? !strcmp(canonical, tx->campaign) : original->state == PT_ST_COMPLETED;
done:
    free(canonical);
    free(document);
    return result;
}
static int recover_one(const state_ops *ops, const char *state_dir, const char *artifact_dir,
                       pt_store *store, int restore_private, int journal_durable) {
    state_transaction *tx = NULL;
    pt_call *original;
    pt_dnd_identity after_identity, current_identity;
    char journal[TS_PATH], path[TS_PATH], *raw = NULL, *canonical = NULL, *current = NULL;
    int missing = 0, create, read_only, result = -1;
    if (!store || !journal_path(ops, state_dir, journal)) return -1;
    raw = read_document(journal, DND_JOURNAL_CAP, &missing);
    if (!raw) return missing ? 0 : -1;
    tx = calloc(1, sizeof(*tx));
    canonical = malloc(PT_OUT);
    if (!tx || !canonical || !transaction_decode(ops, raw, tx)) goto done;
    original = pt_store_get(store, tx->completed.tool_call_id);
    if (!transaction_valid(ops, tx, original, &after_identity) ||
        !ops->path(state_dir, original->input_json, path, sizeof(path), &create, &read_only) ||
        !pt_dnd_authorized(state_dir, original->tool_id, original->input_json, original->user_id,
                           original->state != PT_ST_COMPLETED) ||
        !initiative_campaign_current(state_dir, tx, original)) goto done;
    if ((restore_private ? pt_artifact_restore_private(artifact_dir, &tx->completed) :
                           pt_artifact_verify(artifact_dir, &tx->completed)) != 0) goto done;
    current = read_document(path, PT_OUT, &missing);
    if (current) {
        if (!ops->canonical(current, canonical, PT_OUT, &current_identity)) goto done;
        if (original->state == PT_ST_COMPLETED) {
            /* A completed cleanup journal must never rewind a later operation. */
            if (!same_identity(&current_identity, &after_identity) ||
                current_identity.version < after_identity.version ||
                (current_identity.version == after_identity.version && strcmp(canonical, tx->after))) goto done;
        } else if (strcmp(canonical, tx->before) && strcmp(canonical, tx->after)) goto done;
    } else if (!missing || original->state == PT_ST_COMPLETED || !create || tx->before[0]) goto done;
    /* A journal rename can remain visible after uncertain directory sync. */
    if (!journal_durable && ts_atomic_write(journal, raw, strlen(raw), 0600) != TS_OK) goto done;
    if (original->state != PT_ST_COMPLETED && !read_only &&
        ts_atomic_write(path, tx->after, strlen(tx->after), 0600) != TS_OK) goto done;
    if (pt_store_put(store, &tx->completed) != 0) goto done;
    result = clear_journal(state_dir, journal) ? 0 : -1;
done:
    free(current);
    free(canonical);
    free(tx);
    free(raw);
    return result;
}
int pt_dnd_recover(const char *state_dir, const char *artifact_dir, pt_store *store, int restore_private) {
    for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); ++i)
        if (recover_one(&adapters[i], state_dir, artifact_dir, store, restore_private, 0) != 0) return -1;
    return 0;
}
int pt_dnd_run(const char *state_dir, const char *artifact_dir, pt_store *store, pt_call *call) {
    const state_ops *ops;
    state_transaction *tx = NULL, *checked = NULL;
    pt_call *original;
    pt_dnd_identity identity;
    char path[TS_PATH], *journal = NULL;
    size_t length;
    int result = PT_DND_REJECTED;
    const char *error = "invalid D&D state command";
    if (!call) return PT_DND_REJECTED;
    call->output_json[0] = call->summary[0] = call->error[0] = '\0';
    ops = find_tool(call->tool_id);
    if (!ops || !store || !journal_path(ops, state_dir, path)) goto done;
    if (pt_dnd_recover(state_dir, artifact_dir, store, 0) != 0) {
        result = PT_DND_PENDING;
        goto done;
    }
    original = pt_store_get(store, call->tool_call_id);
    if (!original || original->state != PT_ST_QUEUED) goto done;
    error = "campaign authority is unavailable";
    if (!pt_dnd_authorized(state_dir, call->tool_id, call->input_json, call->user_id, 1)) goto done;
    tx = calloc(1, sizeof(*tx));
    checked = calloc(1, sizeof(*checked));
    journal = malloc(DND_JOURNAL_CAP);
    if (!tx || !checked || !journal || !prepare(ops, state_dir, call, tx)) goto done;
    error = "D&D state artifact persistence failed";
    if (pt_artifact_write(artifact_dir, &tx->completed) != 0) goto done;
    error = "D&D state transaction is invalid";
    length = transaction_encode(ops, journal, DND_JOURNAL_CAP, tx);
    if (!length || !transaction_decode(ops, journal, checked) ||
        !transaction_valid(ops, checked, original, &identity)) goto done;
    result = PT_DND_PENDING;
    if (ts_atomic_write(path, journal, length, 0600) == TS_OK)
        (void)recover_one(ops, state_dir, artifact_dir, store, 0, 1);
    original = pt_store_get(store, call->tool_call_id);
    if (original && original->state == PT_ST_COMPLETED) {
        *call = *original;
        result = PT_DND_OK;
    }
done:
    if (result != PT_DND_OK) {
        call->output_json[0] = call->summary[0] = '\0';
        (void)snprintf(call->error, sizeof(call->error), "%s",
                       result == PT_DND_PENDING ? "D&D state recovery is pending" : error);
    }
    free(tx);
    free(checked);
    free(journal);
    return result;
}
