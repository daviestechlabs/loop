#include "dnd_retrieval.h"
#include "dnd_grounding.h"
#include "response_style.h"
#include "ent_books.h"
#include "pb_min.h"
#include "openai_min.h"
#include "turn_response_json.h"
#include <openssl/sha.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static dnd_rag_config configuration(void) {
    dnd_rag_config config = {
        {"reviewed_books", "dnd_owned_rulebooks_v1", "dnd_campaign_canon_v1",
         "dnd_session_transcripts_v1", "dnd_character_memory_v1"},
        "bge-m3", 3u, DND_RAG_SCORE_COSINE, "", ""
    };
    return config;
}

static void test_scope(void) {
    static const char *const names[] = {"shared_rulebook", "owned_rulebook", "campaign_canon",
        "session_transcript", "character_memory"};
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "character-a", "", 1, 0};
    dnd_rag_scope scope;
    size_t i;
    for (i = 0; i < DND_RAG_SCOPE_COUNT; ++i) {
        identity.knowledge_scope = names[i];
        CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
        CHECK(strcmp(scope.collection, config.collections[i]) == 0);
        CHECK(strcmp(scope.authenticated_user_id, "user-a") == 0);
        if (i == DND_RAG_SHARED_RULEBOOK) {
            CHECK(strstr(scope.filter, "visibility == \"public\"") != NULL);
            CHECK(strstr(scope.filter, "official_book") != NULL);
            CHECK(!scope.owner_user_id[0] && !scope.campaign_id[0]);
        } else {
            CHECK(strstr(scope.filter, "owner_user_id == \"user-a\"") != NULL);
            CHECK(strstr(scope.filter, names[i]) != NULL);
        }
        if (i >= DND_RAG_CAMPAIGN_CANON)
            CHECK(strstr(scope.filter, "campaign_id == \"campaign-a\"") != NULL);
        if (i == DND_RAG_SESSION_TRANSCRIPT)
            CHECK(strstr(scope.filter, "session_id == \"session-a\"") != NULL);
        if (i == DND_RAG_CHARACTER_MEMORY)
            CHECK(strstr(scope.filter, "character_id == \"character-a\"") != NULL);
        identity.entitled = 0;
        CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
        CHECK(!scope.filter[0] && !scope.collection[0]);
        identity.entitled = 1;
    }
    identity.knowledge_scope = "campaign_canon";
    identity.campaign_id = "camp\" or true";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.campaign_id = "";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.campaign_id = "campaign-a";
    identity.user_id = "User-A";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.user_id = "user-a";
    identity.knowledge_scope = "session_transcript";
    identity.session_id = "";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.knowledge_scope = "character_memory";
    identity.character_id = "";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.knowledge_scope = "ephemeral_scene";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.knowledge_scope = "shared_rulebook";
    strcpy(config.collections[0], "books?owner=other");
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
}

static void test_returned_scope(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "character-a", "campaign_canon", 1, 0};
    dnd_rag_scope scope;
    cmp_json_object object;
    const char *valid = "{\"knowledge_scope\":\"campaign_canon\",\"owner_user_id\":\"user-a\","
        "\"visibility\":\"private\",\"campaign_id\":\"campaign-a\"}";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(cmp_json_object_parse(valid, &object));
    CHECK(dnd_rag_scope_matches(&scope, &object));
    identity.user_id = "user-b";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(!dnd_rag_scope_matches(&scope, &object));
    identity.user_id = "user-a";
    identity.campaign_id = "campaign-b";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(!dnd_rag_scope_matches(&scope, &object));
    identity.knowledge_scope = "shared_rulebook";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(!dnd_rag_scope_matches(&scope, &object));
    CHECK(cmp_json_object_parse("{\"knowledge_scope\":\"shared_rulebook\",\"owner_user_id\":\"\","
        "\"visibility\":\"public\",\"source_kind\":\"official_book\",\"campaign_id\":\"\","
        "\"session_id\":\"\",\"character_id\":\"\",\"book_slug\":\"players-handbook\"}", &object));
    CHECK(dnd_rag_scope_matches(&scope, &object));
    CHECK(cmp_json_object_parse("{\"knowledge_scope\":\"shared_rulebook\",\"owner_user_id\":\"\","
        "\"owner_user_id\":\"other\",\"visibility\":\"public\",\"source_kind\":\"official_book\","
        "\"campaign_id\":\"\",\"session_id\":\"\",\"character_id\":\"\"}", &object));
    CHECK(!dnd_rag_scope_matches(&scope, &object));
}

static void test_json(void) {
    cmp_json_array array;
    cmp_json_field value;
    cmp_json_object object;
    char text[128], escaped[256];
    const char *input = "雪\t\r\n\b\f\001\\\"";
    double number;
    CHECK(cmp_json_array_parse("[ {\"a\":\"雪\"}, 1.25e-1, [true], null, \"x\" ]", &array));
    CHECK(cmp_json_array_next(&array, &value) == 1 && cmp_json_field_object(&value, &object));
    CHECK(cmp_json_object_str(&object, "a", text, sizeof(text)) && strcmp(text, "雪") == 0);
    CHECK(cmp_json_array_next(&array, &value) == 1 && cmp_json_field_double(&value, &number));
    CHECK(number == 0.125);
    CHECK(cmp_json_array_next(&array, &value) == 1);
    CHECK(!cmp_json_field_double(&value, &number));
    CHECK(cmp_json_array_next(&array, &value) == 1);
    CHECK(!cmp_json_field_double(&value, &number));
    CHECK(cmp_json_array_next(&array, &value) == 1 && cmp_json_field_str(&value, text, sizeof(text)));
    CHECK(strcmp(text, "x") == 0 && cmp_json_array_next(&array, &value) == 0);
    CHECK(cmp_json_array_next(&array, &value) == 0);
    CHECK(!cmp_json_array_parse("[1,]", &array));
    CHECK(!cmp_json_array_parse("[1] false", &array));
    CHECK(!cmp_json_array_parse("[1e,2]", &array));
    CHECK(!cmp_json_array_parse("[\"\xff\"]", &array));
    CHECK(cmp_json_escape_exact(input, escaped, sizeof(escaped)) == 0);
    CHECK(snprintf(text, sizeof(text), "{\"x\":\"%s\"}", escaped) > 0);
    CHECK(cmp_json_object_parse(text, &object));
    CHECK(cmp_json_object_str(&object, "x", escaped, sizeof(escaped)));
    CHECK(strcmp(escaped, input) == 0);
    CHECK(cmp_json_escape_exact("abc", escaped, 4u) == 0 && strcmp(escaped, "abc") == 0);
    CHECK(cmp_json_escape_exact("abc", escaped, 3u) == -1 && escaped[0] == '\0');
    CHECK(cmp_json_escape_exact("\xff", escaped, sizeof(escaped)) == -1);
}

static void test_embedding_and_search(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "", "campaign_canon", 1, 0};
    dnd_rag_scope scope;
    cmp_json_object object, nested;
    cmp_json_array data;
    cmp_json_field element;
    char body[65536], vector[DND_RAG_VECTOR_JSON_CAP], text[1024];
    const char *valid = "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":["
        "{\"object\":\"embedding\",\"index\":0,\"embedding\":[0.5,-0.25,0.75]}]}";
    const char *bad_vectors[] = {"[]", "[0,0,0]", "[1,2]", "[1,2,3,4]", "[1,NaN,3]",
        "[1,1e309,3]", "[1,1e40,3]", "[1,1e-50,3]", "[1,1e-9999,3]", "[1,true,3]", "[1,\"2\",3]",
        "[1,[2],3]", "[1,null,3]", "[01,2,3]", "[1,2,3],\"filter\":\"true\""};
    size_t i;
    CHECK(dnd_rag_embedding_body(&config, "A\tquoted \"雪\" rule?", body, sizeof(body)) == 0);
    CHECK(cmp_json_object_parse(body, &object));
    CHECK(cmp_json_field_array(cmp_json_object_field(&object, "input"), &data));
    CHECK(cmp_json_array_next(&data, &element) == 1 && cmp_json_field_str(&element, text, sizeof(text)));
    CHECK(strcmp(text, "A\tquoted \"雪\" rule?") == 0);
    CHECK(cmp_json_array_next(&data, &element) == 0);
    CHECK(dnd_rag_embedding_extract(&config, valid, vector, sizeof(vector)) == 0);
    CHECK(strcmp(vector, "[0.5,-0.25,0.75]") == 0);
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_search_body(&config, &scope, vector, 4u, body, sizeof(body)) == 0);
    CHECK(cmp_json_object_parse(body, &object));
    CHECK(cmp_json_object_str(&object, "filter", text, sizeof(text)) && strcmp(text, scope.filter) == 0);
    CHECK(cmp_json_object_str(&object, "annsField", text, sizeof(text)) && strcmp(text, "embedding") == 0);
    CHECK(cmp_json_object_object(&object, "searchParams", &nested));
    CHECK(cmp_json_object_str(&nested, "metricType", text, sizeof(text)) && strcmp(text, "COSINE") == 0);
    for (i = 0; i < sizeof(bad_vectors) / sizeof(bad_vectors[0]); ++i) {
        CHECK(dnd_rag_search_body(&config, &scope, bad_vectors[i], 4u, body, sizeof(body)) == -1);
        CHECK(!body[0]);
        CHECK(snprintf(body, sizeof(body), "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":["
            "{\"object\":\"embedding\",\"index\":0,\"embedding\":%s}]}", bad_vectors[i]) > 0);
        CHECK(dnd_rag_embedding_extract(&config, body, vector, sizeof(vector)) == -1 && !vector[0]);
    }
    CHECK(dnd_rag_embedding_extract(&config, valid, vector, 3u) == -1 && !vector[0]);
    config.embedding_dimensions = 4u;
    CHECK(dnd_rag_embedding_extract(&config, valid, vector, sizeof(vector)) == -1);
    config.embedding_dimensions = 3u;
    strcpy(config.embedding_model, "foreign-model");
    CHECK(dnd_rag_embedding_extract(&config, valid, vector, sizeof(vector)) == -1);
    config = configuration();
    strcpy(scope.filter, "true");
    CHECK(dnd_rag_search_body(&config, &scope, "[1,2,3]", 4u, body, sizeof(body)) == -1);
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_search_body(&config, &scope, "[1,2,3]", 5u, body, sizeof(body)) == -1);
    CHECK(dnd_rag_search_body(&config, &scope, "[1,2,3]", 4u, body, 10u) == -1 && !body[0]);
}

