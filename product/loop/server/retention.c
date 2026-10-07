#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "loop.h"
#include "retention.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAPSHOT_LIMIT 1024u
typedef struct { char name[49]; struct stat state; int eligible; } snapshot;
static int snapshot_name(const char *name) {
    if (strlen(name)!=48 || strncmp(name,"loop-",5) || strcmp(name+41,".sqlite")) return 0;
    for (size_t i=0;i<36;i++) {
        char c=name[i+5];
        if (i==8 || i==13 || i==18 || i==23) { if (c!='-') return 0; }
        else if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return 0;
    }
    return 1;
}
static int safe_snapshot(const struct stat *s, int64_t now) {
    return S_ISREG(s->st_mode) && (s->st_mode&0777)==0600 &&
        s->st_uid==geteuid() && s->st_nlink==1 && s->st_size>0 &&
        s->st_mtime>0 && s->st_mtime<=now;
}
static long modified_ns(const struct stat *s) {
#ifdef __APPLE__
    return s->st_mtimespec.tv_nsec;
#else
    return s->st_mtim.tv_nsec;
#endif
}
static int unchanged(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    if (a->st_mtimespec.tv_nsec!=b->st_mtimespec.tv_nsec || a->st_ctimespec.tv_nsec!=b->st_ctimespec.tv_nsec) return 0;
#else
    if (a->st_mtim.tv_nsec!=b->st_mtim.tv_nsec || a->st_ctim.tv_nsec!=b->st_ctim.tv_nsec) return 0;
#endif
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
        a->st_mtime==b->st_mtime && a->st_ctime==b->st_ctime &&
        a->st_mode==b->st_mode && a->st_uid==b->st_uid && a->st_nlink==b->st_nlink;
}
int loop_retention(const char *directory, uint32_t days, int64_t now, int apply,
                   loop_retention_result *result) {
    if (!result) return 0;
    memset(result,0,sizeof(*result));
    if (!directory || !*directory || days<1 || days>3650 ||
        now<=(int64_t)days*86400 || (apply!=0 && apply!=1)) return 0;
    size_t length=strlen(directory);
    if (length>4096) return 0;
    char *normalized=strdup(directory);
    if (!normalized) return 0;
    while (length>1 && normalized[length-1]=='/') normalized[--length]=0;
    directory=normalized;
    int fd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) { free(normalized); return 0; }
    int ok=0, lock=-1;
    DIR *dir=NULL;
    snapshot *files=NULL;
    struct stat state;
    if (!fstatat(fd,"loop.sqlite",&state,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) goto done;
    /* NFS advisory locks require a writable file, not a read-only directory FD.
     * Dry runs may create this private coordination file, but never change snapshots. */
    lock=openat(fd,".loop-retention.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lock<0 || fstat(lock,&state) || !S_ISREG(state.st_mode) ||
        (state.st_mode&0777)!=0600 || state.st_uid!=geteuid() || state.st_nlink!=1 ||
        flock(lock,LOCK_EX|LOCK_NB)) goto done;
    int scan=dup(fd);
    if (scan<0) goto done;
    dir=fdopendir(scan);
    if (!dir) { close(scan); goto done; }
    files=calloc(SNAPSHOT_LIMIT,sizeof(*files));
    if (!files) goto done;
    size_t count=0, entries=0, newest=0;
    struct dirent *entry;
    errno=0;
    while ((entry=readdir(dir))) {
        if (++entries>4096) goto done;
        if (!snapshot_name(entry->d_name)) continue;
        if (count==SNAPSHOT_LIMIT || fstatat(fd,entry->d_name,&state,AT_SYMLINK_NOFOLLOW) ||
            !safe_snapshot(&state,now)) goto done;
        snapshot *file=&files[count];
        memcpy(file->name,entry->d_name,sizeof(file->name)); file->state=state;
        if (!count || state.st_mtime>files[newest].state.st_mtime ||
            (state.st_mtime==files[newest].state.st_mtime &&
             (modified_ns(&state)>modified_ns(&files[newest].state) ||
              (modified_ns(&state)==modified_ns(&files[newest].state) && strcmp(file->name,files[newest].name)>0)))) newest=count;
        count++;
        errno=0;
    }
    if (errno) goto done;
    result->snapshots=count;
    /* Verify every named snapshot before any deletion. Unknown files stay untouched. */
    for (size_t i=0;i<count;i++) {
        char *path=sqlite3_mprintf("%s/%s",directory,files[i].name);
        uint64_t verified=0, unfinished=0;
        int valid=path && loop_verify_recordings(path,&verified,&unfinished);
        sqlite3_free(path);
        if (!valid || fstatat(fd,files[i].name,&state,AT_SYMLINK_NOFOLLOW) ||
            !unchanged(&files[i].state,&state)) goto done;
        files[i].eligible=i!=newest && files[i].state.st_mtime<now-(int64_t)days*86400;
        result->eligible+=(uint64_t)files[i].eligible;
    }
    if (apply) for (size_t i=0;i<count;i++) {
        if (!files[i].eligible) continue;
        if (fstatat(fd,files[i].name,&state,AT_SYMLINK_NOFOLLOW) ||
            !unchanged(&files[i].state,&state) || unlinkat(fd,files[i].name,0)) goto done;
        result->deleted++;
    }
    ok=1;
done:
    /* Deletion can partially succeed. Report its count even when a later step fails. */
    if (result->deleted && fsync(fd)) ok=0;
    free(files);
    if (dir) closedir(dir);
    if (lock>=0 && close(lock)) ok=0;
    if (close(fd)) ok=0;
    free(normalized);
    return ok;
}
