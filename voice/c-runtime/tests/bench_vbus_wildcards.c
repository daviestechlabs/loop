#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    MAX_SUBSCRIPTIONS = 4096,
    EXACT_BUCKETS = 8192,
    SUBJECT_CAPACITY = 96,
    MISS_QUERY_COUNT = 64,
    BENCH_BATCHES = 301,
    BENCH_MIN_OPS_PER_BATCH = 64,
};

#define NO_SUBSCRIPTION UINT16_MAX

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

typedef struct {
    uint64_t checksum;
    size_t count;
    size_t work;
} match_result;

typedef struct {
    double samples[BENCH_BATCHES];
    double average_ns;
    double p50_ns;
    double p99_ns;
    uint64_t checksum;
    size_t maximum_work;
} bench_result;

typedef struct {
    const char *const *patterns;
    uint16_t entries[MAX_SUBSCRIPTIONS];
    uint16_t prefix_lengths[MAX_SUBSCRIPTIONS];
    size_t count;
} wildcard_list;

typedef struct {
    const char *const *patterns;
    int buckets[EXACT_BUCKETS];
    int next[MAX_SUBSCRIPTIONS];
    uint16_t prefix_lengths[MAX_SUBSCRIPTIONS];
    unsigned char active[MAX_SUBSCRIPTIONS];
} prefix_hash_index;

typedef struct radix_node radix_node;

struct radix_node {
    char *edge;
    size_t edge_length;
    radix_node *parent;
    radix_node *child;
    radix_node *next;
    uint16_t first_subscription;
};

typedef struct {
    radix_node *root;
    uint16_t next_subscription[MAX_SUBSCRIPTIONS];
    unsigned char active[MAX_SUBSCRIPTIONS];
    size_t retained_bytes;
    size_t peak_bytes;
    size_t node_count;
} radix_index;

typedef struct {
    const char *const *subjects;
    int buckets[EXACT_BUCKETS];
    int next[MAX_SUBSCRIPTIONS];
} exact_index;

typedef match_result (*lookup_function)(const void *context, const char *topic);

