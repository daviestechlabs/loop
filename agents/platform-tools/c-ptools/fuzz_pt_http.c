/* Signed identity is an independent input to bounded tool admission. No I/O. */
#include "pt_http.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    pt_start_req request;
    char *input;
    if (size > PT_HTTP_BODY_CAP || memchr(data, 0, size)) return 0;
    input = malloc(size + 1u);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';
    if (!parse_tool_request(input, "fuzz-user", &request)) {
        if (strcmp(request.user_id, "fuzz-user") || strcmp(request.agent_id, "dnd-agent") ||
            (strcmp(request.tool_id, "dnd-dice-roll") && strcmp(request.tool_id, "dnd-campaign-state") &&
             strcmp(request.tool_id, "dnd-encounter-state")) ||
            !request.input_json[0] || !request.tool_call_id[0] || !request.parent_turn_id[0]) abort();
    }
    free(input);
    return 0;
}
