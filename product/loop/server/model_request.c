#include "loop.h"
#include "model_request_capture.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int request_valid(const char *body) {
    cmp_json_object root, options, kwargs;
    cmp_json_array messages; cmp_json_field item;
    char model[256],role[16]; char *content=malloc(DND_RAG_PROMPT_CAP);
    int stream=0, reasoning=1, usage=0, thinking=1, count=0, rc;
    int64_t tokens=0; double temperature=0;
    int valid=content && loop_json_valid(body,&root) && root.field_count==8 &&
        cmp_json_object_str(&root,"model",model,sizeof(model)) && model[0] &&
        cmp_json_object_i64(&root,"max_completion_tokens",&tokens) && tokens>0 && tokens<=4096 &&
        cmp_json_field_double(cmp_json_object_field(&root,"temperature"),&temperature) && temperature==0.2 &&
        cmp_json_object_bool(&root,"stream",&stream) && stream &&
        cmp_json_object_bool(&root,"include_reasoning",&reasoning) && !reasoning &&
        cmp_json_object_object(&root,"stream_options",&options) && options.field_count==1 &&
        cmp_json_object_bool(&options,"include_usage",&usage) && usage &&
        cmp_json_object_object(&root,"chat_template_kwargs",&kwargs) && kwargs.field_count==1 &&
        cmp_json_object_bool(&kwargs,"enable_thinking",&thinking) && !thinking &&
        cmp_json_field_array(cmp_json_object_field(&root,"messages"),&messages);
    int saw_user=0;
    while (valid && (rc=cmp_json_array_next(&messages,&item))==1) {
        cmp_json_object message;
        valid=++count<=2 && cmp_json_field_object(&item,&message) && message.field_count==2 &&
            cmp_json_object_str(&message,"role",role,sizeof(role)) &&
            ((!strcmp(role,"system") && count==1) || (!strcmp(role,"user") && !saw_user)) &&
            cmp_json_object_str(&message,"content",content,
                !strcmp(role,"system")?DND_GROUNDING_TEXT_CAP:DND_RAG_PROMPT_CAP) && content[0];
        if (valid && !strcmp(role,"user")) saw_user=1;
    }
    if (valid) valid=rc==0 && saw_user;
    if (content) { OPENSSL_cleanse(content,DND_RAG_PROMPT_CAP); free(content); }
    return valid;
}

static size_t decode_chunk(const char *encoded, uint8_t decoded[8196]) {
    size_t length=strlen(encoded), padding=0;
    char canonical[10925];
    if (!length || length>10924 || length%4u) return 0;
    if (encoded[length-1u]=='=') padding++;
    if (length>1 && encoded[length-2u]=='=') padding++;
    int bytes=EVP_DecodeBlock(decoded,(const unsigned char *)encoded,(int)length);
    if (bytes<0 || (size_t)bytes<=padding || (size_t)bytes-padding>VOICE_MODEL_REQUEST_CHUNK_MAX) return 0;
    size_t result=(size_t)bytes-padding;
    int canonical_len=EVP_EncodeBlock((unsigned char *)canonical,decoded,(int)result);
    int valid=canonical_len==(int)length && !memcmp(canonical,encoded,length);
    OPENSSL_cleanse(canonical,sizeof(canonical));
    return valid?result:0;
}

/* Chunks remain inside immutable recording events, so existing snapshots and
 * receipt verification cover their bytes. Only this checked projection claims
 * a prepared request signed by the voice edge. Other events remain observations. */
