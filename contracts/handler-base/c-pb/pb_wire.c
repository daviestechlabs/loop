#include "pb_wire.h"

#include <string.h>

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
}

static int read_varint_raw(pb_reader *r, uint64_t *out) {
    uint64_t result = 0;
    int shift = 0;
    while (r->pos < r->len && shift < 64) {
        uint8_t b = r->data[r->pos++];
        if (shift == 63 && (b & 0xfeu) != 0) return -1;
        result |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *out = result;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int pb_read_varint(pb_reader *r, uint64_t *out) {
    return read_varint_raw(r, out);
}

int pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire) {
    uint64_t tag;
    if (read_varint_raw(r, &tag) != 0) return -1;
    if (tag < 8u || tag > UINT32_MAX) return -1;
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7u);
    return 0;
}

int pb_read_bytes(pb_reader *r, const uint8_t **out, size_t *out_len) {
    uint64_t len;
    if (read_varint_raw(r, &len) != 0) return -1;
    if (len > r->len - r->pos) return -1;
    *out = r->data + r->pos;
    *out_len = (size_t)len;
    r->pos += (size_t)len;
    return 0;
}

int pb_read_string(pb_reader *r, char *out, size_t out_cap) {
    const uint8_t *bytes;
    size_t len;
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (pb_read_bytes(r, &bytes, &len) != 0) return -1;
    if (len >= out_cap || memchr(bytes, '\0', len) != NULL) return -1;
    memcpy(out, bytes, len);
    out[len] = '\0';
    return 0;
}

int pb_skip(pb_reader *r, uint32_t wire) {
    uint64_t v;
    const uint8_t *b;
    size_t n;
    switch (wire) {
    case 0:
        return read_varint_raw(r, &v);
    case 1:
        if (r->pos + 8 > r->len) return -1;
        r->pos += 8;
        return 0;
    case 2:
        return pb_read_bytes(r, &b, &n);
    case 5:
        if (r->pos + 4 > r->len) return -1;
        r->pos += 4;
        return 0;
    default:
        return -1;
    }
}

void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap) {
    w->data = buf;
    w->cap = cap;
    w->pos = 0;
}

size_t pb_writer_len(const pb_writer *w) {
    return w ? w->pos : 0;
}

int pb_write_varint(pb_writer *w, uint64_t v) {
    while (v >= 0x80) {
        if (w->pos >= w->cap) return -1;
        w->data[w->pos++] = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    if (w->pos >= w->cap) return -1;
    w->data[w->pos++] = (uint8_t)v;
    return 0;
}

int pb_write_tag(pb_writer *w, uint32_t field, uint32_t wire) {
    return pb_write_varint(w, ((uint64_t)field << 3) | wire);
}

int pb_write_string(pb_writer *w, uint32_t field, const char *s) {
    size_t len;
    if (!s) s = "";
    len = strlen(s);
    if (!len) return 0;
    if (pb_write_tag(w, field, 2) != 0) return -1;
    if (pb_write_varint(w, len) != 0) return -1;
    if (w->pos + len > w->cap) return -1;
    memcpy(w->data + w->pos, s, len);
    w->pos += len;
    return 0;
}

int pb_write_bytes(pb_writer *w, uint32_t field, const uint8_t *data, size_t len) {
    if (!len) return 0;
    if (pb_write_tag(w, field, 2) != 0) return -1;
    if (pb_write_varint(w, len) != 0) return -1;
    if (w->pos + len > w->cap) return -1;
    if (data) memcpy(w->data + w->pos, data, len);
    w->pos += len;
    return 0;
}

int pb_write_int64(pb_writer *w, uint32_t field, int64_t v) {
    if (v == 0) return 0;
    return pb_write_int64_force(w, field, v);
}

int pb_write_int64_force(pb_writer *w, uint32_t field, int64_t v) {
    if (pb_write_tag(w, field, 0) != 0) return -1;
    return pb_write_varint(w, (uint64_t)v);
}

int pb_write_bool(pb_writer *w, uint32_t field, int v) {
    if (pb_write_tag(w, field, 0) != 0) return -1;
    if (w->pos >= w->cap) return -1;
    w->data[w->pos++] = v ? 1 : 0;
    return 0;
}

int pb_write_enum(pb_writer *w, uint32_t field, int v) {
    return pb_write_int64(w, field, v);
}

int pb_write_fixed32(pb_writer *w, uint32_t field, uint32_t bits) {
    size_t i;
    if (pb_write_tag(w, field, 5) != 0)
        return -1;
    if (w->pos + 4 > w->cap)
        return -1;
    for (i = 0; i < 4; i++)
        w->data[w->pos++] = (uint8_t)((bits >> (8 * i)) & 0xff);
    return 0;
}

int pb_write_float(pb_writer *w, uint32_t field, float f) {
    uint32_t bits;
    if (f == 0.0f)
        return 0;
    memcpy(&bits, &f, sizeof(bits));
    return pb_write_fixed32(w, field, bits);
}

int pb_write_uint64(pb_writer *w, uint32_t field, uint64_t v) {
    if (v == 0)
        return 0;
    if (pb_write_tag(w, field, 0) != 0)
        return -1;
    return pb_write_varint(w, v);
}

int pb_write_map_ss(pb_writer *w, uint32_t field, const char *key, const char *val) {
    uint8_t entry[512];
    pb_writer ew;
    if (!key || !key[0]) return 0;
    pb_writer_init(&ew, entry, sizeof(entry));
    if (pb_write_string(&ew, 1, key) != 0) return -1;
    if (pb_write_string(&ew, 2, val ? val : "") != 0) return -1;
    return pb_write_bytes(w, field, entry, ew.pos);
}

int pb_get_string(const uint8_t *in, size_t n, uint32_t want, char *out, size_t cap) {
    pb_reader r;
    out[0] = '\0';
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && field == want) return pb_read_string(&r, out, cap);
        if (pb_skip(&r, wire) != 0) return -1;
    }
    return -1;
}

int64_t pb_get_int(const uint8_t *in, size_t n, uint32_t want) {
    pb_reader r;
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        uint64_t v;
        if (pb_read_tag(&r, &field, &wire) != 0) return 0;
        if (wire == 0) {
            if (pb_read_varint(&r, &v) != 0) return 0;
            if (field == want) return (int64_t)v;
            continue;
        }
        if (pb_skip(&r, wire) != 0) return 0;
    }
    return 0;
}

int pb_get_bool(const uint8_t *in, size_t n, uint32_t want) {
    return (int)pb_get_int(in, n, want) != 0;
}

int pb_get_map_ss(const uint8_t *in, size_t n, uint32_t map_field, const char *key, char *out,
                  size_t cap) {
    pb_reader r;
    out[0] = '\0';
    if (!key) return -1;
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && field == map_field) {
            const uint8_t *entry;
            size_t elen;
            char k[256], v[512];
            if (pb_read_bytes(&r, &entry, &elen) != 0) return -1;
            k[0] = v[0] = '\0';
            pb_get_string(entry, elen, 1, k, sizeof(k));
            pb_get_string(entry, elen, 2, v, sizeof(v));
            if (strcmp(k, key) == 0) {
                size_t ncopy = strlen(v);
                if (ncopy >= cap) ncopy = cap - 1;
                memcpy(out, v, ncopy);
                out[ncopy] = '\0';
                return 0;
            }
            continue;
        }
        if (pb_skip(&r, wire) != 0) return -1;
    }
    return -1;
}
