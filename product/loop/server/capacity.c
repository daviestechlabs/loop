#include "capacity.h"
#include <string.h>

int loop_capacity_evaluate(const struct statvfs *stats, loop_capacity *result) {
    if (!result) return 2;
    memset(result, 0, sizeof(*result));
    if (!stats || !stats->f_frsize || !stats->f_blocks ||
        stats->f_bavail > stats->f_bfree || stats->f_bfree > stats->f_blocks ||
        stats->f_blocks > UINT64_MAX / stats->f_frsize) return 2;
    result->capacity_bytes = (uint64_t)stats->f_blocks * stats->f_frsize;
    result->available_bytes = (uint64_t)stats->f_bavail * stats->f_frsize;
    /* ceil(capacity * 15 / 100), without overflowing the multiplication. */
    uint64_t floor = (result->capacity_bytes / 100) * 15 +
        ((result->capacity_bytes % 100) * 15 + 99) / 100;
    return result->available_bytes < floor ||
        result->available_bytes < UINT64_C(1073741824) ? 1 : 0;
}

int loop_capacity_check(const char *path, loop_capacity *result) {
    if (!result) return 2;
    memset(result, 0, sizeof(*result));
    struct statvfs stats;
    if (!path || !*path || statvfs(path, &stats)) return 2;
    return loop_capacity_evaluate(&stats, result);
}
