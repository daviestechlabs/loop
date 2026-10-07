/* cmp_turn_metadata.h — bounded untrusted turn-context admission. */
#ifndef C_COMPANIONS_CMP_TURN_METADATA_H
#define C_COMPANIONS_CMP_TURN_METADATA_H

#include "cmp_turn_metadata_policy.h"
#include "pb_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the optional metadata object and top-level transport into request metadata. */
int cmp_turn_metadata_parse(
    const cmp_json_object *body,
    pb_turn_start_req *request,
    cmp_turn_metadata_policy *policy
);

#ifdef __cplusplus
}
#endif

#endif
