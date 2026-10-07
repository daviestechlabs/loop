#include "loop.h"
#include "anvil_remote.h"
#include <stdio.h>
#include <string.h>

static int remote_owner_valid(const cmp_json_object *root,const char *issuer,const char *owner) {
    char subject[256],principal[1536],actual[65];
    if (!issuer || !owner || strlen(owner)!=64 ||
        !cmp_json_object_str(root,"ownerSubject",subject,sizeof(subject)) || !subject[0]) return 0;
    for (const unsigned char *p=(const unsigned char *)subject;*p;p++) if (*p<32 || *p==127) return 0;
    int n=snprintf(principal,sizeof(principal),"%s\n%s",issuer,subject);
    return n>=0 && (size_t)n<sizeof(principal) && loop_hash(principal,(size_t)n,actual) && !strcmp(actual,owner);
}
static int remote_uuid_valid(const char *id) {
    if (!id || strlen(id)!=36 || id[14]!='4' || !strchr("89ab",id[19])) return 0;
    for (size_t i=0;i<36;i++) {
        if (i==8 || i==13 || i==18 || i==23) { if (id[i]!='-') return 0; }
        else if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'))) return 0;
    }
    return 1;
}
int loop_anvil_experiment_valid(const char *json,size_t length,const char *issuer,const char *owner,const char *id) {
    cmp_json_object root,experiment; char schema[64],actual_id[37],digest[65],actual_digest[65];
    if (!json || !remote_uuid_valid(id) || !length || length>1024u*1024u || memchr(json,0,length) || json[length]!=0 ||
        !loop_json_valid(json,&root) || root.field_count!=6 ||
        !cmp_json_object_str(&root,"schemaVersion",schema,sizeof(schema)) || strcmp(schema,"anvil-experiment-evidence/v1") ||
        !cmp_json_object_str(&root,"experimentId",actual_id,sizeof(actual_id)) || strcmp(actual_id,id) ||
        !remote_owner_valid(&root,issuer,owner) ||
        !cmp_json_object_object(&root,"experiment",&experiment) ||
        !cmp_json_object_str(&experiment,"id",actual_id,sizeof(actual_id)) || strcmp(actual_id,id) ||
        !cmp_json_object_str(&root,"snapshotSha256",digest,sizeof(digest))) return 0;
    const cmp_json_field *snapshot=cmp_json_object_field(&root,"experiment");
    const cmp_json_field *decision=cmp_json_object_field(&root,"promotionDecision");
    return snapshot && decision && decision->value_len==4 && !memcmp(decision->value,"null",4) &&
        loop_hash(snapshot->value,snapshot->value_len,actual_digest) && !strcmp(digest,actual_digest);
}

