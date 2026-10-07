#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    STORE_MAX_PENDING = 1 << 22,
    STORE_SEGMENT_BYTES = 4096,
    STORE_MAX_SEGMENTS = STORE_MAX_PENDING / STORE_SEGMENT_BYTES,
    STORE_FRAMES = 256,
    STORE_BATCHES = 101,
    STORE_MAX_FRAME = 16384,
};

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t length;
    size_t allocations;
} contiguous_store;

typedef struct {
    uint8_t *segments[STORE_MAX_SEGMENTS];
    size_t head;
    size_t length;
    size_t segment_count;
    size_t allocations;
} segmented_store;

typedef struct {
    double batches[STORE_BATCHES];
    uint64_t checksum;
    size_t allocations;
    size_t retained_bytes;
    size_t flush_vectors;
} store_result;

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static void contiguous_free(contiguous_store *store)
{
    if (!store)
        return;
    free(store->data);
    memset(store, 0, sizeof(*store));
}

static int contiguous_reserve(contiguous_store *store, size_t need)
{
    uint8_t *next;
    size_t next_capacity;
    size_t first;

    if (!store || need > STORE_MAX_PENDING)
        return -1;
    if (need <= store->capacity)
        return 0;
    next_capacity = store->capacity ? store->capacity : 256;
    while (next_capacity < need) {
        if (next_capacity > STORE_MAX_PENDING / 2) {
            next_capacity = STORE_MAX_PENDING;
            break;
        }
        next_capacity *= 2;
    }
    if (next_capacity < need)
        return -1;
    if (store->head == 0) {
        next = (uint8_t *)realloc(store->data, next_capacity);
        if (!next)
            return -1;
        store->data = next;
        store->capacity = next_capacity;
        store->allocations++;
        return 0;
    }
    next = (uint8_t *)malloc(next_capacity);
    if (!next)
        return -1;
    first = store->length;
    if (first > store->capacity - store->head)
        first = store->capacity - store->head;
    if (first != 0)
        memcpy(next, store->data + store->head, first);
    if (store->length > first)
        memcpy(next + first, store->data, store->length - first);
    free(store->data);
    store->data = next;
    store->capacity = next_capacity;
    store->head = 0;
    store->allocations++;
    return 0;
}

static int contiguous_append(
    contiguous_store *store,
    const uint8_t *data,
    size_t length
)
{
    size_t tail;
    size_t first;

    if (!store || (!data && length != 0) ||
        length > STORE_MAX_PENDING - store->length)
        return -1;
    if (length == 0)
        return 0;
    if (contiguous_reserve(store, store->length + length) != 0)
        return -1;
    tail = (store->head + store->length) % store->capacity;
    first = length;
    if (first > store->capacity - tail)
        first = store->capacity - tail;
    memcpy(store->data + tail, data, first);
    if (length > first)
        memcpy(store->data, data + first, length - first);
    store->length += length;
    return 0;
}

static int contiguous_read(
    contiguous_store *store,
    uint8_t *out,
    size_t length
)
{
    size_t first;

    if (!store || (!out && length != 0) || length > store->length)
        return -1;
    if (length == 0)
        return 0;
    first = length;
    if (first > store->capacity - store->head)
        first = store->capacity - store->head;
    memcpy(out, store->data + store->head, first);
    if (length > first)
        memcpy(out + first, store->data, length - first);
    store->head = (store->head + length) % store->capacity;
    store->length -= length;
    if (store->length == 0)
        store->head = 0;
    return 0;
}

static void segmented_free(segmented_store *store)
{
    size_t index;

    if (!store)
        return;
    for (index = 0; index < STORE_MAX_SEGMENTS; index++)
        free(store->segments[index]);
    memset(store, 0, sizeof(*store));
}

static int segmented_ensure(segmented_store *store, size_t segment)
{
    if (!store || segment >= STORE_MAX_SEGMENTS)
        return -1;
    if (store->segments[segment])
        return 0;
    store->segments[segment] = (uint8_t *)malloc(STORE_SEGMENT_BYTES);
    if (!store->segments[segment])
        return -1;
    store->segment_count++;
    store->allocations++;
    return 0;
}

static int segmented_append(
    segmented_store *store,
    const uint8_t *data,
    size_t length
)
{
    size_t position;
    size_t offset = 0;

    if (!store || (!data && length != 0) ||
        length > STORE_MAX_PENDING - store->length)
        return -1;
    position = (store->head + store->length) % STORE_MAX_PENDING;
    while (offset < length) {
        size_t segment = position / STORE_SEGMENT_BYTES;
        size_t segment_offset = position % STORE_SEGMENT_BYTES;
        size_t take = length - offset;
        if (take > STORE_SEGMENT_BYTES - segment_offset)
            take = STORE_SEGMENT_BYTES - segment_offset;
        if (segmented_ensure(store, segment) != 0)
            return -1;
        memcpy(store->segments[segment] + segment_offset, data + offset, take);
        position = (position + take) % STORE_MAX_PENDING;
        offset += take;
    }
    store->length += length;
    return 0;
}

