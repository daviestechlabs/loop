#ifndef LOOP_H
#define LOOP_H
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "cmp_json.h"
#include "cmp_gateway_identity.h"
#define LOOP_VOICE_AUTHORITY "https://voice-session-gateway.lab.daviestechlabs.io:8443"
#define LOOP_WEBTRANSPORT_ENDPOINT LOOP_VOICE_AUTHORITY "/v1/voice/turns"
#define LOOP_ANVIL_ORIGIN "https://anvil.lab.daviestechlabs.io"
#define LOOP_ORIGIN_CAP 256
#define LOOP_DB_DEFAULT_MAX (UINT64_C(2) * 1024 * 1024 * 1024)
#define LOOP_DB_HARD_MAX (UINT64_C(4) * 1024 * 1024 * 1024)
#define LOOP_BODY_MAX (4u * 1024u * 1024u)
#define LOOP_REPLY_MAX (6u * 1024u * 1024u)
#define LOOP_EVENT_MAX 2048
#define LOOP_AUDIO_MAX (4u * 1024u * 1024u)

/* OAuth parsing needs more than musl's default 128 KiB thread stack. */
#define LOOP_THREAD_STACK_BYTES (2u * 1024u * 1024u)
static inline int loop_thread_create(pthread_t *thread, void *(*start)(void *), void *arg) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, LOOP_THREAD_STACK_BYTES);
    if (!rc) rc = pthread_create(thread, &attr, start, arg);
    pthread_attr_destroy(&attr);
    return rc;
}


typedef struct {
    sqlite3 *db;
    cmp_gateway_identity *sso;
    pthread_mutex_t mutex;
    pthread_mutex_t oauth_mutex;
    const char *origin;
    const char *operators;
    const char *gateway_secret;
    const char *wt_url;
    const char *anvil_origin;
    const char *static_root;
    uint64_t database_max_bytes;
    int test_mode;
} loop_service;
typedef struct {
    const char *method, *path, *cookie, *origin, *content_type;
    const char *gateway_token, *verified_owner;
    const unsigned char *body;
    size_t body_len;
} loop_request;
typedef struct {
    int status;
    char *body;
    size_t size;
    char content_type[80];
    char headers[8192];
} loop_response;
int loop_storage_limit(loop_service *s);
int loop_store_error(loop_service *s, loop_response *r);
int loop_backup(const char *source, const char *destination);
int loop_backup_recordings(const char *source, const char *destination, uint64_t *verified, uint64_t *unfinished);
int loop_verify_recordings(const char *source, uint64_t *verified, uint64_t *unfinished);
#ifdef LOOP_TESTING
extern int loop_backup_test_directory_sync_error;
#endif
int loop_open(loop_service *s, const char *path);
void loop_close(loop_service *s);
int loop_handle(loop_service *s, const loop_request *q, loop_response *r);
void loop_response_free(loop_response *r);
int loop_session_create(loop_service *s, const char *owner, char token[65]);
int loop_hash(const void *data, size_t len, char hex[65]);
int loop_id_valid(const char *s);
int loop_json_valid(const char *body, cmp_json_object *o);
int loop_reply(loop_response *r, int status, const char *json);
int loop_evidence(loop_service *s, const char *owner, const loop_request *q, loop_response *r);
int loop_report(loop_service *s, const char *owner, const char *id, loop_response *r);
int loop_receipt(loop_service *s, const char *owner, const char *id, loop_response *r);
int loop_model_request(loop_service *s, const char *owner, const char *id, loop_response *r);
int loop_replay(loop_service *s, const char *owner, const char *id, const char *kind, int raw, loop_response *r);
int loop_anvil_remote_read(loop_service *s, const loop_request *q, loop_response *r,
    int (*fetch)(const char *, const char *, uint8_t *, size_t, size_t *));
int loop_anvil_links(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r);
int loop_anvil_bundle(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r);
int loop_anvil_export(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r);
int loop_rerun(loop_service *s, const char *owner, const char *source, const loop_request *q, loop_response *r);
int loop_rerun_input_check(loop_service *s, const char *owner, const char *id, const void *pcm, size_t bytes, int rate, int complete, loop_response *r);
int loop_http_serve(loop_service *s, const char *bind_address, int port);
const char *loop_runtime_configuration_error(const loop_service *s, const char *oauth_redirect);
int loop_https_origin_valid(const char *origin);
int loop_voice_origin(const loop_service *s, char origin[LOOP_ORIGIN_CAP]);
const char *loop_anvil_origin(const loop_service *s);
#endif
