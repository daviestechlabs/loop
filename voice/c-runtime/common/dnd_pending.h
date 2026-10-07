/* Bounded held proposals. Single owner; no I/O, allocation, or game effects. */
#ifndef VOICE_C_DND_PENDING_H
#define VOICE_C_DND_PENDING_H

#include <stddef.h>
#include <stdint.h>

#define DND_PENDING_CAPACITY 128u
#define DND_PENDING_ID_CAP 128u
#define DND_PENDING_SCOPE_CAP 65u
#define DND_PENDING_LABEL_CAP 201u

typedef struct {
    const char *user_id;
    const char *session_id;
    const char *campaign_id;
    const char *scene_id;
} dnd_pending_key;

/* Internal capability. Never accept a ticket from a browser or model. */
typedef struct {
    uint64_t generation;
    size_t slot;
} dnd_pending_ticket;

typedef struct {
    char spell[DND_PENDING_LABEL_CAP];
    char proposal_request_id[DND_PENDING_ID_CAP];
    char question_character[DND_PENDING_LABEL_CAP];
    int held;
    int scene_changed;
} dnd_pending_view;

typedef struct {
    char user_id[DND_PENDING_ID_CAP];
    char session_id[DND_PENDING_ID_CAP];
    char campaign_id[DND_PENDING_SCOPE_CAP];
    char scene_id[DND_PENDING_SCOPE_CAP];
    char request_id[DND_PENDING_ID_CAP];
    uint8_t input_sha256[32];
    dnd_pending_view view;
    uint64_t generation;
    uint64_t touched_ms;
    uint64_t scene_revision;
    int occupied;
    int active;
    int proposal_update;
} dnd_pending_slot;

typedef struct {
    dnd_pending_slot slots[DND_PENDING_CAPACITY];
    uint64_t generation;
    uint64_t clock_ms;
    uint64_t idle_ms;
} dnd_pending_store;

enum {
    DND_PENDING_OK = 0,
    DND_PENDING_RETRY = 1,
    DND_PENDING_CLOSED = 2,
    DND_PENDING_NOT_FOUND = 3,
    DND_PENDING_INVALID = -1,
    DND_PENDING_FULL = -2,
    DND_PENDING_STALE = -3,
    DND_PENDING_CONFLICT = -4
};

/* Initialize once per process owner, before it admits any turn. Restart loses
 * proposals. The caller must not reset a store with outstanding tickets. */
int dnd_pending_init(dnd_pending_store *store, uint64_t idle_ms);

/* Admission order owns generation. The authenticated edge must reject request
 * replay before this call. Only a retry of the most recent request is retained.
 * input_sha256 binds the original complete input, not a model interpretation.
 * A campaign change clears a proposal. A scene change keeps it held and marks
 * revalidation necessary. No call prepares, authorizes, or executes a spell. */
int dnd_pending_begin(dnd_pending_store *store, dnd_pending_key key,
    const char *request_id, const uint8_t input_sha256[32], uint64_t now_ms,
    dnd_pending_ticket *ticket);
/* Resume only an existing session; ordinary questions cannot fill the store. */
int dnd_pending_resume(dnd_pending_store *store, dnd_pending_key key,
    const char *request_id, const uint8_t input_sha256[32], uint64_t now_ms,
    dnd_pending_ticket *ticket);
/* Response generation may finish before queued speech. Return the latest
 * retained request for this owner/session so a new turn can cancel that speech.
 * Scope changes must also stop the previous response. This grants no ticket. */
int dnd_pending_latest_request(dnd_pending_store *store, const char *user_id,
    const char *session_id, uint64_t now_ms, char request_id[DND_PENDING_ID_CAP]);

int dnd_pending_read(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, dnd_pending_view *view);
int dnd_pending_hold(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, const char *spell);
int dnd_pending_cancel(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms);
/* A name supplied in a question is a reference, not an observed scene fact. */
int dnd_pending_focus(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, const char *character_name);
int dnd_pending_interrupt(dnd_pending_store *store, const char *user_id,
    const char *request_id, uint64_t now_ms);

/* Accept only a revision from a verified scene result for this exact ticket
 * and scope. This stores no presence fact or permission. Revisions cannot move
 * backward within a scene. A newer observation marks a held proposal stale. */
int dnd_pending_observe_scene(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, uint64_t scene_revision);

/* End response work and invalidate its ticket without canceling the player's
 * proposal. A transport interruption is not a semantic spell cancellation. */
int dnd_pending_finish(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms);

#endif
