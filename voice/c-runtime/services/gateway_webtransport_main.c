/* Authenticated pure-C WebTransport and QUIC datagram edge. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_webtransport_core.h"

#include "../bus/vbus.h"
#include "../common/service.h"
#include "../common/product_analytics.h"
#include "../common/turn_response.h"
#include "../common/voice_auth.h"
#include "../common/model_request_capture.h"
#include "../common/base64.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include "turn_frame.h"

#include <lsquic.h>
#include <lsxpack_header.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if !LSQUIC_WEBTRANSPORT_SERVER_SUPPORT
#error "LSQUIC must be built with LSQUIC_WEBTRANSPORT=ON"
#endif

#ifndef VOICE_SOURCE_REVISION
#define VOICE_SOURCE_REVISION "unknown"
#endif

#define WT_MAX_CONNECTIONS 128u
#define WT_MAX_SESSIONS 64u
#define WT_MAX_STREAMS 160u
#define WT_MAX_HEADER_SETS 160u
#define WT_HEADER_BYTES 8192u
#define WT_RX_BYTES (GW_WT_CONTROL_FRAME_CAP + 5u)
#define WT_AUDIO_QUEUE 256u
#define WT_DEFAULT_RESPONSE_KEEPALIVE_MS 5000u
#define WT_EVENT_WIRE_BYTES DND_TOOL_EVENT_MAX
#define WT_DEFAULT_PATH "/v1/voice/turns"
#define WT_MAX_ALLOWED_ORIGINS 4u
#define WT_REFLEX_ENDPOINT_TYPE_ID 10

_Static_assert(GW_WT_EVENT_NONCE_HEX_LEN == VOICE_AUTH_NONCE_HEX_LEN,
               "WebTransport capability nonce width must match voice auth");
_Static_assert((size_t)GW_WT_RESPONSE_QUEUE_CAP * (size_t)WT_MAX_SESSIONS <=
                   (size_t)16u * 1024u * 1024u,
               "WebTransport response queues must fit the 16 MiB memory budget");
_Static_assert(WT_EVENT_WIRE_BYTES + 5u <= GW_WT_RESPONSE_QUEUE_CAP,
               "one maximum response event must fit in the response queue");

typedef struct wt_state wt_state;
typedef struct wt_session wt_session;

typedef struct {
    int in_use;
    char method[16];
    char path[256];
    char protocol[32];
    char scheme[16];
    char origin[256];
    char authority[256];
    unsigned seen_method;
    unsigned seen_path;
    unsigned seen_protocol;
    unsigned seen_scheme;
    unsigned seen_origin;
    unsigned seen_authority;
    struct lsxpack_header current;
    char decode[WT_HEADER_BYTES];
    size_t decode_offset;
} wt_header_set;

struct lsquic_conn_ctx {
    wt_state *state;
    lsquic_conn_t *conn;
    const char *abort_reason;
    int abort_errno;
    size_t response_queue_high_water;
    size_t abort_queue_bytes;
    size_t abort_frame_bytes;
    int in_use;
};

struct lsquic_stream_ctx {
    wt_state *state;
    lsquic_stream_t *stream;
    wt_header_set *headers;
    wt_session *session;
    uint8_t rx[WT_RX_BYTES];
    size_t rx_len;
    uint8_t error_tx[512];
    size_t error_len;
    size_t error_offset;
    int request_processed;
    int error_terminal;
    int in_use;
};

struct wt_session {
    wt_state *state;
    lsquic_conn_t *conn;
    lsquic_stream_t *connect_stream;
    uint64_t connect_stream_id;
    int in_use;
    struct lsquic_stream_ctx *control;
    uint32_t generation;
    char request_id[128];
    size_t request_id_len;
    char session_id[128];
    size_t session_id_len;
    char user_id[128];
    char response_subject[256];
    char reflex_subject[256];
    gw_wt_byte_ring tx;
    size_t tx_dirty;
    gw_wt_audio_reorder audio_reorder;
    gw_wt_audio_delivery audio_delivery;
    turn_response_state response;
    int capture_requested;
    voice_model_capture capture;
    uint8_t *capture_bytes;
    size_t capture_capacity;
    size_t capture_output_offset;
    uint32_t capture_output_sequence;
    int64_t capture_timestamp;
    char capture_nonce[33];
    char capture_signature[65];
    voice_analytics_turn analytics;
    uint32_t expected_datagrams;
    uint32_t expected_audio_bytes;
    size_t audio_queue_high_water;
    uint64_t audio_queue_max_us;
    uint64_t audio_publish_us;
    uint64_t input_started_ns;
    uint64_t input_endpoint_received_ns;
    uint64_t input_end_received_ns;
    uint64_t input_commit_us;
    uint64_t audio_gap_deadline_ms;
    uint64_t response_keepalive_deadline_ms;
    int authenticated;
    int audio_first;
    int audio_stream;
    int end_pending;
    int end_queued;
    int end_seen;
    int transcript_sent;
    int cancel_queued;
    int cancel_sent;
    int terminal;
};

static void process_audio_input(wt_session *session, const gw_wt_datagram *datagram);

enum wt_audio_kind {
    WT_AUDIO_PCM = 1,
    WT_AUDIO_END,
    WT_AUDIO_CANCEL
};

typedef struct {
    enum wt_audio_kind kind;
    uint16_t session_index;
    uint32_t generation;
    uint16_t data_len;
    uint64_t enqueued_ns;
    uint8_t data[GW_WT_DATAGRAM_CAP];
    char reason[128];
} wt_audio_item;

struct wt_state {
    voice_analytics *analytics;
    int socket_fd;
    struct sockaddr_storage local_address;
    socklen_t local_address_len;
    SSL_CTX *ssl_ctx;
    lsquic_engine_t *engine;
    vbus_client *bus;
    vbus_stop_flag stop;
    char path[256];
    char authority[256];
    char allowed_origins[WT_MAX_ALLOWED_ORIGINS][256];
    size_t allowed_origin_count;
    char alpn[256];
    unsigned alpn_len;
    voice_auth_verifier auth_verifier;
    const char *capture_secret;
    voice_auth_nonce_cache nonce_cache;
    voice_auth_nonce_pool capability_nonces;
    struct lsquic_conn_ctx connections[WT_MAX_CONNECTIONS];
    struct lsquic_stream_ctx streams[WT_MAX_STREAMS];
    wt_header_set header_sets[WT_MAX_HEADER_SETS];
    wt_session sessions[WT_MAX_SESSIONS];
    wt_audio_item audio_queue[WT_AUDIO_QUEUE];
    size_t audio_head;
    size_t audio_count;
    uint32_t next_generation;
    uint64_t loop_now_ms;
    uint64_t response_keepalive_interval_ms;
};

static wt_session *response_subject_session(wt_state *state, const char *subject) {
    size_t slot;
    wt_session *session;
    if (!state || gw_wt_response_subject_slot(subject, &slot) != GW_WT_OK ||
        slot >= (size_t)WT_MAX_SESSIONS) return NULL;
    session = &state->sessions[slot];
    if (!session->in_use || !session->authenticated || session->terminal ||
        CRYPTO_memcmp(session->response_subject, subject,
                      GW_WT_EVENT_SUBJECT_LENGTH + 1u) != 0)
        return NULL;
    return session;
}

static int active_audio_session_id_conflict(
    const wt_state *state,
    const wt_session *candidate,
    const char *session_id
) {
    size_t i;
    size_t session_id_len;
    if (!state || !candidate || !session_id || !session_id[0]) return 1;
    session_id_len = strlen(session_id);
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        const wt_session *session = &state->sessions[i];
        if (session != candidate && session->in_use && session->authenticated &&
            session->audio_first && !session->terminal &&
            session->session_id_len == session_id_len &&
            memcmp(session->session_id, session_id, session_id_len) == 0)
            return 1;
    }
    return 0;
}

static wt_session *reflex_subject_session(wt_state *state, const char *subject) {
    size_t i;
    wt_session *found = NULL;
    if (!state || !subject) return NULL;
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        wt_session *session = &state->sessions[i];
        if (!session->in_use || !session->authenticated || !session->audio_first ||
            session->terminal || strcmp(session->reflex_subject, subject) != 0)
            continue;
        if (found) return NULL;
        found = session;
    }
    return found;
}

static int64_t realtime_seconds(void) {
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t elapsed_microseconds(uint64_t started, uint64_t ended) {
    return started != 0u && ended >= started ? (ended - started) / 1000u : 0u;
}

static uint64_t deadline_after(uint64_t now, uint64_t delay) {
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}

static uint32_t next_turn_generation(wt_state *state) {
    state->next_generation++;
    if (state->next_generation == 0u) state->next_generation++;
    return state->next_generation;
}

static int lsquic_log_buffer(void *context, const char *buffer, size_t length) {
    ssize_t written;
    (void)context;
    if (!buffer || length == 0 || length > (size_t)INT_MAX) return 0;
    do {
        written = write(STDERR_FILENO, buffer, length);
    } while (written < 0 && errno == EINTR);
    return written > 0 ? (int)written : 0;
}

static const struct lsquic_logger_if wt_logger = {
    .log_buf = lsquic_log_buffer,
};

static const char *connection_status_name(enum LSQUIC_CONN_STATUS status) {
    switch (status) {
        case LSCONN_ST_HSK_IN_PROGRESS: return "handshake_in_progress";
        case LSCONN_ST_CONNECTED: return "connected";
        case LSCONN_ST_HSK_FAILURE: return "handshake_failure";
        case LSCONN_ST_GOING_AWAY: return "going_away";
        case LSCONN_ST_TIMED_OUT: return "timed_out";
        case LSCONN_ST_RESET: return "reset";
        case LSCONN_ST_USER_ABORTED: return "user_aborted";
        case LSCONN_ST_ERROR: return "error";
        case LSCONN_ST_CLOSED: return "closed";
        case LSCONN_ST_PEER_GOING_AWAY: return "peer_going_away";
        case LSCONN_ST_VERNEG_FAILURE: return "version_negotiation_failure";
        default: return "unknown";
    }
}

static void abort_connection_error(lsquic_conn_t *conn, const char *reason,
                                   int error_number) {
    struct lsquic_conn_ctx *context;
    if (!conn) return;
    context = lsquic_conn_get_ctx(conn);
    if (context) {
        if (!context->abort_reason) {
            context->abort_reason = reason;
            context->abort_errno = error_number;
        }
    } else {
        svc_log("webtransport-gateway",
                "connection abort reason=%s abort_errno=%d context=unavailable",
                reason, error_number);
    }
    lsquic_conn_abort(conn);
}

static void abort_connection(lsquic_conn_t *conn, const char *reason) {
    abort_connection_error(conn, reason, 0);
}

static void abort_connection_queue(lsquic_conn_t *conn, size_t queued_bytes,
                                   size_t frame_bytes) {
    struct lsquic_conn_ctx *context;
    if (!conn) return;
    context = lsquic_conn_get_ctx(conn);
    if (context && !context->abort_reason) {
        context->abort_queue_bytes = queued_bytes;
        context->abort_frame_bytes = frame_bytes;
    }
    abort_connection(conn, "response_queue_full");
}

static void publish_cancel(wt_session *session, const char *reason) {
    uint8_t wire[512];
    size_t wire_len;
    if (!session || !session->authenticated || session->cancel_sent) return;
    wire_len = pb_encode_turn_cancel(wire, sizeof(wire), session->request_id,
                                     session->user_id, reason ? reason : "client_disconnected");
    if (wire_len != 0 && vbus_publish(session->state->bus, SUBJ_TURN_CANCEL, wire, wire_len) == 0)
        session->cancel_sent = 1;
}

static void capture_release(wt_session *session) {
    if (session->capture_bytes) {
        OPENSSL_cleanse(session->capture_bytes,session->capture_capacity);
        free(session->capture_bytes); session->capture_bytes=NULL;
    }
    session->capture_capacity=0;
    voice_model_capture_destroy(&session->capture);
}

static void session_turn_scrub(wt_session *session) {
    size_t dirty;
    size_t i;
    size_t tx_end = offsetof(wt_session, tx) + sizeof(session->tx);
    size_t audio_end = offsetof(wt_session, audio_reorder) + sizeof(session->audio_reorder);
    size_t audio_metadata = offsetof(gw_wt_audio_reorder, next_sequence);
    if (!session) return;
    capture_release(session);
    dirty = session->tx_dirty;
    if (dirty > GW_WT_RESPONSE_QUEUE_CAP) dirty = GW_WT_RESPONSE_QUEUE_CAP;

    if (session->control) session->control->session = NULL;
    OPENSSL_cleanse((uint8_t *)session + offsetof(wt_session, control),
                    offsetof(wt_session, tx) - offsetof(wt_session, control));
    if (dirty != 0u) OPENSSL_cleanse(session->tx.data, dirty);
    OPENSSL_cleanse((uint8_t *)&session->tx + offsetof(gw_wt_byte_ring, head),
                    sizeof(session->tx) - offsetof(gw_wt_byte_ring, head));
    OPENSSL_cleanse((uint8_t *)session + tx_end,
                    offsetof(wt_session, audio_reorder) - tx_end);

    for (i = 0; i < GW_WT_AUDIO_REORDER_WINDOW; ++i) {
        gw_wt_audio_slot *slot = &session->audio_reorder.slots[i];
        if (slot->present) OPENSSL_cleanse(slot, sizeof(*slot));
    }
    OPENSSL_cleanse((uint8_t *)&session->audio_reorder + audio_metadata,
                    sizeof(session->audio_reorder) - audio_metadata);
    OPENSSL_cleanse((uint8_t *)session + audio_end, sizeof(*session) - audio_end);
}

static void turn_release(wt_session *session, int cancel) {
    if (!session || !session->in_use) return;
    if (cancel && !session->terminal) publish_cancel(session, "client_disconnected");
    voice_analytics_finish(session->state->analytics, &session->analytics,
        session->analytics.client_interrupt && session->cancel_sent ?
            VOICE_ANALYTICS_CANCELED : VOICE_ANALYTICS_DISCONNECTED, monotonic_nanoseconds());
    session_turn_scrub(session);
}

static void session_release(wt_session *session, int cancel) {
    if (!session || !session->in_use) return;
    turn_release(session, cancel);
    OPENSSL_cleanse(session, offsetof(wt_session, control));
}

#if defined(WT_VERIFY_ZEROED_FREE_SLOTS)
static int memory_is_zero(const void *memory, size_t length) {
    const uint8_t *bytes = (const uint8_t *)memory;
    size_t i;
    for (i = 0; i < length; ++i)
        if (bytes[i] != 0u) return 0;
    return 1;
}
#endif

static wt_session *session_allocate(wt_state *state, lsquic_conn_t *conn,
                                    lsquic_stream_t *connect_stream) {
    size_t i;
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        wt_session *session = &state->sessions[i];
        if (session->in_use) continue;
#if defined(WT_VERIFY_ZEROED_FREE_SLOTS)
        if (!memory_is_zero(session, sizeof(*session))) {
            svc_log("webtransport-gateway", "free session slot is not zero");
            return NULL;
        }
#endif
        /* State allocation and session_release keep every free slot zeroed. */
        session->state = state;
        session->conn = conn;
        session->connect_stream = connect_stream;
        session->connect_stream_id = lsquic_stream_id(connect_stream);
        session->in_use = 1;
        return session;
    }
    return NULL;
}

