/* Offline adapter to the shared lifecycle kernel. No game executor or network. */
#include "action_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text, uint32_t *value) {
    if (!*text || strlen(text) > 7) return 0;
    *value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        *value = *value * 10 + (uint32_t)(*p - '0');
    }
    return *value <= 1000000;
}

int main(int argc, char **argv) {
    /* requested, eligible, prepared, current generation, result generation,
     * prepared scene, current scene, owner, speaker, operation,
     * explicit confirmation, verified target, executor prerequisites, receipt. */
    uint32_t v[14];
    if (argc != 15) return 2;
    for (unsigned i = 0; i < 14; i++) if (!number(argv[i + 1], &v[i])) return 2;
    for (unsigned i = 0; i < 14; i++)
        if ((i < 3 || i >= 10) && v[i] > 1) return 2;
    struct state s = { .action = v[13] ? COMMITTED : v[2] ? PREPARED : HELD,
        .generation = v[3], .scene = v[5], .actor = v[7], .operation = v[9],
        .evidence = v[11] == 1 };
    struct event e = {CONFIRM, v[4], v[6], v[8], v[9], true};
    /* These flags and identities come from the trusted fixture, not model prose. */
    if (v[0] && v[1] && v[3] && v[4] && v[5] && v[6] && v[7] && v[8] && v[9] &&
        v[10] && v[11] && v[12]) {
        step(&s, e);
        step(&s, e); /* A delivery retry must not create a second effect. */
    }
    printf("{\"new_commits\":%u,\"rejected\":%u,\"state\":%u}\n",
           s.commits, s.rejected, s.action);
    return 0;
}
