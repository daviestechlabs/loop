#include "model_request_capture.h"
#include "voice_auth.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const char secret[] = "capture-test-key-not-a-production-secret";
static const char nonce[] = "0123456789abcdef0123456789abcdef";

static void chunk_wire(void) {
    voice_model_request_chunk chunk={0}, decoded;
    uint8_t wire[VOICE_MODEL_REQUEST_WIRE_MAX];
    memcpy(chunk.request_id,"turn-1",7); chunk.total_bytes=5; chunk.sequence=1;
    chunk.bytes=(const uint8_t *)"abc"; chunk.length=3; chunk.final=1;
    size_t length=voice_model_request_encode(wire,sizeof(wire),&chunk);
    CHECK(length==23);
    CHECK(voice_model_request_decode(wire,length,&decoded)==0);
    CHECK(!strcmp(decoded.request_id,"turn-1") && decoded.total_bytes==5 &&
        decoded.sequence==1 && decoded.final && decoded.length==3 && !memcmp(decoded.bytes,"abc",3));
    CHECK(voice_model_request_encode(wire,length-1u,&chunk)==0);
    wire[0]='X'; CHECK(voice_model_request_decode(wire,length,&decoded)!=0); wire[0]='L';
    wire[12]=2; CHECK(voice_model_request_decode(wire,length,&decoded)!=0); wire[12]=1;
    wire[13]=127; CHECK(voice_model_request_decode(wire,length,&decoded)!=0); wire[13]=6;
    wire[14]=0; CHECK(voice_model_request_decode(wire,length,&decoded)!=0); wire[14]='t';
    wire[14]='/'; CHECK(voice_model_request_decode(wire,length,&decoded)!=0); wire[14]='t';
    for (size_t n=0;n<=20;n++) CHECK(voice_model_request_decode(wire,n,&decoded)!=0);
    chunk.total_bytes=VOICE_MODEL_REQUEST_MAX+1u; CHECK(!voice_model_request_encode(wire,sizeof(wire),&chunk));
    chunk.total_bytes=5; chunk.sequence=5; CHECK(!voice_model_request_encode(wire,sizeof(wire),&chunk));
    chunk.sequence=0; memset(chunk.request_id,'a',sizeof(chunk.request_id));
    CHECK(!voice_model_request_encode(wire,sizeof(wire),&chunk));
}

static void capture_capability(void) {
    char token[VOICE_AUTH_IDENTITY_TOKEN_CAP]; voice_auth_identity_claims claims;
    voice_auth_verifier verifier = {0};
    CHECK(voice_auth_verifier_init(&verifier, secret, strlen(secret)) == VOICE_AUTH_OK);
    for (int premium = 0; premium <= 1; premium++) {
        CHECK(voice_auth_identity_issue(secret, strlen(secret), "turn", "owner", premium, 1000, 90, token, sizeof(token)) == VOICE_AUTH_OK);
        CHECK(voice_auth_identity_verify(secret, strlen(secret), token, "turn", 1001, &claims) == VOICE_AUTH_OK);
        CHECK(claims.premium == premium && claims.model_request_capture == 0);
        CHECK(voice_auth_identity_issue_capture(secret, strlen(secret), "turn", "owner", premium, 1000, 90, token, sizeof(token)) == VOICE_AUTH_OK);
        CHECK(voice_auth_identity_verify(secret, strlen(secret), token, "turn", 1001, &claims) == VOICE_AUTH_OK);
        CHECK(claims.premium == premium && claims.model_request_capture == 1);
        CHECK(voice_auth_verifier_identity_verify(&verifier, token, "turn", 1001, &claims) == VOICE_AUTH_OK);
        CHECK(claims.premium == premium && claims.model_request_capture == 1);
        CHECK(voice_auth_identity_verify(secret, strlen(secret), token, "other", 1001, &claims) != VOICE_AUTH_OK);
        CHECK(claims.model_request_capture == 0);
        CHECK(voice_auth_identity_verify(secret, strlen(secret), token, "turn", 1090, &claims) != VOICE_AUTH_OK);
        CHECK(claims.model_request_capture == 0);
        token[strlen(token)-1u] = token[strlen(token)-1u] == '0' ? '1' : '0';
        CHECK(voice_auth_identity_verify(secret, strlen(secret), token, "turn", 1001, &claims) != VOICE_AUTH_OK);
        CHECK(claims.model_request_capture == 0);
    }
    voice_auth_verifier_destroy(&verifier);
}

static void complete(voice_model_capture *capture, const char *data, size_t length) {
    CHECK(voice_model_capture_begin(capture, length) == 0);
    CHECK(voice_model_capture_append(capture, 0, data, length, 1) == 0);
}

