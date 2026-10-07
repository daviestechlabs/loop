/* Read-only diagnostic client for an operator-owned C RAG process on VBus. */
#define _POSIX_C_SOURCE 200809L
#include "vbus.h"
#include "pb_min.h"
#include "subjects.h"
#include "dnd_retrieval.h"
#include "dnd_grounding.h"
#include "ent_books.h"
#include "http_min.h"
#include "openai_min.h"
#include "speech_display_stream.h"
#include "utf8.h"

#include <inttypes.h>
#include <limits.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Match the cascade's 256-token and 2047-byte answer admission. */
typedef struct {
    dnd_grounding_prompt prompt;
    openai_sse_decoder decoder;
    speech_display_acc_v1 display;
    char model[128], text[2048];
    size_t text_len, deltas;
    int64_t started_ms, first_text_ms, elapsed_ms;
} generation_result;

static int64_t clock_ms(clockid_t clock_id);

static int collect_delta(const char *content, size_t length, void *user) {
    generation_result *result = user;
    char display_delta[2048];
    size_t display_length = 0;
    if (!length || length >= sizeof(result->text) - result->text_len ||
        speech_display_acc_add_v1(&result->display, content, length, display_delta,
            sizeof(display_delta) - 1u, &display_length) != SPEECH_DISPLAY_OK) return -1;
    if (display_length && result->first_text_ms < 0) {
        int64_t now = clock_ms(CLOCK_MONOTONIC);
        if (!now || now < result->started_ms) return -1;
        result->first_text_ms = now - result->started_ms;
    }
    memcpy(result->text + result->text_len, content, length);
    result->text_len += length;
    result->text[result->text_len] = '\0';
    ++result->deltas;
    return 0;
}

static int collect_body(const uint8_t *data, size_t length, void *user) {
    generation_result *result = user;
    return openai_sse_feed(&result->decoder, (const char *)data, length);
}

static int64_t clock_ms(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now) || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)(INT64_MAX - 999) / 1000u) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int copy_text(char *out, size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (!length || length >= capacity ||
        !utf8_validate_v1((const uint8_t *)value, length)) return -1;
    memcpy(out, value, length + 1u);
    return 0;
}

