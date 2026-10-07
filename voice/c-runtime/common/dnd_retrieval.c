#define _POSIX_C_SOURCE 200809L
#include "dnd_retrieval.h"
#include "ent_books.h"
#include "utf8.h"

#include <float.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

static const char *const scope_names[DND_RAG_SCOPE_COUNT] = {
    "shared_rulebook", "owned_rulebook", "campaign_canon",
    "session_transcript", "character_memory"
};

static int hash_valid(const char *value);

static int corpus_valid(const char *text) {
    return text && strnlen(text, 128u) == 71u && !memcmp(text, "sha256:", 7u) && hash_valid(text + 7u);
}

static int identity_valid(const char *text) {
    size_t i, length;
    if (!text) return 0;
    length = strnlen(text, 128u);
    if (!length || length >= 128u ||
        !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (i = 1; i < length; ++i) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ':' || c == '@')) return 0;
    }
    return 1;
}

static int collection_valid(const char *text) {
    size_t i, length = strnlen(text, 128u);
    if (!length || length >= 128u ||
        !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') ||
          text[0] == '_')) return 0;
    for (i = 1; i < length; ++i) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static int ruleset_valid(const char *text) {
    size_t length = strnlen(text, 128u), i;
    if (length >= 128u || (length && !(text[0] >= 'a' && text[0] <= 'z'))) return 0;
    for (i = 1; i < length; ++i)
        if (!((text[i] >= 'a' && text[i] <= 'z') ||
              (text[i] >= '0' && text[i] <= '9') || text[i] == '-')) return 0;
    return 1;
}

static int model_valid(const dnd_rag_config *config) {
    size_t length;
    if (!config || !config->embedding_dimensions ||
        config->embedding_dimensions > DND_RAG_VECTOR_DIM_MAX ||
        (config->shared_rulebook_metric != DND_RAG_SCORE_COSINE &&
         config->shared_rulebook_metric != DND_RAG_SCORE_BM25)) return 0;
    length = strnlen(config->embedding_model, sizeof(config->embedding_model));
    return length > 0u && length < sizeof(config->embedding_model) &&
        utf8_validate_v1((const uint8_t *)config->embedding_model, length);
}

static int filter_books(dnd_rag_scope *scope) {
    size_t count, i, used = strlen(scope->filter), admitted = 0;
    const ent_book *books = ent_books(&count);
    int written;
    scope->book_mask = ent_book_access_mask(scope->premium);
    if (!ent_book_mask_valid(scope->book_mask)) return -1;
    written = snprintf(scope->filter + used, sizeof(scope->filter) - used, " and book_slug in [");
    if (written < 1 || (size_t)written >= sizeof(scope->filter) - used) return -1;
    used += (size_t)written;
    for (i = 0; i < count; ++i) {
        if (!(scope->book_mask & (UINT64_C(1) << i))) continue;
        written = snprintf(scope->filter + used, sizeof(scope->filter) - used,
            "%s\"%s\"", admitted++ ? "," : "", books[i].slug);
        if (written < 1 || (size_t)written >= sizeof(scope->filter) - used) return -1;
        used += (size_t)written;
    }
    if (!admitted || used + 2u > sizeof(scope->filter)) return -1;
    scope->filter[used++] = ']';
    scope->filter[used] = '\0';
    return 0;
}

