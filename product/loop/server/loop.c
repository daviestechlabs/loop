#include "loop.h"
#include "cmp_oauth.h"
#include "voice_auth.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int loop_hash(const void *data, size_t len, char hex[65]) {
    unsigned char hash[32]; unsigned int n = 0;
    if (!EVP_Digest(data, len, hash, &n, EVP_sha256(), NULL) || n != 32) return 0;
    for (size_t i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return 1;
}
static int random_token(char out[65]) {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return 0;
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
    OPENSSL_cleanse(bytes, sizeof(bytes));
    return 1;
}
int loop_id_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 80) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return 0;
    return 1;
}
/* Reject ambiguous JSON directories. The shared parser validates syntax and UTF-8. */
int loop_json_valid(const char *body, cmp_json_object *o) {
    if (!cmp_json_object_parse(body, o)) return 0;
    for (size_t i = 0; i < o->field_count; i++) {
        if (o->fields[i].key_escaped) return 0;
        for (size_t j = 0; j < i; j++)
            if (o->fields[j].key_len == o->fields[i].key_len &&
                !memcmp(o->fields[j].key, o->fields[i].key, o->fields[i].key_len)) return 0;
    }
    return 1;
}
int loop_reply(loop_response *r, int status, const char *json) {
    r->status = status; r->body = strdup(json); r->size = strlen(json);
    snprintf(r->content_type, sizeof(r->content_type), "application/json");
    return r->body != NULL;
}
void loop_response_free(loop_response *r) { free(r->body); memset(r, 0, sizeof(*r)); }
int loop_open(loop_service *s, const char *path) {
    if (pthread_mutex_init(&s->mutex, NULL)) return 0;
    if (pthread_mutex_init(&s->oauth_mutex,NULL)) { pthread_mutex_destroy(&s->mutex); return 0; }
    if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        sqlite3_close(s->db); s->db = NULL; pthread_mutex_destroy(&s->oauth_mutex); pthread_mutex_destroy(&s->mutex); return 0;
    }
    sqlite3_busy_timeout(s->db, 5000);
    sqlite3_stmt *version_stmt=NULL;
    int version=-1;
    if (sqlite3_prepare_v2(s->db,"PRAGMA user_version",-1,&version_stmt,NULL)==SQLITE_OK && sqlite3_step(version_stmt)==SQLITE_ROW)
        version=sqlite3_column_int(version_stmt,0);
    sqlite3_finalize(version_stmt);
    if (version<0 || version>2 || !loop_storage_limit(s)) { loop_close(s); return 0; }
    const char *schema =
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS sessions (hash TEXT PRIMARY KEY, owner TEXT NOT NULL, expires INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS turns (id TEXT NOT NULL, owner TEXT NOT NULL, created INTEGER NOT NULL DEFAULT(unixepoch()), "
        " status TEXT NOT NULL DEFAULT 'recording', manifest TEXT NOT NULL, manifest_sha TEXT NOT NULL, "
        " final TEXT, final_sha TEXT, PRIMARY KEY(owner,id));"
        "CREATE TABLE IF NOT EXISTS events (owner TEXT NOT NULL, turn_id TEXT NOT NULL, seq INTEGER NOT NULL, "
        " received INTEGER NOT NULL DEFAULT(unixepoch()), payload TEXT NOT NULL, sha TEXT NOT NULL, "
        " PRIMARY KEY(owner,turn_id,seq), FOREIGN KEY(owner,turn_id) REFERENCES turns(owner,id));"
        "CREATE TABLE IF NOT EXISTS audio (owner TEXT NOT NULL, turn_id TEXT NOT NULL, kind TEXT NOT NULL, "
        " sample_rate INTEGER NOT NULL, pcm BLOB NOT NULL, sha TEXT NOT NULL, "
        " PRIMARY KEY(owner,turn_id,kind), FOREIGN KEY(owner,turn_id) REFERENCES turns(owner,id));"
        "CREATE TABLE IF NOT EXISTS cases (id TEXT NOT NULL, owner TEXT NOT NULL, source_turn TEXT NOT NULL, "
        " created INTEGER NOT NULL DEFAULT(unixepoch()), definition TEXT NOT NULL, sha TEXT NOT NULL, "
        " PRIMARY KEY(owner,id), FOREIGN KEY(owner,source_turn) REFERENCES turns(owner,id));"
        "CREATE TABLE IF NOT EXISTS anvil_links (owner TEXT NOT NULL, turn_id TEXT NOT NULL, id TEXT NOT NULL, "
        " created INTEGER NOT NULL DEFAULT(unixepoch()), definition TEXT NOT NULL, sha TEXT NOT NULL, "
        " PRIMARY KEY(owner,turn_id,id), FOREIGN KEY(owner,turn_id) REFERENCES turns(owner,id));"
        "PRAGMA user_version=2; COMMIT;";
    if (sqlite3_exec(s->db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL); loop_close(s); return 0;
    }
    return 1;
}
void loop_close(loop_service *s) { sqlite3_close(s->db); s->db = NULL; pthread_mutex_destroy(&s->oauth_mutex); pthread_mutex_destroy(&s->mutex); }
int loop_session_create(loop_service *s, const char *owner, char token[65]) {
    char digest[65]; sqlite3_stmt *stmt = NULL; int ok = 0;
    if (!random_token(token) || !loop_hash(token, 64, digest)) return 0;
    if (sqlite3_prepare_v2(s->db, "INSERT INTO sessions VALUES(?1,?2,?3)", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, digest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL) + 28800);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt); return ok;
}
static int cookie_value(const char *cookies, const char *name, char *out, size_t cap) {
    int found = 0; size_t key_len = strlen(name);
    if (!cookies) return 0;
    for (const char *p = cookies; *p;) {
        while (*p == ' ' || *p == ';') p++;
        const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > key_len && !strncmp(p, name, key_len) && p[key_len] == '=') {
            size_t n = (size_t)(end - p) - key_len - 1;
            if (found || n >= cap) return 0;
            memcpy(out, p + key_len + 1, n); out[n] = 0; found = 1;
        }
        p = end;
    }
    return found;
}
/* Owners are hashes of admitted OAuth principals. Recheck current policy on every request. */
static int owner_allowed(const loop_service *s, const char *owner) {
#ifdef LOOP_TESTING
    if (s->test_mode && !strcmp(owner, "fixture-operator")) return 1;
#endif
    if (!s->operators || strlen(owner) != 64) return 0;
    for (const char *p = s->operators; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        char digest[65];
        if (end != p && loop_hash(p, (size_t)(end - p), digest) &&
            !CRYPTO_memcmp(owner, digest, 64)) return 1;
        p = *end ? end + 1 : end;
    }
    return 0;
}
static int owner_from_cookie(loop_service *s, const char *cookie, char owner[129]) {
    char token[65], digest[65]; sqlite3_stmt *stmt = NULL; int ok = 0;
    if (!cookie_value(cookie, "loop_session", token, sizeof(token)) || strlen(token) != 64 || !loop_hash(token, 64, digest)) return 0;
    if (sqlite3_prepare_v2(s->db, "SELECT owner FROM sessions WHERE hash=?1 AND expires>?2", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, digest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(stmt, 0);
            if (value && strlen(value) < 129 && owner_allowed(s, value)) { strcpy(owner, value); ok = 1; }
        }
    }
    OPENSSL_cleanse(token, sizeof(token)); sqlite3_finalize(stmt); return ok;
}
static int operator_allowed(const char *allowlist, const char *principal) {
    if (!allowlist) return 0;
    const char *p = allowlist;
    while (*p) {
        const char *end = strchr(p, '\n'); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) == strlen(principal) && !memcmp(p, principal, (size_t)(end - p))) return 1;
        p = *end ? end + 1 : end;
    }
    return 0;
}
static int oauth(loop_service *s, const loop_request *q, loop_response *r) {
    char state[CMP_OAUTH_STATE_CAP], location[CMP_OAUTH_LOCATION_CAP], reason[CMP_OAUTH_REASON_CAP];
    if (!strcmp(q->path, "/api/oauth/login")) {
        if (!cmp_oauth_enabled() || !s->operators || !*s->operators) return loop_reply(r, 503, "{\"error\":\"oauth_not_configured\"}");
        if (cmp_oauth_begin("/api/oauth/login", state, sizeof(state), location, sizeof(location), reason, sizeof(reason)))
            return loop_reply(r, 503, "{\"error\":\"oauth_begin_failed\"}");
        snprintf(r->headers, sizeof(r->headers), "Location: %s\r\nSet-Cookie: loop_oauth=%s; Path=/api/oauth; Max-Age=600; HttpOnly; Secure; SameSite=Lax\r\n", location, state);
        return loop_reply(r, 302, "{}");
    }
    if (!strncmp(q->path, "/api/oauth/callback?", 20)) {
        cmp_oauth_identity identity; char principal[384], owner[65], token[65];
        if (!cookie_value(q->cookie, "loop_oauth", state, sizeof(state)) ||
            cmp_oauth_finish(q->path, state, &identity, reason, sizeof(reason)))
            return loop_reply(r, 401, "{\"error\":\"oauth_callback_failed\"}");
        int n = snprintf(principal, sizeof(principal), "%s:%s", identity.provider, identity.subject);
        if (n < 0 || (size_t)n >= sizeof(principal) || !operator_allowed(s->operators, principal))
            return loop_reply(r, 403, "{\"error\":\"operator_not_admitted\"}");
        pthread_mutex_lock(&s->mutex);
        int stored=loop_hash(principal,strlen(principal),owner) && loop_session_create(s,owner,token);
        pthread_mutex_unlock(&s->mutex);
        if (!stored) {
            OPENSSL_cleanse(token,sizeof(token));
            return loop_reply(r,503,"{\"error\":\"session_unavailable\"}");
        }
        snprintf(r->headers, sizeof(r->headers), "Location: /\r\nSet-Cookie: loop_session=%s; Path=/; Max-Age=28800; HttpOnly; Secure; SameSite=Strict\r\nSet-Cookie: loop_oauth=; Path=/api/oauth; Max-Age=0; HttpOnly; Secure; SameSite=Lax\r\n", token);
        OPENSSL_cleanse(token, sizeof(token)); return loop_reply(r, 302, "{}");
    }
    return 0;
}
const char *loop_runtime_configuration_error(const loop_service *s, const char *oauth_redirect) {
    if (!loop_https_origin_valid(s->origin)) return "LOOP_ORIGIN must be an HTTPS origin";
    if (!s->sso && (!s->operators || !*s->operators)) return "LOOP_OPERATOR_SUBJECTS is required";
    if (s->database_max_bytes && (s->database_max_bytes<1024u*1024u || s->database_max_bytes>LOOP_DB_HARD_MAX))
        return "LOOP_DB_MAX_BYTES must be between 1048576 and 4294967296";
    char expected[544];
    snprintf(expected,sizeof(expected),"%s/api/oauth/callback",s->origin);
    if (!s->sso && (!oauth_redirect || strcmp(expected,oauth_redirect))) return "OAUTH_REDIRECT_URL must match LOOP_ORIGIN plus /api/oauth/callback";
    if (!s->gateway_secret || strlen(s->gateway_secret)<32) return "VOICE_GATEWAY_TOKEN must contain at least 32 bytes";
    char voice_origin[LOOP_ORIGIN_CAP];
    if (!loop_voice_origin(s,voice_origin))
        return "LOOP_WEBTRANSPORT_URL must use HTTPS and the exact /v1/voice/turns path";
    if (!loop_anvil_origin(s)) return "LOOP_ANVIL_ORIGIN must be an HTTPS origin";
    return NULL;
}
static int handle_locked(loop_service *s, const loop_request *q, loop_response *r) {
    char owner[129];
#ifdef LOOP_TESTING
    if (s->test_mode && !strcmp(q->path, "/api/test/session") && !strcmp(q->method, "GET")) {
        char token[65];
        if (!loop_session_create(s, "fixture-operator", token)) return loop_reply(r, 503, "{}");
        snprintf(r->headers, sizeof(r->headers), "Location: /\r\nSet-Cookie: loop_session=%s; Path=/; HttpOnly; SameSite=Strict\r\n", token);
        OPENSSL_cleanse(token, sizeof(token)); return loop_reply(r, 302, "{}");
    }
#endif
    if (strcmp(q->method, "GET") && (!q->origin || !s->origin || strcmp(q->origin, s->origin)))
        return loop_reply(r, 403, "{\"error\":\"origin_mismatch\"}");
    if (s->sso) {
        if (!q->verified_owner) return loop_reply(r,401,"{\"error\":\"sign_in_required\"}");
        snprintf(owner,sizeof(owner),"%s",q->verified_owner);
    } else if (!owner_from_cookie(s,q->cookie,owner)) return loop_reply(r,401,"{\"error\":\"sign_in_required\"}");
    if (!strcmp(q->path, "/api/session") && !strcmp(q->method, "GET")) {
        char json[320]; snprintf(json, sizeof(json), "{\"owner\":\"%s\",\"authenticated\":true,\"recording_origin\":\"browser_observed\"}", owner);
        return loop_reply(r, 200, json);
    }
    if (!strcmp(q->path,"/api/config") && !strcmp(q->method,"GET")) {
        const char *origin=loop_anvil_origin(s); char json[320];
        if (!origin) return loop_reply(r,503,"{\"error\":\"anvil_not_configured\"}");
        snprintf(json,sizeof(json),"{\"anvil_origin\":\"%s\"}",origin);
        return loop_reply(r,200,json);
    }
    if (!strcmp(q->path, "/api/logout") && !strcmp(q->method, "POST")) {
        if (s->sso) return loop_reply(r,200,"{\"logout_url\":\"/oauth2/logout\",\"scope\":\"shared_gateway_session\"}");
        char token[65], digest[65]; sqlite3_stmt *stmt = NULL;
        if (!cookie_value(q->cookie, "loop_session", token, sizeof(token)) || !loop_hash(token, strlen(token), digest)) return loop_reply(r, 500, "{\"error\":\"logout_failed\"}");
        int ok = sqlite3_prepare_v2(s->db, "DELETE FROM sessions WHERE hash=?1", -1, &stmt, NULL) == SQLITE_OK;
        if (ok) { sqlite3_bind_text(stmt, 1, digest, -1, SQLITE_TRANSIENT); ok = sqlite3_step(stmt) == SQLITE_DONE; }
        sqlite3_finalize(stmt); OPENSSL_cleanse(token, sizeof(token));
        if (!ok) return loop_reply(r, 503, "{\"error\":\"logout_failed\"}");
        snprintf(r->headers, sizeof(r->headers), "Set-Cookie: loop_session=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict\r\n");
        return loop_reply(r, 200, "{\"ok\":true}");
    }
    if (!strcmp(q->path, "/api/turn-identity") && !strcmp(q->method, "POST")) {
        cmp_json_object o; char id[81], token[VOICE_AUTH_IDENTITY_TOKEN_CAP], json[2048];
        int capture = 0;
        if (!q->body || q->body_len > 512 || !q->content_type || strcmp(q->content_type,"application/json") ||
            !loop_json_valid((const char *)q->body, &o) || (o.field_count != 1 && o.field_count != 2) ||
            !cmp_json_object_str(&o, "request_id", id, sizeof(id)) || !loop_id_valid(id) ||
            (o.field_count == 2 && !cmp_json_object_bool(&o,"capture_model_request",&capture)))
            return loop_reply(r, 400, "{\"error\":\"invalid_request_id\"}");
        if (capture) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(s->db,"SELECT status,manifest,manifest_sha FROM turns WHERE owner=?1 AND id=?2",-1,&st,NULL)!=SQLITE_OK)
                return loop_store_error(s,r);
            sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,id,-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(st), admitted=0;
            if (rc==SQLITE_ROW) {
                cmp_json_object manifest; char digest[65], manifest_id[81]; int consent=0, model_consent=0;
                const char *body=(const char *)sqlite3_column_text(st,1);
                int bytes=sqlite3_column_bytes(st,1);
                admitted=body && bytes>0 && strlen(body)==(size_t)bytes &&
                    !strcmp((const char *)sqlite3_column_text(st,0),"recording") &&
                    loop_hash(body,(size_t)bytes,digest) &&
                    !strcmp(digest,(const char *)sqlite3_column_text(st,2)) &&
                    loop_json_valid(body,&manifest) &&
                    cmp_json_object_str(&manifest,"request_id",manifest_id,sizeof(manifest_id)) && !strcmp(manifest_id,id) &&
                    cmp_json_object_bool(&manifest,"recording_consent",&consent) && consent &&
                    cmp_json_object_bool(&manifest,"model_request_consent",&model_consent) && model_consent;
            }
            sqlite3_finalize(st);
            if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) return loop_store_error(s,r);
            if (!admitted) return loop_reply(r,409,"{\"error\":\"model_capture_not_admitted\"}");
        }
        int (*issue)(const char *,size_t,const char *,const char *,int,int64_t,int64_t,char *,size_t)=
            capture?voice_auth_identity_issue_capture:voice_auth_identity_issue;
        char voice_origin[LOOP_ORIGIN_CAP];
        if (!s->gateway_secret || strlen(s->gateway_secret) < 32 || !loop_voice_origin(s,voice_origin) ||
            issue(s->gateway_secret, strlen(s->gateway_secret), id, owner, 0, (int64_t)time(NULL), 90, token, sizeof(token)))
            return loop_reply(r, 503, "{\"error\":\"voice_identity_not_configured\"}");
        snprintf(json, sizeof(json), "{\"request_id\":\"%s\",\"identity_token\":\"%s\",\"web_transport_endpoint\":\"%s\",\"expires_in\":90}", id, token, s->wt_url);
        OPENSSL_cleanse(token, sizeof(token)); return loop_reply(r, 200, json);
    }
    return loop_evidence(s, owner, q, r);
}
int loop_handle(loop_service *s, const loop_request *q, loop_response *r) {
    memset(r, 0, sizeof(*r));
    if (q->body_len > LOOP_BODY_MAX) return loop_reply(r, 413, "{\"error\":\"body_too_large\"}");
    if (!strcmp(q->path, "/healthz") && !strcmp(q->method, "GET")) return loop_reply(r, 200, "{\"ok\":true}");
    if (q->body_len && q->content_type && !strcmp(q->content_type, "application/json") && memchr(q->body, 0, q->body_len))
        return loop_reply(r, 400, "{\"error\":\"invalid_json\"}");
    char subject[256],principal[1536],owner[65]; loop_request authenticated=*q;
    /* Never accept a caller-supplied verified owner, or a legacy cookie in SSO mode. */
    authenticated.verified_owner=NULL;
    if (s->sso) {
        if (!strcmp(q->method,"GET") && !strcmp(q->path,"/api/oauth/login")) {
            snprintf(r->headers,sizeof(r->headers),"Location: /\r\n");
            return loop_reply(r,302,"{}");
        }
        int status=cmp_gateway_identity_verify(s->sso,q->gateway_token,subject);
        if (status!=200) return loop_reply(r,status,status==403?"{\"error\":\"operator_not_admitted\"}":
            status==503?"{\"error\":\"sso_keys_unavailable\"}":"{\"error\":\"sign_in_required\"}");
        int n=snprintf(principal,sizeof(principal),"%s\n%s",s->sso->policy.issuer,subject);
        if (n<0 || (size_t)n>=sizeof(principal) || !loop_hash(principal,(size_t)n,owner)) return loop_reply(r,503,"{}");
        authenticated.verified_owner=owner;
        if (!strncmp(q->path,"/api/turns/",11) && strstr(q->path,"/anvil-links/"))
            return loop_anvil_remote_read(s,&authenticated,r,NULL);
        pthread_mutex_lock(&s->mutex);
        int result=handle_locked(s,&authenticated,r);
        pthread_mutex_unlock(&s->mutex); return result;
    }
    /* Configuration is immutable while serving. OAuth owns its short state lock;
     * provider I/O must never hold the evidence database lock. */
    if (!strcmp(q->method,"GET") && !strcmp(q->path,"/api/oauth/login")) return oauth(s,q,r);
    if (!strcmp(q->method,"GET") && !strncmp(q->path,"/api/oauth/callback?",20)) {
        if (pthread_mutex_trylock(&s->oauth_mutex)) {
            snprintf(r->headers,sizeof(r->headers),"Retry-After: 1\r\n");
            return loop_reply(r,503,"{\"error\":\"oauth_callback_busy\"}");
        }
        int result=oauth(s,q,r);
        pthread_mutex_unlock(&s->oauth_mutex);
        return result;
    }
    pthread_mutex_lock(&s->mutex);
    int result = handle_locked(s, q, r);
    pthread_mutex_unlock(&s->mutex);
    return result;
}
