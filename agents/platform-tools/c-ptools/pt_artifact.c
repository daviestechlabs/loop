#include "pt_artifact.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int artifact_identity(const char *directory, const pt_call *call,
                              char path[1024], char hash[65], char uri[80]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t i;
    int length;
    if (!directory || !directory[0] || !call || !call->output_json[0] ||
        !SHA256((const unsigned char *)call->output_json, strlen(call->output_json), digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i) {
        hash[i * 2u] = digits[digest[i] >> 4u];
        hash[i * 2u + 1u] = digits[digest[i] & 15u];
    }
    hash[64] = '\0';
    length = snprintf(path, 1024u, "%s/%s.json", directory, hash);
    if (length <= 0 || length >= 1024) return -1;
    (void)snprintf(uri, 80u, "sha256:%s", hash);
    return 0;
}

static int exact_file(const char *path, const char *bytes, int restore_private) {
    struct stat statbuf;
    char buffer[4096];
    size_t length = strlen(bytes), offset = 0;
    int broadened;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &statbuf) != 0 || !S_ISREG(statbuf.st_mode) || statbuf.st_size < 0 ||
        (uint64_t)statbuf.st_size != length) goto failed;
    broadened = (statbuf.st_mode & 077u) != 0u;
    if (broadened && (!restore_private || (statbuf.st_mode & 07777u) != 0660u ||
        statbuf.st_uid != geteuid() || statbuf.st_gid != getegid() ||
        statbuf.st_nlink != 1u)) goto failed;
    while (offset < length) {
        size_t wanted = length - offset;
        ssize_t count;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || memcmp(buffer, bytes + offset, (size_t)count) != 0) goto failed;
        offset += (size_t)count;
    }
    /* Restore only the fsGroup mask, after checking every retained byte.
     * The same descriptor binds validation and the restrictive mode change. */
    if (broadened && (fchmod(fd, 0600) != 0 || fsync(fd) != 0 ||
        fstat(fd, &statbuf) != 0 || (statbuf.st_mode & 07777u) != 0600u)) goto failed;
    return close(fd) == 0 ? 0 : -1;
failed:
    close(fd);
    return -1;
}

static int verify_artifact(const char *directory, const pt_call *call, int restore_private) {
    char path[1024], hash[65], uri[80];
    if (artifact_identity(directory, call, path, hash, uri) != 0 ||
        strcmp(hash, call->output_sha256) != 0 ||
        strcmp(uri, call->output_artifact) != 0) return -1;
    return exact_file(path, call->output_json, restore_private);
}

int pt_artifact_verify(const char *directory, const pt_call *call) {
    return verify_artifact(directory, call, 0);
}

int pt_artifact_restore_private(const char *directory, const pt_call *call) {
    return verify_artifact(directory, call, 1);
}

int pt_artifact_write(const char *directory, pt_call *call) {
    char path[1024], hash[65], uri[80], temporary[1024];
    size_t length, offset = 0;
    int fd = -1, directory_fd = -1, result = -1;
    int temporary_created = 0;
    int count;
    if (artifact_identity(directory, call, path, hash, uri) != 0) return -1;
    count = snprintf(temporary, sizeof(temporary), "%s/.tool-output-XXXXXX", directory);
    if (count <= 0 || (size_t)count >= sizeof(temporary)) return -1;
    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_fd < 0) return -1;
    fd = mkstemp(temporary);
    if (fd < 0) goto done;
    temporary_created = 1;
    if (fchmod(fd, 0600) != 0) goto done;
    length = strlen(call->output_json);
    while (offset < length) {
        ssize_t written = write(fd, call->output_json + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) goto done;
        offset += (size_t)written;
    }
    if (fsync(fd) != 0) goto done;
    if (close(fd) != 0) { fd = -1; goto done; }
    fd = -1;
    /* link publishes complete bytes and cannot replace an existing artifact. */
    if (link(temporary, path) != 0 && errno != EEXIST) goto done;
    if (exact_file(path, call->output_json, 0) != 0 || fsync(directory_fd) != 0) goto done;
    memcpy(call->output_sha256, hash, sizeof(hash));
    memcpy(call->output_artifact, uri, strlen(uri) + 1u);
    result = 0;
done:
    if (fd >= 0) close(fd);
    if (temporary_created &&
        (unlink(temporary) != 0 || fsync(directory_fd) != 0)) result = -1;
    if (directory_fd >= 0) close(directory_fd);
    return result;
}
