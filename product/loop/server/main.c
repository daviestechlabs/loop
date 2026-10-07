#include "loop.h"
#include "capacity.h"
#include "retention.h"
#include "cmp_oauth.h"
#include <signal.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static const char *env_or(const char *key, const char *value) { const char *v = getenv(key); return v && *v ? v : value; }
int main(int argc, char **argv) {
    umask(0077);
    if ((argc==4 || argc==5) && !strcmp(argv[1],"--prune-backups")) {
        char *end=NULL;
        errno=0;
        unsigned long days=strtoul(argv[3],&end,10);
        if (argv[3][0]<'1' || argv[3][0]>'9' || errno || !end || *end || days>3650 ||
            (argc==5 && strcmp(argv[4],"--apply"))) {
            fprintf(stderr,"Use explicit retention days from 1 to 3650 and optional --apply\n"); return 2;
        }
        loop_retention_result result;
        int ok=loop_retention(argv[2],(uint32_t)days,(int64_t)time(NULL),argc==5,&result);
        printf("Retention %s: mode=%s snapshots=%" PRIu64 " eligible=%" PRIu64 " deleted=%" PRIu64 "\n",
            ok?"complete":"incomplete",argc==5?"apply":"dry-run",result.snapshots,result.eligible,result.deleted);
        return ok?0:1;
    }
    if (argc == 3 && !strcmp(argv[1], "--check-storage")) {
        loop_capacity capacity;
        int status = loop_capacity_check(argv[2], &capacity);
        if (status == 2) fprintf(stderr, "Storage capacity unavailable\n");
        else printf("Storage %s: available_bytes=%" PRIu64 " capacity_bytes=%" PRIu64 " minimum_bytes=1073741824 minimum_percent=15\n",
            status ? "low" : "sufficient", capacity.available_bytes, capacity.capacity_bytes);
        return status;
    }
    if (argc == 4 && !strcmp(argv[1], "--backup")) {
        int backed_up = loop_backup(argv[2], argv[3]);
        fprintf(stderr, "%s\n", backed_up ? "Loop database snapshot verified" : "Loop backup failed; check source, new destination, and storage I/O. A publication sync failure can leave a destination file.");
        return backed_up ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "--backup-recordings")) {
        uint64_t verified = 0, unfinished = 0;
        if (!loop_backup_recordings(argv[2], argv[3], &verified, &unfinished)) {
            fprintf(stderr, "Loop recording backup failed; publication sync failure can leave a destination file\n");
            return 1;
        }
        printf("Published snapshot: %" PRIu64 " finalized recordings verified; %" PRIu64 " unfinished recordings not verified\n", verified, unfinished);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--verify-recordings")) {
        uint64_t verified = 0, unfinished = 0;
        if (!loop_verify_recordings(argv[2], &verified, &unfinished)) {
            fprintf(stderr, "Loop recording verification failed\n");
            return 1;
        }
        printf("Verified %" PRIu64 " finalized recordings; %" PRIu64 " unfinished recordings not verified\n", verified, unfinished);
        return 0;
    }
    if (argc != 1) { fprintf(stderr, "Usage: loop-server [--backup SOURCE NEW_DESTINATION | --backup-recordings SOURCE NEW_DESTINATION | --verify-recordings DATABASE | --check-storage PATH | --prune-backups DIRECTORY DAYS [--apply]]\n"); return 2; }
    loop_service s = {0}; cmp_gateway_identity identity;
    const char *mode=env_or("LOOP_AUTH_MODE","gateway");
    if (!strcmp(mode,"gateway")) {
        cmp_gateway_policy policy={.issuer=getenv("SSO_ISSUER"),.audience=getenv("SSO_AUDIENCE"),
            .required_group=getenv("SSO_REQUIRED_GROUP"),.jwks_url=getenv("SSO_JWKS_URL")};
        if (!cmp_gateway_identity_init(&identity,&policy)) { fprintf(stderr,"Configure shared SSO issuer, audience, HTTPS JWKS URL, and group\n"); return 1; }
        s.sso=&identity;
    } else if (strcmp(mode,"oauth")) { fprintf(stderr,"LOOP_AUTH_MODE must be gateway or oauth\n"); return 1; }
    s.origin = getenv("LOOP_ORIGIN"); s.operators = getenv("LOOP_OPERATOR_SUBJECTS");
    s.gateway_secret = getenv("VOICE_GATEWAY_TOKEN"); s.wt_url = getenv("LOOP_WEBTRANSPORT_URL");
    s.anvil_origin = getenv("LOOP_ANVIL_ORIGIN");
    const char *limit=getenv("LOOP_DB_MAX_BYTES");
    if (limit) {
        char *limit_end=NULL; errno=0;
        unsigned long long parsed=strtoull(limit,&limit_end,10);
        if (!*limit || *limit<'0' || *limit>'9' || errno || !limit_end || *limit_end || !parsed) {
            fprintf(stderr,"Set LOOP_DB_MAX_BYTES to a positive byte count\n"); return 1;
        }
        s.database_max_bytes=(uint64_t)parsed;
    }
    s.static_root = env_or("LOOP_STATIC", "../static");
    const char *bind_address = env_or("LOOP_BIND", "127.0.0.1");
    char *end = NULL; long port = strtol(env_or("LOOP_PORT", "8080"), &end, 10);
    const char *configuration_error=loop_runtime_configuration_error(&s,getenv("OAUTH_REDIRECT_URL"));
    if (configuration_error) { fprintf(stderr,"%s\n",configuration_error); return 1; }
    if (!end || *end || port < 1 || port > 65535) { fprintf(stderr,"Set a valid LOOP_PORT\n"); return 1; }
    umask(0077); signal(SIGPIPE, SIG_IGN);
    if (!loop_open(&s, env_or("LOOP_DB", "loop.sqlite"))) { fprintf(stderr, "Loop store unavailable\n"); return 1; }
    if (!s.sso && (cmp_oauth_init() || !cmp_oauth_enabled())) { fprintf(stderr, "Loop OAuth must be configured\n"); loop_close(&s); return 1; }
    int ok = loop_http_serve(&s, bind_address, (int)port);
    if (s.sso) cmp_gateway_identity_close(s.sso); else cmp_oauth_cleanup();
    loop_close(&s); return ok ? 0 : 1;
}
