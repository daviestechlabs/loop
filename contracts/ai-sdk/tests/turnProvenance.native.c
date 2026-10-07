#include "turn_provenance.h"
#include "pb_min.h"
#include "turn_response_json.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    static uint8_t wire[DTL_PROVENANCE_INPUT_CAPACITY + 1u];
    static char output[DND_RAG_PUBLIC_JSON_CAP];
    static uint8_t public_wire[DTL_PROVENANCE_INPUT_CAPACITY];
    turn_event_c event;
    turn_response_state state = {0};
    turn_response_event safe;
    size_t length, request_length, output_length;
    if (argc != 2) return 2;
    request_length = strlen(argv[1]);
    length = fread(wire, 1, sizeof(wire), stdin);
    if (ferror(stdin) || length > DTL_PROVENANCE_INPUT_CAPACITY ||
        request_length >= DTL_PROVENANCE_REQUEST_CAPACITY) return 2;
    memcpy(dtl_provenance_input(), wire, length);
    memcpy(dtl_provenance_request(), argv[1], request_length + 1u);
    if (!dtl_provenance_project((uint32_t)length, (uint32_t)request_length)) return 1;
    /* Compare the browser ABI with the native HTTP boundary's JSON projection. */
    if (pb_decode_turn_event_bound(wire, length, argv[1], request_length, &event) != 0 ||
        turn_response_filter_bound(&state, &event, argv[1], &safe) <= TURN_RESPONSE_DROP) return 3;
    /* The WebTransport encoder must preserve the exact validated snapshot. */
    if (safe.encounter || safe.roster || safe.initiative) {
        turn_event_c projected;
        size_t public_length = turn_response_protobuf_encode(public_wire, sizeof(public_wire), &safe);
        if (!public_length || pb_decode_turn_event(public_wire, public_length, &projected) ||
            (safe.encounter && (projected.encounter.length != safe.encounter->length ||
                memcmp(projected.encounter.data, safe.encounter->data, safe.encounter->length))) ||
            (safe.roster && (projected.roster.length != safe.roster->length ||
                memcmp(projected.roster.data, safe.roster->data, safe.roster->length))) ||
            (safe.initiative && (projected.initiative.length != safe.initiative->length ||
                memcmp(projected.initiative.data, safe.initiative->data, safe.initiative->length)))) return 3;
    }
    output_length = turn_response_json_encode(output, sizeof(output), &safe, 1);
    if (output_length == 0u || fwrite(output, 1, output_length, stdout) != output_length) return 3;
    dtl_provenance_clear();
    return dtl_provenance_count(DTL_PROVENANCE_TOOL) || dtl_provenance_count(DTL_PROVENANCE_CITATION) ||
        dtl_provenance_count(DTL_PROVENANCE_PROMPT) ? 3 : 0;
}