int dnd_rag_scope_resolve(
    const dnd_rag_config *config, const dnd_rag_identity *identity, dnd_rag_scope *out
) {
    dnd_rag_scope scope = {0};
    size_t index;
    int written;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!config || !ruleset_valid(config->shared_rulebook_ruleset) ||
        (config->shared_rulebook_corpus[0] && !corpus_valid(config->shared_rulebook_corpus)) ||
        !identity || identity->entitled != 1 ||
        (identity->premium != 0 && identity->premium != 1) ||
        !identity_valid(identity->user_id) || !identity->knowledge_scope) return -1;
    for (index = 0; index < DND_RAG_SCOPE_COUNT; ++index)
        if (strcmp(identity->knowledge_scope, scope_names[index]) == 0) break;
    if (index == DND_RAG_SCOPE_COUNT || !collection_valid(config->collections[index])) return -1;
    scope.kind = (dnd_rag_scope_kind)index;
    scope.premium = identity->premium;
    memcpy(scope.authenticated_user_id, identity->user_id, strlen(identity->user_id) + 1u);
    memcpy(scope.collection, config->collections[index], strlen(config->collections[index]) + 1u);
    if (scope.kind == DND_RAG_SHARED_RULEBOOK) {
        written = snprintf(scope.filter, sizeof(scope.filter),
            "visibility == \"public\" and owner_user_id == \"\" and campaign_id == \"\""
            " and source_kind == \"official_book\"");
    } else {
        memcpy(scope.owner_user_id, identity->user_id, strlen(identity->user_id) + 1u);
        if (scope.kind >= DND_RAG_CAMPAIGN_CANON) {
            if (!identity_valid(identity->campaign_id)) return -1;
            memcpy(scope.campaign_id, identity->campaign_id, strlen(identity->campaign_id) + 1u);
        }
        if (scope.kind == DND_RAG_SESSION_TRANSCRIPT) {
            if (!identity_valid(identity->session_id)) return -1;
            memcpy(scope.session_id, identity->session_id, strlen(identity->session_id) + 1u);
        }
        if (scope.kind == DND_RAG_CHARACTER_MEMORY) {
            if (!identity_valid(identity->character_id)) return -1;
            memcpy(scope.character_id, identity->character_id, strlen(identity->character_id) + 1u);
        }
        written = snprintf(scope.filter, sizeof(scope.filter),
            "visibility == \"private\" and knowledge_scope == \"%s\" and owner_user_id == \"%s\""
            "%s%s%s%s%s%s%s%s%s",
            scope_names[index], scope.owner_user_id,
            scope.campaign_id[0] ? " and campaign_id == \"" : "", scope.campaign_id,
            scope.campaign_id[0] ? "\"" : "",
            scope.session_id[0] ? " and session_id == \"" : "", scope.session_id,
            scope.session_id[0] ? "\"" : "",
            scope.character_id[0] ? " and character_id == \"" : "", scope.character_id,
            scope.character_id[0] ? "\"" : "");
    }
    if (written < 1 || (size_t)written >= sizeof(scope.filter)) return -1;
    if (scope.kind <= DND_RAG_OWNED_RULEBOOK && filter_books(&scope) != 0) return -1;
    if (scope.kind == DND_RAG_SHARED_RULEBOOK && config->shared_rulebook_ruleset[0]) {
        size_t used = strlen(scope.filter);
        memcpy(scope.ruleset, config->shared_rulebook_ruleset, strlen(config->shared_rulebook_ruleset) + 1u);
        written = snprintf(scope.filter + used, sizeof(scope.filter) - used,
            " and ruleset == \"%s\"", scope.ruleset);
        if (written < 1 || (size_t)written >= sizeof(scope.filter) - used) return -1;
    }
    if (scope.kind == DND_RAG_SHARED_RULEBOOK && config->shared_rulebook_corpus[0]) {
        size_t used = strlen(scope.filter);
        written = snprintf(scope.filter + used, sizeof(scope.filter) - used,
            " and corpus_version == \"%s\"", config->shared_rulebook_corpus);
        if (written < 1 || (size_t)written >= sizeof(scope.filter) - used) return -1;
    }
    *out = scope;
    return 0;
}

static int matches(const cmp_json_object *object, const char *key, const char *expected) {
    char value[128];
    return cmp_json_object_str(object, key, value, sizeof(value)) && strcmp(value, expected) == 0;
}

int dnd_rag_scope_matches(const dnd_rag_scope *scope, const cmp_json_object *metadata) {
    char book_slug[128];
    if (!scope || !metadata || (unsigned)scope->kind >= DND_RAG_SCOPE_COUNT || !ruleset_valid(scope->ruleset) ||
        !identity_valid(scope->authenticated_user_id) ||
        !scope->collection[0] || !scope->filter[0] ||
        !matches(metadata, "knowledge_scope", scope_names[scope->kind]) ||
        !matches(metadata, "owner_user_id", scope->owner_user_id)) return 0;
    if (scope->kind <= DND_RAG_OWNED_RULEBOOK &&
        (scope->book_mask != ent_book_access_mask(scope->premium) ||
         !cmp_json_object_str(metadata, "book_slug", book_slug, sizeof(book_slug)) ||
         !ent_book_allowed(scope->book_mask, book_slug))) return 0;
    if (scope->kind == DND_RAG_SHARED_RULEBOOK)
        return !scope->owner_user_id[0] && matches(metadata, "visibility", "public") &&
            (!scope->ruleset[0] || matches(metadata, "ruleset", scope->ruleset)) &&
            matches(metadata, "source_kind", "official_book") &&
            matches(metadata, "campaign_id", "") && matches(metadata, "session_id", "") &&
            matches(metadata, "character_id", "");
    return !scope->ruleset[0] && identity_valid(scope->owner_user_id) &&
        strcmp(scope->owner_user_id, scope->authenticated_user_id) == 0 &&
        matches(metadata, "visibility", "private") &&
        (!scope->campaign_id[0] || matches(metadata, "campaign_id", scope->campaign_id)) &&
        (!scope->session_id[0] || matches(metadata, "session_id", scope->session_id)) &&
        (!scope->character_id[0] || matches(metadata, "character_id", scope->character_id));
}

