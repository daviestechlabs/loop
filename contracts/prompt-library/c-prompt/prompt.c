#define _POSIX_C_SOURCE 200809L
#include "prompt.h"

#include <ctype.h>
#include <dirent.h>
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
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_transform(sha256_ctx *ctx, const unsigned char data[]) {
    unsigned int a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((unsigned int)data[j]<<24)|((unsigned int)data[j+1]<<16)|((unsigned int)data[j+2]<<8)|(unsigned int)data[j+3];
    for (; i < 64; ++i)
        m[i] = SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h+EP1(e)+CH(e,f,g)+k256[i]+m[i];
        t2 = EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
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
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
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
        hash[i]    = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i+4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i+8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i+12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i+16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i+20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i+24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i+28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }
}

static void sha256_hex(const char *text, char out[65]) {
    sha256_ctx ctx;
    unsigned char hash[32];
    int i;
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)(text ? text : ""), text ? strlen(text) : 0);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

static void trim_inplace(char *s) {
    char *a, *b;
    size_t n;
    if (!s) return;
    a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) {
        n = strlen(a);
        memmove(s, a, n + 1);
    }
    n = strlen(s);
    b = s + n;
    while (b > s && isspace((unsigned char)b[-1])) b--;
    *b = '\0';
}

static int copy_string(char *out, size_t cap, const char *value) {
    size_t len;
    if (!out || cap == 0 || !value) return -1;
    len = strlen(value);
    if (len >= cap) return -1;
    memcpy(out, value, len + 1);
    return 0;
}

static int append_path(char *out, size_t cap, const char *component) {
    size_t out_len;
    size_t component_len;
    if (!out || cap == 0 || !component) return -1;
    out_len = strlen(out);
    component_len = strlen(component);
    if (out_len >= cap || component_len >= cap - out_len - 1) return -1;
    out[out_len] = '/';
    memcpy(out + out_len + 1, component, component_len + 1);
    return 0;
}

static int join_path(char *out, size_t cap, const char *left, const char *right) {
    if (copy_string(out, cap, left) != 0) return -1;
    return append_path(out, cap, right);
}

int prompt_family_from_id(const char *prompt_id, char *family, size_t cap) {
    const char *dot;
    if (!prompt_id || !prompt_id[0] || !family || cap < 2) return PROMPT_ERR_ARGUMENT;
    family[0] = '\0';
    dot = strchr(prompt_id, '.');
    if (dot && dot > prompt_id) {
        size_t n = (size_t)(dot - prompt_id);
        if (n >= cap) return PROMPT_ERR_CAPACITY;
        memcpy(family, prompt_id, n);
        family[n] = '\0';
        return PROMPT_OK;
    }
    if (strncmp(prompt_id, "dnd-", 4) == 0) {
        if (copy_string(family, cap, "dnd") != 0) return PROMPT_ERR_CAPACITY;
        return PROMPT_OK;
    }
    return PROMPT_ERR_ARGUMENT;
}

int prompt_render_template(const char *template_text, const char *const *kv_pairs, char *out, size_t cap) {
    char buf[PROMPT_TEXT];
    char token[PROMPT_STR + 8];
    size_t i;
    if (!template_text || !out || cap < 2) return PROMPT_ERR_ARGUMENT;
    if (copy_string(buf, sizeof(buf), template_text) != 0) return PROMPT_ERR_CAPACITY;
    if (kv_pairs) {
        for (i = 0; kv_pairs[i]; i += 2) {
            const char *key = kv_pairs[i];
            const char *val = kv_pairs[i + 1] ? kv_pairs[i + 1] : "";
            char *pos;
            size_t key_len;
            if (!key) break;
            key_len = strlen(key);
            if (key_len > sizeof(token) - 5) return PROMPT_ERR_CAPACITY;
            memcpy(token, "{{", 2);
            memcpy(token + 2, key, key_len);
            memcpy(token + 2 + key_len, "}}", 3);
            while ((pos = strstr(buf, token)) != NULL) {
                char next[PROMPT_TEXT];
                size_t prefix = (size_t)(pos - buf);
                size_t tlen = strlen(token);
                size_t vlen = strlen(val);
                size_t rest = strlen(pos + tlen);
                size_t total;
                if (prefix > sizeof(next) - 1 || vlen > sizeof(next) - 1 - prefix ||
                    rest > sizeof(next) - 1 - prefix - vlen)
                    return PROMPT_ERR_CAPACITY;
                total = prefix + vlen + rest;
                memcpy(next, buf, prefix);
                memcpy(next + prefix, val, vlen);
                memcpy(next + prefix + vlen, pos + tlen, rest + 1);
                memcpy(buf, next, total + 1);
            }
        }
    }
    trim_inplace(buf);
    if (strlen(buf) + 1 > cap) return PROMPT_ERR_CAPACITY;
    memcpy(out, buf, strlen(buf) + 1);
    return PROMPT_OK;
}

