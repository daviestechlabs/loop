#include "turn_provenance.h"

#include "pb_min.h"
#include "turn_response.h"
#include <string.h>

#if defined(__wasm__)
#define DTL_EXPORT(name) __attribute__((export_name(name)))
#else
#define DTL_EXPORT(name)
#endif

static uint8_t input[DTL_PROVENANCE_INPUT_CAPACITY];
static char request[DTL_PROVENANCE_REQUEST_CAPACITY];
static turn_event_c event;
static dnd_rag_citation citations[DND_RAG_HITS_MAX];
static dnd_grounding_identity prompt;
static uint32_t counts[3];

DTL_EXPORT("input") uint8_t *dtl_provenance_input(void) { return input; }
DTL_EXPORT("request") char *dtl_provenance_request(void) { return request; }

DTL_EXPORT("project") uint32_t dtl_provenance_project(uint32_t length, uint32_t request_length) {
    turn_response_state state = {0};
    turn_response_event safe;
    pb_reader reader;
    uint32_t count = 0u;
    int has_prompt = 0;
    memset(counts, 0, sizeof(counts));
    if (length == 0u || length > sizeof(input) || request_length == 0u ||
        request_length >= sizeof(request) || request[request_length] != '\0' ||
        memchr(request, '\0', request_length) != NULL ||
        pb_decode_turn_event(input, length, &event) != 0 ||
        strcmp(event.request_id, request) != 0 ||
        (!event.tool_result.present && event.retrieval.count == 0u) ||
        turn_response_filter_decoded(&state, &event, &safe) <= TURN_RESPONSE_DROP)
        return 0u;
    if (safe.retrieval) {
        pb_reader_init(&reader, safe.retrieval->data, safe.retrieval->length);
        while (count < safe.retrieval->count) {
            if (count >= DND_RAG_HITS_MAX ||
                pb_retrieval_citation_next(&reader, &citations[count]) != 1) return 0u;
            count++;
        }
        has_prompt = pb_retrieval_grounding(safe.retrieval, &prompt);
        if (has_prompt < 0) return 0u;
    }
    counts[DTL_PROVENANCE_TOOL] = safe.tool_result ? 1u : 0u;
    counts[DTL_PROVENANCE_CITATION] = count;
    counts[DTL_PROVENANCE_PROMPT] = (uint32_t)has_prompt;
    return 1u;
}

DTL_EXPORT("count") uint32_t dtl_provenance_count(uint32_t kind) {
    return kind < 3u ? counts[kind] : 0u;
}

static const dnd_rag_passage_witness *witness_at(uint32_t index) {
    uint32_t citation = index / DND_RAG_PASSAGE_WITNESSES_MAX;
    uint32_t witness = index % DND_RAG_PASSAGE_WITNESSES_MAX;
    return citation < counts[DTL_PROVENANCE_CITATION] && witness < citations[citation].witness_count
        ? &citations[citation].witnesses[witness] : NULL;
}

DTL_EXPORT("string") const char *dtl_provenance_string(uint32_t kind, uint32_t index, uint32_t field) {
    static const size_t tool_fields[] = {
        offsetof(turn_tool_result_c, tool_id), offsetof(turn_tool_result_c, tool_call_id),
        offsetof(turn_tool_result_c, output_sha256)
    };
    static const size_t citation_fields[] = {
#define CITATION_FIELD(member) offsetof(dnd_rag_citation, member)
        CITATION_FIELD(source), CITATION_FIELD(book_slug), CITATION_FIELD(collection),
        CITATION_FIELD(corpus_version), CITATION_FIELD(embedding_model), CITATION_FIELD(record_id),
        CITATION_FIELD(document_id), CITATION_FIELD(content_hash), CITATION_FIELD(source_sha256),
        CITATION_FIELD(section)
#undef CITATION_FIELD
    };
    static const size_t prompt_fields[] = {
        offsetof(dnd_grounding_identity, id), offsetof(dnd_grounding_identity, version),
        offsetof(dnd_grounding_identity, sha256)
    };
    if (kind == DTL_PROVENANCE_WITNESS) {
        const dnd_rag_passage_witness *w = witness_at(index);
        return !w ? NULL : field == 0u ? w->record_id : field == 1u ? w->content_hash : NULL;
    }
    if (index >= dtl_provenance_count(kind)) return NULL;
    if (kind == DTL_PROVENANCE_TOOL && field < sizeof(tool_fields) / sizeof(tool_fields[0]))
        return (const char *)&event.tool_result + tool_fields[field];
    if (kind == DTL_PROVENANCE_CITATION && field < sizeof(citation_fields) / sizeof(citation_fields[0]))
        return (const char *)&citations[index] + citation_fields[field];
    if (kind == DTL_PROVENANCE_CITATION && field == 10u)
        return dnd_rag_score_metric_name(citations[index].score_metric);
    if (kind == DTL_PROVENANCE_CITATION && field == 11u) return citations[index].passage_id;
    if (kind == DTL_PROVENANCE_PROMPT && field < sizeof(prompt_fields) / sizeof(prompt_fields[0]))
        return (const char *)&prompt + prompt_fields[field];
    return NULL;
}

DTL_EXPORT("number") double dtl_provenance_number(uint32_t kind, uint32_t index, uint32_t field) {
    if (kind == DTL_PROVENANCE_WITNESS) {
        const dnd_rag_passage_witness *w = witness_at(index);
        if (!w) return 0.0;
        switch (field) {
        case 0u: return w->page;
        case 1u: return w->chunk;
        case 2u: return w->begin;
        case 3u: return w->end;
        case 4u: return w->record_length;
        default: return 0.0;
        }
    }
    if (index >= dtl_provenance_count(kind)) return 0.0;
    if (kind == DTL_PROVENANCE_TOOL && field == 0u) return (double)event.tool_result.elapsed_ms;
    if (kind == DTL_PROVENANCE_CITATION) {
        switch (field) {
        case 0u: return (double)citations[index].page_start;
        case 1u: return (double)citations[index].page_end;
        case 2u: return (double)citations[index].chunk_index;
        case 3u: return citations[index].score;
        case 4u: return (double)citations[index].excerpt.count;
        case 21u: return (double)citations[index].witness_count;
        default: break;
        }
        if (field >= 5u) {
            uint32_t span = (field - 5u) / 2u;
            if (span < citations[index].excerpt.count)
                return (double)((field - 5u) % 2u ? citations[index].excerpt.spans[span].end :
                    citations[index].excerpt.spans[span].begin);
        }
    }
    return 0.0;
}

DTL_EXPORT("clear") void dtl_provenance_clear(void) {
    memset(input, 0, sizeof(input));
    memset(request, 0, sizeof(request));
    memset(&event, 0, sizeof(event));
    memset(citations, 0, sizeof(citations));
    memset(&prompt, 0, sizeof(prompt));
    memset(counts, 0, sizeof(counts));
}
