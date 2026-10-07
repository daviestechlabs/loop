#ifndef CMP_INDEX_H
#define CMP_INDEX_H

#include <stddef.h>
#include <stdint.h>

enum cmp_index_result {
    CMP_INDEX_OK = 0,
    CMP_INDEX_NOT_FOUND = 1,
    CMP_INDEX_FULL = 2,
    CMP_INDEX_DUPLICATE = 3,
    CMP_INDEX_INVALID = 4,
};

enum cmp_index_slot_state {
    CMP_INDEX_EMPTY = 0,
    CMP_INDEX_USED = 1,
    CMP_INDEX_TOMBSTONE = 2,
};

typedef struct {
    uint64_t hash;
    const char *key;
    uint32_t value;
    uint8_t state;
} cmp_index_entry;

typedef struct {
    cmp_index_entry *entries;
    size_t capacity;
    size_t count;
    size_t tombstones;
} cmp_index;

/*
 * The index does not own keys. Each key pointer must remain valid and its text
 * must remain unchanged until removal or clear. Callers provide synchronization.
 */
int cmp_index_init(cmp_index *index, cmp_index_entry *entries, size_t capacity);
void cmp_index_clear(cmp_index *index);
uint64_t cmp_index_hash(const char *key);
int cmp_index_find(
    const cmp_index *index,
    const char *key,
    uint32_t *value_out,
    size_t *probes_out
);
int cmp_index_insert(
    cmp_index *index,
    const char *stable_key,
    uint32_t value,
    size_t *probes_out
);
int cmp_index_remove(cmp_index *index, const char *key, size_t *probes_out);

#endif
