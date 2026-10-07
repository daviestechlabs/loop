/* Local WebTransport oracle peer for authenticated VBus event delivery. */

#define _POSIX_C_SOURCE 200809L

#include "vbus.h"
#include "voice_auth.h"
#include "gateway_webtransport_core.h"
#include "pb_min.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ORACLE_WAIT_POLLS 200u
#define ORACLE_WAIT_MS 50
typedef struct {
    vbus_client *publisher;
    const char *request_ids[5];
    size_t request_count;
    size_t completed_count;
    size_t first_slot;
    char endpoint_response_subject[256];
    turn_start_c endpoint_turn;
    uint32_t endpoint_pcm_datagrams;
    size_t endpoint_pcm_bytes;
    int first_slot_set;
    int endpoint_feedback_published;
    int endpoint_end_seen;
    int allow_legacy_subject;
    unsigned response_delay_ms;
    int response_delay_used;
    int failed;
} oracle_state;

static void delay_first_response(oracle_state *state) {
    struct timespec delay;
    if (!state || state->response_delay_used || state->response_delay_ms == 0u) return;
    state->response_delay_used = 1;
    delay.tv_sec = (time_t)(state->response_delay_ms / 1000u);
    delay.tv_nsec = (long)(state->response_delay_ms % 1000u) * 1000000L;
    (void)nanosleep(&delay, NULL);
}

static int publish_event(vbus_client *publisher, const char *subject,
                         const char *request_id, const char *type) {
    uint8_t wire[512];
    size_t wire_len = pb_encode_turn_event(
        wire, sizeof(wire), request_id, type, "");
    return wire_len == 0u ? -1 :
        vbus_publish(publisher, subject, wire, wire_len);
}

static int publish_text(vbus_client *publisher, const char *subject,
                        const char *request_id, const char *type,
                        const char *text, int is_final) {
    uint8_t wire[512];
    size_t wire_len = pb_encode_turn_text_event(
        wire, sizeof(wire), request_id, type, text, text, text, 0, is_final);
    return wire_len == 0u ? -1 :
        vbus_publish(publisher, subject, wire, wire_len);
}

static int publish_endpoint_transcript(oracle_state *state) {
    turn_start_c turn = state->endpoint_turn;
    uint8_t wire[DND_TURN_START_WIRE_MAX];
    size_t len;
    unsigned trial;
    /* A valid capability alone must not admit a foreign request, session, or owner. */
    for (trial = 0; trial < 5u; ++trial) {
        turn = state->endpoint_turn;
        strcpy(turn.text, "Lynn says \"bonjour\". Before you cast the spell…");
        if (trial == 0u) strcpy(turn.request_id, "foreign-request");
        if (trial == 1u) strcpy(turn.session_id, "foreign-session");
        if (trial == 2u) strcpy(turn.user_id, "foreign-user");
        len = pb_encode_turn_start(wire, sizeof(wire), &turn);
        if (!len || vbus_publish(state->publisher, "ai.turn.start", wire, len) != 0) return -1;
    }
    return 0;
}

static void on_pcm(const char *subject, const char *reply,
                   const uint8_t *data, size_t data_len, void *user) {
    oracle_state *state = (oracle_state *)user;
    static const char endpoint_subject[] = "ai.voice.reflex.wt-oracle-session";
    static const char wrong_subject[] = "ai.voice.reflex.wt-oracle-session.other";
    static const uint8_t malformed[] = {0xffu, 0x00u, 0x01u};
    (void)reply;
    if (!state || state->failed || state->request_count != 5u || !subject ||
        strcmp(subject, "ai.voice.pcm.wt-oracle-session") != 0) {
        if (state) state->failed = 1;
        return;
    }
    if (data_len == 0u) {
        if (state->endpoint_end_seen) { state->failed = 1; return; }
        state->endpoint_end_seen = 1;
        if (!state->endpoint_feedback_published ||
            state->endpoint_pcm_datagrams != 2u || state->endpoint_pcm_bytes != 1280u ||
            !state->endpoint_response_subject[0] ||
            publish_endpoint_transcript(state) != 0 ||
            publish_event(
                state->publisher, state->endpoint_response_subject,
                state->request_ids[1], "started") != 0 ||
            publish_text(
                state->publisher, state->endpoint_response_subject,
                state->request_ids[1], "text_completed",
                "endpoint transcript", 1) != 0 ||
            publish_event(
                state->publisher, state->endpoint_response_subject,
                state->request_ids[1], "completed") != 0) {
            state->failed = 1;
            return;
        }
        return;
    }
    if (!data || state->endpoint_end_seen || state->endpoint_feedback_published ||
        data_len != 640u) {
        state->failed = 1;
        return;
    }
    state->endpoint_pcm_datagrams++;
    state->endpoint_pcm_bytes += data_len;
    if (state->endpoint_pcm_datagrams != 2u) return;
    if (publish_event(
            state->publisher, wrong_subject,
            state->request_ids[1], "endpoint") != 0 ||
        publish_event(
            state->publisher, endpoint_subject,
            state->request_ids[0], "endpoint") != 0 ||
        publish_event(
            state->publisher, endpoint_subject,
            state->request_ids[1], "interrupt") != 0 ||
        vbus_publish(
            state->publisher, endpoint_subject,
            malformed, sizeof(malformed)) != 0 ||
        publish_event(
            state->publisher, endpoint_subject,
            state->request_ids[1], "endpoint") != 0 ||
        publish_event(
            state->publisher, endpoint_subject,
            state->request_ids[1], "endpoint") != 0) {
        state->failed = 1;
        return;
    }
    state->endpoint_feedback_published = 1;
}

