#include "loop.h"
#include "voice_auth.h"
#include "model_request_capture.h"
#include <openssl/evp.h>
#include "cmp_oauth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static char cookie_a[96], cookie_b[96];
static loop_response call(loop_service *s, const char *method, const char *path, const char *cookie, const char *body, int status) {
    loop_request q = {.method=method,.path=path,.cookie=cookie,.origin="https://loop.test",.content_type="application/json",.body=(const unsigned char *)body,.body_len=body?strlen(body):0};
    loop_response r;
    assert(loop_handle(s,&q,&r));
    if (r.status != status) fprintf(stderr, "%s %s expected %d got %d: %s\n", method,path,status,r.status,r.body);
    assert(r.status==status); return r;
}
static void expect(loop_service *s, const char *method, const char *path, const char *cookie, const char *body, int status) {
    loop_response r=call(s,method,path,cookie,body,status); loop_response_free(&r);
}
static void model_request_fixture(loop_service *s, const char *cookie, const char *owner,
                                  const char *id, const char *request, int variant) {
    const char *nonce="0123456789abcdef0123456789abcdef";
    const int64_t timestamp=1000;
    char manifest[256],path[160],signature[65]; voice_model_capture capture={0};
    snprintf(manifest,sizeof(manifest),"{\"request_id\":\"%s\",\"recording_consent\":true}",id);
    expect(s,"POST","/api/turns",cookie,manifest,201);
    size_t bytes=strlen(request),offset=0; uint32_t sequence=0;
    assert(voice_model_capture_begin(&capture,bytes)==0);
    while (offset<bytes) {
        size_t length=bytes-offset; if (length>VOICE_MODEL_REQUEST_CHUNK_MAX) length=VOICE_MODEL_REQUEST_CHUNK_MAX;
        assert(voice_model_capture_append(&capture,sequence++,request+offset,length,offset+length==bytes)==0);
        offset+=length;
    }
    assert(voice_model_capture_sign(&capture,s->gateway_secret,strlen(s->gateway_secret),
        variant==2?"foreign-owner":owner,variant==3?"different-turn":id,timestamp,nonce,signature)==0);
    if (variant==1) signature[0]=signature[0]=='0'?'1':'0';
    offset=0; sequence=0;
    snprintf(path,sizeof(path),"/api/turns/%s/events",id);
    while (offset<bytes) {
        size_t length=bytes-offset; if (length>VOICE_MODEL_REQUEST_CHUNK_MAX) length=VOICE_MODEL_REQUEST_CHUNK_MAX;
        char encoded[10925];
        assert(EVP_EncodeBlock((unsigned char *)encoded,(const unsigned char *)request+offset,(int)length)>0);
        if (variant==5 && sequence==0) encoded[0]=encoded[0]=='A'?'B':'A';
        int final=offset+length==bytes && variant!=4;
        char *event=sqlite3_mprintf("{\"seq\":%u,\"name\":\"model_request_chunk\",\"source\":\"gateway_observed\","
            "\"elapsed_ms\":%u,\"payload\":{\"type\":\"model_request_chunk\",\"protocol_version\":\"turnstream.v1alpha1\","
            "\"request_id\":\"%s\",\"sequence\":%u,\"total_bytes\":%llu,\"data\":\"%s\",\"final\":%s,"
            "\"sha256\":\"%s\",\"captured_at\":%lld,\"nonce\":\"%s\",\"signature\":\"%s\"}}",
            sequence,sequence,id,variant==6?0:sequence,(unsigned long long)bytes,encoded,final?"true":"false",
            capture.sha256,(long long)timestamp,nonce,signature);
        assert(event); expect(s,"POST",path,cookie,event,201); sqlite3_free(event);
        offset+=length; sequence++;
    }
    snprintf(path,sizeof(path),"/api/turns/%s/finish",id);
    snprintf(manifest,sizeof(manifest),"{\"status\":\"failed\",\"event_count\":%u,\"error\":\"synthetic capture fixture\"}",sequence);
    expect(s,"POST",path,cookie,manifest,200);
    voice_model_capture_destroy(&capture);
}

