/* Bounded protocol core for the pure-C WebTransport edge. */
#ifndef VOICE_C_GATEWAY_WEBTRANSPORT_CORE_H
#define VOICE_C_GATEWAY_WEBTRANSPORT_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../common/byte_ring.h"
#include "../wire/pb_min.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_WT_CONTROL_FRAME_CAP (16u * 1024u)
#define GW_WT_DATAGRAM_CAP 1200u
#define GW_WT_AUDIO_PREFIX "DTVP1"
#define GW_WT_AUDIO_HEADER_BYTES 9u
#define GW_WT_AUDIO_PAYLOAD_CAP (GW_WT_DATAGRAM_CAP - GW_WT_AUDIO_HEADER_BYTES)
#define GW_WT_AUDIO_REORDER_WINDOW 32u
#define GW_WT_AUDIO_GAP_TIMEOUT_MS 250u
#define GW_WT_FRAME_AUDIO_PACKET 0x03u
#define GW_WT_MAX_AUDIO_BYTES (4u * 1024u * 1024u)
#define GW_WT_RESPONSE_QUEUE_CAP (256u * 1024u)
#define GW_WT_EVENT_SUBJECT_PREFIX "ai.turn.events.cwt."
#define GW_WT_EVENT_NONCE_HEX_LEN 32u
#define GW_WT_EVENT_SUBJECT_LENGTH \
    (sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u + 2u + 1u + \
     GW_WT_EVENT_NONCE_HEX_LEN)

enum {
    GW_WT_OK = 0,
    GW_WT_ERR_ARGUMENT = 1,
    GW_WT_ERR_MALFORMED = 2,
    GW_WT_ERR_CAPACITY = 3
};

/* Project a committed, identity-bound audio turn into the browser control frame. */
int gw_wt_transcript_control(
    const turn_start_c *turn, const char *request_id, const char *session_id,
    const char *user_id, char *out, size_t out_cap, size_t *out_len
);

typedef struct {
    char request_id[128];
    char session_id[128];
    char text[2048];
    char identity_token[1024];
    int enable_rag;
    int enable_tts;
    int audio_first;
    int audio_stream;
    turn_client_metadata_c metadata;
    dnd_initiative_request_c dnd_initiative;
    dnd_campaign_request_c dnd_campaign;
    dnd_encounter_action_c dnd_encounter_action;
    char meta_budget_ms[32];
    char meta_deadline_unix_ms[32];
} gw_wt_turn_request;

enum gw_wt_control_kind {
    GW_WT_CONTROL_NONE = 0,
    GW_WT_CONTROL_END,
    GW_WT_CONTROL_CANCEL,
    GW_WT_CONTROL_INTERRUPT
};

typedef struct {
    enum gw_wt_control_kind kind;
    char request_id[128];
    char reason[128];
    uint32_t packet_count;
    uint32_t audio_bytes;
} gw_wt_control;

typedef struct {
    uint64_t session_stream_id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t sequence;
    int is_audio;
    int is_control;
    gw_wt_control control;
} gw_wt_datagram;

enum gw_wt_audio_insert_result {
    GW_WT_AUDIO_INSERTED = 0,
    GW_WT_AUDIO_DUPLICATE = 1,
    GW_WT_AUDIO_CONFLICT = 2,
    GW_WT_AUDIO_OUTSIDE_WINDOW = 3,
    GW_WT_AUDIO_LIMIT = 4,
    GW_WT_AUDIO_INVALID = 5
};

typedef struct {
    uint32_t sequence;
    uint16_t payload_len;
    uint8_t present;
    uint8_t payload[GW_WT_AUDIO_PAYLOAD_CAP];
} gw_wt_audio_slot;

typedef struct {
    gw_wt_audio_slot slots[GW_WT_AUDIO_REORDER_WINDOW];
    uint32_t next_sequence;
    uint32_t highest_sequence_plus_one;
    uint32_t unique_datagrams;
    uint32_t duplicate_datagrams;
    uint32_t reordered_datagrams;
    uint32_t buffered_datagrams;
    size_t unique_audio_bytes;
} gw_wt_audio_reorder;

/* Split admitted PCM into the downstream prefix and post-endpoint drain. */
typedef struct {
    uint32_t forwarded_datagrams;
    uint32_t drained_datagrams;
    size_t forwarded_audio_bytes;
    size_t drained_audio_bytes;
    int endpoint_seen;
} gw_wt_audio_delivery;

typedef struct {
    uint8_t data[GW_WT_RESPONSE_QUEUE_CAP];
    size_t head;
    size_t size;
} gw_wt_byte_ring;

_Static_assert(
    (GW_WT_RESPONSE_QUEUE_CAP & (GW_WT_RESPONSE_QUEUE_CAP - 1u)) == 0u,
    "WebTransport response queue capacity must be a power of two");
_Static_assert(
    GW_WT_RESPONSE_QUEUE_CAP == 4u * VOICE_BYTE_RING_CAPACITY,
    "WebTransport response queue must provide four HTTP queue windows");

/* Write one random capability subject with a two-hex-digit routing slot. */
int gw_wt_response_subject_write(
    char *out,
    size_t out_cap,
    size_t slot,
    const char nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u]
);

/* Parse the routing slot from one fixed-length capability subject. */
int gw_wt_response_subject_slot(const char *subject, size_t *slot);

/* Parse one strict top-level browser request object. */
int gw_wt_turn_request_parse(
    const uint8_t *json,
    size_t json_len,
    gw_wt_turn_request *out
);

/* Parse for the active gateway without clearing unused array tails.
 * Every logical member is initialized on success. Do not inspect out on failure. */
