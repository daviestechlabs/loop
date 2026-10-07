#include "pb_min.h"

#include <assert.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define PB_NOINLINE __attribute__((noinline))
#define PB_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define PB_NOINLINE
#define PB_ALWAYS_INLINE inline
#endif

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len) {
    if (!r) return;
    r->data = data;
    r->len = len;
    r->pos = 0;
}

static int read_varint_raw(pb_reader *r, uint64_t *out) {
    uint64_t result = 0;
    unsigned shift = 0;
    unsigned byte_index = 0;
    if (!r || !out || (!r->data && r->len != 0)) return -1;
    while (r->pos < r->len && byte_index < 10u) {
        uint8_t b = r->data[r->pos++];
        if (byte_index == 9u && b > 1u) return -1;
        result |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            if (byte_index != 0u && b == 0u) return -1;
            *out = result;
            return 0;
        }
        shift += 7;
        byte_index++;
    }
    return -1;
}

int pb_read_varint(pb_reader *r, uint64_t *out) {
    return read_varint_raw(r, out);
}

int pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire) {
    uint64_t tag;
    if (!field || !wire) return -1;
    if (read_varint_raw(r, &tag) != 0) return -1;
    if ((tag >> 3) == 0 || (tag >> 3) > 0x1fffffffu) return -1;
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7u);
    return 0;
}

int pb_read_bytes(pb_reader *r, const uint8_t **out, size_t *out_len) {
    uint64_t len;
    if (!r || !out || !out_len || r->pos > r->len) return -1;
    if (read_varint_raw(r, &len) != 0) return -1;
    if (len > SIZE_MAX || (size_t)len > r->len - r->pos) return -1;
    *out = r->data + r->pos;
    *out_len = (size_t)len;
    r->pos += (size_t)len;
    return 0;
}

static int pb_read_string_view(
    pb_reader *r,
    size_t out_cap,
    const uint8_t **out,
    size_t *out_len
) {
    if (!out || !out_len || out_cap == 0) return -1;
    if (pb_read_bytes(r, out, out_len) != 0) return -1;
    return *out_len < out_cap && memchr(*out, '\0', *out_len) == NULL ? 0 : -1;
}

static int pb_read_string_sized(
    pb_reader *r,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    const uint8_t *bytes;
    size_t len;
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    if (pb_read_string_view(r, out_cap, &bytes, &len) != 0) return -1;
    memcpy(out, bytes, len);
    out[len] = '\0';
    if (out_len) *out_len = len;
    return 0;
}

int pb_read_string(pb_reader *r, char *out, size_t out_cap) {
    return pb_read_string_sized(r, out, out_cap, NULL);
}

int pb_skip(pb_reader *r, uint32_t wire) {
    uint64_t v;
    const uint8_t *b;
    size_t n;
    switch (wire) {
    case 0:
        return read_varint_raw(r, &v);
    case 1:
        if (!r || r->pos > r->len || r->len - r->pos < 8) return -1;
        r->pos += 8;
        return 0;
    case 2:
        return pb_read_bytes(r, &b, &n);
    case 5:
        if (!r || r->pos > r->len || r->len - r->pos < 4) return -1;
        r->pos += 4;
        return 0;
    default:
        return -1;
    }
}

static size_t write_varint(uint8_t *out, size_t cap, size_t pos, uint64_t v) {
    if (!out || pos > cap) return (size_t)-1;
    while (v >= 0x80) {
        if (pos >= cap) return (size_t)-1;
        out[pos++] = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    if (pos >= cap) return (size_t)-1;
    out[pos++] = (uint8_t)v;
    return pos;
}

static size_t write_tag(uint8_t *out, size_t cap, size_t pos, uint32_t field, uint32_t wire) {
    return write_varint(out, cap, pos, ((uint64_t)field << 3) | wire);
}

static size_t write_string_field_n(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    const char *s,
    size_t len
) {
    if (len != 0u && !s) return (size_t)-1;
    pos = write_tag(out, cap, pos, field, 2);
    if (pos == (size_t)-1) return pos;
    pos = write_varint(out, cap, pos, len);
    if (pos == (size_t)-1) return pos;
    if (pos > cap || len > cap - pos) return (size_t)-1;
    if (len) memcpy(out + pos, s, len);
    return pos + len;
}

static size_t write_string_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    const char *s
) {
    return write_string_field_n(
        out, cap, pos, field, s, s ? strlen(s) : 0u);
}

static size_t bounded_string_len(const char *s, size_t cap) {
    const char *end;
    if (!s) return cap;
    end = (const char *)memchr(s, '\0', cap);
    return end ? (size_t)(end - s) : cap;
}

static size_t write_varint_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    uint64_t v
) {
    pos = write_tag(out, cap, pos, field, 0);
    if (pos == (size_t)-1) return pos;
    return write_varint(out, cap, pos, v);
}

static size_t write_bytes_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    const uint8_t *data,
    size_t len
) {
    pos = write_tag(out, cap, pos, field, 2);
    if (pos == (size_t)-1) return pos;
    pos = write_varint(out, cap, pos, len);
    if (pos == (size_t)-1) return pos;
    if (pos > cap || len > cap - pos || (len != 0 && !data)) return (size_t)-1;
    if (len) memcpy(out + pos, data, len);
    return pos + len;
}

size_t pb_encode_error_response(
    uint8_t *out,
    size_t out_cap,
    const char *message,
    const char *type
) {
    size_t pos = 0;
    if (!out || !message || !message[0] || !type || !type[0]) return 0;
    pos = write_varint_field(out, out_cap, pos, 1, 1);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, message);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 3, type);
    return pos == (size_t)-1 ? 0 : pos;
}

int pb_decode_error_response(const uint8_t *data, size_t len, error_response_c *out) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > 1) return -1;
            out->error = (int)value;
        } else if (wire == 2 && (field == 2 || field == 3)) {
            char *dst = field == 2 ? out->message : out->type;
            size_t cap = field == 2 ? sizeof(out->message) : sizeof(out->type);
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    return out->error && out->message[0] && out->type[0] ? 0 : -1;
}

size_t pb_encode_stt_stream_message_at(
    uint8_t *out,
    size_t out_cap,
    const char *type,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int64_t timestamp_ms
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || !type || !type[0] || (audio_len != 0 && !audio) ||
        sample_rate <= 0 || channels <= 0 || bit_depth <= 0 || timestamp_ms < 0) return 0;
    pos = write_string_field(out, out_cap, pos, 1, type ? type : "");
    if (pos == (size_t)-1) return 0;
    if (audio_len > 0) {
        pos = write_bytes_field(out, out_cap, pos, 2, audio, audio_len);
        if (pos == (size_t)-1) return 0;
    }
    pos = write_varint_field(out, out_cap, pos, 5, (uint64_t)(uint32_t)sample_rate);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 6, (uint64_t)(uint32_t)channels);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 7, (uint64_t)(uint32_t)bit_depth);
    if (pos == (size_t)-1) return 0;
    if (timestamp_ms > 0) {
        pos = write_varint_field(out, out_cap, pos, 9, (uint64_t)timestamp_ms);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_stt_stream_message(
    uint8_t *out,
    size_t out_cap,
    const char *type,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth
) {
    return pb_encode_stt_stream_message_at(
        out, out_cap, type, audio, audio_len,
        sample_rate, channels, bit_depth, 0);
}

static int decode_product_stt_stream_message(
    const uint8_t *data,
    size_t len,
    stt_stream_message_c *out
) {
    /* sample_rate(5)=16000, channels(6)=1, bit_depth(7)=16. */
    static const uint8_t audio_format[] = {
        0x28u, 0x80u, 0x7du, 0x30u, 0x01u, 0x38u, 0x10u
    };
    const uint8_t *type;
    const uint8_t *audio = NULL;
    pb_reader timestamp_reader;
    size_t type_len;
    size_t audio_len = 0;
    size_t pos = 0;
    uint64_t value;
    int64_t timestamp_ms = 0;
    uint8_t length_byte;

    if (!data || !out || pos >= len || data[pos++] != 0x0au || pos >= len)
        return -1;
    length_byte = data[pos++];
    if ((length_byte & 0x80u) != 0u) return -1;
    type_len = length_byte;
    if (type_len >= sizeof(out->type) || type_len > len - pos) return -1;
    type = data + pos;
    if (memchr(type, '\0', type_len) != NULL) return -1;
    pos += type_len;

    if (pos < len && data[pos] == 0x12u) {
        pos++;
        if (pos >= len) return -1;
        length_byte = data[pos++];
        audio_len = (size_t)(length_byte & 0x7fu);
        if ((length_byte & 0x80u) != 0u) {
            uint8_t high;
            if (pos >= len) return -1;
            high = data[pos++];
            if (high == 0u || (high & 0x80u) != 0u) return -1;
            audio_len |= (size_t)high << 7u;
        }
        if (audio_len > len - pos) return -1;
        audio = data + pos;
        pos += audio_len;
    }

    if (sizeof(audio_format) > len - pos ||
        memcmp(data + pos, audio_format, sizeof(audio_format)) != 0) return -1;
    pos += sizeof(audio_format);

    if (pos < len && data[pos] == 0x48u) {
        pos++;
        pb_reader_init(&timestamp_reader, data, len);
        timestamp_reader.pos = pos;
        if (read_varint_raw(&timestamp_reader, &value) != 0 ||
            value == 0u || value > INT64_MAX) return -1;
        pos = timestamp_reader.pos;
        timestamp_ms = (int64_t)value;
    }
    if (pos != len) return -1;

    memcpy(out->type, type, type_len);
    out->type[type_len] = '\0';
    out->audio = audio;
    out->audio_len = audio_len;
    out->sample_rate = 16000;
    out->channels = 1;
    out->bit_depth = 16;
    out->timestamp_ms = timestamp_ms;
    return 0;
}

int pb_decode_stt_stream_message(const uint8_t *data, size_t len, stt_stream_message_c *out) {
    pb_reader r;
    int owner_seen = 0;
    if (!out || (!data && len != 0)) return -1;
    out->type[0] = '\0';
    out->audio = NULL;
    out->audio_len = 0u;
    out->sample_rate = 16000;
    out->channels = 1;
    out->bit_depth = 16;
    out->timestamp_ms = 0;
    out->utterance_id[0] = '\0';
    out->speaker_id[0] = '\0';
    out->state[0] = '\0';
    out->user_id[0] = '\0';
    if (decode_product_stt_stream_message(data, len, out) == 0) return 0;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        switch (field) {
        case 1:
            if (wire != 2 || pb_read_string(&r, out->type, sizeof(out->type)) != 0) return -1;
            break;
        case 2:
            if (wire != 2 || pb_read_bytes(&r, &out->audio, &out->audio_len) != 0) return -1;
            break;
        case 3:
            if (wire != 2 || pb_read_string(&r, out->state, sizeof(out->state)) != 0) return -1;
            break;
        case 4:
            if (wire != 2 || pb_read_string(&r, out->speaker_id, sizeof(out->speaker_id)) != 0) return -1;
            break;
        case 5: {
            uint64_t v;
            if (wire != 0 || pb_read_varint(&r, &v) != 0) return -1;
            if (v > INT32_MAX) return -1;
            out->sample_rate = (int32_t)v;
            break;
        }
        case 6: {
            uint64_t v;
            if (wire != 0 || pb_read_varint(&r, &v) != 0) return -1;
            if (v > INT32_MAX) return -1;
            out->channels = (int32_t)v;
            break;
        }
        case 7: {
            uint64_t v;
            if (wire != 0 || pb_read_varint(&r, &v) != 0) return -1;
            if (v > INT32_MAX) return -1;
            out->bit_depth = (int32_t)v;
            break;
        }
        case 9: {
            uint64_t v;
            if (wire != 0 || pb_read_varint(&r, &v) != 0 || v == 0 || v > INT64_MAX)
                return -1;
            out->timestamp_ms = (int64_t)v;
            break;
        }
        case 11:
            if (wire != 2 || pb_read_string(&r, out->utterance_id, sizeof(out->utterance_id)) != 0)
                return -1;
            break;
        case 13:
            if (wire != 2 || owner_seen ||
                pb_read_string(&r, out->user_id, sizeof(out->user_id)) != 0) return -1;
            owner_seen = 1;
            break;
        default:
            if (pb_skip(&r, wire) != 0) return -1;
            break;
        }
    }
    return 0;
}

size_t pb_append_stt_owner(uint8_t *out, size_t capacity, size_t length,
    const char *user_id) {
    stt_stream_message_c decoded;
    if (!out || length > capacity || !user_id || !user_id[0] ||
        strlen(user_id) >= sizeof(decoded.user_id) ||
        decode_product_stt_stream_message(out, length, &decoded) ||
        strcmp(decoded.type, "start")) return 0;
    size_t n = write_string_field(out, capacity, length, 13u, user_id);
    return n == (size_t)-1 ? 0 : n;
}

size_t pb_encode_stt_lifecycle_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    size_t session_id_len,
    int type_id,
    int64_t timestamp_ms
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || (!session_id && session_id_len != 0u) ||
        type_id < STT_LIFECYCLE_STREAM_STARTED ||
        type_id > STT_LIFECYCLE_TRANSCRIPTION_FAILED || timestamp_ms <= 1)
        return 0;
    if (session_id_len < 0x80u) {
        if (pos >= out_cap) return 0;
        out[pos++] = 0x0au;
        if (pos >= out_cap) return 0;
        out[pos++] = (uint8_t)session_id_len;
        if (session_id_len > out_cap - pos) return 0;
        if (session_id_len != 0u)
            memcpy(out + pos, session_id, session_id_len);
        pos += session_id_len;
        if (pos >= out_cap) return 0;
        out[pos++] = 0x18u;
        if (pos >= out_cap) return 0;
        out[pos++] = (uint8_t)type_id;
        if (pos >= out_cap) return 0;
        out[pos++] = 0x28u;
        pos = write_varint(out, out_cap, pos, (uint64_t)timestamp_ms);
        return pos == (size_t)-1 ? 0u : pos;
    }
    pos = write_string_field_n(
        out, out_cap, pos, 1, session_id, session_id_len);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 3, (uint64_t)type_id);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 5, (uint64_t)timestamp_ms);
    if (pos == (size_t)-1) return 0;
    return pos;
}

size_t pb_encode_stt_lifecycle(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    const char *type,
    int64_t timestamp_ms
) {
    int type_id = 0;
    if (type && strcmp(type, "stream_started") == 0) type_id = STT_LIFECYCLE_STREAM_STARTED;
    else if (type && strcmp(type, "speech_started") == 0) type_id = STT_LIFECYCLE_SPEECH_STARTED;
    else if (type && strcmp(type, "speech_ended") == 0) type_id = STT_LIFECYCLE_SPEECH_ENDED;
    else if (type && strcmp(type, "stream_ended") == 0) type_id = STT_LIFECYCLE_STREAM_ENDED;
    else if (type && strcmp(type, "transcription_failed") == 0)
        type_id = STT_LIFECYCLE_TRANSCRIPTION_FAILED;
    return pb_encode_stt_lifecycle_prepared(
        out,
        out_cap,
        session_id,
        session_id ? strlen(session_id) : 0u,
        type_id,
        timestamp_ms);
}

static const char *lifecycle_type_name(uint64_t type_id) {
    switch (type_id) {
    case STT_LIFECYCLE_STREAM_STARTED: return "stream_started";
    case STT_LIFECYCLE_SPEECH_STARTED: return "speech_started";
    case STT_LIFECYCLE_SPEECH_ENDED: return "speech_ended";
    case STT_LIFECYCLE_STREAM_ENDED: return "stream_ended";
    case STT_LIFECYCLE_TRANSCRIPTION_FAILED: return "transcription_failed";
    default: return NULL;
    }
}

int pb_decode_stt_lifecycle(const uint8_t *data, size_t len, stt_lifecycle_c *out) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->session_id, sizeof(out->session_id)) != 0) return -1;
        } else if (field == 2 && wire == 2) {
            if (pb_read_string(&r, out->utterance_id, sizeof(out->utterance_id)) != 0) return -1;
        } else if (field == 3 && wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0) return -1;
            if (!lifecycle_type_name(v)) return -1;
            out->type_id = (int)v;
        } else if (field == 5 && wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > INT64_MAX || v <= 1) return -1;
            out->timestamp_ms = (int64_t)v;
        } else {
            if (pb_skip(&r, wire) != 0) return -1;
        }
    }
    if (out->type_id != 0) {
        const char *name = lifecycle_type_name((uint64_t)out->type_id);
        memcpy(out->type, name, strlen(name) + 1);
    }
    return 0;
}

size_t pb_encode_stt_interrupt(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int64_t timestamp_ms
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || !session_id || !session_id[0] || timestamp_ms <= 1) return 0;
    pos = write_string_field(out, out_cap, pos, 1, session_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, "interrupt");
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 3, (uint64_t)timestamp_ms);
    return pos == (size_t)-1 ? 0 : pos;
}

static int input_stages_any(const turn_input_stage_timestamps_c *stages) {
    return stages &&
        (stages->audio_committed_at_ms != 0 ||
         stages->stt_request_received_at_ms != 0 ||
         stages->stt_provider_request_started_at_ms != 0 ||
         stages->stt_provider_ready_at_ms != 0 ||
         stages->stt_transcript_published_at_ms != 0);
}

static int input_stages_valid(const turn_input_stage_timestamps_c *stages) {
    if (!input_stages_any(stages)) return 1;
    return stages->audio_committed_at_ms > 0 &&
        stages->stt_request_received_at_ms >= stages->audio_committed_at_ms &&
        stages->stt_provider_request_started_at_ms >=
            stages->stt_request_received_at_ms &&
        stages->stt_provider_ready_at_ms >=
            stages->stt_provider_request_started_at_ms &&
        stages->stt_transcript_published_at_ms >=
            stages->stt_provider_ready_at_ms;
}

static size_t write_input_stage_fields(
    uint8_t *out,
    size_t out_cap,
    size_t pos,
    uint32_t first_field,
    const turn_input_stage_timestamps_c *stages
) {
    const int64_t values[] = {
        stages ? stages->audio_committed_at_ms : 0,
        stages ? stages->stt_request_received_at_ms : 0,
        stages ? stages->stt_provider_request_started_at_ms : 0,
        stages ? stages->stt_provider_ready_at_ms : 0,
        stages ? stages->stt_transcript_published_at_ms : 0
    };
    size_t i;
    if (!input_stages_any(stages)) return pos;
    if (!input_stages_valid(stages)) return (size_t)-1;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        pos = write_varint_field(
            out, out_cap, pos, first_field + (uint32_t)i,
            (uint64_t)values[i]);
        if (pos == (size_t)-1) return pos;
    }
    return pos;
}

static int assign_unseen_input_stage(
    turn_input_stage_timestamps_c *stages,
    uint32_t index,
    uint64_t value
) {
    _Static_assert(
        offsetof(
            turn_input_stage_timestamps_c,
            stt_transcript_published_at_ms) <= UINT16_MAX,
        "input stage offset exceeds its decoder map");
    static const uint16_t offsets[] = {
        (uint16_t)offsetof(
            turn_input_stage_timestamps_c, audio_committed_at_ms),
        (uint16_t)offsetof(
            turn_input_stage_timestamps_c, stt_request_received_at_ms),
        (uint16_t)offsetof(
            turn_input_stage_timestamps_c,
            stt_provider_request_started_at_ms),
        (uint16_t)offsetof(
            turn_input_stage_timestamps_c, stt_provider_ready_at_ms),
        (uint16_t)offsetof(
            turn_input_stage_timestamps_c, stt_transcript_published_at_ms),
    };
    int64_t decoded;
    char *field;
    if (!stages || index >= 5u || value == 0 || value > INT64_MAX) return -1;
    field = (char *)stages + offsets[index];
    decoded = (int64_t)value;
    memcpy(field, &decoded, sizeof(decoded));
    return 0;
}

int pb_decode_stt_transcription(
    const uint8_t *data,
    size_t len,
    stt_transcription_c *out
) {
    pb_reader r;
    unsigned input_timing_seen = 0;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->session_id, sizeof(out->session_id)) != 0) return -1;
        } else if (field == 2 && wire == 2) {
            if (pb_read_string(&r, out->transcript, sizeof(out->transcript)) != 0) return -1;
        } else if (field == 9 && wire == 2) {
            if (pb_read_string(&r, out->state, sizeof(out->state)) != 0) return -1;
        } else if ((field == 3 || field == 4 || field == 5 || field == 6 ||
                    field == 8 || field == 17 || field == 18 ||
                    (field >= 26 && field <= 29)) &&
                   wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0) return -1;
            if (field == 3) {
                if (v > INT32_MAX) return -1;
                out->sequence = (int32_t)v;
            } else if (field == 4) {
                if (v > 1) return -1;
                out->is_partial = (int)v;
            } else if (field == 5) {
                if (v > 1) return -1;
                out->is_final = (int)v;
            } else if (field == 6) {
                if (v > INT64_MAX) return -1;
                out->timestamp_ms = (int64_t)v;
            } else if (field == 8) {
                if (v > 1) return -1;
                out->has_voice_activity = (int)v;
            } else if (field == 18) {
                if (v > 2) return -1;
                out->stream_state = (int)v;
            } else if (field >= 26 && field <= 29) {
                unsigned bit = 1u << (field - 26u);
                int64_t *stage = NULL;
                if ((input_timing_seen & bit) != 0 || v == 0 || v > INT64_MAX)
                    return -1;
                input_timing_seen |= bit;
                if (field == 26) stage = &out->input_stages.audio_committed_at_ms;
                else if (field == 27)
                    stage = &out->input_stages.stt_request_received_at_ms;
                else if (field == 28)
                    stage = &out->input_stages.stt_provider_request_started_at_ms;
                else stage = &out->input_stages.stt_provider_ready_at_ms;
                *stage = (int64_t)v;
            } else {
                if (v > 1) return -1;
                out->commit_for_turn = (int)v;
            }
        } else {
            if (pb_skip(&r, wire) != 0) return -1;
        }
    }
    if (input_timing_seen != 0) {
        if (input_timing_seen != 15u || out->timestamp_ms <= 0) return -1;
        out->input_stages.stt_transcript_published_at_ms = out->timestamp_ms;
        if (!input_stages_valid(&out->input_stages)) return -1;
    }
    return 0;
}

size_t pb_encode_stt_transcription(
    uint8_t *out,
    size_t out_cap,
    const stt_transcription_c *in
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || !in || !in->session_id[0] || !in->transcript[0] ||
        in->sequence < 0 || (in->is_partial != 0 && in->is_partial != 1) ||
        (in->is_final != 0 && in->is_final != 1) || in->timestamp_ms <= 1 ||
        (in->has_voice_activity != 0 && in->has_voice_activity != 1) ||
        (in->commit_for_turn != 0 && in->commit_for_turn != 1) ||
        in->stream_state < 0 || in->stream_state > 2 ||
        !input_stages_valid(&in->input_stages) ||
        (input_stages_any(&in->input_stages) &&
         in->input_stages.stt_transcript_published_at_ms != in->timestamp_ms)) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->session_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, in->transcript);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 3, (uint64_t)(uint32_t)in->sequence);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 4, (uint64_t)in->is_partial);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 5, (uint64_t)in->is_final);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 6, (uint64_t)in->timestamp_ms);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 8, (uint64_t)in->has_voice_activity);
    if (pos == (size_t)-1) return 0;
    if (in->state[0]) {
        pos = write_string_field(out, out_cap, pos, 9, in->state);
        if (pos == (size_t)-1) return 0;
    }
    pos = write_varint_field(out, out_cap, pos, 17, (uint64_t)in->commit_for_turn);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 18, (uint64_t)in->stream_state);
    if (pos == (size_t)-1) return 0;
    if (input_stages_any(&in->input_stages)) {
#define WRITE_STT_STAGE(field, member) do { \
        pos = write_varint_field( \
            out, out_cap, pos, (field), \
            (uint64_t)in->input_stages.member); \
        if (pos == (size_t)-1) return 0; \
    } while (0)
        WRITE_STT_STAGE(26, audio_committed_at_ms);
        WRITE_STT_STAGE(27, stt_request_received_at_ms);
        WRITE_STT_STAGE(28, stt_provider_request_started_at_ms);
        WRITE_STT_STAGE(29, stt_provider_ready_at_ms);
#undef WRITE_STT_STAGE
    }
    return pos;
}

typedef struct {
    const char *key;
    size_t offset;
    size_t capacity;
} turn_metadata_rule_c;

#define TURN_METADATA_RULE(member) { \
    #member, \
    offsetof(turn_client_metadata_c, member), \
    sizeof(((turn_client_metadata_c *)0)->member) \
}

static const turn_metadata_rule_c turn_metadata_rules[] = {
    TURN_METADATA_RULE(turn_source),
    TURN_METADATA_RULE(turn_kind),
    TURN_METADATA_RULE(client_trace_id),
    TURN_METADATA_RULE(client_transport),
    TURN_METADATA_RULE(client_surface),
    TURN_METADATA_RULE(interaction_profile),
    TURN_METADATA_RULE(voice_mode),
    TURN_METADATA_RULE(turn_profile),
    TURN_METADATA_RULE(retrieval_skip),
    TURN_METADATA_RULE(agent_id),
    TURN_METADATA_RULE(task_intent),
    TURN_METADATA_RULE(turn_max_tokens),
    TURN_METADATA_RULE(input_mode),
    TURN_METADATA_RULE(capability_id),
    TURN_METADATA_RULE(parent_bundle),
    TURN_METADATA_RULE(prompt_hash),
    TURN_METADATA_RULE(product_session_id),
    TURN_METADATA_RULE(campaign_id),
    TURN_METADATA_RULE(scene_id),
    TURN_METADATA_RULE(character_id),
    TURN_METADATA_RULE(encounter_id),
    TURN_METADATA_RULE(requested_npc_id),
    TURN_METADATA_RULE(knowledge_scope),
    TURN_METADATA_RULE(dnd_session_recap),
    TURN_METADATA_RULE(dnd_forget_session_recap),
    TURN_METADATA_RULE(recap_session_id),
    TURN_METADATA_RULE(dnd_cold_open),
    TURN_METADATA_RULE(evaluation),
    TURN_METADATA_RULE(retrieval_force),
    TURN_METADATA_RULE(memory_context_version),
    TURN_METADATA_RULE(audio_group_session),
    TURN_METADATA_RULE(audio_participant_id),
    TURN_METADATA_RULE(audio_participant_label),
};

#undef TURN_METADATA_RULE

