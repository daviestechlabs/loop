#define _POSIX_C_SOURCE 200809L

/* Compare lane-local worker queues with a bounded shared MPSC queue. */

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    WORKER_LANES = 4,
    TTS_QUEUE_CAPACITY = 16,
    CASCADE_QUEUE_CAPACITY = 8,
    SHARED_QUEUE_CAPACITY = WORKER_LANES * TTS_QUEUE_CAPACITY,
    BENCH_BATCHES = 301,
    BENCH_OPERATIONS = 256,
    THREAD_OPERATIONS = 100000,
    CACHE_OPERATIONS = 5000000,
    WORK_HISTOGRAM_MAX = SHARED_QUEUE_CAPACITY,
};

typedef struct {
    uint32_t producer;
    uint32_t sequence;
} queue_item;

typedef struct linked_node linked_node;

struct linked_node {
    linked_node *next;
    queue_item item;
};

typedef struct {
    linked_node nodes[TTS_QUEUE_CAPACITY];
    linked_node *head;
    linked_node *tail;
    size_t count;
} linked_queue;

typedef struct {
    uint8_t slots[TTS_QUEUE_CAPACITY];
    queue_item jobs[TTS_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} index_ring;

typedef struct {
    atomic_size_t sequence;
    queue_item item;
} mpsc_slot;

typedef struct {
    mpsc_slot slots[SHARED_QUEUE_CAPACITY];
    atomic_size_t enqueue_position;
    size_t dequeue_position;
} mpsc_queue;

typedef struct {
    queue_item slots[TTS_QUEUE_CAPACITY];
    atomic_size_t head;
    atomic_size_t tail;
} spsc_queue;

typedef struct {
    double samples[BENCH_BATCHES];
    size_t work_histogram[WORK_HISTOGRAM_MAX + 1u];
    double average_ns;
    double p50_ns;
    double p99_ns;
    double average_work;
    size_t p50_work;
    size_t p99_work;
    size_t maximum_work;
    size_t work_total;
    size_t operation_count;
    uint64_t checksum;
    int failed;
} bench_result;

typedef void (*queue_init_function)(void *context);
typedef int (*queue_enqueue_function)(void *context, queue_item item);
typedef int (*queue_dequeue_function)(void *context, queue_item *item);
typedef int (*queue_find_function)(
    const void *context,
    uint32_t sequence,
    size_t *work
);

typedef struct {
    const char *name;
    size_t capacity;
    queue_init_function init;
    queue_enqueue_function enqueue;
    queue_dequeue_function dequeue;
    queue_find_function find;
} queue_candidate;

typedef struct {
    spsc_queue *spsc;
    mpsc_queue *mpsc;
    atomic_int *start;
    atomic_int *failed;
    uint32_t producer;
    uint64_t checksum;
} thread_context;

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void linked_init(void *context)
{
    memset(context, 0, sizeof(linked_queue));
}

static int linked_enqueue(void *context, queue_item item)
{
    linked_queue *queue = (linked_queue *)context;
    linked_node *node;

    if (!queue || queue->count >= TTS_QUEUE_CAPACITY)
        return -1;
    node = &queue->nodes[item.sequence % TTS_QUEUE_CAPACITY];
    node->next = NULL;
    node->item = item;
    if (queue->tail)
        queue->tail->next = node;
    else
        queue->head = node;
    queue->tail = node;
    queue->count++;
    return 0;
}

static int linked_dequeue(void *context, queue_item *item)
{
    linked_queue *queue = (linked_queue *)context;
    linked_node *node;

    if (!queue || !item || !queue->head)
        return -1;
    node = queue->head;
    queue->head = node->next;
    if (!queue->head)
        queue->tail = NULL;
    queue->count--;
    *item = node->item;
    return 0;
}

static int linked_find(
    const void *context,
    uint32_t sequence,
    size_t *work
)
{
    const linked_queue *queue = (const linked_queue *)context;
    const linked_node *node;

    if (!queue)
        return 0;
    for (node = queue->head; node; node = node->next) {
        if (work)
            (*work)++;
        if (node->item.sequence == sequence)
            return 1;
    }
    return 0;
}

static void ring_init(void *context)
{
    memset(context, 0, sizeof(index_ring));
}

static int ring_enqueue(void *context, queue_item item)
{
    index_ring *queue = (index_ring *)context;
    size_t index;
    size_t position;

    if (!queue || queue->count >= TTS_QUEUE_CAPACITY)
        return -1;
    index = item.sequence % TTS_QUEUE_CAPACITY;
    position = (queue->head + queue->count) % TTS_QUEUE_CAPACITY;
    queue->jobs[index] = item;
    queue->slots[position] = (uint8_t)index;
    queue->count++;
    return 0;
}

static int ring_dequeue(void *context, queue_item *item)
{
    index_ring *queue = (index_ring *)context;
    size_t index;

    if (!queue || !item || queue->count == 0)
        return -1;
    index = queue->slots[queue->head];
    queue->head = (queue->head + 1u) % TTS_QUEUE_CAPACITY;
    queue->count--;
    *item = queue->jobs[index];
    return 0;
}

static int ring_find(
    const void *context,
    uint32_t sequence,
    size_t *work
)
{
    const index_ring *queue = (const index_ring *)context;
    size_t offset;

    if (!queue)
        return 0;
    for (offset = 0; offset < queue->count; offset++) {
        size_t position = (queue->head + offset) % TTS_QUEUE_CAPACITY;
        size_t index = queue->slots[position];

        if (work)
            (*work)++;
        if (queue->jobs[index].sequence == sequence)
            return 1;
    }
    return 0;
}

static void mpsc_init(void *context)
{
    mpsc_queue *queue = (mpsc_queue *)context;
    size_t i;

    memset(queue, 0, sizeof(*queue));
    for (i = 0; i < SHARED_QUEUE_CAPACITY; i++)
        atomic_init(&queue->slots[i].sequence, i);
    atomic_init(&queue->enqueue_position, 0);
}

static int mpsc_enqueue(void *context, queue_item item)
{
    mpsc_queue *queue = (mpsc_queue *)context;
    mpsc_slot *slot;
    size_t position;

    if (!queue)
        return -1;
    position = atomic_load_explicit(
        &queue->enqueue_position, memory_order_relaxed);
    for (;;) {
        size_t sequence;
        intptr_t difference;

        slot = &queue->slots[position % SHARED_QUEUE_CAPACITY];
        sequence = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        difference = (intptr_t)sequence - (intptr_t)position;
        if (difference == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &queue->enqueue_position,
                    &position,
                    position + 1u,
                    memory_order_relaxed,
                    memory_order_relaxed))
                break;
        } else if (difference < 0) {
            return -1;
        } else {
            position = atomic_load_explicit(
                &queue->enqueue_position, memory_order_relaxed);
        }
    }
    slot->item = item;
    atomic_store_explicit(&slot->sequence, position + 1u, memory_order_release);
    return 0;
}