static void test_model_request_projection(void) {
    char path[]="/tmp/loop-model-projection-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd);
    loop_service s={.origin="https://loop.test",.operators="capture-owner\nother-owner",
        .gateway_secret="0123456789abcdef0123456789abcdef",.wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
    assert(loop_open(&s,path)); char token[65],owner[65],other[65],a[96],b[96];
    assert(loop_hash("capture-owner",strlen("capture-owner"),owner));
    assert(loop_hash("other-owner",strlen("other-owner"),other));
    assert(loop_session_create(&s,owner,token)); snprintf(a,sizeof(a),"loop_session=%s",token);
    assert(loop_session_create(&s,other,token)); snprintf(b,sizeof(b),"loop_session=%s",token);
    char text[9001]; memset(text,'a',sizeof(text)-1); text[sizeof(text)-1]=0;
    char *request=sqlite3_mprintf("{ \n  \"model\":\"fixture-model\",\"messages\":[{\"role\":\"system\",\"content\":\"Synthetic system prompt café\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],\"max_completion_tokens\":128,\"temperature\":0.2,\"stream\":true,"
        "\"stream_options\":{\"include_usage\":true},\"include_reasoning\":false,\"chat_template_kwargs\":{\"enable_thinking\":false}}",text);
    assert(request);
    model_request_fixture(&s,a,owner,"valid-model",request,0);
    loop_response r=call(&s,"GET","/api/turns/valid-model/model-request",a,NULL,200);
    cmp_json_object root; assert(loop_json_valid(r.body,&root));
    const cmp_json_field *body=cmp_json_object_field(&root,"request");
    assert(body && body->value_len==strlen(request) && !memcmp(body->value,request,body->value_len));
    assert(strstr(r.body,"prepared_request_not_model_attestation"));
    char *encoded=malloc(4*((strlen(request)+2)/3)+1), *decoded=malloc(strlen(request)+4);
    assert(encoded && decoded && cmp_json_object_str(&root,"requestBase64",encoded,4*((strlen(request)+2)/3)+1));
    int decoded_length=EVP_DecodeBlock((unsigned char *)decoded,(unsigned char *)encoded,(int)strlen(encoded));
    size_t padding=(encoded[strlen(encoded)-1]=='=')+(encoded[strlen(encoded)-2]=='=');
    assert(decoded_length>=0 && (size_t)decoded_length-padding==strlen(request));
    assert(!memcmp(decoded,request,strlen(request))); free(encoded); free(decoded);
    loop_response_free(&r);
    expect(&s,"GET","/api/anvil-model-request/valid-model",a,NULL,401);
    expect(&s,"GET","/api/turns/valid-model/model-request",b,NULL,404);
    expect(&s,"POST","/api/turns",a,"{\"request_id\":\"no-model\",\"recording_consent\":true}",201);
    expect(&s,"POST","/api/turns/no-model/finish",a,"{\"status\":\"failed\",\"event_count\":0}",200);
    expect(&s,"GET","/api/turns/no-model/model-request",a,NULL,404);
    for (int variant=1;variant<=6;variant++) {
        char id[32],endpoint[128]; snprintf(id,sizeof(id),"bad-model-%d",variant);
        model_request_fixture(&s,a,owner,id,request,variant);
        snprintf(endpoint,sizeof(endpoint),"/api/turns/%s/model-request",id);
        expect(&s,"GET",endpoint,a,NULL,409);
    }
    model_request_fixture(&s,a,owner,"not-json","not JSON",0);
    expect(&s,"GET","/api/turns/not-json/model-request",a,NULL,409);
    char *temperature=strstr(request,"\"temperature\":0.2"); assert(temperature); temperature[strlen("\"temperature\":0.")]='3';
    model_request_fixture(&s,a,owner,"wrong-policy",request,0);
    expect(&s,"GET","/api/turns/wrong-policy/model-request",a,NULL,409);
    sqlite3_free(request);
    loop_close(&s); assert(loop_open(&s,path));
    expect(&s,"GET","/api/turns/valid-model/model-request",a,NULL,200);
    loop_close(&s); unlink(path);
}
static void test_model_capture_consent(void) {
    char path[]="/tmp/loop-capture-test-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0); close(fd);
    loop_service s={.origin="https://loop.test",.gateway_secret="0123456789abcdef0123456789abcdef",
        .wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
    assert(loop_open(&s,path));
    char token[VOICE_AUTH_IDENTITY_TOKEN_CAP],a[96],b[96],owner[65],other[65];
    s.operators="capture-owner\nother-owner";
    assert(loop_hash("capture-owner",strlen("capture-owner"),owner));
    assert(loop_hash("other-owner",strlen("other-owner"),other));
    assert(loop_session_create(&s,owner,token)); snprintf(a,sizeof(a),"loop_session=%.64s",token);
    assert(loop_session_create(&s,other,token)); snprintf(b,sizeof(b),"loop_session=%.64s",token);
    const char *request="{\"request_id\":\"capture-turn\",\"capture_model_request\":true}";
    expect(&s,"POST","/api/turn-identity",a,request,409);
    expect(&s,"POST","/api/turns",a,"{\"request_id\":\"audio-only\",\"recording_consent\":true}",201);
    expect(&s,"POST","/api/turn-identity",a,"{\"request_id\":\"audio-only\",\"capture_model_request\":true}",409);
    expect(&s,"POST","/api/turns",a,"{\"request_id\":\"capture-turn\",\"recording_consent\":true,\"model_request_consent\":true}",201);
    expect(&s,"POST","/api/turn-identity",b,request,409);
    expect(&s,"POST","/api/turn-identity",a,"{\"request_id\":\"capture-turn\",\"capture_model_request\":\"true\"}",400);
    expect(&s,"POST","/api/turn-identity",a,"{\"request_id\":\"capture-turn\",\"unknown\":true}",400);
    loop_response r=call(&s,"POST","/api/turn-identity",a,request,200);
    assert(cmp_json_str(r.body,"identity_token",token,sizeof(token))); loop_response_free(&r);
    voice_auth_identity_claims claims;
    assert(voice_auth_identity_verify(s.gateway_secret,strlen(s.gateway_secret),token,"capture-turn",(int64_t)time(NULL),&claims)==VOICE_AUTH_OK);
    assert(claims.model_request_capture && !claims.premium && !strcmp(claims.user_id,owner));
    r=call(&s,"POST","/api/turn-identity",a,"{\"request_id\":\"capture-turn\"}",200);
    assert(cmp_json_str(r.body,"identity_token",token,sizeof(token))); loop_response_free(&r);
    assert(voice_auth_identity_verify(s.gateway_secret,strlen(s.gateway_secret),token,"capture-turn",(int64_t)time(NULL),&claims)==VOICE_AUTH_OK);
    assert(!claims.model_request_capture);
    assert(sqlite3_exec(s.db,"UPDATE turns SET manifest_sha='corrupt'",NULL,NULL,NULL)==SQLITE_OK);
    expect(&s,"POST","/api/turn-identity",a,request,409);
    const char *invalid_manifests[]={
        "{\"request_id\":\"capture-turn\",\"recording_consent\":false,\"model_request_consent\":true}",
        "{\"request_id\":\"different-turn\",\"recording_consent\":true,\"model_request_consent\":true}",
        "{\"request_id\":\"capture-turn\",\"recording_consent\":true}",
        "{\"request_id\":\"capture-turn\",\"recording_consent\":true,\"model_request_consent\":false}",
        "{\"request_id\":\"capture-turn\",\"recording_consent\":true,\"model_request_consent\":\"true\"}"};
    for (size_t i=0;i<sizeof(invalid_manifests)/sizeof(invalid_manifests[0]);i++) {
        const char *manifest=invalid_manifests[i]; char digest[65];
        assert(loop_hash(manifest,strlen(manifest),digest));
        sqlite3_stmt *st=NULL;
        assert(sqlite3_prepare_v2(s.db,"UPDATE turns SET manifest=?1,manifest_sha=?2",-1,&st,NULL)==SQLITE_OK);
        sqlite3_bind_text(st,1,manifest,-1,SQLITE_STATIC); sqlite3_bind_text(st,2,digest,-1,SQLITE_STATIC);
        assert(sqlite3_step(st)==SQLITE_DONE); sqlite3_finalize(st);
        expect(&s,"POST","/api/turn-identity",a,request,409);
    }
    expect(&s,"POST","/api/turns",a,"{\"request_id\":\"finished\",\"recording_consent\":true,\"model_request_consent\":true}",201);
    expect(&s,"POST","/api/turns/finished/finish",a,"{\"status\":\"failed\",\"event_count\":0}",200);
    expect(&s,"POST","/api/turn-identity",a,"{\"request_id\":\"finished\",\"capture_model_request\":true}",409);
    loop_close(&s); unlink(path);
}
static void test_runtime_configuration(void) {
    loop_service configured={.origin="https://loop.lab.daviestechlabs.io",.operators="authentik:fixture",
        .gateway_secret="synthetic-test-secret-at-least-32-bytes",
        .wt_url="https://voice-session-gateway.lab.daviestechlabs.io:8443/v1/voice/turns"};
    const char *callback="https://loop.lab.daviestechlabs.io/api/oauth/callback";
    assert(!loop_runtime_configuration_error(&configured,callback));
    assert(loop_runtime_configuration_error(&configured,NULL));
    assert(loop_runtime_configuration_error(&configured,"https://companions-chat.lab.daviestechlabs.io/api/oauth/callback"));
    assert(loop_runtime_configuration_error(&configured,"https://loop.lab.daviestechlabs.io/api/oauth/callback?next=other"));
    const char *bad_origins[]={NULL,"https://","http://loop.test","https://loop.test/","https://user@loop.test","https://loop.test#fragment"};
    for(size_t i=0;i<sizeof(bad_origins)/sizeof(bad_origins[0]);i++) {
        loop_service invalid=configured; invalid.origin=bad_origins[i];
        assert(loop_runtime_configuration_error(&invalid,callback));
    }
    const char *good_routes[]={"https://voice.example/v1/voice/turns","https://voice.example:9443/v1/voice/turns",
        "https://127.0.0.1:8443/v1/voice/turns","https://[::1]:8443/v1/voice/turns"};
    for(size_t i=0;i<sizeof(good_routes)/sizeof(good_routes[0]);i++) {
        loop_service portable=configured; portable.wt_url=good_routes[i]; portable.anvil_origin="https://anvil.example:9443";
        char authority[LOOP_ORIGIN_CAP];
        assert(!loop_runtime_configuration_error(&portable,callback));
        assert(loop_voice_origin(&portable,authority));
        assert(!strcmp(good_routes[i]+strlen(authority),"/v1/voice/turns"));
    }
    const char *bad_routes[]={NULL,"","wss://voice.test/v1/voice/turns","http://voice.test/v1/voice/turns",
        "https://voice.test:0/v1/voice/turns","https://voice.test:65536/v1/voice/turns",
        "https://voice.test:/v1/voice/turns","https://voice.test:8443suffix/v1/voice/turns",
        "https://voice.test;connect-src*/v1/voice/turns","https://voice.test\r\nX:injected/v1/voice/turns",
        "https://voice.test\\other/v1/voice/turns","https://[invalid]:8443/v1/voice/turns",
        "https://loop.lab.daviestechlabs.io:8443/v1/voice/turns?token=secret",
        "https://loop.lab.daviestechlabs.io:8443/v1/voice/turns#fragment",
        "https://user@loop.lab.daviestechlabs.io:8443/v1/voice/turns",
        "https://*.test/v1/voice/turns",
        "https://loop.lab.daviestechlabs.io:8443/other"};
    for(size_t i=0;i<sizeof(bad_routes)/sizeof(bad_routes[0]);i++) {
        loop_service invalid=configured; invalid.wt_url=bad_routes[i];
        assert(loop_runtime_configuration_error(&invalid,callback));
    }
    loop_service invalid=configured; invalid.database_max_bytes=1;
    assert(loop_runtime_configuration_error(&invalid,callback));
    invalid=configured; invalid.anvil_origin="https://anvil.test/path";
    assert(loop_runtime_configuration_error(&invalid,callback));
    invalid.database_max_bytes=LOOP_DB_HARD_MAX+1;
    assert(loop_runtime_configuration_error(&invalid,callback));
    invalid=configured; invalid.gateway_secret=NULL;
    assert(loop_runtime_configuration_error(&invalid,callback)); invalid.gateway_secret="short";
    assert(loop_runtime_configuration_error(&invalid,callback)); invalid=configured; invalid.operators="";
    assert(loop_runtime_configuration_error(&invalid,callback));
}

static void test_oauth_begin(loop_service *s) {
    assert(!setenv("OAUTH_CLIENT_ID","loop-test",1));
    assert(!setenv("OAUTH_CLIENT_SECRET","synthetic-test-secret",1));
    assert(!setenv("OAUTH_REDIRECT_URL","https://loop.test/api/oauth/callback",1));
    assert(!setenv("OAUTH_AUTH_URL","https://identity.test/authorize",1));
    assert(!setenv("OAUTH_TOKEN_URL","https://identity.test/token",1));
    assert(!setenv("OAUTH_USERINFO_URL","https://identity.test/userinfo",1));
    const char *providers[]={"authentik","oidc"};
    s->operators="fixture";
    for(size_t i=0;i<2;i++) {
        assert(!setenv("OAUTH_PROVIDER",providers[i],1));
        assert(cmp_oauth_init()==CMP_OAUTH_OK);
        loop_response r=call(s,"GET","/api/oauth/login","",NULL,302);
        assert(strstr(r.headers,"Location: https://identity.test/authorize?"));
        assert(strstr(r.headers,"code_challenge_method=S256"));
        assert(strstr(r.headers,"HttpOnly; Secure; SameSite=Lax"));
        loop_response_free(&r); cmp_oauth_cleanup();
    }
    s->operators=NULL;
}
static void test_report(loop_service *s) {
    const char *manifest="{\"request_id\":\"report-turn\",\"recording_consent\":true,\"purpose\":\"synthetic_test\"}";
    expect(s,"POST","/api/turns",cookie_a,manifest,201);
    expect(s,"GET","/api/turns/report-turn/report",cookie_a,NULL,409);
    const char *names[]={"connecting","listening","committing","first_text_received","first_audio_received","first_audio_received","first_playback_scheduled"};
    const double times[]={0,10,100,100,110,200,300};
    for(size_t i=0;i<7;i++) {
        char body[256]; snprintf(body,sizeof(body),"{\"seq\":%zu,\"name\":\"%s\",\"source\":\"%s\",\"elapsed_ms\":%.0f,\"payload\":{}}",i,names[i],i==4?"gateway_observed":"browser",times[i]);
        expect(s,"POST","/api/turns/report-turn/events",cookie_a,body,201);
    }
    expect(s,"POST","/api/turns/report-turn/finish",cookie_a,"{\"status\":\"failed\",\"event_count\":7,\"error\":\"synthetic fixture\"}",200);
    loop_response r=call(s,"GET","/api/turns/report-turn/report",cookie_a,NULL,200);
    assert(strstr(r.body,"\"name\":\"release_to_first_text\",\"value\":0.000"));
    assert(strstr(r.body,"\"name\":\"release_to_first_audio_received\",\"value\":100.000"));
    assert(strstr(r.body,"\"name\":\"first_text_to_first_audio_received\",\"value\":100.000"));
    assert(strstr(r.body,"\"name\":\"first_audio_to_playback_scheduled\",\"value\":100.000"));
    assert(strstr(r.body,"\"name\":\"interrupt_to_playback_stopped\",\"value\":null"));
    assert(strstr(r.body,"\"promotionDecision\":null"));
    char first_hash[65]; assert(cmp_json_str(r.body,"receiptSha256",first_hash,sizeof(first_hash))); loop_response_free(&r);
    r=call(s,"GET","/api/turns/report-turn/report",cookie_a,NULL,200);
    char repeat_hash[65]; assert(cmp_json_str(r.body,"receiptSha256",repeat_hash,sizeof(repeat_hash))); assert(!strcmp(first_hash,repeat_hash)); loop_response_free(&r);
    expect(s,"GET","/api/turns/report-turn/report",cookie_b,NULL,404);
    expect(s,"GET","/api/turns/report-turn/receipt",cookie_b,NULL,404);
    expect(s,"GET","/api/turns/report-turn/replay/output.wav",cookie_a,NULL,404);
    expect(s,"GET","/api/turns/report-turn/replay/output.s16le",cookie_a,NULL,404);
    expect(s,"GET","/api/turns/turn-1/replay/input.wav",cookie_b,NULL,404);
    expect(s,"GET","/api/turns/turn-1/replay/input.s16le",cookie_b,NULL,404);
    r=call(s,"GET","/api/turns/report-turn/receipt",cookie_a,NULL,200);
    char receipt_hash[65]; assert(loop_hash(r.body,r.size,receipt_hash));
    assert(!strcmp(first_hash,receipt_hash)); loop_response_free(&r);
    char export_body[512];
    snprintf(export_body,sizeof(export_body),"{\"run_id\":\"4b196128-9dc2-4d65-8bc5-8c839875afe1\",\"sequence\":1,\"source_revision\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"receipt_sha256\":\"%s\"}",first_hash);
    r=call(s,"POST","/api/turns/report-turn/anvil-report",cookie_a,export_body,200);
    assert(strstr(r.body,"\"schemaVersion\":\"anvil-learning-report/v1\""));
    assert(strstr(r.body,"\"status\":\"held\""));
    assert(strstr(r.body,"\"name\":\"loop_browser_release_to_first_text\",\"value\":0.0"));
    assert(!strstr(r.body,"loop_browser_interrupt_to_playback_stopped"));
    assert(strstr(r.body,first_hash)); assert(strstr(r.body,"operator-supplied"));
    char *export_copy=strdup(r.body); assert(export_copy); loop_response_free(&r);
    r=call(s,"POST","/api/turns/report-turn/anvil-report",cookie_a,export_body,200);
    assert(!strcmp(export_copy,r.body)); free(export_copy); loop_response_free(&r);
    expect(s,"POST","/api/turns/report-turn/anvil-report",cookie_b,export_body,404);
    expect(s,"POST","/api/turns/report-turn/anvil-report","",export_body,401);
    expect(s,"POST","/api/turns/report-turn/anvil-report",cookie_a,"{}",400);
    char *bound=strstr(export_body,first_hash); assert(bound); bound[0]=bound[0]=='a'?'b':'a';
    expect(s,"POST","/api/turns/report-turn/anvil-report",cookie_a,export_body,409);
    bound[0]=first_hash[0];
    r=call(s,"POST","/api/turns/report-turn/anvil-bundle",cookie_a,export_body,200);
    assert(!strcmp(r.content_type,"application/x-tar") && r.size%512==0 && r.size>2048);
    assert(!memcmp(r.body+257,"ustar",5));
    char bundle_hash[65]; assert(loop_hash(r.body,r.size,bundle_hash));
    assert(strstr(r.headers,bundle_hash)); loop_response_free(&r);
    r=call(s,"POST","/api/turns/report-turn/anvil-bundle",cookie_a,export_body,200);
    char repeated_bundle[65]; assert(loop_hash(r.body,r.size,repeated_bundle));
    assert(!strcmp(bundle_hash,repeated_bundle)); loop_response_free(&r);
    expect(s,"POST","/api/turns/report-turn/anvil-bundle",cookie_b,export_body,404);
    expect(s,"POST","/api/turns/report-turn/anvil-bundle","",export_body,401);

    const char *damage[]={
        "UPDATE events SET payload=json_set(payload,'$.elapsed_ms',999) WHERE turn_id='report-turn' AND seq=0",
        "DELETE FROM events WHERE turn_id='report-turn' AND seq=2",
        "UPDATE turns SET manifest='{}' WHERE id='report-turn'",
        "UPDATE turns SET status='completed' WHERE id='report-turn'"
    };
    for(size_t i=0;i<sizeof(damage)/sizeof(damage[0]);i++) {
        assert(sqlite3_exec(s->db,"BEGIN",NULL,NULL,NULL)==SQLITE_OK);
        assert(sqlite3_exec(s->db,damage[i],NULL,NULL,NULL)==SQLITE_OK);
        assert(sqlite3_changes(s->db)==1);
        expect(s,"GET","/api/turns/report-turn/report",cookie_a,NULL,409);
        expect(s,"GET","/api/turns/report-turn/receipt",cookie_a,NULL,409);
        expect(s,"POST","/api/turns/report-turn/anvil-report",cookie_a,export_body,409);
        expect(s,"POST","/api/turns/report-turn/anvil-bundle",cookie_a,export_body,409);
        assert(sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    }
    /* The earlier fixture owns real saved PCM bytes; corruption must be visible. */
    assert(sqlite3_exec(s->db,"BEGIN; UPDATE audio SET pcm=x'99999999' WHERE turn_id='turn-1'",NULL,NULL,NULL)==SQLITE_OK);
    expect(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,409);
    expect(s,"GET","/api/turns/turn-1/replay/input.wav",cookie_a,NULL,409);
    expect(s,"GET","/api/turns/turn-1/replay/input.s16le",cookie_a,NULL,409);
    assert(sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    expect(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,200);
    r=call(s,"GET","/api/turns/turn-1/replay/input.wav",cookie_a,NULL,200);
    assert(r.size==48 && !strcmp(r.content_type,"audio/wav"));
    assert(!memcmp(r.body,"RIFF",4) && !memcmp(r.body+8,"WAVEfmt ",8));
    const unsigned char expected_header[]={0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',4,0,0,0};
    assert(!memcmp(r.body+24,expected_header,sizeof(expected_header)));
    const unsigned char expected_pcm[]={0,1,2,3}; assert(!memcmp(r.body+44,expected_pcm,4));
    assert(strstr(r.headers,"X-Loop-Receipt-SHA256:")); loop_response_free(&r);
    r=call(s,"GET","/api/turns/turn-1/replay/input.s16le",cookie_a,NULL,200);
    assert(r.size==sizeof(expected_pcm) && !memcmp(r.body,expected_pcm,sizeof(expected_pcm)));
    assert(!strcmp(r.content_type,"application/octet-stream"));
    assert(strstr(r.headers,"X-PCM-Sample-Rate: 16000\r\n"));
    loop_response_free(&r);
}


static void test_ambiguous_measurements(loop_service *s) {
    const char *metrics[]={"release_to_first_text","first_text_to_first_audio_received","first_audio_to_playback_scheduled"};
    const char *starts[]={"committing","first_text_received","first_audio_received"};
    const char *ends[]={"first_text_received","first_audio_received","first_playback_scheduled"};
    for (int pair=0;pair<3;pair++) for (int mode=0;mode<5;mode++) {
        char id[48],path[128],body[512];
        snprintf(id,sizeof(id),"ambiguous-measurement-%d-%d",pair,mode);
        snprintf(body,sizeof(body),"{\"request_id\":\"%s\",\"recording_consent\":true}",id);
        expect(s,"POST","/api/turns",cookie_a,body,201);
        snprintf(path,sizeof(path),"/api/turns/%s/events",id);
        /* Equal timestamps cannot repair reversed order or duplicate observations. */
        const char *names[]={mode==0?ends[pair]:starts[pair],mode==0?starts[pair]:ends[pair],
            mode==1?starts[pair]:ends[pair]};
        int count=mode==3?1:(mode==0 || mode==4)?2:3;
        for(int i=0;i<count;i++) {
            snprintf(body,sizeof(body),"{\"seq\":%d,\"name\":\"%s\",\"source\":\"browser\",\"elapsed_ms\":10,\"payload\":{}}",i,names[i]);
            expect(s,"POST",path,cookie_a,body,201);
        }
        snprintf(path,sizeof(path),"/api/turns/%s/finish",id);
        snprintf(body,sizeof(body),"{\"status\":\"failed\",\"event_count\":%d,\"error\":\"synthetic\"}",count);
        expect(s,"POST",path,cookie_a,body,200);
        snprintf(path,sizeof(path),"/api/turns/%s/report",id);
        loop_response r=call(s,"GET",path,cookie_a,NULL,200);
        char expected[160];
        snprintf(expected,sizeof(expected),"\"name\":\"%s\",\"value\":%s",metrics[pair],mode==4?"0.000":"null");
        const char *metric=strstr(r.body,expected);
        assert(metric);
        const char *reason=strstr(metric,mode==0?"invalid_milestone_order":mode==3?"missing_milestone":mode==4?"\"unavailableReason\":null":"duplicate_milestone");
        assert(reason && reason<strchr(metric,'}'));
        loop_response_free(&r);
    }
}

static void test_storage_full(loop_service *s) {
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"full-store\",\"recording_consent\":true}",201);
    loop_response before=call(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,200);
    char original[65]; assert(cmp_json_str(before.body,"receiptSha256",original,sizeof(original))); loop_response_free(&before);
    assert(sqlite3_exec(s->db,"CREATE TABLE full_store_fault(payload BLOB);"
        "CREATE TRIGGER full_link BEFORE INSERT ON anvil_links WHEN NEW.id='full-link' "
        "BEGIN INSERT INTO full_store_fault VALUES(zeroblob(1048576)); END;"
        "CREATE TRIGGER full_rerun BEFORE INSERT ON turns WHEN NEW.id='full-rerun' "
        "BEGIN INSERT INTO full_store_fault VALUES(zeroblob(1048576)); END;",NULL,NULL,NULL)==SQLITE_OK);
    sqlite3_stmt *st=NULL;
    assert(sqlite3_prepare_v2(s->db,"PRAGMA page_count",-1,&st,NULL)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW);
    int pages=sqlite3_column_int(st,0); sqlite3_finalize(st);
    char limit[80]; snprintf(limit,sizeof(limit),"PRAGMA max_page_count=%d",pages);
    assert(sqlite3_exec(s->db,limit,NULL,NULL,NULL)==SQLITE_OK);
    size_t size=1024u*1024u; unsigned char *pcm=calloc(1,size); assert(pcm); pcm[0]=42;
    loop_request q={.method="POST",.path="/api/turns/full-store/audio/input/16000",.cookie=cookie_a,.origin=s->origin,
        .content_type="application/octet-stream",.body=pcm,.body_len=size};
    loop_response r;
    assert(loop_handle(s,&q,&r));
    assert(r.status==507 && strstr(r.body,"evidence_store_full")); loop_response_free(&r);
    assert(sqlite3_get_autocommit(s->db));
    r=call(s,"GET","/api/turns/full-store",cookie_a,NULL,200);
    assert(strstr(r.body,"\"status\":\"recording\"") && strstr(r.body,"\"audio\":[]")); loop_response_free(&r);
    r=call(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,200);
    char unchanged[65]; assert(cmp_json_str(r.body,"receiptSha256",unchanged,sizeof(unchanged)) && !strcmp(original,unchanged)); loop_response_free(&r);
    char link[512];
    snprintf(link,sizeof(link),"{\"id\":\"full-link\",\"kind\":\"learning_report\",\"reference\":\"%s\",\"receipt_sha256\":\"%s\"}",original,original);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,link,507);
    assert(sqlite3_get_autocommit(s->db));
    const char *rerun="{\"request_id\":\"full-rerun\",\"mode\":\"full_system_active_model\"}";
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,rerun,507);
    assert(sqlite3_get_autocommit(s->db));
    expect(s,"GET","/api/turns/full-rerun",cookie_a,NULL,404);
    assert(loop_storage_limit(s));
    assert(sqlite3_exec(s->db,"DROP TRIGGER full_link; DROP TRIGGER full_rerun; DROP TABLE full_store_fault",NULL,NULL,NULL)==SQLITE_OK);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,link,201);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,link,200);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,rerun,201);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,rerun,200);
    assert(loop_handle(s,&q,&r) && r.status==201); loop_response_free(&r);
    assert(loop_handle(s,&q,&r) && r.status==200); loop_response_free(&r);
    char hash[65]; assert(loop_hash(pcm,size,hash)); free(pcm);
    expect(s,"POST","/api/turns/full-store/finish",cookie_a,"{\"status\":\"failed\",\"event_count\":0,\"error\":\"synthetic storage fault\"}",200);
    r=call(s,"GET","/api/turns/full-store/report",cookie_a,NULL,200); assert(strstr(r.body,hash)); loop_response_free(&r);
}

