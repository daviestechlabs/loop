#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    GROUP_COUNT = 8,
    MEMBERS_PER_GROUP = 4,
    MATCH_COUNT = GROUP_COUNT * MEMBERS_PER_GROUP,
    BROKER_MATCH_CAPACITY = 64 * 64,
    BENCH_BATCHES = 301,
    BENCH_OPERATIONS = 512,
};

typedef struct {
    char queue[16];
    uint16_t group_slot;
    uint16_t member;
} queue_match;

typedef struct {
    double samples[BENCH_BATCHES];
    uint64_t checksum;
    size_t maximum_work;
} bench_result;

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static BENCH_NOINLINE uint64_t deliver_fanout(
    const queue_match *matches,
    size_t *work_out
)
{
    uint64_t checksum = 0;
    size_t index;

    for (index = 0; index < MATCH_COUNT; index++)
        checksum += (uint64_t)matches[index].member + 1;
    *work_out = MATCH_COUNT;
    return checksum;
}

static BENCH_NOINLINE uint64_t deliver_global_string_groups(
    const queue_match *matches,
    uint32_t *global_cursor,
    size_t *work_out
)
{
    unsigned char delivered[MATCH_COUNT] = {0};
    uint64_t checksum = 0;
    size_t work = 0;
    size_t index;

    for (index = 0; index < MATCH_COUNT; index++) {
        size_t group[MATCH_COUNT];
        size_t group_count = 0;
        size_t candidate;

        if (delivered[index]) continue;
        for (candidate = index; candidate < MATCH_COUNT; candidate++) {
            work++;
            if (strcmp(matches[candidate].queue, matches[index].queue) == 0) {
                group[group_count++] = candidate;
                delivered[candidate] = 1;
            }
        }
        if (group_count != 0) {
            size_t pick = *global_cursor % group_count;
            const queue_match *selected = &matches[group[pick]];
            (*global_cursor)++;
            checksum += ((uint64_t)selected->group_slot << 32) |
                (uint64_t)selected->member;
        }
    }
    *work_out = work;
    return checksum;
}

static BENCH_NOINLINE uint64_t deliver_indexed_groups(
    const queue_match *matches,
    uint32_t cursors[GROUP_COUNT],
    size_t *work_out,
    uint32_t member_counts[GROUP_COUNT][MEMBERS_PER_GROUP]
)
{
    unsigned char delivered[MATCH_COUNT] = {0};
    uint64_t checksum = 0;
    size_t work = 0;
    size_t index;

    for (index = 0; index < MATCH_COUNT; index++) {
        size_t group[MATCH_COUNT];
        size_t group_count = 0;
        size_t candidate;
        uint16_t slot;

        if (delivered[index]) continue;
        slot = matches[index].group_slot;
        for (candidate = index; candidate < MATCH_COUNT; candidate++) {
            work++;
            if (matches[candidate].group_slot == slot) {
                group[group_count++] = candidate;
                delivered[candidate] = 1;
            }
        }
        if (group_count != 0) {
            size_t pick = cursors[slot] % group_count;
            const queue_match *selected = &matches[group[pick]];
            cursors[slot]++;
            if (member_counts)
                member_counts[slot][selected->member]++;
            checksum += ((uint64_t)selected->group_slot << 32) |
                (uint64_t)selected->member;
        }
    }
    *work_out = work;
    return checksum;
}

