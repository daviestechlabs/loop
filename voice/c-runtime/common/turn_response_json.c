#include "turn_response_json.h"

#include "base64.h"
#include "runtime_identity.h"
#include "stage_json.h"
#include "utf8.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *cursor;
    size_t remaining;
} response_json_writer;

#define WRITER_LITERAL(writer, literal) \
    writer_append((writer), (literal), sizeof(literal) - 1u)

static const char decimal_pairs[] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

#define JSON_BYTE_LANES_ONES UINT64_C(0x0101010101010101)
#define JSON_BYTE_LANES_HIGH UINT64_C(0x8080808080808080)

static inline int writer_word_has_value(
    uint64_t word,
    uint64_t repeated_value
) {
    uint64_t matched = word ^ repeated_value;
    return ((matched - JSON_BYTE_LANES_ONES) & ~matched &
            JSON_BYTE_LANES_HIGH) != 0u;
}

static inline int writer_word_needs_escape(uint64_t word) {
    return writer_word_has_value(word, UINT64_C(0x2222222222222222)) ||
        writer_word_has_value(word, UINT64_C(0x5c5c5c5c5c5c5c5c));
}

static inline int writer_append(
    response_json_writer *writer,
    const char *value,
    size_t value_len
) {
    if (value_len >= writer->remaining) return -1;
    if (value_len != 0u)
        memcpy(writer->cursor, value, value_len);
    writer->cursor += value_len;
    writer->remaining -= value_len;
    return 0;
}

static int writer_append_cstr_n(
    response_json_writer *writer,
    const char *value,
    size_t known_len
) {
    return value ? writer_append(
        writer, value, known_len != 0u ? known_len : strlen(value)) : -1;
}

static int writer_append_u64(
    response_json_writer *writer,
    uint64_t value
) {
    char digits[20];
    size_t start = sizeof(digits);
    while (value >= UINT64_C(100)) {
        size_t pair = (size_t)(value % UINT64_C(100)) * 2u;
        value /= UINT64_C(100);
        start -= 2u;
        digits[start] = decimal_pairs[pair];
        digits[start + 1u] = decimal_pairs[pair + 1u];
    }
    if (value < UINT64_C(10)) {
        digits[--start] = (char)('0' + value);
    } else {
        size_t pair = (size_t)value * 2u;
        start -= 2u;
        digits[start] = decimal_pairs[pair];
        digits[start + 1u] = decimal_pairs[pair + 1u];
    }
    return writer_append(writer, digits + start, sizeof(digits) - start);
}

static int writer_append_i64(
    response_json_writer *writer,
    int64_t value
) {
    uint64_t magnitude = (uint64_t)value;
    if (value < 0) {
        if (writer_append(writer, "-", 1u) != 0) return -1;
        magnitude = UINT64_C(0) - magnitude;
    }
    return writer_append_u64(writer, magnitude);
}

static int writer_append_timestamp_ms(
    response_json_writer *writer,
    int64_t value
) {
    char digits[13];
    uint64_t timestamp;
    uint32_t high;
    uint32_t low;
    size_t pair;
    if (value < INT64_C(1000000000000) ||
        value > INT64_C(9999999999999))
        return writer_append_i64(writer, value);
    timestamp = (uint64_t)value;
    high = (uint32_t)(timestamp / UINT64_C(100000000));
    low = (uint32_t)(timestamp % UINT64_C(100000000));
    digits[0] = (char)('0' + high / UINT32_C(10000));
    high %= UINT32_C(10000);
    pair = (size_t)(high / UINT32_C(100)) * 2u;
    digits[1] = decimal_pairs[pair];
    digits[2] = decimal_pairs[pair + 1u];
    pair = (size_t)(high % UINT32_C(100)) * 2u;
    digits[3] = decimal_pairs[pair];
    digits[4] = decimal_pairs[pair + 1u];
    pair = (size_t)(low / UINT32_C(1000000)) * 2u;
    digits[5] = decimal_pairs[pair];
    digits[6] = decimal_pairs[pair + 1u];
    low %= UINT32_C(1000000);
    pair = (size_t)(low / UINT32_C(10000)) * 2u;
    digits[7] = decimal_pairs[pair];
    digits[8] = decimal_pairs[pair + 1u];
    low %= UINT32_C(10000);
    pair = (size_t)(low / UINT32_C(100)) * 2u;
    digits[9] = decimal_pairs[pair];
    digits[10] = decimal_pairs[pair + 1u];
    pair = (size_t)(low % UINT32_C(100)) * 2u;
    digits[11] = decimal_pairs[pair];
    digits[12] = decimal_pairs[pair + 1u];
    return writer_append(writer, digits, sizeof(digits));
}

