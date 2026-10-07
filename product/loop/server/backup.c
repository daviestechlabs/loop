#include "loop.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef LOOP_TESTING
int loop_backup_test_directory_sync_error;
#endif

static int sync_destination_directory(const char *destination) {
    const char *slash = strrchr(destination, '/');
    char *directory = slash ? strndup(destination, slash == destination ? 1 : (size_t)(slash - destination)) : strdup(".");
    if (!directory) return 0;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (fd < 0) return 0;
    int result;
#ifdef LOOP_TESTING
    if (loop_backup_test_directory_sync_error) {
        errno = loop_backup_test_directory_sync_error;
        result = -1;
    } else
#endif
    {
        do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    }
    int closed = close(fd);
    return result == 0 && closed == 0;
}

static int scalar_equals(sqlite3 *db, const char *sql, const char *expected) {
    sqlite3_stmt *stmt = NULL;
    int ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        ok = sqlite3_step(stmt) == SQLITE_ROW;
        const unsigned char *value = ok ? sqlite3_column_text(stmt, 0) : NULL;
        ok = value && !strcmp((const char *)value, expected);
        if (ok) ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    return ok;
}

/* Local operator command only. Both paths must reside in trusted directories. */
static int backup_snapshot(const char *source, const char *destination, uint64_t *verified, uint64_t *unfinished) {
    uint64_t checked = 0, pending = 0;
    sqlite3 *input = NULL, *output = NULL;
    sqlite3_backup *copy = NULL;
    int ok = 0, temporary_created = 0;
    char *temporary = NULL;
    struct stat existing;
    if (!source || !*source || !destination || !*destination || !lstat(destination, &existing)) return 0;
    if (sqlite3_open_v2(source, &input, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    sqlite3_busy_timeout(input, 5000);
    /* Pin a source snapshot, including committed WAL pages, before checking its schema. */
    if (sqlite3_exec(input, "BEGIN", NULL, NULL, NULL) != SQLITE_OK ||
        !scalar_equals(input, "PRAGMA user_version", "2") ||
        !scalar_equals(input, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
            "('sessions','turns','events','audio','cases','anvil_links')", "6")) goto done;
    size_t capacity = strlen(destination) + 24;
    temporary = malloc(capacity);
    if (!temporary) goto done;
    snprintf(temporary, capacity, "%s.partial-XXXXXX", destination);
    int fd = mkstemp(temporary);
    if (fd < 0) goto done;
    temporary_created = 1;
    if (close(fd)) goto done;
    if (sqlite3_open_v2(temporary, &output, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) goto done;
    if (sqlite3_exec(output, "PRAGMA synchronous=FULL", NULL, NULL, NULL) != SQLITE_OK) goto done;
    copy = sqlite3_backup_init(output, "main", input, "main");
    if (!copy) goto done;
    int result = sqlite3_backup_step(copy, -1);
    int finish = sqlite3_backup_finish(copy); copy = NULL;
    if (result != SQLITE_DONE || finish != SQLITE_OK) goto done;
    /* Produce a standalone file, independent of destination WAL sidecars. */
    if (!scalar_equals(output, "PRAGMA journal_mode=DELETE", "delete") ||
        !scalar_equals(output, "PRAGMA integrity_check", "ok")) goto done;
    if (sqlite3_close(output) != SQLITE_OK) goto done;
    output = NULL;
    if (verified && !loop_verify_recordings(temporary, &checked, &pending)) goto done;
    /* Publish only the verified file. link() refuses existing files and symlinks. */
    if (link(temporary, destination)) goto done;
    /* SQLite syncs its file; publishing a new name also requires directory sync. */
    ok = sync_destination_directory(destination);
done:
    if (copy) sqlite3_backup_finish(copy);
    if (output) sqlite3_close(output);
    if (input) sqlite3_close(input);
    if (temporary_created) {
        unlink(temporary);
        size_t length = strlen(temporary);
        strcpy(temporary + length, "-wal"); unlink(temporary);
        strcpy(temporary + length, "-shm"); unlink(temporary);
    }
    free(temporary);
    if (ok && verified) { *verified = checked; *unfinished = pending; }
    return ok;
}

int loop_backup(const char *source, const char *destination) {
    return backup_snapshot(source, destination, NULL, NULL);
}

int loop_backup_recordings(const char *source, const char *destination, uint64_t *verified, uint64_t *unfinished) {
    if (!verified || !unfinished) return 0;
    *verified = 0; *unfinished = 0;
    return backup_snapshot(source, destination, verified, unfinished);
}

/* Operator-only, read-only check. Counts describe finalized recordings, not SSO
 * admission, model provenance, annotations, or unfinished recordings. */
int loop_verify_recordings(const char *source, uint64_t *verified, uint64_t *unfinished) {
    loop_service service = {0};
    sqlite3_stmt *rows = NULL;
    uint64_t checked = 0, pending = 0;
    int ok = 0, rc = SQLITE_ERROR;
    if (!verified || !unfinished) return 0;
    *verified = 0; *unfinished = 0;
    if (!source || !*source || sqlite3_open_v2(source, &service.db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto done;
    sqlite3_busy_timeout(service.db, 5000);
    if (sqlite3_exec(service.db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK ||
        !scalar_equals(service.db, "PRAGMA user_version", "2") ||
        !scalar_equals(service.db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
            "('sessions','turns','events','audio','cases','anvil_links')", "6") ||
        !scalar_equals(service.db, "PRAGMA integrity_check", "ok") ||
        sqlite3_prepare_v2(service.db, "SELECT owner,id,status FROM turns ORDER BY owner,id", -1, &rows, NULL) != SQLITE_OK) goto done;
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
        const char *owner = (const char *)sqlite3_column_text(rows, 0);
        const char *id = (const char *)sqlite3_column_text(rows, 1);
        const char *status = (const char *)sqlite3_column_text(rows, 2);
        if (!owner || !*owner || !id || !loop_id_valid(id) || !status ||
            strlen(owner) != (size_t)sqlite3_column_bytes(rows, 0) ||
            strlen(id) != (size_t)sqlite3_column_bytes(rows, 1) ||
            strlen(status) != (size_t)sqlite3_column_bytes(rows, 2)) goto done;
        if (!strcmp(status, "recording")) { pending++; continue; }
        loop_response response = {0};
        int valid = loop_receipt(&service, owner, id, &response) && response.status == 200;
        loop_response_free(&response);
        if (!valid) goto done;
        checked++;
    }
    ok = rc == SQLITE_DONE;
done:
    sqlite3_finalize(rows);
    if (service.db && sqlite3_close(service.db) != SQLITE_OK) ok = 0;
    if (ok) { *verified = checked; *unfinished = pending; }
    return ok;
}