int dnd_rag_embedding_body(
    const dnd_rag_config *config, const char *query, char *out, size_t capacity
) {
    char escaped_query[12289], escaped_model[769];
    int written;
    size_t query_len;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!model_valid(config) || !query) return -1;
    query_len = strnlen(query, 2048u);
    if (!query_len || query_len >= 2048u ||
        !utf8_validate_v1((const uint8_t *)query, query_len) ||
        cmp_json_escape_exact(query, escaped_query, sizeof(escaped_query)) != 0 ||
        cmp_json_escape_exact(config->embedding_model, escaped_model, sizeof(escaped_model)) != 0) return -1;
    written = snprintf(out, capacity, "{\"model\":\"%s\",\"truncate\":false,\"input\":[\"%s\"]}",
        escaped_model, escaped_query);
    if (written < 1 || (size_t)written >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}

static int vector_valid(cmp_json_array *array, size_t dimensions) {
    cmp_json_field element;
    size_t count = 0;
    int rc, nonzero = 0;
    while ((rc = cmp_json_array_next(array, &element)) == 1) {
        double value;
        if (++count > dimensions || !cmp_json_field_double(&element, &value) ||
            fabs(value) > (double)FLT_MAX) return 0;
        if (value != 0.0 && (float)value == 0.0f) return 0;
        if (value != 0.0) nonzero = 1;
    }
    return rc == 0 && count == dimensions && nonzero;
}

static int exact_input_valid(const cmp_json_object *response) {
    cmp_json_object contract;
    cmp_json_array counts;
    cmp_json_field count;
    int64_t maximum;
    size_t value = 0, i;
    int truncated;
    if (!cmp_json_object_object(response, "input_contract", &contract) ||
        contract.field_count != 4u || !matches(&contract, "pooling", "cls") ||
        !cmp_json_object_bool(&contract, "truncated", &truncated) || truncated ||
        !cmp_json_object_i64(&contract, "max_input_tokens", &maximum) ||
        maximum < 1 || maximum > 8192 ||
        !cmp_json_field_array(cmp_json_object_field(&contract, "input_tokens"), &counts) ||
        cmp_json_array_next(&counts, &count) != 1 || !count.value_len ||
        count.value_len > 4u || count.value[0] < '1' || count.value[0] > '9') return 0;
    for (i = 0; i < count.value_len; ++i) {
        if (count.value[i] < '0' || count.value[i] > '9') return 0;
        value = value * 10u + (size_t)(count.value[i] - '0');
    }
    return value <= (size_t)maximum && cmp_json_array_next(&counts, &count) == 0;
}

int dnd_rag_embedding_extract(
    const dnd_rag_config *config, const char *json, char *out, size_t capacity
) {
    cmp_json_object response, item;
    cmp_json_array data, vector;
    cmp_json_field element;
    const cmp_json_field *embedding;
    char model[128];
    int64_t index;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!model_valid(config) || !json || strnlen(json, 65536u) >= 65536u ||
        !cmp_json_object_parse(json, &response) ||
        !exact_input_valid(&response) ||
        cmp_json_object_key_count(&response, "error") != 0 ||
        !cmp_json_object_str(&response, "model", model, sizeof(model)) ||
        strcmp(model, config->embedding_model) != 0 ||
        !matches(&response, "object", "list") ||
        !cmp_json_field_array(cmp_json_object_field(&response, "data"), &data) ||
        cmp_json_array_next(&data, &element) != 1 ||
        !cmp_json_field_object(&element, &item) ||
        item.field_count != 3u ||
        !matches(&item, "object", "embedding") ||
        !cmp_json_object_i64(&item, "index", &index) || index != 0 ||
        cmp_json_array_next(&data, &element) != 0) return -1;
    embedding = cmp_json_object_field(&item, "embedding");
    if (!cmp_json_field_array(embedding, &vector) ||
        !vector_valid(&vector, config->embedding_dimensions) ||
        embedding->value_len >= DND_RAG_VECTOR_JSON_CAP || embedding->value_len >= capacity) return -1;
    memcpy(out, embedding->value, embedding->value_len);
    out[embedding->value_len] = '\0';
    return 0;
}

