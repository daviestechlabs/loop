/*
 * Pure-C voice-session-gateway control plane.
 * Full WebTransport is out of pure-C PoC scope without a QUIC stack; this
 * process owns VBus prepare/forward subjects, reflex PCM path, and the
 * authenticated HTTP fallback edge.
 *
 * Subscribes: ai.voice.turn.prepare, ai.voice.pcm.>, ai.voice.transcription.>, ai.turn.cancel
 * Publishes:  ai.turn.start, ai.voice.stream.{session}, ai.voice.reflex.{session}
 * Listens:    GATEWAY_HTTP_PORT (default 8080) for health and turn routes
 */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../common/vbus_subject.h"
#include "../common/voice_ascii.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"
#include "gateway_http.h"

#include "reflex_session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GW_MAX_SESSIONS 64
#define GW_STREAM_AUDIO_MAX 8192u
#define GW_DEFAULT_PCM_IDLE_TIMEOUT_MS 30000
#define GW_MAX_PCM_IDLE_TIMEOUT_MS 300000

typedef enum {
    GW_PCM_RECEIVING = 0,
    GW_PCM_WAITING_TRANSCRIPT,
    GW_PCM_WAITING_TRANSCRIPT_AFTER_END,
    GW_PCM_DRAINING
} gw_pcm_phase;

typedef struct {
    char session_id[128];
    char request_id[128];
    reflex_session_v1 *reflex;
    turn_start_c pending_turn;
    uint64_t expires_ns;
    gw_pcm_phase phase;
    int has_pending_turn;
    int stream_started;
    int in_use;
} gw_pcm_session;

typedef struct {
    vbus_client *nc;
    gw_pcm_session sessions[GW_MAX_SESSIONS];
    uint64_t pcm_idle_ns;
    uint64_t next_pcm_expiry_ns;
} gw_state;

typedef struct {
    int port;
    vbus_stop_flag *stop;
    int result;
} gateway_http_args;

static void *gateway_http_thread(void *user) {
    gateway_http_args *args = (gateway_http_args *)user;
    args->result = gateway_http_run(args->port, args->stop);
    if (args->result != 0)
        atomic_store_explicit(args->stop, 1, memory_order_relaxed);
    return NULL;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int64_t realtime_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0 ||
        (uint64_t)ts.tv_sec > (uint64_t)INT64_MAX / UINT64_C(1000)) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000) +
        (int64_t)ts.tv_nsec / INT64_C(1000000);
}

static void pcm_drop(gw_state *st, const char *sid);
static void cancel_pcm_stream(
    gw_state *st,
    gw_pcm_session *session,
    const char *sid,
    const char *detail
);
static void publish_turn_failure(gw_state *st, const turn_start_c *turn);

static uint64_t pcm_deadline(const gw_state *st, uint64_t now_ns) {
    if (!st || now_ns == 0 || st->pcm_idle_ns == 0) return 0;
    if (now_ns > UINT64_MAX - st->pcm_idle_ns) return UINT64_MAX;
    return now_ns + st->pcm_idle_ns;
}

static void pcm_touch(gw_state *st, gw_pcm_session *session, uint64_t now_ns) {
    uint64_t deadline;
    if (!st || !session) return;
    deadline = pcm_deadline(st, now_ns);
    session->expires_ns = deadline;
    if (deadline != 0u &&
        (st->next_pcm_expiry_ns == 0u || deadline < st->next_pcm_expiry_ns))
        st->next_pcm_expiry_ns = deadline;
}

static void pcm_expire(gw_state *st, uint64_t now_ns) {
    int i;
    if (!st || now_ns == 0u || st->next_pcm_expiry_ns == 0u ||
        st->next_pcm_expiry_ns > now_ns)
        return;
    st->next_pcm_expiry_ns = 0u;
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        gw_pcm_session *session = &st->sessions[i];
        char sid[sizeof(session->session_id)];
        if (!session->in_use || session->expires_ns == 0u) continue;
        if (session->expires_ns > now_ns) {
            if (st->next_pcm_expiry_ns == 0u ||
                session->expires_ns < st->next_pcm_expiry_ns)
                st->next_pcm_expiry_ns = session->expires_ns;
            continue;
        }
        memcpy(sid, session->session_id, sizeof(sid));
        svc_log("voice-session-gateway", "expire idle PCM session=%s", sid);
        if (session->phase == GW_PCM_DRAINING)
            pcm_drop(st, sid);
        else
            cancel_pcm_stream(st, session, sid, "stream_timeout");
    }
}

