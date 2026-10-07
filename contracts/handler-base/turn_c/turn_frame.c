/* turn_frame.c — pure-C turnstream framing + event name policy. */

#include "turn_frame.h"

#include <string.h>

int turn_frame_encode_v1(
    uint8_t frame_type,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len
) {
    size_t need = 5 + payload_len;
    if (!out || !out_len) {
        return TURN_FRAME_ERR_ARGUMENT;
    }
    *out_len = 0;
    if (payload_len > 0 && !payload) {
        return TURN_FRAME_ERR_ARGUMENT;
    }
    if (out_cap < need) {
        return TURN_FRAME_ERR_CAPACITY;
    }
    out[0] = frame_type;
    out[1] = (uint8_t)((payload_len >> 24) & 0xff);
    out[2] = (uint8_t)((payload_len >> 16) & 0xff);
    out[3] = (uint8_t)((payload_len >> 8) & 0xff);
    out[4] = (uint8_t)(payload_len & 0xff);
    if (payload_len) {
        memcpy(out + 5, payload, payload_len);
    }
    *out_len = need;
    return TURN_FRAME_OK;
}

int turn_frame_decode_v1(
    const uint8_t *in,
    size_t in_len,
    uint8_t *frame_type,
    const uint8_t **payload,
    size_t *payload_len,
    size_t *consumed
) {
    size_t len;
    if (!in || !frame_type || !payload || !payload_len || !consumed) {
        return TURN_FRAME_ERR_ARGUMENT;
    }
    *consumed = 0;
    *payload = NULL;
    *payload_len = 0;
    if (in_len < 5) {
        return TURN_FRAME_ERR_IO;
    }
    *frame_type = in[0];
    len = ((size_t)in[1] << 24) | ((size_t)in[2] << 16) | ((size_t)in[3] << 8) | (size_t)in[4];
    if (in_len < 5 + len) {
        return TURN_FRAME_ERR_IO;
    }
    *payload_len = len;
    *payload = len ? (in + 5) : NULL;
    *consumed = 5 + len;
    return TURN_FRAME_OK;
}

const char *turn_event_name_v1(const char *type_token) {
    if (!type_token || !type_token[0]) {
        return "unknown";
    }
    /* Numeric TurnEventType enum values from messages.proto */
    if (strcmp(type_token, "0") == 0) return "unknown";
    if (strcmp(type_token, "1") == 0 || strcmp(type_token, "started") == 0 ||
        strcmp(type_token, "turn_started") == 0 || strcmp(type_token, "TURN_EVENT_STARTED") == 0) {
        return "started";
    }
    if (strcmp(type_token, "2") == 0 || strcmp(type_token, "thinking_started") == 0 ||
        strcmp(type_token, "TURN_EVENT_THINKING_STARTED") == 0) {
        return "thinking_started";
    }
    if (strcmp(type_token, "3") == 0 || strcmp(type_token, "thinking_ended") == 0 ||
        strcmp(type_token, "TURN_EVENT_THINKING_ENDED") == 0) {
        return "thinking_ended";
    }
    if (strcmp(type_token, "4") == 0 || strcmp(type_token, "text_delta") == 0 ||
        strcmp(type_token, "TURN_EVENT_TEXT_DELTA") == 0) {
        return "text_delta";
    }
    if (strcmp(type_token, "5") == 0 || strcmp(type_token, "text_completed") == 0 ||
        strcmp(type_token, "TURN_EVENT_TEXT_COMPLETED") == 0) {
        return "text_completed";
    }
    if (strcmp(type_token, "6") == 0 || strcmp(type_token, "tts_segment") == 0 ||
        strcmp(type_token, "TURN_EVENT_TTS_SEGMENT") == 0) {
        return "tts_segment";
    }
    if (strcmp(type_token, "7") == 0 || strcmp(type_token, "pcm_started") == 0 ||
        strcmp(type_token, "TURN_EVENT_PCM_STARTED") == 0) {
        return "pcm_started";
    }
    if (strcmp(type_token, "8") == 0 || strcmp(type_token, "pcm_chunk") == 0 ||
        strcmp(type_token, "TURN_EVENT_PCM_CHUNK") == 0) {
        return "pcm_chunk";
    }
    if (strcmp(type_token, "9") == 0 || strcmp(type_token, "pcm_ended") == 0 ||
        strcmp(type_token, "TURN_EVENT_PCM_ENDED") == 0) {
        return "pcm_ended";
    }
    if (strcmp(type_token, "10") == 0 || strcmp(type_token, "completed") == 0 ||
        strcmp(type_token, "turn_completed") == 0 || strcmp(type_token, "TURN_EVENT_COMPLETED") == 0) {
        return "completed";
    }
    if (strcmp(type_token, "11") == 0 || strcmp(type_token, "canceled") == 0 ||
        strcmp(type_token, "cancelled") == 0 || strcmp(type_token, "turn_cancelled") == 0 ||
        strcmp(type_token, "turn_canceled") == 0 || strcmp(type_token, "TURN_EVENT_CANCELED") == 0) {
        return "canceled";
    }
    if (strcmp(type_token, "12") == 0 || strcmp(type_token, "failed") == 0 ||
        strcmp(type_token, "turn_error") == 0 || strcmp(type_token, "TURN_EVENT_FAILED") == 0) {
        return "failed";
    }
    return type_token;
}