static void test_lexical_request(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    cmp_json_object object, parameters;
    cmp_json_array data;
    cmp_json_field value;
    char body[65536], text[4096], too_long[2049];
    const char *query = "A quoted \"雪\" rule?\nNext line.";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 4u, body, sizeof(body)) == -1);
    config.shared_rulebook_metric = DND_RAG_SCORE_BM25;
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 4u, body, sizeof(body)) == 0);
    CHECK(cmp_json_object_parse(body, &object));
    CHECK(cmp_json_object_str(&object, "filter", text, sizeof(text)) && strcmp(text, scope.filter) == 0);
    CHECK(strstr(text, "players-handbook") && !strstr(text, "tashas-cauldron"));
    CHECK(cmp_json_object_str(&object, "annsField", text, sizeof(text)) && strcmp(text, "sparse") == 0);
    CHECK(cmp_json_object_object(&object, "searchParams", &parameters));
    CHECK(cmp_json_object_str(&parameters, "metricType", text, sizeof(text)) && strcmp(text, "BM25") == 0);
    CHECK(cmp_json_field_array(cmp_json_object_field(&object, "data"), &data));
    CHECK(cmp_json_array_next(&data, &value) == 1 && cmp_json_field_str(&value, text, sizeof(text)));
    CHECK(strcmp(query, text) == 0 && cmp_json_array_next(&data, &value) == 0);
    CHECK(dnd_rag_search_body(&config, &scope, "[1,2,3]", 4u, body, sizeof(body)) == -1);
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK(dnd_rag_lexical_search_body(&config, &scope, too_long, 4u, body, sizeof(body)) == -1 && !body[0]);
    CHECK(dnd_rag_lexical_search_body(&config, &scope, "\xff", 4u, body, sizeof(body)) == -1);
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 5u, body, sizeof(body)) == -1);
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 4u, body, 8u) == -1 && !body[0]);
    strcpy(scope.filter, "true");
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 4u, body, sizeof(body)) == -1);
    identity.knowledge_scope = "campaign_canon";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_lexical_search_body(&config, &scope, query, 4u, body, sizeof(body)) == -1);
    CHECK(dnd_rag_search_body(&config, &scope, "[1,2,3]", 4u, body, sizeof(body)) == 0);
    CHECK(strstr(body, "\"metadata_json\",\"knowledge_scope\",\"session_id\",\"character_id\"]") != NULL);
}

