#include "dnd_pending.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dnd_pending_store *store = calloc(1, sizeof(*store));
    dnd_pending_ticket tickets[8] = {{0}};
    uint64_t now = 1;
    if (!store) return 0;
    if (dnd_pending_init(store, 32)) abort();
    for (size_t i = 0; i + 5u < size; i += 6u) {
        size_t which = data[i + 1u] % 8u;
        dnd_pending_ticket ticket = tickets[which];
        dnd_pending_view view;
        dnd_pending_key key;
        char user[32], session[32], request[32], scene[32], campaign[32];
        uint8_t digest[32] = {0};
        (void)snprintf(user, sizeof(user), "user-%u", (unsigned)data[i + 2u] % 3u);
        (void)snprintf(session, sizeof(session), "session-%u", (unsigned)data[i + 3u] % 5u);
        (void)snprintf(request, sizeof(request), "request-%u", (unsigned)data[i + 4u]);
        (void)snprintf(scene, sizeof(scene), "scene-%u", (unsigned)data[i + 5u] % 3u);
        (void)snprintf(campaign, sizeof(campaign), "campaign-%u", (unsigned)data[i + 2u] % 2u);
        key = (dnd_pending_key){user, session, campaign, scene};
        digest[0] = data[i + 5u];
        now += data[i + 5u] % 3u;
        dnd_pending_slot before = store->slots[ticket.slot < DND_PENDING_CAPACITY ? ticket.slot : 0];
        int rc = DND_PENDING_OK;
        switch (data[i] % 11u) {
        case 0:
            (void)dnd_pending_begin(store, key, request, digest, now, &tickets[which]);
            continue;
        case 1:
            rc = dnd_pending_hold(store, ticket, now, "Fireball");
            break;
        case 2:
            rc = dnd_pending_cancel(store, ticket, now);
            if (rc == 0 && dnd_pending_hold(store, ticket, now, "Fireball") != DND_PENDING_CONFLICT)
                abort();
            break;
        case 3:
            rc = dnd_pending_finish(store, ticket, now);
            if (rc == 0 && dnd_pending_hold(store, ticket, now, "Fireball") != DND_PENDING_STALE)
                abort();
            break;
        case 4:
            rc = dnd_pending_read(store, ticket, now, &view);
            if (rc != 0 && (view.held || view.spell[0] || view.proposal_request_id[0])) abort();
            break;
        case 5:
            rc = dnd_pending_observe_scene(store, ticket, now, data[i + 4u]);
            break;
        case 6:
            (void)dnd_pending_resume(store, key, request, digest, now, &tickets[which]);
            continue;
        case 7:
            rc = dnd_pending_focus(store, ticket, now, "Mira");
            break;
        case 8:
            (void)dnd_pending_interrupt(store, user, request, now);
            continue;
        case 9: {
            char latest[DND_PENDING_ID_CAP];
            rc = dnd_pending_latest_request(store, user, session, now, latest);
            if (rc == 0 && !latest[0]) abort();
            if (rc != 0 && latest[0]) abort();
            break;
        }
        default:
            ticket.generation ^= UINT64_C(0x100000000);
            rc = dnd_pending_hold(store, ticket, now, "Fireball");
            if (rc != DND_PENDING_STALE) abort();
            break;
        }
        if (rc < 0 && ticket.slot < DND_PENDING_CAPACITY &&
            memcmp(&before, &store->slots[ticket.slot], sizeof(before))) abort();
    }
    free(store);
    return 0;
}