static int canonical_scope(
    const dnd_rag_config *config, const dnd_rag_scope *scope, dnd_rag_scope *out
) {
    dnd_rag_identity identity;
    if (!scope || (unsigned)scope->kind >= DND_RAG_SCOPE_COUNT ||
        !ruleset_valid(scope->ruleset) ||
        strnlen(scope->collection, sizeof(scope->collection)) >= sizeof(scope->collection) ||
        strnlen(scope->filter, sizeof(scope->filter)) >= sizeof(scope->filter) ||
        strnlen(scope->owner_user_id, sizeof(scope->owner_user_id)) >= sizeof(scope->owner_user_id) ||
        strnlen(scope->campaign_id, sizeof(scope->campaign_id)) >= sizeof(scope->campaign_id) ||
        strnlen(scope->session_id, sizeof(scope->session_id)) >= sizeof(scope->session_id) ||
        strnlen(scope->character_id, sizeof(scope->character_id)) >= sizeof(scope->character_id)) return 0;
    identity = (dnd_rag_identity){scope->authenticated_user_id, scope->session_id,
        scope->campaign_id, scope->character_id, scope_names[scope->kind], 1, scope->premium};
    return dnd_rag_scope_resolve(config, &identity, out) == 0 &&
        strcmp(out->collection, scope->collection) == 0 &&
        strcmp(out->filter, scope->filter) == 0 && out->book_mask == scope->book_mask &&
        strcmp(out->ruleset, scope->ruleset) == 0 &&
        strcmp(out->owner_user_id, scope->owner_user_id) == 0 &&
        strcmp(out->campaign_id, scope->campaign_id) == 0 &&
        strcmp(out->session_id, scope->session_id) == 0 &&
        strcmp(out->character_id, scope->character_id) == 0;
}

static const char search_fields[] =
    "\"outputFields\":[\"record_id\",\"content\",\"source\",\"document_id\",\"book_slug\","
    "\"source_kind\",\"visibility\",\"owner_user_id\",\"campaign_id\",\"corpus_version\","
    "\"source_etag\",\"page_start\",\"page_end\",\"metadata_json\"";

static int query_valid(const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *const *ids, size_t count, dnd_rag_scope *canonical) {
    if (!model_valid(config) || !canonical_scope(config, scope, canonical) ||
        scope->kind != DND_RAG_SHARED_RULEBOOK || !corpus_valid(config->shared_rulebook_corpus) ||
        !ids || !count || count > DND_RAG_HITS_MAX) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!ids[i] || strnlen(ids[i], 65u) != 64u || !hash_valid(ids[i])) return 0;
        for (size_t j = 0; j < i; ++j) if (!strcmp(ids[i], ids[j])) return 0;
    }
    return 1;
}

int dnd_rag_record_query_body(const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *const *ids, size_t count, char *out, size_t capacity) {
    dnd_rag_scope canonical;
    char filter[4608], escaped[27649];
    int n;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!query_valid(config, scope, ids, count, &canonical)) return -1;
    n = snprintf(filter, sizeof(filter), "%s and record_id in [", canonical.filter);
    if (n < 1 || (size_t)n >= sizeof(filter)) return -1;
    size_t used = (size_t)n;
    for (size_t i = 0; i < count; ++i) {
        n = snprintf(filter + used, sizeof(filter) - used, "%s\"%s\"", i ? "," : "", ids[i]);
        if (n < 1 || (size_t)n >= sizeof(filter) - used) return -1;
        used += (size_t)n;
    }
    if (used + 2u > sizeof(filter)) return -1;
    filter[used++] = ']'; filter[used] = '\0';
    if (cmp_json_escape_exact(filter, escaped, sizeof(escaped))) return -1;
    n = snprintf(out, capacity, "{\"collectionName\":\"%s\",\"filter\":\"%s\",\"limit\":%zu,%s%s}",
        canonical.collection, escaped, count, search_fields, canonical.ruleset[0] ? ",\"ruleset\"]" : "]");
    if (n < 1 || (size_t)n >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}