static void test_embedding_envelope(void) {
    dnd_rag_config config = configuration();
    char output[DND_RAG_VECTOR_JSON_CAP];
    const char *cases[] = {
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[{\"object\":\"embedding\",\"index\":1,\"embedding\":[1,2,3]}]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3]},{}]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3]}],\"data\":[]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"model\":\"foreign\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3]}]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"error\":null,\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3]}]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\\u0000\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3]}]}",
        "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[1,2,3],\"embedding\":[4,5,6]}]}"
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(dnd_rag_embedding_extract(&config, cases[i], output, sizeof(output)) == -1);
        CHECK(!output[0]);
    }
}

static void test_model_dimensions(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "", "", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    char response[16384], vector[DND_RAG_VECTOR_JSON_CAP], body[65536];
    size_t i, length;
    config.embedding_dimensions = DND_RAG_VECTOR_DIM_MAX;
    strcpy(response, "{\"object\":\"list\",\"input_contract\":{\"pooling\":\"cls\",\"truncated\":false,\"max_input_tokens\":128,\"input_tokens\":[8]},\"model\":\"bge-m3\",\"data\":[{\"object\":\"embedding\",\"index\":0,\"embedding\":[");
    length = strlen(response);
    for (i = 0; i < config.embedding_dimensions; ++i) {
        const char *value = i ? ",0.125" : "0.125";
        memcpy(response + length, value, strlen(value));
        length += strlen(value);
    }
    memcpy(response + length, "]}]}", 5u);
    CHECK(dnd_rag_embedding_extract(&config, response, vector, sizeof(vector)) == 0);
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_search_body(&config, &scope, vector, 4u, body, sizeof(body)) == 0);
    config.embedding_dimensions = 1023u;
    CHECK(dnd_rag_embedding_extract(&config, response, vector, sizeof(vector)) == -1);
    config.embedding_dimensions = 1025u;
    CHECK(dnd_rag_embedding_body(&config, "rule?", body, sizeof(body)) == -1);
}

static void test_book_entitlements(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "", "", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    cmp_json_object metadata;
    size_t count, i, core_count = 0;
    const ent_book *books = ent_books(&count);
    uint64_t core = ent_book_access_mask(0), all = ent_book_access_mask(1);
    CHECK(count > 4u && count <= 64u && ent_book_mask_valid(core) && ent_book_mask_valid(all));
    CHECK(ent_book_allowed(core, "players-handbook"));
    CHECK(!ent_book_allowed(core, "tashas-cauldron-of-everything"));
    CHECK(ent_book_allowed(all, "tashas-cauldron-of-everything"));
    CHECK(!ent_book_allowed(all, "unreviewed-book"));
    CHECK(!ent_book_allowed(UINT64_MAX, "players-handbook"));
    CHECK(!ent_book_access_mask(2));
    for (i = 0; i < count; ++i) {
        CHECK(ent_book_allowed(all, books[i].slug));
        CHECK(ent_book_allowed(core, books[i].slug) == books[i].core);
        if (books[i].core) ++core_count;
    }
    CHECK(core_count == 4u);
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(strstr(scope.filter, "\"players-handbook\"") != NULL);
    CHECK(strstr(scope.filter, "tashas-cauldron-of-everything") == NULL);
    CHECK(cmp_json_object_parse("{\"knowledge_scope\":\"shared_rulebook\",\"owner_user_id\":\"\","
        "\"visibility\":\"public\",\"source_kind\":\"official_book\",\"campaign_id\":\"\","
        "\"session_id\":\"\",\"character_id\":\"\",\"book_slug\":\"tashas-cauldron-of-everything\"}", &metadata));
    CHECK(!dnd_rag_scope_matches(&scope, &metadata));
    identity.premium = 1;
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(strstr(scope.filter, "\"tashas-cauldron-of-everything\"") != NULL);
    CHECK(dnd_rag_scope_matches(&scope, &metadata));
    scope.book_mask |= UINT64_C(1) << 63u;
    CHECK(!dnd_rag_scope_matches(&scope, &metadata));
    identity.premium = 2;
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    identity.premium = 1;
    identity.entitled = 0;
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
}

static void replace_one(char *text, size_t capacity, const char *from, const char *to) {
    char *at = strstr(text, from);
    size_t before, after, replacement = strlen(to);
    CHECK(at != NULL);
    before = (size_t)(at - text);
    after = strlen(at + strlen(from));
    CHECK(before + replacement + after < capacity);
    memmove(at + replacement, at + strlen(from), after + 1u);
    memcpy(at, to, replacement);
}

static void replace_all(char *text, size_t capacity, const char *from, const char *to) {
    CHECK(!strstr(to, from));
    while (strstr(text, from)) replace_one(text, capacity, from, to);
}

static void test_rag_wire(const dnd_rag_result *result) {
    rag_search_request_c request = {0}, decoded_request;
    rag_search_response_c response = {0}, decoded_response;
    dnd_rag_citation decoded_citation;
    uint8_t wire[DND_RAG_WIRE_CAP], changed[DND_RAG_WIRE_CAP];
    size_t length, i;
    pb_reader reader;
    strcpy(request.request_id, "request-a");
    strcpy(request.user_id, "user-a");
    strcpy(request.query, "Which rules apply?");
    strcpy(request.session_id, "session-a");
    strcpy(request.knowledge_scope, "campaign_canon");
    strcpy(request.campaign_id, "campaign-a");
    strcpy(request.character_id, "character-a");
    request.top_k = 4;
    request.premium = 1;
    request.deadline_unix_ms = 1788655000123LL;
    length = pb_encode_rag_search_request(wire, sizeof(wire), &request);
    CHECK(length > 0u && pb_decode_rag_search_request(wire, length, &decoded_request) == 0);
    CHECK(strcmp(decoded_request.user_id, "user-a") == 0 && decoded_request.premium == 1);
    CHECK(strcmp(decoded_request.knowledge_scope, "campaign_canon") == 0);
    CHECK(strcmp(decoded_request.campaign_id, "campaign-a") == 0);
    CHECK(strcmp(decoded_request.character_id, "character-a") == 0);
    CHECK(decoded_request.deadline_unix_ms == request.deadline_unix_ms);
    /* Reject every duplicate known field, including duplicate equal values. */
    pb_reader_init(&reader, wire, length);
    while (reader.pos < reader.len) {
        uint32_t field, type;
        size_t start = reader.pos, field_length;
        CHECK(pb_read_tag(&reader, &field, &type) == 0 && pb_skip(&reader, type) == 0);
        field_length = reader.pos - start;
        memcpy(changed, wire, length);
        memcpy(changed + length, wire + start, field_length);
        CHECK(pb_decode_rag_search_request(changed, length + field_length, &decoded_request) == -1);
        CHECK(!decoded_request.user_id[0] && !decoded_request.premium);
    }
    memcpy(changed, wire, length);
    changed[length] = 0x62; /* premium with the wrong wire type */
    changed[length + 1u] = 0;
    CHECK(pb_decode_rag_search_request(changed, length + 2u, &decoded_request) == -1);
    CHECK(pb_decode_rag_search_request(wire, length - 1u, &decoded_request) == -1);
    request.premium = 2;
    CHECK(pb_encode_rag_search_request(wire, sizeof(wire), &request) == 0);
    request.premium = 1;
    memset(request.campaign_id, 'a', sizeof(request.campaign_id));
    CHECK(pb_encode_rag_search_request(wire, sizeof(wire), &request) == 0);

    length = pb_encode_retrieval_citation(wire, sizeof(wire), &result->hits[0].citation);
    CHECK(length > 0u && pb_decode_retrieval_citation(wire, length, &decoded_citation) == 0);
    CHECK(decoded_citation.score == 0.875 && decoded_citation.page_start == 96);
    pb_reader_init(&reader, wire, length);
    while (reader.pos < reader.len) {
        uint32_t field, type;
        size_t start = reader.pos, payload, field_length;
        CHECK(pb_read_tag(&reader, &field, &type) == 0);
        payload = reader.pos;
        CHECK(pb_skip(&reader, type) == 0);
        field_length = reader.pos - start;
        memcpy(changed, wire, length);
        memcpy(changed + length, wire + start, field_length);
        CHECK(pb_decode_retrieval_citation(changed, length + field_length, &decoded_citation) == -1);
        CHECK(!decoded_citation.source[0] && !decoded_citation.source_sha256[0]);
        if (type == 1u) {
            static const uint8_t nan_bits[] = {0, 0, 0, 0, 0, 0, 0xf8, 0x7f};
            memcpy(changed, wire, length);
            memcpy(changed + payload, nan_bits, sizeof(nan_bits));
            CHECK(pb_decode_retrieval_citation(changed, length, &decoded_citation) == -1);
        }
        if (type == 2u) {
            /* Replace one byte inside a string with an embedded NUL. */
            memcpy(changed, wire, length);
            changed[reader.pos - 1u] = 0;
            CHECK(pb_decode_retrieval_citation(changed, length, &decoded_citation) == -1);
        }
    }
    strcpy(response.request_id, "request-a");
    response.used_rag = 1;
    response.documents = *result;
    for (i = 1; i < DND_RAG_HITS_MAX; ++i) {
        response.documents.hits[i] = result->hits[0];
        response.documents.hits[i].citation.record_id[0] = (char)('0' + i);
    }
    response.documents.count = DND_RAG_HITS_MAX;
    length = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    CHECK(length > 0u && pb_decode_rag_search_response(wire, length, &decoded_response) == 0);
    CHECK(decoded_response.documents.count == DND_RAG_HITS_MAX);
    CHECK(strcmp(decoded_response.documents.hits[3].content, result->hits[0].content) == 0);
    CHECK(strcmp(decoded_response.documents.hits[3].citation.source_sha256,
                 result->hits[0].citation.source_sha256) == 0);
    CHECK(pb_decode_rag_search_response(wire, length - 1u, &decoded_response) == -1);
    CHECK(!decoded_response.documents.count && !decoded_response.documents.hits[0].content[0]);
    pb_reader_init(&reader, wire, length);
    while (reader.pos < reader.len) {
        uint32_t field, type;
        size_t start = reader.pos, field_length;
        CHECK(pb_read_tag(&reader, &field, &type) == 0 && pb_skip(&reader, type) == 0);
        field_length = reader.pos - start;
        memcpy(changed, wire, length);
        memcpy(changed + length, wire + start, field_length);
        /* The fifth document rejects before its contents can be published. */
        CHECK(pb_decode_rag_search_response(changed, length + field_length, &decoded_response) == -1);
        CHECK(!decoded_response.request_id[0] && !decoded_response.documents.count);
    }
    response.documents.count = DND_RAG_HITS_MAX + 1u;
    CHECK(pb_encode_rag_search_response(wire, sizeof(wire), &response) == 0);
    response.documents.count = 1u;
    strcpy(response.error, "backend failure");
    CHECK(pb_encode_rag_search_response(wire, sizeof(wire), &response) == 0);
    response.error[0] = '\0';
    response.used_rag = 0;
    CHECK(pb_encode_rag_search_response(wire, sizeof(wire), &response) == 0);
    response.used_rag = 1;
    length = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    CHECK(length > 0u);
    pb_reader_init(&reader, wire, length);
    while (reader.pos < reader.len) {
        uint32_t field, type;
        CHECK(pb_read_tag(&reader, &field, &type) == 0);
        if (field == 2u) {
            const uint8_t *document;
            size_t document_length;
            pb_reader nested;
            CHECK(pb_read_bytes(&reader, &document, &document_length) == 0);
            pb_reader_init(&nested, document, document_length);
            while (nested.pos < nested.len) {
                CHECK(pb_read_tag(&nested, &field, &type) == 0);
                if (field == 2u) {
                    const uint8_t *source;
                    size_t source_length;
                    CHECK(pb_read_bytes(&nested, &source, &source_length) == 0 && source_length > 0u);
                    memcpy(changed, wire, length);
                    changed[(size_t)(source - wire)] = 'x';
                    CHECK(pb_decode_rag_search_response(changed, length, &decoded_response) == -1);
                } else CHECK(pb_skip(&nested, type) == 0);
            }
        } else CHECK(pb_skip(&reader, type) == 0);
    }
}

static void test_grounded_context(const dnd_rag_result *admitted) {
    dnd_rag_result result = *admitted;
    char question[2048], prompt[DND_RAG_PROMPT_CAP];
    char request[(DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1024u], decoded[DND_RAG_PROMPT_CAP];
    char system[DND_GROUNDING_TEXT_CAP];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    cmp_json_object object, message;
    cmp_json_array messages;
    cmp_json_field element;
    size_t i, j, length;
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == 0);
    CHECK(strstr(prompt, admitted->hits[0].content) && strstr(prompt, "Question: Which rule applies?"));
    result.hits[0].content[0] = 'x';
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    result = *admitted;
    result.count = DND_RAG_HITS_MAX;
    memset(question, 'q', sizeof(question) - 1u);
    question[sizeof(question) - 1u] = '\0';
    memcpy(question + sizeof(question) - sizeof("雪?"), "雪?", sizeof("雪?"));
    for (i = 0; i < result.count; ++i) {
        result.hits[i] = admitted->hits[0];
        result.hits[i].citation.record_id[0] = (char)('0' + i);
        /* Exercise worst-case JSON escaping at the complete admitted text bound. */
        memset(result.hits[i].citation.book_slug, 'a', sizeof(result.hits[i].citation.book_slug) - 1u);
        result.hits[i].citation.book_slug[sizeof(result.hits[i].citation.book_slug) - 1u] = '\0';
        memset(result.hits[i].citation.section, 1, sizeof(result.hits[i].citation.section) - 1u);
        result.hits[i].citation.section[sizeof(result.hits[i].citation.section) - 1u] = '\0';
        memset(result.hits[i].content, 1, sizeof(result.hits[i].content) - 1u);
        result.hits[i].content[sizeof(result.hits[i].content) - 1u] = '\0';
        CHECK(SHA256((const unsigned char *)result.hits[i].content,
            strlen(result.hits[i].content), digest) != NULL);
        for (j = 0; j < sizeof(digest); ++j)
            CHECK(snprintf(result.hits[i].citation.content_hash + j * 2u, 3u, "%02x", (unsigned)digest[j]) == 2);
    }
    CHECK(dnd_rag_grounded_prompt(&result, question, prompt, sizeof(prompt)) == 0);
    CHECK(strlen(prompt) > 32768u && strstr(prompt, question) != NULL);
    length = openai_chat_request_json_stream_bounded_span_limit(request, sizeof(request),
        "local-model", prompt, strlen(prompt), 48u, sizeof(prompt) - 1u);
    CHECK(length > 196608u && length < sizeof(request));
    CHECK(cmp_json_object_parse(request, &object));
    CHECK(cmp_json_field_array(cmp_json_object_field(&object, "messages"), &messages));
    CHECK(cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message));
    CHECK(cmp_json_object_str(&message, "content", decoded, sizeof(decoded)) && strcmp(decoded, prompt) == 0);
    memset(system, 1, sizeof(system) - 1u); system[sizeof(system) - 1u] = '\0';
    length = openai_chat_request_json_stream_system(request, sizeof(request), "local-model",
        system, sizeof(system) - 1u, prompt, strlen(prompt), 48u, sizeof(prompt) - 1u);
    CHECK(length > 196608u && length < sizeof(request) && cmp_json_object_parse(request, &object));
    CHECK(cmp_json_field_array(cmp_json_object_field(&object, "messages"), &messages));
    CHECK(cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message));
    CHECK(cmp_json_object_str(&message, "content", decoded, sizeof(decoded)) && strcmp(decoded, system) == 0);
    CHECK(cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message));
    CHECK(cmp_json_object_str(&message, "content", decoded, sizeof(decoded)) && strcmp(decoded, prompt) == 0);
    CHECK(cmp_json_array_next(&messages, &element) == 0);
    CHECK(openai_chat_request_json_stream_system(request, sizeof(request), "local-model",
        system, sizeof(system), prompt, strlen(prompt), 48u, sizeof(prompt) - 1u) == 0 && !request[0]);
    CHECK(openai_chat_request_json_stream_bounded_span_limit(request, 4096u,
        "local-model", prompt, strlen(prompt), 48u, sizeof(prompt) - 1u) == 0 && !request[0]);
    CHECK(openai_chat_request_json_stream_bounded_span_limit(request, sizeof(request),
        "local-model", prompt, strlen(prompt), 48u, strlen(prompt) - 1u) == 0 && !request[0]);
    CHECK(dnd_rag_grounded_prompt(&result, question, prompt, 4096u) == -1 && !prompt[0]);
}

