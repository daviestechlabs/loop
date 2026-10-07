#include "dnd_retrieval.h"
#include "pb_min.h"
#include "openai_min.h"
#include "turn_response_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check_grounded_request(const dnd_rag_result *result) {
    static const char system[] = "Use only the cited excerpts.";
    char prompt[DND_RAG_PROMPT_CAP], body[(DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1024u];
    if (dnd_rag_grounded_prompt(result, "Which rule applies?", prompt, sizeof(prompt)) == 0 &&
        !openai_chat_request_json_stream_system(body, sizeof(body), "local-model",
            system, sizeof(system) - 1u, prompt, strlen(prompt), 48u, sizeof(prompt) - 1u)) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dnd_rag_config config = {
        {"books", "owned", "campaign", "session", "character"}, "bge-m3", 3u, DND_RAG_SCORE_COSINE, "", ""
    };
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "character-a", "campaign_canon", 1, 0};
    dnd_rag_scope scope;
    dnd_rag_result result;
    cmp_json_array array;
    cmp_json_object object;
    cmp_json_field element;
    char *input;
    char output[65536], vector[DND_RAG_VECTOR_JSON_CAP];
    size_t count = 0;
    if (size < DND_RAG_WIRE_CAP) {
        uint8_t wire[DND_RAG_WIRE_CAP];
        rag_search_request_c request;
        rag_search_response_c response;
        dnd_rag_citation citation;
        turn_event_c event;
        turn_response_event safe;
        turn_response_state state = {0};
        turn_retrieval_c provenance;
        if (pb_decode_retrieval_provenance(data, size, &provenance) == 0) {
            size_t base = pb_encode_turn_event(wire, sizeof(wire), "fuzz-turn", "completed", "");
            if (!pb_append_turn_retrieval(wire, sizeof(wire), base, &provenance)) abort();
        }
        if (pb_decode_turn_event(data, size, &event) == 0 &&
            turn_response_filter_decoded(&state, &event, &safe) > TURN_RESPONSE_DROP) {
            char json[DND_RAG_PUBLIC_JSON_CAP];
            if (!turn_response_protobuf_encode(wire, sizeof(wire), &safe) ||
                !turn_response_json_encode(json, sizeof(json), &safe, 123)) abort();
        }
        if (pb_decode_rag_search_request(data, size, &request) == 0 &&
            !pb_encode_rag_search_request(wire, sizeof(wire), &request)) abort();
        if (pb_decode_rag_search_response(data, size, &response) == 0 &&
            !pb_encode_rag_search_response(wire, sizeof(wire), &response)) abort();
        if (pb_decode_retrieval_citation(data, size, &citation) == 0 &&
            !pb_encode_retrieval_citation(wire, sizeof(wire), &citation)) abort();
    }
    if (size >= 65536u || memchr(data, 0, size)) return 0;
    input = malloc(size + 1u);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    if (size < DND_GROUNDING_TEXT_CAP && openai_chat_request_json_stream_system(
            output, sizeof(output), "model", input, size, "question", 8u, 48u, 8u)) {
        char decoded[DND_GROUNDING_TEXT_CAP];
        cmp_json_object message;
        if (!cmp_json_object_parse(output, &object) ||
            !cmp_json_field_array(cmp_json_object_field(&object, "messages"), &array) ||
            cmp_json_array_next(&array, &element) != 1 || !cmp_json_field_object(&element, &message) ||
            !cmp_json_object_str(&message, "content", decoded, sizeof(decoded)) || strcmp(decoded, input) != 0 ||
            cmp_json_array_next(&array, &element) != 1 || cmp_json_array_next(&array, &element) != 0) abort();
    }
    if (dnd_rag_scope_resolve(&config, &identity, &scope) != 0) abort();
    (void)dnd_rag_search_extract(&config, &scope, input, 4u, &result);
    check_grounded_request(&result);
    identity.knowledge_scope = "shared_rulebook";
    if (dnd_rag_scope_resolve(&config, &identity, &scope) != 0) abort();
    (void)dnd_rag_search_extract(&config, &scope, input, 4u, &result);
    check_grounded_request(&result);
    if (dnd_rag_embedding_extract(&config, input, vector, sizeof(vector)) == 0 &&
        dnd_rag_search_body(&config, &scope, vector, 4u, output, sizeof(output)) != 0) abort();
    (void)dnd_rag_search_body(&config, &scope, input, 4u, output, sizeof(output));
    config.shared_rulebook_metric = DND_RAG_SCORE_BM25;
    (void)dnd_rag_lexical_search_body(&config, &scope, input, 4u, output, sizeof(output));
    (void)dnd_rag_search_extract(&config, &scope, input, 4u, &result);
    check_grounded_request(&result);
    if (cmp_json_object_parse(input, &object)) (void)dnd_rag_scope_matches(&scope, &object);
    strcpy(config.shared_rulebook_ruleset, "dnd-5e-2014");
    if (dnd_rag_scope_resolve(&config, &identity, &scope) != 0) abort();
    (void)dnd_rag_lexical_search_body(&config, &scope, input, 4u, output, sizeof(output));
    (void)dnd_rag_search_extract(&config, &scope, input, 4u, &result);
    check_grounded_request(&result);
    if (cmp_json_array_parse(input, &array)) {
        while (cmp_json_array_next(&array, &element) == 1) {
            double number;
            if (++count > size) abort();
            (void)cmp_json_field_double(&element, &number);
            (void)cmp_json_field_object(&element, &object);
            (void)cmp_json_field_str(&element, output, sizeof(output));
        }
    }
    config.shared_rulebook_ruleset[0] = '\0';
    strcpy(config.shared_rulebook_corpus, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    if (dnd_rag_scope_resolve(&config, &identity, &scope)) abort();
    const char *ids[] = {"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"};
    if (dnd_rag_record_query_body(&config, &scope, ids, 1, output, sizeof(output))) abort();
    if (!dnd_rag_record_query_extract(&config, &scope, ids, 1, input, &result)) {
        if (result.count != 1 || result.hits[0].citation.score_metric != DND_RAG_SCORE_NONE ||
            result.hits[0].citation.score != 0.0 || strcmp(result.hits[0].citation.record_id, ids[0])) abort();
        check_grounded_request(&result);
    } else if (result.count) abort();
    const char *candidate_ids[] = {input};
    (void)dnd_rag_record_query_body(&config, &scope, candidate_ids, 1, output, sizeof(output));
    if (size < 128u) {
        memcpy(config.shared_rulebook_corpus, input, size + 1u);
        (void)dnd_rag_scope_resolve(&config, &identity, &scope);
        config.shared_rulebook_corpus[0] = '\0';
        memcpy(config.shared_rulebook_ruleset, input, size + 1u);
        (void)dnd_rag_scope_resolve(&config, &identity, &scope);
        config.shared_rulebook_ruleset[0] = '\0';
        identity.user_id = input;
        (void)dnd_rag_scope_resolve(&config, &identity, &scope);
        identity.user_id = "user-a";
        identity.knowledge_scope = input;
        (void)dnd_rag_scope_resolve(&config, &identity, &scope);
    }
    (void)dnd_rag_embedding_body(&config, input, output, sizeof(output));
    free(input);
    return 0;
}
