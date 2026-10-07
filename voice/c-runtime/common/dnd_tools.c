#define _POSIX_C_SOURCE 200809L
#include "dnd_tools.h"

#include "cmp_json.h"
#include "dice.h"
#include "http_min.h"
#include "toolstore.h"
#include "voice_ascii.h"
#include "voice_auth.h"

#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DND_TOOL_PATH "/v1/tools/execute"

static int64_t clock_ms(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int sha256_hex(const char *data, char output[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t i;
    if (!SHA256((const unsigned char *)data, strlen(data), digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i) {
        output[2u * i] = digits[digest[i] >> 4u];
        output[2u * i + 1u] = digits[digest[i] & 15u];
    }
    output[64] = '\0';
    return 0;
}

static int take_prefix(const char **cursor, const char *prefix) {
    size_t length = strlen(prefix);
    if (strncmp(*cursor, prefix, length) != 0) return 0;
    *cursor += length;
    return 1;
}

int dnd_dice_intent(const char *text, char expression[64]) {
    char normalized[2048], initial[64], error[DICE_MSG];
    const char *cursor;
    dice_spec spec;
    size_t length, i, used = 0;
    int advantage = 0, added = 0, written;
    if (!text || !expression) return -1;
    expression[0] = '\0';
    length = strlen(text);
    if (length >= sizeof(normalized)) return -1;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        normalized[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
    }
    normalized[length] = '\0';
    cursor = normalized;
    while (*cursor == ' ') ++cursor;
    (void)(take_prefix(&cursor, "please ") || take_prefix(&cursor, "can you ") ||
           take_prefix(&cursor, "could you ") || take_prefix(&cursor, "would you "));
    if (!take_prefix(&cursor, "roll ")) return strcmp(cursor, "roll") == 0 ? -1 : 0;
    while (*cursor == ' ') ++cursor;
    if (*cursor == 'd') initial[used++] = '1';
    while ((*cursor >= '0' && *cursor <= '9') || *cursor == 'd' || *cursor == 'k' ||
           *cursor == 'h' || *cursor == 'l' || *cursor == '+' || *cursor == '-') {
        if (used + 1u >= sizeof(initial)) return -1;
        initial[used++] = *cursor++;
    }
    initial[used] = '\0';
    if (dice_parse(initial, &spec, error, sizeof(error)) != DICE_OK) return -1;
    for (;;) {
        int sign = 0;
        while (*cursor == ' ') ++cursor;
        if (!*cursor || *cursor == '.' || *cursor == '!' || *cursor == '?' ||
            take_prefix(&cursor, "for ")) break;
        if (take_prefix(&cursor, "with advantage") || take_prefix(&cursor, "with disadvantage")) {
            /* Both phrases end in advantage; the longer one includes "dis". */
            int disadvantage = cursor - normalized >= 17 &&
                memcmp(cursor - 17, "with disadvantage", 17u) == 0;
            if (advantage || spec.keep[0] || spec.count > 2) return -1;
            advantage = 1;
            spec.count = 2;
            memcpy(spec.keep, disadvantage ? "kl1" : "kh1", 4u);
        } else if (take_prefix(&cursor, "and add ") || take_prefix(&cursor, "plus ")) sign = 1;
        else if (take_prefix(&cursor, "and subtract ") || take_prefix(&cursor, "minus ")) sign = -1;
        else return -1;
        if (sign) {
            int value = 0;
            if (added || spec.modifier || *cursor < '0' || *cursor > '9') return -1;
            do {
                value = value * 10 + (*cursor++ - '0');
                if (value > 10000) return -1;
            } while (*cursor >= '0' && *cursor <= '9');
            spec.modifier = sign * value;
            added = 1;
        }
    }
    if (spec.modifier)
        written = snprintf(expression, 64u, "%dd%d%s%+d", spec.count, spec.sides, spec.keep, spec.modifier);
    else written = snprintf(expression, 64u, "%dd%d%s", spec.count, spec.sides, spec.keep);
    return written > 0 && written < 64 ? 1 : -1;
}

int dnd_tools_config_valid(const char *url, const char *secret) {
    http_min_url parsed;
    return url && secret && strlen(secret) >= 32u && strlen(secret) <= 255u &&
        http_min_parse_url(url, &parsed) == HTTP_MIN_OK && strcmp(parsed.path, DND_TOOL_PATH) == 0;
}

static const cmp_json_field *member(const cmp_json_object *object, const char *name) {
    size_t i, length = strlen(name);
    if (cmp_json_object_key_count(object, name) != 1) return NULL;
    for (i = 0; i < object->field_count; ++i)
        if (object->fields[i].key_len == length &&
            memcmp(object->fields[i].key, name, length) == 0) return &object->fields[i];
    return NULL;
}

static int string_matches(const cmp_json_object *object, const char *name, const char *expected) {
    char text[256];
    return cmp_json_object_str(object, name, text, sizeof(text)) && strcmp(text, expected) == 0;
}

static int number_array(const cmp_json_object *object, const char *name,
                        int values[DICE_MAX_COUNT], int *count) {
    const cmp_json_field *field = member(object, name);
    const char *cursor, *end;
    *count = 0;
    if (!field || field->value_len < 2u || field->value[0] != '[') return -1;
    cursor = field->value + 1u;
    end = field->value + field->value_len;
    while (cursor < end) {
        int value = 0;
        while (cursor < end && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) ++cursor;
        if (cursor == end || *cursor < '0' || *cursor > '9' || *count == DICE_MAX_COUNT) return -1;
        do {
            value = value * 10 + (*cursor++ - '0');
            if (value > 1000) return -1;
        } while (cursor < end && *cursor >= '0' && *cursor <= '9');
        values[(*count)++] = value;
        while (cursor < end && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) ++cursor;
        if (cursor < end && *cursor == ']') return cursor + 1 == end ? 0 : -1;
        if (cursor == end || *cursor++ != ',') return -1;
    }
    return -1;
}

static int append_numbers(char *text, size_t capacity, size_t *used, const int *values, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        int length = snprintf(text + *used, capacity - *used, "%s%d", i ? ", " : "", values[i]);
        if (length < 0 || (size_t)length >= capacity - *used) return -1;
        *used += (size_t)length;
    }
    return 0;
}

int dnd_dice_output_text(const char *json, const char *expression,
                         const char *call_id, char *text, size_t capacity) {
    cmp_json_object output;
    dice_spec spec;
    char error[DICE_MSG];
    int rolls[DICE_MAX_COUNT], indexes[DICE_MAX_COUNT], kept[DICE_MAX_COUNT], expected[DICE_MAX_COUNT];
    int count, index_count, kept_count, expected_count = 0, i, total = 0, length;
    int64_t returned_total, modifier;
    size_t used;
    if (!json || !expression || !call_id || !text || !capacity) return -1;
    text[0] = '\0';
    if (dice_parse(expression, &spec, error, sizeof(error)) != DICE_OK ||
        !cmp_json_object_parse(json, &output) || output.field_count != 10u ||
        !string_matches(&output, "expression", spec.expression) ||
        !string_matches(&output, "roll_id", call_id) ||
        !string_matches(&output, "visibility", "public") ||
        !string_matches(&output, "mode", dice_mode(spec.keep)) ||
        !string_matches(&output, "entropy_source", "getrandom") ||
        !cmp_json_object_i64(&output, "modifier", &modifier) || modifier != spec.modifier ||
        !cmp_json_object_i64(&output, "total", &returned_total) ||
        number_array(&output, "rolls", rolls, &count) != 0 || count != spec.count ||
        number_array(&output, "kept_indexes", indexes, &index_count) != 0 ||
        number_array(&output, "kept_rolls", kept, &kept_count) != 0 || kept_count != index_count)
        return -1;
    for (i = 0; i < count; ++i) if (rolls[i] < 1 || rolls[i] > spec.sides) return -1;
    if (dice_select_indexes(rolls, count, spec.keep, expected, &expected_count) != DICE_OK ||
        index_count != expected_count) return -1;
    for (i = 0; i < index_count; ++i) {
        if (indexes[i] != expected[i] || kept[i] != rolls[expected[i]]) return -1;
        total += kept[i];
    }
    total += spec.modifier;
    if (returned_total != total) return -1;
    length = snprintf(text, capacity, "You rolled %s%s%s: ", expression,
                       spec.keep[0] ? " with " : "", spec.keep[0] ? dice_mode(spec.keep) : "");
    if (length < 0 || (size_t)length >= capacity) return -1;
    used = (size_t)length;
    if (append_numbers(text, capacity, &used, rolls, count) != 0 || capacity - used <= 7u) return -1;
    memcpy(text + used, ", kept ", 7u);
    used += 7u;
    if (append_numbers(text, capacity, &used, kept, kept_count) != 0) return -1;
    length = snprintf(text + used, capacity - used, ", with modifier %+d, for a total of %d.", spec.modifier, total);
    return length >= 0 && (size_t)length < capacity - used ? 0 : -1;
}

static int tools_execute_buffered(const char *url, const char *secret,
                      const char *request_id, const char *user_id,
                      const char *session_id, const char *prompt,
                      const char *campaign_id, const char *encounter_id, const char *scene_id,
                      const dnd_initiative_request_c *initiative,
                      const dnd_campaign_request_c *campaign_request,
                      const dnd_encounter_action_c *action,
                      int64_t deadline_ms, int timeout_ms,
                      const atomic_int *canceled, dnd_tool_result *result,
                      char *response, size_t response_capacity) {
    char expression[64], identity[12288], call_hash[65], idem[65], input[8192], escaped[16384];
    char request[16384], output[65536], digest[65], uri[80];
    const char *kind = scene_id ? "scene" : encounter_id ? "encounter" : campaign_id ? "campaign" : "dice";
    const char *tool = scene_id ? "dnd-scene-presence" : encounter_id ? "dnd-encounter-state" : campaign_id ? "dnd-campaign-state" : "dnd-dice-roll";
    char timestamp[32], nonce[33], signature[65], response_signature[65];
    cmp_json_object envelope, call;
    const cmp_json_field *call_wire;
    http_min_header headers[] = {
        {"X-Tool-User", user_id}, {"X-Tool-Timestamp", timestamp},
        {"X-Tool-Nonce", nonce}, {"X-Tool-Signature", signature}
    };
    size_t response_length = 0;
    int accepted, status = 0, length, rc;
    int64_t now = clock_ms(CLOCK_REALTIME), started = clock_ms(CLOCK_MONOTONIC);
    if (!result) return DND_TOOL_INVALID;
    memset(result, 0, sizeof(*result));
    if (canceled && atomic_load_explicit(canceled, memory_order_relaxed)) return DND_TOOL_CANCELED;
    if (!dnd_tools_config_valid(url, secret)) return DND_TOOL_UNAVAILABLE;
    if (!ts_valid_id(request_id) || !ts_valid_id(user_id) || !ts_valid_id(session_id) ||
        strlen(user_id) > 127u ||
        timeout_ms < 1 || timeout_ms > 60000 || now <= 0 || started <= 0) return DND_TOOL_INVALID;
    if (campaign_id) {
        if (!dnd_request_id_valid(campaign_id, strlen(campaign_id) + 1u) ||
            (encounter_id && !dnd_request_id_valid(encounter_id, strlen(encounter_id) + 1u))) return DND_TOOL_INVALID;
        if (scene_id) {
            if (encounter_id || action || initiative || campaign_request) return DND_TOOL_INVALID;
            length = dnd_scene_input(prompt, campaign_id, scene_id, input, sizeof(input));
        } else if (action) {
            if (initiative || campaign_request || !encounter_id || dnd_action_intent(prompt) != (int)action->operation ||
                !dnd_encounter_action_valid(action)) return DND_TOOL_INVALID;
            length = dnd_action_input(action, campaign_id, encounter_id, input, sizeof(input));
        } else if (campaign_request) {
            if (initiative || encounter_id || dnd_campaign_intent(prompt) != (int)campaign_request->operation ||
                !dnd_campaign_request_valid(campaign_request)) return DND_TOOL_INVALID;
            length = dnd_campaign_input(campaign_request, campaign_id, user_id, input, sizeof(input));
        } else if (initiative) {
            if (!encounter_id || !dnd_initiative_intent(prompt) || !dnd_initiative_request_valid(initiative) ||
                initiative->expected_version >= DND_EXACT_VERSION_MAX) return DND_TOOL_INVALID;
            length = snprintf(input, sizeof(input), "{\"operation\":\"roll_initiative\",\"campaign_id\":\"%s\","
                "\"encounter_id\":\"%s\",\"expected_version\":%lld,\"campaign_version\":%lld,\"initiative\":[",
                campaign_id, encounter_id, (long long)initiative->expected_version, (long long)initiative->campaign_version);
            if (length <= 0 || (size_t)length >= sizeof(input)) return DND_TOOL_INVALID;
            size_t used = (size_t)length;
            for (size_t i = 0; i < initiative->count; ++i) {
                length = snprintf(input + used, sizeof(input) - used, "%s{\"character_id\":\"%s\",\"expression\":\"%s\"}",
                    i ? "," : "", initiative->selections[i].character_id, initiative->selections[i].expression);
                if (length <= 0 || (size_t)length >= sizeof(input) - used) return DND_TOOL_INVALID;
                used += (size_t)length;
            }
            if (sizeof(input) - used < 3u) return DND_TOOL_INVALID;
            memcpy(input + used, "]}", 3u);
            length = (int)(used + 2u);
        } else if (encounter_id) {
            if (!dnd_encounter_intent(prompt)) return DND_TOOL_INVALID;
            length = snprintf(input, sizeof(input), "{\"operation\":\"get\",\"campaign_id\":\"%s\",\"encounter_id\":\"%s\"}",
                campaign_id, encounter_id);
        } else {
            if (!dnd_roster_intent(prompt)) return DND_TOOL_INVALID;
            length = snprintf(input, sizeof(input), "{\"operation\":\"get_roster\",\"campaign_id\":\"%s\"}", campaign_id);
        }
    } else {
        if (scene_id || initiative || campaign_request || action || encounter_id || dnd_dice_intent(prompt, expression) != 1) return DND_TOOL_INVALID;
        length = snprintf(input, sizeof(input), "{\"expression\":\"%s\"}", expression);
    }
    if (length <= 0 || (size_t)length >= sizeof(input) ||
        cmp_json_escape_exact(input, escaped, sizeof(escaped))) return DND_TOOL_INVALID;
    if (deadline_ms <= now) return DND_TOOL_DEADLINE;
    if (deadline_ms - now > timeout_ms) deadline_ms = now + timeout_ms;
    timeout_ms = (int)(deadline_ms - now);
    if (voice_auth_random_nonce(nonce) != VOICE_AUTH_OK) return DND_TOOL_INVALID;
    /* Presence asks for a fresh read on every attempt. A reused transport ID
     * must not turn an immutable old read receipt into current scene evidence.
     * Mutating tools retain their existing durable operation identities. */
    length = snprintf(identity, sizeof(identity), "dnd-%s-call-v1\n%s\n%s\n%s%s%s", kind, user_id, session_id,
        request_id, scene_id ? "\n" : "", scene_id ? nonce : "");
    if (length <= 0 || (size_t)length >= sizeof(identity) || sha256_hex(identity, call_hash) != 0) return DND_TOOL_INVALID;
    (void)snprintf(result->call_id, sizeof(result->call_id), "%s-%s", kind, call_hash);
    length = snprintf(identity, sizeof(identity), "dnd-%s-input-v1\n%s\n%s%s%s", kind, call_hash, prompt,
        campaign_id ? "\n" : "", campaign_id ? input : "");
    if (length <= 0 || (size_t)length >= sizeof(identity) || sha256_hex(identity, idem) != 0) return DND_TOOL_INVALID;
    length = snprintf(request, sizeof(request),
        "{\"tool_call_id\":\"%s\",\"idempotency_key\":\"%s\",\"parent_turn_id\":\"%s\","
        "\"session_id\":\"%s\",\"agent_id\":\"dnd-agent\",\"tool_id\":\"%s\","
        "\"input_json\":\"%s\",\"deadline_unix_ms\":%lld}",
        result->call_id, idem, request_id, session_id, tool, escaped, (long long)deadline_ms);
    (void)snprintf(timestamp, sizeof(timestamp), "%lld", (long long)(now / 1000));
    if (length <= 0 || (size_t)length >= sizeof(request) ||
        voice_auth_sign(secret, strlen(secret), "POST", DND_TOOL_PATH, user_id, now / 1000,
                         nonce, (const uint8_t *)request, (size_t)length, signature) != VOICE_AUTH_OK) return DND_TOOL_INVALID;
    rc = http_min_post_headers_cancel(url, "application/json", headers,
        sizeof(headers) / sizeof(headers[0]), (const uint8_t *)request, (size_t)length,
        (uint8_t *)response, response_capacity - 1u, &response_length, &status, timeout_ms, canceled);
    if (rc == HTTP_MIN_ERR_CANCELED ||
        (canceled && atomic_load_explicit(canceled, memory_order_relaxed))) return DND_TOOL_CANCELED;
    if (rc != HTTP_MIN_OK || status != 200) return DND_TOOL_UNAVAILABLE;
    if (clock_ms(CLOCK_REALTIME) >= deadline_ms) return DND_TOOL_DEADLINE;
    if (memchr(response, '\0', response_length)) return DND_TOOL_INVALID;
    response[response_length] = '\0';
    if (!cmp_json_object_parse(response, &envelope) || envelope.field_count != 3u ||
        !cmp_json_object_bool(&envelope, "accepted", &accepted) || !accepted ||
        !(call_wire = member(&envelope, "call")) ||
        !cmp_json_object_str(&envelope, "signature", response_signature, sizeof(response_signature)) ||
        voice_auth_verify(secret, strlen(secret), "RESULT", DND_TOOL_PATH, user_id, now / 1000,
            nonce, (const uint8_t *)call_wire->value, call_wire->value_len, response_signature) != VOICE_AUTH_OK ||
        !cmp_json_object_object(&envelope, "call", &call) ||
        !string_matches(&call, "tool_call_id", result->call_id) ||
        !string_matches(&call, "idempotency_key", idem) ||
        !string_matches(&call, "parent_turn_id", request_id) ||
        !string_matches(&call, "parent_task_id", "") ||
        !string_matches(&call, "user_id", user_id) ||
        !string_matches(&call, "session_id", session_id) ||
        !string_matches(&call, "agent_id", "dnd-agent") ||
        !string_matches(&call, "tool_id", tool) ||
        !cmp_json_object_str(&call, "input_json", escaped, sizeof(escaped)) || strcmp(escaped, input) ||
        !string_matches(&call, "state", "completed") || !string_matches(&call, "error", "") ||
        !cmp_json_object_str(&call, "output_json", output, sizeof(output)) ||
        sha256_hex(output, digest) != 0 || !string_matches(&call, "output_sha256", digest)) return DND_TOOL_INVALID;
    (void)snprintf(uri, sizeof(uri), "sha256:%s", digest);
    if (!string_matches(&call, "output_artifact", uri)) return DND_TOOL_INVALID;
    memcpy(result->output_sha256, digest, sizeof(digest));
    if (scene_id) rc = dnd_scene_output(output, prompt, campaign_id, scene_id, result);
    else if (action) rc = dnd_action_output(output, campaign_id, encounter_id, action, result);
    else if (campaign_request) rc = dnd_campaign_output(output, campaign_id, user_id, campaign_request, result);
    else if (initiative) rc = dnd_initiative_output(output, campaign_id, encounter_id, initiative, result);
    else if (encounter_id) rc = dnd_encounter_output(output, campaign_id, encounter_id, result);
    else if (campaign_id) rc = dnd_roster_output(output, campaign_id, result);
    else rc = dnd_dice_output_text(output, expression, result->call_id, result->text, sizeof(result->text));
    if (rc) return DND_TOOL_INVALID;
    (void)snprintf(result->tool_id, sizeof(result->tool_id), "%s", tool);
    result->elapsed_ms = clock_ms(CLOCK_MONOTONIC) - started;
    return result->elapsed_ms >= 0 ? DND_TOOL_OK : DND_TOOL_INVALID;
}

static int tools_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, const char *scene_id, const dnd_initiative_request_c *initiative,
    const dnd_campaign_request_c *campaign_request,
    const dnd_encounter_action_c *action,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result) {
    /* One bounded worker allocation holds the escaped signed state record.
     * Ordinary dice keeps its existing response limit. No allocation is in a Process kernel. */
    size_t capacity = campaign_id ? 524288u : 49152u;
    char *response;
    int rc;
    if (!result) return DND_TOOL_INVALID;
    response = malloc(capacity);
    if (!response) { memset(result, 0, sizeof(*result)); return DND_TOOL_UNAVAILABLE; }
    rc = tools_execute_buffered(url, secret, request_id, user_id, session_id, prompt,
        campaign_id, encounter_id, scene_id, initiative, campaign_request, action, deadline_ms, timeout_ms, canceled, result, response, capacity);
    free(response);
    if (rc != DND_TOOL_OK) {
        result->encounter_length = result->roster_length = result->initiative_length = 0;
        result->text[0] = '\0';
    }
    return rc;
}

int dnd_tools_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result) {
    return tools_execute(url, secret, request_id, user_id, session_id, prompt,
        NULL, NULL, NULL, NULL, NULL, NULL, deadline_ms, timeout_ms, canceled, result);
}