static int turn_metadata_rule_find(const char *key, size_t *index) {
    size_t i;
    if (!key || !key[0] || !index) return 0;
    for (i = 0; i < sizeof(turn_metadata_rules) / sizeof(turn_metadata_rules[0]); ++i) {
        if (strcmp(key, turn_metadata_rules[i].key) == 0) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int copy_bounded_exact(char *dst, size_t dst_cap, const char *src) {
    size_t len;
    if (!dst || dst_cap == 0u || !src || !src[0]) return -1;
    len = strlen(src);
    if (len >= dst_cap) return -1;
    memcpy(dst, src, len + 1u);
    return 0;
}

int pb_turn_client_metadata_set(turn_client_metadata_c *out, const char *key, const char *value) {
    size_t index;
    char *destination;
    if (!out || !key || !key[0] || !value) return -1;
    if (!turn_metadata_rule_find(key, &index)) return 0;
    destination = (char *)out + turn_metadata_rules[index].offset;
    if (destination[0] || copy_bounded_exact(destination, turn_metadata_rules[index].capacity, value) != 0)
        return -1;
    return 1;
}

/* Decode one protobuf map<string,string> entry (fields 1=key, 2=value). */
static int decode_map_string_entry(
    const uint8_t *entry,
    size_t entry_len,
    char *key,
    size_t key_cap,
    char *val,
    size_t val_cap
) {
    pb_reader er;
    int key_seen = 0;
    int val_seen = 0;
    if (!entry || !key || key_cap == 0u || !val || val_cap == 0u) return -1;
    key[0] = '\0';
    val[0] = '\0';
    pb_reader_init(&er, entry, entry_len);
    while (er.pos < er.len) {
        uint32_t field, wire;
        if (pb_read_tag(&er, &field, &wire) != 0) return -1;
        if ((field == 1u || field == 2u) && wire != 2u) return -1;
        if (wire == 2 && field == 1) {
            if (key_seen || pb_read_string(&er, key, key_cap) != 0) return -1;
            key_seen = 1;
        } else if (wire == 2 && field == 2) {
            if (val_seen || pb_read_string(&er, val, val_cap) != 0) return -1;
            val_seen = 1;
        } else {
            if (pb_skip(&er, wire) != 0) return -1;
        }
    }
    return key_seen && val_seen && key[0] ? 0 : -1;
}

static size_t write_map_string_entry_field(
    uint8_t *out,
    size_t out_cap,
    size_t pos,
    uint32_t field,
    const char *key,
    const char *value
) {
    uint8_t entry[384];
    size_t entry_len = 0;
    if (!key || !key[0] || !value || !value[0]) return (size_t)-1;
    entry_len = write_string_field(entry, sizeof(entry), entry_len, 1, key);
    if (entry_len == (size_t)-1) return (size_t)-1;
    entry_len = write_string_field(entry, sizeof(entry), entry_len, 2, value);
    if (entry_len == (size_t)-1) return (size_t)-1;
    return write_bytes_field(out, out_cap, pos, field, entry, entry_len);
}

size_t pb_append_turn_provider_model(uint8_t *out, size_t capacity, size_t used, const char *model) {
    size_t len, pos;
    if (!out || !used || used > capacity || !model) return 0;
    len = bounded_string_len(model, 256u);
    if (!len || len == 256u) return 0;
    pos = write_map_string_entry_field(out, capacity, used, 16u, "model_id", model);
    if (pos == (size_t)-1) return 0;
    pos = write_map_string_entry_field(out, capacity, pos, 16u, "model_identity_source", "provider_reported");
    return pos == (size_t)-1 ? 0 : pos;
}

size_t pb_append_turn_provider_usage(uint8_t *out, size_t capacity, size_t used,
    uint64_t prompt, uint64_t completion, uint64_t total) {
    static const char *keys[] = {"prompt_tokens", "completion_tokens", "total_tokens"};
    uint64_t counts[] = {prompt, completion, total};
    size_t i;
    if (!out || !used || used > capacity || prompt > UINT64_C(9007199254740991) ||
        completion > UINT64_C(9007199254740991) || total > UINT64_C(9007199254740991) || prompt + completion != total) return 0;
    for (i = 0; i < 3u; ++i) {
        char value[24];
        size_t start = sizeof(value) - 1u;
        uint64_t count = counts[i];
        value[start] = '\0';
        do {
            value[--start] = (char)('0' + count % 10u);
            count /= 10u;
        } while (count);
        used = write_map_string_entry_field(out, capacity, used, 16u, keys[i], value + start);
        if (used == (size_t)-1) return 0;
    }
    used = write_map_string_entry_field(out, capacity, used, 16u, "usage_source", "provider_reported");
    return used == (size_t)-1 ? 0 : used;
}

int pb_turn_provider_usage(const turn_provider_c *provider, uint64_t counts[3]) {
    size_t i, j;
    if (!provider || !counts || (provider->seen & 60u) != 60u ||
        (provider->invalid & 60u) || strcmp(provider->usage_source, "provider_reported")) return 0;
    for (i = 0; i < 3u; ++i) {
        const char *s = provider->tokens[i];
        counts[i] = 0;
        if (!s[0] || (s[0] == '0' && s[1])) return 0;
        for (j = 0; j < sizeof(provider->tokens[i]) && s[j]; ++j) {
            if (s[j] < '0' || s[j] > '9' || counts[i] > (UINT64_C(9007199254740991) - (unsigned)(s[j] - '0')) / 10u) return 0;
            counts[i] = counts[i] * 10u + (unsigned)(s[j] - '0');
        }
        if (j == sizeof(provider->tokens[i])) return 0;
    }
    return counts[0] + counts[1] == counts[2];
}

static int decode_provider_entry(turn_provider_c *provider, const uint8_t *entry, size_t len) {
    static const char *keys[] = {"model_id", "model_identity_source", "prompt_tokens", "completion_tokens", "total_tokens", "usage_source"};
    char key[256], value[256];
    size_t i, n, capacity;
    char *target;
    /* Preserve legacy stage-map semantics; validate only provider entries here. */
    pb_reader probe;
    int recognized = 0;
    pb_reader_init(&probe, entry, len);
    while (probe.pos < probe.len) {
        uint32_t field, wire;
        if (pb_read_tag(&probe, &field, &wire) != 0) return -1;
        if (field == 1u && wire == 2u) {
            if (pb_read_string(&probe, key, sizeof(key)) != 0) return -1;
            for (i = 0; i < 6u; ++i) if (!strcmp(key, keys[i])) recognized = 1;
        } else if (pb_skip(&probe, wire) != 0) return -1;
    }
    if (!recognized) return 0;
    if (decode_map_string_entry(entry, len, key, sizeof(key), value, sizeof(value)) != 0) return -1;
    for (i = 0; i < 6u; ++i) if (!strcmp(key, keys[i])) break;
    if (i == 6u) return 0;
    target = i == 0 ? provider->model : i == 1 ? provider->model_source : i == 5 ? provider->usage_source : provider->tokens[i - 2u];
    capacity = i == 0 ? sizeof(provider->model) : sizeof(provider->usage_source);
    n = strlen(value);
    if ((provider->seen & (1u << i)) || !n || n >= capacity) provider->invalid |= 1u << i;
    else memcpy(target, value, n + 1u);
    provider->seen |= 1u << i;
    return 0;
}

int pb_decode_turn_start(const uint8_t *data, size_t len, turn_start_c *out) {
    pb_reader r;
    unsigned input_timing_seen = 0;
    uint64_t metadata_seen = 0u;
    unsigned metadata_count = 0u;
    int initiative_seen = 0;
    int capture_seen = 0;
    if (!out || (!data && len != 0)) return -1;
    out->request_id[0] = '\0';
    out->request_id_len = 0u;
    out->text[0] = '\0';
    out->text_len = 0u;
    out->session_id[0] = '\0';
    out->session_id_len = 0u;
    out->response_subject[0] = '\0';
    out->response_subject_len = 0u;
    out->user_id[0] = '\0';
    out->user_id_len = 0u;
    out->premium = 0;
    out->enable_rag = 0;
    out->enable_tts = 0;
    out->model_request_capture = 0;
    memset(&out->metadata, 0, sizeof(out->metadata));
    out->meta_budget_ms[0] = '\0';
    out->meta_deadline_unix_ms[0] = '\0';
    out->has_meta_budget = 0;
    out->has_meta_deadline = 0;
    out->input_stages = (turn_input_stage_timestamps_c){0};
    out->dnd_initiative.count = 0;
    out->dnd_campaign.operation = 0;
    out->dnd_encounter_action.operation = 0;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 21u) {
            uint64_t value;
            if (capture_seen || wire != 0u || pb_read_varint(&r,&value) != 0 || value > 1u)
                return -1;
            out->model_request_capture = (int)value;
            capture_seen = 1;
            continue;
        }
        if (field >= 18u && field <= 20u) {
            const uint8_t *request;
            size_t length;
            if (initiative_seen || wire != 2u || pb_read_bytes(&r, &request, &length) != 0 ||
                !(field == 18u ? dnd_initiative_request_decode(request, length, &out->dnd_initiative) :
                  field == 19u ? dnd_campaign_request_decode(request, length, &out->dnd_campaign) :
                  dnd_encounter_action_decode(request, length, &out->dnd_encounter_action))) return -1;
            initiative_seen = 1;
            continue;
        }
        /* Canonical TurnStartRequest: 1 req, 2 user, 3 session, 5 text,
         * 6 premium, 7 RAG, 8 TTS, 11 response, 12 metadata. */
        if (wire == 2) {
            if (field == 12) {
                const uint8_t *entry;
                size_t entry_len;
                char key[64];
                char val[256];
                uint64_t seen_bit;
                size_t rule_index;
                char *destination;
                if (pb_read_bytes(&r, &entry, &entry_len) != 0) return -1;
                metadata_count++;
                if (metadata_count > 32u) return -1;
                if (decode_map_string_entry(
                        entry, entry_len, key, sizeof(key), val, sizeof(val)) != 0) return -1;
                if (strcmp(key, "turn_budget_ms") == 0) {
                    seen_bit = UINT64_C(1);
                    if ((metadata_seen & seen_bit) != 0u ||
                        copy_bounded_exact(
                            out->meta_budget_ms, sizeof(out->meta_budget_ms), val) != 0)
                        return -1;
                    metadata_seen |= seen_bit;
                    out->has_meta_budget = 1;
                } else if (strcmp(key, "turn_deadline_unix_ms") == 0) {
                    seen_bit = UINT64_C(1) << 1u;
                    if ((metadata_seen & seen_bit) != 0u ||
                        copy_bounded_exact(
                            out->meta_deadline_unix_ms,
                            sizeof(out->meta_deadline_unix_ms), val) != 0)
                        return -1;
                    metadata_seen |= seen_bit;
                    out->has_meta_deadline = 1;
                } else if (turn_metadata_rule_find(key, &rule_index)) {
                    seen_bit = UINT64_C(1) << (rule_index + 2u);
                    if ((metadata_seen & seen_bit) != 0u) return -1;
                    destination = (char *)&out->metadata + turn_metadata_rules[rule_index].offset;
                    if (copy_bounded_exact(
                            destination, turn_metadata_rules[rule_index].capacity, val) != 0)
                        return -1;
                    metadata_seen |= seen_bit;
                }
                continue;
            }
            if (field == 1) {
                if (pb_read_string_sized(
                        &r,
                        out->request_id,
                        sizeof(out->request_id),
                        &out->request_id_len) != 0) return -1;
            } else if (field == 2) {
                if (pb_read_string_sized(
                        &r,
                        out->user_id,
                        sizeof(out->user_id),
                        &out->user_id_len) != 0) return -1;
            } else if (field == 3) {
                if (pb_read_string_sized(
                        &r,
                        out->session_id,
                        sizeof(out->session_id),
                        &out->session_id_len) != 0) return -1;
            } else if (field == 5) {
                if (pb_read_string_sized(
                        &r,
                        out->text,
                        sizeof(out->text),
                        &out->text_len) != 0) return -1;
            } else if (field == 11) {
                if (pb_read_string_sized(
                        &r,
                        out->response_subject,
                        sizeof(out->response_subject),
                        &out->response_subject_len) != 0)
                    return -1;
            } else if (pb_skip(&r, wire) != 0) {
                return -1;
            }
        } else if (wire == 0 && field >= 6 && field <= 8) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > 1) return -1;
            if (field == 6) out->premium = (int)value;
            else if (field == 7) out->enable_rag = (int)value;
            else out->enable_tts = (int)value;
        } else if (wire == 0 && field >= 13 && field <= 17) {
            uint64_t value;
            unsigned bit = 1u << (field - 13u);
            if ((input_timing_seen & bit) != 0 ||
                pb_read_varint(&r, &value) != 0 ||
                assign_unseen_input_stage(
                    &out->input_stages, field - 13u, value) != 0)
                return -1;
            input_timing_seen |= bit;
        } else {
            if (pb_skip(&r, wire) != 0) return -1;
        }
    }
    if ((input_timing_seen != 0 && input_timing_seen != 31u) ||
        !input_stages_valid(&out->input_stages)) return -1;
    return 0;
}

