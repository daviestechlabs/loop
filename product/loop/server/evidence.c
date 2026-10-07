#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int invalid(loop_response *r) { return loop_reply(r, 400, "{\"error\":\"invalid_evidence\"}"); }
static int conflict(loop_response *r) { return loop_reply(r, 409, "{\"error\":\"immutable_evidence_conflict\"}"); }
static int prepare(sqlite3 *db, sqlite3_stmt **stmt, const char *sql, const char *owner, const char *id) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) return 0;
    if (owner) sqlite3_bind_text(*stmt, 1, owner, -1, SQLITE_TRANSIENT);
    if (id) sqlite3_bind_text(*stmt, 2, id, -1, SQLITE_TRANSIENT);
    return 1;
}
static int scalar(loop_service *s, loop_response *r, const char *sql, const char *owner, const char *id) {
    sqlite3_stmt *st = NULL; int ok = 0;
    if (prepare(s->db, &st, sql, owner, id) && sqlite3_step(st) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(st, 0);
        if (value && strlen(value) < LOOP_REPLY_MAX) ok = loop_reply(r, 200, value);
        else ok = loop_reply(r, 413, "{\"error\":\"recording_requires_paged_export\"}");
    }
    sqlite3_finalize(st); return ok ? ok : loop_store_error(s,r);
}
static int turn_state(loop_service *s, const char *owner, const char *id, int *recording) {
    sqlite3_stmt *st = NULL; int found = 0;
    if (prepare(s->db, &st, "SELECT status FROM turns WHERE owner=?1 AND id=?2", owner, id) && sqlite3_step(st) == SQLITE_ROW) {
        *recording = !strcmp((const char *)sqlite3_column_text(st, 0), "recording"); found = 1;
    }
    sqlite3_finalize(st); return found;
}
static int body_object(const loop_request *q, cmp_json_object *o) {
    return q->content_type && !strcmp(q->content_type, "application/json") && q->body &&
        !memchr(q->body, 0, q->body_len) && loop_json_valid((const char *)q->body, o);
}
static int string_field(const cmp_json_object *o, const char *key, size_t max) {
    char value[16385];
    return max < sizeof(value) && cmp_json_object_str(o, key, value, max + 1) && *value;
}
/* Tokens and credentials have no place in a browser recording. Recursively reject
 * reserved field names; payload is structured evidence, not raw HTTP headers. */
