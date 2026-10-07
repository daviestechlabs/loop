#define _POSIX_C_SOURCE 200809L
#include "toolstore.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Minimal SHA-256 (public domain style compact). */
typedef struct {
    unsigned long long bitlen;
    unsigned int state[8];
    unsigned char data[64];
    unsigned int datalen;
} sha256_ctx;

static const unsigned int k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx *ctx, const unsigned char data[]) {
    unsigned int a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((unsigned int)data[j] << 24) | ((unsigned int)data[j + 1] << 16) |
               ((unsigned int)data[j + 2] << 8) | (unsigned int)data[j + 3];
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx *ctx, const unsigned char *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, unsigned char hash[32]) {
    unsigned int i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (unsigned long long)ctx->datalen * 8;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4] = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8] = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }
}

static void sha256_hex(const char *text, char out[65]) {
    sha256_ctx ctx;
    unsigned char hash[32];
    int i;
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)(text ? text : ""), text ? strlen(text) : 0);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

static int is_id_first(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static int is_id_rest(char c) {
    return is_id_first(c) || c == '.' || c == '_' || c == ':' || c == '-';
}

int ts_valid_id(const char *id) {
    size_t i, n;
    if (!id || !id[0])
        return 0;
    n = strlen(id);
    if (n < 1 || n > TS_ID_MAX)
        return 0;
    if (!is_id_first(id[0]))
        return 0;
    for (i = 1; i < n; i++) {
        if (!is_id_rest(id[i]))
            return 0;
    }
    return 1;
}

int ts_shard_name(const char *call_id, char *out, size_t cap) {
    char hex[65];
    if (!out || cap < TS_SHARD_NAME)
        return TS_ERR;
    if (!ts_valid_id(call_id))
        return TS_ERR;
    sha256_hex(call_id, hex);
    if ((size_t)snprintf(out, cap, "%s.json", hex) >= cap)
        return TS_ERR;
    return TS_OK;
}

int ts_record_dir(const char *store_path, char *out, size_t cap) {
    const char *base;
    const char *slash;
    const char *dot;
    size_t stem_len;
    if (!store_path || !store_path[0] || !out || cap < 2)
        return TS_ERR;
    /* filepath.Ext: last '.' after last path separator. */
    slash = strrchr(store_path, '/');
    base = slash ? slash + 1 : store_path;
    dot = strrchr(base, '.');
    if (dot && dot != base) {
        stem_len = (size_t)(dot - store_path);
    } else {
        stem_len = strlen(store_path);
    }
    if (stem_len + 3 > cap) /* ".d" + NUL */
        return TS_ERR;
    memcpy(out, store_path, stem_len);
    out[stem_len] = '\0';
    if (snprintf(out + stem_len, cap - stem_len, ".d") >= (int)(cap - stem_len))
        return TS_ERR;
    return TS_OK;
}

static int streq(const char *a, const char *b) {
    if (!a)
        a = "";
    if (!b)
        b = "";
    return strcmp(a, b) == 0;
}

int ts_idempotency_key(const char *user_id, const char *session_id, const char *agent_id,
                       const char *tool_id, const char *idem_key, char *out, size_t cap) {
    if (!out || cap < 1)
        return TS_ERR;
    out[0] = '\0';
    if (!idem_key || !idem_key[0])
        return TS_OK;
    if (!user_id)
        user_id = "";
    if (!session_id)
        session_id = "";
    if (!agent_id)
        agent_id = "";
    if (!tool_id)
        tool_id = "";
    if ((size_t)snprintf(out, cap, "%s%c%s%c%s%c%s%c%s", user_id, 0x1f, session_id, 0x1f, agent_id, 0x1f,
                         tool_id, 0x1f, idem_key) >= cap)
        return TS_ERR;
    return TS_OK;
}

int ts_same_request(const char *idem_a, const char *task_a, const char *turn_a, const char *user_a,
                    const char *session_a, const char *agent_a, const char *tool_a, const char *input_a,
                    const char *idem_b, const char *task_b, const char *turn_b, const char *user_b,
                    const char *session_b, const char *agent_b, const char *tool_b, const char *input_b) {
    return streq(idem_a, idem_b) && streq(task_a, task_b) && streq(turn_a, turn_b) && streq(user_a, user_b) &&
           streq(session_a, session_b) && streq(agent_a, agent_b) && streq(tool_a, tool_b) &&
           streq(input_a, input_b);
}

int ts_is_terminal(int state) {
    /* 4=REJECTED, 7=COMPLETED, 8=FAILED, 9=CANCELED */
    return state == 4 || state == 7 || state == 8 || state == 9;
}

int ts_is_terminal_name(const char *name) {
    if (!name || !name[0])
        return 0;
    if (strcmp(name, "4") == 0 || strcmp(name, "7") == 0 || strcmp(name, "8") == 0 || strcmp(name, "9") == 0)
        return 1;
    if (strcmp(name, "TOOL_CALL_STATE_REJECTED") == 0 || strcmp(name, "rejected") == 0)
        return 1;
    if (strcmp(name, "TOOL_CALL_STATE_COMPLETED") == 0 || strcmp(name, "completed") == 0)
        return 1;
    if (strcmp(name, "TOOL_CALL_STATE_FAILED") == 0 || strcmp(name, "failed") == 0)
        return 1;
    if (strcmp(name, "TOOL_CALL_STATE_CANCELED") == 0 || strcmp(name, "canceled") == 0 ||
        strcmp(name, "cancelled") == 0)
        return 1;
    return 0;
}

int ts_check_shard_identity(const char *call_id, const char *filename) {
    char expect[TS_SHARD_NAME];
    const char *base;
    if (!filename || !filename[0])
        return TS_ERR;
    if (ts_shard_name(call_id, expect, sizeof(expect)) != TS_OK)
        return TS_ERR;
    base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    return strcmp(base, expect) == 0 ? TS_OK : TS_ERR;
}

static int mkdir_p(const char *path) {
    char tmp[TS_PATH];
    size_t len;
    size_t i;
    if (!path || !path[0])
        return -1;
    len = strlen(path);
    if (len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (tmp[0] != '\0' && mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int ts_atomic_write(const char *path, const void *data, size_t len, unsigned mode) {
    char dir[TS_PATH];
    char tmp_template[TS_PATH + 32];
    const char *slash;
    const char *base;
    int fd = -1;
    size_t written = 0;
    const unsigned char *p = (const unsigned char *)data;
    int n;

    if (!path || !path[0])
        return TS_ERR;
    if (!data && len > 0)
        return TS_ERR;

    slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen == 0) {
            strcpy(dir, "/");
        } else {
            if (dlen >= sizeof(dir))
                return TS_ERR;
            memcpy(dir, path, dlen);
            dir[dlen] = '\0';
        }
        if (mkdir_p(dir) != 0)
            return TS_ERR;
        base = slash + 1;
    } else {
        strcpy(dir, ".");
        base = path;
    }

    n = snprintf(tmp_template, sizeof(tmp_template), "%s/.%s.tmp-XXXXXX", dir, base);
    if (n < 0 || (size_t)n >= sizeof(tmp_template))
        return TS_ERR;

    fd = mkstemp(tmp_template);
    if (fd < 0)
        return TS_ERR;
    if (fchmod(fd, (mode_t)mode) != 0) {
        close(fd);
        unlink(tmp_template);
        return TS_ERR;
    }
    while (written < len) {
        ssize_t w = write(fd, p + written, len - written);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            close(fd);
            unlink(tmp_template);
            return TS_ERR;
        }
        written += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_template);
        return TS_ERR;
    }
    if (close(fd) != 0) {
        unlink(tmp_template);
        return TS_ERR;
    }
    fd = -1;
    if (rename(tmp_template, path) != 0) {
        unlink(tmp_template);
        return TS_ERR;
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return TS_ERR;
    if (fsync(fd) != 0) {
        close(fd);
        return TS_ERR;
    }
    if (close(fd) != 0) return TS_ERR;
    return TS_OK;
}

/*
 * filepath.Clean subset for relative POSIX paths under a root.
 * Rejects absolute raw and any ".." that would escape above root.
 * Empty / "." after clean → TS_ERR (matches Go rejecting rel==".").
 */
static int clean_rel(const char *raw, char *out, size_t cap) {
    char segs[64][128];
    int nseg = 0;
    const char *p;
    size_t i, len;

    if (!raw || !out || cap == 0)
        return TS_ERR;
    if (raw[0] == '/')
        return TS_ERR;

    p = raw;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);

        if (seglen == 0) {
            /* collapse // */
        } else if (seglen == 1 && p[0] == '.') {
            /* skip . */
        } else if (seglen == 2 && p[0] == '.' && p[1] == '.') {
            if (nseg == 0)
                return TS_ERR; /* escapes root */
            nseg--;
        } else {
            if (nseg >= 64 || seglen >= sizeof(segs[0]))
                return TS_ERR;
            memcpy(segs[nseg], p, seglen);
            segs[nseg][seglen] = '\0';
            nseg++;
        }
        if (!slash)
            break;
        p = slash + 1;
    }

    if (nseg == 0)
        return TS_ERR;

    len = 0;
    for (i = 0; i < (size_t)nseg; i++) {
        size_t sl = strlen(segs[i]);
        if (len + sl + 2 > cap)
            return TS_ERR;
        if (i > 0)
            out[len++] = '/';
        memcpy(out + len, segs[i], sl);
        len += sl;
    }
    out[len] = '\0';
    return TS_OK;
}