static void test_input_delivery(loop_service *s) {
    const struct { const char *reason, *ack_payload; int mode; } cases[]={
        {NULL,"{\"audio_datagrams\":1,\"audio_bytes\":640,\"lost_datagrams\":0}",0},
        {"delivery_counts_do_not_match_recording","{\"audio_datagrams\":1,\"audio_bytes\":638,\"lost_datagrams\":0}",0},
        {"gateway_reported_lost_datagrams","{\"audio_datagrams\":1,\"audio_bytes\":640,\"lost_datagrams\":1}",0},
        {"invalid_delivery_counters","{\"audio_datagrams\":1,\"audio_bytes\":640.5,\"lost_datagrams\":0}",0},
        {"invalid_delivery_counters","{\"audio_datagrams\":1,\"audio_bytes\":640,\"lost_datagrams\":-1}",0},
        {"input_end_not_observed",NULL,1},
        {"input_commit_not_observed",NULL,2},
        {"input_commit_not_observed",NULL,3},
        {"duplicate_delivery_observation",NULL,4},
        {"invalid_delivery_observation_order",NULL,5},
        {"input_audio_not_recorded",NULL,6},
        {"unsupported_input_framing",NULL,7},
        {"input_end_not_observed",NULL,8},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char id[48],path[128],body[768];
        snprintf(id,sizeof(id),"delivery-%zu",i);
        snprintf(body,sizeof(body),"{\"request_id\":\"%s\",\"recording_consent\":true,\"purpose\":\"synthetic_test\"}",id);
        expect(s,"POST","/api/turns",cookie_a,body,201);
        unsigned char pcm[640]={0};
        if(cases[i].mode!=6) {
            snprintf(path,sizeof(path),"/api/turns/%s/audio/input/16000",id);
            loop_request q={.method="POST",.path=path,.cookie=cookie_a,.origin=s->origin,
                .content_type="application/octet-stream",.body=pcm,.body_len=cases[i].mode==7?638:640};
            loop_response audio; assert(loop_handle(s,&q,&audio)); assert(audio.status==201); loop_response_free(&audio);
        }
        snprintf(path,sizeof(path),"/api/turns/%s/events",id);
        int seq=0;
        for(int j=0;j<(cases[i].mode==4?3:2);j++) {
            int gateway=cases[i].mode==5 ? j==0 : j!=0;
            if((!gateway && cases[i].mode==1) || (gateway && cases[i].mode==2)) continue;
            const char *source=gateway?"gateway_observed":"browser";
            if(cases[i].mode==3 && gateway) source="browser";
            if(cases[i].mode==8 && !gateway) source="gateway_observed";
            const char *payload=gateway ? (cases[i].ack_payload ? cases[i].ack_payload : cases[0].ack_payload) : "{\"packets\":1,\"bytes\":640}";
            snprintf(body,sizeof(body),"{\"seq\":%d,\"name\":\"%s\",\"source\":\"%s\",\"elapsed_ms\":%d,\"payload\":%s}",
                seq,gateway?"input_committed":"input_end_sent",source,seq,payload);
            expect(s,"POST",path,cookie_a,body,201); seq++;
        }
        snprintf(path,sizeof(path),"/api/turns/%s/finish",id);
        snprintf(body,sizeof(body),"{\"status\":\"failed\",\"event_count\":%d}",seq);
        expect(s,"POST",path,cookie_a,body,200);
        snprintf(path,sizeof(path),"/api/turns/%s/report",id);
        loop_response r=call(s,"GET",path,cookie_a,NULL,200);
        cmp_json_object report,delivery; char status[64],reason[80],origin[32]; int attested=1;
        assert(cmp_json_object_parse(r.body,&report) && cmp_json_object_object(&report,"inputDelivery",&delivery));
        assert(cmp_json_object_str(&delivery,"status",status,sizeof(status)));
        assert(!strcmp(status,cases[i].reason?"not_established":"matched_browser_observations"));
        if(cases[i].reason) { assert(cmp_json_object_str(&delivery,"unavailableReason",reason,sizeof(reason))); assert(!strcmp(reason,cases[i].reason)); }
        assert(cmp_json_object_str(&delivery,"evidenceOrigin",origin,sizeof(origin)) && !strcmp(origin,"browser_observed"));
        assert(cmp_json_object_bool(&delivery,"serverAttested",&attested) && !attested);
        if(!cases[i].reason) {
            int64_t recorded,sent,received,lost;
            assert(cmp_json_object_i64(&delivery,"recordedBytes",&recorded) && recorded==640);
            assert(cmp_json_object_i64(&delivery,"browserSentBytes",&sent) && sent==640);
            assert(cmp_json_object_i64(&delivery,"gatewayReportedBytes",&received) && received==640);
            assert(cmp_json_object_i64(&delivery,"gatewayReportedLostDatagrams",&lost) && lost==0);
        }
        loop_response_free(&r);
    }
}