static size_t encode_turn_start_lengths(
    uint8_t *out,
    size_t out_cap,
    const turn_start_c *in,
    size_t request_id_len,
    size_t user_id_len,
    size_t session_id_len,
    size_t text_len,
    size_t response_subject_len
) {
    size_t pos = 0;
    size_t metadata_count = 0u;
    size_t i;
    if (!out || out_cap == 0 || !in || !input_stages_valid(&in->input_stages) ||
        (in->model_request_capture != 0 && in->model_request_capture != 1))
        return 0;
    pos = write_string_field_n(
        out, out_cap, pos, 1, in->request_id, request_id_len);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field_n(
        out, out_cap, pos, 2, in->user_id, user_id_len);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field_n(
        out, out_cap, pos, 3, in->session_id, session_id_len);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field_n(
        out, out_cap, pos, 5, in->text, text_len);
    if (pos == (size_t)-1) return 0;
    if (in->premium) {
        pos = write_varint_field(out, out_cap, pos, 6, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->enable_rag) {
        pos = write_varint_field(out, out_cap, pos, 7, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->enable_tts) {
        pos = write_varint_field(out, out_cap, pos, 8, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->model_request_capture) {
        pos = write_varint_field(out, out_cap, pos, 21, 1);
        if (pos == (size_t)-1) return 0;
    }
    pos = write_string_field_n(
        out, out_cap, pos, 11,
        in->response_subject, response_subject_len);
    if (pos == (size_t)-1) return 0;
    for (i = 0; i < sizeof(turn_metadata_rules) / sizeof(turn_metadata_rules[0]); ++i) {
        const char *value = (const char *)&in->metadata + turn_metadata_rules[i].offset;
        size_t value_len = bounded_string_len(value, turn_metadata_rules[i].capacity);
        if (value_len == turn_metadata_rules[i].capacity) return 0;
        if (value_len == 0u) continue;
        metadata_count++;
        if (metadata_count > 32u) return 0;
        pos = write_map_string_entry_field(
            out, out_cap, pos, 12, turn_metadata_rules[i].key, value);
        if (pos == (size_t)-1) return 0;
    }
    if (in->has_meta_budget) {
        metadata_count++;
        if (metadata_count > 32u) return 0;
        pos = write_map_string_entry_field(
            out,
            out_cap,
            pos,
            12,
            "turn_budget_ms",
            in->meta_budget_ms);
        if (pos == (size_t)-1) return 0;
    }
    if (in->has_meta_deadline) {
        metadata_count++;
        if (metadata_count > 32u) return 0;
        pos = write_map_string_entry_field(
            out,
            out_cap,
            pos,
            12,
            "turn_deadline_unix_ms",
            in->meta_deadline_unix_ms);
        if (pos == (size_t)-1) return 0;
    }
    pos = write_input_stage_fields(
        out, out_cap, pos, 13, &in->input_stages);
    if (pos == (size_t)-1) return 0;
    if (in->dnd_initiative.count) {
        if (in->dnd_campaign.operation || in->dnd_encounter_action.operation) return 0;
        uint8_t request[DND_INITIATIVE_REQUEST_MAX];
        size_t length = dnd_initiative_request_encode(request, sizeof(request), &in->dnd_initiative);
        if (!length) return 0;
        pos = write_bytes_field(out, out_cap, pos, 18u, request, length);
        if (pos == (size_t)-1) return 0;
    }
    if (in->dnd_campaign.operation) {
        if (in->dnd_encounter_action.operation) return 0;
        uint8_t request[DND_CAMPAIGN_REQUEST_MAX];
        size_t length = dnd_campaign_request_encode(request, sizeof(request), &in->dnd_campaign);
        if (!length) return 0;
        pos = write_bytes_field(out, out_cap, pos, 19u, request, length);
        if (pos == (size_t)-1) return 0;
    }
    if (in->dnd_encounter_action.operation) {
        uint8_t request[DND_ENCOUNTER_ACTION_MAX];
        size_t length = dnd_encounter_action_encode(request, sizeof(request), &in->dnd_encounter_action);
        if (!length) return 0;
        pos = write_bytes_field(out, out_cap, pos, 20u, request, length);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_turn_start(uint8_t *out, size_t out_cap, const turn_start_c *in) {
    if (!in) return 0u;
    return encode_turn_start_lengths(
        out, out_cap, in,
        strlen(in->request_id),
        strlen(in->user_id),
        strlen(in->session_id),
        strlen(in->text),
        strlen(in->response_subject));
}

size_t pb_encode_turn_start_prepared(
    uint8_t *out,
    size_t out_cap,
    const turn_start_c *in
) {
    if (!in || in->request_id_len >= sizeof(in->request_id) ||
        in->user_id_len >= sizeof(in->user_id) ||
        in->session_id_len >= sizeof(in->session_id) ||
        in->text_len >= sizeof(in->text) ||
        in->response_subject_len >= sizeof(in->response_subject) ||
        in->request_id[in->request_id_len] != '\0' ||
        in->user_id[in->user_id_len] != '\0' ||
        in->session_id[in->session_id_len] != '\0' ||
        in->text[in->text_len] != '\0' ||
        in->response_subject[in->response_subject_len] != '\0')
        return 0u;
    return encode_turn_start_lengths(
        out, out_cap, in,
        in->request_id_len,
        in->user_id_len,
        in->session_id_len,
        in->text_len,
        in->response_subject_len);
}

size_t pb_encode_turn_cancel(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *user_id,
    const char *reason
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || !request_id || !request_id[0]) return 0;
    pos = write_string_field(out, out_cap, pos, 1, request_id);
    if (pos == (size_t)-1) return 0;
    if (user_id && user_id[0]) {
        pos = write_string_field(out, out_cap, pos, 2, user_id);
        if (pos == (size_t)-1) return 0;
    }
    if (reason && reason[0]) {
        pos = write_string_field(out, out_cap, pos, 3, reason);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_turn_cancel(const uint8_t *data, size_t len, turn_cancel_c *out) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->request_id, sizeof(out->request_id)) != 0) return -1;
        } else if (field == 2 && wire == 2) {
            if (pb_read_string(&r, out->user_id, sizeof(out->user_id)) != 0) return -1;
        } else if (field == 3 && wire == 2) {
            if (pb_read_string(&r, out->reason, sizeof(out->reason)) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    return out->request_id[0] ? 0 : -1;
}

size_t pb_encode_turn_tts_segment_prepared(
    uint8_t *out,
    size_t out_cap,
    const turn_tts_segment_view_c *in
) {
    size_t pos = 0;
    if (!out || out_cap == 0 || !in || !in->request_id ||
        in->request_id_len == 0u ||
        in->request_id_len >= sizeof(((turn_tts_segment_c *)0)->request_id) ||
        (in->user_id_len != 0u && !in->user_id) ||
        in->user_id_len >= sizeof(((turn_tts_segment_c *)0)->user_id) ||
        (in->text_len != 0u && !in->text) ||
        in->text_len >= sizeof(((turn_tts_segment_c *)0)->text) ||
        (in->voice_id_len != 0u && !in->voice_id) ||
        in->voice_id_len >= sizeof(((turn_tts_segment_c *)0)->voice_id) ||
        (in->response_subject_len != 0u && !in->response_subject) ||
        in->response_subject_len >=
            sizeof(((turn_tts_segment_c *)0)->response_subject) ||
        (in->text_len == 0u &&
         !(in->stream_finality_deferred && in->is_final &&
           in->segment_index > 0)) ||
        in->segment_index < 0 || in->output_sample_rate < 0 || in->output_channels < 0 ||
        in->output_bit_depth < 0 || in->output_encoding < 0 ||
        in->first_text_at_ms < 0 || in->segment_emitted_at_ms < 0 ||
        ((in->first_text_at_ms == 0) != (in->segment_emitted_at_ms == 0)) ||
        (in->first_text_at_ms > 0 &&
         in->segment_emitted_at_ms < in->first_text_at_ms) ||
        !input_stages_valid(&in->input_stages) ||
        (input_stages_any(&in->input_stages) &&
         (in->first_text_at_ms == 0 ||
          in->first_text_at_ms <
            in->input_stages.stt_transcript_published_at_ms))) return 0;
#define WRITE_TTS_STR(field, name) do { \
        pos = write_string_field_n( \
            out, out_cap, pos, (field), in->name, in->name##_len); \
        if (pos == (size_t)-1) return 0; \
    } while (0)
    WRITE_TTS_STR(1, request_id);
    if (in->user_id_len != 0u) WRITE_TTS_STR(2, user_id);
    WRITE_TTS_STR(3, text);
    if (in->voice_id_len != 0u) WRITE_TTS_STR(4, voice_id);
    if (in->response_subject_len != 0u)
        WRITE_TTS_STR(5, response_subject);
#undef WRITE_TTS_STR
    if (in->segment_index > 0) {
        pos = write_varint_field(out, out_cap, pos, 6, (uint32_t)in->segment_index);
        if (pos == (size_t)-1) return 0;
    }
    if (in->is_final) {
        pos = write_varint_field(out, out_cap, pos, 7, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->output_sample_rate > 0) {
        pos = write_varint_field(out, out_cap, pos, 8, (uint32_t)in->output_sample_rate);
        if (pos == (size_t)-1) return 0;
    }
    if (in->output_channels > 0) {
        pos = write_varint_field(out, out_cap, pos, 9, (uint32_t)in->output_channels);
        if (pos == (size_t)-1) return 0;
    }
    if (in->output_bit_depth > 0) {
        pos = write_varint_field(out, out_cap, pos, 10, (uint32_t)in->output_bit_depth);
        if (pos == (size_t)-1) return 0;
    }
    if (in->output_encoding > 0) {
        pos = write_varint_field(out, out_cap, pos, 11, (uint32_t)in->output_encoding);
        if (pos == (size_t)-1) return 0;
    }
    if (in->query_hash != 0) {
        if (pos > out_cap || out_cap - pos < 5) return 0;
        out[pos++] = (uint8_t)((12u << 3) | 5u);
        out[pos++] = (uint8_t)in->query_hash;
        out[pos++] = (uint8_t)(in->query_hash >> 8);
        out[pos++] = (uint8_t)(in->query_hash >> 16);
        out[pos++] = (uint8_t)(in->query_hash >> 24);
    }
    if (in->first_text_at_ms > 0) {
        pos = write_varint_field(
            out, out_cap, pos, 13, (uint64_t)in->first_text_at_ms);
        if (pos == (size_t)-1) return 0;
        pos = write_varint_field(
            out, out_cap, pos, 14, (uint64_t)in->segment_emitted_at_ms);
        if (pos == (size_t)-1) return 0;
    }
    pos = write_input_stage_fields(
        out, out_cap, pos, 15, &in->input_stages);
    if (pos == (size_t)-1) return 0;
    if (in->stream_finality_deferred) {
        pos = write_varint_field(out, out_cap, pos, 20, 1u);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_turn_tts_segment(
    uint8_t *out,
    size_t out_cap,
    const turn_tts_segment_c *in
) {
    turn_tts_segment_view_c prepared;
    if (!in) return 0u;
#define PREPARE_TTS_STRING(field) do { \
        prepared.field = in->field; \
        prepared.field##_len = bounded_string_len(in->field, sizeof(in->field)); \
        if (prepared.field##_len >= sizeof(in->field)) return 0u; \
    } while (0)
    PREPARE_TTS_STRING(request_id);
    PREPARE_TTS_STRING(user_id);
    PREPARE_TTS_STRING(text);
    PREPARE_TTS_STRING(voice_id);
    PREPARE_TTS_STRING(response_subject);
#undef PREPARE_TTS_STRING
    prepared.segment_index = in->segment_index;
    prepared.is_final = in->is_final;
    prepared.output_sample_rate = in->output_sample_rate;
    prepared.output_channels = in->output_channels;
    prepared.output_bit_depth = in->output_bit_depth;
    prepared.output_encoding = in->output_encoding;
    prepared.query_hash = in->query_hash;
    prepared.first_text_at_ms = in->first_text_at_ms;
    prepared.segment_emitted_at_ms = in->segment_emitted_at_ms;
    prepared.stream_finality_deferred = in->stream_finality_deferred;
    prepared.input_stages = in->input_stages;
    return pb_encode_turn_tts_segment_prepared(out, out_cap, &prepared);
}

static PB_NOINLINE int pb_read_tts_segment_tag_slow(
    pb_reader *reader,
    uint32_t *field,
    uint32_t *wire
) {
    return pb_read_tag(reader, field, wire);
}

static PB_ALWAYS_INLINE int pb_read_tts_segment_tag(
    pb_reader *reader,
    uint32_t *field,
    uint32_t *wire
) {
    uint32_t tag;
    assert(reader && field && wire && reader->data && reader->pos < reader->len);
    tag = reader->data[reader->pos];
    if ((tag & 0x80u) == 0u) {
        reader->pos++;
    } else if (reader->len - reader->pos >= 2u &&
               (reader->data[reader->pos + 1u] & 0x80u) == 0u &&
               reader->data[reader->pos + 1u] != 0u) {
        tag = (tag & 0x7fu) |
            ((uint32_t)reader->data[reader->pos + 1u] << 7u);
        reader->pos += 2u;
    } else {
        return pb_read_tts_segment_tag_slow(reader, field, wire);
    }
    if ((tag >> 3u) == 0u) return -1;
    *field = tag >> 3u;
    *wire = tag & 7u;
    return 0;
}

static PB_ALWAYS_INLINE int pb_read_tts_segment_string(
    pb_reader *reader,
    size_t out_cap,
    const uint8_t **out,
    size_t *out_len
) {
    size_t length;
    if (!reader || !out || !out_len || out_cap == 0u || !reader->data ||
        reader->pos >= reader->len) return -1;
    length = reader->data[reader->pos];
    if (length >= 0x80u)
        return pb_read_string_view(reader, out_cap, out, out_len);
    reader->pos++;
    if (length >= out_cap || length > reader->len - reader->pos ||
        memchr(reader->data + reader->pos, '\0', length) != NULL) return -1;
    *out = reader->data + reader->pos;
    *out_len = length;
    reader->pos += length;
    return 0;
}

static PB_ALWAYS_INLINE int pb_read_tts_stage_timestamp(
    pb_reader *reader,
    uint64_t *out
) {
    const uint8_t *bytes;
    uint64_t value;
    if (!reader || !out || !reader->data || reader->pos > reader->len)
        return -1;
    if (reader->len - reader->pos < 6u)
        return pb_read_varint(reader, out);
    bytes = reader->data + reader->pos;
    if ((bytes[0] & bytes[1] & bytes[2] & bytes[3] & bytes[4] & 0x80u) == 0u ||
        (bytes[5] & 0x80u) != 0u || bytes[5] == 0u)
        return pb_read_varint(reader, out);
    value = (uint64_t)(bytes[0] & 0x7fu) |
        ((uint64_t)(bytes[1] & 0x7fu) << 7u) |
        ((uint64_t)(bytes[2] & 0x7fu) << 14u) |
        ((uint64_t)(bytes[3] & 0x7fu) << 21u) |
        ((uint64_t)(bytes[4] & 0x7fu) << 28u) |
        ((uint64_t)bytes[5] << 35u);
    reader->pos += 6u;
    *out = value;
    return 0;
}

static int pb_tts_segment_view_valid(
    const turn_tts_segment_view_c *segment,
    unsigned timing_seen,
    unsigned input_timing_seen
) {
    return segment && segment->request_id_len != 0u &&
        (segment->text_len != 0u ||
         (segment->stream_finality_deferred && segment->is_final &&
          segment->segment_index > 0)) &&
        (timing_seen == 0u || timing_seen == 3u) &&
        (input_timing_seen == 0u || input_timing_seen == 31u) &&
        (input_timing_seen == 0u ||
         (segment->input_stages.audio_committed_at_ms > 0 &&
          segment->input_stages.stt_request_received_at_ms >=
            segment->input_stages.audio_committed_at_ms &&
          segment->input_stages.stt_provider_request_started_at_ms >=
            segment->input_stages.stt_request_received_at_ms &&
          segment->input_stages.stt_provider_ready_at_ms >=
            segment->input_stages.stt_provider_request_started_at_ms &&
          segment->input_stages.stt_transcript_published_at_ms >=
            segment->input_stages.stt_provider_ready_at_ms)) &&
        (timing_seen == 0u ||
         segment->segment_emitted_at_ms >= segment->first_text_at_ms) &&
        (input_timing_seen == 0u ||
         (timing_seen == 3u &&
          segment->first_text_at_ms >=
            segment->input_stages.stt_transcript_published_at_ms));
}

static PB_ALWAYS_INLINE int pb_tts_consume_tag(
    pb_reader *reader,
    uint32_t field,
    uint32_t wire
) {
    uint32_t tag = (field << 3u) | wire;
    if (!reader || !reader->data || reader->pos > reader->len) return 0;
    if (tag < 0x80u) {
        if (reader->pos == reader->len ||
            reader->data[reader->pos] != (uint8_t)tag) return 0;
        reader->pos++;
        return 1;
    }
    if (reader->len - reader->pos < 2u ||
        reader->data[reader->pos] != (uint8_t)((tag & 0x7fu) | 0x80u) ||
        reader->data[reader->pos + 1u] != (uint8_t)(tag >> 7u)) return 0;
    reader->pos += 2u;
    return 1;
}

static PB_ALWAYS_INLINE int pb_tts_read_canonical_string(
    pb_reader *reader,
    uint32_t field,
    size_t out_cap,
    const char **out,
    size_t *out_len
) {
    const uint8_t *bytes;
    if (!pb_tts_consume_tag(reader, field, 2u) ||
        pb_read_tts_segment_string(
            reader, out_cap, &bytes, out_len) != 0) return -1;
    *out = (const char *)bytes;
    return 0;
}

static PB_ALWAYS_INLINE int pb_tts_read_optional_positive_varint(
    pb_reader *reader,
    uint32_t field,
    uint64_t maximum,
    uint64_t *out
) {
    uint64_t value;
    if (!pb_tts_consume_tag(reader, field, 0u)) return 0;
    if (reader->pos >= reader->len) return -1;
    value = reader->data[reader->pos];
    if ((value & 0x80u) == 0u) {
        reader->pos++;
    } else if (reader->len - reader->pos >= 3u &&
               reader->data[reader->pos] == 0xc0u &&
               reader->data[reader->pos + 1u] == 0xbbu &&
               reader->data[reader->pos + 2u] == 0x01u) {
        value = 24000u;
        reader->pos += 3u;
    } else if (pb_read_varint(reader, &value) != 0) {
        return -1;
    }
    if (value == 0u || value > maximum) return -1;
    *out = value;
    return 1;
}

static PB_ALWAYS_INLINE int pb_tts_read_required_timestamp(
    pb_reader *reader,
    uint32_t field,
    uint64_t *out
) {
    if (!pb_tts_consume_tag(reader, field, 0u) ||
        pb_read_tts_stage_timestamp(reader, out) != 0 ||
        *out == 0u || *out > INT64_MAX) return -1;
    return 0;
}

static PB_ALWAYS_INLINE int pb_tts_request_id_char_allowed(uint8_t c) {
    return (c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
        (c >= (uint8_t)'A' && c <= (uint8_t)'Z') ||
        (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
        c == (uint8_t)'-' || c == (uint8_t)'_' ||
        c == (uint8_t)'.' || c == (uint8_t)':';
}

static int pb_tts_request_id_hash(
    const char *request_id,
    size_t request_id_len,
    uint32_t *hash_out
) {
    uint32_t hash = 2166136261u;
    size_t i;
    if (!request_id || !hash_out || request_id_len == 0u ||
        request_id_len >= sizeof(((turn_tts_segment_c *)0)->request_id))
        return -1;
    for (i = 0u; i < request_id_len; ++i) {
        uint8_t c = (uint8_t)request_id[i];
        if (!pb_tts_request_id_char_allowed(c)) return -1;
        hash ^= c;
        hash *= 16777619u;
    }
    *hash_out = hash;
    return 0;
}

static PB_ALWAYS_INLINE int pb_tts_read_canonical_request_id(
    pb_reader *reader,
    const char **out,
    size_t *out_len
) {
    const uint8_t *bytes;
    size_t length;
    if (!reader || !out || !out_len ||
        !pb_tts_consume_tag(reader, 1u, 2u) ||
        reader->pos >= reader->len) return -1;
    length = reader->data[reader->pos++];
    if (length == 0u ||
        length >= sizeof(((turn_tts_segment_c *)0)->request_id) ||
        length > reader->len - reader->pos) return -1;
    bytes = reader->data + reader->pos;
    reader->pos += length;
    *out = (const char *)bytes;
    *out_len = length;
    return 0;
}

static PB_ALWAYS_INLINE int pb_decode_turn_tts_segment_view_canonical(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_view_c *out,
    uint32_t *request_hash
) {
    turn_tts_segment_view_c decoded;
    pb_reader reader;
    uint64_t value;
    uint32_t hash;
    int present;
    memset(&decoded, 0, sizeof(decoded));
    pb_reader_init(&reader, data, len);
    if (pb_tts_read_canonical_request_id(
            &reader, &decoded.request_id, &decoded.request_id_len) != 0 ||
        pb_tts_request_id_hash(
            decoded.request_id, decoded.request_id_len, &hash) != 0)
        return -1;
    if (reader.pos < reader.len && reader.data[reader.pos] == 0x12u) {
        if (pb_tts_read_canonical_string(
                &reader, 2u,
                sizeof(((turn_tts_segment_c *)0)->user_id),
                &decoded.user_id, &decoded.user_id_len) != 0 ||
            decoded.user_id_len == 0u) return -1;
    }
    if (pb_tts_read_canonical_string(
            &reader, 3u,
            sizeof(((turn_tts_segment_c *)0)->text),
            &decoded.text, &decoded.text_len) != 0) return -1;
    if (reader.pos < reader.len && reader.data[reader.pos] == 0x22u) {
        if (pb_tts_read_canonical_string(
                &reader, 4u,
                sizeof(((turn_tts_segment_c *)0)->voice_id),
                &decoded.voice_id, &decoded.voice_id_len) != 0 ||
            decoded.voice_id_len == 0u) return -1;
    }
    if (reader.pos < reader.len && reader.data[reader.pos] == 0x2au) {
        if (pb_tts_read_canonical_string(
                &reader, 5u,
                sizeof(((turn_tts_segment_c *)0)->response_subject),
                &decoded.response_subject,
                &decoded.response_subject_len) != 0 ||
            decoded.response_subject_len == 0u) return -1;
    }
    present = pb_tts_read_optional_positive_varint(
        &reader, 6u, INT32_MAX, &value);
    if (present < 0) return -1;
    if (present) decoded.segment_index = (int32_t)value;
    present = pb_tts_read_optional_positive_varint(&reader, 7u, 1u, &value);
    if (present < 0) return -1;
    if (present) decoded.is_final = 1;
    present = pb_tts_read_optional_positive_varint(
        &reader, 8u, INT32_MAX, &value);
    if (present < 0) return -1;
    if (present) decoded.output_sample_rate = (int32_t)value;
    present = pb_tts_read_optional_positive_varint(
        &reader, 9u, INT32_MAX, &value);
    if (present < 0) return -1;
    if (present) decoded.output_channels = (int32_t)value;
    present = pb_tts_read_optional_positive_varint(
        &reader, 10u, INT32_MAX, &value);
    if (present < 0) return -1;
    if (present) decoded.output_bit_depth = (int32_t)value;
    present = pb_tts_read_optional_positive_varint(
        &reader, 11u, INT32_MAX, &value);
    if (present < 0) return -1;
    if (present) decoded.output_encoding = (int32_t)value;
    if (pb_tts_consume_tag(&reader, 12u, 5u)) {
        if (reader.len - reader.pos < 4u) return -1;
        decoded.query_hash = (uint32_t)reader.data[reader.pos] |
            ((uint32_t)reader.data[reader.pos + 1u] << 8u) |
            ((uint32_t)reader.data[reader.pos + 2u] << 16u) |
            ((uint32_t)reader.data[reader.pos + 3u] << 24u);
        if (decoded.query_hash == 0u) return -1;
        reader.pos += 4u;
    }
    if (pb_tts_consume_tag(&reader, 13u, 0u)) {
        if (pb_read_tts_stage_timestamp(&reader, &value) != 0 ||
            value == 0u || value > INT64_MAX) return -1;
        decoded.first_text_at_ms = (int64_t)value;
        if (pb_tts_read_required_timestamp(&reader, 14u, &value) != 0 ||
            value < (uint64_t)decoded.first_text_at_ms)
            return -1;
        decoded.segment_emitted_at_ms = (int64_t)value;
    }
    if (pb_tts_consume_tag(&reader, 15u, 0u)) {
        uint64_t previous;
        if (pb_read_tts_stage_timestamp(&reader, &value) != 0 ||
            value == 0u || value > INT64_MAX) return -1;
        decoded.input_stages.audio_committed_at_ms = (int64_t)value;
        previous = value;
        if (pb_tts_read_required_timestamp(&reader, 16u, &value) != 0 ||
            value < previous)
            return -1;
        decoded.input_stages.stt_request_received_at_ms = (int64_t)value;
        previous = value;
        if (pb_tts_read_required_timestamp(&reader, 17u, &value) != 0 ||
            value < previous)
            return -1;
        decoded.input_stages.stt_provider_request_started_at_ms =
            (int64_t)value;
        previous = value;
        if (pb_tts_read_required_timestamp(&reader, 18u, &value) != 0 ||
            value < previous)
            return -1;
        decoded.input_stages.stt_provider_ready_at_ms = (int64_t)value;
        previous = value;
        if (pb_tts_read_required_timestamp(&reader, 19u, &value) != 0 ||
            value < previous ||
            decoded.first_text_at_ms == 0 ||
            value > (uint64_t)decoded.first_text_at_ms)
            return -1;
        decoded.input_stages.stt_transcript_published_at_ms =
            (int64_t)value;
    }
    present = pb_tts_read_optional_positive_varint(&reader, 20u, 1u, &value);
    if (present < 0) return -1;
    if (present) decoded.stream_finality_deferred = 1;
    if (reader.pos != reader.len ||
        (decoded.text_len == 0u &&
         !(decoded.stream_finality_deferred && decoded.is_final &&
           decoded.segment_index > 0)))
        return -1;
    *out = decoded;
    *request_hash = hash;
    return 0;
}

int pb_decode_turn_tts_segment_view(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_view_c *out
) {
    static const size_t string_capacities[] = {
        sizeof(((turn_tts_segment_c *)0)->request_id),
        sizeof(((turn_tts_segment_c *)0)->user_id),
        sizeof(((turn_tts_segment_c *)0)->text),
        sizeof(((turn_tts_segment_c *)0)->voice_id),
        sizeof(((turn_tts_segment_c *)0)->response_subject),
    };
    pb_reader r;
    unsigned timing_seen = 0;
    unsigned input_timing_seen = 0;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tts_segment_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && field >= 1 && field <= 5) {
            size_t string_index = (size_t)(field - 1u);
            const uint8_t *bytes;
            size_t decoded_len;
            if (pb_read_tts_segment_string(
                    &r,
                    string_capacities[string_index],
                    &bytes,
                    &decoded_len) != 0)
                return -1;
            switch (field) {
            case 1:
                out->request_id = (const char *)bytes;
                out->request_id_len = decoded_len;
                break;
            case 2:
                out->user_id = (const char *)bytes;
                out->user_id_len = decoded_len;
                break;
            case 3:
                out->text = (const char *)bytes;
                out->text_len = decoded_len;
                break;
            case 4:
                out->voice_id = (const char *)bytes;
                out->voice_id_len = decoded_len;
                break;
            default:
                out->response_subject = (const char *)bytes;
                out->response_subject_len = decoded_len;
                break;
            }
        } else if (wire == 0 && field >= 6 && field <= 11) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > INT32_MAX) return -1;
            if (field == 7) {
                if (v > 1) return -1;
                out->is_final = (int)v;
            } else {
                int32_t decoded = (int32_t)v;
                switch (field) {
                case 6: out->segment_index = decoded; break;
                case 8: out->output_sample_rate = decoded; break;
                case 9: out->output_channels = decoded; break;
                case 10: out->output_bit_depth = decoded; break;
                default: out->output_encoding = decoded; break;
                }
            }
        } else if (field == 12 && wire == 5) {
            if (r.pos > r.len || r.len - r.pos < 4) return -1;
            out->query_hash = (uint32_t)r.data[r.pos] |
                ((uint32_t)r.data[r.pos + 1] << 8) |
                ((uint32_t)r.data[r.pos + 2] << 16) |
                ((uint32_t)r.data[r.pos + 3] << 24);
            r.pos += 4;
        } else if (field >= 13 && field <= 19 && wire == 0) {
            uint64_t v;
            unsigned bit;
            if (field <= 14u) {
                bit = 1u << (field - 13u);
                if ((timing_seen & bit) != 0) return -1;
            } else {
                bit = 1u << (field - 15u);
                if ((input_timing_seen & bit) != 0) return -1;
            }
            if (pb_read_tts_stage_timestamp(&r, &v) != 0) return -1;
            if (field <= 14u) {
                if (v == 0u || v > INT64_MAX) return -1;
                timing_seen |= bit;
                if (field == 13u) out->first_text_at_ms = (int64_t)v;
                else out->segment_emitted_at_ms = (int64_t)v;
            } else {
                if (assign_unseen_input_stage(
                        &out->input_stages, field - 15u, v) != 0)
                    return -1;
                input_timing_seen |= bit;
            }
        } else if (field == 20 && wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > 1u) return -1;
            out->stream_finality_deferred = (int)v;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    return pb_tts_segment_view_valid(out, timing_seen, input_timing_seen) ?
        0 : -1;
}

int pb_decode_turn_tts_segment_view_admitted(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_view_c *out,
    uint32_t *request_hash
) {
    uint32_t hash;
    if (!out || !request_hash || (!data && len != 0u)) return -1;
    if (pb_decode_turn_tts_segment_view_canonical(
            data, len, out, &hash) == 0) {
        *request_hash = hash;
        return 0;
    }
    if (pb_decode_turn_tts_segment_view(data, len, out) != 0 ||
        pb_tts_request_id_hash(
            out->request_id, out->request_id_len, &hash) != 0) return -1;
    *request_hash = hash;
    return 0;
}

int pb_decode_turn_tts_segment(
    const uint8_t *data,
    size_t len,
    turn_tts_segment_c *out
) {
    turn_tts_segment_view_c view;
    if (!out || (!data && len != 0)) return -1;
    out->request_id[0] = '\0';
    out->request_id_len = 0u;
    out->user_id[0] = '\0';
    out->user_id_len = 0u;
    out->text[0] = '\0';
    out->text_len = 0u;
    out->voice_id[0] = '\0';
    out->voice_id_len = 0u;
    out->response_subject[0] = '\0';
    out->response_subject_len = 0u;
    out->segment_index = 0;
    out->is_final = 0;
    out->output_sample_rate = 0;
    out->output_channels = 0;
    out->output_bit_depth = 0;
    out->output_encoding = 0;
    out->query_hash = 0;
    out->first_text_at_ms = 0;
    out->segment_emitted_at_ms = 0;
    out->stream_finality_deferred = 0;
    memset(&out->input_stages, 0, sizeof(out->input_stages));
    if (pb_decode_turn_tts_segment_view(data, len, &view) != 0) return -1;
#define COPY_TTS_VIEW_STRING(field) do { \
        if (view.field##_len != 0u) \
            memcpy(out->field, view.field, view.field##_len); \
        out->field[view.field##_len] = '\0'; \
        out->field##_len = view.field##_len; \
    } while (0)
    COPY_TTS_VIEW_STRING(request_id);
    COPY_TTS_VIEW_STRING(user_id);
    COPY_TTS_VIEW_STRING(text);
    COPY_TTS_VIEW_STRING(voice_id);
    COPY_TTS_VIEW_STRING(response_subject);
#undef COPY_TTS_VIEW_STRING
    out->segment_index = view.segment_index;
    out->is_final = view.is_final;
    out->output_sample_rate = view.output_sample_rate;
    out->output_channels = view.output_channels;
    out->output_bit_depth = view.output_bit_depth;
    out->output_encoding = view.output_encoding;
    out->query_hash = view.query_hash;
    out->first_text_at_ms = view.first_text_at_ms;
    out->segment_emitted_at_ms = view.segment_emitted_at_ms;
    out->stream_finality_deferred = view.stream_finality_deferred;
    out->input_stages = view.input_stages;
    return 0;
}

typedef struct {
    uint32_t field;
    size_t offset;
    size_t capacity;
    int required;
} rag_string_field;

#define RAG_STRING(type, member, number, required_value) \
    {number, offsetof(type, member), sizeof(((type *)0)->member), required_value}
static const rag_string_field rag_request_strings[] = {
    RAG_STRING(rag_search_request_c, request_id, 1u, 1),
    RAG_STRING(rag_search_request_c, user_id, 2u, 0),
    RAG_STRING(rag_search_request_c, query, 3u, 1),
    RAG_STRING(rag_search_request_c, collection, 4u, 0),
    RAG_STRING(rag_search_request_c, session_id, 8u, 0),
    RAG_STRING(rag_search_request_c, knowledge_scope, 9u, 0),
    RAG_STRING(rag_search_request_c, campaign_id, 10u, 0),
    RAG_STRING(rag_search_request_c, character_id, 11u, 0)
};
static const rag_string_field rag_citation_strings[] = {
    RAG_STRING(dnd_rag_citation, source, 1u, 1),
    RAG_STRING(dnd_rag_citation, book_slug, 2u, 0),
    RAG_STRING(dnd_rag_citation, collection, 3u, 1),
    RAG_STRING(dnd_rag_citation, corpus_version, 4u, 1),
    RAG_STRING(dnd_rag_citation, embedding_model, 5u, 1),
    RAG_STRING(dnd_rag_citation, record_id, 6u, 0),
    RAG_STRING(dnd_rag_citation, document_id, 7u, 1),
    RAG_STRING(dnd_rag_citation, content_hash, 8u, 1),
    RAG_STRING(dnd_rag_citation, source_sha256, 9u, 1),
    RAG_STRING(dnd_rag_citation, section, 10u, 0),
    RAG_STRING(dnd_rag_citation, passage_id, 17u, 0)
};
#undef RAG_STRING

static size_t rag_write_strings(
    uint8_t *out, size_t cap, const void *object,
    const rag_string_field *fields, size_t count
) {
    size_t i, pos = 0;
    for (i = 0; i < count; ++i) {
        const char *value = (const char *)object + fields[i].offset;
        size_t length = bounded_string_len(value, fields[i].capacity);
        if (length >= fields[i].capacity || (fields[i].required && !length)) return (size_t)-1;
        if (!length) continue;
        pos = write_string_field_n(out, cap, pos, fields[i].field, value, length);
        if (pos == (size_t)-1) return pos;
    }
    return pos;
}

static int rag_read_string(
    pb_reader *r, uint32_t field, void *object,
    const rag_string_field *fields, size_t count
) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (fields[i].field == field)
            return pb_read_string(r, (char *)object + fields[i].offset, fields[i].capacity);
    return -1;
}

static size_t write_double_field(
    uint8_t *out, size_t cap, size_t pos, uint32_t field, double value
) {
    uint64_t bits;
    size_t i;
    _Static_assert(sizeof(double) == 8u && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
        "protobuf double requires IEEE binary64");
    if (!isfinite(value)) return (size_t)-1;
    pos = write_tag(out, cap, pos, field, 1u);
    if (pos == (size_t)-1 || pos > cap || cap - pos < 8u) return (size_t)-1;
    memcpy(&bits, &value, sizeof(bits));
    for (i = 0; i < 8u; ++i) out[pos++] = (uint8_t)(bits >> (8u * i));
    return pos;
}

static int read_double(pb_reader *r, double *out) {
    uint64_t bits = 0;
    size_t i;
    if (r->pos > r->len || r->len - r->pos < 8u) return -1;
    for (i = 0; i < 8u; ++i) bits |= (uint64_t)r->data[r->pos++] << (8u * i);
    memcpy(out, &bits, sizeof(bits));
    return isfinite(*out) ? 0 : -1;
}

size_t pb_encode_rag_search_request(
    uint8_t *out,
    size_t out_cap,
    const rag_search_request_c *in
) {
    size_t pos;
    if (!out || out_cap == 0 || !in || !in->request_id[0] || !in->query[0] ||
        in->top_k < 0 || in->rerank_top_k < 0 || in->deadline_unix_ms < 0 ||
        (in->premium != 0 && in->premium != 1) ||
        (in->enable_rerank != 0 && in->enable_rerank != 1)) return 0;
    pos = rag_write_strings(out, out_cap, in, rag_request_strings,
        sizeof(rag_request_strings) / sizeof(rag_request_strings[0]));
    if (pos == (size_t)-1) return 0;
    if (in->top_k > 0) {
        pos = write_varint_field(out, out_cap, pos, 5, (uint32_t)in->top_k);
        if (pos == (size_t)-1) return 0;
    }
    if (in->rerank_top_k > 0) {
        pos = write_varint_field(out, out_cap, pos, 6, (uint32_t)in->rerank_top_k);
        if (pos == (size_t)-1) return 0;
    }
    if (in->enable_rerank) {
        pos = write_varint_field(out, out_cap, pos, 7, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->premium) {
        pos = write_varint_field(out, out_cap, pos, 12, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->deadline_unix_ms) {
        pos = write_varint_field(out, out_cap, pos, 13, (uint64_t)in->deadline_unix_ms);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_rag_search_request(
    const uint8_t *data,
    size_t len,
    rag_search_request_c *out
) {
    pb_reader r;
    uint32_t seen = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if ((!data && len != 0) || len > 8192u) return -1;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        int scalar;
        if (pb_read_tag(&r, &field, &wire) != 0) goto reject;
        scalar = field == 5u || field == 6u || field == 7u || field == 12u || field == 13u;
        if (field <= 13u) {
            if ((seen & (1u << field)) || wire != (scalar ? 0u : 2u)) goto reject;
            seen |= 1u << field;
        }
        if (field <= 13u && !scalar) {
            if (rag_read_string(&r, field, out, rag_request_strings,
                    sizeof(rag_request_strings) / sizeof(rag_request_strings[0])) != 0) goto reject;
        } else if (scalar) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > INT64_MAX ||
                (field != 13u && v > INT32_MAX)) goto reject;
            if (field == 5) out->top_k = (int32_t)v;
            else if (field == 6) out->rerank_top_k = (int32_t)v;
            else if (field == 13) out->deadline_unix_ms = (int64_t)v;
            else {
                if (v > 1) goto reject;
                if (field == 7) out->enable_rerank = (int)v;
                else out->premium = (int)v;
            }
        } else if (pb_skip(&r, wire) != 0) {
            goto reject;
        }
    }
    if (out->request_id[0] && out->query[0]) return 0;
reject:
    memset(out, 0, sizeof(*out));
    return -1;
}

static int rag_hash_valid(const char *value) {
    size_t i;
    if (bounded_string_len(value, 65u) != 64u) return 0;
    for (i = 0; i < 64u; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}

static int rag_citation_valid(const dnd_rag_citation *value) {
    size_t i;
    for (i = 0; i < sizeof(rag_citation_strings) / sizeof(rag_citation_strings[0]); ++i) {
        const rag_string_field *field = &rag_citation_strings[i];
        size_t length = bounded_string_len((const char *)value + field->offset, field->capacity);
        if (length >= field->capacity || (field->required && !length)) return 0;
    }
    return dnd_rag_citation_shape_valid(value) && rag_hash_valid(value->content_hash) &&
        rag_hash_valid(value->source_sha256) &&
        strncmp(value->corpus_version, "sha256:", 7u) == 0 &&
        rag_hash_valid(value->corpus_version + 7u) &&
        value->page_start >= 0 && value->page_end >= value->page_start && value->chunk_index >= 0 &&
        dnd_rag_score_valid(value->score_metric, value->score) &&
        dnd_rag_excerpt_valid(&value->excerpt, DND_RAG_CONTENT_CAP - 1u);
}

static size_t encode_retrieval_witness(uint8_t out[256], const dnd_rag_passage_witness *w) {
    size_t pos = write_string_field(out, 256, 0, 1u, w->record_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, 256, pos, 2u, w->content_hash);
    const uint32_t values[] = {w->page, w->chunk, w->begin, w->end, w->record_length};
    for (size_t i = 0; i < 5; ++i) {
        if (pos == (size_t)-1) return 0;
        pos = write_varint_field(out, 256, pos, 3u + (uint32_t)i, values[i]);
    }
    return pos == (size_t)-1 ? 0 : pos;
}
static int decode_retrieval_witness(pb_reader *parent, dnd_rag_passage_witness *out) {
    const uint8_t *bytes; size_t length; pb_reader r; unsigned seen = 0;
    if (pb_read_bytes(parent, &bytes, &length) || length > 256u) return -1;
    pb_reader_init(&r, bytes, length);
    while (r.pos < r.len) {
        uint32_t field, wire; uint64_t value;
        if (pb_read_tag(&r, &field, &wire) || !field || field > 7u ||
            (seen & (1u << field)) || wire != (field <= 2u ? 2u : 0u)) return -1;
        seen |= 1u << field;
        if (field <= 2u) {
            if (pb_read_string(&r, field == 1u ? out->record_id : out->content_hash, 65u)) return -1;
        } else {
            if (pb_read_varint(&r, &value) || value > INT32_MAX) return -1;
            switch (field) {
            case 3u: out->page = (uint32_t)value; break;
            case 4u: out->chunk = (uint32_t)value; break;
            case 5u: out->begin = (uint32_t)value; break;
            case 6u: out->end = (uint32_t)value; break;
            case 7u: out->record_length = (uint32_t)value; break;
            default: return -1;
            }
        }
    }
    return 0; /* Enclosing citation validation checks required and correlated values. */
}

size_t pb_encode_retrieval_citation(
    uint8_t *out, size_t out_cap, const dnd_rag_citation *in
) {
    size_t pos;
    if (!out || !out_cap || !in || !rag_citation_valid(in)) return 0;
    pos = rag_write_strings(out, out_cap, in, rag_citation_strings,
        sizeof(rag_citation_strings) / sizeof(rag_citation_strings[0]));
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 11u, (uint32_t)in->page_start);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 12u, (uint32_t)in->page_end);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 13u, (uint32_t)in->chunk_index);
    if (pos == (size_t)-1) return 0;
    pos = write_double_field(out, out_cap, pos, 14u, in->score);
    if (pos == (size_t)-1) return 0;
    /* Keep existing cosine bytes compatible with older public-view decoders. */
    if (in->score_metric != DND_RAG_SCORE_COSINE)
        pos = write_varint_field(out, out_cap, pos, 15u, (uint32_t)in->score_metric);
    if (pos == (size_t)-1) return 0;
    for (size_t i = 0; i < in->excerpt.count; ++i) {
        uint8_t span[16];
        size_t n = write_varint_field(span, sizeof(span), 0, 1u, in->excerpt.spans[i].begin);
        if (n == (size_t)-1) return 0;
        n = write_varint_field(span, sizeof(span), n, 2u, in->excerpt.spans[i].end);
        if (n == (size_t)-1) return 0;
        pos = write_bytes_field(out, out_cap, pos, 16u, span, n);
        if (pos == (size_t)-1) return 0;
    }
    for (size_t i = 0; i < in->witness_count; ++i) {
        uint8_t witness[256];
        size_t n = encode_retrieval_witness(witness, &in->witnesses[i]);
        if (!n) return 0;
        pos = write_bytes_field(out, out_cap, pos, 18u, witness, n);
        if (pos == (size_t)-1) return 0;
    }
    return pos == (size_t)-1 ? 0 : pos;
}

static int decode_retrieval_span(pb_reader *parent, dnd_rag_byte_span *out) {
    const uint8_t *bytes;
    size_t length;
    pb_reader r;
    unsigned seen = 0;
    if (pb_read_bytes(parent, &bytes, &length) || length > 16u) return -1;
    pb_reader_init(&r, bytes, length);
    while (r.pos < r.len) {
        uint32_t field, wire;
        uint64_t value;
        if (pb_read_tag(&r, &field, &wire) || (field != 1u && field != 2u) || wire ||
            (seen & (1u << field)) || pb_read_varint(&r, &value) || value >= DND_RAG_CONTENT_CAP) return -1;
        seen |= 1u << field;
        if (field == 1u) out->begin = (uint32_t)value;
        else out->end = (uint32_t)value;
    }
    return (seen & (1u << 2u)) && out->begin < out->end ? 0 : -1;
}

int pb_decode_retrieval_citation(
    const uint8_t *data, size_t len, dnd_rag_citation *out
) {
    pb_reader r;
    uint32_t seen = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if ((!data && len) || len > 8192u) return -1;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) goto reject;
        if (field <= 15u || field == 17u) {
            uint32_t expected = (field <= 10u || field == 17u) ? 2u : (field == 14u ? 1u : 0u);
            if ((seen & (1u << field)) || wire != expected) goto reject;
            seen |= 1u << field;
        }
        if (field <= 10u || field == 17u) {
            if (rag_read_string(&r, field, out, rag_citation_strings,
                    sizeof(rag_citation_strings) / sizeof(rag_citation_strings[0])) != 0) goto reject;
        } else if (field <= 13u) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > INT32_MAX) goto reject;
            if (field == 11u) out->page_start = (int32_t)value;
            else if (field == 12u) out->page_end = (int32_t)value;
            else out->chunk_index = (int32_t)value;
        } else if (field == 14u) {
            if (read_double(&r, &out->score) != 0) goto reject;
        } else if (field == 15u) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > DND_RAG_SCORE_NONE) goto reject;
            out->score_metric = (dnd_rag_score_metric)value;
        } else if (field == 16u) {
            if (wire != 2u || out->excerpt.count == DND_RAG_EXCERPT_SPANS_MAX ||
                decode_retrieval_span(&r, &out->excerpt.spans[out->excerpt.count])) goto reject;
            ++out->excerpt.count;
        } else if (field == 18u) {
            if (wire != 2u || out->witness_count == DND_RAG_PASSAGE_WITNESSES_MAX ||
                decode_retrieval_witness(&r, &out->witnesses[out->witness_count])) goto reject;
            ++out->witness_count;
        } else if (pb_skip(&r, wire) != 0) goto reject;
    }
    if (rag_citation_valid(out)) return 0;
reject:
    memset(out, 0, sizeof(*out));
    return -1;
}