static gw_pcm_session *pcm_find(gw_state *st, const char *sid) {
    int i;
    uint64_t now_ns = mono_ns();
    pcm_expire(st, now_ns);
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        if (st->sessions[i].in_use &&
            strcmp(st->sessions[i].session_id, sid) == 0)
            return &st->sessions[i];
    }
    return NULL;
}

static gw_pcm_session *pcm_receive(gw_state *st, const char *sid) {
    gw_pcm_session *session;
    int i;
    int free_i = -1;
    uint64_t now_ns;
    session = pcm_find(st, sid);
    now_ns = mono_ns();
    if (session) {
        if (session->phase != GW_PCM_RECEIVING || !session->reflex) return NULL;
        pcm_touch(st, session, now_ns);
        return session;
    }
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        if (!st->sessions[i].in_use) {
            free_i = i;
            break;
        }
    }
    if (free_i < 0) return NULL;
    {
        reflex_config_v1 cfg;
        reflex_session_v1 *rs = NULL;
        reflex_config_default_v1(&cfg);
        cfg.flags &= ~(uint32_t)REFLEX_CONFIG_RETAIN_UTTERANCE;
        if (reflex_session_init_v1(&cfg, &rs) != REFLEX_OK || !rs) return NULL;
        memset(&st->sessions[free_i], 0, sizeof(st->sessions[free_i]));
        snprintf(st->sessions[free_i].session_id, sizeof(st->sessions[free_i].session_id), "%s", sid);
        st->sessions[free_i].reflex = rs;
        st->sessions[free_i].phase = GW_PCM_RECEIVING;
        st->sessions[free_i].in_use = 1;
        pcm_touch(st, &st->sessions[free_i], now_ns);
        return &st->sessions[free_i];
    }
}

static void pcm_finish_reflex(
    gw_state *st,
    gw_pcm_session *session,
    int input_ended
) {
    if (!st || !session) return;
    if (session->reflex) reflex_session_destroy_v1(session->reflex);
    session->reflex = NULL;
    session->phase = input_ended
        ? GW_PCM_WAITING_TRANSCRIPT_AFTER_END
        : GW_PCM_WAITING_TRANSCRIPT;
    pcm_touch(st, session, mono_ns());
}

static void pcm_drop(gw_state *st, const char *sid) {
    int i;
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        if (st->sessions[i].in_use && strcmp(st->sessions[i].session_id, sid) == 0) {
            if (st->sessions[i].reflex) reflex_session_destroy_v1(st->sessions[i].reflex);
            memset(&st->sessions[i], 0, sizeof(st->sessions[i]));
            return;
        }
    }
}

static void pcm_drop_request(gw_state *st, const char *request_id) {
    int i;
    if (!request_id || !request_id[0]) return;
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        if (st->sessions[i].in_use &&
            (strcmp(st->sessions[i].request_id, request_id) == 0 ||
             strcmp(st->sessions[i].session_id, request_id) == 0)) {
            pcm_drop(st, st->sessions[i].session_id);
            return;
        }
    }
}

static void pcm_finish_terminal(gw_state *st, gw_pcm_session *session) {
    char sid[sizeof(session->session_id)];
    if (!st || !session) return;
    if (session->phase == GW_PCM_WAITING_TRANSCRIPT_AFTER_END) {
        memcpy(sid, session->session_id, sizeof(sid));
        pcm_drop(st, sid);
        return;
    }
    session->phase = GW_PCM_DRAINING;
    session->request_id[0] = '\0';
    memset(&session->pending_turn, 0, sizeof(session->pending_turn));
    session->has_pending_turn = 0;
    pcm_touch(st, session, mono_ns());
}

static void publish_cancel(gw_state *st, const char *request_id, const char *reason) {
    uint8_t buf[512];
    size_t n = pb_encode_turn_cancel(
        buf, sizeof(buf), request_id, "", reason ? reason : "barge_in");
    if (n == 0 || vbus_publish(st->nc, SUBJ_TURN_CANCEL, buf, n) != 0)
        svc_log("voice-session-gateway", "cancel publish failed request=%s", request_id);
}

