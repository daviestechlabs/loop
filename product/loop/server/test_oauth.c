#include "loop.h"
#include "cmp_oauth.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    loop_service *service;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int entered, release, probe_done, fail, denied, store_failure;
    char callback[256], cookie[128], second_callback[256], second_cookie[128], session[128];
    loop_response response;
} fixture;

static loop_response request(loop_service *s, const char *method, const char *path, const char *cookie, const char *body, int status) {
    loop_request q={.method=method,.path=path,.cookie=cookie,.origin=s->origin,.content_type="application/json",
        .body=(const unsigned char *)body,.body_len=body?strlen(body):0};
    loop_response r; assert(loop_handle(s,&q,&r));
    assert(r.status==status); return r;
}
static int provider(const char *method, const char *url, const char *content_type, const char *authorization,
                    const uint8_t *body, size_t body_len, uint8_t *out, size_t cap, size_t *len, void *user) {
    (void)url; (void)content_type; (void)authorization; (void)body; (void)body_len;
    fixture *f=user;
    if (!strcmp(method,"POST")) {
        pthread_mutex_lock(&f->mutex);
        f->entered=1; pthread_cond_broadcast(&f->changed);
        while (!f->release) pthread_cond_wait(&f->changed,&f->mutex);
        pthread_mutex_unlock(&f->mutex);
        if (f->fail) return -1;
    }
    const char *json=!strcmp(method,"POST")?"{\"access_token\":\"synthetic\",\"token_type\":\"Bearer\"}":
        f->denied?"{\"sub\":\"not-admitted\",\"preferred_username\":\"test\"}":"{\"sub\":\"operator-a\",\"preferred_username\":\"test\"}";
    *len=strlen(json); assert(*len<cap); memcpy(out,json,*len); return 0;
}
static void login(loop_service *s, char callback[256], char cookie[128]) {
    loop_response r=request(s,"GET","/api/oauth/login","",NULL,302);
    const char *start=strstr(r.headers,"loop_oauth="); assert(start); start+=strlen("loop_oauth=");
    const char *end=strchr(start,';'); assert(end && (size_t)(end-start)<CMP_OAUTH_STATE_CAP);
    char state[CMP_OAUTH_STATE_CAP]; snprintf(state,sizeof(state),"%.*s",(int)(end-start),start);
    snprintf(callback,256,"/api/oauth/callback?code=synthetic&state=%s",state);
    snprintf(cookie,128,"loop_oauth=%s",state); loop_response_free(&r);
}
static void *callback_thread(void *arg) {
    fixture *f=arg;
    f->response=request(f->service,"GET",f->callback,f->cookie,NULL,f->fail?401:f->denied?403:f->store_failure?503:302);
    return NULL;
}
static void *probe_thread(void *arg) {
    fixture *f=arg; loop_service *s=f->service;
    loop_response r=request(s,"GET","/api/session",f->session,NULL,200); loop_response_free(&r);
    r=request(s,"POST","/api/turns",f->session,"{\"request_id\":\"oauth-fault-evidence\",\"recording_consent\":true}",201); loop_response_free(&r);
    r=request(s,"POST","/api/turn-identity",f->session,"{\"request_id\":\"during-oauth\"}",200); loop_response_free(&r);
    r=request(s,"GET",f->second_callback,f->second_cookie,NULL,503);
    assert(strstr(r.body,"oauth_callback_busy") && strstr(r.headers,"Retry-After: 1\r\n")); loop_response_free(&r);
    r=request(s,"GET","/api/oauth/login","",NULL,302); loop_response_free(&r);
    pthread_mutex_lock(&f->mutex); f->probe_done=1; pthread_cond_broadcast(&f->changed); pthread_mutex_unlock(&f->mutex);
    return NULL;
}
static int wait_for(fixture *f, int *flag) {
    struct timespec deadline; assert(!clock_gettime(CLOCK_REALTIME,&deadline)); deadline.tv_sec+=5;
    int rc=0;
    while (!*flag && rc!=ETIMEDOUT) { rc=pthread_cond_timedwait(&f->changed,&f->mutex,&deadline); assert(!rc || rc==ETIMEDOUT); }
    return *flag;
}
int main(void) {
    assert(!setenv("OAUTH_CLIENT_ID","loop-test",1));
    assert(!setenv("OAUTH_CLIENT_SECRET","synthetic-test-secret",1));
    assert(!setenv("OAUTH_PROVIDER","authentik",1));
    assert(!setenv("OAUTH_REDIRECT_URL","https://loop.test/api/oauth/callback",1));
    assert(!setenv("OAUTH_AUTH_URL","https://identity.test/authorize",1));
    assert(!setenv("OAUTH_TOKEN_URL","https://identity.test/token",1));
    assert(!setenv("OAUTH_USERINFO_URL","https://identity.test/userinfo",1));
    for (int mode=0;mode<4;mode++) {
        loop_service s={.origin="https://loop.test",.operators="authentik:operator-a",
            .gateway_secret="synthetic-secret-at-least-thirty-two-bytes",.wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
        assert(loop_open(&s,":memory:") && cmp_oauth_init()==CMP_OAUTH_OK);
        fixture f={.service=&s,.fail=mode==1,.denied=mode==2,.store_failure=mode==3};
        assert(!pthread_mutex_init(&f.mutex,NULL) && !pthread_cond_init(&f.changed,NULL));
        char owner[65],token[65]; assert(loop_hash("authentik:operator-a",strlen("authentik:operator-a"),owner));
        assert(loop_session_create(&s,owner,token)); snprintf(f.session,sizeof(f.session),"loop_session=%s",token);
        if (f.store_failure) assert(sqlite3_exec(s.db,"CREATE TRIGGER reject_session BEFORE INSERT ON sessions BEGIN SELECT RAISE(ABORT,'test storage failure'); END",NULL,NULL,NULL)==SQLITE_OK);
        cmp_oauth_set_http_for_tests(provider,&f);
        login(&s,f.callback,f.cookie); login(&s,f.second_callback,f.second_cookie);
        pthread_t callback,probe; assert(!loop_thread_create(&callback,callback_thread,&f));
        pthread_mutex_lock(&f.mutex); int entered=wait_for(&f,&f.entered); pthread_mutex_unlock(&f.mutex); assert(entered);
        assert(!loop_thread_create(&probe,probe_thread,&f));
        pthread_mutex_lock(&f.mutex); int progressed=wait_for(&f,&f.probe_done);
        f.release=1; pthread_cond_broadcast(&f.changed); pthread_mutex_unlock(&f.mutex);
        assert(!pthread_join(callback,NULL) && !pthread_join(probe,NULL)); assert(progressed);
        if (!mode) assert(strstr(f.response.headers,"loop_session=") && strstr(f.response.headers,"Secure; SameSite=Strict"));
        else assert(!strstr(f.response.headers,"loop_session="));
        loop_response_free(&f.response);
        loop_response r=request(&s,"GET",f.callback,f.cookie,NULL,401); loop_response_free(&r);
        /* Busy admission did not consume the second state. All exit paths free the slot. */
        if (f.store_failure) assert(sqlite3_exec(s.db,"DROP TRIGGER reject_session",NULL,NULL,NULL)==SQLITE_OK);
        f.fail=0; f.denied=0; f.store_failure=0;
        r=request(&s,"GET",f.second_callback,f.second_cookie,NULL,302); loop_response_free(&r);
        cmp_oauth_set_http_for_tests(NULL,NULL); cmp_oauth_cleanup(); loop_close(&s);
        assert(!pthread_cond_destroy(&f.changed) && !pthread_mutex_destroy(&f.mutex));
    }
    puts("Loop OAuth: blocked provider preserves evidence and identity access; callback admission and recovery passed");
    return 0;
}
