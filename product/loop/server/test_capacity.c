#include "capacity.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>

int main(void) {
    loop_capacity result;
    struct statvfs stats = {.f_frsize=10, .f_blocks=1000000000,
        .f_bfree=150000000, .f_bavail=150000000};
    assert(loop_capacity_evaluate(&stats, &result) == 0);
    --stats.f_bavail;
    assert(loop_capacity_evaluate(&stats, &result) == 1);
    stats.f_frsize=1; stats.f_blocks=UINT64_C(2147483648); stats.f_bfree=stats.f_blocks;
    stats.f_bavail=UINT64_C(1073741824);
    assert(loop_capacity_evaluate(&stats, &result) == 0);
    --stats.f_bavail;
    assert(loop_capacity_evaluate(&stats, &result) == 1);
    stats.f_bavail=0;
    assert(loop_capacity_evaluate(&stats, &result) == 1);
    stats.f_bavail=stats.f_blocks; stats.f_bfree=0;
    assert(loop_capacity_evaluate(&stats, &result) == 2);
    assert(result.capacity_bytes == 0 && result.available_bytes == 0);
    stats.f_bavail=0; stats.f_blocks=0;
    assert(loop_capacity_evaluate(&stats, &result) == 2);
    stats.f_blocks=(fsblkcnt_t)-1; stats.f_frsize=ULONG_MAX;
    assert(loop_capacity_evaluate(&stats, &result) == 2);
    stats.f_frsize=1; stats.f_bfree=stats.f_blocks; stats.f_bavail=stats.f_blocks;
    assert(loop_capacity_evaluate(&stats, &result) == 0);
    stats.f_frsize=0;
    assert(loop_capacity_evaluate(&stats, &result) == 2);
    assert(loop_capacity_check("/nonexistent-loop-capacity-test/path", &result) == 2);
    assert(result.capacity_bytes == 0 && result.available_bytes == 0);
    assert(loop_capacity_check(NULL, &result) == 2);
    assert(loop_capacity_check("", &result) == 2);
    assert(loop_capacity_check("/", NULL) == 2);
    int actual = loop_capacity_check("/", &result);
    assert(actual == 0 || actual == 1);
    assert(result.capacity_bytes > 0 && result.available_bytes <= result.capacity_bytes);
    puts("Capacity thresholds, reserved space, invalid statistics, overflow, and filesystem checks passed");
    return 0;
}
