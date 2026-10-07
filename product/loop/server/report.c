#include "loop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This report verifies bytes and derives browser intervals. It does not attest
 * the browser's account of the gateway, model revision, or acoustic output. */
typedef struct { const char *name; double elapsed; int found; int64_t seq; } milestone;
typedef struct { int count, valid; int64_t seq, packets, bytes, lost; } delivery_observation;
static void delivery_event(delivery_observation *out, const cmp_json_object *event, int64_t seq, int gateway) {
    out->count++;
    if (out->count!=1) { out->valid=0; return; }
    out->seq=seq;
    cmp_json_object payload;
    out->valid=cmp_json_object_object(event,"payload",&payload) &&
        cmp_json_object_i64(&payload,gateway?"audio_datagrams":"packets",&out->packets) &&
        cmp_json_object_i64(&payload,gateway?"audio_bytes":"bytes",&out->bytes) &&
        (!gateway || cmp_json_object_i64(&payload,"lost_datagrams",&out->lost)) &&
        out->packets>=0 && out->packets<=LOOP_AUDIO_MAX/640 &&
        out->bytes>=0 && out->bytes<=LOOP_AUDIO_MAX && out->lost>=0 && out->lost<=LOOP_AUDIO_MAX/640;
}
static char *delivery_report(const delivery_observation *sent, const delivery_observation *ack, int bytes, int rate) {
    const char *reason=NULL;
    if (sent->count>1 || ack->count>1) reason="duplicate_delivery_observation";
    else if ((sent->count && !sent->valid) || (ack->count && !ack->valid)) reason="invalid_delivery_counters";
    else if (!bytes) reason="input_audio_not_recorded";
    else if (rate!=16000 || bytes%640) reason="unsupported_input_framing";
    else if (!sent->count) reason="input_end_not_observed";
    else if (!ack->count) reason="input_commit_not_observed";
    else if (ack->seq<=sent->seq) reason="invalid_delivery_observation_order";
    else if (ack->lost) reason="gateway_reported_lost_datagrams";
    else if (sent->bytes!=bytes || ack->bytes!=bytes || sent->packets!=bytes/640 || ack->packets!=bytes/640)
        reason="delivery_counts_do_not_match_recording";
    char captured[32]="null", sample_rate[32]="null", sent_bytes[32]="null", sent_packets[32]="null";
    char ack_bytes[32]="null", ack_packets[32]="null", lost[32]="null", why[96]="null";
    if (bytes) { snprintf(captured,sizeof(captured),"%d",bytes); snprintf(sample_rate,sizeof(sample_rate),"%d",rate); }
    if (sent->valid) {
        snprintf(sent_bytes,sizeof(sent_bytes),"%lld",(long long)sent->bytes);
        snprintf(sent_packets,sizeof(sent_packets),"%lld",(long long)sent->packets);
    }
    if (ack->valid) {
        snprintf(ack_bytes,sizeof(ack_bytes),"%lld",(long long)ack->bytes);
        snprintf(ack_packets,sizeof(ack_packets),"%lld",(long long)ack->packets);
        snprintf(lost,sizeof(lost),"%lld",(long long)ack->lost);
    }
    if (reason) snprintf(why,sizeof(why),"\"%s\"",reason);
    return sqlite3_mprintf("{\"status\":\"%s\",\"unavailableReason\":%s,\"evidenceOrigin\":\"browser_observed\","
        "\"serverAttested\":false,\"recordedBytes\":%s,\"sampleRate\":%s,\"browserSentBytes\":%s,"
        "\"browserSentPackets\":%s,\"gatewayReportedBytes\":%s,\"gatewayReportedPackets\":%s,\"gatewayReportedLostDatagrams\":%s}",
        reason?"not_established":"matched_browser_observations",why,captured,sample_rate,sent_bytes,sent_packets,ack_bytes,ack_packets,lost);
}
/* Output counters describe decoded browser PCM, not acoustic playback. */
typedef struct { int count, valid; int64_t seq, bytes, packets, rate; } response_observation;
static void response_event(response_observation *out, const cmp_json_object *event, int64_t seq) {
    out->count++;
    if (out->count!=1) { out->valid=0; return; }
    out->seq=seq;
    cmp_json_object payload;
    out->valid=cmp_json_object_object(event,"payload",&payload) &&
        cmp_json_object_i64(&payload,"bytes",&out->bytes) &&
        cmp_json_object_i64(&payload,"packets",&out->packets) &&
        cmp_json_object_i64(&payload,"sample_rate",&out->rate) &&
        out->bytes>0 && out->bytes<=LOOP_AUDIO_MAX && out->bytes%2==0 &&
        out->packets>0 && out->packets<=out->bytes/2 && out->bytes<=out->packets*16384 &&
        (out->rate==16000 || out->rate==22050 || out->rate==24000 || out->rate==44100 || out->rate==48000);
}
static char *response_report(const response_observation *received, const milestone *completed, int bytes, int rate) {
    const char *reason=NULL;
    if (received->count>1 || completed->found>1) reason="duplicate_response_observation";
    else if (received->count && !received->valid) reason="invalid_response_counters";
    else if (!bytes) reason="output_audio_not_recorded";
    else if (!received->count) reason="output_completion_not_observed";
    else if (!completed->found) reason="gateway_completion_not_observed";
    else if (received->seq<=completed->seq) reason="invalid_response_observation_order";
    else if (received->bytes!=bytes || received->rate!=rate) reason="response_counts_do_not_match_recording";
    char recorded[32]="null", sample_rate[32]="null", duration[48]="null";
    char received_bytes[32]="null", packets[32]="null", received_rate[32]="null", why[96]="null";
    if (bytes) {
        snprintf(recorded,sizeof(recorded),"%d",bytes); snprintf(sample_rate,sizeof(sample_rate),"%d",rate);
        snprintf(duration,sizeof(duration),"%.3f",(double)bytes*1000.0/(2.0*rate));
    }
    if (received->valid) {
        snprintf(received_bytes,sizeof(received_bytes),"%lld",(long long)received->bytes);
        snprintf(packets,sizeof(packets),"%lld",(long long)received->packets);
        snprintf(received_rate,sizeof(received_rate),"%lld",(long long)received->rate);
    }
    if (reason) snprintf(why,sizeof(why),"\"%s\"",reason);
    return sqlite3_mprintf("{\"status\":\"%s\",\"unavailableReason\":%s,\"evidenceOrigin\":\"browser_observed\","
        "\"serverAttested\":false,\"acousticOutputVerified\":false,\"recordedBytes\":%s,\"sampleRate\":%s,\"pcmDurationMs\":%s,"
        "\"browserReceivedBytes\":%s,\"browserReceivedPackets\":%s,\"browserReceivedSampleRate\":%s}",
        reason?"not_established":"matched_browser_observations",why,recorded,sample_rate,duration,received_bytes,packets,received_rate);
}
static int prepare(sqlite3 *db, sqlite3_stmt **st, const char *sql, const char *owner, const char *id) {
    if (sqlite3_prepare_v2(db, sql, -1, st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(*st, 1, owner, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(*st, 2, id, -1, SQLITE_TRANSIENT);
    return 1;
}
static int digest_matches(const void *body, int len, const char *digest) {
    char computed[65];
    return body && len >= 0 && digest && strlen(digest) == 64 && loop_hash(body, (size_t)len, computed) && !strcmp(computed, digest);
}
static int bad_integrity(loop_response *r) { return loop_reply(r, 409, "{\"error\":\"recording_integrity_failed\"}"); }
static int unavailable(loop_response *r) { return loop_reply(r, 503, "{\"error\":\"evidence_store_unavailable\"}"); }
static int report(loop_service *s, const char *owner, const char *id, loop_response *r) {
    sqlite3_stmt *st = NULL;
    if (!prepare(s->db, &st, "SELECT manifest,manifest_sha,final,final_sha,status FROM turns WHERE owner=?1 AND id=?2", owner, id)) return unavailable(r);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return rc == SQLITE_DONE ? loop_reply(r, 404, "{\"error\":\"turn_not_found\"}") : unavailable(r); }
    const char *status = (const char *)sqlite3_column_text(st, 4);
    if (!strcmp(status, "recording")) { sqlite3_finalize(st); return loop_reply(r, 409, "{\"error\":\"recording_not_final\"}"); }
    const char *manifest = (const char *)sqlite3_column_text(st, 0);
    const char *final = (const char *)sqlite3_column_text(st, 2);
    cmp_json_object object; int64_t expected_events = -1; char final_status[32], request_id[81];
    int valid = digest_matches(manifest, sqlite3_column_bytes(st, 0), (const char *)sqlite3_column_text(st, 1)) &&
        digest_matches(final, sqlite3_column_bytes(st, 2), (const char *)sqlite3_column_text(st, 3)) &&
        loop_json_valid(manifest, &object) && cmp_json_object_str(&object, "request_id", request_id, sizeof(request_id)) && !strcmp(request_id, id) &&
        loop_json_valid(final, &object) && cmp_json_object_i64(&object, "event_count", &expected_events) &&
        cmp_json_object_str(&object, "status", final_status, sizeof(final_status)) && !strcmp(final_status, status) &&
        (!strcmp(status,"completed") || !strcmp(status,"failed") || !strcmp(status,"interrupted"));
    sqlite3_finalize(st);
    if (!valid) return bad_integrity(r);
    milestone milestones[] = {{"connecting",0,0,0},{"listening",0,0,0},{"committing",0,0,0},
        {"first_text_received",0,0,0},{"first_audio_received",0,0,0},{"first_playback_scheduled",0,0,0},
        {"interrupt_requested",0,0,0},{"playback_stopped",0,0,0},{"playback_finished",0,0,0}};
    if (!prepare(s->db, &st, "SELECT seq,payload,sha FROM events WHERE owner=?1 AND turn_id=?2 ORDER BY seq", owner, id)) return unavailable(r);
    int64_t count = 0; double previous = 0;
    delivery_observation sent={0}, ack={0};
    response_observation received={0}; milestone completed={0};
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *body = (const char *)sqlite3_column_text(st, 1);
        char name[81], source[32]; int64_t seq; double elapsed;
        if (count >= LOOP_EVENT_MAX || sqlite3_column_int64(st, 0) != count ||
            !digest_matches(body, sqlite3_column_bytes(st, 1), (const char *)sqlite3_column_text(st, 2)) ||
            !loop_json_valid(body, &object) || !cmp_json_object_i64(&object,"seq",&seq) || seq != count ||
            !cmp_json_object_str(&object,"name",name,sizeof(name)) || !loop_id_valid(name) ||
            !cmp_json_object_str(&object,"source",source,sizeof(source)) ||
            (strcmp(source,"browser") && strcmp(source,"gateway_observed")) ||
            !cmp_json_field_double(cmp_json_object_field(&object,"elapsed_ms"),&elapsed) ||
            !isfinite(elapsed) || elapsed < previous || elapsed > 600000) { sqlite3_finalize(st); return bad_integrity(r); }
        previous = elapsed; count++;
        if (!strcmp(source,"browser") && !strcmp(name,"input_end_sent")) delivery_event(&sent,&object,seq,0);
        if (!strcmp(source,"gateway_observed") && !strcmp(name,"input_committed")) delivery_event(&ack,&object,seq,1);
        if (!strcmp(source,"gateway_observed") && !strcmp(name,"completed")) { completed.found++; completed.seq=seq; }
        if (!strcmp(source,"browser") && !strcmp(name,"output_received_complete")) response_event(&received,&object,seq);
        if (strcmp(source,"browser")) continue;
        for (size_t i=0;i<sizeof(milestones)/sizeof(milestones[0]);i++)
            if (!strcmp(name,milestones[i].name)) {
                milestones[i].found++; milestones[i].elapsed=elapsed; milestones[i].seq=seq;
            }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return unavailable(r);
    if (count != expected_events) return bad_integrity(r);
    if (!prepare(s->db, &st, "SELECT kind,sample_rate,pcm,sha FROM audio WHERE owner=?1 AND turn_id=?2 ORDER BY kind", owner, id)) return unavailable(r);
    int input_bytes=0, input_rate=0, output_bytes=0, output_rate=0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *kind = (const char *)sqlite3_column_text(st,0); int rate=sqlite3_column_int(st,1), len=sqlite3_column_bytes(st,2);
        if (!kind || (strcmp(kind,"input") && strcmp(kind,"output")) ||
            (rate!=16000 && rate!=22050 && rate!=24000 && rate!=44100 && rate!=48000) ||
            len<=0 || (unsigned)len>LOOP_AUDIO_MAX || len%2 ||
            !digest_matches(sqlite3_column_blob(st,2),len,(const char *)sqlite3_column_text(st,3))) { sqlite3_finalize(st); return bad_integrity(r); }
        if (!strcmp(kind,"input")) { input_bytes=len; input_rate=rate; }
        else { output_bytes=len; output_rate=rate; }
    }
    sqlite3_finalize(st); if (rc != SQLITE_DONE) return unavailable(r);
    /* SQLite emits this fixed-key receipt in a stable order. Its exact UTF-8
     * bytes are hashed; array order is defined explicitly, never row order. */
    const char *sql =
        "SELECT json_object('schemaVersion','loop-evidence-receipt/v1','turnId',id,'created',created,'status',status,"
        "'manifestSha256',manifest_sha,'finalSha256',final_sha,"
        "'events',json((SELECT json_group_array(json(item)) FROM (SELECT json_object('sequence',seq,'sha256',sha) item FROM events WHERE owner=?1 AND turn_id=?2 ORDER BY seq))),"
        "'audio',json((SELECT json_group_array(json(item)) FROM (SELECT json_object('kind',kind,'sampleRate',sample_rate,'bytes',length(pcm),'sha256',sha) item FROM audio WHERE owner=?1 AND turn_id=?2 ORDER BY kind)))) "
        "FROM turns WHERE owner=?1 AND id=?2";
    if (!prepare(s->db,&st,sql,owner,id)) return unavailable(r);
    if (sqlite3_step(st)!=SQLITE_ROW) { sqlite3_finalize(st); return unavailable(r); }
    const char *receipt=(const char *)sqlite3_column_text(st,0); char digest[65];
    if (!receipt || !loop_hash(receipt,strlen(receipt),digest)) { sqlite3_finalize(st); return unavailable(r); }
    char *copy=strdup(receipt); sqlite3_finalize(st); if(!copy) return unavailable(r);
    struct { const char *name; size_t start,end; } pairs[] = {
        {"connection_to_listening",0,1},{"release_to_first_text",2,3},{"release_to_first_audio_received",2,4},
        {"release_to_playback_scheduled",2,5},{"interrupt_to_playback_stopped",6,7},{"release_to_playback_finished",2,8},
        {"first_text_to_first_audio_received",3,4},{"first_audio_to_playback_scheduled",4,5}
    };
    char measurements[4096]; size_t used=0;
    for(size_t i=0;i<sizeof(pairs)/sizeof(pairs[0]);i++) {
        milestone *a=&milestones[pairs[i].start], *b=&milestones[pairs[i].end];
        char value[48]; const char *reason="null";
        if(a->found>1 || b->found>1) { strcpy(value,"null"); reason="\"duplicate_milestone\""; }
        else if(!a->found || !b->found) { strcpy(value,"null"); reason="\"missing_milestone\""; }
        else if(b->seq<=a->seq || b->elapsed<a->elapsed) { strcpy(value,"null"); reason="\"invalid_milestone_order\""; }
        else snprintf(value,sizeof(value),"%.3f",b->elapsed-a->elapsed);
        int n=snprintf(measurements+used,sizeof(measurements)-used,
            "%s{\"name\":\"%s\",\"value\":%s,\"unit\":\"ms\",\"startEvent\":\"%s\",\"endEvent\":\"%s\",\"unavailableReason\":%s}",
            i?",":"",pairs[i].name,value,a->name,b->name,reason);
        if(n<0 || (size_t)n>=sizeof(measurements)-used) { free(copy); return unavailable(r); } used+=(size_t)n;
    }
    char *delivery=delivery_report(&sent,&ack,input_bytes,input_rate);
    char *response=response_report(&received,&completed,output_bytes,output_rate);
    if (!delivery || !response) { sqlite3_free(delivery); sqlite3_free(response); free(copy); return unavailable(r); }
    char *json=sqlite3_mprintf("{\"schemaVersion\":\"loop-turn-report/v1\",\"evidenceOrigin\":\"browser_observed\","
        "\"integrity\":\"stored_bytes_verified\",\"clock\":\"browser_performance_now\","
        "\"receipt\":%s,\"receiptSha256\":\"%s\",\"measurements\":[%s],\"inputDelivery\":%s,\"responseAudio\":%s,"
        "\"anvilEvidence\":{\"name\":\"loop-%s-receipt.json\",\"sha256\":\"%s\"},"
        "\"limitations\":[\"Browser events are not independently attested server measurements.\","
        "\"Scheduled playback does not prove acoustic output.\",\"A sent interrupt does not prove server cancellation.\","
        "\"Model revision and human acceptance are not verified by this report.\"],\"promotionDecision\":null}",copy,digest,measurements,delivery,response,id,digest);
    sqlite3_free(delivery); sqlite3_free(response); free(copy); if(!json) return unavailable(r);
    int result=loop_reply(r,200,json); sqlite3_free(json); return result;
}

int loop_report(loop_service *s, const char *owner, const char *id, loop_response *r) {
    if (sqlite3_exec(s->db,"SAVEPOINT loop_report_snapshot",NULL,NULL,NULL)!=SQLITE_OK) return unavailable(r);
    int result=report(s,owner,id,r);
    if (sqlite3_exec(s->db,"RELEASE loop_report_snapshot",NULL,NULL,NULL)!=SQLITE_OK) {
        loop_response_free(r); return unavailable(r);
    }
    return result;
}

/* Export the exact receipt bytes. Consumers must hash the downloaded bytes,
 * not a pretty-printed or re-serialized object. Reuse all report checks. */
int loop_receipt(loop_service *s, const char *owner, const char *id, loop_response *r) {
    int reported=loop_report(s,owner,id,r);
    if (!reported || r->status!=200) return reported;
    cmp_json_object object;
    const cmp_json_field *receipt=NULL;
    if (cmp_json_object_parse(r->body,&object)) receipt=cmp_json_object_field(&object,"receipt");
    char *copy=receipt ? strndup(receipt->value,receipt->value_len) : NULL;
    loop_response_free(r);
    if (!copy) return unavailable(r);
    int result=loop_reply(r,200,copy);
    free(copy);
    if (result) snprintf(r->headers,sizeof(r->headers),"Content-Disposition: attachment; filename=\"loop-%s-receipt.json\"\r\n",id);
    return result;
}