static int writer_append_escaped(
    response_json_writer *writer,
    const char *value,
    size_t known_len
) {
    const uint8_t *bytes = (const uint8_t *)value;
    size_t length;
    size_t start = 0;
    size_t i;
    int non_ascii = 0;
    if (!writer || !value) return -1;
    length = known_len != 0u ? known_len : strlen(value);
    for (i = 0; i < length; ++i) {
        if (bytes[i] < 0x20u) return -1;
        if (bytes[i] >= 0x80u) non_ascii = 1;
        if (bytes[i] == (uint8_t)'"' || bytes[i] == (uint8_t)'\\') {
            if (writer_append(writer, value + start, i - start) != 0 ||
                writer_append(writer, "\\", 1u) != 0)
                return -1;
            start = i;
        }
    }
    if (non_ascii && !utf8_validate_v1(bytes, length)) return -1;
    return writer_append(writer, value + start, length - start);
}

static int writer_append_escaped_validated(
    response_json_writer *writer,
    const char *value,
    size_t length
) {
    size_t start = 0u;
    size_t i = 0u;
    if (!writer || !value) return -1;
    while (length - i >= sizeof(uint64_t)) {
        uint64_t word;
        size_t word_end;
        memcpy(&word, value + i, sizeof(word));
        if (!writer_word_needs_escape(word)) {
            i += sizeof(word);
            continue;
        }
        word_end = i + sizeof(word);
        while (i < word_end && value[i] != '"' && value[i] != '\\') i++;
        if (i == word_end) continue;
        if (writer_append(writer, value + start, i - start) != 0 ||
            writer_append(writer, "\\", 1u) != 0)
            return -1;
        start = i++;
    }
    while (i < length) {
        if (value[i] == '"' || value[i] == '\\') {
            if (writer_append(writer, value + start, i - start) != 0 ||
                writer_append(writer, "\\", 1u) != 0)
                return -1;
            start = i;
        }
        i++;
    }
    return writer_append(writer, value + start, length - start);
}

/* Escape a citation value through its inner JSON string and the metadata string.
 * Only citation events use this path; ordinary display text keeps its fast writer. */
static int writer_append_citation_string(response_json_writer *writer, const char *value) {
    static const char hex[] = "0123456789abcdef";
    size_t length = strlen(value), i, start = 0;
    for (i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)value[i];
        char escape[7];
        size_t count;
        if (byte >= 0x20u && byte != '"' && byte != '\\') continue;
        if (writer_append(writer, value + start, i - start) != 0) return -1;
        escape[0] = escape[1] = '\\';
        if (byte == '"' || byte == '\\') {
            escape[2] = '\\'; escape[3] = (char)byte; count = 4u;
        } else {
            escape[2] = 'u'; escape[3] = escape[4] = '0';
            escape[5] = hex[byte >> 4u]; escape[6] = hex[byte & 15u]; count = 7u;
        }
        if (writer_append(writer, escape, count) != 0) return -1;
        start = i + 1u;
    }
    return writer_append(writer, value + start, length - start);
}

static int writer_encounter_string(response_json_writer *writer, const char *key, const char *value) {
    return WRITER_LITERAL(writer, "\"") || writer_append_cstr_n(writer, key, 0u) ||
        WRITER_LITERAL(writer, "\":\"") || writer_append_escaped(writer, value, 0u) ||
        WRITER_LITERAL(writer, "\"") ? -1 : 0;
}

