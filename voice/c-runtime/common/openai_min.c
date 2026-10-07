/* openai_min.c — bounded model-I/O JSON build and extraction. */

#include "openai_min.h"
#include "utf8.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int json_escape_string_impl(
    const char *in,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    size_t o = 0;
    size_t i;
    if (!in || !out || out_cap == 0 || !out_len) return -1;
    *out_len = 0;
    for (i = 0; in[i]; ++i) {
        unsigned char c = (unsigned char)in[i];
        const char *esc = NULL;
        if (c == '"' || c == '\\') {
            if (out_cap - o <= 2) return -1;
            out[o++] = '\\';
            out[o++] = (char)c;
            continue;
        }
        if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        if (esc) {
            size_t el = strlen(esc);
            if (el >= out_cap - o) return -1;
            memcpy(out + o, esc, el);
            o += el;
            continue;
        }
        if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            if (out_cap - o <= 6) return -1;
            out[o++] = '\\';
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
            continue;
        }
        if (out_cap - o <= 1) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    *out_len = o;
    return 0;
}

int json_escape_string(const char *in, char *out, size_t out_cap, size_t *out_len) {
    return json_escape_string_impl(in, out, out_cap, out_len);
}

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} json_request_writer;

#define JSON_LONG_ESCAPED_INPUT 256u
#define JSON_IDENTIFIER_ESCAPED_LIMIT 767u
#define JSON_SHORT_PREFIX_SCAN 16u

static int json_span_word_has_zero_byte(uint64_t value) {
    const uint64_t ones = UINT64_C(0x0101010101010101);
    const uint64_t high_bits = UINT64_C(0x8080808080808080);
    return ((value - ones) & ~value & high_bits) != 0u;
}

static int json_span_word_is_plain(uint64_t value) {
    const uint64_t control_mask = UINT64_C(0xe0e0e0e0e0e0e0e0);
    const uint64_t quotes = UINT64_C(0x2222222222222222);
    const uint64_t backslashes = UINT64_C(0x5c5c5c5c5c5c5c5c);
    return !json_span_word_has_zero_byte(value & control_mask) &&
        !json_span_word_has_zero_byte(value ^ quotes) &&
        !json_span_word_has_zero_byte(value ^ backslashes);
}

static size_t json_plain_prefix_span(const char *input, size_t input_len) {
    size_t offset = 0u;
    while (input_len - offset >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, input + offset, sizeof(word));
        if (!json_span_word_is_plain(word)) break;
        offset += sizeof(word);
    }
    while (offset < input_len) {
        unsigned char value = (unsigned char)input[offset];
        if (value < 0x20u || value == '"' || value == '\\') break;
        offset++;
    }
    return offset;
}

static size_t json_short_plain_prefix(const char *input, const char *escapes) {
    const unsigned char *cursor = (const unsigned char *)input;
    const unsigned char *start = cursor;
    while ((size_t)(cursor - start) < JSON_SHORT_PREFIX_SCAN &&
           *cursor >= 0x20u && *cursor != '"' && *cursor != '\\') cursor++;
    if ((size_t)(cursor - start) < JSON_SHORT_PREFIX_SCAN || *cursor == '\0')
        return (size_t)(cursor - start);
    return JSON_SHORT_PREFIX_SCAN +
        strcspn((const char *)cursor, escapes);
}

static int json_request_append(
    json_request_writer *writer,
    const char *data,
    size_t data_len
) {
    if (!writer || !data || writer->length >= writer->capacity ||
        data_len >= writer->capacity - writer->length) return -1;
    memcpy(writer->data + writer->length, data, data_len);
    writer->length += data_len;
    return 0;
}

