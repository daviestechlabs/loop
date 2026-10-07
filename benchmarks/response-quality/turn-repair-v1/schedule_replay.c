/* Offline event-order experiment. This is not a concurrent or durable executor. */
#include "action_core.h"
#include <stdio.h>

enum arrival { FIRST_CONFIRM, RETRY_CONFIRM, NEW_SCENE, REPAIR_REVISION,
               OLD_RESEARCH, FOREIGN_CONFIRM, ARRIVALS };
static const char *names[] = {"confirm", "retry", "scene", "revision", "old_research", "foreign"};
static unsigned schedules, failures, positive_schedules;

static void evaluate(const unsigned order[ARRIVALS]) {
    struct state s = {.generation=1, .scene=10, .actor=1};
    struct event e = {PROPOSE, 1, 10, 1, 7, true};
    step(&s, e);
    int passed = s.commits == 0;
    e.kind = EVIDENCE; step(&s, e);
    e.kind = PREPARE; step(&s, e);
    passed = passed && s.commits == 0 && s.action == PREPARED;
    /* Independent policy oracle: old confirmation can win only before either
     * context invalidation. Once a commit wins, retries cannot add an effect. */
    bool invalidated = false;
    unsigned expected = 0, prefix_failures = 0;
    for (unsigned i=0; i<ARRIVALS; i++) {
        unsigned a = order[i];
        e = (struct event){CONFIRM, 1, 10, 1, 7, true};
        if (a == NEW_SCENE) {
            invalidated = true; e.kind = SCENE; e.scene = 11; step(&s, e);
        } else if (a == REPAIR_REVISION) {
            invalidated = true; e.kind = REVISE; e.generation = 2; step(&s, e);
        } else if (a == OLD_RESEARCH) {
            e.kind = EVIDENCE; step(&s, e);
            e.kind = PREPARE; step(&s, e);
        } else if (a == FOREIGN_CONFIRM) {
            e.actor = 2; step(&s, e);
        } else {
            if (!invalidated) expected = 1;
            step(&s, e);
        }
        if (s.commits != expected) prefix_failures++;
    }
    passed = passed && prefix_failures == 0;
    schedules++; failures += !passed; positive_schedules += expected == 1;
    printf("{\"order\":[");
    for (unsigned i=0; i<ARRIVALS; i++) printf("%s\"%s\"", i ? "," : "", names[order[i]]);
    printf("],\"expected_commits\":%u,\"observed_commits\":%u,\"prefix_failures\":%u,\"passed\":%s}\n",
           expected, s.commits, prefix_failures, passed ? "true" : "false");
}

static void enumerate(unsigned order[ARRIVALS], unsigned depth, unsigned used) {
    if (depth == ARRIVALS) { evaluate(order); return; }
    for (unsigned a=0; a<ARRIVALS; a++) {
        if (used & (1u << a)) continue;
        order[depth] = a;
        enumerate(order, depth+1, used | (1u << a));
    }
}

int main(void) {
    unsigned order[ARRIVALS];
    enumerate(order, 0, 0);
    printf("{\"schedules\":%u,\"failures\":%u,\"positive_schedules\":%u}\n",
           schedules, failures, positive_schedules);
    return failures ? 1 : 0;
}
