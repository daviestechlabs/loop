#ifndef LOOP_ANVIL_REMOTE_H
#define LOOP_ANVIL_REMOTE_H
#include <stddef.h>
/* Input contains length bytes followed by a NUL terminator.
 * Validate the Anvil envelope against an already verified Loop session and link.
 * A match establishes a reported owned object, not model or run admission. */
int loop_anvil_remote_valid(const char *json, size_t length, const char *issuer,
                           const char *owner, const char *report_hash, int answers);
int loop_anvil_experiment_valid(const char *json, size_t length, const char *issuer,
                                const char *owner, const char *experiment_id);
int loop_anvil_native_valid(const char *json, size_t length, const char *issuer,
                            const char *owner, const char *candidate_id);
#endif