static int json_request_append_mixed_escaped(
    json_request_writer *writer,
    const char *input,
    size_t plain_len,
    size_t escaped_limit,
    const char *escapes
) {
    static const char hex[] = "0123456789abcdef";
    char *output;
    size_t available;
    size_t consumed;
    size_t written;
    if (!writer || !input || !escapes ||
        writer->length >= writer->capacity) return -1;
    output = writer->data + writer->length;
    available = writer->capacity - writer->length;
    if (plain_len > escaped_limit || plain_len >= available) return -1;
    memcpy(output, input, plain_len);
    consumed = plain_len;
    written = plain_len;
    while (input[consumed] != '\0') {
        unsigned char c = (unsigned char)input[consumed++];
        size_t escaped_bytes = c < 0x20u ? 6u : 2u;
        size_t next_plain;
        if (escaped_bytes > escaped_limit - written ||
            escaped_bytes >= available - written) return -1;
        output[written++] = '\\';
        if (c == '"' || c == '\\') {
            output[written++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t' ||
                   c == '\b' || c == '\f') {
            output[written++] = c == '\n' ? 'n' : c == '\r' ? 'r' :
                c == '\t' ? 't' : c == '\b' ? 'b' : 'f';
        } else if (c < 0x20u) {
            output[written++] = 'u';
            output[written++] = '0';
            output[written++] = '0';
            output[written++] = hex[c >> 4];
            output[written++] = hex[c & 0x0fu];
        } else {
            return -1;
        }
        if (input[consumed] == '\0' ||
            (unsigned char)input[consumed] < 0x20u ||
            input[consumed] == '"' || input[consumed] == '\\') continue;
        next_plain = json_short_plain_prefix(input + consumed, escapes);
        if (next_plain > escaped_limit - written ||
            next_plain >= available - written) return -1;
        memcpy(output + written, input + consumed, next_plain);
        written += next_plain;
        consumed += next_plain;
    }
    writer->length += written;
    return 0;
}

static int json_request_append_mixed_escaped_span(
    json_request_writer *writer,
    const char *input,
    size_t input_len,
    size_t plain_len,
    size_t escaped_limit
) {
    static const char hex[] = "0123456789abcdef";
    char *output;
    size_t available;
    size_t consumed;
    size_t written;
    if (!writer || !input || plain_len > input_len ||
        writer->length >= writer->capacity) return -1;
    output = writer->data + writer->length;
    available = writer->capacity - writer->length;
    if (plain_len > escaped_limit || plain_len >= available) return -1;
    memcpy(output, input, plain_len);
    consumed = plain_len;
    written = plain_len;
    while (consumed < input_len) {
        unsigned char c = (unsigned char)input[consumed++];
        size_t escaped_bytes = c < 0x20u ? 6u : 2u;
        size_t next_plain;
        if (c == '\0' || escaped_bytes > escaped_limit - written ||
            escaped_bytes >= available - written) return -1;
        output[written++] = '\\';
        if (c == '"' || c == '\\') {
            output[written++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t' ||
                   c == '\b' || c == '\f') {
            output[written++] = c == '\n' ? 'n' : c == '\r' ? 'r' :
                c == '\t' ? 't' : c == '\b' ? 'b' : 'f';
        } else if (c < 0x20u) {
            output[written++] = 'u';
            output[written++] = '0';
            output[written++] = '0';
            output[written++] = hex[c >> 4];
            output[written++] = hex[c & 0x0fu];
        } else {
            return -1;
        }
        next_plain = json_plain_prefix_span(
            input + consumed, input_len - consumed);
        if (next_plain > escaped_limit - written ||
            next_plain >= available - written) return -1;
        memcpy(output + written, input + consumed, next_plain);
        written += next_plain;
        consumed += next_plain;
    }
    writer->length += written;
    return 0;
}

static int json_request_append_escaped(
    json_request_writer *writer,
    const char *input,
    size_t escaped_limit
) {
    static const char escapes[] =
        "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
        "\"\\";
    size_t escaped_cap;
    size_t escaped_len;
    size_t plain_len;
    if (!writer || !input || writer->length >= writer->capacity) return -1;
    plain_len = escaped_limit <= JSON_IDENTIFIER_ESCAPED_LIMIT ?
        json_short_plain_prefix(input, escapes) : strcspn(input, escapes);
    if (!input[plain_len]) {
        if (plain_len > escaped_limit) return -1;
        return json_request_append(writer, input, plain_len);
    }
    {
        static const char hex[] = "0123456789abcdef";
        char pattern[6];
        unsigned char repeated = (unsigned char)input[0];
        size_t pattern_len = 0;
        size_t run = 1;
        size_t written;
        char *output;

        while (input[run] == input[0]) run++;
        if (run >= JSON_LONG_ESCAPED_INPUT && input[run] == '\0') {
            pattern[0] = '\\';
            if (repeated == '"' || repeated == '\\') {
                pattern[1] = (char)repeated;
                pattern_len = 2u;
            } else if (repeated == '\n' || repeated == '\r' ||
                       repeated == '\t' || repeated == '\b' ||
                       repeated == '\f') {
                pattern[1] = repeated == '\n' ? 'n' :
                    repeated == '\r' ? 'r' : repeated == '\t' ? 't' :
                    repeated == '\b' ? 'b' : 'f';
                pattern_len = 2u;
            } else if (repeated < 0x20u) {
                pattern[1] = 'u';
                pattern[2] = '0';
                pattern[3] = '0';
                pattern[4] = hex[repeated >> 4];
                pattern[5] = hex[repeated & 0x0fu];
                pattern_len = 6u;
            }
            if (pattern_len != 0) {
                if (run > escaped_limit / pattern_len) return -1;
                escaped_len = run * pattern_len;
                if (escaped_len >= writer->capacity - writer->length) return -1;
                output = writer->data + writer->length;
                memcpy(output, pattern, pattern_len);
                written = pattern_len;
                while (written < escaped_len) {
                    size_t copy_len = written;
                    if (copy_len > escaped_len - written)
                        copy_len = escaped_len - written;
                    memcpy(output + written, output, copy_len);
                    written += copy_len;
                }
                writer->length += escaped_len;
                return 0;
            }
        }
    }
    if (plain_len == 0u && input[1] != '\0' &&
        ((unsigned char)input[1] < 0x20u ||
         input[1] == '"' || input[1] == '\\')) {
        escaped_cap = writer->capacity - writer->length;
        if (escaped_cap > escaped_limit + 1u) escaped_cap = escaped_limit + 1u;
        if (json_escape_string_impl(
                input,
                writer->data + writer->length,
                escaped_cap,
                &escaped_len) != 0) return -1;
        writer->length += escaped_len;
        return 0;
    }
    return json_request_append_mixed_escaped(
        writer, input, plain_len, escaped_limit, escapes);
}

static int json_request_append_escaped_span(
    json_request_writer *writer,
    const char *input,
    size_t input_len,
    size_t escaped_limit
) {
    size_t plain_len;
    if (!writer || !input || input_len > escaped_limit ||
        writer->length >= writer->capacity ||
        input[input_len] != '\0') return -1;
    plain_len = json_plain_prefix_span(input, input_len);
    if (plain_len == input_len) {
        return json_request_append(writer, input, plain_len);
    }
    return json_request_append_mixed_escaped_span(
        writer, input, input_len, plain_len, escaped_limit);
}

static int json_request_append_prepared_span(
    json_request_writer *writer,
    const char *input,
    size_t input_len,
    size_t plain_limit
) {
    if (!input || input_len > plain_limit || input[input_len] != '\0') return -1;
    return json_request_append(writer, input, input_len);
}

static int json_request_append_u32(
    json_request_writer *writer,
    uint32_t value
) {
    char digits[10];
    size_t start = sizeof(digits);
    do {
        digits[--start] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    return json_request_append(writer, digits + start, sizeof(digits) - start);
}

static size_t build_chat_request_json(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    size_t user_text_len,
    int user_text_is_span,
    int stream,
    uint32_t max_completion_tokens,
    size_t escaped_limit,
    const char *system_text,
    size_t system_text_len,
    const openai_history_message *history, size_t history_count
) {
    static const char prefix[] = "{\"model\":\"";
    static const char between[] =
        "\",\"messages\":[{\"role\":\"user\",\"content\":\"";
    static const char system_between[] =
        "\",\"messages\":[{\"role\":\"system\",\"content\":\"";
    static const char user_after_system[] = "\"}";
    static const char user_prefix[] = ",{\"role\":\"user\",\"content\":\"";
    static const char token_field[] =
        "\"}],\"max_completion_tokens\":";
    static const char stream_field[] =
        ",\"temperature\":0.2,\"stream\":";
    static const char suffix[] =
        ",\"include_reasoning\":false,"
        "\"chat_template_kwargs\":{\"enable_thinking\":false}}";
    json_request_writer writer;
    if (!out || out_cap == 0 || !model || !user_text ||
        max_completion_tokens == 0u || max_completion_tokens > 4096u ||
        !escaped_limit || escaped_limit > 6u * 65535u ||
        (system_text && (!system_text_len || system_text_len > 4095u))) return 0;
    writer.data = out;
    writer.capacity = out_cap;
    writer.length = 0;
    if (json_request_append(&writer, prefix, sizeof(prefix) - 1u) != 0 ||
        json_request_append_escaped(&writer, model, 255u) != 0 ||
        (system_text ?
            (json_request_append(&writer, system_between, sizeof(system_between) - 1u) != 0 ||
             json_request_append_escaped_span(&writer, system_text, system_text_len, 6u * 4095u) != 0 ||
             json_request_append(&writer, user_after_system, sizeof(user_after_system) - 1u) != 0) :
            json_request_append(&writer, between, sizeof(between) - 1u) != 0)) { out[0] = '\0'; return 0; }
    if (history_count > 8u || (history_count && (!history || !system_text))) {
        out[0] = '\0'; return 0;
    }
    size_t history_bytes = user_text_len;
    for (size_t i = 0; i < history_count; ++i) {
        const openai_history_message *message = &history[i];
        if (!message->role || (strcmp(message->role, "user") && strcmp(message->role, "assistant")) ||
            !message->content || !message->content_len || message->content_len > 4095u ||
            message->content[message->content_len] || memchr(message->content, 0, message->content_len) ||
            !utf8_validate_v1((const uint8_t *)message->content, message->content_len) ||
            history_bytes > escaped_limit / 6u ||
            message->content_len > escaped_limit / 6u - history_bytes ||
            json_request_append(&writer, ",{\"role\":\"", 10u) ||
            json_request_append(&writer, message->role, strlen(message->role)) ||
            json_request_append(&writer, "\",\"content\":\"", 13u) ||
            json_request_append_escaped_span(&writer, message->content, message->content_len, 6u * 4095u) ||
            json_request_append(&writer, "\"}", 2u)) { out[0] = '\0'; return 0; }
        history_bytes += message->content_len;
    }
    if ((system_text && json_request_append(&writer, user_prefix, sizeof(user_prefix) - 1u)) ||
        (user_text_is_span ?
            json_request_append_escaped_span(
                &writer, user_text, user_text_len, escaped_limit) :
            json_request_append_escaped(&writer, user_text, escaped_limit)) != 0 ||
        json_request_append(
            &writer, token_field, sizeof(token_field) - 1u) != 0 ||
        json_request_append_u32(&writer, max_completion_tokens) != 0 ||
        json_request_append(
            &writer, stream_field, sizeof(stream_field) - 1u) != 0 ||
        json_request_append(
            &writer, stream ? "true" : "false", stream ? 4u : 5u) != 0 ||
        (stream && json_request_append(&writer,
            ",\"stream_options\":{\"include_usage\":true}",
            sizeof(",\"stream_options\":{\"include_usage\":true}") - 1u) != 0) ||
        json_request_append(&writer, suffix, sizeof(suffix) - 1u) != 0) {
        out[0] = '\0';
        return 0;
    }
    out[writer.length] = '\0';
    return writer.length;
}

uint32_t openai_completion_limit(uint32_t server_limit, const char *requested) {
    uint32_t value = 0;
    size_t i;
    if (!server_limit || server_limit > 4096u) return 0;
    if (!requested || !requested[0]) return server_limit;
    if (requested[0] == '0') return 0;
    for (i = 0; i < 8u && requested[i]; ++i) {
        unsigned char c = (unsigned char)requested[i];
        if (c < '0' || c > '9') return 0;
        value = value * 10u + (uint32_t)(c - '0');
    }
    if (i == 8u || !value || value > 1000000u) return 0;
    return value < server_limit ? value : server_limit;
}

size_t openai_chat_request_json(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text
) {
    return build_chat_request_json(
        out, out_cap, model, user_text, 0u, 0, 0, 256u, 3583u, NULL, 0u, NULL, 0u);
}

size_t openai_chat_request_json_stream(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text
) {
    return build_chat_request_json(
        out, out_cap, model, user_text, 0u, 0, 1, 256u, 3583u, NULL, 0u, NULL, 0u);
}

size_t openai_chat_request_json_stream_bounded(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    uint32_t max_completion_tokens
) {
    return build_chat_request_json(
        out, out_cap, model, user_text, 0u, 0, 1, max_completion_tokens, 3583u, NULL, 0u, NULL, 0u);
}

size_t openai_chat_request_json_stream_bounded_span(
    char *out,
    size_t out_cap,
    const char *model,
    const char *user_text,
    size_t user_text_len,
    uint32_t max_completion_tokens
) {
    return build_chat_request_json(
        out,
        out_cap,
        model,
        user_text,
        user_text_len,
        1,
        1,
        max_completion_tokens,
        3583u, NULL, 0u, NULL, 0u);
}

size_t openai_chat_request_json_stream_bounded_span_limit(
    char *out, size_t out_cap, const char *model, const char *user_text,
    size_t user_text_len, uint32_t max_completion_tokens, size_t input_limit
) {
    if (out && out_cap) out[0] = '\0';
    if (!input_limit || input_limit > 65535u || user_text_len > input_limit) return 0;
    return build_chat_request_json(out, out_cap, model, user_text, user_text_len,
        1, 1, max_completion_tokens, 6u * input_limit, NULL, 0u, NULL, 0u);
}

size_t openai_chat_request_json_stream_system(
    char *out, size_t out_cap, const char *model,
    const char *system_text, size_t system_text_len,
    const char *user_text, size_t user_text_len,
    uint32_t max_completion_tokens, size_t input_limit
) {
    if (out && out_cap) out[0] = '\0';
    if (!system_text || !system_text_len || system_text_len > 4095u ||
        !input_limit || input_limit > 65535u || user_text_len > input_limit ||
        !utf8_validate_v1((const uint8_t *)system_text, system_text_len)) return 0;
    return build_chat_request_json(out, out_cap, model, user_text, user_text_len,
        1, 1, max_completion_tokens, 6u * input_limit, system_text, system_text_len, NULL, 0u);
}

size_t openai_chat_request_json_stream_history(
    char *out, size_t out_cap, const char *model,
    const char *system_text, size_t system_text_len,
    const openai_history_message *history, size_t history_count,
    const char *user_text, size_t user_text_len,
    uint32_t max_completion_tokens, size_t input_limit) {
    if (out && out_cap) out[0] = '\0';
    if (!system_text || !system_text_len || system_text_len > 4095u ||
        !input_limit || input_limit > 65535u || user_text_len > input_limit ||
        !utf8_validate_v1((const uint8_t *)system_text, system_text_len)) return 0;
    return build_chat_request_json(out, out_cap, model, user_text, user_text_len,
        1, 1, max_completion_tokens, 6u * input_limit, system_text, system_text_len,
        history, history_count);
}

static size_t build_tts_pcm_request_json_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash,
    int text_prepared,
    int turn_id_prepared
) {
    static const char prefix[] = "{\"text\":\"";
    static const char voice_field[] = "\",\"voice\":\"";
    static const char turn_field[] = "\",\"turn_id\":\"";
    static const char hash_field[] = "\",\"query_hash\":";
    static const char suffix[] = ",\"frames_per_chunk\":1}";
    json_request_writer writer;
    if (!out || out_cap == 0 || !text || !voice || !turn_id) return 0;
    writer.data = out;
    writer.capacity = out_cap;
    writer.length = 0;
    if (json_request_append(&writer, prefix, sizeof(prefix) - 1u) != 0 ||
        (text_prepared ?
            json_request_append_prepared_span(
                &writer, text, text_len, 12287u) :
            json_request_append_escaped_span(
                &writer, text, text_len, 12287u)) != 0 ||
        json_request_append(
            &writer, voice_field, sizeof(voice_field) - 1u) != 0 ||
        (voice_len == 0u ? voice[0] != '\0' :
            json_request_append_escaped_span(
                &writer, voice, voice_len, 767u) != 0) ||
        json_request_append(
            &writer, turn_field, sizeof(turn_field) - 1u) != 0 ||
        (turn_id_prepared ?
            json_request_append_prepared_span(
                &writer, turn_id, turn_id_len, 767u) :
            json_request_append_escaped_span(
                &writer, turn_id, turn_id_len, 767u)) != 0 ||
        json_request_append(
            &writer, hash_field, sizeof(hash_field) - 1u) != 0 ||
        json_request_append_u32(&writer, query_hash) != 0 ||
        json_request_append(&writer, suffix, sizeof(suffix) - 1u) != 0) {
        out[0] = '\0';
        return 0;
    }
    out[writer.length] = '\0';
    return writer.length;
}

size_t tts_pcm_request_json_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash
) {
    return build_tts_pcm_request_json_spans(
        out,
        out_cap,
        text,
        text_len,
        voice,
        voice_len,
        turn_id,
        turn_id_len,
        query_hash,
        0,
        0);
}

size_t tts_pcm_request_json_prepared_id_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash
) {
    return build_tts_pcm_request_json_spans(
        out,
        out_cap,
        text,
        text_len,
        voice,
        voice_len,
        turn_id,
        turn_id_len,
        query_hash,
        0,
        1);
}