static wt_session *session_find(wt_state *state, lsquic_conn_t *conn, uint64_t stream_id) {
    size_t i;
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        wt_session *session = &state->sessions[i];
        if (session->in_use && session->conn == conn &&
            session->connect_stream_id == stream_id) return session;
    }
    return NULL;
}

static void session_queue_frame_allowed(wt_session *session, uint8_t type,
                                const uint8_t *payload, size_t payload_len, int finishing_capture) {
    size_t frame_len = payload_len + 5u;
    size_t dirty;
    if (!session) return;
    if (!session->control || (session->terminal && !finishing_capture) ||
        (payload_len != 0u && !payload) || payload_len > TURN_FRAME_MAX_BYTES ||
        payload_len > GW_WT_RESPONSE_QUEUE_CAP - 5u) {
        const char *reason = !session->control ? "response_control_missing" :
                             session->terminal ? "response_after_terminal" :
                             payload_len > TURN_FRAME_MAX_BYTES ||
                                     payload_len > GW_WT_RESPONSE_QUEUE_CAP - 5u
                                 ? "response_frame_too_large" :
                                   "response_payload_missing";
        abort_connection(session->conn, reason);
        return;
    }
    dirty = gw_wt_byte_ring_dirty_after_write(
        &session->tx, session->tx_dirty, frame_len);
    if (!gw_wt_byte_ring_write_frame(&session->tx, type, payload, payload_len)) {
        publish_cancel(session, "edge_backpressure");
        abort_connection_queue(session->conn, session->tx.size, frame_len);
        return;
    }
    session->tx_dirty = dirty;
    {
        struct lsquic_conn_ctx *context = lsquic_conn_get_ctx(session->conn);
        if (context && session->tx.size > context->response_queue_high_water)
            context->response_queue_high_water = session->tx.size;
    }
    if (session->authenticated && session->state->loop_now_ms != 0u)
        session->response_keepalive_deadline_ms = deadline_after(
            session->state->loop_now_ms,
            session->state->response_keepalive_interval_ms);
    lsquic_stream_wantwrite(session->control->stream, 1);
}

static void session_queue_frame(wt_session *session, uint8_t type,
                                const uint8_t *payload, size_t payload_len) {
    session_queue_frame_allowed(session,type,payload,payload_len,0);
}

/* Emit at most one bounded chunk per write iteration, while leaving half the
 * response ring available for voice. Completion may already be queued; these
 * evidence controls do not change the completed voice result. */
static void pump_model_capture(wt_session *session) {
    char encoded[VOICE_MODEL_REQUEST_CHUNK_MAX * 4u / 3u + 8u];
    char json[VOICE_MODEL_REQUEST_CHUNK_MAX * 4u / 3u + 1024u];
    if (!session->capture_bytes || session->capture.state!=2 ||
        session->tx.size>GW_WT_RESPONSE_QUEUE_CAP/2u) return;
    size_t length=session->capture.expected_bytes-session->capture_output_offset;
    if (length>VOICE_MODEL_REQUEST_CHUNK_MAX) length=VOICE_MODEL_REQUEST_CHUNK_MAX;
    if (!length || !base64_encode_v1(session->capture_bytes+session->capture_output_offset,
            length,encoded,sizeof(encoded))) return;
    int final=session->capture_output_offset+length==session->capture.expected_bytes;
    int written=snprintf(json,sizeof(json),
        "{\"type\":\"model_request_chunk\",\"protocol_version\":\"%s\",\"request_id\":\"%s\","
        "\"sequence\":%" PRIu32 ",\"total_bytes\":%zu,\"data\":\"%s\",\"final\":%s,"
        "\"sha256\":\"%s\",\"captured_at\":%" PRId64 ",\"nonce\":\"%s\",\"signature\":\"%s\"}",
        TURN_PROTOCOL_VERSION,session->request_id,session->capture_output_sequence,
        session->capture.expected_bytes,encoded,final?"true":"false",session->capture.sha256,
        session->capture_timestamp,session->capture_nonce,session->capture_signature);
    OPENSSL_cleanse(encoded,sizeof(encoded));
    if (written<=0 || (size_t)written>=sizeof(json)) {
        OPENSSL_cleanse(json,sizeof(json));
        abort_connection(session->conn,"model_capture_encoding_failed"); return;
    }
    session_queue_frame_allowed(session,TURN_FRAME_CONTROL_JSON,(const uint8_t *)json,(size_t)written,1);
    OPENSSL_cleanse(json,sizeof(json));
    session->capture_output_offset+=length;
    session->capture_output_sequence++;
    if (final) {
        OPENSSL_cleanse(session->capture_bytes,session->capture_capacity);
        free(session->capture_bytes); session->capture_bytes=NULL; session->capture_capacity=0;
    }
}

static void session_fail(wt_session *session, const char *code) {
    char json[320];
    int written;
    if (!session || session->terminal) return;
    voice_analytics_finish(session->state->analytics, &session->analytics,
        VOICE_ANALYTICS_FAILED, monotonic_nanoseconds());
    written = snprintf(json, sizeof(json),
                       "{\"type\":\"error\",\"protocol_version\":\"%s\","
                       "\"request_id\":\"%s\",\"error\":\"%s\"}",
                       TURN_PROTOCOL_VERSION, session->request_id, code);
    if (written > 0 && (size_t)written < sizeof(json))
        session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                            (const uint8_t *)json, (size_t)written);
    publish_cancel(session, code);
    session->cancel_queued = 1;
    session->audio_gap_deadline_ms = 0;
    session->terminal = 1;
}

