#define _POSIX_C_SOURCE 200809L

/* Direct tests for bounded voice edge HMAC authentication. */
#include "voice_auth.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    AUTH_BENCH_BATCHES = 21,
    AUTH_BENCH_OPS = 128,
    NONCE_BENCH_OPS = 4096,
    NONCE_LATENCY_SAMPLES = 4096,
};

static int failures;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int benchmark_verify(
    voice_auth_verifier *verifier,
    const char *secret,
    size_t secret_len,
    int64_t timestamp,
    const char *nonce,
    const uint8_t *body,
    size_t body_len,
    const char *signature
) {
    uint64_t one_shot[AUTH_BENCH_BATCHES];
    uint64_t prepared[AUTH_BENCH_BATCHES];
    uint64_t successes = 0;
    size_t batch;
    for (batch = 0; batch < AUTH_BENCH_BATCHES; ++batch) {
        uint64_t started;
        size_t operation;
        if ((batch & 1u) == 0) {
            started = monotonic_ns();
            for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
                successes += voice_auth_verify(
                    secret, secret_len, "POST", "/v1/voice/turns", "user-1",
                    timestamp, nonce, body, body_len, signature) == VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / AUTH_BENCH_OPS;
        }
        started = monotonic_ns();
        for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
            successes += voice_auth_verifier_verify(
                verifier, "POST", "/v1/voice/turns", "user-1",
                timestamp, nonce, body, body_len, signature) == VOICE_AUTH_OK;
        prepared[batch] =
            (monotonic_ns() - started) / AUTH_BENCH_OPS;
        if ((batch & 1u) != 0) {
            started = monotonic_ns();
            for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
                successes += voice_auth_verify(
                    secret, secret_len, "POST", "/v1/voice/turns", "user-1",
                    timestamp, nonce, body, body_len, signature) == VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / AUTH_BENCH_OPS;
        }
    }
    qsort(one_shot, AUTH_BENCH_BATCHES, sizeof(one_shot[0]), compare_u64);
    qsort(prepared, AUTH_BENCH_BATCHES, sizeof(prepared[0]), compare_u64);
    printf(
        "BenchmarkVoiceAuthVerify\toneshot_p50_ns=%llu\tprepared_p50_ns=%llu\t"
        "speedup_x100=%llu\tsuccesses=%llu\n",
        (unsigned long long)one_shot[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)prepared[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)(
            one_shot[AUTH_BENCH_BATCHES / 2u] * UINT64_C(100) /
            prepared[AUTH_BENCH_BATCHES / 2u]),
        (unsigned long long)successes);
    return successes ==
        (uint64_t)AUTH_BENCH_BATCHES * AUTH_BENCH_OPS * 2u ? 0 : -1;
}

static int benchmark_identity_verify(
    voice_auth_verifier *verifier,
    const char *secret,
    size_t secret_len,
    const char *token,
    const char *request_id,
    int64_t now
) {
    uint64_t one_shot[AUTH_BENCH_BATCHES];
    uint64_t prepared[AUTH_BENCH_BATCHES];
    voice_auth_identity_claims claims;
    uint64_t successes = 0;
    size_t batch;
    for (batch = 0; batch < AUTH_BENCH_BATCHES; ++batch) {
        uint64_t started;
        size_t operation;
        if ((batch & 1u) == 0) {
            started = monotonic_ns();
            for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
                successes += voice_auth_identity_verify(
                    secret, secret_len, token, request_id, now, &claims) ==
                    VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / AUTH_BENCH_OPS;
        }
        started = monotonic_ns();
        for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
            successes += voice_auth_verifier_identity_verify(
                verifier, token, request_id, now, &claims) == VOICE_AUTH_OK;
        prepared[batch] =
            (monotonic_ns() - started) / AUTH_BENCH_OPS;
        if ((batch & 1u) != 0) {
            started = monotonic_ns();
            for (operation = 0; operation < AUTH_BENCH_OPS; ++operation)
                successes += voice_auth_identity_verify(
                    secret, secret_len, token, request_id, now, &claims) ==
                    VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / AUTH_BENCH_OPS;
        }
    }
    qsort(one_shot, AUTH_BENCH_BATCHES, sizeof(one_shot[0]), compare_u64);
    qsort(prepared, AUTH_BENCH_BATCHES, sizeof(prepared[0]), compare_u64);
    printf(
        "BenchmarkVoiceAuthIdentity\toneshot_p50_ns=%llu\tprepared_p50_ns=%llu\t"
        "speedup_x100=%llu\tsuccesses=%llu\n",
        (unsigned long long)one_shot[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)prepared[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)(
            one_shot[AUTH_BENCH_BATCHES / 2u] * UINT64_C(100) /
            prepared[AUTH_BENCH_BATCHES / 2u]),
        (unsigned long long)successes);
    return successes ==
        (uint64_t)AUTH_BENCH_BATCHES * AUTH_BENCH_OPS * 2u ? 0 : -1;
}

