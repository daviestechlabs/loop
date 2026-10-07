#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int invalid(loop_response *r) { return loop_reply(r,400,"{\"error\":\"invalid_anvil_reference\"}"); }
static int prepare(loop_service *s, sqlite3_stmt **st, const char *sql, const char *owner, const char *id) {
    if (sqlite3_prepare_v2(s->db,sql,-1,st,NULL)!=SQLITE_OK) return 0;
    sqlite3_bind_text(*st,1,owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(*st,2,id,-1,SQLITE_TRANSIENT); return 1;
}
static int hex(const char *value, size_t size) {
    if (strlen(value)!=size) return 0;
    for(size_t i=0;i<size;i++) if (!((value[i]>='0' && value[i]<='9') || (value[i]>='a' && value[i]<='f'))) return 0;
    return 1;
}
static int uuid(const char *value) {
    if (strlen(value)!=36 || value[14]!='4' || !strchr("89ab",value[19])) return 0;
    char compact[33]; size_t n=0;
    for(size_t i=0;i<36;i++) {
        if (i==8 || i==13 || i==18 || i==23) { if (value[i]!='-') return 0; }
        else compact[n++]=value[i];
    }
    compact[n]=0; return hex(compact,32);
}
static int links(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r) {
    if (strcmp(q->method,"GET") && strcmp(q->method,"POST")) return loop_reply(r,405,"{\"error\":\"method_not_allowed\"}");
    /* A link binds checked local bytes. It never attests the remote object. */
    int result=loop_report(s,owner,id,r);
    if (!result || r->status!=200) return result;
    char receipt[65]; int valid=cmp_json_str(r->body,"receiptSha256",receipt,sizeof(receipt));
    loop_response_free(r); if (!valid) return loop_store_error(s,r);
    sqlite3_stmt *st=NULL;
    if (!strcmp(q->method,"GET")) {
        if (!prepare(s,&st,"SELECT definition,sha FROM anvil_links WHERE owner=?1 AND turn_id=?2",owner,id)) return loop_store_error(s,r);
        int rc,count=0;
        while ((rc=sqlite3_step(st))==SQLITE_ROW) {
            const char *body=(const char *)sqlite3_column_text(st,0), *saved=(const char *)sqlite3_column_text(st,1);
            char actual[65],bound[65]; cmp_json_object object;
            if (++count>32 || !body || !saved || !loop_hash(body,(size_t)sqlite3_column_bytes(st,0),actual) || strcmp(actual,saved) ||
                !loop_json_valid(body,&object) || !cmp_json_object_str(&object,"receipt_sha256",bound,sizeof(bound)) || strcmp(bound,receipt)) {
                sqlite3_finalize(st); return loop_reply(r,409,"{\"error\":\"reference_integrity_failed\"}");
            }
        }
        sqlite3_finalize(st); if (rc!=SQLITE_DONE) return loop_store_error(s,r);
        if (!prepare(s,&st,"SELECT json_object('schemaVersion','loop-anvil-links/v1','origin','operator_annotation',"
            "'remoteVerification','not_verified','promotionDecision',NULL,'references',json_group_array(json(item))) "
            "FROM (SELECT json_object('reference',json(definition),'sha256',sha,'created',created) item "
            "FROM anvil_links WHERE owner=?1 AND turn_id=?2 ORDER BY created,id)",owner,id)) return loop_store_error(s,r);
        if (sqlite3_step(st)!=SQLITE_ROW) { sqlite3_finalize(st); return loop_store_error(s,r); }
        const char *json=(const char *)sqlite3_column_text(st,0);
        result=json ? loop_reply(r,200,json) : loop_store_error(s,r); sqlite3_finalize(st); return result;
    }
    cmp_json_object object;
    char link_id[81],kind[32],reference[65],expected[65],digest[65];
    if (!q->content_type || strcmp(q->content_type,"application/json") || !q->body || q->body_len>2048 ||
        memchr(q->body,0,q->body_len) || !loop_json_valid((const char *)q->body,&object) || object.field_count!=4 ||
        !cmp_json_object_str(&object,"id",link_id,sizeof(link_id)) || !loop_id_valid(link_id) ||
        !cmp_json_object_str(&object,"kind",kind,sizeof(kind)) ||
        !cmp_json_object_str(&object,"reference",reference,sizeof(reference)) ||
        !cmp_json_object_str(&object,"receipt_sha256",expected,sizeof(expected)) || !hex(expected,64) ||
        !loop_hash(q->body,q->body_len,digest)) return invalid(r);
    if (!strcmp(kind,"learning_report") || !strcmp(kind,"answer_review")) {
        /* Anvil answer review is addressed by its learning report hash. */
        if (!hex(reference,64)) return invalid(r);
    } else if (!strcmp(kind,"experiment") || !strcmp(kind,"native_evaluation")) { if (!uuid(reference)) return invalid(r); }
    else return invalid(r);
    if (strcmp(receipt,expected)) return loop_reply(r,409,"{\"error\":\"source_receipt_mismatch\"}");
    if (!prepare(s,&st,"SELECT sha FROM anvil_links WHERE owner=?1 AND turn_id=?2 AND id=?3",owner,id)) return loop_store_error(s,r);
    sqlite3_bind_text(st,3,link_id,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) {
        const char *old=(const char *)sqlite3_column_text(st,0);
        int same=old && !strcmp(old,digest); sqlite3_finalize(st);
        return loop_reply(r,same?200:409,same?"{\"stored\":true,\"remoteVerification\":\"not_verified\"}":"{\"error\":\"immutable_reference_conflict\"}");
    }
    sqlite3_finalize(st); if (rc!=SQLITE_DONE) return loop_store_error(s,r);
    if (!prepare(s,&st,"SELECT COUNT(*) FROM anvil_links WHERE owner=?1 AND turn_id=?2",owner,id)) return loop_store_error(s,r);
    rc=sqlite3_step(st); int full=rc==SQLITE_ROW && sqlite3_column_int(st,0)>=32;
    sqlite3_finalize(st); if (rc!=SQLITE_ROW) return loop_store_error(s,r);
    if (full) return loop_reply(r,413,"{\"error\":\"reference_capacity\"}");
    if (!prepare(s,&st,"INSERT INTO anvil_links(owner,turn_id,id,definition,sha) VALUES(?1,?2,?3,?4,?5)",owner,id)) return loop_store_error(s,r);
    sqlite3_bind_text(st,3,link_id,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,4,(const char *)q->body,(int)q->body_len,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,5,digest,-1,SQLITE_TRANSIENT); rc=sqlite3_step(st); sqlite3_finalize(st);
    return rc==SQLITE_DONE ? loop_reply(r,201,"{\"stored\":true,\"remoteVerification\":\"not_verified\"}") : loop_store_error(s,r);
}

int loop_anvil_links(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r) {
    if (sqlite3_exec(s->db,"SAVEPOINT loop_links_snapshot",NULL,NULL,NULL)!=SQLITE_OK) return loop_store_error(s,r);
    int result=links(s,owner,id,q,r);
    /* SQLITE_FULL can roll back the savepoint itself. Preserve that failure. */
    if (r->status==507 && sqlite3_get_autocommit(s->db)) return result;
    if (sqlite3_exec(s->db,"RELEASE loop_links_snapshot",NULL,NULL,NULL)!=SQLITE_OK) {
        int full=(sqlite3_errcode(s->db)&0xff)==SQLITE_FULL;
        sqlite3_exec(s->db,"ROLLBACK TO loop_links_snapshot; RELEASE loop_links_snapshot",NULL,NULL,NULL);
        loop_response_free(r);
        return full?loop_reply(r,507,"{\"error\":\"evidence_store_full\"}"):loop_store_error(s,r);
    }
    return result;
}