static void test_response_audio(loop_service *s) {
    const struct { const char *reason, *payload; int mode; } cases[]={
        {NULL,"{\"bytes\":960,\"packets\":1,\"sample_rate\":24000}",0},
        {"response_counts_do_not_match_recording","{\"bytes\":958,\"packets\":1,\"sample_rate\":24000}",0},
        {"response_counts_do_not_match_recording","{\"bytes\":960,\"packets\":1,\"sample_rate\":48000}",0},
        {"invalid_response_counters","{\"bytes\":960.5,\"packets\":1,\"sample_rate\":24000}",0},
        {"invalid_response_counters","{\"bytes\":960,\"packets\":0,\"sample_rate\":24000}",0},
        {"invalid_response_counters","{\"bytes\":960,\"packets\":481,\"sample_rate\":24000}",0},
        {"invalid_response_counters","{\"bytes\":16386,\"packets\":1,\"sample_rate\":24000}",0},
        {"invalid_response_counters","{\"bytes\":960,\"packets\":1,\"sample_rate\":8000}",0},
        {"output_audio_not_recorded",NULL,1},
        {"output_completion_not_observed",NULL,2},
        {"gateway_completion_not_observed",NULL,3},
        {"duplicate_response_observation",NULL,4},
        {"duplicate_response_observation",NULL,5},
        {"invalid_response_observation_order",NULL,6},
        {"output_completion_not_observed",NULL,7},
        {"gateway_completion_not_observed",NULL,8},
        {NULL,NULL,9}, /* Failed playback can still have a complete captured response. */
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char id[48],path[128],body[768];
        snprintf(id,sizeof(id),"response-%zu",i);
        snprintf(body,sizeof(body),"{\"request_id\":\"%s\",\"recording_consent\":true,\"purpose\":\"synthetic_test\"}",id);
        expect(s,"POST","/api/turns",cookie_a,body,201);
        if(cases[i].mode!=1) {
            unsigned char pcm[960]={0};
            snprintf(path,sizeof(path),"/api/turns/%s/audio/output/24000",id);
            loop_request q={.method="POST",.path=path,.cookie=cookie_a,.origin=s->origin,
                .content_type="application/octet-stream",.body=pcm,.body_len=sizeof(pcm)};
            loop_response audio; assert(loop_handle(s,&q,&audio)); assert(audio.status==201); loop_response_free(&audio);
        }
        snprintf(path,sizeof(path),"/api/turns/%s/events",id);
        int seq=0, mode=cases[i].mode;
        for(int j=0;j<((mode==4 || mode==5)?3:2);j++) {
            int gateway=mode==6 ? j==1 : (j==0 || (mode==5 && j==2));
            if((!gateway && mode==2) || (gateway && mode==3)) continue;
            const char *source=gateway?"gateway_observed":"browser";
            if((gateway && mode==8) || (!gateway && mode==7)) source=gateway?"browser":"gateway_observed";
            const char *payload=gateway?"{}":(cases[i].payload?cases[i].payload:cases[0].payload);
            snprintf(body,sizeof(body),"{\"seq\":%d,\"name\":\"%s\",\"source\":\"%s\",\"elapsed_ms\":0,\"payload\":%s}",
                seq,gateway?"completed":"output_received_complete",source,payload);
            expect(s,"POST",path,cookie_a,body,201); seq++;
        }
        snprintf(path,sizeof(path),"/api/turns/%s/finish",id);
        snprintf(body,sizeof(body),"{\"status\":\"%s\",\"event_count\":%d}",mode==9?"failed":"completed",seq);
        expect(s,"POST",path,cookie_a,body,200);
        snprintf(path,sizeof(path),"/api/turns/%s/report",id);
        loop_response r=call(s,"GET",path,cookie_a,NULL,200);
        cmp_json_object report,response; char status[64],reason[80]; int attested=1, acoustic=1;
        assert(cmp_json_object_parse(r.body,&report) && cmp_json_object_object(&report,"responseAudio",&response));
        assert(cmp_json_object_str(&response,"status",status,sizeof(status)));
        assert(!strcmp(status,cases[i].reason?"not_established":"matched_browser_observations"));
        if(cases[i].reason) { assert(cmp_json_object_str(&response,"unavailableReason",reason,sizeof(reason))); assert(!strcmp(reason,cases[i].reason)); }
        assert(cmp_json_object_bool(&response,"serverAttested",&attested) && !attested);
        assert(cmp_json_object_bool(&response,"acousticOutputVerified",&acoustic) && !acoustic);
        if(mode!=1) {
            double duration=0; int64_t recorded=0;
            assert(cmp_json_object_i64(&response,"recordedBytes",&recorded) && recorded==960);
            assert(cmp_json_field_double(cmp_json_object_field(&response,"pcmDurationMs"),&duration) && duration==20.0);
        }
        if(!cases[i].reason) {
            int64_t packets, bytes, rate;
            assert(cmp_json_object_i64(&response,"browserReceivedBytes",&bytes) && bytes==960);
            assert(cmp_json_object_i64(&response,"browserReceivedPackets",&packets) && packets==1);
            assert(cmp_json_object_i64(&response,"browserReceivedSampleRate",&rate) && rate==24000);
        }
        if(mode==4 || (cases[i].reason && !strcmp(cases[i].reason,"invalid_response_counters"))) {
            const cmp_json_field *field=cmp_json_object_field(&response,"browserReceivedBytes");
            assert(field && field->value_len==4 && !memcmp(field->value,"null",4));
        }
        loop_response_free(&r);
        expect(s,"GET",path,cookie_b,NULL,404);
    }
}