static int mpsc_dequeue(void *context, queue_item *item)
{
    mpsc_queue *queue = (mpsc_queue *)context;
    mpsc_slot *slot;
    size_t position;
    size_t sequence;
    intptr_t difference;

    if (!queue || !item)
        return -1;
    position = queue->dequeue_position;
    slot = &queue->slots[position % SHARED_QUEUE_CAPACITY];
    sequence = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    difference = (intptr_t)sequence - (intptr_t)(position + 1u);
    if (difference != 0)
        return -1;
    *item = slot->item;
    queue->dequeue_position = position + 1u;
    atomic_store_explicit(
        &slot->sequence,
        position + SHARED_QUEUE_CAPACITY,
        memory_order_release);
    return 0;
}

static int mpsc_find(
    const void *context,
    uint32_t sequence,
    size_t *work
)
{
    const mpsc_queue *queue = (const mpsc_queue *)context;
    size_t end;
    size_t position;

    if (!queue)
        return 0;
    end = atomic_load_explicit(
        &queue->enqueue_position, memory_order_acquire);
    for (position = queue->dequeue_position; position < end; position++) {
        const mpsc_slot *slot =
            &queue->slots[position % SHARED_QUEUE_CAPACITY];
        size_t slot_sequence = atomic_load_explicit(
            &slot->sequence, memory_order_acquire);

        if (slot_sequence != position + 1u)
            continue;
        if (work)
            (*work)++;
        if (slot->item.sequence == sequence)
            return 1;
    }
    return 0;
}

