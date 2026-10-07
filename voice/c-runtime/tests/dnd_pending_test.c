#include "dnd_pending.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void check(int condition, const char *name) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL %s\n", name); exit(1); }
}

static dnd_pending_store store;
static const uint8_t digest[32] = {1};
static const uint8_t different_digest[32] = {2};
static const dnd_pending_key key = {"player-a", "session-a", "campaign-a", "scene-a"};

static void lifecycle(void) {
    dnd_pending_ticket first, second, repeated;
    dnd_pending_view view;
    check(dnd_pending_init(&store, 1000) == 0, "initialize");
    check(dnd_pending_begin(&store, key, "turn-1", digest, 1, &first) == 0, "admit first");
    check(dnd_pending_read(&store, first, 1, &view) == 0 && !view.held, "initially empty");
    check(dnd_pending_hold(&store, first, 1, "Fireball") == 0, "hold proposal");
    check(dnd_pending_hold(&store, first, 1, "Fireball") == DND_PENDING_RETRY, "same proposal retry");
    check(dnd_pending_hold(&store, first, 1, "Lightning") == DND_PENDING_CONFLICT, "changed interpretation rejects");
    check(dnd_pending_begin(&store, key, "turn-1", digest, 2, &repeated) == DND_PENDING_RETRY &&
        first.generation == repeated.generation, "same admission retry");
    check(dnd_pending_begin(&store, key, "turn-1", different_digest, 2, &repeated) == DND_PENDING_CONFLICT &&
        !repeated.generation, "changed input retry rejects");
    check(dnd_pending_finish(&store, first, 3) == 0, "transport completion");
    check(dnd_pending_hold(&store, first, 3, "Fireball") == DND_PENDING_STALE, "completed ticket sealed");
    check(dnd_pending_begin(&store, key, "turn-1", digest, 4, &repeated) == DND_PENDING_CLOSED &&
        !repeated.generation, "completed retry cannot reopen");
    check(dnd_pending_begin(&store, key, "turn-2", digest, 5, &second) == 0, "admit follow-up");
    check(dnd_pending_read(&store, second, 5, &view) == 0 && view.held &&
        !strcmp(view.spell, "Fireball") && !strcmp(view.proposal_request_id, "turn-1"),
        "proposal survives response completion");
    check(dnd_pending_cancel(&store, first, 5) == DND_PENDING_STALE, "late cancellation cannot change new turn");
    check(dnd_pending_cancel(&store, second, 6) == 0, "semantic cancellation");
    check(dnd_pending_hold(&store, second, 6, "Fireball") == DND_PENDING_CONFLICT,
        "canceled proposal cannot revive within same turn");
    check(dnd_pending_read(&store, second, 6, &view) == 0 && !view.held &&
        !view.spell[0] && !view.proposal_request_id[0], "cancellation clears proposal bytes");
    check(dnd_pending_begin(&store, key, "turn-3", digest, 7, &repeated) == 0 &&
        dnd_pending_hold(&store, repeated, 7, "Lightning") == 0, "later explicit proposal allowed");
    check(dnd_pending_hold(&store, first, 7, "Fireball") == DND_PENDING_STALE,
        "late interpretation cannot restore old proposal");
}