int dnd_rag_search_body(
    const dnd_rag_config *config, const dnd_rag_scope *scope, const char *vector_json,
    size_t top_k, char *out, size_t capacity
) {
    char escaped_filter[24577];
    cmp_json_array vector;
    dnd_rag_scope canonical;
    int written;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!model_valid(config) || !canonical_scope(config, scope, &canonical) ||
        (scope->kind == DND_RAG_SHARED_RULEBOOK && config->shared_rulebook_metric != DND_RAG_SCORE_COSINE) ||
        !vector_json || strnlen(vector_json, DND_RAG_VECTOR_JSON_CAP) >= DND_RAG_VECTOR_JSON_CAP ||
        !top_k || top_k > DND_RAG_HITS_MAX ||
        !cmp_json_array_parse(vector_json, &vector) ||
        !vector_valid(&vector, config->embedding_dimensions) ||
        cmp_json_escape_exact(scope->filter, escaped_filter, sizeof(escaped_filter)) != 0) return -1;
    written = snprintf(out, capacity,
        "{\"collectionName\":\"%s\",\"data\":[%s],\"annsField\":\"embedding\","
        "\"filter\":\"%s\",\"limit\":%zu,\"searchParams\":{\"metricType\":\"COSINE\"},"
        "%s%s}", scope->collection, vector_json, escaped_filter, top_k, search_fields,
        canonical.kind != DND_RAG_SHARED_RULEBOOK ?
            ",\"knowledge_scope\",\"session_id\",\"character_id\"]" :
            canonical.ruleset[0] ? ",\"ruleset\"]" : "]");
    if (written < 1 || (size_t)written >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}

int dnd_rag_lexical_search_body(
    const dnd_rag_config *config, const dnd_rag_scope *scope, const char *query,
    size_t top_k, char *out, size_t capacity
) {
    char escaped_filter[24577], escaped_query[12289];
    dnd_rag_scope canonical;
    size_t length;
    int written;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!model_valid(config) || !canonical_scope(config, scope, &canonical) ||
        config->shared_rulebook_metric != DND_RAG_SCORE_BM25 ||
        scope->kind != DND_RAG_SHARED_RULEBOOK || !query ||
        !top_k || top_k > DND_RAG_HITS_MAX) return -1;
    length = strnlen(query, 2048u);
    if (!length || length >= 2048u || !utf8_validate_v1((const uint8_t *)query, length) ||
        cmp_json_escape_exact(query, escaped_query, sizeof(escaped_query)) != 0 ||
        cmp_json_escape_exact(scope->filter, escaped_filter, sizeof(escaped_filter)) != 0) return -1;
    written = snprintf(out, capacity,
        "{\"collectionName\":\"%s\",\"data\":[\"%s\"],\"annsField\":\"sparse\","
        "\"filter\":\"%s\",\"limit\":%zu,\"searchParams\":{\"metricType\":\"BM25\"},%s%s}",
        scope->collection, escaped_query, escaped_filter, top_k, search_fields,
        canonical.ruleset[0] ? ",\"ruleset\"]" : "]");
    if (written < 1 || (size_t)written >= capacity) { out[0] = '\0'; return -1; }
    return 0;
}

static int hash_valid(const char *value) {
    size_t i;
    if (strlen(value) != 64u) return 0;
    for (i = 0; i < 64u; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}

static int content_matches(const char *content, const char *expected) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    size_t i;
    if (!hash_valid(expected) ||
        !SHA256((const unsigned char *)content, strlen(content), digest)) return 0;
    for (i = 0; i < sizeof(digest); ++i)
        if (expected[2u * i] != hex[digest[i] >> 4u] ||
            expected[2u * i + 1u] != hex[digest[i] & 15u]) return 0;
    return 1;
}

/* Milvus filters scalar columns. Recheck those columns against the separately
 * stored metadata object, which provides the provenance fields. */
static int scalar_agrees(
    const cmp_json_object *row, const cmp_json_object *metadata, const char *key
) {
    char scalar[128], nested[128];
    return cmp_json_object_str(row, key, scalar, sizeof(scalar)) &&
        cmp_json_object_str(metadata, key, nested, sizeof(nested)) &&
        strcmp(scalar, nested) == 0;
}

