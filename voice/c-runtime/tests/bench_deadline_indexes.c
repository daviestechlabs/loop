#define _POSIX_C_SOURCE 200809L

/*
 * Compare bounded indexes for future monotonic deadlines.
 * One model scans owner slots, one uses a 10 ms wheel, and one uses a heap.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    TIMER_MAX = 1024,
    WHEEL_BUCKETS = 256,
    WHEEL_TICK_MS = 10,
    BENCH_BATCHES = 301,
    BENCH_MIN_OPS_PER_BATCH = 64,
    DENSE_TIMER_TARGET = 256,
};

#define TIMER_NONE UINT16_MAX
#define START_TICK UINT64_C(1000000)

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

typedef struct {
    uint64_t deadline;
    unsigned char active;
} timer_slot;

typedef struct {
    timer_slot slots[TIMER_MAX];
    size_t limit;
} scan_index;

typedef struct {
    timer_slot slots[TIMER_MAX];
    uint16_t heap[TIMER_MAX];
    uint16_t position[TIMER_MAX];
    size_t count;
    size_t limit;
} heap_index;

typedef struct {
    timer_slot slots[TIMER_MAX];
    uint16_t heads[WHEEL_BUCKETS];
    uint16_t next[TIMER_MAX];
    uint16_t previous[TIMER_MAX];
    uint64_t current_tick;
    size_t limit;
} wheel_index;

typedef struct {
    uint64_t checksum;
    size_t count;
    size_t work;
} deadline_result;

typedef struct {
    double samples[BENCH_BATCHES];
    double average_ns;
    double p50_ns;
    double p99_ns;
    uint64_t checksum;
    size_t maximum_work;
    int failed;
} bench_result;

typedef void (*index_init_function)(void *context, size_t limit, uint64_t now);
typedef int (*index_insert_function)(
    void *context,
    uint16_t id,
    uint64_t deadline,
    size_t *work
);
typedef int (*index_cancel_function)(void *context, uint16_t id, size_t *work);
typedef deadline_result (*index_expire_function)(void *context, uint64_t now);

typedef struct {
    const char *name;
    index_init_function init;
    index_insert_function insert;
    index_cancel_function cancel;
    index_expire_function expire;
} timer_candidate;

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static uint64_t timer_checksum(uint16_t id)
{
    uint64_t value = (uint64_t)id + UINT64_C(0x9e3779b97f4a7c15);

    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t deadline_for_id(uint16_t id, uint64_t base, uint64_t span)
{
    uint64_t value = timer_checksum(id);

    return base + 1u + value % span;
}

static void scan_init(void *context, size_t limit, uint64_t now)
{
    scan_index *index = (scan_index *)context;

    (void)now;
    memset(index, 0, sizeof(*index));
    index->limit = limit;
}

static int scan_insert(
    void *context,
    uint16_t id,
    uint64_t deadline,
    size_t *work
)
{
    scan_index *index = (scan_index *)context;

    if (!index || id >= index->limit || index->slots[id].active)
        return -1;
    index->slots[id].deadline = deadline;
    index->slots[id].active = 1;
    if (work)
        (*work)++;
    return 0;
}

static int scan_cancel(void *context, uint16_t id, size_t *work)
{
    scan_index *index = (scan_index *)context;

    if (!index || id >= index->limit || !index->slots[id].active)
        return -1;
    index->slots[id].active = 0;
    if (work)
        (*work)++;
    return 0;
}

static BENCH_NOINLINE deadline_result scan_expire(void *context, uint64_t now)
{
    scan_index *index = (scan_index *)context;
    deadline_result result = {0, 0, 0};
    size_t i;

    for (i = 0; i < index->limit; i++) {
        result.work++;
        if (!index->slots[i].active || index->slots[i].deadline > now)
            continue;
        index->slots[i].active = 0;
        result.checksum ^= timer_checksum((uint16_t)i);
        result.count++;
    }
    return result;
}

static void heap_init(void *context, size_t limit, uint64_t now)
{
    heap_index *index = (heap_index *)context;
    size_t i;

    (void)now;
    memset(index, 0, sizeof(*index));
    index->limit = limit;
    for (i = 0; i < limit; i++)
        index->position[i] = TIMER_NONE;
}

static int heap_less(
    const heap_index *index,
    uint16_t left,
    uint16_t right,
    size_t *work
)
{
    uint64_t left_deadline;
    uint64_t right_deadline;

    if (work)
        (*work)++;
    left_deadline = index->slots[left].deadline;
    right_deadline = index->slots[right].deadline;
    return left_deadline < right_deadline ||
        (left_deadline == right_deadline && left < right);
}

static void heap_swap(heap_index *index, size_t left, size_t right)
{
    uint16_t left_id = index->heap[left];
    uint16_t right_id = index->heap[right];

    index->heap[left] = right_id;
    index->heap[right] = left_id;
    index->position[left_id] = (uint16_t)right;
    index->position[right_id] = (uint16_t)left;
}

static void heap_sift_up(heap_index *index, size_t position, size_t *work)
{
    while (position != 0) {
        size_t parent = (position - 1u) / 2u;

        if (!heap_less(index, index->heap[position], index->heap[parent], work))
            break;
        heap_swap(index, position, parent);
        position = parent;
    }
}

static void heap_sift_down(heap_index *index, size_t position, size_t *work)
{
    for (;;) {
        size_t left = position * 2u + 1u;
        size_t right = left + 1u;
        size_t smallest = position;

        if (left < index->count &&
            heap_less(index, index->heap[left], index->heap[smallest], work))
            smallest = left;
        if (right < index->count &&
            heap_less(index, index->heap[right], index->heap[smallest], work))
            smallest = right;
        if (smallest == position)
            break;
        heap_swap(index, position, smallest);
        position = smallest;
    }
}

static int heap_insert(
    void *context,
    uint16_t id,
    uint64_t deadline,
    size_t *work
)
{
    heap_index *index = (heap_index *)context;
    size_t position;

    if (!index || id >= index->limit || index->slots[id].active ||
        index->count >= index->limit)
        return -1;
    position = index->count++;
    index->slots[id].deadline = deadline;
    index->slots[id].active = 1;
    index->heap[position] = id;
    index->position[id] = (uint16_t)position;
    heap_sift_up(index, position, work);
    return 0;
}

static uint16_t heap_remove_at(heap_index *index, size_t position, size_t *work)
{
    uint16_t removed = index->heap[position];

    index->count--;
    index->position[removed] = TIMER_NONE;
    index->slots[removed].active = 0;
    if (position != index->count) {
        uint16_t replacement = index->heap[index->count];

        index->heap[position] = replacement;
        index->position[replacement] = (uint16_t)position;
        if (position != 0 && heap_less(
                index,
                replacement,
                index->heap[(position - 1u) / 2u],
                work)) {
            heap_sift_up(index, position, work);
        } else {
            heap_sift_down(index, position, work);
        }
    }
    return removed;
}

static int heap_cancel(void *context, uint16_t id, size_t *work)
{
    heap_index *index = (heap_index *)context;
    uint16_t position;

    if (!index || id >= index->limit || !index->slots[id].active)
        return -1;
    position = index->position[id];
    if (position == TIMER_NONE || position >= index->count)
        return -1;
    (void)heap_remove_at(index, position, work);
    return 0;
}

static BENCH_NOINLINE deadline_result heap_expire(void *context, uint64_t now)
{
    heap_index *index = (heap_index *)context;
    deadline_result result = {0, 0, 0};

    while (index->count != 0) {
        uint16_t id = index->heap[0];

        result.work++;
        if (index->slots[id].deadline > now)
            break;
        id = heap_remove_at(index, 0, &result.work);
        result.checksum ^= timer_checksum(id);
        result.count++;
    }
    return result;
}

static void wheel_init(void *context, size_t limit, uint64_t now)
{
    wheel_index *index = (wheel_index *)context;
    size_t i;

    memset(index, 0, sizeof(*index));
    index->limit = limit;
    index->current_tick = now;
    for (i = 0; i < WHEEL_BUCKETS; i++)
        index->heads[i] = TIMER_NONE;
    for (i = 0; i < limit; i++) {
        index->next[i] = TIMER_NONE;
        index->previous[i] = TIMER_NONE;
    }
}

static size_t wheel_bucket(uint64_t deadline)
{
    return (size_t)(deadline & (WHEEL_BUCKETS - 1u));
}

static void wheel_unlink(wheel_index *index, uint16_t id)
{
    size_t bucket = wheel_bucket(index->slots[id].deadline);
    uint16_t previous = index->previous[id];
    uint16_t next = index->next[id];

    if (previous == TIMER_NONE)
        index->heads[bucket] = next;
    else
        index->next[previous] = next;
    if (next != TIMER_NONE)
        index->previous[next] = previous;
    index->next[id] = TIMER_NONE;
    index->previous[id] = TIMER_NONE;
}

static int wheel_insert(
    void *context,
    uint16_t id,
    uint64_t deadline,
    size_t *work
)
{
    wheel_index *index = (wheel_index *)context;
    size_t bucket;
    uint16_t head;

    if (!index || id >= index->limit || index->slots[id].active ||
        deadline <= index->current_tick)
        return -1;
    bucket = wheel_bucket(deadline);
    head = index->heads[bucket];
    index->slots[id].deadline = deadline;
    index->slots[id].active = 1;
    index->previous[id] = TIMER_NONE;
    index->next[id] = head;
    if (head != TIMER_NONE)
        index->previous[head] = id;
    index->heads[bucket] = id;
    if (work)
        (*work)++;
    return 0;
}

static int wheel_cancel(void *context, uint16_t id, size_t *work)
{
    wheel_index *index = (wheel_index *)context;

    if (!index || id >= index->limit || !index->slots[id].active)
        return -1;
    wheel_unlink(index, id);
    index->slots[id].active = 0;
    if (work)
        (*work)++;
    return 0;
}

static void wheel_expire_id(
    wheel_index *index,
    uint16_t id,
    deadline_result *result
)
{
    wheel_unlink(index, id);
    index->slots[id].active = 0;
    result->checksum ^= timer_checksum(id);
    result->count++;
}

static BENCH_NOINLINE deadline_result wheel_expire(void *context, uint64_t now)
{
    wheel_index *index = (wheel_index *)context;
    deadline_result result = {0, 0, 0};

    if (now <= index->current_tick)
        return result;
    if (now - index->current_tick > WHEEL_BUCKETS) {
        size_t i;

        for (i = 0; i < index->limit; i++) {
            result.work++;
            if (index->slots[i].active && index->slots[i].deadline <= now)
                wheel_expire_id(index, (uint16_t)i, &result);
        }
        index->current_tick = now;
        return result;
    }
    while (index->current_tick < now) {
        uint16_t id;
        size_t bucket;

        index->current_tick++;
        bucket = wheel_bucket(index->current_tick);
        result.work++;
        id = index->heads[bucket];
        while (id != TIMER_NONE) {
            uint16_t next = index->next[id];

            result.work++;
            if (index->slots[id].deadline <= index->current_tick)
                wheel_expire_id(index, id, &result);
            id = next;
        }
    }
    return result;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static void finalize_benchmark(bench_result *result)
{
    double total = 0.0;
    size_t i;

    if (result->failed)
        return;
    for (i = 0; i < BENCH_BATCHES; i++)
        total += result->samples[i];
    result->average_ns = total / BENCH_BATCHES;
    qsort(result->samples, BENCH_BATCHES, sizeof(result->samples[0]), compare_double);
    result->p50_ns = result->samples[BENCH_BATCHES / 2];
    result->p99_ns = result->samples[(BENCH_BATCHES * 99) / 100];
}

static size_t operations_for_count(size_t count)
{
    size_t operations = BENCH_MIN_OPS_PER_BATCH;

    if (count <= 4)
        operations *= 8;
    else if (count <= 32)
        operations *= 4;
    else if (count <= 68)
        operations *= 2;
    return operations;
}

static int build_far_timers(
    const timer_candidate *candidate,
    void *context,
    size_t count,
    uint64_t now
)
{
    size_t i;

    candidate->init(context, count, now);
    for (i = 0; i < count; i++) {
        size_t work = 0;
        uint64_t deadline = deadline_for_id(
            (uint16_t)i,
            now + UINT64_C(10000000),
            UINT64_C(65536));

        if (candidate->insert(context, (uint16_t)i, deadline, &work) != 0)
            return -1;
    }
    return 0;
}

static void run_idle_benchmark(
    const timer_candidate *candidate,
    void *context,
    size_t count,
    bench_result *result
)
{
    size_t operations = operations_for_count(count);
    size_t batch;
    uint64_t now = START_TICK;

    memset(result, 0, sizeof(*result));
    if (build_far_timers(candidate, context, count, now) != 0) {
        result->failed = 1;
        return;
    }
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations; operation++) {
            deadline_result expired;

            now += 10;
            expired = candidate->expire(context, now);
            if (expired.count != 0) {
                result->failed = 1;
                return;
            }
            result->checksum ^= expired.checksum + now;
            if (expired.work > result->maximum_work)
                result->maximum_work = expired.work;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations;
    }
    finalize_benchmark(result);
}

static void run_churn_benchmark(
    const timer_candidate *candidate,
    void *context,
    size_t count,
    bench_result *result
)
{
    size_t operations = operations_for_count(count);
    size_t batch;
    size_t serial = 0;

    memset(result, 0, sizeof(*result));
    if (build_far_timers(candidate, context, count, START_TICK) != 0) {
        result->failed = 1;
        return;
    }
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations; operation++) {
            uint16_t id = (uint16_t)((serial * 4051u + 17u) % count);
            uint64_t deadline = START_TICK + UINT64_C(2000000) +
                ((uint64_t)serial * 37u + id) % UINT64_C(65536);
            size_t work = 0;

            if (candidate->cancel(context, id, &work) != 0 ||
                candidate->insert(context, id, deadline, &work) != 0) {
                result->failed = 1;
                return;
            }
            result->checksum ^= timer_checksum(id) + (uint64_t)serial;
            if (work > result->maximum_work)
                result->maximum_work = work;
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations;
    }
    finalize_benchmark(result);
}

static void run_dense_benchmark(
    const timer_candidate *candidate,
    void *context,
    size_t count,
    bench_result *result
)
{
    size_t cycles = DENSE_TIMER_TARGET / count;
    size_t batch;
    uint64_t base = START_TICK;

    if (cycles == 0)
        cycles = 1;
    memset(result, 0, sizeof(*result));
    candidate->init(context, count, base);
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t cycle;

        for (cycle = 0; cycle < cycles; cycle++) {
            deadline_result expired;
            size_t work = 0;
            size_t i;

            for (i = 0; i < count; i++) {
                uint64_t deadline = base + 1u + i % 10u;

                if (candidate->insert(
                        context, (uint16_t)i, deadline, &work) != 0) {
                    result->failed = 1;
                    return;
                }
            }
            expired = candidate->expire(context, base + 10u);
            if (expired.count != count) {
                result->failed = 1;
                return;
            }
            work += expired.work;
            if (work > result->maximum_work)
                result->maximum_work = work;
            result->checksum ^= expired.checksum + base + (uint64_t)cycle;
            base += 20u;
        }
        result->samples[batch] = (double)(monotonic_ns() - started) /
            ((double)cycles * (double)count);
    }
    finalize_benchmark(result);
}

static int same_expiry(deadline_result left, deadline_result right)
{
    return left.count == right.count && left.checksum == right.checksum;
}

static int verify_models(void)
{
    scan_index scan;
    heap_index heap;
    wheel_index wheel;
    size_t i;
    uint64_t now;

    scan_init(&scan, TIMER_MAX, START_TICK);
    heap_init(&heap, TIMER_MAX, START_TICK);
    wheel_init(&wheel, TIMER_MAX, START_TICK);
    for (i = 0; i < TIMER_MAX; i++) {
        uint64_t deadline = deadline_for_id(
            (uint16_t)i, START_TICK, UINT64_C(10000));
        size_t work = 0;

        if (scan_insert(&scan, (uint16_t)i, deadline, &work) != 0 ||
            heap_insert(&heap, (uint16_t)i, deadline, &work) != 0 ||
            wheel_insert(&wheel, (uint16_t)i, deadline, &work) != 0)
            return -1;
    }
    for (i = 0; i < TIMER_MAX; i += 7) {
        size_t work = 0;

        if (scan_cancel(&scan, (uint16_t)i, &work) != 0 ||
            heap_cancel(&heap, (uint16_t)i, &work) != 0 ||
            wheel_cancel(&wheel, (uint16_t)i, &work) != 0)
            return -1;
        if ((i % 14u) == 0u) {
            uint64_t deadline = START_TICK + UINT64_C(11000) + i;

            if (scan_insert(&scan, (uint16_t)i, deadline, &work) != 0 ||
                heap_insert(&heap, (uint16_t)i, deadline, &work) != 0 ||
                wheel_insert(&wheel, (uint16_t)i, deadline, &work) != 0)
                return -1;
        }
    }
    for (now = START_TICK + 17u; now <= START_TICK + UINT64_C(12000); now += 17u) {
        deadline_result scan_result = scan_expire(&scan, now);
        deadline_result heap_result = heap_expire(&heap, now);
        deadline_result wheel_result = wheel_expire(&wheel, now);

        if (!same_expiry(scan_result, heap_result) ||
            !same_expiry(scan_result, wheel_result))
            return -1;
    }
    {
        deadline_result scan_result = scan_expire(&scan, START_TICK + UINT64_C(20000));
        deadline_result heap_result = heap_expire(&heap, START_TICK + UINT64_C(20000));
        deadline_result wheel_result = wheel_expire(&wheel, START_TICK + UINT64_C(20000));

        if (!same_expiry(scan_result, heap_result) ||
            !same_expiry(scan_result, wheel_result))
            return -1;
    }

    scan_init(&scan, 4, START_TICK);
    heap_init(&heap, 4, START_TICK);
    wheel_init(&wheel, 4, START_TICK);
    for (i = 0; i < 4; i++) {
        uint64_t deadline = START_TICK + 300u + (uint64_t)i * 200u;
        size_t work = 0;

        if (scan_insert(&scan, (uint16_t)i, deadline, &work) != 0 ||
            heap_insert(&heap, (uint16_t)i, deadline, &work) != 0 ||
            wheel_insert(&wheel, (uint16_t)i, deadline, &work) != 0)
            return -1;
    }
    {
        deadline_result scan_result = scan_expire(&scan, START_TICK + 600u);
        deadline_result heap_result = heap_expire(&heap, START_TICK + 600u);
        deadline_result wheel_result = wheel_expire(&wheel, START_TICK + 600u);

        if (!same_expiry(scan_result, heap_result) ||
            !same_expiry(scan_result, wheel_result))
            return -1;
    }
    return 0;
}

static void print_result(
    size_t count,
    const char *operation,
    const timer_candidate *candidate,
    const bench_result *result
)
{
    printf(
        "count=%zu operation=%s candidate=%s avg_ns=%.2f p50_ns=%.2f "
        "p99_ns=%.2f mops=%.3f max_work=%zu checksum=%" PRIu64 "\n",
        count,
        operation,
        candidate->name,
        result->average_ns,
        result->p50_ns,
        result->p99_ns,
        1000.0 / result->average_ns,
        result->maximum_work,
        result->checksum);
}

static int run_scale(size_t count)
{
    static const timer_candidate candidates[] = {
        {"scan", scan_init, scan_insert, scan_cancel, scan_expire},
        {"timing_wheel", wheel_init, wheel_insert, wheel_cancel, wheel_expire},
        {"indexed_heap", heap_init, heap_insert, heap_cancel, heap_expire},
    };
    scan_index scan;
    wheel_index wheel;
    heap_index heap;
    void *contexts[] = {&scan, &wheel, &heap};
    bench_result idle[3];
    bench_result churn[3];
    bench_result dense[3];
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        run_idle_benchmark(&candidates[i], contexts[i], count, &idle[i]);
        run_churn_benchmark(&candidates[i], contexts[i], count, &churn[i]);
        run_dense_benchmark(&candidates[i], contexts[i], count, &dense[i]);
        if (idle[i].failed || churn[i].failed || dense[i].failed)
            return -1;
    }
    if (idle[0].checksum != idle[1].checksum ||
        idle[0].checksum != idle[2].checksum ||
        churn[0].checksum != churn[1].checksum ||
        churn[0].checksum != churn[2].checksum ||
        dense[0].checksum != dense[1].checksum ||
        dense[0].checksum != dense[2].checksum)
        return -1;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        print_result(count, "idle_tick", &candidates[i], &idle[i]);
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        print_result(count, "cancel_insert", &candidates[i], &churn[i]);
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        print_result(count, "dense_lifecycle", &candidates[i], &dense[i]);
    printf(
        "count=%zu memory common_timer_bytes=%zu scan_index_bytes=0 "
        "wheel_index_bytes=%zu heap_index_bytes=%zu tick_ms=%d\n",
        count,
        sizeof(timer_slot) * count,
        sizeof(uint16_t) * (WHEEL_BUCKETS + count * 2u),
        sizeof(uint16_t) * count * 2u,
        WHEEL_TICK_MS);
    return 0;
}

int main(int argc, char **argv)
{
    static const size_t scales[] = {4, 16, 32, 36, 64, 68, 256, 1024};
    size_t i;

    if (verify_models() != 0) {
        fprintf(stderr, "deadline index model failed\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        puts("deadline index model: PASS");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    for (i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (run_scale(scales[i]) != 0)
            return 1;
    }
    return 0;
}