static void test_exact_excerpts(const dnd_rag_result *admitted) {
    dnd_rag_result result = *admitted;
    dnd_rag_hit *hit = &result.hits[0];
    dnd_rag_citation roundtrip;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char prompt[DND_RAG_PROMPT_CAP];
    uint8_t wire[8192];
    strcpy(hit->content, "Neighbor. Élan protects you. Excluded text. End rule.");
    CHECK(SHA256((const unsigned char *)hit->content, strlen(hit->content), digest));
    for (size_t j = 0; j < sizeof(digest); ++j)
        CHECK(snprintf(hit->citation.content_hash + j * 2u, 3u, "%02x", (unsigned)digest[j]) == 2);
    hit->citation.excerpt = (dnd_rag_excerpt){.count = 2, .spans = {{10, 28}, {44, 53}}};
    /* Derive these fixture offsets from exact UTF-8 bytes, not character counts. */
    hit->citation.excerpt.spans[0].begin = (uint32_t)(strstr(hit->content, "Élan") - hit->content);
    hit->citation.excerpt.spans[0].end = (uint32_t)(strstr(hit->content, " Excluded") - hit->content);
    hit->citation.excerpt.spans[1].begin = (uint32_t)(strstr(hit->content, "End rule") - hit->content);
    hit->citation.excerpt.spans[1].end = (uint32_t)strlen(hit->content);
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == 0);
    CHECK(strstr(prompt, "Élan protects you.\n[Omitted source text]\nEnd rule.") &&
        !strstr(prompt, "Neighbor") && !strstr(prompt, "Excluded"));
    size_t length = pb_encode_retrieval_citation(wire, sizeof(wire), &hit->citation);
    CHECK(length && pb_decode_retrieval_citation(wire, length, &roundtrip) == 0 &&
        memcmp(&roundtrip.excerpt, &hit->citation.excerpt, sizeof(roundtrip.excerpt)) == 0);
    dnd_rag_excerpt valid = hit->citation.excerpt;
    ++hit->citation.excerpt.spans[0].begin;
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    hit->citation.excerpt = valid;
    hit->content[0] = 'X'; /* Excluded bytes still belong to the record hash. */
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    hit->content[0] = 'N';
    for (size_t fault = 0; fault < 5; ++fault) {
        hit->citation.excerpt = valid;
        if (fault == 0) hit->citation.excerpt.count = DND_RAG_EXCERPT_SPANS_MAX + 1u;
        if (fault == 1) hit->citation.excerpt.spans[0].end = hit->citation.excerpt.spans[0].begin;
        if (fault == 2) hit->citation.excerpt.spans[1].begin = hit->citation.excerpt.spans[0].end;
        if (fault == 3) hit->citation.excerpt.spans[1].begin = 0;
        if (fault == 4) hit->citation.excerpt.spans[1].end = DND_RAG_CONTENT_CAP;
        CHECK(!pb_encode_retrieval_citation(wire, sizeof(wire), &hit->citation));
        CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    }
    hit->citation.excerpt = valid;
    ++hit->citation.excerpt.spans[1].end; /* Wire bound fits, but actual content does not. */
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule applies?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
}

static void hash_hit(dnd_rag_hit *hit) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    CHECK(SHA256((const unsigned char *)hit->content, strlen(hit->content), digest));
    for (size_t i = 0; i < sizeof(digest); ++i)
        CHECK(snprintf(hit->citation.content_hash + i * 2u, 3u, "%02x", (unsigned)digest[i]) == 2);
}

static void test_complete_passage_context(const dnd_rag_result *admitted) {
    static dnd_rag_result result;
    static rag_search_response_c response, decoded;
    static char prompt[DND_RAG_PROMPT_CAP], json[DND_RAG_PUBLIC_JSON_CAP];
    static uint8_t wire[DND_RAG_WIRE_CAP], provenance[DND_RAG_CITATIONS_WIRE_CAP];
    result = *admitted; result.count = 1;
    dnd_rag_hit *hit = &result.hits[0];
    dnd_rag_citation *c = &hit->citation;
    c->record_id[0] = 0; memset(c->passage_id, 'a', 64); c->passage_id[64] = 0;
    c->score = 0; c->score_metric = DND_RAG_SCORE_NONE; c->chunk_index = 0;
    c->page_start = 1; c->page_end = 2; c->witness_count = 2;
    c->excerpt.count = 0;
    strcpy(hit->content, "Élan\nprotects.");
    for (size_t i = 0; i < 2; ++i) {
        dnd_rag_passage_witness *w = &c->witnesses[i];
        memset(w->record_id, (int)('b' + i), 64); w->record_id[64] = 0;
        memset(w->content_hash, 'd', 64); w->content_hash[64] = 0;
        w->page = (uint32_t)i + 1u; w->chunk = 0; w->begin = 0;
        w->end = i ? 9u : 5u; w->record_length = w->end;
    }
    hash_hit(hit);
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule?", prompt, sizeof(prompt)) == 0);
    CHECK(strstr(prompt, "Élan\nprotects.") && strstr(prompt, "original records 2"));
    hit->content[0] = 'x';
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    strcpy(hit->content, "Élan protects."); hash_hit(hit); /* Rehashed, but missing the required page separator. */
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule?", prompt, sizeof(prompt)) == -1 && !prompt[0]);
    strcpy(hit->content, "Élan\nprotects."); hash_hit(hit);
    c->page_end = 1; c->witnesses[1].page = 1; c->witnesses[1].chunk = 1;
    c->witnesses[0].end = 1; c->witnesses[1].end = 14; c->witnesses[1].record_length = 14;
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule?", prompt, sizeof(prompt)) == -1 && !prompt[0]); /* UTF-8 split. */
    c->witnesses[0].end = 5; c->witnesses[1].end = 9;
    CHECK(dnd_rag_grounded_prompt(&result, "Which rule?", prompt, sizeof(prompt)) == -1 && !prompt[0]); /* Wrong assembly length. */

    /* Four complete 4096-byte passages, each backed by sixteen distinct records. */
    for (size_t i = 0; i < DND_RAG_HITS_MAX; ++i) {
        result.hits[i] = *hit;
        dnd_rag_hit *h = &result.hits[i]; dnd_rag_citation *v = &h->citation;
        v->passage_id[0] = (char)('0' + i); v->witness_count = DND_RAG_PASSAGE_WITNESSES_MAX;
        memset(h->content, 'x', DND_RAG_PASSAGE_BYTES_MAX); h->content[DND_RAG_PASSAGE_BYTES_MAX] = 0;
        for (size_t j = 0; j < v->witness_count; ++j) {
            dnd_rag_passage_witness *w = &v->witnesses[j];
            *w = v->witnesses[0];
            CHECK(snprintf(w->record_id, sizeof(w->record_id), "%064zu", i * 16u + j) == 64);
            w->page = 1; w->chunk = (uint32_t)j; w->begin = 0; w->end = 256; w->record_length = 256;
        }
        hash_hit(h);
    }
    result.count = DND_RAG_HITS_MAX;
    CHECK(dnd_rag_grounded_prompt(&result, "Which rules?", prompt, sizeof(prompt)) == 0);
    response.documents = result; response.used_rag = 1; strcpy(response.request_id, "passage-turn");
    size_t n = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    CHECK(n && !pb_decode_rag_search_response(wire, n, &decoded) && decoded.documents.count == 4);
    CHECK(decoded.documents.hits[3].citation.witness_count == 16);
    turn_retrieval_c view; turn_event_c event; turn_response_event safe;
    turn_response_state state = {0};
    n = pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result);
    CHECK(n && !pb_decode_retrieval_provenance(provenance, n, &view));
    n = pb_encode_turn_text_event(wire, sizeof(wire), "passage-turn", "text_completed", "Rule.", "Rule.", "Rule.", 0, 1);
    n = pb_append_turn_retrieval(wire, sizeof(wire), n, &view);
    CHECK(n && !pb_decode_turn_event(wire, n, &event));
    CHECK(turn_response_filter_decoded(&state, &event, &safe) == TURN_RESPONSE_FORWARD);
    CHECK(turn_response_json_encode(json, sizeof(json), &safe, 123));
    CHECK(strstr(json, "complete_passage") && strstr(json, "witnesses"));
    result.hits[1].citation = result.hits[0].citation;
    CHECK(!pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result));
}

