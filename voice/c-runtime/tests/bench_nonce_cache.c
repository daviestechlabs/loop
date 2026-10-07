#define _POSIX_C_SOURCE 200809L

/* Compare the replay-cache scan with an indexed expiry heap. */

#include "voice_auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    CACHE_CAPACITY = 4096,
    MODEL_CAPACITY = 64,
    MODEL_KEYS = 257,
    BENCH_BATCHES = 301,
    BENCH_OPS_PER_BATCH = 64,
};

#define REQUEST_ID_HASH_DOMAIN "voice-request-id-replay-v1\n"

typedef struct {
    double samples[BENCH_BATCHES];
    double average_ns;
    double p50_ns;
    double p99_ns;
    uint64_t checksum;
} bench_result;

typedef struct {
    char key[64];
    size_t key_len;
    int64_t expires_at;
} request_model_entry;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void make_nonce(uint64_t value, char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u]) {
    (void)snprintf(
        nonce,
        VOICE_AUTH_NONCE_HEX_LEN + 1u,
        "%016" PRIx64 "%016" PRIx64,
        value,
        value ^ UINT64_C(0x9e3779b97f4a7c15));
}

static size_t make_request_id(uint64_t value, char request_id[64]) {
    int written = snprintf(request_id, 64, "request-%016" PRIx64, value);
    return written > 0 && written < 64 ? (size_t)written : 0u;
}

