/* Local engine fixture behind the real product identity and C WebTransport edge. */
#define _POSIX_C_SOURCE 200809L
#include "vbus.h"
#include "pb_min.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    turn_start_c turn;
    size_t packets;
    int peak;
    int endpoint;
    int committed;
    int canceled;
    int pending;
    uint64_t due_ms;
} fixture_turn;

typedef struct {
    vbus_client *publisher;
    fixture_turn turns[32];
    size_t count;
    int failed;
} fixture_state;

static uint64_t now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) abort();
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void emit(fixture_state *state, const fixture_turn *turn, const char *type) {
    uint8_t wire[512];
    const size_t length = pb_encode_turn_event(wire, sizeof(wire), turn->turn.request_id, type, "");
    if (!length || vbus_publish(state->publisher, turn->turn.response_subject, wire, length) != 0) state->failed = 1;
}

static void pcm(fixture_state *state, const fixture_turn *turn, int32_t first, int32_t last) {
    uint8_t wire[2048];
    uint8_t audio[960];
    for (size_t i = 0u; i < sizeof(audio); i += 2u) { audio[i] = 0u; audio[i + 1u] = 32u; }
    for (int32_t sequence = first; sequence < last; sequence++) {
        const size_t length = pb_encode_turn_audio_event(wire, sizeof(wire), turn->turn.request_id,
            "pcm_chunk", "", audio, sizeof(audio), 24000, 1, 16, sequence, 0, sequence == 99);
        if (!length || vbus_publish(state->publisher, turn->turn.response_subject, wire, length) != 0) state->failed = 1;
    }
}

static void reply(fixture_state *state, fixture_turn *turn) {
    uint8_t wire[DND_TURN_START_WIRE_MAX];
    turn_start_c admitted = turn->turn;
    const char *text = strcmp(turn->turn.metadata.campaign_id, "campaign-cancel") == 0 ?
        "Canceled answer must stay hidden." : "Rain silvered the road beyond camp.";
    size_t length;
    strcpy(admitted.text, "I cast fireball, no wait is Mira still in the room?");
    length = pb_encode_turn_start(wire, sizeof(wire), &admitted);
    if (!length || vbus_publish(state->publisher, "ai.turn.start", wire, length) != 0) state->failed = 1;
    emit(state, turn, "started");
    emit(state, turn, "thinking_started");
    length = pb_encode_turn_text_event(wire, sizeof(wire), turn->turn.request_id,
        "text_completed", text, text, text, 0, 1);
    if (!length || vbus_publish(state->publisher, turn->turn.response_subject, wire, length) != 0) state->failed = 1;
    emit(state, turn, "pcm_started");
    if (strcmp(turn->turn.metadata.campaign_id, "campaign-fail") == 0) {
        pcm(state, turn, 0, 75);
        emit(state, turn, "failed");
        return;
    }
    if (strcmp(turn->turn.metadata.campaign_id, "campaign-cancel") == 0) {
        pcm(state, turn, 0, 75);
        turn->pending = 1;
        turn->due_ms = now_ms() + 3000u;
        return;
    }
    pcm(state, turn, 0, 100);
    emit(state, turn, "pcm_ended");
    emit(state, turn, "completed");
}

static void on_prepare(const char *subject, const char *reply_subject,
                       const uint8_t *data, size_t length, void *user) {
    fixture_state *state = user;
    (void)subject; (void)reply_subject;
    if (state->count >= sizeof(state->turns) / sizeof(state->turns[0])) { state->failed = 1; return; }
    fixture_turn *turn = &state->turns[state->count++];
    if (pb_decode_turn_start(data, length, &turn->turn) != 0 || turn->turn.text[0] ||
        strncmp(turn->turn.user_id, "guest_", 6u) != 0 || turn->turn.premium || turn->turn.enable_rag ||
        !turn->turn.enable_tts || !turn->turn.metadata.product_session_id[0] ||
        strcmp(turn->turn.metadata.client_transport, "webtransport-turn-stream") != 0) {
        fputs("FAIL prepare contract\n", stderr);
        state->failed = 1; return;
    }
    printf("{\"type\":\"prepare\",\"request_id\":\"%s\",\"session_id\":\"%s\",\"owner\":\"%s\",\"campaign\":\"%s\",\"profile\":\"%s\",\"product_session_id\":\"%s\",\"premium\":false,\"rag\":false}\n",
        turn->turn.request_id, turn->turn.session_id, turn->turn.user_id, turn->turn.metadata.campaign_id,
        turn->turn.metadata.interaction_profile, turn->turn.metadata.product_session_id);
}

