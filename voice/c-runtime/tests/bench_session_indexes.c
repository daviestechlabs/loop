#define _POSIX_C_SOURCE 200809L

/* Compare the voice session index with bounded hopscotch hashing. */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SESSION_LIMIT = 1024,
    KEY_TOTAL = SESSION_LIMIT * 2,
    HOP_LARGE_CAPACITY = SESSION_LIMIT * 2,
    HOP_NEIGHBORHOOD = 32,
    CAPACITY_TRIALS = 64,
    BENCH_BATCHES = 301,
    BENCH_OPS_PER_BATCH = 256,
    WORK_HISTOGRAM_MAX = 4096,
};

#define INDEX_NONE UINT16_MAX

enum linear_slot_state {
    LINEAR_EMPTY = 0,
    LINEAR_USED = 1,
    LINEAR_TOMBSTONE = 2,
};

typedef struct {
    uint16_t identifiers[HOP_LARGE_CAPACITY];
    unsigned char states[HOP_LARGE_CAPACITY];
    size_t capacity;
    size_t count;
    size_t limit;
} linear_index;

typedef struct {
    uint16_t identifiers[HOP_LARGE_CAPACITY];
    uint32_t neighborhoods[HOP_LARGE_CAPACITY];
    size_t capacity;
    size_t count;
    size_t limit;
} hop_index;

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
    uint64_t checksum;
    size_t work_total;
    size_t operation_count;
    int failed;
} bench_result;

typedef void (*index_init_function)(void *context, size_t capacity, size_t limit);
typedef int (*index_insert_function)(void *context, uint16_t id, size_t *work);
typedef int (*index_find_function)(
    const void *context,
    uint16_t id,
    uint16_t *stored_id,
    size_t *work
);
typedef int (*index_remove_function)(void *context, uint16_t id, size_t *work);

typedef struct {
    const char *name;
    size_t capacity;
    index_init_function init;
    index_insert_function insert;
    index_find_function find;
    index_remove_function remove;
} index_candidate;

static char session_keys[KEY_TOTAL][48];
static uint64_t session_hashes[KEY_TOTAL];

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

static uint64_t hash_key(const char *key)
{
    const unsigned char *cursor = (const unsigned char *)key;
    uint64_t hash = UINT64_C(1469598103934665603);

    while (*cursor) {
        hash ^= *cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int initialize_keys(void)
{
    size_t i;

    for (i = 0; i < KEY_TOTAL; i++) {
        int written = snprintf(
            session_keys[i],
            sizeof(session_keys[i]),
            "session-%04zu-%016" PRIx64,
            i,
            mix64(i + UINT64_C(0x9e3779b97f4a7c15)));

        if (written < 0 || (size_t)written >= sizeof(session_keys[i]))
            return -1;
        session_hashes[i] = hash_key(session_keys[i]);
    }
    return 0;
}

static void linear_init(void *context, size_t capacity, size_t limit)
{
    linear_index *index = (linear_index *)context;

    memset(index, 0, sizeof(*index));
    index->capacity = capacity;
    index->limit = limit;
}

static int linear_find(
    const void *context,
    uint16_t id,
    uint16_t *stored_id,
    size_t *work
)
{
    const linear_index *index = (const linear_index *)context;
    size_t start;
    size_t probe;

    if (!index || id >= KEY_TOTAL)
        return -1;
    start = (size_t)(session_hashes[id] & (index->capacity - 1u));
    for (probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start + probe) & (index->capacity - 1u);

        if (work)
            (*work)++;
        if (index->states[slot] == LINEAR_EMPTY)
            return 0;
        if (index->states[slot] == LINEAR_USED &&
            strcmp(
                session_keys[index->identifiers[slot]],
                session_keys[id]) == 0) {
            if (stored_id)
                *stored_id = index->identifiers[slot];
            return 1;
        }
    }
    return 0;
}

static int linear_insert(void *context, uint16_t id, size_t *work)
{
    linear_index *index = (linear_index *)context;
    size_t first_tombstone;
    size_t start;
    size_t probe;

    if (!index || id >= KEY_TOTAL)
        return -1;
    first_tombstone = index->capacity;
    start = (size_t)(session_hashes[id] & (index->capacity - 1u));
    for (probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start + probe) & (index->capacity - 1u);

        if (work)
            (*work)++;
        if (index->states[slot] == LINEAR_EMPTY) {
            if (first_tombstone == index->capacity)
                first_tombstone = slot;
            break;
        }
        if (index->states[slot] == LINEAR_TOMBSTONE) {
            if (first_tombstone == index->capacity)
                first_tombstone = slot;
            continue;
        }
        if (strcmp(
                session_keys[index->identifiers[slot]],
                session_keys[id]) == 0)
            return -2;
    }
    if (index->count >= index->limit || first_tombstone == index->capacity)
        return -1;
    index->identifiers[first_tombstone] = id;
    index->states[first_tombstone] = LINEAR_USED;
    index->count++;
    return 0;
}