static void test_canonical_grounding(const dnd_rag_result *admitted) {
    dnd_grounding_prompt prompt, missing;
    dnd_grounding_identity identity;
    turn_retrieval_c view;
    turn_event_c event;
    turn_response_event safe;
    turn_response_state state = {0};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    uint8_t wire[32768], changed[32768], event_wire[32768];
    char source[DND_GROUNDING_TEXT_CAP], system[DND_GROUNDING_TEXT_CAP], user[DND_RAG_PROMPT_CAP];
    char request[(DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1024u], value[256];
    FILE *file = fopen("../../contracts/prompt-library/prompts/rag/grounded-response.system.txt", "rb");
    cmp_json_object object, message, metadata;
    cmp_json_array messages;
    cmp_json_field element;
    size_t length, source_len, base_len, entry_len, i;
    CHECK(file != NULL);
    source_len = fread(source, 1u, sizeof(source) - 1u, file);
    CHECK(feof(file) && !ferror(file) && fclose(file) == 0 && source_len > 0u);
    while (source_len && (source[source_len - 1u] == '\n' || source[source_len - 1u] == '\r')) --source_len;
    source[source_len] = '\0';
    CHECK(dnd_grounding_load(NULL, &prompt) == 0 && prompt.text_len == source_len &&
        strcmp(prompt.text, source) == 0 && strcmp(prompt.identity.version, "v4") == 0);
    {
        voice_response_prompts response, absent;
        dnd_grounding_prompt oversized = prompt;
        CHECK(voice_response_prompts_load(NULL, &prompt, &response) == 0);
        CHECK(response.direct_len > 0u && response.grounded_len == response.direct_len + 2u + source_len);
        CHECK(memcmp(response.grounded, response.direct, response.direct_len) == 0);
        CHECK(memcmp(response.grounded + response.direct_len, "\n\n", 2u) == 0);
        CHECK(strcmp(response.grounded + response.direct_len + 2u, source) == 0);
  CHECK(strcmp(response.version, "v2") == 0 && strlen(response.sha256) == 64u);
        CHECK(strcmp(response.dnd_version, "v2") == 0 && strlen(response.dnd_sha256) == 64u);
        CHECK(response.dnd_direct_len > response.direct_len &&
            !memcmp(response.dnd_direct, response.direct, response.direct_len));
        CHECK(strstr(response.dnd_direct, "Dungeons & Dragons") != NULL);
        CHECK(response.dnd_grounded_len == response.dnd_direct_len + 2u + source_len &&
            !strcmp(response.dnd_grounded + response.dnd_direct_len + 2u, source));
        CHECK(voice_response_prompts_load("/missing-response-style", &prompt, &absent) == -1 &&
            !absent.direct_len && !absent.grounded_len && !absent.sha256[0]);
        memset(oversized.text, 'x', sizeof(oversized.text) - 1u);
        oversized.text[sizeof(oversized.text) - 1u] = '\0';
        oversized.text_len = sizeof(oversized.text) - 1u;
        CHECK(voice_response_prompts_load(NULL, &oversized, &absent) == -1 && !absent.direct[0]);
        oversized = prompt;
        oversized.text[1] = '\0';
        CHECK(voice_response_prompts_load(NULL, &oversized, &absent) == -1 && !absent.grounded[0]);
        CHECK(voice_response_prompts_load(NULL, NULL, &absent) == -1);
    }
    CHECK(SHA256((const unsigned char *)source, source_len, digest) != NULL);
    for (i = 0; i < sizeof(digest); ++i) {
        char hex[3];
        CHECK(snprintf(hex, sizeof(hex), "%02x", (unsigned)digest[i]) == 2);
        CHECK(memcmp(prompt.identity.sha256 + i * 2u, hex, 2u) == 0);
    }
    memset(&missing, 0xa5, sizeof(missing));
    CHECK(dnd_grounding_load("/nonexistent-c-grounding-fixture", &missing) == -1 &&
        !missing.text_len && !missing.identity.id[0] && !missing.text[0]);
    CHECK(dnd_rag_grounded_prompt(admitted, "Which rule applies?", user, sizeof(user)) == 0 &&
        strstr(user, "name players-handbook;") && strstr(user, "section Sneak Attack;"));
    length = openai_chat_request_json_stream_system(request, sizeof(request), "model",
        prompt.text, prompt.text_len, user, strlen(user), 48u, sizeof(user) - 1u);
    CHECK(length && cmp_json_object_parse(request, &object) &&
        cmp_json_field_array(cmp_json_object_field(&object, "messages"), &messages));
    CHECK(cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message));
    CHECK(cmp_json_object_str(&message, "role", value, sizeof(value)) && strcmp(value, "system") == 0);
    CHECK(cmp_json_object_str(&message, "content", system, sizeof(system)) && strcmp(system, source) == 0);
    CHECK(cmp_json_array_next(&messages, &element) == 1 && cmp_json_field_object(&element, &message));
    CHECK(cmp_json_object_str(&message, "role", value, sizeof(value)) && strcmp(value, "user") == 0);
    CHECK(cmp_json_object_str(&message, "content", user, sizeof(user)) && strstr(user, admitted->hits[0].content));
    CHECK(cmp_json_array_next(&messages, &element) == 0);
    CHECK(openai_chat_request_json_stream_system(request, length, "model", prompt.text,
        prompt.text_len, user, strlen(user), 48u, sizeof(user) - 1u) == 0 && !request[0]);
    CHECK(openai_chat_request_json_stream_system(request, length + 1u, "model", prompt.text,
        prompt.text_len, user, strlen(user), 48u, sizeof(user) - 1u) == length);
    CHECK(openai_chat_request_json_stream_system(request, sizeof(request), "model", "a\0b", 3u,
        user, strlen(user), 48u, sizeof(user) - 1u) == 0 && !request[0]);
    CHECK(openai_chat_request_json_stream_system(request, sizeof(request), "model", "\xff", 1u,
        user, strlen(user), 48u, sizeof(user) - 1u) == 0 && !request[0]);
    base_len = pb_encode_retrieval_provenance(wire, sizeof(wire), admitted);
    CHECK(base_len && pb_decode_retrieval_provenance(wire, base_len, &view) == 0 &&
        pb_retrieval_grounding(&view, &identity) == 0);
    length = pb_encode_grounded_retrieval_provenance(wire, sizeof(wire), admitted, &prompt.identity);
    CHECK(length > base_len && pb_decode_retrieval_provenance(wire, length, &view) == 0 &&
        pb_retrieval_grounding(&view, &identity) == 1 && memcmp(&identity, &prompt.identity, sizeof(identity)) == 0);
    entry_len = length - base_len;
    CHECK(wire[base_len] == 0x12u && wire[base_len + 1u] < 120u);
    memcpy(changed, wire, length); memcpy(changed + length, wire + base_len, entry_len);
    CHECK(pb_decode_retrieval_provenance(changed, length + entry_len, &view) == -1 && !view.data);
    memcpy(changed, wire, length); changed[base_len + 1u] += 4u;
    memcpy(changed + length, "\x12\x02v3", 4u);
    CHECK(pb_decode_retrieval_provenance(changed, length + 4u, &view) == -1 && !view.data);
    memcpy(changed, wire, length); changed[base_len + 1u] += 2u;
    changed[length] = 0x20u; changed[length + 1u] = 0u;
    CHECK(pb_decode_retrieval_provenance(changed, length + 2u, &view) == -1 && !view.data);
    for (i = base_len + 1u; i < length; ++i)
        CHECK(pb_decode_retrieval_provenance(wire, i, &view) == -1 && !view.data);
    memcpy(changed, wire, length); changed[length - 1u] = 0u;
    CHECK(pb_decode_retrieval_provenance(changed, length, &view) == -1 && !view.data);
    changed[length - 1u] = 'g';
    CHECK(pb_decode_retrieval_provenance(changed, length, &view) == -1 && !view.data);
    CHECK(pb_decode_retrieval_provenance(wire, length, &view) == 0);
    length = pb_encode_turn_text_event(event_wire, sizeof(event_wire), "grounded-turn", "text_completed",
        "Rule [1].", "Rule [1].", "Rule [1].", 0, 1);
    length = pb_append_turn_retrieval(event_wire, sizeof(event_wire), length, &view);
    CHECK(length && pb_decode_turn_event(event_wire, length, &event) == 0 &&
        turn_response_filter_decoded(&state, &event, &safe) == TURN_RESPONSE_FORWARD);
    CHECK(turn_response_json_encode(request, sizeof(request), &safe, 123) &&
        cmp_json_object_parse(request, &object) && cmp_json_object_object(&object, "metadata", &metadata));
    CHECK(cmp_json_object_str(&metadata, "cascade_grounding_prompt_id", value, sizeof(value)) &&
        strcmp(value, prompt.identity.id) == 0);
    CHECK(cmp_json_object_str(&metadata, "cascade_grounding_prompt_version", value, sizeof(value)) &&
        strcmp(value, prompt.identity.version) == 0);
    CHECK(cmp_json_object_str(&metadata, "cascade_grounding_prompt_sha256", value, sizeof(value)) &&
        strcmp(value, prompt.identity.sha256) == 0);
}

