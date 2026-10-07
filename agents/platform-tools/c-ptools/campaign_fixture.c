#include "pt_campaign.c"
#include "state_fixture.h"

int main(int argc, char **argv) {
    char input[PT_OUT];
    size_t n = fread(input, 1u, sizeof(input), stdin);
    int result = 1;
    if (ferror(stdin) || !n || n == sizeof(input) || memchr(input, '\0', n)) return 2;
    input[n] = '\0';
    if (argc == 2 && !strcmp(argv[1], "roundtrip")) {
        campaign_state *state = calloc(1, sizeof(*state));
        char *output = malloc(PT_OUT);
        size_t length;
        if (state && output && state_decode(input, state)) {
            length = state_encode(output, PT_OUT, state, NULL, NULL);
            if (length && !state_encode(output, length, state, NULL, NULL) && !output[0] &&
                state_encode(output, PT_OUT, state, NULL, NULL)) { puts(output); result = 0; }
        }
        free(output);
        free(state);
        return result;
    }
    return fixture_execute(getenv("PT_TEST_SCENE_TOOL") ? "dnd-scene-presence" : "dnd-campaign-state", argc, argv, input, n);
}
