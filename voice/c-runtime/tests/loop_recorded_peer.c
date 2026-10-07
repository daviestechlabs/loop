/* Isolated engine fixture. The actual C gateway owns authentication and QUIC. */
#define _POSIX_C_SOURCE 200809L
#include "vbus.h"
#include "pb_min.h"
#include "model_request_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { turn_start_c turn; size_t bytes; int committed; char mode[32]; } capture;
typedef struct {
    vbus_client *publisher;
    capture turns[16]; size_t count;
    unsigned char expected[960000]; size_t expected_bytes;
    const char *mode_file; int failed;
} fixture;
static void publish(fixture *s, const char *subject, const unsigned char *wire, size_t n) {
    if (!n || vbus_publish(s->publisher,subject,wire,n)) s->failed=1;
}
static void event(fixture *s,capture *t,const char *name) {
    unsigned char wire[512];
    size_t n=pb_encode_turn_event(wire,sizeof(wire),t->turn.request_id,name,"");
    publish(s,t->turn.response_subject,wire,n);
}
static void model_request(fixture *s,capture *t) {
    /* Synthetic context only. The unsolicited mode tests the edge capability. */
    char request[27000], subject[320];
    const char *prefix="{\"model\":\"synthetic-capture-model\",\"max_completion_tokens\":48,\"temperature\":0.2,"
        "\"stream\":true,\"include_reasoning\":false,\"stream_options\":{\"include_usage\":true},"
        "\"chat_template_kwargs\":{\"enable_thinking\":false},\"messages\":[{\"role\":\"system\","
        "\"content\":\"Synthetic transport fixture only\"},{\"role\":\"user\",\"content\":\"";
    size_t length=strlen(prefix);
    memcpy(request,prefix,length); memset(request+length,'T',26000u); length+=26000u;
    memcpy(request+length,"\"}]}",4u); length+=4u;
    snprintf(subject,sizeof(subject),"%s%s",t->turn.response_subject,VOICE_MODEL_REQUEST_SUBJECT_SUFFIX);
    voice_model_request_chunk chunk={0};
    strcpy(chunk.request_id,t->turn.request_id); chunk.total_bytes=(uint32_t)length;
    for (size_t offset=0;offset<length;) {
        unsigned char wire[VOICE_MODEL_REQUEST_WIRE_MAX];
        chunk.bytes=(const unsigned char *)request+offset; chunk.length=length-offset;
        if (chunk.length>VOICE_MODEL_REQUEST_CHUNK_MAX) chunk.length=VOICE_MODEL_REQUEST_CHUNK_MAX;
        chunk.final=offset+chunk.length==length;
        publish(s,subject,wire,voice_model_request_encode(wire,sizeof(wire),&chunk));
        offset+=chunk.length; chunk.sequence++;
    }
}
static void reply(fixture *s,capture *t) {
    unsigned char wire[DND_TURN_START_WIRE_MAX]; size_t n;
    turn_start_c admitted=t->turn;
    strcpy(admitted.text,"Before you cast the spell, let me check who is still in the room.");
    if (!strcmp(t->mode,"foreign-transcript")) strcpy(admitted.user_id,"foreign-operator");
    if (strcmp(t->mode,"missing-transcript")) {
        n=pb_encode_turn_start(wire,sizeof(wire),&admitted);
        publish(s,"ai.turn.start",wire,n);
    }
    event(s,t,"started");
    if (!strcmp(t->mode,"engine-failure")) { event(s,t,"failed"); return; }
    if (!strcmp(t->mode,"model-capture") || !strcmp(t->mode,"unsolicited-capture")) model_request(s,t);
    const char *answer="Fixture answer: Mira is still in the room. Confirm before casting.";
    n=pb_encode_turn_text_event(wire,sizeof(wire),t->turn.request_id,"text_completed",answer,answer,answer,0,1);
    publish(s,t->turn.response_subject,wire,n);
    event(s,t,"pcm_started");
    unsigned char audio[960];
    for (size_t i=0;i<sizeof(audio);i+=2) { audio[i]=0; audio[i+1]=32; }
    for (int32_t seq=0;seq<10;seq++) {
        n=pb_encode_turn_audio_event(wire,sizeof(wire),t->turn.request_id,"pcm_chunk","",audio,sizeof(audio),24000,1,16,seq,0,seq==9);
        publish(s,t->turn.response_subject,wire,n);
    }
    event(s,t,"pcm_ended"); event(s,t,"completed");
}
static void prepare(const char *subject,const char *reply_subject,const unsigned char *data,size_t n,void *user) {
    fixture *s=user; (void)subject; (void)reply_subject;
    if (s->count==16) { s->failed=1; return; }
    capture *t=&s->turns[s->count++];
    if (pb_decode_turn_start(data,n,&t->turn) || t->turn.text[0] ||
        strcmp(t->turn.user_id,"fixture-operator") || t->turn.premium || !t->turn.enable_tts ||
        strcmp(t->turn.metadata.client_transport,"webtransport-turn-stream")) { s->failed=1; return; }
    FILE *f=fopen(s->mode_file,"r");
    if (!f || fscanf(f,"%31s",t->mode)!=1) { if(f) fclose(f); s->failed=1; return; }
    fclose(f);
    if (strcmp(t->mode,"success") && strcmp(t->mode,"missing-transcript") &&
        strcmp(t->mode,"foreign-transcript") && strcmp(t->mode,"engine-failure") && strcmp(t->mode,"early-endpoint") &&
        strcmp(t->mode,"model-capture") && strcmp(t->mode,"unsolicited-capture")) { s->failed=1; return; }
    if (t->turn.model_request_capture != !strcmp(t->mode,"model-capture")) { s->failed=1; return; }
    printf("{\"type\":\"prepare\",\"request_id\":\"%s\",\"owner\":\"%s\",\"mode\":\"%s\"}\n",t->turn.request_id,t->turn.user_id,t->mode);
}
static void input(const char *subject,const char *reply_subject,const unsigned char *data,size_t n,void *user) {
    fixture *s=user; (void)reply_subject;
    for (size_t i=0;i<s->count;i++) {
        capture *t=&s->turns[i]; char expected[256];
        snprintf(expected,sizeof(expected),"ai.voice.pcm.%s",t->turn.session_id);
        if (strcmp(subject,expected)) continue;
        if (t->committed || (n && n!=640) || t->bytes+n>s->expected_bytes ||
            (n && memcmp(data,s->expected+t->bytes,n))) { s->failed=1; return; }
        if (n) {
            t->bytes+=n;
            /* Normal endpoint follows all bytes; the fault case ends mid-recording. */
            if (t->bytes==(!strcmp(t->mode,"early-endpoint") ? 12800u : s->expected_bytes)) {
                unsigned char wire[512];
                size_t len=pb_encode_turn_event(wire,sizeof(wire),t->turn.request_id,"endpoint","");
                snprintf(expected,sizeof(expected),"ai.voice.reflex.%s",t->turn.session_id);
                publish(s,expected,wire,len);
                printf("{\"type\":\"endpoint\",\"request_id\":\"%s\",\"bytes\":%zu}\n",t->turn.request_id,t->bytes);
            }
            return;
        }
        if (t->bytes!=s->expected_bytes) { s->failed=1; return; }
        t->committed=1;
        printf("{\"type\":\"pcm_commit\",\"request_id\":\"%s\",\"bytes\":%zu,\"byte_equal\":true}\n",t->turn.request_id,t->bytes);
        reply(s,t); return;
    }
    s->failed=1;
}
int main(int argc,char **argv) {
    static fixture s;
    if (argc!=3) return 1;
    FILE *f=fopen(argv[1],"rb"); if(!f) return 1;
    s.expected_bytes=fread(s.expected,1,sizeof(s.expected),f);
    int valid=!ferror(f) && feof(f) && s.expected_bytes && s.expected_bytes%640==0;
    fclose(f); if(!valid) return 1;
    s.mode_file=argv[2]; setvbuf(stdout,NULL,_IOLBF,0);
    vbus_client *subscriber=vbus_connect(vbus_default_path()); s.publisher=vbus_connect(vbus_default_path());
    if(!subscriber || !s.publisher || vbus_subscribe(subscriber,"ai.voice.turn.prepare",NULL,prepare,&s) ||
        vbus_subscribe(subscriber,"ai.voice.pcm.>",NULL,input,&s)) return 1;
    puts("READY C Loop recorded input peer");
    while(!s.failed) if(vbus_poll(subscriber,20)) break;
    vbus_close(subscriber); vbus_close(s.publisher);
    fputs("FAIL C Loop recorded input peer\n",stderr); return 1;
}