static int hash_suffix(const char *value, const char *prefix) {
    size_t offset = strlen(prefix), i;
    if (strlen(value) != offset + 64u || strncmp(value, prefix, offset)) return 0;
    for (i = offset; i < offset + 64u; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}

static int append(char *out, size_t *used, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static int append(char *out, size_t *used, const char *format, ...) {
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(out + *used, DND_RAG_PUBLIC_JSON_CAP - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= DND_RAG_PUBLIC_JSON_CAP - *used) return -1;
    *used += (size_t)written;
    return 0;
}

static int field(char *out, size_t *used, const char *key, const char *value) {
    char escaped[6u * 2048u + 1u];
    if (cmp_json_escape_exact(value, escaped, sizeof(escaped))) return -1;
    return append(out, used, "\"%s\":\"%s\",", key, escaped);
}

static int generate(const char *url, const char *context, generation_result *result) {
    static char body[(DND_RAG_PROMPT_CAP + DND_GROUNDING_TEXT_CAP) * 6u + 1024u];
    const http_min_header headers[] = {{"Accept", "text/event-stream"}};
    size_t length, received = 0, canonical_length = 0;
    const char *canonical;
    int status = 0, rc;
    int64_t started = clock_ms(CLOCK_MONOTONIC), ended;
    result->started_ms = started;
    result->first_text_ms = -1;
    speech_display_acc_init_v1(&result->display);
    if (!started || openai_sse_init(&result->decoder, collect_delta, result)) return -1;
    length = openai_chat_request_json_stream_system(body, sizeof(body), result->model,
        result->prompt.text, result->prompt.text_len, context, strlen(context),
        256u, DND_RAG_PROMPT_CAP - 1u);
    if (!length) return -1;
    rc = http_min_post_stream_headers_cancel(url, "application/json", headers, 1u,
        (const uint8_t *)body, length, collect_body, result,
        4u * 1024u * 1024u, &received, &status, 60000, NULL);
    ended = clock_ms(CLOCK_MONOTONIC);
    if (rc != HTTP_MIN_OK || status < 200 || status >= 300 || !received ||
        !ended || ended < started || !result->text_len || result->first_text_ms < 0 ||
        result->first_text_ms > ended - started ||
        openai_sse_finish(&result->decoder) ||
        result->decoder.finish_reason != OPENAI_FINISH_STOP) return -1;
    result->elapsed_ms = ended - started;
    canonical = speech_display_acc_canonical_ascii_v1(&result->display, &canonical_length);
    if (canonical && canonical_length < sizeof(result->text)) {
        memcpy(result->text, canonical, canonical_length);
        result->text[canonical_length] = '\0';
        result->text_len = canonical_length;
    }
    return result->text_len ? 0 : -1;
}

static int project(const rag_search_response_c *response, const char *corpus, const char *question,
                   const char *prompt, int premium, int64_t elapsed, const generation_result *generation,
                   char *out, size_t *used) {
    size_t i;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char question_hash[65], prompt_hash[65];
    if (!SHA256((const unsigned char *)question, strlen(question), digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i)
        (void)snprintf(question_hash + 2u * i, 3u, "%02x", (unsigned)digest[i]);
    if (!SHA256((const unsigned char *)prompt, strlen(prompt), digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i)
        (void)snprintf(prompt_hash + 2u * i, 3u, "%02x", (unsigned)digest[i]);
    if (append(out, used, "{\"schema\":\"dnd-c-rag-probe/v2\",") ||
        field(out, used, "case", response->request_id) ||
        field(out, used, "question_sha256", question_hash) ||
        field(out, used, "grounded_prompt_sha256", prompt_hash) ||
        field(out, used, "corpus_id", corpus) ||
        field(out, used, "error", response->error) ||
        append(out, used, "\"premium\":%s,\"latency_ms\":%" PRId64 ",\"documents\":[",
            premium ? "true" : "false", elapsed)) return -1;
    for (i = 0; i < response->documents.count; ++i) {
        const dnd_rag_citation *c = &response->documents.hits[i].citation;
        if (append(out, used, "%s{", i ? "," : "") ||
            field(out, used, "source", c->source) || field(out, used, "book_slug", c->book_slug) ||
            field(out, used, "collection", c->collection) || field(out, used, "corpus_version", c->corpus_version) ||
            field(out, used, "embedding_model", c->embedding_model) || field(out, used, "record_id", c->record_id) ||
            field(out, used, "document_id", c->document_id) || field(out, used, "section", c->section) ||
            field(out, used, "content_hash", c->content_hash) || field(out, used, "source_sha256", c->source_sha256) ||
            field(out, used, "score_metric", dnd_rag_score_metric_name(c->score_metric)) ||
            append(out, used, "\"page_start\":%" PRId32 ",\"page_end\":%" PRId32
                ",\"chunk_index\":%" PRId32 ",\"score\":%.17g,\"excerpt_spans\":[",
                c->page_start, c->page_end, c->chunk_index, c->score)) return -1;
        for (size_t j = 0; j < c->excerpt.count; ++j)
            if (append(out, used, "%s{\"begin\":%" PRIu32 ",\"end\":%" PRIu32 "}",
                j ? "," : "", c->excerpt.spans[j].begin, c->excerpt.spans[j].end)) return -1;
        if (append(out, used, "]")) return -1;
        if (c->witness_count) {
            if (append(out, used, ",") || field(out, used, "kind", "complete_passage") ||
                field(out, used, "passage_id", c->passage_id) || append(out, used, "\"witnesses\":[")) return -1;
            for (size_t j = 0; j < c->witness_count; ++j) {
                const dnd_rag_passage_witness *w = &c->witnesses[j];
                if (append(out, used, "%s{", j ? "," : "") || field(out, used, "record_id", w->record_id) ||
                    field(out, used, "content_hash", w->content_hash) ||
                    append(out, used, "\"page\":%u,\"chunk_index\":%u,\"begin\":%u,\"end\":%u,\"record_length\":%u}",
                        w->page, w->chunk, w->begin, w->end, w->record_length)) return -1;
            }
            if (append(out, used, "]")) return -1;
        }
        if (append(out, used, "}")) return -1;
    }
    if (append(out, used, "]")) return -1;
    if (generation && (append(out, used, ",\"generation\":{") ||
        field(out, used, "model", generation->model) ||
        field(out, used, "prompt_id", generation->prompt.identity.id) ||
        field(out, used, "prompt_version", generation->prompt.identity.version) ||
        field(out, used, "prompt_sha256", generation->prompt.identity.sha256) ||
        field(out, used, "answer", generation->text) ||
        append(out, used, "\"finish_reason\":\"stop\",\"max_completion_tokens\":256,"
            "\"deltas\":%zu,\"first_text_ms\":%" PRId64 ",\"latency_ms\":%" PRId64 "}",
            generation->deltas, generation->first_text_ms, generation->elapsed_ms))) return -1;
    return append(out, used, "}\n");
}

int main(int argc, char **argv) {
    rag_search_request_c request = {0};
    rag_search_response_c response;
    uint8_t wire[8192], reply[DND_RAG_WIRE_CAP];
    char prompt[DND_RAG_PROMPT_CAP] = {0}, output[DND_RAG_PUBLIC_JSON_CAP];
    size_t length, reply_length = 0, used = 0, i;
    int64_t started, ended, now;
    int status;
    int with_generation = argc == 11;
    static generation_result generation;
    http_min_url model_url;
    vbus_client *client;
    if ((argc != 7 && !with_generation) || (strcmp(argv[6], "0") && strcmp(argv[6], "1")) ||
        !hash_suffix(argv[3], "dnd_text_chunks_c_") || !hash_suffix(argv[5], "sha256:") ||
        copy_text(request.request_id, sizeof(request.request_id), argv[2]) ||
        copy_text(request.collection, sizeof(request.collection), argv[3]) ||
        copy_text(request.query, sizeof(request.query), argv[4]) ||
        (with_generation && (strcmp(argv[7], "--generate") ||
            http_min_parse_url(argv[8], &model_url) != HTTP_MIN_OK ||
            strcmp(model_url.host, "127.0.0.1") ||
            strcmp(model_url.path, "/v1/chat/completions") ||
            copy_text(generation.model, sizeof(generation.model), argv[9]) ||
            !argv[10][0] || dnd_grounding_load(argv[10], &generation.prompt)))) {
        fputs("usage: c-rag-probe SOCKET CASE CANDIDATE_COLLECTION QUERY CORPUS_SHA256 PREMIUM_0_OR_1"
            " [--generate LOOPBACK_CHAT_URL MODEL PROMPT_LIBRARY_ROOT]\n", stderr);
        return 2;
    }
    memcpy(request.user_id, "candidate-quality-probe", sizeof("candidate-quality-probe"));
    memcpy(request.knowledge_scope, "shared_rulebook", sizeof("shared_rulebook"));
    request.premium = argv[6][0] == '1';
    request.top_k = DND_RAG_HITS_MAX;
    now = clock_ms(CLOCK_REALTIME);
    started = clock_ms(CLOCK_MONOTONIC);
    if (!now || !started || now > INT64_MAX - 30000) return 2;
    request.deadline_unix_ms = now + 30000;
    length = pb_encode_rag_search_request(wire, sizeof(wire), &request);
    if (!length || !(client = vbus_connect(argv[1]))) {
        fputs("C RAG probe could not connect or encode\n", stderr);
        return 2;
    }
    status = vbus_request(client, SUBJ_RAG_SEARCH, wire, length,
        reply, sizeof(reply), &reply_length, 30000);
    vbus_close(client);
    ended = clock_ms(CLOCK_MONOTONIC);
    if (status || !ended || ended < started ||
        pb_decode_rag_search_response(reply, reply_length, &response) ||
        strcmp(response.request_id, request.request_id) ||
        response.used_rag != (response.documents.count != 0u)) goto invalid;
    for (i = 0; i < response.documents.count; ++i) {
        const dnd_rag_citation *c = &response.documents.hits[i].citation;
        if (strcmp(c->collection, request.collection) || strcmp(c->corpus_version, argv[5]) ||
            strcmp(c->embedding_model, "bge-m3") ||
            !ent_book_allowed(ent_book_access_mask(request.premium), c->book_slug)) goto invalid;
    }
    /* Validate every content hash before optional model I/O or output. */
    if (response.documents.count && dnd_rag_grounded_prompt(
            &response.documents, request.query, prompt, sizeof(prompt))) goto invalid;
    if (with_generation && response.documents.count && generate(argv[8], prompt, &generation)) {
        fputs("C RAG probe rejected model transport or incomplete generation\n", stderr);
        return 2;
    }
    if (project(&response, argv[5], request.query, prompt, request.premium, ended - started,
            with_generation && response.documents.count ? &generation : NULL, output, &used)) goto invalid;
    if (fwrite(output, 1, used, stdout) != used || fflush(stdout)) return 2;
    return response.documents.count ? 0 : 1;
invalid:
    fputs("C RAG probe rejected transport or provenance\n", stderr);
    return 2;
}
