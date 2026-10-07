#include "cmp_index.h"

#include <string.h>

static int cmp_index_capacity_valid(size_t capacity)
{
    return capacity >= 2 && (capacity & (capacity - 1)) == 0;
}

uint64_t cmp_index_hash(const char *key)
{
    const unsigned char *cursor = (const unsigned char *)key;
    uint64_t hash = UINT64_C(14695981039346656037);

    if (!cursor)
        return 0;
    while (*cursor) {
        hash ^= *cursor++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int cmp_index_init(cmp_index *index, cmp_index_entry *entries, size_t capacity)
{
    if (!index || !entries || !cmp_index_capacity_valid(capacity) ||
        capacity > SIZE_MAX / sizeof(*entries))
        return CMP_INDEX_INVALID;
    memset(entries, 0, capacity * sizeof(*entries));
    index->entries = entries;
    index->capacity = capacity;
    index->count = 0;
    index->tombstones = 0;
    return CMP_INDEX_OK;
}

void cmp_index_clear(cmp_index *index)
{
    if (!index || !index->entries || !cmp_index_capacity_valid(index->capacity) ||
        index->capacity > SIZE_MAX / sizeof(*index->entries))
        return;
    memset(index->entries, 0, index->capacity * sizeof(*index->entries));
    index->count = 0;
    index->tombstones = 0;
}

int cmp_index_find(
    const cmp_index *index,
    const char *key,
    uint32_t *value_out,
    size_t *probes_out
)
{
    uint64_t hash;
    size_t bucket;
    size_t probe;

    if (probes_out)
        *probes_out = 0;
    if (!index || !index->entries || !key || !key[0] ||
        !cmp_index_capacity_valid(index->capacity))
        return CMP_INDEX_INVALID;
    hash = cmp_index_hash(key);
    bucket = (size_t)hash & (index->capacity - 1);
    for (probe = 0; probe < index->capacity; probe++) {
        const cmp_index_entry *entry =
            &index->entries[(bucket + probe) & (index->capacity - 1)];
        if (probes_out)
            *probes_out = probe + 1;
        if (entry->state == CMP_INDEX_EMPTY)
            return CMP_INDEX_NOT_FOUND;
        if (entry->state == CMP_INDEX_USED && entry->hash == hash &&
            entry->key && strcmp(entry->key, key) == 0) {
            if (value_out)
                *value_out = entry->value;
            return CMP_INDEX_OK;
        }
    }
    return CMP_INDEX_NOT_FOUND;
}

int cmp_index_insert(
    cmp_index *index,
    const char *stable_key,
    uint32_t value,
    size_t *probes_out
)
{
    uint64_t hash;
    size_t bucket;
    size_t probe;
    size_t first_tombstone = SIZE_MAX;

    if (probes_out)
        *probes_out = 0;
    if (!index || !index->entries || !stable_key || !stable_key[0] ||
        !cmp_index_capacity_valid(index->capacity))
        return CMP_INDEX_INVALID;
    hash = cmp_index_hash(stable_key);
    bucket = (size_t)hash & (index->capacity - 1);
    for (probe = 0; probe < index->capacity; probe++) {
        size_t position = (bucket + probe) & (index->capacity - 1);
        cmp_index_entry *entry = &index->entries[position];
        if (probes_out)
            *probes_out = probe + 1;
        if (entry->state == CMP_INDEX_USED) {
            if (entry->hash == hash && entry->key &&
                strcmp(entry->key, stable_key) == 0)
                return CMP_INDEX_DUPLICATE;
            continue;
        }
        if (entry->state == CMP_INDEX_TOMBSTONE) {
            if (first_tombstone == SIZE_MAX)
                first_tombstone = position;
            continue;
        }
        if (first_tombstone != SIZE_MAX)
            entry = &index->entries[first_tombstone];
        entry->hash = hash;
        entry->key = stable_key;
        entry->value = value;
        if (entry->state == CMP_INDEX_TOMBSTONE)
            index->tombstones--;
        entry->state = CMP_INDEX_USED;
        index->count++;
        return CMP_INDEX_OK;
    }
    if (first_tombstone != SIZE_MAX) {
        cmp_index_entry *entry = &index->entries[first_tombstone];
        entry->hash = hash;
        entry->key = stable_key;
        entry->value = value;
        entry->state = CMP_INDEX_USED;
        index->count++;
        index->tombstones--;
        return CMP_INDEX_OK;
    }
    return CMP_INDEX_FULL;
}

int cmp_index_remove(cmp_index *index, const char *key, size_t *probes_out)
{
    uint64_t hash;
    size_t bucket;
    size_t probe;

    if (probes_out)
        *probes_out = 0;
    if (!index || !index->entries || !key || !key[0] ||
        !cmp_index_capacity_valid(index->capacity))
        return CMP_INDEX_INVALID;
    hash = cmp_index_hash(key);
    bucket = (size_t)hash & (index->capacity - 1);
    for (probe = 0; probe < index->capacity; probe++) {
        cmp_index_entry *entry =
            &index->entries[(bucket + probe) & (index->capacity - 1)];
        if (probes_out)
            *probes_out = probe + 1;
        if (entry->state == CMP_INDEX_EMPTY)
            return CMP_INDEX_NOT_FOUND;
        if (entry->state == CMP_INDEX_USED && entry->hash == hash &&
            entry->key && strcmp(entry->key, key) == 0) {
            entry->hash = 0;
            entry->key = NULL;
            entry->value = 0;
            entry->state = CMP_INDEX_TOMBSTONE;
            index->count--;
            index->tombstones++;
            return CMP_INDEX_OK;
        }
    }
    return CMP_INDEX_NOT_FOUND;
}