static int segmented_read(
    segmented_store *store,
    uint8_t *out,
    size_t length
)
{
    size_t position;
    size_t offset = 0;

    if (!store || (!out && length != 0) || length > store->length)
        return -1;
    position = store->head;
    while (offset < length) {
        size_t segment = position / STORE_SEGMENT_BYTES;
        size_t segment_offset = position % STORE_SEGMENT_BYTES;
        size_t take = length - offset;
        if (!store->segments[segment])
            return -1;
        if (take > STORE_SEGMENT_BYTES - segment_offset)
            take = STORE_SEGMENT_BYTES - segment_offset;
        memcpy(out + offset, store->segments[segment] + segment_offset, take);
        position = (position + take) % STORE_MAX_PENDING;
        offset += take;
    }
    store->head = position;
    store->length -= length;
    if (store->length == 0)
        store->head = 0;
    return 0;
}

static size_t frame_size(size_t index)
{
    static const size_t sizes[] = {256, 1024, 4096, 16384};
    return sizes[index % (sizeof(sizes) / sizeof(sizes[0]))];
}

static size_t burst_bytes(void)
{
    size_t total = 0;
    size_t frame;

    for (frame = 0; frame < STORE_FRAMES; frame++)
        total += frame_size(frame);
    return total;
}

static int append_contiguous_burst(
    contiguous_store *store,
    const uint8_t *payload
)
{
    size_t frame;

    for (frame = 0; frame < STORE_FRAMES; frame++)
        if (contiguous_append(store, payload, frame_size(frame)) != 0)
            return -1;
    return 0;
}

static int append_segmented_burst(
    segmented_store *store,
    const uint8_t *payload
)
{
    size_t frame;

    for (frame = 0; frame < STORE_FRAMES; frame++)
        if (segmented_append(store, payload, frame_size(frame)) != 0)
            return -1;
    return 0;
}

static uint64_t sampled_checksum(const uint8_t *data, size_t length)
{
    uint64_t checksum = 0;
    size_t index;

    for (index = 0; index < length; index += STORE_SEGMENT_BYTES)
        checksum += data[index];
    if (length != 0)
        checksum += data[length - 1];
    return checksum;
}

static int run_contiguous(
    int cold,
    const uint8_t *payload,
    uint8_t *drain,
    store_result *result
)
{
    contiguous_store retained;
    size_t bytes = burst_bytes();
    size_t batch;

    memset(&retained, 0, sizeof(retained));
    memset(result, 0, sizeof(*result));
    if (!cold && (append_contiguous_burst(&retained, payload) != 0 ||
            contiguous_read(&retained, drain, bytes) != 0))
        return -1;
    for (batch = 0; batch < STORE_BATCHES; batch++) {
        contiguous_store temporary;
        contiguous_store *store = &retained;
        uint64_t started;

        memset(&temporary, 0, sizeof(temporary));
        if (cold)
            store = &temporary;
        started = monotonic_ns();
        if (append_contiguous_burst(store, payload) != 0 ||
            contiguous_read(store, drain, bytes) != 0) {
            contiguous_free(&temporary);
            contiguous_free(&retained);
            return -1;
        }
        result->checksum += sampled_checksum(drain, bytes);
        result->batches[batch] =
            (double)(monotonic_ns() - started) / STORE_FRAMES;
        if (cold) {
            result->allocations += temporary.allocations;
            result->retained_bytes = temporary.capacity;
            contiguous_free(&temporary);
        }
    }
    if (!cold) {
        result->allocations = retained.allocations;
        result->retained_bytes = retained.capacity;
    }
    result->flush_vectors = 1;
    contiguous_free(&retained);
    return 0;
}