static void session_queue_input_commit(wt_session *session) {
    char json[1024];
    int written;
    if (!session || session->terminal) return;
    written = snprintf(
        json, sizeof(json),
        "{\"type\":\"input_committed\",\"protocol_version\":\"%s\","
        "\"request_id\":\"%s\",\"audio_bytes\":%zu,"
        "\"audio_datagrams\":%" PRIu32 ",\"lost_datagrams\":0,"
        "\"forwarded_audio_bytes\":%zu,\"forwarded_datagrams\":%" PRIu32 ","
        "\"drained_audio_bytes\":%zu,\"drained_datagrams\":%" PRIu32 ","
        "\"server_endpoint\":%s,"
        "\"duplicate_datagrams\":%" PRIu32 ","
        "\"reordered_datagrams\":%" PRIu32 ",\"queue_high_water\":%zu,"
        "\"edge_audio_queue_max_us\":%" PRIu64 ","
        "\"edge_audio_publish_us\":%" PRIu64 ","
        "\"edge_input_commit_us\":%" PRIu64 "}",
        TURN_PROTOCOL_VERSION, session->request_id,
        session->audio_reorder.unique_audio_bytes,
        session->audio_reorder.unique_datagrams,
        session->audio_delivery.forwarded_audio_bytes,
        session->audio_delivery.forwarded_datagrams,
        session->audio_delivery.drained_audio_bytes,
        session->audio_delivery.drained_datagrams,
        session->audio_delivery.endpoint_seen ? "true" : "false",
        session->audio_reorder.duplicate_datagrams,
        session->audio_reorder.reordered_datagrams, session->audio_queue_high_water,
        session->audio_queue_max_us, session->audio_publish_us, session->input_commit_us);
    if (written <= 0 || (size_t)written >= sizeof(json)) {
        session_fail(session, "input_commit_encoding_failed");
        return;
    }
    session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                        (const uint8_t *)json, (size_t)written);
}

static void session_queue_input_endpoint(wt_session *session) {
    char json[640];
    uint64_t endpoint_us;
    int written;
    if (!session || session->terminal || !session->audio_delivery.endpoint_seen) return;
    endpoint_us = elapsed_microseconds(
        session->input_started_ns, session->input_endpoint_received_ns);
    written = snprintf(
        json, sizeof(json),
        "{\"type\":\"input_endpoint\",\"protocol_version\":\"%s\","
        "\"request_id\":\"%s\","
        "\"received_audio_bytes\":%zu,\"received_datagrams\":%" PRIu32 ","
        "\"forwarded_audio_bytes\":%zu,\"forwarded_datagrams\":%" PRIu32 ","
        "\"edge_input_endpoint_us\":%" PRIu64 "}",
        TURN_PROTOCOL_VERSION, session->request_id,
        session->audio_reorder.unique_audio_bytes,
        session->audio_reorder.unique_datagrams,
        session->audio_delivery.forwarded_audio_bytes,
        session->audio_delivery.forwarded_datagrams, endpoint_us);
    if (written <= 0 || (size_t)written >= sizeof(json)) {
        session_fail(session, "input_endpoint_encoding_failed");
        return;
    }
    session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                        (const uint8_t *)json, (size_t)written);
}

static void stream_queue_error(struct lsquic_stream_ctx *stream, const char *code) {
    char json[320];
    size_t frame_len = 0;
    int written;
    if (!stream || stream->error_terminal) return;
    written = snprintf(json, sizeof(json),
                       "{\"type\":\"error\",\"protocol_version\":\"%s\","
                       "\"error\":\"%s\"}", TURN_PROTOCOL_VERSION, code);
    if (written <= 0 || (size_t)written >= sizeof(json) ||
        turn_frame_encode_v1(TURN_FRAME_CONTROL_JSON, (const uint8_t *)json, (size_t)written,
                             stream->error_tx, sizeof(stream->error_tx), &frame_len) != TURN_FRAME_OK) {
        abort_connection(lsquic_stream_conn(stream->stream), "error_frame_encode_failed");
        return;
    }
    stream->error_len = frame_len;
    stream->request_processed = 1;
    stream->error_terminal = 1;
    (void)lsquic_stream_wantread(stream->stream, 0);
    lsquic_stream_wantwrite(stream->stream, 1);
}

static int send_status(lsquic_stream_t *stream, const char *status) {
    struct lsxpack_header header;
    lsquic_http_headers_t headers;
    char buffer[32];
    size_t name_len = sizeof(":status") - 1u;
    size_t value_len = strlen(status);
    if (name_len + value_len > sizeof(buffer)) return -1;
    memcpy(buffer, ":status", name_len);
    memcpy(buffer + name_len, status, value_len);
    lsxpack_header_set_offset2(&header, buffer, 0, name_len, name_len, value_len);
    headers.count = 1;
    headers.headers = &header;
    return lsquic_stream_send_headers(stream, &headers, 0);
}

static struct lsquic_conn_ctx *connection_allocate(wt_state *state, lsquic_conn_t *conn) {
    size_t i;
    for (i = 0; i < WT_MAX_CONNECTIONS; ++i) {
        struct lsquic_conn_ctx *context = &state->connections[i];
        if (context->in_use) continue;
        /* State allocation and on_conn_closed keep every free slot zeroed. */
        context->state = state;
        context->conn = conn;
        context->in_use = 1;
        return context;
    }
    return NULL;
}

static lsquic_conn_ctx_t *on_new_conn(void *user, lsquic_conn_t *conn) {
    wt_state *state = (wt_state *)user;
    struct lsquic_conn_ctx *context = connection_allocate(state, conn);
    if (!context) abort_connection(conn, "connection_capacity");
    return context;
}

static void on_conn_closed(lsquic_conn_t *conn) {
    struct lsquic_conn_ctx *context = lsquic_conn_get_ctx(conn);
    enum LSQUIC_CONN_STATUS status;
    size_t active_sessions = 0u;
    size_t i;
    if (!context) return;
    status = lsquic_conn_status(conn, NULL, 0u);
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        if (context->state->sessions[i].in_use && context->state->sessions[i].conn == conn) {
            active_sessions++;
            session_release(&context->state->sessions[i], 1);
        }
    }
    svc_log(
        "webtransport-gateway",
        "connection closed status=%s status_code=%d abort_reason=%s abort_errno=%d "
        "response_queue_high_water=%zu abort_queue_bytes=%zu abort_frame_bytes=%zu "
        "active_sessions=%zu",
        connection_status_name(status), (int)status,
        context->abort_reason ? context->abort_reason : "none", context->abort_errno,
        context->response_queue_high_water, context->abort_queue_bytes,
        context->abort_frame_bytes, active_sessions);
    memset(context, 0, sizeof(*context));
}

static struct lsquic_stream_ctx *stream_allocate(wt_state *state, lsquic_stream_t *stream) {
    size_t i;
    for (i = 0; i < WT_MAX_STREAMS; ++i) {
        struct lsquic_stream_ctx *context = &state->streams[i];
        if (context->in_use) continue;
#if defined(WT_VERIFY_ZEROED_FREE_SLOTS)
        if (!memory_is_zero(context, sizeof(*context))) {
            svc_log("webtransport-gateway", "free stream slot is not zero");
            return NULL;
        }
#endif
        /* State allocation and stream_slot_scrub keep every free slot zeroed. */
        context->state = state;
        context->stream = stream;
        context->in_use = 1;
        return context;
    }
    return NULL;
}

static lsquic_stream_ctx_t *on_new_stream(void *user, lsquic_stream_t *stream) {
    wt_state *state = (wt_state *)user;
    struct lsquic_stream_ctx *context = stream_allocate(state, stream);
    if (!context) {
        abort_connection(lsquic_stream_conn(stream), "stream_capacity");
        return NULL;
    }
    lsquic_stream_wantread(stream, 1);
    return context;
}