static int legacy_request_id_key(
    EVP_MD_CTX *digest_context,
    const char *request_id,
    size_t request_id_len,
    char out[VOICE_AUTH_NONCE_HEX_LEN + 1u]
) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    size_t i;
    int result = -1;
    if (!digest_context || !request_id || request_id_len == 0u || !out)
        return -1;
    if (EVP_DigestInit_ex(digest_context, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(
            digest_context, REQUEST_ID_HASH_DOMAIN,
            sizeof(REQUEST_ID_HASH_DOMAIN) - 1u) == 1 &&
        EVP_DigestUpdate(digest_context, request_id, request_id_len) == 1 &&
        EVP_DigestFinal_ex(digest_context, digest, &digest_len) == 1 &&
        digest_len == SHA256_DIGEST_LENGTH) {
        for (i = 0; i < VOICE_AUTH_NONCE_HEX_LEN / 2u; ++i) {
            out[i * 2u] = digits[digest[i] >> 4u];
            out[i * 2u + 1u] = digits[digest[i] & 15u];
        }
        out[VOICE_AUTH_NONCE_HEX_LEN] = '\0';
        result = 0;
    }
    OPENSSL_cleanse(digest, sizeof(digest));
    return result;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void summarize(bench_result *result) {
    double sorted[BENCH_BATCHES];
    double total = 0.0;
    size_t i;
    memcpy(sorted, result->samples, sizeof(sorted));
    qsort(sorted, BENCH_BATCHES, sizeof(sorted[0]), compare_double);
    for (i = 0; i < BENCH_BATCHES; ++i) total += result->samples[i];
    result->average_ns = total / (double)BENCH_BATCHES;
    result->p50_ns = sorted[BENCH_BATCHES / 2u];
    result->p99_ns = sorted[(BENCH_BATCHES * 99u) / 100u];
}

static int fill_linear(voice_auth_nonce_entry *entries, int64_t start, int stagger) {
    size_t i;
    for (i = 0; i < CACHE_CAPACITY; ++i) {
        char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
        int64_t now = stagger ? start + (int64_t)i : start;
        make_nonce(i, nonce);
        if (voice_auth_nonce_cache_accept(
                entries, CACHE_CAPACITY, nonce, now, CACHE_CAPACITY) != VOICE_AUTH_OK) return -1;
    }
    return 0;
}

static int fill_indexed(voice_auth_nonce_cache *cache, int64_t start, int stagger) {
    size_t i;
    for (i = 0; i < CACHE_CAPACITY; ++i) {
        char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
        int64_t now = stagger ? start + (int64_t)i : start;
        make_nonce(i, nonce);
        if (voice_auth_nonce_index_accept(cache, nonce, now, CACHE_CAPACITY) != VOICE_AUTH_OK)
            return -1;
    }
    return 0;
}

static int model_verify(void) {
    voice_auth_nonce_entry linear[MODEL_CAPACITY];
    voice_auth_nonce_cache indexed;
    int64_t now = 1000;
    size_t operation;
    memset(linear, 0, sizeof(linear));
    memset(&indexed, 0, sizeof(indexed));
    if (voice_auth_nonce_index_init(&indexed, MODEL_CAPACITY) != VOICE_AUTH_OK) return -1;
    for (operation = 0; operation < 50000; ++operation) {
        char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
        uint64_t selected =
            (operation * 73u + operation / 11u + UINT64_C(19)) % MODEL_KEYS;
        int linear_result;
        int indexed_result;
        if (operation % 17u == 0) now += 3;
        make_nonce(selected, nonce);
        linear_result = voice_auth_nonce_cache_accept(linear, MODEL_CAPACITY, nonce, now, 31);
        indexed_result = voice_auth_nonce_index_accept(&indexed, nonce, now, 31);
        if (linear_result != indexed_result) {
            voice_auth_nonce_index_destroy(&indexed);
            return -1;
        }
    }
    voice_auth_nonce_index_destroy(&indexed);
    return 0;
}

static int request_model_accept(
    request_model_entry *entries,
    size_t entry_count,
    const char *key,
    size_t key_len,
    int64_t now,
    int64_t ttl_seconds
) {
    request_model_entry *available = NULL;
    size_t i;
    for (i = 0; i < entry_count; ++i) {
        if (entries[i].expires_at >= now) {
            if (entries[i].key_len == key_len &&
                memcmp(entries[i].key, key, key_len) == 0)
                return VOICE_AUTH_ERR_REPLAY;
        } else if (!available) {
            available = &entries[i];
        }
    }
    if (!available) return VOICE_AUTH_ERR_CAPACITY;
    memcpy(available->key, key, key_len);
    available->key_len = key_len;
    available->expires_at = now + ttl_seconds;
    return VOICE_AUTH_OK;
}

static int request_model_verify(void) {
    request_model_entry linear[MODEL_CAPACITY];
    voice_auth_replay_cache indexed;
    int64_t now = 1000;
    size_t operation;
    memset(linear, 0, sizeof(linear));
    memset(&indexed, 0, sizeof(indexed));
    if (voice_auth_replay_index_init(&indexed, MODEL_CAPACITY, 63) != VOICE_AUTH_OK)
        return -1;
    for (operation = 0; operation < 50000; ++operation) {
        char request_id[64];
        uint64_t selected =
            (operation * 73u + operation / 11u + UINT64_C(19)) % MODEL_KEYS;
        int written = snprintf(
            request_id, sizeof(request_id),
            operation % 5u == 0u ? "r-%" PRIu64 : "request-%" PRIu64,
            selected);
        int linear_result;
        int indexed_result;
        if (written <= 0 || (size_t)written >= sizeof(request_id)) {
            voice_auth_replay_index_destroy(&indexed);
            return -1;
        }
        if (operation % 17u == 0u) now += 3;
        linear_result = request_model_accept(
            linear, MODEL_CAPACITY, request_id, (size_t)written, now, 31);
        indexed_result = voice_auth_replay_index_accept(
            &indexed, request_id, (size_t)written, now, 31);
        if (linear_result != indexed_result) {
            voice_auth_replay_index_destroy(&indexed);
            return -1;
        }
    }
    voice_auth_replay_index_destroy(&indexed);
    return 0;
}

static int run_replay_linear(bench_result *result) {
    voice_auth_nonce_entry *cache = calloc(CACHE_CAPACITY, sizeof(*cache));
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    size_t batch;
    if (!cache || fill_linear(cache, 1000, 0) != 0) {
        free(cache);
        return -1;
    }
    make_nonce(CACHE_CAPACITY - 1u, nonce);
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation)
            result->checksum += (uint64_t)voice_auth_nonce_cache_accept(
                cache, CACHE_CAPACITY, nonce, 1000, CACHE_CAPACITY);
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    OPENSSL_cleanse(cache, CACHE_CAPACITY * sizeof(*cache));
    free(cache);
    summarize(result);
    return 0;
}

static int run_replay_indexed(bench_result *result) {
    voice_auth_nonce_cache cache;
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    size_t batch;
    memset(&cache, 0, sizeof(cache));
    if (voice_auth_nonce_index_init(&cache, CACHE_CAPACITY) != VOICE_AUTH_OK ||
        fill_indexed(&cache, 1000, 0) != 0) {
        voice_auth_nonce_index_destroy(&cache);
        return -1;
    }
    make_nonce(CACHE_CAPACITY - 1u, nonce);
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation)
            result->checksum += (uint64_t)voice_auth_nonce_index_accept(
                &cache, nonce, 1000, CACHE_CAPACITY);
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    voice_auth_nonce_index_destroy(&cache);
    summarize(result);
    return 0;
}