size_t tts_pcm_request_json_prepared_spans(
    char *out,
    size_t out_cap,
    const char *text,
    size_t text_len,
    const char *voice,
    size_t voice_len,
    const char *turn_id,
    size_t turn_id_len,
    uint32_t query_hash,
    int text_json_plain
) {
    return build_tts_pcm_request_json_spans(
        out,
        out_cap,
        text,
        text_len,
        voice,
        voice_len,
        turn_id,
        turn_id_len,
        query_hash,
        text_json_plain != 0,
        1);
}

size_t tts_pcm_request_json(
    char *out,
    size_t out_cap,
    const char *text,
    const char *voice,
    const char *turn_id,
    uint32_t query_hash
) {
    if (!text || !voice || !turn_id) return 0;
    return tts_pcm_request_json_spans(
        out,
        out_cap,
        text,
        strlen(text),
        voice,
        strlen(voice),
        turn_id,
        strlen(turn_id),
        query_hash);
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(const char *p, const char *end, uint32_t *out) {
    uint32_t value = 0;
    int i;
    if (!p || !out || end - p < 4) return -1;
    for (i = 0; i < 4; ++i) {
        int digit = hex_value((unsigned char)p[i]);
        if (digit < 0) return -1;
        value = (value << 4) | (uint32_t)digit;
    }
    *out = value;
    return 0;
}

static int append_utf8(uint32_t cp, char *out, size_t out_cap, size_t *offset) {
    uint8_t encoded[4];
    size_t n;
    if (cp == 0) return -1;
    if (cp <= 0x7fu) {
        encoded[0] = (uint8_t)cp;
        n = 1;
    } else if (cp <= 0x7ffu) {
        encoded[0] = (uint8_t)(0xc0u | (cp >> 6));
        encoded[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 2;
    } else if (cp <= 0xffffu && (cp < 0xd800u || cp > 0xdfffu)) {
        encoded[0] = (uint8_t)(0xe0u | (cp >> 12));
        encoded[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 3;
    } else if (cp <= 0x10ffffu) {
        encoded[0] = (uint8_t)(0xf0u | (cp >> 18));
        encoded[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[3] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 4;
    } else {
        return -1;
    }
    if (*offset >= out_cap || n >= out_cap - *offset) return -1;
    memcpy(out + *offset, encoded, n);
    *offset += n;
    return 0;
}

static const char *find_string_field_value(
    const char *json,
    const char *end,
    const char *field
) {
    const char *p = json;
    size_t field_len;
    if (!json || !end || !field || !field[0] || end < json) return NULL;
    field_len = strlen(field);
    while (p < end) {
        const char *start;
        int escaped = 0;
        if (*p++ != '"') continue;
        start = p;
        while (p < end) {
            if (!escaped && *p == '"') break;
            if (!escaped && *p == '\\') escaped = 1;
            else escaped = 0;
            p++;
        }
        if (p >= end) return NULL;
        if ((size_t)(p - start) == field_len && memcmp(start, field, field_len) == 0) {
            const char *q = p + 1;
            while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
            if (q < end && *q == ':') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
                return q;
            }
        }
        p++;
    }
    return NULL;
}

static int decode_json_string_cursor(
    const char **cursor,
    const char *end,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int allow_empty
) {
    const char *p;
    size_t o = 0;
    int raw_non_ascii = 0;
    if (out_len) *out_len = 0;
    if (!cursor || !*cursor || !end || end <= *cursor || !out ||
        out_cap == 0 || **cursor != '"')
        return -1;
    out[0] = '\0';
    p = *cursor + 1;
    while (p < end && *p != '"') {
        if ((unsigned char)*p < 0x20u) goto fail;
        if (*p == '\\') {
            char escaped;
            if (p + 1 >= end) goto fail;
            escaped = p[1];
            if (escaped == 'n') escaped = '\n';
            else if (escaped == 'r') escaped = '\r';
            else if (escaped == 't') escaped = '\t';
            else if (escaped == 'b') escaped = '\b';
            else if (escaped == 'f') escaped = '\f';
            else if (escaped == '"' || escaped == '\\' || escaped == '/') {
                /* already decoded */
            } else if (escaped == 'u') {
                uint32_t cp;
                if (read_hex4(p + 2, end, &cp) != 0) goto fail;
                p += 6;
                if (cp >= 0xd800u && cp <= 0xdbffu) {
                    uint32_t low;
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u' ||
                        read_hex4(p + 2, end, &low) != 0 ||
                        low < 0xdc00u || low > 0xdfffu) goto fail;
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
                    p += 6;
                } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
                    goto fail;
                }
                if (cp == 0u) goto fail;
                if (append_utf8(cp, out, out_cap, &o) != 0) goto fail;
                continue;
            } else {
                goto fail;
            }
            if (o + 1u >= out_cap) goto fail;
            out[o++] = escaped;
            p += 2;
            continue;
        }
        if (o + 1u >= out_cap) goto fail;
        if ((unsigned char)*p >= 0x80u) raw_non_ascii = 1;
        out[o++] = *p++;
    }
    if (p >= end || *p != '"') goto fail;
    p++;
    if ((!allow_empty && o == 0u) ||
        (raw_non_ascii && !utf8_validate_v1((const uint8_t *)out, o)))
        goto fail;
    out[o] = '\0';
    if (out_len) *out_len = o;
    *cursor = p;
    return 0;

fail:
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return -1;
}

static int vllm_json_word_has_special(size_t word) {
    const size_t ones = SIZE_MAX / 0xffu;
    const size_t highs = ones << 7;
    size_t controls_or_upper = word & (ones * 0xa0u);
    size_t quotes = word ^ (ones * (size_t)'"');
    /* The broad group contains controls, backslashes, and @ through _. */
    return (word & highs) != 0u ||
        ((controls_or_upper - ones) & ~controls_or_upper & highs) != 0u ||
        ((quotes - ones) & ~quotes & highs) != 0u;
}

static const char *vllm_json_find_special(const char *p, const char *end) {
    while ((size_t)(end - p) >= sizeof(size_t)) {
        size_t word;
        const char *word_end;
        memcpy(&word, p, sizeof(word));
        if (!vllm_json_word_has_special(word)) {
            p += sizeof(word);
            continue;
        }
        word_end = p + sizeof(word);
        while (p < word_end) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20u || c >= 0x80u || c == '"' || c == '\\') return p;
            p++;
        }
    }
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20u || c >= 0x80u || c == '"' || c == '\\') break;
        p++;
    }
    return p;
}