static int run_segmented(
    int cold,
    const uint8_t *payload,
    uint8_t *drain,
    store_result *result
)
{
    segmented_store retained;
    size_t bytes = burst_bytes();
    size_t batch;

    memset(&retained, 0, sizeof(retained));
    memset(result, 0, sizeof(*result));
    if (!cold && (append_segmented_burst(&retained, payload) != 0 ||
            segmented_read(&retained, drain, bytes) != 0))
        return -1;
    for (batch = 0; batch < STORE_BATCHES; batch++) {
        segmented_store temporary;
        segmented_store *store = &retained;
        uint64_t started;

        memset(&temporary, 0, sizeof(temporary));
        if (cold)
            store = &temporary;
        started = monotonic_ns();
        if (append_segmented_burst(store, payload) != 0 ||
            segmented_read(store, drain, bytes) != 0) {
            segmented_free(&temporary);
            segmented_free(&retained);
            return -1;
        }
        result->checksum += sampled_checksum(drain, bytes);
        result->batches[batch] =
            (double)(monotonic_ns() - started) / STORE_FRAMES;
        if (cold) {
            result->allocations += temporary.allocations;
            result->retained_bytes =
                temporary.segment_count * STORE_SEGMENT_BYTES +
                sizeof(temporary.segments);
            segmented_free(&temporary);
        }
    }
    if (!cold) {
        result->allocations = retained.allocations;
        result->retained_bytes = retained.segment_count * STORE_SEGMENT_BYTES +
            sizeof(retained.segments);
        result->flush_vectors =
            (bytes + STORE_SEGMENT_BYTES - 1) / STORE_SEGMENT_BYTES;
    } else {
        result->flush_vectors =
            (bytes + STORE_SEGMENT_BYTES - 1) / STORE_SEGMENT_BYTES;
    }
    segmented_free(&retained);
    return 0;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static void print_result(const char *name, store_result *result, int cold)
{
    double total = 0.0;
    size_t batch;

    for (batch = 0; batch < STORE_BATCHES; batch++)
        total += result->batches[batch];
    qsort(
        result->batches,
        STORE_BATCHES,
        sizeof(result->batches[0]),
        compare_double);
    printf(
        "%s avg_ns_per_frame=%.2f p50_ns=%.2f p99_ns=%.2f "
        "allocations_per_burst=%.2f retained_bytes=%zu flush_vectors=%zu "
        "checksum=%llu\n",
        name,
        total / STORE_BATCHES,
        result->batches[STORE_BATCHES / 2],
        result->batches[(STORE_BATCHES * 99) / 100],
        cold ? (double)result->allocations / STORE_BATCHES
             : (double)result->allocations,
        result->retained_bytes,
        result->flush_vectors,
        (unsigned long long)result->checksum);
}

static uint32_t model_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static int verify_randomized_model(const uint8_t *payload)
{
    contiguous_store contiguous;
    segmented_store segmented;
    uint8_t contiguous_out[STORE_MAX_FRAME * 2];
    uint8_t segmented_out[STORE_MAX_FRAME * 2];
    uint32_t random_state = UINT32_C(0x6d2b79f5);
    size_t operation;
    int result = -1;

    memset(&contiguous, 0, sizeof(contiguous));
    memset(&segmented, 0, sizeof(segmented));
    for (operation = 0; operation < 4096; operation++) {
        uint32_t choice = model_random(&random_state);
        if (contiguous.length == 0 ||
            (contiguous.length < 1024u * 1024u && (choice & 3u) != 0u)) {
            size_t length = (size_t)(model_random(&random_state) %
                STORE_MAX_FRAME) + 1u;
            if (contiguous_append(&contiguous, payload, length) != 0 ||
                segmented_append(&segmented, payload, length) != 0)
                goto done;
        } else {
            size_t limit = contiguous.length;
            size_t length;
            if (limit > sizeof(contiguous_out))
                limit = sizeof(contiguous_out);
            length = (size_t)(model_random(&random_state) % limit) + 1u;
            if (contiguous_read(&contiguous, contiguous_out, length) != 0 ||
                segmented_read(&segmented, segmented_out, length) != 0 ||
                memcmp(contiguous_out, segmented_out, length) != 0)
                goto done;
        }
        if (contiguous.length != segmented.length ||
            contiguous.capacity > STORE_MAX_PENDING ||
            (contiguous.capacity != 0 && contiguous.head >= contiguous.capacity))
            goto done;
    }
    while (contiguous.length != 0) {
        size_t length = contiguous.length;
        if (length > sizeof(contiguous_out))
            length = sizeof(contiguous_out);
        if (contiguous_read(&contiguous, contiguous_out, length) != 0 ||
            segmented_read(&segmented, segmented_out, length) != 0 ||
            memcmp(contiguous_out, segmented_out, length) != 0)
            goto done;
    }
    if (segmented.length != 0)
        goto done;
    result = 0;
done:
    contiguous_free(&contiguous);
    segmented_free(&segmented);
    return result;
}

static int verify_model(const uint8_t *payload)
{
    contiguous_store contiguous;
    contiguous_store linear;
    segmented_store segmented;
    uint8_t contiguous_out[STORE_MAX_FRAME * 2];
    uint8_t linear_out[768];
    uint8_t segmented_out[STORE_MAX_FRAME * 2];
    size_t first = STORE_MAX_FRAME - 73;
    size_t second = STORE_MAX_FRAME / 2 + 41;

    memset(&contiguous, 0, sizeof(contiguous));
    memset(&linear, 0, sizeof(linear));
    memset(&segmented, 0, sizeof(segmented));
    if (verify_randomized_model(payload) != 0 ||
        contiguous_append(&linear, payload, 200) != 0 ||
        contiguous_append(&linear, payload, 300) != 0 ||
        contiguous_read(&linear, linear_out, 500) != 0 ||
        memcmp(linear_out, payload, 200) != 0 ||
        memcmp(linear_out + 200, payload, 300) != 0 ||
        contiguous_append(&contiguous, payload, first) != 0 ||
        segmented_append(&segmented, payload, first) != 0 ||
        contiguous_read(&contiguous, contiguous_out, first - 101) != 0 ||
        segmented_read(&segmented, segmented_out, first - 101) != 0 ||
        memcmp(contiguous_out, segmented_out, first - 101) != 0 ||
        contiguous_append(&contiguous, payload, second) != 0 ||
        segmented_append(&segmented, payload, second) != 0 ||
        contiguous_read(&contiguous, contiguous_out, second + 101) != 0 ||
        segmented_read(&segmented, segmented_out, second + 101) != 0 ||
        memcmp(contiguous_out, segmented_out, second + 101) != 0 ||
        contiguous_append(&contiguous, payload, STORE_MAX_FRAME) != 0 ||
        segmented_append(&segmented, payload, STORE_MAX_FRAME) != 0 ||
        contiguous_append(&contiguous, payload, STORE_MAX_PENDING) == 0 ||
        segmented_append(&segmented, payload, STORE_MAX_PENDING) == 0) {
        contiguous_free(&linear);
        contiguous_free(&contiguous);
        segmented_free(&segmented);
        return -1;
    }
    contiguous_free(&linear);
    contiguous_free(&contiguous);
    segmented_free(&segmented);
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t payload[STORE_MAX_FRAME];
    uint8_t *drain;
    store_result contiguous_cold;
    store_result segmented_cold;
    store_result contiguous_hot;
    store_result segmented_hot;
    size_t bytes = burst_bytes();
    size_t index;
    int verify_only = argc == 2 && strcmp(argv[1], "--verify") == 0;
    const char *cache_candidate =
        argc == 3 && strcmp(argv[1], "--cache") == 0 ? argv[2] : NULL;
    int result = 1;

    if ((argc == 2 && !verify_only) || argc > 3 ||
        (argc == 3 && !cache_candidate))
        return 2;
    for (index = 0; index < sizeof(payload); index++)
        payload[index] = (uint8_t)(index * 37u + 11u);
    if (verify_model(payload) != 0) {
        fputs("segmented storage correctness failed\n", stderr);
        return 1;
    }
    if (verify_only) {
        puts("ALL PASS VBus storage model");
        return 0;
    }
    drain = (uint8_t *)malloc(bytes);
    if (!drain)
        return 1;
    if (cache_candidate) {
        store_result cache_result;
        int cold;

        if (strcmp(cache_candidate, "contiguous_cold") == 0 ||
            strcmp(cache_candidate, "contiguous_hot") == 0) {
            cold = strcmp(cache_candidate, "contiguous_cold") == 0;
            if (run_contiguous(cold, payload, drain, &cache_result) != 0)
                goto done;
        } else if (strcmp(cache_candidate, "segmented_cold") == 0 ||
                   strcmp(cache_candidate, "segmented_hot") == 0) {
            cold = strcmp(cache_candidate, "segmented_cold") == 0;
            if (run_segmented(cold, payload, drain, &cache_result) != 0)
                goto done;
        } else {
            goto done;
        }
        printf("cache_candidate=%s checksum=%llu\n",
            cache_candidate,
            (unsigned long long)cache_result.checksum);
        result = 0;
        goto done;
    }
    if (run_contiguous(1, payload, drain, &contiguous_cold) != 0 ||
        run_segmented(1, payload, drain, &segmented_cold) != 0 ||
        run_contiguous(0, payload, drain, &contiguous_hot) != 0 ||
        run_segmented(0, payload, drain, &segmented_hot) != 0)
        goto done;
    if (contiguous_cold.checksum != segmented_cold.checksum ||
        contiguous_hot.checksum != segmented_hot.checksum)
        goto done;
    printf(
        "storage burst_frames=%d burst_bytes=%zu segment_bytes=%d\n",
        STORE_FRAMES,
        bytes,
        STORE_SEGMENT_BYTES);
    print_result("contiguous_cold", &contiguous_cold, 1);
    print_result("segmented_cold", &segmented_cold, 1);
    print_result("contiguous_hot", &contiguous_hot, 0);
    print_result("segmented_hot", &segmented_hot, 0);
    result = 0;
done:
    free(drain);
    return result;
}