static void test_anvil_links(loop_service *s) {
    loop_response r=call(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,200);
    char receipt[65]; assert(cmp_json_str(r.body,"receiptSha256",receipt,sizeof(receipt))); loop_response_free(&r);
    char body[512];
    snprintf(body,sizeof(body),"{\"id\":\"link-1\",\"kind\":\"experiment\",\"reference\":\"a1234567-1234-4123-8123-123456789abc\",\"receipt_sha256\":\"%s\"}",receipt);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_b,body,404);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,body,201);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,body,200);
    char *reference=strstr(body,"a1234567"); assert(reference); reference[0]='b';
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,body,409);
    snprintf(body,sizeof(body),"{\"id\":\"link-2\",\"kind\":\"learning_report\",\"reference\":\"%s\",\"receipt_sha256\":\"%064d\"}",receipt,0);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,body,409);
    snprintf(body,sizeof(body),"{\"id\":\"link-2\",\"kind\":\"promotion\",\"reference\":\"%s\",\"receipt_sha256\":\"%s\"}",receipt,receipt);
    expect(s,"POST","/api/turns/turn-1/anvil-links",cookie_a,body,400);
    r=call(s,"GET","/api/turns/turn-1/anvil-links",cookie_a,NULL,200);
    assert(strstr(r.body,"operator_annotation") && strstr(r.body,"not_verified") && strstr(r.body,"a1234567")); loop_response_free(&r);
    r=call(s,"GET","/api/turns/turn-1/report",cookie_a,NULL,200);
    char after[65]; assert(cmp_json_str(r.body,"receiptSha256",after,sizeof(after)) && !strcmp(receipt,after)); loop_response_free(&r);
    assert(sqlite3_exec(s->db,"BEGIN; UPDATE anvil_links SET sha='broken'",NULL,NULL,NULL)==SQLITE_OK);
    expect(s,"GET","/api/turns/turn-1/anvil-links",cookie_a,NULL,409);
    assert(sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
}