static int decode_vllm_string_cursor(
    const char **cursor,
    const char *end,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int allow_empty
) {
    const char *start;
    const char *p;
    const char *span;
    size_t o = 0;
    if (!cursor || !*cursor || !end || end <= *cursor || !out ||
        out_cap == 0 || **cursor != '"')
        return -1;
    start = *cursor;
    p = start + 1;
    span = p;
    for (;;) {
        p = vllm_json_find_special(p, end);
        if (p >= end) return -1;
        if (*p == '"') break;
        if (*p != '\\') goto generic;
        {
            char escaped;
            size_t available;
            size_t span_len;
            if (p + 1 >= end) goto generic;
            escaped = p[1];
            if (escaped == 'n') escaped = '\n';
            else if (escaped == 'r') escaped = '\r';
            else if (escaped == 't') escaped = '\t';
            else if (escaped == 'b') escaped = '\b';
            else if (escaped == 'f') escaped = '\f';
            else if (escaped == '"' || escaped == '\\' || escaped == '/') {
                /* already decoded */
            } else {
                goto generic;
            }
            span_len = (size_t)(p - span);
            available = out_cap - o;
            if (available < 2u || span_len > available - 2u) return -1;
            memcpy(out + o, span, span_len);
            o += span_len;
            out[o++] = escaped;
            p += 2;
            span = p;
            continue;
        }
    }
    {
        size_t span_len = (size_t)(p - span);
        if (span_len >= out_cap - o) return -1;
        memcpy(out + o, span, span_len);
        o += span_len;
    }
    if (!allow_empty && o == 0u) return -1;
    out[o] = '\0';
    if (out_len) *out_len = o;
    *cursor = p + 1;
    return 0;

generic:
    *cursor = start;
    return decode_json_string_cursor(
        cursor, end, out, out_cap, out_len, allow_empty);
}

static int decode_json_string_value(
    const char *value,
    const char *end,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int require_exact_end,
    int allow_empty
) {
    const char *p = value;
    if (decode_json_string_cursor(
            &p, end, out, out_cap, out_len, allow_empty) != 0)
        return -1;
    if (require_exact_end && p != end) {
        out[0] = '\0';
        if (out_len) *out_len = 0;
        return -1;
    }
    return 0;
}

int json_extract_string_field(
    const char *json,
    size_t json_len,
    const char *field,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    const char *end;
    const char *p;
    if (out_len) *out_len = 0;
    if (!json || !field || !field[0] || !out || out_cap == 0) return -1;
    out[0] = '\0';
    end = json + json_len;
    p = find_string_field_value(json, end, field);
    return p ? decode_json_string_value(
        p, end, out, out_cap, out_len, 0, 0) : -1;
}