static void publish_reflex_event(
    gw_state *st,
    const char *sid,
    const char *request_id,
    const char *type,
    const char *detail
) {
    uint8_t buf[512];
    char subject[256];
    const char *event_id = request_id && request_id[0] ? request_id : sid;
    size_t n = pb_encode_turn_event(
        buf, sizeof(buf), event_id, type, detail ? detail : "");
    if (n == 0) return;
    snprintf(subject, sizeof(subject), "%s.%s", SUBJ_VOICE_REFLEX_PFX, sid);
    (void)vbus_publish(st->nc, subject, buf, n);
}

/* Publish STTStreamMessage framing onto ai.voice.stream.{sid} for audio-processor. */
static int publish_stream_frame(
    gw_state *st,
    const char *sid,
    const char *type,
    const uint8_t *audio,
    size_t audio_len
) {
    uint8_t wire[GW_STREAM_AUDIO_MAX + 64u];
    char subject[256];
    size_t n;
    int64_t timestamp_ms = 0;
    if ((audio_len != 0 && !audio) || audio_len > GW_STREAM_AUDIO_MAX) {
        svc_log("voice-session-gateway", "reject oversized stream frame bytes=%zu", audio_len);
        return -1;
    }
    if (strcmp(type, "end") == 0) timestamp_ms = realtime_ms();
    n = pb_encode_stt_stream_message_at(
        wire, sizeof(wire), type, audio, audio_len,
        16000, 1, 16, timestamp_ms);
    if (n == 0) return -1;
    if (!strcmp(type, "start")) {
        gw_pcm_session *session = pcm_find(st, sid);
        if (session && session->has_pending_turn &&
            session->pending_turn.user_id[0] &&
            !strcmp(session->pending_turn.metadata.interaction_profile, "realtime_voice") &&
            !strcmp(session->pending_turn.metadata.client_surface, "loop")) {
            n = pb_append_stt_owner(wire, sizeof(wire), n, session->pending_turn.user_id);
            if (!n) return -1;
        }
    }
    snprintf(subject, sizeof(subject), "%s.%s", SUBJ_VOICE_STREAM_PFX, sid);
    return vbus_publish(st->nc, subject, wire, n);
}

static void cancel_pcm_stream(
    gw_state *st,
    gw_pcm_session *session,
    const char *sid,
    const char *detail
) {
    if (session && session->stream_started &&
        publish_stream_frame(st, sid, "cancel", NULL, 0) != 0)
        svc_log("voice-session-gateway", "stream cancel publish failed session=%s", sid);
    publish_reflex_event(
        st, sid, session ? session->request_id : NULL,
        "failed", detail ? detail : "stream_error");
    if (session && session->has_pending_turn)
        publish_turn_failure(st, &session->pending_turn);
    pcm_drop(st, sid);
}

static void on_prepare(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    gw_state *st = (gw_state *)user;
    turn_start_c turn;
    gw_pcm_session *sess;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_start(data, data_len, &turn) != 0 || !turn.request_id[0]) {
        svc_log("voice-session-gateway", "reject malformed turn.prepare");
        return;
    }
    if (turn.session_id[0] && !turn.text[0]) {
        sess = pcm_receive(st, turn.session_id);
        if (!sess) {
            svc_log("voice-session-gateway", "session capacity exhausted");
            publish_turn_failure(st, &turn);
            return;
        }
        memcpy(sess->request_id, turn.request_id, strlen(turn.request_id) + 1);
        sess->pending_turn = turn;
        sess->has_pending_turn = 1;
        pcm_touch(st, sess, mono_ns());
        svc_log("voice-session-gateway", "prepared audio turn request=%s", turn.request_id);
        return;
    }
    if (vbus_publish(st->nc, SUBJ_TURN_START, data, data_len) != 0) {
        svc_log("voice-session-gateway", "turn.start publish failed");
        return;
    }
    svc_log("voice-session-gateway", "prepare -> turn.start (pure-C)");
}