static void check_public_citation_json(
    const char *json, const dnd_rag_result *expected
) {
    cmp_json_object root, metadata, object;
    cmp_json_array array;
    cmp_json_field element;
    char encoded[DND_RAG_PUBLIC_JSON_CAP], value[2048];
    size_t count = 0;
    int rc;
    CHECK(cmp_json_object_parse(json, &root));
    CHECK(cmp_json_object_object(&root, "metadata", &metadata));
    CHECK(cmp_json_object_str(&metadata, "cascade_retrieved_documents", value, sizeof(value)));
    CHECK(value[0] == (char)('0' + expected->count) && value[1] == '\0');
    CHECK(cmp_json_object_str(&metadata, "cascade_retrieval_citations", encoded, sizeof(encoded)));
    CHECK(cmp_json_array_parse(encoded, &array));
    while ((rc = cmp_json_array_next(&array, &element)) == 1) {
        const dnd_rag_citation *citation;
        double score;
        CHECK(count < expected->count && cmp_json_field_object(&element, &object));
        citation = &expected->hits[count++].citation;
#define CHECK_PUBLIC_STRING(member) \
        CHECK(cmp_json_object_str(&object, #member, value, sizeof(value)) && strcmp(value, citation->member) == 0)
        CHECK_PUBLIC_STRING(source); CHECK_PUBLIC_STRING(book_slug); CHECK_PUBLIC_STRING(collection);
        CHECK_PUBLIC_STRING(corpus_version); CHECK_PUBLIC_STRING(embedding_model); CHECK_PUBLIC_STRING(record_id);
        CHECK_PUBLIC_STRING(document_id); CHECK_PUBLIC_STRING(content_hash); CHECK_PUBLIC_STRING(source_sha256);
        CHECK_PUBLIC_STRING(section);
#undef CHECK_PUBLIC_STRING
        CHECK(cmp_json_field_double(cmp_json_object_field(&object, "score"), &score) && score == citation->score);
        CHECK(cmp_json_object_str(&object, "score_metric", value, sizeof(value)) &&
            strcmp(value, dnd_rag_score_metric_name(citation->score_metric)) == 0);
        CHECK(cmp_json_field_double(cmp_json_object_field(&object, "page_start"), &score) && score == citation->page_start);
        CHECK(cmp_json_field_double(cmp_json_object_field(&object, "page_end"), &score) && score == citation->page_end);
        CHECK(cmp_json_field_double(cmp_json_object_field(&object, "chunk_index"), &score) && score == citation->chunk_index);
        const cmp_json_field *ranges = cmp_json_object_field(&object, "excerpt_spans");
        if (!citation->excerpt.count) CHECK(!ranges);
        else {
            cmp_json_array spans;
            CHECK(ranges && cmp_json_field_array(ranges, &spans));
            for (size_t j = 0; j < citation->excerpt.count; ++j) {
                cmp_json_field span;
                cmp_json_object bounds;
                CHECK(cmp_json_array_next(&spans, &span) == 1 && cmp_json_field_object(&span, &bounds));
                CHECK(cmp_json_field_double(cmp_json_object_field(&bounds, "begin"), &score) &&
                    score == citation->excerpt.spans[j].begin);
                CHECK(cmp_json_field_double(cmp_json_object_field(&bounds, "end"), &score) &&
                    score == citation->excerpt.spans[j].end);
            }
            cmp_json_field extra;
            CHECK(cmp_json_array_next(&spans, &extra) == 0);
        }
    }
    CHECK(rc == 0 && count == expected->count);
}

static void test_public_provenance(const dnd_rag_result *admitted) {
    dnd_rag_result result = *admitted;
    dnd_grounding_prompt prompt;
    dnd_grounding_identity identity;
    turn_retrieval_c view;
    turn_event_c event, decoded;
    turn_response_event safe;
    turn_response_state state = {0};
    uint8_t provenance[DND_RAG_CITATIONS_WIRE_CAP], wire[32768], public_wire[32768];
    char json[DND_RAG_PUBLIC_JSON_CAP];
    size_t length, base_len, wire_len, public_len, i;
    length = pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result);
    CHECK(length && pb_decode_retrieval_provenance(provenance, length, &view) == 0);
    CHECK(view.data == provenance && view.length == length && view.count == 1u);
    {
        pb_reader reader;
        const uint8_t *citation;
        size_t citation_len;
        uint32_t field, type;
        turn_retrieval_c rejected;
        pb_reader_init(&reader, provenance, length);
        CHECK(pb_read_tag(&reader, &field, &type) == 0 && field == 1u && type == 2u);
        CHECK(pb_read_bytes(&reader, &citation, &citation_len) == 0 && citation_len > 127u && citation_len + 2u < 16384u);
        public_wire[0] = 0x0au;
        public_wire[1] = (uint8_t)(((citation_len + 3u) & 127u) | 128u);
        public_wire[2] = (uint8_t)((citation_len + 3u) >> 7u);
        memcpy(public_wire + 3u, citation, citation_len);
        public_wire[citation_len + 3u] = 0x80u;
        public_wire[citation_len + 4u] = 1u;
        public_wire[citation_len + 5u] = 1u;
        CHECK(pb_decode_retrieval_provenance(public_wire, citation_len + 6u, &rejected) == -1 && !rejected.data);
    }
    base_len = pb_encode_turn_text_event(wire, sizeof(wire), "citation-turn", "text_completed",
        "Rule [1].", "private speech", "Rule [1].", 0, 1);
    wire_len = pb_append_turn_retrieval(wire, sizeof(wire), base_len, &view);
    CHECK(wire_len && pb_decode_turn_event_public_active(wire, wire_len,
        "citation-turn", sizeof("citation-turn") - 1u, &event) == 0);
    CHECK(!event.speech_text[0] && !event.text[0] && event.retrieval.count == 1u);
    CHECK(turn_response_filter_public_active_n(&state, &event, "citation-turn",
        sizeof("citation-turn") - 1u, &safe) == TURN_RESPONSE_FORWARD);
    CHECK(safe.retrieval && safe.retrieval->count == 1u);
    public_len = turn_response_protobuf_encode(public_wire, sizeof(public_wire), &safe);
    CHECK(public_len && pb_decode_turn_event(public_wire, public_len, &decoded) == 0 && decoded.retrieval.count == 1u);
    CHECK(turn_response_json_encode(json, sizeof(json), &safe, 123) > 0u);
    check_public_citation_json(json, &result);
    CHECK(!strstr(json, admitted->hits[0].content) && !strstr(json, "private speech"));
    CHECK(pb_decode_turn_event_bound(wire, wire_len, "other-turn", sizeof("other-turn") - 1u, &decoded) == -1);
    public_len = pb_append_turn_retrieval(wire, sizeof(wire), wire_len, &view);
    CHECK(public_len && pb_decode_turn_event(wire, public_len, &decoded) == -1);
    wire[base_len] = 0xa8u; wire[base_len + 1u] = 1u; wire[base_len + 2u] = 1u;
    CHECK(pb_decode_turn_event(wire, base_len + 3u, &decoded) == -1);
    memcpy(provenance + length, provenance, length);
    CHECK(pb_decode_retrieval_provenance(provenance, length * 2u, &view) == -1 && !view.data);
    CHECK(pb_decode_retrieval_provenance(provenance, 0, &view) == -1 && !view.data);
    CHECK(pb_decode_retrieval_provenance(provenance, length - 1u, &view) == -1 && !view.data);
    result.count = 2u; result.hits[1] = result.hits[0];
    CHECK(pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result) == 0);
    result.count = DND_RAG_HITS_MAX + 1u;
    CHECK(pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result) == 0);
    /* Five distinct wire entries must fail even when each entry is valid. */
    result = *admitted;
    length = 0;
    for (i = 0; i <= DND_RAG_HITS_MAX; ++i) {
        size_t entry_len;
        result.hits[0].citation.record_id[0] = (char)('0' + i);
        entry_len = pb_encode_retrieval_provenance(public_wire, sizeof(public_wire), &result);
        CHECK(entry_len && entry_len <= sizeof(provenance) - length);
        memcpy(provenance + length, public_wire, entry_len);
        length += entry_len;
    }
    CHECK(pb_decode_retrieval_provenance(provenance, length, &view) == -1 && !view.data);

    /* Full wire bounds, including prompt identity and nested escapes, survive both JSON layers. */
    result.count = DND_RAG_HITS_MAX;
    for (i = 0; i < result.count; ++i) {
        dnd_rag_citation *citation = &result.hits[i].citation;
        result.hits[i] = admitted->hits[0];
        citation->record_id[0] = (char)('0' + i);
        memset(citation->source, '"', sizeof(citation->source) - 1u);
        memcpy(citation->source, "s3://b/", 7u);
        citation->source[sizeof(citation->source) - 1u] = '\0';
        memset(citation->section, 1, sizeof(citation->section) - 1u);
        citation->section[sizeof(citation->section) - 1u] = '\0';
        memset(citation->embedding_model, 1, sizeof(citation->embedding_model) - 1u);
        citation->embedding_model[sizeof(citation->embedding_model) - 1u] = '\0';
        citation->chunk_index = (int32_t)i;
    }
    CHECK(dnd_grounding_load(NULL, &prompt) == 0);
    length = pb_encode_grounded_retrieval_provenance(provenance, sizeof(provenance), &result, &prompt.identity);
    CHECK(length && pb_decode_retrieval_provenance(provenance, length, &view) == 0 &&
        pb_retrieval_grounding(&view, &identity) == 1 &&
        strcmp(identity.sha256, prompt.identity.sha256) == 0);
    base_len = pb_encode_turn_text_event(wire, sizeof(wire), "citation-turn", "text_completed",
        "Rule [1].", "Rule [1].", "Rule [1].", 0, 1);
    wire_len = pb_append_turn_retrieval(wire, sizeof(wire), base_len, &view);
    CHECK(wire_len && pb_decode_turn_event(wire, wire_len, &event) == 0);
    state = (turn_response_state){0};
    CHECK(turn_response_filter_decoded(&state, &event, &safe) == TURN_RESPONSE_FORWARD);
    length = turn_response_json_encode(json, sizeof(json), &safe, 123);
    CHECK(length > 65536u && length < sizeof(json));
    check_public_citation_json(json, &result);
    CHECK(strstr(json, prompt.identity.sha256) != NULL);
    CHECK(turn_response_json_encode(json, length, &safe, 123) == 0 && !json[0]);
    CHECK(turn_response_json_encode(json, length + 1u, &safe, 123) == length);
    safe.type_id = 4;
    CHECK(turn_response_json_encode(json, sizeof(json), &safe, 123) == 0 && !json[0]);
    CHECK(turn_response_protobuf_encode(public_wire, sizeof(public_wire), &safe) == 0);
    event.type_id = 8;
    state = (turn_response_state){0};
    CHECK(turn_response_filter_decoded(&state, &event, &safe) == TURN_RESPONSE_REJECT);

    result = *admitted;
    result.hits[0].citation.section[0] = (char)0xff;
    length = pb_encode_retrieval_provenance(provenance, sizeof(provenance), &result);
    CHECK(length && pb_decode_retrieval_provenance(provenance, length, &view) == 0);
    CHECK(!turn_retrieval_valid(&view));
    /* A later ordinary event must clear a previous borrowed provenance view. */
    length = pb_encode_turn_event(wire, sizeof(wire), "citation-turn", "thinking_started", "");
    CHECK(length && pb_decode_turn_event_public_active(wire, length,
        "citation-turn", sizeof("citation-turn") - 1u, &event) == 0 && !event.retrieval.data);
}

static void test_ruleset_policy(const char *valid) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    dnd_rag_result result, empty = {0};
    char admitted[16384], changed[32768], body[32768];
    const char *invalid[] = {"DND-5e", "-dnd", "dnd_5e", "dnd\" or true", "dnd 5e", "dnd\n", "\xff"};
    static const struct { const char *from; const char *to; } mutations[] = {
        {"\"ruleset\":\"dnd-5e-2014\",", ""},
        {"\"ruleset\":\"dnd-5e-2014\"", "\"ruleset\":\"lotr-5e\""},
        {"\"ruleset\":\"dnd-5e-2014\"", "\"ruleset\":\"dnd-5e-2014\",\"ruleset\":\"dnd-5e-2014\""},
        {"\"ruleset\":\"dnd-5e-2014\"", "\"ruleset\":\"dnd-5e-2014\\u0000\""},
        {"\\\"ruleset\\\":\\\"dnd-5e-2014\\\"", "\\\"ruleset\\\":\\\"lotr-5e\\\""},
        {"\\\"ruleset\\\":\\\"dnd-5e-2014\\\"", "\\\"ruleset\\\":null"},
        {"\\\"ruleset\\\":\\\"dnd-5e-2014\\\",", ""},
        {"\\\"ruleset\\\":\\\"dnd-5e-2014\\\"", "\\\"ruleset\\\":\\\"dnd-5e-2014\\\",\\\"ruleset\\\":\\\"dnd-5e-2014\\\""}
    };
    strcpy(config.shared_rulebook_ruleset, "dnd-5e-2014");
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(strstr(scope.filter, " and ruleset == \"dnd-5e-2014\"") != NULL);
    CHECK(strcmp(scope.ruleset, "dnd-5e-2014") == 0);
    strcpy(admitted, valid);
    replace_one(admitted, sizeof(admitted), "\"metadata_json\":", "\"ruleset\":\"dnd-5e-2014\",\"metadata_json\":");
    for (unsigned metric = 0; metric <= 1u; ++metric) {
        config.shared_rulebook_metric = (dnd_rag_score_metric)metric;
        CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
        if (metric)
            CHECK(dnd_rag_lexical_search_body(&config, &scope, "Sneak Attack", 4u, body, sizeof(body)) == 0);
        else
            CHECK(dnd_rag_search_body(&config, &scope, "[0.1,0.2,0.3]", 4u, body, sizeof(body)) == 0);
        CHECK(strstr(body, "\"metadata_json\",\"ruleset\"]") != NULL);
        CHECK(dnd_rag_search_extract(&config, &scope, admitted, 4u, &result) == 0 && result.count == 1u);
        for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
            strcpy(changed, admitted);
            replace_one(changed, sizeof(changed), mutations[i].from, mutations[i].to);
            memset(&result, 0xa5, sizeof(result));
            CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
            CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
        }
        strcpy(changed, admitted);
        replace_all(changed, sizeof(changed), "dnd-5e-2014", "lotr-5e");
        CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
        scope.ruleset[0] = '\0';
        CHECK(dnd_rag_search_extract(&config, &scope, admitted, 4u, &result) == -1);
    }
    CHECK(dnd_rag_lexical_search_body(&config, &scope, "rule", 4u, body, sizeof(body)) == -1);
    CHECK(!body[0]);
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    strcpy(scope.ruleset, "lotr-5e");
    CHECK(dnd_rag_lexical_search_body(&config, &scope, "rule", 4u, body, sizeof(body)) == -1);
    identity.knowledge_scope = "campaign_canon";
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0 && !scope.ruleset[0]);
    CHECK(strstr(scope.filter, "ruleset") == NULL);
    CHECK(dnd_rag_search_body(&config, &scope, "[0.1,0.2,0.3]", 4u, body, sizeof(body)) == 0);
    CHECK(strstr(body, "\"ruleset\"") == NULL);
    identity.knowledge_scope = "shared_rulebook";
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(config.shared_rulebook_ruleset, invalid[i]);
        CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1 && !scope.filter[0]);
    }
    memset(config.shared_rulebook_ruleset, 'a', sizeof(config.shared_rulebook_ruleset));
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
    config.shared_rulebook_ruleset[0] = '\0';
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0 && !scope.ruleset[0]);
    CHECK(dnd_rag_search_extract(&config, &scope, valid, 4u, &result) == 0);
}