static int authenticate_request(struct lsquic_stream_ctx *stream, wt_session *session,
                                const uint8_t *payload, size_t payload_len) {
    gw_wt_turn_request request;
    voice_auth_identity_claims claims;
    turn_start_c turn;
    uint8_t wire[DND_TURN_START_WIRE_MAX];
    char event_nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u];
    char accepted[768];
    size_t wire_len;
    size_t session_slot;
    int64_t now = realtime_seconds();
    int written;
    int auth_status;
    uint64_t prepare_started = monotonic_nanoseconds();
    uint64_t auth_started;
    uint64_t auth_ended;
    uint64_t publish_started;
    uint64_t publish_ended;
    if (gw_wt_turn_request_parse_active(
            payload, payload_len, &request) != GW_WT_OK || now <= 0)
        return -1;
    auth_started = monotonic_nanoseconds();
    auth_status = voice_auth_verifier_identity_verify(
        &stream->state->auth_verifier,
        request.identity_token, request.request_id, now, &claims);
    if (auth_status != VOICE_AUTH_OK ||
        voice_auth_nonce_index_accept(&stream->state->nonce_cache, claims.nonce, now,
                                      claims.expires_at - now) != VOICE_AUTH_OK) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -2;
    }
    auth_ended = monotonic_nanoseconds();
    if (voice_auth_nonce_pool_next(
            &stream->state->capability_nonces, event_nonce) != VOICE_AUTH_OK) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -3;
    }
    memset(&turn, 0, sizeof(turn));
    memcpy(turn.request_id, request.request_id, strlen(request.request_id) + 1u);
    memcpy(turn.session_id, request.session_id, strlen(request.session_id) + 1u);
    memcpy(turn.user_id, claims.user_id, strlen(claims.user_id) + 1u);
    memcpy(turn.text, request.text, strlen(request.text) + 1u);
    turn.premium = claims.premium;
    turn.model_request_capture = claims.model_request_capture;
    turn.enable_rag = claims.premium && (request.enable_rag ||
        strcmp(request.metadata.retrieval_force, "true") == 0);
    turn.enable_tts = request.enable_tts;
    turn.metadata = request.metadata;
    turn.dnd_initiative = request.dnd_initiative;
    turn.dnd_campaign = request.dnd_campaign;
    turn.dnd_encounter_action = request.dnd_encounter_action;
    memcpy(turn.meta_budget_ms, request.meta_budget_ms, strlen(request.meta_budget_ms) + 1u);
    memcpy(turn.meta_deadline_unix_ms, request.meta_deadline_unix_ms,
        strlen(request.meta_deadline_unix_ms) + 1u);
    turn.has_meta_budget = turn.meta_budget_ms[0] != '\0';
    turn.has_meta_deadline = turn.meta_deadline_unix_ms[0] != '\0';
    if (request.audio_first &&
        active_audio_session_id_conflict(
            stream->state, session, turn.session_id)) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -4;
    }
    if (request.audio_first) {
        written = snprintf(
            session->reflex_subject, sizeof(session->reflex_subject),
            "%s.%s", SUBJ_VOICE_REFLEX_PFX, turn.session_id);
        if (written <= 0 || (size_t)written >= sizeof(session->reflex_subject)) {
            OPENSSL_cleanse(&claims, sizeof(claims));
            return -1;
        }
    }
    session_slot = (size_t)(session - stream->state->sessions);
    if (session_slot >= (size_t)WT_MAX_SESSIONS ||
        gw_wt_response_subject_write(
            turn.response_subject, sizeof(turn.response_subject),
            session_slot, event_nonce) != GW_WT_OK) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -1;
    }
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    publish_started = monotonic_nanoseconds();
    if (wire_len == 0 || vbus_publish(stream->state->bus,
                                     request.audio_first ? SUBJ_VOICE_TURN_PREPARE : SUBJ_TURN_START,
                                     wire, wire_len) != 0) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -3;
    }
    publish_ended = monotonic_nanoseconds();
    session->generation = next_turn_generation(stream->state);
    session->request_id_len = strlen(turn.request_id);
    memcpy(session->request_id, turn.request_id, session->request_id_len + 1u);
    session->session_id_len = strlen(turn.session_id);
    memcpy(session->session_id, turn.session_id, session->session_id_len + 1u);
    memcpy(session->user_id, turn.user_id, strlen(turn.user_id) + 1u);
    memcpy(session->response_subject, turn.response_subject, strlen(turn.response_subject) + 1u);
    session->audio_first = request.audio_first;
    session->capture_requested = claims.model_request_capture;
    session->audio_stream = request.audio_stream;
    session->response.wait_for_pcm = request.enable_tts ? 1 : 0;
    session->authenticated = 1;
    if (stream->state->analytics)
        voice_analytics_begin(&session->analytics, &turn, request.audio_first, 1, publish_ended);
    session->control = stream;
    stream->session = session;
    written = snprintf(
        accepted, sizeof(accepted),
        "{\"type\":\"accepted\",\"protocol_version\":\"%s\","
        "\"request_id\":\"%s\",\"metadata\":{"
        "\"audio_endpointing\":\"%s\",\"audio_endpointing_owner\":\"c_reflex_v1\","
        "\"audio_endpoint_feedback\":\"%s\","
        "\"audio_commit_signal\":\"final_transcript\","
        "\"audio_datagram_protocol\":\"dtvp1\","
        "\"audio_input_transport\":\"%s\","
        "\"model_request_capture\":\"%s\","
        "\"response_event_contract\":\"canonical-v1\","
        "\"runtime\":\"pure-c\",\"source_revision\":\"%s\","
        "\"response_keepalive_ms\":%" PRIu64 ","
        "\"edge_auth_us\":%" PRIu64 ",\"edge_vbus_publish_us\":%" PRIu64 ","
        "\"edge_prepare_us\":%" PRIu64 "}}",
        TURN_PROTOCOL_VERSION, session->request_id, request.audio_first ? "server" : "none",
        request.audio_first ? "control-v1" : "none",
        request.audio_stream ? "stream-v1" : "datagram-v1",
        session->capture_requested ? "prepared-request-v1" : "none",
        VOICE_SOURCE_REVISION,
        stream->state->response_keepalive_interval_ms,
        elapsed_microseconds(auth_started, auth_ended),
        elapsed_microseconds(publish_started, publish_ended),
        elapsed_microseconds(prepare_started, publish_ended));
    if (written <= 0 || (size_t)written >= sizeof(accepted)) {
        OPENSSL_cleanse(&claims, sizeof(claims));
        return -3;
    }
    session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                        (const uint8_t *)accepted, (size_t)written);
    OPENSSL_cleanse(&claims, sizeof(claims));
    return 0;
}

static void process_control_stream(struct lsquic_stream_ctx *context) {
    uint8_t frame_type;
    const uint8_t *payload;
    size_t payload_len;
    size_t consumed;
    int result;
    if (context->request_processed) return;
    result = turn_frame_decode_v1(context->rx, context->rx_len, &frame_type, &payload,
                                  &payload_len, &consumed);
    if (result == TURN_FRAME_ERR_IO) return;
    context->request_processed = 1;
    if (result != TURN_FRAME_OK || frame_type != TURN_FRAME_CONTROL_JSON ||
        consumed != context->rx_len || !context->session) {
        stream_queue_error(context, "invalid_control_frame");
        return;
    }
    result = authenticate_request(context, context->session, payload, payload_len);
    if (result == -2) stream_queue_error(context, "unauthorized");
    else if (result == -3) stream_queue_error(context, "edge_unavailable");
    else if (result == -4) stream_queue_error(context, "audio_session_conflict");
    else if (result != 0) stream_queue_error(context, "invalid_turn");
    else if (!context->session->audio_stream) (void)lsquic_stream_wantread(context->stream, 0);
    if (result == 0 && context->session->audio_stream) {
        OPENSSL_cleanse(context->rx, context->rx_len);
        context->rx_len = 0u;
    }
}

static void process_audio_stream(struct lsquic_stream_ctx *context) {
    wt_session *session = context->session;
    while (context->rx_len >= 5u && session && !session->terminal) {
        uint8_t type;
        const uint8_t *payload;
        size_t payload_len, consumed;
        gw_wt_datagram input;
        size_t declared = ((size_t)context->rx[1] << 24u) | ((size_t)context->rx[2] << 16u) |
            ((size_t)context->rx[3] << 8u) | (size_t)context->rx[4];
        if (declared > GW_WT_DATAGRAM_CAP) {
            session_fail(session, "audio_stream_frame_too_large"); return;
        }
        int decoded = turn_frame_decode_v1(context->rx, context->rx_len, &type, &payload, &payload_len, &consumed);
        if (decoded == TURN_FRAME_ERR_IO) return;
        if (decoded != TURN_FRAME_OK || gw_wt_stream_input_parse(type, payload, payload_len, &input) != GW_WT_OK) {
            session_fail(session, "invalid_audio_stream_frame"); return;
        }
        if ((input.is_audio && (session->end_pending || input.sequence != session->audio_reorder.next_sequence)) ||
            (input.is_control && strcmp(input.control.request_id, session->request_id))) {
            session_fail(session, "audio_stream_sequence_or_owner"); return;
        }
        if (input.is_control && input.control.kind == GW_WT_CONTROL_END &&
            (session->end_pending || gw_wt_audio_reorder_commit_state(&session->audio_reorder,
                input.control.packet_count, input.control.audio_bytes) != GW_WT_AUDIO_COMMIT_READY)) {
            session_fail(session, "audio_commit_mismatch"); return;
        }
        process_audio_input(session, &input);
        size_t remaining = context->rx_len - consumed;
        memmove(context->rx, context->rx + consumed, remaining);
        OPENSSL_cleanse(context->rx + remaining, consumed);
        context->rx_len = remaining;
    }
}

static int origin_allowed(const wt_state *state, const char *origin) {
    size_t i;
    if (!state || !origin) return 0;
    for (i = 0; i < state->allowed_origin_count; ++i)
        if (strcmp(origin, state->allowed_origins[i]) == 0) return 1;
    return 0;
}

static int connect_headers_valid(const wt_state *state, const wt_header_set *headers) {
    return headers && headers->seen_method == 1u && headers->seen_path == 1u &&
           headers->seen_protocol == 1u && headers->seen_scheme == 1u &&
           headers->seen_origin == 1u && headers->seen_authority == 1u &&
           strcmp(headers->method, "CONNECT") == 0 &&
           strcmp(headers->protocol, "webtransport") == 0 &&
           strcmp(headers->scheme, "https") == 0 &&
           strcmp(headers->authority, state->authority) == 0 &&
           origin_allowed(state, headers->origin) && strcmp(headers->path, state->path) == 0;
}

static void process_http_stream(struct lsquic_stream_ctx *context) {
    wt_header_set *headers;
    wt_session *session;
    if (context->request_processed) return;
    headers = (wt_header_set *)lsquic_stream_get_hset(context->stream);
    if (!headers) return;
    context->headers = headers;
    context->request_processed = 1;
    if (!connect_headers_valid(context->state, headers)) {
        (void)send_status(context->stream, "404");
        (void)lsquic_stream_wantread(context->stream, 0);
        lsquic_stream_shutdown(context->stream, 1);
        return;
    }
    /* The Safari-compatible SETTINGS advertise one session per connection. */
    for (size_t i = 0; i < WT_MAX_SESSIONS; ++i) {
        if (context->state->sessions[i].in_use &&
            context->state->sessions[i].conn == lsquic_stream_conn(context->stream)) {
            (void)send_status(context->stream, "429");
            lsquic_stream_shutdown(context->stream, 1);
            return;
        }
    }
    session = session_allocate(context->state, lsquic_stream_conn(context->stream),
                               context->stream);
    if (!session) {
        (void)send_status(context->stream, "503");
        lsquic_stream_shutdown(context->stream, 1);
        return;
    }
    context->session = session;
    if (send_status(context->stream, "200") != 0) {
        session_release(session, 0);
        abort_connection(lsquic_stream_conn(context->stream), "connect_response_failed");
        return;
    }
    lsquic_stream_set_webtransport_session(context->stream);
    if (lsquic_stream_flush(context->stream) != 0) {
        session_release(session, 0);
        abort_connection(lsquic_stream_conn(context->stream), "connect_flush_failed");
        return;
    }
}

static void process_connect_stream(struct lsquic_stream_ctx *context) {
    ssize_t count;
    if (!context || !context->session ||
        context->session->connect_stream != context->stream) return;
    while (context->rx_len < sizeof(context->rx)) {
        count = lsquic_stream_read(context->stream, context->rx + context->rx_len,
                                   sizeof(context->rx) - context->rx_len);
        if (count > 0) {
            context->rx_len += (size_t)count;
            continue;
        }
        if (count == 0) {
            session_release(context->session, 1);
            context->session = NULL;
            (void)lsquic_stream_wantread(context->stream, 0);
            (void)lsquic_stream_shutdown(context->stream, 1);
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            abort_connection_error(lsquic_stream_conn(context->stream),
                                   "connect_read_failed", errno);
        }
        return;
    }
    abort_connection(lsquic_stream_conn(context->stream), "connect_request_too_large");
}