static int position(
    const cmp_json_object *object, const char *key, int32_t *out
) {
    int64_t value;
    if (!cmp_json_object_i64(object, key, &value) || value < 0 || value > INT32_MAX) return 0;
    *out = (int32_t)value;
    return 1;
}

static int read_hit(
    const dnd_rag_config *config, const dnd_rag_scope *scope,
    const cmp_json_object *row, int scored, dnd_rag_hit *hit
) {
    static const char *const scalar_fields[] = {
        "record_id", "document_id", "book_slug", "source_kind", "visibility",
        "owner_user_id", "campaign_id", "corpus_version", "source_etag"
    };
    char metadata_json[16384], expected_source[144], source_key[2048];
    cmp_json_object metadata;
    dnd_rag_citation *citation = &hit->citation;
    int32_t page_start, page_end;
    size_t i;
    if (!cmp_json_object_str(row, "metadata_json", metadata_json, sizeof(metadata_json)) ||
        !cmp_json_object_parse(metadata_json, &metadata) ||
        !dnd_rag_scope_matches(scope, &metadata)) return -1;
    for (i = 0; i < sizeof(scalar_fields) / sizeof(scalar_fields[0]); ++i)
        if (!scalar_agrees(row, &metadata, scalar_fields[i])) return -1;
    if (scope->kind != DND_RAG_SHARED_RULEBOOK &&
        (!scalar_agrees(row, &metadata, "knowledge_scope") ||
         !scalar_agrees(row, &metadata, "session_id") ||
         !scalar_agrees(row, &metadata, "character_id"))) return -1;
    if (scope->ruleset[0] && !scalar_agrees(row, &metadata, "ruleset")) return -1;
    if (!cmp_json_object_str(row, "content", hit->content, sizeof(hit->content)) ||
        !hit->content[0] ||
        !cmp_json_object_str(row, "source", citation->source, sizeof(citation->source)) ||
        !cmp_json_object_str(row, "book_slug", citation->book_slug, sizeof(citation->book_slug)) ||
        !cmp_json_object_str(row, "record_id", citation->record_id, sizeof(citation->record_id)) ||
        !hash_valid(citation->record_id) ||
        !cmp_json_object_str(row, "document_id", citation->document_id, sizeof(citation->document_id)) ||
        !identity_valid(citation->document_id) ||
        !cmp_json_object_str(row, "corpus_version", citation->corpus_version, sizeof(citation->corpus_version)) ||
        strncmp(citation->corpus_version, "sha256:", 7u) != 0 ||
        !hash_valid(citation->corpus_version + 7u) ||
        (scope->kind == DND_RAG_SHARED_RULEBOOK && config->shared_rulebook_corpus[0] &&
         strcmp(citation->corpus_version, config->shared_rulebook_corpus)) ||
        !cmp_json_object_str(&metadata, "embedding_model", citation->embedding_model, sizeof(citation->embedding_model)) ||
        strcmp(citation->embedding_model, config->embedding_model) != 0 ||
        !cmp_json_object_str(&metadata, "content_hash", citation->content_hash, sizeof(citation->content_hash)) ||
        !content_matches(hit->content, citation->content_hash) ||
        !cmp_json_object_str(&metadata, "source_sha256", citation->source_sha256, sizeof(citation->source_sha256)) ||
        !hash_valid(citation->source_sha256) ||
        !cmp_json_object_str(&metadata, "section", citation->section, sizeof(citation->section)) ||
        !position(row, "page_start", &citation->page_start) ||
        !position(row, "page_end", &citation->page_end) ||
        !position(&metadata, "page_start", &page_start) ||
        !position(&metadata, "page_end", &page_end) ||
        citation->page_start != page_start || citation->page_end != page_end ||
        page_end < page_start ||
        !position(&metadata, "chunk_index", &citation->chunk_index)) return -1;
    if (scored) {
        if (!cmp_json_field_double(cmp_json_object_field(row, "distance"), &citation->score)) return -1;
        citation->score_metric = scope->kind == DND_RAG_SHARED_RULEBOOK ?
            config->shared_rulebook_metric : DND_RAG_SCORE_COSINE;
    } else {
        if (cmp_json_object_key_count(row, "distance") || cmp_json_object_key_count(row, "score") ||
            cmp_json_object_key_count(row, "score_metric")) return -1;
        citation->score = 0.0; citation->score_metric = DND_RAG_SCORE_NONE;
    }
    if (!dnd_rag_score_valid(citation->score_metric, citation->score)) return -1;
    /* Official books use their catalog URI. Uploaded books and notes retain
     * the exact object key. EPUB sections have no invented PDF page number. */
    if (matches(&metadata, "source_kind", "official_book")) {
        int written = snprintf(expected_source, sizeof(expected_source), "book://%s", citation->book_slug);
        if (written < 1 || (size_t)written >= sizeof(expected_source) ||
            strcmp(citation->source, expected_source) != 0 ||
            (!page_start && !citation->section[0])) return -1;
    } else {
        const char *key;
        if (strncmp(citation->source, "s3://", 5u) != 0 ||
            !cmp_json_object_str(&metadata, "source_key", source_key, sizeof(source_key)) ||
            !source_key[0]) return -1;
        key = strchr(citation->source + 5u, '/');
        if (!key || key == citation->source + 5u || strcmp(key + 1u, source_key) != 0) return -1;
    }
    for (i = 0; citation->source[i]; ++i)
        if ((unsigned char)citation->source[i] < 0x20u || citation->source[i] == 0x7f) return -1;
    memcpy(citation->collection, scope->collection, strlen(scope->collection) + 1u);
    return 0;
}