static void spsc_init(spsc_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    atomic_init(&queue->head, 0);
    atomic_init(&queue->tail, 0);
}

static int spsc_enqueue(spsc_queue *queue, queue_item item)
{
    size_t tail;
    size_t head;

    if (!queue)
        return -1;
    tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    head = atomic_load_explicit(&queue->head, memory_order_acquire);
    if (tail - head >= TTS_QUEUE_CAPACITY)
        return -1;
    queue->slots[tail % TTS_QUEUE_CAPACITY] = item;
    atomic_store_explicit(&queue->tail, tail + 1u, memory_order_release);
    return 0;
}

static int spsc_dequeue(spsc_queue *queue, queue_item *item)
{
    size_t head;
    size_t tail;

    if (!queue || !item)
        return -1;
    head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    if (head == tail)
        return -1;
    *item = queue->slots[head % TTS_QUEUE_CAPACITY];
    atomic_store_explicit(&queue->head, head + 1u, memory_order_release);
    return 0;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static void record_work(bench_result *result, size_t work)
{
    size_t bucket = work;

    if (bucket > WORK_HISTOGRAM_MAX)
        bucket = WORK_HISTOGRAM_MAX;
    result->work_histogram[bucket]++;
    result->work_total += work;
    result->operation_count++;
    if (work > result->maximum_work)
        result->maximum_work = work;
}

static size_t work_quantile(const bench_result *result, size_t numerator)
{
    size_t target = (result->operation_count * numerator + 99u) / 100u;
    size_t seen = 0;
    size_t i;

    for (i = 0; i <= WORK_HISTOGRAM_MAX; i++) {
        seen += result->work_histogram[i];
        if (seen >= target)
            return i;
    }
    return WORK_HISTOGRAM_MAX;
}

static void finalize_result(bench_result *result)
{
    double total = 0.0;
    size_t i;

    if (result->failed)
        return;
    for (i = 0; i < BENCH_BATCHES; i++)
        total += result->samples[i];
    result->average_ns = total / BENCH_BATCHES;
    qsort(
        result->samples,
        BENCH_BATCHES,
        sizeof(result->samples[0]),
        compare_double);
    result->p50_ns = result->samples[BENCH_BATCHES / 2u];
    result->p99_ns = result->samples[(BENCH_BATCHES * 99u) / 100u];
    if (result->operation_count != 0) {
        result->average_work =
            (double)result->work_total / (double)result->operation_count;
        result->p50_work = work_quantile(result, 50);
        result->p99_work = work_quantile(result, 99);
    }
}

static void benchmark_roundtrip(
    const queue_candidate *candidate,
    void *context,
    bench_result *result
)
{
    size_t serial = 0;
    size_t batch;

    memset(result, 0, sizeof(*result));
    candidate->init(context);
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            queue_item input = {
                (uint32_t)(serial % WORKER_LANES),
                (uint32_t)serial,
            };
            queue_item output;

            if (candidate->enqueue(context, input) != 0 ||
                candidate->dequeue(context, &output) != 0 ||
                output.producer != input.producer ||
                output.sequence != input.sequence) {
                result->failed = 1;
                return;
            }
            result->checksum ^= mix64(
                ((uint64_t)output.producer << 32) | output.sequence);
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;
    }
    finalize_result(result);
}

static void benchmark_cancel_scan(
    const queue_candidate *candidate,
    void *context,
    bench_result *result
)
{
    size_t serial = 0;
    size_t i;
    size_t batch;

    memset(result, 0, sizeof(*result));
    candidate->init(context);
    for (i = 0; i < candidate->capacity; i++) {
        queue_item item = {0, (uint32_t)i};

        if (candidate->enqueue(context, item) != 0) {
            result->failed = 1;
            return;
        }
    }
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            uint32_t query = (serial & 1u) == 0 ?
                (uint32_t)(serial % candidate->capacity) : UINT32_MAX;
            size_t work = 0;
            int found = candidate->find(context, query, &work);
            int expected = query != UINT32_MAX;

            if (found != expected) {
                result->failed = 1;
                return;
            }
            result->checksum ^= mix64((uint64_t)query + serial);
            record_work(result, work);
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;
    }
    finalize_result(result);
}

