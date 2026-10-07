#ifndef WATERDEEP_ACTION_CORE_H
#define WATERDEEP_ACTION_CORE_H
#include <stdbool.h>
#include <stdint.h>

enum action_state { EMPTY, PROPOSED, HELD, PREPARED, COMMITTED };
enum event_kind { PROPOSE, REPAIR, EVIDENCE, PREPARE, CONFIRM, SCENE, REVISE };
struct event {
    enum event_kind kind;
    uint32_t generation, scene, actor, operation;
    bool visible;
};
struct state {
    enum action_state action;
    uint32_t generation, scene, actor, operation;
    uint32_t commits, rejected;
    bool evidence;
};

/* Single-owner mock transaction: validation and commit occur in one step.
 * Production must enforce this boundary in the authoritative executor. */
static inline void step(struct state *s, struct event e) {
    if (e.kind == SCENE) {
        if (e.scene <= s->scene) { s->rejected++; return; }
        s->scene = e.scene;
        s->evidence = false;
        if (s->action == PREPARED) s->action = HELD;
        return;
    }
    if (e.kind == REVISE) {
        if (e.generation <= s->generation) { s->rejected++; return; }
        s->generation = e.generation;
        s->evidence = false;
        if (s->action != EMPTY && s->action != COMMITTED) s->action = HELD;
        return;
    }
#ifndef REPLAY_MUTANT_STALE
    if (e.generation != s->generation || e.scene != s->scene) {
        s->rejected++; return;
    }
#endif
    if (e.actor != s->actor) { s->rejected++; return; }
    switch (e.kind) {
    case PROPOSE:
        if (s->action != EMPTY || e.operation == 0) { s->rejected++; break; }
        s->operation = e.operation;
        s->action = PROPOSED;
#ifdef REPLAY_MUTANT_EAGER
        s->commits++;
#endif
        break;
    case REPAIR:
        if (e.operation != s->operation || s->action == EMPTY || s->action == COMMITTED) {
            s->rejected++; break;
        }
        s->action = HELD;
        break;
    case EVIDENCE:
        if (!e.visible) { s->rejected++; break; }
        s->evidence = true;
        break;
    case PREPARE:
        if (!s->evidence || e.operation != s->operation ||
            (s->action != PROPOSED && s->action != HELD)) {
            s->rejected++; break;
        }
        s->action = PREPARED;
        break;
    case CONFIRM:
        if (e.operation != s->operation) { s->rejected++; break; }
        if (s->action == COMMITTED) {
#ifdef REPLAY_MUTANT_DUPLICATE
            s->commits++;
#endif
            break; /* Existing receipt: no new state change. */
        }
        if (s->action != PREPARED || !s->evidence) { s->rejected++; break; }
        s->commits++;
        s->action = COMMITTED;
        break;
    default: s->rejected++; break;
    }
}


#endif