static void on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *user) {
    struct lsquic_stream_ctx *context = (struct lsquic_stream_ctx *)user;
    ssize_t count;
    if (!context) return;
    if (lsquic_stream_is_webtransport_client_bidi_stream(stream)) {
        int session_id = lsquic_stream_get_webtransport_session_stream_id(stream);
        if (session_id < 0) {
            abort_connection(lsquic_stream_conn(stream), "invalid_session_stream_id");
            return;
        }
        if (!context->session)
            context->session = session_find(context->state, lsquic_stream_conn(stream),
                                            (uint64_t)(unsigned)session_id);
        if (!context->session ||
            (context->session->control && context->session->control != context)) {
            stream_queue_error(context, "session_stream_rejected");
            return;
        }
        if (context->request_processed && (!context->session->in_use ||
            !context->session->authenticated || context->session->control != context ||
            context->session->terminal || !context->session->audio_stream)) {
            (void)lsquic_stream_wantread(stream, 0);
            return;
        }
        while (context->rx_len < sizeof(context->rx)) {
            count = lsquic_stream_read(stream, context->rx + context->rx_len,
                                       sizeof(context->rx) - context->rx_len);
            if (count > 0) {
                context->rx_len += (size_t)count;
                if (context->request_processed) process_audio_stream(context);
                else process_control_stream(context);
                if (context->error_terminal || context->session->terminal ||
                    (context->request_processed && !context->session->audio_stream)) break;
                continue;
            }
            if (count == 0) {
                if (!context->request_processed) {
                    process_control_stream(context);
                    if (!context->request_processed) stream_queue_error(context, "truncated_control");
                } else if (context->session->audio_stream) {
                    process_audio_stream(context);
                    if (context->rx_len || !context->session->end_pending)
                        session_fail(context->session, "truncated_audio_stream");
                    (void)lsquic_stream_wantread(stream, 0);
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                abort_connection_error(lsquic_stream_conn(stream), "control_read_failed",
                                       errno);
            }
            break;
        }
        if (context->rx_len == sizeof(context->rx)) {
            if (!context->request_processed) stream_queue_error(context, "control_too_large");
            else session_fail(context->session, "audio_stream_frame_too_large");
        }
        return;
    }
    process_http_stream(context);
    if (context->request_processed) process_connect_stream(context);
}

static void on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *user) {
    struct lsquic_stream_ctx *context = (struct lsquic_stream_ctx *)user;
    wt_session *session;
    const uint8_t *data;
    size_t length;
    ssize_t written;
    if (!context) return;
    session = context->session;
    if (session && session->control == context) {
        for (;;) {
            pump_model_capture(session);
            if (!gw_wt_byte_ring_peek(&session->tx, &data, &length)) break;
            written = lsquic_stream_write(stream, data, length);
            if (written > 0) {
                if (!gw_wt_byte_ring_consume(&session->tx, (size_t)written)) {
                    abort_connection(lsquic_stream_conn(stream), "response_ring_corrupt");
                    return;
                }
                continue;
            }
            if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                abort_connection_error(lsquic_stream_conn(stream), "response_write_failed",
                                       errno);
            return;
        }
        if (lsquic_stream_flush(stream) != 0) {
            abort_connection_error(lsquic_stream_conn(stream), "response_flush_failed",
                                   errno);
            return;
        }
        lsquic_stream_wantwrite(stream, 0);
        if (session->terminal) {
            (void)lsquic_stream_shutdown(stream, 1);
            turn_release(session, 0);
        }
        return;
    }
    data = context->error_tx;
    length = context->error_len;
    while (context->error_offset < length) {
        written = lsquic_stream_write(stream, data + context->error_offset,
                                      length - context->error_offset);
        if (written > 0) {
            context->error_offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            abort_connection_error(lsquic_stream_conn(stream), "error_write_failed", errno);
        return;
    }
    lsquic_stream_wantwrite(stream, 0);
    if (context->error_terminal) lsquic_stream_shutdown(stream, 1);
}

static void header_release(wt_header_set *headers) {
    if (headers) memset(headers, 0, sizeof(*headers));
}

static void stream_slot_scrub(struct lsquic_stream_ctx *context) {
    size_t dirty;
    size_t rx_offset = offsetof(struct lsquic_stream_ctx, rx);
    size_t rx_end = offsetof(struct lsquic_stream_ctx, rx) + sizeof(context->rx);
    if (!context) return;
    dirty = context->rx_len;
    if (dirty > sizeof(context->rx)) dirty = sizeof(context->rx);
    OPENSSL_cleanse(context, rx_offset + dirty);
    OPENSSL_cleanse((uint8_t *)context + rx_end, sizeof(*context) - rx_end);
}

static void on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *user) {
    struct lsquic_stream_ctx *context = (struct lsquic_stream_ctx *)user;
    wt_session *session;
    (void)stream;
    if (!context) return;
    session = context->session;
    if (session && session->in_use) {
        if (session->connect_stream == context->stream)
            session_release(session, 1);
        else if (session->control == context)
            turn_release(session, 1);
    }
    header_release(context->headers);
    stream_slot_scrub(context);
}

static int audio_enqueue(wt_session *session, enum wt_audio_kind kind,
                         const uint8_t *data, size_t data_len, const char *reason) {
    wt_state *state;
    wt_audio_item *item;
    size_t session_index;
    uint64_t enqueued_ns;
    if (!session || !session->in_use || data_len > GW_WT_DATAGRAM_CAP ||
        (!data && data_len != 0)) return -1;
    state = session->state;
    if (state->audio_count >= WT_AUDIO_QUEUE) return -1;
    session_index = (size_t)(session - state->sessions);
    if (session_index >= WT_MAX_SESSIONS) return -1;
    enqueued_ns = monotonic_nanoseconds();
    if (enqueued_ns == 0u) return -1;
    item = &state->audio_queue[(state->audio_head + state->audio_count) % WT_AUDIO_QUEUE];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->session_index = (uint16_t)session_index;
    item->generation = session->generation;
    item->data_len = (uint16_t)data_len;
    item->enqueued_ns = enqueued_ns;
    if (data_len) memcpy(item->data, data, data_len);
    if (reason) snprintf(item->reason, sizeof(item->reason), "%s", reason);
    state->audio_count++;
    if (state->audio_count > session->audio_queue_high_water)
        session->audio_queue_high_water = state->audio_count;
    return 0;
}

static int audio_promote_contiguous(wt_session *session) {
    const uint8_t *payload;
    size_t payload_len;
    uint64_t now;
    int commit_state;
    uint32_t previous_sequence;
    if (!session || !session->in_use) return -1;
    previous_sequence = session->audio_reorder.next_sequence;
    while (gw_wt_audio_reorder_peek(&session->audio_reorder, &payload, &payload_len)) {
        if (audio_enqueue(session, WT_AUDIO_PCM, payload, payload_len, NULL) != 0 ||
            !gw_wt_audio_reorder_pop(&session->audio_reorder))
            return -1;
    }
    commit_state = session->end_pending
                       ? gw_wt_audio_reorder_commit_state(
                             &session->audio_reorder, session->expected_datagrams,
                             session->expected_audio_bytes)
                       : GW_WT_AUDIO_COMMIT_WAIT;
    if (commit_state == GW_WT_AUDIO_COMMIT_MISMATCH) return -2;
    if (!session->end_queued && commit_state == GW_WT_AUDIO_COMMIT_READY) {
        if (audio_enqueue(session, WT_AUDIO_END, NULL, 0u, NULL) != 0) return -1;
        session->end_queued = 1;
    }
    if ((!session->end_pending && session->audio_reorder.buffered_datagrams == 0u) ||
        session->end_queued) {
        session->audio_gap_deadline_ms = 0;
    } else if (session->audio_reorder.buffered_datagrams != 0u ||
               (session->end_pending &&
                session->audio_reorder.next_sequence < session->expected_datagrams)) {
        now = monotonic_milliseconds();
        if (now == 0u) return -1;
        session->audio_gap_deadline_ms = gw_wt_audio_gap_deadline(
            session->audio_gap_deadline_ms, previous_sequence,
            session->audio_reorder.next_sequence, 1, now);
    }
    return 0;
}

static void process_audio_input(wt_session *session, const gw_wt_datagram *datagram) {
    const char *reason = NULL;
    int inserted;
    int promoted;
    if (!datagram || !session || !session->authenticated || !session->audio_first || session->terminal) return;
    if (datagram->is_control) {
        if (datagram->control.request_id[0] &&
            strcmp(datagram->control.request_id, session->request_id) != 0) return;
        if (datagram->control.kind == GW_WT_CONTROL_END) {
            if (session->end_pending || session->end_queued || session->end_seen ||
                session->cancel_queued) return;
            if (gw_wt_audio_reorder_commit_state(
                    &session->audio_reorder, datagram->control.packet_count,
                    datagram->control.audio_bytes) == GW_WT_AUDIO_COMMIT_MISMATCH) {
                session_fail(session, "audio_commit_mismatch");
                return;
            }
            session->expected_datagrams = datagram->control.packet_count;
            session->expected_audio_bytes = datagram->control.audio_bytes;
            session->input_end_received_ns = monotonic_nanoseconds();
            if (session->input_end_received_ns == 0u) {
                session_fail(session, "edge_clock_failed");
                return;
            }
            session->end_pending = 1;
            promoted = audio_promote_contiguous(session);
            if (promoted != 0)
                session_fail(session, promoted == -2 ? "audio_commit_mismatch" :
                                                           "edge_backpressure");
        } else {
            if (session->cancel_sent || session->cancel_queued) return;
            reason = datagram->control.reason[0] ? datagram->control.reason : "client_interrupt";
            if (audio_enqueue(session, WT_AUDIO_CANCEL, NULL, 0u, reason) != 0)
                session_fail(session, "edge_backpressure");
            else
                session->cancel_queued = 1;
        }
        return;
    }
    if (!datagram->is_audio || session->end_queued || session->end_seen ||
        session->cancel_queued ||
        (session->end_pending && datagram->sequence >= session->expected_datagrams))
        return;
    inserted = gw_wt_audio_reorder_insert(&session->audio_reorder, datagram->sequence,
                                          datagram->payload, datagram->payload_len);
    if (inserted == GW_WT_AUDIO_DUPLICATE) return;
    if (inserted != GW_WT_AUDIO_INSERTED) {
        session_fail(
            session,
            inserted == GW_WT_AUDIO_OUTSIDE_WINDOW ? "audio_reorder_window_exceeded" :
            inserted == GW_WT_AUDIO_LIMIT ? "audio_input_too_large" :
                                            "audio_datagram_conflict");
        return;
    }
    if (session->audio_reorder.unique_datagrams == 1u) {
        session->input_started_ns = monotonic_nanoseconds();
        if (session->input_started_ns == 0u) {
            session_fail(session, "edge_clock_failed");
            return;
        }
    }
    promoted = audio_promote_contiguous(session);
    if (promoted != 0) {
        session_fail(session, promoted == -2 ? "audio_commit_mismatch" :
                                                   "edge_backpressure");
        return;
    }
}

