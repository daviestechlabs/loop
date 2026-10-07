#include "loop.h"
#include "cmp_oauth.h"
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static void encode(const unsigned char *bytes,size_t len,char *out) {
    int n=EVP_EncodeBlock((unsigned char *)out,bytes,(int)len); assert(n>0);
    while (n && out[n-1]=='=') out[--n]=0;
    for (int i=0;i<n;i++) { if (out[i]=='+') out[i]='-'; if (out[i]=='/') out[i]='_'; }
}
static void token(EVP_PKEY *key,const char *header,const char *claims,char out[16385]) {
    char h[2048],c[8192],sig[2048]; unsigned char signed_bytes[1024]; size_t len=sizeof(signed_bytes);
    encode((const unsigned char *)header,strlen(header),h); encode((const unsigned char *)claims,strlen(claims),c);
    int n=snprintf(out,16385,"%s.%s",h,c); assert(n>0 && n<12000);
    EVP_MD_CTX *md=EVP_MD_CTX_new(); assert(md && EVP_DigestSignInit(md,NULL,EVP_sha256(),NULL,key)==1);
    assert(EVP_DigestSign(md,signed_bytes,&len,(const unsigned char *)out,(size_t)n)==1); EVP_MD_CTX_free(md);
    encode(signed_bytes,len,sig); snprintf(out+n,16385-(size_t)n,".%s",sig);
}
static void jwks(EVP_PKEY *key,const char *kid,char out[4096]) {
    BIGNUM *n=NULL,*e=NULL; unsigned char bytes[1024]; char ns[1400],es[24];
    assert(EVP_PKEY_get_bn_param(key,OSSL_PKEY_PARAM_RSA_N,&n) && EVP_PKEY_get_bn_param(key,OSSL_PKEY_PARAM_RSA_E,&e));
    int len=BN_bn2bin(n,bytes); encode(bytes,(size_t)len,ns); len=BN_bn2bin(e,bytes); encode(bytes,(size_t)len,es);
    snprintf(out,4096,"{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"%s\",\"alg\":\"RS256\",\"use\":\"sig\",\"n\":\"%s\",\"e\":\"%s\"}]}",kid,ns,es);
    BN_free(n); BN_free(e);
}
typedef struct { const char *json; int calls,fail; } transport;
static int fetch_keys(const char *method,const char *url,const char *type,const char *auth,const uint8_t *body,size_t len,
                      uint8_t *out,size_t cap,size_t *out_len,void *user) {
    (void)type; transport *t=user; t->calls++;
    assert(!strcmp(method,"GET") && !strcmp(url,"https://issuer.test/jwks") && !auth && !body && !len);
    if (t->fail) return -1;
    *out_len=strlen(t->json); assert(*out_len<cap); memcpy(out,t->json,*out_len); return 0;
}
static loop_response request(loop_service *service,const char *jwt,const char *method,
                             const char *path,const void *body,size_t len,int expected) {
    loop_request q={.method=method,.path=path,.gateway_token=jwt,.origin=service->origin,
        .content_type=strstr(path,"/audio/") ? "application/octet-stream" : "application/json",
        .body=body,.body_len=len};
    loop_response r; assert(loop_handle(service,&q,&r) && r.status==expected); return r;
}
static void test_gateway_recovery(cmp_gateway_identity *identity,EVP_PKEY *key,
                                  const char *header,const char *owner_token,int64_t now) {
    char directory[]="/tmp/loop-sso-recovery-XXXXXX";
    int fd=mkstemp(directory); assert(fd>=0); close(fd);
    assert(!unlink(directory) && !mkdir(directory,0700));
    char source[256],snapshot[256],sidecar[272];
    snprintf(source,sizeof(source),"%s/source.sqlite",directory);
    snprintf(snapshot,sizeof(snapshot),"%s/restored.sqlite",directory);
    loop_service live={.origin="https://loop.test",.sso=identity};
    loop_service restored={.origin="https://restore.test",.sso=identity,
        .gateway_secret="synthetic-recovery-capture-verifier-key"};
    assert(loop_open(&live,source));
    const char *manifest="{\"request_id\":\"sso-recovery\",\"recording_consent\":true}";
    loop_response r=request(&live,owner_token,"POST","/api/turns",manifest,strlen(manifest),201);
    loop_response_free(&r);
    unsigned char pcm[640]={41,0,73,0};
    r=request(&live,owner_token,"POST","/api/turns/sso-recovery/audio/input/16000",pcm,sizeof(pcm),201);
    loop_response_free(&r);
    const char *final="{\"status\":\"failed\",\"event_count\":0,\"error\":\"Synthetic SSO recovery fixture\"}";
    r=request(&live,owner_token,"POST","/api/turns/sso-recovery/finish",final,strlen(final),200);
    loop_response_free(&r);
    const char *paths[]={"/api/turns/sso-recovery/receipt","/api/turns/sso-recovery/report",
        "/api/turns/sso-recovery/replay/input.s16le","/api/turns/sso-recovery/replay/input.wav"};
    loop_response expected[4];
    for(size_t i=0;i<4;i++) expected[i]=request(&live,owner_token,"GET",paths[i],NULL,0,200);
    uint64_t verified=0,unfinished=0;
    /* Keep the source open so recovery includes committed WAL data. */
    assert(loop_backup_recordings(source,snapshot,&verified,&unfinished));
    assert(verified==1 && unfinished==0 && loop_open(&restored,snapshot));
    char other_claims[2048],other_token[16385];
    snprintf(other_claims,sizeof(other_claims),
        "{\"iss\":\"https://issuer.test/\",\"aud\":\"loop-client\",\"sub\":\"operator-b\",\"iat\":%lld,\"exp\":%lld,\"groups\":[\"homelab-admins\"]}",
        (long long)now,(long long)now+300);
    token(key,header,other_claims,other_token);
    r=request(&restored,other_token,"GET","/api/session",NULL,0,200); loop_response_free(&r);
    for(size_t i=0;i<4;i++) {
        r=request(&restored,owner_token,"GET",paths[i],NULL,0,200);
        assert(r.size==expected[i].size && !memcmp(r.body,expected[i].body,r.size)); loop_response_free(&r);
        r=request(&restored,other_token,"GET",paths[i],NULL,0,404); loop_response_free(&r);
        r=request(&restored,NULL,"GET",paths[i],NULL,0,401); loop_response_free(&r);
        loop_response_free(&expected[i]);
    }
    r=request(&restored,owner_token,"GET","/api/anvil-model-request/sso-recovery",NULL,0,404);
    assert(strstr(r.body,"model_request_not_recorded")); loop_response_free(&r);
    r=request(&restored,other_token,"GET","/api/anvil-model-request/sso-recovery",NULL,0,404);
    assert(strstr(r.body,"turn_not_found")); loop_response_free(&r);
    r=request(&restored,NULL,"GET","/api/anvil-model-request/sso-recovery",NULL,0,401); loop_response_free(&r);
    r=request(&restored,owner_token,"POST","/api/anvil-model-request/sso-recovery",NULL,0,405); loop_response_free(&r);
    r=request(&restored,owner_token,"GET","/api/anvil-model-request/sso-recovery/receipt",NULL,0,400); loop_response_free(&r);
    /* Recovery must use gateway identity, never an opaque local-session cookie. */
    loop_request cookie={.method="GET",.path=paths[0],.cookie="loop_session=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    assert(loop_handle(&restored,&cookie,&r) && r.status==401); loop_response_free(&r);
    r=request(&live,owner_token,"GET",paths[0],NULL,0,200); loop_response_free(&r);
    loop_close(&restored); loop_close(&live);
    const char *files[]={source,snapshot};
    for(size_t i=0;i<2;i++) {
        snprintf(sidecar,sizeof(sidecar),"%s-wal",files[i]); unlink(sidecar);
        snprintf(sidecar,sizeof(sidecar),"%s-shm",files[i]); unlink(sidecar);
        assert(!unlink(files[i]));
    }
    assert(!rmdir(directory));
}
int main(void) {
    EVP_PKEY *key=EVP_PKEY_Q_keygen(NULL,NULL,"RSA",2048); assert(key);
    cmp_gateway_policy policy={"https://issuer.test/","loop-client","homelab-admins","https://issuer.test/jwks"};
    char keys[4096],jwt[16385],claims[2048],subject[256]; jwks(key,"first",keys);
    const char *header="{\"alg\":\"RS256\",\"kid\":\"first\",\"typ\":\"JWT\"}";
    int64_t now=(int64_t)time(NULL);
    snprintf(claims,sizeof(claims),"{\"iss\":\"https://issuer.test/\",\"aud\":\"loop-client\",\"sub\":\"operator-a\",\"iat\":%lld,\"exp\":%lld,\"groups\":[\"homelab-admins\"]}",(long long)now,(long long)now+300);
    token(key,header,claims,jwt);
    assert(cmp_gateway_verify(jwt,keys,&policy,now,subject)==200 && !strcmp(subject,"operator-a"));
    assert(cmp_gateway_verify(jwt,keys,&policy,now+306,subject)==401 && !*subject);
    assert(cmp_gateway_verify(jwt,keys,&policy,now-6,subject)==401);
    cmp_gateway_policy wrong=policy; wrong.issuer="https://other.test/"; assert(cmp_gateway_verify(jwt,keys,&wrong,now,subject)==401);
    wrong=policy; wrong.audience="other"; assert(cmp_gateway_verify(jwt,keys,&wrong,now,subject)==401);
    wrong=policy; wrong.required_group="other"; assert(cmp_gateway_verify(jwt,keys,&wrong,now,subject)==403);
    const char *bad_headers[]={"{\"alg\":\"none\",\"kid\":\"first\"}","{\"alg\":\"HS256\",\"kid\":\"first\"}",
        "{\"alg\":\"RS256\",\"kid\":\"first\",\"alg\":\"RS256\"}","{\"alg\":\"RS256\",\"kid\":\"first\",\"jku\":\"https://attacker.test/\"}",
        "{\"alg\":\"RS256\",\"kid\":\"first\",\"crit\":[]}","{\"alg\":\"RS256\"}"};
    for (size_t i=0;i<sizeof(bad_headers)/sizeof(bad_headers[0]);i++) { token(key,bad_headers[i],claims,jwt); assert(cmp_gateway_verify(jwt,keys,&policy,now,subject)==401); }
    const char *bad_fields[]={"\"aud\":\"loop-client\",\"sub\":\"a\",\"sub\":\"b\"", "\"aud\":\"other\",\"sub\":\"a\"",
        "\"aud\":[\"loop-client\",\"other\"],\"sub\":\"a\"", "\"aud\":\"loop-client\",\"sub\":\" \"",
        "\"aud\":\"loop-client\",\"sub\":\"a\\u0000b\"", "\"aud\":\"loop-client\",\"sub\":\"a\",\"nbf\":999999999999"};
    char invalid[2048];
    for (size_t i=0;i<sizeof(bad_fields)/sizeof(bad_fields[0]);i++) {
        snprintf(invalid,sizeof(invalid),"{\"iss\":\"https://issuer.test/\",%s,\"iat\":%lld,\"exp\":%lld,\"groups\":[\"homelab-admins\"]}",bad_fields[i],(long long)now,(long long)now+300);
        token(key,header,invalid,jwt); assert(cmp_gateway_verify(jwt,keys,&policy,now,subject)==401);
    }
    token(key,header,claims,jwt); jwt[strlen(jwt)-10]^=1; assert(cmp_gateway_verify(jwt,keys,&policy,now,subject)==401);
    token(key,header,claims,jwt);
    EVP_PKEY *other=EVP_PKEY_Q_keygen(NULL,NULL,"RSA",2048); assert(other);
    token(other,header,claims,jwt); assert(cmp_gateway_verify(jwt,keys,&policy,now,subject)==401);
    EVP_PKEY_free(other); token(key,header,claims,jwt);
    char duplicate[8192]; size_t kl=strlen(keys);
    snprintf(duplicate,sizeof(duplicate),"{\"keys\":[%.*s,%.*s]}",(int)kl-11,keys+9,(int)kl-11,keys+9);
    assert(cmp_gateway_verify(jwt,duplicate,&policy,now,subject)==503);
    assert(cmp_gateway_verify(jwt,"{\"keys\":false}",&policy,now,subject)==503);
    transport t={.json=keys}; cmp_oauth_set_http_for_tests(fetch_keys,&t);
    cmp_gateway_identity identity; assert(cmp_gateway_identity_init(&identity,&policy));
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==200 && t.calls==1);
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==200 && t.calls==1);
    loop_service s={.origin="https://loop.test",.sso=&identity,.gateway_secret="synthetic-secret-at-least-thirty-two-bytes",.wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
    assert(!loop_runtime_configuration_error(&s,NULL) && loop_open(&s,":memory:"));
    loop_request q={.method="GET",.path="/api/session",.gateway_token=jwt}; loop_response r;
    assert(loop_handle(&s,&q,&r) && r.status==200); assert(!strstr(r.body,"operator-a")); loop_response_free(&r);
    q.gateway_token=NULL; q.verified_owner="forged";
    assert(loop_handle(&s,&q,&r) && r.status==401); loop_response_free(&r);
    q.gateway_token=jwt; q.method="POST"; q.path="/api/turn-identity"; q.origin="https://attacker.test";
    q.content_type="application/json"; q.body=(const unsigned char *)"{\"request_id\":\"sso-turn\"}"; q.body_len=strlen((const char *)q.body);
    assert(loop_handle(&s,&q,&r) && r.status==403); loop_response_free(&r);
    q.origin=s.origin; assert(loop_handle(&s,&q,&r) && r.status==200); loop_response_free(&r); loop_close(&s);
    test_gateway_recovery(&identity,key,header,jwt,now);
    /* Unknown keys cannot trigger an unbounded refresh storm. */
    token(key,"{\"alg\":\"RS256\",\"kid\":\"second\"}",claims,jwt);
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==401 && t.calls==1);
    jwks(key,"second",keys); identity.attempted-=31;
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==200 && t.calls==2);
    t.fail=1; identity.expires=0; identity.attempted-=31;
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==503 && t.calls==3);
    assert(cmp_gateway_identity_verify(&identity,jwt,subject)==503 && t.calls==3);
    cmp_gateway_identity_close(&identity); cmp_oauth_set_http_for_tests(NULL,NULL); EVP_PKEY_free(key);
    puts("Loop shared SSO: signed claims, authorization, source isolation, cache rotation, and failure checks passed"); return 0;
}
