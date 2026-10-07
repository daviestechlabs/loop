#define _POSIX_C_SOURCE 200809L

#include "runtime_identity.h"

#include <openssl/sha.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNTIME_IDENTITY_SCHEMA "cascade-router-runtime-identity/v1"
#define RUNTIME_IDENTITY_SERVICE "cascade-router"
#define RUNTIME_POLICY_MAX 127u
#define KUBERNETES_IDENTITY_MAX 253u

static int lowercase_revision(const char *value) {
    size_t i;
    size_t length;
    if (!value) return 0;
    length = strnlen(value, 65u);
    if (length != 40u && length != 64u) return 0;
    for (i = 0u; i < length; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return 0;
    return 1;
}

static int kubernetes_identity(const char *value) {
    size_t i;
    size_t length;
    if (!value) return 0;
    length = strnlen(value, KUBERNETES_IDENTITY_MAX + 1u);
    if (length == 0u || length > KUBERNETES_IDENTITY_MAX) return 0;
    if (!((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9')))
        return 0;
    if (!((value[length - 1u] >= 'a' && value[length - 1u] <= 'z') ||
          (value[length - 1u] >= '0' && value[length - 1u] <= '9')))
        return 0;
    for (i = 1u; i + 1u < length; ++i)
        if (!((value[i] >= 'a' && value[i] <= 'z') ||
              (value[i] >= '0' && value[i] <= '9') ||
              value[i] == '-' || value[i] == '.'))
            return 0;
    return 1;
}

static int policy_token(const char *value) {
    size_t i;
    size_t length;
    if (!value) return 0;
    length = strnlen(value, RUNTIME_POLICY_MAX + 1u);
    if (length == 0u || length > RUNTIME_POLICY_MAX) return 0;
    for (i = 0u; i < length; ++i) {
        unsigned char byte = (unsigned char)value[i];
        if (!((byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
              (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
              (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
              byte == (unsigned char)'-' || byte == (unsigned char)'_' ||
              byte == (unsigned char)'.' || byte == (unsigned char)':' ||
              byte == (unsigned char)'@'))
            return 0;
    }
    return 1;
}

static int process_timestamp(
    char out[sizeof("0000-00-00T00:00:00.000000000Z")],
    const struct timespec *started_at
) {
    struct tm utc;
    time_t seconds;
    int written;
    if (!out || !started_at || started_at->tv_sec < (time_t)0 ||
        started_at->tv_nsec < 0 || started_at->tv_nsec >= 1000000000L)
        return -1;
    seconds = started_at->tv_sec;
    if (!gmtime_r(&seconds, &utc) || utc.tm_year < -1900 ||
        utc.tm_year > 8099)
        return -1;
    written = snprintf(
        out,
        sizeof("0000-00-00T00:00:00.000000000Z"),
        "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        started_at->tv_nsec);
    return written == (int)(sizeof("0000-00-00T00:00:00.000000000Z") - 1u) ?
        0 : -1;
}

static void lowercase_hex(
    const unsigned char *input,
    size_t input_len,
    char *out
) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0u; i < input_len; ++i) {
        out[i * 2u] = digits[input[i] >> 4u];
        out[i * 2u + 1u] = digits[input[i] & 15u];
    }
    out[input_len * 2u] = '\0';
}

int voice_runtime_identity_build(
    voice_runtime_identity *out,
    const char *source_revision,
    const char *pod_name,
    const char *pod_namespace,
    const char *pod_uid,
    const char *policy_version,
    const struct timespec *process_started_at
) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char started_at[sizeof("0000-00-00T00:00:00.000000000Z")];
    int written;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!lowercase_revision(source_revision) ||
        !kubernetes_identity(pod_name) ||
        !kubernetes_identity(pod_namespace) ||
        !kubernetes_identity(pod_uid) ||
        !policy_token(policy_version) ||
        process_timestamp(started_at, process_started_at) != 0)
        return -1;
    written = snprintf(
        out->body,
        sizeof(out->body),
        "{\"pod_name\":\"%s\",\"pod_namespace\":\"%s\","
        "\"pod_uid\":\"%s\",\"policy_version\":\"%s\","
        "\"process_started_at\":\"%s\",\"schema_version\":\"%s\","
        "\"service\":\"%s\",\"source_revision\":\"%s\"}",
        pod_name,
        pod_namespace,
        pod_uid,
        policy_version,
        started_at,
        RUNTIME_IDENTITY_SCHEMA,
        RUNTIME_IDENTITY_SERVICE,
        source_revision);
    if (written <= 0 || (size_t)written >= sizeof(out->body)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    out->body_len = (size_t)written;
    if (!SHA256(
            (const unsigned char *)out->body,
            out->body_len,
            digest)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    lowercase_hex(digest, sizeof(digest), out->sha256);
    return 0;
}

int voice_runtime_identity_load(
    voice_runtime_identity *out,
    const char *source_revision
) {
    const char *pod_name = getenv("POD_NAME");
    const char *pod_namespace = getenv("POD_NAMESPACE");
    const char *pod_uid = getenv("POD_UID");
    const char *policy_version = getenv("TURN_CORE_POLICY_VERSION");
    struct timespec process_started_at;
    int have_pod_name = pod_name && pod_name[0];
    int have_pod_namespace = pod_namespace && pod_namespace[0];
    int have_pod_uid = pod_uid && pod_uid[0];
    int have_policy_version = policy_version && policy_version[0];
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!have_pod_name && !have_pod_namespace && !have_pod_uid &&
        !have_policy_version)
        return 0;
    if (!have_pod_name || !have_pod_namespace || !have_pod_uid ||
        !have_policy_version ||
        clock_gettime(CLOCK_REALTIME, &process_started_at) != 0)
        return -1;
    return voice_runtime_identity_build(
               out,
               source_revision,
               pod_name,
               pod_namespace,
               pod_uid,
               policy_version,
               &process_started_at) == 0 ? 1 : -1;
}
