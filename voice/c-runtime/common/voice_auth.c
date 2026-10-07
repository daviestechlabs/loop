/* Bounded HMAC authentication for the in-cluster voice HTTP edge. */
#define _POSIX_C_SOURCE 200809L

#include "voice_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOICE_AUTH_REQUEST_DOMAIN "voice-hmac-v1\n"
#define VOICE_AUTH_METHOD_MAX 16u
#define VOICE_AUTH_PATH_MAX 512u
#define VOICE_AUTH_USER_MAX 127u
#define VOICE_AUTH_POSITIVE_I64_DECIMAL_MAX 19u
#define VOICE_AUTH_CANONICAL_CAP \
    (sizeof(VOICE_AUTH_REQUEST_DOMAIN) - 1u + VOICE_AUTH_METHOD_MAX + \
     VOICE_AUTH_PATH_MAX + VOICE_AUTH_USER_MAX + \
     VOICE_AUTH_POSITIVE_I64_DECIMAL_MAX + VOICE_AUTH_NONCE_HEX_LEN + \
     VOICE_AUTH_SIGNATURE_HEX_LEN + 5u + 1u)
#define VOICE_AUTH_INDEX_NONE UINT32_MAX
#define VOICE_AUTH_IDENTITY_PREFIX "vat1."
#define VOICE_AUTH_IDENTITY_DOMAIN "voice-webtransport-identity-v1\n"

_Static_assert(
    sizeof(((voice_auth_nonce_pool *)0)->bytes) <= INT_MAX,
    "nonce pool must fit the RAND_bytes length type");

#if OPENSSL_VERSION_NUMBER >= 0x30000000L && \
    !defined(OPENSSL_IS_AWSLC) && !defined(OPENSSL_IS_BORINGSSL)
#define VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE 1
#else
#define VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE 0
#endif

struct voice_auth_verifier_context {
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    EVP_MD_CTX *digest_template;
    EVP_MD_CTX *digest_context;
    EVP_MD_CTX *hmac_inner_template;
    EVP_MD_CTX *hmac_outer_template;
    EVP_MD_CTX *hmac_context;
#else
    SHA256_CTX digest_template;
    SHA256_CTX digest_context;
    SHA256_CTX hmac_inner_template;
    SHA256_CTX hmac_outer_template;
    SHA256_CTX hmac_context;
#endif
};

static unsigned char *voice_hmac_sha256(
    const void *key,
    size_t key_len,
    const unsigned char *data,
    size_t data_len,
    unsigned char *out,
    unsigned int *out_len
) {
#if defined(OPENSSL_IS_AWSLC) || defined(OPENSSL_IS_BORINGSSL)
    return HMAC(EVP_sha256(), key, key_len, data, data_len, out, out_len);
#else
    if (key_len > (size_t)INT_MAX) return NULL;
    return HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, out_len);
#endif
}

