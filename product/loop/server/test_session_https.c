#include "cmp_oauth.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A local TLS fixture invokes this probe. It never uses a browser credential. */
int main(int argc, char **argv) {
    if (argc!=4 && argc!=5) return 2;
    size_t capacity=(size_t)strtoul(argv[2],NULL,10),length=0;
    if (!capacity || capacity>1024u*1024u) return 2;
    uint8_t *body=calloc(1,capacity);
    if (!body) return 2;
    int rc=argc==5?cmp_oauth_identity_get(argv[1],argv[4],body,capacity,&length):
        cmp_oauth_session_get(argv[1],"fixture-session=not-a-real-session",body,capacity,&length);
    int ok;
    if (!strcmp(argv[3],"reject")) ok=rc!=0;
    else {
        size_t expected=(size_t)strtoul(argv[3],NULL,10);
        ok=rc==0 && length==expected;
        for (size_t i=0;ok && i<length;i++) if (body[i]!='x') ok=0;
    }
    free(body);
    if (!ok) fprintf(stderr,"HTTPS fixture expectation failed (result=%d, bytes=%zu)\n",rc,length);
    return ok?0:1;
}