static size_t encode_rag_document(
    uint8_t *out, size_t cap, const dnd_rag_hit *hit, size_t index
) {
    uint8_t citation[8192];
    size_t length = bounded_string_len(hit->content, sizeof(hit->content));
    size_t citation_len, pos;
    if (!length || length >= sizeof(hit->content) ||
        !dnd_rag_excerpt_valid(&hit->citation.excerpt, length)) return 0;
    citation_len = pb_encode_retrieval_citation(citation, sizeof(citation), &hit->citation);
    if (!citation_len) return 0;
    pos = write_string_field_n(out, cap, 0, 1u, hit->content, length);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, cap, pos, 2u, hit->citation.source);
    if (pos == (size_t)-1) return 0;
    pos = write_double_field(out, cap, pos, 3u, hit->citation.score);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, cap, pos, 4u, index);
    if (pos == (size_t)-1) return 0;
    pos = write_bytes_field(out, cap, pos, 6u, citation, citation_len);
    return pos == (size_t)-1 ? 0 : pos;
}

static int decode_rag_document(
    const uint8_t *data, size_t len, dnd_rag_hit *out, size_t expected_index
) {
    pb_reader r;
    char source[2048] = {0};
    double score = 0;
    uint64_t index = 0;
    uint32_t seen = 0;
    size_t metadata_count = 0;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field <= 6u) {
            uint32_t expected = field == 3u ? 1u : (field == 4u ? 0u : 2u);
            if (wire != expected || (field != 5u && (seen & (1u << field)))) return -1;
            seen |= 1u << field;
        }
        if (field == 1u) {
            if (pb_read_string(&r, out->content, sizeof(out->content)) != 0) return -1;
        } else if (field == 2u) {
            if (pb_read_string(&r, source, sizeof(source)) != 0) return -1;
        } else if (field == 3u) {
            if (read_double(&r, &score) != 0) return -1;
        } else if (field == 4u) {
            if (pb_read_varint(&r, &index) != 0 || index != expected_index) return -1;
        } else if (field == 6u) {
            const uint8_t *bytes;
            size_t bytes_len;
            if (pb_read_bytes(&r, &bytes, &bytes_len) != 0 ||
                pb_decode_retrieval_citation(bytes, bytes_len, &out->citation) != 0) return -1;
        } else {
            /* Legacy map entries carry no authority beside typed provenance. */
            if (field == 5u && ++metadata_count > 32u) return -1;
            if (pb_skip(&r, wire) != 0) return -1;
        }
    }
    return out->content[0] && (seen & (1u << 6u)) &&
        strcmp(source, out->citation.source) == 0 && score == out->citation.score &&
        dnd_rag_excerpt_valid(&out->citation.excerpt, strlen(out->content)) &&
        index == expected_index ? 0 : -1;
}

size_t pb_encode_rag_search_response(
    uint8_t *out,
    size_t out_cap,
    const rag_search_response_c *in
) {
    size_t pos = 0;
    size_t i;
    if (!out || out_cap == 0 || !in || !in->request_id[0] ||
        bounded_string_len(in->request_id, sizeof(in->request_id)) >= sizeof(in->request_id) ||
        bounded_string_len(in->context_text, sizeof(in->context_text)) >= sizeof(in->context_text) ||
        bounded_string_len(in->error, sizeof(in->error)) >= sizeof(in->error) ||
        (in->used_rag != 0 && in->used_rag != 1) ||
        in->documents.count > DND_RAG_HITS_MAX ||
        (in->documents.count && (!in->used_rag || in->error[0]))) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->request_id);
    if (pos == (size_t)-1) return 0;
    for (i = 0; i < in->documents.count; ++i) {
        uint8_t document[16384];
        size_t j, length = encode_rag_document(document, sizeof(document), &in->documents.hits[i], i);
        if (!length) return 0;
        for (j = 0; j < i; ++j)
            if (strcmp(dnd_rag_citation_id(&in->documents.hits[j].citation),
                       dnd_rag_citation_id(&in->documents.hits[i].citation)) == 0) return 0;
        pos = write_bytes_field(out, out_cap, pos, 2u, document, length);
        if (pos == (size_t)-1) return 0;
    }
    if (in->context_text[0]) {
        pos = write_string_field(out, out_cap, pos, 3, in->context_text);
        if (pos == (size_t)-1) return 0;
    }
    if (in->used_rag) {
        pos = write_varint_field(out, out_cap, pos, 4, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (in->error[0]) {
        pos = write_string_field(out, out_cap, pos, 5, in->error);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_rag_search_response(
    const uint8_t *data,
    size_t len,
    rag_search_response_c *out
) {
    pb_reader r;
    uint32_t seen = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if ((!data && len != 0) || len > DND_RAG_WIRE_CAP) return -1;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) goto reject;
        if (field <= 5u) {
            if (wire != (field == 4u ? 0u : 2u) ||
                (field != 2u && (seen & (1u << field)))) goto reject;
            seen |= 1u << field;
        }
        if (wire == 2 && (field == 1 || field == 3 || field == 5)) {
            char *dst;
            size_t cap;
            if (field == 1) { dst = out->request_id; cap = sizeof(out->request_id); }
            else if (field == 3) { dst = out->context_text; cap = sizeof(out->context_text); }
            else { dst = out->error; cap = sizeof(out->error); }
            if (pb_read_string(&r, dst, cap) != 0) goto reject;
        } else if (field == 2u) {
            const uint8_t *document;
            size_t document_len, i, count = out->documents.count;
            if (count >= DND_RAG_HITS_MAX ||
                pb_read_bytes(&r, &document, &document_len) != 0 ||
                decode_rag_document(document, document_len, &out->documents.hits[count], count) != 0)
                goto reject;
            for (i = 0; i < count; ++i)
                if (strcmp(dnd_rag_citation_id(&out->documents.hits[i].citation),
                           dnd_rag_citation_id(&out->documents.hits[count].citation)) == 0) goto reject;
            ++out->documents.count;
        } else if (field == 4 && wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > 1) goto reject;
            out->used_rag = (int)v;
        } else if (pb_skip(&r, wire) != 0) {
            goto reject;
        }
    }
    if (out->request_id[0] && (!out->documents.count || (out->used_rag && !out->error[0]))) return 0;
reject:
    memset(out, 0, sizeof(*out));
    return -1;
}

static int session_role_span_valid(const uint8_t *role, size_t role_len) {
    return role &&
        ((role_len == sizeof("user") - 1u &&
          memcmp(role, "user", sizeof("user") - 1u) == 0) ||
         (role_len == sizeof("assistant") - 1u &&
          memcmp(role, "assistant", sizeof("assistant") - 1u) == 0) ||
         (role_len == sizeof("system") - 1u &&
          memcmp(role, "system", sizeof("system") - 1u) == 0));
}

static int decode_session_message_impl(
    const uint8_t *data,
    size_t len,
    session_message_c *out
) {
    pb_reader r;
    const uint8_t *role = NULL;
    const uint8_t *content = NULL;
    const uint8_t *request_id = NULL;
    size_t role_len = 0;
    size_t content_len = 0;
    size_t request_id_len = 0;
    int64_t timestamp_ms = 0;
    if (!data && len != 0) return -1;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && (field == 1 || field == 2 || field == 4)) {
            const uint8_t *value;
            size_t value_len;
            size_t cap;
            if (field == 1) cap = sizeof(((session_message_c *)0)->role);
            else if (field == 2) cap = sizeof(((session_message_c *)0)->content);
            else cap = sizeof(((session_message_c *)0)->request_id);
            if (pb_read_string_view(&r, cap, &value, &value_len) != 0) return -1;
            if (field == 1) {
                role = value;
                role_len = value_len;
            } else if (field == 2) {
                content = value;
                content_len = value_len;
            } else {
                request_id = value;
                request_id_len = value_len;
            }
        } else if (field == 3 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > INT64_MAX) return -1;
            timestamp_ms = (int64_t)value;
        } else if (field == 5 && wire == 2) {
            const uint8_t *entry;
            size_t entry_len;
            char key[128];
            char value[256];
            if (pb_read_bytes(&r, &entry, &entry_len) != 0 ||
                decode_map_string_entry(
                    entry, entry_len, key, sizeof(key), value, sizeof(value)) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    if (!session_role_span_valid(role, role_len) || content_len == 0) return -1;
    if (out) {
        memcpy(out->role, role, role_len);
        out->role[role_len] = '\0';
        memcpy(out->content, content, content_len);
        out->content[content_len] = '\0';
        out->timestamp_ms = timestamp_ms;
        if (request_id) memcpy(out->request_id, request_id, request_id_len);
        out->request_id[request_id_len] = '\0';
    }
    return 0;
}

int pb_decode_session_message(
    const uint8_t *data,
    size_t len,
    session_message_c *out
) {
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    return decode_session_message_impl(data, len, out);
}

size_t pb_encode_session_message(
    uint8_t *out,
    size_t out_cap,
    const session_message_c *in
) {
    size_t pos = 0;
    if (!out || !in || !in->role[0] || !in->content[0] || in->timestamp_ms < 0 ||
        (strcmp(in->role, "user") != 0 && strcmp(in->role, "assistant") != 0 &&
         strcmp(in->role, "system") != 0)) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->role);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, in->content);
    if (pos == (size_t)-1) return 0;
    if (in->timestamp_ms > 0) {
        pos = write_varint_field(out, out_cap, pos, 3, (uint64_t)in->timestamp_ms);
        if (pos == (size_t)-1) return 0;
    }
    if (in->request_id[0]) {
        pos = write_string_field(out, out_cap, pos, 4, in->request_id);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_session_append_request(
    uint8_t *out,
    size_t out_cap,
    const session_append_request_c *in
) {
    uint8_t message[4608];
    size_t message_len;
    size_t pos = 0;
    if (!out || !in || !in->session_id[0] || !in->user_id[0]) return 0;
    message_len = pb_encode_session_message(message, sizeof(message), &in->message);
    if (!message_len) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->session_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, in->user_id);
    if (pos == (size_t)-1) return 0;
    pos = write_bytes_field(out, out_cap, pos, 3, message, message_len);
    return pos == (size_t)-1 ? 0 : pos;
}

int pb_decode_session_append_request(
    const uint8_t *data,
    size_t len,
    session_append_request_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && (field == 1 || field == 2)) {
            char *dst = field == 1 ? out->session_id : out->user_id;
            size_t cap = field == 1 ? sizeof(out->session_id) : sizeof(out->user_id);
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (field == 3 && wire == 2) {
            if (out->message_wire != NULL ||
                pb_read_bytes(&r, &out->message_wire, &out->message_wire_len) != 0 ||
                pb_decode_session_message(
                    out->message_wire, out->message_wire_len, &out->message) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    return out->session_id[0] && out->message_wire ? 0 : -1;
}

int pb_decode_session_append_view(
    const uint8_t *data,
    size_t len,
    session_append_view_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    out->session_id[0] = '\0';
    out->user_id[0] = '\0';
    out->message_wire = NULL;
    out->message_wire_len = 0;
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && (field == 1 || field == 2)) {
            char *dst = field == 1 ? out->session_id : out->user_id;
            size_t cap = field == 1 ? sizeof(out->session_id) : sizeof(out->user_id);
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (field == 3 && wire == 2) {
            if (out->message_wire != NULL ||
                pb_read_bytes(&r, &out->message_wire, &out->message_wire_len) != 0 ||
                decode_session_message_impl(
                    out->message_wire, out->message_wire_len, NULL) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    return out->session_id[0] && out->message_wire ? 0 : -1;
}

size_t pb_encode_session_append_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int32_t message_count
) {
    size_t pos = 0;
    if (!out || !session_id || !session_id[0] || message_count < 0) return 0;
    pos = write_string_field(out, out_cap, pos, 1, session_id);
    if (pos == (size_t)-1) return 0;
    if (message_count > 0) {
        pos = write_varint_field(out, out_cap, pos, 2, (uint32_t)message_count);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_session_append_response(
    const uint8_t *data,
    size_t len,
    session_append_response_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->session_id, sizeof(out->session_id)) != 0) return -1;
        } else if (field == 2 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > INT32_MAX) return -1;
            out->message_count = (int32_t)value;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

int pb_decode_session_get_request(
    const uint8_t *data,
    size_t len,
    session_get_request_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && (field == 1 || field == 2)) {
            char *dst = field == 1 ? out->session_id : out->user_id;
            size_t cap = field == 1 ? sizeof(out->session_id) : sizeof(out->user_id);
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (field == 3 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > INT32_MAX) return -1;
            out->last_n = (int32_t)value;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

size_t pb_encode_session_get_request(
    uint8_t *out,
    size_t out_cap,
    const session_get_request_c *in
) {
    size_t pos = 0;
    if (!out || !in || !in->session_id[0] || !in->user_id[0] || in->last_n < 0) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->session_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, in->user_id);
    if (pos == (size_t)-1) return 0;
    if (in->last_n > 0) {
        pos = write_varint_field(out, out_cap, pos, 3, (uint32_t)in->last_n);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_session_get_response_prefix(
    uint8_t *out,
    size_t out_cap,
    const char *session_id
) {
    size_t pos;
    if (!out || !session_id || !session_id[0]) return 0;
    pos = write_string_field(out, out_cap, 0, 1, session_id);
    return pos == (size_t)-1 ? 0 : pos;
}

int pb_decode_session_get_response(
    const uint8_t *data,
    size_t len,
    session_get_response_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->session_id, sizeof(out->session_id)) != 0) return -1;
        } else if (field == 2 && wire == 2) {
            const uint8_t *message_wire;
            size_t message_len;
            session_message_c message;
            if (pb_read_bytes(&r, &message_wire, &message_len) != 0 ||
                pb_decode_session_message(message_wire, message_len, &message) != 0) return -1;
            if (out->message_count == 0) out->first_message = message;
            out->last_message = message;
            out->message_count++;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

int pb_decode_session_history_response(const uint8_t *data, size_t len,
    session_history_response_c *out) {
    pb_reader r;
    int identity_seen = 0;
    if (!out || (!data && len)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire)) return -1;
        if (field == 1 && wire == 2) {
            if (identity_seen++ || pb_read_string(&r, out->session_id,
                sizeof(out->session_id))) return -1;
        } else if (field == 2 && wire == 2) {
            const uint8_t *message;
            size_t length;
            if (out->count == SESSION_HISTORY_MAX ||
                pb_read_bytes(&r, &message, &length) ||
                pb_decode_session_message(message, length, &out->messages[out->count])) return -1;
            const session_message_c *m = &out->messages[out->count];
            if ((strcmp(m->role, "user") && strcmp(m->role, "assistant")) ||
                !m->content[0] || !m->request_id[0]) return -1;
            ++out->count;
        } else if (pb_skip(&r, wire)) return -1;
    }
    return identity_seen && out->session_id[0] ? 0 : -1;
}

int pb_decode_session_id_request(
    const uint8_t *data,
    size_t len,
    session_id_request_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && (field == 1 || field == 2)) {
            char *dst = field == 1 ? out->session_id : out->user_id;
            size_t cap = field == 1 ? sizeof(out->session_id) : sizeof(out->user_id);
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

size_t pb_encode_session_id_request(
    uint8_t *out,
    size_t out_cap,
    const session_id_request_c *in
) {
    size_t pos = 0;
    if (!out || !in || !in->session_id[0] || !in->user_id[0]) return 0;
    pos = write_string_field(out, out_cap, pos, 1, in->session_id);
    if (pos == (size_t)-1) return 0;
    pos = write_string_field(out, out_cap, pos, 2, in->user_id);
    return pos == (size_t)-1 ? 0 : pos;
}

size_t pb_encode_session_delete_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    int deleted
) {
    size_t pos;
    if (!out || !session_id || !session_id[0]) return 0;
    pos = write_string_field(out, out_cap, 0, 1, session_id);
    if (pos == (size_t)-1) return 0;
    if (deleted) {
        pos = write_varint_field(out, out_cap, pos, 2, 1);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_session_delete_response(
    const uint8_t *data,
    size_t len,
    session_delete_response_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, out->session_id, sizeof(out->session_id)) != 0) return -1;
        } else if (field == 2 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > 1) return -1;
            out->deleted = (int)value;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

size_t pb_encode_session_summary_response(
    uint8_t *out,
    size_t out_cap,
    const char *session_id,
    const char *user_id,
    const char *summary,
    const char *source,
    int64_t updated_at_ms
) {
    size_t pos = 0;
    if (!out || !session_id || !session_id[0] || !summary || updated_at_ms < 0) return 0;
    pos = write_string_field(out, out_cap, pos, 1, session_id);
    if (pos == (size_t)-1) return 0;
    if (user_id && user_id[0]) {
        pos = write_string_field(out, out_cap, pos, 2, user_id);
        if (pos == (size_t)-1) return 0;
    }
    if (summary[0]) {
        pos = write_string_field(out, out_cap, pos, 3, summary);
        if (pos == (size_t)-1) return 0;
    }
    if (source && source[0]) {
        pos = write_string_field(out, out_cap, pos, 4, source);
        if (pos == (size_t)-1) return 0;
    }
    if (updated_at_ms > 0) {
        pos = write_varint_field(out, out_cap, pos, 5, (uint64_t)updated_at_ms);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

int pb_decode_session_summary_response(
    const uint8_t *data,
    size_t len,
    session_summary_response_c *out
) {
    pb_reader r;
    if (!out || (!data && len != 0)) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2 && field >= 1 && field <= 4) {
            char *dst;
            size_t cap;
            if (field == 1) { dst = out->session_id; cap = sizeof(out->session_id); }
            else if (field == 2) { dst = out->user_id; cap = sizeof(out->user_id); }
            else if (field == 3) { dst = out->summary; cap = sizeof(out->summary); }
            else { dst = out->source; cap = sizeof(out->source); }
            if (pb_read_string(&r, dst, cap) != 0) return -1;
        } else if (field == 5 && wire == 0) {
            uint64_t value;
            if (pb_read_varint(&r, &value) != 0 || value > INT64_MAX) return -1;
            out->updated_at_ms = (int64_t)value;
        } else if (field == 6 && wire == 2) {
            const uint8_t *entry;
            size_t entry_len;
            char key[128];
            char value[256];
            if (pb_read_bytes(&r, &entry, &entry_len) != 0 ||
                decode_map_string_entry(
                    entry, entry_len, key, sizeof(key), value, sizeof(value)) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0) return -1;
    }
    return out->session_id[0] ? 0 : -1;
}

static PB_NOINLINE int turn_event_type_id(const char *type, int *is_canonical) {
    size_t type_len = 0;
    if (!type || !is_canonical) return 0;
    *is_canonical = 0;
    while (type_len <= 16u && type[type_len] != '\0') type_len++;
    if (type_len > 16u) return 0;
#define MATCH_EVENT_TYPE(literal, id, canonical) do { \
        if (memcmp(type, (literal), sizeof(literal) - 1u) == 0) { \
            *is_canonical = (canonical); \
            return (id); \
        } \
    } while (0)
    switch (type_len) {
    case 4:
        MATCH_EVENT_TYPE("done", 10, 0);
        break;
    case 5:
        if (type[0] == 'f') MATCH_EVENT_TYPE("final", 5, 0);
        if (type[0] == 'r') MATCH_EVENT_TYPE("route", 2, 0);
        break;
    case 6:
        MATCH_EVENT_TYPE("failed", 12, 1);
        break;
    case 7:
        if (type[0] == 's') MATCH_EVENT_TYPE("started", 1, 1);
        if (type[0] == 't') MATCH_EVENT_TYPE("tts_pcm", 8, 0);
        break;
    case 8:
        if (type[0] == 'c') MATCH_EVENT_TYPE("canceled", 11, 1);
        if (type[0] == 'e') MATCH_EVENT_TYPE("endpoint", 10, 0);
        if (type[0] == 'p') MATCH_EVENT_TYPE("pcm_meta", 7, 0);
        if (type[0] == 'r') MATCH_EVENT_TYPE("rag_hits", 2, 0);
        if (type[0] == 's') MATCH_EVENT_TYPE("snapshot", 2, 0);
        if (type[0] == 't') MATCH_EVENT_TYPE("tts_done", 9, 0);
        break;
    case 9:
        if (type[0] == 'c' && type[1] == 'o')
            MATCH_EVENT_TYPE("completed", 10, 1);
        if (type[0] == 'c' && type[1] == 'a')
            MATCH_EVENT_TYPE("cancelled", 11, 0);
        if (type[0] == 'i') MATCH_EVENT_TYPE("interrupt", 11, 0);
        if (type[0] == 'p' && type[4] == 'c')
            MATCH_EVENT_TYPE("pcm_chunk", 8, 1);
        if (type[0] == 'p' && type[4] == 'e')
            MATCH_EVENT_TYPE("pcm_ended", 9, 1);
        break;
    case 10:
        MATCH_EVENT_TYPE("text_delta", 4, 1);
        break;
    case 11:
        if (type[0] == 'n') MATCH_EVENT_TYPE("needs_model", 2, 0);
        if (type[0] == 'p') MATCH_EVENT_TYPE("pcm_started", 7, 1);
        if (type[0] == 't' && type[5] == 'e')
            MATCH_EVENT_TYPE("tts_segment", 6, 1);
        if (type[0] == 't' && type[5] == 't')
            MATCH_EVENT_TYPE("tts_started", 7, 0);
        break;
    case 14:
        if (type[1] == 'h') MATCH_EVENT_TYPE("thinking_ended", 3, 1);
        if (type[1] == 'e') MATCH_EVENT_TYPE("text_completed", 5, 1);
        break;
    case 16:
        if (type[1] == 'h') MATCH_EVENT_TYPE("thinking_started", 2, 1);
        if (type[1] == 'r') MATCH_EVENT_TYPE("transcript_clean", 2, 0);
        break;
    default:
        break;
    }
#undef MATCH_EVENT_TYPE
    return 0;
}

static const char *turn_event_type_name(int type_id, size_t *type_len) {
    static const char *const names[] = {
        "unknown", "started", "thinking_started", "thinking_ended", "text_delta",
        "text_completed", "tts_segment", "pcm_started", "pcm_chunk", "pcm_ended",
        "completed", "canceled", "failed"
    };
    static const uint8_t lengths[] = {
        7u, 7u, 16u, 14u, 10u, 14u, 11u, 11u, 9u, 9u, 9u, 8u, 6u
    };
    size_t index = type_id >= 1 && type_id <= 12 ? (size_t)type_id : 0u;
    assert(type_len);
    *type_len = lengths[index];
    return names[index];
}

static size_t write_map_ss_field_lengths(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    const char *key,
    size_t key_len,
    const char *val,
    size_t val_len
) {
    size_t entry_len;
    if (!out || !key || !val || key_len > 127u || val_len > 127u)
        return (size_t)-1;
    entry_len = 4u + key_len + val_len;
    pos = write_tag(out, cap, pos, field, 2);
    if (pos == (size_t)-1) return pos;
    pos = write_varint(out, cap, pos, entry_len);
    if (pos == (size_t)-1 || pos > cap || entry_len > cap - pos)
        return (size_t)-1;
    out[pos++] = (uint8_t)((1u << 3) | 2u);
    out[pos++] = (uint8_t)key_len;
    if (key_len != 0) memcpy(out + pos, key, key_len);
    pos += key_len;
    out[pos++] = (uint8_t)((2u << 3) | 2u);
    out[pos++] = (uint8_t)val_len;
    if (val_len != 0) memcpy(out + pos, val, val_len);
    return pos + val_len;
}

static size_t write_map_ss_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint32_t field,
    const char *key,
    const char *val
) {
    return write_map_ss_field_lengths(
        out, cap, pos, field,
        key, key ? strlen(key) : 0,
        val, val ? strlen(val) : 0);
}

static size_t write_stage_map_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const char *key,
    size_t key_len,
    const char *value,
    size_t value_len
) {
    size_t entry_len;
    size_t field_len;
    if (!out || !key || !value || key_len > 127u || value_len > 127u)
        return (size_t)-1;
    entry_len = 4u + key_len + value_len;
    field_len = 3u + entry_len;
    if (pos > cap || field_len > cap - pos) return (size_t)-1;
    out[pos++] = 0x82u;
    out[pos++] = 0x01u;
    out[pos++] = (uint8_t)entry_len;
    out[pos++] = 0x0au;
    out[pos++] = (uint8_t)key_len;
    memcpy(out + pos, key, key_len);
    pos += key_len;
    out[pos++] = 0x12u;
    out[pos++] = (uint8_t)value_len;
    memcpy(out + pos, value, value_len);
    return pos + value_len;
}

#define STAGE_DECIMAL_DIGITS 19u
#define STAGE_DECIMAL_SUFFIX_DIGITS 6u
#define STAGE_DECIMAL_SUFFIX_BASE UINT64_C(1000000)
#define STAGE_DECIMAL_CURRENT_PREFIX_DIGITS 7u

_Static_assert(
    STAGE_DECIMAL_SUFFIX_BASE <= UINT32_MAX,
    "stage decimal suffix must fit uint32_t");

static const char stage_decimal_pairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

typedef struct {
    char digits[STAGE_DECIMAL_DIGITS - STAGE_DECIMAL_SUFFIX_DIGITS];
    size_t length;
    uint64_t base;
    int ready;
} stage_decimal_prefix;

static PB_ALWAYS_INLINE void write_stage_decimal_suffix_digits(
    uint8_t out[STAGE_DECIMAL_SUFFIX_DIGITS],
    uint32_t low
) {
    size_t pair = (size_t)(low / UINT32_C(10000)) * 2u;
    out[0] = (uint8_t)stage_decimal_pairs[pair];
    out[1] = (uint8_t)stage_decimal_pairs[pair + 1u];
    low %= UINT32_C(10000);
    pair = (size_t)(low / UINT32_C(100)) * 2u;
    out[2] = (uint8_t)stage_decimal_pairs[pair];
    out[3] = (uint8_t)stage_decimal_pairs[pair + 1u];
    pair = (size_t)(low % UINT32_C(100)) * 2u;
    out[4] = (uint8_t)stage_decimal_pairs[pair];
    out[5] = (uint8_t)stage_decimal_pairs[pair + 1u];
}

static size_t write_stage_map_decimal_suffix(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const char *key,
    size_t key_len,
    const stage_decimal_prefix *prefix,
    uint32_t low
) {
    size_t entry_len;
    size_t field_len;
    if (!out || !key || !prefix || key_len > 127u) return (size_t)-1;
    entry_len = 4u + key_len + prefix->length + STAGE_DECIMAL_SUFFIX_DIGITS;
    field_len = 3u + entry_len;
    if (pos > cap || field_len > cap - pos) return (size_t)-1;
    out[pos++] = 0x82u;
    out[pos++] = 0x01u;
    out[pos++] = (uint8_t)entry_len;
    out[pos++] = 0x0au;
    out[pos++] = (uint8_t)key_len;
    memcpy(out + pos, key, key_len);
    pos += key_len;
    out[pos++] = 0x12u;
    out[pos++] = (uint8_t)(prefix->length + STAGE_DECIMAL_SUFFIX_DIGITS);
    if (prefix->length == STAGE_DECIMAL_CURRENT_PREFIX_DIGITS) {
        memcpy(
            out + pos,
            prefix->digits,
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS);
        pos += STAGE_DECIMAL_CURRENT_PREFIX_DIGITS;
    } else {
        memcpy(out + pos, prefix->digits, prefix->length);
        pos += prefix->length;
    }
    write_stage_decimal_suffix_digits(out + pos, low);
    return pos + STAGE_DECIMAL_SUFFIX_DIGITS;
}