int ts_bounded_path(const char *root, const char *raw, char *out, size_t cap) {
    char cleaned[TS_PATH];
    size_t rlen, clen;

    if (!root || !root[0] || !raw || !out || cap == 0)
        return TS_ERR;
    if (raw[0] == '/')
        return TS_ERR;
    if (clean_rel(raw, cleaned, sizeof(cleaned)) != TS_OK)
        return TS_ERR;

    rlen = strlen(root);
    clen = strlen(cleaned);
    while (rlen > 0 && root[rlen - 1] == '/')
        rlen--;
    if (rlen + 1 + clen + 1 > cap)
        return TS_ERR;
    memcpy(out, root, rlen);
    out[rlen] = '/';
    memcpy(out + rlen + 1, cleaned, clen + 1);
    return TS_OK;
}

int ts_edit_mode(int dry_run, char *out, size_t cap) {
    const char *s = dry_run ? "dry-ran" : "applied";
    size_t n;
    if (!out || cap == 0)
        return TS_ERR;
    n = strlen(s);
    if (n + 1 > cap)
        return TS_ERR;
    memcpy(out, s, n + 1);
    return TS_OK;
}

int ts_artifact_extension(const char *content_type, char *out, size_t cap) {
    const char *ext = ".txt";
    size_t n;
    if (!out || cap == 0)
        return TS_ERR;
    if (content_type) {
        if (strcmp(content_type, "application/json") == 0)
            ext = ".json";
        else if (strcmp(content_type, "text/x-diff") == 0)
            ext = ".patch";
    }
    n = strlen(ext);
    if (n + 1 > cap)
        return TS_ERR;
    memcpy(out, ext, n + 1);
    return TS_OK;
}
