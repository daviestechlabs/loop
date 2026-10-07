#define _POSIX_C_SOURCE 200809L
#include "ia_transport.h"
#include "iaevents.h"
#include "pb_wire.h"
#include "utf8.h"
#include <openssl/sha.h>
#include <string.h>

static int token(const char *s, size_t max) {
    size_t n;
    if (!s || !*s) return 0;
    for (n = 0; s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (n >= max || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':')) return 0;
    }
    return 1;
}

static int owner_valid(const char *s) {
    return s && s[0] && strnlen(s, 64u) < 64u &&
        strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:@") == strlen(s);
}

int ia_product_session_id(const char *owner, const char *nonce, char *out, size_t cap) {
    static const char domain[] = "companions.product-session.v1";
    static const char hex[] = "0123456789abcdef";
    unsigned char input[sizeof(domain) + 64u + 128u], digest[SHA256_DIGEST_LENGTH];
    size_t pos = sizeof(domain), n, i;
    if (!out || cap < IA_SESSION_CAP) return -1;
    out[0] = '\0';
    if (!owner_valid(owner) || !token(nonce, 127u)) return -1;
    memcpy(input, domain, sizeof(domain));
    n = strlen(owner) + 1u;
    memcpy(input + pos, owner, n);
    pos += n;
    n = strlen(nonce);
    memcpy(input + pos, nonce, n);
    if (!SHA256(input, pos + n, digest)) return -1;
    memcpy(out, "ps1-", 4u);
    for (i = 0; i < sizeof(digest); ++i) {
        out[4u + i * 2u] = hex[digest[i] >> 4u];
        out[5u + i * 2u] = hex[digest[i] & 15u];
    }
    out[68] = '\0';
    return 0;
}

/* Field numbers follow opentelemetry-proto v1.9.0: logs_service.proto,
 * logs.proto, common.proto, and resource.proto. Two bounded buffers alternate
 * while nesting one record; no generated runtime or heap is needed. */
int ia_otlp_encode(const char *service, const char *wire, long long now_ms, uint8_t *out, size_t cap, size_t *len) {
    uint8_t a[IA_OTLP_CAP], b[IA_OTLP_CAP];
    uint8_t value[80], attr[112], resource[144], scope[64];
    pb_writer aw, bw, vw, kw, rw, sw, result;
    uint64_t ns;
    size_t i;
    if (len) *len = 0;
    if (!token(service, 63u) || !out || !cap || !len || !wire || !wire[0] || now_ms <= 0 ||
        (uint64_t)now_ms > UINT64_MAX / 1000000u ||
        strnlen(wire, IA_WIRE_CAP) >= IA_WIRE_CAP) return -1;
    ns = (uint64_t)now_ms * 1000000u;
    pb_writer_init(&vw, value, sizeof(value));
    pb_writer_init(&kw, attr, sizeof(attr));
    pb_writer_init(&rw, resource, sizeof(resource));
    pb_writer_init(&sw, scope, sizeof(scope));
    if (pb_write_string(&vw, 1u, service) ||
        pb_write_string(&kw, 1u, "service.name") || pb_write_bytes(&kw, 2u, value, vw.pos) ||
        pb_write_bytes(&rw, 1u, attr, kw.pos) || pb_write_string(&sw, 1u, IA_VERSION)) return -1;
    pb_writer_init(&aw, a, sizeof(a));
    pb_writer_init(&bw, b, sizeof(b));
    if (pb_write_string(&aw, 1u, wire) || pb_write_bytes(&bw, 5u, a, aw.pos) ||
        pb_write_enum(&bw, 2u, 9) || pb_write_tag(&bw, 11u, 1u) || bw.cap - bw.pos < 8u) return -1;
    for (i = 0; i < 8u; ++i) b[bw.pos++] = (uint8_t)(ns >> (i * 8u));
    pb_writer_init(&aw, a, sizeof(a));
    if (pb_write_bytes(&aw, 1u, scope, sw.pos) || pb_write_bytes(&aw, 2u, b, bw.pos)) return -1;
    pb_writer_init(&bw, b, sizeof(b));
    if (pb_write_bytes(&bw, 1u, resource, rw.pos) || pb_write_bytes(&bw, 2u, a, aw.pos)) return -1;
    pb_writer_init(&result, out, cap);
    if (pb_write_bytes(&result, 1u, b, bw.pos)) return -1;
    *len = result.pos;
    return 0;
}

int ia_otlp_ack(const uint8_t *body, size_t len) {
    pb_reader outer, partial;
    const uint8_t *bytes;
    size_t n;
    uint32_t field, wire;
    unsigned seen = 0;
    uint64_t rejected;
    if ((!body && len) || len > 2048u) return 0;
    if (!len) return 1; /* Empty ExportLogsServiceResponse is full success. */
    pb_reader_init(&outer, body, len);
    if (pb_read_tag(&outer, &field, &wire) || field != 1u || wire != 2u ||
        pb_read_bytes(&outer, &bytes, &n) || outer.pos != outer.len) return 0;
    pb_reader_init(&partial, bytes, n);
    while (partial.pos < partial.len) {
        if (pb_read_tag(&partial, &field, &wire) || field < 1u || field > 2u ||
            (seen & (1u << field))) return 0;
        seen |= 1u << field;
        if (field == 1u) {
            if (wire != 0u || pb_read_varint(&partial, &rejected) || rejected != 0) return 0;
        } else if (wire != 2u || pb_read_bytes(&partial, &bytes, &n) ||
                   memchr(bytes, 0, n) || !utf8_validate_v1(bytes, n)) return 0;
    }
    return 1;
}