static void isolation_and_scene(void) {
    dnd_pending_ticket original, other, latest;
    dnd_pending_view view;
    dnd_pending_key changed = key;
    check(dnd_pending_init(&store, 1000) == 0, "reset isolated test store");
    check(dnd_pending_begin(&store, key, "first", digest, 1, &original) == 0 &&
        dnd_pending_hold(&store, original, 1, "Fireball") == 0, "initial proposal");
    check(dnd_pending_observe_scene(&store, original, 1, 42) == 0 &&
        dnd_pending_observe_scene(&store, original, 1, 42) == DND_PENDING_RETRY,
        "first observation and identical retry");
    check(dnd_pending_read(&store, original, 1, &view) == 0 && !view.scene_changed,
        "first observation establishes baseline only");
    check(dnd_pending_observe_scene(&store, original, 1, 41) == DND_PENDING_STALE &&
        dnd_pending_observe_scene(&store, original, 1, 0) == DND_PENDING_INVALID,
        "old or missing scene revision rejects");
    check(dnd_pending_observe_scene(&store, original, 1, 43) == 0 &&
        dnd_pending_read(&store, original, 1, &view) == 0 && view.scene_changed,
        "changed revision in same scene invalidates proposal context");
    check(dnd_pending_hold(&store, original, 1, "Fireball") == DND_PENDING_RETRY &&
        dnd_pending_read(&store, original, 1, &view) == 0 && view.scene_changed,
        "proposal retry cannot clear a newer scene invalidation");
    changed.user_id = "player-b";
    check(dnd_pending_begin(&store, changed, "first", digest, 2, &other) == 0 &&
        dnd_pending_read(&store, other, 2, &view) == 0 && !view.held, "foreign owner sees no proposal");
    check(dnd_pending_cancel(&store, other, 2) == 0 &&
        dnd_pending_read(&store, original, 2, &view) == 0 && view.held,
        "foreign owner cannot cancel proposal");
    changed = key; changed.session_id = "session-b";
    check(dnd_pending_begin(&store, changed, "first", digest, 3, &other) == 0 &&
        dnd_pending_read(&store, other, 3, &view) == 0 && !view.held, "session isolation");
    changed = key; changed.scene_id = "scene-b";
    check(dnd_pending_begin(&store, changed, "first", digest, 4, &latest) == DND_PENDING_CONFLICT,
        "changed scene in retry rejects");
    check(dnd_pending_begin(&store, changed, "scene-change", digest, 4, &latest) == 0 &&
        dnd_pending_read(&store, latest, 4, &view) == 0 && view.held && view.scene_changed,
        "scene change keeps proposal held with invalidated context");
    check(dnd_pending_read(&store, original, 4, &view) == DND_PENDING_STALE && !view.held,
        "old scene ticket invalid");
    check(dnd_pending_observe_scene(&store, original, 4, 999) == DND_PENDING_STALE,
        "delayed scene result cannot update new turn");
    check(dnd_pending_begin(&store, key, "scene-return", digest, 5, &latest) == 0 &&
        dnd_pending_read(&store, latest, 5, &view) == 0 && view.scene_changed,
        "returning to old scene cannot restore prior validity");
    changed = key; changed.campaign_id = "campaign-b";
    check(dnd_pending_begin(&store, changed, "campaign-change", digest, 6, &latest) == 0 &&
        dnd_pending_read(&store, latest, 6, &view) == 0 && !view.held && !view.scene_changed,
        "campaign change clears proposal");
}

static void expiry_capacity_and_wrap(void) {
    dnd_pending_ticket first, ticket;
    dnd_pending_view view;
    dnd_pending_key changed = key;
    char session[64];
    check(dnd_pending_init(&store, 10) == 0, "short retention store");
    check(dnd_pending_begin(&store, key, "first", digest, 1, &first) == 0 &&
        dnd_pending_hold(&store, first, 1, "Fireball") == 0, "expiring proposal");
    check(dnd_pending_read(&store, first, 10, &view) == 0 && view.held, "before expiry");
    check(dnd_pending_begin(&store, key, "first", digest, 10, &ticket) == DND_PENDING_RETRY,
        "retry before expiry");
    check(dnd_pending_read(&store, first, 11, &view) == DND_PENDING_STALE && !view.held,
        "retry did not extend expiry");
    check(dnd_pending_read(&store, first, 10, &view) == DND_PENDING_STALE,
        "backward clock cannot revive an expired ticket");
    check(dnd_pending_begin(&store, key, "new", digest, 11, &ticket) == 0 &&
        dnd_pending_read(&store, ticket, 11, &view) == 0 && !view.held, "expired slot reused empty");
    check(first.slot == ticket.slot && first.generation != ticket.generation &&
        dnd_pending_hold(&store, first, 11, "Fireball") == DND_PENDING_STALE,
        "slot reuse cannot accept old ticket");
    check(dnd_pending_read(&store, ticket, 10, &view) == DND_PENDING_STALE,
        "backward clock rejects");
    check(dnd_pending_init(&store, 1000) == 0, "bounded store");
    for (size_t i = 0; i < DND_PENDING_CAPACITY; ++i) {
        (void)snprintf(session, sizeof(session), "session-%zu", i);
        changed.session_id = session;
        check(dnd_pending_begin(&store, changed, "first", digest, 1, &ticket) == 0,
            "fill slot");
        if (!i) first = ticket;
    }
    check(dnd_pending_begin(&store, key, "full", digest, 2, &ticket) == DND_PENDING_FULL &&
        !ticket.generation, "capacity fails without eviction");
    check(dnd_pending_read(&store, first, 2, &view) == 0, "capacity failure preserves active turn");
    store.generation = UINT64_MAX;
    check(dnd_pending_begin(&store, changed, "wrap", digest, 3, &ticket) == DND_PENDING_FULL &&
        store.generation == UINT64_MAX, "generation never wraps");
}