static int verify_candidate(
    const queue_candidate *candidate,
    void *context
)
{
    queue_item item;
    size_t cycle;
    size_t i;

    candidate->init(context);
    if (candidate->dequeue(context, &item) == 0)
        return -1;
    for (i = 0; i < candidate->capacity; i++) {
        queue_item input = {(uint32_t)(i % WORKER_LANES), (uint32_t)i};

        if (candidate->enqueue(context, input) != 0)
            return -1;
    }
    item = (queue_item){0, UINT32_MAX};
    if (candidate->enqueue(context, item) == 0)
        return -1;
    for (i = 0; i < candidate->capacity; i++) {
        if (candidate->dequeue(context, &item) != 0 || item.sequence != i)
            return -1;
    }
    for (cycle = 0; cycle < 1000; cycle++) {
        queue_item input = {
            (uint32_t)(cycle % WORKER_LANES),
            (uint32_t)(cycle + candidate->capacity),
        };

        if (candidate->enqueue(context, input) != 0 ||
            candidate->dequeue(context, &item) != 0 ||
            item.producer != input.producer ||
            item.sequence != input.sequence)
            return -1;
    }
    return 0;
}

static void await_start(const atomic_int *start)
{
    while (atomic_load_explicit(start, memory_order_acquire) == 0)
        sched_yield();
}

static void *spsc_producer_thread(void *argument)
{
    thread_context *context = (thread_context *)argument;
    uint32_t sequence;

    await_start(context->start);
    for (sequence = 0; sequence < THREAD_OPERATIONS; sequence++) {
        queue_item item = {context->producer, sequence};

        while (spsc_enqueue(context->spsc, item) != 0)
            sched_yield();
    }
    return NULL;
}

static void *spsc_consumer_thread(void *argument)
{
    thread_context *context = (thread_context *)argument;
    uint32_t expected;

    await_start(context->start);
    for (expected = 0; expected < THREAD_OPERATIONS; expected++) {
        queue_item item;

        while (spsc_dequeue(context->spsc, &item) != 0)
            sched_yield();
        if (item.producer != context->producer || item.sequence != expected)
            atomic_store_explicit(context->failed, 1, memory_order_relaxed);
        context->checksum ^= mix64(
            ((uint64_t)item.producer << 32) | item.sequence);
    }
    return NULL;
}

static void *mpsc_producer_thread(void *argument)
{
    thread_context *context = (thread_context *)argument;
    uint32_t sequence;

    await_start(context->start);
    for (sequence = 0; sequence < THREAD_OPERATIONS; sequence++) {
        queue_item item = {context->producer, sequence};

        while (mpsc_enqueue(context->mpsc, item) != 0)
            sched_yield();
    }
    return NULL;
}

static void *mpsc_consumer_thread(void *argument)
{
    thread_context *context = (thread_context *)argument;
    uint32_t expected[WORKER_LANES] = {0};
    size_t total = (size_t)WORKER_LANES * THREAD_OPERATIONS;
    size_t consumed;

    await_start(context->start);
    for (consumed = 0; consumed < total; consumed++) {
        queue_item item;

        while (mpsc_dequeue(context->mpsc, &item) != 0)
            sched_yield();
        if (item.producer >= WORKER_LANES ||
            item.sequence != expected[item.producer]++)
            atomic_store_explicit(context->failed, 1, memory_order_relaxed);
        context->checksum ^= mix64(
            ((uint64_t)item.producer << 32) | item.sequence);
    }
    return NULL;
}