static int object_hash_valid(const cmp_json_object *object,const char *field,const char *hash_field) {
    char expected[65],actual[65];
    const cmp_json_field *value=cmp_json_object_field(object,field);
    return value && cmp_json_object_str(object,hash_field,expected,sizeof(expected)) &&
        loop_hash(value->value,value->value_len,actual) && !strcmp(expected,actual);
}
int loop_anvil_native_valid(const char *json,size_t length,const char *issuer,const char *owner,const char *id) {
    cmp_json_object root,admission,profile; char schema[64],actual_id[37],scope[64],revision[41],manifest[65];
    if (!json || !remote_uuid_valid(id) || !length || length>65536 || memchr(json,0,length) || json[length]!=0 ||
        !loop_json_valid(json,&root) || root.field_count!=6 ||
        !cmp_json_object_str(&root,"schemaVersion",schema,sizeof(schema)) || strcmp(schema,"anvil-native-evidence/v1") ||
        !cmp_json_object_str(&root,"candidateId",actual_id,sizeof(actual_id)) || strcmp(actual_id,id) ||
        !remote_owner_valid(&root,issuer,owner) || !cmp_json_object_object(&root,"admission",&admission) ||
        !cmp_json_object_str(&admission,"candidateId",actual_id,sizeof(actual_id)) || strcmp(actual_id,id) ||
        !cmp_json_object_str(&admission,"modelId",actual_id,sizeof(actual_id)) || !remote_uuid_valid(actual_id) ||
        !cmp_json_object_str(&admission,"scope",scope,sizeof(scope)) || strcmp(scope,"historical_admission_not_execution_attestation") ||
        !cmp_json_object_object(&admission,"profile",&profile) ||
        !cmp_json_object_str(&profile,"modelRevision",revision,sizeof(revision)) || strlen(revision)!=40 ||
        !cmp_json_object_str(&profile,"modelManifestSha256",manifest,sizeof(manifest)) || strlen(manifest)!=64 ||
        !object_hash_valid(&root,"admission","snapshotSha256") ||
        !object_hash_valid(&admission,"profile","profileSha256") ||
        !object_hash_valid(&admission,"images","imageBindingSha256")) return 0;
    for (size_t i=0;i<40;i++) if (!strchr("0123456789abcdef",revision[i])) return 0;
    for (size_t i=0;i<64;i++) if (!strchr("0123456789abcdef",manifest[i])) return 0;
    const cmp_json_field *decision=cmp_json_object_field(&root,"promotionDecision");
    const cmp_json_field *receipt=cmp_json_object_field(&admission,"receipt");
    const cmp_json_field *receipt_hash=cmp_json_object_field(&admission,"receiptSha256");
    if (!decision || decision->value_len!=4 || memcmp(decision->value,"null",4) || !receipt || !receipt_hash) return 0;
    if (receipt->value_len==4 && !memcmp(receipt->value,"null",4))
        return receipt_hash->value_len==4 && !memcmp(receipt_hash->value,"null",4);
    cmp_json_object result; char bound[65],expected[65];
    if (!cmp_json_object_object(&admission,"receipt",&result) ||
        !cmp_json_object_str(&result,"candidateId",actual_id,sizeof(actual_id)) || strcmp(actual_id,id) ||
        !cmp_json_object_str(&result,"profileSha256",bound,sizeof(bound)) ||
        !cmp_json_object_str(&admission,"profileSha256",expected,sizeof(expected)) || strcmp(bound,expected) ||
        !cmp_json_object_str(&result,"imageBindingSha256",bound,sizeof(bound)) ||
        !cmp_json_object_str(&admission,"imageBindingSha256",expected,sizeof(expected)) || strcmp(bound,expected)) return 0;
    return object_hash_valid(&admission,"receipt","receiptSha256");
}

int loop_anvil_remote_valid(const char *json, size_t length, const char *issuer,
                           const char *owner, const char *report_hash, int answers) {
    cmp_json_object root, report, review;
    char schema[64], digest[65];
    if (!json || !issuer || !owner || !report_hash || strlen(owner)!=64 || strlen(report_hash)!=64 ||
        !length || length>1024u*1024u || memchr(json,0,length) || json[length]!=0 ||
        !loop_json_valid(json,&root) || root.field_count!=6 ||
        !cmp_json_object_str(&root,"schemaVersion",schema,sizeof(schema)) || strcmp(schema,"anvil-learning-evidence/v1") ||
        !cmp_json_object_str(&root,"reportSha256",digest,sizeof(digest)) || strcmp(digest,report_hash) ||
        !remote_owner_valid(&root,issuer,owner) ||
        !cmp_json_object_object(&root,"report",&report) ||
        !cmp_json_object_str(&report,"schemaVersion",schema,sizeof(schema)) || strcmp(schema,"anvil-learning-report/v1")) return 0;
    for (size_t i=0;i<64;i++) if (!((digest[i]>='a' && digest[i]<='f') || (digest[i]>='0' && digest[i]<='9'))) return 0;
    const cmp_json_field *decision=cmp_json_object_field(&root,"promotionDecision");
    const cmp_json_field *answer=cmp_json_object_field(&root,"answers");
    if (!decision || !answer || (decision->value_len!=4 || memcmp(decision->value,"null",4))) return 0;
    if (!answers) return answer->value_len==4 && !memcmp(answer->value,"null",4);
    if (!cmp_json_object_object(&root,"answers",&review)) return 0;
    int eligible=1;
    return cmp_json_object_bool(&review,"promotionEligible",&eligible) && !eligible;
}

#include "cmp_oauth.h"
#include <stdlib.h>

