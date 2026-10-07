#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int prepare(loop_service *s, sqlite3_stmt **st, const char *sql, const char *owner, const char *id) {
    if (sqlite3_prepare_v2(s->db,sql,-1,st,NULL)!=SQLITE_OK) return 0;
    sqlite3_bind_text(*st,1,owner,-1,SQLITE_TRANSIENT); sqlite3_bind_text(*st,2,id,-1,SQLITE_TRANSIENT); return 1;
}
static int rerun(loop_service *s, const char *owner, const char *source, const loop_request *q, loop_response *r) {
    cmp_json_object object; char id[81],mode[40]; int model_consent=0;
    if (!q->body || q->body_len>512 || !q->content_type || strcmp(q->content_type,"application/json") ||
        memchr(q->body,0,q->body_len) || !loop_json_valid((const char *)q->body,&object) || (object.field_count!=2 && object.field_count!=3) ||
        !cmp_json_object_str(&object,"request_id",id,sizeof(id)) || !loop_id_valid(id) || !strcmp(source,id) ||
        !cmp_json_object_str(&object,"mode",mode,sizeof(mode)) ||
        (object.field_count==3 && !cmp_json_object_bool(&object,"model_request_consent",&model_consent))) return loop_reply(r,400,"{\"error\":\"invalid_rerun\"}");
    if (strcmp(mode,"full_system_active_model")) return loop_reply(r,400,"{\"error\":\"unsupported_rerun_mode\"}");
    int result=loop_report(s,owner,source,r);
    if (!result || r->status!=200) return result;
    char receipt[65]; int valid=cmp_json_str(r->body,"receiptSha256",receipt,sizeof(receipt));
    loop_response_free(r); if (!valid) return loop_store_error(s,r);
    sqlite3_stmt *st=NULL;
    if (!prepare(s,&st,"SELECT sample_rate,length(pcm),sha FROM audio WHERE owner=?1 AND turn_id=?2 AND kind='input'",owner,source)) return loop_store_error(s,r);
    int rc=sqlite3_step(st);
    if (rc!=SQLITE_ROW) { sqlite3_finalize(st); return rc==SQLITE_DONE?loop_reply(r,409,"{\"error\":\"rerun_input_not_recorded\"}"):loop_store_error(s,r); }
    int rate=sqlite3_column_int(st,0),bytes=sqlite3_column_int(st,1);
    char pcm_hash[65]; snprintf(pcm_hash,sizeof(pcm_hash),"%s",sqlite3_column_text(st,2));
    sqlite3_finalize(st);
    if (rate!=16000 || bytes<=0 || bytes>960000 || bytes%640) return loop_reply(r,409,"{\"error\":\"rerun_input_format_unsupported\"}");
    char manifest[1536],digest[65];
    snprintf(manifest,sizeof(manifest),"{\"request_id\":\"%s\",\"recording_consent\":true,%s\"purpose\":\"saved_input_rerun\","
        "\"rerun\":{\"mode\":\"full_system_active_model\",\"source_turn\":\"%s\",\"source_receipt_sha256\":\"%s\","
        "\"input_sha256\":\"%s\",\"input_bytes\":%d,\"sample_rate\":16000,"
        "\"model_revision\":null,\"tool_policy\":\"live_gateway_policy\",\"microphone_recaptured\":false}}",id,
        model_consent?"\"model_request_consent\":true,":"",source,receipt,pcm_hash,bytes);
    if (!loop_hash(manifest,strlen(manifest),digest)) return loop_store_error(s,r);
    if (!prepare(s,&st,"SELECT manifest_sha,status FROM turns WHERE owner=?1 AND id=?2",owner,id)) return loop_store_error(s,r);
    rc=sqlite3_step(st); int exists=rc==SQLITE_ROW;
    int same=exists && !strcmp((const char *)sqlite3_column_text(st,0),digest);
    int recording=exists && !strcmp((const char *)sqlite3_column_text(st,1),"recording");
    sqlite3_finalize(st);
    if (rc!=SQLITE_DONE && !exists) return loop_store_error(s,r);
    if (exists && (!same || !recording)) return loop_reply(r,409,"{\"error\":\"rerun_id_conflict\"}");
    if (!exists) {
        if (!prepare(s,&st,"INSERT INTO turns(owner,id,manifest,manifest_sha) VALUES(?1,?2,?3,?4)",owner,id)) return loop_store_error(s,r);
        sqlite3_bind_text(st,3,manifest,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,4,digest,-1,SQLITE_TRANSIENT);
        rc=sqlite3_step(st); sqlite3_finalize(st); if (rc!=SQLITE_DONE) return loop_store_error(s,r);
    }
    char *json=sqlite3_mprintf("{\"request_id\":\"%s\",\"manifest\":%s,\"source_audio\":{\"path\":\"/api/turns/%s/replay/input.wav\","
        "\"sha256\":\"%s\",\"bytes\":%d,\"sample_rate\":16000},\"execution\":\"pending_browser_transport\",\"promotionDecision\":null}",id,manifest,source,pcm_hash,bytes);
    if (!json) return loop_store_error(s,r);
    result=loop_reply(r,exists?200:201,json); sqlite3_free(json); return result;
}
int loop_rerun(loop_service *s, const char *owner, const char *source, const loop_request *q, loop_response *r) {
    if (sqlite3_exec(s->db,"SAVEPOINT loop_rerun_snapshot",NULL,NULL,NULL)!=SQLITE_OK) return loop_store_error(s,r);
    int result=rerun(s,owner,source,q,r);
    /* SQLITE_FULL can roll back the savepoint itself. Preserve that failure. */
    if (r->status==507 && sqlite3_get_autocommit(s->db)) return result;
    if (sqlite3_exec(s->db,"RELEASE loop_rerun_snapshot",NULL,NULL,NULL)!=SQLITE_OK) {
        int full=(sqlite3_errcode(s->db)&0xff)==SQLITE_FULL;
        sqlite3_exec(s->db,"ROLLBACK TO loop_rerun_snapshot; RELEASE loop_rerun_snapshot",NULL,NULL,NULL);
        loop_response_free(r);
        return full?loop_reply(r,507,"{\"error\":\"evidence_store_full\"}"):loop_store_error(s,r);
    }
    return result;
}