static void test_rerun(loop_service *s) {
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"rerun-source\",\"recording_consent\":true,\"purpose\":\"synthetic_test\"}",201);
    unsigned char pcm[1280]={1,2,3,4};
    loop_request q={.method="POST",.path="/api/turns/rerun-source/audio/input/16000",.cookie=cookie_a,.origin=s->origin,.content_type="application/octet-stream",.body=pcm,.body_len=sizeof(pcm)};
    loop_response r; assert(loop_handle(s,&q,&r) && r.status==201); loop_response_free(&r);
    const char *request="{\"request_id\":\"rerun-result\",\"mode\":\"full_system_active_model\"}";
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,request,409);
    expect(s,"POST","/api/turns/rerun-source/finish",cookie_a,"{\"status\":\"failed\",\"event_count\":0}",200);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_b,request,404);
    r=call(s,"POST","/api/turns/rerun-source/reruns",cookie_a,request,201);
    assert(strstr(r.body,"pending_browser_transport") && strstr(r.body,"saved_input_rerun") && strstr(r.body,"live_gateway_policy"));
    char pcm_hash[65]; assert(loop_hash(pcm,sizeof(pcm),pcm_hash)); assert(strstr(r.body,pcm_hash)); loop_response_free(&r);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,request,200);
    const char *capture_request="{\"request_id\":\"rerun-capture\",\"mode\":\"full_system_active_model\",\"model_request_consent\":true}";
    r=call(s,"POST","/api/turns/rerun-source/reruns",cookie_a,capture_request,201);
    assert(strstr(r.body,"\"model_request_consent\":true")); loop_response_free(&r);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,capture_request,200);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_b,capture_request,404);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,"{\"request_id\":\"rerun-capture\",\"mode\":\"full_system_active_model\",\"model_request_consent\":false}",409);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,"{\"request_id\":\"rerun-invalid\",\"mode\":\"full_system_active_model\",\"model_request_consent\":\"true\"}",400);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,"{\"request_id\":\"rerun-source\",\"mode\":\"full_system_active_model\"}",400);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,"{\"request_id\":\"controlled\",\"mode\":\"controlled\"}",400);
    expect(s,"POST","/api/turns/turn-1/reruns",cookie_a,request,409);
    expect(s,"POST","/api/turns/report-turn/reruns",cookie_a,request,409);
    r=call(s,"GET","/api/turns/rerun-result",cookie_a,NULL,200);
    assert(strstr(r.body,"source_receipt_sha256") && strstr(r.body,"\"model_revision\":null") && strstr(r.body,"\"status\":\"recording\"")); loop_response_free(&r);
    expect(s,"GET","/api/turns/rerun-result",cookie_b,NULL,404);
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"forged-rerun\",\"recording_consent\":true,\"rerun\":{}}",400);
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"forged-rerun\",\"recording_consent\":true,\"purpose\":\"saved_input_rerun\"}",400);
    expect(s,"POST","/api/turns/rerun-result/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":0}",409);
    q.path="/api/turns/rerun-result/audio/input/16000";
    pcm[0]^=1;
    assert(loop_handle(s,&q,&r) && r.status==409 && strstr(r.body,"rerun_input_mismatch")); loop_response_free(&r);
    pcm[0]^=1;
    q.path="/api/turns/rerun-result/audio/input/24000";
    assert(loop_handle(s,&q,&r) && r.status==409); loop_response_free(&r);
    q.path="/api/turns/rerun-result/audio/input/16000"; q.body_len=638;
    assert(loop_handle(s,&q,&r) && r.status==409); loop_response_free(&r);
    q.body_len=640;
    assert(sqlite3_exec(s->db,"BEGIN; UPDATE audio SET pcm=zeroblob(1280) WHERE turn_id='rerun-source'",NULL,NULL,NULL)==SQLITE_OK);
    assert(loop_handle(s,&q,&r) && r.status==409 && strstr(r.body,"rerun_source_integrity_failed")); loop_response_free(&r);
    assert(sqlite3_exec(s->db,"ROLLBACK",NULL,NULL,NULL)==SQLITE_OK);
    assert(loop_handle(s,&q,&r) && r.status==201); loop_response_free(&r);
    assert(loop_handle(s,&q,&r) && r.status==200); loop_response_free(&r);
    expect(s,"POST","/api/turns/rerun-result/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":0}",409);
    expect(s,"POST","/api/turns/rerun-result/finish",cookie_a,"{\"status\":\"failed\",\"event_count\":0}",200);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,request,409);
    expect(s,"POST","/api/turns/rerun-source/reruns",cookie_a,"{\"request_id\":\"rerun-full\",\"mode\":\"full_system_active_model\"}",201);
    q.path="/api/turns/rerun-full/audio/input/16000"; q.body_len=sizeof(pcm);
    q.cookie=cookie_b; assert(loop_handle(s,&q,&r) && r.status==404); loop_response_free(&r); q.cookie=cookie_a;
    assert(loop_handle(s,&q,&r) && r.status==201); loop_response_free(&r);
    expect(s,"POST","/api/turns/rerun-full/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":0}",200);
    expect(s,"POST","/api/turns/rerun-full/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":0}",200);
    r=call(s,"GET","/api/turns/rerun-full/replay/input.s16le",cookie_a,NULL,200);
    assert(r.size==sizeof(pcm) && !memcmp(r.body,pcm,sizeof(pcm))); loop_response_free(&r);
    /* Chained reruns preserve the immediate source, without recursive execution. */
    expect(s,"POST","/api/turns/rerun-full/reruns",cookie_a,"{\"request_id\":\"rerun-chain\",\"mode\":\"full_system_active_model\"}",201);
    q.path="/api/turns/rerun-chain/audio/input/16000";
    assert(loop_handle(s,&q,&r) && r.status==201); loop_response_free(&r);
    expect(s,"POST","/api/turns/rerun-chain/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":0}",200);
}

static void test_backup_readonly_source(loop_service *s, const char *source) {
    /* Permission checks need an unprivileged process. This is not a mount or NFS test. */
    if (!geteuid()) { puts("SKIP read-only backup permissions: process is root"); return; }
    char directory[]="/tmp/loop-backup-readonly-XXXXXX";
    int directory_fd=mkstemp(directory); assert(directory_fd>=0); close(directory_fd);
    assert(!unlink(directory) && !mkdir(directory,0700));
    char input_dir[256], input[300], wal[320], shm[320], output[300];
    snprintf(input_dir,sizeof(input_dir),"%s/source",directory);
    assert(!mkdir(input_dir,0700));
    snprintf(input,sizeof(input),"%s/live.sqlite",input_dir);
    snprintf(wal,sizeof(wal),"%s-wal",input);
    snprintf(shm,sizeof(shm),"%s-shm",input);
    snprintf(output,sizeof(output),"%s/snapshot.sqlite",directory);
    assert(loop_backup(source,input));
    loop_service writer={.origin=s->origin,.operators=s->operators};
    assert(loop_open(&writer,input));
    assert(sqlite3_exec(writer.db,"PRAGMA wal_autocheckpoint=0",NULL,NULL,NULL)==SQLITE_OK);
    expect(&writer,"POST","/api/turns",cookie_a,
        "{\"request_id\":\"readonly-wal\",\"recording_consent\":true}",201);
    struct stat info; assert(!stat(wal,&info) && info.st_size>0);
    assert(!chmod(input,0400) && !chmod(wal,0400) && !chmod(shm,0400) && !chmod(input_dir,0500));
    assert(access(input,W_OK) && access(wal,W_OK) && access(shm,W_OK) && access(input_dir,W_OK));
    assert(loop_backup(input,output));
    assert(!stat(output,&info) && (info.st_mode & 0777)==0600);
    loop_service restored={.origin=s->origin,.operators=s->operators};
    assert(loop_open(&restored,output));
    expect(&restored,"GET","/api/turns/readonly-wal",cookie_a,NULL,200);
    expect(&restored,"GET","/api/turns/readonly-wal",cookie_b,NULL,404);
    loop_response before=call(s,"GET","/api/turns/turn-1/receipt",cookie_a,NULL,200);
    loop_response after=call(&restored,"GET","/api/turns/turn-1/receipt",cookie_a,NULL,200);
    assert(before.size==after.size && !memcmp(before.body,after.body,before.size));
    loop_response_free(&before); loop_response_free(&after);
    loop_close(&restored);
    assert(!chmod(input_dir,0700) && !chmod(input,0600) && !chmod(wal,0600) && !chmod(shm,0600));
    loop_close(&writer);
    unlink(input); unlink(wal); unlink(shm); unlink(output);
    snprintf(wal,sizeof(wal),"%s-wal",output); unlink(wal);
    snprintf(shm,sizeof(shm),"%s-shm",output); unlink(shm);
    assert(!rmdir(input_dir) && !rmdir(directory));
}

