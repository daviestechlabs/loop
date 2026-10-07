#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unavailable(loop_response *r) { return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}"); }
static int hex(const char *s, size_t length) {
    if (strlen(s)!=length) return 0;
    for(size_t i=0;i<length;i++) if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
    return 1;
}
static int uuid(const char *s) {
    if (strlen(s)!=36 || s[14]!='4' || !strchr("89ab",s[19])) return 0;
    char digits[33]; size_t n=0;
    for(size_t i=0;i<36;i++) {
        if (i==8 || i==13 || i==18 || i==23) { if(s[i]!='-') return 0; }
        else digits[n++]=s[i];
    }
    digits[n]=0; return hex(digits,32);
}
int loop_anvil_export(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r) {
    cmp_json_object input; char run[37],revision[41],expected[65]; int64_t sequence;
    if (!q->content_type || strcmp(q->content_type,"application/json") || !q->body || q->body_len>1024 ||
        !loop_json_valid((const char *)q->body,&input) || input.field_count!=4 ||
        !cmp_json_object_str(&input,"run_id",run,sizeof(run)) || !uuid(run) ||
        !cmp_json_object_str(&input,"source_revision",revision,sizeof(revision)) || !hex(revision,40) ||
        !cmp_json_object_str(&input,"receipt_sha256",expected,sizeof(expected)) || !hex(expected,64) ||
        !cmp_json_object_i64(&input,"sequence",&sequence) || sequence<1 || sequence>1000000)
        return loop_reply(r,400,"{\"error\":\"invalid_anvil_export_binding\"}");
    int result=loop_report(s,owner,id,r);
    if (!result || r->status!=200) return result;
    char receipt[65],report_hash[65];
    if (!cmp_json_str(r->body,"receiptSha256",receipt,sizeof(receipt)) || !loop_hash(r->body,r->size,report_hash)) {
        loop_response_free(r); return unavailable(r);
    }
    if (strcmp(receipt,expected)) { loop_response_free(r); return loop_reply(r,409,"{\"error\":\"source_receipt_mismatch\"}"); }
    /* This is an export, not an Anvil run, verdict, or import operation. All data
     * comes from the checked report snapshot; caller-supplied bindings stay explicit. */
    const char *sql="SELECT json_object('schemaVersion','anvil-learning-report/v1',"
        "'goalId','cohesive-homelab-models-v1','trackId','cohesion-speed','runId',?2,'sequence',?3,"
        "'recordedAt',strftime('%Y-%m-%dT%H:%M:%SZ',json_extract(?1,'$.receipt.created'),'unixepoch'),"
        "'title','Loop browser evidence: '||json_extract(?1,'$.receipt.turnId'),"
        "'status','held','scope','development',"
        "'summary','Loop recording status: '||json_extract(?1,'$.receipt.status')||"
        "'. Metrics are browser observations. Run ID and source revision are operator-supplied and not independently verified.',"
        "'sourceRevision',?4,'progress',NULL,"
        "'metrics',json((SELECT json_group_array(json_object('name','loop_browser_'||json_extract(value,'$.name'),"
        "'value',json_extract(value,'$.value'),'unit','ms','direction','lower')) FROM json_each(?1,'$.measurements') "
        "WHERE json_type(value,'$.value') IN ('integer','real'))),"
        "'blockers',json_array('Model revision and Anvil run binding require independent verification.',"
        "'Browser observations do not prove human acceptance or acoustic output.'),"
        "'evidence',json_array(json_object('name','loop-'||json_extract(?1,'$.receipt.turnId')||'-receipt.json','sha256',?5),"
        "json_object('name','loop-'||json_extract(?1,'$.receipt.turnId')||'-report.json','sha256',?6)))";
    sqlite3_stmt *st=NULL;
    if (sqlite3_prepare_v2(s->db,sql,-1,&st,NULL)!=SQLITE_OK) { loop_response_free(r); return unavailable(r); }
    sqlite3_bind_text(st,1,r->body,(int)r->size,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,run,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(st,3,sequence);
    sqlite3_bind_text(st,4,revision,-1,SQLITE_TRANSIENT); sqlite3_bind_text(st,5,receipt,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,6,report_hash,-1,SQLITE_TRANSIENT);
    loop_response_free(r);
    if (sqlite3_step(st)!=SQLITE_ROW || !sqlite3_column_text(st,0)) { sqlite3_finalize(st); return unavailable(r); }
    result=loop_reply(r,200,(const char *)sqlite3_column_text(st,0)); sqlite3_finalize(st);
    snprintf(r->headers,sizeof(r->headers),"Content-Disposition: attachment; filename=\"loop-%s-anvil.json\"\r\n",id);
    return result;
}