static int writer_append_encounter(response_json_writer *writer, const turn_response_event *event) {
    turn_encounter_state_c state;
    turn_encounter_participant_c part;
    pb_reader reader;
    size_t count = 0;
    int rc;
    if (!event->encounter) return 0;
    if (event->type_id != 5 || !event->is_final || event->retrieval ||
        !turn_encounter_valid(event->encounter, event->tool_result) ||
        pb_decode_turn_encounter(event->encounter->data, event->encounter->length, &state)) return -1;
    if (WRITER_LITERAL(writer, ",\"dnd_encounter_state\":{") ||
        writer_encounter_string(writer, "campaign_id", state.campaign_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "encounter_id", state.encounter_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "status", state.status) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "active_participant_id", state.active_participant_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "tool_call_id", state.tool_call_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "output_sha256", state.output_sha256) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "operation", state.operation) ||
        WRITER_LITERAL(writer, ",\"version\":") || writer_append_i64(writer, state.version) ||
        WRITER_LITERAL(writer, ",\"round\":") || writer_append_i64(writer, state.round) ||
        WRITER_LITERAL(writer, ",\"active_index\":") || writer_append_i64(writer, state.active_index) ||
        WRITER_LITERAL(writer, ",\"participants\":[")) return -1;
    pb_reader_init(&reader, event->encounter->data, event->encounter->length);
    while ((rc = pb_encounter_participant_next(&reader, &part)) == 1) {
        if ((count++ && WRITER_LITERAL(writer, ",")) || WRITER_LITERAL(writer, "{") ||
            writer_encounter_string(writer, "id", part.id) || WRITER_LITERAL(writer, ",") ||
            writer_encounter_string(writer, "name", part.name) ||
            WRITER_LITERAL(writer, ",\"initiative\":") || writer_append_i64(writer, part.initiative) ||
            WRITER_LITERAL(writer, ",\"max_hp\":") || writer_append_i64(writer, part.max_hp) ||
            WRITER_LITERAL(writer, ",\"current_hp\":") || writer_append_i64(writer, part.current_hp) ||
            WRITER_LITERAL(writer, ",\"conditions\":[")) return -1;
        for (size_t i = 0; i < part.condition_count; ++i)
            if ((i && WRITER_LITERAL(writer, ",")) || WRITER_LITERAL(writer, "\"") ||
                writer_append_escaped(writer, part.conditions[i], 0u) || WRITER_LITERAL(writer, "\"")) return -1;
        if (WRITER_LITERAL(writer, "]}")) return -1;
    }
    return rc == 0 && count == state.participant_count && !WRITER_LITERAL(writer, "]}") ? 0 : -1;
}

static int writer_append_roster(response_json_writer *writer, const turn_roster_c *roster) {
    turn_campaign_roster_c state;
    turn_campaign_character_c character;
    pb_reader reader;
    size_t count = 0;
    int rc;
    if (!roster) return 0;
    if (pb_decode_turn_roster(roster->data, roster->length, &state)) return -1;
    if (WRITER_LITERAL(writer, ",\"dnd_campaign_roster\":{") ||
        writer_encounter_string(writer, "campaign_id", state.campaign_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "status", state.status) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "tool_call_id", state.tool_call_id) || WRITER_LITERAL(writer, ",") ||
        writer_encounter_string(writer, "output_sha256", state.output_sha256) ||
        WRITER_LITERAL(writer, ",\"version\":") || writer_append_i64(writer, state.version) ||
        WRITER_LITERAL(writer, ",\"characters\":[")) return -1;
    pb_reader_init(&reader, roster->data, roster->length);
    while ((rc = pb_roster_character_next(&reader, &character)) == 1) {
        if ((count++ && WRITER_LITERAL(writer, ",")) || WRITER_LITERAL(writer, "{") ||
            writer_encounter_string(writer, "id", character.id) || WRITER_LITERAL(writer, ",") ||
            writer_encounter_string(writer, "name", character.name) || WRITER_LITERAL(writer, ",") ||
            writer_encounter_string(writer, "kind", character.kind) ||
            WRITER_LITERAL(writer, ",\"max_hp\":") || writer_append_i64(writer, character.max_hp) ||
            WRITER_LITERAL(writer, "}")) return -1;
    }
    return rc == 0 && count == state.character_count && !WRITER_LITERAL(writer, "]}") ? 0 : -1;
}