static size_t write_stage_decimal_digits(
    char digits[STAGE_DECIMAL_DIGITS],
    uint64_t remaining
) {
    size_t digit_pos = STAGE_DECIMAL_DIGITS;
    while (remaining >= UINT64_C(100)) {
        uint64_t quotient = remaining / UINT64_C(100);
        size_t pair =
            (size_t)(remaining - quotient * UINT64_C(100)) * 2u;
        digit_pos -= 2u;
        digits[digit_pos] = stage_decimal_pairs[pair];
        digits[digit_pos + 1u] = stage_decimal_pairs[pair + 1u];
        remaining = quotient;
    }
    if (remaining < UINT64_C(10)) {
        digits[--digit_pos] = (char)('0' + remaining);
    } else {
        size_t pair = (size_t)remaining * 2u;
        digit_pos -= 2u;
        digits[digit_pos] = stage_decimal_pairs[pair];
        digits[digit_pos + 1u] = stage_decimal_pairs[pair + 1u];
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
    if (!prefix) return;
    memset(prefix, 0, sizeof(*prefix));
    if (value < (int64_t)STAGE_DECIMAL_SUFFIX_BASE) return;
    high = (uint64_t)value / STAGE_DECIMAL_SUFFIX_BASE;
    prefix->base = high * STAGE_DECIMAL_SUFFIX_BASE;
    if (high >= UINT64_C(1000000) && high < UINT64_C(10000000)) {
        uint32_t current_high = (uint32_t)high;
        prefix->digits[0] =
            (char)('0' + current_high / UINT32_C(1000000));
        write_stage_decimal_suffix_digits(
            (uint8_t *)prefix->digits + 1u,
            current_high % UINT32_C(1000000));
        prefix->length = STAGE_DECIMAL_CURRENT_PREFIX_DIGITS;
    } else {
        start = write_stage_decimal_digits(digits, high);
        prefix->length = STAGE_DECIMAL_DIGITS - start;
        memcpy(prefix->digits, digits + start, prefix->length);
    }
    prefix->ready = 1;
}

enum {
    STAGE_TTS_FIRST_TEXT_VALUE_OFFSET = 29u,
    STAGE_TTS_SEGMENT_VALUE_OFFSET = 80u,
    STAGE_TTS_RECEIVED_VALUE_OFFSET = 132u,
    STAGE_TTS_PROVIDER_START_VALUE_OFFSET = 192u,
    STAGE_TTS_PROVIDER_READY_VALUE_OFFSET = 242u,
    STAGE_TTS_PCM_START_VALUE_OFFSET = 285u,
    STAGE_TTS_CURRENT_WIRE_LENGTH = 298u,
    STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET = 34u,
    STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH = 47u,
};

static const uint8_t stage_tts_current_wire[] =
    "\x82\x01\x27\x0a\x16" "stage_first_text_at_ms" "\x12\x0d" "0000000000000"
    "\x82\x01\x30\x0a\x1f" "stage_tts_segment_emitted_at_ms" "\x12\x0d" "0000000000000"
    "\x82\x01\x31\x0a\x20" "stage_tts_request_received_at_ms" "\x12\x0d" "0000000000000"
    "\x82\x01\x39\x0a\x28" "stage_tts_provider_request_started_at_ms" "\x12\x0d" "0000000000000"
    "\x82\x01\x2f\x0a\x1e" "stage_tts_provider_ready_at_ms" "\x12\x0d" "0000000000000"
    "\x82\x01\x28\x0a\x17" "stage_pcm_started_at_ms" "\x12\x0d" "0000000000000";

static const uint8_t stage_pcm_first_chunk_current_wire[] =
    "\x82\x01\x2c\x0a\x1b" "stage_pcm_first_chunk_at_ms" "\x12\x0d"
    "0000000000000";

_Static_assert(
    sizeof(stage_tts_current_wire) - 1u == STAGE_TTS_CURRENT_WIRE_LENGTH,
    "current TTS stage wire length must match its offsets");
_Static_assert(
    STAGE_TTS_PCM_START_VALUE_OFFSET + STAGE_DECIMAL_CURRENT_PREFIX_DIGITS +
        STAGE_DECIMAL_SUFFIX_DIGITS == STAGE_TTS_CURRENT_WIRE_LENGTH,
    "current TTS stage value offsets must cover the wire");
_Static_assert(
    sizeof(stage_pcm_first_chunk_current_wire) - 1u ==
        STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH,
    "current first PCM stage wire length must match its offset");
_Static_assert(
    STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET +
        STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS ==
        STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH,
    "current first PCM value offset must cover the wire");

static PB_ALWAYS_INLINE int stage_decimal_prefix_contains(
    const stage_decimal_prefix *prefix,
    int64_t value
) {
    uint64_t remaining;
    if (!prefix || !prefix->ready || value < 0) return 0;
    remaining = (uint64_t)value;
    return remaining >= prefix->base &&
        remaining - prefix->base < STAGE_DECIMAL_SUFFIX_BASE;
}

static PB_ALWAYS_INLINE void write_stage_current_value(
    uint8_t *out,
    const stage_decimal_prefix *prefix,
    int64_t value
) {
    uint32_t low = (uint32_t)((uint64_t)value - prefix->base);
    memcpy(out, prefix->digits, STAGE_DECIMAL_CURRENT_PREFIX_DIGITS);
    write_stage_decimal_suffix_digits(
        out + STAGE_DECIMAL_CURRENT_PREFIX_DIGITS, low);
}

static size_t write_current_tts_stage_timestamps(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const turn_stage_timestamps_c *stages,
    const stage_decimal_prefix *prefix
) {
    if (!out || !stages || !prefix || pos > cap ||
        STAGE_TTS_CURRENT_WIRE_LENGTH > cap - pos) return (size_t)-1;
    memcpy(
        out + pos,
        stage_tts_current_wire,
        STAGE_TTS_CURRENT_WIRE_LENGTH);
    write_stage_current_value(
        out + pos + STAGE_TTS_FIRST_TEXT_VALUE_OFFSET,
        prefix,
        stages->first_text_at_ms);
    write_stage_current_value(
        out + pos + STAGE_TTS_SEGMENT_VALUE_OFFSET,
        prefix,
        stages->tts_segment_emitted_at_ms);
    write_stage_current_value(
        out + pos + STAGE_TTS_RECEIVED_VALUE_OFFSET,
        prefix,
        stages->tts_request_received_at_ms);
    write_stage_current_value(
        out + pos + STAGE_TTS_PROVIDER_START_VALUE_OFFSET,
        prefix,
        stages->tts_provider_request_started_at_ms);
    write_stage_current_value(
        out + pos + STAGE_TTS_PROVIDER_READY_VALUE_OFFSET,
        prefix,
        stages->tts_provider_ready_at_ms);
    write_stage_current_value(
        out + pos + STAGE_TTS_PCM_START_VALUE_OFFSET,
        prefix,
        stages->pcm_started_at_ms);
    return pos + STAGE_TTS_CURRENT_WIRE_LENGTH;
}

static int prepare_current_tts_stage_wire(
    turn_stage_wire_c *prepared,
    const turn_stage_timestamps_c *stages
) {
    stage_decimal_prefix prefix;
    uint64_t high;
    uint64_t base;
    size_t length;
    if (!prepared || !stages || input_stages_any(&stages->input) ||
        stages->first_text_at_ms <= 0 ||
        stages->tts_segment_emitted_at_ms < stages->first_text_at_ms ||
        stages->tts_request_received_at_ms <
            stages->tts_segment_emitted_at_ms ||
        stages->tts_provider_request_started_at_ms <
            stages->tts_request_received_at_ms ||
        stages->tts_provider_ready_at_ms <
            stages->tts_provider_request_started_at_ms ||
        stages->pcm_started_at_ms < stages->tts_provider_ready_at_ms ||
        stages->pcm_first_chunk_at_ms != 0)
        return 0;
    high = (uint64_t)stages->tts_request_received_at_ms /
        STAGE_DECIMAL_SUFFIX_BASE;
    if (high < UINT64_C(1000000) || high >= UINT64_C(10000000)) return 0;
    base = high * STAGE_DECIMAL_SUFFIX_BASE;
    if ((uint64_t)stages->first_text_at_ms < base ||
        (uint64_t)stages->pcm_started_at_ms - base >=
            STAGE_DECIMAL_SUFFIX_BASE)
        return 0;
    prefix.base = base;
    prefix.length = STAGE_DECIMAL_CURRENT_PREFIX_DIGITS;
    prefix.ready = 1;
    prefix.digits[0] = (char)('0' + high / UINT64_C(1000000));
    write_stage_decimal_suffix_digits(
        (uint8_t *)prefix.digits + 1u,
        (uint32_t)(high % UINT64_C(1000000)));
    length = write_current_tts_stage_timestamps(
        prepared->data,
        sizeof(prepared->data),
        0u,
        stages,
        &prefix);
    if (length == (size_t)-1) return 0;
    prepared->len = length;
    prepared->request_received_at_ms = stages->tts_request_received_at_ms;
    prepared->pcm_started_at_ms = stages->pcm_started_at_ms;
    prepared->current_tts_template = 1;
    return 1;
}

static PB_ALWAYS_INLINE size_t write_stage_timestamp(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const char *key,
    size_t key_len,
    int64_t value,
    const stage_decimal_prefix *prefix
) {
    uint64_t remaining;
    if (value == 0) return pos;
    if (value < 0) return (size_t)-1;
    remaining = (uint64_t)value;
    if (prefix && prefix->ready &&
        remaining >= prefix->base &&
        remaining - prefix->base < STAGE_DECIMAL_SUFFIX_BASE) {
        return write_stage_map_decimal_suffix(
            out, cap, pos, key, key_len, prefix,
            (uint32_t)(remaining - prefix->base));
    }
    {
        char digits[STAGE_DECIMAL_DIGITS];
        size_t digit_pos = write_stage_decimal_digits(digits, remaining);
        return write_stage_map_field(
            out, cap, pos, key, key_len,
            digits + digit_pos, STAGE_DECIMAL_DIGITS - digit_pos);
    }
}

static PB_NOINLINE size_t write_stage_timestamps(
    uint8_t *out,
    size_t cap,
    size_t pos,
    const turn_stage_timestamps_c *stages,
    int *current_tts_out
) {
    stage_decimal_prefix prefix;
    int any;
    if (current_tts_out) *current_tts_out = 0;
    if (!stages) return pos;
    any = input_stages_any(&stages->input) || stages->first_text_at_ms != 0 ||
        stages->tts_segment_emitted_at_ms != 0 ||
        stages->tts_request_received_at_ms != 0 ||
        stages->tts_provider_request_started_at_ms != 0 ||
        stages->tts_provider_ready_at_ms != 0 || stages->pcm_started_at_ms != 0 ||
        stages->pcm_first_chunk_at_ms != 0;
    if (!any) return pos;
    if (!input_stages_valid(&stages->input) ||
        (input_stages_any(&stages->input) &&
         (stages->first_text_at_ms == 0 ||
          stages->first_text_at_ms <
            stages->input.stt_transcript_published_at_ms)) ||
        (stages->first_text_at_ms == 0) !=
            (stages->tts_segment_emitted_at_ms == 0) ||
        stages->tts_request_received_at_ms <= 0 ||
        stages->tts_provider_request_started_at_ms <
            stages->tts_request_received_at_ms ||
        stages->tts_provider_ready_at_ms <
            stages->tts_provider_request_started_at_ms ||
        stages->pcm_started_at_ms < stages->tts_provider_ready_at_ms ||
        (stages->pcm_first_chunk_at_ms != 0 &&
         stages->pcm_first_chunk_at_ms < stages->pcm_started_at_ms) ||
        (stages->first_text_at_ms > 0 &&
         (stages->tts_segment_emitted_at_ms < stages->first_text_at_ms ||
          stages->tts_request_received_at_ms <
            stages->tts_segment_emitted_at_ms))) return (size_t)-1;
    stage_decimal_prefix_init(&prefix, stages->tts_request_received_at_ms);
    if (!input_stages_any(&stages->input) && stages->first_text_at_ms != 0 &&
        stages->pcm_first_chunk_at_ms == 0 &&
        prefix.length == STAGE_DECIMAL_CURRENT_PREFIX_DIGITS &&
        stage_decimal_prefix_contains(&prefix, stages->first_text_at_ms) &&
        stage_decimal_prefix_contains(
            &prefix, stages->tts_segment_emitted_at_ms) &&
        stage_decimal_prefix_contains(
            &prefix, stages->tts_provider_request_started_at_ms) &&
        stage_decimal_prefix_contains(
            &prefix, stages->tts_provider_ready_at_ms) &&
        stage_decimal_prefix_contains(&prefix, stages->pcm_started_at_ms))
        {
            size_t current_pos = write_current_tts_stage_timestamps(
                out, cap, pos, stages, &prefix);
            if (current_pos != (size_t)-1 && current_tts_out)
                *current_tts_out = 1;
            return current_pos;
        }
#define WRITE_STAGE(member, key) do { \
        pos = write_stage_timestamp( \
            out, cap, pos, (key), sizeof(key) - 1u, stages->member, &prefix); \
        if (pos == (size_t)-1) return pos; \
    } while (0)
    WRITE_STAGE(input.audio_committed_at_ms, "stage_audio_committed_at_ms");
    WRITE_STAGE(input.stt_request_received_at_ms, "stage_stt_request_received_at_ms");
    WRITE_STAGE(
        input.stt_provider_request_started_at_ms,
        "stage_stt_provider_request_started_at_ms");
    WRITE_STAGE(input.stt_provider_ready_at_ms, "stage_stt_provider_ready_at_ms");
    WRITE_STAGE(
        input.stt_transcript_published_at_ms,
        "stage_stt_transcript_published_at_ms");
    WRITE_STAGE(first_text_at_ms, "stage_first_text_at_ms");
    WRITE_STAGE(tts_segment_emitted_at_ms, "stage_tts_segment_emitted_at_ms");
    WRITE_STAGE(tts_request_received_at_ms, "stage_tts_request_received_at_ms");
    WRITE_STAGE(
        tts_provider_request_started_at_ms,
        "stage_tts_provider_request_started_at_ms");
    WRITE_STAGE(tts_provider_ready_at_ms, "stage_tts_provider_ready_at_ms");
    WRITE_STAGE(pcm_started_at_ms, "stage_pcm_started_at_ms");
    WRITE_STAGE(pcm_first_chunk_at_ms, "stage_pcm_first_chunk_at_ms");
#undef WRITE_STAGE
    return pos;
}

static int turn_stage_wire_valid(const turn_stage_wire_c *prepared) {
    size_t current_len;
    if (!prepared || prepared->len == 0u ||
        prepared->len > sizeof(prepared->data) ||
        prepared->request_received_at_ms <= 0 || prepared->pcm_started_at_ms <= 0 ||
        (prepared->complete != 0 && prepared->complete != 1) ||
        (prepared->current_tts_template != 0 &&
         prepared->current_tts_template != 1)) return 0;
    if (!prepared->current_tts_template) return 1;
    current_len = STAGE_TTS_CURRENT_WIRE_LENGTH;
    if (prepared->complete)
        current_len += STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH;
    return prepared->len == current_len;
}

int pb_prepare_turn_stage_wire(
    turn_stage_wire_c *prepared,
    const turn_stage_timestamps_c *stages
) {
    size_t length;
    if (!prepared) return -1;
    prepared->len = 0u;
    prepared->request_received_at_ms = 0;
    prepared->pcm_started_at_ms = 0;
    prepared->complete = 0;
    prepared->current_tts_template = 0;
    if (!stages || stages->pcm_started_at_ms <= 0 ||
        stages->pcm_first_chunk_at_ms != 0) return -1;
    if (prepare_current_tts_stage_wire(prepared, stages)) return 0;
    length = write_stage_timestamps(
        prepared->data,
        sizeof(prepared->data),
        0u,
        stages,
        &prepared->current_tts_template);
    if (length == 0u || length == (size_t)-1) return -1;
    prepared->len = length;
    prepared->request_received_at_ms = stages->tts_request_received_at_ms;
    prepared->pcm_started_at_ms = stages->pcm_started_at_ms;
    return 0;
}

int pb_complete_turn_stage_wire(
    turn_stage_wire_c *prepared,
    int64_t pcm_first_chunk_at_ms
) {
    static const char key[] = "stage_pcm_first_chunk_at_ms";
    stage_decimal_prefix prefix;
    size_t length;
    if (!turn_stage_wire_valid(prepared) || prepared->complete ||
        pcm_first_chunk_at_ms < prepared->pcm_started_at_ms) return -1;
    if (prepared->current_tts_template) {
        prefix.length = STAGE_DECIMAL_CURRENT_PREFIX_DIGITS;
        prefix.base =
            (uint64_t)prepared->request_received_at_ms /
                STAGE_DECIMAL_SUFFIX_BASE * STAGE_DECIMAL_SUFFIX_BASE;
        prefix.ready = 1;
        memcpy(
            prefix.digits,
            prepared->data + STAGE_TTS_RECEIVED_VALUE_OFFSET,
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS);
    } else {
        stage_decimal_prefix_init(&prefix, prepared->request_received_at_ms);
    }
    if (prepared->current_tts_template &&
        stage_decimal_prefix_contains(&prefix, pcm_first_chunk_at_ms) &&
        prepared->len <= sizeof(prepared->data) &&
        STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH <=
            sizeof(prepared->data) - prepared->len) {
        memcpy(
            prepared->data + prepared->len,
            stage_pcm_first_chunk_current_wire,
            STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH);
        write_stage_current_value(
            prepared->data + prepared->len +
                STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET,
            &prefix,
            pcm_first_chunk_at_ms);
        length = prepared->len + STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH;
    } else {
        length = write_stage_timestamp(
            prepared->data,
            sizeof(prepared->data),
            prepared->len,
            key,
            sizeof(key) - 1u,
            pcm_first_chunk_at_ms,
            &prefix);
    }
    if (length == (size_t)-1) return -1;
    prepared->len = length;
    prepared->complete = 1;
    return 0;
}

static size_t encode_turn_event_fields(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const char *speech_text,
    const char *display_text,
    const turn_stage_timestamps_c *stages,
    const turn_stage_wire_c *prepared_stages
) {
    size_t pos = 0;
    size_t text_len = 0;
    size_t speech_len = 0;
    size_t display_len = 0;
    int type_is_canonical;
    int type_id = turn_event_type_id(type, &type_is_canonical);
    if (!out || out_cap == 0 || !request_id || !request_id[0] || type_id == 0 ||
        (stages && prepared_stages) ||
        (prepared_stages && !turn_stage_wire_valid(prepared_stages)) ||
        (audio_len != 0 && !audio) ||
        sample_rate < 0 || channels < 0 || bit_depth < 0 || sequence < 0 ||
        segment_index < 0) return 0;
    if (text && text[0]) text_len = strlen(text);
    if (speech_text && speech_text[0]) {
        speech_len = speech_text == text ? text_len : strlen(speech_text);
    }
    if (display_text && display_text[0]) {
        if (display_text == text) display_len = text_len;
        else if (display_text == speech_text) display_len = speech_len;
        else display_len = strlen(display_text);
    }
    pos = write_string_field(out, out_cap, pos, 1, request_id);
    if (pos == (size_t)-1) return 0;
    pos = write_varint_field(out, out_cap, pos, 3, (uint64_t)type_id);
    if (pos == (size_t)-1) return 0;
    if (text_len != 0u) {
        pos = write_string_field_n(out, out_cap, pos, 5, text, text_len);
        if (pos == (size_t)-1) return 0;
    }
    if (audio_len) {
        pos = write_bytes_field(out, out_cap, pos, 6, audio, audio_len);
        if (pos == (size_t)-1) return 0;
    }
    if (sample_rate > 0) {
        pos = write_varint_field(out, out_cap, pos, 7, (uint32_t)sample_rate);
        if (pos == (size_t)-1) return 0;
    }
    if (channels > 0) {
        pos = write_varint_field(out, out_cap, pos, 8, (uint32_t)channels);
        if (pos == (size_t)-1) return 0;
    }
    if (bit_depth > 0) {
        pos = write_varint_field(out, out_cap, pos, 9, (uint32_t)bit_depth);
        if (pos == (size_t)-1) return 0;
    }
    if (sequence > 0) {
        pos = write_varint_field(out, out_cap, pos, 10, (uint32_t)sequence);
        if (pos == (size_t)-1) return 0;
    }
    if (segment_index > 0) {
        pos = write_varint_field(out, out_cap, pos, 11, (uint32_t)segment_index);
        if (pos == (size_t)-1) return 0;
    }
    if (is_final) {
        pos = write_varint_field(out, out_cap, pos, 12, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (audio_len) {
        /* This helper emits raw PCM frames, never a container or compressed stream. */
        pos = write_varint_field(out, out_cap, pos, 15, 1);
        if (pos == (size_t)-1) return 0;
    }
    if (!type_is_canonical) {
        pos = write_map_ss_field(out, out_cap, pos, 16, "c_event_type", type);
        if (pos == (size_t)-1) return 0;
    }
    if (prepared_stages) {
        if (pos > out_cap || prepared_stages->len > out_cap - pos) return 0;
        memcpy(out + pos, prepared_stages->data, prepared_stages->len);
        pos += prepared_stages->len;
    } else {
        pos = write_stage_timestamps(out, out_cap, pos, stages, NULL);
        if (pos == (size_t)-1) return 0;
    }
    if (speech_len != 0u) {
        pos = write_string_field_n(
            out, out_cap, pos, 17, speech_text, speech_len);
        if (pos == (size_t)-1) return 0;
    }
    if (display_len != 0u) {
        pos = write_string_field_n(
            out, out_cap, pos, 18, display_text, display_len);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_turn_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, NULL, 0, 0, 0, 0, 0, 0, 0,
        NULL, NULL, NULL, NULL);
}

size_t pb_encode_turn_event_stages(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const turn_stage_timestamps_c *stages
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, NULL, 0, 0, 0, 0, 0, 0, 0,
        NULL, NULL, stages, NULL);
}

size_t pb_encode_turn_event_stage_wire(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const turn_stage_wire_c *prepared
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, NULL, 0, 0, 0, 0, 0, 0, 0,
        NULL, NULL, NULL, prepared);
}

static PB_ALWAYS_INLINE size_t varint_size(uint64_t value) {
    size_t length = 1u;
    while (value >= UINT64_C(0x80)) {
        value >>= 7u;
        length++;
    }
    return length;
}

static PB_ALWAYS_INLINE size_t write_known_string_field_n(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint16_t tag,
    size_t tag_len,
    const char *value,
    size_t value_len
) {
    size_t length_len;
    uint64_t remaining;
    if (!out || tag_len == 0u || tag_len > 2u ||
        (value_len != 0u && !value)) return (size_t)-1;
    length_len = varint_size((uint64_t)value_len);
    if (pos > cap || tag_len > cap - pos ||
        length_len > cap - pos - tag_len ||
        value_len > cap - pos - tag_len - length_len) return (size_t)-1;
    out[pos++] = (uint8_t)tag;
    if (tag_len == 2u) out[pos++] = (uint8_t)(tag >> 8u);
    remaining = (uint64_t)value_len;
    while (remaining >= UINT64_C(0x80)) {
        out[pos++] = (uint8_t)((remaining & UINT64_C(0x7f)) | UINT64_C(0x80));
        remaining >>= 7u;
    }
    out[pos++] = (uint8_t)remaining;
    if (value_len != 0u) memcpy(out + pos, value, value_len);
    return pos + value_len;
}

static PB_ALWAYS_INLINE size_t write_known_varint_field(
    uint8_t *out,
    size_t cap,
    size_t pos,
    uint8_t tag,
    uint64_t value
) {
    size_t value_len = varint_size(value);
    if (!out || pos > cap || value_len >= cap - pos) return (size_t)-1;
    out[pos++] = tag;
    while (value >= UINT64_C(0x80)) {
        out[pos++] = (uint8_t)((value & UINT64_C(0x7f)) | UINT64_C(0x80));
        value >>= 7u;
    }
    out[pos++] = (uint8_t)value;
    return pos;
}

size_t pb_encode_turn_route_event_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    const char *route_text,
    size_t route_text_len
) {
    static const uint8_t route_metadata[] = {
        0x82u, 0x01u, 0x15u,
        0x0au, 0x0cu,
        'c', '_', 'e', 'v', 'e', 'n', 't', '_', 't', 'y', 'p', 'e',
        0x12u, 0x05u, 'r', 'o', 'u', 't', 'e'
    };
    size_t pos = 0u;
    if (!out || out_cap == 0u || !request_id || request_id_len == 0u ||
        request_id_len >= UINT64_C(0x80) || !route_text ||
        route_text_len == 0u || route_text_len >= UINT64_C(0x80)) return 0u;
    if (request_id_len + route_text_len + sizeof(route_metadata) + 6u > out_cap)
        return 0u;
    out[pos++] = 0x0au;
    out[pos++] = (uint8_t)request_id_len;
    memcpy(out + pos, request_id, request_id_len);
    pos += request_id_len;
    out[pos++] = 0x18u;
    out[pos++] = 0x02u;
    out[pos++] = 0x2au;
    out[pos++] = (uint8_t)route_text_len;
    memcpy(out + pos, route_text, route_text_len);
    pos += route_text_len;
    memcpy(out + pos, route_metadata, sizeof(route_metadata));
    return pos + sizeof(route_metadata);
}

size_t pb_encode_turn_pcm_boundary_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int type_id,
    const turn_stage_wire_c *prepared
) {
    size_t pos = 0u;
    size_t required;
    if (!out || out_cap == 0u || !request_id || request_id_len == 0u ||
        request_id_len >= UINT64_C(0x80) ||
        (type_id != 7 && type_id != 9) ||
        (type_id == 7 &&
         (!turn_stage_wire_valid(prepared) || prepared->complete)) ||
        (type_id == 9 && prepared)) return 0u;
    required = request_id_len + 4u + (prepared ? prepared->len : 0u);
    if (required > out_cap) return 0u;
    out[pos++] = 0x0au;
    out[pos++] = (uint8_t)request_id_len;
    memcpy(out + pos, request_id, request_id_len);
    pos += request_id_len;
    out[pos++] = 0x18u;
    out[pos++] = (uint8_t)type_id;
    if (prepared) {
        memcpy(out + pos, prepared->data, prepared->len);
        pos += prepared->len;
    }
    return pos;
}

size_t pb_encode_turn_text_event_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int type_id,
    const char *legacy_text,
    size_t legacy_len,
    const char *speech_text,
    size_t speech_len,
    const char *display_text,
    size_t display_len,
    int32_t segment_index,
    int is_final
) {
    size_t pos = 0u;
    if (!out || out_cap == 0u || !request_id || request_id_len == 0u ||
        type_id < 1 || type_id > 12 ||
        (legacy_len != 0u && !legacy_text) ||
        (speech_len != 0u && !speech_text) ||
        (display_len != 0u && !display_text) || segment_index < 0) return 0u;
    if (request_id_len < UINT64_C(0x80) &&
        legacy_len != 0u && legacy_len < UINT64_C(0x80) &&
        speech_len != 0u && speech_len < UINT64_C(0x80) &&
        display_len != 0u && display_len < UINT64_C(0x80) &&
        segment_index < INT32_C(0x80)) {
        size_t required = request_id_len + legacy_len + speech_len + display_len + 12u +
            (segment_index > 0 ? 2u : 0u) + (is_final ? 2u : 0u);
        if (required > out_cap) return 0u;
        out[pos++] = 0x0au;
        out[pos++] = (uint8_t)request_id_len;
        memcpy(out + pos, request_id, request_id_len);
        pos += request_id_len;
        out[pos++] = 0x18u;
        out[pos++] = (uint8_t)type_id;
        out[pos++] = 0x2au;
        out[pos++] = (uint8_t)legacy_len;
        memcpy(out + pos, legacy_text, legacy_len);
        pos += legacy_len;
        if (segment_index > 0) {
            out[pos++] = 0x58u;
            out[pos++] = (uint8_t)segment_index;
        }
        if (is_final) {
            out[pos++] = 0x60u;
            out[pos++] = 0x01u;
        }
        out[pos++] = 0x8au;
        out[pos++] = 0x01u;
        out[pos++] = (uint8_t)speech_len;
        memcpy(out + pos, speech_text, speech_len);
        pos += speech_len;
        out[pos++] = 0x92u;
        out[pos++] = 0x01u;
        out[pos++] = (uint8_t)display_len;
        memcpy(out + pos, display_text, display_len);
        return pos + display_len;
    }
    if (request_id_len < UINT64_C(0x80) &&
        legacy_text == speech_text && legacy_text == display_text &&
        legacy_len == speech_len && legacy_len == display_len &&
        legacy_len >= UINT64_C(0x80) && legacy_len < UINT64_C(0x4000) &&
        segment_index < INT32_C(0x80)) {
        size_t required = request_id_len + 3u * legacy_len + 15u +
            (segment_index > 0 ? 2u : 0u) + (is_final ? 2u : 0u);
        if (required > out_cap) return 0u;
        out[pos++] = 0x0au;
        out[pos++] = (uint8_t)request_id_len;
        memcpy(out + pos, request_id, request_id_len);
        pos += request_id_len;
        out[pos++] = 0x18u;
        out[pos++] = (uint8_t)type_id;
        out[pos++] = 0x2au;
        out[pos++] = (uint8_t)((legacy_len & UINT64_C(0x7f)) | UINT64_C(0x80));
        out[pos++] = (uint8_t)(legacy_len >> 7u);
        memcpy(out + pos, legacy_text, legacy_len);
        pos += legacy_len;
        if (segment_index > 0) {
            out[pos++] = 0x58u;
            out[pos++] = (uint8_t)segment_index;
        }
        if (is_final) {
            out[pos++] = 0x60u;
            out[pos++] = 0x01u;
        }
        out[pos++] = 0x8au;
        out[pos++] = 0x01u;
        out[pos++] = (uint8_t)((speech_len & UINT64_C(0x7f)) | UINT64_C(0x80));
        out[pos++] = (uint8_t)(speech_len >> 7u);
        memcpy(out + pos, speech_text, speech_len);
        pos += speech_len;
        out[pos++] = 0x92u;
        out[pos++] = 0x01u;
        out[pos++] = (uint8_t)((display_len & UINT64_C(0x7f)) | UINT64_C(0x80));
        out[pos++] = (uint8_t)(display_len >> 7u);
        memcpy(out + pos, display_text, display_len);
        return pos + display_len;
    }
    pos = write_known_string_field_n(
        out, out_cap, pos, UINT16_C(0x000a), 1u, request_id, request_id_len);
    if (pos == (size_t)-1) return 0u;
    if (pos > out_cap || out_cap - pos < 2u) return 0u;
    out[pos++] = 0x18u;
    out[pos++] = (uint8_t)type_id;
    if (legacy_len != 0u) {
        pos = write_known_string_field_n(
            out, out_cap, pos, UINT16_C(0x002a), 1u, legacy_text, legacy_len);
        if (pos == (size_t)-1) return 0u;
    }
    if (segment_index > 0) {
        pos = write_known_varint_field(
            out, out_cap, pos, 0x58u, (uint32_t)segment_index);
        if (pos == (size_t)-1) return 0u;
    }
    if (is_final) {
        if (pos > out_cap || out_cap - pos < 2u) return 0u;
        out[pos++] = 0x60u;
        out[pos++] = 0x01u;
    }
    if (speech_len != 0u) {
        pos = write_known_string_field_n(
            out, out_cap, pos, UINT16_C(0x018a), 2u, speech_text, speech_len);
        if (pos == (size_t)-1) return 0u;
    }
    if (display_len != 0u) {
        pos = write_known_string_field_n(
            out, out_cap, pos, UINT16_C(0x0192), 2u, display_text, display_len);
        if (pos == (size_t)-1) return 0u;
    }
    return pos;
}

