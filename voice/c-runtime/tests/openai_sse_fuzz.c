/* Coverage-guided harness for bounded model-I/O JSON and SSE input. */
#include "openai_min.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_CAPTURE_CAP 20000u

typedef struct {
    char content[FUZZ_CAPTURE_CAP];
    size_t content_len;
    size_t calls;
    int feed_result;
    int finish_result;
    int finish_reason;
    int done;
    int failed;
} fuzz_result;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int capture_delta(const char *content, size_t content_len, void *user) {
    fuzz_result *result = (fuzz_result *)user;
    if (!result || !content || content_len >= sizeof(result->content) - result->content_len)
        return -1;
    memcpy(result->content + result->content_len, content, content_len);
    result->content_len += content_len;
    result->content[result->content_len] = '\0';
    result->calls++;
    return 0;
}

static void decode_stream(
    const uint8_t *data,
    size_t size,
    size_t chunk_size,
    fuzz_result *result
) {
    openai_sse_decoder decoder;
    size_t offset = 0;
    memset(result, 0, sizeof(*result));
    if (openai_sse_init(&decoder, capture_delta, result) != 0) {
        result->feed_result = -1;
        result->finish_result = -1;
        return;
    }
    result->feed_result = 0;
    while (offset < size) {
        size_t take = size - offset;
        if (take > chunk_size) take = chunk_size;
        if (openai_sse_feed(
                &decoder, (const char *)data + offset, take) != 0) {
            result->feed_result = -1;
            break;
        }
        offset += take;
    }
    result->finish_result = result->feed_result == 0 ?
        openai_sse_finish(&decoder) : -1;
    result->finish_reason = decoder.finish_reason;
    result->done = decoder.done;
    result->failed = decoder.failed;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_result whole;
    fuzz_result bytes;
    char transcript[2048];
    size_t transcript_len;
    size_t whole_chunk = size == 0u ? 1u : size;
    if (size > 0u && size <= 8u) {
        char requested[9];
        uint32_t server_limit = 1u + (uint32_t)data[0] * 16u;
        memcpy(requested, data, size);
        requested[size] = '\0';
        if (openai_completion_limit(server_limit, requested) > server_limit) abort();
    }
    (void)stt_json_extract_transcript(
        (const char *)data, size,
        transcript, sizeof(transcript), &transcript_len);
    decode_stream(data, size, whole_chunk, &whole);
    decode_stream(data, size, 1u, &bytes);
    if (whole.feed_result != bytes.feed_result ||
        whole.finish_result != bytes.finish_result ||
        whole.finish_reason != bytes.finish_reason ||
        whole.done != bytes.done || whole.failed != bytes.failed ||
        whole.calls != bytes.calls || whole.content_len != bytes.content_len ||
        memcmp(whole.content, bytes.content, whole.content_len) != 0)
        abort();
    return 0;
}