int loop_model_request(loop_service *s, const char *owner, const char *id, loop_response *r) {
    int result=loop_report(s,owner,id,r);
    if (!result || r->status!=200) return result;
    char receipt[65];
    int valid=cmp_json_str(r->body,"receiptSha256",receipt,sizeof(receipt));
    loop_response_free(r);
    if (!valid) return loop_reply(r,409,"{\"error\":\"recording_integrity_failed\"}");
    if (!s->gateway_secret) return loop_reply(r,503,"{\"error\":\"capture_verifier_unavailable\"}");
    sqlite3_stmt *st=NULL;
    if (sqlite3_prepare_v2(s->db,"SELECT payload FROM events WHERE owner=?1 AND turn_id=?2 ORDER BY seq",-1,&st,NULL)!=SQLITE_OK)
        return loop_store_error(s,r);
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,2,id,-1,SQLITE_TRANSIENT);
    voice_model_capture capture={0};
    char *request=NULL; size_t capacity=0;
    char nonce[33]={0},signature[65]={0},claimed_hash[65]={0};
    int64_t captured_at=0; int rc=SQLITE_DONE,found=0;
    while (valid && (rc=sqlite3_step(st))==SQLITE_ROW) {
        cmp_json_object event,payload;
        char name[81],source[32],request_id[81],type[32],protocol[32],encoded[10925];
        char row_nonce[33],row_signature[65],row_hash[65];
        uint8_t bytes[8196]; int64_t sequence,total,timestamp; int final;
        valid=loop_json_valid((const char *)sqlite3_column_text(st,0),&event) &&
            cmp_json_object_str(&event,"name",name,sizeof(name));
        if (!valid || strcmp(name,"model_request_chunk")) continue;
        found=1;
        valid=cmp_json_object_str(&event,"source",source,sizeof(source)) && !strcmp(source,"gateway_observed") &&
            cmp_json_object_object(&event,"payload",&payload) && payload.field_count==11 &&
            cmp_json_object_str(&payload,"type",type,sizeof(type)) && !strcmp(type,"model_request_chunk") &&
            cmp_json_object_str(&payload,"protocol_version",protocol,sizeof(protocol)) && !strcmp(protocol,"turnstream.v1alpha1") &&
            cmp_json_object_str(&payload,"request_id",request_id,sizeof(request_id)) && !strcmp(request_id,id) &&
            cmp_json_object_i64(&payload,"sequence",&sequence) && sequence>=0 && sequence<VOICE_MODEL_REQUEST_MAX &&
            cmp_json_object_i64(&payload,"total_bytes",&total) && total>0 && total<=VOICE_MODEL_REQUEST_MAX &&
            cmp_json_object_bool(&payload,"final",&final) &&
            cmp_json_object_str(&payload,"data",encoded,sizeof(encoded)) &&
            cmp_json_object_str(&payload,"sha256",row_hash,sizeof(row_hash)) && strlen(row_hash)==64 &&
            cmp_json_object_i64(&payload,"captured_at",&timestamp) && timestamp>0 &&
            cmp_json_object_str(&payload,"nonce",row_nonce,sizeof(row_nonce)) && strlen(row_nonce)==32 &&
            cmp_json_object_str(&payload,"signature",row_signature,sizeof(row_signature)) && strlen(row_signature)==64;
        size_t length=valid?decode_chunk(encoded,bytes):0;
        if (valid && !capture.state) {
            valid=voice_model_capture_begin(&capture,(size_t)total)==0;
            if (valid) { capacity=(size_t)total+1u; request=malloc(capacity); valid=request!=NULL; }
            memcpy(nonce,row_nonce,sizeof(nonce)); memcpy(signature,row_signature,sizeof(signature));
            memcpy(claimed_hash,row_hash,sizeof(claimed_hash)); captured_at=timestamp;
        }
        size_t offset=capture.received_bytes;
        valid=valid && length && timestamp==captured_at && !strcmp(nonce,row_nonce) &&
            !strcmp(signature,row_signature) && !strcmp(claimed_hash,row_hash) &&
            (size_t)total==capture.expected_bytes &&
            voice_model_capture_append(&capture,(uint32_t)sequence,bytes,length,final)==0;
        if (valid) memcpy(request+offset,bytes,length);
        OPENSSL_cleanse(encoded,sizeof(encoded)); OPENSSL_cleanse(bytes,sizeof(bytes));
    }
    sqlite3_finalize(st);
    if (valid && found) {
        valid=capture.state==2 && capture.received_bytes+1u==capacity &&
            !memcmp(capture.sha256,claimed_hash,65) &&
            voice_model_capture_verify(capture.sha256,s->gateway_secret,strlen(s->gateway_secret),
                owner,id,captured_at,nonce,signature)==0 && !memchr(request,0,capture.received_bytes);
        if (valid) { request[capture.received_bytes]=0; valid=request_valid(request); }
    }
    if (rc!=SQLITE_DONE && rc!=SQLITE_ROW) result=loop_store_error(s,r);
    else if (!valid) result=loop_reply(r,409,"{\"error\":\"model_request_integrity_failed\"}");
    else if (!found) result=loop_reply(r,404,"{\"error\":\"model_request_not_recorded\"}");
    else {
        size_t encoded_capacity=4u*((capture.received_bytes+2u)/3u)+1u;
        char *encoded=malloc(encoded_capacity);
        if (!encoded) { result=loop_store_error(s,r); goto cleanup; }
        EVP_EncodeBlock((unsigned char *)encoded,(unsigned char *)request,(int)capture.received_bytes);
        char *json=sqlite3_mprintf("{\"schemaVersion\":\"loop-prepared-model-request/v1\",\"requestId\":\"%s\","
            "\"scope\":\"prepared_request_not_model_attestation\",\"receiptSha256\":\"%s\","
            "\"requestSha256\":\"%s\",\"bytes\":%llu,\"capturedAt\":%lld,\"nonce\":\"%s\","
            "\"signature\":\"%s\",\"requestBase64\":\"%s\",\"request\":%s,\"promotionDecision\":null}",
            id,receipt,capture.sha256,(unsigned long long)capture.received_bytes,
            (long long)captured_at,nonce,signature,encoded,request);
        result=json?loop_reply(r,200,json):loop_store_error(s,r); sqlite3_free(json);
        OPENSSL_cleanse(encoded,encoded_capacity); free(encoded);
    }
cleanup:
    if (request) { OPENSSL_cleanse(request,capacity); free(request); }
    voice_model_capture_destroy(&capture);
    return result;
}