static int turn_pcm_chunk_inputs_valid(
    const char *request_id,
    size_t request_id_len,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    (void)is_final;
    return request_id && request_id_len != 0u &&
        (audio_len == 0u || audio) && sample_rate >= 0 && channels >= 0 &&
        bit_depth >= 0 && sequence >= 0 && segment_index >= 0 &&
        (!prepared || turn_stage_wire_valid(prepared));
}

static const uint8_t turn_current_pcm_format[] = {
    0x38u, 0xc0u, 0xbbu, 0x01u,
    0x40u, 0x01u,
    0x48u, 0x10u,
};

static PB_ALWAYS_INLINE size_t encode_turn_pcm_chunk_prefix(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    size_t audio_len
) {
    size_t pos = write_known_string_field_n(
        out, out_cap, 0u, UINT16_C(0x000a), 1u, request_id, request_id_len);
    if (pos == (size_t)-1 || pos > out_cap || out_cap - pos < 2u)
        return (size_t)-1;
    out[pos++] = 0x18u;
    out[pos++] = 0x08u;
    if (audio_len != 0u) {
        pos = write_known_varint_field(
            out, out_cap, pos, 0x32u, (uint64_t)audio_len);
        if (pos == (size_t)-1) return (size_t)-1;
    }
    return pos;
}

static PB_ALWAYS_INLINE size_t encode_turn_pcm_chunk_suffix(
    uint8_t *out,
    size_t out_cap,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    size_t pos = 0u;
    if (sample_rate == 24000 && channels == 1 && bit_depth == 16) {
        if (pos > out_cap || sizeof(turn_current_pcm_format) > out_cap - pos)
            return (size_t)-1;
        memcpy(
            out + pos,
            turn_current_pcm_format,
            sizeof(turn_current_pcm_format));
        pos += sizeof(turn_current_pcm_format);
    } else {
        if (sample_rate > 0) {
            pos = write_known_varint_field(
                out, out_cap, pos, 0x38u, (uint32_t)sample_rate);
            if (pos == (size_t)-1) return (size_t)-1;
        }
        if (channels > 0) {
            pos = write_known_varint_field(
                out, out_cap, pos, 0x40u, (uint32_t)channels);
            if (pos == (size_t)-1) return (size_t)-1;
        }
        if (bit_depth > 0) {
            pos = write_known_varint_field(
                out, out_cap, pos, 0x48u, (uint32_t)bit_depth);
            if (pos == (size_t)-1) return (size_t)-1;
        }
    }
    if (sequence > 0) {
        pos = write_known_varint_field(
            out, out_cap, pos, 0x50u, (uint32_t)sequence);
        if (pos == (size_t)-1) return (size_t)-1;
    }
    if (segment_index > 0) {
        pos = write_known_varint_field(
            out, out_cap, pos, 0x58u, (uint32_t)segment_index);
        if (pos == (size_t)-1) return (size_t)-1;
    }
    if (is_final) {
        if (pos > out_cap || out_cap - pos < 2u) return (size_t)-1;
        out[pos++] = 0x60u;
        out[pos++] = 0x01u;
    }
    if (audio_len != 0u) {
        if (pos > out_cap || out_cap - pos < 2u) return (size_t)-1;
        out[pos++] = 0x78u;
        out[pos++] = 0x01u;
    }
    if (prepared) {
        if (pos > out_cap || prepared->len > out_cap - pos)
            return (size_t)-1;
        memcpy(out + pos, prepared->data, prepared->len);
        pos += prepared->len;
    }
    return pos;
}

size_t pb_encode_turn_pcm_chunk_prepared(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    size_t prefix_len;
    size_t suffix_len;
    if (!out || out_cap == 0u || !turn_pcm_chunk_inputs_valid(
            request_id, request_id_len, audio, audio_len, sample_rate,
            channels, bit_depth, sequence, segment_index, is_final, prepared))
        return 0u;
    prefix_len = encode_turn_pcm_chunk_prefix(
        out, out_cap, request_id, request_id_len, audio_len);
    if (prefix_len == (size_t)-1 || audio_len > out_cap - prefix_len)
        return 0u;
    if (audio_len != 0u) memcpy(out + prefix_len, audio, audio_len);
    suffix_len = encode_turn_pcm_chunk_suffix(
        out + prefix_len + audio_len, out_cap - prefix_len - audio_len,
        audio_len, sample_rate, channels, bit_depth, sequence, segment_index,
        is_final, prepared);
    if (suffix_len == (size_t)-1) return 0u;
    return prefix_len + audio_len + suffix_len;
}

size_t pb_encode_turn_pcm_chunk_spans_prepared(
    uint8_t *prefix,
    size_t prefix_cap,
    size_t *prefix_len,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    const char *request_id,
    size_t request_id_len,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    size_t prefix_size;
    size_t suffix_size;
    if (prefix_len) *prefix_len = 0u;
    if (suffix_len) *suffix_len = 0u;
    if (!prefix || prefix_cap == 0u || !prefix_len || !suffix || !suffix_len ||
        !turn_pcm_chunk_inputs_valid(
            request_id, request_id_len, audio, audio_len, sample_rate,
            channels, bit_depth, sequence, segment_index, is_final, prepared))
        return 0u;
    prefix_size = encode_turn_pcm_chunk_prefix(
        prefix, prefix_cap, request_id, request_id_len, audio_len);
    if (prefix_size == (size_t)-1) return 0u;
    suffix_size = encode_turn_pcm_chunk_suffix(
        suffix, suffix_cap, audio_len, sample_rate, channels, bit_depth,
        sequence, segment_index, is_final, prepared);
    if (suffix_size == (size_t)-1 || prefix_size > SIZE_MAX - audio_len ||
        prefix_size + audio_len > SIZE_MAX - suffix_size) return 0u;
    *prefix_len = prefix_size;
    *suffix_len = suffix_size;
    return prefix_size + audio_len + suffix_size;
}

int pb_prepare_turn_pcm_chunk_current_wire(
    turn_pcm_chunk_current_wire_c *wire,
    const char *request_id,
    size_t request_id_len,
    size_t audio_len,
    int32_t segment_index
) {
    size_t pos = 0u;
    uint32_t value;
    if (!wire) return -1;
    memset(wire, 0, sizeof(*wire));
    if (!request_id || request_id_len == 0u ||
        request_id_len >= UINT64_C(0x80) || audio_len == 0u ||
        audio_len > 960u || segment_index < 0) return -1;
    wire->prefix[pos++] = 0x0au;
    wire->prefix[pos++] = (uint8_t)request_id_len;
    memcpy(wire->prefix + pos, request_id, request_id_len);
    pos += request_id_len;
    wire->prefix[pos++] = 0x18u;
    wire->prefix[pos++] = 0x08u;
    wire->prefix[pos++] = 0x32u;
    if (audio_len < UINT64_C(0x80)) {
        wire->prefix[pos++] = (uint8_t)audio_len;
    } else {
        wire->prefix[pos++] =
            (uint8_t)((audio_len & UINT64_C(0x7f)) | UINT64_C(0x80));
        wire->prefix[pos++] = (uint8_t)(audio_len >> 7u);
    }
    wire->prefix_len = (uint16_t)pos;
    wire->audio_len = (uint16_t)audio_len;
    if (segment_index > 0) {
        pos = 0u;
        wire->segment[pos++] = 0x58u;
        value = (uint32_t)segment_index;
        while (value >= UINT32_C(0x80)) {
            wire->segment[pos++] =
                (uint8_t)((value & UINT32_C(0x7f)) | UINT32_C(0x80));
            value >>= 7u;
        }
        wire->segment[pos++] = (uint8_t)value;
        wire->segment_len = (uint8_t)pos;
    }
    return 0;
}

static PB_ALWAYS_INLINE size_t encode_turn_pcm_chunk_current_suffix(
    const turn_pcm_chunk_current_wire_c *wire,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    int32_t sequence,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    size_t required = sizeof(turn_current_pcm_format) + 2u;
    size_t pos;
    if (sequence > 0) required += sequence < INT32_C(0x80) ? 2u : 3u;
    required += wire->segment_len;
    if (is_final) required += 2u;
    if (prepared) required += prepared->len;
    if (required > suffix_cap) return 0u;

    memcpy(suffix, turn_current_pcm_format, sizeof(turn_current_pcm_format));
    pos = sizeof(turn_current_pcm_format);
    if (sequence > 0) {
        suffix[pos++] = 0x50u;
        if (sequence < INT32_C(0x80)) {
            suffix[pos++] = (uint8_t)sequence;
        } else {
            suffix[pos++] = (uint8_t)((uint32_t)sequence | UINT32_C(0x80));
            suffix[pos++] = (uint8_t)((uint32_t)sequence >> 7u);
        }
    }
    if (wire->segment_len != 0u) {
        memcpy(suffix + pos, wire->segment, wire->segment_len);
        pos += wire->segment_len;
    }
    if (is_final) {
        suffix[pos++] = 0x60u;
        suffix[pos++] = 0x01u;
    }
    suffix[pos++] = 0x78u;
    suffix[pos++] = 0x01u;
    if (prepared) {
        memcpy(suffix + pos, prepared->data, prepared->len);
        pos += prepared->len;
    }
    *suffix_len = pos;
    return (size_t)wire->prefix_len + (size_t)wire->audio_len + pos;
}

size_t pb_encode_turn_pcm_chunk_current_suffix_prepared(
    const turn_pcm_chunk_current_wire_c *wire,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    int32_t sequence,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    if (suffix_len) *suffix_len = 0u;
    if (!wire || !suffix || !suffix_len || wire->prefix_len == 0u ||
        wire->prefix_len > sizeof(wire->prefix) || wire->audio_len == 0u ||
        wire->audio_len > 960u || wire->segment_len > sizeof(wire->segment) ||
        sequence < 0 || sequence >= INT32_C(0x4000) ||
        (prepared && !turn_stage_wire_valid(prepared))) return 0u;
    return encode_turn_pcm_chunk_current_suffix(
        wire, suffix, suffix_cap, suffix_len, sequence, is_final, prepared);
}

size_t pb_encode_turn_pcm_chunk_current_suffix_admitted(
    const turn_pcm_chunk_current_wire_c *wire,
    uint8_t *suffix,
    size_t suffix_cap,
    size_t *suffix_len,
    int32_t sequence,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    if (suffix_len) *suffix_len = 0u;
    if (!wire || !suffix || !suffix_len ||
        wire->segment_len > sizeof(wire->segment) ||
        (prepared &&
         (prepared->len == 0u || prepared->len > sizeof(prepared->data))))
        return 0u;
    return encode_turn_pcm_chunk_current_suffix(
        wire, suffix, suffix_cap, suffix_len, sequence, is_final, prepared);
}

size_t pb_encode_turn_text_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *legacy_text,
    const char *speech_text,
    const char *display_text,
    int32_t segment_index,
    int is_final
) {
    int type_id = 0;
    if (type && strcmp(type, "text_delta") == 0) type_id = 4;
    else if (type && strcmp(type, "text_completed") == 0) type_id = 5;
    if (type_id != 0) {
        size_t request_id_len = request_id ? strlen(request_id) : 0u;
        size_t legacy_len = legacy_text ? strlen(legacy_text) : 0u;
        size_t speech_len = speech_text ?
            (speech_text == legacy_text ? legacy_len : strlen(speech_text)) : 0u;
        size_t display_len = display_text ?
            (display_text == legacy_text ? legacy_len :
             display_text == speech_text ? speech_len : strlen(display_text)) : 0u;
        return pb_encode_turn_text_event_prepared(
            out, out_cap, request_id, request_id_len, type_id,
            legacy_text, legacy_len, speech_text, speech_len,
            display_text, display_len, segment_index, is_final);
    }
    return encode_turn_event_fields(
        out, out_cap, request_id, type, legacy_text, NULL, 0, 0, 0, 0, 0,
        segment_index, is_final, speech_text, display_text, NULL, NULL);
}

size_t pb_encode_turn_audio_event(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, audio, audio_len,
        sample_rate, channels, bit_depth, sequence, segment_index, is_final,
        NULL, NULL, NULL, NULL);
}

size_t pb_encode_turn_audio_event_stages(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_timestamps_c *stages
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, audio, audio_len,
        sample_rate, channels, bit_depth, sequence, segment_index, is_final,
        NULL, NULL, stages, NULL);
}

size_t pb_encode_turn_audio_event_stage_wire(
    uint8_t *out,
    size_t out_cap,
    const char *request_id,
    const char *type,
    const char *text,
    const uint8_t *audio,
    size_t audio_len,
    int32_t sample_rate,
    int32_t channels,
    int32_t bit_depth,
    int32_t sequence,
    int32_t segment_index,
    int is_final,
    const turn_stage_wire_c *prepared
) {
    return encode_turn_event_fields(
        out, out_cap, request_id, type, text, audio, audio_len,
        sample_rate, channels, bit_depth, sequence, segment_index, is_final,
        NULL, NULL, NULL, prepared);
}

/* Explicit byte assembly keeps the decimal lanes independent of host byte order. */
static PB_ALWAYS_INLINE uint32_t load_le_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
        (uint32_t)bytes[1] << 8u |
        (uint32_t)bytes[2] << 16u |
        (uint32_t)bytes[3] << 24u;
}

static PB_ALWAYS_INLINE uint64_t load_le_u64(const uint8_t *bytes) {
    return (uint64_t)bytes[0] |
        (uint64_t)bytes[1] << 8u |
        (uint64_t)bytes[2] << 16u |
        (uint64_t)bytes[3] << 24u |
        (uint64_t)bytes[4] << 32u |
        (uint64_t)bytes[5] << 40u |
        (uint64_t)bytes[6] << 48u |
        (uint64_t)bytes[7] << 56u;
}

static PB_ALWAYS_INLINE int parse_four_decimal_digits(
    const uint8_t *text,
    uint32_t *out
) {
    uint32_t word = load_le_u32(text);
    uint32_t digits;
    uint32_t pairs;
    /* Adding six sets bit four only for low nibbles outside decimal 0..9. */
    if ((word & UINT32_C(0xf0f0f0f0)) != UINT32_C(0x30303030)) return -1;
    digits = word & UINT32_C(0x0f0f0f0f);
    if (((digits + UINT32_C(0x06060606)) & UINT32_C(0x10101010)) != 0u)
        return -1;
    pairs = (digits & UINT32_C(0x000f000f)) * 10u +
        ((digits >> 8u) & UINT32_C(0x000f000f));
    *out = (pairs & UINT32_C(0x000000ff)) * 100u +
        ((pairs >> 16u) & UINT32_C(0x000000ff));
    return 0;
}

static PB_ALWAYS_INLINE int parse_eight_decimal_digits(
    const uint8_t *text,
    uint32_t *out
) {
    uint64_t word = load_le_u64(text);
    uint64_t digits;
    uint64_t pairs;
    uint64_t quads;
    /* Keep arithmetic inside byte, pair, and four-digit lanes. */
    if ((word & UINT64_C(0xf0f0f0f0f0f0f0f0)) !=
        UINT64_C(0x3030303030303030)) return -1;
    digits = word & UINT64_C(0x0f0f0f0f0f0f0f0f);
    if (((digits + UINT64_C(0x0606060606060606)) &
         UINT64_C(0x1010101010101010)) != 0u)
        return -1;
    pairs = (digits & UINT64_C(0x000f000f000f000f)) * 10u +
        ((digits >> 8u) & UINT64_C(0x000f000f000f000f));
    quads = (pairs & UINT64_C(0x000000ff000000ff)) * 100u +
        ((pairs >> 16u) & UINT64_C(0x000000ff000000ff));
    *out = (uint32_t)(
        (quads & UINT64_C(0x00000000ffffffff)) * UINT64_C(10000) +
        (quads >> 32u));
    return 0;
}

static PB_ALWAYS_INLINE int parse_stage_timestamp(
    const uint8_t *text,
    size_t text_len,
    int64_t *out
) {
    uint64_t value = 0;
    size_t i;
    if (!text || text_len == 0u || !out) return -1;
    if (text_len == 13u) {
        uint32_t first_four;
        uint32_t last_eight;
        unsigned fifth = (unsigned)(text[4] - (uint8_t)'0');
        if (fifth > 9u ||
            parse_four_decimal_digits(text, &first_four) != 0 ||
            parse_eight_decimal_digits(text + 5u, &last_eight) != 0)
            return -1;
        value = ((uint64_t)first_four * 10u + fifth) * UINT64_C(100000000) +
            last_eight;
        if (value == 0u) return -1;
        *out = (int64_t)value;
        return 0;
    }
    if (text_len <= 18u) {
        i = 0;
        if ((text_len & 1u) != 0u) {
            if (text[0] < (uint8_t)'0' || text[0] > (uint8_t)'9') return -1;
            value = text[0] - (uint8_t)'0';
            i = 1u;
        }
        for (; i < text_len; i += 2u) {
            unsigned high;
            unsigned low;
            if (text[i] < (uint8_t)'0' || text[i] > (uint8_t)'9' ||
                text[i + 1u] < (uint8_t)'0' ||
                text[i + 1u] > (uint8_t)'9') return -1;
            high = (unsigned)(text[i] - (uint8_t)'0');
            low = (unsigned)(text[i + 1u] - (uint8_t)'0');
            value = value * UINT64_C(100) + high * 10u + low;
        }
        if (value == 0u) return -1;
        *out = (int64_t)value;
        return 0;
    }
    for (i = 0; i < text_len; ++i) {
        unsigned digit;
        if (text[i] < (uint8_t)'0' || text[i] > (uint8_t)'9') return -1;
        digit = (unsigned)(text[i] - (uint8_t)'0');
        if (value > ((uint64_t)INT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    if (value == 0) return -1;
    *out = (int64_t)value;
    return 0;
}

typedef struct {
    uint64_t word;
    uint64_t value;
} stage_timestamp_prefix;

static PB_ALWAYS_INLINE int parse_stage_timestamp_with_prefix(
    const uint8_t *text,
    size_t text_len,
    int64_t *out,
    stage_timestamp_prefix *prefix
) {
    if (prefix->word != 0u && text_len == 13u &&
        (load_le_u64(text) & UINT64_C(0x00ffffffffffffff)) == prefix->word) {
        uint32_t first_four;
        unsigned fifth = (unsigned)(text[11] - (uint8_t)'0');
        unsigned sixth = (unsigned)(text[12] - (uint8_t)'0');
        if (fifth > 9u || sixth > 9u ||
            parse_four_decimal_digits(text + 7u, &first_four) != 0)
            return -1;
        *out = (int64_t)(
            prefix->value * UINT64_C(1000000) +
            (uint64_t)first_four * UINT64_C(100) + fifth * 10u + sixth);
        return 0;
    }
    if (parse_stage_timestamp(text, text_len, out) != 0) return -1;
    if (text_len == 13u) {
        prefix->word =
            load_le_u64(text) & UINT64_C(0x00ffffffffffffff);
        prefix->value = (uint64_t)*out / UINT64_C(1000000);
    }
    return 0;
}

static PB_ALWAYS_INLINE int64_t *stage_timestamp_field_trusted(
    turn_stage_timestamps_c *stages,
    const uint8_t *key,
    size_t key_len
) {
    int64_t *field = NULL;
#define MATCH_STAGE_KEY(literal, member) do { \
        if (memcmp(key, (literal), sizeof(literal) - 1u) == 0) \
            field = &stages->member; \
    } while (0)
    switch (key_len) {
    case 22:
        MATCH_STAGE_KEY("stage_first_text_at_ms", first_text_at_ms);
        break;
    case 23:
        MATCH_STAGE_KEY("stage_pcm_started_at_ms", pcm_started_at_ms);
        break;
    case 27:
        if (key[6] == 'a')
            MATCH_STAGE_KEY("stage_audio_committed_at_ms", input.audio_committed_at_ms);
        else if (key[6] == 'p')
            MATCH_STAGE_KEY("stage_pcm_first_chunk_at_ms", pcm_first_chunk_at_ms);
        break;
    case 30:
        if (key[6] == 's')
            MATCH_STAGE_KEY(
                "stage_stt_provider_ready_at_ms", input.stt_provider_ready_at_ms);
        else if (key[6] == 't')
            MATCH_STAGE_KEY("stage_tts_provider_ready_at_ms", tts_provider_ready_at_ms);
        break;
    case 31:
        MATCH_STAGE_KEY("stage_tts_segment_emitted_at_ms", tts_segment_emitted_at_ms);
        break;
    case 32:
        if (key[6] == 's')
            MATCH_STAGE_KEY(
                "stage_stt_request_received_at_ms", input.stt_request_received_at_ms);
        else if (key[6] == 't')
            MATCH_STAGE_KEY("stage_tts_request_received_at_ms", tts_request_received_at_ms);
        break;
    case 36:
        MATCH_STAGE_KEY(
            "stage_stt_transcript_published_at_ms",
            input.stt_transcript_published_at_ms);
        break;
    case 40:
        if (key[6] == 's')
            MATCH_STAGE_KEY(
                "stage_stt_provider_request_started_at_ms",
                input.stt_provider_request_started_at_ms);
        else if (key[6] == 't')
            MATCH_STAGE_KEY(
                "stage_tts_provider_request_started_at_ms",
                tts_provider_request_started_at_ms);
        break;
    default:
        break;
    }
#undef MATCH_STAGE_KEY
    return field;
}

/* Return -1 for invalid, zero for unknown, or one for decoded. */
static int decode_stage_timestamp_trusted(
    turn_stage_timestamps_c *stages,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len
) {
    int64_t *field = stage_timestamp_field_trusted(stages, key, key_len);
    if (!field) return 0;
    if (*field != 0 || parse_stage_timestamp(value, value_len, field) != 0)
        return -1;
    return 1;
}

/* Return -1 for invalid, zero for unknown, or one for decoded. */
static int decode_stage_timestamp_prefixed(
    turn_stage_timestamps_c *stages,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len,
    stage_timestamp_prefix *prefix
) {
    int64_t *field = stage_timestamp_field_trusted(stages, key, key_len);
    if (!field) return 0;
    if (*field != 0 ||
        parse_stage_timestamp_with_prefix(
            value, value_len, field, prefix) != 0) return -1;
    return 1;
}

static int decode_stage_timestamp(
    turn_stage_timestamps_c *stages,
    const uint8_t *key,
    size_t key_len,
    const uint8_t *value,
    size_t value_len
) {
    if (!stages || (!key && key_len != 0u) || (!value && value_len != 0u))
        return -1;
    return decode_stage_timestamp_trusted(
        stages, key, key_len, value, value_len);
}

/* Return 1 for a decoded canonical entry, 0 for the generic parser, or -1. */
static PB_ALWAYS_INLINE int decode_stage_map_entry_direct(
    turn_stage_timestamps_c *stages,
    const uint8_t *entry,
    size_t entry_len,
    stage_timestamp_prefix *prefix
) {
    if (!stages || (!entry && entry_len != 0u)) return -1;
    /* The canonical writer uses one-byte lengths for bounded stage keys and values. */
    if (entry_len >= 4u && entry[0] == 0x0au) {
        size_t direct_key_len = entry[1];
        size_t value_tag = 2u + direct_key_len;
        if (direct_key_len < 64u && value_tag + 2u <= entry_len &&
            entry[value_tag] == 0x12u) {
            size_t direct_value_len = entry[value_tag + 1u];
            const uint8_t *direct_key = entry + 2u;
            const uint8_t *direct_value = entry + value_tag + 2u;
            if (direct_value_len < 64u &&
                direct_value_len == entry_len - value_tag - 2u) {
                /* The entry bounds above establish all pointer invariants. */
                int decoded;
                if (prefix)
                    decoded = decode_stage_timestamp_prefixed(
                        stages, direct_key, direct_key_len,
                        direct_value, direct_value_len, prefix);
                else
                    decoded = decode_stage_timestamp_trusted(
                        stages, direct_key, direct_key_len,
                        direct_value, direct_value_len);
                if (decoded < 0) return -1;
                if (decoded == 0 &&
                    (memchr(direct_key, '\0', direct_key_len) != NULL ||
                     memchr(direct_value, '\0', direct_value_len) != NULL))
                    return -1;
                return 1;
            }
        }
    }
    return 0;
}

static PB_NOINLINE int decode_stage_map_entry(
    turn_stage_timestamps_c *stages,
    const uint8_t *entry,
    size_t entry_len
) {
    pb_reader reader;
    const uint8_t *key = NULL;
    const uint8_t *value = NULL;
    size_t key_len = 0;
    size_t value_len = 0;
    int direct = decode_stage_map_entry_direct(stages, entry, entry_len, NULL);
    if (direct != 0) return direct < 0 ? -1 : 0;
    pb_reader_init(&reader, entry, entry_len);
    while (reader.pos < reader.len) {
        const uint8_t *bytes;
        size_t bytes_len;
        uint32_t field;
        uint32_t wire;
        if (pb_read_tag(&reader, &field, &wire) != 0) return -1;
        if (wire != 2u) {
            if (pb_skip(&reader, wire) != 0) return -1;
            continue;
        }
        if (pb_read_bytes(&reader, &bytes, &bytes_len) != 0 ||
            bytes_len >= 256u || memchr(bytes, '\0', bytes_len) != NULL)
            return -1;
        if (field == 1u) {
            key = bytes;
            key_len = bytes_len < 64u ? bytes_len : 63u;
        } else if (field == 2u) {
            value = bytes;
            value_len = bytes_len < 64u ? bytes_len : 63u;
        }
    }
    {
        int decoded = decode_stage_timestamp(
            stages, key, key_len, value, value_len);
        return decoded < 0 ? -1 : 0;
    }
}

static int tool_hash_valid(const char *hash) {
    size_t i;
    for (i = 0; i < 64u; ++i)
        if (!((hash[i] >= '0' && hash[i] <= '9') ||
              (hash[i] >= 'a' && hash[i] <= 'f'))) return 0;
    return hash[64] == '\0';
}

int pb_turn_tool_result_valid(const turn_tool_result_c *tool) {
    return tool && tool->present == 1 && tool->elapsed_ms >= 0 && tool->elapsed_ms <= 60000 &&
        ((memcmp(tool->tool_id, "dnd-dice-roll", sizeof("dnd-dice-roll")) == 0 &&
          memcmp(tool->tool_call_id, "dice-", 5u) == 0 && tool_hash_valid(tool->tool_call_id + 5u)) ||
         (memcmp(tool->tool_id, "dnd-encounter-state", sizeof("dnd-encounter-state")) == 0 &&
          memcmp(tool->tool_call_id, "encounter-", 10u) == 0 && tool_hash_valid(tool->tool_call_id + 10u)) ||
         (memcmp(tool->tool_id, "dnd-campaign-state", sizeof("dnd-campaign-state")) == 0 &&
          memcmp(tool->tool_call_id, "campaign-", 9u) == 0 && tool_hash_valid(tool->tool_call_id + 9u)) ||
         (memcmp(tool->tool_id, "dnd-scene-presence", sizeof("dnd-scene-presence")) == 0 &&
          memcmp(tool->tool_call_id, "scene-", 6u) == 0 && tool_hash_valid(tool->tool_call_id + 6u))) &&
        tool_hash_valid(tool->output_sha256);
}

static int encounter_id_valid(const char *id) {
    size_t length = strlen(id);
    if (!length || length > 64u) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (i && (c == '.' || c == '_' || c == '-')))) return 0;
    }
    return 1;
}