static BENCH_NOINLINE uint64_t clear_delivery_flags(
    unsigned char flags[BROKER_MATCH_CAPACITY],
    size_t clear_count,
    size_t match_count
)
{
    memset(flags, 0, clear_count);
    return (uint64_t)flags[0] + (uint64_t)flags[match_count - 1];
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static void print_result(const char *name, bench_result *result)
{
    double total = 0.0;
    size_t batch;

    for (batch = 0; batch < BENCH_BATCHES; batch++)
        total += result->samples[batch];
    qsort(
        result->samples,
        BENCH_BATCHES,
        sizeof(result->samples[0]),
        compare_double);
    printf(
        "%s avg_ns=%.2f p50_ns=%.2f p99_ns=%.2f max_work=%zu checksum=%llu\n",
        name,
        total / BENCH_BATCHES,
        result->samples[BENCH_BATCHES / 2],
        result->samples[(BENCH_BATCHES * 99) / 100],
        result->maximum_work,
        (unsigned long long)result->checksum);
}

static int verify_round_robin(const queue_match *matches)
{
    uint32_t cursors[GROUP_COUNT] = {0};
    uint32_t member_counts[GROUP_COUNT][MEMBERS_PER_GROUP] = {{0}};
    size_t delivery;
    size_t group;
    size_t member;

    for (delivery = 0; delivery < MEMBERS_PER_GROUP * 3; delivery++) {
        size_t work;
        (void)deliver_indexed_groups(matches, cursors, &work, member_counts);
    }
    for (group = 0; group < GROUP_COUNT; group++)
        for (member = 0; member < MEMBERS_PER_GROUP; member++)
            if (member_counts[group][member] != 3)
                return -1;
    return 0;
}

static int verify_bounded_clear(void)
{
    unsigned char full[BROKER_MATCH_CAPACITY];
    unsigned char bounded[BROKER_MATCH_CAPACITY];

    memset(full, 0xa5, sizeof(full));
    memset(bounded, 0xa5, sizeof(bounded));
    if (clear_delivery_flags(full, sizeof(full), MATCH_COUNT) != 0 ||
        clear_delivery_flags(bounded, MATCH_COUNT, MATCH_COUNT) != 0 ||
        memcmp(full, bounded, MATCH_COUNT) != 0)
        return -1;
    if (full[MATCH_COUNT] != 0 || bounded[MATCH_COUNT] != 0xa5)
        return -1;
    return 0;
}

static void run_benchmarks(
    const queue_match *matches,
    bench_result *fanout,
    bench_result *global_string,
    bench_result *indexed
)
{
    uint32_t global_cursor = 0;
    uint32_t group_cursors[GROUP_COUNT] = {0};
    size_t batch;

    memset(fanout, 0, sizeof(*fanout));
    memset(global_string, 0, sizeof(*global_string));
    memset(indexed, 0, sizeof(*indexed));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started;
        size_t operation;

        started = monotonic_ns();
        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            size_t work;
            fanout->checksum += deliver_fanout(matches, &work) + operation;
            if (work > fanout->maximum_work) fanout->maximum_work = work;
        }
        fanout->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;

        started = monotonic_ns();
        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            size_t work;
            global_string->checksum +=
                deliver_global_string_groups(matches, &global_cursor, &work) + operation;
            if (work > global_string->maximum_work)
                global_string->maximum_work = work;
        }
        global_string->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;

        started = monotonic_ns();
        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            size_t work;
            indexed->checksum +=
                deliver_indexed_groups(matches, group_cursors, &work, NULL) + operation;
            if (work > indexed->maximum_work) indexed->maximum_work = work;
        }
        indexed->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;
    }
}

static void run_clear_benchmarks(bench_result *full, bench_result *bounded)
{
    unsigned char full_flags[BROKER_MATCH_CAPACITY];
    unsigned char bounded_flags[BROKER_MATCH_CAPACITY];
    size_t batch;

    memset(full, 0, sizeof(*full));
    memset(bounded, 0, sizeof(*bounded));
    memset(full_flags, 0xa5, sizeof(full_flags));
    memset(bounded_flags, 0xa5, sizeof(bounded_flags));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started;
        size_t operation;

        started = monotonic_ns();
        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            full_flags[operation % MATCH_COUNT] = 0xa5;
            full->checksum += clear_delivery_flags(
                full_flags, sizeof(full_flags), MATCH_COUNT) + operation;
        }
        full->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;
        full->maximum_work = sizeof(full_flags);

        started = monotonic_ns();
        for (operation = 0; operation < BENCH_OPERATIONS; operation++) {
            bounded_flags[operation % MATCH_COUNT] = 0xa5;
            bounded->checksum += clear_delivery_flags(
                bounded_flags, MATCH_COUNT, MATCH_COUNT) + operation;
        }
        bounded->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPERATIONS;
        bounded->maximum_work = MATCH_COUNT;
    }
}

int main(int argc, char **argv)
{
    queue_match matches[MATCH_COUNT];
    bench_result fanout;
    bench_result global_string;
    bench_result indexed;
    bench_result full_clear;
    bench_result bounded_clear;
    size_t index;
    int verify_only = argc == 2 && strcmp(argv[1], "--verify") == 0;

    if (argc > 2 || (argc == 2 && !verify_only)) return 2;
    for (index = 0; index < MATCH_COUNT; index++) {
        size_t group = index % GROUP_COUNT;
        int written = snprintf(
            matches[index].queue,
            sizeof(matches[index].queue),
            "queue-%zu",
            group);

        if (written < 0 || (size_t)written >= sizeof(matches[index].queue))
            return 1;
        matches[index].group_slot = (uint16_t)group;
        matches[index].member = (uint16_t)(index / GROUP_COUNT);
    }
    if (verify_round_robin(matches) != 0 || verify_bounded_clear() != 0) {
        fputs("queue-group model failed\n", stderr);
        return 1;
    }
    run_benchmarks(matches, &fanout, &global_string, &indexed);
    run_clear_benchmarks(&full_clear, &bounded_clear);
    if (verify_only) {
        puts("ALL PASS VBus queue-group model");
        return 0;
    }
    puts("queue_groups=8 members_per_group=4 matches=32");
    print_result("queue_fanout", &fanout);
    print_result("queue_global_string", &global_string);
    print_result("queue_indexed_cursor", &indexed);
    print_result("queue_full_delivery_clear", &full_clear);
    print_result("queue_bounded_delivery_clear", &bounded_clear);
    return 0;
}