int openai_chat_extract_content(
    const char *json,
    size_t json_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    return json_extract_string_field(
        json, json_len, "content", out, out_cap, out_len);
}

#define OPENAI_JSON_MAX_DEPTH 32u

static void json_skip_space(const char **cursor, const char *end) {
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    *cursor = p;
}

static int json_skip_string(const char **cursor, const char *end) {
    const char *p = *cursor;
    if (p >= end || *p != '"') return -1;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *cursor = p;
            return 0;
        }
        if (c < 0x20u) return -1;
        if (c == '\\') {
            unsigned char escaped;
            if (p >= end) return -1;
            escaped = (unsigned char)*p++;
            if (escaped == 'u') {
                uint32_t ignored;
                if (read_hex4(p, end, &ignored) != 0) return -1;
                p += 4;
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                       escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                       escaped != 'r' && escaped != 't') {
                return -1;
            }
        }
    }
    return -1;
}

static int json_read_plain_key(
    const char **cursor,
    const char *end,
    const char **key,
    size_t *key_len
) {
    const char *p;
    const char *start;
    if (!cursor || !*cursor || !end || !key || !key_len ||
        *cursor >= end || **cursor != '"') return -1;
    p = *cursor + 1;
    start = p;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *cursor = p;
            *key = start;
            *key_len = (size_t)((p - 1) - start);
            return 0;
        }
        if (c < 0x20u || c == '\\') return -1;
    }
    return -1;
}

static int json_skip_value(const char **cursor, const char *end, unsigned depth);

static int json_skip_array(const char **cursor, const char *end, unsigned depth) {
    const char *p = *cursor;
    if (p >= end || *p != '[' || depth > OPENAI_JSON_MAX_DEPTH) return -1;
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == ']') {
        *cursor = p + 1;
        return 0;
    }
    /* Root values start at depth 1, this array at 2, and its first object at 3. */
    for (;;) {
        if (json_skip_value(&p, end, depth) != 0) return -1;
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == ']') {
            *cursor = p + 1;
            return 0;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

static int json_skip_object(const char **cursor, const char *end, unsigned depth) {
    const char *p = *cursor;
    if (p >= end || *p != '{' || depth > OPENAI_JSON_MAX_DEPTH) return -1;
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == '}') {
        *cursor = p + 1;
        return 0;
    }
    for (;;) {
        if (json_skip_string(&p, end) != 0) return -1;
        json_skip_space(&p, end);
        if (p >= end || *p++ != ':') return -1;
        json_skip_space(&p, end);
        if (json_skip_value(&p, end, depth) != 0) return -1;
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            *cursor = p + 1;
            return 0;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

static int json_skip_number(const char **cursor, const char *end) {
    const char *p = *cursor;
    if (p < end && *p == '-') p++;
    if (p >= end) return -1;
    if (*p == '0') {
        p++;
    } else {
        if (*p < '1' || *p > '9') return -1;
        do {
            p++;
        } while (p < end && *p >= '0' && *p <= '9');
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || *p < '0' || *p > '9') return -1;
        do {
            p++;
        } while (p < end && *p >= '0' && *p <= '9');
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || *p < '0' || *p > '9') return -1;
        do {
            p++;
        } while (p < end && *p >= '0' && *p <= '9');
    }
    *cursor = p;
    return 0;
}

static int json_skip_value(const char **cursor, const char *end, unsigned depth) {
    const char *p = *cursor;
    json_skip_space(&p, end);
    if (p >= end) return -1;
    if (*p == '{') {
        if (depth >= OPENAI_JSON_MAX_DEPTH ||
            json_skip_object(&p, end, depth + 1u) != 0) return -1;
    } else if (*p == '[') {
        if (depth >= OPENAI_JSON_MAX_DEPTH ||
            json_skip_array(&p, end, depth + 1u) != 0) return -1;
    } else if (*p == '"') {
        if (json_skip_string(&p, end) != 0) return -1;
    } else if ((size_t)(end - p) >= 4u && memcmp(p, "true", 4) == 0) {
        p += 4;
    } else if ((size_t)(end - p) >= 5u && memcmp(p, "false", 5) == 0) {
        p += 5;
    } else if ((size_t)(end - p) >= 4u && memcmp(p, "null", 4) == 0) {
        p += 4;
    } else if (json_skip_number(&p, end) != 0) {
        return -1;
    }
    *cursor = p;
    return 0;
}

static int json_find_object_member(
    const char *object,
    const char *end,
    const char *field,
    const char **value,
    const char **value_end,
    int *present
) {
    const char *p = object;
    size_t field_len;
    if (!object || !end || !field || !value || !value_end || !present ||
        object >= end || *object != '{') return -1;
    *value = NULL;
    *value_end = NULL;
    *present = 0;
    field_len = strlen(field);
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == '}') return p + 1 == end ? 0 : -1;
    for (;;) {
        const char *key;
        const char *candidate;
        const char *candidate_end;
        size_t key_len;
        int match;
        if (json_read_plain_key(&p, end, &key, &key_len) != 0) return -1;
        match = key_len == field_len &&
            memcmp(key, field, field_len) == 0;
        json_skip_space(&p, end);
        if (p >= end || *p++ != ':') return -1;
        json_skip_space(&p, end);
        candidate = p;
        if (json_skip_value(&p, end, 1u) != 0) return -1;
        candidate_end = p;
        if (match) {
            if (*present) return -1;
            *present = 1;
            *value = candidate;
            *value_end = candidate_end;
        }
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == '}') return p + 1 == end ? 0 : -1;
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