static int encounter_participant_decode(const uint8_t *data, size_t length,
                                        turn_encounter_participant_c *out) {
    pb_reader reader;
    unsigned seen = 0;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        if (pb_read_tag(&reader, &field, &wire) != 0 || field < 1u || field > 6u ||
            (field != 6u && (seen & (1u << field)))) return -1;
        seen |= 1u << field;
        if (field >= 3u && field <= 5u) {
            uint64_t value;
            if (wire != 0u || pb_read_varint(&reader, &value) != 0 || value > UINT32_MAX) return -1;
            if (field == 3u) {
                if (value > 200u) return -1;
                out->initiative = (int32_t)(value >> 1u) * ((value & 1u) ? -1 : 1) - (int32_t)(value & 1u);
            } else {
                if (value > 100000u) return -1;
                if (field == 4u) out->max_hp = (int32_t)value;
                else out->current_hp = (int32_t)value;
            }
        } else {
            char *target;
            size_t capacity;
            if (field == 6u && out->condition_count >= 16u) return -1;
            target = field == 1u ? out->id : field == 2u ? out->name : out->conditions[out->condition_count++];
            capacity = field == 1u ? sizeof(out->id) : field == 2u ? sizeof(out->name) : sizeof(out->conditions[0]);
            if (wire != 2u || pb_read_string(&reader, target, capacity) != 0) return -1;
        }
    }
    if (!encounter_id_valid(out->id) || !out->name[0] || out->name[0] == ' ' ||
        out->name[strlen(out->name) - 1u] == ' ' || out->current_hp > out->max_hp) return -1;
    for (size_t i = 0; i < out->condition_count; ++i) {
        if (i && strcmp(out->conditions[i - 1u], out->conditions[i]) >= 0) return -1;
        if (!dnd_condition_number(out->conditions[i])) return -1;
    }
    return 0;
}

int pb_encounter_participant_next(pb_reader *reader, turn_encounter_participant_c *out) {
    if (!reader || !out || reader->pos > reader->len) return -1;
    while (reader->pos < reader->len) {
        uint32_t field, wire;
        if (pb_read_tag(reader, &field, &wire) != 0) return -1;
        if (field == 8u) {
            const uint8_t *data;
            size_t length;
            if (wire != 2u || pb_read_bytes(reader, &data, &length) != 0 ||
                encounter_participant_decode(data, length, out) != 0) return -1;
            return 1;
        }
        if (pb_skip(reader, wire) != 0) return -1;
    }
    return 0;
}

int pb_decode_turn_encounter(const uint8_t *data, size_t length, turn_encounter_state_c *out) {
    pb_reader reader;
    unsigned seen = 0;
    char ids[DND_TURN_PARTICIPANTS_MAX][65];
    char active[65] = "";
    int32_t previous_initiative = 101;
    if (!out || !data || !length || length > DND_TURN_ENCOUNTER_MAX) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        if (pb_read_tag(&reader, &field, &wire) != 0 || field < 1u || field > 11u ||
            (field != 8u && (seen & (1u << field)))) return -1;
        seen |= 1u << field;
        if (field == 3u || field == 5u || field == 6u) {
            uint64_t value;
            if (wire != 0u || pb_read_varint(&reader, &value) != 0) return -1;
            if (field == 3u) {
                /* JavaScript's existing version field must remain exact. */
                if (!value || value > UINT64_C(9007199254740991)) return -1;
                out->version = (int64_t)value;
            } else if (field == 5u) {
                if (!value || value > INT32_MAX) return -1;
                out->round = (int32_t)value;
            } else {
                if (value > 2u * (DND_TURN_PARTICIPANTS_MAX - 1u) || ((value & 1u) && value != 1u)) return -1;
                out->active_index = value == 1u ? -1 : (int32_t)(value >> 1u);
            }
        } else if (field == 8u) {
            const uint8_t *part;
            size_t part_length;
            turn_encounter_participant_c participant;
            if (out->participant_count >= DND_TURN_PARTICIPANTS_MAX || wire != 2u ||
                pb_read_bytes(&reader, &part, &part_length) != 0 ||
                encounter_participant_decode(part, part_length, &participant) != 0) return -1;
            if (previous_initiative < participant.initiative ||
                (out->participant_count && previous_initiative == participant.initiative &&
                 strcmp(ids[out->participant_count - 1u], participant.id) >= 0)) return -1;
            for (size_t i = 0; i < out->participant_count; ++i)
                if (!strcmp(ids[i], participant.id)) return -1;
            memcpy(ids[out->participant_count++], participant.id, sizeof(participant.id));
            previous_initiative = participant.initiative;
        } else {
            char *target;
            size_t capacity;
            switch (field) {
            case 1u: target = out->campaign_id; capacity = sizeof(out->campaign_id); break;
            case 2u: target = out->encounter_id; capacity = sizeof(out->encounter_id); break;
            case 4u: target = out->status; capacity = sizeof(out->status); break;
            case 7u: target = out->active_participant_id; capacity = sizeof(out->active_participant_id); break;
            case 9u: target = out->tool_call_id; capacity = sizeof(out->tool_call_id); break;
            case 10u: target = out->output_sha256; capacity = sizeof(out->output_sha256); break;
            default: target = out->operation; capacity = sizeof(out->operation); break;
            }
            if (wire != 2u || pb_read_string(&reader, target, capacity) != 0) return -1;
        }
    }
    if (!encounter_id_valid(out->campaign_id) || !encounter_id_valid(out->encounter_id) ||
        out->version < 1 || out->round < 1 ||
        (strcmp(out->operation, "get") && strcmp(out->operation, "roll_initiative") &&
         strcmp(out->operation, "advance") && strcmp(out->operation, "damage") && strcmp(out->operation, "heal") &&
         strcmp(out->operation, "condition_add") && strcmp(out->operation, "condition_remove") && strcmp(out->operation, "end")) ||
        (strcmp(out->status, "active") && strcmp(out->status, "ended")) ||
        out->active_index >= (int32_t)out->participant_count ||
        (!strcmp(out->status, "ended") && out->active_index != -1) ||
        memcmp(out->tool_call_id, "encounter-", 10u) || !tool_hash_valid(out->tool_call_id + 10u) ||
        !tool_hash_valid(out->output_sha256)) return -1;
    if (out->active_index >= 0) memcpy(active, ids[out->active_index], sizeof(active));
    if (strcmp(active, out->active_participant_id)) return -1;
    out->wire = (turn_encounter_c){data, length};
    return 0;
}

size_t pb_encode_turn_encounter(uint8_t *out, size_t capacity, const turn_encounter_state_c *state,
                                const turn_encounter_participant_c *participants) {
    size_t used = 0;
    turn_encounter_state_c checked;
    if (!out || !state || state->participant_count > DND_TURN_PARTICIPANTS_MAX ||
        state->active_index < -1 || state->active_index >= (int32_t)state->participant_count ||
        (state->participant_count && !participants)) return 0;
#define ENCOUNTER_STRING(field, value) do { \
    if (!memchr(value, '\0', sizeof(value))) return 0; \
    used = write_string_field(out, capacity, used, field, value); \
    if (used == (size_t)-1) return 0; \
} while (0)
#define ENCOUNTER_NUMBER(field, value) do { \
    used = write_varint_field(out, capacity, used, field, (uint64_t)(value)); \
    if (used == (size_t)-1) return 0; \
} while (0)
    ENCOUNTER_STRING(1u, state->campaign_id);
    ENCOUNTER_STRING(2u, state->encounter_id);
    ENCOUNTER_NUMBER(3u, state->version);
    ENCOUNTER_STRING(4u, state->status);
    ENCOUNTER_NUMBER(5u, state->round);
    ENCOUNTER_NUMBER(6u, state->active_index < 0 ? 1u : (uint64_t)(uint32_t)state->active_index * 2u);
    ENCOUNTER_STRING(7u, state->active_participant_id);
    ENCOUNTER_STRING(9u, state->tool_call_id);
    ENCOUNTER_STRING(10u, state->output_sha256);
    ENCOUNTER_STRING(11u, state->operation);
#undef ENCOUNTER_STRING
#undef ENCOUNTER_NUMBER
    for (size_t i = 0; i < state->participant_count; ++i) {
        const turn_encounter_participant_c *p = &participants[i];
        uint8_t nested[1024];
        size_t n = 0;
        if (p->condition_count > 16u || p->initiative < -100 || p->initiative > 100 ||
            p->max_hp < 0 || p->current_hp < 0 || !memchr(p->id, '\0', sizeof(p->id)) ||
            !memchr(p->name, '\0', sizeof(p->name))) return 0;
#define PARTICIPANT_PUT(call) do { n = (call); if (n == (size_t)-1) return 0; } while (0)
        PARTICIPANT_PUT(write_string_field(nested, sizeof(nested), n, 1u, p->id));
        PARTICIPANT_PUT(write_string_field(nested, sizeof(nested), n, 2u, p->name));
        PARTICIPANT_PUT(write_varint_field(nested, sizeof(nested), n, 3u,
            (uint64_t)(p->initiative < 0 ? -2 * p->initiative - 1 : 2 * p->initiative)));
        PARTICIPANT_PUT(write_varint_field(nested, sizeof(nested), n, 4u, (uint64_t)p->max_hp));
        PARTICIPANT_PUT(write_varint_field(nested, sizeof(nested), n, 5u, (uint64_t)p->current_hp));
        for (size_t j = 0; j < p->condition_count; ++j) {
            if (!memchr(p->conditions[j], '\0', sizeof(p->conditions[j]))) return 0;
            PARTICIPANT_PUT(write_string_field(nested, sizeof(nested), n, 6u, p->conditions[j]));
        }
#undef PARTICIPANT_PUT
        used = write_bytes_field(out, capacity, used, 8u, nested, n);
        if (used == (size_t)-1) return 0;
    }
    return pb_decode_turn_encounter(out, used, &checked) == 0 ? used : 0;
}

size_t pb_append_turn_encounter(uint8_t *out, size_t capacity, size_t used,
                               const turn_encounter_c *encounter) {
    turn_encounter_state_c checked;
    if (!out || !used || used > capacity || !encounter ||
        pb_decode_turn_encounter(encounter->data, encounter->length, &checked) != 0) return 0;
    used = write_bytes_field(out, capacity, used, 19u, encounter->data, encounter->length);
    return used == (size_t)-1 ? 0 : used;
}

static int roster_character_decode(const uint8_t *data, size_t length, turn_campaign_character_c *out) {
    pb_reader reader;
    unsigned seen = 0;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        uint64_t value;
        if (pb_read_tag(&reader, &field, &wire) || field < 1u || field > 4u || (seen & (1u << field))) return -1;
        seen |= 1u << field;
        if (field == 4u) {
            if (wire != 0u || pb_read_varint(&reader, &value) || value > 100000u) return -1;
            out->max_hp = (int32_t)value;
        } else {
            char *target = field == 1u ? out->id : field == 2u ? out->name : out->kind;
            size_t cap = field == 1u ? sizeof(out->id) : field == 2u ? sizeof(out->name) : sizeof(out->kind);
            if (wire != 2u || pb_read_string(&reader, target, cap)) return -1;
        }
    }
    return encounter_id_valid(out->id) && out->name[0] &&
        (!strcmp(out->kind, "player") || !strcmp(out->kind, "npc")) ? 0 : -1;
}

int pb_roster_character_next(pb_reader *reader, turn_campaign_character_c *out) {
    if (!reader || !out) return -1;
    while (reader->pos < reader->len) {
        uint32_t field, wire;
        const uint8_t *data;
        size_t length;
        if (pb_read_tag(reader, &field, &wire)) return -1;
        if (field == 4u) {
            if (wire != 2u || pb_read_bytes(reader, &data, &length) || roster_character_decode(data, length, out)) return -1;
            return 1;
        }
        if (pb_skip(reader, wire)) return -1;
    }
    return 0;
}

int pb_decode_turn_roster(const uint8_t *data, size_t length, turn_campaign_roster_c *out) {
    pb_reader reader;
    char ids[DND_TURN_CHARACTERS_MAX][65];
    unsigned seen = 0;
    if (!data || !out || !length || length > DND_TURN_ROSTER_MAX) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        uint64_t value;
        if (pb_read_tag(&reader, &field, &wire) || field < 1u || field > 6u ||
            (field != 4u && (seen & (1u << field)))) return -1;
        seen |= 1u << field;
        if (field == 2u) {
            if (wire != 0u || pb_read_varint(&reader, &value) || value > (uint64_t)DND_EXACT_VERSION_MAX) return -1;
            out->version = (int64_t)value;
        } else if (field == 4u) {
            const uint8_t *part;
            size_t n;
            turn_campaign_character_c character;
            if (wire != 2u || out->character_count >= DND_TURN_CHARACTERS_MAX ||
                pb_read_bytes(&reader, &part, &n) || roster_character_decode(part, n, &character)) return -1;
            for (size_t i = 0; i < out->character_count; ++i)
                if (!strcmp(ids[i], character.id)) return -1;
            memcpy(ids[out->character_count++], character.id, sizeof(character.id));
        } else {
            char *target = field == 1u ? out->campaign_id : field == 3u ? out->status :
                field == 5u ? out->tool_call_id : out->output_sha256;
            size_t cap = field == 1u ? sizeof(out->campaign_id) : field == 3u ? sizeof(out->status) :
                field == 5u ? sizeof(out->tool_call_id) : sizeof(out->output_sha256);
            if (wire != 2u || pb_read_string(&reader, target, cap)) return -1;
        }
    }
    if (!encounter_id_valid(out->campaign_id) || out->version < 1 ||
        (strcmp(out->status, "active") && strcmp(out->status, "archived")) ||
        memcmp(out->tool_call_id, "campaign-", 9u) || !tool_hash_valid(out->tool_call_id + 9u) ||
        !tool_hash_valid(out->output_sha256)) return -1;
    out->wire = (turn_roster_c){data, length};
    return 0;
}

size_t pb_encode_turn_roster(uint8_t *out, size_t capacity, const turn_campaign_roster_c *roster,
                              const turn_campaign_character_c *characters) {
    size_t used = 0;
    turn_campaign_roster_c checked;
    if (!out || !roster || !dnd_request_id_valid(roster->campaign_id, sizeof(roster->campaign_id)) ||
        !memchr(roster->status, 0, sizeof(roster->status)) ||
        !memchr(roster->tool_call_id, 0, sizeof(roster->tool_call_id)) ||
        !tool_hash_valid(roster->output_sha256) || roster->character_count > DND_TURN_CHARACTERS_MAX ||
        (roster->character_count && !characters)) return 0;
#define ROSTER_STRING(field, value) do { used = write_string_field(out, capacity, used, field, value); if (used == (size_t)-1) return 0; } while (0)
    ROSTER_STRING(1u, roster->campaign_id);
    used = write_varint_field(out, capacity, used, 2u, (uint64_t)roster->version);
    if (used == (size_t)-1) return 0;
    ROSTER_STRING(3u, roster->status);
    for (size_t i = 0; i < roster->character_count; ++i) {
        uint8_t nested[320];
        const turn_campaign_character_c *c = &characters[i];
        if (!dnd_request_id_valid(c->id, sizeof(c->id)) ||
            !memchr(c->name, 0, sizeof(c->name)) || !memchr(c->kind, 0, sizeof(c->kind))) return 0;
        size_t n = write_string_field(nested, sizeof(nested), 0, 1u, c->id);
        if (n != (size_t)-1) n = write_string_field(nested, sizeof(nested), n, 2u, c->name);
        if (n != (size_t)-1) n = write_string_field(nested, sizeof(nested), n, 3u, c->kind);
        if (n != (size_t)-1) n = write_varint_field(nested, sizeof(nested), n, 4u, (uint64_t)c->max_hp);
        if (n == (size_t)-1) return 0;
        used = write_bytes_field(out, capacity, used, 4u, nested, n);
        if (used == (size_t)-1) return 0;
    }
    ROSTER_STRING(5u, roster->tool_call_id);
    ROSTER_STRING(6u, roster->output_sha256);
#undef ROSTER_STRING
    return pb_decode_turn_roster(out, used, &checked) == 0 ? used : 0;
}

size_t pb_append_turn_roster(uint8_t *out, size_t capacity, size_t used, const turn_roster_c *roster) {
    turn_campaign_roster_c checked;
    if (!out || !used || used > capacity || !roster ||
        pb_decode_turn_roster(roster->data, roster->length, &checked)) return 0;
    used = write_bytes_field(out, capacity, used, 22u, roster->data, roster->length);
    return used == (size_t)-1 ? 0 : used;
}

static int initiative_roll_decode(const uint8_t *data, size_t length, turn_initiative_roll_c *out) {
    pb_reader reader;
    unsigned seen = 0;
    size_t kept_count = 0;
    char entropy[16] = "";
    int keep, modifier;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        uint64_t value;
        if (pb_read_tag(&reader, &field, &wire) || field < 1u || field > 6u ||
            ((field != 3u && field != 4u) && (seen & (1u << field)))) return -1;
        seen |= 1u << field;
        if (field == 3u || field == 4u) {
            pb_reader packed;
            const uint8_t *span;
            size_t n;
            if (wire == 2u) {
                if (pb_read_bytes(&reader, &span, &n) || !n) return -1;
                pb_reader_init(&packed, span, n);
            } else if (wire == 0u) packed = reader;
            else return -1;
            do {
                if (pb_read_varint(&packed, &value)) return -1;
                if (field == 3u) {
                    if (out->roll_count >= 2u || value < 1u || value > 20u) return -1;
                    out->rolls[out->roll_count++] = (uint32_t)value;
                } else {
                    if (kept_count++ || value > 1u) return -1;
                    out->kept_index = (uint32_t)value;
                }
                if (wire == 0u) { reader = packed; break; }
            } while (packed.pos < packed.len);
        } else if (field == 5u) {
            if (wire != 0u || pb_read_varint(&reader, &value) || value > UINT32_MAX) return -1;
            out->total = (int32_t)(value >> 1u) ^ -(int32_t)(value & 1u);
        } else {
            char *target = field == 1u ? out->character_id : field == 2u ? out->expression : entropy;
            size_t cap = field == 1u ? sizeof(out->character_id) : field == 2u ? sizeof(out->expression) : sizeof(entropy);
            if (wire != 2u || pb_read_string(&reader, target, cap)) return -1;
        }
    }
    if (!encounter_id_valid(out->character_id) || strcmp(entropy, "getrandom") || kept_count != 1u ||
        !dnd_request_expression(out->expression, sizeof(out->expression), &keep, &modifier) ||
        out->roll_count != (keep ? 2u : 1u)) return -1;
    uint32_t selected = keep && (keep > 0 ? out->rolls[1] > out->rolls[0] : out->rolls[1] < out->rolls[0]) ? 1u : 0u;
    return out->kept_index == selected && out->total == (int32_t)out->rolls[selected] + modifier ? 0 : -1;
}

int pb_initiative_roll_next(pb_reader *reader, turn_initiative_roll_c *out) {
    if (!reader || !out || reader->pos > reader->len) return -1;
    while (reader->pos < reader->len) {
        uint32_t field, wire;
        const uint8_t *data;
        size_t length;
        if (pb_read_tag(reader, &field, &wire)) return -1;
        if (field == 3u) {
            if (wire != 2u || pb_read_bytes(reader, &data, &length) || initiative_roll_decode(data, length, out)) return -1;
            return 1;
        }
        if (pb_skip(reader, wire)) return -1;
    }
    return 0;
}

int pb_decode_turn_initiative(const uint8_t *data, size_t length, turn_initiative_result_c *out) {
    pb_reader reader;
    char ids[DND_INITIATIVE_SELECTIONS_MAX][65];
    unsigned seen = 0;
    if (!data || !out || !length || length > DND_TURN_INITIATIVE_MAX) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        uint64_t value;
        if (pb_read_tag(&reader, &field, &wire) || field < 1u || field > 3u ||
            (field != 3u && (seen & (1u << field)))) return -1;
        seen |= 1u << field;
        if (field == 1u) {
            if (wire != 0u || pb_read_varint(&reader, &value) || value > (uint64_t)DND_EXACT_VERSION_MAX) return -1;
            out->campaign_version = (int64_t)value;
        } else if (field == 2u) {
            if (wire != 2u || pb_read_string(&reader, out->campaign_sha256, sizeof(out->campaign_sha256))) return -1;
        } else {
            const uint8_t *part;
            size_t n;
            turn_initiative_roll_c roll;
            if (wire != 2u || out->roll_count >= DND_INITIATIVE_SELECTIONS_MAX ||
                pb_read_bytes(&reader, &part, &n) || initiative_roll_decode(part, n, &roll)) return -1;
            for (size_t i = 0; i < out->roll_count; ++i) if (!strcmp(ids[i], roll.character_id)) return -1;
            memcpy(ids[out->roll_count++], roll.character_id, sizeof(roll.character_id));
        }
    }
    if (!out->roll_count || out->campaign_version < 1 || !tool_hash_valid(out->campaign_sha256)) return -1;
    out->wire = (turn_initiative_c){data, length};
    return 0;
}

size_t pb_encode_turn_initiative(uint8_t *out, size_t capacity, const turn_initiative_result_c *result,
                                  const turn_initiative_roll_c *rolls) {
    turn_initiative_result_c checked;
    size_t used;
    if (!out || !result || !rolls || !tool_hash_valid(result->campaign_sha256) ||
        result->roll_count > DND_INITIATIVE_SELECTIONS_MAX) return 0;
    used = write_varint_field(out, capacity, 0, 1u, (uint64_t)result->campaign_version);
    if (used != (size_t)-1) used = write_string_field(out, capacity, used, 2u, result->campaign_sha256);
    if (used == (size_t)-1) return 0;
    for (size_t i = 0; i < result->roll_count; ++i) {
        uint8_t nested[160], packed[2];
        const turn_initiative_roll_c *r = &rolls[i];
        int keep, modifier;
        if (!dnd_request_id_valid(r->character_id, sizeof(r->character_id)) ||
            !dnd_request_expression(r->expression, sizeof(r->expression), &keep, &modifier) ||
            !r->roll_count || r->roll_count > 2u || r->kept_index > 1u) return 0;
        size_t n = write_string_field(nested, sizeof(nested), 0, 1u, r->character_id);
        if (n != (size_t)-1) n = write_string_field(nested, sizeof(nested), n, 2u, r->expression);
        for (size_t j = 0; j < r->roll_count; ++j) {
            if (r->rolls[j] < 1u || r->rolls[j] > 20u) return 0;
            packed[j] = (uint8_t)r->rolls[j];
        }
        if (n != (size_t)-1) n = write_bytes_field(nested, sizeof(nested), n, 3u, packed, r->roll_count);
        packed[0] = (uint8_t)r->kept_index;
        if (n != (size_t)-1) n = write_bytes_field(nested, sizeof(nested), n, 4u, packed, 1u);
        uint32_t zigzag = ((uint32_t)r->total << 1u) ^ (uint32_t)-(r->total < 0);
        if (n != (size_t)-1) n = write_varint_field(nested, sizeof(nested), n, 5u, zigzag);
        if (n != (size_t)-1) n = write_string_field(nested, sizeof(nested), n, 6u, "getrandom");
        if (n == (size_t)-1) return 0;
        used = write_bytes_field(out, capacity, used, 3u, nested, n);
        if (used == (size_t)-1) return 0;
    }
    return pb_decode_turn_initiative(out, used, &checked) == 0 ? used : 0;
}

size_t pb_append_turn_initiative(uint8_t *out, size_t capacity, size_t used, const turn_initiative_c *initiative) {
    turn_initiative_result_c checked;
    if (!out || !used || used > capacity || !initiative ||
        pb_decode_turn_initiative(initiative->data, initiative->length, &checked)) return 0;
    used = write_bytes_field(out, capacity, used, 23u, initiative->data, initiative->length);
    return used == (size_t)-1 ? 0 : used;
}

size_t pb_append_turn_tool_result(uint8_t *out, size_t capacity, size_t used,
                                  const turn_tool_result_c *tool) {
    uint8_t nested[256];
    size_t length = 0;
    if (!out || !used || used > capacity || !pb_turn_tool_result_valid(tool)) return 0;
    length = write_string_field(nested, sizeof(nested), length, 1u, tool->tool_id);
    if (length == (size_t)-1) return 0;
    length = write_string_field(nested, sizeof(nested), length, 2u, tool->tool_call_id);
    if (length == (size_t)-1) return 0;
    length = write_string_field(nested, sizeof(nested), length, 3u, tool->output_sha256);
    if (length == (size_t)-1) return 0;
    /* Presence matters even when the measured duration rounds to zero. */
    length = write_varint_field(nested, sizeof(nested), length, 4u, (uint64_t)tool->elapsed_ms);
    if (length == (size_t)-1) return 0;
    length = write_bytes_field(out, capacity, used, 20u, nested, length);
    return length == (size_t)-1 ? 0 : length;
}

static int decode_turn_tool_result(const uint8_t *data, size_t length, turn_tool_result_c *tool) {
    pb_reader reader;
    unsigned seen = 0;
    if (tool->present || length > 256u) return -1;
    memset(tool, 0, sizeof(*tool));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        if (pb_read_tag(&reader, &field, &wire) != 0 || field < 1u || field > 4u ||
            (seen & (1u << field)) != 0u) return -1;
        seen |= 1u << field;
        if (field == 4u) {
            uint64_t value;
            if (wire != 0u || pb_read_varint(&reader, &value) != 0 || value > 60000u) return -1;
            tool->elapsed_ms = (int64_t)value;
        } else {
            char *target = field == 1u ? tool->tool_id : field == 2u ? tool->tool_call_id : tool->output_sha256;
            size_t capacity = field == 1u ? sizeof(tool->tool_id) : field == 2u ? sizeof(tool->tool_call_id) : sizeof(tool->output_sha256);
            if (wire != 2u || pb_read_string(&reader, target, capacity) != 0) return -1;
        }
    }
    /* Canonical proto3 writers may omit the zero duration scalar. */
    if ((seen & 14u) != 14u) return -1;
    tool->present = 1;
    return pb_turn_tool_result_valid(tool) ? 0 : -1;
}

