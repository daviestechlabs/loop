#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(unsigned char *out, uint16_t value) {
    out[0]=(unsigned char)value; out[1]=(unsigned char)(value>>8);
}
static void put32(unsigned char *out, uint32_t value) {
    for (unsigned i=0;i<4;i++) out[i]=(unsigned char)(value>>(i*8));
}

/* Offline playback only. No model, transport, or recorded tool is executed. */
static int replay(loop_service *s, const char *owner, const char *id, const char *kind, int raw, loop_response *r) {
    if (strcmp(kind,"input") && strcmp(kind,"output")) return loop_reply(r,400,"{\"error\":\"invalid_audio_kind\"}");
    int result=loop_report(s,owner,id,r);
    if (!result || r->status!=200) return result;
    char receipt[65];
    int valid=cmp_json_str(r->body,"receiptSha256",receipt,sizeof(receipt));
    loop_response_free(r);
    if (!valid) return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}");

    sqlite3_stmt *st=NULL;
    if (sqlite3_prepare_v2(s->db,"SELECT sample_rate,pcm,sha FROM audio WHERE owner=?1 AND turn_id=?2 AND kind=?3",-1,&st,NULL)!=SQLITE_OK)
        return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}");
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,id,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,3,kind,-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    if (rc!=SQLITE_ROW) {
        sqlite3_finalize(st);
        return loop_reply(r,rc==SQLITE_DONE?404:503,rc==SQLITE_DONE?"{\"error\":\"audio_not_recorded\"}":"{\"error\":\"evidence_store_unavailable\"}");
    }
    int rate=sqlite3_column_int(st,0), size=sqlite3_column_bytes(st,1);
    const void *pcm=sqlite3_column_blob(st,1);
    const char *saved=(const char *)sqlite3_column_text(st,2);
    char pcm_hash[65];
    /* Check the selected bytes before constructing the playback container. */
    if (size<=0 || (unsigned)size>LOOP_AUDIO_MAX || size%2 ||
        (rate!=16000 && rate!=22050 && rate!=24000 && rate!=44100 && rate!=48000) ||
        !pcm || !saved || !loop_hash(pcm,(size_t)size,pcm_hash) || strcmp(saved,pcm_hash)) {
        sqlite3_finalize(st); return loop_reply(r,409,"{\"error\":\"recording_integrity_failed\"}");
    }
    size_t offset=raw?0:44, export_size=(size_t)size+offset;
    unsigned char *audio=malloc(export_size);
    if (!audio) { sqlite3_finalize(st); return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}"); }
    if (!raw) {
        memcpy(audio,"RIFF",4); put32(audio+4,(uint32_t)size+36); memcpy(audio+8,"WAVEfmt ",8);
        put32(audio+16,16); put16(audio+20,1); put16(audio+22,1);
        put32(audio+24,(uint32_t)rate); put32(audio+28,(uint32_t)rate*2);
        put16(audio+32,2); put16(audio+34,16); memcpy(audio+36,"data",4); put32(audio+40,(uint32_t)size);
    }
    memcpy(audio+offset,pcm,(size_t)size); sqlite3_finalize(st);
    char content_hash[65];
    if (!loop_hash(audio,export_size,content_hash)) { free(audio); return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}"); }
    r->body=(char *)audio; r->size=export_size; r->status=200;
    snprintf(r->content_type,sizeof(r->content_type),"%s",raw?"application/octet-stream":"audio/wav");
    snprintf(r->headers,sizeof(r->headers),
        "Content-Disposition: attachment; filename=\"loop-%s-%s.%s\"\r\n"
        "X-Content-SHA256: %s\r\nX-PCM-SHA256: %s\r\nX-Loop-Receipt-SHA256: %s\r\n"
        "X-PCM-Encoding: pcm_s16le\r\nX-PCM-Sample-Rate: %d\r\nX-PCM-Channels: 1\r\n",
        id,kind,raw?"s16le":"wav",content_hash,pcm_hash,receipt,rate);
    return 1;
}

int loop_replay(loop_service *s, const char *owner, const char *id, const char *kind, int raw, loop_response *r) {
    if (sqlite3_exec(s->db,"SAVEPOINT loop_replay_snapshot",NULL,NULL,NULL)!=SQLITE_OK)
        return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}");
    int result=replay(s,owner,id,kind,raw,r);
    if (sqlite3_exec(s->db,"RELEASE loop_replay_snapshot",NULL,NULL,NULL)!=SQLITE_OK) {
        loop_response_free(r);
        return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}");
    }
    return result;
}