static void test_backup(loop_service *s, const char *source) {
    char directory[]="/tmp/loop-backup-XXXXXX";
    int directory_fd=mkstemp(directory); assert(directory_fd>=0); close(directory_fd);
    assert(!unlink(directory) && !mkdir(directory,0700));
    char target[256], missing[256], alias[256], wal[256];
    snprintf(target,sizeof(target),"%s/snapshot.sqlite",directory);
    snprintf(missing,sizeof(missing),"%s/missing.sqlite",directory);
    snprintf(alias,sizeof(alias),"%s/alias.sqlite",directory);
    snprintf(wal,sizeof(wal),"%s-wal",source);
    assert(sqlite3_exec(s->db,"PRAGMA wal_autocheckpoint=0",NULL,NULL,NULL)==SQLITE_OK);
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"wal-only\",\"recording_consent\":true}",201);
    struct stat info; assert(!stat(wal,&info) && info.st_size>0);
    loop_response before=call(s,"GET","/api/turns/turn-1/receipt",cookie_a,NULL,200);
    assert(!loop_backup(missing,target)); assert(access(missing,F_OK) && access(target,F_OK));
    assert(sqlite3_exec(s->db,"PRAGMA user_version=99",NULL,NULL,NULL)==SQLITE_OK);
    assert(!loop_backup(source,target)); assert(access(target,F_OK));
    assert(sqlite3_exec(s->db,"PRAGMA user_version=2",NULL,NULL,NULL)==SQLITE_OK);
    assert(!loop_backup(source,source));
    assert(!symlink(source,alias)); assert(!loop_backup(source,alias));
    /* A failed publication sync cannot report success or overwrite its uncertain result. */
    loop_backup_test_directory_sync_error=EIO;
    assert(!loop_backup(source,target));
    loop_backup_test_directory_sync_error=0;
    assert(!stat(target,&info) && (info.st_mode & 0777)==0600);
    assert(!loop_backup(source,target));
    assert(!unlink(target));
    int cwd=open(".",O_RDONLY); assert(cwd>=0 && !chdir(directory));
    assert(loop_backup(source,"relative.sqlite"));
    assert(!unlink("relative.sqlite") && !fchdir(cwd) && !close(cwd));
    assert(loop_backup(source,target));
    assert(!stat(target,&info) && (info.st_mode & 0777)==0600);
    char exported_sidecar[272];
    snprintf(exported_sidecar,sizeof(exported_sidecar),"%s-wal",target); assert(access(exported_sidecar,F_OK));
    snprintf(exported_sidecar,sizeof(exported_sidecar),"%s-shm",target); assert(access(exported_sidecar,F_OK));
    assert(!loop_backup(source,target));
    /* New source writes cannot alter the published snapshot. */
    expect(s,"POST","/api/turns",cookie_a,"{\"request_id\":\"after-backup\",\"recording_consent\":true}",201);
    loop_service restored={.origin=s->origin,.operators=s->operators};
    assert(loop_open(&restored,target));
    expect(&restored,"GET","/api/turns/wal-only",cookie_a,NULL,200);
    expect(&restored,"GET","/api/turns/after-backup",cookie_a,NULL,404);
    loop_response after=call(&restored,"GET","/api/turns/turn-1/receipt",cookie_a,NULL,200);
    assert(before.size==after.size && !memcmp(before.body,after.body,before.size));
    loop_response_free(&before); loop_response_free(&after);
    after=call(&restored,"GET","/api/turns/turn-1/replay/input.s16le",cookie_a,NULL,200);
    const unsigned char pcm[]={0,1,2,3}; assert(after.size==4 && !memcmp(after.body,pcm,4)); loop_response_free(&after);
    after=call(&restored,"GET","/api/captures",cookie_a,NULL,200);
    assert(strstr(after.body,"capture-1")); loop_response_free(&after);
    after=call(&restored,"GET","/api/turns/turn-1/anvil-links",cookie_a,NULL,200);
    assert(strstr(after.body,"link-1")); loop_response_free(&after);
    expect(&restored,"GET","/api/turns/turn-1",cookie_b,NULL,404);
    restored.operators="authentik:operator-b";
    expect(&restored,"GET","/api/turns/turn-1",cookie_a,NULL,401);
    loop_close(&restored);
    unlink(alias); unlink(target);
    char sidecar[272];
    snprintf(sidecar,sizeof(sidecar),"%s-wal",target); unlink(sidecar);
    snprintf(sidecar,sizeof(sidecar),"%s-shm",target); unlink(sidecar);
    assert(!rmdir(directory));
}


static void test_recording_verifier(void) {
    char directory[]="/tmp/loop-verify-XXXXXX";
    int directory_fd=mkstemp(directory); assert(directory_fd>=0); close(directory_fd);
    assert(!unlink(directory) && !mkdir(directory,0700));
    char source[256], target[256];
    snprintf(source,sizeof(source),"%s/source.sqlite",directory);
    snprintf(target,sizeof(target),"%s/candidate.sqlite",directory);
    uint64_t verified=99,unfinished=99;
    assert(!loop_verify_recordings(source,&verified,&unfinished));
    assert(!verified && !unfinished && access(source,F_OK));
    loop_service fixture={0}; assert(loop_open(&fixture,source));
    assert(loop_verify_recordings(source,&verified,&unfinished) && !verified && !unfinished);
    const char *manifest="{\"request_id\":\"saved\",\"recording_consent\":true}";
    const char *final="{\"status\":\"completed\",\"event_count\":1}";
    const char *event="{\"seq\":0,\"source\":\"browser\",\"name\":\"connecting\",\"elapsed_ms\":0}";
    const unsigned char pcm[]={0,1,2,3};
    char mh[65],fh[65],eh[65],ah[65];
    assert(loop_hash(manifest,strlen(manifest),mh) && loop_hash(final,strlen(final),fh));
    assert(loop_hash(event,strlen(event),eh) && loop_hash(pcm,sizeof(pcm),ah));
    for(int i=0;i<2;i++) {
        char *sql=sqlite3_mprintf(
            "INSERT INTO turns(owner,id,manifest,manifest_sha,final,final_sha,status) "
            "VALUES('owner-%d','saved',%Q,%Q,%Q,%Q,'completed');"
            "INSERT INTO events(owner,turn_id,seq,payload,sha) VALUES('owner-%d','saved',0,%Q,%Q);"
            "INSERT INTO audio(owner,turn_id,kind,sample_rate,pcm,sha) VALUES('owner-%d','saved','input',16000,X'00010203',%Q)",
            i,manifest,mh,final,fh,i,event,eh,i,ah);
        assert(sql && sqlite3_exec(fixture.db,sql,NULL,NULL,NULL)==SQLITE_OK); sqlite3_free(sql);
    }
    assert(sqlite3_exec(fixture.db,"INSERT INTO turns(owner,id,manifest,manifest_sha) VALUES('owner-0','unfinished','{}','unverified')",NULL,NULL,NULL)==SQLITE_OK);
    /* A separate read-only connection includes committed WAL pages. */
    assert(loop_verify_recordings(source,&verified,&unfinished) && verified==2 && unfinished==1);
    /* Verify the copied snapshot before publishing, including committed WAL data. */
    assert(loop_backup_recordings(source,target,&verified,&unfinished) && verified==2 && unfinished==1);
    assert(!loop_backup_recordings(source,target,&verified,&unfinished) && !verified && !unfinished);
    assert(!unlink(target));
    loop_backup_test_directory_sync_error=EIO;
    assert(!loop_backup_recordings(source,target,&verified,&unfinished) && !verified && !unfinished);
    loop_backup_test_directory_sync_error=0;
    assert(!access(target,F_OK) && !unlink(target));
    assert(sqlite3_exec(fixture.db,"PRAGMA journal_mode=DELETE",NULL,NULL,NULL)==SQLITE_OK);
    loop_close(&fixture);
    struct stat before,after; assert(!stat(source,&before));
    assert(!chmod(source,0400));
    assert(loop_verify_recordings(source,&verified,&unfinished) && verified==2 && unfinished==1);
    assert(!stat(source,&after) && before.st_size==after.st_size && before.st_mtime==after.st_mtime);
    const char *mutations[]={
        "UPDATE turns SET manifest='{}' WHERE status='completed'",
        "UPDATE turns SET final='{}' WHERE status='completed'",
        "UPDATE events SET payload='{}'",
        "DELETE FROM events WHERE owner='owner-1'",
        "UPDATE audio SET pcm=X'00000000'",
        "UPDATE turns SET status='unknown' WHERE status='completed'",
        "UPDATE turns SET owner=owner || char(0) || 'hidden'",
        "PRAGMA user_version=99"
    };
    for(size_t i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++) {
        assert(loop_backup(source,target));
        sqlite3 *db=NULL; assert(sqlite3_open(target,&db)==SQLITE_OK);
        assert(sqlite3_exec(db,mutations[i],NULL,NULL,NULL)==SQLITE_OK);
        assert(sqlite3_close(db)==SQLITE_OK);
        verified=unfinished=99;
        assert(!loop_verify_recordings(target,&verified,&unfinished) && !verified && !unfinished);
        char rejected[256]; snprintf(rejected,sizeof(rejected),"%s/rejected.sqlite",directory);
        verified=unfinished=99;
        assert(!loop_backup_recordings(target,rejected,&verified,&unfinished) && !verified && !unfinished);
        assert(access(rejected,F_OK));
        assert(!unlink(target));
    }
    char sidecar[272];
    snprintf(sidecar,sizeof(sidecar),"%s-wal",source); unlink(sidecar);
    snprintf(sidecar,sizeof(sidecar),"%s-shm",source); unlink(sidecar);
    assert(!unlink(source) && !rmdir(directory));
}