int pb_grounding_identity_valid(const dnd_grounding_identity *identity) {
    size_t i;
    if (!identity || memcmp(identity->id, DND_GROUNDING_PROMPT_ID,
            sizeof(DND_GROUNDING_PROMPT_ID)) != 0 || !identity->version[0] ||
        !tool_hash_valid(identity->sha256)) return 0;
    for (i = 0; i < sizeof(identity->version); ++i) {
        unsigned char c = (unsigned char)identity->version[i];
        if (!c) return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 0;
}

static int decode_grounding_identity(const uint8_t *data, size_t length, dnd_grounding_identity *out) {
    pb_reader reader;
    unsigned seen = 0;
    if (!out || !data || !length || length > 192u) return -1;
    memset(out, 0, sizeof(*out));
    pb_reader_init(&reader, data, length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        char *target;
        size_t capacity;
        if (pb_read_tag(&reader, &field, &wire) != 0 || field < 1u || field > 3u ||
            wire != 2u || (seen & (1u << field))) return -1;
        seen |= 1u << field;
        target = field == 1u ? out->id : field == 2u ? out->version : out->sha256;
        capacity = field == 1u ? sizeof(out->id) : field == 2u ? sizeof(out->version) : sizeof(out->sha256);
        if (pb_read_string(&reader, target, capacity) != 0) return -1;
    }
    return seen == 14u && pb_grounding_identity_valid(out) ? 0 : -1;
}

static int retrieval_entry_next(pb_reader *reader, dnd_rag_citation *citation,
                                 dnd_grounding_identity *identity) {
    const uint8_t *data;
    size_t length;
    uint32_t field, wire;
    pb_reader fields;
    if (!reader || !citation || reader->pos > reader->len) return -1;
    if (reader->pos == reader->len) return 0;
    if (pb_read_tag(reader, &field, &wire) != 0 || (field != 1u && field != 2u) || wire != 2u ||
        pb_read_bytes(reader, &data, &length) != 0) return -1;
    if (field == 2u) return decode_grounding_identity(data, length, identity) == 0 ? 2 : -1;
    /* A borrowed public view must not forward opaque extension fields. */
    pb_reader_init(&fields, data, length);
    while (fields.pos < fields.len)
        if (pb_read_tag(&fields, &field, &wire) != 0 || field > 18u ||
            pb_skip(&fields, wire) != 0) return -1;
    if (pb_decode_retrieval_citation(data, length, citation) != 0) return -1;
    return 1;
}

int pb_retrieval_citation_next(pb_reader *reader, dnd_rag_citation *citation) {
    dnd_grounding_identity identity;
    int rc;
    do { rc = retrieval_entry_next(reader, citation, &identity); } while (rc == 2);
    return rc;
}

int pb_decode_retrieval_provenance(const uint8_t *data, size_t length, turn_retrieval_c *out) {
    pb_reader reader;
    dnd_rag_citation citation;
    char records[DND_RAG_HITS_MAX][65];
    size_t count = 0, i;
    int rc, have_identity = 0;
    dnd_grounding_identity identity;
    if (!out) return -1;
    *out = (turn_retrieval_c){0};
    if (!data || !length || length > DND_RAG_CITATIONS_WIRE_CAP) return -1;
    pb_reader_init(&reader, data, length);
    while ((rc = retrieval_entry_next(&reader, &citation, &identity)) > 0) {
        if (rc == 2) {
            if (have_identity) return -1;
            have_identity = 1;
            continue;
        }
        if (count == DND_RAG_HITS_MAX) return -1;
        for (i = 0; i < count; ++i)
            if (strcmp(records[i], dnd_rag_citation_id(&citation)) == 0) return -1;
        memcpy(records[count++], dnd_rag_citation_id(&citation), sizeof(citation.record_id));
    }
    if (rc != 0 || !count) return -1;
    *out = (turn_retrieval_c){data, length, count};
    return 0;
}

int pb_retrieval_grounding(const turn_retrieval_c *retrieval, dnd_grounding_identity *identity) {
    turn_retrieval_c checked;
    pb_reader reader;
    if (!retrieval || !identity) return -1;
    memset(identity, 0, sizeof(*identity));
    if (pb_decode_retrieval_provenance(retrieval->data, retrieval->length, &checked) != 0 ||
        checked.count != retrieval->count) return -1;
    pb_reader_init(&reader, checked.data, checked.length);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        const uint8_t *data;
        size_t length;
        if (pb_read_tag(&reader, &field, &wire) != 0 ||
            pb_read_bytes(&reader, &data, &length) != 0) return -1;
        if (field == 2u) return decode_grounding_identity(data, length, identity) == 0 ? 1 : -1;
    }
    return 0;
}

size_t pb_encode_retrieval_provenance(uint8_t *out, size_t capacity, const dnd_rag_result *result) {
    uint8_t citation[8192];
    size_t i, j, pos = 0;
    if (!out || !result || !result->count || result->count > DND_RAG_HITS_MAX) return 0;
    if (capacity > DND_RAG_CITATIONS_WIRE_CAP) capacity = DND_RAG_CITATIONS_WIRE_CAP;
    for (i = 0; i < result->count; ++i) {
        size_t length = pb_encode_retrieval_citation(citation, sizeof(citation), &result->hits[i].citation);
        if (!length) return 0;
        for (j = 0; j < i; ++j)
            if (strcmp(dnd_rag_citation_id(&result->hits[j].citation), dnd_rag_citation_id(&result->hits[i].citation)) == 0) return 0;
        pos = write_bytes_field(out, capacity, pos, 1u, citation, length);
        if (pos == (size_t)-1) return 0;
    }
    return pos;
}

size_t pb_encode_grounded_retrieval_provenance(uint8_t *out, size_t capacity,
    const dnd_rag_result *result, const dnd_grounding_identity *identity) {
    uint8_t nested[192];
    size_t pos, length = 0;
    if (!pb_grounding_identity_valid(identity)) return 0;
    pos = pb_encode_retrieval_provenance(out, capacity, result);
    if (!pos) return 0;
    length = write_string_field(nested, sizeof(nested), length, 1u, identity->id);
    if (length == (size_t)-1) return 0;
    length = write_string_field(nested, sizeof(nested), length, 2u, identity->version);
    if (length == (size_t)-1) return 0;
    length = write_string_field(nested, sizeof(nested), length, 3u, identity->sha256);
    if (length == (size_t)-1) return 0;
    if (capacity > DND_RAG_CITATIONS_WIRE_CAP) capacity = DND_RAG_CITATIONS_WIRE_CAP;
    pos = write_bytes_field(out, capacity, pos, 2u, nested, length);
    return pos == (size_t)-1 ? 0 : pos;
}

size_t pb_append_turn_retrieval(uint8_t *out, size_t capacity, size_t used,
                                const turn_retrieval_c *retrieval) {
    turn_retrieval_c checked;
    size_t length;
    if (!out || !used || used > capacity || !retrieval ||
        pb_decode_retrieval_provenance(retrieval->data, retrieval->length, &checked) != 0 ||
        checked.count != retrieval->count) return 0;
    length = write_bytes_field(out, capacity, used, 21u, checked.data, checked.length);
    return length == (size_t)-1 ? 0 : length;
}

static void turn_event_init(turn_event_c *out) {
    out->request_id[0] = '\0';
    out->type[0] = '\0';
    out->type_id = 0;
    out->text[0] = '\0';
    out->audio = NULL;
    out->audio_len = 0;
    out->sample_rate = 0;
    out->channels = 0;
    out->bit_depth = 0;
    out->sequence = 0;
    out->segment_index = 0;
    out->is_final = 0;
    out->audio_encoding = 0;
    out->speech_text[0] = '\0';
    out->display_text[0] = '\0';
    out->display_text_len = 0u;
    out->display_text_len_known = 0;
    out->display_text_borrowed = 0u;
    out->current_tts_stage_wire = NULL;
    memset(&out->provider, 0, sizeof(out->provider));
    memset(&out->stages, 0, sizeof(out->stages));
    out->tool_result.present = 0;
    out->retrieval = (turn_retrieval_c){0};
    out->encounter = (turn_encounter_c){0};
    out->roster = (turn_roster_c){0};
    out->initiative = (turn_initiative_c){0};
}

static PB_ALWAYS_INLINE int pb_turn_event_canonical_span(
    const uint8_t *data,
    size_t len,
    size_t *pos,
    const uint8_t **span,
    size_t *span_len
) {
    size_t cursor;
    size_t length;
    uint8_t first;
    if (!data || !pos || !span || !span_len || *pos >= len) return 0;
    cursor = *pos;
    first = data[cursor++];
    if ((first & 0x80u) == 0u) {
        length = first;
    } else {
        uint8_t second;
        if (cursor >= len) return 0;
        second = data[cursor++];
        if ((second & 0x80u) != 0u || second == 0u) return 0;
        length = (size_t)(first & 0x7fu) | (size_t)second << 7u;
    }
    if (length > len - cursor) return 0;
    *span = data + cursor;
    *span_len = length;
    *pos = cursor + length;
    return 1;
}

static PB_ALWAYS_INLINE int pb_turn_event_canonical_u32(
    const uint8_t *data,
    size_t len,
    size_t *pos,
    uint32_t *out
) {
    uint32_t value = 0u;
    unsigned shift = 0u;
    unsigned index;
    size_t cursor;
    if (!data || !pos || !out) return 0;
    cursor = *pos;
    for (index = 0u; index < 5u && cursor < len; ++index) {
        uint8_t byte = data[cursor++];
        if (index == 4u && byte > 7u) return 0;
        value |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0u) {
            if (index != 0u && byte == 0u) return 0;
            *pos = cursor;
            *out = value;
            return 1;
        }
        shift += 7u;
    }
    return 0;
}

static PB_ALWAYS_INLINE int pb_turn_event_canonical_stages(
    const uint8_t *data,
    size_t len,
    size_t *pos,
    turn_stage_timestamps_c *stages
) {
    stage_timestamp_prefix prefix = {0u, 0u};
    while (*pos < len) {
        const uint8_t *entry;
        size_t entry_len;
        if (len - *pos < 3u || data[*pos] != 0x82u ||
            data[*pos + 1u] != 0x01u)
            return 0;
        *pos += 2u;
        entry_len = data[(*pos)++];
        if ((entry_len & 0x80u) != 0u || entry_len > len - *pos)
            return 0;
        entry = data + *pos;
        *pos += entry_len;
        if (decode_stage_map_entry_direct(
                stages, entry, entry_len, &prefix) <= 0)
            return 0;
    }
    return 1;
}

/* Return one for the exact stage wire emitted by the current C TTS service.
 * Return zero to preserve the complete canonical and protobuf fallbacks. */
static PB_ALWAYS_INLINE int pb_turn_event_current_suffix_timestamp(
    const uint8_t *text,
    int64_t *field,
    const stage_timestamp_prefix *prefix
) {
    uint32_t first_four;
    unsigned fifth;
    unsigned sixth;
    if (!text || !field || !prefix ||
        (load_le_u64(text) & UINT64_C(0x00ffffffffffffff)) != prefix->word)
        return -1;
    fifth = (unsigned)(text[11] - (uint8_t)'0');
    sixth = (unsigned)(text[12] - (uint8_t)'0');
    if (fifth > 9u || sixth > 9u ||
        parse_four_decimal_digits(text + 7u, &first_four) != 0)
        return -1;
    *field = (int64_t)(
        prefix->value * UINT64_C(1000000) +
        (uint64_t)first_four * UINT64_C(100) + fifth * 10u + sixth);
    return 0;
}

static PB_NOINLINE int pb_turn_event_current_tts_stages(
    const uint8_t *data,
    size_t len,
    size_t *pos,
    turn_stage_timestamps_c *stages
) {
    stage_timestamp_prefix prefix = {0u, 0u};
    const uint8_t *wire;
    size_t remaining;
    if (!data || !pos || !stages || *pos > len) return 0;
    wire = data + *pos;
    remaining = len - *pos;
    if (remaining != STAGE_TTS_CURRENT_WIRE_LENGTH &&
        remaining != STAGE_TTS_CURRENT_WIRE_LENGTH +
            STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH)
        return 0;
    if (memcmp(
            wire,
            stage_tts_current_wire,
            STAGE_TTS_FIRST_TEXT_VALUE_OFFSET) != 0 ||
        parse_stage_timestamp(
            wire + STAGE_TTS_FIRST_TEXT_VALUE_OFFSET,
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS +
                STAGE_DECIMAL_SUFFIX_DIGITS,
            &stages->first_text_at_ms) != 0)
        return 0;
    prefix.word = load_le_u64(
        wire + STAGE_TTS_FIRST_TEXT_VALUE_OFFSET) &
        UINT64_C(0x00ffffffffffffff);
    prefix.value =
        (uint64_t)stages->first_text_at_ms / UINT64_C(1000000);
#define READ_CURRENT_TTS_STAGE(previous_end, value_offset, member) do { \
        if (memcmp( \
                wire + (previous_end), \
                stage_tts_current_wire + (previous_end), \
                (value_offset) - (previous_end)) != 0 || \
            pb_turn_event_current_suffix_timestamp( \
                wire + (value_offset), &stages->member, &prefix) != 0) \
            return 0; \
    } while (0)
    READ_CURRENT_TTS_STAGE(
        STAGE_TTS_FIRST_TEXT_VALUE_OFFSET +
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS,
        STAGE_TTS_SEGMENT_VALUE_OFFSET, tts_segment_emitted_at_ms);
    READ_CURRENT_TTS_STAGE(
        STAGE_TTS_SEGMENT_VALUE_OFFSET +
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS,
        STAGE_TTS_RECEIVED_VALUE_OFFSET, tts_request_received_at_ms);
    READ_CURRENT_TTS_STAGE(
        STAGE_TTS_RECEIVED_VALUE_OFFSET +
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS,
        STAGE_TTS_PROVIDER_START_VALUE_OFFSET,
        tts_provider_request_started_at_ms);
    READ_CURRENT_TTS_STAGE(
        STAGE_TTS_PROVIDER_START_VALUE_OFFSET +
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS,
        STAGE_TTS_PROVIDER_READY_VALUE_OFFSET, tts_provider_ready_at_ms);
    READ_CURRENT_TTS_STAGE(
        STAGE_TTS_PROVIDER_READY_VALUE_OFFSET +
            STAGE_DECIMAL_CURRENT_PREFIX_DIGITS + STAGE_DECIMAL_SUFFIX_DIGITS,
        STAGE_TTS_PCM_START_VALUE_OFFSET, pcm_started_at_ms);
#undef READ_CURRENT_TTS_STAGE
    if (remaining == STAGE_TTS_CURRENT_WIRE_LENGTH +
            STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH) {
        const uint8_t *first_chunk =
            wire + STAGE_TTS_CURRENT_WIRE_LENGTH;
        if (memcmp(
                first_chunk,
                stage_pcm_first_chunk_current_wire,
                STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET) != 0 ||
            pb_turn_event_current_suffix_timestamp(
                first_chunk + STAGE_PCM_FIRST_CHUNK_VALUE_OFFSET,
                &stages->pcm_first_chunk_at_ms, &prefix) != 0)
            return 0;
    }
    *pos = len;
    return 1;
}

/* Return one for the ordered wire emitted by the C services, or zero to use
 * the complete protobuf decoder. The fallback preserves repeated fields,
 * reordered fields, unknown fields, and every malformed-input decision. */
static PB_NOINLINE int pb_decode_turn_event_public_canonical(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
) {
    const uint8_t *span;
    size_t span_len;
    size_t pos = 0u;
    uint32_t value;
    if (!data || !out || len < request_id_len + 6u || data[pos++] != 0x0au ||
        data[pos++] != request_id_len ||
        memcmp(data + pos, request_id, request_id_len) != 0)
        return 0;
    pos += request_id_len;
    if (len - pos < 2u || data[pos++] != 0x18u ||
        (data[pos] != 7u && data[pos] != 8u))
        return 0;
    turn_event_init(out);
    out->type_id = data[pos++];
    if (out->type_id == 8) {
        unsigned last_field = 6u;
        if (pos < len && data[pos] == 0x32u) {
            pos++;
            if (!pb_turn_event_canonical_span(
                    data, len, &pos, &span, &span_len))
                return 0;
            out->audio = span;
            out->audio_len = span_len;
        }
        while (pos < len && data[pos] != 0x82u) {
            uint8_t tag = data[pos++];
            unsigned field;
            if (tag < 0x38u || tag > 0x78u || (tag & 7u) != 0u)
                return 0;
            field = tag >> 3u;
            if (field <= last_field || field == 13u || field == 14u ||
                !pb_turn_event_canonical_u32(data, len, &pos, &value) ||
                value > INT32_MAX)
                return 0;
            if (field == 7u) out->sample_rate = (int32_t)value;
            else if (field == 8u) out->channels = (int32_t)value;
            else if (field == 9u) out->bit_depth = (int32_t)value;
            else if (field == 10u) out->sequence = (int32_t)value;
            else if (field == 11u) out->segment_index = (int32_t)value;
            else if (field == 12u) {
                if (value > 1u) return 0;
                out->is_final = (int)value;
            } else if (field == 15u) {
                out->audio_encoding = (int32_t)value;
            } else {
                return 0;
            }
            last_field = field;
        }
    }
    {
        const uint8_t *stage_wire = data + pos;
        int current_tts_stage =
            (len - pos == STAGE_TTS_CURRENT_WIRE_LENGTH ||
             len - pos == STAGE_TTS_CURRENT_WIRE_LENGTH +
                 STAGE_PCM_FIRST_CHUNK_WIRE_LENGTH) &&
            pb_turn_event_current_tts_stages(
                data, len, &pos, &out->stages);
        if (current_tts_stage) {
            out->current_tts_stage_wire = stage_wire;
        } else {
            memset(&out->stages, 0, sizeof(out->stages));
            if (!pb_turn_event_canonical_stages(
                    data, len, &pos, &out->stages))
                return 0;
        }
    }
    {
        size_t type_len;
        const char *type_name = turn_event_type_name(out->type_id, &type_len);
        memcpy(out->type, type_name, type_len + 1u);
    }
    return 1;
}

/* Return one for the ordered text wire emitted by the C cascade service.
 * The public edge consumes the display span before the VBus callback returns. */
static PB_NOINLINE int pb_decode_turn_event_public_text_canonical(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
) {
    const uint8_t *private_start;
    const uint8_t *speech_text;
    const uint8_t *display_text;
    size_t private_len;
    size_t speech_len;
    size_t display_len;
    size_t pos = 0u;
    uint32_t value;
    int32_t segment_index = 0;
    int is_final = 0;
    int type_id;
    if (!data || !out || len < request_id_len + 13u ||
        data[pos++] != 0x0au || data[pos++] != request_id_len ||
        memcmp(data + pos, request_id, request_id_len) != 0)
        return 0;
    pos += request_id_len;
    if (len - pos < 4u || data[pos++] != 0x18u ||
        (data[pos] != 4u && data[pos] != 5u))
        return 0;
    type_id = data[pos++];
    if (data[pos++] != 0x2au || !pb_turn_event_canonical_span(
            data, len, &pos, &private_start, &private_len) ||
        private_len == 0u || private_len >= sizeof(out->text))
        return 0;
    if (pos < len && data[pos] == 0x58u) {
        pos++;
        if (!pb_turn_event_canonical_u32(data, len, &pos, &value) ||
            value == 0u || value > INT32_MAX)
            return 0;
        segment_index = (int32_t)value;
    }
    if (pos < len && data[pos] == 0x60u) {
        if (len - pos < 2u || data[pos + 1u] != 1u) return 0;
        pos += 2u;
        is_final = 1;
    }
    if (len - pos < 3u || data[pos++] != 0x8au || data[pos++] != 0x01u ||
        !pb_turn_event_canonical_span(
            data, len, &pos, &speech_text, &speech_len) ||
        speech_len == 0u || speech_len >= sizeof(out->speech_text) ||
        speech_text <= private_start)
        return 0;
    if (len - pos < 3u || data[pos++] != 0x92u || data[pos++] != 0x01u ||
        !pb_turn_event_canonical_span(
            data, len, &pos, &display_text, &display_len) ||
        display_len == 0u || display_len >= sizeof(out->display_text) ||
        display_text <= speech_text || pos != len ||
        memchr(private_start, '\0', len - (size_t)(private_start - data)) != NULL)
        return 0;
    turn_event_init(out);
    out->type_id = type_id;
    out->segment_index = segment_index;
    out->is_final = is_final;
    out->display_text_view = (const char *)display_text;
    out->display_text_len = display_len;
    out->display_text_len_known = 1;
    out->display_text_borrowed = 1u;
    {
        size_t type_len;
        const char *type_name = turn_event_type_name(type_id, &type_len);
        memcpy(out->type, type_name, type_len + 1u);
    }
    return 1;
}

static PB_NOINLINE int pb_read_turn_event_tag_slow(
    pb_reader *reader,
    uint32_t *tag
);

static PB_ALWAYS_INLINE int pb_read_turn_event_tag(
    pb_reader *reader,
    uint32_t *tag
) {
    uint32_t value;
    assert(reader && tag && reader->data && reader->pos < reader->len);
    value = reader->data[reader->pos];
    if ((value & 0x80u) == 0u) {
        reader->pos++;
    } else if (reader->len - reader->pos >= 2u &&
               (reader->data[reader->pos + 1u] & 0x80u) == 0u &&
               reader->data[reader->pos + 1u] != 0u) {
        value = (value & 0x7fu) |
            ((uint32_t)reader->data[reader->pos + 1u] << 7u);
        reader->pos += 2u;
    } else {
        return pb_read_turn_event_tag_slow(reader, tag);
    }
    if ((value >> 3u) == 0u) return -1;
    *tag = value;
    return 0;
}

static PB_ALWAYS_INLINE int pb_decode_turn_event_impl(
    const uint8_t *data,
    size_t len,
    const char *bound_request_id,
    size_t bound_request_id_len,
    int public_only,
    turn_event_c *out
) {
    pb_reader r;
    int request_id_seen = 0;
    int request_id_matches = 0;
    if (!out || (!data && len != 0)) return -1;
    turn_event_init(out);
    pb_reader_init(&r, data, len);
    while (r.pos < r.len) {
        uint32_t tag;
        if (pb_read_turn_event_tag(&r, &tag) != 0) return -1;
        switch (tag) {
        case 24u: {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > 12) return -1;
            out->type_id = (int)v;
            break;
        }
        case 56u:
        case 64u:
        case 72u:
        case 80u:
        case 88u:
        case 96u:
        case 120u: {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0 || v > INT32_MAX) return -1;
            if (tag == 56u) out->sample_rate = (int32_t)v;
            else if (tag == 64u) out->channels = (int32_t)v;
            else if (tag == 72u) out->bit_depth = (int32_t)v;
            else if (tag == 80u) out->sequence = (int32_t)v;
            else if (tag == 88u) out->segment_index = (int32_t)v;
            else if (tag == 96u) {
                if (v > 1) return -1;
                out->is_final = (int)v;
            } else out->audio_encoding = (int32_t)v;
            break;
        }
        case 154u: {
            const uint8_t *value;
            size_t value_len;
            turn_encounter_state_c checked;
            if (out->encounter.data || pb_read_bytes(&r, &value, &value_len) != 0 ||
                pb_decode_turn_encounter(value, value_len, &checked) != 0) return -1;
            out->encounter = checked.wire;
            break;
        }
        case 178u: {
            const uint8_t *value;
            size_t value_len;
            turn_campaign_roster_c checked;
            if (out->roster.data || pb_read_bytes(&r, &value, &value_len) ||
                pb_decode_turn_roster(value, value_len, &checked)) return -1;
            out->roster = checked.wire;
            break;
        }
        case 186u: {
            const uint8_t *value;
            size_t value_len;
            turn_initiative_result_c checked;
            if (out->initiative.data || pb_read_bytes(&r, &value, &value_len) ||
                pb_decode_turn_initiative(value, value_len, &checked)) return -1;
            out->initiative = checked.wire;
            break;
        }
        case 170u: {
            const uint8_t *value;
            size_t value_len;
            if (out->retrieval.data || pb_read_bytes(&r, &value, &value_len) != 0 ||
                pb_decode_retrieval_provenance(value, value_len, &out->retrieval) != 0) return -1;
            break;
        }
        case 162u: {
            const uint8_t *value;
            size_t value_len;
            if (pb_read_bytes(&r, &value, &value_len) != 0 ||
                decode_turn_tool_result(value, value_len, &out->tool_result) != 0) return -1;
            break;
        }
        case 130u: {
            const uint8_t *entry;
            size_t entry_len;
            if (pb_read_bytes(&r, &entry, &entry_len) != 0) return -1;
            if (bound_request_id) {
                int direct = decode_stage_map_entry_direct(
                    &out->stages, entry, entry_len, NULL);
                if (direct < 0 || (direct == 0 &&
                    decode_stage_map_entry(&out->stages, entry, entry_len) != 0) ||
                    decode_provider_entry(&out->provider, entry, entry_len) != 0) return -1;
            } else if (decode_stage_map_entry(&out->stages, entry, entry_len) != 0 ||
                       decode_provider_entry(&out->provider, entry, entry_len) != 0) return -1;
            break;
        }
        case 10u:
            if (bound_request_id) {
                const uint8_t *request_id;
                size_t request_id_len;
                if (pb_read_bytes(&r, &request_id, &request_id_len) != 0 ||
                    request_id_len >= sizeof(out->request_id)) return -1;
                request_id_seen = 1;
                request_id_matches =
                    request_id_len == bound_request_id_len &&
                    memcmp(request_id, bound_request_id, request_id_len) == 0;
                if (!request_id_matches &&
                    memchr(request_id, '\0', request_id_len) != NULL) return -1;
            } else if (pb_read_string(
                           &r, out->request_id,
                           sizeof(out->request_id)) != 0) return -1;
            break;
        case 42u:
            if (public_only) {
                const uint8_t *private_text;
                size_t private_text_len;
                if (pb_read_string_view(
                        &r, sizeof(out->text),
                        &private_text, &private_text_len) != 0) return -1;
            } else if (pb_read_string(
                           &r, out->text, sizeof(out->text)) != 0) return -1;
            break;
        case 50u:
            if (pb_read_bytes(&r, &out->audio, &out->audio_len) != 0) return -1;
            break;
        case 138u:
            if (public_only) {
                const uint8_t *private_text;
                size_t private_text_len;
                if (pb_read_string_view(
                        &r, sizeof(out->speech_text),
                        &private_text, &private_text_len) != 0) return -1;
            } else if (pb_read_string(
                           &r, out->speech_text,
                           sizeof(out->speech_text)) != 0) return -1;
            break;
        case 146u:
            if (public_only) {
                const uint8_t *display_text;
                if (pb_read_string_view(
                        &r, sizeof(out->display_text), &display_text,
                        &out->display_text_len) != 0) return -1;
                out->display_text_view = (const char *)display_text;
                out->display_text_borrowed = 1u;
            } else if (pb_read_string_sized(
                           &r, out->display_text, sizeof(out->display_text),
                           &out->display_text_len) != 0) return -1;
            out->display_text_len_known = 1;
            break;
        default:
            if ((tag >> 3u) >= 19u && (tag >> 3u) <= 23u) return -1;
            if (pb_skip(&r, tag & 7u) != 0) return -1;
            break;
        }
    }
    if (bound_request_id) {
        if (!request_id_seen || !request_id_matches) return -1;
    }
    {
        size_t type_len;
        const char *type_name = turn_event_type_name(out->type_id, &type_len);
        memcpy(out->type, type_name, type_len + 1u);
    }
    return (bound_request_id || out->request_id[0]) && out->type_id != 0 ? 0 : -1;
}

int pb_decode_turn_event(const uint8_t *data, size_t len, turn_event_c *out) {
    return pb_decode_turn_event_impl(data, len, NULL, 0u, 0, out);
}

int pb_decode_turn_event_bound(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
) {
    if (!request_id || request_id_len == 0u || request_id_len >= 128u ||
        request_id[request_id_len] != '\0') return -1;
    return pb_decode_turn_event_impl(
        data, len, request_id, request_id_len, 0, out);
}

int pb_decode_turn_event_active(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
) {
    assert(request_id && request_id_len != 0u && request_id_len < 128u &&
           request_id[request_id_len] == '\0');
#if defined(__GNUC__) || defined(__clang__)
    if (!request_id || request_id_len == 0u || request_id_len >= 128u ||
        request_id[request_id_len] != '\0') __builtin_unreachable();
#endif
    return pb_decode_turn_event_impl(
        data, len, request_id, request_id_len, 0, out);
}

int pb_decode_turn_event_public_active(
    const uint8_t *data,
    size_t len,
    const char *request_id,
    size_t request_id_len,
    turn_event_c *out
) {
    assert(request_id && request_id_len != 0u && request_id_len < 128u &&
           request_id[request_id_len] == '\0');
#if defined(__GNUC__) || defined(__clang__)
    if (!request_id || request_id_len == 0u || request_id_len >= 128u ||
        request_id[request_id_len] != '\0') __builtin_unreachable();
#endif
    /* C lifecycle events contain only the bound identifier and type fields.
     * Any additional or reordered field uses the complete decoder below. */
    if (data && len == request_id_len + 4u) {
        uint8_t type_id = data[request_id_len + 3u];
        if ((type_id <= 3u || type_id >= 9u) && type_id <= 12u &&
            type_id != 0u && data[0] == 0x0au &&
            data[1] == request_id_len &&
            data[request_id_len + 2u] == 0x18u &&
            memcmp(data + 2u, request_id, request_id_len) == 0) {
            size_t type_len;
            const char *type_name;
            turn_event_init(out);
            out->type_id = type_id;
            type_name = turn_event_type_name(type_id, &type_len);
            memcpy(out->type, type_name, type_len + 1u);
            return 0;
        }
    }
    if (data && len >= request_id_len + 4u &&
        (data[request_id_len + 3u] == 4u ||
         data[request_id_len + 3u] == 5u) &&
        pb_decode_turn_event_public_text_canonical(
            data, len, request_id, request_id_len, out))
        return 0;
    if (data && len >= request_id_len + 4u &&
        (data[request_id_len + 3u] == 7u ||
         data[request_id_len + 3u] == 8u) &&
        pb_decode_turn_event_public_canonical(
            data, len, request_id, request_id_len, out))
        return 0;
    return pb_decode_turn_event_impl(
        data, len, request_id, request_id_len, 1, out);
}

static PB_NOINLINE int pb_read_turn_event_tag_slow(
    pb_reader *reader,
    uint32_t *tag
) {
    uint32_t field;
    uint32_t wire;
    if (!tag || pb_read_tag(reader, &field, &wire) != 0) return -1;
    *tag = field << 3u | wire;
    return 0;
}
