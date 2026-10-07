/* Bounded HMAC authentication for the in-cluster voice HTTP edge. */
#ifndef VOICE_C_AUTH_H
#define VOICE_C_AUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VOICE_AUTH_OK = 0,
    VOICE_AUTH_ERR_ARGUMENT = 1,
    VOICE_AUTH_ERR_CRYPTO = 2,
    VOICE_AUTH_ERR_CAPACITY = 3,
    VOICE_AUTH_ERR_SIGNATURE = 4,
    VOICE_AUTH_ERR_REPLAY = 5
};

#define VOICE_AUTH_NONCE_HEX_LEN 32u
#define VOICE_AUTH_SIGNATURE_HEX_LEN 64u
#define VOICE_AUTH_IDENTITY_TOKEN_CAP 512u
#define VOICE_AUTH_IDENTITY_TTL_MAX_SECONDS 120
#define VOICE_AUTH_NONCE_POOL_CAPACITY 128u

/* A non-copyable CSPRNG batch for one single-threaded edge. */
typedef struct {
    unsigned char bytes[
        VOICE_AUTH_NONCE_POOL_CAPACITY * (VOICE_AUTH_NONCE_HEX_LEN / 2u)];
    size_t next;
    int initialized;
} voice_auth_nonce_pool;

typedef struct {
    char request_id[128];
    char user_id[128];
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    int64_t issued_at;
    int64_t expires_at;
    int premium;
    int model_request_capture;
} voice_auth_identity_claims;

typedef struct {
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    int64_t expires_at;
} voice_auth_nonce_entry;

typedef struct voice_auth_replay_index_entry voice_auth_replay_index_entry;

typedef struct voice_auth_verifier_context voice_auth_verifier_context;

/* A process-initialized verifier for one single-threaded HTTP edge. */
typedef struct {
    voice_auth_verifier_context *context;
} voice_auth_verifier;

/*
 * A bounded replay cache with an open-addressed index and an expiry heap.
 * The cache allocates all storage during initialization. Accept performs no
 * allocation and remains bounded when caller time moves backward.
 */
typedef struct {
    voice_auth_replay_index_entry *entries;
    unsigned char *keys;
    uint32_t *index;
    uint32_t *heap;
    size_t capacity;
    size_t index_capacity;
    size_t max_key_len;
    size_t count;
    size_t heap_count;
    uint32_t free_head;
    uint64_t hash_key[2];
} voice_auth_replay_cache;

typedef voice_auth_replay_cache voice_auth_nonce_cache;

/* Write a cryptographically random lowercase hexadecimal nonce. */
int voice_auth_random_nonce(char out[VOICE_AUTH_NONCE_HEX_LEN + 1u]);

/* Initialize one zeroed nonce batch before accepting traffic. */
int voice_auth_nonce_pool_init(voice_auth_nonce_pool *pool);

/* Consume one single-use nonce and refill the batch when required. */
int voice_auth_nonce_pool_next(
    voice_auth_nonce_pool *pool,
    char out[VOICE_AUTH_NONCE_HEX_LEN + 1u]
);

/* Cleanse one initialized or zeroed nonce batch. */
void voice_auth_nonce_pool_destroy(voice_auth_nonce_pool *pool);

/* Initialize and verify the process CSPRNG before accepting traffic. */
int voice_auth_random_warmup(void);

/* Sign method, path, user, timestamp, nonce, and the SHA-256 body digest. */
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
);

/* Verify a lowercase hexadecimal signature in constant time. */
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
);

/* Initialize reusable digest and HMAC state for one serial caller. */
int voice_auth_verifier_init(
    voice_auth_verifier *verifier,
    const char *secret,
    size_t secret_len
);

/* Cleanse and release reusable verifier state. */
void voice_auth_verifier_destroy(voice_auth_verifier *verifier);

/* Verify one request without allocating per-request crypto state. */
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
);

/* Issue one signed, request-bound browser-to-gateway identity capability. */
int voice_auth_identity_issue(
    const char *secret,
    size_t secret_len,
    const char *request_id,
    const char *user_id,
    int premium,
    int64_t now,
    int64_t ttl_seconds,
    char *out,
    size_t out_cap
);

/* Capture requires an explicit issuer-side consent check. Ordinary issuance
 * retains the version-one token and grants no model-request capture. */
int voice_auth_identity_issue_capture(
    const char *secret, size_t secret_len, const char *request_id,
    const char *user_id, int premium, int64_t now, int64_t ttl_seconds,
    char *out, size_t out_cap
);

/* Verify the capability signature, canonical encoding, lifetime, and request binding. */
int voice_auth_identity_verify(
    const char *secret,
    size_t secret_len,
    const char *token,
    const char *request_id,
    int64_t now,
    voice_auth_identity_claims *out
);

/* Verify one identity capability with process-initialized HMAC state. */
int voice_auth_verifier_identity_verify(
    voice_auth_verifier *verifier,
    const char *token,
    const char *request_id,
    int64_t now,
    voice_auth_identity_claims *out
);

/* Record one nonce. Reject a replay and fail closed when all entries are live. */
int voice_auth_nonce_cache_accept(
    voice_auth_nonce_entry *entries,
    size_t entry_count,
    const char *nonce,
    int64_t now,
    int64_t ttl_seconds
);

/* Allocate a fixed-capacity exact-key replay cache. */
int voice_auth_replay_index_init(
    voice_auth_replay_cache *cache,
    size_t capacity,
    size_t max_key_len
);

/* Cleanse and release all exact-key replay-cache storage. */
void voice_auth_replay_index_destroy(voice_auth_replay_cache *cache);

/* Return all bytes owned by an initialized exact-key replay cache. */
size_t voice_auth_replay_index_bytes(const voice_auth_replay_cache *cache);

/* Record one bounded byte key. Hash collisions retain exact-key semantics. */
int voice_auth_replay_index_accept(
    voice_auth_replay_cache *cache,
    const void *key,
    size_t key_len,
    int64_t now,
    int64_t ttl_seconds
);

/* Allocate a fixed-capacity indexed replay cache. */
int voice_auth_nonce_index_init(voice_auth_nonce_cache *cache, size_t capacity);

/* Cleanse and release all indexed replay-cache storage. */
void voice_auth_nonce_index_destroy(voice_auth_nonce_cache *cache);

/* Return all bytes owned by an initialized indexed cache. */
size_t voice_auth_nonce_index_bytes(const voice_auth_nonce_cache *cache);

/* Record one nonce in the indexed cache. */
int voice_auth_nonce_index_accept(
    voice_auth_nonce_cache *cache,
    const char *nonce,
    int64_t now,
    int64_t ttl_seconds
);

#ifdef __cplusplus
}
#endif

#endif