static void on_pcm(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    gw_state *st = (gw_state *)user;
    const char *last;
    const char *sid;
    gw_pcm_session *sess;
    reflex_decision_v1 dec;
    int rc;
    (void)reply;

    last = strrchr(subject, '.');
    if (!last || last[1] == '\0') return;
    sid = last + 1;
    if (strlen(sid) >= sizeof(st->sessions[0].session_id)) {
        svc_log("voice-session-gateway", "reject oversized session id");
        return;
    }

    if (data_len == 0) {
        /* Empty datagram commits the authoritative streaming audio path. */
        sess = pcm_find(st, sid);
        if (!sess) return;
        if (sess->phase == GW_PCM_DRAINING) {
            pcm_drop(st, sid);
            return;
        }
        if (sess->phase == GW_PCM_WAITING_TRANSCRIPT) {
            sess->phase = GW_PCM_WAITING_TRANSCRIPT_AFTER_END;
            pcm_touch(st, sess, mono_ns());
            return;
        }
        if (sess->phase == GW_PCM_WAITING_TRANSCRIPT_AFTER_END) return;
        if (sess->reflex) {
            if (!sess->stream_started) {
                publish_reflex_event(
                    st, sid, sess->request_id, "failed", "empty_stream");
                if (sess->has_pending_turn)
                    publish_turn_failure(st, &sess->pending_turn);
                pcm_drop(st, sid);
                return;
            }
            if (publish_stream_frame(st, sid, "end", NULL, 0) != 0) {
                svc_log("voice-session-gateway", "stream end publish failed session=%s", sid);
                cancel_pcm_stream(st, sess, sid, "stream_publish");
                return;
            }
            publish_reflex_event(
                st, sid, sess->request_id, "endpoint", "client_end");
            pcm_finish_reflex(st, sess, 1);
        }
        return;
    }

    sess = pcm_receive(st, sid);
    if (!sess || !sess->reflex) return;
    memset(&dec, 0, sizeof(dec));
    rc = reflex_session_process_v1(sess->reflex, data, data_len, mono_ns(), 0, &dec);
    if (rc != REFLEX_OK) {
        svc_log("voice-session-gateway", "reflex process rc=%d session=%s", rc, sid);
        if (rc == REFLEX_ERR_CAPACITY &&
            (dec.flags & REFLEX_DECISION_ENDPOINT) != 0u && sess->stream_started) {
            char reason[32];
            snprintf(reason, sizeof(reason), "%u", dec.endpoint_reason);
            if (publish_stream_frame(st, sid, "end", NULL, 0) != 0) {
                cancel_pcm_stream(st, sess, sid, "stream_publish");
                return;
            }
            publish_reflex_event(st, sid, sess->request_id, "endpoint", reason);
            pcm_finish_reflex(st, sess, 0);
            return;
        }
        cancel_pcm_stream(st, sess, sid, "invalid_pcm");
        return;
    }
    if (!sess->stream_started) {
        if (publish_stream_frame(st, sid, "start", NULL, 0) != 0) {
            svc_log("voice-session-gateway", "stream start publish failed session=%s", sid);
            cancel_pcm_stream(st, sess, sid, "stream_publish");
            return;
        }
        sess->stream_started = 1;
    }
    if (publish_stream_frame(st, sid, "chunk", data, data_len) != 0) {
        svc_log("voice-session-gateway", "stream chunk publish failed session=%s", sid);
        cancel_pcm_stream(st, sess, sid, "stream_publish");
        return;
    }
    if (dec.flags & REFLEX_DECISION_INTERRUPT) {
        publish_reflex_event(st, sid, sess->request_id, "interrupt", "");
        publish_cancel(st, sess->request_id[0] ? sess->request_id : sid, "barge_in");
    }
    if (dec.flags & REFLEX_DECISION_ENDPOINT) {
        char reason[32];
        snprintf(reason, sizeof(reason), "%u", dec.endpoint_reason);
        if (publish_stream_frame(st, sid, "end", NULL, 0) != 0) {
            svc_log("voice-session-gateway", "stream end publish failed session=%s", sid);
            cancel_pcm_stream(st, sess, sid, "stream_publish");
            return;
        }
        publish_reflex_event(st, sid, sess->request_id, "endpoint", reason);
        pcm_finish_reflex(st, sess, 0);
    }
}

static int make_audio_request_id(const char *sid, char *out, size_t out_cap) {
    static uint32_t sequence;
    struct timespec ts;
    uint64_t stamp;
    char sid_short[41];
    size_t sid_len;
    int written;
    if (!sid || !sid[0] || !out || out_cap == 0 ||
        clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0)
        return -1;
    stamp = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    sid_len = strlen(sid);
    if (sid_len > sizeof(sid_short) - 1u) sid_len = sizeof(sid_short) - 1u;
    memcpy(sid_short, sid, sid_len);
    sid_short[sid_len] = '\0';
    sequence++;
    written = snprintf(out, out_cap, "auto-%s-%" PRIu64 "-%" PRIu32,
                       sid_short, stamp, sequence);
    return written > 0 && (size_t)written < out_cap ? 0 : -1;
}