static void on_turn_start(const char *subject, const char *reply,
                          const uint8_t *data, size_t data_len, void *user) {
    oracle_state *state = (oracle_state *)user;
    turn_start_c turn;
    char forged_subject[256];
    char malformed_subject[256];
    size_t prefix_len = sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u;
    size_t subject_len;
    size_t response_slot;
    const char *expected_request_id;
    const char *expected_subject;
    int audio_prepare;
    int endpoint_audio_prepare;
    int slotted_subject;
    int scoped;
    int expected_premium;
    (void)reply;
    if (!state || state->completed_count >= state->request_count || state->failed || !subject ||
        pb_decode_turn_start(data, data_len, &turn) != 0) {
        if (state) state->failed = 1;
        return;
    }
    if (state->endpoint_end_seen && strcmp(subject, "ai.turn.start") == 0 &&
        strcmp(turn.response_subject, state->endpoint_response_subject) == 0) return;
    expected_request_id = state->request_ids[state->completed_count];
    endpoint_audio_prepare = state->request_count == 5u && state->completed_count == 1u;
    audio_prepare = endpoint_audio_prepare ||
        (state->request_count == 5u && state->completed_count == 2u) ||
        (state->request_count >= 3u && state->request_count < 5u &&
         state->completed_count == 1u);
    expected_subject = audio_prepare ? "ai.voice.turn.prepare" : "ai.turn.start";
    scoped = state->request_count == 5u && state->completed_count != 3u;
    expected_premium = !(state->request_count == 5u && state->completed_count == 4u);
    if (strcmp(subject, expected_subject) != 0 ||
        strcmp(turn.request_id, expected_request_id) != 0 ||
        strcmp(turn.text, audio_prepare ? "" : "local WebTransport oracle") != 0 ||
        strcmp(turn.session_id, "wt-oracle-session") != 0 ||
        strcmp(turn.user_id, "wt-oracle-user") != 0 || turn.premium != expected_premium ||
        turn.enable_rag != (scoped && expected_premium) || turn.enable_tts != 0) {
        if (state) state->failed = 1;
        return;
    }
    if (scoped &&
        (strcmp(turn.metadata.interaction_profile, "dnd_app") != 0 ||
         strcmp(turn.metadata.voice_mode, "app") != 0 || strcmp(turn.metadata.turn_profile, "dnd_app") != 0 ||
         strcmp(turn.metadata.agent_id, "dnd-agent") != 0 || strcmp(turn.metadata.task_intent, "dnd_action") != 0 ||
         strcmp(turn.metadata.client_transport, "webtransport-turn-stream") != 0 ||
         strcmp(turn.metadata.campaign_id, "campaign-wt") != 0 ||
         strcmp(turn.metadata.character_id, "character-wt") != 0 ||
         strcmp(turn.metadata.encounter_id, "encounter-wt") != 0 ||
         strcmp(turn.metadata.requested_npc_id, "npc-wt") != 0 ||
         strcmp(turn.metadata.knowledge_scope, "shared_rulebook") != 0 ||
         strcmp(turn.metadata.retrieval_force, "true") != 0 ||
         strcmp(turn.metadata.audio_group_session, "true") != 0 ||
         strcmp(turn.metadata.audio_participant_id, "player-wt") != 0 ||
         strcmp(turn.metadata.audio_participant_label, "Mira 🐉") != 0 ||
         !turn.has_meta_budget || strcmp(turn.meta_budget_ms, "45000") != 0 ||
         !turn.has_meta_deadline || strcmp(turn.meta_deadline_unix_ms, "4102444800000") != 0)) {
        state->failed = 1;
        fputs("FAIL WebTransport lost admitted D&D metadata\n", stderr);
        return;
    }
    if (!scoped && (turn.metadata.campaign_id[0] || turn.metadata.character_id[0] ||
        turn.metadata.knowledge_scope[0] || turn.metadata.audio_participant_id[0] ||
        turn.has_meta_budget || turn.has_meta_deadline)) {
        state->failed = 1;
        fputs("FAIL reused WebTransport retained a previous D&D scope\n", stderr);
        return;
    }
    subject_len = strlen(turn.response_subject);
    slotted_subject = subject_len == GW_WT_EVENT_SUBJECT_LENGTH &&
        memcmp(turn.response_subject, GW_WT_EVENT_SUBJECT_PREFIX, prefix_len) == 0 &&
        turn.response_subject[prefix_len + 2u] == '.';
    if (slotted_subject &&
        gw_wt_response_subject_slot(turn.response_subject, &response_slot) != GW_WT_OK) {
        state->failed = 1;
        return;
    }
    if (slotted_subject && !state->first_slot_set) {
        state->first_slot = response_slot;
        state->first_slot_set = 1;
    } else if (slotted_subject && response_slot != state->first_slot) {
        state->failed = 1;
        return;
    }
    if (audio_prepare) {
        if (endpoint_audio_prepare) {
            state->endpoint_turn = turn;
            memcpy(
                state->endpoint_response_subject, turn.response_subject,
                subject_len + 1u);
        }
        state->completed_count++;
        return;
    }
    if (!slotted_subject &&
        (!state->allow_legacy_subject ||
         subject_len != prefix_len + strlen(turn.request_id) + 1u +
             VOICE_AUTH_NONCE_HEX_LEN ||
         memcmp(turn.response_subject, GW_WT_EVENT_SUBJECT_PREFIX, prefix_len) != 0 ||
         memcmp(turn.response_subject + prefix_len, turn.request_id,
                strlen(turn.request_id)) != 0 ||
         turn.response_subject[prefix_len + strlen(turn.request_id)] != '.')) {
        state->failed = 1;
        return;
    }
    memcpy(forged_subject, turn.response_subject, subject_len + 1u);
    forged_subject[subject_len - 1u] =
        forged_subject[subject_len - 1u] == '0' ? '1' : '0';
    memcpy(malformed_subject, turn.response_subject, subject_len + 1u);
    malformed_subject[prefix_len] = slotted_subject ? 'f' : 'x';
    malformed_subject[prefix_len + 1u] = slotted_subject ? 'f' : 'x';
    if (publish_event(state->publisher, forged_subject, turn.request_id, "failed") != 0 ||
        publish_event(state->publisher, malformed_subject, turn.request_id, "failed") != 0) {
        state->failed = 1;
        return;
    }
    malformed_subject[prefix_len] = slotted_subject ? 'g' : 'y';
    malformed_subject[prefix_len + 1u] = slotted_subject ? 'g' : 'y';
    if (publish_event(state->publisher, malformed_subject, turn.request_id, "failed") != 0) {
        state->failed = 1;
        return;
    }
    malformed_subject[subject_len - 1u] = '\0';
    delay_first_response(state);
    if (publish_event(state->publisher, malformed_subject, turn.request_id, "failed") != 0 ||
        publish_event(state->publisher, turn.response_subject, turn.request_id, "started") != 0 ||
        publish_event(state->publisher, turn.response_subject, turn.request_id,
                      "thinking_started") != 0 ||
        publish_event(state->publisher, turn.response_subject, turn.request_id,
                      "thinking_started") != 0 ||
        publish_text(state->publisher, turn.response_subject, turn.request_id,
                     "text_delta", "oracle delta", 0) != 0 ||
        publish_text(state->publisher, turn.response_subject, turn.request_id,
                     "text_completed", "oracle final", 1) != 0 ||
        publish_event(state->publisher, turn.response_subject, turn.request_id,
                      "completed") != 0 ||
        publish_event(state->publisher, turn.response_subject, turn.request_id,
                      "failed") != 0) {
        state->failed = 1;
        return;
    }
    state->completed_count++;
}

