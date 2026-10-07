#include "loop.h"
#include <stdio.h>

static int64_t pragma_value(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st=NULL; int64_t value=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&st,NULL)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW)
        value=sqlite3_column_int64(st,0);
    sqlite3_finalize(st); return value;
}
int loop_storage_limit(loop_service *s) {
    uint64_t bytes=s->database_max_bytes?s->database_max_bytes:LOOP_DB_DEFAULT_MAX;
    if(bytes<1024u*1024u || bytes>LOOP_DB_HARD_MAX) return 0;
    int64_t page_size=pragma_value(s->db,"PRAGMA page_size"), pages=pragma_value(s->db,"PRAGMA page_count");
    if(page_size<512 || page_size>65536 || pages<0) return 0;
    uint64_t limit=bytes/(uint64_t)page_size;
    if((uint64_t)pages>limit) return 0;
    char sql[80]; snprintf(sql,sizeof(sql),"PRAGMA max_page_count=%llu",(unsigned long long)limit);
    return pragma_value(s->db,sql)==(int64_t)limit;
}
int loop_store_error(loop_service *s, loop_response *r) {
    if ((sqlite3_errcode(s->db)&0xff)==SQLITE_FULL)
        return loop_reply(r,507,"{\"error\":\"evidence_store_full\"}");
    return loop_reply(r,503,"{\"error\":\"evidence_store_unavailable\"}");
}
