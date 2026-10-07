#include "loop.h"
#include <arpa/inet.h>
#include <string.h>

/* Configuration comes from the operator, never from a turn or browser URL. */
static size_t https_authority(const char *url) {
    if (!url || strncmp(url,"https://",8)) return 0;
    const char *host=url+8, *end=host;
    while (*end && *end!='/') end++;
    size_t length=(size_t)(end-url);
    if (length<9 || length>=LOOP_ORIGIN_CAP) return 0;
    const char *port=NULL;
    if (*host=='[') {
        const char *close=memchr(host,']',(size_t)(end-host));
        char address[INET6_ADDRSTRLEN]; unsigned char bytes[16];
        if (!close || close-host<2 || (size_t)(close-host-1)>=sizeof(address)) return 0;
        memcpy(address,host+1,(size_t)(close-host-1)); address[close-host-1]=0;
        if (inet_pton(AF_INET6,address,bytes)!=1) return 0;
        if (close+1!=end) { if (close[1]!=':') return 0; port=close+2; }
    } else {
        const char *colon=memchr(host,':',(size_t)(end-host));
        const char *host_end=colon?colon:end;
        if (host_end==host) return 0;
        const char *label=host;
        for (const char *p=host;p<host_end;p++) {
            unsigned char c=(unsigned char)*p;
            if (c=='.') {
                if (p==label || p-label>63 || p[-1]=='-') return 0;
                label=p+1;
            } else if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                         (c>='0' && c<='9') || (c=='-' && p!=label))) return 0;
        }
        if (host_end==label || host_end-label>63 || host_end[-1]=='-') return 0;
        if (colon) port=colon+1;
    }
    if (port) {
        unsigned value=0;
        if (port==end || end-port>5) return 0;
        for (const char *p=port;p<end;p++) {
            if (*p<'0' || *p>'9') return 0;
            value=value*10u+(unsigned)(*p-'0');
        }
        if (!value || value>65535) return 0;
    }
    return length;
}
int loop_https_origin_valid(const char *origin) {
    size_t length=https_authority(origin);
    return length && origin[length]==0;
}
int loop_voice_origin(const loop_service *s,char origin[LOOP_ORIGIN_CAP]) {
    size_t length=https_authority(s->wt_url);
    if (!length || strcmp(s->wt_url+length,"/v1/voice/turns")) return 0;
    memcpy(origin,s->wt_url,length); origin[length]=0;
    return 1;
}
const char *loop_anvil_origin(const loop_service *s) {
    const char *origin=s->anvil_origin?s->anvil_origin:LOOP_ANVIL_ORIGIN;
    return loop_https_origin_valid(origin)?origin:NULL;
}
