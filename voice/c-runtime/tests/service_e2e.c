/* Process-level smoke: real broker + real C services + canonical wire. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "vbus.h"
#include "gateway_http.h"
#include "runtime_identity.h"
#include "pb_min.h"
#include "service.h"
#include "subjects.h"
#include "voice_auth.h"
#include "model_request_capture.h"
#include "dnd_tools.h"
#include "http_min.h"
#include <limits.h>
#include "cmp_json.h"

#include <arpa/inet.h>
#include <openssl/sha.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#endif

#define CHILDREN_MAX 8
#define SERVICE_E2E_BENCH_DEFAULT 32
#define SERVICE_E2E_BENCH_MAX 128
#define SERVICE_E2E_BENCH_CONCURRENCY_MAX 4
#define SERVICE_E2E_BENCH_TTS_FRAMES_MAX 8
#define SERVICE_E2E_PRODUCTION_TTS_SAMPLE_RATE 24000u
#define SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES 960u
#define SERVICE_E2E_TEST_TTS_SAMPLE_RATE 16000u
#define SERVICE_E2E_TEST_TTS_FRAME_BYTES 640u
#define SERVICE_E2E_TTS_ORDER_CAPACITY 68

static pid_t children[CHILDREN_MAX];
static char child_names[CHILDREN_MAX][64];
static size_t child_count;
static int failures;
static char expected_grounding_text[DND_GROUNDING_TEXT_CAP];
static char expected_grounding_sha256[65];
static char expected_response_style[DND_GROUNDING_TEXT_CAP];
static char expected_grounded_system[DND_GROUNDING_TEXT_CAP];
static char expected_dnd_direct[DND_GROUNDING_TEXT_CAP];
static char expected_dnd_grounded[DND_GROUNDING_TEXT_CAP];
static char expected_loop_direct[DND_GROUNDING_TEXT_CAP];
static const char llm_plain_answer[] =
    "The ancient gate opens when moonlight touches the hidden \"rune\" and reveals the path ahead to any adventurer brave enough "
    "to follow the silver\\road home.";

static int load_grounding_fixture(void) {
    FILE *file = fopen("../../contracts/prompt-library/prompts/rag/grounded-response.system.txt", "rb");
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t length, i;
    int valid;
    if (!file) return -1;
    length = fread(expected_grounding_text, 1u, sizeof(expected_grounding_text) - 1u, file);
    valid = !ferror(file) && feof(file);
    if (fclose(file) != 0) valid = 0;
    if (!valid || !length) return -1;
    while (length && (expected_grounding_text[length - 1u] == '\n' ||
                     expected_grounding_text[length - 1u] == '\r')) --length;
    expected_grounding_text[length] = '\0';
    if (!SHA256((const unsigned char *)expected_grounding_text, length, digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i)
        if (snprintf(expected_grounding_sha256 + i * 2u, 3u, "%02x", (unsigned)digest[i]) != 2) return -1;
    file = fopen("../../contracts/prompt-library/prompts/voice/product-response-style.system.txt", "rb");
    if (!file) return -1;
    length = fread(expected_response_style, 1u, sizeof(expected_response_style) - 1u, file);
    valid = !ferror(file) && feof(file);
    if (fclose(file) != 0) valid = 0;
    if (!valid || !length) return -1;
    while (length && (expected_response_style[length - 1u] == '\n' ||
                     expected_response_style[length - 1u] == '\r')) --length;
    expected_response_style[length] = '\0';
    valid = snprintf(expected_grounded_system, sizeof(expected_grounded_system), "%s\n\n%s",
        expected_response_style, expected_grounding_text);
    if (valid <= 0 || (size_t)valid >= sizeof(expected_grounded_system)) return -1;
    {
        char dialogue[DND_GROUNDING_TEXT_CAP];
        file = fopen("../../contracts/prompt-library/prompts/voice/dnd-dialogue.system.txt", "rb");
        if (!file) return -1;
        length = fread(dialogue, 1u, sizeof(dialogue) - 1u, file);
        valid = !ferror(file) && feof(file);
        if (fclose(file) != 0) valid = 0;
        if (!valid || !length) return -1;
        while (length && (dialogue[length - 1u] == '\n' || dialogue[length - 1u] == '\r')) --length;
        dialogue[length] = '\0';
        valid = snprintf(expected_dnd_direct, sizeof(expected_dnd_direct), "%s\n\n%s",
            expected_response_style, dialogue);
        if (valid <= 0 || (size_t)valid >= sizeof(expected_dnd_direct)) return -1;
        valid = snprintf(expected_dnd_grounded, sizeof(expected_dnd_grounded), "%s\n\n%s",
            expected_dnd_direct, expected_grounding_text);
        if (valid <= 0 || (size_t)valid >= sizeof(expected_dnd_grounded)) return -1;
        file = fopen("../../contracts/prompt-library/prompts/voice/loop-assistant.system.txt", "rb");
        if (!file) return -1;
        length = fread(dialogue, 1u, sizeof(dialogue) - 1u, file);
        valid = !ferror(file) && feof(file);
        if (fclose(file) != 0) valid = 0;
        if (!valid || !length) return -1;
        while (length && (dialogue[length - 1u] == '\n' || dialogue[length - 1u] == '\r')) --length;
        dialogue[length] = '\0';
        valid = snprintf(expected_loop_direct, sizeof(expected_loop_direct), "%s\n\n%s",
            expected_response_style, dialogue);
        return valid > 0 && (size_t)valid < sizeof(expected_loop_direct) ? 0 : -1;
    }
}

static int loop_fixture_prompt(char *text, size_t cap, int index) {
    return snprintf(text, cap, "Loop continuity %d: %s", index,
        index == 9 ? "Explain a deliberately slow answer without casting anything." :
        index == 6 ? "Exhaust the token budget without casting anything." :
        "Explain how invisibility changes attack rolls without casting anything.");
}

/* Verify actual process history against the earlier completed fixture turns.
 * A failed model turn retains its user's speech, but adds no assistant claim. */
static int loop_request_index(const char *body) {
    cmp_json_object root, message;
    cmp_json_array messages;
    cmp_json_field element;
    char text[DND_GROUNDING_TEXT_CAP] = {0};
    int index = -1;
    if (!cmp_json_object_parse(body, &root) ||
        !cmp_json_field_array(cmp_json_object_field(&root, "messages"), &messages)) return -1;
    while (cmp_json_array_next(&messages, &element) == 1) {
        if (!cmp_json_field_object(&element, &message) ||
            !cmp_json_object_str(&message, "content", text, sizeof(text))) return -1;
    }
    return sscanf(text, "Loop continuity %d:", &index) == 1 ? index : -1;
}

static int canonical_loop_request(const char *body) {
    cmp_json_object root, message;
    cmp_json_array messages;
    cmp_json_field element;
    char role[16], text[DND_GROUNDING_TEXT_CAP], current[256], expected[256];
    int index = -1;
    size_t count = 0;
    if (!cmp_json_object_parse(body, &root) ||
        !cmp_json_field_array(cmp_json_object_field(&root, "messages"), &messages)) return 0;
    while (cmp_json_array_next(&messages, &element) == 1) {
        if (!cmp_json_field_object(&element, &message) ||
            !cmp_json_object_str(&message, "content", text, sizeof(text))) return 0;
        ++count;
    }
    if (!count || sscanf(text, "Loop continuity %d:", &index) != 1 || index < 0 || index > 10 ||
        loop_fixture_prompt(current, sizeof(current), index) <= 0 || strcmp(text, current)) return 0;
    size_t prior = index == 8 || index == 9 ? 0u : index == 10 ? 1u :
        index == 7 ? 13u : (size_t)index * 2u;
    size_t start = prior > 8u ? prior - 8u : 0u;
    if (count != prior - start + 2u ||
        !cmp_json_field_array(cmp_json_object_field(&root, "messages"), &messages) ||
        cmp_json_array_next(&messages, &element) != 1 || !cmp_json_field_object(&element, &message) ||
        !cmp_json_object_str(&message, "role", role, sizeof(role)) || strcmp(role, "system") ||
        !cmp_json_object_str(&message, "content", text, sizeof(text)) || strcmp(text, expected_loop_direct)) return 0;
    for (size_t i = start; i < prior; ++i) {
        if (cmp_json_array_next(&messages, &element) != 1 || !cmp_json_field_object(&element, &message) ||
            !cmp_json_object_str(&message, "role", role, sizeof(role)) ||
            strcmp(role, i % 2u ? "assistant" : "user") ||
            !cmp_json_object_str(&message, "content", text, sizeof(text))) return 0;
        if (i % 2u) { if (strcmp(text, llm_plain_answer)) return 0; }
        else if (loop_fixture_prompt(expected, sizeof(expected), index == 10 ? 9 : (int)(i / 2u)) <= 0 || strcmp(text, expected)) return 0;
    }
    return cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message) &&
        cmp_json_object_str(&message, "role", role, sizeof(role)) && !strcmp(role, "user") &&
        cmp_json_object_str(&message, "content", text, sizeof(text)) && !strcmp(text, current) &&
        cmp_json_array_next(&messages, &element) == 0;
}

static int canonical_model_request(const char *body, int grounded) {
    if (strstr(body, "Loop continuity ")) return grounded <= 0 && canonical_loop_request(body);
    cmp_json_object root, message;
    cmp_json_array messages;
    cmp_json_field element;
    char role[16], system[DND_GROUNDING_TEXT_CAP], context[DND_RAG_PROMPT_CAP];
    if (!cmp_json_object_parse(body, &root) ||
        !cmp_json_field_array(cmp_json_object_field(&root, "messages"), &messages) ||
        cmp_json_array_next(&messages, &element) != 1 || !cmp_json_field_object(&element, &message) ||
        !cmp_json_object_str(&message, "role", role, sizeof(role)) || strcmp(role, "system") != 0 ||
        !cmp_json_object_str(&message, "content", system, sizeof(system)) ||
        (grounded < 0 ? (strcmp(system, expected_grounded_system) != 0 &&
            strcmp(system, expected_response_style) != 0 &&
            strcmp(system, expected_dnd_grounded) != 0 && strcmp(system, expected_dnd_direct) != 0) :
            (strcmp(system, grounded ? expected_grounded_system : expected_response_style) != 0 &&
             strcmp(system, grounded ? expected_dnd_grounded : expected_dnd_direct) != 0)) ||
        cmp_json_array_next(&messages, &element) != 1 || !cmp_json_field_object(&element, &message) ||
        !cmp_json_object_str(&message, "role", role, sizeof(role)) || strcmp(role, "user") != 0 ||
        !cmp_json_object_str(&message, "content", context, sizeof(context)) ||
        cmp_json_array_next(&messages, &element) != 0) return 0;
    if (grounded <= 0) return context[0] != '\0';
    return strstr(context, "Retrieved excerpts (source data):") &&
        strstr(context, "name players-handbook;") && strstr(context, "section Sneak Attack;") &&
        strstr(context, "Sneak Attack can apply once per turn.") &&
        strstr(context, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
}

typedef struct {
    uint64_t runtime_ns;
    uint64_t runqueue_ns;
    uint64_t timeslices;
    int available;
} scheduler_sample;

static const char *last_occurrence(const char *text, const char *needle) {
    const char *match = NULL;
    const char *cursor;
    if (!text || !needle || !needle[0]) return NULL;
    cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        match = cursor;
        cursor++;
    }
    return match;
}

static size_t count_occurrences(const char *text, const char *needle) {
    size_t count = 0;
    size_t needle_len;
    if (!text || !needle || !needle[0]) return 0;
    needle_len = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += needle_len;
    }
    return count;
}

static int service_log_timestamp_valid(const char *line, size_t length) {
    static const size_t digit_positions[] = {
        0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
        11u, 12u, 14u, 15u, 17u, 18u
    };
    size_t i;
    if (!line || length < 22u || line[4] != '-' || line[7] != '-' ||
        line[10] != 'T' || line[13] != ':' || line[16] != ':' ||
        line[19] != 'Z' || line[20] != ' ' || line[21] != '[') return 0;
    for (i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); ++i) {
        char c = line[digit_positions[i]];
        if (c < '0' || c > '9') return 0;
    }
    return 1;
}

static int service_log_timestamp_matches(
    const char *line,
    time_t first,
    time_t last
) {
    time_t candidate;
    if (!line || first == (time_t)-1 || last < first ||
        difftime(last, first) > 10.0)
        return 0;
    for (candidate = first;; ++candidate) {
        char expected[21];
        struct tm tm;
        if (gmtime_r(&candidate, &tm) != NULL &&
            strftime(
                expected, sizeof(expected), "%Y-%m-%dT%H:%M:%SZ", &tm) == 20u &&
            memcmp(line, expected, 20u) == 0) return 1;
        if (candidate == last) break;
    }
    return 0;
}

static int service_log_line_matches(
    const char *line,
    size_t line_length,
    const char *suffix
) {
    size_t suffix_length;
    if (!line || !suffix || line_length < 20u ||
        !service_log_timestamp_valid(line, line_length)) return 0;
    suffix_length = strlen(suffix);
    return suffix_length == line_length - 20u &&
        memcmp(line + 20u, suffix, suffix_length) == 0;
}

enum {
    SERVICE_LOG_THREAD_COUNT = 4,
    SERVICE_LOG_LINES_PER_THREAD = 8
};

typedef struct {
    unsigned worker;
} service_log_thread_args;

static void *service_log_thread(void *opaque) {
    service_log_thread_args *args = opaque;
    unsigned line;
    for (line = 0; line < SERVICE_LOG_LINES_PER_THREAD; ++line) {
        char message[64];
        int message_len;
        if ((line & 1u) == 0u) {
            svc_log("logger-thread", "worker=%u line=%u", args->worker, line);
            continue;
        }
        message_len = snprintf(
            message, sizeof(message), "worker=%u line=%u", args->worker, line);
        if (message_len <= 0 || (size_t)message_len >= sizeof(message)) return NULL;
        svc_log_join_n(
            "logger-thread", "", 0u, message, (size_t)message_len);
    }
    return NULL;
}

static void service_log_null_format(void) {
    void (*log_fn)(const char *, const char *, ...) = svc_log;
    log_fn("audio-processor", NULL);
}

static int service_log_contract(void) {
    char captured[8192];
    char long_message[2048];
    char long_service[80];
    pthread_t threads[SERVICE_LOG_THREAD_COUNT];
    service_log_thread_args thread_args[SERVICE_LOG_THREAD_COUNT];
    int descriptors[2];
    int saved_stderr;
    size_t used = 0;
    size_t created = 0;
    ssize_t received;
    time_t first_log_time;
    time_t last_log_time;
    const char *first_end;
    const char *second;
    const char *second_end;
    const char *third;
    const char *third_end;
    const char *fourth;
    const char *fourth_end;
    const char *fifth;
    const char *fifth_end;
    const char *sixth;
    const char *sixth_end;
    const char *line;
    size_t i;
    uint32_t seen = 0;
    size_t concurrent_lines = 0;

    memset(long_message, 'x', sizeof(long_message));
    long_message[sizeof(long_message) - 1u] = '\0';
    memset(long_service, 's', sizeof(long_service));
    long_service[sizeof(long_service) - 1u] = '\0';
    if (pipe(descriptors) != 0) return -1;
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    fflush(stderr);
    if (dup2(descriptors[1], STDERR_FILENO) < 0) {
        close(saved_stderr);
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    close(descriptors[1]);
    first_log_time = time(NULL);
    svc_log("audio-processor", "message=%d value=%s", 7, "ok");
    svc_log(NULL, "fallback");
    service_log_null_format();
    svc_log_join_n("invalid", NULL, 0u, "value", sizeof("value") - 1u);
    svc_log_join_n("invalid", "field=", sizeof("field=") - 1u, NULL, 1u);
    svc_log_join_n(
        "joined",
        "field=",
        sizeof("field=") - 1u,
        long_message,
        strlen(long_message));
    svc_log_join_n(
        "joined-empty",
        "field=",
        sizeof("field=") - 1u,
        NULL,
        0u);
    svc_log_join_n(
        "joined-long",
        long_message,
        strlen(long_message),
        "ignored",
        sizeof("ignored") - 1u);
    svc_log(long_service, "%s", long_message);
    for (i = 0; i < SERVICE_LOG_THREAD_COUNT; ++i) {
        thread_args[i].worker = (unsigned)i;
        if (pthread_create(
                &threads[i], NULL, service_log_thread, &thread_args[i]) != 0)
            break;
        created++;
    }
    for (i = 0; i < created; ++i) pthread_join(threads[i], NULL);
    last_log_time = time(NULL);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) {
        close(saved_stderr);
        close(descriptors[0]);
        return -1;
    }
    close(saved_stderr);
    if (created != SERVICE_LOG_THREAD_COUNT) {
        close(descriptors[0]);
        return -1;
    }
    while (used < sizeof(captured) - 1u) {
        received = read(
            descriptors[0], captured + used, sizeof(captured) - 1u - used);
        if (received > 0) {
            used += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        break;
    }
    close(descriptors[0]);
    captured[used] = '\0';

    first_end = strchr(captured, '\n');
    if (!first_end || !service_log_line_matches(
            captured,
            (size_t)(first_end + 1 - captured),
            " [audio-processor] message=7 value=ok\n") ||
        !service_log_timestamp_matches(
            captured, first_log_time, last_log_time)) return -1;
    second = first_end + 1;
    second_end = strchr(second, '\n');
    if (!second_end || !service_log_line_matches(
            second,
            (size_t)(second_end + 1 - second),
            " [unknown] fallback\n") ||
        !service_log_timestamp_matches(
            second, first_log_time, last_log_time)) return -1;
    third = second_end + 1;
    third_end = strchr(third, '\n');
    if (!third_end ||
        (size_t)(third_end + 1 - third) != 1054u ||
        !service_log_timestamp_valid(third, (size_t)(third_end + 1 - third)) ||
        !service_log_timestamp_matches(
            third, first_log_time, last_log_time) ||
        memcmp(third + 20u, " [joined] field=", 16u) != 0) return -1;
    for (i = 0; i < 1017u; ++i) {
        if (third[36u + i] != 'x') return -1;
    }
    fourth = third_end + 1;
    fourth_end = strchr(fourth, '\n');
    if (!fourth_end || !service_log_line_matches(
            fourth,
            (size_t)(fourth_end + 1 - fourth),
            " [joined-empty] field=\n") ||
        !service_log_timestamp_matches(
            fourth, first_log_time, last_log_time)) return -1;
    fifth = fourth_end + 1;
    fifth_end = strchr(fifth, '\n');
    if (!fifth_end ||
        (size_t)(fifth_end + 1 - fifth) != 1059u ||
        !service_log_timestamp_valid(
            fifth, (size_t)(fifth_end + 1 - fifth)) ||
        !service_log_timestamp_matches(
            fifth, first_log_time, last_log_time) ||
        memcmp(fifth + 20u, " [joined-long] ", 15u) != 0) return -1;
    for (i = 0; i < 1023u; ++i) {
        if (fifth[35u + i] != 'x') return -1;
    }
    sixth = fifth_end + 1;
    sixth_end = strchr(sixth, '\n');
    if (!sixth_end ||
        (size_t)(sixth_end + 1 - sixth) != 1111u ||
        !service_log_timestamp_valid(
            sixth, (size_t)(sixth_end + 1 - sixth)) ||
        !service_log_timestamp_matches(
            sixth, first_log_time, last_log_time)) return -1;
    for (i = 0; i < 63u; ++i) {
        if (sixth[22u + i] != 's') return -1;
    }
    if (sixth[85] != ']' || sixth[86] != ' ') return -1;
    for (i = 0; i < 1023u; ++i) {
        if (sixth[87u + i] != 'x') return -1;
    }
    line = sixth_end + 1;
    while (*line) {
        const char *line_end = strchr(line, '\n');
        unsigned worker;
        unsigned entry;
        unsigned bit;
        int consumed = 0;
        if (!line_end || !service_log_timestamp_valid(
                line, (size_t)(line_end + 1 - line)) ||
            !service_log_timestamp_matches(
                line, first_log_time, last_log_time) ||
            sscanf(
                line + 20u,
                " [logger-thread] worker=%u line=%u%n",
                &worker,
                &entry,
                &consumed) != 2 ||
            consumed < 0 || line + 20u + (size_t)consumed != line_end ||
            worker >= SERVICE_LOG_THREAD_COUNT ||
            entry >= SERVICE_LOG_LINES_PER_THREAD) return -1;
        bit = worker * SERVICE_LOG_LINES_PER_THREAD + entry;
        if ((seen & (UINT32_C(1) << bit)) != 0) return -1;
        seen |= UINT32_C(1) << bit;
        concurrent_lines++;
        line = line_end + 1;
    }
    return concurrent_lines ==
            SERVICE_LOG_THREAD_COUNT * SERVICE_LOG_LINES_PER_THREAD &&
        seen == UINT32_MAX ? 0 : -1;
}

static int parse_uint64_field(
    const char *text,
    const char *field,
    uint64_t *value
) {
    const char *start;
    char *end;
    unsigned long long parsed;
    if (!text || !field || !field[0] || !value) return -1;
    start = strstr(text, field);
    if (!start) return -1;
    start += strlen(field);
    if (*start < '0' || *start > '9') return -1;
    errno = 0;
    parsed = strtoull(start, &end, 10);
    if (errno != 0 || end == start) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int startup_settle_ms(void) {
    const char *value = getenv("SERVICE_E2E_STARTUP_SETTLE_MS");
    char *end = NULL;
    long parsed;
    if (!value || !value[0]) return 200;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 200 || parsed > 5000)
        return 200;
    return (int)parsed;
}

static int bench_turn_count(void) {
    const char *value = getenv("SERVICE_E2E_BENCH_TURNS");
    char *end = NULL;
    long parsed;
    if (!value || !value[0]) return SERVICE_E2E_BENCH_DEFAULT;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 ||
        parsed > SERVICE_E2E_BENCH_MAX)
        return SERVICE_E2E_BENCH_DEFAULT;
    return (int)parsed;
}

static int bench_concurrency_count(void) {
    const char *value = getenv("SERVICE_E2E_BENCH_CONCURRENCY");
    char *end = NULL;
    long parsed;
    if (!value || !value[0]) return 1;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 1 ||
        parsed > SERVICE_E2E_BENCH_CONCURRENCY_MAX)
        return 1;
    return (int)parsed;
}

static int bench_tts_frame_count(void) {
    const char *value = getenv("SERVICE_E2E_BENCH_TTS_FRAMES");
    char *end = NULL;
    long parsed;
    if (!value || !value[0]) return SERVICE_E2E_BENCH_TTS_FRAMES_MAX;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 1 ||
        parsed > SERVICE_E2E_BENCH_TTS_FRAMES_MAX)
        return SERVICE_E2E_BENCH_TTS_FRAMES_MAX;
    return (int)parsed;
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

typedef struct {
    int listen_fd;
    atomic_int stop;
    atomic_int accepts;
    atomic_int requests;
    atomic_int valid_requests;
    atomic_int keepalive_requests;
    atomic_int reused_requests;
    atomic_int first_tail_sent;
    atomic_int responses_sent;
    atomic_uint_fast64_t first_llm_accept_at_ns;
    atomic_uint_fast64_t first_llm_request_at_ns;
    atomic_uint_fast64_t first_llm_response_at_ns;
    atomic_int llm_active_requests;
    atomic_int llm_max_active_requests;
    atomic_int llm_provider_requests;
    atomic_int pipeline_cancel_active_requests;
    atomic_int pipeline_cancel_max_active_requests;
    atomic_int late_error_requests;
    atomic_int late_error_pcm_sent;
    atomic_int late_error_disconnects;
    atomic_int cancel_header_sent;
    atomic_int partial_header_sent;
    atomic_int escaped_quote_requests;
    atomic_int escaped_backslash_requests;
    atomic_int model_markup_requests;
    atomic_uint_fast64_t benchmark_request_at_ns[SERVICE_E2E_BENCH_MAX];
    atomic_uint_fast64_t benchmark_response_at_ns[SERVICE_E2E_BENCH_MAX];
    int expected_requests;
    int benchmark_pcm_frames;
} fake_tts;

typedef struct {
    fake_tts *server;
    int client;
    uint64_t accepted_at_ns;
} fake_tts_connection;

typedef struct {
    int listen_fd;
    atomic_int accepts;
    atomic_int requests;
    atomic_int valid_requests;
    atomic_int keepalive_requests;
    atomic_size_t first_request_bytes;
} fake_stt;

typedef struct {
    int listen_fd;
    atomic_int accepts;
    atomic_int requests;
    atomic_int valid_requests;
    atomic_int keepalive_requests;
    atomic_int slow_request_started;
    int search;
    char reply[16384];
} fake_embed;

typedef struct {
    int listen_fd;
    atomic_int accepts;
    atomic_int requests;
    atomic_int valid_requests;
    atomic_int keepalive_requests;
    atomic_int reused_requests;
    atomic_int grounded_requests;
    atomic_uint_fast64_t policy_connection_at_ns;
    atomic_int policy_connection_failures;
    atomic_int first_tail_sent;
    atomic_uint_fast64_t first_accept_at_ns;
    atomic_uint_fast64_t first_request_at_ns;
    atomic_uint_fast64_t first_response_at_ns;
    atomic_int benchmark_requests;
    atomic_int benchmark_binding_failures;
    atomic_uint_fast64_t benchmark_request_at_ns[SERVICE_E2E_BENCH_MAX];
    atomic_uint_fast64_t benchmark_response_at_ns[SERVICE_E2E_BENCH_MAX];
    atomic_int *late_error_tts_pcm_sent;
    int expected_requests;
} fake_llm;

typedef struct {
    fake_llm *server;
    int client;
    uint64_t accepted_at_ns;
} fake_llm_connection;

typedef struct {
    int text_delta;
    int text_delta_clean_channels;
    int text_before_segment;
    int final_event;
    int segment_event;
    int segment_before_pcm;
    int pcm_started;
    int pcm_chunk;
    int staged_pcm_chunks;
    int pcm_ended;
    int pcm_end_after_chunk;
    int completed;
    int canceled;
    int failed;
    int completion_limited;
    int clean_channels;
    int audio_fields;
    int pcm_bytes_valid;
    int pcm_sequence_ordered;
    int pcm_final_seen;
    int pcm_final_sequence;
    char final_text[2048];
    turn_stage_timestamps_c pcm_started_stages;
    turn_stage_timestamps_c first_pcm_stages;
} turn_seen;

typedef struct {
    uint64_t turn_start_at_ns;
    int product_metadata_seen;
    uint64_t turn_generate_at_ns;
    uint64_t tts_speak_at_ns;
    size_t first_tts_segment_bytes;
    int tts_text_segments;
    int tts_final_markers;
    char response_subject[256];
    int first_tts_segment_final;
    int first_tts_segment_deferred;
    int response_subject_seen;
} bus_stage_seen;

typedef struct {
    int starts;
    int chunks;
    int ends;
    int ordered;
    int valid_pcm;
} gateway_stream_seen;

enum {
    GATEWAY_AUDIO_SPEECH_FRAMES = 1,
    GATEWAY_AUDIO_SILENCE_FRAMES = 41,
    GATEWAY_AUDIO_TOTAL_FRAMES =
        GATEWAY_AUDIO_SPEECH_FRAMES + GATEWAY_AUDIO_SILENCE_FRAMES
};

static turn_seen normal_seen;
static turn_seen cancel_seen;
static turn_seen early_cancel_seen;
static turn_seen auto_seen;
static turn_seen llm_seen;
static turn_seen llm_cancel_seen;
static turn_seen llm_late_error_seen;
static turn_seen rag_success_seen;
static turn_seen rag_cancel_seen;
static turn_seen pipeline_cancel_seen;
static turn_seen subject_bind_seen;
static turn_seen fallback_subject_seen;
static turn_seen max_derived_subject_seen;
static turn_seen future_stage_seen;
static turn_seen partial_pcm_seen;
static turn_seen tts_header_only_seen;
static turn_seen tts_truncated_seen;
static turn_seen tts_recovered_seen;
static turn_seen deferred_single_seen;
static turn_seen deferred_missing_seen;
static turn_seen deferred_bad_marker_seen;
static turn_seen deferred_mode_mismatch_seen;
static turn_seen deferred_orphan_marker_seen;
static turn_seen tts_index_collision_a_seen;
static turn_seen tts_index_collision_b_seen;
static turn_seen tts_order_capacity_seen;
static turn_seen tts_order_overflow_seen;
static turn_seen tts_order_capacity_recovered_seen;
static turn_seen stt_failure_seen;
static turn_seen stt_recovered_seen;
static turn_seen stt_silence_seen;
static turn_seen pcm_rejected_seen;
static turn_seen disconnect_seen;
static turn_seen no_tts_seen;
static turn_seen budget_valid_seen;
static turn_seen budget_invalid_seen;
static turn_seen budget_expired_seen;
static turn_seen token_idle_seen;
static turn_seen token_flush_seen;
static bus_stage_seen llm_bus_stages;
static gateway_stream_seen gateway_audio_seen = {
    .ordered = 1,
    .valid_pcm = 1,
};
static int rejected_audio_canceled;
static int abandoned_audio_started;
static int expired_audio_canceled;
static int recovered_audio_started;
static int refreshed_audio_chunks;
static int refreshed_audio_canceled;
static int direct_idle_audio_started;
static int direct_idle_audio_ended;
static int audio_store_rejected_ended;
static int audio_store_recovered_started;
static int audio_store_recovered_ended;
static int oversized_audio_chunk_ended;
static int token_idle_failures;
static int rag_requests_seen;
static int lifecycle_started;
static int lifecycle_ended;
static int transcription_final;
static int transcription_schema_clean;
static int transcription_stage_clean;
static int transcription_failed_lifecycle;
static int endpoint_reflex_request_bound;
static int rejected_reflex_request_bound;
static int llm_pcm_ordered;
static int llm_pcm_last_segment;
static int llm_pcm_segments;
static int llm_pcm_final_seen;
static int subject_bind_wrong_subject;
static int fallback_wrong_subject;
static int max_derived_wrong_subject;
static char max_derived_request_id[128];
static char max_derived_subject[160];

static void expect(const char *name, int condition) {
    if (condition) printf("PASS %s\n", name);
    else {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static int stages_complete_and_ordered(const turn_stage_timestamps_c *stages) {
    return stages && stages->first_text_at_ms > 0 &&
        stages->tts_segment_emitted_at_ms >= stages->first_text_at_ms &&
        stages->tts_request_received_at_ms >=
            stages->tts_segment_emitted_at_ms &&
        stages->tts_provider_request_started_at_ms >=
            stages->tts_request_received_at_ms &&
        stages->tts_provider_ready_at_ms >=
            stages->tts_provider_request_started_at_ms &&
        stages->pcm_started_at_ms >= stages->tts_provider_ready_at_ms &&
        stages->pcm_first_chunk_at_ms >= stages->pcm_started_at_ms;
}

static int input_stages_complete_and_ordered(
    const turn_input_stage_timestamps_c *stages
) {
    return stages && stages->audio_committed_at_ms > 0 &&
        stages->stt_request_received_at_ms >= stages->audio_committed_at_ms &&
        stages->stt_provider_request_started_at_ms >=
            stages->stt_request_received_at_ms &&
        stages->stt_provider_ready_at_ms >=
            stages->stt_provider_request_started_at_ms &&
        stages->stt_transcript_published_at_ms >=
            stages->stt_provider_ready_at_ms;
}

static int request_session_append(
    vbus_client *client,
    const char *session_id,
    const char *user_id,
    const char *content
) {
    session_append_request_c request;
    session_append_response_c response;
    uint8_t request_wire[8192];
    uint8_t response_wire[256];
    size_t request_len;
    size_t response_len = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.session_id, sizeof(request.session_id), "%s", session_id);
    snprintf(request.user_id, sizeof(request.user_id), "%s", user_id);
    snprintf(request.message.role, sizeof(request.message.role), "user");
    snprintf(request.message.content, sizeof(request.message.content), "%s", content);
    snprintf(request.message.request_id, sizeof(request.message.request_id), "index-e2e");
    request.message.timestamp_ms = 12345;
    request_len = pb_encode_session_append_request(
        request_wire, sizeof(request_wire), &request);
    if (!request_len || vbus_request(
            client, SUBJ_SESSION_APPEND, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len, 1000) != 0 ||
        pb_decode_session_append_response(
            response_wire, response_len, &response) != 0)
        return -1;
    return response.message_count;
}

static int request_session_append_error(
    vbus_client *client,
    const char *session_id,
    const char *user_id,
    const char *content,
    const char *expected_type
) {
    session_append_request_c request;
    error_response_c response;
    uint8_t request_wire[8192];
    uint8_t response_wire[512];
    size_t request_len;
    size_t response_len = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.session_id, sizeof(request.session_id), "%s", session_id);
    snprintf(request.user_id, sizeof(request.user_id), "%s", user_id);
    snprintf(request.message.role, sizeof(request.message.role), "user");
    snprintf(request.message.content, sizeof(request.message.content), "%s", content);
    snprintf(request.message.request_id, sizeof(request.message.request_id), "capacity-e2e");
    request.message.timestamp_ms = 12345;
    request_len = pb_encode_session_append_request(
        request_wire, sizeof(request_wire), &request);
    if (!request_len || vbus_request(
            client, SUBJ_SESSION_APPEND, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len, 1000) != 0 ||
        pb_decode_error_response(response_wire, response_len, &response) != 0)
        return -1;
    return response.error && strcmp(response.type, expected_type) == 0 ? 0 : -1;
}

static int request_session_get_n(
    vbus_client *client,
    const char *session_id,
    const char *user_id,
    const char *expected_content,
    int32_t last_n
) {
    session_get_request_c request;
    session_get_response_c response;
    uint8_t request_wire[512];
    uint8_t response_wire[8192];
    size_t request_len;
    size_t response_len = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.session_id, sizeof(request.session_id), "%s", session_id);
    snprintf(request.user_id, sizeof(request.user_id), "%s", user_id);
    request.last_n = last_n;
    request_len = pb_encode_session_get_request(
        request_wire, sizeof(request_wire), &request);
    if (!request_len || vbus_request(
            client, SUBJ_SESSION_GET, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len, 1000) != 0 ||
        pb_decode_session_get_response(response_wire, response_len, &response) != 0)
        return -1;
    if (expected_content &&
        (response.message_count != 1 ||
         strcmp(response.first_message.content, expected_content) != 0))
        return -1;
    return (int)response.message_count;
}

static int request_session_get(
    vbus_client *client,
    const char *session_id,
    const char *user_id,
    const char *expected_content
) {
    return request_session_get_n(client, session_id, user_id, expected_content, 1);
}

static int request_session_delete(
    vbus_client *client,
    const char *session_id,
    const char *user_id
) {
    session_id_request_c request;
    session_delete_response_c response;
    uint8_t request_wire[512];
    uint8_t response_wire[256];
    size_t request_len;
    size_t response_len = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.session_id, sizeof(request.session_id), "%s", session_id);
    snprintf(request.user_id, sizeof(request.user_id), "%s", user_id);
    request_len = pb_encode_session_id_request(
        request_wire, sizeof(request_wire), &request);
    if (!request_len || vbus_request(
            client, SUBJ_SESSION_DELETE, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len, 1000) != 0 ||
        pb_decode_session_delete_response(
            response_wire, response_len, &response) != 0)
        return -1;
    return response.deleted;
}

static int request_rag_search(vbus_client *client, const char *request_id) {
    rag_search_request_c request;
    rag_search_response_c response;
    uint8_t request_wire[4096];
    uint8_t response_wire[4096];
    size_t request_len;
    size_t response_len = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.request_id, sizeof(request.request_id), "%s", request_id);
    snprintf(request.query, sizeof(request.query), "rules query");
    snprintf(request.user_id, sizeof(request.user_id), "user-rag");
    snprintf(request.knowledge_scope, sizeof(request.knowledge_scope), "shared_rulebook");
    request.premium = 1;
    request.top_k = 4;
    request_len = pb_encode_rag_search_request(
        request_wire, sizeof(request_wire), &request);
    if (!request_len || vbus_request(
            client, SUBJ_RAG_SEARCH, request_wire, request_len,
            response_wire, sizeof(response_wire), &response_len, 1000) != 0 ||
        pb_decode_rag_search_response(response_wire, response_len, &response) != 0)
        return -1;
    return strcmp(response.request_id, request_id) == 0 && response.used_rag &&
        response.documents.count == 1u &&
        strcmp(response.documents.hits[0].citation.book_slug, "players-handbook") == 0 &&
        strcmp(response.documents.hits[0].citation.source_sha256,
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0;
}

static int public_rag_citation(const char *response) {
    const char *start = strstr(response, "{\"type\":\"text_completed\"");
    const char *end;
    char line[8192], encoded[4096], value[256];
    cmp_json_object event, metadata, citation;
    cmp_json_array array;
    cmp_json_field element;
    double number;
    if (!start || !(end = strchr(start, '\n')) || (size_t)(end - start) >= sizeof(line)) return 0;
    memcpy(line, start, (size_t)(end - start)); line[end - start] = '\0';
    if (!cmp_json_object_parse(line, &event) || !cmp_json_object_object(&event, "metadata", &metadata) ||
        !cmp_json_object_str(&metadata, "cascade_grounding_prompt_id", value, sizeof(value)) ||
        strcmp(value, DND_GROUNDING_PROMPT_ID) != 0 ||
        !cmp_json_object_str(&metadata, "cascade_grounding_prompt_version", value, sizeof(value)) || strcmp(value, "v4") != 0 ||
        !cmp_json_object_str(&metadata, "cascade_grounding_prompt_sha256", value, sizeof(value)) ||
        strcmp(value, expected_grounding_sha256) != 0 ||
        !cmp_json_object_str(&metadata, "cascade_retrieved_documents", value, sizeof(value)) || strcmp(value, "1") != 0 ||
        !cmp_json_object_str(&metadata, "cascade_retrieval_citations", encoded, sizeof(encoded)) ||
        !cmp_json_array_parse(encoded, &array) || cmp_json_array_next(&array, &element) != 1 ||
        !cmp_json_field_object(&element, &citation)) return 0;
    return cmp_json_object_str(&citation, "source", value, sizeof(value)) && strcmp(value, "book://players-handbook") == 0 &&
        cmp_json_object_str(&citation, "source_sha256", value, sizeof(value)) &&
        strcmp(value, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0 &&
        cmp_json_object_str(&citation, "content_hash", value, sizeof(value)) &&
        strcmp(value, "3d5d9eb3ca84da74316b9fd72406305d69018ffc9e9df7916d99c345ed390425") == 0 &&
        cmp_json_field_double(cmp_json_object_field(&citation, "page_start"), &number) && number == 96.0 &&
        cmp_json_field_double(cmp_json_object_field(&citation, "score"), &number) && number == 0.875 &&
        cmp_json_array_next(&array, &element) == 0;
}

static void test_rag_request_denials(vbus_client *client) {
    size_t i;
    for (i = 0; i < 8u; ++i) {
        rag_search_request_c request = {0};
        rag_search_response_c response;
        uint8_t request_wire[4096], response_wire[4096];
        size_t request_len, response_len = 0;
        strcpy(request.request_id, "rag-denied");
        strcpy(request.user_id, "user-rag");
        strcpy(request.campaign_id, "campaign-rag");
        strcpy(request.query, "rules query");
        strcpy(request.knowledge_scope, "shared_rulebook");
        request.premium = 1;
        request.top_k = 4;
        switch (i) {
        case 0: request.deadline_unix_ms = 1; break;
        case 1: strcpy(request.collection, "caller-selected-collection"); break;
        case 2: request.user_id[0] = '\0'; break;
        case 3: request.top_k = 5; break;
        case 4: request.enable_rerank = 1; break;
        case 5: strcpy(request.knowledge_scope, "owned_rulebook"); break;
        case 6:
            strcpy(request.knowledge_scope, "campaign_canon");
            request.premium = 0;
            break;
        default: strcpy(request.knowledge_scope, "caller-scope"); break;
        }
        request_len = pb_encode_rag_search_request(request_wire, sizeof(request_wire), &request);
        expect("RAG rejects invalid authority, scope, options, or deadline before HTTP",
            request_len && vbus_request(client, SUBJ_RAG_SEARCH, request_wire, request_len,
                response_wire, sizeof(response_wire), &response_len, 1000) == 0 &&
            pb_decode_rag_search_response(response_wire, response_len, &response) == 0 &&
            strcmp(response.request_id, request.request_id) == 0 && response.error[0] &&
            !response.used_rag && !response.documents.count && !response.context_text[0]);
    }
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void record_first_value(atomic_uint_fast64_t *target, uint64_t value) {
    uint_fast64_t expected = 0;
    if (!target || value == 0) return;
    (void)atomic_compare_exchange_strong_explicit(
        target, &expected, (uint_fast64_t)value,
        memory_order_relaxed, memory_order_relaxed);
}

static void record_first_timestamp(atomic_uint_fast64_t *target) {
    record_first_value(target, mono_ns());
}

static void record_min_value(atomic_uint_fast64_t *target, uint64_t value) {
    uint_fast64_t current;
    if (!target || value == 0) return;
    current = atomic_load_explicit(target, memory_order_relaxed);
    while ((current == 0 || (uint_fast64_t)value < current) &&
           !atomic_compare_exchange_weak_explicit(
               target,
               &current,
               (uint_fast64_t)value,
               memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

static int benchmark_turn_index(const char *body) {
    static const char prefix[] = "\"turn_id\":\"req-bench-";
    const char *digits;
    int index;
    if (!body) return -1;
    digits = strstr(body, prefix);
    if (!digits) return -1;
    digits += sizeof(prefix) - 1u;
    if (strlen(digits) < 4u || digits[0] < '0' || digits[0] > '9' ||
        digits[1] < '0' || digits[1] > '9' ||
        digits[2] < '0' || digits[2] > '9' || digits[3] != '"') return -1;
    index = (digits[0] - '0') * 100 +
        (digits[1] - '0') * 10 + (digits[2] - '0');
    return index < SERVICE_E2E_BENCH_MAX ? index : -1;
}

static int benchmark_prompt_index(const char *body) {
    static const char prefix[] = "Benchmark sample ";
    const char *digits;
    int index;
    if (!body) return -1;
    digits = strstr(body, prefix);
    if (!digits) return -1;
    digits += sizeof(prefix) - 1u;
    if (strlen(digits) < 4u || digits[0] < '0' || digits[0] > '9' ||
        digits[1] < '0' || digits[1] > '9' ||
        digits[2] < '0' || digits[2] > '9' || digits[3] != ':') return -1;
    index = (digits[0] - '0') * 100 +
        (digits[1] - '0') * 10 + (digits[2] - '0');
    return index < SERVICE_E2E_BENCH_MAX ? index : -1;
}

static int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int accept_bounded(int listen_fd, int timeout_ms) {
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0 || (pfd.revents & POLLIN) == 0) return -1;
    return accept(listen_fd, NULL, NULL);
}

static int reserve_loopback_port(void) {
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int port;
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(fd);
        return -1;
    }
    port = (int)ntohs(address.sin_port);
    close(fd);
    return port;
}

static int check_gateway_health(int port) {
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    char response[256];
    int attempt;
    for (attempt = 0; attempt < 50; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        ssize_t n;
        if (fd < 0) return -1;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons((uint16_t)port);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            size_t used = 0;
            (void)write_all(fd, "GET /healthz HTTP/1.0\r\n\r\n", 25);
            while (used + 1 < sizeof(response)) {
                n = recv(fd, response + used, sizeof(response) - used - 1, 0);
                if (n <= 0) break;
                used += (size_t)n;
            }
            close(fd);
            if (used > 0) {
                response[used] = '\0';
                return strstr(response, "200 OK") && strstr(response, "\"live\":true") ? 0 : -1;
            }
        } else {
            close(fd);
        }
        {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&delay, NULL);
        }
    }
    return -1;
}

static int gateway_connect(int port) {
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int gateway_read_response_observe(
    int fd,
    char *out,
    size_t out_cap,
    uint64_t *first_pcm_at_ns
) {
    size_t used = 0;
    if (!out || out_cap == 0) return -1;
    if (first_pcm_at_ns) *first_pcm_at_ns = 0;
    while (used + 1 < out_cap) {
        ssize_t count = recv(fd, out + used, out_cap - used - 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        if (count <= 0) break;
        used += (size_t)count;
        out[used] = '\0';
        if (first_pcm_at_ns && *first_pcm_at_ns == 0 &&
            strstr(out, "\"type\":\"pcm_chunk\"") != NULL)
            *first_pcm_at_ns = mono_ns();
    }
    out[used] = '\0';
    return used > 0 ? 0 : -1;
}

static int gateway_read_response(int fd, char *out, size_t out_cap) {
    return gateway_read_response_observe(fd, out, out_cap, NULL);
}

static int gateway_raw_request(int port, const void *request, size_t request_len,
                               char *response, size_t response_cap) {
    int fd = gateway_connect(port);
    int result = -1;
    if (fd < 0 || !request || request_len == 0) return -1;
    if (write_all(fd, request, request_len) == 0) {
        (void)shutdown(fd, SHUT_WR);
        result = gateway_read_response(fd, response, response_cap);
    }
    close(fd);
    return result;
}

static int gateway_runtime_identity(
    int port,
    char hash_out[VOICE_RUNTIME_IDENTITY_HASH_LEN + 1u]
) {
    static const char request[] =
        "GET " VOICE_RUNTIME_IDENTITY_PATH " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n\r\n";
    static const char hash_header[] = VOICE_RUNTIME_IDENTITY_HEADER ": ";
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char expected_hash[VOICE_RUNTIME_IDENTITY_HASH_LEN + 1u];
    char expected_length[64];
    char response[4096];
    const char *header;
    const char *body;
    size_t i;
    if (!hash_out ||
        gateway_raw_request(
            port,
            request,
            sizeof(request) - 1u,
            response,
            sizeof(response)) != 0 ||
        strstr(response, "HTTP/1.1 200 OK") == NULL ||
        strstr(response, "Content-Type: application/json\r\n") == NULL ||
        strstr(response, "Cache-Control: no-store\r\n") == NULL)
        return -1;
    header = strstr(response, hash_header);
    body = strstr(response, "\r\n\r\n");
    if (!header || !body) return -1;
    header += sizeof(hash_header) - 1u;
    body += 4u;
    if (snprintf(
            expected_length,
            sizeof(expected_length),
            "Content-Length: %zu\r\n",
            strlen(body)) <= 0 ||
        strstr(response, expected_length) == NULL ||
        header[VOICE_RUNTIME_IDENTITY_HASH_LEN] != '\r' ||
        strstr(body, "\"pod_name\":\"c-voice-runtime-poc-canary-test\"") == NULL ||
        strstr(body, "\"pod_namespace\":\"ai-ml\"") == NULL ||
        strstr(
            body,
            "\"pod_uid\":\"2f1c93ce-69aa-4a43-817a-42fe9f05d341\"") == NULL ||
        strstr(body, "\"policy_version\":\"turn-core-v1\"") == NULL ||
        strstr(
            body,
            "\"schema_version\":\"cascade-router-runtime-identity/v1\"") == NULL ||
        strstr(body, "\"service\":\"cascade-router\"") == NULL ||
        strstr(
            body,
            "\"source_revision\":\"" VOICE_SOURCE_REVISION "\"}") == NULL ||
        !SHA256((const unsigned char *)body, strlen(body), digest))
        return -1;
    for (i = 0u; i < sizeof(digest); ++i) {
        expected_hash[i * 2u] = digits[digest[i] >> 4u];
        expected_hash[i * 2u + 1u] = digits[digest[i] & 15u];
    }
    expected_hash[VOICE_RUNTIME_IDENTITY_HASH_LEN] = '\0';
    if (memcmp(header, expected_hash, VOICE_RUNTIME_IDENTITY_HASH_LEN) != 0)
        return -1;
    memcpy(hash_out, expected_hash, sizeof(expected_hash));
    return 0;
}

static int gateway_fragmented_request(
    int port,
    const void *request,
    size_t request_len,
    size_t split,
    char *response,
    size_t response_cap
) {
    const uint8_t *bytes = (const uint8_t *)request;
    struct timespec fragment_wait = {.tv_sec = 0, .tv_nsec = 20000000L};
    int fd = gateway_connect(port);
    int result = -1;
    if (fd < 0 || !request || split == 0u || split >= request_len) return -1;
    if (write_all(fd, bytes, split) == 0) {
        nanosleep(&fragment_wait, NULL);
        if (write_all(fd, bytes + split, request_len - split) == 0) {
            (void)shutdown(fd, SHUT_WR);
            result = gateway_read_response(fd, response, response_cap);
        }
    }
    close(fd);
    return result;
}

static int gateway_capacity_is_bounded(int port) {
    int clients[40];
    int extra = -1;
    char response[512];
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 100000000L};
    int i;
    int result = -1;
    for (i = 0; i < 40; ++i) clients[i] = -1;
    for (i = 0; i < 40; ++i) {
        clients[i] = gateway_connect(port);
        if (clients[i] < 0 || write_all(clients[i], "G", 1) != 0) goto done;
    }
    nanosleep(&settle, NULL);
    extra = gateway_connect(port);
    if (extra >= 0 && gateway_read_response(extra, response, sizeof(response)) == 0 &&
        strstr(response, "503 Service Unavailable") != NULL &&
        strstr(response, "{\"error\":\"busy\"}") != NULL) result = 0;
done:
    if (extra >= 0) close(extra);
    for (i = 0; i < 40; ++i)
        if (clients[i] >= 0) close(clients[i]);
    nanosleep(&settle, NULL);
    return result;
}

static int gateway_post_turn_observe_ex(
    int port,
    const char *token,
    const char *request_id,
    const char *text,
    const char *header_user,
    const char *body_user,
    int64_t timestamp,
    const char *fixed_nonce,
    int enable_tts,
    int enable_rag,
    int tamper_body,
    const char *extra_headers,
    char *response,
    size_t response_cap,
    uint64_t *first_pcm_at_ns,
    uint64_t *request_started_at_ns
) {
    turn_start_c turn;
    uint8_t wire[4096];
    char header[1024];
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    size_t wire_len;
    int header_len;
    int fd;
    if (!extra_headers) return -1;
    if (timestamp == 0) timestamp = (int64_t)time(NULL);
    memset(&turn, 0, sizeof(turn));
    snprintf(turn.request_id, sizeof(turn.request_id), "%s", request_id);
    snprintf(turn.user_id, sizeof(turn.user_id), "%s", body_user);
    snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e");
    snprintf(turn.text, sizeof(turn.text), "%s", text);
    turn.enable_tts = enable_tts;
    turn.enable_rag = enable_rag;
    turn.premium = enable_rag;
    if (strcmp(request_id, "req-policy-rag-ceiling-2") == 0)
        strcpy(turn.metadata.turn_max_tokens, "1000000");
    else if (strcmp(request_id, "req-policy-rag-length") == 0 ||
             strcmp(request_id, "req-llm-late-limit") == 0)
        strcpy(turn.metadata.turn_max_tokens, "8");
    else if (strcmp(request_id, "req-policy-invalid") == 0)
        strcpy(turn.metadata.turn_max_tokens, "0");
    if (strcmp(request_id, "req-llm") == 0 || strncmp(request_id, "req-dice-", 9u) == 0 ||
        strncmp(request_id, "req-encounter-", 14u) == 0 ||
        strncmp(request_id, "req-governed-", sizeof("req-governed-") - 1u) == 0) {
        snprintf(turn.metadata.interaction_profile, sizeof(turn.metadata.interaction_profile), "dnd_app");
        snprintf(turn.metadata.client_transport, sizeof(turn.metadata.client_transport), "http-turn-stream");
        snprintf(turn.metadata.campaign_id, sizeof(turn.metadata.campaign_id), "campaign-e2e");
        if (strncmp(request_id, "req-encounter-", 14u) == 0 && strcmp(request_id, "req-encounter-missing"))
            strcpy(turn.metadata.encounter_id, "battle");
        snprintf(turn.metadata.knowledge_scope, sizeof(turn.metadata.knowledge_scope), "shared_rulebook");
        snprintf(turn.metadata.retrieval_force, sizeof(turn.metadata.retrieval_force), "true");
        if (strcmp(request_id, "req-governed-owner") == 0 || strcmp(request_id, "req-governed-campaign") == 0)
            strcpy(turn.metadata.knowledge_scope, "campaign_canon");
    }
    if (strncmp(request_id, "req-loop-", 9u) == 0) {
        strcpy(turn.metadata.interaction_profile, "realtime_voice");
        strcpy(turn.metadata.client_surface, "loop");
        strcpy(turn.session_id, !strncmp(request_id, "req-loop-scene-", 15u) ? "loop-scene" :
            !strncmp(request_id, "req-loop-rules-", 15u) ? "loop-rules" :
            !strcmp(request_id, "req-loop-8") ? "loop-fresh" :
            (!strcmp(request_id, "req-loop-9") || !strcmp(request_id, "req-loop-10")) ? "loop-cancel" : "loop-fixture");
        turn.enable_tts = 0;
        if (!strcmp(request_id, "req-loop-6")) strcpy(turn.metadata.turn_max_tokens, "8");
    }
    snprintf(turn.response_subject, sizeof(turn.response_subject), "ai.private.must-not-win");
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    if (fixed_nonce) snprintf(nonce, sizeof(nonce), "%s", fixed_nonce);
    if (wire_len == 0 || (!fixed_nonce && voice_auth_random_nonce(nonce) != VOICE_AUTH_OK) ||
        voice_auth_sign(
            token, strlen(token), "POST", "/v1/voice/turns", header_user,
            timestamp, nonce, wire, wire_len, signature) != VOICE_AUTH_OK) return -1;
    if (tamper_body) wire[wire_len - 1u] ^= 1u;
    header_len = snprintf(
        header, sizeof(header),
        "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "X-Voice-User: %s\r\nX-Voice-Timestamp: %lld\r\n"
        "X-Voice-Nonce: %s\r\nX-Voice-Signature: %s\r\n"
        "%sContent-Type: application/x-protobuf\r\nContent-Length: %zu\r\n\r\n",
        header_user, (long long)timestamp, nonce, signature, extra_headers, wire_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) return -1;
    fd = gateway_connect(port);
    if (fd < 0) return -1;
    if (request_started_at_ns) {
        *request_started_at_ns = mono_ns();
        if (*request_started_at_ns == 0) {
            close(fd);
            return -1;
        }
    }
    if (write_all(fd, header, (size_t)header_len) != 0 ||
        write_all(fd, wire, wire_len) != 0) {
        close(fd);
        return -1;
    }
    if (gateway_read_response_observe(
            fd, response, response_cap, first_pcm_at_ns) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int gateway_post_turn_ex(int port, const char *token, const char *request_id,
                                const char *text, const char *header_user,
                                const char *body_user, int64_t timestamp,
                                const char *fixed_nonce, int tamper_body,
                                char *response, size_t response_cap) {
    return gateway_post_turn_observe_ex(
        port, token, request_id, text, header_user, body_user, timestamp,
        fixed_nonce, 1, 0, tamper_body, "", response, response_cap, NULL, NULL);
}

static int gateway_post_turn(int port, const char *token, const char *request_id,
                             const char *text, char *response, size_t response_cap) {
    return gateway_post_turn_ex(
        port, token, request_id, text, "u-e2e", "u-e2e", 0, NULL, 0,
        response, response_cap);
}

static int gateway_request_id_replay_survives_churn(int port, const char *token) {
    static const char anchor[] = "req-replay-anchor";
    char response[8192];
    char request_id[64];
    int i;
    if (gateway_post_turn_observe_ex(
            port, token, anchor, "hi", "u-e2e", "u-e2e", 0, NULL,
            0, 0, 0, "", response, sizeof(response), NULL, NULL) != 0 ||
        strstr(response, "HTTP/1.1 200 OK") == NULL ||
        strstr(response, "\"type\":\"completed\"") == NULL) return -1;
    for (i = 0; i < 70; ++i) {
        int written = snprintf(
            request_id, sizeof(request_id), "req-replay-churn-%03d", i);
        if (written <= 0 || (size_t)written >= sizeof(request_id) ||
            gateway_post_turn_observe_ex(
                port, token, request_id, "hi", "u-e2e", "u-e2e", 0, NULL,
                0, 0, 0, "", response, sizeof(response), NULL, NULL) != 0 ||
            strstr(response, "HTTP/1.1 200 OK") == NULL) return -1;
    }
    return gateway_post_turn_observe_ex(
               port, token, anchor, "hi", "u-e2e", "u-e2e", 0, NULL,
               0, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 &&
           strstr(response, "HTTP/1.1 409 Conflict") != NULL &&
           strstr(response, "request_id_reused") != NULL ? 0 : -1;
}

static int gateway_post_cancel_ex(
    int port,
    const char *request_id,
    int alternate_fields,
    char *response,
    size_t response_cap
) {
    uint8_t wire[512];
    char header[1024];
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    size_t wire_len = pb_encode_turn_cancel(
        wire, sizeof(wire), request_id, "u-e2e", "client_cancel");
    int64_t timestamp = (int64_t)time(NULL);
    int header_len;
    int fd;
    if (wire_len == 0 || voice_auth_random_nonce(nonce) != VOICE_AUTH_OK ||
        voice_auth_sign(
            "test-gateway-token-0123456789abcdef-0123456789abcdef",
            sizeof("test-gateway-token-0123456789abcdef-0123456789abcdef") - 1u,
            "POST", "/v1/voice/turns/cancel", "u-e2e", timestamp, nonce,
            wire, wire_len, signature) != VOICE_AUTH_OK) return -1;
    if (alternate_fields) {
        header_len = snprintf(
            header, sizeof(header),
            "POST /v1/voice/turns/cancel HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "x-VoIcE-uSeR: u-e2e\r\nX-vOiCe-TiMeStAmP:\t+%lld \t\r\n"
            "x-VoIcE-nOnCe: %s\r\nX-vOiCe-SiGnAtUrE: %s\r\n"
            "cOnTeNt-TyPe:\tApplication/X-Protobuf \t\r\n"
            "CoNtEnT-lEnGtH:\t000%zu \t\r\n\r\n",
            (long long)timestamp, nonce, signature, wire_len);
    } else {
        header_len = snprintf(
            header, sizeof(header),
            "POST /v1/voice/turns/cancel HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "X-Voice-User: u-e2e\r\nX-Voice-Timestamp: %lld\r\n"
            "X-Voice-Nonce: %s\r\nX-Voice-Signature: %s\r\n"
            "Content-Type: application/x-protobuf\r\n"
            "Content-Length: %zu\r\n\r\n",
            (long long)timestamp, nonce, signature, wire_len);
    }
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) return -1;
    fd = gateway_connect(port);
    if (fd < 0) return -1;
    if (write_all(fd, header, (size_t)header_len) != 0 ||
        write_all(fd, wire, wire_len) != 0 ||
        gateway_read_response(fd, response, response_cap) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int gateway_post_cancel(
    int port,
    const char *request_id,
    char *response,
    size_t response_cap
) {
    return gateway_post_cancel_ex(
        port, request_id, 0, response, response_cap);
}

typedef struct {
    int port;
    const char *request_id;
    const char *text;
    const char *nonce;
    char response[65536];
    uint64_t request_started_at_ns;
    uint64_t first_pcm_at_ns;
    int disable_tts;
    int enable_rag;
    int result;
} gateway_turn_call;

static void *gateway_turn_thread(void *user) {
    gateway_turn_call *call = (gateway_turn_call *)user;
    call->result = gateway_post_turn_observe_ex(
        call->port,
        "test-gateway-token-0123456789abcdef-0123456789abcdef",
        call->request_id,
        call->text,
        "u-e2e",
        "u-e2e",
        0,
        call->nonce,
        !call->disable_tts,
        call->enable_rag,
        0,
        "",
        call->response,
        sizeof(call->response),
        &call->first_pcm_at_ns,
        &call->request_started_at_ns);
    return NULL;
}

typedef struct {
    uint64_t client[SERVICE_E2E_BENCH_MAX];
    uint64_t request_write[SERVICE_E2E_BENCH_MAX];
    uint64_t request_to_llm[SERVICE_E2E_BENCH_MAX];
    uint64_t llm_provider[SERVICE_E2E_BENCH_MAX];
    uint64_t llm_to_tts[SERVICE_E2E_BENCH_MAX];
    uint64_t tts_provider[SERVICE_E2E_BENCH_MAX];
    uint64_t tts_to_pcm[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_auth[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_admission[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_capability[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_encode[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_publish[SERVICE_E2E_BENCH_MAX];
    uint64_t edge_prepare[SERVICE_E2E_BENCH_MAX];
} benchmark_metrics;

typedef struct {
    int gateway_port;
    int worker_index;
    int concurrency;
    int turns;
    fake_tts *tts_server;
    fake_llm *llm_server;
    benchmark_metrics *metrics;
    atomic_int *ready;
    atomic_int *start;
    atomic_int *cohort_ok;
} benchmark_worker;

static void *benchmark_gateway_worker(void *user) {
    static const char token[] =
        "test-gateway-token-0123456789abcdef-0123456789abcdef";
    benchmark_worker *worker = (benchmark_worker *)user;
    struct timespec wait = {.tv_sec = 0, .tv_nsec = 100000L};
    int i;

    atomic_fetch_add_explicit(worker->ready, 1, memory_order_release);
    while (!atomic_load_explicit(worker->start, memory_order_acquire))
        nanosleep(&wait, NULL);

    for (i = worker->worker_index; i < worker->turns; i += worker->concurrency) {
        char request_id[64];
        char prompt[128];
        char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
        char response[65536];
        uint64_t started_at_ns = mono_ns();
        uint64_t request_started_at_ns = 0;
        uint64_t first_pcm_at_ns = 0;
        uint64_t llm_request_at_ns;
        uint64_t llm_response_at_ns;
        uint64_t tts_request_at_ns;
        uint64_t tts_response_at_ns;
        uint64_t edge_auth_us;
        uint64_t edge_admission_us;
        uint64_t edge_capability_us;
        uint64_t edge_encode_us;
        uint64_t edge_publish_us;
        uint64_t edge_prepare_us;
        int result;
        int request_id_len = snprintf(
            request_id, sizeof(request_id), "req-bench-%03d", i);
        int prompt_len = snprintf(
            prompt,
            sizeof(prompt),
            "Benchmark sample %03d: Look up how invisibility changes attack rolls.",
            i);
        int nonce_len = snprintf(
            nonce,
            sizeof(nonce),
            "%016llx%016llx",
            (unsigned long long)(UINT64_C(0x600d000000000000) + (uint64_t)i),
            (unsigned long long)(UINT64_C(0xcafe000000000000) + (uint64_t)i));
        if (request_id_len <= 0 ||
            (size_t)request_id_len >= sizeof(request_id) ||
            prompt_len <= 0 || (size_t)prompt_len >= sizeof(prompt) ||
            nonce_len != VOICE_AUTH_NONCE_HEX_LEN || started_at_ns == 0) {
            atomic_store_explicit(worker->cohort_ok, 0, memory_order_relaxed);
            continue;
        }
        result = gateway_post_turn_observe_ex(
            worker->gateway_port,
            token,
            request_id,
            prompt,
            "u-e2e",
            "u-e2e",
            0,
            nonce,
            1,
            0,
            0,
            "",
            response,
            sizeof(response),
            &first_pcm_at_ns,
            &request_started_at_ns);
        llm_request_at_ns = (uint64_t)atomic_load_explicit(
            &worker->llm_server->benchmark_request_at_ns[i],
            memory_order_relaxed);
        llm_response_at_ns = (uint64_t)atomic_load_explicit(
            &worker->llm_server->benchmark_response_at_ns[i],
            memory_order_relaxed);
        tts_request_at_ns = (uint64_t)atomic_load_explicit(
            &worker->tts_server->benchmark_request_at_ns[i],
            memory_order_relaxed);
        tts_response_at_ns = (uint64_t)atomic_load_explicit(
            &worker->tts_server->benchmark_response_at_ns[i],
            memory_order_relaxed);
        if (result != 0 || first_pcm_at_ns < request_started_at_ns ||
            request_started_at_ns < started_at_ns ||
            llm_request_at_ns < request_started_at_ns ||
            llm_response_at_ns < llm_request_at_ns ||
            tts_request_at_ns < llm_response_at_ns ||
            tts_response_at_ns < tts_request_at_ns ||
            first_pcm_at_ns < tts_response_at_ns ||
            parse_uint64_field(
                response, "\"edge_auth_us\":", &edge_auth_us) != 0 ||
            parse_uint64_field(
                response, "\"edge_admission_us\":", &edge_admission_us) != 0 ||
            parse_uint64_field(
                response, "\"edge_capability_us\":", &edge_capability_us) != 0 ||
            parse_uint64_field(
                response, "\"edge_encode_us\":", &edge_encode_us) != 0 ||
            parse_uint64_field(
                response, "\"edge_vbus_publish_us\":", &edge_publish_us) != 0 ||
            parse_uint64_field(
                response, "\"edge_prepare_us\":", &edge_prepare_us) != 0 ||
            edge_auth_us > edge_prepare_us ||
            edge_publish_us > edge_prepare_us ||
            strstr(response, "\"type\":\"text_delta\"") == NULL ||
            count_occurrences(response, "\"type\":\"pcm_chunk\"") !=
                (size_t)worker->tts_server->benchmark_pcm_frames * 3u ||
            count_occurrences(response, "\"sample_rate\":24000") !=
                (size_t)worker->tts_server->benchmark_pcm_frames * 3u ||
            strstr(response, "\"type\":\"completed\"") == NULL) {
            atomic_store_explicit(worker->cohort_ok, 0, memory_order_relaxed);
            continue;
        }
        worker->metrics->client[i] =
            (first_pcm_at_ns - started_at_ns) / UINT64_C(1000);
        worker->metrics->request_write[i] =
            (first_pcm_at_ns - request_started_at_ns) / UINT64_C(1000);
        worker->metrics->request_to_llm[i] =
            (llm_request_at_ns - request_started_at_ns) / UINT64_C(1000);
        worker->metrics->llm_provider[i] =
            (llm_response_at_ns - llm_request_at_ns) / UINT64_C(1000);
        worker->metrics->llm_to_tts[i] =
            (tts_request_at_ns - llm_response_at_ns) / UINT64_C(1000);
        worker->metrics->tts_provider[i] =
            (tts_response_at_ns - tts_request_at_ns) / UINT64_C(1000);
        worker->metrics->tts_to_pcm[i] =
            (first_pcm_at_ns - tts_response_at_ns) / UINT64_C(1000);
        worker->metrics->edge_auth[i] = edge_auth_us;
        worker->metrics->edge_admission[i] = edge_admission_us;
        worker->metrics->edge_capability[i] = edge_capability_us;
        worker->metrics->edge_encode[i] = edge_encode_us;
        worker->metrics->edge_publish[i] = edge_publish_us;
        worker->metrics->edge_prepare[i] = edge_prepare_us;
    }
    return NULL;
}

static void update_atomic_max(atomic_int *maximum, int candidate) {
    int current = atomic_load_explicit(maximum, memory_order_relaxed);
    while (current < candidate &&
           !atomic_compare_exchange_weak_explicit(
               maximum, &current, candidate,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void *fake_tts_connection_thread(void *arg) {
    fake_tts_connection *connection = (fake_tts_connection *)arg;
    fake_tts *server = connection->server;
    int client = connection->client;
    int requests_on_connection = 0;

    for (;;) {
        char request[4096];
        size_t used = 0;
        size_t body_len = 0;
        uint8_t pcm[
            8 + SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES *
                SERVICE_E2E_BENCH_TTS_FRAMES_MAX];
        size_t response_pcm_len;
        size_t response_frame_bytes;
        size_t response_len;
        uint32_t response_sample_rate;
        char header[128];
        int request_complete = 0;
        int llm_request = 0;
        int pipeline_cancel_request = 0;
        int late_error_request = 0;
        int cancel_request = 0;
        int header_only_request = 0;
        int truncated_request = 0;
        int partial_request = 0;
        int benchmark_index = -1;
        int request_no;
        int response_failed = 0;
        int hn;
        size_t header_prefix;

        while (used + 1 < sizeof(request)) {
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (n <= 0) break;
            used += (size_t)n;
            request[used] = '\0';
            {
                char *headers_end = strstr(request, "\r\n\r\n");
                if (headers_end) {
                    char *length_header = strstr(request, "Content-Length: ");
                    size_t headers_len = (size_t)(headers_end + 4 - request);
                    if (!length_header) break;
                    body_len = (size_t)strtoul(
                        length_header + sizeof("Content-Length: ") - 1, NULL, 10);
                    if (body_len > sizeof(request) - headers_len - 1) break;
                    if (used >= headers_len + body_len) {
                        const char *body = headers_end + 4;
                        request_complete = 1;
                        if (strstr(request, "Connection: keep-alive\r\n"))
                            atomic_fetch_add_explicit(
                                &server->keepalive_requests, 1,
                                memory_order_relaxed);
                        if (strncmp(
                                request, "POST /pcm HTTP/1.1\r\n",
                                sizeof("POST /pcm HTTP/1.1\r\n") - 1) == 0 &&
                            strstr(request, "Content-Type: application/json\r\n") &&
                            strstr(request, "Connection: keep-alive\r\n") &&
                            strstr(body, "\"text\":\"") &&
                            strstr(body, "\"turn_id\":") &&
                            strstr(body, "\"query_hash\":") &&
                            strstr(body, "\"frames_per_chunk\":1"))
                            atomic_fetch_add_explicit(
                                &server->valid_requests, 1, memory_order_relaxed);
                        llm_request =
                            strstr(body, "\"turn_id\":\"req-llm\"") != NULL;
                        if (llm_request && (strchr(body, '*') || strchr(body, '`')))
                            atomic_fetch_add_explicit(&server->model_markup_requests, 1, memory_order_relaxed);
                        pipeline_cancel_request = strstr(
                            body,
                            "\"turn_id\":\"req-tts-pipeline-cancel\"") != NULL;
                        late_error_request = strstr(
                            body,
                            "\"turn_id\":\"req-llm-late-error\"") != NULL || strstr(
                            body, "\"turn_id\":\"req-llm-late-limit\"") != NULL || strstr(
                            body, "\"turn_id\":\"req-dice-speech-cancel\"") != NULL;
                        cancel_request = strstr(
                            body, "\"turn_id\":\"req-cancel\"") != NULL;
                        header_only_request = strstr(
                            body,
                            "\"turn_id\":\"req-tts-header-only\"") != NULL;
                        truncated_request = strstr(
                            body,
                            "\"turn_id\":\"req-tts-truncated\"") != NULL;
                        partial_request = strstr(
                            body,
                            "\"turn_id\":\"req-tts-partial\"") != NULL;
                        if (strstr(body, "\\\"rune\\\"") != NULL)
                            atomic_fetch_add_explicit(
                                &server->escaped_quote_requests,
                                1,
                                memory_order_relaxed);
                        if (strstr(body, "silver\\\\road") != NULL)
                            atomic_fetch_add_explicit(
                                &server->escaped_backslash_requests,
                                1,
                                memory_order_relaxed);
                        benchmark_index = benchmark_turn_index(body);
                        if (llm_request) {
                            record_first_value(
                                &server->first_llm_accept_at_ns,
                                connection->accepted_at_ns);
                            record_first_timestamp(&server->first_llm_request_at_ns);
                        }
                        break;
                    }
                }
            }
        }
        if (!request_complete) break;
        if (benchmark_index >= 0)
            record_min_value(
                &server->benchmark_request_at_ns[benchmark_index], mono_ns());
        request_no = atomic_fetch_add_explicit(
            &server->requests, 1, memory_order_relaxed);
        if (requests_on_connection != 0)
            atomic_fetch_add_explicit(
                &server->reused_requests, 1, memory_order_relaxed);
        if (llm_request) {
            atomic_fetch_add_explicit(
                &server->llm_provider_requests, 1, memory_order_relaxed);
            int active = atomic_fetch_add_explicit(
                &server->llm_active_requests, 1, memory_order_relaxed) + 1;
            update_atomic_max(&server->llm_max_active_requests, active);
        }
        if (pipeline_cancel_request) {
            int active = atomic_fetch_add_explicit(
                &server->pipeline_cancel_active_requests, 1,
                memory_order_relaxed) + 1;
            update_atomic_max(&server->pipeline_cancel_max_active_requests, active);
        }
        if (late_error_request)
            atomic_fetch_add_explicit(
                &server->late_error_requests, 1, memory_order_relaxed);
        response_frame_bytes = benchmark_index >= 0 || partial_request ?
            SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES :
            SERVICE_E2E_TEST_TTS_FRAME_BYTES;
        response_sample_rate = benchmark_index >= 0 || partial_request ?
            SERVICE_E2E_PRODUCTION_TTS_SAMPLE_RATE :
            SERVICE_E2E_TEST_TTS_SAMPLE_RATE;
        response_pcm_len = header_only_request ? 0u :
            (partial_request ?
             SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES * 2u +
                 SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES / 2u :
            (benchmark_index >= 0 ?
             response_frame_bytes * (size_t)server->benchmark_pcm_frames :
            (request_no == 0 || llm_request || pipeline_cancel_request ||
                 late_error_request || truncated_request || strstr(request, "req-wakeup-") ?
             SERVICE_E2E_TEST_TTS_FRAME_BYTES * 3u :
             SERVICE_E2E_TEST_TTS_FRAME_BYTES)));
        response_len = 8u + response_pcm_len;
        memset(pcm, 0, sizeof(pcm));
        pcm[0] = (uint8_t)(response_sample_rate & 0xffu);
        pcm[1] = (uint8_t)((response_sample_rate >> 8u) & 0xffu);
        pcm[2] = (uint8_t)((response_sample_rate >> 16u) & 0xffu);
        pcm[3] = (uint8_t)((response_sample_rate >> 24u) & 0xffu);
        pcm[4] = 1;
        pcm[6] = 16;
        {
            size_t pcm_index;
            for (pcm_index = 0u; pcm_index < response_pcm_len; ++pcm_index)
                pcm[8u + pcm_index] = (uint8_t)(pcm_index * 131u + 17u);
        }
        hn = snprintf(
            header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
            "Connection: keep-alive\r\n\r\n",
            response_len);
        header_prefix = partial_request ? 3u : 8u;
        if (hn <= 0 || write_all(client, header, (size_t)hn) != 0 ||
            write_all(client, pcm, header_prefix) != 0) {
            response_failed = 1;
        } else {
            if (cancel_request)
                atomic_store_explicit(
                    &server->cancel_header_sent, 1, memory_order_relaxed);
            if (partial_request)
                atomic_store_explicit(
                    &server->partial_header_sent, 1, memory_order_relaxed);
            if (llm_request)
                record_first_timestamp(&server->first_llm_response_at_ns);
            if (benchmark_index >= 0)
                record_min_value(
                    &server->benchmark_response_at_ns[benchmark_index], mono_ns());
            if (truncated_request) {
                (void)write_all(
                    client,
                    pcm + 8,
                    SERVICE_E2E_TEST_TTS_FRAME_BYTES * 2u);
                response_failed = 1;
            } else if (header_only_request) {
                /* The format header is the complete response body. */
            } else if (partial_request) {
                struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000L};
                nanosleep(&delay, NULL);
                if (write_all(
                        client,
                        pcm + header_prefix,
                        response_len - header_prefix) != 0)
                    response_failed = 1;
            } else if (request_no == 0 || llm_request || pipeline_cancel_request ||
                late_error_request) {
                if (write_all(
                        client,
                        pcm + 8,
                        SERVICE_E2E_TEST_TTS_FRAME_BYTES * 2u) != 0)
                    response_failed = 1;
                if (late_error_request && !response_failed)
                    atomic_store_explicit(
                        &server->late_error_pcm_sent, 1, memory_order_relaxed);
                if (late_error_request && !response_failed) {
                    struct pollfd pfd = {.fd = client, .events = POLLIN};
                    int poll_rc;
                    do {
                        poll_rc = poll(&pfd, 1, 600);
                    } while (poll_rc < 0 && errno == EINTR);
                    if (poll_rc > 0) {
                        uint8_t byte;
                        ssize_t peek = recv(
                            client, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
                        if (peek == 0 ||
                            (peek < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            atomic_fetch_add_explicit(
                                &server->late_error_disconnects,
                                1,
                                memory_order_relaxed);
                            response_failed = 1;
                        }
                    }
                } else {
                    struct timespec delay = {
                        .tv_sec = pipeline_cancel_request ? 2 : 0,
                        .tv_nsec = pipeline_cancel_request ? 0 :
                            (llm_request ? 800000000L : 500000000L),
                    };
                    nanosleep(&delay, NULL);
                }
                if (!response_failed && write_all(
                        client,
                        pcm + 8 + SERVICE_E2E_TEST_TTS_FRAME_BYTES * 2u,
                        SERVICE_E2E_TEST_TTS_FRAME_BYTES) != 0)
                    response_failed = 1;
                if (request_no == 0)
                    atomic_store_explicit(
                        &server->first_tail_sent, 1, memory_order_relaxed);
            } else if (request_no == 1) {
                struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000L};
                nanosleep(&delay, NULL);
                if (write_all(client, pcm + 8, response_pcm_len) != 0)
                    response_failed = 1;
            } else if (write_all(client, pcm + 8, response_pcm_len) != 0) {
                response_failed = 1;
            }
        }
        if (llm_request)
            atomic_fetch_sub_explicit(
                &server->llm_active_requests, 1, memory_order_relaxed);
        if (pipeline_cancel_request)
            atomic_fetch_sub_explicit(
                &server->pipeline_cancel_active_requests, 1,
                memory_order_relaxed);
        if (!response_failed)
            atomic_fetch_add_explicit(&server->responses_sent, 1, memory_order_relaxed);
        requests_on_connection++;
        if (response_failed) break;
    }
    close(client);
    return NULL;
}

static void *fake_tts_thread(void *arg) {
    fake_tts *server = (fake_tts *)arg;
    pthread_t handlers[11];
    fake_tts_connection connections[11];
    size_t started = 0;
    int idle = 0;
    while (started < sizeof(handlers) / sizeof(handlers[0]) && idle < 200 &&
           !atomic_load_explicit(&server->stop, memory_order_relaxed)) {
        int client = accept_bounded(server->listen_fd, 100);
        if (client < 0) {
            if (atomic_load_explicit(
                    &server->requests, memory_order_relaxed) >=
                    server->expected_requests)
                break;
            idle++;
            continue;
        }
        idle = 0;
        {
            int enabled = 1;
            if (setsockopt(
                    client, IPPROTO_TCP, TCP_NODELAY,
                    &enabled, sizeof(enabled)) != 0) {
                close(client);
                break;
            }
        }
        atomic_fetch_add_explicit(&server->accepts, 1, memory_order_relaxed);
        connections[started].server = server;
        connections[started].client = client;
        connections[started].accepted_at_ns = mono_ns();
        if (pthread_create(
                &handlers[started], NULL,
                fake_tts_connection_thread, &connections[started]) != 0) {
            close(client);
            break;
        }
        started++;
    }
    {
        size_t index;
        for (index = 0; index < started; ++index)
            pthread_join(handlers[index], NULL);
    }
    return NULL;
}

static void *fake_llm_connection_thread(void *arg) {
    fake_llm_connection *connection = (fake_llm_connection *)arg;
    fake_llm *server = connection->server;
    int client = connection->client;
    int requests_on_connection = 0;
    static const char first_head[] =
        "data: {\"id\":\"chatcmpl-e2e\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"fixture-provider-model\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"logprobs\":null,"
        "\"finish_reason\":null}],\"prompt_token_ids\":null,"
        "\"prompt_text\":null}\n\n"
        "data: {\"id\":\"chatcmpl-e2e\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"fixture-provider-model\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"**The an\"},"
        "\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"cient*\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"*\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" \"}}]}\n\n";
    static const char first_tail[] =
        "data: {\"id\":\"chatcmpl-e2e\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"fixture-provider-model\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":"
        "\"gate opens when moonlight touches the **hidden** `\\\"rune\\\"` and reveals the path ahead to any adventurer brave enough "
        "to follow the silver\\\\road home.\"},\"logprobs\":null,"
        "\"finish_reason\":null,\"token_ids\":null}]}\n\n"
        "data: {\"id\":\"chatcmpl-e2e\",\"object\":\"chat.completion.chunk\","
        "\"created\":1750000000,\"model\":\"fixture-provider-model\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"\"},\"logprobs\":null,"
        "\"finish_reason\":\"stop\",\"stop_reason\":null,"
        "\"token_ids\":null}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":39,\"completion_tokens\":7,\"total_tokens\":46}}\n\n"
        "data: [DONE]\n\n";
    static const char cancel_body[] =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":null}}]}\n\n";
    static const char late_error_head[] =
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"The ancient gate opens when moonlight touches the unfinished **unsent_pending_markup\"}}]}\n\n";
    static const char late_error_tail[] =
        "data: {\"error\":{\"message\":\"late provider failure\"}}\n\n";
    static const char length_body[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"**The rule applies when\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
        "data: [DONE]\n\n";
    static const char length_tail[] =
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
        "data: [DONE]\n\n";

    for (;;) {
        char request[8192];
        size_t used = 0;
        size_t body_len = 0;
        size_t response_len;
        char header[192];
        int header_len;
        int request_complete = 0;
        int llm_request = 0;
        int rag_success_request = 0;
        int late_error_request = 0;
        int length_request = 0;
        int benchmark_request = 0;
        int benchmark_index = -1;
        const char *request_body = NULL;
        while (used + 1 < sizeof(request)) {
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (n <= 0) break;
            used += (size_t)n;
            request[used] = '\0';
            {
                char *headers_end = strstr(request, "\r\n\r\n");
                if (headers_end) {
                    char *length_header = strstr(request, "Content-Length: ");
                    size_t headers_len = (size_t)(headers_end + 4 - request);
                    if (!length_header) break;
                    body_len = (size_t)strtoul(
                        length_header + sizeof("Content-Length: ") - 1, NULL, 10);
                    if (body_len > sizeof(request) - headers_len - 1) break;
                    if (used >= headers_len + body_len) {
                        cmp_json_object request_object;
                        int64_t completion_tokens = 0;
                        request_body = headers_end + 4;
                        request_complete = 1;
                        rag_success_request = strstr(
                            request_body,
                            "ancient dragon lore") != NULL;
                        llm_request = !rag_success_request && strstr(
                            request_body,
                            "invisibility changes attack rolls") != NULL;
                        benchmark_request = llm_request &&
                            strstr(request_body, "Benchmark sample") != NULL;
                        late_error_request = strstr(
                            request_body,
                            "late provider error after speakable text") != NULL;
                        int loop_index = loop_request_index(request_body);
                        if (loop_index >= 0) llm_request = loop_index != 9;
                        length_request = loop_index >= 0 ? loop_index == 6 :
                            strstr(request_body, "Exhaust the token budget") != NULL;
                        if (length_request && rag_success_request)
                            atomic_store_explicit(&server->policy_connection_at_ns,
                                connection->accepted_at_ns, memory_order_relaxed);
                        else if ((strstr(request_body, "Keep the server ceiling") ||
                                  strstr(request_body, "after recovery")) &&
                                 atomic_load_explicit(&server->policy_connection_at_ns,
                                     memory_order_relaxed) != connection->accepted_at_ns)
                            atomic_fetch_add_explicit(&server->policy_connection_failures, 1, memory_order_relaxed);
                        if (strncmp(
                                request,
                                "POST /v1/chat/completions HTTP/1.1\r\n",
                            sizeof("POST /v1/chat/completions HTTP/1.1\r\n") - 1) == 0 &&
                            strstr(request, "Accept: text/event-stream\r\n") &&
                            strstr(request, "Content-Type: application/json\r\n") &&
                            strstr(request, "Connection: keep-alive\r\n") &&
                            strstr(request_body, "\"model\":\"default\"") &&
                            cmp_json_object_parse(request_body, &request_object) &&
                            cmp_json_object_i64(&request_object, "max_completion_tokens", &completion_tokens) &&
                            completion_tokens == (length_request ? 8 :
                                strstr(request_body, "Retrieved excerpts (source data):") ? 256 : 48) &&
                            canonical_model_request(request_body, -1) &&
                            !strstr(request_body, "\"max_tokens\":") &&
                            strstr(request_body, "\"stream\":true") &&
                            strstr(request_body, "\"stream_options\":{\"include_usage\":true}") &&
                            strstr(request_body, "\"include_reasoning\":false") &&
                            strstr(request_body, "\"enable_thinking\":false"))
                            atomic_fetch_add_explicit(
                                &server->valid_requests, 1, memory_order_relaxed);
                        if (strstr(request, "Connection: keep-alive\r\n"))
                            atomic_fetch_add_explicit(
                                &server->keepalive_requests, 1,
                                memory_order_relaxed);
                        if (rag_success_request && canonical_model_request(request_body, 1))
                            atomic_fetch_add_explicit(
                                &server->grounded_requests, 1, memory_order_relaxed);
                        if (llm_request) {
                            record_first_value(
                                &server->first_accept_at_ns,
                                connection->accepted_at_ns);
                            record_first_timestamp(&server->first_request_at_ns);
                        }
                        break;
                    }
                }
            }
        }
        if (!request_complete) break;
        if (benchmark_request) {
            uint64_t request_at_ns = mono_ns();
            benchmark_index = benchmark_prompt_index(request_body);
            atomic_fetch_add_explicit(
                &server->benchmark_requests, 1, memory_order_relaxed);
            if (benchmark_index < 0 || request_at_ns == 0 ||
                atomic_exchange_explicit(
                    &server->benchmark_request_at_ns[benchmark_index],
                    request_at_ns,
                    memory_order_relaxed) != 0)
                atomic_fetch_add_explicit(
                    &server->benchmark_binding_failures, 1,
                    memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&server->requests, 1, memory_order_relaxed);
        if (requests_on_connection != 0)
            atomic_fetch_add_explicit(
                &server->reused_requests, 1, memory_order_relaxed);
        response_len = late_error_request ? sizeof(late_error_head) - 1u +
            (length_request ? sizeof(length_tail) - 1u : sizeof(late_error_tail) - 1u) :
            length_request ? sizeof(length_body) - 1u :
            llm_request || rag_success_request ?
            sizeof(first_head) - 1 + sizeof(first_tail) - 1 :
            sizeof(cancel_body) - 1 + 1024u;
        header_len = snprintf(
            header,
            sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
            response_len);
        if (header_len > 0) {
            int response_failed =
                write_all(client, header, (size_t)header_len) != 0;
            if (length_request && !late_error_request) {
                if (write_all(client, length_body, sizeof(length_body) - 1u) != 0) response_failed = 1;
            } else if (late_error_request) {
                int wait_count;
                if (write_all(
                        client, late_error_head, sizeof(late_error_head) - 1u) != 0)
                    response_failed = 1;
                for (wait_count = 0; !response_failed && wait_count < 100 &&
                     server->late_error_tts_pcm_sent &&
                     atomic_load_explicit(
                         server->late_error_tts_pcm_sent,
                         memory_order_relaxed) == 0; ++wait_count) {
                    struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000L};
                    nanosleep(&delay, NULL);
                }
                if (!response_failed && write_all(
                        client, length_request ? length_tail : late_error_tail,
                        length_request ? sizeof(length_tail) - 1u : sizeof(late_error_tail) - 1u) != 0)
                    response_failed = 1;
            } else if (llm_request || rag_success_request) {
                if (llm_request)
                    record_first_timestamp(&server->first_response_at_ns);
                if (benchmark_index >= 0 &&
                    benchmark_index < SERVICE_E2E_BENCH_MAX)
                    atomic_store_explicit(
                        &server->benchmark_response_at_ns[benchmark_index],
                        mono_ns(),
                        memory_order_relaxed);
                if (write_all(client, first_head, sizeof(first_head) - 1) != 0)
                    response_failed = 1;
                if (llm_request && !benchmark_request) {
                    struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000L};
                    nanosleep(&delay, NULL);
                }
                if (write_all(client, first_tail, sizeof(first_tail) - 1) != 0)
                    response_failed = 1;
                if (llm_request)
                    atomic_store_explicit(
                        &server->first_tail_sent, 1, memory_order_relaxed);
            } else {
                if (write_all(client, cancel_body, sizeof(cancel_body) - 1) != 0)
                    response_failed = 1;
                {
                    struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
                    nanosleep(&delay, NULL);
                }
                if (write_all(client, "x", 1) != 0)
                    response_failed = 1;
            }
            requests_on_connection++;
            if (response_failed) break;
        }
    }
    close(client);
    return NULL;
}

static void *fake_llm_thread(void *arg) {
    fake_llm *server = (fake_llm *)arg;
    pthread_t handlers[32];
    fake_llm_connection connections[32];
    size_t started = 0;
    int idle = 0;

    while (started < sizeof(handlers) / sizeof(handlers[0]) && idle < 2000) {
        int client = accept_bounded(server->listen_fd, 100);
        if (client < 0) {
            if (atomic_load_explicit(
                    &server->requests, memory_order_relaxed) >=
                    server->expected_requests)
                break;
            idle++;
            continue;
        }
        idle = 0;
        {
            int enabled = 1;
            if (setsockopt(
                    client, IPPROTO_TCP, TCP_NODELAY,
                    &enabled, sizeof(enabled)) != 0) {
                close(client);
                break;
            }
        }
        atomic_fetch_add_explicit(&server->accepts, 1, memory_order_relaxed);
        connections[started].server = server;
        connections[started].client = client;
        connections[started].accepted_at_ns = mono_ns();
        if (pthread_create(
                &handlers[started], NULL,
                fake_llm_connection_thread, &connections[started]) != 0) {
            close(client);
            break;
        }
        started++;
    }
    {
        size_t index;
        for (index = 0; index < started; ++index)
            pthread_join(handlers[index], NULL);
    }
    return NULL;
}

static void *fake_stt_thread(void *arg) {
    fake_stt *server = (fake_stt *)arg;
    static const char response_body[] =
        "{\"diagnostic\":{\"status\":\"ok\",\"text\":\"metadata poison\"},"
        "\"status\":\"ok\",\"text\":\"hello\",\"transcript\":\"hello\"}";
    static const char malformed_body[] =
        "{\"status\":\"ok\",\"text\":42}";
    int client = -1;
    int request_no;
    for (request_no = 0; request_no < 7; ++request_no) {
        char request[8192];
        size_t used = 0;
        size_t expected = 0;
        int request_complete = 0;
        int request_keepalive = 0;
        if (client < 0) {
            client = accept_bounded(server->listen_fd, 5000);
            if (client < 0) break;
            atomic_fetch_add_explicit(&server->accepts, 1, memory_order_relaxed);
        }
        while (used + 1 < sizeof(request)) {
            char *headers_end;
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (n <= 0) break;
            used += (size_t)n;
            request[used] = '\0';
            headers_end = strstr(request, "\r\n\r\n");
            if (headers_end) {
                char *length_header = strstr(request, "Content-Length: ");
                size_t headers_len = (size_t)(headers_end + 4 - request);
                if (!length_header) break;
                expected = (size_t)strtoul(
                    length_header + sizeof("Content-Length: ") - 1, NULL, 10);
                if (expected > sizeof(request) - headers_len - 1) break;
                if (used >= headers_len + expected) {
                    const uint8_t *body = (const uint8_t *)(headers_end + 4);
                    size_t i;
                    int any_signal = 0;
                    for (i = 0; i < expected; ++i) {
                        if (body[i] != 0) {
                            any_signal = 1;
                            break;
                        }
                    }
                    request_keepalive =
                        strstr(request, "Connection: keep-alive\r\n") != NULL;
                    if (strncmp(
                            request,
                            "POST /v1/internal/transcribe_pcm_s16le HTTP/1.1\r\n",
                            sizeof("POST /v1/internal/transcribe_pcm_s16le HTTP/1.1\r\n") - 1) == 0 &&
                        strstr(request, "Content-Type: application/octet-stream\r\n") &&
                        strstr(request, "X-Sample-Rate: 16000\r\n") &&
                        strstr(request, "X-Language: en\r\n") &&
                        strstr(request, "X-PCM-Format: pcm_s16le\r\n") &&
                        expected > 0 && (expected & 1u) == 0u && any_signal)
                        atomic_fetch_add_explicit(
                            &server->valid_requests, 1, memory_order_relaxed);
                    if (request_keepalive)
                        atomic_fetch_add_explicit(
                            &server->keepalive_requests, 1, memory_order_relaxed);
                    if (request_no == 0)
                        atomic_store_explicit(
                            &server->first_request_bytes,
                            expected,
                            memory_order_relaxed);
                    request_complete = 1;
                    break;
                }
            }
        }
        if (!request_complete) {
            close(client);
            client = -1;
            break;
        }
        atomic_fetch_add_explicit(&server->requests, 1, memory_order_relaxed);
        if (request_no == 5) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000L};
            nanosleep(&delay, NULL);
        }
        {
            char header[160];
            const char *body = request_no == 3 ? malformed_body : response_body;
            size_t body_len = request_no == 3 ?
                sizeof(malformed_body) - 1u : sizeof(response_body) - 1u;
            int keep_open =
                (request_no == 0 || request_no == 3) && request_keepalive;
            int hn = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                body_len, keep_open ? "keep-alive" : "close");
            if (hn > 0) {
                (void)write_all(client, header, (size_t)hn);
                (void)write_all(client, body, body_len);
            }
            if (!keep_open) {
                close(client);
                client = -1;
            }
        }
    }
    if (client >= 0) close(client);
    return NULL;
}

static int replace_fixture_text(char *text, size_t capacity, const char *from, const char *to) {
    char *at;
    size_t old_length = strlen(from), new_length = strlen(to);
    if (!old_length || strstr(to, from)) return -1;
    while ((at = strstr(text, from)) != NULL) {
        size_t before = (size_t)(at - text), after = strlen(at + old_length);
        if (before + new_length + after >= capacity) return -1;
        memmove(at + new_length, at + old_length, after + 1u);
        memcpy(at, to, new_length);
    }
    return 0;
}

static void *fake_embed_thread(void *arg) {
    fake_embed *server = (fake_embed *)arg;
    static const char embedding_body[] = "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":["
        "{\"object\":\"embedding\",\"index\":0,\"embedding\":[0.5,-0.25,0.75]}]}";
    int client = -1;
    int request_no;
    for (request_no = 0; request_no < 13; ++request_no) {
        char request[16384];
        char response_body[16384];
        size_t response_length;
        size_t used = 0;
        size_t body_len = 0;
        int request_complete = 0;
        int request_keepalive = 0;
        int slow_request = 0;
        int cascade_request = 0;
        if (client < 0) {
            client = accept_bounded(server->listen_fd, 60000);
            if (client < 0) break;
            atomic_fetch_add_explicit(&server->accepts, 1, memory_order_relaxed);
        }
        while (used + 1 < sizeof(request)) {
            char *headers_end;
            ssize_t n = recv(client, request + used, sizeof(request) - used - 1, 0);
            if (n <= 0) break;
            used += (size_t)n;
            request[used] = '\0';
            headers_end = strstr(request, "\r\n\r\n");
            if (headers_end) {
                char *length_header = strstr(request, "Content-Length: ");
                size_t headers_len = (size_t)(headers_end + 4 - request);
                if (!length_header) break;
                body_len = (size_t)strtoul(
                    length_header + sizeof("Content-Length: ") - 1, NULL, 10);
                if (body_len > sizeof(request) - headers_len - 1) break;
                if (used >= headers_len + body_len) {
                    const char *body = headers_end + 4;
                    slow_request = !server->search && request_no == 4 &&
                        strstr(body, "Look up how invisibility") != NULL;
                    cascade_request = strstr(body, "Look up") != NULL;
                    request_keepalive =
                        strstr(request, "Connection: keep-alive\r\n") != NULL;
                    {
                        cmp_json_object object;
                        char filter[4096], collection[128], query[2048];
                        cmp_json_array inputs;
                        cmp_json_field input;
                        int valid = cmp_json_object_parse(body, &object) &&
                            strstr(request, "Content-Type: application/json\r\n");
                        if (server->search) {
                            valid = valid && strncmp(request, "POST /v2/vectordb/entities/search HTTP/1.1\r\n",
                                sizeof("POST /v2/vectordb/entities/search HTTP/1.1\r\n") - 1u) == 0 &&
                                strstr(request, "Authorization: Bearer test-milvus-token\r\n") &&
                                cmp_json_object_str(&object, "collectionName", collection, sizeof(collection)) &&
                                cmp_json_object_str(&object, "filter", filter, sizeof(filter)) &&
                                strstr(body, "[[0.5,-0.25,0.75]]");
                            if (request_no < 9 || request_no >= 11) {
                                valid = valid && strcmp(collection, "reviewed_books") == 0 &&
                                    strstr(filter, "visibility == \"public\"") &&
                                    strstr(filter, "owner_user_id == \"\"") &&
                                    strstr(filter, "\"players-handbook\"") &&
                                    ((strstr(filter, "\"tashas-cauldron-of-everything\"") != NULL) == (request_no < 5 || request_no >= 11));
                            } else {
                                valid = valid && strcmp(collection, "dnd_campaign_canon_v1") == 0 &&
                                    strstr(filter, "visibility == \"private\"") &&
                                    strstr(filter, "owner_user_id == \"u-e2e\"") &&
                                    strstr(filter, "campaign_id == \"campaign-e2e\"");
                            }
                        } else {
                            valid = valid && strncmp(request, "POST /embeddings HTTP/1.1\r\n",
                                sizeof("POST /embeddings HTTP/1.1\r\n") - 1u) == 0 &&
                                cmp_json_object_str(&object, "model", collection, sizeof(collection)) &&
                                strcmp(collection, "bge-m3") == 0 &&
                                cmp_json_field_array(cmp_json_object_field(&object, "input"), &inputs) &&
                                cmp_json_array_next(&inputs, &input) == 1 &&
                                cmp_json_field_str(&input, query, sizeof(query)) && query[0] &&
                                cmp_json_array_next(&inputs, &input) == 0 &&
                                (strcmp(query, "rules query") == 0 || cascade_request || strstr(query, "invisibility"));
                        }
                        if (valid) atomic_fetch_add_explicit(&server->valid_requests, 1, memory_order_relaxed);
                    }
                    if (request_keepalive)
                        atomic_fetch_add_explicit(
                            &server->keepalive_requests, 1, memory_order_relaxed);
                    request_complete = 1;
                    break;
                }
            }
        }
        if (!request_complete) {
            close(client);
            client = -1;
            break;
        }
        atomic_fetch_add_explicit(&server->requests, 1, memory_order_relaxed);
        strcpy(response_body, server->search ? server->reply : embedding_body);
        if (server->search) {
            if (request_no == 6 && replace_fixture_text(response_body, sizeof(response_body),
                    "players-handbook", "tashas-cauldron-of-everything") != 0) break;
            if (request_no == 7 && replace_fixture_text(response_body, sizeof(response_body),
                    "source_sha256", "untrusted_hash") != 0) break;
            if (request_no == 8 && replace_fixture_text(response_body, sizeof(response_body),
                    "Sneak Attack can apply once per turn.", "Sneak Attack applies once per round.") != 0) break;
            if (request_no >= 9 && request_no < 11) {
                if (replace_fixture_text(response_body, sizeof(response_body), "shared_rulebook", "campaign_canon") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "official_book", "notes") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "public", "private") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "book://players-handbook", "s3://test/books/phb.pdf") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "\"owner_user_id\":\"\"",
                        request_no == 9 ? "\"owner_user_id\":\"other-user\"" : "\"owner_user_id\":\"u-e2e\"") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "\\\"owner_user_id\\\":\\\"\\\"",
                        request_no == 9 ? "\\\"owner_user_id\\\":\\\"other-user\\\"" : "\\\"owner_user_id\\\":\\\"u-e2e\\\"") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "\"campaign_id\":\"\"",
                        request_no == 9 ? "\"campaign_id\":\"campaign-e2e\"" : "\"campaign_id\":\"other-campaign\"") != 0 ||
                    replace_fixture_text(response_body, sizeof(response_body), "\\\"campaign_id\\\":\\\"\\\"",
                        request_no == 9 ? "\\\"campaign_id\\\":\\\"campaign-e2e\\\"" : "\\\"campaign_id\\\":\\\"other-campaign\\\"") != 0)
                    break;
            }
        }
        response_length = strlen(response_body);
        if (slow_request) {
            struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
            atomic_store_explicit(
                &server->slow_request_started, 1, memory_order_relaxed);
            nanosleep(&delay, NULL);
        }
        {
            char header[160];
            int keep_open = request_no == 0 && request_keepalive;
            int hn = snprintf(
                header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                response_length, keep_open ? "keep-alive" : "close");
            if (hn > 0) {
                (void)write_all(client, header, (size_t)hn);
                (void)write_all(client, response_body, response_length);
            }
            if (!keep_open) {
                close(client);
                client = -1;
            }
        }
    }
    if (client >= 0) close(client);
    return NULL;
}

#if defined(__linux__)
static int decimal_name(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    if (!cursor || !cursor[0]) return 0;
    while (*cursor) {
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') return 0;
        cursor++;
    }
    return 1;
}

static int scheduler_read_thread(
    pid_t process_id,
    const char *thread_id,
    scheduler_sample *sample
) {
    char path[160];
    FILE *input;
    unsigned long long runtime;
    unsigned long long runqueue;
    unsigned long long timeslices;
    int written;
    if (process_id <= 0 || !decimal_name(thread_id) || !sample) return -1;
    written = snprintf(
        path,
        sizeof(path),
        "/proc/%ld/task/%s/schedstat",
        (long)process_id,
        thread_id);
    if (written <= 0 || (size_t)written >= sizeof(path)) return -1;
    input = fopen(path, "r");
    if (!input) return -1;
    if (fscanf(input, "%llu %llu %llu", &runtime, &runqueue, &timeslices) != 3) {
        fclose(input);
        return -1;
    }
    fclose(input);
    if (UINT64_MAX - sample->runtime_ns < (uint64_t)runtime ||
        UINT64_MAX - sample->runqueue_ns < (uint64_t)runqueue ||
        UINT64_MAX - sample->timeslices < (uint64_t)timeslices) return -1;
    sample->runtime_ns += (uint64_t)runtime;
    sample->runqueue_ns += (uint64_t)runqueue;
    sample->timeslices += (uint64_t)timeslices;
    sample->available = 1;
    return 0;
}

static int scheduler_read_process(pid_t process_id, scheduler_sample *sample) {
    char path[64];
    struct dirent *entry;
    DIR *tasks;
    int written;
    int found = 0;
    int failed = 0;
    if (process_id <= 0 || !sample) return -1;
    memset(sample, 0, sizeof(*sample));
    written = snprintf(path, sizeof(path), "/proc/%ld/task", (long)process_id);
    if (written <= 0 || (size_t)written >= sizeof(path)) return -1;
    tasks = opendir(path);
    if (!tasks) return -1;
    while ((entry = readdir(tasks)) != NULL) {
        if (!decimal_name(entry->d_name)) continue;
        if (scheduler_read_thread(process_id, entry->d_name, sample) == 0)
            found = 1;
        else
            failed = 1;
    }
    closedir(tasks);
    if (!found || failed) {
        memset(sample, 0, sizeof(*sample));
        return -1;
    }
    return 0;
}
#else
static int scheduler_read_process(pid_t process_id, scheduler_sample *sample) {
    (void)process_id;
    if (sample) memset(sample, 0, sizeof(*sample));
    return -1;
}
#endif

static void scheduler_snapshot_all(scheduler_sample *samples, size_t capacity) {
    size_t index;
    if (!samples || capacity < child_count) return;
    memset(samples, 0, capacity * sizeof(samples[0]));
    for (index = 0; index < child_count; ++index)
        (void)scheduler_read_process(children[index], &samples[index]);
}

static void scheduler_report(
    const scheduler_sample *before,
    const scheduler_sample *after,
    size_t count
) {
    uint64_t total_runtime = 0;
    uint64_t total_runqueue = 0;
    uint64_t total_timeslices = 0;
    size_t reported = 0;
    size_t index;
    if (!before || !after || count > child_count) return;
    for (index = 0; index < count; ++index) {
        uint64_t runtime;
        uint64_t runqueue;
        uint64_t timeslices;
        if (!before[index].available || !after[index].available ||
            after[index].runtime_ns < before[index].runtime_ns ||
            after[index].runqueue_ns < before[index].runqueue_ns ||
            after[index].timeslices < before[index].timeslices) continue;
        runtime = after[index].runtime_ns - before[index].runtime_ns;
        runqueue = after[index].runqueue_ns - before[index].runqueue_ns;
        timeslices = after[index].timeslices - before[index].timeslices;
        if (UINT64_MAX - total_runtime < runtime ||
            UINT64_MAX - total_runqueue < runqueue ||
            UINT64_MAX - total_timeslices < timeslices) continue;
        total_runtime += runtime;
        total_runqueue += runqueue;
        total_timeslices += timeslices;
        reported++;
        printf(
            "BenchmarkHTTP_WarmScheduler\tservice=%s\truntime_us=%llu\t"
            "runqueue_us=%llu\ttimeslices=%llu\n",
            child_names[index],
            (unsigned long long)(runtime / UINT64_C(1000)),
            (unsigned long long)(runqueue / UINT64_C(1000)),
            (unsigned long long)timeslices);
    }
    if (reported != 0) {
        printf(
            "BenchmarkHTTP_WarmSchedulerTotal\tservices=%zu\truntime_us=%llu\t"
            "runqueue_us=%llu\ttimeslices=%llu\n",
            reported,
            (unsigned long long)(total_runtime / UINT64_C(1000)),
            (unsigned long long)(total_runqueue / UINT64_C(1000)),
            (unsigned long long)total_timeslices);
    }
}

static pid_t spawn_service(const char *path) {
    const char *cpuset = getenv("SERVICE_E2E_SERVICE_CPUSET");
    const char *taskset_bin = getenv("SERVICE_E2E_TASKSET_BIN");
    if (!path || !path[0]) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (cpuset && cpuset[0]) {
            if (!taskset_bin || !taskset_bin[0]) _exit(127);
            execl(
                taskset_bin,
                taskset_bin,
                "-c",
                cpuset,
                path,
                (char *)NULL);
            _exit(127);
        }
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0 && child_count < CHILDREN_MAX) {
        const char *name = strrchr(path, '/');
        children[child_count] = pid;
        (void)snprintf(
            child_names[child_count],
            sizeof(child_names[child_count]),
            "%s",
            name ? name + 1 : path);
        child_count++;
    }
    return pid;
}

static int stop_children(void) {
    size_t i;
    int bad = 0;
    /* Stop consumers while the broker is still available, then broker last. */
    for (i = child_count; i > 1; --i) {
        if (children[i - 1] > 0) (void)kill(children[i - 1], SIGTERM);
    }
    for (i = child_count; i > 1; --i) {
        int status;
        if (children[i - 1] > 0) {
            if (waitpid(children[i - 1], &status, 0) < 0 ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0) bad = 1;
        }
    }
    if (child_count > 0 && children[0] > 0) {
        int status;
        (void)kill(children[0], SIGTERM);
        if (waitpid(children[0], &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) bad = 1;
    }
    child_count = 0;
    return bad;
}

static void on_turn_event(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_event_c event;
    turn_seen *seen;
    (void)subject;
    (void)reply;
    (void)user;
    if (pb_decode_turn_event(data, data_len, &event) != 0) return;
    if (strcmp(event.request_id, "req-e2e") == 0) seen = &normal_seen;
    else if (strcmp(event.request_id, "req-cancel") == 0) seen = &cancel_seen;
    else if (strcmp(event.request_id, "req-early-cancel") == 0)
        seen = &early_cancel_seen;
    else if (strcmp(event.request_id, "req-llm") == 0) seen = &llm_seen;
    else if (strcmp(event.request_id, "req-llm-cancel") == 0) seen = &llm_cancel_seen;
    else if (strcmp(event.request_id, "req-llm-late-error") == 0 ||
             strcmp(event.request_id, "req-llm-late-limit") == 0)
        seen = &llm_late_error_seen;
    else if (strcmp(event.request_id, "req-rag-success") == 0) seen = &rag_success_seen;
    else if (strcmp(event.request_id, "req-rag-cancel") == 0) seen = &rag_cancel_seen;
    else if (strcmp(event.request_id, "req-tts-pipeline-cancel") == 0)
        seen = &pipeline_cancel_seen;
    else if (strcmp(event.request_id, "req-tts-subject-bind") == 0)
        seen = &subject_bind_seen;
    else if (strcmp(event.request_id, "req-tts-fallback-subject") == 0)
        seen = &fallback_subject_seen;
    else if (strcmp(event.request_id, max_derived_request_id) == 0)
        seen = &max_derived_subject_seen;
    else if (strcmp(event.request_id, "req-tts-future-stage") == 0)
        seen = &future_stage_seen;
    else if (strcmp(event.request_id, "req-tts-partial") == 0)
        seen = &partial_pcm_seen;
    else if (strcmp(event.request_id, "req-tts-header-only") == 0)
        seen = &tts_header_only_seen;
    else if (strcmp(event.request_id, "req-tts-truncated") == 0)
        seen = &tts_truncated_seen;
    else if (strcmp(event.request_id, "req-tts-truncated-recovered") == 0)
        seen = &tts_recovered_seen;
    else if (strcmp(event.request_id, "req-tts-deferred-single") == 0)
        seen = &deferred_single_seen;
    else if (strcmp(event.request_id, "req-tts-deferred-missing") == 0)
        seen = &deferred_missing_seen;
    else if (strcmp(event.request_id, "req-tts-deferred-bad-marker") == 0)
        seen = &deferred_bad_marker_seen;
    else if (strcmp(event.request_id, "req-tts-deferred-mode-mismatch") == 0)
        seen = &deferred_mode_mismatch_seen;
    else if (strcmp(event.request_id, "req-tts-deferred-orphan-marker") == 0)
        seen = &deferred_orphan_marker_seen;
    else if (strcmp(
                 event.request_id,
                 "req-tts-index-full-collision-122789") == 0)
        seen = &tts_index_collision_a_seen;
    else if (strcmp(
                 event.request_id,
                 "req-tts-index-full-collision-339192") == 0)
        seen = &tts_index_collision_b_seen;
    else if (strncmp(
                 event.request_id,
                 "req-tts-order-fill-",
                 sizeof("req-tts-order-fill-") - 1u) == 0)
        seen = &tts_order_capacity_seen;
    else if (strcmp(event.request_id, "req-tts-order-overflow") == 0)
        seen = &tts_order_overflow_seen;
    else if (strcmp(event.request_id, "req-tts-order-recovered") == 0)
        seen = &tts_order_capacity_recovered_seen;
    else if (strcmp(event.request_id, "req-stt-malformed") == 0)
        seen = &stt_failure_seen;
    else if (strcmp(event.request_id, "req-stt-recovered") == 0)
        seen = &stt_recovered_seen;
    else if (strcmp(event.request_id, "req-stt-silence") == 0)
        seen = &stt_silence_seen;
    else if (strcmp(event.request_id, "req-pcm-rejected") == 0)
        seen = &pcm_rejected_seen;
    else if (strcmp(event.request_id, "req-disconnect") == 0) seen = &disconnect_seen;
    else if (strcmp(event.request_id, "req-no-tts") == 0) seen = &no_tts_seen;
    else if (strcmp(event.request_id, "req-budget-valid") == 0)
        seen = &budget_valid_seen;
    else if (strcmp(event.request_id, "req-budget-invalid") == 0)
        seen = &budget_invalid_seen;
    else if (strcmp(event.request_id, "req-budget-expired") == 0)
        seen = &budget_expired_seen;
    else if (strcmp(event.request_id, "req-token-idle") == 0)
        seen = &token_idle_seen;
    else if (strcmp(event.request_id, "req-token-flush") == 0)
        seen = &token_flush_seen;
    else if (strncmp(
                 event.request_id, "auto-audio-e2e-",
                 sizeof("auto-audio-e2e-") - 1) == 0) seen = &auto_seen;
    else return;
    if (seen == &fallback_subject_seen &&
        strcmp(subject, "ai.turn.events.req-tts-fallback-subject") != 0)
        fallback_wrong_subject = 1;
    if (seen == &max_derived_subject_seen &&
        strcmp(subject, max_derived_subject) != 0)
        max_derived_wrong_subject = 1;
    if (event.type_id == 4) {
        seen->text_delta = 1;
        if (event.text[0] && event.speech_text[0] && event.display_text[0] &&
            strcmp(event.text, event.display_text) == 0 &&
            strcmp(event.speech_text, event.display_text) == 0 &&
            strchr(event.display_text, '<') == NULL)
            seen->text_delta_clean_channels = 1;
    } else if (event.type_id == 5) {
        seen->final_event = 1;
        (void)snprintf(seen->final_text, sizeof(seen->final_text), "%s", event.display_text);
        if (event.speech_text[0] && event.display_text[0] &&
            strchr(event.display_text, '<') == NULL) seen->clean_channels = 1;
    } else if (event.type_id == 6) {
        if (!seen->segment_event && seen->text_delta)
            seen->text_before_segment = 1;
        seen->segment_event = 1;
    } else if (event.type_id == 7) {
        if (!seen->pcm_started && seen->segment_event) seen->segment_before_pcm = 1;
        seen->pcm_started = 1;
        seen->pcm_started_stages = event.stages;
    } else if (event.type_id == 8) {
        int pcm_index_matches = 1;
        int verify_pcm = seen == &normal_seen || seen == &partial_pcm_seen;
        size_t expected_frame_bytes = seen == &partial_pcm_seen ?
            SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES :
            SERVICE_E2E_TEST_TTS_FRAME_BYTES;
        size_t expected_len = seen == &partial_pcm_seen && event.sequence == 2 ?
            expected_frame_bytes / 2u : expected_frame_bytes;
        uint32_t expected_sample_rate = seen == &partial_pcm_seen ?
            SERVICE_E2E_PRODUCTION_TTS_SAMPLE_RATE :
            SERVICE_E2E_TEST_TTS_SAMPLE_RATE;
        size_t pcm_index;
        if (!seen->pcm_chunk) seen->first_pcm_stages = event.stages;
        if (verify_pcm && event.sequence != seen->pcm_chunk)
            seen->pcm_sequence_ordered = 0;
        for (pcm_index = 0u; verify_pcm && pcm_index < event.audio_len;
             ++pcm_index) {
            size_t absolute =
                (size_t)event.sequence * expected_frame_bytes + pcm_index;
            if (event.audio[pcm_index] != (uint8_t)(absolute * 131u + 17u))
                pcm_index_matches = 0;
        }
        if (verify_pcm && event.audio_len == expected_len && pcm_index_matches)
            seen->pcm_bytes_valid++;
        if (event.is_final) {
            seen->pcm_final_seen++;
            seen->pcm_final_sequence = event.sequence;
        }
        seen->pcm_chunk++;
        if (event.stages.tts_request_received_at_ms > 0)
            seen->staged_pcm_chunks++;
        if (seen == &llm_seen) {
            if (event.segment_index < llm_pcm_last_segment ||
                event.segment_index > llm_pcm_last_segment + 1)
                llm_pcm_ordered = 0;
            if (event.segment_index > llm_pcm_last_segment) {
                llm_pcm_last_segment = event.segment_index;
                llm_pcm_segments++;
            }
            if (event.is_final) llm_pcm_final_seen = 1;
        }
        if (event.audio_len == expected_len &&
            event.sample_rate == (int32_t)expected_sample_rate &&
            event.channels == 1 && event.bit_depth == 16 &&
            event.segment_index == 0 && event.is_final && event.audio_encoding == 1)
            seen->audio_fields++;
    } else if (event.type_id == 9) {
        if (seen->pcm_chunk > 0) seen->pcm_end_after_chunk++;
        seen->pcm_ended++;
    } else if (event.type_id == 10) {
        seen->completed = 1;
    } else if (event.type_id == 11) {
        seen->canceled = 1;
    } else if (event.type_id == 12) {
        seen->failed = 1;
        seen->completion_limited = strcmp(event.text, "model completion token limit reached") == 0;
        if (seen == &token_idle_seen) token_idle_failures++;
        if (seen == &subject_bind_seen &&
            strcmp(subject, "ai.turn.events.req-tts-subject-bind") != 0)
            subject_bind_wrong_subject = 1;
    }
}

static void on_transcription(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    stt_transcription_c transcript;
    stt_lifecycle_c event;
    (void)subject;
    (void)reply;
    (void)user;
    if (pb_decode_stt_transcription(data, data_len, &transcript) == 0 &&
        transcript.transcript[0]) {
        if (strcmp(transcript.session_id, "audio-e2e") == 0 &&
            strcmp(transcript.transcript, "hello") == 0 && transcript.is_final)
            transcription_final++;
        if (transcript.sequence == 1 && !transcript.is_partial &&
            transcript.has_voice_activity && strcmp(transcript.state, "final") == 0 &&
            transcript.commit_for_turn && transcript.stream_state == 2)
            transcription_schema_clean++;
        if (input_stages_complete_and_ordered(&transcript.input_stages) &&
            transcript.timestamp_ms ==
                transcript.input_stages.stt_transcript_published_at_ms)
            transcription_stage_clean++;
        return;
    }
    if (pb_decode_stt_lifecycle(data, data_len, &event) != 0) return;
    if (event.type_id == STT_LIFECYCLE_STREAM_STARTED) lifecycle_started++;
    if (event.type_id == STT_LIFECYCLE_STREAM_ENDED) lifecycle_ended++;
    if (event.type_id == STT_LIFECYCLE_TRANSCRIPTION_FAILED)
        transcription_failed_lifecycle++;
    if (strcmp(event.session_id, "audio-idle-direct") == 0) {
        if (event.type_id == STT_LIFECYCLE_STREAM_STARTED)
            direct_idle_audio_started++;
        if (event.type_id == STT_LIFECYCLE_STREAM_ENDED)
            direct_idle_audio_ended++;
    }
    if (strcmp(event.session_id, "audio-store-rejected") == 0 &&
        event.type_id == STT_LIFECYCLE_STREAM_ENDED)
        audio_store_rejected_ended++;
    if (strcmp(event.session_id, "audio-store-recovered") == 0) {
        if (event.type_id == STT_LIFECYCLE_STREAM_STARTED)
            audio_store_recovered_started++;
        if (event.type_id == STT_LIFECYCLE_STREAM_ENDED)
            audio_store_recovered_ended++;
    }
    if (strcmp(event.session_id, "audio-chunk-oversized") == 0 &&
        event.type_id == STT_LIFECYCLE_STREAM_ENDED)
        oversized_audio_chunk_ended++;
}

static void on_voice_reflex(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_event_c event;
    (void)reply;
    (void)user;
    if (!subject || pb_decode_turn_event(data, data_len, &event) != 0) return;
    if (strcmp(subject, "ai.voice.reflex.audio-stt-recovered") == 0 &&
        strcmp(event.request_id, "req-stt-recovered") == 0 &&
        event.type_id == 10)
        endpoint_reflex_request_bound++;
    if (strcmp(subject, "ai.voice.reflex.audio-pcm-rejected") == 0 &&
        strcmp(event.request_id, "req-pcm-rejected") == 0 &&
        event.type_id == 12)
        rejected_reflex_request_bound++;
}

static void on_gateway_audio_stream(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    stt_stream_message_c msg;
    size_t i;
    (void)subject;
    (void)reply;
    (void)user;
    if (pb_decode_stt_stream_message(data, data_len, &msg) != 0) return;
    if (strcmp(msg.type, "start") == 0) {
        if (gateway_audio_seen.starts != 0 || gateway_audio_seen.chunks != 0 ||
            gateway_audio_seen.ends != 0)
            gateway_audio_seen.ordered = 0;
        gateway_audio_seen.starts++;
        return;
    }
    if (strcmp(msg.type, "chunk") == 0) {
        int matches = msg.audio_len == 640u && msg.sample_rate == 16000 &&
            msg.channels == 1 && msg.bit_depth == 16;
        if (gateway_audio_seen.starts != 1 || gateway_audio_seen.ends != 0 ||
            gateway_audio_seen.chunks >= GATEWAY_AUDIO_TOTAL_FRAMES)
            gateway_audio_seen.ordered = 0;
        for (i = 0; matches && i < 320u; ++i) {
            uint16_t expected = gateway_audio_seen.chunks == 0
                ? (uint16_t)(int16_t)((i & 1u) ? 10000 : -10000)
                : 0u;
            if (msg.audio[i * 2u] != (uint8_t)(expected & 0xffu) ||
                msg.audio[i * 2u + 1u] != (uint8_t)(expected >> 8))
                matches = 0;
        }
        if (!matches) gateway_audio_seen.valid_pcm = 0;
        gateway_audio_seen.chunks++;
        return;
    }
    if (strcmp(msg.type, "end") == 0) {
        if (gateway_audio_seen.starts != 1 ||
            gateway_audio_seen.chunks != GATEWAY_AUDIO_TOTAL_FRAMES ||
            gateway_audio_seen.ends != 0)
            gateway_audio_seen.ordered = 0;
        gateway_audio_seen.ends++;
    }
}

static void on_rejected_audio_stream(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    stt_stream_message_c msg;
    (void)subject;
    (void)reply;
    (void)user;
    if (pb_decode_stt_stream_message(data, data_len, &msg) == 0 &&
        strcmp(msg.type, "cancel") == 0) rejected_audio_canceled++;
}

static void on_expiring_audio_stream(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    static const char abandoned_prefix[] = "ai.voice.stream.audio-abandoned-";
    stt_stream_message_c msg;
    (void)reply;
    (void)user;
    if (!subject || pb_decode_stt_stream_message(data, data_len, &msg) != 0) return;
    if (strncmp(subject, abandoned_prefix, sizeof(abandoned_prefix) - 1u) == 0) {
        if (strcmp(msg.type, "start") == 0) abandoned_audio_started++;
        if (strcmp(msg.type, "cancel") == 0) expired_audio_canceled++;
    } else if (strcmp(subject, "ai.voice.stream.audio-recovered") == 0 &&
               strcmp(msg.type, "start") == 0) {
        recovered_audio_started++;
    } else if (strcmp(subject, "ai.voice.stream.audio-refreshed") == 0) {
        if (strcmp(msg.type, "chunk") == 0) refreshed_audio_chunks++;
        if (strcmp(msg.type, "cancel") == 0) refreshed_audio_canceled++;
    }
}

static void on_rag_request(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    rag_search_request_c request;
    (void)subject;
    (void)reply;
    (void)user;
    if (pb_decode_rag_search_request(data, data_len, &request) == 0)
        rag_requests_seen++;
}

static void on_bus_stage(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_start_c turn;
    turn_tts_segment_c segment;
    (void)reply;
    (void)user;
    if (!subject) return;
    if ((strcmp(subject, SUBJ_TURN_START) == 0 ||
         strcmp(subject, SUBJ_TURN_GENERATE) == 0) &&
        pb_decode_turn_start(data, data_len, &turn) == 0 &&
        strcmp(turn.request_id, "req-llm") == 0) {
        if (strcmp(subject, SUBJ_TURN_START) == 0 &&
            llm_bus_stages.turn_start_at_ns == 0) {
            llm_bus_stages.turn_start_at_ns = mono_ns();
            llm_bus_stages.product_metadata_seen =
                strcmp(turn.metadata.interaction_profile, "dnd_app") == 0 &&
                strcmp(turn.metadata.client_transport, "http-turn-stream") == 0 &&
                strcmp(turn.metadata.campaign_id, "campaign-e2e") == 0 &&
                strcmp(turn.metadata.knowledge_scope, "shared_rulebook") == 0 &&
                strcmp(turn.metadata.retrieval_force, "true") == 0 &&
                strcmp(turn.user_id, "u-e2e") == 0;
            memcpy(
                llm_bus_stages.response_subject,
                turn.response_subject,
                sizeof(llm_bus_stages.response_subject));
            llm_bus_stages.response_subject[
                sizeof(llm_bus_stages.response_subject) - 1u] = '\0';
            llm_bus_stages.response_subject_seen = 1;
        }
        if (strcmp(subject, SUBJ_TURN_GENERATE) == 0 &&
            llm_bus_stages.turn_generate_at_ns == 0)
            llm_bus_stages.turn_generate_at_ns = mono_ns();
        return;
    }
    if (strcmp(subject, SUBJ_TURN_TTS_SPEAK) == 0 &&
        pb_decode_turn_tts_segment(data, data_len, &segment) == 0 &&
        strcmp(segment.request_id, "req-llm") == 0) {
        if (segment.text_len == 0u) {
            if (segment.is_final && segment.stream_finality_deferred)
                llm_bus_stages.tts_final_markers++;
        } else {
            llm_bus_stages.tts_text_segments++;
            if (segment.segment_index == 0 &&
                llm_bus_stages.tts_speak_at_ns == 0) {
                llm_bus_stages.tts_speak_at_ns = mono_ns();
                llm_bus_stages.first_tts_segment_bytes = segment.text_len;
                llm_bus_stages.first_tts_segment_final = segment.is_final;
                llm_bus_stages.first_tts_segment_deferred =
                    segment.stream_finality_deferred;
            }
        }
    }
}

static int poll_until(vbus_client *client, const int *flag, int timeout_ms) {
    uint64_t started_at_ns;
    uint64_t timeout_ns;
    if (!client || !flag || timeout_ms <= 0) return -1;
    started_at_ns = mono_ns();
    timeout_ns = (uint64_t)(unsigned int)timeout_ms * UINT64_C(1000000);
    while (!*flag) {
        uint64_t now_ns = mono_ns();
        uint64_t remaining_ns;
        int poll_ms;
        if (now_ns == 0 || now_ns < started_at_ns ||
            now_ns - started_at_ns >= timeout_ns) break;
        remaining_ns = timeout_ns - (now_ns - started_at_ns);
        poll_ms = remaining_ns > UINT64_C(20000000) ?
            20 : (int)((remaining_ns + UINT64_C(999999)) / UINT64_C(1000000));
        if (vbus_poll(client, poll_ms) != 0) return -1;
    }
    return *flag ? 0 : -1;
}

static int poll_until_atomic(
    vbus_client *client,
    const atomic_int *value,
    int timeout_ms
) {
    uint64_t started_at_ns;
    uint64_t timeout_ns;
    if (!client || !value || timeout_ms <= 0) return -1;
    started_at_ns = mono_ns();
    timeout_ns = (uint64_t)(unsigned int)timeout_ms * UINT64_C(1000000);
    while (atomic_load_explicit(value, memory_order_relaxed) == 0) {
        uint64_t now_ns = mono_ns();
        uint64_t remaining_ns;
        int poll_ms;
        if (now_ns == 0 || now_ns < started_at_ns ||
            now_ns - started_at_ns >= timeout_ns) break;
        remaining_ns = timeout_ns - (now_ns - started_at_ns);
        poll_ms = remaining_ns > UINT64_C(20000000) ?
            20 : (int)((remaining_ns + UINT64_C(999999)) / UINT64_C(1000000));
        if (vbus_poll(client, poll_ms) != 0) return -1;
    }
    return atomic_load_explicit(value, memory_order_relaxed) != 0 ? 0 : -1;
}

static int poll_until_count(
    vbus_client *client,
    const int *value,
    int target,
    int timeout_ms
) {
    uint64_t started_at_ns;
    uint64_t timeout_ns;
    if (!client || !value || timeout_ms <= 0) return -1;
    started_at_ns = mono_ns();
    timeout_ns = (uint64_t)(unsigned int)timeout_ms * UINT64_C(1000000);
    while (*value < target) {
        uint64_t now_ns = mono_ns();
        uint64_t remaining_ns;
        int poll_ms;
        if (now_ns == 0 || now_ns < started_at_ns ||
            now_ns - started_at_ns >= timeout_ns) break;
        remaining_ns = timeout_ns - (now_ns - started_at_ns);
        poll_ms = remaining_ns > UINT64_C(20000000) ?
            20 : (int)((remaining_ns + UINT64_C(999999)) / UINT64_C(1000000));
        if (vbus_poll(client, poll_ms) != 0) return -1;
    }
    return *value >= target ? 0 : -1;
}

static int publish_turn_text_config(
    vbus_client *client,
    const char *request_id,
    const char *text,
    int enable_tts,
    int enable_rag
) {
    turn_start_c turn;
    uint8_t wire[4096];
    size_t wire_len;
    memset(&turn, 0, sizeof(turn));
    snprintf(turn.request_id, sizeof(turn.request_id), "%s", request_id);
    snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e");
    snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e");
    snprintf(turn.text, sizeof(turn.text), "%s", text);
    turn.enable_tts = enable_tts;
    turn.enable_rag = enable_rag;
    snprintf(
        turn.response_subject, sizeof(turn.response_subject),
        "%s.%s", SUBJ_TURN_EVENTS_PFX, request_id);
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    return wire_len ? vbus_publish(client, SUBJ_VOICE_TURN_PREPARE, wire, wire_len) : -1;
}

static int publish_turn_text(
    vbus_client *client,
    const char *request_id,
    const char *text
) {
    return publish_turn_text_config(client, request_id, text, 1, 0);
}

static int publish_turn_without_response_subject(
    vbus_client *client,
    const char *request_id,
    const char *text
) {
    turn_start_c turn;
    uint8_t wire[4096];
    size_t wire_len;
    int written;
    if (!client || !request_id || !request_id[0] || !text || !text[0]) return -1;
    memset(&turn, 0, sizeof(turn));
    written = snprintf(
        turn.request_id, sizeof(turn.request_id), "%s", request_id);
    if (written <= 0 || (size_t)written >= sizeof(turn.request_id)) return -1;
    written = snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e");
    if (written <= 0 || (size_t)written >= sizeof(turn.user_id)) return -1;
    written = snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e");
    if (written <= 0 || (size_t)written >= sizeof(turn.session_id)) return -1;
    written = snprintf(turn.text, sizeof(turn.text), "%s", text);
    if (written <= 0 || (size_t)written >= sizeof(turn.text)) return -1;
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    return wire_len != 0 &&
        vbus_publish(client, SUBJ_VOICE_TURN_PREPARE, wire, wire_len) == 0 ? 0 : -1;
}

static int publish_turn_text_budget(
    vbus_client *client,
    const char *request_id,
    const char *budget_ms,
    const char *deadline_ms
) {
    turn_start_c turn;
    uint8_t wire[4096];
    size_t wire_len;
    memset(&turn, 0, sizeof(turn));
    snprintf(turn.request_id, sizeof(turn.request_id), "%s", request_id);
    snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e");
    snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e");
    snprintf(turn.text, sizeof(turn.text), "hello");
    if (budget_ms) {
        turn.has_meta_budget = 1;
        snprintf(turn.meta_budget_ms, sizeof(turn.meta_budget_ms), "%s", budget_ms);
    }
    if (deadline_ms) {
        turn.has_meta_deadline = 1;
        snprintf(
            turn.meta_deadline_unix_ms,
            sizeof(turn.meta_deadline_unix_ms),
            "%s",
            deadline_ms);
    }
    snprintf(
        turn.response_subject,
        sizeof(turn.response_subject),
        "%s.%s",
        SUBJ_TURN_EVENTS_PFX,
        request_id);
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    return wire_len ? vbus_publish(client, SUBJ_TURN_START, wire, wire_len) : -1;
}

static int publish_tts_segment_subject(
    vbus_client *client,
    const char *request_id,
    const char *text,
    int32_t segment_index,
    int is_final,
    const char *response_subject
) {
    turn_tts_segment_c segment;
    uint8_t wire[4096];
    size_t wire_len;
    memset(&segment, 0, sizeof(segment));
    snprintf(segment.request_id, sizeof(segment.request_id), "%s", request_id);
    snprintf(segment.text, sizeof(segment.text), "%s", text);
    snprintf(segment.voice_id, sizeof(segment.voice_id), "tara");
    if (response_subject)
        snprintf(
            segment.response_subject, sizeof(segment.response_subject),
            "%s", response_subject);
    else
        snprintf(
            segment.response_subject, sizeof(segment.response_subject),
            "%s.%s", SUBJ_TURN_EVENTS_PFX, request_id);
    segment.segment_index = segment_index;
    segment.is_final = is_final;
    wire_len = pb_encode_turn_tts_segment(wire, sizeof(wire), &segment);
    return wire_len ? vbus_publish(client, SUBJ_TURN_TTS_SPEAK, wire, wire_len) : -1;
}

static int publish_tts_segment(
    vbus_client *client,
    const char *request_id,
    const char *text,
    int32_t segment_index,
    int is_final
) {
    return publish_tts_segment_subject(
        client, request_id, text, segment_index, is_final, NULL);
}

static int publish_deferred_tts_segment_subject(
    vbus_client *client,
    const char *request_id,
    const char *text,
    int32_t segment_index,
    int is_final,
    const char *response_subject
) {
    turn_tts_segment_c segment;
    uint8_t wire[4096];
    size_t wire_len;
    int written;
    if (!client || !request_id || !text) return -1;
    memset(&segment, 0, sizeof(segment));
    written = snprintf(
        segment.request_id, sizeof(segment.request_id), "%s", request_id);
    if (written <= 0 || (size_t)written >= sizeof(segment.request_id))
        return -1;
    written = snprintf(segment.text, sizeof(segment.text), "%s", text);
    if (written < 0 || (size_t)written >= sizeof(segment.text))
        return -1;
    if (response_subject) {
        written = snprintf(
            segment.response_subject,
            sizeof(segment.response_subject),
            "%s",
            response_subject);
        if (written < 0 ||
            (size_t)written >= sizeof(segment.response_subject))
            return -1;
    } else {
        written = snprintf(
            segment.response_subject,
            sizeof(segment.response_subject),
            "%s.%s",
            SUBJ_TURN_EVENTS_PFX,
            request_id);
        if (written <= 0 ||
            (size_t)written >= sizeof(segment.response_subject))
            return -1;
    }
    segment.segment_index = segment_index;
    segment.is_final = is_final;
    segment.stream_finality_deferred = 1;
    wire_len = pb_encode_turn_tts_segment(wire, sizeof(wire), &segment);
    return wire_len != 0u &&
        vbus_publish(client, SUBJ_TURN_TTS_SPEAK, wire, wire_len) == 0 ? 0 : -1;
}

static int publish_deferred_tts_segment(
    vbus_client *client,
    const char *request_id,
    const char *text,
    int32_t segment_index,
    int is_final
) {
    return publish_deferred_tts_segment_subject(
        client, request_id, text, segment_index, is_final, NULL);
}


static int publish_turn(vbus_client *client, const char *request_id) {
    return publish_turn_text(client, request_id, "hello");
}

static int publish_token_chunk(
    vbus_client *client,
    const char *request_id,
    const char *chunk
) {
    uint8_t wire[2304];
    int written;
    if (!client || !request_id || !chunk) return -1;
    written = snprintf(
        (char *)wire, sizeof(wire), "%s\n%s", request_id, chunk);
    return written > 0 && (size_t)written < sizeof(wire) &&
        vbus_publish(client, SUBJ_TURN_TOKEN, wire, (size_t)written) == 0
        ? 0 : -1;
}

static int publish_token_start(
    vbus_client *client,
    const char *request_id,
    const char *chunk
) {
    turn_start_c turn;
    uint8_t wire[4096];
    size_t wire_len;
    if (!client || !request_id || !request_id[0] || !chunk || !chunk[0]) return -1;
    memset(&turn, 0, sizeof(turn));
    if (snprintf(turn.request_id, sizeof(turn.request_id), "%s", request_id) <= 0 ||
        snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e") <= 0 ||
        snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e") <= 0 ||
        snprintf(turn.text, sizeof(turn.text), "%s", chunk) <= 0)
        return -1;
    turn.enable_tts = 1;
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    return wire_len != 0u &&
        vbus_publish(client, SUBJ_TURN_TOKEN, wire, wire_len) == 0 ? 0 : -1;
}

static int publish_audio_turn(vbus_client *client, const char *subject) {
    uint8_t wire[1024];
    uint8_t pcm[640];
    size_t wire_len;
    size_t i;
    for (i = 0; i < 320; ++i) {
        uint16_t sample = (uint16_t)(int16_t)((i & 1u) ? 10000 : -10000);
        pcm[i * 2u] = (uint8_t)(sample & 0xffu);
        pcm[i * 2u + 1u] = (uint8_t)(sample >> 8);
    }
    wire_len = pb_encode_stt_stream_message(
        wire, sizeof(wire), "start", NULL, 0, 16000, 1, 16);
    if (!wire_len || vbus_publish(client, subject, wire, wire_len) != 0) return -1;
    wire_len = pb_encode_stt_stream_message(
        wire, sizeof(wire), "chunk", pcm, sizeof(pcm), 16000, 1, 16);
    if (!wire_len || vbus_publish(client, subject, wire, wire_len) != 0) return -1;
    wire_len = pb_encode_stt_stream_message(
        wire, sizeof(wire), "end", NULL, 0, 16000, 1, 16);
    return wire_len && vbus_publish(client, subject, wire, wire_len) == 0 ? 0 : -1;
}

static int publish_audio_prepare(
    vbus_client *client,
    const char *request_id,
    const char *session_id
) {
    turn_start_c turn;
    uint8_t wire[1024];
    size_t wire_len;
    int written;
    if (!client || !request_id || !session_id) return -1;
    memset(&turn, 0, sizeof(turn));
    written = snprintf(
        turn.request_id, sizeof(turn.request_id), "%s", request_id);
    if (written <= 0 || (size_t)written >= sizeof(turn.request_id)) return -1;
    written = snprintf(
        turn.session_id, sizeof(turn.session_id), "%s", session_id);
    if (written <= 0 || (size_t)written >= sizeof(turn.session_id)) return -1;
    written = snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e");
    if (written <= 0 || (size_t)written >= sizeof(turn.user_id)) return -1;
    written = snprintf(
        turn.response_subject, sizeof(turn.response_subject),
        "%s.%s", SUBJ_TURN_EVENTS_PFX, request_id);
    if (written <= 0 || (size_t)written >= sizeof(turn.response_subject)) return -1;
    turn.enable_tts = 1;
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    return wire_len != 0 &&
        vbus_publish(client, SUBJ_VOICE_TURN_PREPARE, wire, wire_len) == 0 ? 0 : -1;
}

static int publish_audio_stream_marker(
    vbus_client *client,
    const char *subject,
    const char *type
) {
    uint8_t wire[128];
    size_t wire_len = pb_encode_stt_stream_message(
        wire, sizeof(wire), type, NULL, 0, 16000, 1, 16);
    return wire_len != 0 && vbus_publish(client, subject, wire, wire_len) == 0
        ? 0 : -1;
}

static int publish_audio_stream_chunk(
    vbus_client *client,
    const char *subject,
    size_t pcm_len
) {
    uint8_t *wire;
    uint8_t *pcm;
    size_t wire_cap;
    size_t wire_len;
    size_t i;
    int result = -1;
    if (!client || !subject || pcm_len == 0 || (pcm_len & 1u) != 0u ||
        pcm_len > SIZE_MAX - 64u) return -1;
    wire_cap = pcm_len + 64u;
    wire = (uint8_t *)malloc(wire_cap);
    pcm = (uint8_t *)malloc(pcm_len);
    if (!wire || !pcm) goto done;
    for (i = 0; i < pcm_len; i += 2u) {
        uint16_t sample = (uint16_t)(int16_t)((i & 2u) ? 10000 : -10000);
        pcm[i] = (uint8_t)(sample & 0xffu);
        pcm[i + 1u] = (uint8_t)(sample >> 8);
    }
    wire_len = pb_encode_stt_stream_message(
        wire, wire_cap, "chunk", pcm, pcm_len, 16000, 1, 16);
    if (wire_len != 0 && vbus_publish(client, subject, wire, wire_len) == 0)
        result = 0;
done:
    free(pcm);
    free(wire);
    return result;
}

static int publish_gateway_audio_turn(vbus_client *client, const char *subject) {
    uint8_t pcm[640];
    size_t i;
    if (!client || !subject) return -1;
    for (i = 0; i < 320; ++i) {
        uint16_t sample = (uint16_t)(int16_t)((i & 1u) ? 10000 : -10000);
        pcm[i * 2u] = (uint8_t)(sample & 0xffu);
        pcm[i * 2u + 1u] = (uint8_t)(sample >> 8);
    }
    if (vbus_publish(client, subject, pcm, sizeof(pcm)) != 0) return -1;
    return vbus_publish(client, subject, NULL, 0u);
}

static int publish_gateway_audio_chunk(vbus_client *client, const char *subject) {
    uint8_t pcm[640];
    size_t i;
    if (!client || !subject) return -1;
    for (i = 0; i < 320; ++i) {
        uint16_t sample = (uint16_t)(int16_t)((i & 1u) ? 10000 : -10000);
        pcm[i * 2u] = (uint8_t)(sample & 0xffu);
        pcm[i * 2u + 1u] = (uint8_t)(sample >> 8);
    }
    return vbus_publish(client, subject, pcm, sizeof(pcm));
}

static int publish_gateway_silence_chunk(
    vbus_client *client,
    const char *subject
) {
    uint8_t pcm[640] = {0};
    if (!client || !subject) return -1;
    return vbus_publish(client, subject, pcm, sizeof(pcm));
}

static int publish_gateway_silence(vbus_client *client, const char *subject) {
    if (!client || !subject) return -1;
    if (publish_gateway_silence_chunk(client, subject) != 0) return -1;
    return vbus_publish(client, subject, NULL, 0u);
}

static int publish_rejected_gateway_audio(vbus_client *client, const char *subject) {
    uint8_t pcm[640];
    static const uint8_t malformed[3] = {1u, 2u, 3u};
    size_t i;
    if (!client || !subject) return -1;
    for (i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i + 1u);
    if (vbus_publish(client, subject, pcm, sizeof(pcm)) != 0) return -1;
    return vbus_publish(client, subject, malformed, sizeof(malformed));
}

static int tool_health_ready(int port) {
    char url[128], response[512], auth[32];
    uint64_t now = mono_ns(), deadline;
    if (!now || snprintf(url, sizeof(url), "http://127.0.0.1:%d/healthz", port) <= 0) return -1;
    deadline = now + UINT64_C(15000000000);
    /* A loaded CI host can need over nine seconds to initialize its store.
     * Bound fixture startup separately from admitted turn deadlines. Each
     * probe has short I/O; only configured authentication is Ready. */
    while ((now = mono_ns()) != 0 && now < deadline) {
        struct timespec delay = {0, 10000000};
        uint64_t remaining_ms = (deadline - now) / UINT64_C(1000000);
        size_t length = 0;
        int status = 0;
        cmp_json_object health;
        if (!remaining_ms) break;
        int timeout_ms = remaining_ms < 100u ? (int)remaining_ms : 100;
        if (http_min_get(url, (uint8_t *)response, sizeof(response) - 1u,
                &length, &status, timeout_ms) == HTTP_MIN_OK && status == 200 && length < sizeof(response)) {
            response[length] = '\0';
            if (cmp_json_object_parse(response, &health) &&
                cmp_json_object_str(&health, "auth", auth, sizeof(auth)) && !strcmp(auth, "configured")) return 0;
        }
        (void)nanosleep(&delay, NULL);
    }
    return -1;
}

static int dnd_public_result(char *response, dnd_tool_result *result) {
    cmp_json_object event, metadata;
    char *line = strstr(response, "{\"type\":\"text_completed\"");
    char *end;
    char reason[32], tool[32];
    if (!line || !(end = strchr(line, '\n'))) return -1;
    *end = '\0';
    memset(result, 0, sizeof(*result));
    return cmp_json_object_parse(line, &event) &&
        cmp_json_object_str(&event, "text", result->text, sizeof(result->text)) &&
        cmp_json_object_object(&event, "metadata", &metadata) &&
        cmp_json_object_str(&metadata, "cascade_fallback_reason", reason, sizeof(reason)) &&
        strcmp(reason, "tool_executed") == 0 &&
        cmp_json_object_str(&metadata, "cascade_tool_id", tool, sizeof(tool)) &&
        strcmp(tool, "dnd-dice-roll") == 0 &&
        cmp_json_object_str(&metadata, "cascade_tool_call_id", result->call_id, sizeof(result->call_id)) &&
        cmp_json_object_str(&metadata, "cascade_tool_output_hash", result->output_sha256, sizeof(result->output_sha256)) ? 0 : -1;
}

typedef struct {
    char text[2048];
    const char *request_id;
    size_t length;
    int segments;
    int final;
    int invalid;
} dnd_speech_proof;

static void on_dnd_speech(const char *subject, const char *reply, const uint8_t *data,
                           size_t length, void *user) {
    turn_tts_segment_c segment;
    dnd_speech_proof *proof = user;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_tts_segment(data, length, &segment) != 0 ||
        strcmp(segment.request_id, proof->request_id ? proof->request_id : "req-dice-speech") != 0) return;
    if (proof->final || segment.segment_index != proof->segments ||
        segment.text_len == 0u || segment.text_len + 2u > sizeof(proof->text) - proof->length) {
        proof->invalid = 1;
        return;
    }
    if (proof->length) proof->text[proof->length++] = ' ';
    memcpy(proof->text + proof->length, segment.text, segment.text_len);
    proof->length += segment.text_len;
    proof->text[proof->length] = '\0';
    proof->segments++;
    proof->final = segment.is_final;
}

static int tool_owned_read(int port, const char *secret, const char *id, const char *user,
                           char *response, size_t capacity) {
    char path[128], request[1024], nonce[33], signature[65];
    int64_t timestamp = (int64_t)time(NULL);
    int length;
    if (snprintf(path, sizeof(path), "/v1/tools/calls/%s", id) >= (int)sizeof(path) ||
        voice_auth_random_nonce(nonce) != VOICE_AUTH_OK ||
        voice_auth_sign(secret, strlen(secret), "GET", path, user, timestamp, nonce,
                        NULL, 0u, signature) != VOICE_AUTH_OK) return -1;
    length = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: localhost\r\n"
        "X-Tool-User: %s\r\nX-Tool-Timestamp: %lld\r\nX-Tool-Nonce: %s\r\nX-Tool-Signature: %s\r\n\r\n",
        path, user, (long long)timestamp, nonce, signature);
    return length > 0 && (size_t)length < sizeof(request) ?
        gateway_raw_request(port, request, (size_t)length, response, capacity) : -1;
}

static int tool_fixture_command(const char *url, const char *secret, const char *id,
                                const char *tool, const char *input) {
    char request[2048], escaped[1024], response[8192];
    char timestamp[32], nonce[33], signature[65];
    int64_t now = (int64_t)time(NULL);
    int length, status = 0;
    size_t received = 0;
    http_min_header headers[] = {{"X-Tool-User", "u-e2e"}, {"X-Tool-Timestamp", timestamp},
        {"X-Tool-Nonce", nonce}, {"X-Tool-Signature", signature}};
    if (cmp_json_escape_exact(input, escaped, sizeof(escaped))) return -1;
    length = snprintf(request, sizeof(request), "{\"tool_call_id\":\"%s\",\"idempotency_key\":\"%s\","
        "\"parent_turn_id\":\"fixture\",\"session_id\":\"s-e2e\",\"agent_id\":\"dnd-agent\","
        "\"tool_id\":\"%s\",\"input_json\":\"%s\",\"deadline_unix_ms\":%lld}",
        id, id, tool, escaped, (long long)(now * 1000 + 5000));
    (void)snprintf(timestamp, sizeof(timestamp), "%lld", (long long)now);
    if (length <= 0 || (size_t)length >= sizeof(request) || voice_auth_random_nonce(nonce) != VOICE_AUTH_OK ||
        voice_auth_sign(secret, strlen(secret), "POST", "/v1/tools/execute", "u-e2e", now, nonce,
            (const uint8_t *)request, (size_t)length, signature) != VOICE_AUTH_OK) return -1;
    int rc = http_min_post_headers_cancel(url, "application/json", headers, 4u,
        (const uint8_t *)request, (size_t)length, (uint8_t *)response, sizeof(response) - 1u,
        &received, &status, 3000, NULL);
    response[received] = '\0';
    return rc == HTTP_MIN_OK && status == 200 && strstr(response, "\"state\":\"completed\"") ? 0 : -1;
}

static void encounter_turn_proof(int gateway_port, const char *token, const char *url, const char *secret,
                                 vbus_client *client, dnd_speech_proof *speech) {
    static const char prompt[] = "Show the initiative order.";
    char response[32768] = {0};
    dnd_tool_result snapshot = {0}, retry = {0};
    atomic_int canceled = 0;
    expect("create governed campaign through signed C HTTP", tool_fixture_command(url, secret, "fixture-campaign",
        "dnd-campaign-state", "{\"operation\":\"create\",\"campaign_id\":\"campaign-e2e\",\"expected_version\":0,"
        "\"campaign\":{\"name\":\"E2E\",\"ruleset\":\"5e\"}}") == 0);
    expect("start governed encounter through signed C HTTP", tool_fixture_command(url, secret, "fixture-start",
        "dnd-encounter-state", "{\"operation\":\"start\",\"campaign_id\":\"campaign-e2e\",\"encounter_id\":\"battle\",\"expected_version\":0}") == 0);
    expect("add fixture participant through signed C HTTP", tool_fixture_command(url, secret, "fixture-add",
        "dnd-encounter-state", "{\"operation\":\"add\",\"campaign_id\":\"campaign-e2e\",\"encounter_id\":\"battle\",\"expected_version\":1,"
        "\"participant\":{\"id\":\"rogue\",\"name\":\"Rogue\",\"initiative\":18,\"max_hp\":20,\"current_hp\":0}}") == 0);
    expect("advance fixture encounter through signed C HTTP", tool_fixture_command(url, secret, "fixture-advance",
        "dnd-encounter-state", "{\"operation\":\"advance\",\"campaign_id\":\"campaign-e2e\",\"encounter_id\":\"battle\",\"expected_version\":2}") == 0);
    if (failures) return;
    memset(speech, 0, sizeof(*speech));
    speech->request_id = "req-encounter-speech";
    int gateway_result = gateway_post_turn_observe_ex(
        gateway_port, token, speech->request_id, prompt, "u-e2e", "u-e2e", 0, NULL,
        1, 1, 0, "", response, sizeof(response), NULL, NULL);
    int complete = gateway_result == 0 &&
        strstr(response, "\"dnd_encounter_state\":{") && strstr(response, "\"current_hp\":0") &&
        strstr(response, "\"cascade_tool_id\":\"dnd-encounter-state\"") &&
        strstr(response, "\"type\":\"pcm_chunk\"") && strstr(response, "\"type\":\"completed\"") &&
        !strstr(response, "\"type\":\"failed\"") && !strstr(response, "cascade_retrieval_used");
    if (!complete) fprintf(stderr, "encounter fixture gateway result=%d response=%s\n", gateway_result, response);
    expect("C encounter turn reaches governed state, PCM and completion without a model", complete);
    int retry_result = dnd_encounter_execute(url, secret,
        speech->request_id, "u-e2e", "s-e2e", prompt, "campaign-e2e", "battle",
        (int64_t)time(NULL) * 1000 + 5000, 3000, &canceled, &snapshot);
    if (retry_result != DND_TOOL_OK)
        fprintf(stderr, "encounter fixture signed retry result=%d\n", retry_result);
    expect("signed encounter retry returns the projected receipt", retry_result == DND_TOOL_OK &&
        strstr(response, snapshot.output_sha256) && strstr(response, snapshot.text));
    for (int i = 0; i < 20 && !speech->final; ++i) (void)vbus_poll(client, 100);
    expect("encounter speech contains the saved active participant and round", speech->final && !speech->invalid &&
        !strcmp(speech->text, snapshot.text) && strstr(speech->text, "Round 1. Rogue acts now."));
    const char *ended = last_occurrence(response, "\"type\":\"pcm_ended\"");
    const char *completed = strstr(response, "\"type\":\"completed\"");
    expect("encounter completion follows final PCM", ended && completed && completed > ended);
    expect("changed encounter retry cannot switch state", dnd_encounter_execute(url, secret,
        speech->request_id, "u-e2e", "s-e2e", prompt, "campaign-e2e", "other",
        (int64_t)time(NULL) * 1000 + 5000, 3000, &canceled, &retry) != DND_TOOL_OK);
    expect("wrong campaign owner gets no encounter or speech", gateway_post_turn_observe_ex(
        gateway_port, token, "req-encounter-owner", prompt, "other-user", "other-user", 0, NULL,
        1, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 &&
        strstr(response, "\"type\":\"failed\"") && !strstr(response, "\"type\":\"text_completed\"") &&
        !strstr(response, "\"type\":\"pcm_chunk\""));
    expect("missing encounter context stops before output", gateway_post_turn_observe_ex(
        gateway_port, token, "req-encounter-missing", prompt, "u-e2e", "u-e2e", 0, NULL,
        0, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 &&
        strstr(response, "\"type\":\"failed\"") && !strstr(response, "\"type\":\"text_completed\""));
}

/* Reuse the native process harness for the complete authenticated dice path.
 * No model process or scripting interpreter participates in these turns. */
static int dnd_service_e2e(void) {
    static const char token[] = "test-gateway-token-0123456789abcdef-0123456789abcdef";
    static const char secret[] = "test-tool-secret-0123456789abcdef-0123456789abcdef";
    static const char prompt[] = "Roll 2d20 with advantage and add 5 for my Perception check. Tell me the actual dice and total.";
    const char *tool_bin = getenv("PTOOLS_BIN");
    char directory[] = "/tmp/c-dnd-e2e-XXXXXX";
    char socket_path[128], path[256], url[160], response[16384], saved_output[4096];
    dnd_tool_result public_result = {0}, retry = {0}, other = {0};
    dnd_speech_proof speech = {0};
    fake_tts tts = {0};
    pthread_t tts_thread;
    int tts_started = 0;
    atomic_int cancel = 0;
    vbus_client *client = NULL;
    int gateway_port = reserve_loopback_port(), tool_port = reserve_loopback_port();
    int i, status = 0;
    size_t tool_index;
    FILE *artifact;
    failures = 0;
    child_count = 0;
    if (gateway_port <= 0 || tool_port <= 0 || !mkdtemp(directory)) return 1;
    tts.listen_fd = -1;
    atomic_init(&tts.stop, 0);
    if (!tool_bin || !tool_bin[0]) tool_bin = "../../agents/platform-tools/c-ptools/c-ptools";
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("VOICE_GATEWAY_TOKEN", token, 1);
    setenv("TOOL_HTTP_AUTH_SECRET", secret, 1);
    setenv("TOOL_HTTP_TIMEOUT_MS", "1000", 1);
    setenv("LLM_HTTP_URL", "", 1);
    unsetenv("TURN_BUDGET_MS");
    unsetenv("NATS_URL");
    (void)snprintf(path, sizeof(path), "%d", gateway_port);
    setenv("GATEWAY_HEALTH_PORT", path, 1);
    (void)snprintf(path, sizeof(path), "127.0.0.1:%d", tool_port);
    setenv("TOOL_HTTP_ADDR", path, 1);
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/tools/execute", tool_port);
    setenv("TOOL_HTTP_URL", url, 1);
    (void)snprintf(path, sizeof(path), "%s/calls.json", directory);
    setenv("TOOL_CALL_STORE_PATH", path, 1);
    (void)snprintf(path, sizeof(path), "%s/artifacts", directory);
    setenv("TOOL_ARTIFACT_DIR", path, 1);
    (void)snprintf(path, sizeof(path), "%s/dnd", directory);
    setenv("TOOL_DND_STATE_DIR", path, 1);
    expect("spawn dice proof broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (i = 0; i < 100; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (client) break;
        (void)nanosleep(&delay, NULL);
    }
    if (!client) { (void)stop_children(); return 1; }
    tool_index = child_count;
    expect("spawn real C dice host", spawn_service(tool_bin) > 0);
    expect("authenticated dice host ready", tool_health_ready(tool_port) == 0);
    if (failures) goto done;
    {
        struct sockaddr_in address = {0};
        socklen_t address_length = sizeof(address);
        tts.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (tts.listen_fd < 0 || bind(tts.listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(tts.listen_fd, 8) != 0 || getsockname(tts.listen_fd, (struct sockaddr *)&address, &address_length) != 0) {
            expect("bind native dice TTS fixture", 0);
            goto done;
        }
        (void)snprintf(path, sizeof(path), "http://127.0.0.1:%u/pcm", (unsigned)ntohs(address.sin_port));
        setenv("TTS_HTTP_URL", path, 1);
        /* State fixtures create a longer pause before the encounter speech turn. */
        tts.expected_requests = INT_MAX;
        if (pthread_create(&tts_thread, NULL, fake_tts_thread, &tts) != 0) {
            expect("start native dice TTS fixture", 0);
            goto done;
        }
        tts_started = 1;
    }
    expect("observe dice speech segments", vbus_subscribe(client, SUBJ_TURN_TTS_SPEAK, NULL, on_dnd_speech, &speech) == 0);
    expect("spawn C cascade for dice", spawn_service(getenv("CASCADE_ROUTER_BIN") ? getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    expect("spawn C TTS for dice", spawn_service(getenv("TTS_MODULE_BIN") ? getenv("TTS_MODULE_BIN") : "./c-tts-module") > 0);
    expect("spawn C gateway for dice", spawn_service(getenv("GATEWAY_BIN") ? getenv("GATEWAY_BIN") : "./c-voice-session-gateway") > 0);
    expect("dice gateway ready", check_gateway_health(gateway_port) == 0);
    if (failures) goto done;
    /* Broker subscription readiness follows process startup within a bounded wait. */
    (void)vbus_poll(client, 100);
    expect("authenticated gateway dice turn completes without a model", gateway_post_turn_observe_ex(
        gateway_port, token, "req-dice-real", prompt, "u-e2e", "u-e2e", 0, NULL,
        0, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 &&
        strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"type\":\"failed\""));
    expect("public dice response carries typed audited provenance", dnd_public_result(response, &public_result) == 0);
    expect("authenticated retry returns the public result", dnd_tools_execute(url, secret,
        "req-dice-real", "u-e2e", "s-e2e", prompt, (int64_t)time(NULL) * 1000 + 3000,
        2000, &cancel, &retry) == DND_TOOL_OK && strcmp(retry.text, public_result.text) == 0 &&
        strcmp(retry.call_id, public_result.call_id) == 0 && strcmp(retry.output_sha256, public_result.output_sha256) == 0);
    (void)snprintf(path, sizeof(path), "%s/artifacts/%s.json", directory, retry.output_sha256);
    artifact = fopen(path, "rb");
    expect("public hash identifies a durable dice artifact", artifact != NULL);
    if (artifact) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        char hash[65], text[2048];
        size_t length = fread(saved_output, 1, sizeof(saved_output) - 1u, artifact);
        saved_output[length] = '\0';
        expect("artifact is bounded", feof(artifact) && !ferror(artifact));
        fclose(artifact);
        (void)SHA256((const unsigned char *)saved_output, length, digest);
        for (i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            (void)snprintf(hash + 2 * i, 3u, "%02x", (unsigned)digest[i]);
        expect("artifact hash and recomputed arithmetic match public speech", strcmp(hash, retry.output_sha256) == 0 &&
            dnd_dice_output_text(saved_output, "2d20kh1+5", retry.call_id, text, sizeof(text)) == 0 && strcmp(text, public_result.text) == 0);
    }
    expect("another user cannot read the dice result", tool_owned_read(tool_port, secret, retry.call_id,
        "other-user", response, sizeof(response)) == 0 && strstr(response, "404") && !strstr(response, retry.output_sha256));
    expect("changed dice retry cannot reroll", dnd_tools_execute(url, secret, "req-dice-real", "u-e2e", "s-e2e",
        "Roll 1d6", (int64_t)time(NULL) * 1000 + 3000, 2000, &cancel, &other) != DND_TOOL_OK);
    expect("authenticated dice speech reaches PCM and completion", gateway_post_turn_observe_ex(
        gateway_port, token, "req-dice-speech", prompt, "u-e2e", "u-e2e", 0, NULL,
        1, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 &&
        strstr(response, "\"type\":\"pcm_chunk\"") && strstr(response, "\"is_final\":true") &&
        strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"type\":\"failed\""));
    {
        const char *pcm_end = last_occurrence(response, "\"type\":\"pcm_ended\"");
        const char *completed = strstr(response, "\"type\":\"completed\"");
        expect("dice completion follows the final PCM boundary", pcm_end && completed && completed > pcm_end);
    }
    expect("spoken dice retains its public provenance", dnd_public_result(response, &other) == 0);
    for (i = 0; i < 20 && !speech.final; ++i) (void)vbus_poll(client, 100);
    expect("ordered TTS segments contain exactly the audited dice receipt", speech.final && speech.segments > 0 &&
        !speech.invalid && strcmp(speech.text, other.text) == 0);
    expect("C TTS uses the native provider contract", atomic_load(&tts.requests) > 0 &&
        atomic_load(&tts.valid_requests) == atomic_load(&tts.requests));
    {
        gateway_turn_call call = {0};
        pthread_t thread;
        call.port = gateway_port;
        call.request_id = "req-dice-speech-cancel";
        call.text = prompt;
        if (pthread_create(&thread, NULL, gateway_turn_thread, &call) == 0) {
            for (i = 0; i < 200 && !atomic_load(&tts.late_error_pcm_sent); ++i) {
                struct timespec delay = {0, 10000000};
                (void)nanosleep(&delay, NULL);
            }
            expect("dice barge-in begins after provider PCM", atomic_load(&tts.late_error_pcm_sent) != 0);
            expect("authenticated dice speech cancellation succeeds", gateway_post_cancel(
                gateway_port, call.request_id, response, sizeof(response)) == 0 && strstr(response, "200 OK"));
            pthread_join(thread, NULL);
            expect("dice barge-in closes without late PCM finality", call.result == 0 &&
                strstr(call.response, "\"type\":\"canceled\"") &&
                !strstr(call.response, "\"type\":\"pcm_ended\"") &&
                !strstr(call.response, "\"type\":\"completed\""));
            expect("canceled speech retains its audited roll", dnd_public_result(call.response, &other) == 0 &&
                tool_owned_read(tool_port, secret, other.call_id, "u-e2e", response, sizeof(response)) == 0 &&
                strstr(response, "\"state\":\"completed\"") && strstr(response, other.output_sha256));
        } else expect("start cancelable dice speech", 0);
    }
    encounter_turn_proof(gateway_port, token, url, secret, client, &speech);
    expect("stop dice host before restart", kill(children[tool_index], SIGTERM) == 0 &&
        waitpid(children[tool_index], &status, 0) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    children[tool_index] = 0;
    tool_index = child_count;
    expect("restart real C dice host", spawn_service(tool_bin) > 0 && tool_health_ready(tool_port) == 0);
    if (failures) goto done;
    expect("restart preserves one audited roll", dnd_tools_execute(url, secret, "req-dice-real", "u-e2e", "s-e2e",
        prompt, (int64_t)time(NULL) * 1000 + 3000, 2000, &cancel, &other) == DND_TOOL_OK &&
        strcmp(other.text, retry.text) == 0 && strcmp(other.output_sha256, retry.output_sha256) == 0);
    expect("unsupported dice commands fail without model fallback", gateway_post_turn_observe_ex(
        gateway_port, token, "req-dice-unsupported", "Roll initiative for the party.", "u-e2e", "u-e2e", 0, NULL,
        0, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 && strstr(response, "\"type\":\"failed\"") &&
        !strstr(response, "\"type\":\"text_completed\""));
    expect("stop tool host to exercise a stalled call", kill(children[tool_index], SIGSTOP) == 0);
    {
        gateway_turn_call call = {0};
        pthread_t thread;
        struct timespec delay = {0, 100000000};
        call.port = gateway_port;
        call.request_id = "req-dice-cancel";
        call.text = prompt;
        call.disable_tts = 1;
        if (pthread_create(&thread, NULL, gateway_turn_thread, &call) == 0) {
            (void)nanosleep(&delay, NULL);
            expect("authenticated cancellation reaches the stalled dice turn", gateway_post_cancel(
                gateway_port, call.request_id, response, sizeof(response)) == 0 && strstr(response, "200 OK"));
            pthread_join(thread, NULL);
            expect("stalled dice cancellation emits no result", call.result == 0 &&
                strstr(call.response, "\"type\":\"canceled\"") && !strstr(call.response, "\"type\":\"text_completed\""));
        } else expect("start stalled dice request", 0);
    }
    expect("stalled dice deadline emits no result", gateway_post_turn_observe_ex(
        gateway_port, token, "req-dice-deadline", prompt, "u-e2e", "u-e2e", 0, NULL,
        0, 0, 0, "", response, sizeof(response), NULL, NULL) == 0 && strstr(response, "\"type\":\"failed\"") &&
        !strstr(response, "\"type\":\"text_completed\""));
    (void)kill(children[tool_index], SIGCONT);
done:
    vbus_close(client);
    expect("dice proof services stop cleanly", stop_children() == 0);
    atomic_store_explicit(&tts.stop, 1, memory_order_relaxed);
    if (tts_started) pthread_join(tts_thread, NULL);
    if (tts.listen_fd >= 0) close(tts.listen_fd);
    printf("DND_TEST_ARTIFACT_DIR=%s\n", directory);
    if (!failures) puts("ALL PASS native authenticated gateway/cascade/tool dice path");
    return failures ? 1 : 0;
}

typedef struct {
    vbus_client *client;
    turn_retrieval_c provenance;
    int published;
} citation_edge_publisher;

typedef struct {
    int port;
    const char *request_id;
    char response[DND_RAG_PUBLIC_JSON_CAP + 2048u];
    int result;
    atomic_int done;
} citation_edge_call;

static void on_citation_edge_turn(const char *subject, const char *reply,
    const uint8_t *data, size_t length, void *user) {
    citation_edge_publisher *publisher = user;
    turn_start_c request;
    uint8_t wire[32768];
    size_t used;
    const char *event_id;
    (void)subject; (void)reply;
    if (pb_decode_turn_start(data, length, &request) != 0 ||
        strcmp(request.user_id, "u-e2e") != 0 || !request.response_subject[0]) return;
    event_id = strcmp(request.request_id, "citation-edge-wrong") == 0 ? "wrong-turn" : request.request_id;
    used = pb_encode_turn_text_event(wire, sizeof(wire), event_id, "text_completed",
        "Rule [1].", "Rule [1].", "Rule [1].", 0, 1);
    used = pb_append_turn_retrieval(wire, sizeof(wire), used, &publisher->provenance);
    if (!used || vbus_publish(publisher->client, request.response_subject, wire, used) != 0) return;
    used = pb_encode_turn_event(wire, sizeof(wire), request.request_id, "completed", "");
    if (used && vbus_publish(publisher->client, request.response_subject, wire, used) == 0) publisher->published++;
}

static void *citation_edge_request(void *opaque) {
    citation_edge_call *call = opaque;
    call->result = gateway_post_turn_observe_ex(call->port,
        "test-gateway-token-0123456789abcdef-0123456789abcdef", call->request_id,
        "rules", "u-e2e", "u-e2e", 0, NULL, 0, 0, 0, "", call->response, sizeof(call->response), NULL, NULL);
    atomic_store_explicit(&call->done, 1, memory_order_release);
    return NULL;
}

static int citation_edge_e2e(void) {
    char directory[] = "/tmp/c-citation-edge-XXXXXX", socket_path[128], port_text[16];
    uint8_t provenance[DND_RAG_CITATIONS_WIRE_CAP];
    dnd_rag_result result = {0};
    citation_edge_publisher publisher = {0};
    citation_edge_call *call = calloc(1, sizeof(*call));
    int port = reserve_loopback_port(), i;
    size_t index, length;
    failures = 0; child_count = 0;
    if (!call || port <= 0 || !mkdtemp(directory)) { free(call); return 1; }
    snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    snprintf(port_text, sizeof(port_text), "%d", port);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("GATEWAY_HEALTH_PORT", port_text, 1);
    setenv("VOICE_GATEWAY_TOKEN", "test-gateway-token-0123456789abcdef-0123456789abcdef", 1);
    result.count = 4u;
    for (index = 0; index < result.count; ++index) {
        dnd_rag_citation *citation = &result.hits[index].citation;
        memset(citation->source, '"', sizeof(citation->source) - 1u);
        memcpy(citation->source, "s3://b/", 7u);
        memset(citation->section, 1, sizeof(citation->section) - 1u);
        memset(citation->embedding_model, 1, sizeof(citation->embedding_model) - 1u);
        strcpy(citation->collection, "dnd_campaign_canon_v1");
        strcpy(citation->document_id, "private-note");
        strcpy(citation->corpus_version, "sha256:");
        memset(citation->corpus_version + 7u, 'a', 64u);
        memset(citation->record_id, (int)('a' + index), 64u);
        memset(citation->content_hash, 'b', 64u);
        memset(citation->source_sha256, 'c', 64u);
        citation->score = 0.875;
    }
    length = pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result);
    expect("large native citation provenance encodes", length &&
        pb_decode_retrieval_provenance(provenance, length, &publisher.provenance) == 0);
    expect("spawn citation broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (i = 0; i < 100 && !publisher.client; ++i) {
        struct timespec delay = {0, 10000000};
        publisher.client = vbus_connect(socket_path);
        if (!publisher.client) nanosleep(&delay, NULL);
    }
    expect("connect native citation publisher", publisher.client != NULL);
    if (!publisher.client || failures) goto done;
    expect("subscribe authenticated citation turns", vbus_subscribe(publisher.client,
        SUBJ_TURN_START, NULL, on_citation_edge_turn, &publisher) == 0);
    expect("spawn citation HTTP gateway", spawn_service(getenv("GATEWAY_BIN") ? getenv("GATEWAY_BIN") : "./c-voice-session-gateway") > 0);
    for (i = 0; i < 100 && check_gateway_health(port) != 0; ++i) {
        struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
    }
    expect("citation HTTP gateway ready", check_gateway_health(port) == 0);
    if (failures) goto done;
    for (index = 0; index < 2u; ++index) {
        pthread_t thread;
        call->port = port;
        call->request_id = index ? "citation-edge-wrong" : "citation-edge-large";
        call->response[0] = '\0'; atomic_init(&call->done, 0);
        if (pthread_create(&thread, NULL, citation_edge_request, call) != 0) { failures++; break; }
        for (i = 0; i < 800 && !atomic_load_explicit(&call->done, memory_order_acquire); ++i)
            (void)vbus_poll(publisher.client, 10);
        pthread_join(thread, NULL);
        if (!index) expect("C HTTP gateway forwards complete citation JSON above 64 KiB",
            call->result == 0 && strlen(call->response) > 65536u &&
            strstr(call->response, "source_sha256") && strstr(call->response, "\"cascade_retrieved_documents\":\"4\"") &&
            strstr(call->response, "\"type\":\"completed\"") && !strstr(call->response, "\"type\":\"failed\""));
        else expect("C HTTP gateway rejects citations bound to another request",
            call->result == 0 && strstr(call->response, "\"type\":\"failed\"") &&
            !strstr(call->response, "cascade_retrieval_citations"));
    }
    expect("native citation publisher saw both authenticated turns", publisher.published == 2);
done:
    if (publisher.client) vbus_close(publisher.client);
    expect("citation services stop cleanly", stop_children() == 0);
    unlink(socket_path); rmdir(directory); free(call);
    if (!failures) puts("ALL PASS native authenticated citation HTTP edge");
    return failures ? 1 : 0;
}

typedef struct {
    vbus_client *client;
    turn_start_c expected;
    int ended;
    int received;
    int matched;
} prepared_metadata_fixture;

static void on_prepared_metadata(const char *subject, const char *reply,
    const uint8_t *data, size_t length, void *user) {
    prepared_metadata_fixture *fixture = user;
    (void)reply;
    if (strcmp(subject, "ai.voice.stream.metadata-audio") == 0) {
        stt_stream_message_c stream;
        stt_transcription_c transcript = {0};
        uint8_t wire[4096];
        size_t used;
        if (pb_decode_stt_stream_message(data, length, &stream) != 0 || strcmp(stream.type, "end") != 0) return;
        fixture->ended++;
        strcpy(transcript.session_id, fixture->expected.session_id);
        strcpy(transcript.transcript, fixture->expected.dnd_initiative.count ?
            "Roll initiative for the party." : fixture->expected.dnd_campaign.operation ? "Create the campaign." :
            fixture->expected.dnd_encounter_action.operation ? "Apply damage in the encounter." : "Which rule applies?");
        transcript.sequence = 1;
        transcript.is_final = transcript.commit_for_turn = transcript.has_voice_activity = 1;
        transcript.timestamp_ms = (int64_t)time(NULL) * 1000;
        used = pb_encode_stt_transcription(wire, sizeof(wire), &transcript);
        expect("publish native STT contract after PCM commit", used &&
            vbus_publish(fixture->client, "ai.voice.transcription.metadata-audio", wire, used) == 0);
    } else if (strcmp(subject, SUBJ_TURN_START) == 0) {
        turn_start_c actual;
        const turn_start_c *expected = &fixture->expected;
        fixture->received++;
        fixture->matched = pb_decode_turn_start(data, length, &actual) == 0 &&
            strcmp(actual.request_id, expected->request_id) == 0 &&
            strcmp(actual.session_id, expected->session_id) == 0 &&
            strcmp(actual.user_id, expected->user_id) == 0 &&
            strcmp(actual.response_subject, expected->response_subject) == 0 &&
            strcmp(actual.text, expected->dnd_initiative.count ?
                "Roll initiative for the party." : expected->dnd_campaign.operation ? "Create the campaign." :
                expected->dnd_encounter_action.operation ? "Apply damage in the encounter." : "Which rule applies?") == 0 &&
            actual.dnd_initiative.count == expected->dnd_initiative.count &&
            (!actual.dnd_initiative.count ||
             memcmp(&actual.dnd_initiative, &expected->dnd_initiative, sizeof(actual.dnd_initiative)) == 0) &&
            actual.dnd_campaign.operation == expected->dnd_campaign.operation &&
            (!actual.dnd_campaign.operation ||
             memcmp(&actual.dnd_campaign, &expected->dnd_campaign, sizeof(actual.dnd_campaign)) == 0) &&
            actual.dnd_encounter_action.operation == expected->dnd_encounter_action.operation &&
            (!actual.dnd_encounter_action.operation || memcmp(&actual.dnd_encounter_action,
                &expected->dnd_encounter_action, sizeof(actual.dnd_encounter_action)) == 0) &&
            actual.premium == expected->premium && actual.enable_rag == expected->enable_rag &&
            actual.enable_tts == expected->enable_tts &&
            memcmp(&actual.metadata, &expected->metadata, sizeof(actual.metadata)) == 0 &&
            actual.has_meta_budget == expected->has_meta_budget &&
            actual.has_meta_deadline == expected->has_meta_deadline &&
            strcmp(actual.meta_budget_ms, expected->meta_budget_ms) == 0 &&
            strcmp(actual.meta_deadline_unix_ms, expected->meta_deadline_unix_ms) == 0;
    }
}

static int prepared_metadata_e2e(void) {
    char directory[] = "/tmp/c-audio-metadata-XXXXXX", socket_path[128], port_text[16];
    prepared_metadata_fixture fixture = {0};
    int port = reserve_loopback_port(), i, turn_index;
    failures = 0; child_count = 0;
    if (port <= 0 || !mkdtemp(directory)) return 1;
    snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    snprintf(port_text, sizeof(port_text), "%d", port);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("GATEWAY_HEALTH_PORT", port_text, 1);
    setenv("VOICE_GATEWAY_TOKEN", "test-gateway-token-0123456789abcdef-0123456789abcdef", 1);
    expect("spawn audio metadata broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (i = 0; i < 100 && !fixture.client; ++i) {
        struct timespec delay = {0, 10000000};
        fixture.client = vbus_connect(socket_path);
        if (!fixture.client) nanosleep(&delay, NULL);
    }
    expect("connect native audio metadata fixture", fixture.client != NULL);
    if (!fixture.client || failures) goto done;
    expect("observe prepared audio stream", vbus_subscribe(fixture.client,
        "ai.voice.stream.metadata-audio", NULL, on_prepared_metadata, &fixture) == 0);
    expect("observe transcribed turn metadata", vbus_subscribe(fixture.client,
        SUBJ_TURN_START, NULL, on_prepared_metadata, &fixture) == 0);
    expect("spawn audio metadata gateway", spawn_service(getenv("GATEWAY_BIN") ? getenv("GATEWAY_BIN") : "./c-voice-session-gateway") > 0);
    for (i = 0; i < 100 && check_gateway_health(port) != 0; ++i) {
        struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
    }
    expect("audio metadata gateway ready", check_gateway_health(port) == 0);
    if (failures) goto done;
    for (turn_index = 0; turn_index < 5; ++turn_index) {
        uint8_t wire[DND_TURN_START_WIRE_MAX];
        size_t length;
        turn_start_c *turn = &fixture.expected;
        memset(turn, 0, sizeof(*turn));
        fixture.ended = fixture.received = fixture.matched = 0;
        strcpy(turn->request_id, turn_index == 4 ? "metadata-next" : turn_index == 3 ? "metadata-action" : turn_index == 2 ? "metadata-campaign" :
            turn_index == 1 ? "metadata-initiative" : "metadata-first");
        strcpy(turn->user_id, turn_index ? "next-owner" : "first-owner");
        strcpy(turn->session_id, "metadata-audio");
        snprintf(turn->response_subject, sizeof(turn->response_subject), "ai.turn.events.%s", turn->request_id);
        turn->premium = turn->enable_rag = !turn_index;
        turn->enable_tts = 1;
        if (turn_index != 4) {
            strcpy(turn->metadata.interaction_profile, "dnd_app");
            strcpy(turn->metadata.client_transport, "webtransport-turn-stream");
            strcpy(turn->metadata.campaign_id, "campaign-a");
            strcpy(turn->metadata.character_id, "character-a");
            strcpy(turn->metadata.knowledge_scope, "character_memory");
            strcpy(turn->metadata.audio_group_session, "true");
            strcpy(turn->metadata.audio_participant_id, "player-a");
            strcpy(turn->metadata.audio_participant_label, "Mira 🐉");
            strcpy(turn->meta_budget_ms, "45000");
            strcpy(turn->meta_deadline_unix_ms, "4102444800000");
            turn->has_meta_budget = turn->has_meta_deadline = 1;
        }
        if (turn_index == 1) {
            strcpy(turn->metadata.encounter_id, "battle");
            turn->dnd_initiative.campaign_version = 2;
            turn->dnd_initiative.count = DND_INITIATIVE_SELECTIONS_MAX;
            strcpy(turn->dnd_initiative.operation_id, "saved-audio-roll");
            for (size_t choice = 0; choice < DND_INITIATIVE_SELECTIONS_MAX; ++choice) {
                dnd_initiative_selection_c *selection = &turn->dnd_initiative.selections[choice];
                snprintf(selection->character_id, sizeof(selection->character_id), "pc-%02zu", choice);
                memset(selection->character_id + 5, 'a', 59u);
                selection->character_id[64] = '\0';
                strcpy(selection->expression, "2d20kh1+5");
            }
        }
        if (turn_index == 2) {
            turn->dnd_campaign.operation = DND_CAMPAIGN_CREATE;
            strcpy(turn->dnd_campaign.operation_id, "saved-audio-campaign");
            strcpy(turn->dnd_campaign.data.campaign.name, "Mira's campaign");
            strcpy(turn->dnd_campaign.data.campaign.ruleset, "5e");
        }
        if (turn_index == 3) {
            strcpy(turn->metadata.encounter_id, "battle");
            turn->dnd_encounter_action.operation = DND_ACTION_DAMAGE;
            turn->dnd_encounter_action.expected_version = 2;
            strcpy(turn->dnd_encounter_action.operation_id, "saved-audio-damage");
            strcpy(turn->dnd_encounter_action.participant_id, "aria");
            turn->dnd_encounter_action.amount = 7;
            turn->dnd_encounter_action.damage_type = 4;
        }
        length = pb_encode_turn_start(wire, sizeof(wire), turn);
        expect("prepare native audio context", length &&
            vbus_publish(fixture.client, SUBJ_VOICE_TURN_PREPARE, wire, length) == 0);
        expect("commit real PCM through the C gateway",
            publish_gateway_audio_turn(fixture.client, "ai.voice.pcm.metadata-audio") == 0);
        expect("STT completion preserves the complete prepared identity and context",
            poll_until(fixture.client, &fixture.received, 2000) == 0 &&
            fixture.received == 1 && fixture.ended == 1 && fixture.matched);
    }
done:
    if (fixture.client) vbus_close(fixture.client);
    expect("audio metadata services stop cleanly", stop_children() == 0);
    unlink(socket_path); rmdir(directory);
    if (!failures) puts("ALL PASS native prepared audio metadata and session reuse");
    return failures ? 1 : 0;
}

/* Exercise token-stream state without a model or TTS backend. HTTP/SSE and
 * actual provider request text are covered by the full process gate below. */
static int markup_token_e2e(void) {
    static const struct { const char *first, *tail, *expected; } cases[] = {
        {"**Sneak ", "Attack** deals 1d6; 2 * 3.", "Sneak Attack deals 1d6; 2 * 3."},
        {"**unfinished", "", "**unfinished"},
        {"<la**ugh**> **Stay**", "", "Stay"},
        {"__Fresh__", " answer.", "Fresh answer."},
        {"**Ready**", "", "Ready"}
    };
    char directory[] = "/tmp/c-markup-e2e-XXXXXX", socket_path[128];
    uint8_t wire[4096];
    turn_start_c turn = {0};
    vbus_client *client = NULL;
    size_t wire_len;
    failures = 0;
    child_count = 0;
    if (!mkdtemp(directory)) return 1;
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("LLM_HTTP_URL", "", 1);
    setenv("CASCADE_STREAM_IDLE_TIMEOUT_MS", "1000", 1);
    unsetenv("TURN_BUDGET_MS");
    expect("spawn markup broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (int i = 0; i < 100 && !client; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (!client) (void)nanosleep(&delay, NULL);
    }
    if (!client) { expect("connect markup broker", 0); goto done; }
    expect("observe markup events", vbus_subscribe(client, "ai.turn.events.req-token-flush", NULL, on_turn_event, NULL) == 0);
    expect("spawn markup cascade", spawn_service(getenv("CASCADE_ROUTER_BIN") ? getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    (void)vbus_poll(client, 100);
    (void)snprintf(turn.request_id, sizeof(turn.request_id), "req-token-flush");
    (void)snprintf(turn.user_id, sizeof(turn.user_id), "u-e2e");
    (void)snprintf(turn.session_id, sizeof(turn.session_id), "s-e2e");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&token_flush_seen, 0, sizeof(token_flush_seen));
        (void)snprintf(turn.text, sizeof(turn.text), "%s", cases[i].first);
        wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
        expect("start text-only model token stream", wire_len && vbus_publish(client, SUBJ_TURN_TOKEN, wire, wire_len) == 0);
        if (cases[i].tail[0]) expect("append split model token", publish_token_chunk(client, turn.request_id, cases[i].tail) == 0);
        expect("flush model presentation at successful end", publish_token_chunk(client, turn.request_id, "") == 0);
        expect("complete token output preserves exact literal and formatted text",
            poll_until(client, &token_flush_seen.completed, 1000) == 0 && token_flush_seen.final_event &&
            !token_flush_seen.failed && !token_flush_seen.segment_event &&
            strcmp(token_flush_seen.final_text, cases[i].expected) == 0);
    }
    memset(&token_flush_seen, 0, sizeof(token_flush_seen));
    (void)snprintf(turn.text, sizeof(turn.text), "**discard this pending text");
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    expect("start pending markup before cancellation", wire_len && vbus_publish(client, SUBJ_TURN_TOKEN, wire, wire_len) == 0);
    wire_len = pb_encode_turn_cancel(wire, sizeof(wire), turn.request_id, turn.user_id, "barge-in");
    expect("cancel pending model markup", wire_len && vbus_publish(client, SUBJ_TURN_CANCEL, wire, wire_len) == 0);
    (void)snprintf(turn.text, sizeof(turn.text), "Recovered");
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    expect("reuse canceled model stream", wire_len && vbus_publish(client, SUBJ_TURN_TOKEN, wire, wire_len) == 0 &&
        publish_token_chunk(client, turn.request_id, "") == 0);
    expect("cancellation clears pending markup before reuse", poll_until(client, &token_flush_seen.completed, 1000) == 0 &&
        !strcmp(token_flush_seen.final_text, "Recovered") && !token_flush_seen.failed && !token_flush_seen.segment_event);
    memset(&token_flush_seen, 0, sizeof(token_flush_seen));
    memset(turn.text, 'a', sizeof(turn.text) - 1u);
    turn.text[sizeof(turn.text) - 1u] = '\0';
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    expect("start maximum model input", wire_len && vbus_publish(client, SUBJ_TURN_TOKEN, wire, wire_len) == 0);
    expect("exceed model input bound", publish_token_chunk(client, turn.request_id, "x") == 0);
    expect("oversized model stream fails before a final answer", poll_until(client, &token_flush_seen.failed, 1000) == 0 &&
        !token_flush_seen.completed && !token_flush_seen.final_event);
done:
    if (client) vbus_close(client);
    expect("markup processes stop cleanly", stop_children() == 0);
    (void)unlink(socket_path);
    (void)rmdir(directory);
    printf("%s model presentation process gate\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

typedef struct {
    int segments, markers, invalid;
    size_t largest, used;
    char text[1024];
} segment_bound_fixture;

static void on_segment_bound(const char *subject, const char *reply,
    const uint8_t *data, size_t length, void *user) {
    segment_bound_fixture *fixture = user;
    turn_tts_segment_c segment;
    (void)subject; (void)reply;
    if (pb_decode_turn_tts_segment(data, length, &segment) != 0 ||
        strcmp(segment.request_id, "req-segment-bound")) {
        fixture->invalid = 1;
        return;
    }
    if (segment.segment_index != fixture->segments || !segment.stream_finality_deferred)
        fixture->invalid = 1;
    if (!segment.text_len) {
        if (!segment.is_final || fixture->markers) fixture->invalid = 1;
        fixture->markers++;
        return;
    }
    if (segment.is_final || fixture->markers ||
        segment.text_len + fixture->used + 2u > sizeof(fixture->text)) {
        fixture->invalid = 1;
        return;
    }
    if (fixture->used) fixture->text[fixture->used++] = ' ';
    memcpy(fixture->text + fixture->used, segment.text, segment.text_len);
    fixture->used += segment.text_len;
    fixture->text[fixture->used] = '\0';
    if (segment.text_len > fixture->largest) fixture->largest = segment.text_len;
    fixture->segments++;
}

static int segment_bound_process_e2e(void) {
    static const char text[] =
        "Check who is still in the room before you cast the spell and ask each player "
        "to confirm their position before choosing a safe place for the spell.";
    char directory[] = "/tmp/c-segment-bound-XXXXXX", socket_path[128];
    uint8_t wire[4096];
    turn_start_c turn = {0};
    segment_bound_fixture fixture = {0};
    vbus_client *client = NULL;
    failures = 0; child_count = 0;
    if (!mkdtemp(directory)) return 1;
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("LLM_HTTP_URL", "", 1);
    setenv("CASCADE_FIRST_SEGMENT_CHARS", "5", 1);
    setenv("CASCADE_MAX_SEGMENT_CHARS", "40", 1);
    setenv("CASCADE_EAGER_TTS_SEGMENTS", "1", 1);
    unsetenv("TURN_BUDGET_MS");
    expect("spawn segment-bound broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (int i = 0; i < 100 && !client; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (!client) (void)nanosleep(&delay, NULL);
    }
    if (!client) { expect("connect segment-bound broker", 0); goto done; }
    expect("observe admitted speech segments", vbus_subscribe(client, SUBJ_TURN_TTS_SPEAK, NULL, on_segment_bound, &fixture) == 0);
    expect("spawn configured segment-bound cascade", spawn_service(getenv("CASCADE_ROUTER_BIN") ? getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    int settle_ms = startup_settle_ms();
    struct timespec settle = { (time_t)(settle_ms / 1000), (long)(settle_ms % 1000) * 1000000L };
    (void)nanosleep(&settle, NULL);
    strcpy(turn.request_id, "req-segment-bound");
    strcpy(turn.user_id, "u-e2e"); strcpy(turn.session_id, "s-e2e");
    turn.enable_tts = 1;
    strcpy(turn.text, text);
    size_t length = pb_encode_turn_start(wire, sizeof(wire), &turn);
    expect("publish long speech with configured segment bound", length && vbus_publish(client, SUBJ_TURN_TOKEN, wire, length) == 0 &&
        publish_token_chunk(client, turn.request_id, "") == 0);
    expect("C cascade bounds every admitted ASCII segment to forty characters",
        poll_until(client, &fixture.markers, 1000) == 0 && fixture.segments >= 4 &&
        fixture.largest > 0 && fixture.largest <= 40u);
    expect("bounded speech retains exact content, ordering, and one final marker",
        !fixture.invalid && fixture.markers == 1 && !strcmp(fixture.text, text));
done:
    if (client) vbus_close(client);
    expect("segment-bound processes stop cleanly", stop_children() == 0);
    (void)unlink(socket_path); (void)rmdir(directory);
    printf("%s configured speech segment process gate\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

typedef struct {
    int starts[2], ends[2], chunks[2], finals, canceled, failed, invalid;
    size_t bytes[2];
} tts_wakeup_fixture;

static void on_tts_wakeup(const char *subject, const char *reply,
    const uint8_t *data, size_t length, void *user) {
    tts_wakeup_fixture *fixture = user;
    turn_event_c event;
    (void)subject; (void)reply;
    if (pb_decode_turn_event(data, length, &event) != 0) {
        fixture->invalid = 1;
        return;
    }
    if (event.type_id >= 7 && event.type_id <= 9) {
        /* PCM boundaries carry no segment scalar. Ordered ends select it. */
        int index = event.type_id == 8 ? event.segment_index :
            (fixture->ends[0] ? 1 : 0);
        if (index < 0 || index > 1 || (index && !fixture->ends[0])) {
            fixture->invalid = 1;
            return;
        }
        if (event.type_id == 7) fixture->starts[index]++;
        if (event.type_id == 8) {
            if (fixture->ends[index] || !fixture->starts[index] ||
                event.sequence != fixture->chunks[index] ||
                event.sample_rate != 16000 || event.channels != 1 ||
                event.bit_depth != 16 || event.audio_len != 640u)
                fixture->invalid = 1;
            for (size_t i = 0; i < event.audio_len; ++i)
                if (event.audio[i] != (uint8_t)((fixture->bytes[index] + i) * 131u + 17u))
                    fixture->invalid = 1;
            fixture->bytes[index] += event.audio_len;
            fixture->chunks[index]++;
            if (event.is_final) fixture->finals++;
        }
        if (event.type_id == 9) {
            if (!fixture->chunks[index]) fixture->invalid = 1;
            fixture->ends[index]++;
        }
    }
    if (event.type_id == 11) fixture->canceled++;
    if (event.type_id == 12) fixture->failed++;
}

static int tts_wakeup_process_e2e(void) {
    char directory[] = "/tmp/c-tts-wakeup-XXXXXX", socket_path[128], url[128];
    struct sockaddr_in address = {0};
    socklen_t address_length = sizeof(address);
    fake_tts server = {0};
    tts_wakeup_fixture fixtures[2] = {0};
    pthread_t server_thread;
    int server_started = 0;
    vbus_client *client = NULL;
    failures = 0; child_count = 0; server.listen_fd = -1;
    if (!mkdtemp(directory)) return 1;
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    setenv("VBUS_PATH", socket_path, 1);
    setenv("TTS_STREAM_FINALITY_TIMEOUT_MS", "5000", 1);
    server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (server.listen_fd < 0 ||
        bind(server.listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server.listen_fd, 8) != 0 ||
        getsockname(server.listen_fd, (struct sockaddr *)&address, &address_length) != 0) {
        expect("bind wakeup provider", 0); goto done;
    }
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/pcm", (unsigned)ntohs(address.sin_port));
    setenv("TTS_HTTP_URL", url, 1);
    server.expected_requests = 4;
    if (pthread_create(&server_thread, NULL, fake_tts_thread, &server) != 0) {
        expect("start wakeup provider", 0); goto done;
    }
    server_started = 1;
    expect("spawn wakeup broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (int i = 0; i < 100 && !client; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (!client) (void)nanosleep(&delay, NULL);
    }
    if (!client) { expect("connect wakeup broker", 0); goto done; }
    expect("spawn wakeup TTS", spawn_service(getenv("TTS_MODULE_BIN") ? getenv("TTS_MODULE_BIN") : "./c-tts-module") > 0);
    int settle_ms = startup_settle_ms();
    if (settle_ms < 1000) settle_ms = 1000;
    struct timespec settle = {(time_t)(settle_ms / 1000), (long)(settle_ms % 1000) * 1000000L};
    (void)nanosleep(&settle, NULL);
    for (int cancel = 0; cancel < 2; ++cancel) {
        const char *id = cancel ? "req-wakeup-cancel" : "req-wakeup-finish";
        char subject[128];
        tts_wakeup_fixture *fixture = &fixtures[cancel];
        (void)snprintf(subject, sizeof(subject), "%s.%s", SUBJ_TURN_EVENTS_PFX, id);
        expect("subscribe wakeup turn", vbus_subscribe(client, subject, NULL, on_tts_wakeup, fixture) == 0);
        atomic_store_explicit(&server.responses_sent, 0, memory_order_relaxed);
        expect("publish deferred prefix before successor exists", publish_deferred_tts_segment(client, id, "First prefix.", 0, 0) == 0);
        expect("provider finishes prefix before successor admission", poll_until_atomic(client, &server.responses_sent, 1500) == 0);
        /* The completed body leaves the worker waiting for finality. */
        expect("prefix holds its tail while finality is unknown", poll_until(client, &fixture->ends[0], 150) != 0 &&
            fixture->starts[0] == 1 && !fixture->finals && !fixture->failed);
        atomic_store_explicit(&server.responses_sent, 0, memory_order_relaxed);
        expect("admit successor while prefix waits", publish_deferred_tts_segment(client, id, "Next segment.", 1, 0) == 0);
        expect("successor admission releases prefix before final marker", poll_until(client, &fixture->ends[0], 500) == 0 &&
            fixture->ends[0] == 1 && fixture->bytes[0] == 1920u && !fixture->finals && !fixture->failed);
        expect("successor publishes ordered start before final marker", poll_until(client, &fixture->starts[1], 1000) == 0);
        expect("provider finishes successor before terminal decision", poll_until_atomic(client, &server.responses_sent, 1000) == 0);
        expect("last segment retains unknown final tail", poll_until(client, &fixture->ends[1], 150) != 0 && !fixture->finals && !fixture->failed);
        if (cancel) {
            uint8_t wire[512];
            size_t length = pb_encode_turn_cancel(wire, sizeof(wire), id, "u-e2e", "barge-in");
            expect("cancel deferred finality wait", length && vbus_publish(client, SUBJ_TURN_CANCEL, wire, length) == 0);
            expect("cancellation wakes finality wait without final PCM", poll_until(client, &fixture->canceled, 500) == 0 && fixture->canceled == 1 &&
                !fixture->ends[1] && !fixture->finals && !fixture->failed);
            (void)poll_until(client, &fixture->ends[1], 200);
            expect("canceled wait emits no late tail", !fixture->ends[1] && !fixture->finals && !fixture->invalid);
        } else {
            expect("publish late final marker", publish_deferred_tts_segment(client, id, "", 2, 1) == 0);
            expect("only last ordered PCM tail is final", poll_until(client, &fixture->ends[1], 500) == 0 &&
                fixture->ends[1] == 1 && fixture->bytes[1] == 1920u && fixture->finals == 1 &&
                !fixture->invalid && !fixture->failed && !fixture->canceled);
        }
    }
    expect("markers and cancellation add no provider requests", atomic_load_explicit(&server.requests, memory_order_relaxed) == 4);
done:
    if (client) vbus_close(client);
    expect("wakeup processes stop cleanly", stop_children() == 0);
    atomic_store_explicit(&server.stop, 1, memory_order_relaxed);
    if (server_started) pthread_join(server_thread, NULL);
    if (server.listen_fd >= 0) close(server.listen_fd);
    (void)unlink(socket_path); (void)rmdir(directory);
    printf("%s TTS successor finality wakeup process gate\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

typedef struct {
    voice_model_capture capture;
    char request_id[128];
    uint8_t bytes[VOICE_MODEL_REQUEST_MAX];
    int chunks, completed, failed;
} model_capture_fixture;

static void on_model_capture_fixture(const char *subject, const char *reply,
    const uint8_t *data, size_t length, void *user) {
    model_capture_fixture *fixture = user;
    (void)reply;
    if (strstr(subject, VOICE_MODEL_REQUEST_SUBJECT_SUFFIX)) {
        voice_model_request_chunk chunk;
        size_t offset = fixture->capture.received_bytes;
        fixture->chunks++;
        if (voice_model_request_decode(data, length, &chunk) != 0 ||
            strcmp(chunk.request_id, fixture->request_id) ||
            (!fixture->capture.state && voice_model_capture_begin(&fixture->capture, chunk.total_bytes) != 0) ||
            fixture->capture.expected_bytes != chunk.total_bytes ||
            voice_model_capture_append(&fixture->capture, chunk.sequence, chunk.bytes, chunk.length, chunk.final) != 0) {
            fixture->failed = 1;
            return;
        }
        memcpy(fixture->bytes + offset, chunk.bytes, chunk.length);
    } else {
        turn_event_c event;
        if (pb_decode_turn_event(data, length, &event) != 0 ||
            strcmp(event.request_id, fixture->request_id)) return;
        if (!strcmp(event.type, "completed")) fixture->completed++;
        if (!strcmp(event.type, "failed")) fixture->failed = 1;
    }
}

/* Synthetic data only. Compare private VBus bytes with the actual model HTTP
 * body, then reuse the worker without capture to check permission isolation. */
static int model_capture_process_e2e(void) {
    char directory[] = "/tmp/c-capture-e2e-XXXXXX", socket_path[128], url[128];
    struct sockaddr_in address = {.sin_family = AF_INET};
    socklen_t address_length = sizeof(address);
    vbus_client *client = NULL;
    model_capture_fixture *fixture = calloc(1, sizeof(*fixture));
    int listener = -1, connection = -1;
    int connections[8];
    for (size_t slot = 0; slot < 8u; ++slot) connections[slot] = -1;
    failures = 0;
    child_count = 0;
    if (!fixture || !mkdtemp(directory)) { free(fixture); return 1; }
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0 || getsockname(listener, (struct sockaddr *)&address, &address_length) != 0) {
        expect("create synthetic model listener", 0); goto done;
    }
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1/chat/completions", (unsigned)ntohs(address.sin_port));
    setenv("VBUS_PATH", socket_path, 1);
    setenv("LLM_HTTP_URL", url, 1);
    setenv("LLM_HTTP_TIMEOUT_MS", "3000", 1);
    setenv("LLM_MODEL", "synthetic-capture-model", 1);
    unsetenv("TURN_BUDGET_MS");
    unsetenv("TOOL_HTTP_URL");
    unsetenv("TOOL_HTTP_AUTH_SECRET");
    expect("spawn capture broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (int i = 0; i < 100 && !client; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (!client) (void)nanosleep(&delay, NULL);
    }
    if (!client) { expect("connect capture broker", 0); goto done; }
    expect("subscribe to synthetic capture and turn events",
        vbus_subscribe(client, "ai.turn.events.capture-proof.>", NULL, on_model_capture_fixture, fixture) == 0);
    expect("spawn capture cascade", spawn_service(getenv("CASCADE_ROUTER_BIN") ? getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    (void)vbus_poll(client, 500);
    for (int index = 0; index < 3 && !failures; ++index) {
        turn_start_c turn = {0};
        uint8_t wire[8192];
        char request[16384] = {0}, *body = NULL;
        size_t used = 0, body_length = 0, wire_length;
        int received = 0;
        voice_model_capture_destroy(&fixture->capture);
        memset(fixture, 0, sizeof(*fixture));
        /* Suffixes 0, 4, and 8 share the current four-lane request hash. */
        (void)snprintf(turn.request_id, sizeof(turn.request_id), "capture-proof-%d", index * 4);
        (void)snprintf(fixture->request_id, sizeof(fixture->request_id), "%s", turn.request_id);
        (void)snprintf(turn.user_id, sizeof(turn.user_id), "synthetic-owner");
        (void)snprintf(turn.session_id, sizeof(turn.session_id), "synthetic-session-%d", index);
        (void)snprintf(turn.response_subject, sizeof(turn.response_subject), "ai.turn.events.capture-proof.%d", index);
        (void)snprintf(turn.text, sizeof(turn.text), "Explain why the fictional gate says \"Lynn's path\".\nUse a dragon: 🐉.");
        turn.model_request_capture = index == 1;
        wire_length = pb_encode_turn_start(wire, sizeof(wire), &turn);
        expect("publish synthetic capture permission", wire_length && vbus_publish(client, SUBJ_TURN_START, wire, wire_length) == 0);
        for (int tick = 0; tick < 300 && !received && !fixture->failed; ++tick) {
            struct pollfd ready[9];
            ready[0] = (struct pollfd){.fd = listener, .events = POLLIN};
            for (size_t slot = 0; slot < 8u; ++slot)
                ready[slot + 1u] = (struct pollfd){.fd = connections[slot], .events = POLLIN};
            (void)vbus_poll(client, 5);
            if (poll(ready, 9, 5) <= 0) continue;
            if (ready[0].revents & POLLIN) {
                int accepted = accept(listener, NULL, NULL);
                size_t slot = 0;
                while (slot < 8u && connections[slot] >= 0) ++slot;
                if (slot == 8u) { if (accepted >= 0) close(accepted); fixture->failed = 1; break; }
                connections[slot] = accepted;
            }
            int readable = 0;
            for (size_t slot = 0; slot < 8u; ++slot) {
                if ((ready[slot + 1u].revents & POLLIN) &&
                    (connection < 0 || connection == connections[slot])) {
                    connection = connections[slot]; readable = 1; break;
                }
            }
            if (!readable) continue;
            ssize_t count = recv(connection, request + used, sizeof(request) - used - 1u, MSG_DONTWAIT);
            if (count <= 0) break;
            used += (size_t)count;
            request[used] = '\0';
            body = strstr(request, "\r\n\r\n");
            if (body) {
                char *content_length = strstr(request, "Content-Length: ");
                if (!content_length) break;
                body += 4;
                body_length = (size_t)strtoul(content_length + strlen("Content-Length: "), NULL, 10);
                received = body_length > 0 && body_length <= used - (size_t)(body - request);
            }
        }
        expect("real cascade sends a bounded HTTP model request", received && body &&
            strstr(body, "\"model\":\"synthetic-capture-model\"") && strstr(body, "Lynn's path"));
        if (!received) break;
        static const char answer[] =
            "data: {\"model\":\"synthetic-capture-model\",\"choices\":[{\"delta\":{\"content\":\"The gate is open.\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        char header[256];
        int header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", sizeof(answer) - 1u);
        expect("reply from synthetic model", header_length > 0 && (size_t)header_length < sizeof(header) &&
            write_all(connection, header, (size_t)header_length) == 0 && write_all(connection, answer, sizeof(answer) - 1u) == 0);
        close(connection);
        for (size_t slot = 0; slot < 8u; ++slot)
            if (connections[slot] == connection) connections[slot] = -1;
        connection = -1;
        expect("synthetic model turn completes", poll_until(client, &fixture->completed, 2000) == 0 && !fixture->failed);
        (void)vbus_poll(client, 100);
        if (turn.model_request_capture) {
            expect("captured bytes equal the actual HTTP model request", fixture->capture.state == 2 &&
                fixture->chunks > 0 && fixture->capture.received_bytes == body_length &&
                !memcmp(fixture->bytes, body, body_length));
        } else {
            expect("ordinary turn emits no prepared model request before or after capture",
                fixture->chunks == 0 && fixture->capture.state == 0);
        }
    }
done:
    for (size_t slot = 0; slot < 8u; ++slot)
        if (connections[slot] >= 0) close(connections[slot]);
    if (listener >= 0) close(listener);
    if (client) vbus_close(client);
    expect("capture processes stop cleanly", stop_children() == 0);
    voice_model_capture_destroy(&fixture->capture);
    free(fixture);
    (void)unlink(socket_path); (void)rmdir(directory);
    printf("%s prepared model request process gate\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}


/* A configured, unresponsive lab endpoint must receive no Loop request.
 * Real C processes retain owner-bound named question focus without a model. */
static int loop_scene_process_e2e(void) {
    const char *token = "test-gateway-token-0123456789abcdef-0123456789abcdef";
    char directory[] = "/tmp/c-loop-scene-e2e-XXXXXX", socket_path[128], url[128], port[16];
    char response[16384], request[1024];
    struct sockaddr_in address = {.sin_family = AF_INET};
    socklen_t address_length = sizeof(address);
    vbus_client *client = NULL;
    int listener = -1, connection = -1, gateway_port = reserve_loopback_port();
    int model_port = reserve_loopback_port();
    failures = 0; child_count = 0;
    if (gateway_port <= 0 || model_port <= 0 || !mkdtemp(directory)) return 1;
    (void)snprintf(socket_path, sizeof(socket_path), "%s/bus.sock", directory);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 8) != 0 || getsockname(listener, (struct sockaddr *)&address, &address_length) != 0) {
        expect("create unresponsive lab listener", 0); goto done;
    }
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%u", (unsigned)ntohs(address.sin_port));
    (void)snprintf(port, sizeof(port), "%d", gateway_port);
    setenv("VBUS_PATH", socket_path, 1); setenv("VOICE_GATEWAY_TOKEN", token, 1);
    setenv("GATEWAY_HEALTH_PORT", port, 1); setenv("SYSTEMONE_BASE_URL", url, 1);
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", model_port);
    setenv("LLM_HTTP_URL", url, 1); setenv("LLM_HTTP_TIMEOUT_MS", "500", 1);
    setenv("SYSTEMONE_TIMEOUT_MS", "500", 1);
    unsetenv("TURN_BUDGET_MS"); unsetenv("TOOL_HTTP_URL"); unsetenv("TOOL_HTTP_AUTH_SECRET");
    expect("spawn Loop scene broker", spawn_service(getenv("BROKER_BIN") ? getenv("BROKER_BIN") : "./vbus-broker") > 0);
    for (int i = 0; i < 100 && !client; ++i) {
        struct timespec delay = {0, 10000000};
        client = vbus_connect(socket_path);
        if (!client) (void)nanosleep(&delay, NULL);
    }
    if (!client) { expect("connect Loop scene broker", 0); goto done; }
    expect("spawn Loop scene store", spawn_service(getenv("SESSION_MANAGER_BIN") ? getenv("SESSION_MANAGER_BIN") : "./c-session-manager") > 0);
    expect("spawn Loop scene cascade", spawn_service(getenv("CASCADE_ROUTER_BIN") ? getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    expect("spawn Loop scene gateway", spawn_service(getenv("GATEWAY_BIN") ? getenv("GATEWAY_BIN") : "./c-voice-session-gateway") > 0);
    expect("Loop scene gateway ready", check_gateway_health(gateway_port) == 0);
    (void)vbus_poll(client, 100);
    static const char *const questions[] = {
        "I cast fireball. No wait, is Mira still in the room?",
        "What do we still need to establish before choosing a spell?",
        "Is Jorin Vale still here?",
        "Nothing yet. What else should we confirm before casting? Do not cast anything."
    };
    static const char *const expected[] = {
        "Mira is still here.", "Mira's presence is still unknown.",
        "Jorin Vale is still here.", "Jorin Vale's presence is still unknown."
    };
    for (size_t i = 0; i < sizeof(questions) / sizeof(questions[0]) && !failures; ++i) {
        char id[64];
        (void)snprintf(id, sizeof(id), "req-loop-scene-%zu", i);
        expect("authenticated named question and follow-up complete without model or lab",
            gateway_post_turn(gateway_port, token, id, questions[i], response, sizeof(response)) == 0 &&
            strstr(response, expected[i]) && strstr(response, "\"type\":\"completed\"") &&
            !strstr(response, "\"provider_model\"") &&
            (i != 0 || strstr(response, "fireball remains a proposal")));
        struct pollfd ready = {.fd = listener, .events = POLLIN};
        expect("Loop sends no HTTP request to configured lab endpoint", poll(&ready, 1, 0) == 0);
    }
    expect("explicit user name establishes fresh spelling focus",
        gateway_post_turn(gateway_port, token, "req-loop-scene-mixed-focus", "Is Mira still here?",
            response, sizeof(response)) == 0 && strstr(response, "\"type\":\"completed\""));
    expect("mixed spelling conflict clarifies before narration or a model request",
        gateway_post_turn(gateway_port, token, "req-loop-scene-mixed-conflict",
            "I need to know if Mir is nearby. Describe one sound in an imagined courtyard.",
            response, sizeof(response)) == 0 && strstr(response, "Do you mean Mira or a different character?") &&
            strstr(response, "Presence is unknown") && strstr(response, "\"type\":\"completed\"") &&
            !strstr(response, "\"provider_model\""));
    expect("abstention after mixed spelling conflict retains ambiguity without a model",
        gateway_post_turn(gateway_port, token, "req-loop-scene-mixed-followup", questions[3],
            response, sizeof(response)) == 0 && strstr(response, "I heard Mir.") &&
            strstr(response, "Do you mean Mira or a different character?") &&
            strstr(response, "Presence is unknown") && strstr(response, "\"type\":\"completed\"") &&
            !strstr(response, "\"provider_model\""));
    expect("explicit retained-name correction resolves spelling but not presence",
        gateway_post_turn(gateway_port, token, "req-loop-scene-mixed-correction", "I mean Mira.",
            response, sizeof(response)) == 0 && strstr(response, "Mira's presence is still unknown.") &&
            strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"provider_model\""));
    expect("follow-up after explicit correction retains only the selected question name",
        gateway_post_turn(gateway_port, token, "req-loop-scene-corrected-followup", questions[3],
            response, sizeof(response)) == 0 && strstr(response, "Mira's presence is still unknown.") &&
            strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"provider_model\""));
    expect("fresh user question precedes the captured malformed nearby clause",
        gateway_post_turn(gateway_port, token, "req-loop-scene-missing-verb-focus", "Is Mira still here?",
            response, sizeof(response)) == 0 && strstr(response, "\"type\":\"completed\""));
    expect("captured missing verb and punctuation clarify without a model request",
        gateway_post_turn(gateway_port, token, "req-loop-scene-missing-verb",
            "I need to know if mirrors nearby describe one sound in an imagined watchtower.",
            response, sizeof(response)) == 0 && strstr(response, "I heard mirrors.") &&
            strstr(response, "Do you mean Mira or a different character?") &&
            strstr(response, "Presence is unknown") && strstr(response, "\"type\":\"completed\"") &&
            !strstr(response, "\"provider_model\""));
    expect("captured malformed nearby clause keeps ambiguity on an abstention follow-up",
        gateway_post_turn(gateway_port, token, "req-loop-scene-missing-verb-followup", questions[3],
            response, sizeof(response)) == 0 && strstr(response, "I heard mirrors.") &&
            strstr(response, "Do you mean Mira or a different character?") &&
            strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"provider_model\""));
    expect("explicit correction resolves captured token ambiguity without presence or action permission",
        gateway_post_turn(gateway_port, token, "req-loop-scene-missing-verb-correction", "I mean Mira.",
            response, sizeof(response)) == 0 && strstr(response, "Mira's presence is still unknown.") &&
            strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"provider_model\""));
    {
        struct pollfd ready = {.fd = listener, .events = POLLIN};
        expect("captured malformed nearby questions send no configured lab request", poll(&ready, 1, 0) == 0);
    }
    expect("unavailable imagined narration cannot complete on the uncertainty preface alone",
        gateway_post_turn(gateway_port, token, "req-loop-scene-mixed-model-failure",
            "Is Elara still here? Describe a smell in an imagined orchard.",
            response, sizeof(response)) == 0 && strstr(response, "\"type\":\"failed\"") &&
            !strstr(response, "\"type\":\"completed\""));
    expect("failed model narration saves user speech but no assistant success",
        request_session_get_n(client, "loop-scene", "u-e2e",
            "Is Elara still here? Describe a smell in an imagined orchard.", 1) == 1);
    expect("follow-up retains the user scene question after imagined narration fails",
        gateway_post_turn(gateway_port, token, "req-loop-scene-failed-model-followup", questions[3],
            response, sizeof(response)) == 0 && strstr(response, "Elara's presence is still unknown.") &&
            strstr(response, "\"type\":\"completed\"") && !strstr(response, "\"provider_model\""));
    expect("scene conversation retains eight recent messages",
        request_session_get_n(client, "loop-scene", "u-e2e", NULL, 8) == 8);
    expect("another owner cannot obtain scene focus",
        gateway_post_turn_ex(gateway_port, token, "req-loop-scene-other", questions[1],
            "other-owner", "other-owner", 0, NULL, 0, response, sizeof(response)) == 0 &&
        strstr(response, "\"type\":\"failed\"") && !strstr(response, "Mira") && !strstr(response, "Jorin"));
    expect("rejected owner leaves history intact",
        request_session_get_n(client, "loop-scene", "u-e2e", NULL, 8) == 8);
    expect("non-Loop retains configured lab path",
        gateway_post_turn(gateway_port, token, "req-scene-lab-control", "Explain a fictional stone tower.",
            response, sizeof(response)) == 0 && strstr(response, "\"type\":\"failed\""));
    {
        struct pollfd ready = {.fd = listener, .events = POLLIN};
        expect("non-Loop lab control opens HTTP connection", poll(&ready, 1, 100) == 1 && (ready.revents & POLLIN));
        if (ready.revents & POLLIN) {
            connection = accept(listener, NULL, NULL);
            ssize_t count = connection >= 0 ? recv(connection, request, sizeof(request) - 1u, 0) : -1;
            if (count > 0) request[(size_t)count] = '\0';
            expect("control request reaches SystemOne endpoint", count > 0 && strstr(request, "POST /v1/systemone "));
        }
    }
    expect("scene fixture cleanup", request_session_delete(client, "loop-scene", "u-e2e") == 1);
done:
    if (connection >= 0) close(connection);
    if (listener >= 0) close(listener);
    if (client) vbus_close(client);
    expect("Loop scene processes stop cleanly", stop_children() == 0);
    (void)unlink(socket_path); (void)rmdir(directory);
    printf("%s Loop scene focus and lab bypass process gate\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

int main(void) {
    char socket_path[128];
    char tts_url[128];
    char stt_url[128];
    char embed_url[128];
    char search_url[128];
    char llm_url[160];
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    fake_tts server;
    fake_stt stt_server;
    fake_embed embed_server;
    fake_embed search_server;
    fake_llm llm_server;
    pthread_t server_thread;
    pthread_t stt_server_thread;
    pthread_t embed_server_thread;
    pthread_t search_server_thread;
    pthread_t llm_server_thread;
    vbus_client *client = NULL;
    vbus_client *request_client = NULL;
    vbus_client *expiry_client = NULL;
    int listen_fd;
    int stt_listen_fd;
    int embed_listen_fd;
    int search_listen_fd;
    int llm_listen_fd;
    int gateway_port;
    const char *broker_bin;
    const char *rollback_orchestrator;
    int bench_turns = bench_turn_count();
    int bench_concurrency = bench_concurrency_count();
    int bench_tts_frames = bench_tts_frame_count();
    int i;
    char runtime_identity_hash[VOICE_RUNTIME_IDENTITY_HASH_LEN + 1u] = {0};

    if (getenv("SERVICE_E2E_DND_TOOLS")) return dnd_service_e2e();
    if (getenv("SERVICE_E2E_DND_CITATIONS")) return citation_edge_e2e();
    if (getenv("SERVICE_E2E_DND_AUDIO_METADATA")) return prepared_metadata_e2e();
    if (getenv("SERVICE_E2E_SPEECH_MARKUP")) return markup_token_e2e();
    if (getenv("SERVICE_E2E_TTS_WAKEUP")) return tts_wakeup_process_e2e();
    if (getenv("SERVICE_E2E_SEGMENT_BOUND")) return segment_bound_process_e2e();
    if (getenv("SERVICE_E2E_MODEL_CAPTURE")) return model_capture_process_e2e();
    if (getenv("SERVICE_E2E_LOOP_SCENE")) return loop_scene_process_e2e();

    if (load_grounding_fixture() != 0) return 1;
    failures = 0;
    child_count = 0;
    memset(&normal_seen, 0, sizeof(normal_seen));
    memset(&cancel_seen, 0, sizeof(cancel_seen));
    memset(&early_cancel_seen, 0, sizeof(early_cancel_seen));
    memset(&auto_seen, 0, sizeof(auto_seen));
    memset(&llm_seen, 0, sizeof(llm_seen));
    memset(&llm_cancel_seen, 0, sizeof(llm_cancel_seen));
    memset(&llm_late_error_seen, 0, sizeof(llm_late_error_seen));
    memset(&rag_success_seen, 0, sizeof(rag_success_seen));
    memset(&rag_cancel_seen, 0, sizeof(rag_cancel_seen));
    memset(&pipeline_cancel_seen, 0, sizeof(pipeline_cancel_seen));
    memset(&subject_bind_seen, 0, sizeof(subject_bind_seen));
    memset(&fallback_subject_seen, 0, sizeof(fallback_subject_seen));
    memset(&max_derived_subject_seen, 0, sizeof(max_derived_subject_seen));
    memset(&future_stage_seen, 0, sizeof(future_stage_seen));
    memset(&partial_pcm_seen, 0, sizeof(partial_pcm_seen));
    memset(&tts_header_only_seen, 0, sizeof(tts_header_only_seen));
    memset(&tts_truncated_seen, 0, sizeof(tts_truncated_seen));
    memset(&tts_recovered_seen, 0, sizeof(tts_recovered_seen));
    memset(&deferred_single_seen, 0, sizeof(deferred_single_seen));
    memset(&deferred_missing_seen, 0, sizeof(deferred_missing_seen));
    memset(&deferred_bad_marker_seen, 0, sizeof(deferred_bad_marker_seen));
    memset(&deferred_mode_mismatch_seen, 0, sizeof(deferred_mode_mismatch_seen));
    memset(&deferred_orphan_marker_seen, 0, sizeof(deferred_orphan_marker_seen));
    memset(&stt_failure_seen, 0, sizeof(stt_failure_seen));
    memset(&stt_recovered_seen, 0, sizeof(stt_recovered_seen));
    memset(&stt_silence_seen, 0, sizeof(stt_silence_seen));
    memset(&pcm_rejected_seen, 0, sizeof(pcm_rejected_seen));
    memset(&disconnect_seen, 0, sizeof(disconnect_seen));
    memset(&no_tts_seen, 0, sizeof(no_tts_seen));
    memset(&budget_valid_seen, 0, sizeof(budget_valid_seen));
    memset(&budget_invalid_seen, 0, sizeof(budget_invalid_seen));
    memset(&budget_expired_seen, 0, sizeof(budget_expired_seen));
    memset(&token_idle_seen, 0, sizeof(token_idle_seen));
    memset(&token_flush_seen, 0, sizeof(token_flush_seen));
    memset(&llm_bus_stages, 0, sizeof(llm_bus_stages));
    rag_requests_seen = 0;
    lifecycle_started = 0;
    lifecycle_ended = 0;
    transcription_final = 0;
    transcription_schema_clean = 0;
    transcription_stage_clean = 0;
    transcription_failed_lifecycle = 0;
    llm_pcm_ordered = 1;
    llm_pcm_last_segment = -1;
    llm_pcm_segments = 0;
    llm_pcm_final_seen = 0;
    subject_bind_wrong_subject = 0;
    fallback_wrong_subject = 0;
    max_derived_wrong_subject = 0;
    memset(max_derived_request_id, 'r', sizeof(max_derived_request_id) - 1u);
    max_derived_request_id[sizeof(max_derived_request_id) - 1u] = '\0';
    expect(
        "construct maximum derived event subject",
        snprintf(
            max_derived_subject,
            sizeof(max_derived_subject),
            "%s.%s",
            SUBJ_TURN_EVENTS_PFX,
            max_derived_request_id) == (int)(
                sizeof(SUBJ_TURN_EVENTS_PFX) - 1u + 1u +
                sizeof(max_derived_request_id) - 1u));
    expect(
        "service log preserves bounded concurrent line contract",
        service_log_contract() == 0);
    abandoned_audio_started = 0;
    expired_audio_canceled = 0;
    recovered_audio_started = 0;
    refreshed_audio_chunks = 0;
    refreshed_audio_canceled = 0;
    direct_idle_audio_started = 0;
    direct_idle_audio_ended = 0;
    audio_store_rejected_ended = 0;
    audio_store_recovered_started = 0;
    audio_store_recovered_ended = 0;
    oversized_audio_chunk_ended = 0;
    token_idle_failures = 0;
    snprintf(socket_path, sizeof(socket_path), "/tmp/vbus-e2e-%d.sock", (int)getpid());
    setenv("VBUS_PATH", socket_path, 1);
    setenv(
        "VOICE_GATEWAY_TOKEN",
        "test-gateway-token-0123456789abcdef-0123456789abcdef",
        1);
    setenv("POD_NAME", "c-voice-runtime-poc-canary-test", 1);
    setenv("POD_NAMESPACE", "ai-ml", 1);
    setenv("POD_UID", "2f1c93ce-69aa-4a43-817a-42fe9f05d341", 1);
    setenv("TURN_CORE_POLICY_VERSION", "turn-core-v1", 1);
    setenv("GATEWAY_PCM_IDLE_TIMEOUT_MS", "1000", 1);
    setenv("AUDIO_STREAM_IDLE_TIMEOUT_MS", "1500", 1);
    setenv("AUDIO_STORE_MAX_BYTES", "131072", 1);
    setenv("CASCADE_FIRST_SEGMENT_CHARS", "5", 1);
    setenv("CASCADE_EAGER_TTS_SEGMENTS", "1", 1);
    setenv("CASCADE_STREAM_IDLE_TIMEOUT_MS", "1800", 1);
    setenv("TTS_STREAM_FINALITY_TIMEOUT_MS", "2500", 1);
    setenv("SESSION_STORE_MAX_BYTES", "8192", 1);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 16) != 0 ||
        getsockname(listen_fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(listen_fd);
        return 1;
    }
    snprintf(
        tts_url, sizeof(tts_url), "http://127.0.0.1:%u/pcm",
        (unsigned)ntohs(address.sin_port));
    setenv("TTS_HTTP_URL", tts_url, 1);

    stt_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stt_listen_fd < 0) {
        close(listen_fd);
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(stt_listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(stt_listen_fd, 2) != 0 ||
        getsockname(stt_listen_fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    snprintf(
        stt_url, sizeof(stt_url), "http://127.0.0.1:%u",
        (unsigned)ntohs(address.sin_port));
    setenv("STT_BACKEND", "internal-pcm-s16le", 1);
    setenv("STT_BACKEND_URL", stt_url, 1);
    setenv("STT_LANGUAGE", "en", 1);
    setenv("STT_TIMEOUT_MS", "2000", 1);
    setenv("STT_SPEECH_CONTEXT_MS", "200", 1);

    embed_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (embed_listen_fd < 0) {
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(embed_listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(embed_listen_fd, 2) != 0 ||
        getsockname(embed_listen_fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    snprintf(
        embed_url, sizeof(embed_url), "http://127.0.0.1:%u/embeddings",
        (unsigned)ntohs(address.sin_port));
    setenv("EMBED_HTTP_URL", embed_url, 1);
    setenv("RAG_HTTP_TIMEOUT_MS", "2000", 1);
    setenv("EMBEDDING_MODEL_ID", "bge-m3", 1);
    setenv("EMBEDDING_DIMENSIONS", "3", 1);
    setenv("RAG_SHARED_RULEBOOK_COLLECTION", "reviewed_books", 1);
    setenv("RAG_CAMPAIGN_CANON_COLLECTION", "dnd_campaign_canon_v1", 1);
    setenv("MILVUS_HTTP_AUTH_TOKEN", "test-milvus-token", 1);
    search_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_port = 0;
    address_len = sizeof(address);
    if (search_listen_fd < 0 ||
        bind(search_listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(search_listen_fd, 2) != 0 ||
        getsockname(search_listen_fd, (struct sockaddr *)&address, &address_len) != 0) return 1;
    snprintf(search_url, sizeof(search_url), "http://127.0.0.1:%u/v2/vectordb/entities/search",
        (unsigned)ntohs(address.sin_port));
    setenv("MILVUS_SEARCH_URL", search_url, 1);

    llm_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (llm_listen_fd < 0) {
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(llm_listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(llm_listen_fd, 8) != 0 ||
        getsockname(llm_listen_fd, (struct sockaddr *)&address, &address_len) != 0) {
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    snprintf(
        llm_url, sizeof(llm_url),
        "http://127.0.0.1:%u/v1/chat/completions",
        (unsigned)ntohs(address.sin_port));
    setenv("LLM_HTTP_URL", llm_url, 1);
    setenv("LLM_MODEL", "default", 1);
    setenv("LLM_HTTP_TIMEOUT_MS", "3000", 1);
    setenv("LLM_MAX_COMPLETION_TOKENS", "48", 1);
    unsetenv("LLM_GROUNDED_MAX_COMPLETION_TOKENS");
    gateway_port = reserve_loopback_port();
    if (gateway_port <= 0) {
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    {
        char port_text[16];
        snprintf(port_text, sizeof(port_text), "%d", gateway_port);
        setenv("GATEWAY_HEALTH_PORT", port_text, 1);
    }
    memset(&server, 0, sizeof(server));
    server.listen_fd = listen_fd;
    atomic_init(&server.stop, 0);
    atomic_init(&server.accepts, 0);
    atomic_init(&server.requests, 0);
    atomic_init(&server.valid_requests, 0);
    atomic_init(&server.keepalive_requests, 0);
    atomic_init(&server.reused_requests, 0);
    atomic_init(&server.first_tail_sent, 0);
    normal_seen.pcm_sequence_ordered = 1;
    partial_pcm_seen.pcm_sequence_ordered = 1;
    atomic_init(&server.first_llm_accept_at_ns, 0);
    atomic_init(&server.first_llm_request_at_ns, 0);
    atomic_init(&server.first_llm_response_at_ns, 0);
    atomic_init(&server.llm_active_requests, 0);
    atomic_init(&server.llm_max_active_requests, 0);
    atomic_init(&server.llm_provider_requests, 0);
    atomic_init(&server.pipeline_cancel_active_requests, 0);
    atomic_init(&server.pipeline_cancel_max_active_requests, 0);
    atomic_init(&server.late_error_requests, 0);
    atomic_init(&server.late_error_pcm_sent, 0);
    atomic_init(&server.late_error_disconnects, 0);
    atomic_init(&server.cancel_header_sent, 0);
    atomic_init(&server.partial_header_sent, 0);
    atomic_init(&server.escaped_quote_requests, 0);
    atomic_init(&server.escaped_backslash_requests, 0);
    atomic_init(&server.model_markup_requests, 0);
    for (i = 0; i < SERVICE_E2E_BENCH_MAX; ++i) {
        atomic_init(&server.benchmark_request_at_ns[i], 0);
        atomic_init(&server.benchmark_response_at_ns[i], 0);
    }
    server.expected_requests =
        32 + SERVICE_E2E_TTS_ORDER_CAPACITY + bench_turns * 3;
    server.benchmark_pcm_frames = bench_tts_frames;
    memset(&stt_server, 0, sizeof(stt_server));
    stt_server.listen_fd = stt_listen_fd;
    atomic_init(&stt_server.accepts, 0);
    atomic_init(&stt_server.requests, 0);
    atomic_init(&stt_server.valid_requests, 0);
    atomic_init(&stt_server.keepalive_requests, 0);
    atomic_init(&stt_server.first_request_bytes, 0u);
    memset(&embed_server, 0, sizeof(embed_server));
    embed_server.listen_fd = embed_listen_fd;
    atomic_init(&embed_server.accepts, 0);
    atomic_init(&embed_server.requests, 0);
    atomic_init(&embed_server.valid_requests, 0);
    atomic_init(&embed_server.keepalive_requests, 0);
    atomic_init(&embed_server.slow_request_started, 0);
    memset(&search_server, 0, sizeof(search_server));
    search_server.listen_fd = search_listen_fd;
    search_server.search = 1;
    atomic_init(&search_server.accepts, 0);
    atomic_init(&search_server.requests, 0);
    atomic_init(&search_server.valid_requests, 0);
    atomic_init(&search_server.keepalive_requests, 0);
    atomic_init(&search_server.slow_request_started, 0);
    {
        FILE *fixture = fopen("tests/fuzz-corpus/dnd-retrieval/search.json", "rb");
        size_t length;
        if (!fixture) return 1;
        length = fread(search_server.reply, 1u, sizeof(search_server.reply) - 1u, fixture);
        if (!length || !feof(fixture) || ferror(fixture) || fclose(fixture) != 0) return 1;
        search_server.reply[length] = '\0';
    }
    memset(&llm_server, 0, sizeof(llm_server));
    llm_server.listen_fd = llm_listen_fd;
    atomic_init(&llm_server.accepts, 0);
    atomic_init(&llm_server.requests, 0);
    atomic_init(&llm_server.valid_requests, 0);
    atomic_init(&llm_server.keepalive_requests, 0);
    atomic_init(&llm_server.reused_requests, 0);
    atomic_init(&llm_server.grounded_requests, 0);
    atomic_init(&llm_server.policy_connection_at_ns, 0);
    atomic_init(&llm_server.policy_connection_failures, 0);
    atomic_init(&llm_server.first_tail_sent, 0);
    atomic_init(&llm_server.first_accept_at_ns, 0);
    atomic_init(&llm_server.first_request_at_ns, 0);
    atomic_init(&llm_server.first_response_at_ns, 0);
    atomic_init(&llm_server.benchmark_requests, 0);
    atomic_init(&llm_server.benchmark_binding_failures, 0);
    for (i = 0; i < SERVICE_E2E_BENCH_MAX; ++i) {
        atomic_init(&llm_server.benchmark_request_at_ns[i], 0);
        atomic_init(&llm_server.benchmark_response_at_ns[i], 0);
    }
    llm_server.late_error_tts_pcm_sent = &server.late_error_pcm_sent;
    llm_server.expected_requests = 19 + bench_turns;
    if (pthread_create(&server_thread, NULL, fake_tts_thread, &server) != 0) {
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    if (pthread_create(&stt_server_thread, NULL, fake_stt_thread, &stt_server) != 0) {
        pthread_cancel(server_thread);
        pthread_join(server_thread, NULL);
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    if (pthread_create(&llm_server_thread, NULL, fake_llm_thread, &llm_server) != 0) {
        pthread_cancel(server_thread);
        pthread_cancel(stt_server_thread);
        pthread_join(server_thread, NULL);
        pthread_join(stt_server_thread, NULL);
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    if (pthread_create(
            &embed_server_thread, NULL, fake_embed_thread, &embed_server) != 0) {
        pthread_cancel(server_thread);
        pthread_cancel(stt_server_thread);
        pthread_cancel(llm_server_thread);
        pthread_join(server_thread, NULL);
        pthread_join(stt_server_thread, NULL);
        pthread_join(llm_server_thread, NULL);
        close(llm_listen_fd);
        close(embed_listen_fd);
        close(stt_listen_fd);
        close(listen_fd);
        return 1;
    }
    if (pthread_create(&search_server_thread, NULL, fake_embed_thread, &search_server) != 0) {
        pthread_cancel(server_thread);
        pthread_cancel(stt_server_thread);
        pthread_cancel(llm_server_thread);
        pthread_cancel(embed_server_thread);
        pthread_join(server_thread, NULL);
        pthread_join(stt_server_thread, NULL);
        pthread_join(llm_server_thread, NULL);
        pthread_join(embed_server_thread, NULL);
        return 1;
    }

    broker_bin = getenv("BROKER_BIN");
    expect(
        "spawn broker",
        spawn_service(broker_bin && broker_bin[0] ? broker_bin : "./vbus-broker") > 0);
    for (i = 0; i < 100; ++i) {
        client = vbus_connect(socket_path);
        if (client) break;
        {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&delay, NULL);
        }
    }
    expect("connect process broker", client != NULL);
    if (!client) {
        (void)stop_children();
        close(listen_fd);
        close(stt_listen_fd);
        close(embed_listen_fd);
        close(llm_listen_fd);
        close(search_listen_fd);
        pthread_cancel(server_thread);
        pthread_cancel(stt_server_thread);
        pthread_cancel(embed_server_thread);
        pthread_cancel(search_server_thread);
        pthread_cancel(llm_server_thread);
        pthread_join(server_thread, NULL);
        pthread_join(stt_server_thread, NULL);
        pthread_join(embed_server_thread, NULL);
        pthread_join(search_server_thread, NULL);
        pthread_join(llm_server_thread, NULL);
        return 1;
    }
    request_client = vbus_connect(socket_path);
    expect("connect request client", request_client != NULL);
    expiry_client = vbus_connect(socket_path);
    expect("connect expiry observer", expiry_client != NULL);
    rollback_orchestrator = getenv("ORCHESTRATOR_BIN");
    if (rollback_orchestrator && rollback_orchestrator[0])
        expect(
            "spawn rollback orchestrator",
            spawn_service(rollback_orchestrator) > 0);
    expect(
        "spawn cascade",
        spawn_service(
            getenv("CASCADE_ROUTER_BIN") ?
                getenv("CASCADE_ROUTER_BIN") : "./c-cascade-router") > 0);
    expect(
        "spawn tts",
        spawn_service(getenv("TTS_MODULE_BIN") ? getenv("TTS_MODULE_BIN") : "./c-tts-module") > 0);
    expect(
        "spawn rag",
        spawn_service(
            getenv("RAG_GATEWAY_BIN") ?
                getenv("RAG_GATEWAY_BIN") : "./c-rag-gateway") > 0);
    expect(
        "spawn audio",
        spawn_service(
            getenv("AUDIO_PROCESSOR_BIN") ?
                getenv("AUDIO_PROCESSOR_BIN") : "./c-audio-processor") > 0);
    expect(
        "spawn session",
        spawn_service(
            getenv("SESSION_MANAGER_BIN") ?
                getenv("SESSION_MANAGER_BIN") : "./c-session-manager") > 0);
    expect(
        "spawn gateway",
        spawn_service(
            getenv("GATEWAY_BIN") ?
                getenv("GATEWAY_BIN") : "./c-voice-session-gateway") > 0);
    expect(
        "subscribe turn events",
        vbus_subscribe(client, "ai.turn.events.>", NULL, on_turn_event, NULL) == 0);
    expect(
        "subscribe lifecycle",
        vbus_subscribe(client, "ai.voice.transcription.>", NULL, on_transcription, NULL) == 0);
    expect(
        "subscribe request-bound voice reflex events",
        vbus_subscribe(client, "ai.voice.reflex.>", NULL, on_voice_reflex, NULL) == 0);
    expect(
        "subscribe gateway audio stream",
        vbus_subscribe(
            client, "ai.voice.stream.audio-e2e", NULL,
            on_gateway_audio_stream, NULL) == 0);
    expect(
        "subscribe rejected gateway audio stream",
        vbus_subscribe(
            client, "ai.voice.stream.audio-rejected", NULL,
            on_rejected_audio_stream, NULL) == 0);
    expect(
        "subscribe expiring gateway audio streams",
        vbus_subscribe(
            expiry_client, "ai.voice.stream.>", NULL,
            on_expiring_audio_stream, NULL) == 0);
    expect(
        "subscribe turn start stage",
        vbus_subscribe(client, SUBJ_TURN_START, NULL, on_bus_stage, NULL) == 0);
    expect(
        "subscribe turn generate stage",
        vbus_subscribe(client, SUBJ_TURN_GENERATE, NULL, on_bus_stage, NULL) == 0);
    expect(
        "subscribe TTS speak stage",
        vbus_subscribe(client, SUBJ_TURN_TTS_SPEAK, NULL, on_bus_stage, NULL) == 0);
    {
        int settle_ms = startup_settle_ms();
        struct timespec settle = {
            .tv_sec = (time_t)(settle_ms / 1000),
            .tv_nsec = (long)(settle_ms % 1000) * 1000000L,
        };
        nanosleep(&settle, NULL);
    }
    expect("gateway health", check_gateway_health(gateway_port) == 0);
    expect(
        "gateway exposes source-bound runtime identity",
        gateway_runtime_identity(gateway_port, runtime_identity_hash) == 0);
    expect("gateway bounds slow-client capacity", gateway_capacity_is_bounded(gateway_port) == 0);
    expect("gateway recovers after capacity release", check_gateway_health(gateway_port) == 0);
    {
        static uint8_t full_request[GATEWAY_HTTP_RX_CAP];
        static uint8_t exact_header[8192];
        char boundary_request[384];
        uint8_t byte_value_header[128];
        char response[2048];
        size_t boundary_path_len;
        size_t boundary_request_len;
        size_t byte_value_offset;
        static const char request_suffix[] =
            " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char health_request[] =
            "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char exact_header_prefix[] =
            "GET /healthz HTTP/1.1\r\nX-Pad: ";
        static const char unknown_route[] =
            "GET /missing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char identity_query[] =
            "GET " VOICE_RUNTIME_IDENTITY_PATH "?x=1 HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n\r\n";
        static const char identity_post[] =
            "POST " VOICE_RUNTIME_IDENTITY_PATH " HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
        static const char maximum_method[] =
            "ABCDEFGHIJKLMNO /missing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char oversized_method[] =
            "ABCDEFGHIJKLMNOP /missing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char ambiguous_line[] =
            "GET /healthz HTTP/1.1 extra\r\nHost: 127.0.0.1\r\n\r\n";
        static const char leading_space_line[] =
            " GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char repeated_space_line[] =
            "GET  /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char tab_line[] =
            "GET\t/healthz\tHTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        static const char transfer_encoding[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "tRaNsFeR-eNcOdInG: chunked\r\nContent-Length: 1\r\n\r\n0";
        static const char folded_header[] =
            "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n X-Folded: value\r\n\r\n";
        static const char bare_lf_header[] =
            "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\nX-Test: value\r\n\r\n";
        static const char bare_cr_header[] =
            "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\rX-Test: value\r\n\r\n";
        static const char missing_colon_header[] =
            "GET /healthz HTTP/1.1\r\nHost 127.0.0.1\r\n\r\n";
        static const char invalid_name_header[] =
            "GET /healthz HTTP/1.1\r\nBad Name: value\r\n\r\n";
        static const char control_value_header[] =
            "GET /healthz HTTP/1.1\r\nX-Test: value\001\r\n\r\n";
        static const char byte_value_template[] =
            "GET /healthz HTTP/1.1\r\nX-Test: valueA\r\n\r\n";
        static const char duplicate_length[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: 1\r\nContent-Length: 1\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char duplicate_type[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: 1\r\nContent-Type: application/x-protobuf\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char oversized_content_length[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: 11111111111111111111111111111111\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char signed_content_length[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: +1\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char nondecimal_content_length[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: 1x\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char overflowing_timestamp[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "X-Voice-User: u-e2e\r\n"
            "X-Voice-Timestamp: 9223372036854775808\r\n"
            "X-Voice-Nonce: 0123456789abcdef0123456789abcdef\r\n"
            "X-Voice-Signature: 0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef\r\n"
            "Content-Length: 1\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        static const char sign_only_timestamp[] =
            "POST /v1/voice/turns HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "X-Voice-User: u-e2e\r\nX-Voice-Timestamp: +\r\n"
            "X-Voice-Nonce: 0123456789abcdef0123456789abcdef\r\n"
            "X-Voice-Signature: 0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef\r\n"
            "Content-Length: 1\r\n"
            "Content-Type: application/x-protobuf\r\n\r\nx";
        memset(full_request, 'G', sizeof(full_request));
        memcpy(
            exact_header,
            exact_header_prefix,
            sizeof(exact_header_prefix) - 1u);
        memset(
            exact_header + sizeof(exact_header_prefix) - 1u,
            'a',
            sizeof(exact_header) - (sizeof(exact_header_prefix) - 1u) - 4u);
        memcpy(exact_header + sizeof(exact_header) - 4u, "\r\n\r\n", 4u);
        expect(
            "gateway accepts the exact header limit",
            gateway_raw_request(
                gateway_port, exact_header, sizeof(exact_header),
                response, sizeof(response)) == 0 &&
            strstr(response, "200 OK") != NULL);
        for (i = 1; i <= 4; ++i) {
            size_t split = sizeof(health_request) - (size_t)i - 1u;
            expect(
                "gateway accepts a fragmented header terminator",
                gateway_fragmented_request(
                    gateway_port,
                    health_request,
                    sizeof(health_request) - 1u,
                    split,
                    response,
                    sizeof(response)) == 0 &&
                strstr(response, "200 OK") != NULL);
        }
        expect(
            "gateway rejects a full receive buffer",
            gateway_raw_request(
                gateway_port, full_request, sizeof(full_request),
                response, sizeof(response)) == 0 &&
            strstr(response, "431 Request Header Fields Too Large") != NULL);
        expect(
            "gateway reuses a full receive-buffer slot",
            check_gateway_health(gateway_port) == 0);
        expect(
            "gateway rejects ambiguous request lines",
            gateway_raw_request(
                gateway_port, ambiguous_line, sizeof(ambiguous_line) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects leading request-line whitespace",
            gateway_raw_request(
                gateway_port, leading_space_line, sizeof(leading_space_line) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects repeated request-line whitespace",
            gateway_raw_request(
                gateway_port, repeated_space_line, sizeof(repeated_space_line) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects tab request-line whitespace",
            gateway_raw_request(
                gateway_port, tab_line, sizeof(tab_line) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway returns not found for an unknown route",
            gateway_raw_request(
                gateway_port, unknown_route, sizeof(unknown_route) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "404 Not Found") != NULL);
        expect(
            "gateway rejects runtime identity query parameters",
            gateway_raw_request(
                gateway_port, identity_query, sizeof(identity_query) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "404 Not Found") != NULL);
        expect(
            "gateway rejects non-GET runtime identity requests",
            gateway_raw_request(
                gateway_port, identity_post, sizeof(identity_post) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "404 Not Found") != NULL);
        expect(
            "gateway accepts the maximum method length",
            gateway_raw_request(
                gateway_port, maximum_method, sizeof(maximum_method) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "404 Not Found") != NULL);
        expect(
            "gateway rejects an oversized method",
            gateway_raw_request(
                gateway_port, oversized_method, sizeof(oversized_method) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        boundary_path_len = 255u;
        memcpy(boundary_request, "GET /", sizeof("GET /") - 1u);
        memset(
            boundary_request + sizeof("GET /") - 1u, 'a',
            boundary_path_len - 1u);
        boundary_request_len = sizeof("GET ") - 1u + boundary_path_len;
        memcpy(
            boundary_request + boundary_request_len,
            request_suffix, sizeof(request_suffix) - 1u);
        boundary_request_len += sizeof(request_suffix) - 1u;
        expect(
            "gateway accepts the maximum path length",
            gateway_raw_request(
                gateway_port, boundary_request, boundary_request_len,
                response, sizeof(response)) == 0 &&
            strstr(response, "404 Not Found") != NULL);
        boundary_path_len = 256u;
        memcpy(boundary_request, "GET /", sizeof("GET /") - 1u);
        memset(
            boundary_request + sizeof("GET /") - 1u, 'a',
            boundary_path_len - 1u);
        boundary_request_len = sizeof("GET ") - 1u + boundary_path_len;
        memcpy(
            boundary_request + boundary_request_len,
            request_suffix, sizeof(request_suffix) - 1u);
        boundary_request_len += sizeof(request_suffix) - 1u;
        expect(
            "gateway rejects an oversized path",
            gateway_raw_request(
                gateway_port, boundary_request, boundary_request_len,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects transfer encoding ambiguity",
            gateway_raw_request(
                gateway_port, transfer_encoding, sizeof(transfer_encoding) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects folded headers",
            gateway_raw_request(
                gateway_port, folded_header, sizeof(folded_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects bare line feeds",
            gateway_raw_request(
                gateway_port, bare_lf_header, sizeof(bare_lf_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects bare carriage returns",
            gateway_raw_request(
                gateway_port, bare_cr_header, sizeof(bare_cr_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects header lines without a colon",
            gateway_raw_request(
                gateway_port, missing_colon_header,
                sizeof(missing_colon_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects invalid header names",
            gateway_raw_request(
                gateway_port, invalid_name_header,
                sizeof(invalid_name_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects control bytes in header values",
            gateway_raw_request(
                gateway_port, control_value_header,
                sizeof(control_value_header) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        memcpy(
            byte_value_header, byte_value_template,
            sizeof(byte_value_template));
        byte_value_offset = sizeof(
            "GET /healthz HTTP/1.1\r\nX-Test: value") - 1u;
        byte_value_header[byte_value_offset] = (uint8_t)'\t';
        expect(
            "gateway accepts horizontal tabs in header values",
            gateway_raw_request(
                gateway_port, byte_value_header,
                sizeof(byte_value_template) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "200 OK") != NULL);
        byte_value_header[byte_value_offset] = UINT8_C(0x7f);
        expect(
            "gateway rejects delete bytes in header values",
            gateway_raw_request(
                gateway_port, byte_value_header,
                sizeof(byte_value_template) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        byte_value_header[byte_value_offset] = UINT8_C(0x80);
        expect(
            "gateway rejects high bytes in header values",
            gateway_raw_request(
                gateway_port, byte_value_header,
                sizeof(byte_value_template) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects duplicate content lengths",
            gateway_raw_request(
                gateway_port, duplicate_length, sizeof(duplicate_length) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects duplicate content types",
            gateway_raw_request(
                gateway_port, duplicate_type, sizeof(duplicate_type) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "415 Unsupported Media Type") != NULL);
        expect(
            "gateway rejects an oversized content length",
            gateway_raw_request(
                gateway_port, oversized_content_length,
                sizeof(oversized_content_length) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects a signed content length",
            gateway_raw_request(
                gateway_port, signed_content_length,
                sizeof(signed_content_length) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects a nondecimal content length",
            gateway_raw_request(
                gateway_port, nondecimal_content_length,
                sizeof(nondecimal_content_length) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects an overflowing timestamp",
            gateway_raw_request(
                gateway_port, overflowing_timestamp,
                sizeof(overflowing_timestamp) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "401 Unauthorized") != NULL);
        expect(
            "gateway rejects a sign-only timestamp",
            gateway_raw_request(
                gateway_port, sign_only_timestamp,
                sizeof(sign_only_timestamp) - 1u,
                response, sizeof(response)) == 0 &&
            strstr(response, "401 Unauthorized") != NULL);
        expect(
            "gateway accepts bounded alternate header forms",
            gateway_post_cancel_ex(
                gateway_port, "req-alternate-header-forms", 1,
                response, sizeof(response)) == 0 &&
            strstr(response, "200 OK") != NULL &&
            strstr(response, "\"cancelled\":false") != NULL);
        expect(
            "gateway rejects duplicate authentication headers",
            gateway_post_turn_observe_ex(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-duplicate-auth",
                "this must not reach the model",
                "u-e2e",
                "u-e2e",
                0,
                NULL,
                1,
                0,
                0,
                "X-Voice-User: u-e2e\r\n",
                response,
                sizeof(response),
                NULL,
                NULL) == 0 &&
            strstr(response, "401 Unauthorized") != NULL);
        expect(
            "gateway rejects invalid HMAC",
            gateway_post_turn(
                gateway_port,
                "wrong-token-0123456789abcdef-0123456789abcdef",
                "req-unauthorized",
                "this must not reach the model",
                response,
                sizeof(response)) == 0 &&
            strstr(response, "401 Unauthorized") != NULL &&
            strstr(response, "unauthorized") != NULL);
        expect(
            "gateway rejects stale HMAC",
            gateway_post_turn_ex(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-stale", "this must not reach the model", "u-e2e", "u-e2e",
                (int64_t)time(NULL) - 120, NULL, 0, response, sizeof(response)) == 0 &&
            strstr(response, "401 Unauthorized") != NULL);
        expect(
            "gateway rejects body tampering",
            gateway_post_turn_ex(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-tampered", "this must not reach the model", "u-e2e", "u-e2e",
                0, NULL, 1, response, sizeof(response)) == 0 &&
            strstr(response, "401 Unauthorized") != NULL);
        expect(
            "gateway rejects user mismatch",
            gateway_post_turn_ex(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-user-mismatch", "this must not reach the model", "u-other", "u-e2e",
                0, NULL, 0, response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
        expect(
            "gateway rejects invalid UTF-8 input",
            gateway_post_turn(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-invalid-utf8", "\xc0\xaf", response, sizeof(response)) == 0 &&
            strstr(response, "400 Bad Request") != NULL);
    }

    {
        uint8_t wire[256];
        size_t wire_len = pb_encode_turn_cancel(
            wire,
            sizeof(wire),
            "req-early-cancel",
            "u-e2e",
            "client_disconnect");
        int tts_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish cancel before turn start",
            wire_len != 0u &&
            vbus_publish(client, SUBJ_TURN_CANCEL, wire, wire_len) == 0);
        expect(
            "publish turn after its early cancel",
            publish_turn(client, "req-early-cancel") == 0);
        expect(
            "cascade consumes early cancel",
            poll_until(client, &early_cancel_seen.canceled, 1000) == 0 &&
            !early_cancel_seen.final_event && !early_cancel_seen.completed &&
            !early_cancel_seen.failed);
        expect(
            "early cancel suppresses TTS",
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == tts_before &&
            !early_cancel_seen.segment_event && !early_cancel_seen.pcm_started);
    }

    {
        uint64_t started = mono_ns();
        uint64_t first_pcm_ms;
        expect("publish process turn", publish_turn(client, "req-e2e") == 0);
        expect("streaming first PCM before backend completion",
            poll_until(client, &normal_seen.pcm_chunk, 300) == 0);
        first_pcm_ms = (mono_ns() - started) / UINT64_C(1000000);
        expect("streaming first PCM under 300ms", first_pcm_ms < 300);
        expect(
            "streaming does not buffer full TTS response",
            atomic_load_explicit(&server.first_tail_sent, memory_order_relaxed) == 0);
        printf("BenchmarkTTS_IncrementalFirstPCM\t%llu ms\tbackend_tail=500 ms\n",
            (unsigned long long)first_pcm_ms);
    }
    expect("process PCM ended", poll_until(client, &normal_seen.pcm_ended, 2000) == 0);
    expect("process final event", normal_seen.final_event);
    expect("process explicit text channels", normal_seen.clean_channels);
    expect("process TTS segment", normal_seen.segment_event);
    expect("process TTS segment precedes PCM", normal_seen.segment_before_pcm);
    expect("process PCM started", normal_seen.pcm_started);
    expect("process PCM chunk", normal_seen.pcm_chunk);
    expect(
        "process PCM end follows its final chunk",
        normal_seen.pcm_end_after_chunk == 1);
    expect("process PCM fields", normal_seen.audio_fields);
    expect(
        "process PCM buffer ownership preserves bytes and sequence",
        normal_seen.pcm_sequence_ordered &&
        normal_seen.pcm_bytes_valid == normal_seen.pcm_chunk);
    expect(
        "publish maximum request ID without response subject",
        publish_turn_without_response_subject(
            client, max_derived_request_id, "hello") == 0);
    expect(
        "cascade derives exact subject at request ID boundary",
        poll_until(client, &max_derived_subject_seen.completed, 1000) == 0 &&
        max_derived_subject_seen.final_event &&
        !max_derived_subject_seen.failed &&
        !max_derived_wrong_subject);
    expect(
        "process PCM start exposes bounded stages",
        normal_seen.pcm_started_stages.first_text_at_ms > 0 &&
        normal_seen.pcm_started_stages.tts_segment_emitted_at_ms >=
            normal_seen.pcm_started_stages.first_text_at_ms &&
        normal_seen.pcm_started_stages.tts_request_received_at_ms >=
            normal_seen.pcm_started_stages.tts_segment_emitted_at_ms &&
        normal_seen.pcm_started_stages.tts_provider_request_started_at_ms >=
            normal_seen.pcm_started_stages.tts_request_received_at_ms &&
        normal_seen.pcm_started_stages.tts_provider_ready_at_ms >=
            normal_seen.pcm_started_stages.tts_provider_request_started_at_ms &&
        normal_seen.pcm_started_stages.pcm_started_at_ms >=
            normal_seen.pcm_started_stages.tts_provider_ready_at_ms &&
        normal_seen.pcm_started_stages.pcm_first_chunk_at_ms == 0);
    expect(
        "process first PCM exposes ordered stages",
        stages_complete_and_ordered(&normal_seen.first_pcm_stages));
    expect(
        "process later PCM omits repeated stages",
        normal_seen.pcm_chunk > 1 && normal_seen.staged_pcm_chunks == 1);
    if (stages_complete_and_ordered(&normal_seen.first_pcm_stages)) {
        const turn_stage_timestamps_c *stages = &normal_seen.first_pcm_stages;
        printf(
            "BenchmarkTTS_CStages\ttext_to_segment=%lld ms\tdispatch=%lld ms\t"
            "queue=%lld ms\tprovider=%lld ms\t"
            "provider_to_start=%lld ms\tstart_to_first_chunk=%lld ms\n",
            (long long)(stages->tts_segment_emitted_at_ms -
                stages->first_text_at_ms),
            (long long)(stages->tts_request_received_at_ms -
                stages->tts_segment_emitted_at_ms),
            (long long)(stages->tts_provider_request_started_at_ms -
                stages->tts_request_received_at_ms),
            (long long)(stages->tts_provider_ready_at_ms -
                stages->tts_provider_request_started_at_ms),
            (long long)(stages->pcm_started_at_ms - stages->tts_provider_ready_at_ms),
            (long long)(stages->pcm_first_chunk_at_ms - stages->pcm_started_at_ms));
    }
    expect("process turn completed", normal_seen.completed);

    {
        int tts_before = atomic_load_explicit(&server.requests, memory_order_relaxed);
        expect(
            "publish no-TTS turn",
            publish_turn_text_config(client, "req-no-tts", "hello", 0, 0) == 0);
        expect(
            "no-TTS turn completes",
            poll_until(client, &no_tts_seen.completed, 1000) == 0);
        expect("no-TTS turn keeps text", no_tts_seen.final_event && no_tts_seen.clean_channels);
        expect(
            "no-TTS turn suppresses audio",
            !no_tts_seen.segment_event && !no_tts_seen.pcm_started &&
            atomic_load_explicit(&server.requests, memory_order_relaxed) == tts_before);
    }

    if (!rollback_orchestrator || !rollback_orchestrator[0]) {
        expect(
            "publish valid admitted turn",
            publish_turn_text_budget(
                client, "req-budget-valid", "5000", NULL) == 0);
        expect(
            "valid admitted turn completes",
            poll_until(client, &budget_valid_seen.completed, 1000) == 0 &&
            budget_valid_seen.final_event && !budget_valid_seen.failed);
        expect(
            "publish invalid-budget turn",
            publish_turn_text_budget(
                client, "req-budget-invalid", "invalid", NULL) == 0);
        expect(
            "invalid budget fails visibly",
            poll_until(client, &budget_invalid_seen.failed, 1000) == 0 &&
            !budget_invalid_seen.final_event && !budget_invalid_seen.completed);
        expect(
            "publish expired-deadline turn",
            publish_turn_text_budget(
                client, "req-budget-expired", NULL, "1") == 0);
        expect(
            "expired deadline fails visibly",
            poll_until(client, &budget_expired_seen.failed, 1000) == 0 &&
            !budget_expired_seen.final_event && !budget_expired_seen.completed);
    }

    expect("publish cancel turn", publish_turn(client, "req-cancel") == 0);
    expect(
        "cancel turn reached the TTS provider",
        poll_until_atomic(client, &server.cancel_header_sent, 1000) == 0);
    expect("cancel turn has no provider audio", !cancel_seen.pcm_started);
    for (i = 0; i < 50 &&
         atomic_load_explicit(&server.requests, memory_order_relaxed) < 2; ++i) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
        nanosleep(&delay, NULL);
    }
    expect(
        "cancel turn reached backend",
        atomic_load_explicit(&server.requests, memory_order_relaxed) == 2);
    {
        uint8_t wire[256];
        size_t wire_len = pb_encode_turn_cancel(
            wire, sizeof(wire), "req-cancel", "u-e2e", "barge_in");
        uint64_t started = mono_ns();
        uint64_t elapsed_ms;
        expect(
            "publish process cancel",
            wire_len && vbus_publish(client, SUBJ_TURN_CANCEL, wire, wire_len) == 0);
        expect("process cancel observed", poll_until(client, &cancel_seen.canceled, 500) == 0);
        elapsed_ms = (mono_ns() - started) / UINT64_C(1000000);
        expect("cancel latency under 250ms", elapsed_ms < 250);
        expect("cancel suppresses PCM", !cancel_seen.pcm_chunk);
    }

    expect(
        "publish TTS response with a partial final frame",
        publish_tts_segment(
            client, "req-tts-partial", "partial final PCM frame", 0, 1) == 0);
    expect(
        "partial PCM header reaches the TTS module",
        poll_until_atomic(client, &server.partial_header_sent, 1000) == 0);
    expect(
        "PCM start waits for provider audio",
        poll_until(client, &partial_pcm_seen.pcm_started, 250) != 0);
    expect(
        "partial final PCM preserves bytes, order, and finality",
        poll_until(client, &partial_pcm_seen.pcm_ended, 1000) == 0 &&
        partial_pcm_seen.pcm_started && partial_pcm_seen.pcm_chunk == 3 &&
        partial_pcm_seen.pcm_bytes_valid == 3 &&
        partial_pcm_seen.audio_fields == 1 &&
        partial_pcm_seen.pcm_sequence_ordered &&
        partial_pcm_seen.pcm_final_seen == 1 &&
        partial_pcm_seen.pcm_final_sequence == 2 &&
        partial_pcm_seen.pcm_end_after_chunk == 1 &&
        !partial_pcm_seen.failed && !partial_pcm_seen.canceled);

    expect(
        "process first RAG embedding request",
        request_client && request_rag_search(request_client, "rag-e2e-1") == 1);
    expect(
        "process second RAG embedding request",
        request_rag_search(request_client, "rag-e2e-2") == 1);
    expect(
        "process third RAG embedding request after backend close",
        request_rag_search(request_client, "rag-e2e-3") == 1);
    test_rag_request_denials(request_client);
    expect("denied RAG requests never reach either HTTP backend",
        atomic_load_explicit(&embed_server.requests, memory_order_relaxed) == 3 &&
        atomic_load_explicit(&search_server.requests, memory_order_relaxed) == 3);

    expect(
        "subscribe RAG request observer",
        vbus_subscribe(client, SUBJ_RAG_SEARCH, NULL, on_rag_request, NULL) == 0);

    {
        gateway_turn_call call;
        pthread_t turn_thread;
        memset(&call, 0, sizeof(call));
        call.port = gateway_port;
        call.request_id = "req-rag-success";
        call.text = "Look up the source for ancient dragon lore.";
        call.disable_tts = 1;
        call.enable_rag = 1;
        expect(
            "start successful RAG-backed HTTP turn",
            pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
        pthread_join(turn_thread, NULL);
        expect("successful RAG-backed HTTP turn completes", call.result == 0);
        expect(
            "RAG-backed HTTP turn emits one thinking start",
            count_occurrences(call.response, "\"type\":\"thinking_started\"") == 1);
        expect(
            "RAG-backed HTTP turn hides internal context",
            strstr(call.response, "Answer the question using only") == NULL &&
            strstr(call.response, "Sneak Attack can apply once per turn.") == NULL);
        expect(
            "RAG-backed HTTP turn streams model completion",
            strstr(call.response, "\"type\":\"text_completed\"") != NULL &&
            strstr(call.response, "\"type\":\"completed\"") != NULL);
        expect("authenticated HTTP turn publishes typed source and content hashes", public_rag_citation(call.response));
    }
    expect(
        "successful RAG-backed turn completes",
        poll_until(client, &rag_success_seen.completed, 1000) == 0);
    expect(
        "successful RAG-backed turn returns model text",
        rag_success_seen.final_event && rag_success_seen.clean_channels);
    expect(
        "successful RAG-backed turn grounds model prompt",
        atomic_load_explicit(
            &llm_server.grounded_requests, memory_order_relaxed) == 1);

    {
        gateway_turn_call call;
        pthread_t turn_thread;
        char cancel_response[2048];
        uint64_t cancel_started;
        uint64_t cancel_elapsed_ms;
        int llm_requests_before = atomic_load_explicit(
            &llm_server.requests, memory_order_relaxed);
        memset(&call, 0, sizeof(call));
        call.port = gateway_port;
        call.request_id = "req-rag-cancel";
        call.text = "Look up how invisibility changes attack rolls.";
        call.enable_rag = 1;
        expect(
            "start RAG-backed HTTP turn",
            pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
        for (i = 0; i < 100 &&
             atomic_load_explicit(
                 &embed_server.slow_request_started, memory_order_relaxed) == 0; ++i)
            (void)vbus_poll(client, 10);
        expect(
            "RAG-backed turn reaches stalled retrieval",
            atomic_load_explicit(
                &embed_server.slow_request_started, memory_order_relaxed) == 1);
        cancel_started = mono_ns();
        expect(
            "cancel stalled RAG turn",
            gateway_post_cancel(
                gateway_port,
                call.request_id,
                cancel_response,
                sizeof(cancel_response)) == 0 &&
            strstr(cancel_response, "200 OK") != NULL &&
            strstr(cancel_response, "\"cancelled\":true") != NULL);
        expect(
            "stalled RAG cancellation reaches cascade",
            poll_until(client, &rag_cancel_seen.canceled, 500) == 0);
        cancel_elapsed_ms = (mono_ns() - cancel_started) / UINT64_C(1000000);
        expect("stalled RAG cancel latency under 250ms", cancel_elapsed_ms < 250);
        expect(
            "stalled RAG cancellation suppresses model dispatch",
            atomic_load_explicit(&llm_server.requests, memory_order_relaxed) ==
                llm_requests_before);
        pthread_join(turn_thread, NULL);
        expect("canceled RAG HTTP stream closes cleanly", call.result == 0);
        expect(
            "canceled RAG HTTP stream has terminal",
            strstr(call.response, "\"type\":\"canceled\"") != NULL);
        printf(
            "BenchmarkRAG_CancelMs\t%llu ms\tbackend_stall=1000 ms\n",
            (unsigned long long)cancel_elapsed_ms);
    }

    {
        static const char subject[] = "ai.voice.pcm.audio-e2e";
        size_t silence_frame;
        int silence_published = 1;
        struct timespec speech_wait = {
            .tv_sec = 0,
            .tv_nsec = 200000000L,
        };
        struct timespec endpoint_wait = {
            .tv_sec = 0,
            .tv_nsec = 850000000L,
        };
        expect(
            "publish gateway PCM speech",
            publish_gateway_audio_chunk(client, subject) == 0);
        nanosleep(&speech_wait, NULL);
        for (silence_frame = 0u;
             silence_frame < GATEWAY_AUDIO_SILENCE_FRAMES - 1u;
             ++silence_frame) {
            if (publish_gateway_silence_chunk(client, subject) != 0) {
                silence_published = 0;
                break;
            }
        }
        expect("publish gateway PCM silence start", silence_published);
        nanosleep(&endpoint_wait, NULL);
        expect(
            "publish gateway PCM endpoint silence",
            publish_gateway_silence_chunk(client, subject) == 0);
        expect(
            "gateway VAD endpoint ends PCM stream",
            poll_until(client, &gateway_audio_seen.ends, 1000) == 0);
        expect("process lifecycle ended", poll_until(client, &lifecycle_ended, 1000) == 0);
        expect("process lifecycle started", lifecycle_started);
        expect("process final STT", poll_until(client, &transcription_final, 3000) == 0);
        expect("process canonical STT fields", transcription_schema_clean);
        expect("process ordered STT stage fields", transcription_stage_clean);
        expect(
            "process canonical STT HTTP request",
            atomic_load_explicit(&stt_server.requests, memory_order_relaxed) == 1 &&
            atomic_load_explicit(&stt_server.valid_requests, memory_order_relaxed) == 1);
        expect(
            "audio processor keeps 200ms STT speech context",
            atomic_load_explicit(
                &stt_server.first_request_bytes,
                memory_order_relaxed) == 640u + 200u * 32u);
        {
            const size_t captured_bytes = GATEWAY_AUDIO_TOTAL_FRAMES * 640u;
            const size_t stt_bytes = atomic_load_explicit(
                &stt_server.first_request_bytes, memory_order_relaxed);
            expect("STT fixture removes silence bytes", stt_bytes < captured_bytes);
            printf(
                "BenchmarkSTTSpeechWindowFixture\t"
                "captured_bytes=%zu\tstt_bytes=%zu\t"
                "removed_ms=%zu\tpayload_reduction=%.2f%%\n",
                captured_bytes, stt_bytes,
                stt_bytes <= captured_bytes ? (captured_bytes - stt_bytes) / 32u : 0u,
                stt_bytes <= captured_bytes
                    ? 100.0 * (double)(captured_bytes - stt_bytes) /
                        (double)captured_bytes
                    : 0.0);
        }
        expect(
            "publish PCM after early transcript",
            publish_gateway_audio_chunk(client, subject) == 0);
        expect(
            "publish authoritative PCM end after early transcript",
            vbus_publish(client, subject, NULL, 0u) == 0);
        (void)vbus_poll(client, 200);
        expect(
            "gateway drains PCM after early transcript",
            gateway_audio_seen.starts == 1 &&
            gateway_audio_seen.chunks == GATEWAY_AUDIO_TOTAL_FRAMES &&
            gateway_audio_seen.ends == 1 && gateway_audio_seen.ordered &&
            gateway_audio_seen.valid_pcm &&
            atomic_load_explicit(
                &stt_server.requests, memory_order_relaxed) == 1);
        expect("audio autostart PCM ended", poll_until(client, &auto_seen.pcm_ended, 3000) == 0);
        expect("audio autostart text channels", auto_seen.clean_channels);
        expect("audio autostart PCM fields", auto_seen.audio_fields);
        expect(
            "audio autostart exposes complete input waterfall",
            input_stages_complete_and_ordered(&auto_seen.first_pcm_stages.input) &&
            auto_seen.first_pcm_stages.first_text_at_ms >=
                auto_seen.first_pcm_stages.input.stt_transcript_published_at_ms &&
            stages_complete_and_ordered(&auto_seen.first_pcm_stages));
        expect("audio autostart completed", auto_seen.completed);
    }

    {
        memset(&auto_seen, 0, sizeof(auto_seen));
        expect(
            "publish second audio turn",
            publish_audio_turn(client, "ai.voice.stream.audio-e2e") == 0);
        expect(
            "process second lifecycle ended",
            poll_until_count(client, &lifecycle_ended, 2, 1000) == 0);
        expect("process second lifecycle started", lifecycle_started == 2);
        expect(
            "process second final STT",
            poll_until_count(client, &transcription_final, 2, 3000) == 0);
        expect("process second canonical STT fields", transcription_schema_clean == 2);
        expect(
            "process two canonical STT HTTP requests",
            atomic_load_explicit(&stt_server.requests, memory_order_relaxed) == 2 &&
            atomic_load_explicit(&stt_server.valid_requests, memory_order_relaxed) == 2);
        expect(
            "second audio autostart PCM ended",
            poll_until(client, &auto_seen.pcm_ended, 3000) == 0);
        expect("second audio autostart completed", auto_seen.completed);
    }

    {
        memset(&auto_seen, 0, sizeof(auto_seen));
        expect(
            "publish third audio turn after backend close",
            publish_audio_turn(client, "ai.voice.stream.audio-e2e") == 0);
        expect(
            "process third lifecycle ended",
            poll_until_count(client, &lifecycle_ended, 3, 1000) == 0);
        expect(
            "process third final STT",
            poll_until_count(client, &transcription_final, 3, 3000) == 0);
        expect(
            "STT reconnect preserves canonical response",
            transcription_schema_clean == 3 &&
            atomic_load_explicit(&stt_server.requests, memory_order_relaxed) == 3 &&
            atomic_load_explicit(&stt_server.valid_requests, memory_order_relaxed) == 3);
        expect(
            "third audio autostart completes",
            poll_until(client, &auto_seen.pcm_ended, 3000) == 0 &&
            poll_until(client, &auto_seen.completed, 1000) == 0);
    }

    {
        static const char session_id[] = "audio-stt-malformed";
        char subject[192];
        int subject_len = snprintf(
            subject, sizeof(subject), "%s.%s", SUBJ_VOICE_PCM_PFX, session_id);
        int transcripts_before = transcription_schema_clean;
        int tts_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "prepare authenticated audio turn for malformed STT",
            subject_len > 0 && (size_t)subject_len < sizeof(subject) &&
            publish_audio_prepare(
                client, "req-stt-malformed", session_id) == 0);
        expect(
            "publish authenticated audio for malformed STT",
            publish_gateway_audio_turn(client, subject) == 0);
        expect(
            "malformed STT response emits failure lifecycle",
            poll_until(client, &transcription_failed_lifecycle, 1000) == 0);
        expect(
            "malformed STT response fails pending public turn",
            poll_until(client, &stt_failure_seen.failed, 1000) == 0 &&
            !stt_failure_seen.completed && !stt_failure_seen.canceled);
        expect(
            "malformed STT response suppresses transcript and TTS",
            transcription_schema_clean == transcripts_before &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == tts_before);
    }

    {
        static const char session_id[] = "audio-stt-recovered";
        char subject[192];
        int subject_len = snprintf(
            subject, sizeof(subject), "%s.%s", SUBJ_VOICE_PCM_PFX, session_id);
        int transcripts_before = transcription_schema_clean;
        expect(
            "prepare authenticated audio turn after malformed STT",
            subject_len > 0 && (size_t)subject_len < sizeof(subject) &&
            publish_audio_prepare(
                client, "req-stt-recovered", session_id) == 0);
        expect(
            "publish authenticated audio after malformed STT",
            publish_gateway_audio_turn(client, subject) == 0);
        expect(
            "voice endpoint reflex carries the active request id",
            poll_until(client, &endpoint_reflex_request_bound, 1000) == 0);
        expect(
            "STT worker recovers after malformed response",
            poll_until_count(
                client, &transcription_schema_clean, transcripts_before + 1, 3000) == 0);
        expect(
            "recovered STT turn completes through TTS",
            poll_until(client, &stt_recovered_seen.pcm_ended, 3000) == 0 &&
            poll_until(client, &stt_recovered_seen.completed, 1000) == 0 &&
            !stt_recovered_seen.failed &&
            !stt_recovered_seen.canceled && stt_failure_seen.failed &&
            !stt_failure_seen.completed && !stt_failure_seen.canceled);
    }

    {
        static const char subject[] = "ai.voice.stream.audio-e2e";
        int started_before = lifecycle_started;
        int ended_before = lifecycle_ended;
        int transcripts_before = transcription_schema_clean;
        int failures_before = transcription_failed_lifecycle;
        int stt_before = atomic_load_explicit(
            &stt_server.requests, memory_order_relaxed);
        memset(&auto_seen, 0, sizeof(auto_seen));
        expect(
            "publish audio generation that will be superseded",
            publish_audio_turn(client, subject) == 0);
        for (i = 0; i < 100 &&
             atomic_load_explicit(
                 &stt_server.requests, memory_order_relaxed) < stt_before + 1; ++i)
            (void)vbus_poll(client, 10);
        expect(
            "superseded generation reaches stalled STT",
            atomic_load_explicit(
                &stt_server.requests, memory_order_relaxed) == stt_before + 1);
        expect(
            "publish replacement audio generation",
            publish_audio_turn(client, subject) == 0);
        expect(
            "both audio generations close their streams",
            poll_until_count(
                client, &lifecycle_started, started_before + 2, 1000) == 0 &&
            poll_until_count(
                client, &lifecycle_ended, ended_before + 2, 1000) == 0);
        expect(
            "replacement generation publishes one transcript",
            poll_until_count(
                client, &transcription_schema_clean,
                transcripts_before + 1, 3000) == 0 &&
            transcription_schema_clean == transcripts_before + 1);
        expect(
            "superseded generation stays silent",
            transcription_failed_lifecycle == failures_before);
        expect(
            "replacement generation completes through TTS",
            poll_until(client, &auto_seen.pcm_ended, 3000) == 0 &&
            poll_until(client, &auto_seen.completed, 1000) == 0);
    }

    {
        static const char session_id[] = "audio-stt-silence";
        char subject[192];
        int subject_len = snprintf(
            subject, sizeof(subject), "%s.%s", SUBJ_VOICE_PCM_PFX, session_id);
        int failures_before = transcription_failed_lifecycle;
        int stt_before = atomic_load_explicit(
            &stt_server.requests, memory_order_relaxed);
        int tts_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "prepare authenticated silent audio turn",
            subject_len > 0 && (size_t)subject_len < sizeof(subject) &&
            publish_audio_prepare(client, "req-stt-silence", session_id) == 0);
        expect(
            "publish authenticated silent audio turn",
            publish_gateway_silence(client, subject) == 0);
        expect(
            "silent utterance emits failure lifecycle",
            poll_until_count(
                client, &transcription_failed_lifecycle,
                failures_before + 1, 1000) == 0);
        expect(
            "silent utterance fails pending public turn",
            poll_until(client, &stt_silence_seen.failed, 1000) == 0 &&
            !stt_silence_seen.completed && !stt_silence_seen.canceled);
        expect(
            "silent utterance suppresses STT and TTS",
            atomic_load_explicit(
                &stt_server.requests, memory_order_relaxed) == stt_before &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == tts_before);
    }

    {
        static const char session_id[] = "audio-pcm-rejected";
        char subject[192];
        int subject_len = snprintf(
            subject, sizeof(subject), "%s.%s", SUBJ_VOICE_PCM_PFX, session_id);
        int stt_before = atomic_load_explicit(
            &stt_server.requests, memory_order_relaxed);
        int tts_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "prepare authenticated malformed PCM turn",
            subject_len > 0 && (size_t)subject_len < sizeof(subject) &&
            publish_audio_prepare(client, "req-pcm-rejected", session_id) == 0);
        expect(
            "publish authenticated malformed PCM turn",
            publish_rejected_gateway_audio(client, subject) == 0);
        expect(
            "malformed PCM fails pending public turn",
            poll_until(client, &pcm_rejected_seen.failed, 1000) == 0 &&
            !pcm_rejected_seen.completed && !pcm_rejected_seen.canceled);
        expect(
            "voice rejection reflex carries the active request id",
            poll_until(client, &rejected_reflex_request_bound, 1000) == 0);
        expect(
            "malformed PCM suppresses STT and TTS",
            atomic_load_explicit(
                &stt_server.requests, memory_order_relaxed) == stt_before &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == tts_before);
    }

    {
        int ended_before = lifecycle_ended;
        int stt_before = atomic_load_explicit(&stt_server.requests, memory_order_relaxed);
        expect(
            "publish malformed gateway PCM turn",
            publish_rejected_gateway_audio(
                client, "ai.voice.pcm.audio-rejected") == 0);
        expect(
            "malformed gateway PCM cancels stream",
            poll_until(client, &rejected_audio_canceled, 1000) == 0);
        expect(
            "audio processor closes rejected stream",
            poll_until_count(client, &lifecycle_ended, ended_before + 1, 1000) == 0);
        expect(
            "rejected PCM never reaches STT",
            atomic_load_explicit(&stt_server.requests, memory_order_relaxed) == stt_before);
    }

    {
        static const char first[] = "ai.voice.stream.audio-store-first";
        static const char second[] = "ai.voice.stream.audio-store-second";
        static const char rejected[] = "ai.voice.stream.audio-store-rejected";
        static const char recovered[] = "ai.voice.stream.audio-store-recovered";
        expect(
            "start first aggregate audio allocation",
            publish_audio_stream_marker(client, first, "start") == 0);
        expect(
            "fill first aggregate audio allocation",
            publish_audio_stream_chunk(client, first, 65536u) == 0);
        expect(
            "start second aggregate audio allocation",
            publish_audio_stream_marker(client, second, "start") == 0);
        expect(
            "fill aggregate audio capacity",
            publish_audio_stream_chunk(client, second, 65536u) == 0);
        expect(
            "start audio stream beyond aggregate capacity",
            publish_audio_stream_marker(client, rejected, "start") == 0);
        expect(
            "publish audio beyond aggregate capacity",
            publish_audio_stream_chunk(client, rejected, 640u) == 0);
        expect(
            "aggregate audio capacity rejects one stream",
            poll_until(client, &audio_store_rejected_ended, 1000) == 0);
        expect(
            "release aggregate audio capacity",
            publish_audio_stream_marker(client, first, "cancel") == 0);
        expect(
            "start audio after aggregate capacity release",
            publish_audio_stream_marker(client, recovered, "start") == 0);
        expect(
            "reuse released aggregate audio capacity",
            publish_audio_stream_chunk(client, recovered, 65536u) == 0);
        expect(
            "audio recovery stream starts",
            poll_until(client, &audio_store_recovered_started, 1000) == 0);
        (void)vbus_poll(client, 100);
        expect(
            "recovered audio remains admitted after its chunk",
            audio_store_recovered_ended == 0);
        expect(
            "cancel recovered aggregate audio stream",
            publish_audio_stream_marker(client, recovered, "cancel") == 0);
        expect(
            "recovered aggregate audio stream ends",
            poll_until(client, &audio_store_recovered_ended, 1000) == 0);
        expect(
            "release second aggregate audio allocation",
            publish_audio_stream_marker(client, second, "cancel") == 0);
    }

    {
        static const char subject[] =
            "ai.voice.stream.audio-chunk-oversized";
        expect(
            "start oversized audio chunk stream",
            publish_audio_stream_marker(client, subject, "start") == 0);
        expect(
            "publish oversized audio chunk",
            publish_audio_stream_chunk(client, subject, 65538u) == 0);
        expect(
            "audio processor rejects oversized chunk",
            poll_until(client, &oversized_audio_chunk_ended, 1000) == 0);
    }

    {
        /* Both IDs map to session bucket 379 and generation bucket 891. */
        static const char first[] =
            "ai.voice.stream.audio-index-collision-00085";
        static const char second[] =
            "ai.voice.stream.audio-index-collision-00210";
        int started_before = lifecycle_started;
        int ended_before = lifecycle_ended;
        expect(
            "start first colliding audio session",
            publish_audio_stream_marker(client, first, "start") == 0);
        expect(
            "start second colliding audio session",
            publish_audio_stream_marker(client, second, "start") == 0);
        expect(
            "audio index admits both colliding sessions",
            poll_until_count(
                client, &lifecycle_started, started_before + 2, 1000) == 0);
        expect(
            "drop head of audio collision chain",
            publish_audio_stream_marker(client, second, "cancel") == 0);
        expect(
            "audio index removes collision-chain head",
            poll_until_count(
                client, &lifecycle_ended, ended_before + 1, 1000) == 0);
        expect(
            "drop surviving colliding audio session",
            publish_audio_stream_marker(client, first, "cancel") == 0);
        expect(
            "audio index preserves collision-chain survivor",
            poll_until_count(
                client, &lifecycle_ended, ended_before + 2, 1000) == 0);
        expect(
            "restart removed colliding audio session",
            publish_audio_stream_marker(client, second, "start") == 0);
        expect(
            "audio index reuses removed session slot",
            poll_until_count(
                client, &lifecycle_started, started_before + 3, 1000) == 0);
        expect(
            "cancel restarted colliding audio session",
            publish_audio_stream_marker(client, second, "cancel") == 0);
        expect(
            "restarted colliding audio session ends",
            poll_until_count(
                client, &lifecycle_ended, ended_before + 3, 1000) == 0);
    }

    {
        static const char prefix[] = "ai.voice.stream.";
        char subject[sizeof(prefix) - 1u + 127u + 1u];
        int started_before = lifecycle_started;
        int ended_before = lifecycle_ended;
        memcpy(subject, prefix, sizeof(prefix) - 1u);
        memset(subject + sizeof(prefix) - 1u, 'm', 127u);
        subject[sizeof(subject) - 1u] = '\0';
        expect(
            "start maximum-length audio session key",
            publish_audio_stream_marker(client, subject, "start") == 0);
        expect(
            "audio index admits maximum-length session key",
            poll_until_count(
                client, &lifecycle_started, started_before + 1, 1000) == 0);
        expect(
            "cancel maximum-length audio session key",
            publish_audio_stream_marker(client, subject, "cancel") == 0);
        expect(
            "audio index removes maximum-length session key",
            poll_until_count(
                client, &lifecycle_ended, ended_before + 1, 1000) == 0);
    }

    {
        char subject[128];
        int published = 1;
        for (i = 0; i < 64; ++i) {
            int written = snprintf(
                subject, sizeof(subject),
                "ai.voice.pcm.audio-abandoned-%03d", i);
            if (written <= 0 || (size_t)written >= sizeof(subject) ||
                publish_gateway_audio_chunk(client, subject) != 0) {
                published = 0;
                break;
            }
        }
        expect("publish abandoned PCM sessions", published);
        expect(
            "gateway fills bounded PCM session table",
            poll_until_count(expiry_client, &abandoned_audio_started, 64, 1500) == 0);
        expect(
            "gateway proactively cancels expired downstream streams",
            poll_until_count(expiry_client, &expired_audio_canceled, 64, 2500) == 0);
        expect(
            "publish PCM after idle session timeout",
            publish_gateway_audio_chunk(
                client, "ai.voice.pcm.audio-recovered") == 0);
        expect(
            "gateway recovers PCM capacity after idle timeout",
            poll_until(expiry_client, &recovered_audio_started, 2000) == 0);
        expect(
            "gateway cancels every expired downstream stream",
            expired_audio_canceled == 64);
    }

    {
        struct timespec active_wait = {.tv_sec = 0, .tv_nsec = 700000000L};
        struct timespec expire_wait = {.tv_sec = 0, .tv_nsec = 400000000L};
        expect(
            "publish first active PCM chunk",
            publish_gateway_audio_chunk(
                client, "ai.voice.pcm.audio-refreshed") == 0);
        expect(
            "gateway forwards first active PCM chunk",
            poll_until_count(expiry_client, &refreshed_audio_chunks, 1, 1000) == 0);
        nanosleep(&active_wait, NULL);
        expect(
            "publish PCM chunk before idle deadline",
            publish_gateway_audio_chunk(
                client, "ai.voice.pcm.audio-refreshed") == 0);
        expect(
            "gateway forwards refreshed PCM chunk",
            poll_until_count(expiry_client, &refreshed_audio_chunks, 2, 1000) == 0);
        nanosleep(&active_wait, NULL);
        (void)vbus_poll(expiry_client, 0);
        expect("active PCM refreshes idle deadline", refreshed_audio_canceled == 0);
        nanosleep(&expire_wait, NULL);
        expect(
            "refreshed PCM expires after later idle deadline",
            poll_until(expiry_client, &refreshed_audio_canceled, 1000) == 0);
    }

    {
        static const char subject[] = "ai.voice.stream.audio-idle-direct";
        expect(
            "publish abandoned direct audio start",
            publish_audio_stream_marker(client, subject, "start") == 0);
        expect(
            "audio processor admits direct stream",
            poll_until(client, &direct_idle_audio_started, 1000) == 0);
        expect(
            "audio processor expires idle direct stream",
            poll_until(client, &direct_idle_audio_ended, 2500) == 0);
        expect(
            "publish direct audio after idle expiry",
            publish_audio_stream_marker(client, subject, "start") == 0);
        expect(
            "audio processor reuses expired stream capacity",
            poll_until_count(client, &direct_idle_audio_started, 2, 1000) == 0);
        expect(
            "cancel recovered direct audio stream",
            publish_audio_stream_marker(client, subject, "cancel") == 0);
        expect(
            "recovered direct audio stream ends",
            poll_until_count(client, &direct_idle_audio_ended, 2, 1000) == 0);
    }

    {
        struct timespec flush_wait = {.tv_sec = 1, .tv_nsec = 500000000L};
        expect(
            "publish partial external token stream",
            publish_token_start(client, "req-token-flush", "partial") == 0);
        nanosleep(&flush_wait, NULL);
        expect(
            "publish token after segment idle deadline",
            publish_token_chunk(client, "req-token-flush", " more") == 0);
        expect(
            "cascade flushes segment before stream expiry",
            poll_until(client, &token_flush_seen.segment_event, 1000) == 0);
        expect(
            "finish idle-flushed external token stream",
            publish_token_chunk(client, "req-token-flush", "") == 0);
        expect(
            "idle-flushed external token stream completes",
            poll_until(client, &token_flush_seen.completed, 1000) == 0);
    }

    {
        expect(
            "publish abandoned external token stream",
            publish_token_chunk(client, "req-token-idle", "partial") == 0);
        expect(
            "cascade expires idle external token stream",
            poll_until(client, &token_idle_failures, 2500) == 0);
        expect(
            "publish external token stream after idle expiry",
            publish_token_chunk(client, "req-token-idle", "retry") == 0);
        expect(
            "cascade reuses expired token stream capacity",
            poll_until_count(client, &token_idle_failures, 2, 2500) == 0);
        expect(
            "expired token streams suppress deferred TTS completion",
            !token_idle_seen.pcm_ended && !token_idle_seen.completed &&
            !token_idle_seen.canceled);
    }

    {
        gateway_turn_call call;
        pthread_t turn_thread;
        uint64_t started = mono_ns();
        uint64_t first_pcm_ms;
        int rag_requests_before = rag_requests_seen;
        memset(&call, 0, sizeof(call));
        call.port = gateway_port;
        call.request_id = "req-llm";
        call.text = "Look up how invisibility changes attack rolls.";
        call.nonce = "0123456789abcdef0123456789abcdef";
        expect(
            "start authenticated HTTP model turn",
            pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
        expect(
            "capture private HTTP capability subject",
            poll_until(client, &llm_bus_stages.response_subject_seen, 1000) == 0);
        expect("authenticated gateway preserves product metadata on VBus",
               llm_bus_stages.product_metadata_seen);
        {
            uint8_t forged[512];
            char near_match[sizeof(llm_bus_stages.response_subject)];
            size_t subject_len = strnlen(
                llm_bus_stages.response_subject,
                sizeof(llm_bus_stages.response_subject));
            size_t forged_len = pb_encode_turn_event(
                forged, sizeof(forged), "req-llm", "completed", "");
            expect(
                "publish guessed-subject forged terminal",
                forged_len != 0 &&
                vbus_publish(client, "ai.turn.events.req-llm", forged, forged_len) == 0);
            memcpy(near_match, llm_bus_stages.response_subject, subject_len + 1u);
            if (subject_len != 0u)
                near_match[subject_len - 1u] =
                    near_match[subject_len - 1u] == '0' ? '1' : '0';
            expect(
                "publish near-match capability forged terminal",
                forged_len != 0 && subject_len != 0u &&
                vbus_publish(client, near_match, forged, forged_len) == 0);
            expect(
                "publish out-of-range capability slot",
                forged_len != 0 &&
                vbus_publish(
                    client,
                    "ai.turn.events.cgh.ff.00000000000000000000000000000000",
                    forged, forged_len) == 0);
            expect(
                "publish invalid capability slot",
                forged_len != 0 &&
                vbus_publish(
                    client,
                    "ai.turn.events.cgh.zz.00000000000000000000000000000000",
                    forged, forged_len) == 0);
            expect(
                "publish malformed capability lengths",
                forged_len != 0 &&
                vbus_publish(
                    client,
                    "ai.turn.events.cgh.00.0000000000000000000000000000000",
                    forged, forged_len) == 0 &&
                vbus_publish(
                    client,
                    "ai.turn.events.cgh.00.000000000000000000000000000000000",
                    forged, forged_len) == 0);
        }
        expect(
            "SSE model dispatches TTS before completion",
            poll_until(client, &llm_seen.pcm_started, 300) == 0);
        first_pcm_ms = (mono_ns() - started) / UINT64_C(1000000);
        expect("SSE first PCM under 300ms", first_pcm_ms < 300);
        expect(
            "SSE uses configured eager first segment without claiming finality",
            llm_bus_stages.first_tts_segment_bytes ==
                sizeof("The ancient") - 1u &&
            !llm_bus_stages.first_tts_segment_final &&
            llm_bus_stages.first_tts_segment_deferred);
        expect(
            "SSE model response is not fully buffered",
            atomic_load_explicit(
                &llm_server.first_tail_sent, memory_order_relaxed) == 0);
        printf(
            "BenchmarkLLM_SSEToFirstPCM\t%llu ms\tbackend_tail=500 ms\n",
            (unsigned long long)first_pcm_ms);
        pthread_join(turn_thread, NULL);
        expect("authenticated HTTP model turn completes", call.result == 0);
        expect(
            "authenticated HTTP client observes first PCM",
            call.first_pcm_at_ns >= started);
        if (call.first_pcm_at_ns >= started) {
            uint64_t client_first_pcm_us =
                (call.first_pcm_at_ns - started) / UINT64_C(1000);
            printf(
                "BenchmarkHTTP_ClientFirstPCM\t%llu us\n",
                (unsigned long long)client_first_pcm_us);
        }
        {
            uint64_t llm_accept_at_ns = (uint64_t)atomic_load_explicit(
                &llm_server.first_accept_at_ns, memory_order_relaxed);
            uint64_t llm_request_at_ns = (uint64_t)atomic_load_explicit(
                &llm_server.first_request_at_ns, memory_order_relaxed);
            uint64_t llm_response_at_ns = (uint64_t)atomic_load_explicit(
                &llm_server.first_response_at_ns, memory_order_relaxed);
            uint64_t tts_accept_at_ns = (uint64_t)atomic_load_explicit(
                &server.first_llm_accept_at_ns, memory_order_relaxed);
            uint64_t tts_request_at_ns = (uint64_t)atomic_load_explicit(
                &server.first_llm_request_at_ns, memory_order_relaxed);
            uint64_t tts_response_at_ns = (uint64_t)atomic_load_explicit(
                &server.first_llm_response_at_ns, memory_order_relaxed);
            int ordered = started <= call.request_started_at_ns &&
                llm_accept_at_ns <= llm_request_at_ns &&
                call.request_started_at_ns <= llm_request_at_ns &&
                llm_request_at_ns <= llm_response_at_ns &&
                llm_response_at_ns <= tts_request_at_ns &&
                tts_accept_at_ns <= tts_request_at_ns &&
                tts_request_at_ns <= tts_response_at_ns &&
                tts_response_at_ns <= call.first_pcm_at_ns;
            expect("microsecond HTTP waterfall is ordered", ordered);
            if (ordered) {
                printf(
                    "BenchmarkHTTP_StageUs\t"
                    "client_to_request_write=%llu\t"
                    "request_write_to_llm_request=%llu\t"
                    "llm_request_to_first_body=%llu\t"
                    "llm_body_to_tts_request=%llu\t"
                    "tts_request_to_first_body=%llu\t"
                    "tts_body_to_client_pcm=%llu\n",
                    (unsigned long long)((call.request_started_at_ns - started) /
                        UINT64_C(1000)),
                    (unsigned long long)((llm_request_at_ns -
                        call.request_started_at_ns) /
                        UINT64_C(1000)),
                    (unsigned long long)((llm_response_at_ns - llm_request_at_ns) /
                        UINT64_C(1000)),
                    (unsigned long long)((tts_request_at_ns - llm_response_at_ns) /
                        UINT64_C(1000)),
                    (unsigned long long)((tts_response_at_ns - tts_request_at_ns) /
                        UINT64_C(1000)),
                        (unsigned long long)((call.first_pcm_at_ns - tts_response_at_ns) /
                            UINT64_C(1000)));
            }
            expect(
                "microsecond VBus stage events are observed",
                llm_bus_stages.turn_start_at_ns != 0 &&
                llm_bus_stages.tts_speak_at_ns != 0 &&
                ((!rollback_orchestrator || !rollback_orchestrator[0]) ||
                 llm_bus_stages.turn_generate_at_ns != 0));
            /* This subscriber is an independent observer. Scheduler order can
             * place its callback after the backend has accepted the request. */
            if (rollback_orchestrator && rollback_orchestrator[0]) {
                ordered = call.request_started_at_ns <=
                        llm_bus_stages.turn_start_at_ns &&
                    llm_bus_stages.turn_start_at_ns <=
                        llm_bus_stages.turn_generate_at_ns &&
                    llm_accept_at_ns <= llm_request_at_ns &&
                    llm_bus_stages.turn_generate_at_ns <= llm_request_at_ns &&
                    llm_response_at_ns <= llm_bus_stages.tts_speak_at_ns &&
                    tts_accept_at_ns <= tts_request_at_ns &&
                    llm_bus_stages.tts_speak_at_ns <= tts_request_at_ns;
                if (ordered) {
                    printf(
                        "BenchmarkHTTP_BusStageUs\t"
                        "request_write_to_turn_start=%llu\t"
                        "turn_start_to_generate=%llu\t"
                        "generate_to_llm_request=%llu\t"
                        "llm_body_to_tts_speak=%llu\t"
                        "tts_speak_to_tts_request=%llu\n",
                        (unsigned long long)((llm_bus_stages.turn_start_at_ns -
                            call.request_started_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((llm_bus_stages.turn_generate_at_ns -
                            llm_bus_stages.turn_start_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((llm_request_at_ns -
                            llm_bus_stages.turn_generate_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((llm_bus_stages.tts_speak_at_ns -
                            llm_response_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((tts_request_at_ns -
                            llm_bus_stages.tts_speak_at_ns) / UINT64_C(1000)));
                }
            } else {
                ordered = call.request_started_at_ns <=
                        llm_bus_stages.turn_start_at_ns &&
                    llm_accept_at_ns <= llm_request_at_ns &&
                    llm_bus_stages.turn_start_at_ns <= llm_request_at_ns &&
                    llm_response_at_ns <= llm_bus_stages.tts_speak_at_ns &&
                    tts_accept_at_ns <= tts_request_at_ns &&
                    llm_bus_stages.tts_speak_at_ns <= tts_request_at_ns;
                if (ordered) {
                    printf(
                        "BenchmarkHTTP_BusStageUs\t"
                        "request_write_to_turn_start=%llu\t"
                        "turn_start_to_llm_request=%llu\t"
                        "llm_body_to_tts_speak=%llu\t"
                        "tts_speak_to_tts_request=%llu\n",
                        (unsigned long long)((llm_bus_stages.turn_start_at_ns -
                            call.request_started_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((llm_request_at_ns -
                            llm_bus_stages.turn_start_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((llm_bus_stages.tts_speak_at_ns -
                            llm_response_at_ns) / UINT64_C(1000)),
                        (unsigned long long)((tts_request_at_ns -
                            llm_bus_stages.tts_speak_at_ns) / UINT64_C(1000)));
                }
            }
            if (ordered) {
                uint64_t admission_at_ns =
                    rollback_orchestrator && rollback_orchestrator[0] ?
                    llm_bus_stages.turn_generate_at_ns :
                    llm_bus_stages.turn_start_at_ns;
                int llm_preconnected = llm_accept_at_ns <= admission_at_ns;
                int tts_preconnected =
                    tts_accept_at_ns <= llm_bus_stages.tts_speak_at_ns;
                printf(
                    "BenchmarkHTTP_BackendSetupUs\t"
                    "llm_preconnected=%d\t"
                    "tts_preconnected=%d\t"
                    "admission_to_llm_request=%llu\t"
                    "tts_speak_to_tts_request=%llu\n",
                    llm_preconnected,
                    tts_preconnected,
                    (unsigned long long)((llm_request_at_ns -
                        admission_at_ns) / UINT64_C(1000)),
                    (unsigned long long)((tts_request_at_ns -
                        llm_bus_stages.tts_speak_at_ns) / UINT64_C(1000)));
                expect(
                    "model and TTS lanes preconnect before measured dispatch",
                    llm_preconnected && tts_preconnected);
            }
        }
        expect(
            "SSE model observer receives first PCM",
            poll_until(client, &llm_seen.pcm_chunk, 1000) == 0);
        expect(
            "SSE model first PCM exposes ordered stages",
            stages_complete_and_ordered(&llm_seen.first_pcm_stages));
        expect(
            "HTTP turn hides internal event subject",
            strstr(call.response, "\"protocol\":\"turnstream.v1alpha1\"") != NULL &&
            strstr(call.response, "\"response_event_contract\":\"canonical-v1\"") != NULL &&
            strstr(call.response, "ai.turn.events") == NULL &&
            strstr(call.response, "ai.private.must-not-win") == NULL &&
            strstr(call.response, "\"type\":\"tts_segment\"") == NULL);
        expect("HTTP turn preserves the provider model and final usage through the cascade",
            strstr(call.response, "\"model_id\":\"fixture-provider-model\"") &&
            strstr(call.response, "\"model_identity_source\":\"provider_reported\"") &&
            strstr(call.response, "\"usage_source\":\"provider_reported\"") &&
            strstr(call.response, "\"prompt_tokens\":\"39\"") &&
            strstr(call.response, "\"completion_tokens\":\"7\"") &&
            strstr(call.response, "\"total_tokens\":\"46\""));
        expect(
            "HTTP turn streams product events",
            strstr(call.response, "\"type\":\"accepted\"") != NULL &&
            strstr(call.response, "\"accepted_at\":") != NULL &&
            strstr(call.response, "\"type\":\"text_delta\"") != NULL &&
            strstr(call.response, "\"type\":\"text_completed\"") != NULL &&
            strstr(call.response, "\"timestamp\":") != NULL &&
            strstr(call.response, "\"type\":\"completed\"") != NULL);
        {
            char runtime_metadata[160];
            int metadata_len = snprintf(
                runtime_metadata,
                sizeof(runtime_metadata),
                "\"cascade_runtime_identity_sha256\":\"%s\"",
                runtime_identity_hash);
            expect(
                "HTTP turn binds text and completion to runtime identity",
                metadata_len > 0 &&
                (size_t)metadata_len < sizeof(runtime_metadata) &&
                count_occurrences(call.response, runtime_metadata) >= 2);
        }
        expect(
            "HTTP text preserves bounded JSON escaping",
            strstr(call.response, "\\\"rune\\\"") != NULL &&
            strstr(call.response, "silver\\\\road") != NULL);
        expect(
            "HTTP model turn emits one thinking start",
            count_occurrences(call.response, "\"type\":\"thinking_started\"") == 1);
        {
            uint64_t auth_us;
            uint64_t publish_us;
            uint64_t prepare_us;
            uint64_t admission_us;
            uint64_t capability_us;
            uint64_t encode_us;
            int edge_metrics =
                parse_uint64_field(
                    call.response, "\"edge_auth_us\":", &auth_us) == 0 &&
                parse_uint64_field(
                    call.response, "\"edge_vbus_publish_us\":", &publish_us) == 0 &&
                parse_uint64_field(
                    call.response, "\"edge_prepare_us\":", &prepare_us) == 0 &&
                parse_uint64_field(
                    call.response, "\"edge_admission_us\":", &admission_us) == 0 &&
                parse_uint64_field(
                    call.response, "\"edge_capability_us\":", &capability_us) == 0 &&
                parse_uint64_field(
                    call.response, "\"edge_encode_us\":", &encode_us) == 0 &&
                auth_us <= prepare_us && publish_us <= prepare_us;
            expect("HTTP turn exposes edge timings", edge_metrics);
            if (edge_metrics) {
                printf(
                    "BenchmarkHTTP_EdgeUs\tauth=%llu\tpublish=%llu\tprepare=%llu\n",
                    (unsigned long long)auth_us,
                    (unsigned long long)publish_us,
                    (unsigned long long)prepare_us);
                printf(
                    "BenchmarkHTTP_EdgeDetailUs\t"
                    "admission=%llu\tcapability=%llu\tencode=%llu\n",
                    (unsigned long long)admission_us,
                    (unsigned long long)capability_us,
                    (unsigned long long)encode_us);
            }
        }
        expect(
            "HTTP turn exposes canonical PCM fields",
            strstr(call.response, "\"audio_encoding\":1") != NULL &&
            strstr(call.response, "\"is_final\":true") != NULL);
        expect(
            "HTTP turn ignores out-of-schema model content",
            strstr(call.response, "metadata poison") == NULL);
        expect(
            "HTTP turn exposes bounded TTS stages",
            strstr(call.response, "\"stage_first_text_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_tts_segment_emitted_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_tts_request_received_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_tts_provider_request_started_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_tts_provider_ready_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_pcm_started_at_ms\":") != NULL &&
            strstr(call.response, "\"stage_pcm_first_chunk_at_ms\":") != NULL);
        expect(
            "HTTP turn ends final PCM before completion",
            last_occurrence(call.response, "\"type\":\"pcm_ended\"") != NULL &&
            strstr(call.response, "\"type\":\"completed\"") != NULL &&
            last_occurrence(call.response, "\"type\":\"pcm_ended\"") <
                strstr(call.response, "\"type\":\"completed\""));
        {
            char replay_response[2048];
            expect(
                "gateway rejects replayed nonce",
                gateway_post_turn_ex(
                    gateway_port,
                    "test-gateway-token-0123456789abcdef-0123456789abcdef",
                    call.request_id, call.text, "u-e2e", "u-e2e", 0,
                    call.nonce, 0, replay_response, sizeof(replay_response)) == 0 &&
                strstr(replay_response, "401 Unauthorized") != NULL);
            expect(
                "gateway rejects recently used request id",
                gateway_post_turn(
                    gateway_port,
                    "test-gateway-token-0123456789abcdef-0123456789abcdef",
                    call.request_id, call.text,
                    replay_response, sizeof(replay_response)) == 0 &&
                strstr(replay_response, "409 Conflict") != NULL &&
                strstr(replay_response, "request_id_reused") != NULL);
            expect(
                "gateway request ID replay guard survives churn",
                gateway_request_id_replay_survives_churn(
                    gateway_port,
                    "test-gateway-token-0123456789abcdef-0123456789abcdef") == 0);
        }
        expect("SSE model final event", poll_until(client, &llm_seen.final_event, 1500) == 0);
        expect("SSE model schema-clean channels", llm_seen.clean_channels);
        expect("split model emphasis preserves the complete plain answer",
            strcmp(llm_seen.final_text, llm_plain_answer) == 0 &&
            !strstr(call.response, "**") && !strchr(call.response, '`'));
        expect("model presentation markers never reach the TTS provider",
            atomic_load_explicit(&server.model_markup_requests, memory_order_relaxed) == 0);
        expect("SSE model delta has schema-clean channels", llm_seen.text_delta_clean_channels);
        expect("SSE model TTS segment", llm_seen.segment_event);
        expect("SSE model text precedes TTS segment", llm_seen.text_before_segment);
        expect("SSE model TTS segment precedes PCM", llm_seen.segment_before_pcm);
        expect("SSE model turn completed", poll_until(client, &llm_seen.completed, 1000) == 0);
        expect(
            "SSE TTS segments overlap synthesis",
            atomic_load_explicit(
                &server.llm_max_active_requests, memory_order_relaxed) >= 2);
        expect(
            "SSE TTS final audio observed",
            poll_until(client, &llm_pcm_final_seen, 1500) == 0);
        expect(
            "SSE TTS publication stays ordered",
            llm_pcm_ordered && llm_pcm_segments >= 2);
        expect(
            "SSE stream has one internal final marker",
            llm_bus_stages.tts_text_segments >= 2 &&
            llm_bus_stages.tts_final_markers == 1);
        expect(
            "SSE final marker never reaches the TTS provider",
            atomic_load_explicit(
                &server.llm_provider_requests, memory_order_relaxed) ==
                llm_bus_stages.tts_text_segments);
        expect(
            "free DND rules request admits scoped retrieval without generic RAG",
            rag_requests_seen == rag_requests_before + 1);
    }

    {
        static const char *const requests[] = {
            "req-governed-bookdeny", "req-governed-sourcehash", "req-governed-content",
            "req-governed-owner", "req-governed-campaign"
        };
        size_t index;
        for (index = 0; index < sizeof(requests) / sizeof(requests[0]); ++index) {
            gateway_turn_call call;
            int model_before = atomic_load_explicit(&llm_server.requests, memory_order_relaxed);
            int speech_before = atomic_load_explicit(&server.requests, memory_order_relaxed);
            memset(&call, 0, sizeof(call));
            call.port = gateway_port;
            call.request_id = requests[index];
            call.text = "Look up the source for ancient dragon lore.";
            call.enable_rag = index >= 3u;
            (void)gateway_turn_thread(&call);
            expect(requests[index], call.result == 0 &&
                strstr(call.response, "\"type\":\"failed\"") &&
                !strstr(call.response, "\"type\":\"text_delta\"") &&
                !strstr(call.response, "\"type\":\"pcm_chunk\"") &&
                atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == model_before &&
                atomic_load_explicit(&server.requests, memory_order_relaxed) == speech_before);
        }
    }

    {
        static const struct { const char *id; const char *text; int rag; int fail; } cases[] = {
            /* These first three identifiers select the same model worker. */
            {"req-policy-rag-length", "Look up ancient dragon lore. Exhaust the token budget.", 1, 1},
            {"req-policy-rag-ceiling-2", "Look up ancient dragon lore. Keep the server ceiling.", 1, 0},
            {"req-policy-after-0", "Explain how invisibility changes attack rolls after recovery.", 0, 0},
            {"req-policy-invalid", "Explain how invisibility changes attack rolls with an invalid budget.", 0, 1}
        };
        size_t index;
        for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            gateway_turn_call call = {0};
            int requests_before = atomic_load_explicit(&llm_server.requests, memory_order_relaxed);
            call.port = gateway_port;
            call.request_id = cases[index].id;
            call.text = cases[index].text;
            call.enable_rag = cases[index].rag;
            call.disable_tts = 1;
            (void)gateway_turn_thread(&call);
            expect(cases[index].id, call.result == 0 && (cases[index].fail ?
                count_occurrences(call.response, "\"type\":\"failed\"") == 1 &&
                !strstr(call.response, "\"type\":\"text_completed\"") &&
                !strstr(call.response, "\"type\":\"completed\"") &&
                !strstr(call.response, "cascade_retrieval_citations") :
                strstr(call.response, "\"type\":\"text_completed\"") &&
                strstr(call.response, "\"type\":\"completed\"") &&
                !strstr(call.response, "\"type\":\"failed\"")));
            if (index == 3u) expect("invalid client budget never reaches the model",
                atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == requests_before);
        }
        expect("completion failure and budget reset preserve the same model connection",
            atomic_load_explicit(&llm_server.policy_connection_at_ns, memory_order_relaxed) > 0u &&
            atomic_load_explicit(&llm_server.policy_connection_failures, memory_order_relaxed) == 0);
    }

    {
        gateway_turn_call call;
        pthread_t turn_thread;
        int variant;
        memset(&call, 0, sizeof(call));
        call.port = gateway_port;
        for (variant = 0; variant < 2; ++variant) {
            memset(&llm_late_error_seen, 0, sizeof(llm_late_error_seen));
            atomic_store_explicit(&server.late_error_pcm_sent, 0, memory_order_relaxed);
            call.request_id = variant ? "req-llm-late-limit" : "req-llm-late-error";
            call.text = variant ? "Trigger a late provider error after speakable text. Exhaust the token budget." :
                "Trigger a late provider error after speakable text.";
            expect(
                "start late-error model turn",
                pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
            expect(
                "late-error model starts TTS before failing",
                poll_until(client, &llm_late_error_seen.pcm_started, 1000) == 0 &&
                atomic_load_explicit(
                    &server.late_error_requests, memory_order_relaxed) == variant + 1);
            expect(
                "late model error fails the public turn",
                poll_until(client, &llm_late_error_seen.failed, 1000) == 0);
            pthread_join(turn_thread, NULL);
            expect(
                "late-error HTTP stream has one failure terminal",
                call.result == 0 &&
                count_occurrences(call.response, "\"type\":\"failed\"") == 1 &&
                strstr(call.response, "\"type\":\"completed\"") == NULL &&
                strstr(call.response, "\"type\":\"canceled\"") == NULL);
            for (i = 0; i < 100 && atomic_load_explicit(&server.late_error_disconnects,
                    memory_order_relaxed) < variant + 1; ++i) (void)vbus_poll(client, 5);
            expect(variant ? "token exhaustion cancels active TTS provider work" :
                "late model error cancels active TTS provider work",
                atomic_load_explicit(&server.late_error_disconnects, memory_order_relaxed) == variant + 1);
            if (variant) expect("token exhaustion remains an explicit failure",
                llm_late_error_seen.completion_limited);
            expect(
                "late model error suppresses TTS completion",
                poll_until(client, &llm_late_error_seen.pcm_ended, 300) != 0 &&
                !llm_late_error_seen.completed && !llm_late_error_seen.canceled);
            expect("model failure discards pending emphasis without public text",
                !strstr(call.response, "unsent_pending_markup") && !llm_late_error_seen.final_event);
        }
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish one deferred TTS text segment",
            publish_deferred_tts_segment(
                client,
                "req-tts-deferred-single",
                "one deferred segment",
                0,
                0) == 0);
        expect(
            "publish one deferred TTS final marker",
            publish_deferred_tts_segment(
                client, "req-tts-deferred-single", "", 1, 1) == 0);
        expect(
            "deferred one-segment stream marks its only PCM tail final",
            poll_until(client, &deferred_single_seen.pcm_ended, 1000) == 0 &&
            deferred_single_seen.pcm_started &&
            deferred_single_seen.pcm_final_seen &&
            deferred_single_seen.pcm_end_after_chunk == 1 &&
            !deferred_single_seen.failed && !deferred_single_seen.canceled);
        expect(
            "deferred final marker creates no provider request",
            atomic_load_explicit(&server.requests, memory_order_relaxed) ==
                requests_before + 1);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish orphan deferred final marker",
            publish_deferred_tts_segment(
                client, "req-tts-deferred-orphan-marker", "", 1, 1) == 0);
        expect(
            "orphan deferred final marker fails closed",
            poll_until(client, &deferred_orphan_marker_seen.failed, 500) == 0 &&
            !deferred_orphan_marker_seen.pcm_started &&
            atomic_load_explicit(&server.requests, memory_order_relaxed) ==
                requests_before);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        int poll_count = 0;
        expect(
            "publish deferred segment before out-of-order marker",
            publish_deferred_tts_segment(
                client,
                "req-tts-deferred-bad-marker",
                "reject an out of order marker",
                0,
                0) == 0);
        while (atomic_load_explicit(
                   &server.requests, memory_order_relaxed) == requests_before &&
               poll_count < 100) {
            (void)vbus_poll(client, 10);
            poll_count++;
        }
        expect(
            "deferred segment reaches provider before marker rejection",
            atomic_load_explicit(&server.requests, memory_order_relaxed) ==
                requests_before + 1);
        expect(
            "publish out-of-order deferred final marker",
            publish_deferred_tts_segment(
                client, "req-tts-deferred-bad-marker", "", 2, 1) == 0);
        expect(
            "out-of-order deferred final marker aborts the stream",
            poll_until(client, &deferred_bad_marker_seen.failed, 500) == 0 &&
            !deferred_bad_marker_seen.pcm_ended &&
            !deferred_bad_marker_seen.completed);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        int poll_count = 0;
        expect(
            "publish deferred segment before mode mismatch",
            publish_deferred_tts_segment(
                client,
                "req-tts-deferred-mode-mismatch",
                "never mix finality modes",
                0,
                0) == 0);
        while (atomic_load_explicit(
                   &server.requests, memory_order_relaxed) == requests_before &&
               poll_count < 100) {
            (void)vbus_poll(client, 10);
            poll_count++;
        }
        expect(
            "deferred segment reaches provider before mode mismatch",
            atomic_load_explicit(&server.requests, memory_order_relaxed) ==
                requests_before + 1);
        expect(
            "publish legacy segment into deferred stream",
            publish_tts_segment(
                client,
                "req-tts-deferred-mode-mismatch",
                "legacy finality must be rejected",
                1,
                1) == 0);
        expect(
            "mixed TTS finality modes abort the stream",
            poll_until(client, &deferred_mode_mismatch_seen.failed, 500) == 0 &&
            !deferred_mode_mismatch_seen.pcm_ended &&
            !deferred_mode_mismatch_seen.completed);
    }

    {
        expect(
            "publish deferred segment without final marker",
            publish_deferred_tts_segment(
                client,
                "req-tts-deferred-missing",
                "missing markers must fail closed",
                0,
                0) == 0);
        expect(
            "missing deferred final marker times out",
            poll_until(client, &deferred_missing_seen.failed, 4000) == 0 &&
            deferred_missing_seen.pcm_started &&
            !deferred_missing_seen.pcm_ended &&
            !deferred_missing_seen.completed &&
            !deferred_missing_seen.canceled);
    }

    {
        int accepts_before = atomic_load_explicit(
            &server.accepts, memory_order_relaxed);
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish header-only TTS response turn",
            publish_tts_segment(
                client, "req-tts-header-only", "send no provider audio", 0, 1) == 0);
        expect(
            "header-only TTS response fails without false PCM events",
            poll_until(client, &tts_header_only_seen.failed, 1000) == 0 &&
            !tts_header_only_seen.pcm_started &&
            !tts_header_only_seen.pcm_chunk &&
            !tts_header_only_seen.pcm_ended &&
            !tts_header_only_seen.completed &&
            !tts_header_only_seen.canceled);
        expect(
            "publish truncated TTS response turn",
            publish_tts_segment(
                client, "req-tts-truncated", "truncate this provider response", 0, 1) == 0);
        expect(
            "truncated TTS response exposes only partial PCM",
            poll_until(client, &tts_truncated_seen.pcm_chunk, 1000) == 0 &&
            tts_truncated_seen.pcm_started && tts_truncated_seen.pcm_chunk == 1 &&
            !tts_truncated_seen.audio_fields);
        expect(
            "truncated TTS response fails without a completion marker",
            poll_until(client, &tts_truncated_seen.failed, 1000) == 0 &&
            !tts_truncated_seen.pcm_ended && !tts_truncated_seen.completed &&
            !tts_truncated_seen.canceled);
        expect(
            "publish TTS turn after truncated response",
            publish_tts_segment(
                client, "req-tts-truncated-recovered",
                "recover the discarded provider connection", 0, 1) == 0);
        expect(
            "TTS lane recovers after truncated provider response",
            poll_until(client, &tts_recovered_seen.pcm_ended, 1000) == 0 &&
            tts_recovered_seen.pcm_started && tts_recovered_seen.audio_fields &&
            !tts_recovered_seen.failed && !tts_recovered_seen.canceled &&
            tts_truncated_seen.failed && !tts_truncated_seen.pcm_ended &&
            !tts_truncated_seen.completed && !tts_truncated_seen.canceled &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == requests_before + 3 &&
            atomic_load_explicit(
                &server.accepts, memory_order_relaxed) > accepts_before);
    }

    {
        uint8_t wire[256];
        size_t wire_len;
        expect(
            "publish pipelined TTS segment zero",
            publish_tts_segment(
                client, "req-tts-pipeline-cancel", "first pipelined segment", 0, 0) == 0);
        expect(
            "publish pipelined TTS segment one",
            publish_tts_segment(
                client, "req-tts-pipeline-cancel", "second pipelined segment", 1, 1) == 0);
        for (i = 0; i < 100 &&
             atomic_load_explicit(
                 &server.pipeline_cancel_active_requests, memory_order_relaxed) < 2; ++i)
            (void)vbus_poll(client, 10);
        expect(
            "pipelined TTS cancel reaches two provider requests",
            atomic_load_explicit(
                &server.pipeline_cancel_max_active_requests, memory_order_relaxed) >= 2);
        expect(
            "pipelined TTS starts first segment",
            poll_until(client, &pipeline_cancel_seen.pcm_started, 500) == 0);
        wire_len = pb_encode_turn_cancel(
            wire, sizeof(wire), "req-tts-pipeline-cancel", "u-e2e", "barge_in");
        expect(
            "publish pipelined TTS cancel",
            wire_len != 0 && vbus_publish(client, SUBJ_TURN_CANCEL, wire, wire_len) == 0);
        expect(
            "pipelined TTS cancel observed",
            poll_until(client, &pipeline_cancel_seen.canceled, 500) == 0);
        expect(
            "pipelined TTS cancel suppresses segment completion",
            !pipeline_cancel_seen.pcm_ended);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish TTS subject-binding segment zero",
            publish_tts_segment(
                client, "req-tts-subject-bind", "trusted first segment", 0, 0) == 0);
        for (i = 0; i < 100 &&
             atomic_load_explicit(
                 &server.requests, memory_order_relaxed) == requests_before; ++i)
            (void)vbus_poll(client, 10);
        expect(
            "TTS subject-binding first segment reaches provider",
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) > requests_before);
        expect(
            "publish TTS mismatched response subject",
            publish_tts_segment_subject(
                client, "req-tts-subject-bind", "redirected second segment", 1, 1,
                "ai.turn.events.attacker") == 0);
        expect(
            "TTS response subject mismatch fails request",
            poll_until(client, &subject_bind_seen.failed, 500) == 0);
        expect(
            "TTS failure stays on bound response subject",
            !subject_bind_wrong_subject);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        expect(
            "publish TTS invalid near-match response subject",
            publish_tts_segment_subject(
                client, "req-tts-invalid-subject", "reject this subject",
                0, 1, "ai.turn.events.req-tts-invalid-subject*") == 0);
        expect(
            "publish TTS segment without response subject",
            publish_tts_segment_subject(
                client, "req-tts-fallback-subject", "use the derived subject",
                0, 1, "") == 0);
        expect(
            "TTS derives the bounded response subject",
            poll_until(client, &fallback_subject_seen.pcm_ended, 1000) == 0 &&
            fallback_subject_seen.pcm_started && fallback_subject_seen.audio_fields &&
            !fallback_subject_seen.failed && !fallback_wrong_subject);
        expect(
            "TTS validates non-derived response subject",
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == requests_before + 1);
    }

    {
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        int poll_count = 0;
        expect(
            "publish first TTS index collision",
            publish_tts_segment(
                client, "req-tts-index-full-collision-122789",
                "first collision chain head", 0, 0) == 0);
        expect(
            "publish second TTS index collision",
            publish_tts_segment(
                client, "req-tts-index-full-collision-339192",
                "second collision chain head", 0, 0) == 0);
        expect(
            "TTS index preserves colliding active requests",
            poll_until(client, &tts_index_collision_a_seen.pcm_ended, 1000) == 0 &&
            poll_until(client, &tts_index_collision_b_seen.pcm_ended, 1000) == 0 &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == requests_before + 2);
        expect(
            "publish first TTS collision continuation",
            publish_tts_segment(
                client, "req-tts-index-full-collision-122789",
                "finish first collision chain", 1, 1) == 0);
        expect(
            "publish second TTS collision continuation",
            publish_tts_segment(
                client, "req-tts-index-full-collision-339192",
                "finish second collision chain", 1, 1) == 0);
        while ((tts_index_collision_a_seen.pcm_ended < 2 ||
                tts_index_collision_b_seen.pcm_ended < 2) && poll_count < 200) {
            (void)vbus_poll(client, 10);
            poll_count++;
        }
        expect(
            "TTS index resolves both collision-chain continuations",
            tts_index_collision_a_seen.pcm_ended == 2 &&
            tts_index_collision_b_seen.pcm_ended == 2 &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == requests_before + 4);
        expect(
            "publish recycled TTS collision key",
            publish_tts_segment(
                client, "req-tts-index-full-collision-122789",
                "reuse recycled collision slot", 0, 1) == 0);
        while (tts_index_collision_a_seen.pcm_ended < 3 && poll_count < 300) {
            (void)vbus_poll(client, 10);
            poll_count++;
        }
        expect(
            "TTS index recycles a collision-chain slot",
            tts_index_collision_a_seen.pcm_ended == 3 &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) == requests_before + 5);
    }

    {
        uint8_t wire[256];
        char request_id[64];
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        int fill_ok = 1;
        int cancel_ok = 1;
        int admitted = 0;
        int fill;
        for (fill = 0; fill < SERVICE_E2E_TTS_ORDER_CAPACITY; ++fill) {
            int poll_count = 0;
            int written = snprintf(
                request_id, sizeof(request_id), "req-tts-order-fill-%03d", fill);
            if (written <= 0 || (size_t)written >= sizeof(request_id) ||
                publish_tts_segment(
                    client, request_id, "hold bounded order state", 0, 0) != 0) {
                fill_ok = 0;
                break;
            }
            admitted++;
            while (tts_order_capacity_seen.pcm_ended < admitted &&
                   poll_count < 200) {
                (void)vbus_poll(client, 10);
                poll_count++;
            }
            if (tts_order_capacity_seen.pcm_ended != admitted) {
                fill_ok = 0;
                break;
            }
        }
        expect(
            "TTS order index admits its exact bounded capacity",
            fill_ok && admitted == SERVICE_E2E_TTS_ORDER_CAPACITY &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) ==
                requests_before + SERVICE_E2E_TTS_ORDER_CAPACITY);
        if (fill_ok) {
            expect(
                "publish TTS order beyond bounded capacity",
                publish_tts_segment(
                    client, "req-tts-order-overflow",
                    "this order must fail closed", 0, 1) == 0);
            expect(
                "TTS order index rejects excess active state",
                poll_until(client, &tts_order_overflow_seen.failed, 1000) == 0 &&
                atomic_load_explicit(
                    &server.requests, memory_order_relaxed) ==
                    requests_before + SERVICE_E2E_TTS_ORDER_CAPACITY);
        }
        for (fill = 0; fill < admitted; ++fill) {
            size_t wire_len;
            int written = snprintf(
                request_id, sizeof(request_id), "req-tts-order-fill-%03d", fill);
            if (written <= 0 || (size_t)written >= sizeof(request_id)) {
                cancel_ok = 0;
                break;
            }
            wire_len = pb_encode_turn_cancel(
                wire, sizeof(wire), request_id, "u-e2e", "barge_in");
            if (wire_len == 0u ||
                vbus_publish(client, SUBJ_TURN_CANCEL, wire, wire_len) != 0) {
                cancel_ok = 0;
                break;
            }
        }
        expect("publish cancellation for every bounded TTS order", cancel_ok);
        expect(
            "publish TTS after full order-table release",
            publish_tts_segment(
                client, "req-tts-order-recovered",
                "reuse capacity after bounded release", 0, 1) == 0);
        expect(
            "TTS order index recovers after full release",
            poll_until(
                client, &tts_order_capacity_recovered_seen.pcm_ended, 1500) == 0 &&
            !tts_order_capacity_recovered_seen.failed &&
            atomic_load_explicit(
                &server.requests, memory_order_relaxed) ==
                requests_before + SERVICE_E2E_TTS_ORDER_CAPACITY + 1);
    }

    {
        turn_tts_segment_c segment;
        uint8_t wire[4096];
        size_t wire_len;
        int requests_before = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        memset(&segment, 0, sizeof(segment));
        snprintf(segment.request_id, sizeof(segment.request_id), "req-tts-future-stage");
        snprintf(segment.text, sizeof(segment.text), "future timing must fail");
        snprintf(
            segment.response_subject, sizeof(segment.response_subject),
            "%s.%s", SUBJ_TURN_EVENTS_PFX, segment.request_id);
        segment.is_final = 1;
        segment.first_text_at_ms = INT64_MAX - 2;
        segment.segment_emitted_at_ms = INT64_MAX - 1;
        wire_len = pb_encode_turn_tts_segment(wire, sizeof(wire), &segment);
        expect(
            "publish future TTS stage timestamp",
            wire_len > 0 &&
            vbus_publish(client, SUBJ_TURN_TTS_SPEAK, wire, wire_len) == 0);
        expect(
            "future TTS stage fails closed",
            poll_until(client, &future_stage_seen.failed, 500) == 0);
        expect(
            "future TTS stage never reaches provider",
            atomic_load_explicit(&server.requests, memory_order_relaxed) == requests_before);
    }

    {
        gateway_turn_call call;
        pthread_t turn_thread;
        char cancel_response[2048];
        int llm_requests_before = atomic_load_explicit(
            &llm_server.requests, memory_order_relaxed);
        memset(&call, 0, sizeof(call));
        call.port = gateway_port;
        call.request_id = "req-llm-cancel";
        call.text = "explain a deliberately slow answer";
        expect(
            "start cancelable HTTP model turn",
            pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
        for (i = 0; i < 100 &&
             atomic_load_explicit(&llm_server.requests, memory_order_relaxed) ==
                 llm_requests_before; ++i) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&delay, NULL);
        }
        expect(
            "cancelable SSE reached model backend",
            atomic_load_explicit(&llm_server.requests, memory_order_relaxed) ==
                llm_requests_before + 1);
        expect(
            "authenticated HTTP cancel accepted",
            gateway_post_cancel(
                gateway_port, "req-llm-cancel", cancel_response, sizeof(cancel_response)) == 0 &&
            strstr(cancel_response, "200 OK") != NULL &&
            strstr(cancel_response, "\"cancelled\":true") != NULL);
        expect(
            "SSE model cancel observed",
            poll_until(client, &llm_cancel_seen.canceled, 500) == 0);
        expect("SSE cancel suppresses TTS", !llm_cancel_seen.pcm_started);
        pthread_join(turn_thread, NULL);
        expect("canceled HTTP stream closes cleanly", call.result == 0);
        expect(
            "canceled HTTP stream has terminal",
            strstr(call.response, "\"type\":\"canceled\"") != NULL);
    }

    {
        char partial_response[128];
        expect(
            "start model turn then disconnect",
            gateway_post_turn_ex(
                gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-disconnect", "explain another deliberately slow answer",
                "u-e2e", "u-e2e", 0, NULL, 0,
                partial_response, sizeof(partial_response)) == 0);
        expect(
            "disconnect reaches model cancellation",
            poll_until(client, &disconnect_seen.canceled, 500) == 0);
        expect("disconnect suppresses TTS", !disconnect_seen.pcm_started);
    }

    {
        char response[16384], text[256], request_id[64];
        for (int turn_index = 0; turn_index < 9; ++turn_index) {
            (void)snprintf(request_id, sizeof(request_id), "req-loop-%d", turn_index);
            (void)loop_fixture_prompt(text, sizeof(text), turn_index);
            int result = gateway_post_turn(gateway_port,
                    "test-gateway-token-0123456789abcdef-0123456789abcdef",
                    request_id, text, response, sizeof(response));
            int terminal_seen = strstr(response, turn_index == 6
                ? "\"type\":\"failed\"" : "\"type\":\"completed\"") != NULL;
            if (result != 0 || !terminal_seen)
                fprintf(stderr, "Loop fixture index=%d transport=%d failed=%d completed=%d model_requests=%d\n",
                    turn_index, result, strstr(response, "\"type\":\"failed\"") != NULL,
                    strstr(response, "\"type\":\"completed\"") != NULL,
                    atomic_load_explicit(&llm_server.requests, memory_order_relaxed));
            expect("authenticated Loop process conversation", result == 0 && terminal_seen);
        }
        int requests_before = atomic_load_explicit(&llm_server.requests, memory_order_relaxed);
        expect("another owner cannot read or append Loop context",
            gateway_post_turn_ex(gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-loop-other", "Explain how invisibility changes attack rolls.",
                "loop-other", "loop-other", 0, NULL, 0, response, sizeof(response)) == 0 &&
            strstr(response, "\"type\":\"failed\"") &&
            atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == requests_before);
        expect("failed ownership check preserves owner history",
            request_session_get_n(request_client, "loop-fixture", "u-e2e", NULL, 0) == 15 &&
            request_session_get(request_client, "loop-fixture", "u-e2e", llm_plain_answer) == 1);
        expect("fresh Loop conversation has only its own pair",
            request_session_get_n(request_client, "loop-fresh", "u-e2e", NULL, 0) == 2 &&
            request_session_get(request_client, "loop-fresh", "u-e2e", llm_plain_answer) == 1);
        expect("fixture conversation cleanup",
            request_session_delete(request_client, "loop-fixture", "u-e2e") == 1 &&
            request_session_delete(request_client, "loop-fresh", "u-e2e") == 1);
        gateway_turn_call call = {0};
        pthread_t turn_thread;
        char cancel_response[2048];
        (void)loop_fixture_prompt(text, sizeof(text), 9);
        call.port = gateway_port;
        call.request_id = "req-loop-9";
        call.text = text;
        requests_before = atomic_load_explicit(&llm_server.requests, memory_order_relaxed);
        expect("start cancelable Loop model turn",
            pthread_create(&turn_thread, NULL, gateway_turn_thread, &call) == 0);
        for (int wait_index = 0; wait_index < 100 &&
             atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == requests_before; ++wait_index) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&delay, NULL);
        }
        expect("Loop cancellation reaches active model",
            atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == requests_before + 1 &&
            gateway_post_cancel(gateway_port, "req-loop-9", cancel_response, sizeof(cancel_response)) == 0 &&
            strstr(cancel_response, "\"cancelled\":true"));
        pthread_join(turn_thread, NULL);
        expect("canceled Loop stream has terminal", call.result == 0 &&
            strstr(call.response, "\"type\":\"canceled\""));
        expect("canceled model adds no assistant claim",
            request_session_get(request_client, "loop-cancel", "u-e2e", text) == 1);
        (void)loop_fixture_prompt(text, sizeof(text), 10);
        expect("Loop recovers with only canceled user speech in context",
            gateway_post_turn(gateway_port,
                "test-gateway-token-0123456789abcdef-0123456789abcdef",
                "req-loop-10", text, response, sizeof(response)) == 0 &&
            strstr(response, "\"type\":\"completed\""));
        expect("canceled conversation cleanup",
            request_session_delete(request_client, "loop-cancel", "u-e2e") == 1);
    }

    {
        char response[16384];
        static const char *const questions[] = {
            "We play D&D. I am considering the fireball spell. What should I check?",
            "Which materials does that spell require?"
        };
        int model_before = atomic_load_explicit(&llm_server.requests, memory_order_relaxed);
        int speech_before = atomic_load_explicit(&server.requests, memory_order_relaxed);
        for (size_t q = 0; q < sizeof(questions) / sizeof(questions[0]); ++q) {
            expect("uncited Loop mechanics clarify before any model or speech request",
                gateway_post_turn(gateway_port,
                    "test-gateway-token-0123456789abcdef-0123456789abcdef",
                    q ? "req-loop-rules-followup" : "req-loop-rules-first",
                    questions[q], response, sizeof(response)) == 0 &&
                strstr(response, "Which edition or verified source should I check?") &&
                strstr(response, "\"type\":\"completed\"") &&
                !strstr(response, "\"provider_model\"") &&
                atomic_load_explicit(&llm_server.requests, memory_order_relaxed) == model_before &&
                atomic_load_explicit(&server.requests, memory_order_relaxed) == speech_before);
        }
        expect("source clarification keeps both user and assistant pairs",
            request_session_get_n(request_client, "loop-rules", "u-e2e", NULL, 0) == 4);
        expect("source clarification conversation cleanup",
            request_session_delete(request_client, "loop-rules", "u-e2e") == 1);
    }

    {
        session_append_request_c append;
        session_append_response_c append_response;
        session_get_request_c get;
        session_get_response_c get_response;
        session_id_request_c id_request;
        session_summary_response_c summary_response;
        session_delete_response_c delete_response;
        uint8_t request_wire[8192];
        uint8_t response_wire[8192];
        size_t request_len;
        size_t response_len = 0;
        memset(&append, 0, sizeof(append));
        snprintf(append.session_id, sizeof(append.session_id), "session-e2e");
        snprintf(append.user_id, sizeof(append.user_id), "user-e2e");
        snprintf(append.message.role, sizeof(append.message.role), "user");
        snprintf(append.message.content, sizeof(append.message.content), "remember this");
        snprintf(append.message.request_id, sizeof(append.message.request_id), "req-e2e");
        append.message.timestamp_ms = 12345;
        request_len = pb_encode_session_append_request(
            request_wire, sizeof(request_wire), &append);
        expect(
            "process session append",
            request_len && vbus_request(
                request_client, SUBJ_SESSION_APPEND, request_wire, request_len,
                response_wire, sizeof(response_wire), &response_len, 1000) == 0 &&
            pb_decode_session_append_response(
                response_wire, response_len, &append_response) == 0 &&
            append_response.message_count == 1);
        expect(
            "process existing session append",
            request_session_append(
                request_client, "session-e2e", "user-e2e",
                "remember this too") == 2);

        memset(&get, 0, sizeof(get));
        snprintf(get.session_id, sizeof(get.session_id), "session-e2e");
        snprintf(get.user_id, sizeof(get.user_id), "user-e2e");
        get.last_n = 1;
        request_len = pb_encode_session_get_request(request_wire, sizeof(request_wire), &get);
        response_len = 0;
        expect(
            "process session get",
            request_len && vbus_request(
                request_client, SUBJ_SESSION_GET, request_wire, request_len,
                response_wire, sizeof(response_wire), &response_len, 1000) == 0 &&
            pb_decode_session_get_response(response_wire, response_len, &get_response) == 0 &&
            get_response.message_count == 1 &&
            strcmp(get_response.first_message.content, "remember this too") == 0);

        memset(&id_request, 0, sizeof(id_request));
        snprintf(id_request.session_id, sizeof(id_request.session_id), "session-e2e");
        snprintf(id_request.user_id, sizeof(id_request.user_id), "user-e2e");
        request_len = pb_encode_session_id_request(
            request_wire, sizeof(request_wire), &id_request);
        response_len = 0;
        expect(
            "process session summary",
            request_len && vbus_request(
                request_client, SUBJ_SESSION_SUMMARY, request_wire, request_len,
                response_wire, sizeof(response_wire), &response_len, 1000) == 0 &&
            pb_decode_session_summary_response(
                response_wire, response_len, &summary_response) == 0 &&
            strstr(summary_response.summary, "remember this") != NULL &&
            strstr(summary_response.summary, "remember this too") != NULL);

        response_len = 0;
        expect(
            "process session delete",
            vbus_request(
                request_client, SUBJ_SESSION_DELETE, request_wire, request_len,
                response_wire, sizeof(response_wire), &response_len, 1000) == 0 &&
            pb_decode_session_delete_response(
                response_wire, response_len, &delete_response) == 0 &&
            delete_response.deleted == 1);

        /* These identifiers share bucket 516 in the 2,048-slot FNV index. */
        expect(
            "session collision first append",
            request_session_append(
                request_client, "index-collision-111", "index-user-a", "collision-a") == 1);
        expect(
            "session collision second append",
            request_session_append(
                request_client, "index-collision-568", "index-user-b", "collision-b") == 1);
        expect(
            "session collision first lookup",
            request_session_get(
                request_client, "index-collision-111", "index-user-a", "collision-a") == 1);
        expect(
            "session collision second lookup",
            request_session_get(
                request_client, "index-collision-568", "index-user-b", "collision-b") == 1);
        expect(
            "session ownership mismatch hides payload",
            request_session_get(
                request_client, "index-collision-111", "index-user-b", NULL) == 0);
        expect(
            "session ownership mismatch blocks delete",
            request_session_delete(
                request_client, "index-collision-111", "index-user-b") == 0);
        expect(
            "session collision delete",
            request_session_delete(
                request_client, "index-collision-111", "index-user-a") == 1);
        expect(
            "session tombstone reuse",
            request_session_append(
                request_client, "index-collision-920", "index-user-c", "collision-c") == 1);
        expect(
            "session reused collision lookup",
            request_session_get(
                request_client, "index-collision-920", "index-user-c", "collision-c") == 1);
        expect(
            "session collision survivor lookup",
            request_session_get(
                request_client, "index-collision-568", "index-user-b", "collision-b") == 1);
        expect(
            "session collision survivor cleanup",
            request_session_delete(
                request_client, "index-collision-568", "index-user-b") == 1);
        expect(
            "session reused collision cleanup",
            request_session_delete(
                request_client, "index-collision-920", "index-user-c") == 1);
        expect(
            "session malformed append fails immediately",
            request_session_append_error(
                request_client, "bad/session", "capacity-user", "small",
                "validation_error") == 0);
        {
            char large_content[3600];
            memset(large_content, 'A', sizeof(large_content) - 1u);
            large_content[sizeof(large_content) - 1u] = '\0';
            expect(
                "session store fills first half",
                request_session_append(
                    request_client, "capacity-a", "capacity-user", large_content) == 1);
            expect(
                "session store fills second half",
                request_session_append(
                    request_client, "capacity-b", "capacity-user", large_content) == 1);
            expect(
                "session store rejects excess immediately",
                request_session_append_error(
                    request_client, "capacity-c", "capacity-user", "small",
                    "transient") == 0);
            expect(
                "session failed create rolls back index",
                request_session_delete(
                    request_client, "capacity-c", "capacity-user") == 0);
            expect(
                "session store releases deleted capacity",
                request_session_delete(
                    request_client, "capacity-a", "capacity-user") == 1);
            expect(
                "session store accepts after release",
                request_session_append(
                    request_client, "capacity-c", "capacity-user", "small") == 1);
            expect(
                "session capacity second cleanup",
                request_session_delete(
                    request_client, "capacity-b", "capacity-user") == 1);
            expect(
                "session capacity recovered cleanup",
                request_session_delete(
                    request_client, "capacity-c", "capacity-user") == 1);
        }
        /* Keep one allocation live so leak gates exercise shutdown ownership. */
        expect(
            "session shutdown owns active message storage",
            request_session_append(
                request_client, "shutdown-live", "shutdown-user",
                "release this message during graceful shutdown") == 1);
    }

    if (bench_turns > 0) {
        benchmark_metrics metrics;
        benchmark_worker workers[SERVICE_E2E_BENCH_CONCURRENCY_MAX];
        pthread_t worker_threads[SERVICE_E2E_BENCH_CONCURRENCY_MAX];
        scheduler_sample scheduler_before[CHILDREN_MAX];
        scheduler_sample scheduler_after[CHILDREN_MAX];
        atomic_int ready;
        atomic_int start;
        atomic_int cohort_ok;
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 100000L};
        uint64_t cohort_started_at_ns;
        uint64_t cohort_ended_at_ns;
        uint64_t cohort_elapsed_us;
        double throughput;
        int initial_llm_requests = atomic_load_explicit(
            &llm_server.requests, memory_order_relaxed);
        int initial_tts_requests = atomic_load_explicit(
            &server.requests, memory_order_relaxed);
        int workers_started = 0;
        int cohort_pass;
        size_t scheduler_child_count = child_count;
        int j;

        memset(&metrics, 0, sizeof(metrics));
        memset(workers, 0, sizeof(workers));
        memset(scheduler_before, 0, sizeof(scheduler_before));
        memset(scheduler_after, 0, sizeof(scheduler_after));
        atomic_init(&ready, 0);
        atomic_init(&start, 0);
        atomic_init(&cohort_ok, 1);
        for (j = 0; j < bench_concurrency; ++j) {
            workers[j].gateway_port = gateway_port;
            workers[j].worker_index = j;
            workers[j].concurrency = bench_concurrency;
            workers[j].turns = bench_turns;
            workers[j].tts_server = &server;
            workers[j].llm_server = &llm_server;
            workers[j].metrics = &metrics;
            workers[j].ready = &ready;
            workers[j].start = &start;
            workers[j].cohort_ok = &cohort_ok;
        }
        if (bench_concurrency == 1) {
            scheduler_snapshot_all(
                scheduler_before,
                sizeof(scheduler_before) / sizeof(scheduler_before[0]));
            cohort_started_at_ns = mono_ns();
            atomic_store_explicit(&start, 1, memory_order_release);
            (void)benchmark_gateway_worker(&workers[0]);
        } else {
            for (j = 0; j < bench_concurrency; ++j) {
                if (pthread_create(
                        &worker_threads[j],
                        NULL,
                        benchmark_gateway_worker,
                        &workers[j]) != 0) {
                    atomic_store_explicit(&cohort_ok, 0, memory_order_relaxed);
                    break;
                }
                workers_started++;
            }
            if (workers_started == 0) {
                workers[0].worker_index = 0;
                workers[0].concurrency = 1;
                scheduler_snapshot_all(
                    scheduler_before,
                    sizeof(scheduler_before) / sizeof(scheduler_before[0]));
                cohort_started_at_ns = mono_ns();
                atomic_store_explicit(&start, 1, memory_order_release);
                (void)benchmark_gateway_worker(&workers[0]);
            } else {
                for (j = 0; j < workers_started; ++j)
                    workers[j].concurrency = workers_started;
                while (atomic_load_explicit(&ready, memory_order_acquire) <
                       workers_started)
                    nanosleep(&wait, NULL);
                scheduler_snapshot_all(
                    scheduler_before,
                    sizeof(scheduler_before) / sizeof(scheduler_before[0]));
                cohort_started_at_ns = mono_ns();
                atomic_store_explicit(&start, 1, memory_order_release);
                for (j = 0; j < workers_started; ++j)
                    if (pthread_join(worker_threads[j], NULL) != 0)
                        atomic_store_explicit(
                            &cohort_ok, 0, memory_order_relaxed);
            }
        }
        cohort_ended_at_ns = mono_ns();
        scheduler_snapshot_all(
            scheduler_after,
            sizeof(scheduler_after) / sizeof(scheduler_after[0]));
        if (cohort_started_at_ns == 0 ||
            cohort_ended_at_ns <= cohort_started_at_ns ||
            atomic_load_explicit(
                &llm_server.requests, memory_order_relaxed) !=
                initial_llm_requests + bench_turns ||
            atomic_load_explicit(&server.requests, memory_order_relaxed) !=
                initial_tts_requests + bench_turns * 3)
            atomic_store_explicit(&cohort_ok, 0, memory_order_relaxed);
        cohort_pass = atomic_load_explicit(&cohort_ok, memory_order_relaxed);
        expect("warm authenticated model cohort", cohort_pass);
        if (cohort_pass) {
            size_t p50_index =
                ((size_t)bench_turns * 50u + 99u) / 100u - 1u;
            size_t p95_index =
                ((size_t)bench_turns * 95u + 99u) / 100u - 1u;
            cohort_elapsed_us =
                (cohort_ended_at_ns - cohort_started_at_ns) / UINT64_C(1000);
            throughput = (double)bench_turns * 1000000000.0 /
                (double)(cohort_ended_at_ns - cohort_started_at_ns);
            qsort(
                metrics.client,
                (size_t)bench_turns,
                sizeof(metrics.client[0]),
                compare_u64);
            qsort(
                metrics.request_write,
                (size_t)bench_turns,
                sizeof(metrics.request_write[0]),
                compare_u64);
            qsort(
                metrics.request_to_llm,
                (size_t)bench_turns,
                sizeof(metrics.request_to_llm[0]),
                compare_u64);
            qsort(
                metrics.llm_provider,
                (size_t)bench_turns,
                sizeof(metrics.llm_provider[0]),
                compare_u64);
            qsort(
                metrics.llm_to_tts,
                (size_t)bench_turns,
                sizeof(metrics.llm_to_tts[0]),
                compare_u64);
            qsort(
                metrics.tts_provider,
                (size_t)bench_turns,
                sizeof(metrics.tts_provider[0]),
                compare_u64);
            qsort(
                metrics.tts_to_pcm,
                (size_t)bench_turns,
                sizeof(metrics.tts_to_pcm[0]),
                compare_u64);
            qsort(
                metrics.edge_auth,
                (size_t)bench_turns,
                sizeof(metrics.edge_auth[0]),
                compare_u64);
            qsort(
                metrics.edge_admission,
                (size_t)bench_turns,
                sizeof(metrics.edge_admission[0]),
                compare_u64);
            qsort(
                metrics.edge_capability,
                (size_t)bench_turns,
                sizeof(metrics.edge_capability[0]),
                compare_u64);
            qsort(
                metrics.edge_encode,
                (size_t)bench_turns,
                sizeof(metrics.edge_encode[0]),
                compare_u64);
            qsort(
                metrics.edge_publish,
                (size_t)bench_turns,
                sizeof(metrics.edge_publish[0]),
                compare_u64);
            qsort(
                metrics.edge_prepare,
                (size_t)bench_turns,
                sizeof(metrics.edge_prepare[0]),
                compare_u64);
            printf(
                "BenchmarkHTTP_WarmCohort\tturns=%d\tconcurrency=%d\t"
                "tts_sample_rate_hz=%u\ttts_frame_bytes=%u\t"
                "tts_frames_per_segment=%d\t"
                "elapsed_us=%llu\tthroughput_tps=%.2f\t"
                "client_p50_us=%llu\tclient_p95_us=%llu\t"
                "request_write_p50_us=%llu\trequest_write_p95_us=%llu\n",
                bench_turns,
                bench_concurrency,
                SERVICE_E2E_PRODUCTION_TTS_SAMPLE_RATE,
                SERVICE_E2E_PRODUCTION_TTS_FRAME_BYTES,
                bench_tts_frames,
                (unsigned long long)cohort_elapsed_us,
                throughput,
                (unsigned long long)metrics.client[p50_index],
                (unsigned long long)metrics.client[p95_index],
                (unsigned long long)metrics.request_write[p50_index],
                (unsigned long long)metrics.request_write[p95_index]);
            printf(
                "BenchmarkHTTP_WarmStages\tturns=%d\tconcurrency=%d\t"
                "request_to_llm_p50_us=%llu\trequest_to_llm_p95_us=%llu\t"
                "llm_provider_p50_us=%llu\tllm_provider_p95_us=%llu\t"
                "llm_to_tts_p50_us=%llu\tllm_to_tts_p95_us=%llu\t"
                "tts_provider_p50_us=%llu\ttts_provider_p95_us=%llu\t"
                "tts_to_pcm_p50_us=%llu\ttts_to_pcm_p95_us=%llu\n",
                bench_turns,
                bench_concurrency,
                (unsigned long long)metrics.request_to_llm[p50_index],
                (unsigned long long)metrics.request_to_llm[p95_index],
                (unsigned long long)metrics.llm_provider[p50_index],
                (unsigned long long)metrics.llm_provider[p95_index],
                (unsigned long long)metrics.llm_to_tts[p50_index],
                (unsigned long long)metrics.llm_to_tts[p95_index],
                (unsigned long long)metrics.tts_provider[p50_index],
                (unsigned long long)metrics.tts_provider[p95_index],
                (unsigned long long)metrics.tts_to_pcm[p50_index],
                (unsigned long long)metrics.tts_to_pcm[p95_index]);
            printf(
                "BenchmarkHTTP_WarmEdge\tturns=%d\tconcurrency=%d\t"
                "auth_p50_us=%llu\tauth_p95_us=%llu\t"
                "admission_p50_us=%llu\tadmission_p95_us=%llu\t"
                "capability_p50_us=%llu\tcapability_p95_us=%llu\t"
                "encode_p50_us=%llu\tencode_p95_us=%llu\t"
                "publish_p50_us=%llu\tpublish_p95_us=%llu\t"
                "prepare_p50_us=%llu\tprepare_p95_us=%llu\n",
                bench_turns,
                bench_concurrency,
                (unsigned long long)metrics.edge_auth[p50_index],
                (unsigned long long)metrics.edge_auth[p95_index],
                (unsigned long long)metrics.edge_admission[p50_index],
                (unsigned long long)metrics.edge_admission[p95_index],
                (unsigned long long)metrics.edge_capability[p50_index],
                (unsigned long long)metrics.edge_capability[p95_index],
                (unsigned long long)metrics.edge_encode[p50_index],
                (unsigned long long)metrics.edge_encode[p95_index],
                (unsigned long long)metrics.edge_publish[p50_index],
                (unsigned long long)metrics.edge_publish[p95_index],
                (unsigned long long)metrics.edge_prepare[p50_index],
                (unsigned long long)metrics.edge_prepare[p95_index]);
            scheduler_report(
                scheduler_before,
                scheduler_after,
                scheduler_child_count);
        }
    }

    vbus_close(expiry_client);
    vbus_close(request_client);
    vbus_close(client);
    expect("service processes exit cleanly", stop_children() == 0);
    close(listen_fd);
    close(stt_listen_fd);
    close(embed_listen_fd);
    close(llm_listen_fd);
    close(search_listen_fd);
    pthread_join(server_thread, NULL);
    pthread_join(stt_server_thread, NULL);
    pthread_join(embed_server_thread, NULL);
    pthread_join(search_server_thread, NULL);
    pthread_join(llm_server_thread, NULL);
    unlink(socket_path);
    expect(
        "fake TTS requests",
        atomic_load_explicit(&server.requests, memory_order_relaxed) ==
            server.expected_requests);
    expect(
        "canonical TTS HTTP requests",
        atomic_load_explicit(&server.valid_requests, memory_order_relaxed) ==
            server.expected_requests);
    expect(
        "TTS HTTP keeps escaped model text",
        atomic_load_explicit(
            &server.escaped_quote_requests, memory_order_relaxed) > 0 &&
            atomic_load_explicit(
                &server.escaped_backslash_requests, memory_order_relaxed) > 0);
    expect(
        "persistent TTS lanes reuse backend connections",
        atomic_load_explicit(&server.keepalive_requests, memory_order_relaxed) ==
            server.expected_requests &&
        atomic_load_explicit(&server.reused_requests, memory_order_relaxed) > 0 &&
        atomic_load_explicit(&server.accepts, memory_order_relaxed) <
            server.expected_requests);
    expect(
        "fake STT requests",
        atomic_load_explicit(&stt_server.requests, memory_order_relaxed) == 7);
    expect(
        "canonical STT HTTP requests",
        atomic_load_explicit(&stt_server.valid_requests, memory_order_relaxed) == 7);
    expect(
        "persistent STT reuses then reconnects",
        atomic_load_explicit(&stt_server.accepts, memory_order_relaxed) == 5 &&
        atomic_load_explicit(
            &stt_server.keepalive_requests, memory_order_relaxed) == 7);
    expect(
        "fake embedding requests",
        atomic_load_explicit(&embed_server.requests, memory_order_relaxed) == 13 &&
        atomic_load_explicit(
            &embed_server.valid_requests, memory_order_relaxed) == 13);
    expect(
        "persistent RAG reuses then reconnects",
        atomic_load_explicit(&embed_server.accepts, memory_order_relaxed) == 12 &&
        atomic_load_explicit(
            &embed_server.keepalive_requests, memory_order_relaxed) == 13);
    expect("Milvus receives filtered authenticated searches",
        atomic_load_explicit(&search_server.requests, memory_order_relaxed) == 13 &&
        atomic_load_explicit(&search_server.valid_requests, memory_order_relaxed) == 13);
    expect("Milvus connection reuse remains bounded",
        atomic_load_explicit(&search_server.accepts, memory_order_relaxed) == 12 &&
        atomic_load_explicit(&search_server.keepalive_requests, memory_order_relaxed) == 13);
    expect(
        "fake LLM SSE requests",
        atomic_load_explicit(&llm_server.requests, memory_order_relaxed) >=
            19 + bench_turns &&
        atomic_load_explicit(&llm_server.requests, memory_order_relaxed) <=
            20 + bench_turns);
    expect(
        "canonical LLM SSE HTTP requests",
        atomic_load_explicit(&llm_server.valid_requests, memory_order_relaxed) ==
        atomic_load_explicit(&llm_server.requests, memory_order_relaxed));
    expect(
        "warm cascade job reuse preserves prompt binding",
        bench_turns == 0 ||
        (atomic_load_explicit(
             &llm_server.benchmark_requests, memory_order_relaxed) == bench_turns &&
         atomic_load_explicit(
             &llm_server.benchmark_binding_failures,
             memory_order_relaxed) == 0));
    expect(
        "persistent LLM lanes reuse backend connections",
        atomic_load_explicit(
            &llm_server.keepalive_requests, memory_order_relaxed) ==
            atomic_load_explicit(&llm_server.requests, memory_order_relaxed) &&
        atomic_load_explicit(
            &llm_server.reused_requests, memory_order_relaxed) > 0);
    if (failures) return 1;
    printf("ALL PASS service process e2e\n");
    return 0;
}