int dnd_encounter_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id || !encounter_id) return DND_TOOL_INVALID;
    return tools_execute(url, secret, request_id, user_id, session_id, prompt,
        campaign_id, encounter_id, NULL, NULL, NULL, NULL, deadline_ms, timeout_ms, canceled, result);
}

int dnd_roster_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id) return DND_TOOL_INVALID;
    return tools_execute(url, secret, request_id, user_id, session_id, prompt,
        campaign_id, NULL, NULL, NULL, NULL, NULL, deadline_ms, timeout_ms, canceled, result);
}

int dnd_initiative_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, const dnd_initiative_request_c *initiative,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id || !encounter_id || !ts_valid_id(request_id) ||
        !dnd_initiative_request_valid(initiative)) return DND_TOOL_INVALID;
    /* The durable operation outlives a one-use gateway transport request. */
    return tools_execute(url, secret, initiative->operation_id, user_id, session_id, prompt,
        campaign_id, encounter_id, NULL, initiative, NULL, NULL, deadline_ms, timeout_ms, canceled, result);
}

int dnd_campaign_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const dnd_campaign_request_c *request,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id || !ts_valid_id(request_id) || !dnd_campaign_request_valid(request)) return DND_TOOL_INVALID;
    return tools_execute(url, secret, request->operation_id, user_id, session_id, prompt,
        campaign_id, NULL, NULL, NULL, request, NULL, deadline_ms, timeout_ms, canceled, result);
}

int dnd_action_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *encounter_id, const dnd_encounter_action_c *request,
    int64_t deadline_ms, int timeout_ms, const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id || !encounter_id || !ts_valid_id(request_id) || !dnd_encounter_action_valid(request)) return DND_TOOL_INVALID;
    return tools_execute(url, secret, request->operation_id, user_id, session_id, prompt,
        campaign_id, encounter_id, NULL, NULL, NULL, request, deadline_ms, timeout_ms, canceled, result);
}

int dnd_scene_execute(const char *url, const char *secret,
    const char *request_id, const char *user_id, const char *session_id, const char *prompt,
    const char *campaign_id, const char *scene_id, int64_t deadline_ms, int timeout_ms,
    const atomic_int *canceled, dnd_tool_result *result) {
    if (!campaign_id || !scene_id) return DND_TOOL_INVALID;
    return tools_execute(url, secret, request_id, user_id, session_id, prompt,
        campaign_id, NULL, scene_id, NULL, NULL, NULL, deadline_ms, timeout_ms, canceled, result);
}
