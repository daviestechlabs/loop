/* Codec-independent access to the shared untrusted turn-context policy. */
#ifndef C_COMPANIONS_CMP_TURN_METADATA_POLICY_H
#define C_COMPANIONS_CMP_TURN_METADATA_POLICY_H

#include "cmp_json.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_request.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_campaign.h"
#include "../../../contracts/handler-base/c-pb/pb_dnd_action.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int retrieval_force;
} cmp_turn_metadata_policy;

/* Visit admitted fields only after the complete object passes policy.
 * The callback copies a field and returns zero, or returns nonzero to reject.
 * Field pointers are borrowed for the callback duration only. */
typedef int (*cmp_turn_metadata_writer)(const char *key, const char *value, void *user);
int cmp_turn_metadata_visit(const cmp_json_object *body, cmp_turn_metadata_writer writer,
                            void *user, cmp_turn_metadata_policy *policy,
                            dnd_initiative_request_c *initiative, dnd_campaign_request_c *campaign,
                            dnd_encounter_action_c *action);

#ifdef __cplusplus
}
#endif

#endif