static int benchmark_nonce_pool(void) {
    uint64_t one_shot[AUTH_BENCH_BATCHES];
    uint64_t pooled[AUTH_BENCH_BATCHES];
    static uint64_t one_shot_latency[NONCE_LATENCY_SAMPLES];
    static uint64_t pooled_latency[NONCE_LATENCY_SAMPLES];
    voice_auth_nonce_pool pool = {0};
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    uint64_t successes = 0;
    size_t batch;
    if (voice_auth_nonce_pool_init(&pool) != VOICE_AUTH_OK) return -1;
    for (batch = 0; batch < AUTH_BENCH_BATCHES; ++batch) {
        uint64_t started;
        size_t operation;
        if ((batch & 1u) == 0) {
            started = monotonic_ns();
            for (operation = 0; operation < NONCE_BENCH_OPS; ++operation)
                successes += voice_auth_random_nonce(nonce) == VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / NONCE_BENCH_OPS;
        }
        started = monotonic_ns();
        for (operation = 0; operation < NONCE_BENCH_OPS; ++operation)
            successes += voice_auth_nonce_pool_next(&pool, nonce) == VOICE_AUTH_OK;
        pooled[batch] =
            (monotonic_ns() - started) / NONCE_BENCH_OPS;
        if ((batch & 1u) != 0) {
            started = monotonic_ns();
            for (operation = 0; operation < NONCE_BENCH_OPS; ++operation)
                successes += voice_auth_random_nonce(nonce) == VOICE_AUTH_OK;
            one_shot[batch] =
                (monotonic_ns() - started) / NONCE_BENCH_OPS;
        }
    }
    voice_auth_nonce_pool_destroy(&pool);
    qsort(one_shot, AUTH_BENCH_BATCHES, sizeof(one_shot[0]), compare_u64);
    qsort(pooled, AUTH_BENCH_BATCHES, sizeof(pooled[0]), compare_u64);
    printf(
        "BenchmarkVoiceAuthNonce\toneshot_p50_ns=%llu\tpooled_p50_ns=%llu\t"
        "speedup_x100=%llu\tsuccesses=%llu\n",
        (unsigned long long)one_shot[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)pooled[AUTH_BENCH_BATCHES / 2u],
        (unsigned long long)(
            one_shot[AUTH_BENCH_BATCHES / 2u] * UINT64_C(100) /
            pooled[AUTH_BENCH_BATCHES / 2u]),
        (unsigned long long)successes);
    if (voice_auth_nonce_pool_init(&pool) != VOICE_AUTH_OK) return -1;
    for (batch = 0; batch < NONCE_LATENCY_SAMPLES; ++batch) {
        uint64_t started;
        if ((batch & 1u) == 0) {
            started = monotonic_ns();
            successes += voice_auth_random_nonce(nonce) == VOICE_AUTH_OK;
            one_shot_latency[batch] = monotonic_ns() - started;
        }
        started = monotonic_ns();
        successes += voice_auth_nonce_pool_next(&pool, nonce) == VOICE_AUTH_OK;
        pooled_latency[batch] = monotonic_ns() - started;
        if ((batch & 1u) != 0) {
            started = monotonic_ns();
            successes += voice_auth_random_nonce(nonce) == VOICE_AUTH_OK;
            one_shot_latency[batch] = monotonic_ns() - started;
        }
    }
    voice_auth_nonce_pool_destroy(&pool);
    qsort(one_shot_latency, NONCE_LATENCY_SAMPLES,
          sizeof(one_shot_latency[0]), compare_u64);
    qsort(pooled_latency, NONCE_LATENCY_SAMPLES,
          sizeof(pooled_latency[0]), compare_u64);
    printf(
        "BenchmarkVoiceAuthNonceTail\toneshot_p95_ns=%llu\toneshot_p99_ns=%llu\t"
        "oneshot_p999_ns=%llu\tpooled_p95_ns=%llu\tpooled_p99_ns=%llu\t"
        "pooled_p999_ns=%llu\n",
        (unsigned long long)one_shot_latency[3891u],
        (unsigned long long)one_shot_latency[4055u],
        (unsigned long long)one_shot_latency[4091u],
        (unsigned long long)pooled_latency[3891u],
        (unsigned long long)pooled_latency[4055u],
        (unsigned long long)pooled_latency[4091u]);
    return successes ==
        (uint64_t)AUTH_BENCH_BATCHES * NONCE_BENCH_OPS * 2u +
        (uint64_t)NONCE_LATENCY_SAMPLES * 2u ? 0 : -1;
}

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int lowercase_hex(const char *value, size_t expected_len) {
    size_t i;
    if (!value || strlen(value) != expected_len) return 0;
    for (i = 0; i < expected_len; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}

static int bytes_zero(const unsigned char *value, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i)
        if (value[i] != 0) return 0;
    return 1;
}

static int test_b64url_value(unsigned char value) {
    if (value >= 'A' && value <= 'Z') return (int)(value - 'A');
    if (value >= 'a' && value <= 'z') return (int)(value - 'a') + 26;
    if (value >= '0' && value <= '9') return (int)(value - '0') + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
}

static int correctly_signed_base64url_alias(
    const char *secret,
    size_t secret_len,
    const char *token,
    char *out,
    size_t out_cap
) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    static const char domain[] = "voice-webtransport-identity-v1\n";
    unsigned char canonical[sizeof(domain) - 1u + VOICE_AUTH_IDENTITY_TOKEN_CAP];
    unsigned char mac[SHA256_DIGEST_LENGTH];
    const char *encoded;
    const char *separator;
    size_t token_len;
    size_t encoded_len;
    size_t separator_at;
    unsigned int mac_len = 0u;
    int final_value;
    size_t i;

    if (!secret || !token || !out) return -1;
    token_len = strlen(token);
    if (token_len + 1u > out_cap ||
        strncmp(token, "vat1.", sizeof("vat1.") - 1u) != 0)
        return -1;
    encoded = token + sizeof("vat1.") - 1u;
    separator = strchr(encoded, '.');
    if (!separator || strlen(separator + 1u) != VOICE_AUTH_SIGNATURE_HEX_LEN)
        return -1;
    encoded_len = (size_t)(separator - encoded);
    if (encoded_len == 0u ||
        (encoded_len % 4u != 2u && encoded_len % 4u != 3u))
        return -1;
    final_value = test_b64url_value((unsigned char)separator[-1]);
    if (final_value < 0 ||
        (encoded_len % 4u == 2u && (final_value & 15) != 0) ||
        (encoded_len % 4u == 3u && (final_value & 3) != 0))
        return -1;

    memcpy(out, token, token_len + 1u);
    separator_at = (size_t)(separator - token);
    out[separator_at - 1u] = alphabet[(unsigned)final_value | 1u];
    memcpy(canonical, domain, sizeof(domain) - 1u);
    memcpy(canonical + sizeof(domain) - 1u,
           out + sizeof("vat1.") - 1u, encoded_len);