static int issue_token(const char *secret, const char *request_id, int premium) {
    char token[VOICE_AUTH_IDENTITY_TOKEN_CAP];
    time_t now = time(NULL);
    if (now <= 0 || voice_auth_identity_issue(
            secret, strlen(secret), request_id, "wt-oracle-user", premium,
            (int64_t)now, 120, token, sizeof(token)) != VOICE_AUTH_OK) return 1;
    if (printf("%s\n", token) < 0) return 1;
    return 0;
}

static int serve(const char *request_id, const char *second_request_id,
                 const char *third_request_id, const char *fourth_request_id,
                 const char *fifth_request_id,
                 int allow_legacy_subject) {
    oracle_state state;
    vbus_client *subscriber = NULL;
    const char *response_delay = getenv("WT_ORACLE_RESPONSE_DELAY_MS");
    unsigned i;
    memset(&state, 0, sizeof(state));
    state.request_ids[0] = request_id;
    state.request_ids[1] = second_request_id;
    state.request_ids[2] = third_request_id;
    state.request_ids[3] = fourth_request_id;
    state.request_ids[4] = fifth_request_id;
    state.request_count = fifth_request_id ? 5u : fourth_request_id ? 4u :
                          third_request_id ? 3u :
                          second_request_id ? 2u : 1u;
    state.allow_legacy_subject = allow_legacy_subject;
    if (response_delay && response_delay[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(response_delay, &end, 10);
        if (!end || *end != '\0' || parsed > 5000u) return 1;
        state.response_delay_ms = (unsigned)parsed;
    }
    state.publisher = vbus_connect(vbus_default_path());
    subscriber = vbus_connect(vbus_default_path());
    if (!state.publisher || !subscriber ||
        vbus_subscribe(subscriber, "ai.turn.start", NULL, on_turn_start, &state) != 0 ||
        vbus_subscribe(subscriber, "ai.voice.turn.prepare", NULL, on_turn_start, &state) != 0 ||
        vbus_subscribe(subscriber, "ai.voice.pcm.>", NULL, on_pcm, &state) != 0) {
        vbus_close(subscriber);
        vbus_close(state.publisher);
        return 1;
    }
    for (i = 0; i < ORACLE_WAIT_POLLS &&
                state.completed_count < state.request_count && !state.failed; ++i) {
        if (vbus_poll(subscriber, ORACLE_WAIT_MS) != 0) state.failed = 1;
    }
    vbus_close(subscriber);
    vbus_close(state.publisher);
    if (state.completed_count != state.request_count || state.failed ||
        (state.request_count == 5u && !state.endpoint_end_seen)) return 1;
    if (state.request_count == 5u)
        puts("PASS native WebTransport D&D metadata, signed owner and premium, free RAG denial, and scope reset");
    if (printf("PASS WebTransport VBus oracle published canonical lifecycle%s\n",
               state.request_count == 5u ?
                   ", stopped PCM at a request-bound endpoint, drained its tail, reused one transport, exercised datagram conflict, and reallocated its routing slot" :
               state.request_count == 4u ?
                   ", reused one transport, exercised datagram conflict, and reallocated its routing slot" :
               state.request_count == 3u ?
                   ", exercised datagram conflict, and reused routing slot" :
               state.request_count == 2u ? " and reused routing slot" : "") < 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "issue") == 0)
        return issue_token(argv[2], argv[3], 1);
    if (argc == 4 && strcmp(argv[1], "issue-free") == 0)
        return issue_token(argv[2], argv[3], 0);
    if (argc >= 3 && argc <= 7 && strcmp(argv[1], "serve") == 0)
        return serve(argv[2], argc >= 4 ? argv[3] : NULL,
                     argc >= 5 ? argv[4] : NULL,
                     argc >= 6 ? argv[5] : NULL,
                     argc == 7 ? argv[6] : NULL, 0);
    if (argc >= 3 && argc <= 7 && strcmp(argv[1], "serve-legacy") == 0)
        return serve(argv[2], argc >= 4 ? argv[3] : NULL,
                     argc >= 5 ? argv[4] : NULL,
                     argc >= 6 ? argv[5] : NULL,
                     argc == 7 ? argv[6] : NULL, 1);
    fprintf(stderr,
            "usage: %s issue SECRET REQUEST_ID | "
            "serve REQUEST_ID [REQUEST_ID [REQUEST_ID [REQUEST_ID [REQUEST_ID]]]] | "
            "serve-legacy REQUEST_ID [REQUEST_ID [REQUEST_ID [REQUEST_ID [REQUEST_ID]]]]\n",
            argv[0]);
    return 2;
}