static int run_capacity_linear(bench_result *result) {
    voice_auth_nonce_entry *cache = calloc(CACHE_CAPACITY, sizeof(*cache));
    size_t batch;
    if (!cache || fill_linear(cache, 1000, 0) != 0) {
        free(cache);
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
            make_nonce(CACHE_CAPACITY + batch * BENCH_OPS_PER_BATCH + operation, nonce);
            result->checksum += (uint64_t)voice_auth_nonce_cache_accept(
                cache, CACHE_CAPACITY, nonce, 1000, CACHE_CAPACITY);
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    OPENSSL_cleanse(cache, CACHE_CAPACITY * sizeof(*cache));
    free(cache);
    summarize(result);
    return 0;
}

static int run_capacity_indexed(bench_result *result) {
    voice_auth_nonce_cache cache;
    size_t batch;
    memset(&cache, 0, sizeof(cache));
    if (voice_auth_nonce_index_init(&cache, CACHE_CAPACITY) != VOICE_AUTH_OK ||
        fill_indexed(&cache, 1000, 0) != 0) {
        voice_auth_nonce_index_destroy(&cache);
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
            make_nonce(CACHE_CAPACITY + batch * BENCH_OPS_PER_BATCH + operation, nonce);
            result->checksum += (uint64_t)voice_auth_nonce_index_accept(
                &cache, nonce, 1000, CACHE_CAPACITY);
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    voice_auth_nonce_index_destroy(&cache);
    summarize(result);
    return 0;
}

static int run_churn_linear(bench_result *result) {
    voice_auth_nonce_entry *cache = calloc(CACHE_CAPACITY, sizeof(*cache));
    uint64_t next_nonce = CACHE_CAPACITY;
    int64_t now = 1000 + CACHE_CAPACITY + 1;
    size_t batch;
    if (!cache || fill_linear(cache, 1000, 1) != 0) {
        free(cache);
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
            make_nonce(next_nonce++, nonce);
            result->checksum += (uint64_t)voice_auth_nonce_cache_accept(
                cache, CACHE_CAPACITY, nonce, now++, CACHE_CAPACITY);
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    OPENSSL_cleanse(cache, CACHE_CAPACITY * sizeof(*cache));
    free(cache);
    summarize(result);
    return 0;
}

static int run_churn_indexed(bench_result *result) {
    voice_auth_nonce_cache cache;
    uint64_t next_nonce = CACHE_CAPACITY;
    int64_t now = 1000 + CACHE_CAPACITY + 1;
    size_t batch;
    memset(&cache, 0, sizeof(cache));
    if (voice_auth_nonce_index_init(&cache, CACHE_CAPACITY) != VOICE_AUTH_OK ||
        fill_indexed(&cache, 1000, 1) != 0) {
        voice_auth_nonce_index_destroy(&cache);
        return -1;
    }
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
            make_nonce(next_nonce++, nonce);
            result->checksum += (uint64_t)voice_auth_nonce_index_accept(
                &cache, nonce, now++, CACHE_CAPACITY);
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    voice_auth_nonce_index_destroy(&cache);
    summarize(result);
    return 0;
}

static int run_request_hashed(bench_result *result, size_t *owned_bytes) {
    EVP_MD_CTX *digest_context = NULL;
    voice_auth_nonce_cache cache;
    uint64_t next_request = 0;
    size_t batch;
    int failed = 0;
    memset(&cache, 0, sizeof(cache));
    digest_context = EVP_MD_CTX_new();
    if (!digest_context ||
        voice_auth_nonce_index_init(&cache, CACHE_CAPACITY) != VOICE_AUTH_OK) {
        EVP_MD_CTX_free(digest_context);
        voice_auth_nonce_index_destroy(&cache);
        return -1;
    }
    if (owned_bytes) *owned_bytes = voice_auth_nonce_index_bytes(&cache);
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES && !failed; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char request_id[64];
            char replay_key[VOICE_AUTH_NONCE_HEX_LEN + 1u];
            uint64_t sequence = next_request++;
            size_t request_len = make_request_id(sequence, request_id);
            int status;
            if (request_len == 0u || legacy_request_id_key(
                    digest_context, request_id, request_len, replay_key) != 0) {
                failed = 1;
                break;
            }
            status = voice_auth_nonce_index_accept(
                &cache, replay_key, 1000 + (int64_t)sequence,
                CACHE_CAPACITY - 1);
            if (status != VOICE_AUTH_OK) {
                failed = 1;
                break;
            }
            result->checksum += (uint64_t)status + request_len;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    voice_auth_nonce_index_destroy(&cache);
    EVP_MD_CTX_free(digest_context);
    if (failed) return -1;
    summarize(result);
    return 0;
}

static int run_request_exact(bench_result *result, size_t *owned_bytes) {
    voice_auth_replay_cache cache;
    uint64_t next_request = 0;
    size_t batch;
    int failed = 0;
    memset(&cache, 0, sizeof(cache));
    if (voice_auth_replay_index_init(&cache, CACHE_CAPACITY, 127) != VOICE_AUTH_OK)
        return -1;
    if (owned_bytes) *owned_bytes = voice_auth_replay_index_bytes(&cache);
    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES && !failed; ++batch) {
        uint64_t started = monotonic_ns();
        size_t operation;
        for (operation = 0; operation < BENCH_OPS_PER_BATCH; ++operation) {
            char request_id[64];
            uint64_t sequence = next_request++;
            size_t request_len = make_request_id(sequence, request_id);
            int status;
            if (request_len == 0u) {
                failed = 1;
                break;
            }
            status = voice_auth_replay_index_accept(
                &cache, request_id, request_len, 1000 + (int64_t)sequence,
                CACHE_CAPACITY - 1);
            if (status != VOICE_AUTH_OK) {
                failed = 1;
                break;
            }
            result->checksum += (uint64_t)status + request_len;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)BENCH_OPS_PER_BATCH;
    }
    voice_auth_replay_index_destroy(&cache);
    if (failed) return -1;
    summarize(result);
    return 0;
}

static void print_result(const char *name, const bench_result *result) {
    printf("%-24s avg=%9.2f ns p50=%9.2f ns p99=%9.2f ns checksum=%" PRIu64 "\n",
           name, result->average_ns, result->p50_ns, result->p99_ns, result->checksum);
}

int main(int argc, char **argv) {
    bench_result linear;
    bench_result indexed;
    voice_auth_nonce_cache memory_probe;
    size_t indexed_bytes;
    size_t hashed_request_bytes;
    size_t exact_request_bytes;
    int verify_only = argc == 2 && strcmp(argv[1], "--verify") == 0;
    if (argc > 2 || (argc == 2 && !verify_only)) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    if (model_verify() != 0 || request_model_verify() != 0) {
        fprintf(stderr, "replay cache model mismatch\n");
        return 1;
    }
    if (verify_only) {
        puts("ALL PASS nonce cache model");
        return 0;
    }
    if (run_replay_linear(&linear) != 0 || run_replay_indexed(&indexed) != 0 ||
        linear.checksum != indexed.checksum) return 1;
    print_result("linear replay", &linear);
    print_result("indexed replay", &indexed);
    if (run_capacity_linear(&linear) != 0 || run_capacity_indexed(&indexed) != 0 ||
        linear.checksum != indexed.checksum) return 1;
    print_result("linear full miss", &linear);
    print_result("indexed full miss", &indexed);
    if (run_churn_linear(&linear) != 0 || run_churn_indexed(&indexed) != 0 ||
        linear.checksum != indexed.checksum) return 1;
    print_result("linear expiry churn", &linear);
    print_result("indexed expiry churn", &indexed);
    memset(&memory_probe, 0, sizeof(memory_probe));
    if (voice_auth_nonce_index_init(&memory_probe, CACHE_CAPACITY) != VOICE_AUTH_OK) return 1;
    indexed_bytes = voice_auth_nonce_index_bytes(&memory_probe);
    voice_auth_nonce_index_destroy(&memory_probe);
    printf("linear bytes=%zu indexed bytes=%zu\n",
           CACHE_CAPACITY * sizeof(voice_auth_nonce_entry), indexed_bytes);
    if (run_request_hashed(&linear, &hashed_request_bytes) != 0 ||
        run_request_exact(&indexed, &exact_request_bytes) != 0 ||
        linear.checksum != indexed.checksum) return 1;
    print_result("hashed request IDs", &linear);
    print_result("exact request IDs", &indexed);
    printf("request bytes hashed=%zu exact=%zu\n",
           hashed_request_bytes, exact_request_bytes);
    return 0;
}
