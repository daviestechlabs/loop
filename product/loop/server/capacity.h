#ifndef LOOP_CAPACITY_H
#define LOOP_CAPACITY_H
#include <stdint.h>
#include <sys/statvfs.h>

typedef struct {
    uint64_t capacity_bytes;
    uint64_t available_bytes;
} loop_capacity;

/* 0: sufficient, 1: low, 2: unknown. No paths or file contents are exported. */
int loop_capacity_evaluate(const struct statvfs *stats, loop_capacity *result);
int loop_capacity_check(const char *path, loop_capacity *result);
#endif