static int linear_remove(void *context, uint16_t id, size_t *work)
{
    linear_index *index = (linear_index *)context;
    size_t start;
    size_t probe;

    if (!index || id >= KEY_TOTAL)
        return -1;
    start = (size_t)(session_hashes[id] & (index->capacity - 1u));
    for (probe = 0; probe < index->capacity; probe++) {
        size_t slot = (start + probe) & (index->capacity - 1u);

        if (work)
            (*work)++;
        if (index->states[slot] == LINEAR_EMPTY)
            return -1;
        if (index->states[slot] == LINEAR_USED &&
            strcmp(
                session_keys[index->identifiers[slot]],
                session_keys[id]) == 0) {
            index->states[slot] = LINEAR_TOMBSTONE;
            index->identifiers[slot] = INDEX_NONE;
            index->count--;
            return 0;
        }
    }
    return -1;
}

static void hop_init(void *context, size_t capacity, size_t limit)
{
    hop_index *index = (hop_index *)context;
    size_t i;

    memset(index, 0, sizeof(*index));
    index->capacity = capacity;
    index->limit = limit;
    for (i = 0; i < capacity; i++)
        index->identifiers[i] = INDEX_NONE;
}

static unsigned trailing_zeroes(uint32_t value)
{
    unsigned count = 0;

    while ((value & 1u) == 0u) {
        value >>= 1;
        count++;
    }
    return count;
}

static int hop_find_bucket(
    const hop_index *index,
    uint16_t id,
    size_t *bucket_out,
    size_t *work
)
{
    size_t mask;
    size_t home;
    uint32_t neighborhood;

    if (!index || id >= KEY_TOTAL || index->capacity == 0)
        return -1;
    mask = index->capacity - 1u;
    home = (size_t)(session_hashes[id] & mask);
    neighborhood = index->neighborhoods[home];
    if (work)
        (*work)++;
    while (neighborhood != 0) {
        unsigned offset = trailing_zeroes(neighborhood);
        size_t bucket = (home + offset) & mask;
        uint16_t stored = index->identifiers[bucket];

        if (work)
            (*work)++;
        if (stored != INDEX_NONE &&
            strcmp(session_keys[stored], session_keys[id]) == 0) {
            if (bucket_out)
                *bucket_out = bucket;
            return 1;
        }
        neighborhood &= neighborhood - 1u;
    }
    return 0;
}

static int hop_find(
    const void *context,
    uint16_t id,
    uint16_t *stored_id,
    size_t *work
)
{
    const hop_index *index = (const hop_index *)context;
    size_t bucket = 0;
    int found = hop_find_bucket(index, id, &bucket, work);

    if (found == 1 && stored_id)
        *stored_id = index->identifiers[bucket];
    return found;
}

static int hop_move_empty(hop_index *index, size_t *empty, size_t *work)
{
    size_t mask = index->capacity - 1u;
    size_t back;

    for (back = HOP_NEIGHBORHOOD - 1u; back != 0; back--) {
        size_t home = (*empty + index->capacity - back) & mask;
        uint32_t movable = index->neighborhoods[home] &
            ((UINT32_C(1) << back) - 1u);

        if (work)
            (*work)++;
        if (movable != 0) {
            unsigned offset = trailing_zeroes(movable);
            size_t source = (home + offset) & mask;

            index->identifiers[*empty] = index->identifiers[source];
            index->identifiers[source] = INDEX_NONE;
            index->neighborhoods[home] &= ~(UINT32_C(1) << offset);
            index->neighborhoods[home] |= UINT32_C(1) << back;
            *empty = source;
            return 0;
        }
    }
    return -1;
}

