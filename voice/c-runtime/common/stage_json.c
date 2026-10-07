#include "stage_json.h"

#include <stdint.h>
#include <string.h>

static const char decimal_pairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

#define STAGE_DECIMAL_DIGITS 20u
#define STAGE_DECIMAL_SUFFIX_DIGITS 6u
#define STAGE_DECIMAL_SUFFIX_BASE UINT64_C(1000000)
#define STAGE_DECIMAL_CURRENT_PREFIX_DIGITS 7u
#define STAGE_DECIMAL_CURRENT_DIGITS \
    (STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS)

typedef struct {
    char digits[STAGE_DECIMAL_DIGITS - STAGE_DECIMAL_SUFFIX_DIGITS];
    size_t length;
    uint64_t base;
    int ready;
} stage_decimal_prefix;

static inline void write_decimal_suffix_digits(
    char out[STAGE_DECIMAL_SUFFIX_DIGITS],
    uint32_t low
) {
    size_t pair = (size_t)(low / UINT32_C(10000)) * 2u;
    out[0] = decimal_pairs[pair];
    out[1] = decimal_pairs[pair + 1u];
    low %= UINT32_C(10000);
    pair = (size_t)(low / UINT32_C(100)) * 2u;
    out[2] = decimal_pairs[pair];
    out[3] = decimal_pairs[pair + 1u];
    pair = (size_t)(low % UINT32_C(100)) * 2u;
    out[4] = decimal_pairs[pair];
    out[5] = decimal_pairs[pair + 1u];
}

static size_t write_decimal_digits(
    char digits[STAGE_DECIMAL_DIGITS],
    uint64_t remaining
) {
    size_t digit_pos = STAGE_DECIMAL_DIGITS;
    while (remaining >= UINT64_C(100)) {
        uint64_t quotient = remaining / UINT64_C(100);
        size_t pair =
            (size_t)(remaining - quotient * UINT64_C(100)) * 2u;
        digit_pos -= 2u;
        digits[digit_pos] = decimal_pairs[pair];
        digits[digit_pos + 1u] = decimal_pairs[pair + 1u];
        remaining = quotient;
    }
    if (remaining < UINT64_C(10)) {
        digits[--digit_pos] = (char)('0' + remaining);
    } else {
        size_t pair = (size_t)remaining * 2u;
        digit_pos -= 2u;
        digits[digit_pos] = decimal_pairs[pair];
        digits[digit_pos + 1u] = decimal_pairs[pair + 1u];
    }
    return digit_pos;
}

static void stage_decimal_prefix_init(
    stage_decimal_prefix *prefix,
    int64_t value
) {
    char digits[STAGE_DECIMAL_DIGITS];
    uint64_t high;
    size_t start;
    prefix->ready = 0;
    if (value < (int64_t)STAGE_DECIMAL_SUFFIX_BASE) return;
    high = (uint64_t)value / STAGE_DECIMAL_SUFFIX_BASE;
    prefix->base = high * STAGE_DECIMAL_SUFFIX_BASE;
    start = write_decimal_digits(digits, high);
    prefix->length = STAGE_DECIMAL_DIGITS - start;
    memcpy(prefix->digits, digits + start, prefix->length);
    prefix->ready = 1;
}

enum {
    STAGE_TTS_FIRST_TEXT_VALUE_OFFSET = 39u,
    STAGE_TTS_SEGMENT_VALUE_OFFSET = 89u,
    STAGE_TTS_RECEIVED_VALUE_OFFSET = 140u,
    STAGE_TTS_PROVIDER_START_VALUE_OFFSET = 199u,
    STAGE_TTS_PROVIDER_READY_VALUE_OFFSET = 248u,
    STAGE_TTS_PCM_START_VALUE_OFFSET = 290u,
    STAGE_TTS_JSON_LENGTH = 304u,
    STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET = 32u,
    STAGE_PCM_FIRST_CHUNK_JSON_LENGTH = 47u,
    STAGE_WIRE_FIRST_TEXT_VALUE_OFFSET = 29u,
    STAGE_WIRE_SEGMENT_VALUE_OFFSET = 80u,
    STAGE_WIRE_RECEIVED_VALUE_OFFSET = 132u,
    STAGE_WIRE_PROVIDER_START_VALUE_OFFSET = 192u,
    STAGE_WIRE_PROVIDER_READY_VALUE_OFFSET = 242u,
    STAGE_WIRE_PCM_START_VALUE_OFFSET = 285u,
    STAGE_WIRE_LENGTH = 298u,
    STAGE_WIRE_PCM_FIRST_CHUNK_VALUE_OFFSET = 34u,
    STAGE_WIRE_PCM_FIRST_CHUNK_LENGTH = 47u,
};