static int voice_hmac_prepared(
    voice_auth_verifier_context *context,
    const unsigned char *data,
    size_t data_len,
    unsigned char *out,
    size_t out_cap,
    size_t *out_len
) {
    unsigned char inner_digest[SHA256_DIGEST_LENGTH];
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    unsigned int inner_length = 0;
    unsigned int length = 0;
#endif
    if (out_len) *out_len = 0;
    if (!context || !data || !out || out_cap < SHA256_DIGEST_LENGTH || !out_len)
        return VOICE_AUTH_ERR_ARGUMENT;
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    if (EVP_MD_CTX_copy_ex(
            context->hmac_context, context->hmac_inner_template) != 1 ||
        EVP_DigestUpdate(context->hmac_context, data, data_len) != 1 ||
        EVP_DigestFinal_ex(
            context->hmac_context, inner_digest, &inner_length) != 1 ||
        inner_length != SHA256_DIGEST_LENGTH ||
        EVP_MD_CTX_copy_ex(
            context->hmac_context, context->hmac_outer_template) != 1 ||
        EVP_DigestUpdate(
            context->hmac_context, inner_digest, sizeof(inner_digest)) != 1 ||
        EVP_DigestFinal_ex(context->hmac_context, out, &length) != 1 ||
        length != SHA256_DIGEST_LENGTH) {
        OPENSSL_cleanse(inner_digest, sizeof(inner_digest));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    OPENSSL_cleanse(inner_digest, sizeof(inner_digest));
#else
    /* Restore the keyed inner state without rebuilding an HMAC context. */
    context->hmac_context = context->hmac_inner_template;
    if (SHA256_Update(&context->hmac_context, data, data_len) != 1 ||
        SHA256_Final(inner_digest, &context->hmac_context) != 1) {
        OPENSSL_cleanse(inner_digest, sizeof(inner_digest));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    context->hmac_context = context->hmac_outer_template;
    if (SHA256_Update(
            &context->hmac_context,
            inner_digest,
            sizeof(inner_digest)) != 1 ||
        SHA256_Final(out, &context->hmac_context) != 1) {
        OPENSSL_cleanse(inner_digest, sizeof(inner_digest));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    OPENSSL_cleanse(inner_digest, sizeof(inner_digest));
#endif
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    *out_len = (size_t)length;
#else
    *out_len = SHA256_DIGEST_LENGTH;
#endif
    return VOICE_AUTH_OK;
}

#define VOICE_AUTH_IDENTITY_PAYLOAD_CAP 296u
#define VOICE_AUTH_IDENTITY_ENCODED_CAP 400u

struct voice_auth_replay_index_entry {
    int64_t expires_at;
    uint32_t heap_position;
    uint32_t index_position;
    uint32_t next_free;
    uint32_t key_len;
};

static void hex_encode(const unsigned char *input, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; ++i) {
        out[i * 2u] = digits[input[i] >> 4u];
        out[i * 2u + 1u] = digits[input[i] & 15u];
    }
    out[len * 2u] = '\0';
}

static int lowercase_hex_matches(
    const unsigned char *input,
    size_t input_len,
    const char *hex
) {
    unsigned mismatch = 0;
    size_t i;
    if (!input || !hex) return 0;
    for (i = 0; i < input_len; ++i) {
        unsigned high = (unsigned)input[i] >> 4u;
        unsigned low = (unsigned)input[i] & 15u;
        high += (unsigned)'0' +
            ((9u - high) >> (sizeof(unsigned) * CHAR_BIT - 1u)) * 39u;
        low += (unsigned)'0' +
            ((9u - low) >> (sizeof(unsigned) * CHAR_BIT - 1u)) * 39u;
        mismatch |= high ^ (unsigned)(unsigned char)hex[i * 2u];
        mismatch |= low ^ (unsigned)(unsigned char)hex[i * 2u + 1u];
    }
    return mismatch == 0u;
}

static size_t positive_decimal(int64_t value, char out[20]) {
    char reverse[20];
    uint64_t remaining;
    size_t length = 0;
    size_t i;

    if (value <= 0 || !out) return 0;
    remaining = (uint64_t)value;
    do {
        reverse[length++] = (char)('0' + remaining % UINT64_C(10));
        remaining /= UINT64_C(10);
    } while (remaining != 0);
    for (i = 0; i < length; ++i) out[i] = reverse[length - i - 1u];
    return length;
}

static size_t write_request_canonical(
    char *out,
    size_t out_cap,
    const char *method,
    const char *path,
    const char *user,
    int64_t timestamp,
    const char *nonce,
    const char digest_hex[SHA256_DIGEST_LENGTH * 2u + 1u]
) {
    static const char domain[] = VOICE_AUTH_REQUEST_DOMAIN;
    char timestamp_text[20];
    size_t method_len;
    size_t path_len;
    size_t user_len;
    size_t timestamp_len;
    size_t required;
    size_t length = 0;

    if (!out || out_cap == 0u || !method || !path || !user || !nonce ||
        !digest_hex) return 0;
    timestamp_len = positive_decimal(timestamp, timestamp_text);
    if (timestamp_len == 0u) return 0;
    method_len = strlen(method);
    path_len = strlen(path);
    user_len = strlen(user);
    required = sizeof(domain) - 1u + method_len + path_len + user_len +
        timestamp_len + VOICE_AUTH_NONCE_HEX_LEN +
        SHA256_DIGEST_LENGTH * 2u + 5u;
    if (required >= out_cap) return 0;
#define COPY_CANONICAL(value, value_len) do { \
        memcpy(out + length, (value), (value_len)); \
        length += (value_len); \
    } while (0)
    COPY_CANONICAL(domain, sizeof(domain) - 1u);
    COPY_CANONICAL(method, method_len);
    out[length++] = '\n';
    COPY_CANONICAL(path, path_len);
    out[length++] = '\n';
    COPY_CANONICAL(user, user_len);
    out[length++] = '\n';
    COPY_CANONICAL(timestamp_text, timestamp_len);
    out[length++] = '\n';
    COPY_CANONICAL(nonce, VOICE_AUTH_NONCE_HEX_LEN);
    out[length++] = '\n';
    COPY_CANONICAL(digest_hex, SHA256_DIGEST_LENGTH * 2u);
#undef COPY_CANONICAL
    out[length] = '\0';
    return length;
}

static int simple_token(const char *value, size_t max_len) {
    const unsigned char *p = (const unsigned char *)value;
    size_t len = 0;
    if (!p || !*p) return 0;
    while (*p) {
        if (*p <= 0x20u || *p >= 0x7fu) return 0;
        ++p;
        if (++len > max_len) return 0;
    }
    return 1;
}

static int nonce_valid(const char *nonce) {
    size_t i;
    if (!nonce || strlen(nonce) != VOICE_AUTH_NONCE_HEX_LEN) return 0;
    for (i = 0; i < VOICE_AUTH_NONCE_HEX_LEN; ++i)
        if (!((nonce[i] >= '0' && nonce[i] <= '9') ||
              (nonce[i] >= 'a' && nonce[i] <= 'f'))) return 0;
    return 1;
}

static size_t b64url_encoded_size(size_t input_len) {
    size_t groups;
    if (input_len > (SIZE_MAX - 2u) / 4u * 3u) return 0;
    groups = ((input_len + 2u) / 3u) * 4u;
    if (input_len % 3u != 0) groups -= 3u - input_len % 3u;
    return groups;
}

static int b64url_encode(
    const uint8_t *input,
    size_t input_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t required;
    size_t input_at = 0;
    size_t output_at = 0;
    if (!out || !out_len || (!input && input_len != 0)) return -1;
    *out_len = 0;
    required = b64url_encoded_size(input_len);
    if ((input_len != 0 && required == 0) || required >= out_cap) return -1;
    while (input_at < input_len) {
        uint32_t value = (uint32_t)input[input_at] << 16u;
        size_t remaining = input_len - input_at;
        if (remaining > 1u) value |= (uint32_t)input[input_at + 1u] << 8u;
        if (remaining > 2u) value |= input[input_at + 2u];
        out[output_at++] = alphabet[(value >> 18u) & 63u];
        out[output_at++] = alphabet[(value >> 12u) & 63u];
        if (remaining > 1u) out[output_at++] = alphabet[(value >> 6u) & 63u];
        if (remaining > 2u) out[output_at++] = alphabet[value & 63u];
        input_at += remaining > 3u ? 3u : remaining;
    }
    out[output_at] = '\0';
    *out_len = output_at;
    return 0;
}

static int b64url_value(unsigned char value) {
    static const uint8_t decoded_plus_one[UINT8_MAX + 1u] = {
        ['-'] = 63u,
        ['0'] = 53u, ['1'] = 54u, ['2'] = 55u,
        ['3'] = 56u, ['4'] = 57u, ['5'] = 58u,
        ['6'] = 59u, ['7'] = 60u, ['8'] = 61u,
        ['9'] = 62u,
        ['A'] = 1u, ['B'] = 2u, ['C'] = 3u,
        ['D'] = 4u, ['E'] = 5u, ['F'] = 6u,
        ['G'] = 7u, ['H'] = 8u, ['I'] = 9u,
        ['J'] = 10u, ['K'] = 11u, ['L'] = 12u,
        ['M'] = 13u, ['N'] = 14u, ['O'] = 15u,
        ['P'] = 16u, ['Q'] = 17u, ['R'] = 18u,
        ['S'] = 19u, ['T'] = 20u, ['U'] = 21u,
        ['V'] = 22u, ['W'] = 23u, ['X'] = 24u,
        ['Y'] = 25u, ['Z'] = 26u,
        ['_'] = 64u,
        ['a'] = 27u, ['b'] = 28u, ['c'] = 29u,
        ['d'] = 30u, ['e'] = 31u, ['f'] = 32u,
        ['g'] = 33u, ['h'] = 34u, ['i'] = 35u,
        ['j'] = 36u, ['k'] = 37u, ['l'] = 38u,
        ['m'] = 39u, ['n'] = 40u, ['o'] = 41u,
        ['p'] = 42u, ['q'] = 43u, ['r'] = 44u,
        ['s'] = 45u, ['t'] = 46u, ['u'] = 47u,
        ['v'] = 48u, ['w'] = 49u, ['x'] = 50u,
        ['y'] = 51u, ['z'] = 52u,
    };
    return (int)decoded_plus_one[value] - 1;
}

/* Decode only the unique unpadded base64url spelling of each byte string. */
static int b64url_decode_canonical(
    const char *input,
    size_t input_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    size_t full_input;
    size_t input_at = 0u;
    size_t output_at = 0;
    size_t groups;
    size_t tail;
    size_t tail_bytes;
    size_t required;
    if (!input || !out || !out_len) return -1;
    *out_len = 0;
    tail = input_len & 3u;
    if (tail == 1u) return -1;
    full_input = input_len - tail;
    groups = full_input / 4u;
    tail_bytes = tail != 0u ? tail - 1u : 0u;
    if (groups > (SIZE_MAX - tail_bytes) / 3u) return -1;
    required = groups * 3u + tail_bytes;
    if (required > out_cap) return -1;
    while (input_at < full_input) {
        int a = b64url_value((unsigned char)input[input_at]);
        int b = b64url_value((unsigned char)input[input_at + 1u]);
        int c = b64url_value((unsigned char)input[input_at + 2u]);
        int d = b64url_value((unsigned char)input[input_at + 3u]);
        uint32_t value;
        if ((a | b | c | d) < 0) return -1;
        value = (uint32_t)a << 18u | (uint32_t)b << 12u |
                (uint32_t)c << 6u | (uint32_t)d;
        out[output_at++] = (uint8_t)(value >> 16u);
        out[output_at++] = (uint8_t)(value >> 8u);
        out[output_at++] = (uint8_t)value;
        input_at += 4u;
    }
    if (tail != 0u) {
        int a = b64url_value((unsigned char)input[input_at]);
        int b = b64url_value((unsigned char)input[input_at + 1u]);
        int c = tail == 3u ?
            b64url_value((unsigned char)input[input_at + 2u]) : 0;
        uint32_t value;
        if ((a | b | c) < 0 ||
            (tail == 2u && (b & 15) != 0) ||
            (tail == 3u && (c & 3) != 0))
            return -1;
        value = (uint32_t)a << 18u | (uint32_t)b << 12u |
                (uint32_t)c << 6u;
        out[output_at++] = (uint8_t)(value >> 16u);
        if (tail == 3u) out[output_at++] = (uint8_t)(value >> 8u);
    }
    if (output_at != required) return -1;
    *out_len = output_at;
    return 0;
}

static void put_u16_be(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8u);
    out[1] = (uint8_t)value;
}

