#include "pt_campaign.c"

/* Exercise the same deterministic transition that journal recovery repeats. */
static void transition_fuzz(const char *input) {
    cmp_json_object envelope;
    const cmp_json_field *before_field, *command_field;
    char *before = NULL, *after = NULL, *again = NULL;
    pt_call *call = NULL, *retry = NULL;
    campaign_state *state = NULL;
    if (!cmp_json_object_parse(input, &envelope)) return;
    before_field = cmp_json_object_field(&envelope, "before");
    command_field = cmp_json_object_field(&envelope, "command");
    if (!before_field || !command_field || before_field->value_len >= PT_OUT || command_field->value_len >= PT_INPUT) return;
    before = calloc(1, PT_OUT);
    after = malloc(PT_OUT);
    again = malloc(PT_OUT);
    call = calloc(1, sizeof(*call));
    retry = calloc(1, sizeof(*retry));
    state = calloc(1, sizeof(*state));
    if (!before || !after || !again || !call || !retry || !state) goto done;
    memcpy(before, before_field->value, before_field->value_len);
    memcpy(call->input_json, command_field->value, command_field->value_len);
    strcpy(call->user_id, "user");
    strcpy(call->tool_call_id, "roster-fuzz");
    *retry = *call;
    if (pt_campaign_transition(before, after, PT_OUT, call)) {
        size_t length = strlen(after);
        if (!state_decode(after, state) || !pt_campaign_transition(before, again, PT_OUT, retry) ||
            strcmp(after, again) || memcmp(call, retry, sizeof(*call)) ||
            pt_campaign_transition(before, again, length, retry) || retry->output_json[0]) abort();
    }
done:
    free(state);
    free(retry);
    free(call);
    free(again);
    free(after);
    free(before);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    campaign_command command;
    campaign_state *state = NULL, *roundtrip = NULL;
    char *input = NULL, *encoded = NULL, *again = NULL;
    size_t length;
    if (size >= PT_OUT || memchr(data, 0, size)) return 0;
    input = malloc(size + 1u);
    state = calloc(1, sizeof(*state));
    roundtrip = calloc(1, sizeof(*roundtrip));
    encoded = malloc(PT_OUT);
    again = malloc(PT_OUT);
    if (!input || !state || !roundtrip || !encoded || !again) goto done;
    memcpy(input, data, size);
    input[size] = '\0';
    (void)command_decode(input, &command);
    transition_fuzz(input);
    if (state_decode(input, state)) {
        length = state_encode(encoded, PT_OUT, state, NULL, NULL);
        if (length && (!state_decode(encoded, roundtrip) ||
            !state_encode(again, PT_OUT, roundtrip, NULL, NULL) || strcmp(encoded, again) ||
            state_encode(again, length, state, NULL, NULL) || again[0])) abort();
    }
done:
    free(again);
    free(encoded);
    free(roundtrip);
    free(state);
    free(input);
    return 0;
}
