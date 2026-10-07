#ifndef VOICE_MODEL_REQUEST_CAPTURE_H
#define VOICE_MODEL_REQUEST_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <openssl/evp.h>
#include "dnd_retrieval_types.h"

/* The cascade's complete JSON request capacity, excluding its terminator. */
#define VOICE_MODEL_REQUEST_MAX ((DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1023u)
#define VOICE_MODEL_REQUEST_CHUNK_MAX 8192u
#define VOICE_MODEL_REQUEST_WIRE_MAX (VOICE_MODEL_REQUEST_CHUNK_MAX + 142u)
#define VOICE_MODEL_REQUEST_SUBJECT_SUFFIX ".model-request"

typedef struct {
    char request_id[128];
    uint32_t sequence;
    uint32_t total_bytes;
    int final;
    const uint8_t *bytes;
    size_t length;
} voice_model_request_chunk;

/* Private VBus frame, distinct from public TurnEvent protobuf. Decoded bytes
 * borrow the input. The authenticated edge binds request_id before use. */
size_t voice_model_request_encode(uint8_t *out, size_t capacity,
    const voice_model_request_chunk *chunk);
int voice_model_request_decode(const uint8_t *wire, size_t length,
    voice_model_request_chunk *chunk);

typedef struct {
    EVP_MD_CTX *digest;
    size_t expected_bytes;
    size_t received_bytes;
    uint32_t next_sequence;
    int state;
    char sha256[65];
} voice_model_capture;

/* Start with a zeroed object. Every error invalidates its partial capture.
 * This collector hashes bytes only. The receiving store must validate JSON.
 * No model artifact or successful inference is attested by this protocol. */
int voice_model_capture_begin(voice_model_capture *capture, size_t bytes);
int voice_model_capture_append(voice_model_capture *capture, uint32_t sequence,
    const void *bytes, size_t length, int final);
void voice_model_capture_destroy(voice_model_capture *capture);

/* Bind the complete prepared-request hash to its authenticated owner and turn.
 * The edge supplies time and a random nonce. Upload admission checks freshness.
 * Verification of an already admitted immutable record need not expire it. */
int voice_model_capture_sign(const voice_model_capture *capture,
    const char *secret, size_t secret_len, const char *owner, const char *request_id,
    int64_t captured_at, const char *nonce, char signature[65]);
int voice_model_capture_verify(const char *sha256,
    const char *secret, size_t secret_len, const char *owner, const char *request_id,
    int64_t captured_at, const char *nonce, const char *signature);

#endif
