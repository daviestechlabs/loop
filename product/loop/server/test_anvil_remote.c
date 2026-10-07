#include "loop.h"
#include "anvil_remote.h"
#include "cmp_oauth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void envelope(char out[4096],const char *subject,const char *digest,const char *answer,const char *decision) {
    snprintf(out,4096,"{\"schemaVersion\":\"anvil-learning-evidence/v1\",\"ownerSubject\":\"%s\","
        "\"reportSha256\":\"%s\",\"report\":{\"schemaVersion\":\"anvil-learning-report/v1\"},"
        "\"answers\":%s,\"promotionDecision\":%s}",subject,digest,answer,decision);
}
static const char *experiment_id="a1234567-1234-4123-8123-123456789abc";
static void experiment_envelope(char out[4096],const char *subject,const char *id,const char *decision) {
    char snapshot[256],digest[65];
    snprintf(snapshot,sizeof(snapshot),"{\"id\":\"%s\",\"reviews\":[{\"winner\":\"tie\"}]}",id);
    assert(loop_hash(snapshot,strlen(snapshot),digest));
    snprintf(out,4096,"{\"schemaVersion\":\"anvil-experiment-evidence/v1\",\"ownerSubject\":\"%s\","
        "\"experimentId\":\"%s\",\"experiment\":%s,\"snapshotSha256\":\"%s\",\"promotionDecision\":%s}",
        subject,id,snapshot,digest,decision);
}
static void native_envelope(char out[4096],const char *subject,const char *id,int corrupt) {
    char profile[512],profile_sha[65],image_sha[65],snapshot_sha[65];
    const char *images="{\"candidate\":{\"id\":\"fixture\"}}";
    snprintf(profile,sizeof(profile),"{\"modelRevision\":\"%040d\",\"modelManifestSha256\":\"%064d\"}",1,2);
    assert(loop_hash(profile,strlen(profile),profile_sha));
    assert(loop_hash(images,strlen(images),image_sha));
    if (corrupt&1) profile_sha[0]=profile_sha[0]=='a'?'b':'a';
    char receipt[512]="null",receipt_hash[68]="null",sha[65];
    if (corrupt&2) {
        snprintf(receipt,sizeof(receipt),"{\"candidateId\":\"%s\",\"profileSha256\":\"%s\",\"imageBindingSha256\":\"%s\"}",id,profile_sha,image_sha);
        assert(loop_hash(receipt,strlen(receipt),sha));
        if (corrupt&4) sha[0]=sha[0]=='a'?'b':'a';
        snprintf(receipt_hash,sizeof(receipt_hash),"\"%s\"",sha);
    }
    char snapshot[2048];
    snprintf(snapshot,sizeof(snapshot),"{\"candidateId\":\"%s\",\"modelId\":\"%s\",\"scope\":\"historical_admission_not_execution_attestation\","
        "\"profile\":%s,\"profileSha256\":\"%s\",\"images\":%s,\"imageBindingSha256\":\"%s\",\"receipt\":%s,\"receiptSha256\":%s}",
        id,id,profile,profile_sha,images,image_sha,receipt,receipt_hash);
    assert(loop_hash(snapshot,strlen(snapshot),snapshot_sha));
    snprintf(out,4096,"{\"schemaVersion\":\"anvil-native-evidence/v1\",\"ownerSubject\":\"%s\",\"candidateId\":\"%s\","
        "\"admission\":%s,\"snapshotSha256\":\"%s\",\"promotionDecision\":null}",subject,id,snapshot,snapshot_sha);
}
static char expected_url[256];
static loop_service *current;
static char response[4096];
static int calls,fail;
static int fetch(const char *url,const char *token,uint8_t *out,size_t cap,size_t *length) {
    assert(!strcmp(url,expected_url));
    assert(!strcmp(token,"fixture.identity.signature"));
    assert(!pthread_mutex_trylock(&current->mutex));pthread_mutex_unlock(&current->mutex);
    calls++;if (fail) return -1;
    *length=strlen(response);assert(*length<cap);memcpy(out,response,*length);return 0;
}
static void transport_test(const char *owner,const char *digest) {
    cmp_gateway_identity identity={.policy={.issuer="https://issuer.test"}};
    loop_service service={.sso=&identity,.anvil_origin="https://anvil.example:9443"};assert(loop_open(&service,":memory:"));current=&service;
    loop_response r={0};loop_request q={.method="POST",.path="/api/turns",.content_type="application/json"};
    q.body=(const unsigned char *)"{\"request_id\":\"remote-turn\",\"recording_consent\":true}";q.body_len=strlen((const char *)q.body);
    assert(loop_evidence(&service,owner,&q,&r) && r.status==201);loop_response_free(&r);
    q.path="/api/turns/remote-turn/finish";q.body=(const unsigned char *)"{\"status\":\"failed\",\"event_count\":0}";q.body_len=strlen((const char *)q.body);
    assert(loop_evidence(&service,owner,&q,&r) && r.status==200);loop_response_free(&r);
    assert(loop_report(&service,owner,"remote-turn",&r) && r.status==200);
    char receipt[65],definition[512];assert(cmp_json_str(r.body,"receiptSha256",receipt,sizeof(receipt)));loop_response_free(&r);
    snprintf(definition,sizeof(definition),"{\"id\":\"report-1\",\"kind\":\"learning_report\",\"reference\":\"%s\",\"receipt_sha256\":\"%s\"}",digest,receipt);
    q.body=(const unsigned char *)definition;q.body_len=strlen(definition);
    assert(loop_anvil_links(&service,owner,"remote-turn",&q,&r) && r.status==201);loop_response_free(&r);
    q=(loop_request){.method="GET",.path="/api/turns/remote-turn/anvil-links/report-1/evidence",.verified_owner=owner,.cookie="must-not-forward=fixture",.gateway_token="fixture.identity.signature"};
    snprintf(expected_url,sizeof(expected_url),"https://anvil.example:9443/api/loop-evidence/learning-evidence/%s",digest);
    envelope(response,"owner",digest,"null","null");
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==200);loop_response_free(&r);assert(calls==1);
    service.anvil_origin="https://anvil.example:9443/?redirect=other";
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==503);loop_response_free(&r);assert(calls==1);
    service.anvil_origin="https://anvil.example:9443";
    envelope(response,"other",digest,"null","null");
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==502);loop_response_free(&r);
    fail=1;assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==502);loop_response_free(&r);fail=0;
    int before=calls;q.verified_owner="other";
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==404);loop_response_free(&r);assert(calls==before);
    q.verified_owner=owner;q.gateway_token=NULL;
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==401);loop_response_free(&r);assert(calls==before);
    q.gateway_token="fixture.identity.signature";q.path="/api/turns/remote-turn/anvil-links/report-1/evidence?url=https://other.test";
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==400);loop_response_free(&r);assert(calls==before);
    snprintf(definition,sizeof(definition),"{\"id\":\"experiment-1\",\"kind\":\"experiment\",\"reference\":\"%s\",\"receipt_sha256\":\"%s\"}",experiment_id,receipt);
    loop_request add={.method="POST",.content_type="application/json",.body=(const unsigned char *)definition,.body_len=strlen(definition)};
    assert(loop_anvil_links(&service,owner,"remote-turn",&add,&r) && r.status==201);loop_response_free(&r);
    q.path="/api/turns/remote-turn/anvil-links/experiment-1/evidence";
    snprintf(expected_url,sizeof(expected_url),"https://anvil.example:9443/api/loop-evidence/experiment-evidence/%s",experiment_id);
    experiment_envelope(response,"owner",experiment_id,"null");
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==200);loop_response_free(&r);
    experiment_envelope(response,"other",experiment_id,"null");
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==502);loop_response_free(&r);
    snprintf(definition,sizeof(definition),"{\"id\":\"native-1\",\"kind\":\"native_evaluation\",\"reference\":\"%s\",\"receipt_sha256\":\"%s\"}",experiment_id,receipt);
    add.body=(const unsigned char *)definition;add.body_len=strlen(definition);
    assert(loop_anvil_links(&service,owner,"remote-turn",&add,&r) && r.status==201);loop_response_free(&r);
    q.path="/api/turns/remote-turn/anvil-links/native-1/evidence";
    snprintf(expected_url,sizeof(expected_url),"https://anvil.example:9443/api/loop-evidence/native-evidence/%s",experiment_id);
    native_envelope(response,"owner",experiment_id,0);
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==200);loop_response_free(&r);
    native_envelope(response,"other",experiment_id,0);
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==502);loop_response_free(&r);
    native_envelope(response,"owner",experiment_id,1);
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==502);loop_response_free(&r);
    before=calls;
    q.path="/api/turns/remote-turn/anvil-links/report-1/evidence";
    assert(sqlite3_exec(service.db,"UPDATE anvil_links SET sha='changed'",NULL,NULL,NULL)==SQLITE_OK);
    assert(loop_anvil_remote_read(&service,&q,&r,fetch) && r.status==409);loop_response_free(&r);assert(calls==before);
    loop_close(&service);
    uint8_t out[8];size_t len=0;
    assert(cmp_oauth_session_get("https://anvil.lab.daviestechlabs.io/","bad\r\nHeader: injected",out,sizeof(out),&len)!=0);
    assert(cmp_oauth_session_get("http://anvil.lab.daviestechlabs.io/","gateway=fixture",out,sizeof(out),&len)!=0);
}
int main(void) {
    char json[4096],owner[65],digest[65]; memset(digest,'a',64);digest[64]=0;
    const char *issuer="https://issuer.test",*principal="https://issuer.test\nowner";
    assert(loop_hash(principal,strlen(principal),owner));
    envelope(json,"owner",digest,"null","null");
    assert(loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,0));
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,1));
    assert(!loop_anvil_remote_valid(json,strlen(json),"https://other.test",owner,digest,0));
    char changed[65];memset(changed,'b',64);changed[64]=0;
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,changed,0));
    assert(!loop_anvil_remote_valid(json,strlen(json)-1,issuer,owner,digest,0));
    envelope(json,"other",digest,"null","null");
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,0));
    envelope(json,"owner",digest,"{\"promotionEligible\":false}","null");
    assert(loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,1));
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,0));
    envelope(json,"owner",digest,"{\"promotionEligible\":true}","null");
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,1));
    envelope(json,"owner",digest,"{\"promotionEligible\":false,\"promotionEligible\":true}","null");
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,1));
    envelope(json,"owner",digest,"null","\"approved\"");
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,0));
    envelope(json,"owner",digest,"null","null");
    size_t n=strlen(json);snprintf(json+n-1,sizeof(json)-n+1,",\"ownerSubject\":\"other\"}");
    assert(!loop_anvil_remote_valid(json,strlen(json),issuer,owner,digest,0));
    experiment_envelope(json,"owner",experiment_id,"null");
    assert(loop_anvil_experiment_valid(json,strlen(json),issuer,owner,experiment_id));
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,"b1234567-1234-4123-8123-123456789abc"));
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,"../other"));
    assert(!loop_anvil_experiment_valid(json,strlen(json),"https://other.test",owner,experiment_id));
    assert(!loop_anvil_experiment_valid(json,strlen(json)-1,issuer,owner,experiment_id));
    char *hash=strstr(json,"snapshotSha256\":\"");assert(hash);hash+=strlen("snapshotSha256\":\"");hash[0]=hash[0]=='a'?'b':'a';
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,experiment_id));
    experiment_envelope(json,"owner",experiment_id,"true");
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,experiment_id));
    experiment_envelope(json,"other",experiment_id,"null");
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,experiment_id));
    experiment_envelope(json,"owner",experiment_id,"null");
    char *winner=strstr(json,"tie");assert(winner);memcpy(winner,"win",3);
    assert(!loop_anvil_experiment_valid(json,strlen(json),issuer,owner,experiment_id));
    native_envelope(json,"owner",experiment_id,0);
    assert(loop_anvil_native_valid(json,strlen(json),issuer,owner,experiment_id));
    assert(!loop_anvil_native_valid(json,strlen(json)-1,issuer,owner,experiment_id));
    assert(!loop_anvil_native_valid(json,strlen(json),"other",owner,experiment_id));
    assert(!loop_anvil_native_valid(json,strlen(json),issuer,owner,"../other"));
    native_envelope(json,"owner",experiment_id,1);
    assert(!loop_anvil_native_valid(json,strlen(json),issuer,owner,experiment_id));
    native_envelope(json,"owner",experiment_id,2);
    assert(loop_anvil_native_valid(json,strlen(json),issuer,owner,experiment_id));
    native_envelope(json,"owner",experiment_id,6);
    assert(!loop_anvil_native_valid(json,strlen(json),issuer,owner,experiment_id));
    transport_test(owner,digest);
    puts("Anvil remote envelope and read tests passed");return 0;
}