static int writer_append_initiative(response_json_writer *writer, const turn_initiative_c *initiative) {
    turn_initiative_result_c result;
    turn_initiative_roll_c roll;
    pb_reader reader;
    size_t count = 0;
    int rc;
    if (!initiative) return 0;
    if (pb_decode_turn_initiative(initiative->data, initiative->length, &result)) return -1;
    if (WRITER_LITERAL(writer, ",\"dnd_initiative_result\":{") ||
        writer_encounter_string(writer, "campaign_sha256", result.campaign_sha256) ||
        WRITER_LITERAL(writer, ",\"campaign_version\":") || writer_append_i64(writer, result.campaign_version) ||
        WRITER_LITERAL(writer, ",\"rolls\":[")) return -1;
    pb_reader_init(&reader, initiative->data, initiative->length);
    while ((rc = pb_initiative_roll_next(&reader, &roll)) == 1) {
        if ((count++ && WRITER_LITERAL(writer, ",")) || WRITER_LITERAL(writer, "{") ||
            writer_encounter_string(writer, "character_id", roll.character_id) || WRITER_LITERAL(writer, ",") ||
            writer_encounter_string(writer, "expression", roll.expression) ||
            WRITER_LITERAL(writer, ",\"rolls\":[")) return -1;
        for (size_t i = 0; i < roll.roll_count; ++i)
            if ((i && WRITER_LITERAL(writer, ",")) || writer_append_u64(writer, roll.rolls[i])) return -1;
        if (WRITER_LITERAL(writer, "],\"kept_indices\":[") || writer_append_u64(writer, roll.kept_index) ||
            WRITER_LITERAL(writer, "],\"total\":") || writer_append_i64(writer, roll.total) ||
            WRITER_LITERAL(writer, ",\"entropy_source\":\"getrandom\"}")) return -1;
    }
    return rc == 0 && count == result.roll_count && !WRITER_LITERAL(writer, "]}") ? 0 : -1;
}

