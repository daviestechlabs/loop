#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unavailable(loop_response *r) { return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}"); }
/* Same bounded ustar layout as voice/c-runtime/tools/dnd_source_bundle.c.
 * This writer emits only three regular files; no paths, links, or input filenames. */
static int octal(unsigned char *field, size_t width, size_t value) {
    field[--width]=0;
    while(width) { field[--width]=(unsigned char)('0'+(value&7u)); value>>=3; }
    return value==0;
}
static int entry(unsigned char *out, const char *name, const void *body, size_t length) {
    if (strlen(name)>=100) return 0;
    memcpy(out,name,strlen(name));
    if (!octal(out+100,8,0600) || !octal(out+108,8,0) || !octal(out+116,8,0) ||
        !octal(out+124,12,length) || !octal(out+136,12,0)) return 0;
    memset(out+148,' ',8); out[156]='0'; memcpy(out+257,"ustar",6); memcpy(out+263,"00",2);
    size_t checksum=0;
    for(size_t i=0;i<512;i++) checksum+=out[i];
    if(!octal(out+148,7,checksum)) return 0;
    out[155]=' '; memcpy(out+512,body,length); return 1;
}
static int bundle(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r) {
    loop_response anvil={0}, report={0};
    int result=loop_anvil_export(s,owner,id,q,&anvil);
    if (!result || anvil.status!=200) { *r=anvil; return result; }
    result=loop_report(s,owner,id,&report);
    if (!result || report.status!=200) { loop_response_free(&anvil); *r=report; return result; }
    cmp_json_object object;
    const cmp_json_field *receipt=NULL;
    if (cmp_json_object_parse(report.body,&object)) receipt=cmp_json_object_field(&object,"receipt");
    if (!receipt || !loop_id_valid(id)) { loop_response_free(&anvil); loop_response_free(&report); return unavailable(r); }
    const void *bodies[]={anvil.body,report.body,receipt->value};
    const size_t lengths[]={anvil.size,report.size,receipt->value_len};
    const char *suffixes[]={"anvil","report","receipt"};
    size_t total=1024;
    for(size_t i=0;i<3;i++) {
        if (lengths[i]>LOOP_REPLY_MAX) { total=LOOP_REPLY_MAX+1u; break; }
        total+=512+((lengths[i]+511)/512)*512;
    }
    if(total>LOOP_REPLY_MAX) {
        loop_response_free(&anvil); loop_response_free(&report);
        return loop_reply(r,413,"{\"error\":\"evidence_bundle_too_large\"}");
    }
    unsigned char *bytes=calloc(1,total); size_t offset=0; int valid=bytes!=NULL;
    for(size_t i=0;valid && i<3;i++) {
        char name[100]; int n=snprintf(name,sizeof(name),"loop-%s-%s.json",id,suffixes[i]);
        valid=n>0 && (size_t)n<sizeof(name) && entry(bytes+offset,name,bodies[i],lengths[i]);
        offset+=512+((lengths[i]+511)/512)*512;
    }
    loop_response_free(&anvil); loop_response_free(&report);
    char digest[65];
    if(!valid || !loop_hash(bytes,total,digest)) { free(bytes); return unavailable(r); }
    r->body=(char *)bytes; r->size=total; r->status=200;
    snprintf(r->content_type,sizeof(r->content_type),"application/x-tar");
    snprintf(r->headers,sizeof(r->headers),"Content-Disposition: attachment; filename=\"loop-%s-anvil.tar\"\r\nX-Content-SHA256: %s\r\n",id,digest);
    return 1;
}
int loop_anvil_bundle(loop_service *s, const char *owner, const char *id, const loop_request *q, loop_response *r) {
    if(sqlite3_exec(s->db,"SAVEPOINT loop_bundle_snapshot",NULL,NULL,NULL)!=SQLITE_OK) return unavailable(r);
    int result=bundle(s,owner,id,q,r);
    if(sqlite3_exec(s->db,"RELEASE loop_bundle_snapshot",NULL,NULL,NULL)!=SQLITE_OK) {
        loop_response_free(r); return unavailable(r);
    }
    return result;
}