const char *turn_agent_task_event_name_v1(const char *type_token) {
    if (!type_token || !type_token[0]) return "agent_task_unknown";
    if (strcmp(type_token, "1") == 0 || strcmp(type_token, "agent_task_started") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_STARTED") == 0) {
        return "agent_task_started";
    }
    if (strcmp(type_token, "2") == 0 || strcmp(type_token, "agent_task_queued") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_QUEUED") == 0) {
        return "agent_task_queued";
    }
    if (strcmp(type_token, "3") == 0 || strcmp(type_token, "agent_task_progress") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_PROGRESS") == 0) {
        return "agent_task_progress";
    }
    if (strcmp(type_token, "4") == 0 || strcmp(type_token, "agent_task_artifact") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_ARTIFACT") == 0) {
        return "agent_task_artifact";
    }
    if (strcmp(type_token, "5") == 0 || strcmp(type_token, "agent_task_completed") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_COMPLETED") == 0) {
        return "agent_task_completed";
    }
    if (strcmp(type_token, "6") == 0 || strcmp(type_token, "agent_task_failed") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_FAILED") == 0) {
        return "agent_task_failed";
    }
    if (strcmp(type_token, "7") == 0 || strcmp(type_token, "agent_task_canceled") == 0 ||
        strcmp(type_token, "AGENT_TASK_EVENT_CANCELED") == 0) {
        return "agent_task_canceled";
    }
    if (strcmp(type_token, "0") == 0) return "agent_task_unknown";
    return "agent_task_unknown";
}

int turn_event_type_id_v1(const char *type_token) {
    const char *n = turn_event_name_v1(type_token);
    if (!n)
        return 0;
    if (strcmp(n, "started") == 0)
        return 1;
    if (strcmp(n, "thinking_started") == 0)
        return 2;
    if (strcmp(n, "thinking_ended") == 0)
        return 3;
    if (strcmp(n, "text_delta") == 0)
        return 4;
    if (strcmp(n, "text_completed") == 0)
        return 5;
    if (strcmp(n, "tts_segment") == 0)
        return 6;
    if (strcmp(n, "pcm_started") == 0)
        return 7;
    if (strcmp(n, "pcm_chunk") == 0)
        return 8;
    if (strcmp(n, "pcm_ended") == 0)
        return 9;
    if (strcmp(n, "completed") == 0)
        return 10;
    if (strcmp(n, "canceled") == 0)
        return 11;
    if (strcmp(n, "failed") == 0)
        return 12;
    return 0;
}

int turn_event_is_terminal_v1(const char *type_token) {
    const char *n = turn_event_name_v1(type_token);
    return strcmp(n, "completed") == 0 || strcmp(n, "failed") == 0 || strcmp(n, "canceled") == 0;
}

int turn_agent_task_event_is_terminal_v1(const char *type_token) {
    const char *n = turn_agent_task_event_name_v1(type_token);
    return strcmp(n, "agent_task_completed") == 0 || strcmp(n, "agent_task_failed") == 0 ||
           strcmp(n, "agent_task_canceled") == 0;
}