int dnd_rag_search_extract(
    const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *json, size_t top_k, dnd_rag_result *out
) {
    dnd_rag_scope canonical;
    cmp_json_object response, row;
    cmp_json_array data;
    cmp_json_field element;
    int64_t code;
    int rc;
    size_t i;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!model_valid(config) || !canonical_scope(config, scope, &canonical) ||
        !json || strnlen(json, DND_RAG_REPLY_CAP) >= DND_RAG_REPLY_CAP ||
        !top_k || top_k > DND_RAG_HITS_MAX) return -1;
    if (!cmp_json_object_parse(json, &response) ||
        !cmp_json_object_i64(&response, "code", &code) || code != 0 ||
        cmp_json_object_key_count(&response, "error") != 0 ||
        !cmp_json_field_array(cmp_json_object_field(&response, "data"), &data)) return -1;
    while ((rc = cmp_json_array_next(&data, &element)) == 1) {
        if (out->count >= top_k || !cmp_json_field_object(&element, &row) ||
            read_hit(config, &canonical, &row, 1, &out->hits[out->count]) != 0) goto reject;
        for (i = 0; i < out->count; ++i)
            if (strcmp(out->hits[i].citation.record_id,
                       out->hits[out->count].citation.record_id) == 0) goto reject;
        ++out->count;
    }
    if (rc == 0) return 0;
reject:
    memset(out, 0, sizeof(*out));
    return -1;
}

int dnd_rag_record_query_extract(const dnd_rag_config *config, const dnd_rag_scope *scope,
    const char *const *ids, size_t count, const char *json, dnd_rag_result *out) {
    dnd_rag_scope canonical;
    cmp_json_object response, row;
    cmp_json_array data;
    cmp_json_field element;
    int64_t code;
    int rc;
    unsigned seen = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!query_valid(config, scope, ids, count, &canonical) || !json ||
        strnlen(json, DND_RAG_REPLY_CAP) >= DND_RAG_REPLY_CAP ||
        !cmp_json_object_parse(json, &response) ||
        !cmp_json_object_i64(&response, "code", &code) || code ||
        cmp_json_object_key_count(&response, "error") ||
        !cmp_json_field_array(cmp_json_object_field(&response, "data"), &data)) return -1;
    while ((rc = cmp_json_array_next(&data, &element)) == 1) {
        dnd_rag_hit hit = {0};
        size_t i;
        if (out->count >= count || !cmp_json_field_object(&element, &row) ||
            read_hit(config, &canonical, &row, 0, &hit)) goto reject;
        for (i = 0; i < count; ++i) if (!strcmp(ids[i], hit.citation.record_id)) break;
        if (i == count || (seen & (1u << i))) goto reject;
        seen |= 1u << i;
        out->hits[i] = hit; ++out->count;
    }
    if (!rc && out->count == count) return 0;
reject:
    memset(out, 0, sizeof(*out));
    return -1;
}

