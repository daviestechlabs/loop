/* Exercise pure admission/serialization without touching retained files. */
#include "pt_encounter.c"

static void initiative_case(const char *json) {
    cmp_json_object object;
    const char *const names[] = {"before", "campaign", "draws", "command"};
    const size_t caps[] = {PT_OUT, PT_OUT, PT_INITIATIVE_DRAWS_CAP, PT_INPUT};
    char *values[4] = {NULL}, *after = NULL, *again = NULL;
    pt_call *call = NULL, *repeated = NULL;
    if (!cmp_json_object_parse(json, &object) || object.field_count != 4u) return;
    for (size_t i = 0; i < 4u; ++i) {
        const cmp_json_field *field = cmp_json_object_field(&object, names[i]);
        if (!field || field->value_len >= caps[i]) goto done;
        values[i] = calloc(caps[i], 1u);
        if (!values[i]) goto done;
        memcpy(values[i], field->value, field->value_len);
    }
    call = calloc(1, sizeof(*call));
    repeated = calloc(1, sizeof(*repeated));
    after = malloc(PT_OUT);
    again = malloc(PT_OUT);
    if (!call || !repeated || !after || !again) goto done;
    strcpy(call->tool_call_id, "initiative-fuzz");
    strcpy(call->user_id, "user");
    strcpy(call->input_json, values[3]);
    *repeated = *call;
    const char *before = !strcmp(values[0], "null") ? NULL : values[0];
    if (pt_encounter_initiative_transition(before, values[1], values[2], after, PT_OUT, call)) {
        size_t length = strlen(after);
        if (!pt_encounter_initiative_transition(before, values[1], values[2], again, PT_OUT, repeated) ||
            strcmp(after, again) || memcmp(call, repeated, sizeof(*call)) ||
            pt_encounter_initiative_transition(before, values[1], values[2], again, length, repeated) ||
            again[0] || repeated->output_json[0]) abort();
    } else if (after[0] || call->output_json[0]) abort();
done:
    free(again);
    free(after);
    free(repeated);
    free(call);
    for (size_t i = 0; i < 4u; ++i) free(values[i]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void action_case(const char *json) {
    cmp_json_object object;
    const cmp_json_field *prior, *command;
    char *before = NULL, *after = NULL;
    pt_call *call = NULL;
    if (!cmp_json_object_parse(json, &object) || object.field_count != 2u ||
        !(prior = cmp_json_object_field(&object, "before")) || prior->value_len >= PT_OUT ||
        !(command = cmp_json_object_field(&object, "command")) || command->value_len >= PT_INPUT) return;
    before = calloc(PT_OUT, 1u);
    after = malloc(PT_OUT);
    call = calloc(1, sizeof(*call));
    if (!before || !after || !call) goto done;
    memcpy(before, prior->value, prior->value_len);
    memcpy(call->input_json, command->value, command->value_len);
    strcpy(call->tool_call_id, "action-fuzz");
    strcpy(call->user_id, "user");
    if (pt_encounter_transition(before, after, PT_OUT, call)) {
        dnd_encounter_v1 state;
        size_t length = strlen(after);
        if (!state_decode(after, &state) || !call->output_json[0] ||
            pt_encounter_transition(before, after, length, call) || after[0]) abort();
    }
    /* The legacy transition API requires callers to discard all results on
     * failure. Only the state serializer promises an empty short buffer. */
done:
    free(call);
    free(after);
    free(before);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *input, *output;
    encounter_command command;
    dnd_encounter_v1 before, after;
    if (size >= PT_OUT || memchr(data, '\0', size)) return 0;
    input = malloc(size + 1u);
    output = malloc(PT_OUT);
    if (!input || !output) { free(input); free(output); return 0; }
    memcpy(input, data, size);
    input[size] = '\0';
    initiative_case(input);
    action_case(input);
    (void)command_decode(input, &command);
    if (state_decode(input, &before)) {
        size_t length = state_encode(output, PT_OUT, &before);
        if (length) {
            if (!state_decode(output, &after) || memcmp(&before, &after, sizeof(before)) ||
                state_encode(output, length, &before) || output[0]) abort();
        } else if (output[0]) abort();
    }
    free(input);
    free(output);
    return 0;
}