static uint16_t get_u16_be(const uint8_t *input) {
    return (uint16_t)((uint16_t)input[0] << 8u | input[1]);
}

static void put_u64_be(uint8_t *out, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8u; ++i) out[i] = (uint8_t)(value >> (56u - i * 8u));
}

static uint64_t get_u64_be(const uint8_t *input) {
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < 8u; ++i) value = value << 8u | input[i];
    return value;
}

static int identity_field_valid(const char *value, size_t max_len) {
    const unsigned char *cursor = (const unsigned char *)value;
    size_t len = 0;
    if (!cursor || !*cursor) return 0;
    while (*cursor) {
        if (*cursor < 0x20u || *cursor == 0x7fu) return 0;
        ++cursor;
        if (++len > max_len) return 0;
    }
    return 1;
}

static int identity_mac(
    const char *secret,
    size_t secret_len,
    const char *encoded,
    size_t encoded_len,
    unsigned char mac[SHA256_DIGEST_LENGTH]
) {
    unsigned char canonical[
        sizeof(VOICE_AUTH_IDENTITY_DOMAIN) - 1u + VOICE_AUTH_IDENTITY_ENCODED_CAP
    ];
    unsigned int mac_len = 0;
    size_t domain_len = sizeof(VOICE_AUTH_IDENTITY_DOMAIN) - 1u;
    if (!secret || secret_len < 32u || secret_len > 255u || !encoded ||
        encoded_len > sizeof(canonical) - domain_len) return VOICE_AUTH_ERR_ARGUMENT;
    memcpy(canonical, VOICE_AUTH_IDENTITY_DOMAIN, domain_len);
    memcpy(canonical + domain_len, encoded, encoded_len);
    if (!voice_hmac_sha256(secret, secret_len, canonical, domain_len + encoded_len,
                           mac, &mac_len) || mac_len != SHA256_DIGEST_LENGTH) {
        OPENSSL_cleanse(canonical, sizeof(canonical));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    OPENSSL_cleanse(canonical, sizeof(canonical));
    return VOICE_AUTH_OK;
}

static int identity_mac_prepared(
    voice_auth_verifier_context *context,
    const char *encoded,
    size_t encoded_len,
    unsigned char mac[SHA256_DIGEST_LENGTH]
) {
    unsigned char canonical[
        sizeof(VOICE_AUTH_IDENTITY_DOMAIN) - 1u + VOICE_AUTH_IDENTITY_ENCODED_CAP
    ];
    size_t domain_len = sizeof(VOICE_AUTH_IDENTITY_DOMAIN) - 1u;
    size_t mac_len = 0;
    int result;
    if (!context || !encoded || encoded_len > sizeof(canonical) - domain_len)
        return VOICE_AUTH_ERR_ARGUMENT;
    memcpy(canonical, VOICE_AUTH_IDENTITY_DOMAIN, domain_len);
    memcpy(canonical + domain_len, encoded, encoded_len);
    result = voice_hmac_prepared(
        context, canonical, domain_len + encoded_len,
        mac, SHA256_DIGEST_LENGTH, &mac_len);
    OPENSSL_cleanse(canonical, sizeof(canonical));
    return result == VOICE_AUTH_OK && mac_len == SHA256_DIGEST_LENGTH ?
        VOICE_AUTH_OK : VOICE_AUTH_ERR_CRYPTO;
}

static int identity_issue(
    const char *secret,
    size_t secret_len,
    const char *request_id,
    const char *user_id,
    int premium,
    int capture,
    int64_t now,
    int64_t ttl_seconds,
    char *out,
    size_t out_cap
) {
    uint8_t payload[VOICE_AUTH_IDENTITY_PAYLOAD_CAP];
    unsigned char random[VOICE_AUTH_NONCE_HEX_LEN / 2u];
    unsigned char mac[SHA256_DIGEST_LENGTH];
    char encoded[VOICE_AUTH_IDENTITY_ENCODED_CAP];
    char mac_hex[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    size_t request_len;
    size_t user_len;
    size_t payload_len;
    size_t encoded_len;
    int written;
    int result = VOICE_AUTH_ERR_ARGUMENT;
    if (out && out_cap) out[0] = '\0';
    if (!out || !identity_field_valid(request_id, 127u) ||
        !identity_field_valid(user_id, 127u) ||
        (premium != 0 && premium != 1) || now <= 0 || ttl_seconds <= 0 ||
        ttl_seconds > VOICE_AUTH_IDENTITY_TTL_MAX_SECONDS || now > INT64_MAX - ttl_seconds)
        return VOICE_AUTH_ERR_ARGUMENT;
    request_len = strlen(request_id);
    user_len = strlen(user_id);
    payload_len = 38u + request_len + user_len;
    if (payload_len > sizeof(payload) || RAND_bytes(random, (int)sizeof(random)) != 1)
        return payload_len > sizeof(payload) ? VOICE_AUTH_ERR_CAPACITY : VOICE_AUTH_ERR_CRYPTO;
    memset(payload, 0, sizeof(payload));
    payload[0] = capture ? 2u : 1u;
    payload[1] = (uint8_t)((premium ? 1u : 0u) | (capture ? 2u : 0u));
    put_u64_be(payload + 2u, (uint64_t)now);
    put_u64_be(payload + 10u, (uint64_t)(now + ttl_seconds));
    memcpy(payload + 18u, random, sizeof(random));
    put_u16_be(payload + 34u, (uint16_t)request_len);
    put_u16_be(payload + 36u, (uint16_t)user_len);
    memcpy(payload + 38u, request_id, request_len);
    memcpy(payload + 38u + request_len, user_id, user_len);
    if (b64url_encode(payload, payload_len, encoded, sizeof(encoded), &encoded_len) != 0) {
        result = VOICE_AUTH_ERR_CAPACITY;
        goto cleanup;
    }
    result = identity_mac(secret, secret_len, encoded, encoded_len, mac);
    if (result != VOICE_AUTH_OK) goto cleanup;
    hex_encode(mac, sizeof(mac), mac_hex);
    written = snprintf(out, out_cap, "%s%s.%s", VOICE_AUTH_IDENTITY_PREFIX, encoded, mac_hex);
    if (written <= 0 || (size_t)written >= out_cap) {
        if (out_cap) out[0] = '\0';
        result = VOICE_AUTH_ERR_CAPACITY;
        goto cleanup;
    }
    result = VOICE_AUTH_OK;
cleanup:
    OPENSSL_cleanse(payload, sizeof(payload));
    OPENSSL_cleanse(random, sizeof(random));
    OPENSSL_cleanse(mac, sizeof(mac));
    OPENSSL_cleanse(mac_hex, sizeof(mac_hex));
    OPENSSL_cleanse(encoded, sizeof(encoded));
    return result;
}

int voice_auth_identity_issue(
    const char *secret, size_t secret_len, const char *request_id,
    const char *user_id, int premium, int64_t now, int64_t ttl_seconds,
    char *out, size_t out_cap
) {
    return identity_issue(secret, secret_len, request_id, user_id, premium, 0,
        now, ttl_seconds, out, out_cap);
}

int voice_auth_identity_issue_capture(
    const char *secret, size_t secret_len, const char *request_id,
    const char *user_id, int premium, int64_t now, int64_t ttl_seconds,
    char *out, size_t out_cap
) {
    return identity_issue(secret, secret_len, request_id, user_id, premium, 1,
        now, ttl_seconds, out, out_cap);
}

static int identity_verify(
    const char *secret,
    size_t secret_len,
    voice_auth_verifier_context *context,
    const char *token,
    const char *request_id,
    int64_t now,
    voice_auth_identity_claims *out
) {
    uint8_t payload[VOICE_AUTH_IDENTITY_PAYLOAD_CAP];
    unsigned char expected_mac[SHA256_DIGEST_LENGTH];
    const char *encoded;
    const char *separator;
    size_t encoded_len;
    size_t payload_len = 0;
    size_t request_len;
    size_t user_len;
    size_t expected_len;
    uint64_t issued;
    uint64_t expires;
    int result = VOICE_AUTH_ERR_SIGNATURE;
    if (out) memset(out, 0, sizeof(*out));
    if ((!context && (!secret || secret_len < 32u || secret_len > 255u)) ||
        !token || !request_id || !out ||
        now <= 0 || strlen(token) >= VOICE_AUTH_IDENTITY_TOKEN_CAP ||
        strncmp(token, VOICE_AUTH_IDENTITY_PREFIX,
                sizeof(VOICE_AUTH_IDENTITY_PREFIX) - 1u) != 0)
        return VOICE_AUTH_ERR_ARGUMENT;
    encoded = token + sizeof(VOICE_AUTH_IDENTITY_PREFIX) - 1u;
    separator = strchr(encoded, '.');
    if (!separator || strchr(separator + 1, '.') ||
        strlen(separator + 1) != VOICE_AUTH_SIGNATURE_HEX_LEN) goto cleanup;
    encoded_len = (size_t)(separator - encoded);
    if (encoded_len == 0 || encoded_len >= VOICE_AUTH_IDENTITY_ENCODED_CAP ||
        b64url_decode_canonical(
            encoded, encoded_len, payload, sizeof(payload), &payload_len) != 0 ||
        (context ? identity_mac_prepared(
                       context, encoded, encoded_len, expected_mac) :
                   identity_mac(
                       secret, secret_len, encoded, encoded_len, expected_mac)) != VOICE_AUTH_OK)
        goto cleanup;
    if (!lowercase_hex_matches(
            expected_mac, sizeof(expected_mac), separator + 1u) ||
        payload_len < 38u ||
        !((payload[0] == 1u && payload[1] <= 1u) ||
          (payload[0] == 2u && (payload[1] == 2u || payload[1] == 3u))) ||
        payload[2] & 0x80u || payload[10] & 0x80u) goto cleanup;
    issued = get_u64_be(payload + 2u);
    expires = get_u64_be(payload + 10u);
    request_len = get_u16_be(payload + 34u);
    user_len = get_u16_be(payload + 36u);
    expected_len = 38u + request_len + user_len;
    if (expected_len != payload_len || request_len == 0 || request_len >= sizeof(out->request_id) ||
        user_len == 0 || user_len >= sizeof(out->user_id) || issued == 0 || expires <= issued ||
        expires - issued > VOICE_AUTH_IDENTITY_TTL_MAX_SECONDS || (uint64_t)now < issued ||
        (uint64_t)now >= expires || strlen(request_id) != request_len ||
        memcmp(payload + 38u, request_id, request_len) != 0) goto cleanup;
    memcpy(out->request_id, payload + 38u, request_len);
    memcpy(out->user_id, payload + 38u + request_len, user_len);
    if (!identity_field_valid(out->request_id, 127u) ||
        !identity_field_valid(out->user_id, 127u)) goto cleanup;
    hex_encode(payload + 18u, VOICE_AUTH_NONCE_HEX_LEN / 2u, out->nonce);
    out->issued_at = (int64_t)issued;
    out->expires_at = (int64_t)expires;
    out->premium = (payload[1] & 1u) != 0;
    out->model_request_capture = (payload[1] & 2u) != 0;
    result = VOICE_AUTH_OK;
cleanup:
    if (result != VOICE_AUTH_OK) memset(out, 0, sizeof(*out));
    OPENSSL_cleanse(payload, sizeof(payload));
    OPENSSL_cleanse(expected_mac, sizeof(expected_mac));
    return result;
}

int voice_auth_identity_verify(
    const char *secret,
    size_t secret_len,
    const char *token,
    const char *request_id,
    int64_t now,
    voice_auth_identity_claims *out
) {
    return identity_verify(
        secret, secret_len, NULL, token, request_id, now, out);
}

int voice_auth_verifier_identity_verify(
    voice_auth_verifier *verifier,
    const char *token,
    const char *request_id,
    int64_t now,
    voice_auth_identity_claims *out
) {
    if (!verifier || !verifier->context) {
        if (out) memset(out, 0, sizeof(*out));
        return VOICE_AUTH_ERR_ARGUMENT;
    }
    return identity_verify(
        NULL, 0u, verifier->context, token, request_id, now, out);
}

int voice_auth_random_warmup(void) {
    unsigned char random[32];
    int result = RAND_bytes(random, (int)sizeof(random)) == 1 ?
        VOICE_AUTH_OK : VOICE_AUTH_ERR_CRYPTO;
    OPENSSL_cleanse(random, sizeof(random));
    return result;
}

int voice_auth_random_nonce(char out[VOICE_AUTH_NONCE_HEX_LEN + 1u]) {
    unsigned char random[VOICE_AUTH_NONCE_HEX_LEN / 2u];
    if (!out) return VOICE_AUTH_ERR_ARGUMENT;
    if (RAND_bytes(random, (int)sizeof(random)) != 1) {
        out[0] = '\0';
        return VOICE_AUTH_ERR_CRYPTO;
    }
    hex_encode(random, sizeof(random), out);
    OPENSSL_cleanse(random, sizeof(random));
    return VOICE_AUTH_OK;
}

int voice_auth_nonce_pool_init(voice_auth_nonce_pool *pool) {
    if (!pool || pool->initialized) return VOICE_AUTH_ERR_ARGUMENT;
    memset(pool, 0, sizeof(*pool));
    if (RAND_bytes(pool->bytes, (int)sizeof(pool->bytes)) != 1) {
        OPENSSL_cleanse(pool, sizeof(*pool));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    pool->initialized = 1;
    return VOICE_AUTH_OK;
}

int voice_auth_nonce_pool_next(
    voice_auth_nonce_pool *pool,
    char out[VOICE_AUTH_NONCE_HEX_LEN + 1u]
) {
    unsigned char *random;
    if (!pool || !pool->initialized || !out) return VOICE_AUTH_ERR_ARGUMENT;
    if (pool->next >= VOICE_AUTH_NONCE_POOL_CAPACITY) {
        if (RAND_bytes(pool->bytes, (int)sizeof(pool->bytes)) != 1) {
            out[0] = '\0';
            OPENSSL_cleanse(pool->bytes, sizeof(pool->bytes));
            return VOICE_AUTH_ERR_CRYPTO;
        }
        pool->next = 0;
    }
    random = pool->bytes + pool->next * (VOICE_AUTH_NONCE_HEX_LEN / 2u);
    hex_encode(random, VOICE_AUTH_NONCE_HEX_LEN / 2u, out);
    OPENSSL_cleanse(random, VOICE_AUTH_NONCE_HEX_LEN / 2u);
    pool->next++;
    return VOICE_AUTH_OK;
}

void voice_auth_nonce_pool_destroy(voice_auth_nonce_pool *pool) {
    if (pool) OPENSSL_cleanse(pool, sizeof(*pool));
}

int voice_auth_sign(
    const char *secret,
    size_t secret_len,
    const char *method,
    const char *path,
    const char *user,
    int64_t timestamp,
    const char *nonce,
    const uint8_t *body,
    size_t body_len,
    char out[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u]
) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char mac[EVP_MAX_MD_SIZE];
    char digest_hex[SHA256_DIGEST_LENGTH * 2u + 1u];
    char canonical[VOICE_AUTH_CANONICAL_CAP];
    unsigned int mac_len = 0;
    size_t canonical_len;
    if (!out || !secret || secret_len < 32u || secret_len > 255u ||
        !simple_token(method, VOICE_AUTH_METHOD_MAX) ||
        !simple_token(path, VOICE_AUTH_PATH_MAX) ||
        !simple_token(user, VOICE_AUTH_USER_MAX) ||
        !nonce_valid(nonce) || timestamp <= 0 ||
        (!body && body_len != 0)) return VOICE_AUTH_ERR_ARGUMENT;
    if (!SHA256(body, body_len, digest)) return VOICE_AUTH_ERR_CRYPTO;
    hex_encode(digest, sizeof(digest), digest_hex);
    canonical_len = write_request_canonical(
        canonical, sizeof(canonical), method, path, user, timestamp, nonce,
        digest_hex);
    if (canonical_len == 0u) {
        OPENSSL_cleanse(digest, sizeof(digest));
        return VOICE_AUTH_ERR_CAPACITY;
    }
    if (!voice_hmac_sha256(
            secret, secret_len, (const unsigned char *)canonical,
            canonical_len, mac, &mac_len) ||
        mac_len != SHA256_DIGEST_LENGTH) {
        OPENSSL_cleanse(digest, sizeof(digest));
        OPENSSL_cleanse(canonical, sizeof(canonical));
        return VOICE_AUTH_ERR_CRYPTO;
    }
    hex_encode(mac, mac_len, out);
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(mac, sizeof(mac));
    OPENSSL_cleanse(canonical, sizeof(canonical));
    return VOICE_AUTH_OK;
}

int voice_auth_verify(
    const char *secret,
    size_t secret_len,
    const char *method,
    const char *path,
    const char *user,
    int64_t timestamp,
    const char *nonce,
    const uint8_t *body,
    size_t body_len,
    const char *signature
) {
    char expected[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    int result;
    if (!signature || strlen(signature) != VOICE_AUTH_SIGNATURE_HEX_LEN)
        return VOICE_AUTH_ERR_SIGNATURE;
    result = voice_auth_sign(
        secret, secret_len, method, path, user, timestamp, nonce, body, body_len, expected);
    if (result != VOICE_AUTH_OK) return result;
    result = CRYPTO_memcmp(expected, signature, VOICE_AUTH_SIGNATURE_HEX_LEN) == 0
                 ? VOICE_AUTH_OK
                 : VOICE_AUTH_ERR_SIGNATURE;
    OPENSSL_cleanse(expected, sizeof(expected));
    return result;
}

static void voice_auth_verifier_context_free(
    voice_auth_verifier_context *context
) {
    if (!context) return;
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    EVP_MD_CTX_free(context->hmac_context);
    EVP_MD_CTX_free(context->hmac_outer_template);
    EVP_MD_CTX_free(context->hmac_inner_template);
    EVP_MD_CTX_free(context->digest_context);
    EVP_MD_CTX_free(context->digest_template);
#endif
    OPENSSL_cleanse(context, sizeof(*context));
    free(context);
}

int voice_auth_verifier_init(
    voice_auth_verifier *verifier,
    const char *secret,
    size_t secret_len
) {
    voice_auth_verifier_context *context;
    if (!verifier || verifier->context || !secret ||
        secret_len < 32u || secret_len > 255u) return VOICE_AUTH_ERR_ARGUMENT;
    context = (voice_auth_verifier_context *)calloc(1, sizeof(*context));
    if (!context) return VOICE_AUTH_ERR_CRYPTO;
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    {
        unsigned char key_block[SHA256_CBLOCK];
        unsigned char inner_pad[SHA256_CBLOCK];
        unsigned char outer_pad[SHA256_CBLOCK];
        unsigned char reduced_key[SHA256_DIGEST_LENGTH];
        const unsigned char *key = (const unsigned char *)secret;
        size_t key_len = secret_len;
        unsigned int reduced_length = 0;
        size_t i;
        int initialized = 0;

        memset(key_block, 0, sizeof(key_block));
        memset(reduced_key, 0, sizeof(reduced_key));
        context->digest_template = EVP_MD_CTX_new();
        context->digest_context = EVP_MD_CTX_new();
        context->hmac_inner_template = EVP_MD_CTX_new();
        context->hmac_outer_template = EVP_MD_CTX_new();
        context->hmac_context = EVP_MD_CTX_new();
        if (context->digest_template && context->digest_context &&
            context->hmac_inner_template &&
            context->hmac_outer_template && context->hmac_context) {
            if (EVP_DigestInit_ex(
                    context->digest_template, EVP_sha256(), NULL) != 1)
                goto template_done;
            if (key_len > sizeof(key_block)) {
                if (EVP_MD_CTX_copy_ex(
                        context->digest_context,
                        context->digest_template) != 1 ||
                    EVP_DigestUpdate(
                        context->digest_context, key, key_len) != 1 ||
                    EVP_DigestFinal_ex(
                        context->digest_context, reduced_key,
                        &reduced_length) != 1 ||
                    reduced_length != SHA256_DIGEST_LENGTH)
                    goto template_done;
                key = reduced_key;
                key_len = sizeof(reduced_key);
            }
            memcpy(key_block, key, key_len);
            /* Cache SHA-256 state after each fixed HMAC key pad. */
            for (i = 0u; i < sizeof(key_block); ++i) {
                inner_pad[i] = key_block[i] ^ 0x36u;
                outer_pad[i] = key_block[i] ^ 0x5cu;
            }
            if (EVP_DigestInit_ex(
                    context->hmac_inner_template, EVP_sha256(), NULL) == 1 &&
                EVP_DigestUpdate(
                    context->hmac_inner_template,
                    inner_pad, sizeof(inner_pad)) == 1 &&
                EVP_DigestInit_ex(
                    context->hmac_outer_template, EVP_sha256(), NULL) == 1 &&
                EVP_DigestUpdate(
                    context->hmac_outer_template,
                    outer_pad, sizeof(outer_pad)) == 1)
                initialized = 1;
        }
template_done:
        OPENSSL_cleanse(key_block, sizeof(key_block));
        OPENSSL_cleanse(inner_pad, sizeof(inner_pad));
        OPENSSL_cleanse(outer_pad, sizeof(outer_pad));
        OPENSSL_cleanse(reduced_key, sizeof(reduced_key));
        if (!initialized) {
            voice_auth_verifier_context_free(context);
            return VOICE_AUTH_ERR_CRYPTO;
        }
    }
#else
    {
        unsigned char key_block[SHA256_CBLOCK];
        unsigned char inner_pad[SHA256_CBLOCK];
        unsigned char outer_pad[SHA256_CBLOCK];
        unsigned char reduced_key[SHA256_DIGEST_LENGTH];
        const unsigned char *key = (const unsigned char *)secret;
        size_t key_len = secret_len;
        size_t i;
        int initialized = 0;

        memset(key_block, 0, sizeof(key_block));
        memset(reduced_key, 0, sizeof(reduced_key));
        if (SHA256_Init(&context->digest_template) != 1)
            goto direct_template_done;
        if (key_len > sizeof(key_block)) {
            context->digest_context = context->digest_template;
            if (SHA256_Update(
                    &context->digest_context, key, key_len) != 1 ||
                SHA256_Final(
                    reduced_key, &context->digest_context) != 1)
                goto direct_template_done;
            key = reduced_key;
            key_len = sizeof(reduced_key);
        }
        memcpy(key_block, key, key_len);
        for (i = 0u; i < sizeof(key_block); ++i) {
            inner_pad[i] = key_block[i] ^ 0x36u;
            outer_pad[i] = key_block[i] ^ 0x5cu;
        }
        /* Keep one SHA-256 state after each fixed HMAC key pad. */
        context->hmac_inner_template = context->digest_template;
        context->hmac_outer_template = context->digest_template;
        if (SHA256_Update(
                &context->hmac_inner_template,
                inner_pad,
                sizeof(inner_pad)) == 1 &&
            SHA256_Update(
                &context->hmac_outer_template,
                outer_pad,
                sizeof(outer_pad)) == 1)
            initialized = 1;
direct_template_done:
        OPENSSL_cleanse(key_block, sizeof(key_block));
        OPENSSL_cleanse(inner_pad, sizeof(inner_pad));
        OPENSSL_cleanse(outer_pad, sizeof(outer_pad));
        OPENSSL_cleanse(reduced_key, sizeof(reduced_key));
        if (!initialized) {
            voice_auth_verifier_context_free(context);
            return VOICE_AUTH_ERR_CRYPTO;
        }
    }
#endif
    verifier->context = context;
    return VOICE_AUTH_OK;
}

void voice_auth_verifier_destroy(voice_auth_verifier *verifier) {
    if (!verifier) return;
    voice_auth_verifier_context_free(verifier->context);
    verifier->context = NULL;
}

int voice_auth_verifier_verify(
    voice_auth_verifier *verifier,
    const char *method,
    const char *path,
    const char *user,
    int64_t timestamp,
    const char *nonce,
    const uint8_t *body,
    size_t body_len,
    const char *signature
) {
    voice_auth_verifier_context *context;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char mac[EVP_MAX_MD_SIZE];
    char digest_hex[SHA256_DIGEST_LENGTH * 2u + 1u];
    char canonical[VOICE_AUTH_CANONICAL_CAP];
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    unsigned int digest_len = 0;
#endif
    size_t mac_len = 0;
    size_t canonical_len;
    int result = VOICE_AUTH_ERR_CRYPTO;
    if (!verifier || !verifier->context) return VOICE_AUTH_ERR_ARGUMENT;
    context = verifier->context;
    if (!signature || strlen(signature) != VOICE_AUTH_SIGNATURE_HEX_LEN ||
        !simple_token(method, VOICE_AUTH_METHOD_MAX) ||
        !simple_token(path, VOICE_AUTH_PATH_MAX) ||
        !simple_token(user, VOICE_AUTH_USER_MAX) ||
        !nonce_valid(nonce) || timestamp <= 0 ||
        (!body && body_len != 0)) return VOICE_AUTH_ERR_ARGUMENT;
#if VOICE_AUTH_HAS_EVP_DIGEST_TEMPLATE
    if (EVP_MD_CTX_copy_ex(
            context->digest_context, context->digest_template) != 1 ||
        (body_len != 0 &&
         EVP_DigestUpdate(context->digest_context, body, body_len) != 1) ||
        EVP_DigestFinal_ex(context->digest_context, digest, &digest_len) != 1 ||
        digest_len != SHA256_DIGEST_LENGTH) goto cleanup;
#else
    context->digest_context = context->digest_template;
    if ((body_len != 0 &&
         SHA256_Update(&context->digest_context, body, body_len) != 1) ||
        SHA256_Final(digest, &context->digest_context) != 1) goto cleanup;
#endif
    hex_encode(digest, sizeof(digest), digest_hex);
    canonical_len = write_request_canonical(
        canonical, sizeof(canonical), method, path, user, timestamp, nonce,
        digest_hex);
    if (canonical_len == 0u) {
        result = VOICE_AUTH_ERR_CAPACITY;
        goto cleanup;
    }
    if (voice_hmac_prepared(
            context, (const unsigned char *)canonical, canonical_len,
            mac, sizeof(mac), &mac_len) != VOICE_AUTH_OK ||
        mac_len != SHA256_DIGEST_LENGTH) goto cleanup;
    result = lowercase_hex_matches(mac, mac_len, signature) ?
        VOICE_AUTH_OK : VOICE_AUTH_ERR_SIGNATURE;
cleanup:
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(mac, sizeof(mac));
    OPENSSL_cleanse(canonical, sizeof(canonical));
    return result;
}

int voice_auth_nonce_cache_accept(
    voice_auth_nonce_entry *entries,
    size_t entry_count,
    const char *nonce,
    int64_t now,
    int64_t ttl_seconds
) {
    voice_auth_nonce_entry *available = NULL;
    size_t i;
    if (!entries || entry_count == 0 || !nonce_valid(nonce) || now <= 0 ||
        ttl_seconds <= 0 || now > INT64_MAX - ttl_seconds) return VOICE_AUTH_ERR_ARGUMENT;
    for (i = 0; i < entry_count; ++i) {
        if (entries[i].expires_at >= now) {
            if (strcmp(entries[i].nonce, nonce) == 0) return VOICE_AUTH_ERR_REPLAY;
        } else if (!available) {
            available = &entries[i];
        }
    }
    if (!available) return VOICE_AUTH_ERR_CAPACITY;
    memcpy(available->nonce, nonce, VOICE_AUTH_NONCE_HEX_LEN + 1u);
    available->expires_at = now + ttl_seconds;
    return VOICE_AUTH_OK;
}

static unsigned char *replay_key_at(
    const voice_auth_replay_cache *cache,
    uint32_t entry_index
) {
    return cache->keys + (size_t)entry_index * cache->max_key_len;
}

static uint64_t rotate_left_64(uint64_t value, unsigned count) {
    return (value << count) | (value >> (64u - count));
}

static void sip_round(
    uint64_t *v0,
    uint64_t *v1,
    uint64_t *v2,
    uint64_t *v3
) {
    *v0 += *v1;
    *v1 = rotate_left_64(*v1, 13u);
    *v1 ^= *v0;
    *v0 = rotate_left_64(*v0, 32u);
    *v2 += *v3;
    *v3 = rotate_left_64(*v3, 16u);
    *v3 ^= *v2;
    *v0 += *v3;
    *v3 = rotate_left_64(*v3, 21u);
    *v3 ^= *v0;
    *v2 += *v1;
    *v1 = rotate_left_64(*v1, 17u);
    *v1 ^= *v2;
    *v2 = rotate_left_64(*v2, 32u);
}

static uint64_t read_u64_le(const unsigned char *input) {
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8u; ++i) value |= (uint64_t)input[i] << (i * 8u);
    return value;
}

static uint64_t replay_hash(
    const voice_auth_replay_cache *cache,
    const void *key,
    size_t key_len
) {
    const unsigned char *cursor = (const unsigned char *)key;
    size_t remaining = key_len;
    uint64_t v0 = cache->hash_key[0] ^ UINT64_C(0x736f6d6570736575);
    uint64_t v1 = cache->hash_key[1] ^ UINT64_C(0x646f72616e646f6d);
    uint64_t v2 = cache->hash_key[0] ^ UINT64_C(0x6c7967656e657261);
    uint64_t v3 = cache->hash_key[1] ^ UINT64_C(0x7465646279746573);
    uint64_t tail;
    size_t i;
    while (remaining >= 8u) {
        uint64_t block = read_u64_le(cursor);
        v3 ^= block;
        sip_round(&v0, &v1, &v2, &v3);
        sip_round(&v0, &v1, &v2, &v3);
        v0 ^= block;
        cursor += 8u;
        remaining -= 8u;
    }
    tail = (uint64_t)key_len << 56u;
    for (i = 0; i < remaining; ++i)
        tail |= (uint64_t)cursor[i] << (i * 8u);
    v3 ^= tail;
    sip_round(&v0, &v1, &v2, &v3);
    sip_round(&v0, &v1, &v2, &v3);
    v0 ^= tail;
    v2 ^= UINT64_C(0xff);
    for (i = 0; i < 4u; ++i) sip_round(&v0, &v1, &v2, &v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

static int replay_hash_self_test(void) {
    voice_auth_replay_cache cache;
    const unsigned char message[] = {0x00u};
    memset(&cache, 0, sizeof(cache));
    cache.hash_key[0] = UINT64_C(0x0706050403020100);
    cache.hash_key[1] = UINT64_C(0x0f0e0d0c0b0a0908);
    return replay_hash(&cache, message, sizeof(message)) ==
        UINT64_C(0x74f839c593dc67fd);
}

static void heap_swap(voice_auth_replay_cache *cache, size_t left, size_t right) {
    uint32_t entry = cache->heap[left];
    cache->heap[left] = cache->heap[right];
    cache->heap[right] = entry;
    cache->entries[cache->heap[left]].heap_position = (uint32_t)left;
    cache->entries[cache->heap[right]].heap_position = (uint32_t)right;
}

static int heap_entry_before(
    const voice_auth_replay_cache *cache,
    uint32_t left,
    uint32_t right
) {
    const voice_auth_replay_index_entry *a = &cache->entries[left];
    const voice_auth_replay_index_entry *b = &cache->entries[right];
    if (a->expires_at != b->expires_at) return a->expires_at < b->expires_at;
    return left < right;
}

static void heap_up(voice_auth_replay_cache *cache, size_t position) {
    while (position != 0) {
        size_t parent = (position - 1u) / 2u;
        if (!heap_entry_before(cache, cache->heap[position], cache->heap[parent])) break;
        heap_swap(cache, position, parent);
        position = parent;
    }
}

static void heap_down(voice_auth_replay_cache *cache, size_t position) {
    for (;;) {
        size_t left = position * 2u + 1u;
        size_t right = left + 1u;
        size_t smallest = position;
        if (left < cache->heap_count &&
            heap_entry_before(cache, cache->heap[left], cache->heap[smallest])) smallest = left;
        if (right < cache->heap_count &&
            heap_entry_before(cache, cache->heap[right], cache->heap[smallest])) smallest = right;
        if (smallest == position) return;
        heap_swap(cache, position, smallest);
        position = smallest;
    }
}

static void heap_remove(voice_auth_replay_cache *cache, uint32_t entry_index) {
    size_t position = cache->entries[entry_index].heap_position;
    size_t final;
    if (cache->heap_count == 0 || position >= cache->heap_count ||
        cache->heap[position] != entry_index) return;
    final = --cache->heap_count;
    if (position != final) {
        cache->heap[position] = cache->heap[final];
        cache->entries[cache->heap[position]].heap_position = (uint32_t)position;
        if (position != 0 && heap_entry_before(
                cache, cache->heap[position], cache->heap[(position - 1u) / 2u])) {
            heap_up(cache, position);
        } else {
            heap_down(cache, position);
        }
    }
    cache->entries[entry_index].heap_position = VOICE_AUTH_INDEX_NONE;
}

static size_t probe_distance(size_t home, size_t position, size_t mask) {
    return (position - home) & mask;
}

static void index_remove(voice_auth_replay_cache *cache, uint32_t entry_index) {
    size_t mask = cache->index_capacity - 1u;
    size_t hole = cache->entries[entry_index].index_position;
    size_t current;
    if (hole >= cache->index_capacity || cache->index[hole] != entry_index) return;
    current = (hole + 1u) & mask;
    while (cache->index[current] != VOICE_AUTH_INDEX_NONE) {
        uint32_t candidate = cache->index[current];
        voice_auth_replay_index_entry *entry = &cache->entries[candidate];
        size_t home = (size_t)replay_hash(
            cache, replay_key_at(cache, candidate), entry->key_len) & mask;
        if (probe_distance(home, current, mask) > probe_distance(home, hole, mask)) {
            cache->index[hole] = candidate;
            cache->entries[candidate].index_position = (uint32_t)hole;
            hole = current;
        }
        current = (current + 1u) & mask;
    }
    cache->index[hole] = VOICE_AUTH_INDEX_NONE;
    cache->entries[entry_index].index_position = VOICE_AUTH_INDEX_NONE;
}

static void release_entry(voice_auth_replay_cache *cache, uint32_t entry_index) {
    voice_auth_replay_index_entry *entry = &cache->entries[entry_index];
    index_remove(cache, entry_index);
    heap_remove(cache, entry_index);
    OPENSSL_cleanse(replay_key_at(cache, entry_index), cache->max_key_len);
    entry->expires_at = 0;
    entry->key_len = 0;
    entry->next_free = cache->free_head;
    cache->free_head = entry_index;
    cache->count--;
}

static void expire_entries(voice_auth_replay_cache *cache, int64_t now) {
    while (cache->heap_count != 0) {
        uint32_t entry_index = cache->heap[0];
        if (cache->entries[entry_index].expires_at >= now) break;
        release_entry(cache, entry_index);
    }
}

static uint32_t index_find(
    const voice_auth_replay_cache *cache,
    const void *key,
    size_t key_len,
    size_t *empty_position
) {
    size_t mask = cache->index_capacity - 1u;
    size_t start = (size_t)replay_hash(cache, key, key_len) & mask;
    size_t probe;
    for (probe = 0; probe < cache->index_capacity; ++probe) {
        size_t position = (start + probe) & mask;
        uint32_t entry_index = cache->index[position];
        if (entry_index == VOICE_AUTH_INDEX_NONE) {
            if (empty_position) *empty_position = position;
            return VOICE_AUTH_INDEX_NONE;
        }
        if (cache->entries[entry_index].key_len == key_len &&
            memcmp(replay_key_at(cache, entry_index), key, key_len) == 0)
            return entry_index;
    }
    if (empty_position) *empty_position = cache->index_capacity;
    return VOICE_AUTH_INDEX_NONE;
}

int voice_auth_replay_index_init(
    voice_auth_replay_cache *cache,
    size_t capacity,
    size_t max_key_len
) {
    size_t index_capacity = 2u;
    size_t i;
    if (!cache || capacity == 0 || max_key_len == 0 ||
        max_key_len > UINT32_MAX || capacity > UINT32_MAX - 1u ||
        capacity > SIZE_MAX / 2u ||
        capacity > SIZE_MAX / sizeof(*cache->entries) ||
        capacity > SIZE_MAX / sizeof(*cache->heap) ||
        capacity > SIZE_MAX / max_key_len) return VOICE_AUTH_ERR_ARGUMENT;
    while (index_capacity < capacity * 2u) {
        if (index_capacity > SIZE_MAX / 2u || index_capacity > UINT32_MAX / 2u)
            return VOICE_AUTH_ERR_CAPACITY;
        index_capacity *= 2u;
    }
    if (index_capacity > SIZE_MAX / sizeof(*cache->index))
        return VOICE_AUTH_ERR_CAPACITY;
    memset(cache, 0, sizeof(*cache));
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    cache->keys = calloc(capacity, max_key_len);
    cache->index = malloc(index_capacity * sizeof(*cache->index));
    cache->heap = malloc(capacity * sizeof(*cache->heap));
    cache->capacity = capacity;
    cache->index_capacity = index_capacity;
    cache->max_key_len = max_key_len;
    if (!cache->entries || !cache->keys || !cache->index || !cache->heap) {
        voice_auth_replay_index_destroy(cache);
        return VOICE_AUTH_ERR_CAPACITY;
    }
    if (!replay_hash_self_test() ||
        RAND_bytes(
            (unsigned char *)cache->hash_key,
            (int)sizeof(cache->hash_key)) != 1) {
        voice_auth_replay_index_destroy(cache);
        return VOICE_AUTH_ERR_CRYPTO;
    }
    for (i = 0; i < index_capacity; ++i) cache->index[i] = VOICE_AUTH_INDEX_NONE;
    for (i = 0; i < capacity; ++i) {
        cache->entries[i].heap_position = VOICE_AUTH_INDEX_NONE;
        cache->entries[i].index_position = VOICE_AUTH_INDEX_NONE;
        cache->entries[i].next_free = i + 1u < capacity ? (uint32_t)(i + 1u) : VOICE_AUTH_INDEX_NONE;
    }
    cache->free_head = 0;
    return VOICE_AUTH_OK;
}

void voice_auth_replay_index_destroy(voice_auth_replay_cache *cache) {
    if (!cache) return;
    if (cache->entries) {
        OPENSSL_cleanse(cache->entries, cache->capacity * sizeof(*cache->entries));
        free(cache->entries);
    }
    if (cache->keys) {
        OPENSSL_cleanse(cache->keys, cache->capacity * cache->max_key_len);
        free(cache->keys);
    }
    if (cache->index) {
        OPENSSL_cleanse(cache->index, cache->index_capacity * sizeof(*cache->index));
        free(cache->index);
    }
    if (cache->heap) {
        OPENSSL_cleanse(cache->heap, cache->capacity * sizeof(*cache->heap));
        free(cache->heap);
    }
    memset(cache, 0, sizeof(*cache));
    cache->free_head = VOICE_AUTH_INDEX_NONE;
}

size_t voice_auth_replay_index_bytes(const voice_auth_replay_cache *cache) {
    if (!cache || !cache->entries || !cache->keys ||
        !cache->index || !cache->heap) return 0;
    return sizeof(*cache) + cache->capacity * sizeof(*cache->entries) +
        cache->capacity * cache->max_key_len +
        cache->index_capacity * sizeof(*cache->index) +
        cache->capacity * sizeof(*cache->heap);
}

int voice_auth_replay_index_accept(
    voice_auth_replay_cache *cache,
    const void *key,
    size_t key_len,
    int64_t now,
    int64_t ttl_seconds
) {
    voice_auth_replay_index_entry *entry;
    uint32_t entry_index;
    size_t index_position = 0;
    if (!cache || !cache->entries || !cache->keys || !cache->index || !cache->heap ||
        cache->capacity == 0 || cache->index_capacity < cache->capacity * 2u ||
        cache->max_key_len == 0 || !key || key_len == 0 ||
        key_len > cache->max_key_len || now <= 0 || ttl_seconds <= 0 ||
        now > INT64_MAX - ttl_seconds) return VOICE_AUTH_ERR_ARGUMENT;
    expire_entries(cache, now);
    if (index_find(cache, key, key_len, &index_position) != VOICE_AUTH_INDEX_NONE)
        return VOICE_AUTH_ERR_REPLAY;
    if (cache->count >= cache->capacity || cache->free_head == VOICE_AUTH_INDEX_NONE ||
        index_position >= cache->index_capacity) return VOICE_AUTH_ERR_CAPACITY;
    entry_index = cache->free_head;
    entry = &cache->entries[entry_index];
    cache->free_head = entry->next_free;
    memcpy(replay_key_at(cache, entry_index), key, key_len);
    entry->key_len = (uint32_t)key_len;
    entry->expires_at = now + ttl_seconds;
    entry->index_position = (uint32_t)index_position;
    entry->heap_position = (uint32_t)cache->heap_count;
    entry->next_free = VOICE_AUTH_INDEX_NONE;
    cache->index[index_position] = entry_index;
    cache->heap[cache->heap_count++] = entry_index;
    cache->count++;
    heap_up(cache, entry->heap_position);
    return VOICE_AUTH_OK;
}

int voice_auth_nonce_index_init(voice_auth_nonce_cache *cache, size_t capacity) {
    return voice_auth_replay_index_init(
        cache, capacity, VOICE_AUTH_NONCE_HEX_LEN);
}

void voice_auth_nonce_index_destroy(voice_auth_nonce_cache *cache) {
    voice_auth_replay_index_destroy(cache);
}

size_t voice_auth_nonce_index_bytes(const voice_auth_nonce_cache *cache) {
    return voice_auth_replay_index_bytes(cache);
}

int voice_auth_nonce_index_accept(
    voice_auth_nonce_cache *cache,
    const char *nonce,
    int64_t now,
    int64_t ttl_seconds
) {
    if (!nonce_valid(nonce)) return VOICE_AUTH_ERR_ARGUMENT;
    return voice_auth_replay_index_accept(
        cache, nonce, VOICE_AUTH_NONCE_HEX_LEN, now, ttl_seconds);
}