/* Accept only the bound source prefix for a stopped rerun, and all bytes for a
 * completed rerun. This checks stored input, not gateway receipt or model use. */
int loop_rerun_input_check(loop_service *s, const char *owner, const char *id,
                           const void *pcm, size_t bytes, int rate, int complete, loop_response *r) {
    sqlite3_stmt *st=NULL;
    if (!prepare(s,&st,"SELECT manifest,manifest_sha FROM turns WHERE owner=?1 AND id=?2",owner,id)) {
        loop_store_error(s,r); return 0;
    }
    int rc=sqlite3_step(st);
    cmp_json_object manifest,binding; char digest[65],source[81],expected[65],mode[40]; int64_t expected_bytes=0,expected_rate=0;
    int valid=rc==SQLITE_ROW && loop_hash(sqlite3_column_text(st,0),(size_t)sqlite3_column_bytes(st,0),digest) &&
        !strcmp(digest,(const char *)sqlite3_column_text(st,1)) &&
        loop_json_valid((const char *)sqlite3_column_text(st,0),&manifest);
    int bound=valid && cmp_json_object_field(&manifest,"rerun")!=NULL;
    if (bound) valid=cmp_json_object_object(&manifest,"rerun",&binding) &&
        cmp_json_object_str(&binding,"source_turn",source,sizeof(source)) && loop_id_valid(source) && strcmp(source,id) &&
        cmp_json_object_str(&binding,"mode",mode,sizeof(mode)) && !strcmp(mode,"full_system_active_model") &&
        cmp_json_object_str(&binding,"input_sha256",expected,sizeof(expected)) && strlen(expected)==64 &&
        cmp_json_object_i64(&binding,"input_bytes",&expected_bytes) && expected_bytes>0 && expected_bytes<=960000 && expected_bytes%640==0 &&
        cmp_json_object_i64(&binding,"sample_rate",&expected_rate) && expected_rate==16000;
    sqlite3_finalize(st);
    if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) { loop_store_error(s,r); return 0; }
    if (!valid) { loop_reply(r,409,"{\"error\":\"rerun_source_integrity_failed\"}"); return 0; }
    if (!bound) return 1;
    if (!prepare(s,&st,"SELECT a.pcm,a.sha,a.sample_rate,t.status FROM audio a JOIN turns t ON t.owner=a.owner AND t.id=a.turn_id "
        "WHERE a.owner=?1 AND a.turn_id=?2 AND a.kind='input'",owner,source)) { loop_store_error(s,r); return 0; }
    rc=sqlite3_step(st);
    valid=rc==SQLITE_ROW && sqlite3_column_bytes(st,0)==expected_bytes && sqlite3_column_int(st,2)==16000 &&
        strcmp((const char *)sqlite3_column_text(st,3),"recording") &&
        loop_hash(sqlite3_column_blob(st,0),(size_t)expected_bytes,digest) && !strcmp(digest,expected) &&
        !strcmp(digest,(const char *)sqlite3_column_text(st,1));
    sqlite3_stmt *input=NULL;
    if (valid && complete) {
        if (!prepare(s,&input,"SELECT pcm,sample_rate FROM audio WHERE owner=?1 AND turn_id=?2 AND kind='input'",owner,id)) {
            sqlite3_finalize(st); loop_store_error(s,r); return 0;
        }
        int input_rc=sqlite3_step(input);
        if (input_rc!=SQLITE_ROW && input_rc!=SQLITE_DONE) {
            sqlite3_finalize(input); sqlite3_finalize(st); loop_store_error(s,r); return 0;
        }
        pcm=input_rc==SQLITE_ROW?sqlite3_column_blob(input,0):NULL;
        bytes=input_rc==SQLITE_ROW?(size_t)sqlite3_column_bytes(input,0):0;
        rate=input_rc==SQLITE_ROW?sqlite3_column_int(input,1):0;
    }
    int matches=valid && pcm && rate==16000 && bytes>0 && bytes%640==0 && bytes<=(size_t)expected_bytes &&
        (!complete || bytes==(size_t)expected_bytes) && !memcmp(pcm,sqlite3_column_blob(st,0),bytes);
    sqlite3_finalize(input); sqlite3_finalize(st);
    if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) { loop_store_error(s,r); return 0; }
    if (!valid) { loop_reply(r,409,"{\"error\":\"rerun_source_integrity_failed\"}"); return 0; }
    if (!matches) { loop_reply(r,409,complete?"{\"error\":\"rerun_input_incomplete\"}":"{\"error\":\"rerun_input_mismatch\"}"); return 0; }
    return 1;
}