#if defined(OPENSSL_IS_AWSLC) || defined(OPENSSL_IS_BORINGSSL)
    if (!HMAC(EVP_sha256(), secret, secret_len, canonical,
              sizeof(domain) - 1u + encoded_len, mac, &mac_len))
#else
    if (secret_len > (size_t)INT_MAX ||
        !HMAC(EVP_sha256(), secret, (int)secret_len, canonical,
              sizeof(domain) - 1u + encoded_len, mac, &mac_len))
#endif
        return -1;
    if (mac_len != SHA256_DIGEST_LENGTH) return -1;
    for (i = 0; i < sizeof(mac); ++i) {
        static const char hex[] = "0123456789abcdef";
        out[separator_at + 1u + i * 2u] = hex[mac[i] >> 4u];
        out[separator_at + 2u + i * 2u] = hex[mac[i] & 15u];
    }
    out[separator_at + 1u + sizeof(mac) * 2u] = '\0';
    return 0;
}

static void test_prepared_verifier_sequence(
    voice_auth_verifier *verifier,
    const char *secret,
    size_t secret_len
) {
    static const uint8_t short_body[] = {0x0a, 0x03, 'r', 'e', 'q'};
    static const char *const methods[] = {
        "POST", "DELETE", "PUT", "ABCDEFGHIJKLMNOP"
    };
    static const char *const nonces[] = {
        "00112233445566778899aabbccddeeff",
        "ffeeddccbbaa99887766554433221100",
        "0123456789abcdef0123456789abcdef",
        "abcdef0123456789abcdef0123456789"
    };
    static const int64_t timestamps[] = {
        INT64_C(1787572800),
        INT64_C(1787572801),
        INT64_C(2147483648),
        INT64_MAX
    };
    uint8_t binary_body[257];
    char maximum_path[513];
    char maximum_user[128];
    const char *paths[4];
    const char *users[4];
    const uint8_t *bodies[4];
    size_t body_lengths[4];
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char rejected_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    int sequence_matches = 1;
    int rejection_recovers = 1;
    int maximum_bound_matches = 0;
    size_t i;

    maximum_path[0] = '/';
    memset(maximum_path + 1u, 'p', sizeof(maximum_path) - 2u);
    maximum_path[sizeof(maximum_path) - 1u] = '\0';
    memset(maximum_user, 'u', sizeof(maximum_user) - 1u);
    maximum_user[sizeof(maximum_user) - 1u] = '\0';
    for (i = 0; i < sizeof(binary_body); ++i)
        binary_body[i] = (uint8_t)(i * 131u + 17u);

    paths[0] = "/v1/voice/turns";
    paths[1] = "/v1/voice/turns/request.with-punctuation_123";
    paths[2] = "/v1/voice/audio";
    paths[3] = maximum_path;
    users[0] = "user-1";
    users[1] = "user.with-punctuation_123";
    users[2] = "u";
    users[3] = maximum_user;
    bodies[0] = short_body;
    bodies[1] = NULL;
    bodies[2] = binary_body;
    bodies[3] = binary_body;
    body_lengths[0] = sizeof(short_body);
    body_lengths[1] = 0u;
    body_lengths[2] = sizeof(binary_body);
    body_lengths[3] = sizeof(binary_body);

    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        if (voice_auth_sign(
                secret, secret_len, methods[i], paths[i], users[i],
                timestamps[i], nonces[i], bodies[i], body_lengths[i],
                signature) != VOICE_AUTH_OK) {
            sequence_matches = 0;
            rejection_recovers = 0;
            continue;
        }
        if (voice_auth_verify(
                secret, secret_len, methods[i], paths[i], users[i],
                timestamps[i], nonces[i], bodies[i], body_lengths[i],
                signature) != VOICE_AUTH_OK ||
            voice_auth_verifier_verify(
                verifier, methods[i], paths[i], users[i], timestamps[i],
                nonces[i], bodies[i], body_lengths[i], signature) !=
                VOICE_AUTH_OK)
            sequence_matches = 0;
        else if (i == 3u)
            maximum_bound_matches = 1;

        memcpy(rejected_signature, signature, sizeof(rejected_signature));
        rejected_signature[i] = rejected_signature[i] == '0' ? '1' : '0';
        if (voice_auth_verifier_verify(
                verifier, methods[i], paths[i], users[i], timestamps[i],
                nonces[i], bodies[i], body_lengths[i], rejected_signature) !=
                VOICE_AUTH_ERR_SIGNATURE ||
            voice_auth_verifier_verify(
                verifier, methods[i], paths[i], users[i], timestamps[i],
                nonces[i], bodies[i], body_lengths[i], signature) !=
                VOICE_AUTH_OK)
            rejection_recovers = 0;
    }
    expect(sequence_matches,
           "prepared verifier matches varied keyed request sequence");
    expect(maximum_bound_matches,
           "maximum canonical request fits its exact buffer bound");
    expect(rejection_recovers,
           "prepared verifier recovers after rejected signature sequence");
}

