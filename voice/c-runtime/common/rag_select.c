#include "rag_select.h"

#include <string.h>

static void trim_ascii(const char **p, size_t *n) {
    while (*n > 0 && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) {
        (*p)++;
        (*n)--;
    }
    while (*n > 0) {
        char c = (*p)[*n - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        (*n)--;
    }
}

int rag_select_strip_options(const char *in, size_t in_len, char *out, size_t out_cap) {
    size_t i, n;
    const char *p;
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (!in) return -1;
    p = in;
    n = in_len;
    trim_ascii(&p, &n);
    for (i = 0; i < n; ++i) {
        if (p[i] == '?' || p[i] == ' ' || p[i] == '\t') {
            n = i;
            break;
        }
    }
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

int rag_select_split(
    const char *in,
    size_t in_len,
    char parts[][RAG_SELECT_MAX_NAME],
    size_t parts_cap,
    size_t *out_n
) {
    size_t count = 0;
    size_t i = 0;
    if (!out_n) return -1;
    *out_n = 0;
    if (!in || !parts || parts_cap == 0) return -1;
    while (i < in_len && count < parts_cap && count < RAG_SELECT_MAX_PARTS) {
        size_t start = i;
        size_t seg_len;
        while (i < in_len && in[i] != ',') i++;
        seg_len = i - start;
        if (rag_select_strip_options(in + start, seg_len, parts[count], RAG_SELECT_MAX_NAME) != 0) {
            return -1;
        }
        if (parts[count][0] != '\0') {
            count++;
        }
        if (i < in_len && in[i] == ',') i++;
    }
    *out_n = count;
    return 0;
}

/* Find "key":"value" (simple JSON string field; no escape decode beyond \"). */
static int extract_json_string_field(
    const char *in,
    size_t in_len,
    const char *key,
    size_t *io_pos,
    char *val,
    size_t val_cap
) {
    size_t klen = strlen(key);
    size_t p = *io_pos;
    size_t o = 0;
    if (!in || !key || !val || val_cap == 0) return -1;
    val[0] = '\0';
    while (p + klen + 3 < in_len) {
        if (in[p] == '"' && memcmp(in + p + 1, key, klen) == 0 && in[p + 1 + klen] == '"') {
            size_t q = p + 1 + klen + 1;
            while (q < in_len && (in[q] == ' ' || in[q] == '\t' || in[q] == ':')) q++;
            if (q < in_len && in[q] == '"') {
                q++;
                while (q < in_len && in[q] != '"' && o + 1 < val_cap) {
                    if (in[q] == '\\' && q + 1 < in_len) {
                        q++;
                        val[o++] = in[q++];
                        continue;
                    }
                    val[o++] = in[q++];
                }
                val[o] = '\0';
                *io_pos = q < in_len ? q + 1 : q;
                return 0;
            }
        }
        p++;
    }
    *io_pos = p;
    return -1;
}

int rag_hits_excerpt_v1(
    const char *in,
    size_t in_len,
    size_t max_hits,
    char *out,
    size_t out_cap
) {
    static const char *keys[] = {"text", "content", "snippet", NULL};
    size_t pos = 0;
    size_t found = 0;
    size_t out_len = 0;
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (!in || in_len == 0) return -1;
    if (max_hits == 0) max_hits = 3;

    while (found < max_hits && pos < in_len) {
        size_t k;
        char val[256];
        int hit = 0;
        size_t try_pos = pos;
        for (k = 0; keys[k]; ++k) {
            size_t scan = try_pos;
            if (extract_json_string_field(in, in_len, keys[k], &scan, val, sizeof(val)) == 0 &&
                val[0]) {
                pos = scan;
                hit = 1;
                break;
            }
        }
        if (!hit) break;
        if (out_len > 0 && out_len + 3 < out_cap) {
            out[out_len++] = ' ';
            out[out_len++] = '|';
            out[out_len++] = ' ';
        }
        {
            size_t vl = strlen(val);
            if (out_len + vl >= out_cap) vl = out_cap - out_len - 1;
            memcpy(out + out_len, val, vl);
            out_len += vl;
            out[out_len] = '\0';
        }
        found++;
    }

    if (found == 0) {
        /* Fallback: trim and copy raw (cap). */
        const char *p = in;
        size_t n = in_len;
        while (n > 0 && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
            p++;
            n--;
        }
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, p, n);
        out[n] = '\0';
    }
    return 0;
}
