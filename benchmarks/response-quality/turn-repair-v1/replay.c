/* Development model of the action boundary. No speech model or live executor. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "action_core.h"

static unsigned failures;
static unsigned checks;
static void check(const char *id, bool pass) {
    checks++;
    if (!pass) failures++;
    printf("{\"check\":\"%s\",\"passed\":%s}\n", id, pass ? "true" : "false");
}
static struct state initial(void) {
    return (struct state){ .generation=1, .scene=42, .actor=7 };
}
static struct event event(enum event_kind kind) {
    return (struct event){kind, 1, 42, 7, 9, true};
}
static struct state prepared(void) {
    struct state s = initial();
    step(&s, event(PROPOSE)); step(&s, event(EVIDENCE)); step(&s, event(PREPARE));
    return s;
}

int main(void) {
    struct state s = initial();
    step(&s, event(PROPOSE));
    check("proposal-has-no-effect", s.action == PROPOSED && s.commits == 0);
    step(&s, event(REPAIR)); step(&s, event(EVIDENCE));
    check("question-keeps-spell-held", s.action == HELD && s.evidence && s.commits == 0);
    step(&s, event(CONFIRM));
    check("confirmation-needs-preparation", s.action == HELD && s.commits == 0);

    s = prepared(); step(&s, event(CONFIRM));
    check("valid-confirmation-commits", s.action == COMMITTED && s.commits == 1);
    step(&s, event(CONFIRM));
    check("duplicate-confirmation-is-idempotent", s.commits == 1);
    step(&s, event(REPAIR));
    check("late-repair-preserves-receipt", s.action == COMMITTED && s.commits == 1 && s.rejected == 1);

    s = prepared(); struct event e = event(SCENE); e.scene = 43; step(&s, e);
    step(&s, event(CONFIRM));
    check("scene-change-blocks-old-confirmation", s.action == HELD && s.commits == 0);

    s = initial(); e = event(REVISE); e.generation = 2; step(&s, e);
    step(&s, event(EVIDENCE));
    check("late-evidence-is-rejected", !s.evidence && s.rejected == 1);
    step(&s, event(PROPOSE));
    check("late-model-proposal-is-rejected", s.action == EMPTY && s.commits == 0);

    s = initial(); e = event(EVIDENCE); e.visible = false; step(&s, e);
    step(&s, event(PROPOSE)); step(&s, event(PREPARE));
    check("hidden-evidence-cannot-prepare", !s.evidence && s.action == PROPOSED);

    s = prepared(); e = event(CONFIRM); e.actor = 8; step(&s, e);
    check("other-speaker-cannot-confirm", s.commits == 0 && s.action == PREPARED);
    e = event(CONFIRM); e.operation = 10; step(&s, e);
    check("wrong-operation-cannot-confirm", s.commits == 0);

    s = prepared(); e = event(REVISE); e.generation = 2; step(&s, e);
    step(&s, event(CONFIRM));
    check("revision-invalidates-preparation", s.action == HELD && !s.evidence && s.commits == 0);

    /* Exhaust both serialized orders at the mock commit boundary. */
    s = prepared(); step(&s, event(REPAIR)); step(&s, event(CONFIRM));
    check("repair-wins-before-commit", s.action == HELD && s.commits == 0);
    s = prepared(); step(&s, event(CONFIRM)); step(&s, event(REPAIR));
    check("commit-wins-before-repair", s.action == COMMITTED && s.commits == 1);
    printf("{\"checks\":%u,\"failures\":%u,\"scope\":\"mock-c-lifecycle\"}\n", checks, failures);
    return failures ? 1 : 0;
}
