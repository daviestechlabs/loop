/* Bounded source and Pod identity for the pure-C interactive runtime. */
#ifndef VOICE_C_RUNTIME_IDENTITY_H
#define VOICE_C_RUNTIME_IDENTITY_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_RUNTIME_IDENTITY_PATH "/v1/runtime-identity"
#define VOICE_RUNTIME_IDENTITY_HEADER "X-Cascade-Router-Runtime-SHA256"
#define VOICE_RUNTIME_IDENTITY_HASH_LEN 64u
#define VOICE_RUNTIME_IDENTITY_BODY_CAP 1536u

typedef struct {
    char body[VOICE_RUNTIME_IDENTITY_BODY_CAP];
    size_t body_len;
    char sha256[VOICE_RUNTIME_IDENTITY_HASH_LEN + 1u];
} voice_runtime_identity;

/* Build one canonical identity from validated, source-bound values. */
int voice_runtime_identity_build(
    voice_runtime_identity *out,
    const char *source_revision,
    const char *pod_name,
    const char *pod_namespace,
    const char *pod_uid,
    const char *policy_version,
    const struct timespec *process_started_at
);

/*
 * Load the Pod identity from the environment and sample process start time.
 * Return one when enabled, zero outside Kubernetes, or minus one on drift.
 */
int voice_runtime_identity_load(
    voice_runtime_identity *out,
    const char *source_revision
);

#ifdef __cplusplus
}
#endif

#endif