static int json_read_delta_members(
    const char **cursor,
    const char *end,
    char *content,
    size_t content_cap,
    size_t *content_len,
    int *content_present,
    unsigned depth
) {
    const char *p;
    if (!cursor || !*cursor || !end || !content || content_cap == 0u ||
        !content_len || !content_present || *cursor >= end ||
        **cursor != '{' || depth > OPENAI_JSON_MAX_DEPTH) return -1;
    p = *cursor;
    content[0] = '\0';
    *content_len = 0;
    *content_present = 0;
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == '}') {
        *cursor = p + 1;
        return 0;
    }
    for (;;) {
        const char *key;
        size_t key_len;
        int match;
        if (json_read_plain_key(&p, end, &key, &key_len) != 0) return -1;
        match = key_len == 7u && memcmp(key, "content", 7u) == 0;
        json_skip_space(&p, end);
        if (p >= end || *p++ != ':') return -1;
        json_skip_space(&p, end);
        if (match) {
            const char *candidate = p;
            if (*content_present) return -1;
            *content_present = 1;
            if (p < end && *p == '"') {
                if (decode_json_string_cursor(
                        &p, end, content, content_cap, content_len, 1) != 0)
                    return -1;
            } else {
                if (json_skip_value(&p, end, depth) != 0 ||
                    (size_t)(p - candidate) != 4u ||
                    memcmp(candidate, "null", 4u) != 0)
                    return -1;
            }
        } else {
            if (json_skip_value(&p, end, depth) != 0) return -1;
        }
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            *cursor = p + 1;
            return 0;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

static int json_read_choice_members(
    const char **cursor,
    const char *end,
    const char **finish_reason,
    const char **finish_reason_end,
    int *finish_present,
    char *content,
    size_t content_cap,
    size_t *content_len,
    int *content_present,
    int *delta_present,
    unsigned depth
) {
    const char *p;
    if (!cursor || !*cursor || !end || !finish_reason || !finish_reason_end ||
        !finish_present || !content || content_cap == 0u || !content_len ||
        !content_present || !delta_present ||
        *cursor >= end || **cursor != '{' ||
        depth > OPENAI_JSON_MAX_DEPTH) return -1;
    p = *cursor;
    *finish_reason = NULL;
    *finish_reason_end = NULL;
    *finish_present = 0;
    content[0] = '\0';
    *content_len = 0;
    *content_present = 0;
    *delta_present = 0;
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == '}') {
        *cursor = p + 1;
        return 0;
    }
    for (;;) {
        const char *key;
        const char *candidate;
        const char *candidate_end;
        size_t key_len;
        int member = 0;
        if (json_read_plain_key(&p, end, &key, &key_len) != 0) return -1;
        if (key_len == 13u && memcmp(key, "finish_reason", 13u) == 0) {
            member = 1;
        } else if (key_len == 5u && memcmp(key, "delta", 5u) == 0) {
            member = 2;
        }
        json_skip_space(&p, end);
        if (p >= end || *p++ != ':') return -1;
        json_skip_space(&p, end);
        if (member == 2) {
            if (*delta_present) return -1;
            *delta_present = 1;
            if (depth >= OPENAI_JSON_MAX_DEPTH ||
                json_read_delta_members(
                    &p, end, content, content_cap, content_len,
                    content_present, depth + 1u) != 0)
                return -1;
        } else {
            candidate = p;
            if (json_skip_value(&p, end, depth) != 0) return -1;
            candidate_end = p;
        }
        if (member == 1) {
            if (*finish_present) return -1;
            *finish_present = 1;
            *finish_reason = candidate;
            *finish_reason_end = candidate_end;
        }
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            *cursor = p + 1;
            return 0;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

static int json_read_choices(
    const char **cursor,
    const char *end,
    const char **finish_reason,
    const char **finish_reason_end,
    int *finish_present,
    char *content,
    size_t content_cap,
    size_t *content_len,
    int *content_present,
    int *delta_present,
    int *choice_present
) {
    const char *p;
    if (!cursor || !*cursor || !end || !finish_reason ||
        !finish_reason_end || !finish_present || !content ||
        content_cap == 0u || !content_len || !content_present ||
        !delta_present || !choice_present || *cursor >= end ||
        **cursor != '[') return -1;
    p = *cursor;
    *choice_present = 0;
    p++;
    json_skip_space(&p, end);
    if (p < end && *p == ']') {
        *cursor = p + 1;
        return 0;
    }
    for (;;) {
        if (!*choice_present) {
            if (json_read_choice_members(
                    &p, end, finish_reason, finish_reason_end,
                    finish_present, content, content_cap, content_len,
                    content_present, delta_present, 3u) != 0)
                return -1;
            *choice_present = 1;
        } else if (json_skip_value(&p, end, 2u) != 0) {
            return -1;
        }
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == ']') {
            *cursor = p + 1;
            return 0;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
}

int stt_json_extract_transcript(
    const char *json,
    size_t json_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    const char *end;
    const char *status;
    const char *status_end;
    const char *text;
    const char *text_end;
    const char *transcript;
    const char *transcript_end;
    char transcript_alias[2048];
    char status_text[3];
    size_t canonical_len = 0;
    size_t alias_len = 0;
    size_t status_len = 0;
    int status_present;
    int text_present;
    int transcript_present;
    if (out_len) *out_len = 0;
    if (!json || json_len < 2u || !out || out_cap == 0u) return -1;
    out[0] = '\0';
    end = json + json_len;
    if (json_find_object_member(
            json, end, "status", &status, &status_end, &status_present) != 0 ||
        !status_present ||
        json_find_object_member(
            json, end, "text", &text, &text_end, &text_present) != 0 ||
        json_find_object_member(
            json, end, "transcript",
            &transcript, &transcript_end, &transcript_present) != 0 ||
        (!text_present && !transcript_present) ||
        decode_json_string_value(
            status, status_end, status_text, sizeof(status_text),
            &status_len, 1, 0) != 0 ||
        status_len != 2u || memcmp(status_text, "ok", 2u) != 0)
        goto fail;
    if (!text_present) {
        text = transcript;
        text_end = transcript_end;
    }
    if (decode_json_string_value(
            text, text_end, out, out_cap, &canonical_len, 1, 0) != 0)
        goto fail;
    if (text_present && transcript_present &&
        (decode_json_string_value(
             transcript, transcript_end,
             transcript_alias, sizeof(transcript_alias), &alias_len, 1, 0) != 0 ||
         alias_len != canonical_len || memcmp(transcript_alias, out, canonical_len) != 0))
        goto fail;
    if (out_len) *out_len = canonical_len;
    return 0;

fail:
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return -1;
}

static int json_take_literal(
    const char **cursor,
    const char *end,
    const char *literal,
    size_t literal_len
) {
    if ((size_t)(end - *cursor) < literal_len ||
        memcmp(*cursor, literal, literal_len) != 0)
        return 0;
    *cursor += literal_len;
    return 1;
}

static int vllm_json_word_is_digits(size_t word) {
    const size_t ones = SIZE_MAX / 0xffu;
    const size_t digits = word & (ones * 0x0fu);
    return (word & (ones * 0xf0u)) == ones * 0x30u &&
        ((digits + ones * 0x06u) & (ones * 0x10u)) == 0u;
}

static int json_skip_vllm_uint(const char **cursor, const char *end) {
    const char *p = *cursor;
    if (p >= end || *p < '0' || *p > '9') return -1;
    if (*p == '0') {
        p++;
        if (p < end && *p >= '0' && *p <= '9') return -1;
    } else {
        while ((size_t)(end - p) >= sizeof(size_t)) {
            size_t word;
            memcpy(&word, p, sizeof(word));
            if (!vllm_json_word_is_digits(word)) break;
            p += sizeof(word);
        }
        while (p < end && *p >= '0' && *p <= '9') {
            p++;
        }
    }
    *cursor = p;
    return 0;
}

static int json_skip_vllm_plain_string(const char **cursor, const char *end) {
    const char *p = *cursor;
    if (p >= end || *p++ != '"') return -1;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *cursor = p;
            return 0;
        }
        if (c < 0x20u || c == '\\') return -1;
    }
    return -1;
}

#define JSON_TAKE(cursor, end, literal) \
    json_take_literal(&(cursor), (end), (literal), sizeof(literal) - 1u)

static int vllm_finish_reason_code(
    const char *reason,
    size_t reason_len
) {
    if (reason_len == sizeof("stop") - 1u &&
        memcmp(reason, "stop", sizeof("stop") - 1u) == 0) return OPENAI_FINISH_STOP;
    if (reason_len == sizeof("length") - 1u &&
        memcmp(reason, "length", sizeof("length") - 1u) == 0) return OPENAI_FINISH_LENGTH;
    return OPENAI_FINISH_NONE;
}

static int match_vllm_028_exact_suffix(
    const char *cursor,
    const char *end,
    int *finished
) {
    static const char first_suffix[] =
        "},\"logprobs\":null,\"finish_reason\":null}],"
        "\"prompt_token_ids\":null,\"prompt_text\":null}";
    static const char delta_suffix[] =
        "},\"logprobs\":null,\"finish_reason\":null,\"token_ids\":null}]}";
    static const char terminal_suffix[] =
        "},\"logprobs\":null,\"finish_reason\":\"stop\","
        "\"stop_reason\":null,\"token_ids\":null}]}";
    size_t remaining = (size_t)(end - cursor);
    if (remaining == sizeof(delta_suffix) - 1u &&
        memcmp(cursor, delta_suffix, sizeof(delta_suffix) - 1u) == 0) {
        *finished = 0;
        return 1;
    }
    if (remaining == sizeof(first_suffix) - 1u &&
        memcmp(cursor, first_suffix, sizeof(first_suffix) - 1u) == 0) {
        *finished = 0;
        return 1;
    }
    if (remaining == sizeof(terminal_suffix) - 1u &&
        memcmp(cursor, terminal_suffix, sizeof(terminal_suffix) - 1u) == 0) {
        *finished = OPENAI_FINISH_STOP;
        return 1;
    }
    return 0;
}

/*
 * vLLM 0.28 emits compact Pydantic JSON in model-field order. This path
 * validates the complete common shape before it exposes content. Any field,
 * order, or option mismatch falls back to the strict generic parser below.
 */
static int extract_vllm_028_delta_content(
    const char *json,
    size_t json_len,
    size_t known_prefix_len,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int *present,
    int *finished,
    size_t *validated_prefix_len
) {
    const char *end = json + json_len;
    const char *p = json;
    char finish_text[64];
    size_t decoded_len = 0;
    size_t finish_len = 0;
    int decoded_present = 0;
    int decoded_finished = 0;

    if (known_prefix_len != 0u) {
        if (known_prefix_len >= json_len) return 0;
        p += known_prefix_len;
    } else {
        if (!JSON_TAKE(p, end, "{\"id\":") ||
            json_skip_vllm_plain_string(&p, end) != 0 ||
            !JSON_TAKE(
                p, end,
                ",\"object\":\"chat.completion.chunk\",\"created\":") ||
            json_skip_vllm_uint(&p, end) != 0 ||
            !JSON_TAKE(p, end, ",\"model\":") ||
            json_skip_vllm_plain_string(&p, end) != 0 ||
            !JSON_TAKE(p, end, ",\"choices\":[{\"index\":") ||
            json_skip_vllm_uint(&p, end) != 0 ||
            !JSON_TAKE(p, end, ",\"delta\":"))
            return 0;
        known_prefix_len = (size_t)(p - json);
    }

    if (JSON_TAKE(p, end, "{\"role\":")) {
        if (json_skip_vllm_plain_string(&p, end) != 0 ||
            !JSON_TAKE(p, end, ",\"content\":"))
            return 0;
    } else if (!JSON_TAKE(p, end, "{\"content\":")) {
        return 0;
    }
    decoded_present = 1;
    if (p < end && *p == '"') {
        if ((size_t)(end - p) >= 2u && p[1] == '"') {
            p += 2;
        } else if (decode_vllm_string_cursor(
                       &p, end, out, out_cap, &decoded_len, 1) != 0) {
            return 0;
        }
    } else if (!JSON_TAKE(p, end, "null")) {
        return 0;
    }
    if (match_vllm_028_exact_suffix(p, end, &decoded_finished)) {
        *out_len = decoded_len;
        *present = decoded_present;
        *finished = decoded_finished;
        *validated_prefix_len = known_prefix_len;
        return 1;
    }
    if (!JSON_TAKE(
            p, end,
            "},\"logprobs\":null,\"finish_reason\":"))
        return 0;
    if (!JSON_TAKE(p, end, "null")) {
        if (decode_vllm_string_cursor(
                &p, end, finish_text, sizeof(finish_text),
                &finish_len, 0) != 0)
            return 0;
        decoded_finished = vllm_finish_reason_code(finish_text, finish_len);
        if (!decoded_finished) return 0;
    }
    if (JSON_TAKE(p, end, ",\"stop_reason\":")) {
        if (json_skip_value(&p, end, 4u) != 0) return 0;
    }
    (void)JSON_TAKE(p, end, ",\"token_ids\":null");
    if (!JSON_TAKE(p, end, "}]") ||
        !(JSON_TAKE(p, end, "}") ||
          JSON_TAKE(
              p, end,
              ",\"prompt_token_ids\":null,\"prompt_text\":null}")) ||
        p != end)
        return 0;

    *out_len = decoded_len;
    *present = decoded_present;
    *finished = decoded_finished;
    *validated_prefix_len = known_prefix_len;
    return 1;
}

#undef JSON_TAKE

static int extract_delta_content(
    const char *json,
    size_t json_len,
    size_t known_vllm_prefix_len,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int *present,
    int *finished,
    size_t *validated_vllm_prefix_len
) {
    const char *end;
    const char *p;
    const char *finish_reason = NULL;
    const char *finish_reason_end = NULL;
    char finish_text[64];
    size_t finish_len;
    int choices_present = 0;
    int choice_present = 0;
    int delta_present = 0;
    int finish_present = 0;
    if (out_len) *out_len = 0;
    if (present) *present = 0;
    if (finished) *finished = 0;
    if (validated_vllm_prefix_len) *validated_vllm_prefix_len = 0;
    if (!json || json_len < 2 || !out || out_cap == 0 || !out_len || !present ||
        !finished || !validated_vllm_prefix_len)
        return -1;
    out[0] = '\0';
    if (extract_vllm_028_delta_content(
            json, json_len, known_vllm_prefix_len,
            out, out_cap, out_len, present, finished,
            validated_vllm_prefix_len))
        return 0;
    end = json + json_len;
    p = json + 1;
    json_skip_space(&p, end);
    if (p < end && *p == '}') return -1;
    for (;;) {
        const char *key;
        size_t key_len;
        int match;
        if (json_read_plain_key(&p, end, &key, &key_len) != 0) return -1;
        match = key_len == 7u && memcmp(key, "choices", 7u) == 0;
        json_skip_space(&p, end);
        if (p >= end || *p++ != ':') return -1;
        json_skip_space(&p, end);
        if (match) {
            if (choices_present) return -1;
            choices_present = 1;
            if (json_read_choices(
                    &p, end, &finish_reason, &finish_reason_end,
                    &finish_present, out, out_cap, out_len, present,
                    &delta_present, &choice_present) != 0)
                return -1;
        } else if (json_skip_value(&p, end, 1u) != 0) {
            return -1;
        }
        json_skip_space(&p, end);
        if (p >= end) return -1;
        if (*p == '}') {
            if (p + 1 != end) return -1;
            break;
        }
        if (*p++ != ',') return -1;
        json_skip_space(&p, end);
    }
    if (!choices_present) return -1;
    if (!choice_present) return 0;
    if (!delta_present) return -1;
    if (finish_present &&
        !((size_t)(finish_reason_end - finish_reason) == 4u &&
          memcmp(finish_reason, "null", 4) == 0)) {
        if (decode_json_string_value(
                finish_reason, finish_reason_end,
                finish_text, sizeof(finish_text), &finish_len, 1, 0) != 0)
            return -1;
        *finished = vllm_finish_reason_code(finish_text, finish_len);
        if (!*finished) return -1;
    }
    if (!*present) return 0;
    return 0;
}

static void sse_observe_model(openai_sse_decoder *decoder, const char *json, size_t len) {
    const char *value, *end;
    char model[sizeof(decoder->reported_model)];
    size_t model_len = 0;
    int present = 0;
    if (decoder->model_conflict) return;
    if (json_find_object_member(json, json + len, "model", &value, &end, &present) != 0 ||
        (present && (decode_json_string_value(value, end, model, sizeof(model), &model_len, 1, 0) != 0 ||
                     !utf8_validate_v1((const uint8_t *)model, model_len) ||
                     memchr(model, '\0', model_len) != NULL))) {
        decoder->model_conflict = 1;
    } else if (present) {
        if (decoder->reported_model[0] && strcmp(decoder->reported_model, model) != 0)
            decoder->model_conflict = 1;
        else memcpy(decoder->reported_model, model, model_len + 1u);
    }
    if (decoder->model_conflict) decoder->reported_model[0] = '\0';
}

/* Accept complete, provider-reported cumulative usage. Never sum chunks. */
static void sse_observe_usage(openai_sse_decoder *decoder, const char *json, size_t len) {
    const char *usage, *usage_end, *value, *end;
    static const char *keys[] = {"prompt_tokens", "completion_tokens", "total_tokens"};
    uint64_t counts[3] = {0};
    int present = 0;
    size_t i;
    if (decoder->usage_invalid) return;
    if (json_find_object_member(json, json + len, "usage", &usage, &usage_end, &present) != 0) goto invalid;
    if (!present || ((size_t)(usage_end - usage) == 4u && !memcmp(usage, "null", 4u))) return;
    if (!decoder->finish_reason) goto invalid;
    for (i = 0; i < 3u; ++i) {
        const char *p;
        if (json_find_object_member(usage, usage_end, keys[i], &value, &end, &present) != 0 || !present ||
            value == end || (end - value > 1 && *value == '0')) goto invalid;
        for (p = value; p < end; ++p) {
            if (*p < '0' || *p > '9' || counts[i] > (UINT64_C(9007199254740991) - (unsigned)(*p - '0')) / 10u) goto invalid;
            counts[i] = counts[i] * 10u + (unsigned)(*p - '0');
        }
    }
    if (counts[0] + counts[1] != counts[2] || (decoder->usage_present &&
        (decoder->prompt_tokens != counts[0] || decoder->completion_tokens != counts[1] || decoder->total_tokens != counts[2]))) goto invalid;
    decoder->prompt_tokens = counts[0]; decoder->completion_tokens = counts[1]; decoder->total_tokens = counts[2];
    decoder->usage_present = 1;
    return;
invalid:
    decoder->usage_invalid = 1;
    decoder->usage_present = 0;
}

static int sse_process_line(
    openai_sse_decoder *decoder,
    const char *line,
    size_t line_len
) {
    const char *payload;
    size_t payload_len;
    size_t prior_vllm_prefix_len;
    size_t content_len = 0;
    size_t known_vllm_prefix_len = 0;
    size_t validated_vllm_prefix_len = 0;
    int present = 0;
    int finished = 0;
    if (!decoder || (!line && line_len != 0u)) return -1;
    while (line_len > 0u && line[line_len - 1u] == '\r') line_len--;
    if (line_len == 0u) return 0;
    if (line[0] == ':' || line_len < 5u ||
        memcmp(line, "data:", 5u) != 0) {
        decoder->vllm_line_prefix_len = 0;
        decoder->vllm_prefix_match = 0;
        return 0;
    }
    payload = line + 5u;
    payload_len = line_len - 5u;
    if (payload_len > 0 && *payload == ' ') {
        payload++;
        payload_len--;
    }
    if (payload_len == 6 && memcmp(payload, "[DONE]", 6) == 0) {
        if (!decoder->finish_reason) return -1;
        decoder->done = 1;
        return 0;
    }
    if (decoder->done || payload_len < 2 || payload[0] != '{' ||
        payload[payload_len - 1] != '}') return -1;
    if (decoder->vllm_prefix_match &&
        decoder->vllm_line_prefix_len > (size_t)(payload - line) &&
        line_len >= decoder->vllm_line_prefix_len) {
        known_vllm_prefix_len = decoder->vllm_line_prefix_len -
            (size_t)(payload - line);
    }
    prior_vllm_prefix_len = decoder->vllm_line_prefix_len;
    if (extract_delta_content(
            payload,
            payload_len,
            known_vllm_prefix_len,
            decoder->content,
            sizeof(decoder->content),
            &content_len,
            &present, &finished,
            &validated_vllm_prefix_len) != 0)
        return -1;
    if (validated_vllm_prefix_len != 0u) {
        size_t cache_len = (size_t)(payload - line) +
            validated_vllm_prefix_len;
        if (cache_len > line_len || cache_len > sizeof(decoder->line)) return -1;
        if (line != decoder->line &&
            (!decoder->vllm_prefix_match ||
             cache_len != prior_vllm_prefix_len))
            memmove(decoder->line, line, cache_len);
        decoder->vllm_line_prefix_len = cache_len;
        decoder->vllm_prefix_match = 1;
    } else {
        decoder->vllm_line_prefix_len = 0;
        decoder->vllm_prefix_match = 0;
    }
    /* An unchanged validated prefix includes the complete model field. */
    if (!known_vllm_prefix_len) sse_observe_model(decoder, payload, payload_len);
    if (decoder->finish_reason && (present || finished)) return -1;
    if (finished) decoder->finish_reason = finished;
    if (decoder->finish_reason) sse_observe_usage(decoder, payload, payload_len);
    if (present && content_len > 0 &&
        decoder->callback(
            decoder->content, content_len, decoder->callback_user) != 0)
        return -1;
    return 0;
}

int openai_sse_init(
    openai_sse_decoder *decoder,
    openai_delta_callback callback,
    void *callback_user
) {
    if (!decoder || !callback) return -1;
    /* line_len hides dormant line storage until feed writes each byte. */
    decoder->line[0] = '\0';
    decoder->line_len = 0;
    decoder->vllm_line_prefix_len = 0;
    decoder->callback = callback;
    decoder->callback_user = callback_user;
    decoder->vllm_prefix_match = 0;
    decoder->finish_reason = OPENAI_FINISH_NONE;
    decoder->done = 0;
    decoder->failed = 0;
    decoder->reported_model[0] = '\0';
    decoder->model_conflict = 0;
    decoder->prompt_tokens = decoder->completion_tokens = decoder->total_tokens = 0;
    decoder->usage_present = decoder->usage_invalid = 0;
    return 0;
}

int openai_sse_feed(openai_sse_decoder *decoder, const char *data, size_t data_len) {
    size_t offset = 0;
    if (!decoder || (!data && data_len != 0) || decoder->failed) return -1;
    while (offset < data_len) {
        const char *newline;
        size_t take;
        if (decoder->done) {
            unsigned char c = (unsigned char)data[offset++];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') goto fail;
            continue;
        }
        newline = memchr(data + offset, '\n', data_len - offset);
        take = newline ? (size_t)(newline - (data + offset)) : data_len - offset;
        if (take >= sizeof(decoder->line) - decoder->line_len) goto fail;
        if (newline && decoder->line_len == 0u) {
            decoder->vllm_prefix_match =
                decoder->vllm_line_prefix_len != 0u;
            if (decoder->vllm_prefix_match && take != 0u) {
                size_t compare_len = decoder->vllm_line_prefix_len;
                if (compare_len > take) compare_len = take;
                if (memcmp(decoder->line, data + offset, compare_len) != 0)
                    decoder->vllm_prefix_match = 0;
            }
            if (sse_process_line(
                    decoder, data + offset, take) != 0) goto fail;
            offset += take + 1u;
            continue;
        }
        if (take != 0u) {
            if (decoder->line_len == 0u)
                decoder->vllm_prefix_match =
                    decoder->vllm_line_prefix_len != 0u;
            if (decoder->vllm_prefix_match &&
                decoder->line_len < decoder->vllm_line_prefix_len) {
                size_t compare_len = decoder->vllm_line_prefix_len -
                    decoder->line_len;
                if (compare_len > take) compare_len = take;
                if (memcmp(
                        decoder->line + decoder->line_len,
                        data + offset, compare_len) != 0)
                    decoder->vllm_prefix_match = 0;
            }
        }
        memcpy(decoder->line + decoder->line_len, data + offset, take);
        decoder->line_len += take;
        offset += take;
        if (!newline) break;
        offset++;
        if (sse_process_line(
                decoder, decoder->line, decoder->line_len) != 0) goto fail;
        decoder->line_len = 0;
    }
    return 0;

fail:
    decoder->failed = 1;
    return -1;
}

int openai_sse_finish(openai_sse_decoder *decoder) {
    if (!decoder || decoder->failed) return -1;
    if (decoder->line_len != 0) {
        if (sse_process_line(
                decoder, decoder->line, decoder->line_len) != 0) {
            decoder->failed = 1;
            return -1;
        }
        decoder->line_len = 0;
    }
    return decoder->done ? 0 : -1;
}