static const char stage_tts_json[] =
    ",\"metadata\":{\"stage_first_text_at_ms\":\"0000000000000\""
    ",\"stage_tts_segment_emitted_at_ms\":\"0000000000000\""
    ",\"stage_tts_request_received_at_ms\":\"0000000000000\""
    ",\"stage_tts_provider_request_started_at_ms\":\"0000000000000\""
    ",\"stage_tts_provider_ready_at_ms\":\"0000000000000\""
    ",\"stage_pcm_started_at_ms\":\"0000000000000\"";

static const char stage_pcm_first_chunk_json[] =
    ",\"stage_pcm_first_chunk_at_ms\":\"0000000000000\"}";

_Static_assert(
    sizeof(stage_tts_json) - 1u == STAGE_TTS_JSON_LENGTH,
    "current TTS stage JSON length must match its offsets");
_Static_assert(
    STAGE_TTS_PCM_START_VALUE_OFFSET + STAGE_DECIMAL_CURRENT_DIGITS + 1u ==
        STAGE_TTS_JSON_LENGTH,
    "current TTS stage JSON offsets must cover the template");
_Static_assert(
    sizeof(stage_pcm_first_chunk_json) - 1u ==
        STAGE_PCM_FIRST_CHUNK_JSON_LENGTH,
    "current first PCM stage JSON length must match its offset");
_Static_assert(
    STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET + STAGE_DECIMAL_CURRENT_DIGITS + 2u ==
        STAGE_PCM_FIRST_CHUNK_JSON_LENGTH,
    "current first PCM stage JSON offset must cover the template");
_Static_assert(
    STAGE_WIRE_PCM_START_VALUE_OFFSET + STAGE_DECIMAL_CURRENT_DIGITS ==
        STAGE_WIRE_LENGTH,
    "current TTS stage wire offsets must cover the source");
_Static_assert(
    STAGE_WIRE_PCM_FIRST_CHUNK_VALUE_OFFSET + STAGE_DECIMAL_CURRENT_DIGITS ==
        STAGE_WIRE_PCM_FIRST_CHUNK_LENGTH,
    "current first PCM wire offset must cover the source");

static inline int stage_decimal_prefix_contains(
    const stage_decimal_prefix *prefix,
    int64_t value
) {
    uint64_t remaining;
    if (!prefix || !prefix->ready || value < 0) return 0;
    remaining = (uint64_t)value;
    return remaining >= prefix->base &&
        remaining - prefix->base < STAGE_DECIMAL_SUFFIX_BASE;
}

static inline void write_current_stage_value(
    char *out,
    const stage_decimal_prefix *prefix,
    int64_t value
) {
    uint32_t low = (uint32_t)((uint64_t)value - prefix->base);
    memcpy(out, prefix->digits, STAGE_DECIMAL_CURRENT_PREFIX_DIGITS);
    write_decimal_suffix_digits(
        out + STAGE_DECIMAL_CURRENT_PREFIX_DIGITS, low);
}

static size_t write_current_tts_stages(
    const turn_stage_timestamps_c *stages,
    const stage_decimal_prefix *prefix,
    char *out,
    size_t out_cap
) {
    size_t length = STAGE_TTS_JSON_LENGTH + 1u;
    if (stages->pcm_first_chunk_at_ms > 0)
        length = STAGE_TTS_JSON_LENGTH + STAGE_PCM_FIRST_CHUNK_JSON_LENGTH;
    if (length >= out_cap) {
        out[0] = '\0';
        return SIZE_MAX;
    }
    memcpy(out, stage_tts_json, STAGE_TTS_JSON_LENGTH);
    write_current_stage_value(
        out + STAGE_TTS_FIRST_TEXT_VALUE_OFFSET,
        prefix,
        stages->first_text_at_ms);
    write_current_stage_value(
        out + STAGE_TTS_SEGMENT_VALUE_OFFSET,
        prefix,
        stages->tts_segment_emitted_at_ms);
    write_current_stage_value(
        out + STAGE_TTS_RECEIVED_VALUE_OFFSET,
        prefix,
        stages->tts_request_received_at_ms);
    write_current_stage_value(
        out + STAGE_TTS_PROVIDER_START_VALUE_OFFSET,
        prefix,
        stages->tts_provider_request_started_at_ms);
    write_current_stage_value(
        out + STAGE_TTS_PROVIDER_READY_VALUE_OFFSET,
        prefix,
        stages->tts_provider_ready_at_ms);
    write_current_stage_value(
        out + STAGE_TTS_PCM_START_VALUE_OFFSET,
        prefix,
        stages->pcm_started_at_ms);
    if (stages->pcm_first_chunk_at_ms > 0) {
        memcpy(
            out + STAGE_TTS_JSON_LENGTH,
            stage_pcm_first_chunk_json,
            STAGE_PCM_FIRST_CHUNK_JSON_LENGTH);
        write_current_stage_value(
            out + STAGE_TTS_JSON_LENGTH +
                STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET,
            prefix,
            stages->pcm_first_chunk_at_ms);
    } else {
        out[STAGE_TTS_JSON_LENGTH] = '}';
    }
    out[length] = '\0';
    return length;
}