static void hash_and_binding(void) {
    voice_model_capture capture = {0};
    char signature[65], changed[65];
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "a", 1, 0) == 0);
    CHECK(voice_model_capture_sign(&capture, secret, strlen(secret), "owner", "turn", 1000, nonce, signature) != 0);
    CHECK(signature[0] == 0);
    CHECK(voice_model_capture_append(&capture, 1, "bc", 2, 1) == 0);
    CHECK(!strcmp(capture.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(voice_model_capture_sign(&capture, secret, strlen(secret), "owner", "turn", 1000, nonce, signature) == 0);
    CHECK(voice_model_capture_verify(capture.sha256, secret, strlen(secret), "owner", "turn", 1000, nonce, signature) == 0);
    CHECK(voice_model_capture_verify(capture.sha256, secret, strlen(secret), "other", "turn", 1000, nonce, signature) != 0);
    CHECK(voice_model_capture_verify(capture.sha256, secret, strlen(secret), "owner", "other", 1000, nonce, signature) != 0);
    CHECK(voice_model_capture_verify(capture.sha256, secret, strlen(secret), "owner", "turn", 1001, nonce, signature) != 0);
    CHECK(voice_model_capture_verify(capture.sha256, secret, strlen(secret), "owner", "turn", 1000, "1123456789abcdef0123456789abcdef", signature) != 0);
    memcpy(changed, capture.sha256, sizeof(changed)); changed[0] = '0';
    CHECK(voice_model_capture_verify(changed, secret, strlen(secret), "owner", "turn", 1000, nonce, signature) != 0);
    CHECK(voice_model_capture_verify(capture.sha256, "different-key-with-at-least-32-bytes", 36, "owner", "turn", 1000, nonce, signature) != 0);
    /* This proof cannot be reused as an ordinary request authorization. */
    CHECK(voice_auth_verify(secret, strlen(secret), "POST", "/v1/voice/turns", "owner", 1000, nonce,
        (const uint8_t *)capture.sha256, 64, signature) != VOICE_AUTH_OK);
    CHECK(voice_model_capture_sign(&capture, secret, strlen(secret), "owner", "../turn", 1000, nonce, changed) != 0);
    CHECK(voice_model_capture_append(&capture, 2, "d", 1, 1) != 0);
    CHECK(capture.sha256[0] == 0 && capture.digest == NULL);
    CHECK(voice_model_capture_sign(&capture, secret, strlen(secret), "owner", "turn", 1000, nonce, changed) != 0);
    voice_model_capture_destroy(&capture);
}

static void rejected_sequences(void) {
    voice_model_capture capture = {0};
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 1, "a", 1, 0) != 0);
    CHECK(voice_model_capture_append(&capture, 0, "abc", 3, 1) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "a", 1, 0) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "bc", 2, 1) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "ab", 2, 1) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "abc", 3, 0) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, "abcd", 4, 1) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 3) == 0);
    CHECK(voice_model_capture_append(&capture, 0, NULL, 3, 1) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, 0) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, SIZE_MAX) != 0);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, VOICE_MODEL_REQUEST_MAX + 1u) != 0);
    voice_model_capture_destroy(&capture);
    complete(&capture, "{}", 2);
    CHECK(voice_model_capture_begin(&capture, 2) != 0);
    voice_model_capture_destroy(&capture);
}

static void full_capacity(void) {
    voice_model_capture capture = {0};
    unsigned char *data = malloc(VOICE_MODEL_REQUEST_MAX);
    unsigned char expected[32]; unsigned int expected_len = 0;
    char expected_hex[65];
    static const char hex[] = "0123456789abcdef";
    CHECK(data != NULL);
    for (size_t i = 0; i < VOICE_MODEL_REQUEST_MAX; ++i) data[i] = (unsigned char)(i % 251u);
    CHECK(EVP_Digest(data, VOICE_MODEL_REQUEST_MAX, expected, &expected_len, EVP_sha256(), NULL) == 1);
    CHECK(expected_len == sizeof(expected));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        expected_hex[i * 2u] = hex[expected[i] >> 4u];
        expected_hex[i * 2u + 1u] = hex[expected[i] & 15u];
    }
    expected_hex[64] = 0;
    CHECK(voice_model_capture_begin(&capture, VOICE_MODEL_REQUEST_MAX) == 0);
    size_t offset = 0; uint32_t sequence = 0;
    while (offset < VOICE_MODEL_REQUEST_MAX) {
        size_t length = VOICE_MODEL_REQUEST_MAX - offset;
        if (length > VOICE_MODEL_REQUEST_CHUNK_MAX) length = VOICE_MODEL_REQUEST_CHUNK_MAX;
        CHECK(voice_model_capture_append(&capture, sequence++, data + offset, length,
            offset + length == VOICE_MODEL_REQUEST_MAX) == 0);
        offset += length;
    }
    CHECK(!strcmp(capture.sha256, expected_hex));
    CHECK(capture.received_bytes == VOICE_MODEL_REQUEST_MAX && capture.digest == NULL);
    voice_model_capture_destroy(&capture);
    CHECK(voice_model_capture_begin(&capture, VOICE_MODEL_REQUEST_MAX) == 0);
    CHECK(voice_model_capture_append(&capture, 0, data, VOICE_MODEL_REQUEST_CHUNK_MAX + 1u, 0) != 0);
    voice_model_capture_destroy(&capture);
    free(data);
}

int main(void) {
    chunk_wire(); capture_capability(); hash_and_binding(); rejected_sequences(); full_capacity();
    puts("Model request capture: complete bytes, binding, truncation, ordering, and capacity passed");
    return 0;
}