static void on_pcm(const char *subject, const char *reply_subject,
                   const uint8_t *data, size_t length, void *user) {
    fixture_state *state = user;
    (void)reply_subject;
    /* The gateway admits a new request after the prior conversation turn ends. */
    for (size_t i = state->count; i > 0u; i--) {
        fixture_turn *turn = &state->turns[i - 1u];
        char expected[256];
        snprintf(expected, sizeof(expected), "ai.voice.pcm.%s", turn->turn.session_id);
        if (strcmp(subject, expected) != 0) continue;
        if (turn->committed || turn->canceled || (length && length != 640u)) { state->failed = 1; return; }
        if (length) {
            turn->packets++;
            for (size_t j = 0u; j < length; j += 2u) {
                int value = (int)data[j] | ((int)data[j + 1u] << 8u);
                if (value >= 32768) value -= 65536;
                if (value < 0) value = -value;
                if (value > turn->peak) turn->peak = value;
            }
            if (turn->packets >= 20u && turn->peak > 32 && !turn->endpoint &&
                strcmp(turn->turn.metadata.campaign_id, "campaign-manual") != 0) {
                uint8_t wire[512];
                snprintf(expected, sizeof(expected), "ai.voice.reflex.%s", turn->turn.session_id);
                const size_t size = pb_encode_turn_event(wire, sizeof(wire), turn->turn.request_id, "endpoint", "");
                if (!size || vbus_publish(state->publisher, expected, wire, size) != 0) state->failed = 1;
                turn->endpoint = 1;
            }
            return;
        }
        if (!turn->packets || turn->peak <= 32) { state->failed = 1; return; }
        turn->committed = 1;
        printf("{\"type\":\"pcm_commit\",\"request_id\":\"%s\",\"packets\":%zu,\"bytes\":%zu,\"peak\":%d,\"server_endpoint\":%s}\n",
            turn->turn.request_id, turn->packets, turn->packets * 640u, turn->peak, turn->endpoint ? "true" : "false");
        reply(state, turn);
        return;
    }
    state->failed = 1;
}

static void on_cancel(const char *subject, const char *reply_subject,
                      const uint8_t *data, size_t length, void *user) {
    fixture_state *state = user;
    turn_cancel_c cancel;
    (void)subject; (void)reply_subject;
    if (pb_decode_turn_cancel(data, length, &cancel) != 0) { state->failed = 1; return; }
    for (size_t i = 0u; i < state->count; i++) {
        fixture_turn *turn = &state->turns[i];
        if (strcmp(cancel.request_id, turn->turn.request_id) != 0) continue;
        if (strcmp(cancel.user_id, turn->turn.user_id) != 0) { state->failed = 1; return; }
        turn->canceled = 1;
        turn->pending = 0;
        printf("{\"type\":\"cancel\",\"request_id\":\"%s\",\"reason\":\"%s\"}\n", cancel.request_id, cancel.reason);
        return;
    }
    state->failed = 1;
}

int main(void) {
    static fixture_state state;
    vbus_client *subscriber = vbus_connect(vbus_default_path());
    state.publisher = vbus_connect(vbus_default_path());
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!subscriber || !state.publisher ||
        vbus_subscribe(subscriber, "ai.voice.turn.prepare", NULL, on_prepare, &state) != 0 ||
        vbus_subscribe(subscriber, "ai.voice.pcm.>", NULL, on_pcm, &state) != 0 ||
        vbus_subscribe(subscriber, "ai.turn.cancel", NULL, on_cancel, &state) != 0) return 1;
    puts("READY C product microphone peer");
    while (!state.failed) {
        if (vbus_poll(subscriber, 20) != 0) break;
        for (size_t i = 0u; i < state.count; i++) {
            fixture_turn *turn = &state.turns[i];
            if (turn->pending && turn->due_ms <= now_ms()) {
                turn->pending = 0;
                pcm(&state, turn, 75, 100);
                emit(&state, turn, "pcm_ended");
                emit(&state, turn, "completed");
            }
        }
    }
    vbus_close(subscriber);
    vbus_close(state.publisher);
    fputs("FAIL C product microphone peer\n", stderr);
    return 1;
}