static int verify_concurrent_queues(void)
{
    spsc_queue spsc[WORKER_LANES];
    mpsc_queue mpsc;
    pthread_t spsc_producers[WORKER_LANES];
    pthread_t spsc_consumers[WORKER_LANES];
    pthread_t mpsc_producers[WORKER_LANES];
    pthread_t mpsc_consumer;
    thread_context producer_contexts[WORKER_LANES];
    thread_context consumer_contexts[WORKER_LANES];
    thread_context mpsc_producer_contexts[WORKER_LANES];
    thread_context mpsc_consumer_context;
    atomic_int start;
    atomic_int failed;
    uint64_t spsc_checksum = 0;
    uint64_t expected_checksum = 0;
    size_t i;

    atomic_init(&start, 0);
    atomic_init(&failed, 0);
    for (i = 0; i < WORKER_LANES; i++) {
        uint32_t sequence;

        spsc_init(&spsc[i]);
        producer_contexts[i] = (thread_context){
            &spsc[i], NULL, &start, &failed, (uint32_t)i, 0};
        consumer_contexts[i] = producer_contexts[i];
        if (pthread_create(
                &spsc_producers[i], NULL,
                spsc_producer_thread, &producer_contexts[i]) != 0 ||
            pthread_create(
                &spsc_consumers[i], NULL,
                spsc_consumer_thread, &consumer_contexts[i]) != 0)
            return -1;
        for (sequence = 0; sequence < THREAD_OPERATIONS; sequence++)
            expected_checksum ^= mix64(((uint64_t)i << 32) | sequence);
    }
    atomic_store_explicit(&start, 1, memory_order_release);
    for (i = 0; i < WORKER_LANES; i++) {
        if (pthread_join(spsc_producers[i], NULL) != 0 ||
            pthread_join(spsc_consumers[i], NULL) != 0)
            return -1;
        spsc_checksum ^= consumer_contexts[i].checksum;
    }
    if (atomic_load_explicit(&failed, memory_order_relaxed) != 0 ||
        spsc_checksum != expected_checksum)
        return -1;

    mpsc_init(&mpsc);
    atomic_store_explicit(&start, 0, memory_order_relaxed);
    atomic_store_explicit(&failed, 0, memory_order_relaxed);
    memset(&mpsc_consumer_context, 0, sizeof(mpsc_consumer_context));
    mpsc_consumer_context.mpsc = &mpsc;
    mpsc_consumer_context.start = &start;
    mpsc_consumer_context.failed = &failed;
    if (pthread_create(
            &mpsc_consumer, NULL,
            mpsc_consumer_thread, &mpsc_consumer_context) != 0)
        return -1;
    for (i = 0; i < WORKER_LANES; i++) {
        mpsc_producer_contexts[i] = (thread_context){
            NULL, &mpsc, &start, &failed, (uint32_t)i, 0};
        if (pthread_create(
                &mpsc_producers[i], NULL,
                mpsc_producer_thread, &mpsc_producer_contexts[i]) != 0)
            return -1;
    }
    atomic_store_explicit(&start, 1, memory_order_release);
    for (i = 0; i < WORKER_LANES; i++) {
        if (pthread_join(mpsc_producers[i], NULL) != 0)
            return -1;
    }
    if (pthread_join(mpsc_consumer, NULL) != 0 ||
        atomic_load_explicit(&failed, memory_order_relaxed) != 0 ||
        mpsc_consumer_context.checksum != expected_checksum)
        return -1;
    return 0;
}

static void print_result(
    const char *operation,
    const queue_candidate *candidate,
    const bench_result *result
)
{
    printf(
        "operation=%s candidate=%s avg_ns=%.2f p50_ns=%.2f "
        "p99_ns=%.2f mops=%.3f avg_work=%.2f p50_work=%zu "
        "p99_work=%zu max_work=%zu checksum=%" PRIu64 "\n",
        operation,
        candidate->name,
        result->average_ns,
        result->p50_ns,
        result->p99_ns,
        1000.0 / result->average_ns,
        result->average_work,
        result->p50_work,
        result->p99_work,
        result->maximum_work,
        result->checksum);
}