static void on_datagram(lsquic_conn_t *conn, const void *buffer, size_t buffer_len) {
    struct lsquic_conn_ctx *connection = lsquic_conn_get_ctx(conn);
    gw_wt_datagram datagram;
    wt_session *session;
    if (!connection || gw_wt_datagram_parse(buffer, buffer_len, &datagram) != GW_WT_OK) return;
    session = session_find(connection->state, conn, datagram.session_stream_id);
    /* A negotiated stream has one input source. Datagrams can still interrupt it. */
    if (session && session->audio_stream && (!datagram.is_control || datagram.control.kind == GW_WT_CONTROL_END)) return;
    process_audio_input(session, &datagram);
}

static const struct lsquic_stream_if stream_interface = {
    .on_new_conn = on_new_conn,
    .on_conn_closed = on_conn_closed,
    .on_new_stream = on_new_stream,
    .on_read = on_read,
    .on_write = on_write,
    .on_close = on_close,
    .on_datagram = on_datagram,
};

static void *header_create(void *user, lsquic_stream_t *stream, int push) {
    wt_state *state = (wt_state *)user;
    size_t i;
    (void)stream;
    if (push) return NULL;
    for (i = 0; i < WT_MAX_HEADER_SETS; ++i) {
        if (state->header_sets[i].in_use) continue;
        /* State allocation and header_release keep every free slot zeroed. */
        state->header_sets[i].in_use = 1;
        return &state->header_sets[i];
    }
    return NULL;
}

static struct lsxpack_header *header_prepare(void *user, struct lsxpack_header *header,
                                             size_t required) {
    wt_header_set *set = (wt_header_set *)user;
    if (!set || !set->in_use) return NULL;
    if (header) return NULL;
    if (set->current.buf) {
        size_t previous = lsxpack_header_get_dec_size(&set->current);
        if (previous > sizeof(set->decode) - set->decode_offset) return NULL;
        set->decode_offset += previous;
    }
    if (required > sizeof(set->decode) - set->decode_offset) return NULL;
    lsxpack_header_prepare_decode(&set->current, set->decode, set->decode_offset,
                                  sizeof(set->decode) - set->decode_offset);
    return &set->current;
}

static int header_copy(char *out, size_t out_cap, unsigned *seen,
                       const char *value, size_t value_len) {
    if (!out || !seen || !value || *seen != 0u || value_len == 0 || value_len >= out_cap)
        return 1;
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    *seen = 1u;
    return 0;
}

static int header_process(void *user, struct lsxpack_header *header) {
    wt_header_set *set = (wt_header_set *)user;
    const char *name;
    const char *value;
    size_t name_len;
    size_t value_len;
    if (!set || !set->in_use) return -1;
    if (!header) return 0;
    name = lsxpack_header_get_name(header);
    value = lsxpack_header_get_value(header);
    name_len = header->name_len;
    value_len = header->val_len;
    if (name_len == sizeof(":method") - 1u &&
        memcmp(name, ":method", name_len) == 0)
        return header_copy(set->method, sizeof(set->method), &set->seen_method, value, value_len);
    if (name_len == sizeof(":path") - 1u && memcmp(name, ":path", name_len) == 0)
        return header_copy(set->path, sizeof(set->path), &set->seen_path, value, value_len);
    if (name_len == sizeof(":protocol") - 1u && memcmp(name, ":protocol", name_len) == 0)
        return header_copy(set->protocol, sizeof(set->protocol), &set->seen_protocol,
                           value, value_len);
    if (name_len == sizeof(":scheme") - 1u && memcmp(name, ":scheme", name_len) == 0)
        return header_copy(set->scheme, sizeof(set->scheme), &set->seen_scheme,
                           value, value_len);
    if (name_len == sizeof("origin") - 1u && memcmp(name, "origin", name_len) == 0)
        return header_copy(set->origin, sizeof(set->origin), &set->seen_origin,
                           value, value_len);
    if (name_len == sizeof(":authority") - 1u && memcmp(name, ":authority", name_len) == 0)
        return header_copy(set->authority, sizeof(set->authority), &set->seen_authority,
                           value, value_len);
    return 0;
}

static void header_discard(void *user) {
    header_release((wt_header_set *)user);
}

static const struct lsquic_hset_if header_interface = {
    .hsi_create_header_set = header_create,
    .hsi_prepare_decode = header_prepare,
    .hsi_process_header = header_process,
    .hsi_discard_header_set = header_discard,
};

static int packet_output(void *user, const struct lsquic_out_spec *specifications,
                         unsigned count) {
    wt_state *state = (wt_state *)user;
    unsigned i;
    if (count > (unsigned)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; ++i) {
        struct msghdr message;
        ssize_t sent;
        memset(&message, 0, sizeof(message));
        message.msg_name = (void *)specifications[i].dest_sa;
        message.msg_namelen = specifications[i].dest_sa->sa_family == AF_INET6
                                  ? (socklen_t)sizeof(struct sockaddr_in6)
                                  : (socklen_t)sizeof(struct sockaddr_in);
        message.msg_iov = specifications[i].iov;
#if defined(__APPLE__)
        if (specifications[i].iovlen > (size_t)INT_MAX) {
            errno = EINVAL;
            return i == 0u ? -1 : (int)i;
        }
        message.msg_iovlen = (int)specifications[i].iovlen;
#else
        message.msg_iovlen = specifications[i].iovlen;
#endif
        do {
            sent = sendmsg(state->socket_fd, &message, 0);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0) return i == 0 ? -1 : (int)i;
    }
    return (int)count;
}

static SSL_CTX *lookup_certificate(void *user, const struct sockaddr *local, const char *sni) {
    wt_state *state = (wt_state *)user;
    (void)local;
    (void)sni;
    return state->ssl_ctx;
}

static SSL_CTX *get_ssl_context(void *peer, const struct sockaddr *local) {
    wt_state *state = (wt_state *)peer;
    (void)local;
    return state ? state->ssl_ctx : NULL;
}

