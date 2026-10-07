#include "dnd_tools.h"
#include "pb_min.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char input[65536], expression[64], canonical[80], reparsed[64], text[2048];
    turn_event_c event;
    dnd_tool_result result = {0};
    dnd_initiative_request_c request, decoded;
    dnd_campaign_request_c campaign, decoded_campaign;
    dnd_encounter_action_c action, decoded_action;
    uint8_t wire[DND_INITIATIVE_REQUEST_MAX];
    if (size <= DND_TOOL_EVENT_MAX) (void)pb_decode_turn_event(data, size, &event);
    if (dnd_initiative_request_decode(data, size, &request)) {
        size_t length = dnd_initiative_request_encode(wire, sizeof(wire), &request);
        if (!length || !dnd_initiative_request_decode(wire, length, &decoded) ||
            memcmp(&request, &decoded, sizeof(request))) abort();
    }
    if (dnd_campaign_request_decode(data, size, &campaign)) {
        size_t length = dnd_campaign_request_encode(wire, sizeof(wire), &campaign);
        if (!length || !dnd_campaign_request_decode(wire, length, &decoded_campaign) ||
            memcmp(&campaign, &decoded_campaign, sizeof(campaign))) abort();
    }
    if (size >= sizeof(input)) return 0;
    if (dnd_encounter_action_decode(data, size, &action)) {
        size_t length = dnd_encounter_action_encode(wire, sizeof(wire), &action);
        if (!length || !dnd_encounter_action_decode(wire, length, &decoded_action) ||
            memcmp(&action, &decoded_action, sizeof(action))) abort();
    }
    if (memchr(data, '\0', size)) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    (void)dnd_encounter_intent(input);
    (void)dnd_action_intent(input);
    strcpy(result.call_id, "encounter-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    memset(result.output_sha256, 'b', 64u);
    for (uint32_t operation = DND_ACTION_ADVANCE; operation <= DND_ACTION_END; ++operation) {
        memset(&action, 0, sizeof(action));
        action.operation = operation;
        action.expected_version = 2;
        strcpy(action.operation_id, "action-1");
        if (operation != DND_ACTION_ADVANCE && operation != DND_ACTION_END) strcpy(action.participant_id, "aria");
        if (operation == DND_ACTION_DAMAGE || operation == DND_ACTION_HEAL) action.amount = 7;
        if (operation == DND_ACTION_CONDITION_ADD || operation == DND_ACTION_CONDITION_REMOVE) action.condition = 11;
        if (dnd_action_output(input, "table", "battle", &action, &result) == 0) {
            turn_encounter_state_c state;
            if (!result.encounter_length || pb_decode_turn_encounter(result.encounter_wire,
                    result.encounter_length, &state)) abort();
        }
    }
    if (dnd_encounter_output(input, "table", "battle", &result) == 0) {
        turn_encounter_state_c state;
        if (!result.encounter_length || pb_decode_turn_encounter(result.encounter_wire,
                result.encounter_length, &state) != 0) abort();
    }
    (void)dnd_roster_intent(input);
    (void)dnd_initiative_intent(input);
    strcpy(result.call_id, "campaign-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    (void)dnd_campaign_intent(input);
    memset(&campaign, 0, sizeof(campaign));
    campaign.operation = DND_CAMPAIGN_CREATE;
    strcpy(campaign.operation_id, "create-1");
    strcpy(campaign.data.campaign.name, "Table");
    strcpy(campaign.data.campaign.ruleset, "5e");
    if (dnd_campaign_output(input, "table", "user", &campaign, &result) == 0) {
        turn_campaign_roster_c roster;
        if (!result.roster_length || pb_decode_turn_roster(result.roster_wire, result.roster_length, &roster)) abort();
    }
    memset(&campaign, 0, sizeof(campaign));
    campaign.operation = DND_CAMPAIGN_ADD_CHARACTER;
    campaign.expected_version = 1;
    strcpy(campaign.operation_id, "add-1");
    strcpy(campaign.data.character.id, "pc");
    strcpy(campaign.data.character.name, "Aria");
    campaign.data.character.kind = 1;
    campaign.data.character.max_hp = 10;
    for (size_t i = 0; i < 6u; ++i) campaign.data.character.abilities[i] = 10;
    if (dnd_campaign_output(input, "table", "user", &campaign, &result) == 0) {
        turn_campaign_roster_c roster;
        if (!result.roster_length || pb_decode_turn_roster(result.roster_wire, result.roster_length, &roster)) abort();
    }
    if (dnd_roster_output(input, "table", &result) == 0) {
        turn_campaign_roster_c roster;
        if (!result.roster_length || pb_decode_turn_roster(result.roster_wire,
                result.roster_length, &roster) != 0) abort();
    }
    memset(&request, 0, sizeof(request));
    request.campaign_version = 2;
    request.count = 1;
    strcpy(request.operation_id, "roll-1");
    strcpy(request.selections[0].character_id, "pc-0");
    strcpy(request.selections[0].expression, "2d20kh1+5");
    strcpy(result.call_id, "encounter-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    if (dnd_initiative_output(input, "table", "battle", &request, &result) == 0) {
        turn_encounter_state_c state;
        turn_initiative_result_c rolls;
        if (!result.encounter_length || !result.initiative_length ||
            pb_decode_turn_encounter(result.encounter_wire, result.encounter_length, &state) != 0 ||
            pb_decode_turn_initiative(result.initiative_wire, result.initiative_length, &rolls) != 0) abort();
    }
    if (dnd_dice_intent(input, expression) == 1) {
        (void)snprintf(canonical, sizeof(canonical), "Roll %s", expression);
        if (dnd_dice_intent(canonical, reparsed) != 1 || strcmp(expression, reparsed) != 0) abort();
    }
    (void)dnd_dice_output_text(input, "2d20kh1+5", "dice-test", text, sizeof(text));
    (void)dnd_dice_output_text(input, "2d20kh1+5", "dice-test", text, size % sizeof(text));
    return 0;
}