static int safe_payload(const cmp_json_field *field, unsigned depth) {
    if (depth > 12) return 0;
    if (field->value[0] == '{') {
        cmp_json_object o;
        if (!cmp_json_field_object(field, &o)) return 0;
        const char *forbidden[] = { "identity_token", "access_token", "refresh_token", "authorization", "cookie", "secret" };
        for (size_t i = 0; i < o.field_count; i++) {
            const cmp_json_field *f = &o.fields[i];
            if (f->key_escaped) return 0;
            for (size_t j = 0; j < sizeof(forbidden) / sizeof(forbidden[0]); j++)
                if (f->key_len == strlen(forbidden[j]) && !memcmp(f->key, forbidden[j], f->key_len)) return 0;
            for (size_t j = 0; j < i; j++)
                if (f->key_len == o.fields[j].key_len && !memcmp(f->key, o.fields[j].key, f->key_len)) return 0;
            if (!safe_payload(f, depth + 1)) return 0;
        }
    } else if (field->value[0] == '[') {
        cmp_json_array a; cmp_json_field item; int n;
        if (!cmp_json_field_array(field, &a)) return 0;
        while ((n = cmp_json_array_next(&a, &item)) == 1) if (!safe_payload(&item, depth + 1)) return 0;
        if (n < 0) return 0;
    }
    return 1;
}
static int safe_object(const loop_request *q) {
    cmp_json_field f = { .value = (const char *)q->body, .value_len = q->body_len };
    return safe_payload(&f, 0);
}
static int immutable_insert(loop_service *s, const char *sql, const char *owner, const char *id,
                            const char *payload, const char *digest, loop_response *r) {
    sqlite3_stmt *st = NULL; int rc = SQLITE_ERROR;
    if (prepare(s->db, &st, sql, owner, id)) {
        sqlite3_bind_text(st, 3, payload, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, digest, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return (rc == SQLITE_CONSTRAINT) ? conflict(r) : loop_store_error(s,r);
    return loop_reply(r, 201, "{\"stored\":true}");
}
static int create_turn(loop_service *s, const char *owner, const loop_request *q, loop_response *r) {
    cmp_json_object o; char id[81], digest[65]; int consent = 0;
    if (q->body_len > 16384 || !body_object(q, &o) || !safe_object(q) ||
        !cmp_json_object_str(&o, "request_id", id, sizeof(id)) || !loop_id_valid(id) ||
        !cmp_json_object_bool(&o, "recording_consent", &consent) || !consent ||
        !loop_hash(q->body, q->body_len, digest)) return invalid(r);
    char purpose[81];
    if (cmp_json_object_field(&o,"rerun") ||
        (cmp_json_object_str(&o,"purpose",purpose,sizeof(purpose)) && !strcmp(purpose,"saved_input_rerun"))) return invalid(r);
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT manifest_sha FROM turns WHERE owner=?1 AND id=?2", owner, id)) return loop_store_error(s,r);
    int found = sqlite3_step(st) == SQLITE_ROW;
    int same = found && !strcmp((const char *)sqlite3_column_text(st, 0), digest);
    sqlite3_finalize(st);
    if (found) return same ? loop_reply(r, 200, "{\"stored\":true}") : conflict(r);
    return immutable_insert(s, "INSERT INTO turns(owner,id,manifest,manifest_sha) VALUES(?1,?2,?3,?4)", owner, id, (const char *)q->body, digest, r);
}
static int append_event(loop_service *s, const char *owner, const char *id, int recording, const loop_request *q, loop_response *r) {
    cmp_json_object o; int64_t seq; double elapsed; char name[81], source[32], digest[65];
    if (q->body_len > 65536 || !body_object(q, &o) || !safe_object(q) || o.field_count != 5 ||
        !cmp_json_object_i64(&o, "seq", &seq) || seq < 0 || seq >= LOOP_EVENT_MAX ||
        !cmp_json_object_str(&o, "name", name, sizeof(name)) || !loop_id_valid(name) ||
        !cmp_json_object_str(&o, "source", source, sizeof(source)) ||
        (strcmp(source, "browser") && strcmp(source, "gateway_observed")) ||
        !cmp_json_field_double(cmp_json_object_field(&o, "elapsed_ms"), &elapsed) || !isfinite(elapsed) || elapsed < 0 || elapsed > 600000 ||
        !cmp_json_object_field(&o, "payload") || !loop_hash(q->body, q->body_len, digest)) return invalid(r);
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT sha FROM events WHERE owner=?1 AND turn_id=?2 AND seq=?3", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_int64(st, 3, seq);
    int found = sqlite3_step(st) == SQLITE_ROW;
    int same = found && !strcmp((const char *)sqlite3_column_text(st, 0), digest);
    sqlite3_finalize(st);
    if (found) return same ? loop_reply(r, 200, "{\"stored\":true}") : conflict(r);
    if (!recording) return conflict(r);
    if (!prepare(s->db, &st, "SELECT COUNT(*),coalesce(SUM(length(payload)),0) FROM events WHERE owner=?1 AND turn_id=?2", owner, id)) return loop_store_error(s,r);
    int ordered = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == seq;
    int bounded = ordered && sqlite3_column_int64(st, 1) + (sqlite3_int64)q->body_len <= 2 * 1024 * 1024;
    sqlite3_finalize(st);
    if (!ordered) return loop_reply(r, 409, "{\"error\":\"event_sequence_gap\"}");
    if (!bounded) return loop_reply(r, 413, "{\"error\":\"event_evidence_capacity\"}");
    if (!prepare(s->db, &st, "INSERT INTO events(owner,turn_id,seq,payload,sha) VALUES(?1,?2,?3,?4,?5)", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_int64(st, 3, seq); sqlite3_bind_text(st, 4, (const char *)q->body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, digest, -1, SQLITE_TRANSIENT); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? loop_reply(r, 201, "{\"stored\":true}") : loop_store_error(s,r);
}
static int finish_turn(loop_service *s, const char *owner, const char *id, int recording, const loop_request *q, loop_response *r) {
    cmp_json_object o; char status[32], digest[65]; int64_t count;
    if (q->body_len > 65536 || !body_object(q, &o) || !safe_object(q) ||
        !cmp_json_object_str(&o, "status", status, sizeof(status)) ||
        (strcmp(status, "completed") && strcmp(status, "failed") && strcmp(status, "interrupted")) ||
        !cmp_json_object_i64(&o, "event_count", &count) || count < 0 || count > LOOP_EVENT_MAX ||
        !loop_hash(q->body, q->body_len, digest)) return invalid(r);
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT final_sha,(SELECT COUNT(*) FROM events WHERE owner=?1 AND turn_id=?2) FROM turns WHERE owner=?1 AND id=?2", owner, id)) return loop_store_error(s,r);
    int rc = sqlite3_step(st);
    const char *old = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    int same = old && !strcmp(old, digest);
    int counts_match = rc == SQLITE_ROW && sqlite3_column_int64(st, 1) == count;
    sqlite3_finalize(st);
    if (!recording) return same ? loop_reply(r, 200, "{\"stored\":true}") : conflict(r);
    if (!counts_match) return loop_reply(r, 409, "{\"error\":\"recording_incomplete\"}");
    if (!strcmp(status,"completed") && !loop_rerun_input_check(s,owner,id,NULL,0,0,1,r)) return r->body!=NULL;
    if (!prepare(s->db, &st, "UPDATE turns SET status=?3,final=?4,final_sha=?5 WHERE owner=?1 AND id=?2", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_text(st, 3, status, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 4, (const char *)q->body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, digest, -1, SQLITE_TRANSIENT); rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? loop_reply(r, 200, "{\"stored\":true}") : loop_store_error(s,r);
}
static int audio(loop_service *s, const char *owner, const char *id, const char *tail, int recording, const loop_request *q, loop_response *r) {
    char kind[8], extra; int rate = 0;
    if (sscanf(tail, "/audio/%7[^/]/%d%c", kind, &rate, &extra) != 2 ||
        (strcmp(kind, "input") && strcmp(kind, "output")) ||
        (rate != 16000 && rate != 22050 && rate != 24000 && rate != 44100 && rate != 48000)) return invalid(r);
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT pcm,sha,sample_rate FROM audio WHERE owner=?1 AND turn_id=?2 AND kind=?3", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_text(st, 3, kind, -1, SQLITE_TRANSIENT); int found = sqlite3_step(st) == SQLITE_ROW;
    if (!strcmp(q->method, "GET")) {
        if (!found || sqlite3_column_int(st, 2) != rate) { sqlite3_finalize(st); return loop_reply(r, 404, "{\"error\":\"audio_not_recorded\"}"); }
        r->size = (size_t)sqlite3_column_bytes(st, 0); r->body = malloc(r->size);
        if (!r->body) { sqlite3_finalize(st); return loop_store_error(s,r); }
        memcpy(r->body, sqlite3_column_blob(st, 0), r->size); r->status = 200;
        snprintf(r->content_type, sizeof(r->content_type), "application/octet-stream");
        snprintf(r->headers, sizeof(r->headers), "X-Audio-Sample-Rate: %d\r\nX-Content-SHA256: %s\r\n", rate, sqlite3_column_text(st, 1));
        sqlite3_finalize(st); return 1;
    }
    char digest[65];
    if (strcmp(q->method, "POST") || !q->content_type || strcmp(q->content_type, "application/octet-stream") ||
        !q->body_len || q->body_len > LOOP_AUDIO_MAX || q->body_len % 2 || !loop_hash(q->body, q->body_len, digest)) { sqlite3_finalize(st); return invalid(r); }
    int same = found && sqlite3_column_int(st, 2) == rate && !strcmp((const char *)sqlite3_column_text(st, 1), digest);
    sqlite3_finalize(st);
    if (found) return same ? loop_reply(r, 200, "{\"stored\":true}") : conflict(r);
    if (!recording) return conflict(r);
    if (!strcmp(kind,"input") && !loop_rerun_input_check(s,owner,id,q->body,q->body_len,rate,0,r)) return r->body!=NULL;
    if (!prepare(s->db, &st, "INSERT INTO audio(owner,turn_id,kind,sample_rate,pcm,sha) VALUES(?1,?2,?3,?4,?5,?6)", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_text(st, 3, kind, -1, SQLITE_TRANSIENT); sqlite3_bind_int(st, 4, rate);
    sqlite3_bind_blob(st, 5, q->body, (int)q->body_len, SQLITE_TRANSIENT); sqlite3_bind_text(st, 6, digest, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? loop_reply(r, 201, "{\"stored\":true}") : loop_store_error(s,r);
}
static int save_case(loop_service *s, const char *owner, const loop_request *q, loop_response *r) {
    cmp_json_object o; char id[81], source[81], digest[65]; int recording = 0;
    if (q->body_len > 32768 || !body_object(q, &o) || !safe_object(q) ||
        !cmp_json_object_str(&o, "id", id, sizeof(id)) || !loop_id_valid(id) ||
        !cmp_json_object_str(&o, "source_turn", source, sizeof(source)) || !loop_id_valid(source) ||
        !string_field(&o, "title", 160) || !string_field(&o, "expected", 8192) ||
        !string_field(&o, "category", 80) || !loop_hash(q->body, q->body_len, digest)) return invalid(r);
    if (!turn_state(s, owner, source, &recording)) return loop_reply(r, 404, "{\"error\":\"source_turn_not_found\"}");
    if (recording) return loop_reply(r, 409, "{\"error\":\"source_turn_not_final\"}");
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT sha FROM cases WHERE owner=?1 AND id=?2", owner, id)) return loop_store_error(s,r);
    int found = sqlite3_step(st) == SQLITE_ROW;
    int same = found && !strcmp((const char *)sqlite3_column_text(st, 0), digest); sqlite3_finalize(st);
    if (found) return same ? loop_reply(r, 200, "{\"stored\":true}") : conflict(r);
    if (!prepare(s->db, &st, "INSERT INTO cases(owner,id,source_turn,definition,sha) VALUES(?1,?2,?3,?4,?5)", owner, id)) return loop_store_error(s,r);
    sqlite3_bind_text(st, 3, source, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 4, (const char *)q->body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, digest, -1, SQLITE_TRANSIENT); int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE ? loop_reply(r, 201, "{\"stored\":true}") : loop_store_error(s,r);
}
int loop_evidence(loop_service *s, const char *owner, const loop_request *q, loop_response *r) {
    /* The delegated gateway route only reaches this owned, immutable projection. */
    const char *delegated="/api/anvil-model-request/";
    if (!strncmp(q->path,delegated,strlen(delegated))) {
        const char *id=q->path+strlen(delegated);
        if (!loop_id_valid(id)) return invalid(r);
        if (strcmp(q->method,"GET")) return loop_reply(r,405,"{\"error\":\"method_not_allowed\"}");
        if (!s->sso || !q->verified_owner) return loop_reply(r,401,"{\"error\":\"shared_session_required\"}");
        return loop_model_request(s,owner,id,r);
    }
    if (!strcmp(q->path, "/api/turns")) {
        if (!strcmp(q->method, "POST")) return create_turn(s, owner, q, r);
        if (!strcmp(q->method, "GET")) return scalar(s, r,
            "SELECT json_object('turns',json_group_array(json(item))) FROM (SELECT json_object('id',id,'created',created,'status',status,'purpose',json_extract(manifest,'$.purpose'),'manifest_sha256',manifest_sha,'final',json(final),'final_sha256',final_sha) item FROM turns WHERE owner=?1 ORDER BY created DESC,rowid DESC LIMIT 50)", owner, NULL);
    }
    if (!strcmp(q->path, "/api/captures")) {
        if (!strcmp(q->method, "POST")) return save_case(s, owner, q, r);
        if (!strcmp(q->method, "GET")) return scalar(s, r,
            "SELECT json_object('captures',json_group_array(json(item))) FROM (SELECT json_object('definition',json(definition),'sha256',sha,'created',created) item FROM cases WHERE owner=?1 ORDER BY created DESC,rowid DESC LIMIT 50)", owner, NULL);
    }
    if (!strncmp(q->path, "/api/turns/", 11)) {
        char id[81]; const char *start = q->path + 11, *tail = strchr(start, '/');
        size_t n = tail ? (size_t)(tail - start) : strlen(start);
        if (!n || n >= sizeof(id)) return invalid(r);
        memcpy(id, start, n); id[n] = 0;
        if (!loop_id_valid(id)) return invalid(r);
        int recording = 0;
        if (!turn_state(s, owner, id, &recording)) return loop_reply(r, 404, "{\"error\":\"turn_not_found\"}");
        if (tail && !strcmp(tail, "/anvil-bundle") && !strcmp(q->method,"POST")) return loop_anvil_bundle(s,owner,id,q,r);
        if (tail && !strcmp(tail, "/anvil-report") && !strcmp(q->method,"POST")) return loop_anvil_export(s,owner,id,q,r);
        if (tail && !strcmp(tail, "/anvil-links")) return loop_anvil_links(s, owner, id, q, r);
        if (tail && !strcmp(tail, "/reruns") && !strcmp(q->method,"POST")) return loop_rerun(s,owner,id,q,r);
        if (tail && !strcmp(tail, "/report") && !strcmp(q->method, "GET")) return loop_report(s, owner, id, r);
        if (tail && !strcmp(tail, "/receipt") && !strcmp(q->method, "GET")) return loop_receipt(s, owner, id, r);
        if (tail && !strcmp(tail, "/model-request") && !strcmp(q->method, "GET")) return loop_model_request(s, owner, id, r);
        if (tail && !strcmp(tail, "/replay/input.s16le") && !strcmp(q->method, "GET")) return loop_replay(s, owner, id, "input", 1, r);
        if (tail && !strcmp(tail, "/replay/output.s16le") && !strcmp(q->method, "GET")) return loop_replay(s, owner, id, "output", 1, r);
        if (tail && !strcmp(tail, "/replay/input.wav") && !strcmp(q->method, "GET")) return loop_replay(s, owner, id, "input", 0, r);
        if (tail && !strcmp(tail, "/replay/output.wav") && !strcmp(q->method, "GET")) return loop_replay(s, owner, id, "output", 0, r);
        if (!tail && !strcmp(q->method, "GET")) return scalar(s, r,
            "SELECT json_object('id',id,'status',status,'created',created,'purpose',json_extract(manifest,'$.purpose'),'evidence_origin','browser_observed',"
            "'manifest',json(manifest),'manifest_sha256',manifest_sha,'final',json(final),'final_sha256',final_sha,"
            "'events',json((SELECT json_group_array(json(item)) FROM (SELECT json_object('event',json(payload),'sha256',sha,'received',received) item FROM events WHERE owner=?1 AND turn_id=?2 ORDER BY seq))),"
            "'audio',json((SELECT json_group_array(json_object('kind',kind,'sample_rate',sample_rate,'bytes',length(pcm),'sha256',sha)) FROM audio WHERE owner=?1 AND turn_id=?2))) FROM turns WHERE owner=?1 AND id=?2", owner, id);
        if (tail && !strcmp(q->method, "POST")) {
            if (!strcmp(tail, "/events")) return append_event(s, owner, id, recording, q, r);
            if (!strcmp(tail, "/finish")) return finish_turn(s, owner, id, recording, q, r);
        }
        if (tail && !strncmp(tail, "/audio/", 7)) return audio(s, owner, id, tail, recording, q, r);
    }
    return loop_reply(r, 404, "{\"error\":\"not_found\"}");
}
