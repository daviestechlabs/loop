#include "dnd_dialogue.h"
#include <stdio.h>

int main(int argc, char **argv) {
    dnd_pending_view empty = {0};
    char output[4096];
    if (argc != 2 || dnd_dialogue_prompt(&empty, 0, argv[1], output, sizeof(output))) return 1;
    return fputs(output, stdout) < 0 ? 1 : 0;
}