static char pattern_storage[MAX_SUBSCRIPTIONS][SUBJECT_CAPACITY];
static const char *patterns[MAX_SUBSCRIPTIONS];
static char hit_query_storage[MAX_SUBSCRIPTIONS][SUBJECT_CAPACITY];
static const char *hit_queries[MAX_SUBSCRIPTIONS];
static char exact_subject_storage[MAX_SUBSCRIPTIONS][SUBJECT_CAPACITY];
static const char *exact_subjects[MAX_SUBSCRIPTIONS];
static char miss_query_storage[MISS_QUERY_COUNT][SUBJECT_CAPACITY];
static const char *miss_queries[MISS_QUERY_COUNT];

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static uint64_t subscription_checksum(uint16_t id)
{
    uint64_t value = (uint64_t)id + UINT64_C(0x9e3779b97f4a7c15);

    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int wildcard_prefix_length(const char *pattern, size_t *prefix_length)
{
    size_t length;

    if (!pattern || !prefix_length)
        return -1;
    length = strlen(pattern);
    if (length == 1 && pattern[0] == '>') {
        *prefix_length = 0;
        return 0;
    }
    if (length >= 2 && pattern[length - 2] == '.' && pattern[length - 1] == '>') {
        *prefix_length = length - 1;
        return 0;
    }
    return -1;
}

static int wildcard_matches(const char *pattern, const char *topic)
{
    size_t prefix_length;

    if (wildcard_prefix_length(pattern, &prefix_length) != 0)
        return 0;
    return prefix_length == 0 || strncmp(pattern, topic, prefix_length) == 0;
}

static int wildcard_list_insert(wildcard_list *list, uint16_t id)
{
    size_t prefix_length;

    if (!list || list->count >= MAX_SUBSCRIPTIONS)
        return -1;
    if (wildcard_prefix_length(list->patterns[id], &prefix_length) != 0 ||
        prefix_length > UINT16_MAX)
        return -1;
    list->prefix_lengths[id] = (uint16_t)prefix_length;
    list->entries[list->count++] = id;
    return 0;
}

static int wildcard_list_remove(wildcard_list *list, uint16_t id)
{
    size_t i;

    if (!list)
        return -1;
    for (i = 0; i < list->count; i++) {
        if (list->entries[i] != id)
            continue;
        list->count--;
        list->entries[i] = list->entries[list->count];
        return 0;
    }
    return -1;
}

static BENCH_NOINLINE match_result wildcard_list_lookup(
    const void *context,
    const char *topic
)
{
    const wildcard_list *list = (const wildcard_list *)context;
    match_result result = {0, 0, 0};
    size_t i;

    for (i = 0; i < list->count; i++) {
        uint16_t id = list->entries[i];

        result.work++;
        if (!wildcard_matches(list->patterns[id], topic))
            continue;
        result.checksum ^= subscription_checksum(id);
        result.count++;
    }
    return result;
}

static BENCH_NOINLINE match_result wildcard_cached_list_lookup(
    const void *context,
    const char *topic
)
{
    const wildcard_list *list = (const wildcard_list *)context;
    match_result result = {0, 0, 0};
    size_t i;

    for (i = 0; i < list->count; i++) {
        uint16_t id = list->entries[i];
        size_t prefix_length = list->prefix_lengths[id];

        result.work++;
        if (prefix_length != 0 &&
            strncmp(list->patterns[id], topic, prefix_length) != 0)
            continue;
        result.checksum ^= subscription_checksum(id);
        result.count++;
    }
    return result;
}

static uint32_t prefix_hash_bytes(const char *value, size_t length)
{
    const unsigned char *cursor = (const unsigned char *)value;
    uint32_t hash = UINT32_C(2166136261);
    size_t i;

    for (i = 0; i < length; i++) {
        hash ^= cursor[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void prefix_hash_init(prefix_hash_index *index, const char *const *source)
{
    size_t i;

    memset(index, 0, sizeof(*index));
    index->patterns = source;
    for (i = 0; i < EXACT_BUCKETS; i++)
        index->buckets[i] = -1;
    for (i = 0; i < MAX_SUBSCRIPTIONS; i++)
        index->next[i] = -1;
}

static int prefix_hash_insert(prefix_hash_index *index, uint16_t id)
{
    size_t prefix_length;
    size_t bucket;

    if (!index || id >= MAX_SUBSCRIPTIONS || index->active[id] ||
        wildcard_prefix_length(index->patterns[id], &prefix_length) != 0 ||
        prefix_length > UINT16_MAX)
        return -1;
    bucket = (size_t)(
        prefix_hash_bytes(index->patterns[id], prefix_length) &
        (EXACT_BUCKETS - 1u));
    index->prefix_lengths[id] = (uint16_t)prefix_length;
    index->next[id] = index->buckets[bucket];
    index->buckets[bucket] = (int)id;
    index->active[id] = 1;
    return 0;
}

static int prefix_hash_remove(prefix_hash_index *index, uint16_t id)
{
    size_t prefix_length;
    size_t bucket;
    int *link;

    if (!index || id >= MAX_SUBSCRIPTIONS || !index->active[id])
        return -1;
    prefix_length = index->prefix_lengths[id];
    bucket = (size_t)(
        prefix_hash_bytes(index->patterns[id], prefix_length) &
        (EXACT_BUCKETS - 1u));
    link = &index->buckets[bucket];
    while (*link >= 0 && *link != (int)id)
        link = &index->next[*link];
    if (*link != (int)id)
        return -1;
    *link = index->next[id];
    index->next[id] = -1;
    index->prefix_lengths[id] = 0;
    index->active[id] = 0;
    return 0;
}

static void prefix_hash_collect(
    const prefix_hash_index *index,
    const char *topic,
    size_t prefix_length,
    uint32_t hash,
    match_result *result
)
{
    size_t bucket = (size_t)(hash & (EXACT_BUCKETS - 1u));
    int id = index->buckets[bucket];

    while (id >= 0) {
        result->work++;
        if (index->prefix_lengths[id] == prefix_length &&
            memcmp(index->patterns[id], topic, prefix_length) == 0) {
            result->checksum ^= subscription_checksum((uint16_t)id);
            result->count++;
        }
        id = index->next[id];
    }
}

static BENCH_NOINLINE match_result prefix_hash_lookup(
    const void *context,
    const char *topic
)
{
    const prefix_hash_index *index = (const prefix_hash_index *)context;
    const unsigned char *cursor = (const unsigned char *)topic;
    uint32_t hash = UINT32_C(2166136261);
    match_result result = {0, 0, 0};
    size_t length = 0;

    prefix_hash_collect(index, topic, 0, hash, &result);
    while (*cursor) {
        hash ^= *cursor;
        hash *= UINT32_C(16777619);
        length++;
        result.work++;
        if (*cursor++ == (unsigned char)'.')
            prefix_hash_collect(index, topic, length, hash, &result);
    }
    return result;
}

static void track_allocation(radix_index *index, size_t bytes)
{
    index->retained_bytes += bytes;
    if (index->retained_bytes > index->peak_bytes)
        index->peak_bytes = index->retained_bytes;
}

static void track_release(radix_index *index, size_t bytes)
{
    index->retained_bytes -= bytes;
}

static char *radix_edge_copy(radix_index *index, const char *source, size_t length)
{
    char *copy;

    if (length == 0)
        return NULL;
    copy = (char *)malloc(length);
    if (!copy)
        return NULL;
    memcpy(copy, source, length);
    track_allocation(index, length);
    return copy;
}

static radix_node *radix_node_create(
    radix_index *index,
    const char *edge,
    size_t edge_length
)
{
    radix_node *node = (radix_node *)calloc(1, sizeof(*node));

    if (!node)
        return NULL;
    track_allocation(index, sizeof(*node));
    node->first_subscription = NO_SUBSCRIPTION;
    if (edge_length != 0) {
        node->edge = radix_edge_copy(index, edge, edge_length);
        if (!node->edge) {
            track_release(index, sizeof(*node));
            free(node);
            return NULL;
        }
    }
    node->edge_length = edge_length;
    index->node_count++;
    return node;
}

static void radix_node_release(radix_index *index, radix_node *node)
{
    if (!node)
        return;
    if (node->edge) {
        track_release(index, node->edge_length);
        free(node->edge);
    }
    track_release(index, sizeof(*node));
    index->node_count--;
    free(node);
}

static int radix_index_init(radix_index *index)
{
    size_t i;

    memset(index, 0, sizeof(*index));
    for (i = 0; i < MAX_SUBSCRIPTIONS; i++)
        index->next_subscription[i] = NO_SUBSCRIPTION;
    index->root = radix_node_create(index, NULL, 0);
    return index->root ? 0 : -1;
}

static size_t common_prefix_length(
    const char *left,
    size_t left_length,
    const char *right,
    size_t right_length
)
{
    size_t limit = left_length < right_length ? left_length : right_length;
    size_t i = 0;

    while (i < limit && left[i] == right[i])
        i++;
    return i;
}

static void radix_attach_subscription(radix_index *index, radix_node *node, uint16_t id)
{
    index->next_subscription[id] = node->first_subscription;
    node->first_subscription = id;
    index->active[id] = 1;
}

static int radix_index_insert(radix_index *index, const char *pattern, uint16_t id)
{
    radix_node *parent;
    const char *remaining;
    size_t remaining_length;
    size_t prefix_length;

    if (!index || !index->root || id >= MAX_SUBSCRIPTIONS || index->active[id] ||
        wildcard_prefix_length(pattern, &prefix_length) != 0)
        return -1;
    parent = index->root;
    remaining = pattern;
    remaining_length = prefix_length;
    if (remaining_length == 0) {
        radix_attach_subscription(index, parent, id);
        return 0;
    }
    while (remaining_length != 0) {
        radix_node **place = &parent->child;
        radix_node *child;
        size_t shared;

        while (*place && (*place)->edge[0] != remaining[0])
            place = &(*place)->next;
        child = *place;
        if (!child) {
            radix_node *leaf = radix_node_create(index, remaining, remaining_length);

            if (!leaf)
                return -1;
            leaf->parent = parent;
            *place = leaf;
            radix_attach_subscription(index, leaf, id);
            return 0;
        }
        shared = common_prefix_length(
            child->edge,
            child->edge_length,
            remaining,
            remaining_length);
        if (shared == child->edge_length) {
            parent = child;
            remaining += shared;
            remaining_length -= shared;
            continue;
        }
        {
            size_t suffix_length = child->edge_length - shared;
            size_t leaf_length = remaining_length - shared;
            radix_node *middle = radix_node_create(index, child->edge, shared);
            radix_node *leaf = NULL;
            char *suffix;

            if (!middle)
                return -1;
            suffix = radix_edge_copy(index, child->edge + shared, suffix_length);
            if (!suffix) {
                radix_node_release(index, middle);
                return -1;
            }
            if (leaf_length != 0) {
                leaf = radix_node_create(index, remaining + shared, leaf_length);
                if (!leaf) {
                    track_release(index, suffix_length);
                    free(suffix);
                    radix_node_release(index, middle);
                    return -1;
                }
            }
            middle->parent = parent;
            middle->next = child->next;
            middle->child = child;
            child->parent = middle;
            child->next = leaf;
            track_release(index, child->edge_length);
            free(child->edge);
            child->edge = suffix;
            child->edge_length = suffix_length;
            *place = middle;
            if (leaf) {
                leaf->parent = middle;
                radix_attach_subscription(index, leaf, id);
            } else {
                radix_attach_subscription(index, middle, id);
            }
            return 0;
        }
    }
    radix_attach_subscription(index, parent, id);
    return 0;
}

static radix_node *radix_find_prefix(radix_index *index, const char *pattern)
{
    radix_node *node;
    const char *remaining;
    size_t remaining_length;
    size_t prefix_length;

    if (wildcard_prefix_length(pattern, &prefix_length) != 0)
        return NULL;
    node = index->root;
    remaining = pattern;
    remaining_length = prefix_length;
    while (remaining_length != 0) {
        radix_node *child = node->child;

        while (child && child->edge[0] != remaining[0])
            child = child->next;
        if (!child || child->edge_length > remaining_length ||
            memcmp(child->edge, remaining, child->edge_length) != 0)
            return NULL;
        remaining += child->edge_length;
        remaining_length -= child->edge_length;
        node = child;
    }
    return node;
}

static void radix_unlink_child(radix_node *parent, radix_node *node)
{
    radix_node **place = &parent->child;

    while (*place && *place != node)
        place = &(*place)->next;
    if (*place == node)
        *place = node->next;
}

static int radix_merge_only_child(radix_index *index, radix_node *node)
{
    radix_node *child = node->child;
    size_t merged_length;
    char *merged;
    radix_node *grandchild;

    if (!child || child->next)
        return 0;
    merged_length = node->edge_length + child->edge_length;
    merged = (char *)malloc(merged_length);
    if (!merged)
        return -1;
    track_allocation(index, merged_length);
    memcpy(merged, node->edge, node->edge_length);
    memcpy(merged + node->edge_length, child->edge, child->edge_length);

    if (node->edge) {
        track_release(index, node->edge_length);
        free(node->edge);
    }
    node->edge = merged;
    node->edge_length = merged_length;
    node->first_subscription = child->first_subscription;
    node->child = child->child;
    for (grandchild = node->child; grandchild; grandchild = grandchild->next)
        grandchild->parent = node;
    child->child = NULL;
    track_release(index, child->edge_length);
    free(child->edge);
    child->edge = NULL;
    child->edge_length = 0;
    radix_node_release(index, child);
    return 0;
}

static int radix_index_remove(radix_index *index, const char *pattern, uint16_t id)
{
    radix_node *node;
    uint16_t *link;

    if (!index || id >= MAX_SUBSCRIPTIONS || !index->active[id])
        return -1;
    node = radix_find_prefix(index, pattern);
    if (!node)
        return -1;
    link = &node->first_subscription;
    while (*link != NO_SUBSCRIPTION && *link != id)
        link = &index->next_subscription[*link];
    if (*link != id)
        return -1;
    *link = index->next_subscription[id];
    index->next_subscription[id] = NO_SUBSCRIPTION;
    index->active[id] = 0;

    while (node != index->root && node->first_subscription == NO_SUBSCRIPTION) {
        radix_node *parent = node->parent;

        if (!node->child) {
            radix_unlink_child(parent, node);
            radix_node_release(index, node);
            node = parent;
            continue;
        }
        if (!node->child->next) {
            if (radix_merge_only_child(index, node) != 0)
                return -1;
            continue;
        }
        break;
    }
    return 0;
}

static void radix_destroy_nodes(radix_index *index, radix_node *node)
{
    radix_node *child;

    if (!node)
        return;
    child = node->child;
    while (child) {
        radix_node *next = child->next;

        radix_destroy_nodes(index, child);
        child = next;
    }
    radix_node_release(index, node);
}

static void radix_index_destroy(radix_index *index)
{
    radix_destroy_nodes(index, index->root);
    index->root = NULL;
}

static void radix_collect_matches(
    const radix_index *index,
    const radix_node *node,
    match_result *result
)
{
    uint16_t id = node->first_subscription;

    while (id != NO_SUBSCRIPTION) {
        result->checksum ^= subscription_checksum(id);
        result->count++;
        result->work++;
        id = index->next_subscription[id];
    }
}

static BENCH_NOINLINE match_result radix_index_lookup(
    const void *context,
    const char *topic
)
{
    const radix_index *index = (const radix_index *)context;
    const radix_node *node = index->root;
    const char *remaining = topic;
    size_t remaining_length = strlen(topic);
    match_result result = {0, 0, 0};

    radix_collect_matches(index, node, &result);
    while (remaining_length != 0) {
        const radix_node *child = node->child;

        while (child) {
            result.work++;
            if (child->edge[0] == remaining[0])
                break;
            child = child->next;
        }
        if (!child || child->edge_length > remaining_length ||
            memcmp(child->edge, remaining, child->edge_length) != 0)
            break;
        remaining += child->edge_length;
        remaining_length -= child->edge_length;
        node = child;
        radix_collect_matches(index, node, &result);
    }
    return result;
}

static uint32_t topic_hash(const char *topic)
{
    const unsigned char *cursor = (const unsigned char *)topic;
    uint32_t hash = UINT32_C(2166136261);

    while (*cursor) {
        hash ^= *cursor++;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void exact_index_build(exact_index *index, size_t count)
{
    size_t i;

    index->subjects = exact_subjects;
    for (i = 0; i < EXACT_BUCKETS; i++)
        index->buckets[i] = -1;
    for (i = 0; i < count; i++) {
        size_t bucket = (size_t)(topic_hash(index->subjects[i]) & (EXACT_BUCKETS - 1u));

        index->next[i] = index->buckets[bucket];
        index->buckets[bucket] = (int)i;
    }
}

static BENCH_NOINLINE match_result exact_index_lookup(
    const void *context,
    const char *topic
)
{
    const exact_index *index = (const exact_index *)context;
    size_t bucket = (size_t)(topic_hash(topic) & (EXACT_BUCKETS - 1u));
    int id = index->buckets[bucket];
    match_result result = {0, 0, 0};

    while (id >= 0) {
        result.work++;
        if (strcmp(index->subjects[id], topic) == 0) {
            result.checksum = subscription_checksum((uint16_t)id);
            result.count = 1;
            break;
        }
        id = index->next[id];
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

    for (i = 0; i < BENCH_BATCHES; i++)
        total += result->samples[i];
    result->average_ns = total / BENCH_BATCHES;
    qsort(result->samples, BENCH_BATCHES, sizeof(result->samples[0]), compare_double);
    result->p50_ns = result->samples[BENCH_BATCHES / 2];
    result->p99_ns = result->samples[(BENCH_BATCHES * 99) / 100];
}

static void run_lookup_benchmark(
    lookup_function lookup,
    const void *context,
    const char *const *queries,
    size_t query_count,
    size_t operations_per_batch,
    bench_result *result
)
{
    size_t batch;
    size_t serial = 0;

    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations_per_batch; operation++) {
            match_result match = lookup(context, queries[serial % query_count]);

            result->checksum ^= match.checksum + (uint64_t)match.count + (uint64_t)serial;
            if (match.work > result->maximum_work)
                result->maximum_work = match.work;
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations_per_batch;
    }
    finalize_benchmark(result);
}

static int verify_lookup_equivalence(
    const wildcard_list *list,
    const prefix_hash_index *prefix_hash,
    const radix_index *radix,
    const char *const *queries,
    size_t query_count
)
{
    size_t i;

    for (i = 0; i < query_count; i++) {
        match_result expected = wildcard_list_lookup(list, queries[i]);
        match_result cached = wildcard_cached_list_lookup(list, queries[i]);
        match_result hashed = prefix_hash_lookup(prefix_hash, queries[i]);
        match_result actual = radix_index_lookup(radix, queries[i]);

        if (expected.count != cached.count || expected.checksum != cached.checksum ||
            expected.count != hashed.count || expected.checksum != hashed.checksum ||
            expected.count != actual.count || expected.checksum != actual.checksum) {
            fprintf(stderr, "wildcard mismatch topic=%s expected=%zu actual=%zu\n",
                queries[i], expected.count, actual.count);
            return -1;
        }
    }
    return 0;
}

static int verify_contract_grammar(void)
{
    static const char *const grammar_patterns[] = {
        ">",
        "ai.>",
        "ai.voice.>",
        "ai.voice.stream.>",
        "ai.voice.stream.>",
        "ai.turn.>",
    };
    static const char *const grammar_queries[] = {
        "ai.voice.stream.session-1",
        "ai.voice.pcm.session-1",
        "ai.turn.events.request-1",
        "other.subject",
        "ai",
    };
    wildcard_list list;
    prefix_hash_index prefix_hash;
    radix_index radix;
    size_t i;
    int status = -1;

    memset(&list, 0, sizeof(list));
    list.patterns = grammar_patterns;
    prefix_hash_init(&prefix_hash, grammar_patterns);
    if (radix_index_init(&radix) != 0)
        return -1;
    for (i = 0; i < sizeof(grammar_patterns) / sizeof(grammar_patterns[0]); i++) {
        if (wildcard_list_insert(&list, (uint16_t)i) != 0 ||
            prefix_hash_insert(&prefix_hash, (uint16_t)i) != 0 ||
            radix_index_insert(&radix, grammar_patterns[i], (uint16_t)i) != 0)
            goto done;
    }
    if (verify_lookup_equivalence(
            &list,
            &prefix_hash,
            &radix,
            grammar_queries,
            sizeof(grammar_queries) / sizeof(grammar_queries[0])) != 0)
        goto done;
    if (wildcard_list_remove(&list, 3) != 0 ||
        prefix_hash_remove(&prefix_hash, 3) != 0 ||
        radix_index_remove(&radix, grammar_patterns[3], 3) != 0 ||
        verify_lookup_equivalence(
            &list,
            &prefix_hash,
            &radix,
            grammar_queries,
            sizeof(grammar_queries) / sizeof(grammar_queries[0])) != 0)
        goto done;
    if (wildcard_list_insert(&list, 3) != 0 ||
        prefix_hash_insert(&prefix_hash, 3) != 0 ||
        radix_index_insert(&radix, grammar_patterns[3], 3) != 0 ||
        verify_lookup_equivalence(
            &list,
            &prefix_hash,
            &radix,
            grammar_queries,
            sizeof(grammar_queries) / sizeof(grammar_queries[0])) != 0)
        goto done;
    status = 0;

done:
    radix_index_destroy(&radix);
    return status;
}

static int make_subject_data(void)
{
    static const char *const production_patterns[] = {
        "ai.voice.stream.>",
        "ai.voice.pcm.>",
        "ai.voice.transcription.>",
        "ai.turn.events.>",
    };
    static const char *const production_misses[] = {
        "ai.turn.start",
        "ai.turn.generate",
        "ai.turn.tts.speak",
        "ai.session.append",
        "ai.session.get",
        "ai.rag.search",
        "ai.voice.turn.prepare",
        "ai.voice.unmatched.session-1",
    };
    size_t i;

    for (i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        int written;
        size_t prefix_length;

        if (i < sizeof(production_patterns) / sizeof(production_patterns[0])) {
            written = snprintf(
                pattern_storage[i],
                sizeof(pattern_storage[i]),
                "%s",
                production_patterns[i]);
        } else {
            written = snprintf(
                pattern_storage[i],
                sizeof(pattern_storage[i]),
                "ai.voice.family%02zu.channel%04zu.>",
                i % 64,
                i);
        }
        if (written < 0 || (size_t)written >= sizeof(pattern_storage[i]) ||
            wildcard_prefix_length(pattern_storage[i], &prefix_length) != 0)
            return -1;
        written = snprintf(
            hit_query_storage[i],
            sizeof(hit_query_storage[i]),
            "%.*ssession-%04zu",
            (int)prefix_length,
            pattern_storage[i],
            i);
        if (written < 0 || (size_t)written >= sizeof(hit_query_storage[i]))
            return -1;
        written = snprintf(
            exact_subject_storage[i],
            sizeof(exact_subject_storage[i]),
            "%s",
            hit_query_storage[i]);
        if (written < 0 || (size_t)written >= sizeof(exact_subject_storage[i]))
            return -1;
        patterns[i] = pattern_storage[i];
        hit_queries[i] = hit_query_storage[i];
        exact_subjects[i] = exact_subject_storage[i];
    }
    for (i = 0; i < MISS_QUERY_COUNT; i++) {
        int written = snprintf(
            miss_query_storage[i],
            sizeof(miss_query_storage[i]),
            "%s",
            production_misses[i %
                (sizeof(production_misses) / sizeof(production_misses[0]))]);

        if (written < 0 || (size_t)written >= sizeof(miss_query_storage[i]))
            return -1;
        miss_queries[i] = miss_query_storage[i];
    }
    return 0;
}

static int build_wildcard_indexes(
    size_t count,
    wildcard_list *list,
    prefix_hash_index *prefix_hash,
    radix_index *radix
)
{
    size_t i;

    memset(list, 0, sizeof(*list));
    list->patterns = patterns;
    prefix_hash_init(prefix_hash, patterns);
    if (radix_index_init(radix) != 0)
        return -1;
    for (i = 0; i < count; i++) {
        if (wildcard_list_insert(list, (uint16_t)i) != 0 ||
            prefix_hash_insert(prefix_hash, (uint16_t)i) != 0 ||
            radix_index_insert(radix, patterns[i], (uint16_t)i) != 0) {
            radix_index_destroy(radix);
            return -1;
        }
    }
    return 0;
}

static int verify_scale(
    size_t count,
    wildcard_list *list,
    prefix_hash_index *prefix_hash,
    radix_index *radix
)
{
    size_t i;

    if (verify_lookup_equivalence(
            list, prefix_hash, radix, hit_queries, count) != 0 ||
        verify_lookup_equivalence(
            list, prefix_hash, radix, miss_queries, MISS_QUERY_COUNT) != 0)
        return -1;
    for (i = 0; i < count; i += 3) {
        if (wildcard_list_remove(list, (uint16_t)i) != 0 ||
            prefix_hash_remove(prefix_hash, (uint16_t)i) != 0 ||
            radix_index_remove(radix, patterns[i], (uint16_t)i) != 0)
            return -1;
    }
    if (verify_lookup_equivalence(
            list, prefix_hash, radix, hit_queries, count) != 0)
        return -1;
    for (i = 0; i < count; i += 3) {
        if (wildcard_list_insert(list, (uint16_t)i) != 0 ||
            prefix_hash_insert(prefix_hash, (uint16_t)i) != 0 ||
            radix_index_insert(radix, patterns[i], (uint16_t)i) != 0)
            return -1;
    }
    return verify_lookup_equivalence(
        list, prefix_hash, radix, hit_queries, count);
}

static void run_list_churn(
    wildcard_list *list,
    size_t count,
    size_t operations_per_batch,
    bench_result *result
)
{
    size_t batch;
    size_t serial = 0;

    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations_per_batch; operation++) {
            uint16_t id = (uint16_t)((serial * 4051u) & (count - 1u));

            if (wildcard_list_remove(list, id) != 0 ||
                wildcard_list_insert(list, id) != 0) {
                result->checksum = UINT64_MAX;
                return;
            }
            result->checksum ^= subscription_checksum(id) + (uint64_t)serial;
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations_per_batch;
    }
    finalize_benchmark(result);
}

static void run_radix_churn(
    radix_index *radix,
    size_t count,
    size_t operations_per_batch,
    bench_result *result
)
{
    size_t batch;
    size_t serial = 0;

    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations_per_batch; operation++) {
            uint16_t id = (uint16_t)((serial * 4051u) & (count - 1u));

            if (radix_index_remove(radix, patterns[id], id) != 0 ||
                radix_index_insert(radix, patterns[id], id) != 0) {
                result->checksum = UINT64_MAX;
                return;
            }
            result->checksum ^= subscription_checksum(id) + (uint64_t)serial;
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations_per_batch;
    }
    finalize_benchmark(result);
}

static void run_prefix_hash_churn(
    prefix_hash_index *index,
    size_t count,
    size_t operations_per_batch,
    bench_result *result
)
{
    size_t batch;
    size_t serial = 0;

    memset(result, 0, sizeof(*result));
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < operations_per_batch; operation++) {
            uint16_t id = (uint16_t)((serial * 4051u) & (count - 1u));

            if (prefix_hash_remove(index, id) != 0 ||
                prefix_hash_insert(index, id) != 0) {
                result->checksum = UINT64_MAX;
                return;
            }
            result->checksum ^= subscription_checksum(id) + (uint64_t)serial;
            serial++;
        }
        result->samples[batch] =
            (double)(monotonic_ns() - started) / (double)operations_per_batch;
    }
    finalize_benchmark(result);
}

static void print_lookup(
    size_t count,
    const char *operation,
    const char *candidate,
    const bench_result *result
)
{
    printf(
        "count=%zu operation=%s candidate=%s avg_ns=%.2f p50_ns=%.2f "
        "p99_ns=%.2f mops=%.3f max_work=%zu checksum=%" PRIu64 "\n",
        count,
        operation,
        candidate,
        result->average_ns,
        result->p50_ns,
        result->p99_ns,
        1000.0 / result->average_ns,
        result->maximum_work,
        result->checksum);
}

static int run_scale(size_t count)
{
    wildcard_list list;
    prefix_hash_index prefix_hash;
    radix_index radix;
    exact_index exact;
    bench_result list_hit;
    bench_result list_miss;
    bench_result cached_list_hit;
    bench_result cached_list_miss;
    bench_result prefix_hash_hit;
    bench_result prefix_hash_miss;
    bench_result radix_hit;
    bench_result radix_miss;
    bench_result exact_hit;
    bench_result exact_miss;
    bench_result list_churn;
    bench_result prefix_hash_churn;
    bench_result radix_churn;
    size_t retained_bytes;
    size_t peak_bytes;
    size_t node_count;
    size_t operations_per_batch = BENCH_MIN_OPS_PER_BATCH;
    int status = -1;

    if (build_wildcard_indexes(count, &list, &prefix_hash, &radix) != 0)
        return -1;
    exact_index_build(&exact, count);
    if (verify_scale(count, &list, &prefix_hash, &radix) != 0)
        goto done;
    if (count <= 4)
        operations_per_batch *= 8;
    else if (count <= 16)
        operations_per_batch *= 4;
    else if (count <= 64)
        operations_per_batch *= 2;

    run_lookup_benchmark(
        wildcard_list_lookup,
        &list,
        hit_queries,
        count,
        operations_per_batch,
        &list_hit);
    run_lookup_benchmark(
        wildcard_list_lookup,
        &list,
        miss_queries,
        MISS_QUERY_COUNT,
        operations_per_batch,
        &list_miss);
    run_lookup_benchmark(
        wildcard_cached_list_lookup,
        &list,
        hit_queries,
        count,
        operations_per_batch,
        &cached_list_hit);
    run_lookup_benchmark(
        wildcard_cached_list_lookup,
        &list,
        miss_queries,
        MISS_QUERY_COUNT,
        operations_per_batch,
        &cached_list_miss);
    run_lookup_benchmark(
        prefix_hash_lookup,
        &prefix_hash,
        hit_queries,
        count,
        operations_per_batch,
        &prefix_hash_hit);
    run_lookup_benchmark(
        prefix_hash_lookup,
        &prefix_hash,
        miss_queries,
        MISS_QUERY_COUNT,
        operations_per_batch,
        &prefix_hash_miss);
    run_lookup_benchmark(
        radix_index_lookup,
        &radix,
        hit_queries,
        count,
        operations_per_batch,
        &radix_hit);
    run_lookup_benchmark(
        radix_index_lookup,
        &radix,
        miss_queries,
        MISS_QUERY_COUNT,
        operations_per_batch,
        &radix_miss);
    run_lookup_benchmark(
        exact_index_lookup,
        &exact,
        exact_subjects,
        count,
        operations_per_batch,
        &exact_hit);
    run_lookup_benchmark(
        exact_index_lookup,
        &exact,
        miss_queries,
        MISS_QUERY_COUNT,
        operations_per_batch,
        &exact_miss);
    run_list_churn(&list, count, operations_per_batch, &list_churn);
    run_prefix_hash_churn(
        &prefix_hash, count, operations_per_batch, &prefix_hash_churn);
    run_radix_churn(&radix, count, operations_per_batch, &radix_churn);
    if (list_churn.checksum == UINT64_MAX ||
        prefix_hash_churn.checksum == UINT64_MAX ||
        radix_churn.checksum == UINT64_MAX ||
        list_churn.checksum != prefix_hash_churn.checksum ||
        list_churn.checksum != radix_churn.checksum ||
        verify_lookup_equivalence(
            &list, &prefix_hash, &radix, hit_queries, count) != 0)
        goto done;

    retained_bytes = radix.retained_bytes;
    peak_bytes = radix.peak_bytes;
    node_count = radix.node_count;
    print_lookup(count, "wildcard_hit", "list", &list_hit);
    print_lookup(count, "wildcard_hit", "cached_list", &cached_list_hit);
    print_lookup(count, "wildcard_hit", "prefix_hash", &prefix_hash_hit);
    print_lookup(count, "wildcard_hit", "radix", &radix_hit);
    print_lookup(count, "wildcard_miss", "list", &list_miss);
    print_lookup(count, "wildcard_miss", "cached_list", &cached_list_miss);
    print_lookup(count, "wildcard_miss", "prefix_hash", &prefix_hash_miss);
    print_lookup(count, "wildcard_miss", "radix", &radix_miss);
    print_lookup(count, "exact_hit", "hash", &exact_hit);
    print_lookup(count, "exact_miss", "hash", &exact_miss);
    print_lookup(count, "remove_insert", "cached_list", &list_churn);
    print_lookup(count, "remove_insert", "prefix_hash", &prefix_hash_churn);
    print_lookup(count, "remove_insert", "radix", &radix_churn);
    printf(
        "count=%zu memory list_fixed_bytes=%zu radix_retained_bytes=%zu "
        "radix_peak_bytes=%zu radix_nodes=%zu\n",
        count,
        sizeof(list.entries),
        retained_bytes,
        peak_bytes,
        node_count);
    status = 0;

done:
    radix_index_destroy(&radix);
    return status;
}

int main(int argc, char **argv)
{
    static const size_t scales[] = {4, 8, 16, 64, 256, 1024, 4096};
    size_t i;

    if (make_subject_data() != 0 || verify_contract_grammar() != 0) {
        fprintf(stderr, "wildcard benchmark setup failed\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--verify") == 0) {
        wildcard_list list;
        prefix_hash_index prefix_hash;
        radix_index radix;
        int status;

        if (build_wildcard_indexes(
                MAX_SUBSCRIPTIONS, &list, &prefix_hash, &radix) != 0)
            return 1;
        status = verify_scale(
            MAX_SUBSCRIPTIONS, &list, &prefix_hash, &radix);
        radix_index_destroy(&radix);
        if (status != 0)
            return 1;
        puts("vbus wildcard model: PASS");
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
