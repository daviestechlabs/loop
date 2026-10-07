/* Bounded JSON serialization for public turn-stage metadata. */
#ifndef VOICE_C_STAGE_JSON_H
#define VOICE_C_STAGE_JSON_H

#include "../wire/pb_min.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write the optional `,"metadata":{...}` suffix. */
int turn_stage_metadata_json_v1(
    const turn_stage_timestamps_c *stages,
    char *out,
    size_t out_cap
);

/* Write the optional suffix. Return its length, zero when absent, or SIZE_MAX. */
size_t turn_stage_metadata_json_write_v1(
    const turn_stage_timestamps_c *stages,
    char *out,
    size_t out_cap
);

/* Reuse the exact stage wire returned by the authenticated public decoder.
 * The wire must remain valid until this function returns. */
size_t turn_stage_metadata_json_write_current_wire_v1(
    const turn_stage_timestamps_c *stages,
    const uint8_t *current_tts_stage_wire,
    char *out,
    size_t out_cap
);

#ifdef __cplusplus
}
#endif

#endif