static void test_prepared_verifier_secret_boundaries(void) {
    static const size_t secret_lengths[] = {32u, 63u, 64u, 65u, 255u};
    static const char nonce[] = "fedcba98765432100123456789abcdef";
    static const uint8_t body[] = {0x00, 0xff, 0x0a, 0x80, 0x01};
    unsigned char secret[255];
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char rejected_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char identity_token[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    char rejected_identity[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    voice_auth_identity_claims claims;
    int request_matches = 1;
    int identity_matches = 1;
    int rejection_recovers = 1;
    size_t i;
    size_t j;

    for (i = 0u; i < sizeof(secret); ++i)
        secret[i] = (unsigned char)(i * 29u + 3u);
    for (i = 0u; i < sizeof(secret_lengths) / sizeof(secret_lengths[0]); ++i) {
        voice_auth_verifier verifier = {0};
        size_t secret_len = secret_lengths[i];

        if (voice_auth_sign(
                (const char *)secret, secret_len, "POST", "/v1/voice/turns",
                "boundary-user", INT64_C(1787572800), nonce, body,
                sizeof(body), signature) != VOICE_AUTH_OK ||
            voice_auth_verifier_init(
                &verifier, (const char *)secret, secret_len) != VOICE_AUTH_OK ||
            voice_auth_verifier_verify(
                &verifier, "POST", "/v1/voice/turns", "boundary-user",
                INT64_C(1787572800), nonce, body, sizeof(body), signature) !=
                VOICE_AUTH_OK) {
            request_matches = 0;
            voice_auth_verifier_destroy(&verifier);
            continue;
        }

        memcpy(rejected_signature, signature, sizeof(rejected_signature));
        rejected_signature[i] = rejected_signature[i] == '0' ? '1' : '0';
        if (voice_auth_verifier_verify(
                &verifier, "POST", "/v1/voice/turns", "boundary-user",
                INT64_C(1787572800), nonce, body, sizeof(body),
                rejected_signature) != VOICE_AUTH_ERR_SIGNATURE ||
            voice_auth_verifier_verify(
                &verifier, "POST", "/v1/voice/turns", "boundary-user",
                INT64_C(1787572800), nonce, body, sizeof(body), signature) !=
                VOICE_AUTH_OK)
            rejection_recovers = 0;

        if (voice_auth_identity_issue(
                (const char *)secret, secret_len, "boundary-request",
                "boundary-user", 1, INT64_C(1787572800), 60,
                identity_token, sizeof(identity_token)) != VOICE_AUTH_OK ||
            voice_auth_verifier_identity_verify(
                &verifier, identity_token, "boundary-request",
                INT64_C(1787572800), &claims) != VOICE_AUTH_OK)
            identity_matches = 0;
        else {
            memcpy(
                rejected_identity, identity_token, strlen(identity_token) + 1u);
            j = strlen(rejected_identity) - 1u;
            rejected_identity[j] = rejected_identity[j] == '0' ? '1' : '0';
            if (voice_auth_verifier_identity_verify(
                    &verifier, rejected_identity, "boundary-request",
                    INT64_C(1787572800), &claims) != VOICE_AUTH_ERR_SIGNATURE ||
                voice_auth_verifier_identity_verify(
                    &verifier, identity_token, "boundary-request",
                    INT64_C(1787572800), &claims) != VOICE_AUTH_OK)
                rejection_recovers = 0;
        }
        voice_auth_verifier_destroy(&verifier);
    }

    expect(request_matches,
           "prepared verifier matches HMAC key block boundaries");
    expect(identity_matches,
           "prepared identity verifier matches HMAC key block boundaries");
    expect(rejection_recovers,
           "prepared verifier recovers across HMAC key block boundaries");
}

int main(void) {
    static const char secret[] =
        "test-auth-secret-0123456789abcdef-0123456789abcdef";
    static const char fixed_nonce[] = "00112233445566778899aabbccddeeff";
    static const char fixed_signature[] =
        "c87755b6f04c8633e936a8806b1142c96de489bad8be73621d81b05a872d87ea";
    static const uint8_t body[] = {0x0a, 0x03, 'r', 'e', 'q'};
    uint8_t changed_body[sizeof(body)];
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char second_nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char changed_nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char maximum_replay_key[127];
    char oversized_replay_key[128] = {0};
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char vector_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char changed_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    char oversized_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 2u];
    char identity_token[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    char changed_identity[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    char short_identity[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    voice_auth_identity_claims identity;
    voice_auth_identity_claims prepared_identity;
    voice_auth_nonce_entry replay_entries[2];
    voice_auth_nonce_cache replay_index;
    voice_auth_replay_cache request_index;
    voice_auth_nonce_pool nonce_pool = {0};
    const voice_auth_nonce_pool empty_nonce_pool = {0};
    char pooled_nonces[VOICE_AUTH_NONCE_POOL_CAPACITY + 1u]
        [VOICE_AUTH_NONCE_HEX_LEN + 1u];
    voice_auth_verifier verifier = {0};
    int64_t timestamp = INT64_C(1787572800);
    size_t i;
    size_t j;

    memset(replay_entries, 0, sizeof(replay_entries));
    memset(&replay_index, 0, sizeof(replay_index));
    memset(&request_index, 0, sizeof(request_index));
    memset(maximum_replay_key, 'r', sizeof(maximum_replay_key));
    expect(voice_auth_random_warmup() == VOICE_AUTH_OK, "CSPRNG warmup");
    expect(voice_auth_nonce_pool_init(NULL) == VOICE_AUTH_ERR_ARGUMENT,
           "nonce pool rejects a null initializer");
    expect(voice_auth_nonce_pool_next(NULL, nonce) == VOICE_AUTH_ERR_ARGUMENT,
           "nonce pool rejects a null state");
    expect(voice_auth_random_nonce(nonce) == VOICE_AUTH_OK, "first nonce generation");
    expect(voice_auth_random_nonce(second_nonce) == VOICE_AUTH_OK, "second nonce generation");
    expect(lowercase_hex(nonce, VOICE_AUTH_NONCE_HEX_LEN), "nonce is canonical hex");
    expect(strcmp(nonce, second_nonce) != 0, "independent nonces differ");
    expect(voice_auth_nonce_pool_init(&nonce_pool) == VOICE_AUTH_OK,
           "nonce pool initializes");
    expect(voice_auth_nonce_pool_init(&nonce_pool) == VOICE_AUTH_ERR_ARGUMENT,
           "nonce pool rejects double initialization");
    expect(voice_auth_nonce_pool_next(&nonce_pool, NULL) == VOICE_AUTH_ERR_ARGUMENT,
           "nonce pool rejects a null output");
    for (i = 0; i < VOICE_AUTH_NONCE_POOL_CAPACITY + 1u; ++i) {
        expect(voice_auth_nonce_pool_next(&nonce_pool, pooled_nonces[i]) == VOICE_AUTH_OK,
               "nonce pool crosses a refill boundary");
        expect(lowercase_hex(pooled_nonces[i], VOICE_AUTH_NONCE_HEX_LEN),
               "pooled nonce is canonical hex");
        for (j = 0; j < i; ++j)
            expect(strcmp(pooled_nonces[i], pooled_nonces[j]) != 0,
                   "pooled sample has no nonce collision");
    }
    expect(bytes_zero(
               nonce_pool.bytes, VOICE_AUTH_NONCE_HEX_LEN / 2u),
           "nonce pool cleanses consumed random bytes");
    voice_auth_nonce_pool_destroy(&nonce_pool);
    expect(memcmp(&nonce_pool, &empty_nonce_pool, sizeof(nonce_pool)) == 0,
           "nonce pool destroy cleanses buffered randomness");
    voice_auth_nonce_pool_destroy(NULL);
    expect(voice_auth_nonce_pool_next(&nonce_pool, pooled_nonces[0]) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "destroyed nonce pool rejects use");
    expect(benchmark_nonce_pool() == 0, "nonce pool benchmark succeeds");
    expect(voice_auth_sign(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), signature) == VOICE_AUTH_OK,
           "sign canonical request");
    expect(voice_auth_sign(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, fixed_nonce, body, sizeof(body), vector_signature) ==
               VOICE_AUTH_OK &&
               strcmp(vector_signature, fixed_signature) == 0,
           "canonical request signature matches fixed vector");
    expect(lowercase_hex(signature, VOICE_AUTH_SIGNATURE_HEX_LEN),
           "signature is canonical hex");
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), signature) == VOICE_AUTH_OK,
           "verify canonical request");
    expect(voice_auth_verifier_init(
               &verifier, secret, sizeof(secret) - 1u) == VOICE_AUTH_OK,
           "prepared verifier initializes");
    expect(voice_auth_verifier_init(
               &verifier, secret, sizeof(secret) - 1u) == VOICE_AUTH_ERR_ARGUMENT,
           "prepared verifier rejects double initialization");
    test_prepared_verifier_sequence(
        &verifier, secret, sizeof(secret) - 1u);
    test_prepared_verifier_secret_boundaries();
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), signature) == VOICE_AUTH_OK,
           "prepared verifier accepts canonical request");
    expect(benchmark_verify(
               &verifier, secret, sizeof(secret) - 1u, timestamp, nonce,
               body, sizeof(body), signature) == 0,
           "prepared verifier benchmark succeeds");
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "GET", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), signature) != VOICE_AUTH_OK,
           "method is signed");
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns/cancel", "user-1",
               timestamp, nonce, body, sizeof(body), signature) != VOICE_AUTH_OK,
           "path is signed");
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-2",
               timestamp, nonce, body, sizeof(body), signature) != VOICE_AUTH_OK,
           "user is signed");
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp + 1, nonce, body, sizeof(body), signature) != VOICE_AUTH_OK,
           "timestamp is signed");
    snprintf(changed_nonce, sizeof(changed_nonce), "%s", nonce);
    changed_nonce[0] = changed_nonce[0] == '0' ? '1' : '0';
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, changed_nonce, body, sizeof(body), signature) != VOICE_AUTH_OK,
           "nonce is signed");
    memcpy(changed_body, body, sizeof(body));
    changed_body[sizeof(changed_body) - 1u] ^= 1u;
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, changed_body, sizeof(changed_body), signature) != VOICE_AUTH_OK,
           "body is signed");
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, changed_body, sizeof(changed_body), signature) != VOICE_AUTH_OK,
           "prepared verifier signs the body");
    snprintf(changed_signature, sizeof(changed_signature), "%s", signature);
    changed_signature[0] = changed_signature[0] == '0' ? '1' : '0';
    expect(voice_auth_verify(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), changed_signature) != VOICE_AUTH_OK,
           "changed signature fails");
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), changed_signature) != VOICE_AUTH_OK,
           "prepared verifier rejects a changed signature");
    {
        int rejects_every_position = 1;
        size_t signature_at;
        for (signature_at = 0; signature_at < VOICE_AUTH_SIGNATURE_HEX_LEN;
             ++signature_at) {
            char saved = signature[signature_at];
            signature[signature_at] = saved == '0' ? '1' : '0';
            if (voice_auth_verifier_verify(
                    &verifier, "POST", "/v1/voice/turns", "user-1",
                    timestamp, nonce, body, sizeof(body), signature) ==
                VOICE_AUTH_OK)
                rejects_every_position = 0;
            signature[signature_at] = saved;
        }
        expect(rejects_every_position,
               "prepared verifier compares every signature digit");
    }
    snprintf(changed_signature, sizeof(changed_signature), "%s", signature);
    changed_signature[0] = 'A';
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), changed_signature) != VOICE_AUTH_OK,
           "prepared verifier rejects uppercase signature hex");
    changed_signature[0] = 'g';
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), changed_signature) != VOICE_AUTH_OK,
           "prepared verifier rejects non-hex signature text");
    snprintf(changed_signature, sizeof(changed_signature), "%s", signature);
    changed_signature[VOICE_AUTH_SIGNATURE_HEX_LEN - 1u] = '\0';
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), changed_signature) != VOICE_AUTH_OK,
           "prepared verifier rejects a short signature");
    memcpy(oversized_signature, signature, sizeof(signature));
    oversized_signature[VOICE_AUTH_SIGNATURE_HEX_LEN] = '0';
    oversized_signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u] = '\0';
    expect(voice_auth_verifier_verify(
               &verifier, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), oversized_signature) != VOICE_AUTH_OK,
           "prepared verifier rejects a long signature");
    expect(voice_auth_sign(
               "short", sizeof("short") - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, body, sizeof(body), signature) == VOICE_AUTH_ERR_ARGUMENT,
           "short secret fails closed");
    snprintf(changed_nonce, sizeof(changed_nonce), "%s", nonce);
    changed_nonce[0] = 'A';
    expect(voice_auth_sign(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, changed_nonce, body, sizeof(body), signature) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "noncanonical nonce fails closed");
    expect(voice_auth_sign(
               secret, sizeof(secret) - 1u, "POST", "/v1/voice/turns", "user-1",
               timestamp, nonce, NULL, 0, signature) == VOICE_AUTH_OK,
           "empty body is canonical");
    expect(voice_auth_nonce_cache_accept(
               replay_entries, 2, nonce, timestamp, 60) == VOICE_AUTH_OK,
           "nonce cache accepts first nonce");
    expect(voice_auth_nonce_cache_accept(
               replay_entries, 2, nonce, timestamp, 60) == VOICE_AUTH_ERR_REPLAY,
           "nonce cache rejects replay");
    expect(voice_auth_nonce_cache_accept(
               replay_entries, 2, second_nonce, timestamp, 60) == VOICE_AUTH_OK,
           "nonce cache accepts second nonce");
    snprintf(changed_nonce, sizeof(changed_nonce), "%s", nonce);
    changed_nonce[0] = changed_nonce[0] == '0' ? '1' : '0';
    expect(voice_auth_nonce_cache_accept(
               replay_entries, 2, changed_nonce, timestamp, 60) == VOICE_AUTH_ERR_CAPACITY,
           "nonce cache fails closed at capacity");
    expect(voice_auth_nonce_cache_accept(
               replay_entries, 2, changed_nonce, timestamp + 61, 60) == VOICE_AUTH_OK,
           "nonce cache reuses expired entry");

    expect(voice_auth_nonce_index_init(&replay_index, 2) == VOICE_AUTH_OK,
           "indexed nonce cache initializes");
    expect(voice_auth_nonce_index_accept(
               &replay_index, nonce, timestamp, 60) == VOICE_AUTH_OK,
           "indexed cache accepts first nonce");
    expect(voice_auth_nonce_index_accept(
               &replay_index, nonce, timestamp, 60) == VOICE_AUTH_ERR_REPLAY,
           "indexed cache rejects replay");
    expect(voice_auth_nonce_index_accept(
               &replay_index, second_nonce, timestamp - 10, 5) == VOICE_AUTH_OK,
           "indexed cache accepts a backward caller clock");
    expect(voice_auth_nonce_index_accept(
               &replay_index, changed_nonce, timestamp - 10, 60) == VOICE_AUTH_ERR_CAPACITY,
           "indexed cache fails closed at capacity");
    expect(voice_auth_nonce_index_accept(
               &replay_index, changed_nonce, timestamp + 1, 60) == VOICE_AUTH_OK,
           "indexed cache expires the earlier heap entry");
    expect(voice_auth_nonce_index_accept(
               &replay_index, changed_nonce, timestamp + 1, 60) == VOICE_AUTH_ERR_REPLAY,
           "indexed cache retains the replacement nonce");
    voice_auth_nonce_index_destroy(&replay_index);
    expect(replay_index.entries == NULL && replay_index.keys == NULL &&
               replay_index.index == NULL &&
               replay_index.heap == NULL && replay_index.count == 0,
           "indexed cache destroy clears state");

    expect(voice_auth_replay_index_init(NULL, 2, 127) == VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects a null initializer");
    expect(voice_auth_replay_index_init(&request_index, 0, 127) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects zero capacity");
    expect(voice_auth_replay_index_init(&request_index, 2, 0) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects a zero key bound");
    expect(voice_auth_replay_index_init(&request_index, 2, 127) == VOICE_AUTH_OK,
           "exact request replay cache initializes");
    expect(voice_auth_replay_index_accept(
               &request_index, NULL, 1, timestamp, 60) == VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects a null key");
    expect(voice_auth_replay_index_accept(
               &request_index, "a", 1, timestamp, 60) == VOICE_AUTH_OK,
           "exact request replay cache accepts its first key");
    expect(voice_auth_replay_index_accept(
               &request_index, "a", 1, timestamp, 60) == VOICE_AUTH_ERR_REPLAY,
           "exact request replay cache rejects the same key");
    expect(voice_auth_replay_index_accept(
               &request_index, "e", 1, timestamp, 60) == VOICE_AUTH_OK,
           "exact request replay cache accepts a second distinct key");
    expect(voice_auth_replay_index_accept(
               &request_index, "a-longer", 8, timestamp, 60) ==
               VOICE_AUTH_ERR_CAPACITY,
           "exact request replay cache fails closed at capacity");
    expect(voice_auth_replay_index_accept(
               &request_index, "a-longer", 8, timestamp + 61, 60) == VOICE_AUTH_OK,
           "exact request replay cache reuses expired capacity");
    expect(voice_auth_replay_index_accept(
               &request_index, "a-longer", 1, timestamp + 61, 60) ==
               VOICE_AUTH_OK,
           "exact request replay cache compares the full key length");
    expect(voice_auth_replay_index_accept(
               &request_index, "a-longer", 1, timestamp + 61, 60) ==
               VOICE_AUTH_ERR_REPLAY,
           "exact request replay cache retains the distinct prefix key");
    expect(voice_auth_replay_index_accept(
               &request_index, "", 0, timestamp, 60) == VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects an empty key");
    expect(voice_auth_replay_index_accept(
               &request_index, "time", 4, 0, 60) == VOICE_AUTH_ERR_ARGUMENT &&
               voice_auth_replay_index_accept(
                   &request_index, "time", 4, timestamp, 0) ==
                   VOICE_AUTH_ERR_ARGUMENT &&
               voice_auth_replay_index_accept(
                   &request_index, "time", 4, INT64_MAX, 1) ==
                   VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects invalid lifetimes");
    expect(voice_auth_replay_index_accept(
               &request_index, oversized_replay_key,
               sizeof(oversized_replay_key), timestamp, 60) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "exact request replay cache rejects an oversized key");
    expect(voice_auth_replay_index_bytes(&request_index) > sizeof(request_index),
           "exact request replay cache reports owned storage");
    voice_auth_replay_index_destroy(&request_index);
    expect(request_index.entries == NULL && request_index.keys == NULL &&
               request_index.index == NULL && request_index.heap == NULL &&
               request_index.count == 0,
           "exact request replay cache destroy clears state");
    expect(voice_auth_replay_index_bytes(&request_index) == 0,
           "destroyed exact request replay cache owns no storage");
    expect(voice_auth_replay_index_init(&request_index, 1, 127) == VOICE_AUTH_OK &&
               voice_auth_replay_index_accept(
                   &request_index, maximum_replay_key,
                   sizeof(maximum_replay_key), timestamp, 60) == VOICE_AUTH_OK &&
               voice_auth_replay_index_accept(
                   &request_index, maximum_replay_key,
                   sizeof(maximum_replay_key), timestamp, 60) ==
                   VOICE_AUTH_ERR_REPLAY,
           "exact request replay cache accepts its maximum key boundary");
    voice_auth_replay_index_destroy(&request_index);
    voice_auth_replay_index_destroy(NULL);

    expect(voice_auth_identity_issue(
               secret, sizeof(secret) - 1u, "request-1", "user-1", 1,
               timestamp, 90, identity_token, sizeof(identity_token)) == VOICE_AUTH_OK,
           "identity capability issues");
    expect(strncmp(identity_token, "vat1.", sizeof("vat1.") - 1u) == 0 &&
               strlen(identity_token) < sizeof(identity_token),
           "identity capability has bounded versioned form");
    memset(&identity, 0, sizeof(identity));
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, identity_token, "request-1", timestamp,
               &identity) == VOICE_AUTH_OK,
           "identity capability verifies");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-1", timestamp,
               &prepared_identity) == VOICE_AUTH_OK,
           "prepared identity capability verifies");
    expect(strcmp(identity.request_id, "request-1") == 0 &&
               strcmp(identity.user_id, "user-1") == 0 && identity.premium == 1 &&
               identity.issued_at == timestamp && identity.expires_at == timestamp + 90 &&
               lowercase_hex(identity.nonce, VOICE_AUTH_NONCE_HEX_LEN),
           "verified identity claims are exact");
    expect(strcmp(prepared_identity.request_id, identity.request_id) == 0 &&
               strcmp(prepared_identity.user_id, identity.user_id) == 0 &&
               strcmp(prepared_identity.nonce, identity.nonce) == 0 &&
               prepared_identity.premium == identity.premium &&
               prepared_identity.issued_at == identity.issued_at &&
               prepared_identity.expires_at == identity.expires_at,
           "prepared identity claims match one-shot verification");
    expect(correctly_signed_base64url_alias(
               secret, sizeof(secret) - 1u, identity_token,
               changed_identity, sizeof(changed_identity)) == 0 &&
               voice_auth_identity_verify(
                   secret, sizeof(secret) - 1u, changed_identity,
                   "request-1", timestamp, &identity) == VOICE_AUTH_ERR_SIGNATURE &&
               voice_auth_verifier_identity_verify(
                   &verifier, changed_identity, "request-1", timestamp,
                   &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE &&
               voice_auth_identity_issue(
                   secret, sizeof(secret) - 1u, "r", "u", 0,
                   timestamp, 90, short_identity, sizeof(short_identity)) ==
                   VOICE_AUTH_OK &&
               correctly_signed_base64url_alias(
                   secret, sizeof(secret) - 1u, short_identity,
                   changed_identity, sizeof(changed_identity)) == 0 &&
               voice_auth_identity_verify(
                   secret, sizeof(secret) - 1u, changed_identity,
                   "r", timestamp, &identity) == VOICE_AUTH_ERR_SIGNATURE &&
               voice_auth_verifier_identity_verify(
                   &verifier, changed_identity, "r", timestamp,
                   &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity verifier rejects correctly signed base64url aliases");
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, identity_token, "request-2", timestamp,
               &identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity capability is request bound");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-2", timestamp,
               &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE,
           "prepared identity capability is request bound");
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, identity_token, "request-1", timestamp - 1,
               &identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity capability rejects pre-issue use");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-1", timestamp - 1,
               &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE,
           "prepared identity capability rejects pre-issue use");
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, identity_token, "request-1", timestamp + 90,
               &identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity capability rejects expiry boundary");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-1", timestamp + 90,
               &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE,
           "prepared identity capability rejects expiry boundary");
    snprintf(changed_identity, sizeof(changed_identity), "%s", identity_token);
    changed_identity[strlen(changed_identity) - 1u] =
        changed_identity[strlen(changed_identity) - 1u] == '0' ? '1' : '0';
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, changed_identity, "request-1", timestamp,
               &identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity capability rejects a changed MAC");
    expect(voice_auth_verifier_identity_verify(
               &verifier, changed_identity, "request-1", timestamp,
               &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE &&
               voice_auth_verifier_identity_verify(
                   &verifier, identity_token, "request-1", timestamp,
                   &prepared_identity) == VOICE_AUTH_OK,
           "prepared identity verifier recovers after a changed MAC");
    {
        int rejects_every_mac_digit = 1;
        int clears_rejected_claims = 1;
        size_t mac_at;
        for (mac_at = 0; mac_at < VOICE_AUTH_SIGNATURE_HEX_LEN; ++mac_at) {
            char *mac;
            snprintf(changed_identity, sizeof(changed_identity), "%s", identity_token);
            mac = strrchr(changed_identity, '.');
            if (!mac) {
                rejects_every_mac_digit = 0;
                clears_rejected_claims = 0;
                break;
            }
            mac[1u + mac_at] = mac[1u + mac_at] == '0' ? '1' : '0';
            memset(&identity, 0xa5, sizeof(identity));
            if (voice_auth_identity_verify(
                    secret, sizeof(secret) - 1u, changed_identity,
                    "request-1", timestamp, &identity) !=
                VOICE_AUTH_ERR_SIGNATURE)
                rejects_every_mac_digit = 0;
            if (!bytes_zero(
                    (const unsigned char *)&identity, sizeof(identity)))
                clears_rejected_claims = 0;
            memset(&prepared_identity, 0xa5, sizeof(prepared_identity));
            if (voice_auth_verifier_identity_verify(
                    &verifier, changed_identity, "request-1", timestamp,
                    &prepared_identity) != VOICE_AUTH_ERR_SIGNATURE)
                rejects_every_mac_digit = 0;
            if (!bytes_zero(
                    (const unsigned char *)&prepared_identity,
                    sizeof(prepared_identity)))
                clears_rejected_claims = 0;
        }
        expect(rejects_every_mac_digit,
               "identity verifier compares every MAC digit");
        expect(clears_rejected_claims,
               "identity verifier clears claims after rejected MAC sequence");
        expect(voice_auth_identity_verify(
                   secret, sizeof(secret) - 1u, identity_token,
                   "request-1", timestamp, &identity) == VOICE_AUTH_OK &&
                   voice_auth_verifier_identity_verify(
                       &verifier, identity_token, "request-1", timestamp,
                       &prepared_identity) == VOICE_AUTH_OK,
               "prepared identity verifier recovers after rejected MAC sequence");
    }
    snprintf(changed_identity, sizeof(changed_identity), "%s", identity_token);
    {
        char *mac = strrchr(changed_identity, '.');
        if (mac && mac[1] >= 'a' && mac[1] <= 'f') mac[1] = (char)(mac[1] - 'a' + 'A');
        else if (mac) mac[1] = 'A';
    }
    expect(voice_auth_identity_verify(
               secret, sizeof(secret) - 1u, changed_identity, "request-1", timestamp,
               &identity) == VOICE_AUTH_ERR_SIGNATURE,
           "identity capability rejects noncanonical MAC text");
    expect(voice_auth_verifier_identity_verify(
               &verifier, changed_identity, "request-1", timestamp,
               &prepared_identity) == VOICE_AUTH_ERR_SIGNATURE,
           "prepared identity verifier rejects noncanonical MAC text");
    expect(voice_auth_verifier_identity_verify(
               NULL, identity_token, "request-1", timestamp,
               &prepared_identity) == VOICE_AUTH_ERR_ARGUMENT,
           "prepared identity verifier rejects null state");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-1", timestamp, NULL) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "prepared identity verifier rejects null claims");
    {
        char maximum_request_id[128];
        char maximum_user_id[128];
        char maximum_identity[VOICE_AUTH_IDENTITY_TOKEN_CAP];
        memset(maximum_request_id, 'r', sizeof(maximum_request_id) - 1u);
        maximum_request_id[sizeof(maximum_request_id) - 1u] = '\0';
        memset(maximum_user_id, 'u', sizeof(maximum_user_id) - 1u);
        maximum_user_id[sizeof(maximum_user_id) - 1u] = '\0';
        expect(voice_auth_identity_issue(
                   secret, sizeof(secret) - 1u, maximum_request_id,
                   maximum_user_id, 0, timestamp, 90, maximum_identity,
                   sizeof(maximum_identity)) == VOICE_AUTH_OK &&
                   voice_auth_verifier_identity_verify(
                       &verifier, maximum_identity, maximum_request_id,
                       timestamp, &prepared_identity) == VOICE_AUTH_OK &&
                   strcmp(prepared_identity.request_id, maximum_request_id) == 0 &&
                   strcmp(prepared_identity.user_id, maximum_user_id) == 0,
               "prepared identity verifier accepts maximum bounded claims");
    }
    expect(benchmark_identity_verify(
               &verifier, secret, sizeof(secret) - 1u, identity_token,
               "request-1", timestamp) == 0,
           "prepared identity benchmark succeeds");
    expect(voice_auth_identity_issue(
               secret, sizeof(secret) - 1u, "request-1", "user-1", 0,
               timestamp, VOICE_AUTH_IDENTITY_TTL_MAX_SECONDS + 1, identity_token,
               sizeof(identity_token)) == VOICE_AUTH_ERR_ARGUMENT,
           "identity capability rejects excessive lifetime");
    expect(voice_auth_identity_issue(
               "short", sizeof("short") - 1u, "request-1", "user-1", 0,
               timestamp, 90, identity_token, sizeof(identity_token)) ==
               VOICE_AUTH_ERR_ARGUMENT,
           "identity capability rejects short secret");

    voice_auth_verifier_destroy(&verifier);
    expect(verifier.context == NULL, "prepared verifier destroy clears state");
    expect(voice_auth_verifier_identity_verify(
               &verifier, identity_token, "request-1", timestamp,
               &prepared_identity) == VOICE_AUTH_ERR_ARGUMENT,
           "destroyed verifier rejects prepared identity verification");
    voice_auth_verifier_destroy(&verifier);
    if (failures != 0) return 1;
    printf("ALL PASS voice auth\n");
    return 0;
}
