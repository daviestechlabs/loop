/* turn_frame.h — pure-C binary frame + turn event name helpers.
 *
 * Mirrors contracts/handler-base/turnstream WriteFrame/ReadFrame and
 * EventName/IsTerminalEvent pure policy (no NDJSON/Go types).
 */
#ifndef HANDLER_BASE_TURN_FRAME_H
#define HANDLER_BASE_TURN_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURN_FRAME_OK = 0,
    TURN_FRAME_ERR_ARGUMENT = 1,
    TURN_FRAME_ERR_CAPACITY = 2,
    TURN_FRAME_ERR_IO = 3
};

/* Frame layout: 1 byte type + 4 byte big-endian length + payload. */
int turn_frame_encode_v1(
    uint8_t frame_type,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
);

int turn_frame_decode_v1(
    const uint8_t *in,
    size_t in_len,
    uint8_t *frame_type,
    const uint8_t **payload,
    size_t *payload_len,
    size_t *consumed
);

/* Map numeric TurnEventType codes / string tokens to stable event names. */
const char *turn_event_name_v1(const char *type_token);

/*
 * Map type token/name/id → messages.TurnEventType enum id (1..12).
 * Returns 0 for unspecified/unknown.
 */
int turn_event_type_id_v1(const char *type_token);

/* Map AgentTaskEventType codes / tokens → agent_task_* names. */
const char *turn_agent_task_event_name_v1(const char *type_token);

/* Terminal turn events: completed / failed / canceled. */
int turn_event_is_terminal_v1(const char *type_token);

/* Terminal agent-task events: agent_task_completed / failed / canceled. */
int turn_agent_task_event_is_terminal_v1(const char *type_token);

/* Frame type constants (match product turnstream). */
enum {
    TURN_FRAME_CONTROL_JSON = 0x01,
    TURN_FRAME_TURN_EVENT_PROTO = 0x02
};

#define TURN_FRAME_MAX_BYTES (1u << 20)
#define TURN_PROTOCOL_VERSION "turnstream.v1alpha1"
#define TURN_CONTROL_ACCEPTED "accepted"
#define TURN_CONTROL_ERROR "error"

#ifdef __cplusplus
}
#endif

#endif /* HANDLER_BASE_TURN_FRAME_H */