static int writer_append_retrieval(response_json_writer *writer, const turn_retrieval_c *retrieval) {
    static const struct { const char *name; size_t offset; } fields[] = {
#define CITATION_STRING(member) {#member, offsetof(dnd_rag_citation, member)}
        CITATION_STRING(source), CITATION_STRING(book_slug), CITATION_STRING(collection),
        CITATION_STRING(corpus_version), CITATION_STRING(embedding_model), CITATION_STRING(record_id),
        CITATION_STRING(document_id), CITATION_STRING(content_hash), CITATION_STRING(source_sha256),
        CITATION_STRING(section)
#undef CITATION_STRING
    };
    pb_reader reader;
    dnd_rag_citation citation;
    dnd_grounding_identity identity;
    size_t count = 0, i;
    int rc;
    if (!turn_retrieval_valid(retrieval) ||
        WRITER_LITERAL(writer, "\"cascade_route\":\"retrieve_then_escalate\","
            "\"cascade_retrieval_used\":\"true\",\"cascade_retrieved_documents\":\"") != 0 ||
        writer_append_u64(writer, retrieval->count) != 0 ||
        WRITER_LITERAL(writer, "\",\"cascade_retrieval_citations\":\"[") != 0) return -1;
    pb_reader_init(&reader, retrieval->data, retrieval->length);
    while ((rc = pb_retrieval_citation_next(&reader, &citation)) == 1) {
        char numbers[192];
        int length;
        if ((count++ && WRITER_LITERAL(writer, ",") != 0) || WRITER_LITERAL(writer, "{") != 0) return -1;
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            const char *value = (const char *)&citation + fields[i].offset;
            if ((i && WRITER_LITERAL(writer, ",") != 0) || WRITER_LITERAL(writer, "\\\"") != 0 ||
                writer_append(writer, fields[i].name, strlen(fields[i].name)) != 0 ||
                WRITER_LITERAL(writer, "\\\":\\\"") != 0 ||
                writer_append_citation_string(writer, value) != 0 ||
                WRITER_LITERAL(writer, "\\\"") != 0) return -1;
        }
        length = snprintf(numbers, sizeof(numbers),
            ",\\\"page_start\\\":%d,\\\"page_end\\\":%d,\\\"chunk_index\\\":%d,\\\"score\\\":%.17g,"
            "\\\"score_metric\\\":\\\"%s\\\"",
            citation.page_start, citation.page_end, citation.chunk_index, citation.score,
            dnd_rag_score_metric_name(citation.score_metric));
        if (length < 1 || (size_t)length >= sizeof(numbers) ||
            writer_append(writer, numbers, (size_t)length) != 0) return -1;
        if (citation.excerpt.count) {
            if (WRITER_LITERAL(writer, ",\\\"excerpt_spans\\\":[")) return -1;
            for (size_t j = 0; j < citation.excerpt.count; ++j) {
                if ((j && WRITER_LITERAL(writer, ",")) ||
                    WRITER_LITERAL(writer, "{\\\"begin\\\":") ||
                    writer_append_u64(writer, citation.excerpt.spans[j].begin) ||
                    WRITER_LITERAL(writer, ",\\\"end\\\":") ||
                    writer_append_u64(writer, citation.excerpt.spans[j].end) ||
                    WRITER_LITERAL(writer, "}")) return -1;
            }
            if (WRITER_LITERAL(writer, "]")) return -1;
        }
        if (citation.witness_count) {
            if (WRITER_LITERAL(writer, ",\\\"kind\\\":\\\"complete_passage\\\",\\\"passage_id\\\":\\\"") ||
                writer_append_citation_string(writer, citation.passage_id) ||
                WRITER_LITERAL(writer, "\\\",\\\"witnesses\\\":[")) return -1;
            for (size_t j = 0; j < citation.witness_count; ++j) {
                const dnd_rag_passage_witness *w = &citation.witnesses[j];
                if ((j && WRITER_LITERAL(writer, ",")) ||
                    WRITER_LITERAL(writer, "{\\\"record_id\\\":\\\"") ||
                    writer_append_citation_string(writer, w->record_id) ||
                    WRITER_LITERAL(writer, "\\\",\\\"content_hash\\\":\\\"") ||
                    writer_append_citation_string(writer, w->content_hash) ||
                    WRITER_LITERAL(writer, "\\\",\\\"page\\\":") || writer_append_u64(writer, w->page) ||
                    WRITER_LITERAL(writer, ",\\\"chunk_index\\\":") || writer_append_u64(writer, w->chunk) ||
                    WRITER_LITERAL(writer, ",\\\"begin\\\":") || writer_append_u64(writer, w->begin) ||
                    WRITER_LITERAL(writer, ",\\\"end\\\":") || writer_append_u64(writer, w->end) ||
                    WRITER_LITERAL(writer, ",\\\"record_length\\\":") || writer_append_u64(writer, w->record_length) ||
                    WRITER_LITERAL(writer, "}")) return -1;
            }
            if (WRITER_LITERAL(writer, "]")) return -1;
        }
        if (WRITER_LITERAL(writer, "}")) return -1;
    }
    if (rc != 0 || count != retrieval->count || WRITER_LITERAL(writer, "]\"") != 0) return -1;
    rc = pb_retrieval_grounding(retrieval, &identity);
    if (rc < 0) return -1;
    if (!rc) return 0;
    /* The wire validator admits only JSON-plain identity fields. */
    return WRITER_LITERAL(writer, ",\"cascade_grounding_prompt_id\":\"") != 0 ||
        writer_append(writer, identity.id, strlen(identity.id)) != 0 ||
        WRITER_LITERAL(writer, "\",\"cascade_grounding_prompt_version\":\"") != 0 ||
        writer_append(writer, identity.version, strlen(identity.version)) != 0 ||
        WRITER_LITERAL(writer, "\",\"cascade_grounding_prompt_sha256\":\"") != 0 ||
        writer_append(writer, identity.sha256, 64u) != 0 || WRITER_LITERAL(writer, "\"") != 0 ? -1 : 0;
}