static void test_bm25_scores(const char *valid) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "", "", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    dnd_rag_result result;
    dnd_rag_citation decoded;
    rag_search_response_c response = {0}, roundtrip;
    uint8_t wire[DND_RAG_WIRE_CAP];
    char changed[32768];
    size_t length;
    config.shared_rulebook_metric = DND_RAG_SCORE_BM25;
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    strcpy(changed, valid);
    replace_one(changed, sizeof(changed), "\"distance\":0.875", "\"distance\":42.25");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0);
    CHECK(result.hits[0].citation.score == 42.25 && result.hits[0].citation.score_metric == DND_RAG_SCORE_BM25);
    config.shared_rulebook_metric = DND_RAG_SCORE_COSINE;
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
    config.shared_rulebook_metric = DND_RAG_SCORE_BM25;
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0);
    test_public_provenance(&result);
    {
        dnd_rag_result sliced = result;
        sliced.hits[0].citation.excerpt = (dnd_rag_excerpt){.count = 2, .spans = {{0, 5}, {10, 20}}};
        test_public_provenance(&sliced);
    }
    strcpy(response.request_id, "bm25-wire");
    response.documents = result;
    response.used_rag = 1;
    length = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    CHECK(length && pb_decode_rag_search_response(wire, length, &roundtrip) == 0);
    CHECK(roundtrip.documents.hits[0].citation.score == 42.25 &&
        roundtrip.documents.hits[0].citation.score_metric == DND_RAG_SCORE_BM25);
    length = pb_encode_retrieval_citation(wire, sizeof(wire), &result.hits[0].citation);
    CHECK(length > 2u && wire[length - 2u] == 0x78u && wire[length - 1u] == 1u);
    CHECK(pb_decode_retrieval_citation(wire, length - 1u, &decoded) == -1);
    wire[length - 1u] = 0u;
    CHECK(pb_decode_retrieval_citation(wire, length, &decoded) == -1);
    wire[length - 1u] = 2u;
    CHECK(pb_decode_retrieval_citation(wire, length, &decoded) == -1);
    wire[length - 1u] = 1u;
    wire[length] = 0x78u; wire[length + 1u] = 1u;
    CHECK(pb_decode_retrieval_citation(wire, length + 2u, &decoded) == -1);
    result.hits[0].citation.score = -0.5;
    CHECK(pb_encode_retrieval_citation(wire, sizeof(wire), &result.hits[0].citation) == 0);
    replace_one(changed, sizeof(changed), "\"distance\":42.25", "\"distance\":-0.5");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
}

static void test_record_queries(const char *valid) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "", "", "", "shared_rulebook", 1, 0};
    dnd_rag_scope scope, forged;
    dnd_rag_result result, empty = {0};
    dnd_rag_citation citation;
    cmp_json_object object;
    cmp_json_array data;
    cmp_json_field element;
    char corpus[128], id[65], other[65], row[16384], second[16384], reply[65536], body[32768], text[8192];
    uint8_t wire[8192];
    size_t length;
    CHECK(!dnd_rag_scope_resolve(&config, &identity, &scope));
    CHECK(!dnd_rag_search_extract(&config, &scope, valid, 4, &result));
    strcpy(id, result.hits[0].citation.record_id); strcpy(corpus, result.hits[0].citation.corpus_version);
    memset(other, 'f', 64); other[64] = 0; CHECK(strcmp(id, other));
    const char *ids[] = {other, id};
    CHECK(dnd_rag_record_query_body(&config, &scope, ids, 2, body, sizeof(body)) == -1 && !body[0]);
    strcpy(config.shared_rulebook_corpus, corpus);
    CHECK(!dnd_rag_scope_resolve(&config, &identity, &scope));
    CHECK(strstr(scope.filter, corpus));
    CHECK(!dnd_rag_record_query_body(&config, &scope, ids, 2, body, sizeof(body)));
    CHECK(cmp_json_object_parse(body, &object));
    CHECK(cmp_json_object_str(&object, "filter", text, sizeof(text)));
    CHECK(!strncmp(text, scope.filter, strlen(scope.filter)) && strstr(text, " and record_id in [\""));
    CHECK(strstr(text, other) && strstr(text, id) && !strstr(text, "tashas-cauldron-of-everything"));
    CHECK(!cmp_json_object_key_count(&object, "data") && !cmp_json_object_key_count(&object, "searchParams"));
    CHECK(dnd_rag_record_query_body(&config, &scope, ids, 2, body, 20) == -1 && !body[0]);
    const char *duplicates[] = {id, id};
    CHECK(dnd_rag_record_query_body(&config, &scope, duplicates, 2, body, sizeof(body)) == -1);
    const char *bad[] = {"x\" or true"};
    CHECK(dnd_rag_record_query_body(&config, &scope, bad, 1, body, sizeof(body)) == -1);
    CHECK(dnd_rag_record_query_body(&config, &scope, ids, 0, body, sizeof(body)) == -1);
    CHECK(dnd_rag_record_query_body(&config, &scope, ids, 5, body, sizeof(body)) == -1);
    forged = scope; strcpy(forged.filter, "true");
    CHECK(dnd_rag_record_query_body(&config, &forged, ids, 2, body, sizeof(body)) == -1);
    forged = scope; forged.book_mask = ent_book_access_mask(1);
    CHECK(dnd_rag_record_query_body(&config, &forged, ids, 2, body, sizeof(body)) == -1);

    CHECK(cmp_json_object_parse(valid, &object));
    CHECK(cmp_json_field_array(cmp_json_object_field(&object, "data"), &data));
    CHECK(cmp_json_array_next(&data, &element) == 1 && element.value_len < sizeof(row));
    memcpy(row, element.value, element.value_len); row[element.value_len] = 0;
    replace_one(row, sizeof(row), ",\"distance\":0.875", "");
    strcpy(second, row); replace_one(second, sizeof(second), id, other); replace_one(second, sizeof(second), id, other);
    CHECK(snprintf(reply, sizeof(reply), "{\"code\":0,\"data\":[%s,%s]}", row, second) > 0);
    CHECK(!dnd_rag_record_query_extract(&config, &scope, ids, 2, reply, &result));
    CHECK(result.count == 2 && !strcmp(result.hits[0].citation.record_id, other) && !strcmp(result.hits[1].citation.record_id, id));
    for (size_t i = 0; i < result.count; ++i) {
        CHECK(result.hits[i].citation.score_metric == DND_RAG_SCORE_NONE && result.hits[i].citation.score == 0.0);
        length = pb_encode_retrieval_citation(wire, sizeof(wire), &result.hits[i].citation);
        CHECK(length && !pb_decode_retrieval_citation(wire, length, &citation));
        CHECK(citation.score_metric == DND_RAG_SCORE_NONE && citation.score == 0.0);
        wire[length - 1u] = 3;
        CHECK(pb_decode_retrieval_citation(wire, length, &citation) == -1);
    }
    result.count = 1; /* The existing public-boundary fixture checks one citation. */
    test_public_provenance(&result);
    citation = result.hits[0].citation; citation.score = 1.0;
    CHECK(!pb_encode_retrieval_citation(wire, sizeof(wire), &citation));
    citation.score = -1.0; CHECK(!pb_encode_retrieval_citation(wire, sizeof(wire), &citation));
    CHECK(dnd_rag_search_extract(&config, &scope, reply, 4, &result) == -1);
    CHECK(dnd_rag_record_query_extract(&config, &scope, ids, 2, valid, &result) == -1);
    CHECK(dnd_rag_record_query_extract(&config, &scope, ids, 1, reply, &result) == -1);
    CHECK(snprintf(reply, sizeof(reply), "{\"code\":0,\"data\":[%s]}", row) > 0);
    CHECK(dnd_rag_record_query_extract(&config, &scope, ids, 2, reply, &result) == -1);
    CHECK(snprintf(reply, sizeof(reply), "{\"code\":0,\"data\":[%s,%s]}", row, row) > 0);
    CHECK(dnd_rag_record_query_extract(&config, &scope, ids, 2, reply, &result) == -1);
    char wrong_corpus[128]; strcpy(wrong_corpus, corpus); wrong_corpus[10] = 'f';
    replace_one(second, sizeof(second), corpus, wrong_corpus); replace_one(second, sizeof(second), corpus, wrong_corpus);
    CHECK(snprintf(reply, sizeof(reply), "{\"code\":0,\"data\":[%s,%s]}", row, second) > 0);
    memset(&result, 0xa5, sizeof(result));
    CHECK(dnd_rag_record_query_extract(&config, &scope, ids, 2, reply, &result) == -1);
    CHECK(!memcmp(&result, &empty, sizeof(result)));
    config.shared_rulebook_corpus[10] = 'f';
    CHECK(!dnd_rag_scope_resolve(&config, &identity, &scope));
    CHECK(dnd_rag_search_extract(&config, &scope, valid, 4, &result) == -1);
    strcpy(config.shared_rulebook_corpus, "sha256:wrong");
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == -1);
}