static void input_bounds(void) {
    dnd_pending_ticket ticket;
    dnd_pending_view view;
    dnd_pending_key changed = key;
    char label[DND_PENDING_LABEL_CAP + 1u];
    char id[DND_PENDING_ID_CAP + 1u];
    check(dnd_pending_init(NULL, 1) == DND_PENDING_INVALID &&
        dnd_pending_init(&store, 0) == DND_PENDING_INVALID, "invalid initialization");
    check(dnd_pending_init(&store, UINT64_MAX) == 0, "max retention accepted without addition");
    check(dnd_pending_begin(&store, key, "first", digest, 0, &ticket) == DND_PENDING_INVALID,
        "zero clock rejects");
    changed.user_id = "../player";
    check(dnd_pending_begin(&store, changed, "first", digest, 1, &ticket) == DND_PENDING_INVALID,
        "invalid identity rejects");
    changed = key; changed.scene_id = "";
    check(dnd_pending_begin(&store, changed, "first", digest, 1, &ticket) == DND_PENDING_INVALID,
        "missing scope rejects");
    memset(id, 'a', sizeof(id)); changed = key; changed.session_id = id;
    check(dnd_pending_begin(&store, changed, "first", digest, 1, &ticket) == DND_PENDING_INVALID,
        "unterminated bounded identity rejects");
    id[DND_PENDING_ID_CAP - 1u] = '\0';
    check(dnd_pending_begin(&store, changed, "first", digest, 1, &ticket) == 0,
        "exact identity limit accepted");
    memset(label, 'x', sizeof(label));
    check(dnd_pending_hold(&store, ticket, 1, label) == DND_PENDING_INVALID, "overlong label rejects");
    label[DND_PENDING_LABEL_CAP - 1u] = '\0';
    check(dnd_pending_hold(&store, ticket, 1, label) == 0, "exact label limit accepted");
    check(dnd_pending_begin(&store, changed, "second", digest, UINT64_MAX - 1u, &ticket) == 0,
        "large clock no overflow");
    const char *invalid[] = {NULL, "", " ", " Fireball", "Fireball ", "Fire\nball", "\xff"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(dnd_pending_hold(&store, ticket, UINT64_MAX - 1u, invalid[i]) == DND_PENDING_INVALID,
            "invalid label rejects");
    check(dnd_pending_hold(&store, ticket, UINT64_MAX - 1u, "Éclair") == 0,
        "canonical UTF-8 proposal");
    check(dnd_pending_read(&store, ticket, UINT64_MAX, &view) == 0 &&
        !strcmp(view.spell, "Éclair"), "large clock view");
    ticket.slot = DND_PENDING_CAPACITY;
    check(dnd_pending_read(&store, ticket, UINT64_MAX, &view) == DND_PENDING_STALE && !view.held,
        "invalid slot rejects and clears output");
}

static void admission_and_interrupt(void) {
    dnd_pending_ticket ticket, next;
    dnd_pending_view view;
    check(dnd_pending_init(&store, 100) == 0, "initialize admission test");
    check(dnd_pending_resume(&store, key, "ordinary", digest, 1, &ticket) == DND_PENDING_NOT_FOUND &&
        !ticket.generation && !store.slots[0].occupied, "ordinary turn cannot allocate dialogue state");
    check(dnd_pending_begin(&store, key, "repair", digest, 2, &ticket) == 0 &&
        dnd_pending_hold(&store, ticket, 2, "Fireball") == 0 &&
        dnd_pending_focus(&store, ticket, 2, "Mira") == 0, "store proposal and explicit question reference");
    check(dnd_pending_interrupt(&store, "player-b", "repair", 3) == DND_PENDING_NOT_FOUND &&
        dnd_pending_read(&store, ticket, 3, &view) == 0 && view.held, "foreign interrupt cannot change owner state");
    check(dnd_pending_interrupt(&store, key.user_id, "repair", 4) == 0 &&
        dnd_pending_focus(&store, ticket, 4, "Eli") == DND_PENDING_STALE, "transport interrupt seals results");
    check(dnd_pending_resume(&store, key, "follow-up", digest, 5, &next) == 0 &&
        dnd_pending_read(&store, next, 5, &view) == 0 && view.held &&
        !strcmp(view.question_character, "Mira"), "transport interrupt preserves proposal and reference");
    check(dnd_pending_focus(&store, next, 5, "Eli") == 0 &&
        dnd_pending_read(&store, next, 5, &view) == 0 && !strcmp(view.question_character, "Eli"),
        "latest explicit question replaces reference");
    check(dnd_pending_focus(&store, next, 5, "Mira\n") == DND_PENDING_INVALID &&
        dnd_pending_focus(&store, next, 5, "\xff") == DND_PENDING_INVALID,
        "invalid reference rejects");
    check(dnd_pending_resume(&store, key, "expired", digest, 105, &next) == DND_PENDING_NOT_FOUND,
        "expired session cannot resume");
}

static void retained_response_identity(void) {
    dnd_pending_ticket ticket;
    char request[DND_PENDING_ID_CAP] = "old";
    check(dnd_pending_init(&store, 100) == 0, "initialize response identity test");
    check(dnd_pending_latest_request(&store, key.user_id, key.session_id, 1, request) ==
        DND_PENDING_NOT_FOUND && !request[0], "absent response clears output");
    check(dnd_pending_begin(&store, key, "speech-turn", digest, 2, &ticket) == 0 &&
        dnd_pending_finish(&store, ticket, 3) == 0, "generation finishes before queued speech");
    check(dnd_pending_latest_request(&store, key.user_id, key.session_id, 4, request) == 0 &&
        !strcmp(request, "speech-turn"), "completed response remains cancelable");
    check(dnd_pending_latest_request(&store, "player-b", key.session_id, 4, request) ==
        DND_PENDING_NOT_FOUND && !request[0], "foreign owner cannot find response");
    check(dnd_pending_latest_request(&store, key.user_id, "other-session", 4, request) ==
        DND_PENDING_NOT_FOUND && !request[0], "foreign session cannot find response");
    check(dnd_pending_latest_request(&store, key.user_id, key.session_id, 102, request) ==
        DND_PENDING_NOT_FOUND && !request[0], "expired response clears output");
    check(dnd_pending_latest_request(&store, key.user_id, key.session_id, 4, request) ==
        DND_PENDING_INVALID && !request[0], "clock rollback cannot restore response");
}

int main(void) {
    lifecycle();
    isolation_and_scene();
    expiry_capacity_and_wrap();
    input_bounds();
    admission_and_interrupt();
    retained_response_identity();
    printf("ALL PASS held proposal state: %u checks\n", checks);
    return 0;
}