static int valid_turn_request_id(const char *request_id) {
    size_t i;
    size_t len;
    if (!request_id || !request_id[0]) return 0;
    len = strlen(request_id);
    if (len >= sizeof(((turn_start_c *)0)->request_id)) return 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)request_id[i];
        if (!(voice_ascii_is_alnum(c) || c == '-' || c == '_' || c == '.' || c == ':'))
            return 0;
    }
    return 1;
}

static void publish_turn_failure(gw_state *st, const turn_start_c *turn) {
    uint8_t wire[512];
    char fallback_subject[256];
    const char *event_subject;
    size_t wire_len;
    int written;
    if (!st || !turn) return;
    event_subject = turn->response_subject;
    if (!vbus_publish_subject_valid(
            event_subject, sizeof(turn->response_subject))) {
        if (!valid_turn_request_id(turn->request_id)) return;
        written = snprintf(
            fallback_subject, sizeof(fallback_subject), "%s.%s",
            SUBJ_TURN_EVENTS_PFX, turn->request_id);
        if (written <= 0 || (size_t)written >= sizeof(fallback_subject)) return;
        event_subject = fallback_subject;
    }
    wire_len = pb_encode_turn_event(
        wire, sizeof(wire), turn->request_id,
        "failed", "speech recognition failed");
    if (wire_len == 0 ||
        vbus_publish(st->nc, event_subject, wire, wire_len) != 0)
        svc_log(
            "voice-session-gateway",
            "audio turn failure publish failed request=%s",
            turn->request_id);
}

static void on_transcription(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    gw_state *st = (gw_state *)user;
    stt_transcription_c transcript;
    stt_lifecycle_c lifecycle;
    const char *last;
    const char *sid;
    gw_pcm_session *session;
    turn_start_c turn;
    uint8_t wire[DND_TURN_START_WIRE_MAX];
    size_t wire_len;
    (void)reply;
    memset(&transcript, 0, sizeof(transcript));
    if (pb_decode_stt_transcription(data, data_len, &transcript) == 0 &&
        transcript.is_final && transcript.commit_for_turn &&
        transcript.transcript[0]) {
        sid = transcript.session_id;
    } else {
        memset(&lifecycle, 0, sizeof(lifecycle));
        if (pb_decode_stt_lifecycle(data, data_len, &lifecycle) != 0 ||
            lifecycle.type_id != STT_LIFECYCLE_TRANSCRIPTION_FAILED)
            return;
        sid = lifecycle.session_id;
        if (!sid[0]) {
            last = strrchr(subject, '.');
            if (!last || !last[1]) return;
            sid = last + 1;
        }
        session = pcm_find(st, sid);
        if (!session) return;
        if (session->phase == GW_PCM_DRAINING) return;
        if (session->phase != GW_PCM_WAITING_TRANSCRIPT &&
            session->phase != GW_PCM_WAITING_TRANSCRIPT_AFTER_END)
            return;
        publish_reflex_event(
            st, sid, session->request_id, "failed", "stt_failed");
        if (session->has_pending_turn)
            publish_turn_failure(st, &session->pending_turn);
        svc_log("voice-session-gateway", "STT failed session=%s", sid);
        pcm_finish_terminal(st, session);
        return;
    }
    if (!sid[0]) {
        last = strrchr(subject, '.');
        if (!last || !last[1]) return;
        sid = last + 1;
    }
    session = pcm_find(st, sid);
    if (session && session->phase != GW_PCM_WAITING_TRANSCRIPT &&
        session->phase != GW_PCM_WAITING_TRANSCRIPT_AFTER_END)
        return;
    memset(&turn, 0, sizeof(turn));
    if (session && session->has_pending_turn) {
        turn = session->pending_turn;
    } else {
        if (strlen(sid) >= sizeof(turn.session_id) ||
            make_audio_request_id(sid, turn.request_id, sizeof(turn.request_id)) != 0)
            return;
        memcpy(turn.session_id, sid, strlen(sid) + 1u);
        turn.enable_tts = 1;
        if (snprintf(turn.response_subject, sizeof(turn.response_subject), "%s.%s",
                     SUBJ_TURN_EVENTS_PFX, turn.request_id) >=
            (int)sizeof(turn.response_subject)) return;
    }
    if (strlen(transcript.transcript) >= sizeof(turn.text)) {
        if (session) {
            if (session->has_pending_turn)
                publish_turn_failure(st, &session->pending_turn);
            pcm_finish_terminal(st, session);
        }
        return;
    }
    memcpy(turn.text, transcript.transcript, strlen(transcript.transcript) + 1u);
    turn.input_stages = transcript.input_stages;
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    if (wire_len == 0 || vbus_publish(st->nc, SUBJ_TURN_START, wire, wire_len) != 0) {
        svc_log("voice-session-gateway", "audio turn publish failed session=%s", sid);
        return;
    }
    svc_log("voice-session-gateway", "audio transcript -> turn.start request=%s", turn.request_id);
    if (session) pcm_finish_terminal(st, session);
}