static int hop_insert(void *context, uint16_t id, size_t *work)
{
    hop_index *index = (hop_index *)context;
    size_t mask;
    size_t home;
    size_t empty;
    size_t distance;
    size_t scanned;
    int found;

    if (!index || id >= KEY_TOTAL || index->capacity == 0)
        return -1;
    found = hop_find_bucket(index, id, NULL, work);
    if (found != 0)
        return found > 0 ? -2 : -1;
    if (index->count >= index->limit)
        return -1;
    mask = index->capacity - 1u;
    home = (size_t)(session_hashes[id] & mask);
    empty = home;
    for (scanned = 0; scanned < index->capacity; scanned++) {
        if (work)
            (*work)++;
        if (index->identifiers[empty] == INDEX_NONE)
            break;
        empty = (empty + 1u) & mask;
    }
    if (scanned == index->capacity)
        return -1;
    distance = (empty + index->capacity - home) & mask;
    while (distance >= HOP_NEIGHBORHOOD) {
        if (hop_move_empty(index, &empty, work) != 0)
            return -1;
        distance = (empty + index->capacity - home) & mask;
    }
    index->identifiers[empty] = id;
    index->neighborhoods[home] |= UINT32_C(1) << distance;
    index->count++;
    return 0;
}

static int hop_remove(void *context, uint16_t id, size_t *work)
{
    hop_index *index = (hop_index *)context;
    size_t bucket = 0;
    size_t home;
    size_t distance;
    int found = hop_find_bucket(index, id, &bucket, work);

    if (found != 1)
        return -1;
    home = (size_t)(session_hashes[id] & (index->capacity - 1u));
    distance = (bucket + index->capacity - home) & (index->capacity - 1u);
    index->neighborhoods[home] &= ~(UINT32_C(1) << distance);
    index->identifiers[bucket] = INDEX_NONE;
    index->count--;
    return 0;
}

static uint16_t ordered_id(size_t position, size_t seed)
{
    size_t step = seed * 2u + 1u;
    size_t start = (seed * 4051u + 17u) & (SESSION_LIMIT - 1u);

    return (uint16_t)((position * step + start) & (SESSION_LIMIT - 1u));
}

static int build_index(
    const index_candidate *candidate,
    void *context,
    size_t seed,
    size_t *maximum_work
)
{
    size_t i;

    candidate->init(context, candidate->capacity, SESSION_LIMIT);
    for (i = 0; i < SESSION_LIMIT; i++) {
        size_t work = 0;
        uint16_t id = ordered_id(i, seed);

        if (candidate->insert(context, id, &work) != 0)
            return -1;
        if (work > *maximum_work)
            *maximum_work = work;
    }
    return 0;
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

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
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
    result->p50_ns = result->samples[BENCH_BATCHES / 2u];
    result->p99_ns = result->samples[(BENCH_BATCHES * 99u) / 100u];
    result->average_work =
        (double)result->work_total / (double)result->operation_count;
    result->p50_work = work_quantile(result, 50);
    result->p99_work = work_quantile(result, 99);
}

static void run_lookup_benchmark(
    const index_candidate *candidate,
    void *context,
    int hit,
    bench_result *result
)
{
    size_t maximum_build_work = 0;
    size_t serial = 0;
    size_t batch;

    memset(result, 0, sizeof(*result));
    if (build_index(candidate, context, 11, &maximum_build_work) != 0) {
        result->failed = 1;
        return;
    }
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPS_PER_BATCH; operation++) {
            uint16_t query = (uint16_t)((serial * 4051u + 17u) &
                (SESSION_LIMIT - 1u));
            uint16_t stored = INDEX_NONE;
            size_t work = 0;
            int found;

            if (!hit)
                query = (uint16_t)(query + SESSION_LIMIT);
            found = candidate->find(context, query, &stored, &work);
            if (found != hit || (hit && stored != query)) {
                result->failed = 1;
                return;
            }
            result->checksum ^= mix64((uint64_t)query + serial + (uint64_t)found);
            record_work(result, work);
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPS_PER_BATCH;
    }
    finalize_benchmark(result);
}

static void run_churn_benchmark(
    const index_candidate *candidate,
    void *context,
    bench_result *result
)
{
    size_t maximum_build_work = 0;
    size_t serial = 0;
    size_t batch;

    memset(result, 0, sizeof(*result));
    if (build_index(candidate, context, 23, &maximum_build_work) != 0) {
        result->failed = 1;
        return;
    }
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPS_PER_BATCH; operation++) {
            uint16_t id = (uint16_t)((serial * 4051u + 17u) &
                (SESSION_LIMIT - 1u));
            size_t work = 0;

            if (candidate->remove(context, id, &work) != 0 ||
                candidate->insert(context, id, &work) != 0) {
                result->failed = 1;
                return;
            }
            result->checksum ^= mix64((uint64_t)id + serial);
            record_work(result, work);
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPS_PER_BATCH;
    }
    finalize_benchmark(result);
}

