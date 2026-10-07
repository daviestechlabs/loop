#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "loop.h"
#include "retention.h"
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void age(const char *path, int64_t when) {
    struct timespec times[2]={{(time_t)when,0},{(time_t)when,0}};
    assert(!utimensat(AT_FDCWD,path,times,0));
}
static void snapshot(const char *source, const char *path, int64_t when) {
    uint64_t verified, unfinished;
    assert(loop_backup_recordings(source,path,&verified,&unfinished));
    age(path,when);
}
int main(void) {
    umask(0077);
    char root[]="/tmp/loop-retention-XXXXXX";
    assert(mkdtemp(root));
    char dir[256],source[256],paths[5][320],extra[320],linkdir[512];
    snprintf(dir,sizeof(dir),"%s/backups",root); assert(!mkdir(dir,0700));
    snprintf(source,sizeof(source),"%s/source.sqlite",root);
    loop_service s={0}; assert(loop_open(&s,source)); loop_close(&s);
    const int64_t now=1800000000, day=86400;
    for (size_t i=0;i<5;i++) snprintf(paths[i],sizeof(paths[i]),"%s/loop-00000000-0000-4000-8000-%012zu.sqlite",dir,i);
    loop_retention_result result;
    assert(!loop_retention(NULL,7,now,0,&result));
    assert(!loop_retention(dir,0,now,0,&result));
    assert(!loop_retention(dir,3651,now,0,&result));
    assert(!loop_retention(dir,7,0,0,&result));
    assert(!loop_retention(dir,7,now,2,&result));
    assert(!loop_retention(dir,7,now,0,NULL));
    assert(loop_retention(dir,7,now,0,&result) && result.snapshots==0);
    snapshot(source,paths[0],now-12*day);
    snapshot(source,paths[1],now-11*day);
    snapshot(source,paths[2],now-10*day);
    snapshot(source,paths[3],now-2*day);
    snprintf(extra,sizeof(extra),"%s/keep-me.sqlite",dir);
    int fd=open(extra,O_CREAT|O_WRONLY|O_EXCL,0600); assert(fd>=0); assert(!close(fd));
    assert(loop_retention(dir,10,now,0,&result));
    assert(result.snapshots==4 && result.eligible==2 && result.deleted==0);
    for (size_t i=0;i<4;i++) assert(!access(paths[i],F_OK));
    /* A malformed recognized snapshot must stop all deletion, regardless of scan order. */
    fd=open(paths[4],O_CREAT|O_WRONLY|O_EXCL,0600); assert(fd>=0);
    assert(write(fd,"not a database",14)==14); assert(!close(fd)); age(paths[4],now-day);
    assert(!loop_retention(dir,10,now,1,&result) && result.deleted==0);
    assert(!access(paths[0],F_OK)); assert(!unlink(paths[4]));
    assert(!symlink(paths[0],paths[4]));
    assert(!loop_retention(dir,10,now,1,&result) && result.deleted==0); assert(!unlink(paths[4]));
    assert(!link(paths[0],paths[4]));
    assert(!loop_retention(dir,10,now,1,&result) && result.deleted==0); assert(!unlink(paths[4]));
    assert(!chmod(paths[0],0644));
    assert(!loop_retention(dir,10,now,1,&result) && result.deleted==0); assert(!chmod(paths[0],0600));
    age(paths[0],now+1);
    assert(!loop_retention(dir,10,now,1,&result) && result.deleted==0); age(paths[0],now-12*day);
    snprintf(linkdir,sizeof(linkdir),"%s/link",root); assert(!symlink(dir,linkdir));
    assert(!loop_retention(linkdir,10,now,1,&result));
    strcat(linkdir,"/"); assert(!loop_retention(linkdir,10,now,1,&result));
    linkdir[strlen(linkdir)-1]=0; assert(!unlink(linkdir));
    snprintf(linkdir,sizeof(linkdir),"%s/.loop-retention.lock",dir);
    fd=open(linkdir,O_RDWR); assert(fd>=0); assert(!flock(fd,LOCK_EX|LOCK_NB));
    assert(!loop_retention(dir,10,now,1,&result)); assert(!close(fd));
    snprintf(linkdir,sizeof(linkdir),"%s/loop.sqlite",dir);
    fd=open(linkdir,O_CREAT|O_WRONLY|O_EXCL,0600); assert(fd>=0); assert(!close(fd));
    assert(!loop_retention(dir,10,now,1,&result)); assert(!unlink(linkdir));
    assert(loop_retention(dir,10,now,1,&result));
    assert(result.snapshots==4 && result.eligible==2 && result.deleted==2);
    assert(access(paths[0],F_OK) && access(paths[1],F_OK));
    assert(!access(paths[2],F_OK) && !access(paths[3],F_OK) && !access(extra,F_OK));
    assert(loop_retention(dir,10,now,1,&result) && result.deleted==0);
    /* Keep one verified snapshot even if every snapshot is older than the policy. */
    age(paths[2],now-20*day); age(paths[3],now-20*day);
    assert(loop_retention(dir,10,now,1,&result) && result.deleted==1);
    assert(!access(paths[3],F_OK));
    assert(loop_retention(dir,10,now,1,&result) && result.deleted==0);
    snapshot(source,paths[2],now-20*day);
    struct timespec precise[2]={{(time_t)(now-20*day),123},{(time_t)(now-20*day),123}};
    assert(!utimensat(AT_FDCWD,paths[2],precise,0));
    assert(loop_retention(dir,10,now,1,&result) && result.deleted==1);
    assert(!access(paths[2],F_OK) && access(paths[3],F_OK));
    assert(!unlink(paths[2])); assert(!unlink(extra)); assert(!unlink(source));
    snprintf(linkdir,sizeof(linkdir),"%s/.loop-retention.lock",dir); assert(!unlink(linkdir));
    assert(!rmdir(dir));
    const char *suffixes[]={"-wal","-shm"};
    for (size_t i=0;i<2;i++) { snprintf(extra,sizeof(extra),"%s%s",source,suffixes[i]); assert(!unlink(extra) || errno==ENOENT); }
    assert(!rmdir(root));
    puts("Retention: dry run, expiry boundary, newest preservation, invalid evidence, links, permissions, clocks, locks, and source isolation passed");
    return 0;
}