size_t turn_stage_metadata_json_write_current_wire_v1(
    const turn_stage_timestamps_c *stages,
    const uint8_t *current_tts_stage_wire,
    char *out,
    size_t out_cap
) {
    size_t length;
    if (!stages || !current_tts_stage_wire || !out || out_cap == 0u)
        return SIZE_MAX;
    length = stages->pcm_first_chunk_at_ms > 0 ?
        STAGE_TTS_JSON_LENGTH + STAGE_PCM_FIRST_CHUNK_JSON_LENGTH :
        STAGE_TTS_JSON_LENGTH + 1u;
    if (length >= out_cap) {
        out[0] = '\0';
        return SIZE_MAX;
    }
    memcpy(out, stage_tts_json, STAGE_TTS_JSON_LENGTH);
#define COPY_CURRENT_STAGE(json_offset, wire_offset) \
    memcpy( \
        out + (json_offset), current_tts_stage_wire + (wire_offset), \
        STAGE_DECIMAL_CURRENT_DIGITS)
    COPY_CURRENT_STAGE(
        STAGE_TTS_FIRST_TEXT_VALUE_OFFSET,
        STAGE_WIRE_FIRST_TEXT_VALUE_OFFSET);
    COPY_CURRENT_STAGE(
        STAGE_TTS_SEGMENT_VALUE_OFFSET,
        STAGE_WIRE_SEGMENT_VALUE_OFFSET);
    COPY_CURRENT_STAGE(
        STAGE_TTS_RECEIVED_VALUE_OFFSET,
        STAGE_WIRE_RECEIVED_VALUE_OFFSET);
    COPY_CURRENT_STAGE(
        STAGE_TTS_PROVIDER_START_VALUE_OFFSET,
        STAGE_WIRE_PROVIDER_START_VALUE_OFFSET);
    COPY_CURRENT_STAGE(
        STAGE_TTS_PROVIDER_READY_VALUE_OFFSET,
        STAGE_WIRE_PROVIDER_READY_VALUE_OFFSET);
    COPY_CURRENT_STAGE(
        STAGE_TTS_PCM_START_VALUE_OFFSET,
        STAGE_WIRE_PCM_START_VALUE_OFFSET);
    if (stages->pcm_first_chunk_at_ms > 0) {
        memcpy(
            out + STAGE_TTS_JSON_LENGTH,
            stage_pcm_first_chunk_json,
            STAGE_PCM_FIRST_CHUNK_JSON_LENGTH);
        COPY_CURRENT_STAGE(
            STAGE_TTS_JSON_LENGTH + STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET,
            STAGE_WIRE_LENGTH + STAGE_WIRE_PCM_FIRST_CHUNK_VALUE_OFFSET);
    } else {
        out[STAGE_TTS_JSON_LENGTH] = '}';
    }
#undef COPY_CURRENT_STAGE
    out[length] = '\0';
    return length;
}

static size_t append_stage(
    char *out,
    size_t out_cap,
    int comma,
    const char *key,
    size_t key_len,
    uint64_t value,
    const stage_decimal_prefix *prefix
) {
    char digits[STAGE_DECIMAL_DIGITS];
    size_t digit_pos;
    size_t digit_len;
    size_t needed;
    uint64_t remaining;
    uint64_t suffix = 0;
    int use_prefix;
    char *cursor = out;
    if (!out || !key || !prefix) return (size_t)-1;
    remaining = value;
    use_prefix = prefix->ready && remaining >= prefix->base &&
        remaining - prefix->base < STAGE_DECIMAL_SUFFIX_BASE;
    if (use_prefix) {
        suffix = remaining - prefix->base;
        digit_pos = 0;
        digit_len = prefix->length + STAGE_DECIMAL_SUFFIX_DIGITS;
    } else {
        digit_pos = write_decimal_digits(digits, remaining);
        digit_len = sizeof(digits) - digit_pos;
    }
    needed = (comma ? 1u : 0u) + 1u + key_len + 3u + digit_len + 1u;
    if (needed >= out_cap) return (size_t)-1;
    if (comma) *cursor++ = ',';
    *cursor++ = '"';
    memcpy(cursor, key, key_len);
    cursor += key_len;
    memcpy(cursor, "\":\"", 3u);
    cursor += 3u;
    if (use_prefix) {
        memcpy(cursor, prefix->digits, prefix->length);
        cursor += prefix->length;
        write_decimal_suffix_digits(cursor, (uint32_t)suffix);
        cursor += STAGE_DECIMAL_SUFFIX_DIGITS;
    } else {
        memcpy(cursor, digits + digit_pos, digit_len);
        cursor += digit_len;
    }
    *cursor++ = '"';
    return (size_t)(cursor - out);
}