static void on_cancel(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    gw_state *st = (gw_state *)user;
    turn_cancel_c cancel;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_cancel(data, data_len, &cancel) != 0) return;
    {
        int i;
        for (i = 0; i < GW_MAX_SESSIONS; ++i) {
            gw_pcm_session *sess = &st->sessions[i];
            if (!sess->in_use ||
                (strcmp(sess->request_id, cancel.request_id) != 0 &&
                 strcmp(sess->session_id, cancel.request_id) != 0)) continue;
            if (sess->reflex) {
                reflex_decision_v1 dec;
                reflex_session_cancel_v1(sess->reflex, &dec);
                publish_reflex_event(
                    st, sess->session_id, sess->request_id, "cancelled", "");
            }
            if (sess->stream_started && sess->phase != GW_PCM_DRAINING &&
                publish_stream_frame(st, sess->session_id, "cancel", NULL, 0) != 0)
                svc_log(
                    "voice-session-gateway", "stream cancel publish failed session=%s",
                    sess->session_id);
            pcm_drop_request(st, cancel.request_id);
            break;
        }
    }
}

int main(void) {
    vbus_stop_flag stop = 0;
    gw_state *st;
    gateway_http_args http_args;
    pthread_t http_thread;
    int http_started = 0;
    int port;
    int i;
    int run_rc = 0;
    st = (gw_state *)calloc(1, sizeof(*st));
    if (!st) {
        svc_log("voice-session-gateway", "state allocation failed");
        return 1;
    }
    st->pcm_idle_ns =
        (uint64_t)svc_env_int_range(
            "GATEWAY_PCM_IDLE_TIMEOUT_MS",
            GW_DEFAULT_PCM_IDLE_TIMEOUT_MS,
            1000,
            GW_MAX_PCM_IDLE_TIMEOUT_MS) * UINT64_C(1000000);
    svc_install_signals(&stop);
    st->nc = svc_connect_bus();
    if (!st->nc) {
        free(st);
        return 1;
    }
    port = svc_env_int_range(
        "GATEWAY_HTTP_PORT",
        svc_env_int_range("GATEWAY_HEALTH_PORT", 8080, 1, 65535),
        1,
        65535);
    if (vbus_subscribe(st->nc, SUBJ_VOICE_TURN_PREPARE, NULL, on_prepare, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_VOICE_PCM_ALL, NULL, on_pcm, st) != 0 ||
        vbus_subscribe(st->nc, "ai.voice.transcription.>", NULL, on_transcription, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_TURN_CANCEL, NULL, on_cancel, st) != 0) {
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    memset(&http_args, 0, sizeof(http_args));
    http_args.port = port;
    http_args.stop = &stop;
    if (pthread_create(&http_thread, NULL, gateway_http_thread, &http_args) != 0) {
        svc_log("voice-session-gateway", "HTTP thread start failed");
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    http_started = 1;
    svc_log(
        "voice-session-gateway",
        "pure-C control plane + reflex PCM + authenticated HTTP :%d",
        port
    );
    while (atomic_load_explicit(&stop, memory_order_relaxed) == 0) {
        if (vbus_poll(st->nc, 100) != 0) {
            run_rc = 1;
            break;
        }
        pcm_expire(st, mono_ns());
    }
    atomic_store_explicit(&stop, 1, memory_order_relaxed);
    if (http_started) pthread_join(http_thread, NULL);
    if (http_args.result != 0) run_rc = 1;
    for (i = 0; i < GW_MAX_SESSIONS; ++i) {
        if (st->sessions[i].in_use && st->sessions[i].reflex) {
            reflex_session_destroy_v1(st->sessions[i].reflex);
        }
    }
    vbus_close(st->nc);
    free(st);
    return run_rc;
}
