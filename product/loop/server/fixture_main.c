/* Linked only into loop-test-http. It cannot be enabled in the release binary. */
#ifndef LOOP_TESTING
#error Test fixture must never be linked into the production service
#endif
#include "loop.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) { fprintf(stderr,"usage: loop-test-http database static-root [port]\n"); return 1; }
    long port=8876;
    if (argc==4) {
        char *end=NULL;
        port=strtol(argv[3],&end,10);
        if (!argv[3][0] || *end || port<1024 || port>65535) return 1;
    }
    char origin[64];
    snprintf(origin,sizeof(origin),"http://127.0.0.1:%ld",port);
    /* Optional disposable TLS proxy for the native QUIC browser test only. */
    const char *tls_origin=getenv("LOOP_TEST_TLS_ORIGIN");
    const char *secret=getenv("LOOP_TEST_VOICE_SECRET");
    if (tls_origin || secret) {
        unsigned tls_port=0; char extra=0;
        if (!tls_origin || !secret || strlen(secret)!=64 ||
            sscanf(tls_origin,"https://localhost:%u%c",&tls_port,&extra)!=1 ||
            tls_port<1024 || tls_port>65535) return 1;
    }
    loop_service s={.origin=tls_origin ? tls_origin : origin,.test_mode=1,.static_root=argv[2],
        .gateway_secret=secret,.wt_url=LOOP_WEBTRANSPORT_ENDPOINT};
    const char *test_voice=getenv("LOOP_TEST_WEBTRANSPORT_URL");
    if (test_voice) {
        char voice_origin[LOOP_ORIGIN_CAP]; s.wt_url=test_voice;
        if (!loop_voice_origin(&s,voice_origin)) return 1;
    }
    s.anvil_origin=getenv("LOOP_TEST_ANVIL_ORIGIN");
    if (!loop_anvil_origin(&s)) return 1;
    umask(0077); signal(SIGPIPE,SIG_IGN);
    if (!loop_open(&s,argv[1])) return 1;
    fprintf(stderr,"TEST FIXTURE ONLY: synthetic local evidence; no model or microphone proof\n");
    return loop_http_serve(&s,"127.0.0.1",(int)port) ? 0 : 1;
}
