#include "pt_encounter.c"
#include "state_fixture.h"

int main(int argc, char **argv) {
    char input[PT_OUT], output[PT_OUT];
    size_t size = fread(input, 1u, sizeof(input), stdin);
    if (ferror(stdin) || !size || size == sizeof(input) || memchr(input, '\0', size)) return 2;
    input[size] = '\0';
    if (argc == 2 && !strcmp(argv[1], "roundtrip")) {
        dnd_encounter_v1 state;
        size_t length;
        if (!state_decode(input, &state)) return 1;
        length = state_encode(output, sizeof(output), &state);
        if (!length || state_encode(output, length, &state) || output[0] ||
            !state_encode(output, sizeof(output), &state)) return 2;
        puts(output);
        return 0;
    }
    return fixture_execute("dnd-encounter-state", argc, argv, input, size);
}