static int select_alpn(SSL *ssl, const unsigned char **out, unsigned char *out_len,
                       const unsigned char *input, unsigned int input_len, void *user) {
    wt_state *state = (wt_state *)user;
    int selected;
    (void)ssl;
    selected = SSL_select_next_proto((unsigned char **)out, out_len, input, input_len,
                                     (const unsigned char *)state->alpn, state->alpn_len);
    return selected == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

static int configure_alpn(wt_state *state, unsigned versions) {
    const char *const *names = lsquic_get_h3_alpns(versions);
    unsigned used = 0;
    while (*names) {
        size_t len = strlen(*names);
        if (len == 0 || len > 255u || len + 1u > sizeof(state->alpn) - used) return -1;
        state->alpn[used++] = (char)len;
        memcpy(state->alpn + used, *names, len);
        used += (unsigned)len;
        names++;
    }
    state->alpn_len = used;
    return used != 0 ? 0 : -1;
}

static int configure_tls(wt_state *state, const char *certificate, const char *key) {
    state->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!state->ssl_ctx || SSL_CTX_set_min_proto_version(state->ssl_ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(state->ssl_ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(state->ssl_ctx, certificate) != 1 ||
        SSL_CTX_use_PrivateKey_file(state->ssl_ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(state->ssl_ctx) != 1) return -1;
    SSL_CTX_set_alpn_select_cb(state->ssl_ctx, select_alpn, state);
    SSL_CTX_set_options(state->ssl_ctx, SSL_OP_NO_TICKET);
    return 0;
}

static int configure_allowed_origins(wt_state *state, const char *origins) {
    const char *cursor = origins;
    if (!state || !origins || !origins[0]) return -1;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t len = comma ? (size_t)(comma - cursor) : strlen(cursor);
        size_t i;
        if (state->allowed_origin_count >= WT_MAX_ALLOWED_ORIGINS || len < 9u || len >= 256u ||
            memcmp(cursor, "https://", sizeof("https://") - 1u) != 0 ||
            memchr(cursor + sizeof("https://") - 1u, '/', len - (sizeof("https://") - 1u)))
            return -1;
        for (i = 0; i < state->allowed_origin_count; ++i)
            if (strlen(state->allowed_origins[i]) == len &&
                memcmp(state->allowed_origins[i], cursor, len) == 0) return -1;
        memcpy(state->allowed_origins[state->allowed_origin_count], cursor, len);
        state->allowed_origins[state->allowed_origin_count][len] = '\0';
        state->allowed_origin_count++;
        if (!comma) break;
        cursor = comma + 1;
        if (!*cursor) return -1;
    }
    return state->allowed_origin_count != 0 ? 0 : -1;
}

static int configure_socket(wt_state *state, int port) {
    struct sockaddr_in6 address;
    int off = 0;
    int flags;
    state->socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (state->socket_fd < 0 ||
        setsockopt(state->socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0)
        return -1;
    flags = fcntl(state->socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(state->socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    const char *bind_address = svc_env("GATEWAY_WEBTRANSPORT_BIND_ADDRESS", "::");
    if (inet_pton(AF_INET6, bind_address, &address.sin6_addr) != 1) return -1;
    address.sin6_port = htons((uint16_t)port);
    if (bind(state->socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) return -1;
    state->local_address_len = sizeof(state->local_address);
    if (getsockname(state->socket_fd, (struct sockaddr *)&state->local_address,
                    &state->local_address_len) != 0) return -1;
    return 0;
}

/* The audio admission service publishes this committed transcript before it
 * starts the cascade. VBus preserves that order on this subscriber connection. */
static void on_audio_turn_start(const char *subject, const char *reply,
                                const uint8_t *data, size_t data_len, void *user) {
    wt_state *state = (wt_state *)user;
    turn_start_c turn;
    wt_session *session;
    char control[GW_WT_CONTROL_FRAME_CAP];
    size_t control_len;
    (void)subject;
    (void)reply;
    if (!state || pb_decode_turn_start(data, data_len, &turn) != 0) return;
    session = response_subject_session(state, turn.response_subject);
    if (!session || !session->audio_first ||
        session->cancel_queued || session->transcript_sent) return;
    if (gw_wt_transcript_control(&turn, session->request_id, session->session_id,
            session->user_id, control, sizeof(control), &control_len) != GW_WT_OK)
        return;
    session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                        (const uint8_t *)control, control_len);
    session->transcript_sent = 1;
}

static void on_model_capture(wt_state *state, const char *subject, const uint8_t *data, size_t length) {
    char original[GW_WT_EVENT_SUBJECT_LENGTH+1u];
    voice_model_request_chunk chunk;
    memcpy(original,subject,GW_WT_EVENT_SUBJECT_LENGTH); original[GW_WT_EVENT_SUBJECT_LENGTH]=0;
    wt_session *session=response_subject_session(state,original);
    if (!session || !session->capture_requested) return;
    if (voice_model_request_decode(data,length,&chunk)!=0 || strcmp(chunk.request_id,session->request_id)) {
        capture_release(session); session_fail(session,"invalid_model_capture"); return;
    }
    if (!session->capture.state) {
        if (voice_model_capture_begin(&session->capture,chunk.total_bytes)!=0 ||
            !(session->capture_bytes=malloc(chunk.total_bytes))) {
            capture_release(session); session_fail(session,"model_capture_capacity"); return;
        }
        session->capture_capacity=chunk.total_bytes;
    }
    size_t offset=session->capture.received_bytes;
    if (chunk.total_bytes!=session->capture.expected_bytes ||
        voice_model_capture_append(&session->capture,chunk.sequence,chunk.bytes,chunk.length,chunk.final)!=0) {
        capture_release(session); session_fail(session,"invalid_model_capture"); return;
    }
    memcpy(session->capture_bytes+offset,chunk.bytes,chunk.length);
    if (chunk.final) {
        session->capture_timestamp=realtime_seconds();
        if (voice_auth_nonce_pool_next(&state->capability_nonces,session->capture_nonce)!=VOICE_AUTH_OK ||
            voice_model_capture_sign(&session->capture,state->capture_secret,strlen(state->capture_secret),
                session->user_id,session->request_id,session->capture_timestamp,session->capture_nonce,
                session->capture_signature)!=0) {
            capture_release(session); session_fail(session,"model_capture_signing_failed"); return;
        }
        lsquic_stream_wantwrite(session->control->stream,1);
    }
}

static void on_turn_event(const char *subject, const char *reply, const uint8_t *data,
                          size_t data_len, void *user) {
    wt_state *state = (wt_state *)user;
    wt_session *session;
    turn_event_c event;
    turn_response_event safe_event;
    enum turn_response_action action;
    uint8_t wire[WT_EVENT_WIRE_BYTES];
    size_t wire_len;
    (void)reply;
    if (!state || !subject) return;
    if (strlen(subject)==GW_WT_EVENT_SUBJECT_LENGTH+sizeof(VOICE_MODEL_REQUEST_SUBJECT_SUFFIX)-1u &&
        !strcmp(subject+GW_WT_EVENT_SUBJECT_LENGTH,VOICE_MODEL_REQUEST_SUBJECT_SUFFIX)) {
        on_model_capture(state,subject,data,data_len); return;
    }
    session = response_subject_session(state, subject);
    if (!session) return;
    if (pb_decode_turn_event_public_active(
            data, data_len, session->request_id,
            session->request_id_len, &event) != 0) {
        session_fail(session, "invalid_turn_event");
        return;
    }
    action = turn_response_filter_public_active_n(
        &session->response, &event, session->request_id,
        session->request_id_len, &safe_event);
    if (action == TURN_RESPONSE_DROP) return;
    if (action == TURN_RESPONSE_REJECT) {
        session_fail(session, "invalid_turn_event");
        return;
    }
    if (action == TURN_RESPONSE_HOLD_COMPLETED) return;
    wire_len = turn_response_protobuf_encode(wire, sizeof(wire), &safe_event);
    if (wire_len == 0) {
        session_fail(session, "turn_event_encoding_failed");
        return;
    }
    session_queue_frame(session, TURN_FRAME_TURN_EVENT_PROTO, wire, wire_len);
    {
        struct lsquic_conn_ctx *context = lsquic_conn_get_ctx(session->conn);
        if (!context || context->abort_reason) {
            voice_analytics_finish(state->analytics, &session->analytics,
                VOICE_ANALYTICS_FAILED, monotonic_nanoseconds());
            return;
        }
    }
    if (safe_event.audio_len && session->analytics.admitted_ns && !session->analytics.first_audio_ns)
        voice_analytics_audio(&session->analytics, monotonic_nanoseconds());
    if (action == TURN_RESPONSE_FORWARD_AND_COMPLETE) {
        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), safe_event.request_id, "completed", "");
        if (wire_len == 0) {
            session_fail(session, "turn_event_encoding_failed");
            return;
        }
        session_queue_frame(session, TURN_FRAME_TURN_EVENT_PROTO, wire, wire_len);
        session->terminal = 1;
        {
            struct lsquic_conn_ctx *context = lsquic_conn_get_ctx(session->conn);
            voice_analytics_finish(state->analytics, &session->analytics,
                context && !context->abort_reason ? VOICE_ANALYTICS_COMPLETED : VOICE_ANALYTICS_FAILED,
                monotonic_nanoseconds());
        }
    } else if (action == TURN_RESPONSE_FORWARD_TERMINAL) {
        session->terminal = 1;
        voice_analytics_finish(state->analytics, &session->analytics,
            safe_event.type_id == 10 ? VOICE_ANALYTICS_COMPLETED :
            safe_event.type_id == 11 ? VOICE_ANALYTICS_CANCELED : VOICE_ANALYTICS_FAILED,
            monotonic_nanoseconds());
    }
}

static void on_reflex_event(const char *subject, const char *reply, const uint8_t *data,
                            size_t data_len, void *user) {
    wt_state *state = (wt_state *)user;
    wt_session *session;
    turn_event_c event;
    uint64_t observed_ns;
    int marked;
    (void)reply;
    if (!state || !subject || !data || data_len == 0u) return;
    session = reflex_subject_session(state, subject);
    if (!session || session->end_pending || session->end_queued ||
        session->end_seen || session->cancel_queued ||
        session->audio_delivery.forwarded_datagrams == 0u)
        return;
    if (pb_decode_turn_event_bound(
            data, data_len, session->request_id,
            session->request_id_len, &event) != 0 ||
        event.type_id != WT_REFLEX_ENDPOINT_TYPE_ID)
        return;
    marked = gw_wt_audio_delivery_mark_endpoint(&session->audio_delivery);
    if (marked <= 0) return;
    observed_ns = monotonic_nanoseconds();
    if (observed_ns == 0u) {
        session_fail(session, "edge_clock_failed");
        return;
    }
    session->input_endpoint_received_ns = observed_ns;
    session_queue_input_endpoint(session);
}

static void drain_audio(wt_state *state) {
    while (state->audio_count != 0) {
        wt_audio_item *item = &state->audio_queue[state->audio_head];
        wt_session *session = item->session_index < WT_MAX_SESSIONS
                                  ? &state->sessions[item->session_index] : NULL;
        if (session && session->in_use && !session->terminal &&
            session->generation == item->generation) {
            if (item->kind == WT_AUDIO_PCM) {
                char subject[256];
                uint64_t publish_started;
                uint64_t publish_ended;
                uint64_t queue_us;
                uint64_t publish_us;
                int forwarded = gw_wt_audio_delivery_should_forward(
                    &session->audio_delivery);
                int publish_status;
                int written = snprintf(
                    subject, sizeof(subject), "%s.%s",
                    SUBJ_VOICE_PCM_PFX, session->session_id);
                publish_started = monotonic_nanoseconds();
                publish_status = written <= 0 || (size_t)written >= sizeof(subject) ||
                                         publish_started == 0u
                                     ? -1
                                     : forwarded
                                           ? vbus_publish(
                                                 state->bus, subject,
                                                 item->data, item->data_len)
                                           : 0;
                publish_ended = monotonic_nanoseconds();
                if (publish_status != 0 || publish_ended == 0u) {
                    abort_connection(session->conn, "audio_publish_failed");
                } else {
                    queue_us = elapsed_microseconds(item->enqueued_ns, publish_started);
                    publish_us = elapsed_microseconds(publish_started, publish_ended);
                    if (queue_us > session->audio_queue_max_us)
                        session->audio_queue_max_us = queue_us;
                    if (forwarded) session->audio_publish_us += publish_us;
                    if (gw_wt_audio_delivery_record(
                            &session->audio_delivery, forwarded,
                            item->data_len) != GW_WT_OK)
                        session_fail(session, "audio_delivery_mismatch");
                }
            } else if (item->kind == WT_AUDIO_END) {
                char subject[256];
                uint64_t publish_started;
                uint64_t publish_ended;
                uint64_t queue_us;
                int written = snprintf(
                    subject, sizeof(subject), "%s.%s",
                    SUBJ_VOICE_PCM_PFX, session->session_id);
                publish_started = monotonic_nanoseconds();
                if (written <= 0 || (size_t)written >= sizeof(subject) ||
                    publish_started == 0u ||
                    vbus_publish(state->bus, subject, NULL, 0u) != 0 ||
                    (publish_ended = monotonic_nanoseconds()) == 0u) {
                    abort_connection(session->conn, "audio_end_publish_failed");
                } else if (!gw_wt_audio_delivery_matches(
                               &session->audio_delivery,
                               session->expected_datagrams,
                               session->expected_audio_bytes)) {
                    session_fail(session, "audio_delivery_mismatch");
                } else {
                    queue_us = elapsed_microseconds(item->enqueued_ns, publish_started);
                    if (queue_us > session->audio_queue_max_us)
                        session->audio_queue_max_us = queue_us;
                    session->audio_publish_us += elapsed_microseconds(
                        publish_started, publish_ended);
                    session->end_seen = 1;
                    session->audio_gap_deadline_ms = 0;
                    session->input_commit_us = elapsed_microseconds(
                        session->input_end_received_ns, publish_ended);
                    session_queue_input_commit(session);
                }
            } else if (item->kind == WT_AUDIO_CANCEL) {
                session->analytics.client_interrupt = 1;
                publish_cancel(session, item->reason[0] ? item->reason : "client_interrupt");
                if (session->audio_stream && session->cancel_sent) {
                    char ack[320];
                    int written = snprintf(ack, sizeof(ack),
                        "{\"type\":\"interrupt_ack\",\"protocol_version\":\"%s\","
                        "\"request_id\":\"%s\",\"delivery\":\"vbus_publish\"}",
                        TURN_PROTOCOL_VERSION, session->request_id);
                    if (written > 0 && (size_t)written < sizeof(ack))
                        session_queue_frame(session, TURN_FRAME_CONTROL_JSON,
                            (const uint8_t *)ack, (size_t)written);
                }
            }
        }
        OPENSSL_cleanse(item, sizeof(*item));
        state->audio_head = (state->audio_head + 1u) % WT_AUDIO_QUEUE;
        state->audio_count--;
    }
}

static void expire_audio_gaps(wt_state *state, uint64_t now) {
    size_t i;
    if (!state || now == 0u) return;
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        wt_session *session = &state->sessions[i];
        if (session->in_use && !session->terminal && session->audio_gap_deadline_ms != 0u &&
            now >= session->audio_gap_deadline_ms)
            session_fail(session, "audio_datagram_gap");
    }
}

static void emit_response_keepalives(wt_state *state, uint64_t now) {
    size_t i;
    if (!state || now == 0u) return;
    for (i = 0; i < WT_MAX_SESSIONS; ++i) {
        wt_session *session = &state->sessions[i];
        char json[320];
        int written;
        if (!session->in_use || !session->authenticated || session->terminal ||
            session->response_keepalive_deadline_ms == 0u ||
            session->response_keepalive_deadline_ms > now) continue;
        written = snprintf(
            json, sizeof(json),
            "{\"type\":\"keepalive\",\"protocol_version\":\"%s\","
            "\"request_id\":\"%s\"}",
            TURN_PROTOCOL_VERSION, session->request_id);
        if (written <= 0 || (size_t)written >= sizeof(json)) {
            session_fail(session, "keepalive_encoding_failed");
            continue;
        }
        session_queue_frame(
            session, TURN_FRAME_CONTROL_JSON,
            (const uint8_t *)json, (size_t)written);
    }
}

static int run_loop(wt_state *state) {
    struct pollfd descriptors[2];
    uint8_t packet[65536];
    int bus_fd;
    if (!state || !state->bus) return -1;
    bus_fd = vbus_poll_fd(state->bus);
    if (bus_fd < 0) return -1;
    descriptors[0].fd = state->socket_fd;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = bus_fd;
    descriptors[1].events = POLLIN;
    while (atomic_load_explicit(&state->stop, memory_order_relaxed) == 0) {
        int diff = 10000;
        int timeout_ms;
        int poll_result;
        if (lsquic_engine_earliest_adv_tick(state->engine, &diff)) {
            if (diff < 0) diff = 0;
            if (diff > 10000) diff = 10000;
        }
        timeout_ms = (diff + 999) / 1000;
        descriptors[0].revents = 0;
        descriptors[1].revents = 0;
        do {
            poll_result = poll(descriptors, 2, timeout_ms);
        } while (poll_result < 0 && errno == EINTR &&
                 atomic_load_explicit(&state->stop, memory_order_relaxed) == 0);
        if (poll_result < 0) {
            if (errno == EINTR &&
                atomic_load_explicit(&state->stop, memory_order_relaxed) != 0)
                break;
            return -1;
        }
        state->loop_now_ms = monotonic_milliseconds();
        if (state->loop_now_ms == 0u) return -1;
        if (descriptors[0].revents & POLLIN) {
            for (;;) {
                struct sockaddr_storage peer;
                socklen_t peer_len = sizeof(peer);
                ssize_t received = recvfrom(state->socket_fd, packet, sizeof(packet), 0,
                                            (struct sockaddr *)&peer, &peer_len);
                if (received < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    return -1;
                }
                if (received != 0)
                    (void)lsquic_engine_packet_in(
                        state->engine, packet, (size_t)received,
                        (const struct sockaddr *)&state->local_address,
                        (const struct sockaddr *)&peer, state, 0);
            }
        }
        expire_audio_gaps(state, state->loop_now_ms);
        drain_audio(state);
        if (vbus_poll(state->bus, 0) != 0) return -1;
        emit_response_keepalives(state, state->loop_now_ms);
        lsquic_engine_process_conns(state->engine);
        lsquic_engine_send_unsent_packets(state->engine);
    }
    return 0;
}

int main(void) {
    wt_state *state = NULL;
    struct lsquic_engine_settings settings;
    struct lsquic_engine_api api;
    const char *certificate = getenv("GATEWAY_WEBTRANSPORT_CERT_FILE");
    const char *key = getenv("GATEWAY_WEBTRANSPORT_KEY_FILE");
    const char *secret = getenv("VOICE_GATEWAY_TOKEN");
    const char *path = getenv("GATEWAY_WEBTRANSPORT_PATH");
    const char *authority = getenv("GATEWAY_WEBTRANSPORT_AUTHORITY");
    const char *allowed_origins = getenv("GATEWAY_WEBTRANSPORT_ALLOWED_ORIGINS");
    int port = svc_env_int_range("GATEWAY_WEBTRANSPORT_PORT", 8443, 1, 65535);
    int response_keepalive_ms = svc_env_int_range(
        "GATEWAY_WEBTRANSPORT_KEEPALIVE_MS",
        (int)WT_DEFAULT_RESPONSE_KEEPALIVE_MS, 100, 30000);
    int result = 1;
    int lsquic_initialized = 0;
    char settings_error[256];
    if (!allowed_origins)
        allowed_origins = getenv("GATEWAY_WEBTRANSPORT_ALLOWED_ORIGIN");
    if (!certificate || !certificate[0] || !key || !key[0] || !secret ||
        strlen(secret) < 32u || strlen(secret) > 255u ||
        !allowed_origins || !authority || !authority[0] || strlen(authority) >= 256u ||
        strchr(authority, '/') || strchr(authority, ' ') ||
        (path && (path[0] != '/' || strlen(path) >= sizeof(state->path)))) {
        svc_log("webtransport-gateway",
                "TLS files, signing token, exact authority, and HTTPS origin are required");
        return 1;
    }
    state = calloc(1, sizeof(*state));
    if (!state) return 1;
    state->socket_fd = -1;
    state->response_keepalive_interval_ms = (uint64_t)response_keepalive_ms;
    snprintf(state->path, sizeof(state->path), "%s", path && path[0] ? path : WT_DEFAULT_PATH);
    snprintf(state->authority, sizeof(state->authority), "%s", authority);
    state->capture_secret = secret;
    if (configure_allowed_origins(state, allowed_origins) != 0) {
        svc_log("webtransport-gateway", "HTTPS origin allowlist is invalid");
        goto cleanup;
    }
    if (voice_auth_verifier_init(
            &state->auth_verifier, secret, strlen(secret)) != VOICE_AUTH_OK) {
        svc_log("webtransport-gateway", "identity verifier initialization failed");
        goto cleanup;
    }
    svc_install_signals(&state->stop);
    if (voice_auth_nonce_index_init(&state->nonce_cache, 4096u) != VOICE_AUTH_OK) {
        svc_log("webtransport-gateway", "nonce cache initialization failed");
        goto cleanup;
    }
    if (voice_auth_nonce_pool_init(&state->capability_nonces) != VOICE_AUTH_OK) {
        svc_log("webtransport-gateway", "CSPRNG initialization failed");
        goto cleanup;
    }
    if (configure_socket(state, port) != 0) {
        svc_log("webtransport-gateway", "UDP bind failed port=%d errno=%d", port, errno);
        goto cleanup;
    }
    lsquic_logger_init(&wt_logger, NULL, LLTS_YYYYMMDD_HHMMSSMS);
    if (lsquic_set_log_level("warn") != 0) {
        svc_log("webtransport-gateway", "LSQUIC log configuration failed");
        goto cleanup;
    }
    if (lsquic_global_init(LSQUIC_GLOBAL_SERVER) != 0) {
        svc_log("webtransport-gateway", "LSQUIC global initialization failed");
        goto cleanup;
    }
    lsquic_initialized = 1;
    lsquic_engine_init_settings(&settings, LSENG_HTTP_SERVER);
    /* LSQUIC 4.9.3 emits no usable HTTP/3 ALPN for QUIC v2. Keep the
     * browser edge on the interoperable RFC 9000 v1 wire version. */
    settings.es_versions = 1u << LSQVER_I001;
    settings.es_datagrams = 1;
    settings.es_webtransport_server = 1;
    settings.es_max_webtransport_server_streams = 1u;
    settings.es_idle_timeout = 30u;
    settings.es_ping_period = 15u;
    settings.es_max_header_list_size = WT_HEADER_BYTES;
    settings.es_max_streams_in = WT_MAX_STREAMS;
    memset(settings_error, 0, sizeof(settings_error));
    if (lsquic_engine_check_settings(&settings, LSENG_HTTP_SERVER,
                                     settings_error, sizeof(settings_error)) != 0) {
        svc_log("webtransport-gateway", "invalid LSQUIC settings: %s", settings_error);
        goto cleanup;
    }
    if (configure_alpn(state, settings.es_versions) != 0) {
        svc_log("webtransport-gateway", "HTTP/3 ALPN configuration failed");
        goto cleanup;
    }
    if (configure_tls(state, certificate, key) != 0) {
        svc_log("webtransport-gateway", "TLS certificate configuration failed");
        goto cleanup;
    }
    memset(&api, 0, sizeof(api));
    api.ea_settings = &settings;
    api.ea_stream_if = &stream_interface;
    api.ea_stream_if_ctx = state;
    api.ea_packets_out = packet_output;
    api.ea_packets_out_ctx = state;
    api.ea_lookup_cert = lookup_certificate;
    api.ea_cert_lu_ctx = state;
    api.ea_get_ssl_ctx = get_ssl_context;
    api.ea_hsi_if = &header_interface;
    api.ea_hsi_ctx = state;
    state->engine = lsquic_engine_new(LSENG_HTTP_SERVER, &api);
    if (!state->engine) {
        svc_log("webtransport-gateway", "LSQUIC engine initialization failed");
        goto cleanup;
    }
    state->bus = svc_connect_bus();
    if (!state->bus) {
        svc_log("webtransport-gateway", "VBus connection failed");
        goto cleanup;
    }
    if (vbus_subscribe(
            state->bus, GW_WT_EVENT_SUBJECT_PREFIX ">", NULL,
            on_turn_event, state) != 0) {
        svc_log("webtransport-gateway", "turn event subscription failed");
        goto cleanup;
    }
    if (vbus_subscribe(
            state->bus, SUBJ_VOICE_REFLEX_PFX ".>", NULL,
            on_reflex_event, state) != 0) {
        svc_log("webtransport-gateway", "reflex event subscription failed");
        goto cleanup;
    }
    if (vbus_subscribe(state->bus, SUBJ_TURN_START, NULL,
                       on_audio_turn_start, state) != 0) {
        svc_log("webtransport-gateway", "audio transcript subscription failed");
        goto cleanup;
    }
    state->analytics = voice_analytics_start(getenv("VOICE_ANALYTICS_OTLP_URL"), "c-webtransport-gateway");
    svc_log("webtransport-gateway", "pure-C WebTransport + datagrams UDP :%d", port);
    result = run_loop(state) == 0 ? 0 : 1;
cleanup:
    if (state) {
        if (state->engine) lsquic_engine_destroy(state->engine);
        voice_analytics_stop(state->analytics);
        if (state->bus) vbus_close(state->bus);
        if (state->ssl_ctx) SSL_CTX_free(state->ssl_ctx);
        if (state->socket_fd >= 0) close(state->socket_fd);
        voice_auth_verifier_destroy(&state->auth_verifier);
        voice_auth_nonce_pool_destroy(&state->capability_nonces);
        voice_auth_nonce_index_destroy(&state->nonce_cache);
        OPENSSL_cleanse(state, sizeof(*state));
        free(state);
    }
    if (lsquic_initialized) lsquic_global_cleanup();
    return result;
}