static int verify_candidate(
    const index_candidate *candidate,
    void *context
)
{
    size_t maximum_work = 0;
    size_t i;

    if (build_index(candidate, context, 7, &maximum_work) != 0)
        return -1;
    for (i = 0; i < KEY_TOTAL; i++) {
        uint16_t stored_id = INDEX_NONE;
        size_t work = 0;
        int expected = i < SESSION_LIMIT;
        int found = candidate->find(
            context, (uint16_t)i, &stored_id, &work);

        if (found != expected || (expected && stored_id != i))
            return -1;
    }
    for (i = 0; i < SESSION_LIMIT; i += 7) {
        size_t work = 0;

        if (candidate->remove(context, (uint16_t)i, &work) != 0)
            return -1;
        if (candidate->find(context, (uint16_t)i, NULL, &work) != 0)
            return -1;
        if (candidate->insert(context, (uint16_t)i, &work) != 0)
            return -1;
    }
    {
        size_t work = 0;

        if (candidate->insert(context, SESSION_LIMIT, &work) == 0)
            return -1;
    }
    return 0;
}

static int verify_models(void)
{
    static const index_candidate candidates[] = {
        {
            "linear_probing_1024",
            SESSION_LIMIT,
            linear_init,
            linear_insert,
            linear_find,
            linear_remove,
        },
        {
            "linear_probing_2048",
            HOP_LARGE_CAPACITY,
            linear_init,
            linear_insert,
            linear_find,
            linear_remove,
        },
        {
            "hopscotch_1024",
            SESSION_LIMIT,
            hop_init,
            hop_insert,
            hop_find,
            hop_remove,
        },
        {
            "hopscotch_2048",
            HOP_LARGE_CAPACITY,
            hop_init,
            hop_insert,
            hop_find,
            hop_remove,
        },
    };
    linear_index linear;
    linear_index doubled_linear;
    hop_index same_size_hop;
    hop_index doubled_hop;
    void *contexts[] = {
        &linear,
        &doubled_linear,
        &same_size_hop,
        &doubled_hop,
    };
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (verify_candidate(&candidates[i], contexts[i]) != 0)
            return -1;
    }
    return 0;
}

static int compare_size_t(const void *left, const void *right)
{
    size_t a = *(const size_t *)left;
    size_t b = *(const size_t *)right;

    return (a > b) - (a < b);
}

static void print_same_capacity_trials(void)
{
    size_t fills[CAPACITY_TRIALS];
    size_t maximum_work = 0;
    size_t linear_maximum_work = 0;
    size_t trial;

    for (trial = 0; trial < CAPACITY_TRIALS; trial++) {
        hop_index hop;
        linear_index linear;
        size_t position;

        hop_init(&hop, SESSION_LIMIT, SESSION_LIMIT);
        linear_init(&linear, SESSION_LIMIT, SESSION_LIMIT);
        for (position = 0; position < SESSION_LIMIT; position++) {
            size_t work = 0;
            size_t linear_work = 0;
            uint16_t id = ordered_id(position, trial);

            if (hop_insert(&hop, id, &work) != 0)
                break;
            if (linear_insert(&linear, id, &linear_work) != 0)
                break;
            if (work > maximum_work)
                maximum_work = work;
            if (linear_work > linear_maximum_work)
                linear_maximum_work = linear_work;
        }
        fills[trial] = position;
    }
    qsort(fills, CAPACITY_TRIALS, sizeof(fills[0]), compare_size_t);
    printf(
        "same_capacity_trials=%d neighborhood=%d min_fill=%zu "
        "median_fill=%zu p99_fill=%zu max_fill=%zu max_insert_work=%zu "
        "linear_max_insert_work=%zu\n",
        CAPACITY_TRIALS,
        HOP_NEIGHBORHOOD,
        fills[0],
        fills[CAPACITY_TRIALS / 2u],
        fills[(CAPACITY_TRIALS * 99u) / 100u],
        fills[CAPACITY_TRIALS - 1u],
        maximum_work,
        linear_maximum_work);
}

