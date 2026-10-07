/*
 * VBus — pure-C topic bus (NATS alternative for the gated voice-runtime path).
 * Length-framed messages over a Unix domain socket to a pure-C broker.
 * No NATS protocol, no Go, no third-party broker.
 */
#ifndef VOICE_C_VBUS_H
#define VOICE_C_VBUS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VBUS_MAGIC 0x56425553u /* 'VBUS' */
#define VBUS_VERSION 2u
#define VBUS_FRAME_HEADER_BYTES 32u
#define VBUS_MAX_FRAME_BYTES (1u << 22)
#define VBUS_SUBSCRIPTION_NONE UINT32_MAX

enum vbus_op {
    VBUS_OP_HELLO = 1,
    VBUS_OP_SUB = 2,
    VBUS_OP_PUB = 3,
    VBUS_OP_MSG = 4,
    VBUS_OP_OK = 5,
    VBUS_OP_ERR = 6,
    VBUS_OP_SUB_FLOW = 7
};

typedef struct vbus_client vbus_client;
#if ATOMIC_INT_LOCK_FREE != 2
#error "VBus signal stop flag requires lock-free atomic int"
#endif
typedef _Atomic int vbus_stop_flag;

typedef struct {
    const uint8_t *data;
    size_t len;
} vbus_publish_span;

typedef void (*vbus_msg_handler)(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
);

/* Connect and wait for broker acknowledgement at the Unix path. */
vbus_client *vbus_connect(const char *unix_path);
/* Limit complete inbound frames and their receive storage. The limit includes
 * the VBus header and subjects, and must be 8192..VBUS_MAX_FRAME_BYTES. */
vbus_client *vbus_connect_bounded(const char *unix_path, size_t receive_limit);
void vbus_close(vbus_client *c);

/* Subscribe and wait until the broker activates it. queue_group may be NULL.
 * topic may end with ".>" for a prefix match. */
int vbus_subscribe(
    vbus_client *c,
    const char *topic,
    const char *queue_group,
    vbus_msg_handler handler,
    void *user
);

/* One exact, receive-only subscription on a fresh client. Requires broker
 * support for VBUS_OP_SUB_FLOW. When its bounded
 * broker queue fills, publishers pause until it drains. A subscriber that
 * stays full for one second is disconnected so it cannot pin producers. */
int vbus_subscribe_flow(vbus_client *c, const char *topic,
    vbus_msg_handler handler, void *user);

int vbus_publish(
    vbus_client *c,
    const char *topic,
    const uint8_t *data,
    size_t data_len
);

/* Publish a proven non-empty, NUL-free topic span without rescanning it. */
int vbus_publish_prepared(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const uint8_t *data,
    size_t data_len
);

/* Publish exactly three non-empty borrowed spans as one wire body.
 * The call retains no span after it returns. */
int vbus_publish_spans3_prepared(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const vbus_publish_span spans[3]
);

/* Publish three spans after the caller proves the complete checked contract.
 * Every pointer and length must be valid, nonzero, and within the frame limit.
 * The call retains no topic or body span after it returns. */
int vbus_publish_spans3_admitted(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const vbus_publish_span spans[3]
);

/* Publish two messages in order through one socket send sequence. */
int vbus_publish_pair(
    vbus_client *c,
    const char *first_topic,
    const uint8_t *first_data,
    size_t first_data_len,
    const char *second_topic,
    const uint8_t *second_data,
    size_t second_data_len
);

/* Publish two proven topic spans in order through one socket send sequence. */
int vbus_publish_pair_prepared(
    vbus_client *c,
    const char *first_topic,
    size_t first_topic_len,
    const uint8_t *first_data,
    size_t first_data_len,
    const char *second_topic,
    size_t second_topic_len,
    const uint8_t *second_data,
    size_t second_data_len
);

/* Publish a three-span body, then one contiguous body, in order. */
int vbus_publish_spans3_pair_prepared(
    vbus_client *c,
    const char *first_topic,
    size_t first_topic_len,
    const vbus_publish_span first_spans[3],
    const char *second_topic,
    size_t second_topic_len,
    const uint8_t *second_data,
    size_t second_data_len
);

/* Publish the same ordered pair after the caller proves all pointer and frame
 * bounds. The call retains no topic or body span after it returns. */
int vbus_publish_spans3_pair_admitted(
    vbus_client *c,
    const char *first_topic,
    size_t first_topic_len,
    const vbus_publish_span first_spans[3],
    const char *second_topic,
    size_t second_topic_len,
    const uint8_t *second_data,
    size_t second_data_len
);

int vbus_publish_reply(
    vbus_client *c,
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len
);

/*
 * Request/reply: publish to topic with a unique _INBOX reply subject, wait up to
 * timeout_ms for a body on that inbox. Returns 0 on success, -1 on timeout/error.
 * Not re-entrant on the same client (one outstanding request).
 */
int vbus_request(
    vbus_client *c,
    const char *topic,
    const uint8_t *data,
    size_t data_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int timeout_ms
);

/*
 * Cancelable request/reply variant for bounded workers. A nonzero flag stops
 * the wait without closing the client. A late reply is ignored.
 */
int vbus_request_cancel(
    vbus_client *c,
    const char *topic,
    const uint8_t *data,
    size_t data_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int timeout_ms,
    const atomic_int *cancel_flag
);

/*
 * Return the borrowed socket descriptor for an outer poll loop.
 * The caller must not read, write, or close it. It stays valid until vbus_close.
 */
int vbus_poll_fd(const vbus_client *c);

/* Process inbound; timeout_ms 0 = non-blocking. */
int vbus_poll(vbus_client *c, int timeout_ms);

/* Dispatch at most one frame, including a buffered frame. Returns 1 for a
 * dispatched frame, 0 for idle, and -1 for a transport/protocol error. Owners
 * can drain their output between frames without increasing receive storage. */
int vbus_poll_one(vbus_client *c, int timeout_ms);
int vbus_run(vbus_client *c, const vbus_stop_flag *stop);

/* Broker: bind Unix path and serve forever until *stop. Pure C. */
int vbus_broker_run(const char *unix_path, const vbus_stop_flag *stop);

/* Default path from VBUS_PATH env or /tmp/voice-vbus.sock */
const char *vbus_default_path(void);

#ifdef __cplusplus
}
#endif

#endif