size_t turn_stage_metadata_json_write_v1(
    const turn_stage_timestamps_c *stages,
    char *out,
    size_t out_cap
) {
    static const char prefix[] = ",\"metadata\":{";
    stage_decimal_prefix decimal_prefix;
    size_t used = sizeof(prefix) - 1u;
    int have_fields = 0;
    if (!stages || !out || out_cap == 0) return SIZE_MAX;
    out[0] = '\0';
    if (stages->tts_request_received_at_ms == 0) return 0;
    stage_decimal_prefix_init(
        &decimal_prefix, stages->tts_request_received_at_ms);
    if (stages->input.audio_committed_at_ms <= 0 &&
        stages->input.stt_request_received_at_ms <= 0 &&
        stages->input.stt_provider_request_started_at_ms <= 0 &&
        stages->input.stt_provider_ready_at_ms <= 0 &&
        stages->input.stt_transcript_published_at_ms <= 0 &&
        stages->first_text_at_ms > 0 &&
        stages->tts_segment_emitted_at_ms > 0 &&
        stages->tts_provider_request_started_at_ms > 0 &&
        stages->tts_provider_ready_at_ms > 0 &&
        stages->pcm_started_at_ms > 0 && decimal_prefix.ready &&
        decimal_prefix.length == STAGE_DECIMAL_CURRENT_PREFIX_DIGITS &&
        stage_decimal_prefix_contains(
            &decimal_prefix, stages->first_text_at_ms) &&
        stage_decimal_prefix_contains(
            &decimal_prefix, stages->tts_segment_emitted_at_ms) &&
        stage_decimal_prefix_contains(
            &decimal_prefix, stages->tts_provider_request_started_at_ms) &&
        stage_decimal_prefix_contains(
            &decimal_prefix, stages->tts_provider_ready_at_ms) &&
        stage_decimal_prefix_contains(
            &decimal_prefix, stages->pcm_started_at_ms) &&
        (stages->pcm_first_chunk_at_ms <= 0 ||
         stage_decimal_prefix_contains(
            &decimal_prefix, stages->pcm_first_chunk_at_ms)))
        return write_current_tts_stages(
            stages, &decimal_prefix, out, out_cap);
    if (used >= out_cap) return SIZE_MAX;
    memcpy(out, prefix, used);
#define APPEND_STAGE(member, key) do { \
        int64_t stage_value = stages->member; \
        if (stage_value > 0) { \
            size_t next = append_stage( \
                out + used, out_cap - used, have_fields, (key), \
                sizeof(key) - 1u, (uint64_t)stage_value, &decimal_prefix); \
            if (next == (size_t)-1) { \
                out[0] = '\0'; \
                return SIZE_MAX; \
            } \
            used += next; \
            have_fields = 1; \
        } \
    } while (0)
    APPEND_STAGE(input.audio_committed_at_ms, "stage_audio_committed_at_ms");
    APPEND_STAGE(input.stt_request_received_at_ms, "stage_stt_request_received_at_ms");
    APPEND_STAGE(
        input.stt_provider_request_started_at_ms,
        "stage_stt_provider_request_started_at_ms");
    APPEND_STAGE(input.stt_provider_ready_at_ms, "stage_stt_provider_ready_at_ms");
    APPEND_STAGE(
        input.stt_transcript_published_at_ms,
        "stage_stt_transcript_published_at_ms");
    APPEND_STAGE(first_text_at_ms, "stage_first_text_at_ms");
    APPEND_STAGE(tts_segment_emitted_at_ms, "stage_tts_segment_emitted_at_ms");
    APPEND_STAGE(tts_request_received_at_ms, "stage_tts_request_received_at_ms");
    APPEND_STAGE(
        tts_provider_request_started_at_ms,
        "stage_tts_provider_request_started_at_ms");
    APPEND_STAGE(tts_provider_ready_at_ms, "stage_tts_provider_ready_at_ms");
    APPEND_STAGE(pcm_started_at_ms, "stage_pcm_started_at_ms");
    APPEND_STAGE(pcm_first_chunk_at_ms, "stage_pcm_first_chunk_at_ms");
#undef APPEND_STAGE
    if (!have_fields || out_cap - used < 2u) {
        out[0] = '\0';
        return SIZE_MAX;
    }
    out[used++] = '}';
    out[used] = '\0';
    return used;
}

int turn_stage_metadata_json_v1(
    const turn_stage_timestamps_c *stages,
    char *out,
    size_t out_cap
) {
    return turn_stage_metadata_json_write_v1(
        stages, out, out_cap) == SIZE_MAX ? -1 : 0;
}
