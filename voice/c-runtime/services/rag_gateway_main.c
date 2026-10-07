/* Pure-C authenticated-scope retrieval: embedding bridge -> Milvus -> VBus. */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../common/dnd_retrieval.h"
#include "../common/dnd_source_artifact.h"
#include "../common/http_min.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    vbus_client *nc;
    http_min_client embed_http;
    http_min_client search_http;
    http_min_client query_http;
    dnd_source_artifact *source_artifact;
    dnd_rag_result source_hits, passage_hits;
    dnd_rag_config config;
    char bearer[1032];
    int timeout_ms;
    int enabled;
    /* One serial turn owner reuses bounded scratch storage. */
    char body[65536];
    char reply[DND_RAG_REPLY_CAP];
    char vector[DND_RAG_VECTOR_JSON_CAP];
} rag_state;

static int64_t clock_ms(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int remaining_ms(int64_t deadline) {
    int64_t now = clock_ms(CLOCK_MONOTONIC);
    if (now <= 0 || now >= deadline || deadline - now > INT_MAX) return 0;
    return (int)(deadline - now);
}

static int copy_config(char *out, size_t cap, const char *value) {
    size_t length = strlen(value);
    if (length >= cap) return -1;
    memcpy(out, value, length + 1u);
    return 0;
}

static int configure(rag_state *st) {
    static const char *const collection_vars[DND_RAG_SCOPE_COUNT] = {
        "RAG_SHARED_RULEBOOK_COLLECTION", "RAG_OWNED_RULEBOOK_COLLECTION",
        "RAG_CAMPAIGN_CANON_COLLECTION", "RAG_SESSION_TRANSCRIPT_COLLECTION",
        "RAG_CHARACTER_MEMORY_COLLECTION"
    };
    static const char *const scope_names[DND_RAG_SCOPE_COUNT] = {
        "shared_rulebook", "owned_rulebook", "campaign_canon",
        "session_transcript", "character_memory"
    };
    const char *embed_url = svc_env("EMBED_HTTP_URL", "");
    const char *search_url = svc_env("MILVUS_SEARCH_URL", "");
    const char *model = svc_env("EMBEDDING_MODEL_ID", "");
    const char *dimensions = svc_env("EMBEDDING_DIMENSIONS", "");
    const char *token = svc_env("MILVUS_HTTP_AUTH_TOKEN", "");
    const char *metric = svc_env("RAG_SHARED_RULEBOOK_SEARCH", "");
    const char *ruleset = svc_env("RAG_SHARED_RULEBOOK_RULESET", "");
    const char *corpus = svc_env("RAG_SHARED_RULEBOOK_CORPUS_VERSION", "");
    const char *index_path = svc_env("RAG_SOURCE_INDEX_PATH", "");
    const char *index_hash = svc_env("RAG_SOURCE_INDEX_SHA256", "");
    const char *manifest_path = svc_env("RAG_SOURCE_MANIFEST_PATH", "");
    const char *manifest_hash = svc_env("RAG_SOURCE_MANIFEST_SHA256", "");
    const char *compiler_hash = svc_env("RAG_SOURCE_COMPILER_SHA256", "");
    const char *query_url = svc_env("MILVUS_QUERY_URL", "");
    int source_configured = index_path[0] || index_hash[0] || manifest_path[0] ||
        manifest_hash[0] || compiler_hash[0] || query_url[0];
    int configured = embed_url[0] || search_url[0] || model[0] || dimensions[0] || token[0] ||
        metric[0] || ruleset[0] || corpus[0] || source_configured;
    size_t i;
    if (!metric[0] || strcmp(metric, "cosine") == 0)
        st->config.shared_rulebook_metric = DND_RAG_SCORE_COSINE;
    else if (strcmp(metric, "bm25") == 0)
        st->config.shared_rulebook_metric = DND_RAG_SCORE_BM25;
    else return -1;
    if (copy_config(st->config.shared_rulebook_ruleset, sizeof(st->config.shared_rulebook_ruleset), ruleset) != 0)
        return -1;
    if (copy_config(st->config.shared_rulebook_corpus, sizeof(st->config.shared_rulebook_corpus), corpus)) return -1;
    for (i = 0; i < DND_RAG_SCOPE_COUNT; ++i) {
        const char *value = svc_env(collection_vars[i], "");
        if (value[0]) configured = 1;
        if (copy_config(st->config.collections[i], sizeof(st->config.collections[i]), value) != 0)
            return -1;
    }
    if (!configured) return 0;
    if (!embed_url[0] || !search_url[0] || !model[0] || !dimensions[0] ||
        !st->config.collections[0][0] ||
        copy_config(st->config.embedding_model, sizeof(st->config.embedding_model), model) != 0)
        return -1;
    st->config.embedding_dimensions = (size_t)svc_env_int_range(
        "EMBEDDING_DIMENSIONS", 0, 0, DND_RAG_VECTOR_DIM_MAX);
    st->timeout_ms = svc_env("RAG_HTTP_TIMEOUT_MS", "")[0] ?
        svc_env_int_range("RAG_HTTP_TIMEOUT_MS", 0, 0, 60000) : 3000;
    if (!st->timeout_ms) return -1;
    if (dnd_rag_embedding_body(&st->config, "configuration", st->body, sizeof(st->body)) != 0)
        return -1;
    for (i = 0; i < DND_RAG_SCOPE_COUNT; ++i) {
        dnd_rag_scope scope;
        dnd_rag_identity identity = {
            "configuration-user", "configuration-session", "configuration-campaign",
            "configuration-character", scope_names[i], 1, 1
        };
        if (st->config.collections[i][0] &&
            dnd_rag_scope_resolve(&st->config, &identity, &scope) != 0) return -1;
    }
    if (token[0]) {
        size_t length = strlen(token);
        if (length > sizeof(st->bearer) - sizeof("Bearer ")) return -1;
        for (i = 0; i < length; ++i)
            if ((unsigned char)token[i] <= 0x20u || (unsigned char)token[i] > 0x7eu) return -1;
        memcpy(st->bearer, "Bearer ", sizeof("Bearer ") - 1u);
        memcpy(st->bearer + sizeof("Bearer ") - 1u, token, length + 1u);
    }
    if (http_min_client_init(&st->embed_http, embed_url) != HTTP_MIN_OK ||
        http_min_client_init(&st->search_http, search_url) != HTTP_MIN_OK) return -1;
    if (source_configured) {
        dnd_source_artifact_expectation expected = {0};
        if (!index_path[0] || !manifest_path[0] || !query_url[0] || !corpus[0] || !ruleset[0] ||
            copy_config(expected.artifact_sha256, sizeof(expected.artifact_sha256), index_hash) ||
            copy_config(expected.manifest_sha256, sizeof(expected.manifest_sha256), manifest_hash) ||
            copy_config(expected.compiler_sha256, sizeof(expected.compiler_sha256), compiler_hash) ||
            copy_config(expected.collection, sizeof(expected.collection), st->config.collections[0]) ||
            copy_config(expected.corpus_version, sizeof(expected.corpus_version), corpus) ||
            copy_config(expected.ruleset, sizeof(expected.ruleset), ruleset) ||
            dnd_source_artifact_load_reviewed(index_path, manifest_path, &expected, &st->source_artifact) ||
            http_min_client_init(&st->query_http, query_url) != HTTP_MIN_OK) return -1;
    }
    st->enabled = 1;
    return 0;
}

static int post_json(
    rag_state *st, http_min_client *client, int64_t deadline, int authenticate
) {
    http_min_header header = {"Authorization", st->bearer};
    size_t length = 0;
    int status = 0;
    int timeout = remaining_ms(deadline);
    if (!timeout || http_min_client_post_headers_cancel(
            client, "application/json", &header,
            authenticate && st->bearer[0] ? 1u : 0u,
            (const uint8_t *)st->body, strlen(st->body),
            (uint8_t *)st->reply, sizeof(st->reply) - 1u, &length,
            &status, timeout, NULL) != HTTP_MIN_OK ||
        status != 200 || !length || memchr(st->reply, '\0', length) != NULL)
        return -1;
    st->reply[length] = '\0';
    return remaining_ms(deadline) ? 0 : -1;
}

static int source_record_matches(rag_state *st, const dnd_source_data *data,
    const dnd_source_record *r, const dnd_rag_hit *hit) {
    const dnd_source_document *d = &data->documents[r->document];
    const dnd_rag_citation *c = &hit->citation;
    return dnd_source_artifact_matches_source(st->source_artifact, c) &&
        !c->witness_count && !c->passage_id[0] && !c->excerpt.count &&
        !strcmp(c->record_id, r->record_id) && !strcmp(c->document_id, d->document_id) &&
        !strcmp(c->book_slug, d->book_slug) && !strcmp(c->source_sha256, d->source_sha256) &&
        !strcmp(c->content_hash, r->content_hash) && c->page_start == (int32_t)r->page &&
        c->page_end == (int32_t)r->page && c->chunk_index == (int32_t)r->chunk &&
        strlen(hit->content) == r->content_len && !memcmp(hit->content, r->content, r->content_len);
}
static const char *fetch_passages(rag_state *st, const dnd_rag_scope *scope,
    const dnd_source_passage_selection *selection, int64_t deadline, dnd_rag_result *result) {
    const dnd_source_data *data = dnd_source_artifact_index(st->source_artifact)->data;
    const dnd_source_passage *passages = dnd_source_artifact_passages(st->source_artifact, NULL);
    memset(&st->passage_hits, 0, sizeof(st->passage_hits));
    /* Keep four-record backend batches; every batch must pass before publication. */
    for (size_t offset = 0; offset < selection->record_count; offset += DND_RAG_HITS_MAX) {
        const char *ids[DND_RAG_HITS_MAX];
        size_t count = selection->record_count - offset;
        if (count > DND_RAG_HITS_MAX) count = DND_RAG_HITS_MAX;
        for (size_t i = 0; i < count; ++i) ids[i] = data->records[selection->records[offset + i]].record_id;
        if (dnd_rag_record_query_body(&st->config, scope, ids, count, st->body, sizeof(st->body)))
            return "invalid passage witness query";
        if (post_json(st, &st->query_http, deadline, 1)) return "passage witness response unavailable";
        if (dnd_rag_record_query_extract(&st->config, scope, ids, count, st->reply, &st->source_hits))
            return "passage witness provenance rejected";
        for (size_t i = 0; i < count; ++i) {
            uint32_t record = selection->records[offset + i];
            const dnd_rag_hit *hit = &st->source_hits.hits[i];
            if (!source_record_matches(st, data, &data->records[record], hit))
                return "passage witness differs from index";
            for (size_t j = 0; j < selection->count; ++j)
                if (passages[selection->passages[j]].spans[0].record == record)
                    st->passage_hits.hits[j].citation = hit->citation;
        }
    }
    for (size_t i = 0; i < selection->count; ++i) {
        const dnd_source_passage *p = &passages[selection->passages[i]];
        dnd_rag_hit *hit = &st->passage_hits.hits[i];
        dnd_rag_citation *c = &hit->citation;
        if (!c->record_id[0]) return "missing passage witness";
        memset(c->record_id, 0, sizeof(c->record_id));
        memcpy(c->passage_id, p->passage_id, sizeof(c->passage_id));
        memcpy(c->content_hash, p->text_sha256, sizeof(c->content_hash));
        c->page_start = (int32_t)p->page_start; c->page_end = (int32_t)p->page_end; c->chunk_index = 0;
        if (copy_config(c->section, sizeof(c->section), p->name)) return "invalid passage heading";
        c->witness_count = p->count;
        for (size_t j = 0; j < p->count; ++j) {
            const dnd_source_passage_span *span = &p->spans[j];
            const dnd_source_record *record = &data->records[span->record];
            dnd_rag_passage_witness *w = &c->witnesses[j];
            memcpy(w->record_id, record->record_id, sizeof(w->record_id));
            memcpy(w->content_hash, record->content_hash, sizeof(w->content_hash));
            w->page = record->page; w->chunk = record->chunk;
            w->begin = span->begin; w->end = span->end; w->record_length = (uint32_t)record->content_len;
        }
        if (dnd_rag_passage_length(c) != p->length) return "invalid passage assembly";
        memcpy(hit->content, p->text, p->length + 1u);
    }
    st->passage_hits.count = selection->count;
    *result = st->passage_hits;
    return NULL;
}

static const char *extend_sources(rag_state *st, const dnd_rag_scope *scope,
    const rag_search_request_c *req, int64_t deadline, dnd_rag_result *result) {
    const dnd_source_index *index = dnd_source_artifact_index(st->source_artifact);
    const dnd_source_data *data = index->data;
    dnd_source_anchor anchors[DND_RAG_HITS_MAX];
    dnd_source_selection selection;
    const char *ids[DND_RAG_HITS_MAX];
    for (size_t i = 0; i < result->count; ++i) {
        const dnd_rag_citation *c = &result->hits[i].citation;
        if (!dnd_source_artifact_matches_source(st->source_artifact, c)) return "search source identity rejected";
        anchors[i] = (dnd_source_anchor){UINT32_MAX, (uint32_t)c->page_start};
        for (size_t j = 0; j < data->document_count; ++j)
            if (!strcmp(c->document_id, data->documents[j].document_id)) anchors[i].document = (uint32_t)j;
    }
    dnd_source_passage_selection passages;
    if (dnd_source_artifact_select_passages(st->source_artifact, scope,
        st->config.shared_rulebook_corpus, req->query, strlen(req->query), anchors, result->count, &passages))
        return "invalid passage selection";
    if (passages.reason != DND_SOURCE_NO_MATCH) {
        if (passages.reason != DND_SOURCE_SELECTED) return "complete passage selection unavailable";
        if (passages.count > (size_t)req->top_k) return "passages exceed retrieval limit";
        return fetch_passages(st, scope, &passages, deadline, result);
    }
    if (dnd_source_index_select(index, scope, st->config.shared_rulebook_corpus, req->query, strlen(req->query),
            anchors, result->count, &selection)) return "invalid source selection";
    if (!selection.count) return NULL;
    if (selection.count > (size_t)req->top_k) return "source group exceeds retrieval limit";
    for (size_t i = 0; i < selection.count; ++i) ids[i] = data->records[selection.records[i]].record_id;
    if (dnd_rag_record_query_body(&st->config, scope, ids, selection.count, st->body, sizeof(st->body)))
        return "invalid source record query";
    if (post_json(st, &st->query_http, deadline, 1)) return "source record response unavailable";
    if (dnd_rag_record_query_extract(&st->config, scope, ids, selection.count, st->reply, &st->source_hits))
        return "source record provenance rejected";
    for (size_t i = 0; i < selection.count; ++i) {
        const dnd_source_record *r = &data->records[selection.records[i]];
        if (!source_record_matches(st, data, r, &st->source_hits.hits[i]))
            return "source record differs from index";
        st->source_hits.hits[i].citation.excerpt = selection.excerpts[i];
    }
    if (!selection.replace_baseline) {
        for (size_t i = 0; i < result->count && st->source_hits.count < (size_t)req->top_k; ++i) {
            size_t j;
            for (j = 0; j < st->source_hits.count; ++j)
                if (!strcmp(result->hits[i].citation.record_id, st->source_hits.hits[j].citation.record_id)) break;
            if (j == st->source_hits.count) st->source_hits.hits[st->source_hits.count++] = result->hits[i];
        }
    }
    *result = st->source_hits;
    return NULL;
}

static const char *retrieve(
    rag_state *st, const rag_search_request_c *req, dnd_rag_result *result
) {
    dnd_rag_identity identity;
    dnd_rag_scope scope;
    int64_t unix_now = clock_ms(CLOCK_REALTIME);
    int64_t monotonic_now = clock_ms(CLOCK_MONOTONIC);
    int timeout = st->timeout_ms;
    int64_t deadline;
    if (!st->enabled) return "retrieval backend unavailable";
    if (!unix_now || !monotonic_now) return "retrieval clock unavailable";
    if (!req->knowledge_scope[0] || !req->user_id[0] ||
        req->top_k < 1 || req->top_k > DND_RAG_HITS_MAX ||
        req->enable_rerank || req->rerank_top_k)
        return "invalid scoped retrieval request";
    identity = (dnd_rag_identity){
        req->user_id, req->session_id, req->campaign_id, req->character_id,
        req->knowledge_scope,
        req->premium || strcmp(req->knowledge_scope, "shared_rulebook") == 0,
        req->premium
    };
    if (dnd_rag_scope_resolve(&st->config, &identity, &scope) != 0 ||
        (req->collection[0] && strcmp(req->collection, scope.collection) != 0))
        return "retrieval scope denied";
    if (req->deadline_unix_ms) {
        if (req->deadline_unix_ms <= unix_now) return "retrieval deadline exceeded";
        if (req->deadline_unix_ms - unix_now < timeout)
            timeout = (int)(req->deadline_unix_ms - unix_now);
    }
    if (monotonic_now > INT64_MAX - timeout) return "retrieval clock unavailable";
    deadline = monotonic_now + timeout;
    if (scope.kind == DND_RAG_SHARED_RULEBOOK && st->config.shared_rulebook_metric == DND_RAG_SCORE_BM25) {
        if (dnd_rag_lexical_search_body(&st->config, &scope, req->query, (size_t)req->top_k,
                st->body, sizeof(st->body)) != 0) return "invalid retrieval query";
    } else {
        if (dnd_rag_embedding_body(&st->config, req->query, st->body, sizeof(st->body)) != 0)
            return "invalid retrieval query";
        if (post_json(st, &st->embed_http, deadline, 0) != 0 ||
            dnd_rag_embedding_extract(&st->config, st->reply, st->vector, sizeof(st->vector)) != 0)
            return "embedding response unavailable";
        if (dnd_rag_search_body(&st->config, &scope, st->vector, (size_t)req->top_k,
                st->body, sizeof(st->body)) != 0) return "invalid retrieval query";
    }
    if (post_json(st, &st->search_http, deadline, 1) != 0)
        return "search response unavailable";
    if (dnd_rag_search_extract(&st->config, &scope, st->reply, (size_t)req->top_k, result) != 0)
        return "search provenance rejected";
    if (st->source_artifact && scope.kind == DND_RAG_SHARED_RULEBOOK) {
        const char *error = extend_sources(st, &scope, req, deadline, result);
        if (error) return error;
    }
    if (!remaining_ms(deadline)) return "retrieval deadline exceeded";
    if (!result->count) return "no authorized retrieval hits";
    return NULL;
}

static void on_search(
    const char *subject, const char *reply, const uint8_t *data, size_t data_len, void *user
) {
    rag_state *st = (rag_state *)user;
    rag_search_request_c req;
    rag_search_response_c response = {0};
    uint8_t wire[DND_RAG_WIRE_CAP];
    const char *error;
    size_t length;
    (void)subject;
    if (pb_decode_rag_search_request(data, data_len, &req) != 0) {
        svc_log("rag-gateway", "reject malformed request bytes=%zu", data_len);
        return;
    }
    memcpy(response.request_id, req.request_id, strlen(req.request_id) + 1u);
    error = retrieve(st, &req, &response.documents);
    if (error) {
        memset(&response.documents, 0, sizeof(response.documents));
        (void)copy_config(response.error, sizeof(response.error), error);
    } else response.used_rag = 1;
    length = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    if (reply && reply[0] &&
        (!length || vbus_publish(st->nc, reply, wire, length) != 0))
        svc_log("rag-gateway", "reply failed request=%s", req.request_id);
    svc_log("rag-gateway", "search request=%s admitted_hits=%zu",
        req.request_id, response.documents.count);
}

int main(void) {
    vbus_stop_flag stop = 0;
    rag_state *st = calloc(1, sizeof(*st));
    int run_rc = -1;
    if (!st) return 1;
    if (configure(st) != 0) {
        svc_log("rag-gateway", "invalid retrieval backend configuration");
        goto done;
    }
    svc_install_signals(&stop);
    st->nc = svc_connect_bus();
    if (!st->nc) goto done;
    if (vbus_subscribe(st->nc, SUBJ_RAG_SEARCH, svc_env("VBUS_QUEUE_GROUP", "rag-gateways"),
            on_search, st) != 0) {
        svc_log("rag-gateway", "subscribe failed");
        goto done;
    }
    svc_log("rag-gateway", "pure-C scoped retrieval enabled=%d source_index=%d", st->enabled, st->source_artifact != NULL);
    run_rc = vbus_run(st->nc, &stop);
done:
    if (st->nc) vbus_close(st->nc);
    if (st->embed_http.initialized) http_min_client_destroy(&st->embed_http);
    if (st->search_http.initialized) http_min_client_destroy(&st->search_http);
    if (st->query_http.initialized) http_min_client_destroy(&st->query_http);
    dnd_source_artifact_free(st->source_artifact);
    free(st);
    return run_rc == 0 ? 0 : 1;
}