static int writer_append_metadata(
    response_json_writer *writer,
    const turn_response_event *event
) {
    size_t metadata_len = 0u;
    size_t i;
    int have_runtime;
    if (!event) return -1;
    if (!writer || !writer->cursor || writer->remaining == 0u) return -1;
    have_runtime = event->runtime_identity_sha256_len != 0u;
    if (have_runtime &&
        (!event->runtime_identity_sha256 ||
         event->runtime_identity_sha256_len != VOICE_RUNTIME_IDENTITY_HASH_LEN))
        return -1;
    for (i = 0u; i < event->runtime_identity_sha256_len; ++i) {
        unsigned char byte =
            (unsigned char)event->runtime_identity_sha256[i];
        if (!((byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
              (byte >= (unsigned char)'a' && byte <= (unsigned char)'f')))
            return -1;
    }
    if (event->stages) {
        metadata_len = event->current_tts_stage_wire ?
            turn_stage_metadata_json_write_current_wire_v1(
                event->stages, event->current_tts_stage_wire,
                writer->cursor, writer->remaining) :
            turn_stage_metadata_json_write_v1(
                event->stages, writer->cursor, writer->remaining);
        if (metadata_len == SIZE_MAX) return -1;
        writer->cursor += metadata_len;
        writer->remaining -= metadata_len;
    }
    if (!have_runtime && !event->tool_result && !event->retrieval && !event->provider_model && !event->provider_usage) return 0;
    if (metadata_len != 0u) {
        if (writer->cursor[-1] != '}') return -1;
        writer->cursor--;
        writer->remaining++;
    } else if (WRITER_LITERAL(writer, ",\"metadata\":{") != 0) return -1;
    if (event->provider_model) {
        if ((metadata_len && WRITER_LITERAL(writer, ",") != 0) ||
            WRITER_LITERAL(writer, "\"model_id\":\"") != 0 ||
            writer_append_escaped(writer, event->provider_model, strlen(event->provider_model)) != 0 ||
            WRITER_LITERAL(writer, "\",\"model_identity_source\":\"provider_reported\"") != 0) return -1;
        metadata_len = 1u;
    }
    if (event->provider_usage) {
        uint64_t counts[3];
        if (!pb_turn_provider_usage(event->provider_usage, counts) ||
            (metadata_len && WRITER_LITERAL(writer, ",") != 0) ||
            WRITER_LITERAL(writer, "\"usage_source\":\"provider_reported\",\"prompt_tokens\":\"") != 0 ||
            writer_append_u64(writer, counts[0]) != 0 ||
            WRITER_LITERAL(writer, "\",\"completion_tokens\":\"") != 0 ||
            writer_append_u64(writer, counts[1]) != 0 ||
            WRITER_LITERAL(writer, "\",\"total_tokens\":\"") != 0 ||
            writer_append_u64(writer, counts[2]) != 0 || WRITER_LITERAL(writer, "\"") != 0) return -1;
        metadata_len = 1u;
    }
    if (have_runtime) {
        if ((metadata_len != 0u && WRITER_LITERAL(writer, ",") != 0) ||
            WRITER_LITERAL(writer, "\"cascade_runtime_identity_sha256\":\"") != 0 ||
            writer_append(writer, event->runtime_identity_sha256, event->runtime_identity_sha256_len) != 0 ||
            WRITER_LITERAL(writer, "\"") != 0) return -1;
        metadata_len = 1u;
    }
    if (event->tool_result) {
        const turn_tool_result_c *tool = event->tool_result;
        if (!pb_turn_tool_result_valid(tool) ||
            (metadata_len != 0u && WRITER_LITERAL(writer, ",") != 0) ||
            WRITER_LITERAL(writer, "\"cascade_fallback_reason\":\"tool_executed\",\"cascade_tool_id\":\"") != 0 ||
            writer_append(writer, tool->tool_id, strlen(tool->tool_id)) != 0 ||
            WRITER_LITERAL(writer, "\",\"cascade_tool_call_id\":\"") != 0 ||
            writer_append(writer, tool->tool_call_id, strlen(tool->tool_call_id)) != 0 ||
            WRITER_LITERAL(writer, "\",\"cascade_tool_output_hash\":\"") != 0 ||
            writer_append(writer, tool->output_sha256, 64u) != 0 ||
            WRITER_LITERAL(writer, "\",\"cascade_tool_ms\":\"") != 0 ||
            writer_append_u64(writer, (uint64_t)tool->elapsed_ms) != 0 ||
            WRITER_LITERAL(writer, "\"") != 0) return -1;
        metadata_len = 1u;
    }
    if (event->retrieval &&
        ((metadata_len != 0u && WRITER_LITERAL(writer, ",") != 0) ||
         writer_append_retrieval(writer, event->retrieval) != 0)) return -1;
    return WRITER_LITERAL(writer, "}");
}

static int writer_append_pcm_contract(
    response_json_writer *writer,
    const turn_response_event *event
) {
    if (event->channels == 1 && event->bit_depth == 16 &&
        event->audio_encoding == 1) {
        if (event->sample_rate == 16000)
            return WRITER_LITERAL(
                writer,
                "\",\"sample_rate\":16000,\"channels\":1,"
                "\"bit_depth\":16,\"audio_encoding\":1,\"sequence\":");
        if (event->sample_rate == 24000)
            return WRITER_LITERAL(
                writer,
                "\",\"sample_rate\":24000,\"channels\":1,"
                "\"bit_depth\":16,\"audio_encoding\":1,\"sequence\":");
    }
    if (WRITER_LITERAL(writer, "\",\"sample_rate\":") != 0 ||
        writer_append_i64(writer, event->sample_rate) != 0 ||
        WRITER_LITERAL(writer, ",\"channels\":") != 0 ||
        writer_append_i64(writer, event->channels) != 0 ||
        WRITER_LITERAL(writer, ",\"bit_depth\":") != 0 ||
        writer_append_i64(writer, event->bit_depth) != 0 ||
        WRITER_LITERAL(writer, ",\"audio_encoding\":") != 0 ||
        writer_append_i64(writer, event->audio_encoding) != 0 ||
        WRITER_LITERAL(writer, ",\"sequence\":") != 0)
        return -1;
    return 0;
}

size_t turn_acceptance_json_encode(
    char *out,
    size_t out_cap,
    const char *request_id,
    size_t request_id_len,
    int64_t accepted_at,
    const turn_acceptance_metrics *metrics
) {
    response_json_writer writer;
    if (!out || out_cap == 0u || !request_id || request_id_len == 0u ||
        request_id_len > 127u || request_id[0] == '\0' || accepted_at <= 0 ||
        !metrics)
        return 0;
    writer.cursor = out;
    writer.remaining = out_cap;
    out[0] = '\0';
    if (WRITER_LITERAL(
            &writer, "{\"type\":\"accepted\",\"request_id\":\"") != 0 ||
        writer_append(&writer, request_id, request_id_len) != 0 ||
        WRITER_LITERAL(
            &writer,
            "\",\"protocol\":\"turnstream.v1alpha1\","
            "\"response_event_contract\":\"canonical-v1\","
            "\"accepted_at\":") != 0 ||
        writer_append_timestamp_ms(&writer, accepted_at) != 0 ||
        WRITER_LITERAL(&writer, ",\"metadata\":{\"edge_auth_us\":") != 0 ||
        writer_append_u64(&writer, metrics->auth_us) != 0 ||
        WRITER_LITERAL(&writer, ",\"edge_vbus_publish_us\":") != 0 ||
        writer_append_u64(&writer, metrics->vbus_publish_us) != 0 ||
        WRITER_LITERAL(&writer, ",\"edge_prepare_us\":") != 0 ||
        writer_append_u64(&writer, metrics->prepare_us) != 0 ||
        WRITER_LITERAL(&writer, ",\"edge_admission_us\":") != 0 ||
        writer_append_u64(&writer, metrics->admission_us) != 0 ||
        WRITER_LITERAL(&writer, ",\"edge_capability_us\":") != 0 ||
        writer_append_u64(&writer, metrics->capability_us) != 0 ||
        WRITER_LITERAL(&writer, ",\"edge_encode_us\":") != 0 ||
        writer_append_u64(&writer, metrics->encode_us) != 0 ||
        WRITER_LITERAL(&writer, "}}\n") != 0)
        goto fail;
    out[out_cap - writer.remaining] = '\0';
    return out_cap - writer.remaining;
fail:
    out[0] = '\0';
    return 0;
}

size_t turn_response_json_encode(
    char *out,
    size_t out_cap,
    const turn_response_event *event,
    int64_t timestamp_ms
) {
    response_json_writer writer;
    if (!out || out_cap == 0u || !event || !event->type ||
        !event->request_id || timestamp_ms <= 0)
        return 0;
    if (((event->retrieval || event->encounter || event->roster || event->initiative) &&
         (event->type_id != 5 || !event->is_final)) ||
        (event->retrieval && (event->encounter || event->roster || event->initiative)) ||
        (event->type_id == 5 && !turn_state_result_valid(event->encounter,
            event->roster, event->initiative, event->tool_result))) {
        out[0] = '\0';
        return 0;
    }
    writer.cursor = out;
    writer.remaining = out_cap;
    out[0] = '\0';
    if (event->type_id == 4 || event->type_id == 5) {
        if (!event->display_text ||
            WRITER_LITERAL(&writer, "{\"type\":\"") != 0 ||
            writer_append_cstr_n(&writer, event->type, event->type_len) != 0 ||
            WRITER_LITERAL(&writer, "\",\"request_id\":\"") != 0 ||
            writer_append_cstr_n(
                &writer, event->request_id, event->request_id_len) != 0 ||
            WRITER_LITERAL(&writer, "\",\"text\":\"") != 0 ||
            (event->text_json_raw ?
                 writer_append(
                     &writer, event->display_text, event->text_len) :
                 event->text_validated ?
                     writer_append_escaped_validated(
                         &writer, event->display_text, event->text_len) :
                     writer_append_escaped(
                         &writer, event->display_text, event->text_len)) != 0 ||
            WRITER_LITERAL(&writer, "\"") != 0 ||
            (event->type_id == 5 && (WRITER_LITERAL(&writer, ",\"is_final\":") != 0 ||
                writer_append(&writer, event->is_final ? "true" : "false",
                    event->is_final ? 4u : 5u) != 0)) ||
            WRITER_LITERAL(&writer, ",\"timestamp\":") != 0 ||
            writer_append_timestamp_ms(&writer, timestamp_ms) != 0 ||
            writer_append_metadata(&writer, event) != 0 ||
            writer_append_encounter(&writer, event) != 0 ||
            writer_append_roster(&writer, event->roster) != 0 ||
            writer_append_initiative(&writer, event->initiative) != 0 ||
            WRITER_LITERAL(&writer, "}\n") != 0)
            goto fail;
    } else if (event->type_id == 8) {
        size_t audio_len;
        if ((!event->audio && event->audio_len != 0u) ||
            WRITER_LITERAL(
                &writer, "{\"type\":\"pcm_chunk\",\"request_id\":\"") != 0 ||
            writer_append_cstr_n(
                &writer, event->request_id, event->request_id_len) != 0 ||
            WRITER_LITERAL(&writer, "\",\"audio_base64\":\"") != 0)
            goto fail;
        audio_len = base64_encode_v1(
            event->audio, event->audio_len, writer.cursor, writer.remaining);
        if (audio_len == 0u) goto fail;
        writer.cursor += audio_len;
        writer.remaining -= audio_len;
        if (writer_append_pcm_contract(&writer, event) != 0 ||
            writer_append_i64(&writer, event->sequence) != 0 ||
            WRITER_LITERAL(&writer, ",\"segment_index\":") != 0 ||
            writer_append_i64(&writer, event->segment_index) != 0 ||
            WRITER_LITERAL(&writer, ",\"is_final\":") != 0 ||
            writer_append(
                &writer, event->is_final ? "true" : "false",
                event->is_final ? 4u : 5u) != 0 ||
            WRITER_LITERAL(&writer, ",\"timestamp\":") != 0 ||
            writer_append_timestamp_ms(&writer, timestamp_ms) != 0 ||
            writer_append_metadata(&writer, event) != 0 ||
            WRITER_LITERAL(&writer, "}\n") != 0)
            goto fail;
    } else if (event->type_id == 12) {
        if (WRITER_LITERAL(
                &writer, "{\"type\":\"failed\",\"request_id\":\"") != 0 ||
            writer_append_cstr_n(
                &writer, event->request_id, event->request_id_len) != 0 ||
            WRITER_LITERAL(
                &writer, "\",\"error\":\"upstream_failed\",\"timestamp\":") != 0 ||
            writer_append_timestamp_ms(&writer, timestamp_ms) != 0 ||
            writer_append_metadata(&writer, event) != 0 ||
            WRITER_LITERAL(&writer, "}\n") != 0)
            goto fail;
    } else {
        if (WRITER_LITERAL(&writer, "{\"type\":\"") != 0 ||
            writer_append_cstr_n(&writer, event->type, event->type_len) != 0 ||
            WRITER_LITERAL(&writer, "\",\"request_id\":\"") != 0 ||
            writer_append_cstr_n(
                &writer, event->request_id, event->request_id_len) != 0 ||
            WRITER_LITERAL(&writer, "\",\"timestamp\":") != 0 ||
            writer_append_timestamp_ms(&writer, timestamp_ms) != 0 ||
            writer_append_metadata(&writer, event) != 0 ||
            WRITER_LITERAL(&writer, "}\n") != 0)
            goto fail;
    }
    out[out_cap - writer.remaining] = '\0';
    return out_cap - writer.remaining;
fail:
    out[0] = '\0';
    return 0;
}