int loop_anvil_remote_read(loop_service *s,const loop_request *q,loop_response *r,
                          int (*fetch)(const char *,const char *,uint8_t *,size_t,size_t *)) {
    char turn[81],link[81],extra;
    if (strcmp(q->method,"GET")) return loop_reply(r,405,"{\"error\":\"method_not_allowed\"}");
    if (!s->sso || !q->verified_owner) return loop_reply(r,401,"{\"error\":\"shared_session_required\"}");
    if (sscanf(q->path,"/api/turns/%80[^/]/anvil-links/%80[^/]/evidence%c",turn,link,&extra)!=2 ||
        !loop_id_valid(turn) || !loop_id_valid(link)) return loop_reply(r,400,"{\"error\":\"invalid_reference_path\"}");
    char expected_path[256];snprintf(expected_path,sizeof(expected_path),"/api/turns/%s/anvil-links/%s/evidence",turn,link);
    if (strcmp(expected_path,q->path)) return loop_reply(r,400,"{\"error\":\"invalid_reference_path\"}");
    pthread_mutex_lock(&s->mutex);
    loop_request local=*q;local.method="GET";
    int result=loop_anvil_links(s,q->verified_owner,turn,&local,r);
    char digest[65]={0},kind[32]={0};
    if (result && r->status==200) {
        loop_response_free(r);
        sqlite3_stmt *st=NULL;
        if (sqlite3_prepare_v2(s->db,"SELECT definition FROM anvil_links WHERE owner=?1 AND turn_id=?2 AND id=?3",-1,&st,NULL)!=SQLITE_OK) {
            result=loop_store_error(s,r);
        } else {
            sqlite3_bind_text(st,1,q->verified_owner,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,turn,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,3,link,-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(st);cmp_json_object object;
            if (rc==SQLITE_DONE) result=loop_reply(r,404,"{\"error\":\"reference_not_found\"}");
            else if (rc!=SQLITE_ROW) result=loop_store_error(s,r);
            else if (!loop_json_valid((const char *)sqlite3_column_text(st,0),&object) ||
                !cmp_json_object_str(&object,"kind",kind,sizeof(kind)) ||
                !cmp_json_object_str(&object,"reference",digest,sizeof(digest)))
                result=loop_reply(r,409,"{\"error\":\"reference_integrity_failed\"}");
            sqlite3_finalize(st);
        }
    }
    pthread_mutex_unlock(&s->mutex);
    if (r->body || !result) return result;
    int answers=!strcmp(kind,"answer_review"),experiment=!strcmp(kind,"experiment"),native=!strcmp(kind,"native_evaluation");
    if (!native && !experiment && !answers && strcmp(kind,"learning_report")) return loop_reply(r,409,"{\"error\":\"reference_read_unsupported\"}");
    if (!q->gateway_token || !*q->gateway_token) return loop_reply(r,401,"{\"error\":\"shared_session_required\"}");
    /* Only checked hashes or UUIDs reach the fixed Anvil HTTPS authority. */
    if (experiment || native) {
        if (!remote_uuid_valid(digest)) return loop_reply(r,409,"{\"error\":\"reference_integrity_failed\"}");
    } else {
        if (strlen(digest)!=64) return loop_reply(r,409,"{\"error\":\"reference_integrity_failed\"}");
        for (size_t i=0;i<64;i++) if (!((digest[i]>='0' && digest[i]<='9') || (digest[i]>='a' && digest[i]<='f')))
            return loop_reply(r,409,"{\"error\":\"reference_integrity_failed\"}");
    }
    const char *origin=loop_anvil_origin(s);
    if (!origin) return loop_reply(r,503,"{\"error\":\"anvil_not_configured\"}");
    char url[512];
    if (native) snprintf(url,sizeof(url),"%s/api/loop-evidence/native-evidence/%s",origin,digest);
    else if (experiment) snprintf(url,sizeof(url),"%s/api/loop-evidence/experiment-evidence/%s",origin,digest);
    else snprintf(url,sizeof(url),"%s/api/loop-evidence/learning-evidence/%s%s",origin,digest,answers?"?view=answers":"");
    size_t capacity=1024u*1024u,length=0;char *body=calloc(1,capacity+1);
    if (!body) return loop_reply(r,503,"{\"error\":\"evidence_unavailable\"}");
    if (!fetch) fetch=cmp_oauth_identity_get;
    int fetched=fetch(url,q->gateway_token,(uint8_t *)body,capacity,&length)==0 && length<=capacity;
    int ok=fetched;
    if (ok) { body[length]=0;ok=native?loop_anvil_native_valid(body,length,s->sso->policy.issuer,q->verified_owner,digest):experiment?loop_anvil_experiment_valid(body,length,s->sso->policy.issuer,q->verified_owner,digest):
        loop_anvil_remote_valid(body,length,s->sso->policy.issuer,q->verified_owner,digest,answers); }
    /* Log no credential, evidence body, or owner. The API stays fail-closed. */
    if (!ok) fprintf(stderr,"Loop Anvil evidence read failed: %s\n",fetched?"verification":"HTTPS fetch");
    result=ok?loop_reply(r,200,body):loop_reply(r,502,"{\"error\":\"anvil_evidence_unverified\"}");
    free(body);return result;
}