static int run_cache_workload(
    const queue_candidate *candidate,
    void *context
)
{
    uint64_t checksum = 0;
    size_t operation;

    candidate->init(context);
    for (operation = 0; operation < CACHE_OPERATIONS; operation++) {
        queue_item input = {
            (uint32_t)(operation % WORKER_LANES),
            (uint32_t)operation,
        };
        queue_item output;

        if (candidate->enqueue(context, input) != 0 ||
            candidate->dequeue(context, &output) != 0)
            return -1;
        checksum ^= mix64(
            ((uint64_t)output.producer << 32) | output.sequence);
    }
    printf("cache_candidate=%s checksum=%" PRIu64 "\n", candidate->name, checksum);
    return 0;
}

int main(int argc, char **argv)
{
    static const queue_candidate candidates[] = {
        {
            "linked_lanes",
            TTS_QUEUE_CAPACITY,
            linked_init,
            linked_enqueue,
            linked_dequeue,
            linked_find,
        },
        {
            "index_ring_lanes",
            TTS_QUEUE_CAPACITY,
            ring_init,
            ring_enqueue,
            ring_dequeue,
            ring_find,
        },
        {
            "shared_mpsc",
            SHARED_QUEUE_CAPACITY,
            mpsc_init,
            mpsc_enqueue,
            mpsc_dequeue,
            mpsc_find,
        },
    };
    linked_queue linked;
    index_ring ring;
    mpsc_queue mpsc;
    void *contexts[] = {&linked, &ring, &mpsc};
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (verify_candidate(&candidates[i], contexts[i]) != 0) {
            fprintf(stderr, "worker queue model failed: %s\n", candidates[i].name);
            return 1;
        }
    }
    if (argc == 3 && strcmp(argv[1], "--cache") == 0) {
        for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (strcmp(argv[2], candidates[i].name) == 0)
                return run_cache_workload(&candidates[i], contexts[i]) == 0 ? 0 : 1;
        }
        fprintf(stderr, "unknown cache candidate: %s\n", argv[2]);
        return 2;
    }
    if (verify_concurrent_queues() != 0) {
        fprintf(stderr, "concurrent worker queue model failed\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        puts("worker queue models: PASS");
        return 0;
    }
    if (argc != 1) {
        fprintf(
            stderr,
            "usage: %s [--verify|--cache linked_lanes|"
            "index_ring_lanes|shared_mpsc]\n",
            argv[0]);
        return 2;
    }
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        bench_result roundtrip;
        bench_result cancel;

        benchmark_roundtrip(&candidates[i], contexts[i], &roundtrip);
        benchmark_cancel_scan(&candidates[i], contexts[i], &cancel);
        if (roundtrip.failed || cancel.failed)
            return 1;
        print_result("enqueue_dequeue", &candidates[i], &roundtrip);
        print_result("cancel_scan", &candidates[i], &cancel);
    }
    printf(
        "metadata tts_linked_bytes=%zu tts_ring_bytes=%zu "
        "cascade_linked_bytes=%zu cascade_ring_bytes=%zu "
        "shared_mpsc_bytes=%zu\n",
        (size_t)WORKER_LANES *
            (2u * sizeof(void *) + sizeof(size_t)) +
            (size_t)WORKER_LANES *
                (TTS_QUEUE_CAPACITY + 1u) * sizeof(void *),
        (size_t)WORKER_LANES *
            (TTS_QUEUE_CAPACITY * sizeof(uint8_t) + 2u * sizeof(size_t)),
        (size_t)WORKER_LANES *
            (2u * sizeof(void *) + sizeof(size_t)) +
            (size_t)WORKER_LANES *
                (CASCADE_QUEUE_CAPACITY + 1u) * sizeof(void *),
        (size_t)WORKER_LANES *
            (CASCADE_QUEUE_CAPACITY * sizeof(uint8_t) + 2u * sizeof(size_t)),
        sizeof(mpsc_queue));
    printf(
        "burst_jobs=%d linked_signal_calls=%d ring_transition_signals=%d "
        "shared_transition_signals=1 lane_consumers=%d shared_consumers=1\n",
        SHARED_QUEUE_CAPACITY,
        SHARED_QUEUE_CAPACITY,
        WORKER_LANES,
        WORKER_LANES);
    return 0;
}
