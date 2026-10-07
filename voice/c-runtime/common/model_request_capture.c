#include "model_request_capture.h"
#include "voice_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string.h>

static int request_id_valid(const char *id) {
    if (!id) return 0;
    size_t length = strlen(id);
    if (!length || length > 127u) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static void put32(uint8_t *out, uint32_t value) {
    out[0]=(uint8_t)(value>>24u); out[1]=(uint8_t)(value>>16u);
    out[2]=(uint8_t)(value>>8u); out[3]=(uint8_t)value;
}
static uint32_t get32(const uint8_t *in) {
    return ((uint32_t)in[0]<<24u)|((uint32_t)in[1]<<16u)|((uint32_t)in[2]<<8u)|in[3];
}

size_t voice_model_request_encode(uint8_t *out, size_t capacity,
    const voice_model_request_chunk *chunk) {
    if (!out || !chunk || !memchr(chunk->request_id,0,sizeof(chunk->request_id)) ||
        !request_id_valid(chunk->request_id) || !chunk->bytes || !chunk->length ||
        chunk->length > VOICE_MODEL_REQUEST_CHUNK_MAX ||
        !chunk->total_bytes || chunk->total_bytes > VOICE_MODEL_REQUEST_MAX ||
        chunk->length > chunk->total_bytes || chunk->sequence >= chunk->total_bytes ||
        (chunk->final != 0 && chunk->final != 1)) return 0;
    size_t id_len=strlen(chunk->request_id), length=14u+id_len+chunk->length;
    if (length>capacity) return 0;
    memcpy(out,"LMR1",4); put32(out+4,chunk->sequence); put32(out+8,chunk->total_bytes);
    out[12]=(uint8_t)chunk->final; out[13]=(uint8_t)id_len;
    memcpy(out+14,chunk->request_id,id_len); memcpy(out+14+id_len,chunk->bytes,chunk->length);
    return length;
}

int voice_model_request_decode(const uint8_t *wire, size_t length,
    voice_model_request_chunk *chunk) {
    if (!chunk) return -1;
    memset(chunk,0,sizeof(*chunk));
    if (!wire || length<16u || length>VOICE_MODEL_REQUEST_WIRE_MAX ||
        memcmp(wire,"LMR1",4) || wire[12]>1u || !wire[13] || wire[13]>127u ||
        length<=14u+(size_t)wire[13]) return -1;
    size_t id_len=wire[13], bytes=length-14u-id_len;
    uint32_t total=get32(wire+8), sequence=get32(wire+4);
    if (!bytes || bytes>VOICE_MODEL_REQUEST_CHUNK_MAX || !total || total>VOICE_MODEL_REQUEST_MAX ||
        bytes>total || sequence>=total || memchr(wire+14,0,id_len)) return -1;
    memcpy(chunk->request_id,wire+14,id_len);
    if (!request_id_valid(chunk->request_id)) { memset(chunk,0,sizeof(*chunk)); return -1; }
    chunk->sequence=sequence; chunk->total_bytes=total; chunk->final=wire[12];
    chunk->bytes=wire+14+id_len; chunk->length=bytes;
    return 0;
}

void voice_model_capture_destroy(voice_model_capture *capture) {
    if (!capture) return;
    EVP_MD_CTX_free(capture->digest);
    OPENSSL_cleanse(capture, sizeof(*capture));
}

static int fail(voice_model_capture *capture) {
    voice_model_capture_destroy(capture);
    if (capture) capture->state = -1;
    return -1;
}

int voice_model_capture_begin(voice_model_capture *capture, size_t bytes) {
    if (!capture) return -1;
    if (capture->state || capture->digest || !bytes || bytes > VOICE_MODEL_REQUEST_MAX)
        return fail(capture);
    capture->digest = EVP_MD_CTX_new();
    if (!capture->digest || EVP_DigestInit_ex(capture->digest, EVP_sha256(), NULL) != 1)
        return fail(capture);
    capture->expected_bytes = bytes;
    capture->state = 1;
    return 0;
}

int voice_model_capture_append(voice_model_capture *capture, uint32_t sequence,
    const void *bytes, size_t length, int final) {
    unsigned char digest[32];
    unsigned int digest_len = 0;
    static const char hex[] = "0123456789abcdef";
    if (!capture) return -1;
    if (capture->state != 1 || !capture->digest || !bytes || !length ||
        length > VOICE_MODEL_REQUEST_CHUNK_MAX || sequence != capture->next_sequence ||
        capture->received_bytes > capture->expected_bytes ||
        length > capture->expected_bytes - capture->received_bytes ||
        (final != 0 && final != 1) ||
        final != (length == capture->expected_bytes - capture->received_bytes))
        return fail(capture);
    if (EVP_DigestUpdate(capture->digest, bytes, length) != 1) return fail(capture);
    capture->received_bytes += length;
    capture->next_sequence++;
    if (!final) return 0;
    if (EVP_DigestFinal_ex(capture->digest, digest, &digest_len) != 1 || digest_len != 32u) {
        OPENSSL_cleanse(digest, sizeof(digest));
        return fail(capture);
    }
    for (size_t i = 0; i < sizeof(digest); ++i) {
        capture->sha256[i * 2u] = hex[digest[i] >> 4u];
        capture->sha256[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    capture->sha256[64] = '\0';
    OPENSSL_cleanse(digest, sizeof(digest));
    EVP_MD_CTX_free(capture->digest);
    capture->digest = NULL;
    capture->state = 2;
    return 0;
}

static int binding(const char *sha256, const char *request_id, char path[160]) {
    static const char prefix[] = "/loop/prepared-model-request/v1/";
    if (!sha256 || strlen(sha256) != 64u || !request_id_valid(request_id)) return -1;
    for (size_t i = 0; i < 64u; ++i)
        if (!((sha256[i] >= '0' && sha256[i] <= '9') ||
              (sha256[i] >= 'a' && sha256[i] <= 'f'))) return -1;
    size_t length = strlen(request_id);
    memcpy(path, prefix, sizeof(prefix) - 1u);
    memcpy(path + sizeof(prefix) - 1u, request_id, length + 1u);
    return 0;
}

int voice_model_capture_sign(const voice_model_capture *capture,
    const char *secret, size_t secret_len, const char *owner, const char *request_id,
    int64_t captured_at, const char *nonce, char signature[65]) {
    char path[160];
    if (signature) signature[0] = '\0';
    if (!capture || capture->state != 2 || !signature || captured_at <= 0 ||
        binding(capture->sha256, request_id, path) != 0) return -1;
    return voice_auth_sign(secret, secret_len, "CAPTURE", path, owner,
        captured_at, nonce, (const uint8_t *)capture->sha256, 64u, signature) == VOICE_AUTH_OK ? 0 : -1;
}

int voice_model_capture_verify(const char *sha256,
    const char *secret, size_t secret_len, const char *owner, const char *request_id,
    int64_t captured_at, const char *nonce, const char *signature) {
    char path[160];
    if (captured_at <= 0 || binding(sha256, request_id, path) != 0) return -1;
    return voice_auth_verify(secret, secret_len, "CAPTURE", path, owner,
        captured_at, nonce, (const uint8_t *)sha256, 64u, signature) == VOICE_AUTH_OK ? 0 : -1;
}