static void test_search_results(void) {
    dnd_rag_config config = configuration();
    dnd_rag_identity identity = {"user-a", "session-a", "campaign-a", "character-a", "shared_rulebook", 1, 0};
    dnd_rag_scope scope;
    dnd_rag_result result;
    const dnd_rag_result empty = {0};
    char valid[16384], changed[65536], row[16384];
    size_t length, i;
    FILE *file = fopen("tests/fuzz-corpus/dnd-retrieval/search.json", "rb");
    static const struct { const char *from; const char *to; } cases[] = {
        {"\"code\":0", "\"code\":1"},
        {"\"code\":0", "\"code\":0,\"code\":0"},
        {"\"code\":0", "\"code\":0,\"error\":null"},
        {"\"data\":[", "\"data\":[],\"data\":["},
        {"Sneak Attack can apply", "Sneak Attack must apply"},
        {"book://players-handbook", "book://tashas-cauldron-of-everything"},
        {"\"distance\":0.875", "\"distance\":2"},
        {"\"distance\":0.875", "\"distance\":1e309"},
        {"\"distance\":0.875", "\"distance\":0.875,\"distance\":0.875"},
        {"\"page_start\":96", "\"page_start\":95"},
        {"\"page_end\":96", "\"page_end\":97"},
        {"\\\"content_hash\\\":\\\"3d", "\\\"content_hash\\\":\\\"4d"},
        {"\\\"source_sha256\\\":", "\\\"untrusted_hash\\\":"},
        {"\\\"source_sha256\\\":\\\"bb", "\\\"source_sha256\\\":\\\"xb"},
        {"\\\"source_sha256\\\":", "\\\"source_sha256\\\":\\\"etag\\\",\\\"source_sha256\\\":"},
        {"\\\"embedding_model\\\":\\\"bge-m3\\\"", "\\\"embedding_model\\\":\\\"other\\\""},
        {"\\\"embedding_model\\\":\\\"bge-m3\\\"", "\\\"embedding_model\\\":\\\"bge-m3\\\\u0000\\\""},
        {"\\\"knowledge_scope\\\":\\\"shared_rulebook\\\"", "\\\"knowledge_scope\\\":\\\"owned_rulebook\\\""},
        {"\\\"owner_user_id\\\":\\\"\\\"", "\\\"owner_user_id\\\":\\\"user-a\\\""},
        {"\\\"campaign_id\\\":\\\"\\\"", "\\\"campaign_id\\\":\\\"campaign-a\\\""},
        {"\\\"session_id\\\":\\\"\\\"", "\\\"session_id\\\":\\\"session-a\\\""},
        {"\\\"character_id\\\":\\\"\\\"", "\\\"character_id\\\":\\\"character-a\\\""},
        {"\\\"chunk_index\\\":0", "\\\"chunk_index\\\":-1"},
        {"\\\"chunk_index\\\":0", "\\\"chunk_index\\\":2147483648"},
        {"\\\"chunk_index\\\":0", "\\\"chunk_index\\\":0.5"},
        {"\\\"section\\\":\\\"Sneak Attack\\\"", "\\\"section\\\":null"},
    };
    CHECK(file != NULL);
    length = fread(valid, 1u, sizeof(valid) - 1u, file);
    CHECK(!ferror(file) && feof(file) && fclose(file) == 0 && length > 0u);
    valid[length] = '\0';
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_search_extract(&config, &scope, valid, 4u, &result) == 0);
    CHECK(result.count == 1u && result.hits[0].citation.score == 0.875);
    CHECK(strcmp(result.hits[0].citation.collection, "reviewed_books") == 0);
    CHECK(result.hits[0].citation.page_start == 96 && result.hits[0].citation.chunk_index == 0);
    CHECK(strcmp(result.hits[0].content, "Sneak Attack can apply once per turn.") == 0);
    test_rag_wire(&result);
    test_grounded_context(&result);
    test_complete_passage_context(&result);
    test_exact_excerpts(&result);
    test_canonical_grounding(&result);
    test_public_provenance(&result);
    test_bm25_scores(valid);
    test_record_queries(valid);
    test_ruleset_policy(valid);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        strcpy(changed, valid);
        replace_one(changed, sizeof(changed), cases[i].from, cases[i].to);
        memset(&result, 0xa5, sizeof(result));
        CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
        CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
    }
    /* A later unauthorized hit must erase the earlier admitted hit. */
    {
        cmp_json_object response;
        cmp_json_array data;
        cmp_json_field element;
        CHECK(cmp_json_object_parse(valid, &response));
        CHECK(cmp_json_field_array(cmp_json_object_field(&response, "data"), &data));
        CHECK(cmp_json_array_next(&data, &element) == 1 && element.value_len < sizeof(row));
        memcpy(row, element.value, element.value_len);
        row[element.value_len] = '\0';
        CHECK(snprintf(changed, sizeof(changed), "{\"code\":0,\"data\":[%s,%s]}", row, row) > 0);
        CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
        CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
        CHECK(snprintf(changed, sizeof(changed), "{\"code\":0,\"data\":[%s,{}]}", row) > 0);
        CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
        CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
        CHECK(dnd_rag_search_extract(&config, &scope, valid, 0u, &result) == -1);
        CHECK(dnd_rag_search_extract(&config, &scope, valid, 5u, &result) == -1);
    }
    /* Change all copies so these checks exercise policy, not column disagreement. */
    strcpy(changed, valid);
    while (strstr(changed, "players-handbook"))
        replace_one(changed, sizeof(changed), "players-handbook", "tashas-cauldron-of-everything");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
    identity.premium = 1;
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0 && result.count == 1u);
    while (strstr(changed, "tashas-cauldron-of-everything"))
        replace_one(changed, sizeof(changed), "tashas-cauldron-of-everything", "unknown-book");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
    CHECK(dnd_rag_search_extract(&config, &scope, "{\"code\":0,\"data\":[]}", 4u, &result) == 0);
    CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
    /* The same schema covers EPUB sections without fabricating page numbers. */
    strcpy(changed, valid);
    replace_all(changed, sizeof(changed), "page_start\":96", "page_start\":0");
    replace_all(changed, sizeof(changed), "page_end\":96", "page_end\":0");
    replace_all(changed, sizeof(changed), "page_start\\\":96", "page_start\\\":0");
    replace_all(changed, sizeof(changed), "page_end\\\":96", "page_end\\\":0");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0);
    CHECK(result.hits[0].citation.page_start == 0 && result.hits[0].citation.section[0]);
    replace_one(changed, sizeof(changed), "\\\"section\\\":\\\"Sneak Attack\\\"", "\\\"section\\\":\\\"\\\"");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
    /* Each private scope admits its owner and rejects a different identity. */
    {
        static const char *const private_scopes[] = {
            "owned_rulebook", "campaign_canon", "session_transcript", "character_memory"
        };
        for (i = 0; i < sizeof(private_scopes) / sizeof(private_scopes[0]); ++i) {
            char scalars[256];
            strcpy(changed, valid);
            replace_all(changed, sizeof(changed), "shared_rulebook", private_scopes[i]);
            replace_all(changed, sizeof(changed), "official_book", i ? "notes" : "uploaded_book");
            replace_all(changed, sizeof(changed), "public", "private");
            replace_one(changed, sizeof(changed), "book://players-handbook", "s3://test/books/phb.pdf");
            replace_one(changed, sizeof(changed), "\"owner_user_id\":\"\"", "\"owner_user_id\":\"user-a\"");
            replace_one(changed, sizeof(changed), "\\\"owner_user_id\\\":\\\"\\\"", "\\\"owner_user_id\\\":\\\"user-a\\\"");
            replace_one(changed, sizeof(changed), "\"campaign_id\":\"\"", "\"campaign_id\":\"campaign-a\"");
            replace_one(changed, sizeof(changed), "\\\"campaign_id\\\":\\\"\\\"", "\\\"campaign_id\\\":\\\"campaign-a\\\"");
            replace_one(changed, sizeof(changed), "\\\"session_id\\\":\\\"\\\"", "\\\"session_id\\\":\\\"session-a\\\"");
            replace_one(changed, sizeof(changed), "\\\"character_id\\\":\\\"\\\"", "\\\"character_id\\\":\\\"character-a\\\"");
            CHECK(snprintf(scalars, sizeof(scalars),
                "\"knowledge_scope\":\"%s\",\"session_id\":\"session-a\","
                "\"character_id\":\"character-a\",\"metadata_json\":", private_scopes[i]) > 0);
            replace_one(changed, sizeof(changed), "\"metadata_json\":", scalars);
            identity = (dnd_rag_identity){"user-a", "session-a", "campaign-a", "character-a", private_scopes[i], 1, 0};
            CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
            CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0);
            CHECK(result.count == 1u);
            identity.user_id = "user-b";
            CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
            CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
            CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
            identity.user_id = "user-a";
            if (i) {
                identity.campaign_id = "campaign-b";
                CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
                CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
                identity.campaign_id = "campaign-a";
            }
            if (i == 2u || i == 3u) {
                if (i == 2u) identity.session_id = "session-b";
                else identity.character_id = "character-b";
                CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
                CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == -1);
            }
        }
    }
    /* Four distinct valid hits fit. Returning more than requested is rejected. */
    identity = (dnd_rag_identity){"user-a", "", "", "", "shared_rulebook", 1, 0};
    CHECK(dnd_rag_scope_resolve(&config, &identity, &scope) == 0);
    strcpy(changed, "{\"code\":0,\"data\":[");
    for (i = 0; i < 4u; ++i) {
        char previous[65], next[65];
        int written;
        memset(previous, i ? (int)('c' + i - 1u) : 'c', 64u);
        memset(next, (int)('c' + i), 64u);
        previous[64] = next[64] = '\0';
        if (i) replace_all(row, sizeof(row), previous, next);
        length = strlen(changed);
        written = snprintf(changed + length, sizeof(changed) - length, "%s%s", i ? "," : "", row);
        CHECK(written > 0 && (size_t)written < sizeof(changed) - length);
    }
    strcat(changed, "]}");
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 4u, &result) == 0 && result.count == 4u);
    CHECK(dnd_rag_search_extract(&config, &scope, changed, 3u, &result) == -1);
    CHECK(memcmp(&result, &empty, sizeof(result)) == 0);
}

int main(void) {
    test_scope();
    test_returned_scope();
    test_json();
    test_embedding_and_search();
    test_lexical_request();
    test_embedding_envelope();
    test_model_dimensions();
    test_book_entitlements();
    test_search_results();
    puts("OK C retrieval scope, embedding, search, and source verification contracts");
    return 0;
}