static int read_file(const char *path, char *out, size_t cap) {
    FILE *f;
    size_t n = 0;
    if (!path || !out || cap == 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    while (n + 1 < cap) {
        size_t r = fread(out + n, 1, cap - 1 - n, f);
        if (r == 0) break;
        n += r;
    }
    if (ferror(f) || memchr(out, '\0', n) != NULL) {
        fclose(f);
        out[0] = '\0';
        return -1;
    }
    if (n + 1 == cap) {
        int extra = fgetc(f);
        if (extra != EOF || ferror(f)) {
            fclose(f);
            out[0] = '\0';
            return -1;
        }
    }
    fclose(f);
    out[n] = '\0';
    return 0;
}

static int json_str_field(const char *obj, const char *key, char *out, size_t cap) {
    char pat[128];
    const char *p;
    size_t key_len;
    size_t o = 0;
    if (!obj || !key || !out || cap == 0) return -1;
    key_len = strlen(key);
    if (key_len > sizeof(pat) - 3) return -1;
    pat[0] = '"';
    memcpy(pat + 1, key, key_len);
    pat[key_len + 1] = '"';
    pat[key_len + 2] = '\0';
    p = strstr(obj, pat);
    if (!p) {
        if (out && cap) out[0] = '\0';
        return -1;
    }
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

/* Find prompt object in manifest with matching id. */
static int find_prompt_entry(const char *manifest, const char *prompt_id,
                             char *id, size_t id_cap,
                             char *version, size_t version_cap,
                             char *type, size_t type_cap,
                             char *path, size_t path_cap) {
    const char *p = manifest;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        char cand[PROMPT_STR];
        const char *obj_start = p;
        /* walk back to nearest { */
        while (obj_start > manifest && *obj_start != '{') obj_start--;
        if (*obj_start != '{') {
            p += 4;
            continue;
        }
        if (json_str_field(obj_start, "id", cand, sizeof(cand)) != 0) {
            p += 4;
            continue;
        }
        if (strcmp(cand, prompt_id) != 0) {
            p += 4;
            continue;
        }
        if (copy_string(id, id_cap, cand) != 0) return -1;
        version[0] = type[0] = path[0] = '\0';
        if (json_str_field(obj_start, "version", version, version_cap) != 0 ||
            json_str_field(obj_start, "type", type, type_cap) != 0 ||
            json_str_field(obj_start, "path", path, path_cap) != 0)
            return -1;
        return 0;
    }
    return -1;
}

static void slash_normalize(char *path) {
    char *p;
    for (p = path; *p; p++)
        if (*p == '\\') *p = '/';
}

static int try_read_candidates(const char *root, const char *entry_path, const char *family, char *text,
                               size_t cap) {
    char candidates[4][PROMPT_PATH];
    char full[PROMPT_PATH * 2];
    char clean[PROMPT_PATH];
    char base[PROMPT_STR];
    int n = 0, i;
    const char *b;

    if (copy_string(clean, sizeof(clean), entry_path ? entry_path : "") != 0) return -1;
    slash_normalize(clean);
    if (copy_string(candidates[n++], PROMPT_PATH, clean) != 0) return -1;
    if (strncmp(clean, "prompts/", 8) != 0) {
        if (join_path(candidates[n++], PROMPT_PATH, "prompts", clean) != 0) return -1;
    }
    b = strrchr(clean, '/');
    if (copy_string(base, sizeof(base), b ? b + 1 : clean) != 0) return -1;
    if (join_path(candidates[n], PROMPT_PATH, "prompts", family) != 0 ||
        append_path(candidates[n], PROMPT_PATH, base) != 0)
        return -1;
    n++;

    for (i = 0; i < n; i++) {
        if (join_path(full, sizeof(full), root, candidates[i]) != 0) continue;
        if (read_file(full, text, cap) == 0) return 0;
    }
    return -1;
}

static int discover_default_root(char *out, size_t cap) {
    char wd[PROMPT_PATH];
    char dir[PROMPT_PATH];
    char cand[PROMPT_PATH * 2];
    if (!getcwd(wd, sizeof(wd))) return -1;
    if (copy_string(dir, sizeof(dir), wd) != 0) return -1;
    for (;;) {
        if (join_path(cand, sizeof(cand), dir, "contracts/prompt-library") == 0) {
            struct stat st;
            char prompts[PROMPT_PATH * 2];
            if (join_path(prompts, sizeof(prompts), cand, "prompts") == 0 &&
                stat(prompts, &st) == 0 && S_ISDIR(st.st_mode) &&
                copy_string(out, cap, cand) == 0) {
                return 0;
            }
        }
        if (join_path(cand, sizeof(cand), dir, "prompt-library") == 0) {
            struct stat st;
            char prompts[PROMPT_PATH * 2];
            if (join_path(prompts, sizeof(prompts), cand, "prompts") == 0 &&
                stat(prompts, &st) == 0 && S_ISDIR(st.st_mode) &&
                copy_string(out, cap, cand) == 0) {
                return 0;
            }
        }
        {
            char *slash = strrchr(dir, '/');
            if (!slash || slash == dir) break;
            *slash = '\0';
        }
    }
    return -1;
}

int prompt_load(const char *root, int root_is_env, const char *prompt_id, prompt_entry *out) {
    char family[PROMPT_STR];
    char root_buf[PROMPT_PATH];
    char manifest_path[PROMPT_PATH * 2];
    char manifest[PROMPT_TEXT];
    char id[PROMPT_STR], version[64], type[64], path[PROMPT_PATH];
    char text[PROMPT_TEXT];
    const char *use_root;

    if (!prompt_id || !out) return PROMPT_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (prompt_family_from_id(prompt_id, family, sizeof(family)) != PROMPT_OK)
        return PROMPT_ERR_ARGUMENT;

    if (root && root[0]) {
        use_root = root;
    } else {
        if (discover_default_root(root_buf, sizeof(root_buf)) != 0) return PROMPT_ERR_NOT_FOUND;
        use_root = root_buf;
        root_is_env = 0;
    }

    if (copy_string(manifest_path, sizeof(manifest_path), use_root) != 0 ||
        append_path(manifest_path, sizeof(manifest_path), "prompts") != 0 ||
        append_path(manifest_path, sizeof(manifest_path), family) != 0 ||
        append_path(manifest_path, sizeof(manifest_path), "manifest.json") != 0)
        return PROMPT_ERR_CAPACITY;
    if (read_file(manifest_path, manifest, sizeof(manifest)) != 0) return PROMPT_ERR_NOT_FOUND;
    if (find_prompt_entry(manifest, prompt_id,
                          id, sizeof(id),
                          version, sizeof(version),
                          type, sizeof(type),
                          path, sizeof(path)) != 0)
        return PROMPT_ERR_NOT_FOUND;
    if (try_read_candidates(use_root, path, family, text, sizeof(text)) != 0) return PROMPT_ERR_NOT_FOUND;
    trim_inplace(text);
    if (copy_string(out->id, sizeof(out->id), id) != 0 ||
        copy_string(out->version, sizeof(out->version), version) != 0 ||
        copy_string(out->type, sizeof(out->type), type) != 0 ||
        copy_string(out->path, sizeof(out->path), path) != 0 ||
        copy_string(out->text, sizeof(out->text), text) != 0)
        return PROMPT_ERR_CAPACITY;
    sha256_hex(text, out->sha256);
    if (copy_string(out->origin, sizeof(out->origin),
                    root_is_env ? "external" : "embedded") != 0)
        return PROMPT_ERR_CAPACITY;
    return PROMPT_OK;
}

static void esc_json(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!in) in = "";
    while (*in && o + 2 < cap) {
        char c = *in++;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\n') {
            if (o + 2 >= cap) break;
            out[o++] = '\\';
            out[o++] = 'n';
        } else if ((unsigned char)c >= 0x20) {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

int prompt_entry_encode_json(const prompt_entry *e, char *out, size_t cap) {
    char eid[PROMPT_STR * 2], ever[128], etype[128], epath[PROMPT_PATH * 2];
    char etext[PROMPT_TEXT * 2], eorig[64];
    if (!e || !out) return PROMPT_ERR_ARGUMENT;
    esc_json(e->id, eid, sizeof(eid));
    esc_json(e->version, ever, sizeof(ever));
    esc_json(e->type, etype, sizeof(etype));
    esc_json(e->path, epath, sizeof(epath));
    esc_json(e->text, etext, sizeof(etext));
    esc_json(e->origin, eorig, sizeof(eorig));
    int written = snprintf(out, cap,
                           "{\"id\":\"%s\",\"version\":\"%s\",\"type\":\"%s\",\"path\":\"%s\","
                           "\"text\":\"%s\",\"sha256\":\"%s\",\"origin\":\"%s\"}",
                           eid, ever, etype, epath, etext, e->sha256, eorig);
    if (written < 0 || (size_t)written >= cap)
        return PROMPT_ERR_CAPACITY;
    return PROMPT_OK;
}

static int replace_all_inplace(char *buf, size_t cap, const char *token, const char *val) {
    char next[PROMPT_TEXT];
    char *pos;
    size_t tlen, vlen;
    if (!buf || !token || !token[0])
        return -1;
    if (!val)
        val = "";
    tlen = strlen(token);
    vlen = strlen(val);
    while ((pos = strstr(buf, token)) != NULL) {
        size_t prefix = (size_t)(pos - buf);
        size_t rest = strlen(pos + tlen);
        size_t limit = cap < sizeof(next) ? cap : sizeof(next);
        size_t total;
        if (limit == 0 || prefix > limit - 1 || vlen > limit - 1 - prefix ||
            rest > limit - 1 - prefix - vlen)
            return -1;
        total = prefix + vlen + rest;
        memcpy(next, buf, prefix);
        memcpy(next + prefix, val, vlen);
        memcpy(next + prefix + vlen, pos + tlen, rest + 1);
        memcpy(buf, next, total + 1);
    }
    return 0;
}

static int line_has_token(const char *line, const char *token) {
    return line && token && token[0] && strstr(line, token) != NULL;
}

int prompt_render_companion_persona(const char *template_text, const char *preset,
                                    const char *custom_traits, const char *speaking_style,
                                    const char *catchphrases, char *out, size_t cap) {
    char work[PROMPT_TEXT];
    char line[PROMPT_STR * 4];
    char result[PROMPT_TEXT];
    const char *p;
    size_t result_len = 0;
    int first = 1;

    if (!template_text || !out || cap < 2)
        return PROMPT_ERR_ARGUMENT;
    if (!preset)
        preset = "";
    if (!custom_traits)
        custom_traits = "";
    if (!speaking_style)
        speaking_style = "";
    if (!catchphrases)
        catchphrases = "";

    if (copy_string(work, sizeof(work), template_text) != 0)
        return PROMPT_ERR_CAPACITY;
    result[0] = '\0';
    p = work;
    while (*p) {
        size_t i = 0;
        char *start;
        int skip = 0;
        /* take one line */
        while (*p && *p != '\n' && i + 1 < sizeof(line))
            line[i++] = *p++;
        if (*p && *p != '\n')
            return PROMPT_ERR_CAPACITY;
        line[i] = '\0';
        if (*p == '\n')
            p++;
        /* trim (in-place on line[]) */
        start = line;
        while (*start == ' ' || *start == '\t' || *start == '\r')
            start++;
        {
            char *e = start + strlen(start);
            while (e > start && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
                e--;
            *e = '\0';
        }
        if (!start[0])
            continue;
        /* optional fields: skip line when placeholder present and value empty */
        if (line_has_token(start, "{{custom_traits}}") && !custom_traits[0])
            skip = 1;
        else if (line_has_token(start, "{{speaking_style}}") && !speaking_style[0])
            skip = 1;
        else if (line_has_token(start, "{{catchphrases}}") && !catchphrases[0])
            skip = 1;
        if (skip)
            continue;
        {
            char rendered[PROMPT_STR * 4];
            if (copy_string(rendered, sizeof(rendered), start) != 0 ||
                replace_all_inplace(rendered, sizeof(rendered), "{{preset}}", preset) != 0 ||
                replace_all_inplace(rendered, sizeof(rendered), "{{custom_traits}}", custom_traits) != 0 ||
                replace_all_inplace(rendered, sizeof(rendered), "{{speaking_style}}", speaking_style) != 0 ||
                replace_all_inplace(rendered, sizeof(rendered), "{{catchphrases}}", catchphrases) != 0)
                return PROMPT_ERR_CAPACITY;
            {
                size_t rlen = strlen(rendered);
                size_t need = rlen + (first ? 0 : 1);
                if (result_len + need + 1 > sizeof(result))
                    return PROMPT_ERR_CAPACITY;
                if (!first)
                    result[result_len++] = ' ';
                memcpy(result + result_len, rendered, rlen + 1);
                result_len += rlen;
                first = 0;
            }
        }
    }
    if (result_len + 1 > cap)
        return PROMPT_ERR_CAPACITY;
    memcpy(out, result, result_len + 1);
    return PROMPT_OK;
}

int prompt_build_companion_system(const char *root, int root_is_env, const char *preset,
                                  const char *custom_traits, const char *speaking_style,
                                  const char *catchphrases, char *out, size_t cap) {
    prompt_entry *entry;
    int rc;
    if (!out || cap < 2)
        return PROMPT_ERR_ARGUMENT;
    entry = (prompt_entry *)calloc(1, sizeof(*entry));
    if (!entry)
        return PROMPT_ERR_IO;
    rc = prompt_load(root, root_is_env, PROMPT_COMPANION_PERSONA_ID, entry);
    if (rc != PROMPT_OK) {
        free(entry);
        return rc;
    }
    rc = prompt_render_companion_persona(entry->text, preset, custom_traits, speaking_style,
                                         catchphrases, out, cap);
    free(entry);
    return rc;
}
