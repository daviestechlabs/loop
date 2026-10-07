#include "dnd_pending.h"
#include "utf8.h"

#include <string.h>

_Static_assert(sizeof(dnd_pending_store) <= 192u * 1024u,
    "held proposal store exceeds its memory bound");

static size_t bounded_length(const char *text, size_t cap) {
    size_t n = 0;
    if (!text) return cap;
    while (n < cap && text[n]) ++n;
    return n;
}

static int valid_id(const char *text, size_t cap) {
    size_t n = bounded_length(text, cap);
    if (!n || n == cap) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == ':')) return 0;
    }
    return 1;
}

static int valid_key(dnd_pending_key key) {
    return valid_id(key.user_id, DND_PENDING_ID_CAP) &&
        valid_id(key.session_id, DND_PENDING_ID_CAP) &&
        valid_id(key.campaign_id, DND_PENDING_SCOPE_CAP) &&
        valid_id(key.scene_id, DND_PENDING_SCOPE_CAP);
}

static int clock_valid(const dnd_pending_store *store, uint64_t now_ms) {
    return store && store->idle_ms && now_ms && now_ms >= store->clock_ms;
}

static int expired(const dnd_pending_store *store,
    const dnd_pending_slot *slot, uint64_t now_ms) {
    return now_ms - slot->touched_ms >= store->idle_ms;
}

int dnd_pending_init(dnd_pending_store *store, uint64_t idle_ms) {
    if (!store || !idle_ms) return DND_PENDING_INVALID;
    memset(store, 0, sizeof(*store));
    store->idle_ms = idle_ms;
    return DND_PENDING_OK;
}

static int admit(dnd_pending_store *store, dnd_pending_key key,
    const char *request_id, const uint8_t input_sha256[32], uint64_t now_ms,
    dnd_pending_ticket *ticket, int create) {
    size_t selected = DND_PENDING_CAPACITY;
    size_t available = DND_PENDING_CAPACITY;
    dnd_pending_slot *slot;
    if (ticket) memset(ticket, 0, sizeof(*ticket));
    if (!ticket || !input_sha256 || !clock_valid(store, now_ms) ||
        !valid_key(key) || !valid_id(request_id, DND_PENDING_ID_CAP))
        return DND_PENDING_INVALID;
    store->clock_ms = now_ms;
    for (size_t i = 0; i < DND_PENDING_CAPACITY; ++i) {
        dnd_pending_slot *candidate = &store->slots[i];
        if (!candidate->occupied || expired(store, candidate, now_ms)) {
            if (available == DND_PENDING_CAPACITY) available = i;
            continue;
        }
        if (!strcmp(candidate->user_id, key.user_id) &&
            !strcmp(candidate->session_id, key.session_id)) {
            selected = i;
            break;
        }
    }
    if (selected == DND_PENDING_CAPACITY) {
        if (!create) return DND_PENDING_NOT_FOUND;
        if (available == DND_PENDING_CAPACITY) return DND_PENDING_FULL;
        selected = available;
    }
    slot = &store->slots[selected];
    int reuse = !slot->occupied || expired(store, slot, now_ms);
    if (!reuse && !strcmp(slot->request_id, request_id)) {
        if (strcmp(slot->campaign_id, key.campaign_id) ||
            strcmp(slot->scene_id, key.scene_id) ||
            memcmp(slot->input_sha256, input_sha256, sizeof(slot->input_sha256)))
            return DND_PENDING_CONFLICT;
        store->clock_ms = now_ms;
        /* A replay does not extend the retention deadline. */
        if (!slot->active) return DND_PENDING_CLOSED;
        ticket->slot = selected;
        ticket->generation = slot->generation;
        return DND_PENDING_RETRY;
    }
    if (store->generation == UINT64_MAX) return DND_PENDING_FULL;
    if (reuse) {
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->user_id, key.user_id, strlen(key.user_id) + 1u);
        memcpy(slot->session_id, key.session_id, strlen(key.session_id) + 1u);
    } else if (strcmp(slot->campaign_id, key.campaign_id)) {
        memset(&slot->view, 0, sizeof(slot->view));
        slot->scene_revision = 0;
    } else if (strcmp(slot->scene_id, key.scene_id)) {
        if (slot->view.held) slot->view.scene_changed = 1;
        slot->scene_revision = 0;
    }
    memcpy(slot->campaign_id, key.campaign_id, strlen(key.campaign_id) + 1u);
    memcpy(slot->scene_id, key.scene_id, strlen(key.scene_id) + 1u);
    memcpy(slot->request_id, request_id, strlen(request_id) + 1u);
    memcpy(slot->input_sha256, input_sha256, sizeof(slot->input_sha256));
    slot->generation = ++store->generation;
    slot->touched_ms = now_ms;
    slot->occupied = slot->active = 1;
    slot->proposal_update = 0;
    store->clock_ms = now_ms;
    ticket->slot = selected;
    ticket->generation = slot->generation;
    return DND_PENDING_OK;
}

int dnd_pending_begin(dnd_pending_store *store, dnd_pending_key key,
    const char *request_id, const uint8_t input_sha256[32], uint64_t now_ms,
    dnd_pending_ticket *ticket) {
    return admit(store, key, request_id, input_sha256, now_ms, ticket, 1);
}