static int print_adversarial_capacity(void)
{
    enum { COLLISION_COUNT = HOP_NEIGHBORHOOD + 1 };
    char saved_keys[COLLISION_COUNT][48];
    uint64_t saved_hashes[COLLISION_COUNT];
    linear_index linear;
    hop_index same_size;
    hop_index doubled;
    size_t found = 0;
    size_t serial;
    size_t linear_accepted = 0;
    size_t same_size_accepted = 0;
    size_t doubled_accepted = 0;
    size_t i;

    memcpy(saved_keys, session_keys, sizeof(saved_keys));
    memcpy(saved_hashes, session_hashes, sizeof(saved_hashes));
    for (serial = 0; found < COLLISION_COUNT && serial < 1000000u; serial++) {
        char key[48];
        uint64_t hash;
        int written = snprintf(key, sizeof(key), "same-home-%zu", serial);

        if (written < 0 || (size_t)written >= sizeof(key))
            return -1;
        hash = hash_key(key);
        if ((hash & (HOP_LARGE_CAPACITY - 1u)) != 0)
            continue;
        memcpy(session_keys[found], key, (size_t)written + 1u);
        session_hashes[found] = hash;
        found++;
    }
    if (found != COLLISION_COUNT)
        return -1;
    linear_init(&linear, SESSION_LIMIT, SESSION_LIMIT);
    hop_init(&same_size, SESSION_LIMIT, SESSION_LIMIT);
    hop_init(&doubled, HOP_LARGE_CAPACITY, SESSION_LIMIT);
    for (i = 0; i < COLLISION_COUNT; i++) {
        size_t work = 0;

        if (linear_insert(&linear, (uint16_t)i, &work) == 0)
            linear_accepted++;
        work = 0;
        if (hop_insert(&same_size, (uint16_t)i, &work) == 0)
            same_size_accepted++;
        work = 0;
        if (hop_insert(&doubled, (uint16_t)i, &work) == 0)
            doubled_accepted++;
    }
    printf(
        "same_home_keys=%d linear_accepted=%zu hopscotch_1024_accepted=%zu "
        "hopscotch_2048_accepted=%zu neighborhood=%d\n",
        COLLISION_COUNT,
        linear_accepted,
        same_size_accepted,
        doubled_accepted,
        HOP_NEIGHBORHOOD);
    memcpy(session_keys, saved_keys, sizeof(saved_keys));
    memcpy(session_hashes, saved_hashes, sizeof(saved_hashes));
    return linear_accepted == COLLISION_COUNT &&
        same_size_accepted < COLLISION_COUNT &&
        doubled_accepted < COLLISION_COUNT ? 0 : -1;
}

static void print_result(
    const char *operation,
    const index_candidate *candidate,
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

int main(int argc, char **argv)
{
    static const index_candidate candidates[] = {
        {
            "linear_probing_1024",
            SESSION_LIMIT,
            linear_init,
            linear_insert,
            linear_find,
            linear_remove,
        },
        {
            "linear_probing_2048",
            HOP_LARGE_CAPACITY,
            linear_init,
            linear_insert,
            linear_find,
            linear_remove,
        },
        {
            "hopscotch_1024",
            SESSION_LIMIT,
            hop_init,
            hop_insert,
            hop_find,
            hop_remove,
        },
        {
            "hopscotch_2048",
            HOP_LARGE_CAPACITY,
            hop_init,
            hop_insert,
            hop_find,
            hop_remove,
        },
    };
    linear_index linear;
    linear_index doubled_linear;
    hop_index same_size_hop;
    hop_index doubled_hop;
    void *contexts[] = {
        &linear,
        &doubled_linear,
        &same_size_hop,
        &doubled_hop,
    };
    size_t i;

    if (initialize_keys() != 0 || verify_models() != 0) {
        fprintf(stderr, "session index model failed\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        if (print_adversarial_capacity() != 0) {
            fprintf(stderr, "session index adversarial model failed\n");
            return 1;
        }
        puts("session index model: PASS");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--verify]\n", argv[0]);
        return 2;
    }
    print_same_capacity_trials();
    if (print_adversarial_capacity() != 0)
        return 1;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        bench_result hit;
        bench_result miss;
        bench_result churn;

        run_lookup_benchmark(&candidates[i], contexts[i], 1, &hit);
        run_lookup_benchmark(&candidates[i], contexts[i], 0, &miss);
        run_churn_benchmark(&candidates[i], contexts[i], &churn);
        if (hit.failed || miss.failed || churn.failed)
            return 1;
        print_result("hit", &candidates[i], &hit);
        print_result("miss", &candidates[i], &miss);
        print_result("delete_insert", &candidates[i], &churn);
    }
    printf(
        "memory sessions=%d linear_1024_production_bytes=%zu "
        "linear_2048_production_bytes=%zu "
        "hopscotch_1024_production_bytes=%zu "
        "hopscotch_2048_production_bytes=%zu neighborhood=%d\n",
        SESSION_LIMIT,
        (sizeof(void *) + sizeof(uint8_t)) * SESSION_LIMIT,
        (sizeof(void *) + sizeof(uint8_t)) * HOP_LARGE_CAPACITY,
        (sizeof(void *) + sizeof(uint32_t)) * SESSION_LIMIT,
        (sizeof(void *) + sizeof(uint32_t)) * HOP_LARGE_CAPACITY,
        HOP_NEIGHBORHOOD);
    return 0;
}
