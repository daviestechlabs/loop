#ifndef LOOP_RETENTION_H
#define LOOP_RETENTION_H
#include <stdint.h>
typedef struct { uint64_t snapshots, eligible, deleted; } loop_retention_result;
/* Operator-only command. The dedicated directory must have no concurrent writers.
 * No policy default: days is explicit, and apply must be exactly zero or one. */
int loop_retention(const char *directory, uint32_t days, int64_t now, int apply,
                   loop_retention_result *result);
#endif