int dnd_pending_resume(dnd_pending_store *store, dnd_pending_key key,
    const char *request_id, const uint8_t input_sha256[32], uint64_t now_ms,
    dnd_pending_ticket *ticket) {
    return admit(store, key, request_id, input_sha256, now_ms, ticket, 0);
}

int dnd_pending_latest_request(dnd_pending_store *store, const char *user_id,
    const char *session_id, uint64_t now_ms, char request_id[DND_PENDING_ID_CAP]) {
    if (!request_id) return DND_PENDING_INVALID;
    request_id[0] = '\0';
    if (!clock_valid(store, now_ms) || !valid_id(user_id, DND_PENDING_ID_CAP) ||
        !valid_id(session_id, DND_PENDING_ID_CAP)) return DND_PENDING_INVALID;
    store->clock_ms = now_ms;
    for (size_t i = 0; i < DND_PENDING_CAPACITY; ++i) {
        const dnd_pending_slot *slot = &store->slots[i];
        if (slot->occupied && !expired(store, slot, now_ms) &&
            !strcmp(slot->user_id, user_id) && !strcmp(slot->session_id, session_id)) {
            memcpy(request_id, slot->request_id, strlen(slot->request_id) + 1u);
            return DND_PENDING_OK;
        }
    }
    return DND_PENDING_NOT_FOUND;
}

static dnd_pending_slot *current(dnd_pending_store *store,
    dnd_pending_ticket ticket, uint64_t now_ms) {
    if (!clock_valid(store, now_ms)) return NULL;
    store->clock_ms = now_ms;
    if (ticket.slot >= DND_PENDING_CAPACITY || !ticket.generation) return NULL;
    dnd_pending_slot *slot = &store->slots[ticket.slot];
    if (!slot->occupied || !slot->active || slot->generation != ticket.generation ||
        expired(store, slot, now_ms)) return NULL;
    store->clock_ms = now_ms;
    return slot;
}

int dnd_pending_read(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, dnd_pending_view *view) {
    if (!view) return DND_PENDING_INVALID;
    memset(view, 0, sizeof(*view));
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    *view = slot->view;
    return DND_PENDING_OK;
}

int dnd_pending_hold(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, const char *spell) {
    size_t n = bounded_length(spell, DND_PENDING_LABEL_CAP);
    if (!n || n == DND_PENDING_LABEL_CAP || spell[0] == ' ' || spell[n - 1u] == ' ' ||
        !utf8_validate_v1((const uint8_t *)spell, n)) return DND_PENDING_INVALID;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)spell[i];
        if (c < 32u || c == 127u) return DND_PENDING_INVALID;
    }
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    if (slot->proposal_update == 2) return DND_PENDING_CONFLICT;
    if (slot->proposal_update == 1)
        return !strcmp(slot->view.spell, spell) ? DND_PENDING_RETRY : DND_PENDING_CONFLICT;
    memset(&slot->view, 0, sizeof(slot->view));
    memcpy(slot->view.spell, spell, n + 1u);
    memcpy(slot->view.proposal_request_id, slot->request_id,
        strlen(slot->request_id) + 1u);
    slot->view.held = 1;
    slot->proposal_update = 1;
    return DND_PENDING_OK;
}

int dnd_pending_cancel(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms) {
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    memset(&slot->view, 0, sizeof(slot->view));
    slot->proposal_update = 2;
    return DND_PENDING_OK;
}

int dnd_pending_finish(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms) {
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    slot->active = 0;
    return DND_PENDING_OK;
}

int dnd_pending_observe_scene(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, uint64_t scene_revision) {
    if (!scene_revision) return DND_PENDING_INVALID;
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    if (scene_revision < slot->scene_revision) return DND_PENDING_STALE;
    if (scene_revision == slot->scene_revision) return DND_PENDING_RETRY;
    if (slot->scene_revision && slot->view.held) slot->view.scene_changed = 1;
    slot->scene_revision = scene_revision;
    return DND_PENDING_OK;
}

int dnd_pending_focus(dnd_pending_store *store, dnd_pending_ticket ticket,
    uint64_t now_ms, const char *character_name) {
    size_t n = bounded_length(character_name, DND_PENDING_LABEL_CAP);
    if (!n || n == DND_PENDING_LABEL_CAP ||
        !utf8_validate_v1((const uint8_t *)character_name, n)) return DND_PENDING_INVALID;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)character_name[i] < 32u || (unsigned char)character_name[i] == 127u)
            return DND_PENDING_INVALID;
    dnd_pending_slot *slot = current(store, ticket, now_ms);
    if (!slot) return DND_PENDING_STALE;
    memcpy(slot->view.question_character, character_name, n + 1u);
    return DND_PENDING_OK;
}

int dnd_pending_interrupt(dnd_pending_store *store, const char *user_id,
    const char *request_id, uint64_t now_ms) {
    if (!clock_valid(store, now_ms) || !valid_id(user_id, DND_PENDING_ID_CAP) ||
        !valid_id(request_id, DND_PENDING_ID_CAP)) return DND_PENDING_INVALID;
    store->clock_ms = now_ms;
    for (size_t i = 0; i < DND_PENDING_CAPACITY; ++i) {
        dnd_pending_slot *slot = &store->slots[i];
        if (slot->occupied && slot->active && !expired(store, slot, now_ms) &&
            !strcmp(slot->user_id, user_id) && !strcmp(slot->request_id, request_id)) {
            slot->active = 0;
            return DND_PENDING_OK;
        }
    }
    return DND_PENDING_NOT_FOUND;
}