int gw_wt_turn_request_parse_active(
    const uint8_t *json,
    size_t json_len,
    gw_wt_turn_request *out
);

/* Decode a canonical QUIC variable-length integer. */
int gw_wt_quic_varint_decode(
    const uint8_t *input,
    size_t input_len,
    uint64_t *value,
    size_t *consumed
);

/* Decode the HTTP Datagram quarter-stream ID and bounded voice payload. */
int gw_wt_datagram_parse(
    const uint8_t *input,
    size_t input_len,
    gw_wt_datagram *out
);

/* Parse one authenticated stream frame. Control frames require a request ID. */
int gw_wt_stream_input_parse(uint8_t frame_type, const uint8_t *payload,
    size_t payload_len, gw_wt_datagram *out);

/* Insert one sequenced PCM datagram into a fixed, allocation-free window. */
int gw_wt_audio_reorder_insert(
    gw_wt_audio_reorder *window,
    uint32_t sequence,
    const uint8_t *payload,
    size_t payload_len
);

/* Return the next contiguous PCM item without removing it. */
int gw_wt_audio_reorder_peek(
    const gw_wt_audio_reorder *window,
    const uint8_t **payload,
    size_t *payload_len
);

/* Remove the item returned by gw_wt_audio_reorder_peek. */
int gw_wt_audio_reorder_pop(gw_wt_audio_reorder *window);

/* Bound each stalled contiguous prefix, without extending a persistent gap. */
uint64_t gw_wt_audio_gap_deadline(
    uint64_t deadline_ms,
    uint32_t previous_sequence,
    uint32_t next_sequence,
    int has_gap,
    uint64_t now_ms
);

enum {
    GW_WT_AUDIO_COMMIT_WAIT = 0,
    GW_WT_AUDIO_COMMIT_READY = 1,
    GW_WT_AUDIO_COMMIT_MISMATCH = -1
};

/* Validate final client totals against the contiguous delivery window. */
int gw_wt_audio_reorder_commit_state(
    const gw_wt_audio_reorder *window,
    uint32_t expected_datagrams,
    uint32_t expected_audio_bytes
);

/* Mark the first server endpoint. Return one once and zero for a duplicate. */
int gw_wt_audio_delivery_mark_endpoint(gw_wt_audio_delivery *delivery);

/* Return one while admitted PCM still belongs on the downstream voice bus. */
int gw_wt_audio_delivery_should_forward(const gw_wt_audio_delivery *delivery);

/* Account for one successfully forwarded or intentionally drained PCM item. */
int gw_wt_audio_delivery_record(
    gw_wt_audio_delivery *delivery,
    int forwarded,
    size_t audio_bytes
);

/* Prove that the delivery split covers the complete client receipt. */
int gw_wt_audio_delivery_matches(
    const gw_wt_audio_delivery *delivery,
    uint32_t received_datagrams,
    size_t received_audio_bytes
);

/* Atomically append two byte spans to a fixed circular queue. */
static inline int gw_wt_byte_ring_write_pair(
    gw_wt_byte_ring *ring,
    const uint8_t *first,
    size_t first_len,
    const uint8_t *second,
    size_t second_len
) {
    return ring && voice_byte_ring_write_pair_bounded(
        ring->data, GW_WT_RESPONSE_QUEUE_CAP, &ring->head, &ring->size,
        first, first_len, second, second_len);
}

/* Return the scrub prefix after a successful write from the current tail. */
static inline size_t gw_wt_byte_ring_dirty_after_write(
    const gw_wt_byte_ring *ring,
    size_t dirty_bytes,
    size_t written_bytes
) {
    return ring ? voice_byte_ring_dirty_after_write_bounded(
        dirty_bytes, ring->head, ring->size, written_bytes,
        GW_WT_RESPONSE_QUEUE_CAP) : GW_WT_RESPONSE_QUEUE_CAP;
}

/* Atomically append one turn-stream frame without an intermediate payload copy. */
static inline int gw_wt_byte_ring_write_frame(
    gw_wt_byte_ring *ring,
    uint8_t frame_type,
    const uint8_t *payload,
    size_t payload_len
) {
    uint8_t header[5];
    if ((payload_len != 0u && !payload) ||
        payload_len > GW_WT_RESPONSE_QUEUE_CAP - sizeof(header))
        return 0;
    header[0] = frame_type;
    header[1] = (uint8_t)((payload_len >> 24u) & 0xffu);
    header[2] = (uint8_t)((payload_len >> 16u) & 0xffu);
    header[3] = (uint8_t)((payload_len >> 8u) & 0xffu);
    header[4] = (uint8_t)(payload_len & 0xffu);
    return gw_wt_byte_ring_write_pair(ring, header, sizeof(header), payload, payload_len);
}

/* Return the next contiguous readable span. */
static inline int gw_wt_byte_ring_peek(
    const gw_wt_byte_ring *ring,
    const uint8_t **data,
    size_t *data_len
) {
    return ring && voice_byte_ring_peek_bounded(
        ring->data, GW_WT_RESPONSE_QUEUE_CAP, ring->head, ring->size,
        data, data_len);
}

/* Consume bytes from the span returned by gw_wt_byte_ring_peek. */
static inline int gw_wt_byte_ring_consume(gw_wt_byte_ring *ring, size_t data_len) {
    return ring && voice_byte_ring_consume_bounded(
        GW_WT_RESPONSE_QUEUE_CAP, &ring->head, &ring->size, data_len);
}

#ifdef __cplusplus
}
#endif

#endif