int main(void) {
    test_model_request_projection();
    test_model_capture_consent();
    test_recording_verifier();
    test_runtime_configuration();
    char path[]="/tmp/loop-test-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd);
    loop_service s={.origin="https://loop.test",.gateway_secret="0123456789abcdef0123456789abcdef",.wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
    assert(loop_open(&s,path)); char token[65];
    test_oauth_begin(&s);
    char owner_a[65], owner_b[65];
    s.operators="authentik:operator-a\nauthentik:operator-b";
    assert(loop_hash("authentik:operator-a",strlen("authentik:operator-a"),owner_a));
    assert(loop_hash("authentik:operator-b",strlen("authentik:operator-b"),owner_b));
    assert(loop_session_create(&s,owner_a,token)); snprintf(cookie_a,sizeof(cookie_a),"loop_session=%s",token);
    assert(loop_session_create(&s,owner_b,token)); snprintf(cookie_b,sizeof(cookie_b),"loop_session=%s",token);
    expect(&s,"GET","/api/turns","",NULL,401);
    expect(&s,"GET","/api/turns",cookie_a,NULL,200);
    expect(&s,"GET","/api/config","",NULL,401);
    expect(&s,"GET","/api/config",cookie_a,NULL,200);
    s.wt_url="https://voice.example:9443/v1/voice/turns";
    s.anvil_origin="https://anvil.example";
    loop_response portable=call(&s,"POST","/api/turn-identity",cookie_a,"{\"request_id\":\"portable-authority\"}",200);
    assert(strstr(portable.body,"https://voice.example:9443/v1/voice/turns")); loop_response_free(&portable);
    portable=call(&s,"GET","/api/config",cookie_a,NULL,200);
    assert(strstr(portable.body,"https://anvil.example")); loop_response_free(&portable);
    s.wt_url="https://loop.lab.daviestechlabs.io:8443/other";
    expect(&s,"POST","/api/turn-identity",cookie_a,"{\"request_id\":\"wrong-authority\"}",503);
    s.wt_url=LOOP_WEBTRANSPORT_ENDPOINT;

    loop_request bad={.method="POST",.path="/api/turns",.cookie=cookie_a,.origin="https://evil.test"}; loop_response r;
    assert(loop_handle(&s,&bad,&r)); assert(r.status==403); loop_response_free(&r);
    const char *manifest="{\"request_id\":\"turn-1\",\"recording_consent\":true,\"browser\":\"test\"}";
    expect(&s,"POST","/api/turns",cookie_a,manifest,201);
    expect(&s,"POST","/api/turns",cookie_a,manifest,200);
    expect(&s,"POST","/api/turns",cookie_a,"{\"request_id\":\"turn-1\",\"recording_consent\":true}",409);
    expect(&s,"GET","/api/turns/turn-1",cookie_b,NULL,404);
    expect(&s,"POST","/api/turns",cookie_a,"{\"request_id\":\"dup\",\"request_id\":\"dup\",\"recording_consent\":true}",400);
    expect(&s,"POST","/api/turns",cookie_a,"{\"request_id\":\"secret\",\"recording_consent\":true,\"metadata\":{\"identity_token\":\"secret\"}}",400);
    const char *event="{\"seq\":0,\"name\":\"transcript\",\"source\":\"gateway_observed\",\"elapsed_ms\":0,\"payload\":{\"text\":\"hello\"}}";
    expect(&s,"POST","/api/turns/turn-1/events",cookie_a,event,201);
    expect(&s,"POST","/api/turns/turn-1/events",cookie_a,event,200);
    expect(&s,"POST","/api/turns/turn-1/events",cookie_a,"{\"seq\":2,\"name\":\"gap\",\"source\":\"browser\",\"elapsed_ms\":2,\"payload\":{}}",409);
    unsigned char pcm[]={0,1,2,3}; loop_request audio={.method="POST",.path="/api/turns/turn-1/audio/input/16000",.cookie=cookie_a,.origin=s.origin,.content_type="application/octet-stream",.body=pcm,.body_len=sizeof(pcm)};
    assert(loop_handle(&s,&audio,&r)); assert(r.status==201); loop_response_free(&r);
    assert(loop_handle(&s,&audio,&r)); assert(r.status==200); loop_response_free(&r);
    pcm[0]=9; assert(loop_handle(&s,&audio,&r)); assert(r.status==409); loop_response_free(&r); pcm[0]=0;
    r=call(&s,"GET","/api/turns/turn-1/audio/input/16000",cookie_a,NULL,200); assert(r.size==4 && !memcmp(r.body,pcm,4)); loop_response_free(&r);
    expect(&s,"POST","/api/turns/turn-1/finish",cookie_a,"{\"status\":\"completed\",\"event_count\":2}",409);
    const char *final="{\"status\":\"completed\",\"event_count\":1,\"transcript\":\"hello\",\"answer\":\"hello back\"}";
    expect(&s,"POST","/api/turns/turn-1/finish",cookie_a,final,200);
    expect(&s,"POST","/api/turns/turn-1/finish",cookie_a,final,200);
    expect(&s,"POST","/api/turns/turn-1/events",cookie_a,"{\"seq\":1,\"name\":\"late\",\"source\":\"browser\",\"elapsed_ms\":3,\"payload\":{}}",409);
    const char *capture="{\"id\":\"capture-1\",\"source_turn\":\"turn-1\",\"title\":\"Greeting\",\"category\":\"voice\",\"expected\":\"Say hello\"}";
    expect(&s,"POST","/api/captures",cookie_a,capture,201);
    expect(&s,"POST","/api/captures",cookie_a,capture,200);
    expect(&s,"POST","/api/captures",cookie_b,capture,404);
    r=call(&s,"GET","/api/turns/turn-1",cookie_a,NULL,200); assert(strstr(r.body,"browser_observed") && strstr(r.body,"manifest_sha256") && strstr(r.body,"hello back")); loop_response_free(&r);
    r=call(&s,"POST","/api/turn-identity",cookie_a,"{\"request_id\":\"voice-test\"}",200);
    char identity[512]; assert(cmp_json_str(r.body,"identity_token",identity,sizeof(identity)));
    voice_auth_identity_claims claims; assert(!voice_auth_identity_verify(s.gateway_secret,strlen(s.gateway_secret),identity,"voice-test",(int64_t)time(NULL),&claims));
    assert(!strcmp(claims.user_id,owner_a) && !claims.premium); loop_response_free(&r);
    /* Upgrade an existing v1 database without losing its private evidence. */
    assert(sqlite3_exec(s.db,"DROP TABLE anvil_links; PRAGMA user_version=1",NULL,NULL,NULL)==SQLITE_OK);
    loop_close(&s); assert(loop_open(&s,path));
    r=call(&s,"GET","/api/turns/turn-1",cookie_a,NULL,200); assert(strstr(r.body,"hello back")); loop_response_free(&r);
    /* Persisted sessions must obey the restarted service's current admission policy. */
    loop_close(&s); s.operators="authentik:operator-b"; assert(loop_open(&s,path));
    expect(&s,"GET","/api/session",cookie_a,NULL,401);
    expect(&s,"GET","/api/turns/turn-1",cookie_a,NULL,401);
    expect(&s,"POST","/api/turn-identity",cookie_a,"{\"request_id\":\"revoked\"}",401);
    expect(&s,"GET","/api/session",cookie_b,NULL,200);
    s.operators="authentik:operator-a-extra\noidc:operator-a\n";
    expect(&s,"GET","/api/session",cookie_a,NULL,401);
    s.operators=NULL; expect(&s,"GET","/api/session",cookie_b,NULL,401);
    s.operators=""; expect(&s,"GET","/api/session",cookie_b,NULL,401);
    s.operators="authentik:operator-a\nauthentik:operator-b";
    expect(&s,"GET","/api/turns/turn-1",cookie_a,NULL,200);
    test_report(&s);
    test_ambiguous_measurements(&s);
    test_input_delivery(&s);
    test_response_audio(&s);
    test_anvil_links(&s);
    test_rerun(&s);
    test_storage_full(&s);
    test_backup(&s,path);
    test_backup_readonly_source(&s,path);
    loop_close(&s);
    s.database_max_bytes=1024u*1024u; assert(!loop_open(&s,path));
    s.database_max_bytes=0; assert(loop_open(&s,path));
    expect(&s,"GET","/api/turns/full-store/report",cookie_a,NULL,200);
    r=call(&s,"GET","/api/turns/turn-1/anvil-links",cookie_a,NULL,200); assert(strstr(r.body,"link-1")); loop_response_free(&r);
    assert(sqlite3_exec(s.db,"PRAGMA user_version=99",NULL,NULL,NULL)==SQLITE_OK);
    loop_close(&s); assert(!loop_open(&s,path));
    sqlite3 *future=NULL; sqlite3_stmt *version=NULL;
    assert(sqlite3_open(path,&future)==SQLITE_OK);
    assert(sqlite3_prepare_v2(future,"PRAGMA user_version",-1,&version,NULL)==SQLITE_OK);
    assert(sqlite3_step(version)==SQLITE_ROW && sqlite3_column_int(version,0)==99); sqlite3_finalize(version);
    assert(sqlite3_exec(future,"PRAGMA user_version=2",NULL,NULL,NULL)==SQLITE_OK); sqlite3_close(future);
    assert(loop_open(&s,path));
    expect(&s,"POST","/api/logout",cookie_a,"{}",200); expect(&s,"GET","/api/turns",cookie_a,NULL,401);
    expect(&s,"GET","/api/turns",cookie_b,NULL,200);
    loop_close(&s); unlink(path); puts("Loop C: authentication, origin, ownership, immutable evidence, sequence, audio, failure capture, identity, restart, logout passed"); return 0;
}