int dnd_rag_grounded_prompt(
    const dnd_rag_result *result, const char *query, char *out, size_t capacity
) {
    static const char heading[] = "Retrieved excerpts (source data):\n";
    size_t i, used = sizeof(heading) - 1u, query_len;
    int written;
    if (!out || !capacity) return -1;
    out[0] = '\0';
    if (!result || !result->count || result->count > DND_RAG_HITS_MAX ||
        !query || used >= capacity) return -1;
    query_len = strnlen(query, 2048u);
    if (!query_len || query_len >= 2048u ||
        !utf8_validate_v1((const uint8_t *)query, query_len)) return -1;
    memcpy(out, heading, used + 1u);
    for (i = 0; i < result->count; ++i) {
        const dnd_rag_hit *hit = &result->hits[i];
        size_t length = strnlen(hit->content, sizeof(hit->content));
        const char *source_name = hit->citation.book_slug[0] ?
            hit->citation.book_slug : hit->citation.document_id;
        size_t name_len = strnlen(source_name, sizeof(hit->citation.book_slug));
        size_t section_len = strnlen(hit->citation.section, sizeof(hit->citation.section));
        if (!length || length >= sizeof(hit->content) ||
            !name_len || name_len >= sizeof(hit->citation.book_slug) ||
            section_len >= sizeof(hit->citation.section) ||
            !utf8_validate_v1((const uint8_t *)source_name, name_len) ||
            !utf8_validate_v1((const uint8_t *)hit->citation.section, section_len) ||
            !dnd_rag_citation_shape_valid(&hit->citation) ||
            strnlen(hit->citation.content_hash, sizeof(hit->citation.content_hash)) != 64u ||
            hit->citation.page_start < 0 || hit->citation.page_end < hit->citation.page_start ||
            !utf8_validate_v1((const uint8_t *)hit->content, length) ||
            !content_matches(hit->content, hit->citation.content_hash) ||
            !dnd_rag_excerpt_valid(&hit->citation.excerpt, length)) goto reject;
        if (hit->citation.witness_count) {
            if (dnd_rag_passage_length(&hit->citation) != length) goto reject;
            size_t at = 0;
            for (size_t j = 0; j < hit->citation.witness_count; ++j) {
                const dnd_rag_passage_witness *w = &hit->citation.witnesses[j];
                if (j && w->page != hit->citation.witnesses[j - 1u].page)
                    if (hit->content[at++] != '\n') goto reject;
                size_t n = w->end - w->begin;
                if (!utf8_validate_v1((const uint8_t *)hit->content + at, n)) goto reject;
                at += n;
            }
            written = snprintf(out + used, capacity - used,
                "\n[Source %zu; name %s; passage %s; pages %d-%d; section %s; original records %zu]\n",
                i + 1u, source_name, hit->citation.passage_id, hit->citation.page_start, hit->citation.page_end,
                hit->citation.section, hit->citation.witness_count);
        } else {
            written = snprintf(out + used, capacity - used,
                "\n[Source %zu; name %s; record %s; pages %d-%d; section %s; chunk %d]\n",
                i + 1u, source_name, hit->citation.record_id, hit->citation.page_start, hit->citation.page_end,
                hit->citation.section, hit->citation.chunk_index);
        }
        if (written < 1 || (size_t)written >= capacity - used) goto reject;
        used += (size_t)written;
        size_t count = hit->citation.excerpt.count;
        for (size_t j = 0; j < (count ? count : 1u); ++j) {
            size_t begin = count ? hit->citation.excerpt.spans[j].begin : 0;
            size_t end = count ? hit->citation.excerpt.spans[j].end : length;
            size_t bytes = end - begin;
            if (!utf8_validate_v1((const uint8_t *)hit->content + begin, bytes)) goto reject;
            /* An explicit separator prevents disjoint source spans from joining
             * into a sentence that never appeared in the original record. */
            if (j) {
                static const char gap[] = "\n[Omitted source text]\n";
                if (sizeof(gap) - 1u >= capacity - used) goto reject;
                memcpy(out + used, gap, sizeof(gap) - 1u);
                used += sizeof(gap) - 1u;
            }
            if (bytes >= capacity - used) goto reject;
            memcpy(out + used, hit->content + begin, bytes);
            used += bytes;
            out[used] = '\0';
        }
        written = snprintf(out + used, capacity - used, "\n[End source %zu]\n", i + 1u);
        if (written < 1 || (size_t)written >= capacity - used) goto reject;
        used += (size_t)written;
    }
    written = snprintf(out + used, capacity - used, "\nQuestion: %s", query);
    if (written < 1 || (size_t)written >= capacity - used) goto reject;
    return 0;
reject:
    out[0] = '\0';
    return -1;
}
