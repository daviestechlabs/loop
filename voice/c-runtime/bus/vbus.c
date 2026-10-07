#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "vbus.h"
#include "../common/dynbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define VBUS_MAX_SUBS 64
#define VBUS_MAX_CLIENTS 64
#define VBUS_MAX_FRAME VBUS_MAX_FRAME_BYTES
#define VBUS_MAX_PENDING (1u << 22)
#define VBUS_RBUF_SOFT_MIN 4096
#define VBUS_BROKER_DEFAULT_BUFFER_BYTES (64u * 1024u * 1024u)
#define VBUS_BROKER_MIN_BUFFER_BYTES (1u * 1024u * 1024u)
#define VBUS_BROKER_MAX_BUFFER_BYTES (96u * 1024u * 1024u)
#define VBUS_EXACT_BUCKETS 8192
#define VBUS_QUEUE_BUCKETS 8192
#define VBUS_SUB_INDEX_NONE (-1)
#define VBUS_CONTROL_TIMEOUT_MS 1000
#define VBUS_HANDSHAKE_TIMEOUT_MS 1000u
#define VBUS_FLOW_STALL_TIMEOUT_MS 1000u

#if defined(__GNUC__) && !defined(__clang__)
#define VBUS_NOINLINE __attribute__((noinline, noipa))
#elif defined(__clang__)
#define VBUS_NOINLINE __attribute__((noinline))
#else
#define VBUS_NOINLINE
#endif

_Static_assert(
    VBUS_BROKER_MIN_BUFFER_BYTES >= VBUS_MAX_CLIENTS * 2u * 8192u,
    "broker minimum buffer cap must admit every client");

enum broker_queue_state {
    BROKER_QUEUE_EMPTY = 0,
    BROKER_QUEUE_OCCUPIED = 1,
    BROKER_QUEUE_TOMBSTONE = 2,
};

typedef struct {
    uint8_t topic_len;
    char topic[255];
    uint8_t queue_len;
    char queue[127];
    vbus_msg_handler handler;
    void *user;
} vbus_sub;

struct vbus_client {
    int fd;
    vbus_sub subs[VBUS_MAX_SUBS];
    int nsubs;
    dynbuf rx; /* grow under load, reclaim when empty */
    size_t rx_head;
    size_t rx_limit;
    /* single outstanding request/reply */
    int req_pending;
    int req_done;
    int inbox_subbed;
    char req_inbox[128];
    uint8_t *req_out;
    size_t req_cap;
    size_t req_len;
    int req_overflow;
    unsigned req_seq;
    int dispatch_depth;
    uint64_t hello_acks;
    uint64_t sub_acks;
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t op;
    uint32_t subscription_id;
    uint32_t topic_len;
    uint32_t queue_len;
    uint32_t reply_len;
    uint32_t body_len;
} vbus_hdr;
#pragma pack(pop)

_Static_assert(
    sizeof(vbus_hdr) == VBUS_FRAME_HEADER_BYTES,
    "VBus wire header size changed without a protocol update");

const char *vbus_default_path(void) {
    const char *p = getenv("VBUS_PATH");
    if (p && p[0]) return p;
    return "/tmp/voice-vbus.sock";
}

static int send_vectors_all(
    int fd,
    struct iovec *vectors,
    size_t count,
    size_t total_len
) {
    size_t first = 0;
    size_t remaining = total_len;
#if defined(__APPLE__)
    if (count > INT_MAX) { errno = EINVAL; return -1; }
#endif
    while (first < count) {
        struct msghdr message;
        size_t sent;
        ssize_t result;
        memset(&message, 0, sizeof(message));
        message.msg_iov = vectors + first;
#if defined(__APPLE__)
        message.msg_iovlen = (int)(count - first);
#else
        message.msg_iovlen = count - first;
#endif
        result = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return -1;
        sent = (size_t)result;
        if (sent == remaining) return 0;
        if (sent > remaining) return -1;
        remaining -= sent;
        while (first < count && sent >= vectors[first].iov_len) {
            sent -= vectors[first].iov_len;
            first++;
        }
        if (first < count && sent != 0) {
            vectors[first].iov_base = (uint8_t *)vectors[first].iov_base + sent;
            vectors[first].iov_len -= sent;
        }
    }
    return 0;
}

static int frame_size(
    size_t topic_len,
    size_t queue_len,
    size_t reply_len,
    size_t body_len,
    size_t *out_size
) {
    uint64_t total;
    if (!out_size || topic_len > UINT32_MAX || queue_len > UINT32_MAX ||
        reply_len > UINT32_MAX || body_len > UINT32_MAX)
        return -1;
    total = sizeof(vbus_hdr) + (uint64_t)topic_len + (uint64_t)queue_len +
        (uint64_t)reply_len + (uint64_t)body_len;
    if (total > VBUS_MAX_FRAME) return -1;
    *out_size = (size_t)total;
    return 0;
}

static int frame_size_from_header(const vbus_hdr *header, size_t *out_size) {
    uint64_t total;
    if (!header || !out_size) return -1;
    total = sizeof(*header) + (uint64_t)header->topic_len +
        (uint64_t)header->queue_len + (uint64_t)header->reply_len +
        (uint64_t)header->body_len;
    if (total > VBUS_MAX_FRAME) return -1;
    *out_size = (size_t)total;
    return 0;
}

static int send_frame(
    int fd,
    uint32_t op,
    uint32_t subscription_id,
    const char *topic,
    const char *queue,
    const char *reply,
    const uint8_t *body,
    size_t body_len
) {
    vbus_hdr h;
    struct iovec vectors[5];
    size_t vector_count = 0;
    size_t frame_len;
    size_t tlen = topic ? strlen(topic) : 0;
    size_t qlen = queue ? strlen(queue) : 0;
    size_t rlen = reply ? strlen(reply) : 0;
    if ((body_len != 0 && !body) || frame_size(tlen, qlen, rlen, body_len, &frame_len) != 0)
        return -1;
    memset(&h, 0, sizeof(h));
    h.magic = VBUS_MAGIC;
    h.version = VBUS_VERSION;
    h.op = op;
    h.subscription_id = subscription_id;
    h.topic_len = (uint32_t)tlen;
    h.queue_len = (uint32_t)qlen;
    h.reply_len = (uint32_t)rlen;
    h.body_len = (uint32_t)body_len;
    vectors[vector_count].iov_base = &h;
    vectors[vector_count++].iov_len = sizeof(h);
    if (tlen != 0) {
        vectors[vector_count].iov_base = (void *)topic;
        vectors[vector_count++].iov_len = tlen;
    }
    if (qlen != 0) {
        vectors[vector_count].iov_base = (void *)queue;
        vectors[vector_count++].iov_len = qlen;
    }
    if (rlen != 0) {
        vectors[vector_count].iov_base = (void *)reply;
        vectors[vector_count++].iov_len = rlen;
    }
    if (body_len != 0) {
        vectors[vector_count].iov_base = (void *)body;
        vectors[vector_count++].iov_len = body_len;
    }
    return send_vectors_all(fd, vectors, vector_count, frame_len);
}

static int receive_window_prepare_bounded(
    dynbuf *rx,
    size_t *head,
    size_t need,
    size_t max_capacity
) {
    size_t tail;
    if (!rx || !head || *head > rx->cap || rx->len > rx->cap - *head ||
        rx->len > VBUS_MAX_FRAME || need > VBUS_MAX_FRAME - rx->len) return -1;
    tail = *head + rx->len;
    if (need <= rx->cap - tail) return 0;
    if (*head != 0) {
        if (rx->len != 0) memmove(rx->data, rx->data + *head, rx->len);
        *head = 0;
        if (need <= rx->cap - rx->len) return 0;
    }
    return dynbuf_reserve_bounded(rx, rx->len + need, max_capacity);
}

static int ensure_rbuf(vbus_client *c, size_t need) {
    if (!c) return -1;
    return receive_window_prepare_bounded(&c->rx, &c->rx_head, need, c->rx_limit);
}

static int wait_for_control_ack(vbus_client *c, uint64_t *counter, uint64_t previous) {
    struct timespec deadline;
    if (!c || !counter || *counter != previous || c->dispatch_depth != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return -1;
    deadline.tv_sec += VBUS_CONTROL_TIMEOUT_MS / 1000;
    deadline.tv_nsec +=
        (long)(VBUS_CONTROL_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (*counter == previous) {
        struct timespec now;
        int remaining_ms;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) return -1;
        remaining_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
            (deadline.tv_nsec - now.tv_nsec + 999999L) / 1000000L);
        if (remaining_ms < 1) remaining_ms = 1;
        if (vbus_poll(c, remaining_ms) != 0) return -1;
    }
    return 0;
}

/* Topic match: exact, or pattern ending in ".>" matches prefix. */
static int subscription_topic_matches(
    const vbus_sub *sub,
    const char *topic,
    size_t topic_len
) {
    size_t pattern_len;
    if (!sub || !topic) return 0;
    pattern_len = sub->topic_len;
    if (pattern_len >= 2u && sub->topic[pattern_len - 2u] == '.' &&
        sub->topic[pattern_len - 1u] == '>') {
        /* pattern "ai.voice.stream.>" → prefix "ai.voice.stream." */
        size_t prefix_len = pattern_len - 1u;
        return topic_len >= prefix_len &&
            memcmp(sub->topic, topic, prefix_len) == 0 &&
            memchr(topic + prefix_len, '\0', topic_len - prefix_len) == NULL;
    }
    if (pattern_len >= 1u && sub->topic[pattern_len - 1u] == '>') {
        /* "ai.voice.stream.>" style already handled; bare ">" matches all */
        if (pattern_len == 1u)
            return memchr(topic, '\0', topic_len) == NULL;
    }
    return pattern_len == topic_len &&
        memcmp(sub->topic, topic, topic_len) == 0;
}

static void vbus_client_init(vbus_client *c) {
    /* vbus_subscribe clears each subscription before nsubs exposes it. */
    c->fd = -1;
    c->nsubs = 0;
    dynbuf_init(&c->rx);
    c->rx_head = 0;
    c->rx_limit = VBUS_MAX_FRAME;
    c->req_pending = 0;
    c->req_done = 0;
    c->inbox_subbed = 0;
    c->req_inbox[0] = '\0';
    c->req_out = NULL;
    c->req_cap = 0;
    c->req_len = 0;
    c->req_overflow = 0;
    c->req_seq = 0;
    c->dispatch_depth = 0;
    c->hello_acks = 0;
    c->sub_acks = 0;
}

vbus_client *vbus_connect_bounded(const char *unix_path, size_t receive_limit) {
    vbus_client *c;
    struct sockaddr_un addr;
    uint64_t previous_acks;
    const char *path = unix_path && unix_path[0] ? unix_path : vbus_default_path();
    if (receive_limit < 8192u || receive_limit > VBUS_MAX_FRAME) return NULL;
    c = (vbus_client *)malloc(sizeof(*c));
    if (!c) return NULL;
    vbus_client_init(c);
    c->rx_limit = receive_limit;
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(c->fd);
        free(c);
        return NULL;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(c->fd);
        free(c);
        return NULL;
    }
    if (dynbuf_reserve(&c->rx, 8192) != 0) {
        close(c->fd);
        free(c);
        return NULL;
    }
    previous_acks = c->hello_acks;
    if (send_frame(
            c->fd, VBUS_OP_HELLO, VBUS_SUBSCRIPTION_NONE,
            "client", NULL, NULL, NULL, 0) != 0 ||
        wait_for_control_ack(c, &c->hello_acks, previous_acks) != 0) {
        vbus_close(c);
        return NULL;
    }
    return c;
}

vbus_client *vbus_connect(const char *unix_path) {
    return vbus_connect_bounded(unix_path, VBUS_MAX_FRAME);
}

void vbus_close(vbus_client *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    dynbuf_free(&c->rx);
    free(c);
}

static int subscribe_client(
    vbus_client *c,
    const char *topic,
    const char *queue_group,
    vbus_msg_handler handler,
    void *user,
    uint32_t operation
) {
    vbus_sub *s;
    uint64_t previous_acks;
    size_t topic_len;
    size_t queue_len;
    if (!c || !topic || !topic[0] || !handler || c->nsubs >= VBUS_MAX_SUBS)
        return -1;
    topic_len = strlen(topic);
    queue_len = queue_group ? strlen(queue_group) : 0u;
    if (topic_len > sizeof(c->subs[0].topic) ||
        queue_len > sizeof(c->subs[0].queue)) return -1;
    s = &c->subs[c->nsubs];
    memset(s, 0, sizeof(*s));
    s->topic_len = (uint8_t)topic_len;
    memcpy(s->topic, topic, topic_len);
    s->queue_len = (uint8_t)queue_len;
    if (queue_len != 0u) memcpy(s->queue, queue_group, queue_len);
    s->handler = handler;
    s->user = user;
    previous_acks = c->sub_acks;
    if (send_frame(
            c->fd, operation, (uint32_t)c->nsubs,
            topic, queue_group, NULL, NULL, 0) != 0) {
        memset(s, 0, sizeof(*s));
        return -1;
    }
    c->nsubs++;
    if (wait_for_control_ack(c, &c->sub_acks, previous_acks) != 0) {
        c->nsubs--;
        memset(s, 0, sizeof(*s));
        return -1;
    }
    return 0;
}

int vbus_subscribe(vbus_client *c, const char *topic, const char *queue_group,
    vbus_msg_handler handler, void *user) {
    return subscribe_client(c, topic, queue_group, handler, user, VBUS_OP_SUB);
}

int vbus_subscribe_flow(vbus_client *c, const char *topic,
    vbus_msg_handler handler, void *user) {
    if (!c || c->nsubs != 0 || !topic || strchr(topic, '>') || strchr(topic, '*'))
        return -1;
    return subscribe_client(c, topic, NULL, handler, user, VBUS_OP_SUB_FLOW);
}

int vbus_publish(vbus_client *c, const char *topic, const uint8_t *data, size_t data_len) {
    if (!c || !topic) return -1;
    return vbus_publish_prepared(c, topic, strlen(topic), data, data_len);
}

static int append_publish_vectors_prepared(
    vbus_hdr *header,
    struct iovec *vectors,
    size_t vectors_cap,
    size_t *vector_count,
    size_t *frame_len_out,
    const char *topic,
    size_t topic_len,
    const uint8_t *data,
    size_t data_len
) {
    size_t frame_len;
    size_t needed;
    if (!header || !vectors || !vector_count || !frame_len_out || !topic ||
        topic_len == 0u ||
        (data_len != 0 && !data)) return -1;
    if (frame_size(topic_len, 0, 0, data_len, &frame_len) != 0) return -1;
    needed = 2u + (data_len != 0 ? 1u : 0u);
    if (*vector_count > vectors_cap || needed > vectors_cap - *vector_count) return -1;
    memset(header, 0, sizeof(*header));
    header->magic = VBUS_MAGIC;
    header->version = VBUS_VERSION;
    header->op = VBUS_OP_PUB;
    header->subscription_id = VBUS_SUBSCRIPTION_NONE;
    header->topic_len = (uint32_t)topic_len;
    header->body_len = (uint32_t)data_len;
    vectors[*vector_count].iov_base = header;
    vectors[(*vector_count)++].iov_len = sizeof(*header);
    vectors[*vector_count].iov_base = (void *)topic;
    vectors[(*vector_count)++].iov_len = topic_len;
    if (data_len != 0) {
        vectors[*vector_count].iov_base = (void *)data;
        vectors[(*vector_count)++].iov_len = data_len;
    }
    *frame_len_out = frame_len;
    return 0;
}

int vbus_publish_prepared(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const uint8_t *data,
    size_t data_len
) {
    vbus_hdr header;
    struct iovec vectors[3];
    size_t vector_count = 0;
    size_t frame_len;
    if (!c ||
        append_publish_vectors_prepared(
            &header,
            vectors,
            sizeof(vectors) / sizeof(vectors[0]),
            &vector_count,
            &frame_len,
            topic,
            topic_len,
            data,
            data_len) != 0) return -1;
    return send_vectors_all(c->fd, vectors, vector_count, frame_len);
}

int vbus_publish_spans3_prepared(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const vbus_publish_span spans[3]
) {
    uint64_t body_len;
    uint64_t frame_len;
    if (!c || !topic || topic_len == 0u || topic_len > UINT32_MAX || !spans ||
        !spans[0].data || spans[0].len == 0u || spans[0].len > UINT32_MAX ||
        !spans[1].data || spans[1].len == 0u || spans[1].len > UINT32_MAX ||
        !spans[2].data || spans[2].len == 0u || spans[2].len > UINT32_MAX)
        return -1;
    body_len = (uint64_t)spans[0].len + (uint64_t)spans[1].len +
        (uint64_t)spans[2].len;
    frame_len = sizeof(vbus_hdr) + (uint64_t)topic_len + body_len;
    if (body_len > UINT32_MAX || frame_len > VBUS_MAX_FRAME) return -1;
    return vbus_publish_spans3_admitted(c, topic, topic_len, spans);
}

int vbus_publish_spans3_admitted(
    vbus_client *c,
    const char *topic,
    size_t topic_len,
    const vbus_publish_span spans[3]
) {
    vbus_hdr header = {0};
    struct iovec vectors[5];
    size_t body_len = spans[0].len + spans[1].len + spans[2].len;
    size_t frame_len = sizeof(header) + topic_len + body_len;
    header.magic = VBUS_MAGIC;
    header.version = VBUS_VERSION;
    header.op = VBUS_OP_PUB;
    header.subscription_id = VBUS_SUBSCRIPTION_NONE;
    header.topic_len = (uint32_t)topic_len;
    header.body_len = (uint32_t)body_len;
    vectors[0].iov_base = &header;
    vectors[0].iov_len = sizeof(header);
    vectors[1].iov_base = (void *)topic;
    vectors[1].iov_len = topic_len;
    vectors[2].iov_base = (void *)spans[0].data;
    vectors[2].iov_len = spans[0].len;
    vectors[3].iov_base = (void *)spans[1].data;
    vectors[3].iov_len = spans[1].len;
    vectors[4].iov_base = (void *)spans[2].data;
    vectors[4].iov_len = spans[2].len;
    return send_vectors_all(c->fd, vectors, 5u, frame_len);
}

int vbus_publish_pair(
    vbus_client *c,
    const char *first_topic,
    const uint8_t *first_data,
    size_t first_data_len,
    const char *second_topic,
    const uint8_t *second_data,
    size_t second_data_len
) {
    if (!c || !first_topic || !first_topic[0] || !second_topic || !second_topic[0])
        return -1;
    return vbus_publish_pair_prepared(
        c,
        first_topic,
        strlen(first_topic),
        first_data,
        first_data_len,
        second_topic,
        strlen(second_topic),
        second_data,
        second_data_len);
}

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
) {
    vbus_hdr headers[2];
    struct iovec vectors[6];
    size_t vector_count = 0;
    size_t first_frame_len;
    size_t second_frame_len;
    size_t total_frame_len;
    const size_t vector_capacity = sizeof(vectors) / sizeof(vectors[0]);
    if (!c ||
        append_publish_vectors_prepared(
            &headers[0], vectors, vector_capacity, &vector_count,
            &first_frame_len, first_topic, first_topic_len,
            first_data, first_data_len) != 0 ||
        append_publish_vectors_prepared(
            &headers[1], vectors, vector_capacity, &vector_count,
            &second_frame_len, second_topic, second_topic_len,
            second_data, second_data_len) != 0) return -1;
    if (first_frame_len > SIZE_MAX - second_frame_len) return -1;
    total_frame_len = first_frame_len + second_frame_len;
    return send_vectors_all(
        c->fd,
        vectors,
        vector_count,
        total_frame_len);
}

int vbus_publish_spans3_pair_prepared(
    vbus_client *c,
    const char *first_topic,
    size_t first_topic_len,
    const vbus_publish_span first_spans[3],
    const char *second_topic,
    size_t second_topic_len,
    const uint8_t *second_data,
    size_t second_data_len
) {
    uint64_t first_body_len;
    uint64_t first_frame_len;
    uint64_t second_frame_len;
    if (!c || !first_topic || first_topic_len == 0u ||
        first_topic_len > UINT32_MAX || !first_spans ||
        !first_spans[0].data || first_spans[0].len == 0u ||
        first_spans[0].len > UINT32_MAX ||
        !first_spans[1].data || first_spans[1].len == 0u ||
        first_spans[1].len > UINT32_MAX ||
        !first_spans[2].data || first_spans[2].len == 0u ||
        first_spans[2].len > UINT32_MAX ||
        !second_topic || second_topic_len == 0u ||
        second_topic_len > UINT32_MAX || !second_data ||
        second_data_len == 0u || second_data_len > UINT32_MAX) return -1;
    first_body_len = (uint64_t)first_spans[0].len +
        (uint64_t)first_spans[1].len + (uint64_t)first_spans[2].len;
    first_frame_len = sizeof(vbus_hdr) +
        (uint64_t)first_topic_len + first_body_len;
    second_frame_len = sizeof(vbus_hdr) +
        (uint64_t)second_topic_len + (uint64_t)second_data_len;
    if (first_body_len > UINT32_MAX || first_frame_len > VBUS_MAX_FRAME ||
        second_frame_len > VBUS_MAX_FRAME) return -1;
    return vbus_publish_spans3_pair_admitted(
        c, first_topic, first_topic_len, first_spans,
        second_topic, second_topic_len, second_data, second_data_len);
}

int vbus_publish_spans3_pair_admitted(
    vbus_client *c,
    const char *first_topic,
    size_t first_topic_len,
    const vbus_publish_span first_spans[3],
    const char *second_topic,
    size_t second_topic_len,
    const uint8_t *second_data,
    size_t second_data_len
) {
    vbus_hdr headers[2] = {{0}};
    struct iovec vectors[8];
    size_t first_body_len = first_spans[0].len +
        first_spans[1].len + first_spans[2].len;
    size_t first_frame_len = sizeof(headers[0]) +
        first_topic_len + first_body_len;
    size_t second_frame_len = sizeof(headers[1]) +
        second_topic_len + second_data_len;
    headers[0].magic = VBUS_MAGIC;
    headers[0].version = VBUS_VERSION;
    headers[0].op = VBUS_OP_PUB;
    headers[0].subscription_id = VBUS_SUBSCRIPTION_NONE;
    headers[0].topic_len = (uint32_t)first_topic_len;
    headers[0].body_len = (uint32_t)first_body_len;
    headers[1].magic = VBUS_MAGIC;
    headers[1].version = VBUS_VERSION;
    headers[1].op = VBUS_OP_PUB;
    headers[1].subscription_id = VBUS_SUBSCRIPTION_NONE;
    headers[1].topic_len = (uint32_t)second_topic_len;
    headers[1].body_len = (uint32_t)second_data_len;
    vectors[0].iov_base = &headers[0];
    vectors[0].iov_len = sizeof(headers[0]);
    vectors[1].iov_base = (void *)first_topic;
    vectors[1].iov_len = first_topic_len;
    vectors[2].iov_base = (void *)first_spans[0].data;
    vectors[2].iov_len = first_spans[0].len;
    vectors[3].iov_base = (void *)first_spans[1].data;
    vectors[3].iov_len = first_spans[1].len;
    vectors[4].iov_base = (void *)first_spans[2].data;
    vectors[4].iov_len = first_spans[2].len;
    vectors[5].iov_base = &headers[1];
    vectors[5].iov_len = sizeof(headers[1]);
    vectors[6].iov_base = (void *)second_topic;
    vectors[6].iov_len = second_topic_len;
    vectors[7].iov_base = (void *)second_data;
    vectors[7].iov_len = second_data_len;
    return send_vectors_all(
        c->fd, vectors, 8u, first_frame_len + second_frame_len);
}

int vbus_publish_reply(
    vbus_client *c,
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len
) {
    if (!c || !topic || !topic[0]) return -1;
    return send_frame(
        c->fd, VBUS_OP_PUB, VBUS_SUBSCRIPTION_NONE,
        topic, NULL, reply, data, data_len);
}

static void on_inbox_msg(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    vbus_client *c = (vbus_client *)user;
    (void)reply;
    if (!c || !c->req_pending || !c->req_inbox[0]) return;
    if (strcmp(topic, c->req_inbox) != 0) return;
    if (c->req_out && data_len <= c->req_cap) {
        memcpy(c->req_out, data, data_len);
        c->req_len = data_len;
    } else if (c->req_out && c->req_cap > 0) {
        c->req_len = 0;
        c->req_overflow = 1;
    } else {
        c->req_len = 0;
    }
    c->req_done = 1;
}

static int vbus_request_impl(
    vbus_client *c,
    const char *topic,
    const uint8_t *data,
    size_t data_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int timeout_ms,
    const atomic_int *cancel_flag
) {
    struct timespec deadline;
    if (!c || !topic || !out || out_cap == 0) return -1;
    if (out_len) *out_len = 0;
    if (c->req_pending || c->dispatch_depth != 0) return -1;
    if (!c->inbox_subbed) {
        if (vbus_subscribe(c, "_INBOX.>", NULL, on_inbox_msg, c) != 0) return -1;
        c->inbox_subbed = 1;
    }
    c->req_seq++;
    snprintf(
        c->req_inbox, sizeof(c->req_inbox), "_INBOX.%d.%d.%u",
        (int)getpid(), c->fd, c->req_seq);
    c->req_out = out;
    c->req_cap = out_cap;
    c->req_len = 0;
    c->req_overflow = 0;
    c->req_done = 0;
    c->req_pending = 1;
    if (vbus_publish_reply(c, topic, c->req_inbox, data, data_len) != 0) {
        c->req_pending = 0;
        c->req_out = NULL;
        c->req_cap = 0;
        return -1;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        c->req_pending = 0;
        c->req_out = NULL;
        c->req_cap = 0;
        return -1;
    }
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!c->req_done &&
           (!cancel_flag ||
            atomic_load_explicit(cancel_flag, memory_order_relaxed) == 0)) {
        struct timespec now;
        int remaining_ms;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
        remaining_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
            (deadline.tv_nsec - now.tv_nsec + 999999L) / 1000000L);
        if (remaining_ms < 1) remaining_ms = 1;
        if (cancel_flag && remaining_ms > 10) remaining_ms = 10;
        if (vbus_poll(c, remaining_ms) != 0) break;
    }
    c->req_pending = 0;
    if (out_len) *out_len = c->req_len;
    c->req_out = NULL;
    c->req_cap = 0;
    return c->req_done && !c->req_overflow &&
        (!cancel_flag ||
         atomic_load_explicit(cancel_flag, memory_order_relaxed) == 0) ? 0 : -1;
}

int vbus_request(
    vbus_client *c,
    const char *topic,
    const uint8_t *data,
    size_t data_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len,
    int timeout_ms
) {
    return vbus_request_impl(
        c, topic, data, data_len, out, out_cap, out_len, timeout_ms, NULL);
}

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
) {
    return vbus_request_impl(
        c, topic, data, data_len, out, out_cap, out_len, timeout_ms, cancel_flag);
}

static int dispatch_msg(
    vbus_client *c,
    uint32_t subscription_id,
    const char *topic,
    size_t topic_len,
    const char *queue,
    size_t queue_len,
    const char *reply,
    const uint8_t *body,
    size_t body_len
) {
    vbus_sub *sub;
    if (!c || subscription_id >= (uint32_t)c->nsubs) return -1;
    sub = &c->subs[subscription_id];
    if (!sub->handler ||
        !subscription_topic_matches(sub, topic, topic_len) ||
        queue_len != (size_t)sub->queue_len ||
        (queue_len != 0u && memcmp(sub->queue, queue, queue_len) != 0))
        return -1;
    c->dispatch_depth++;
    sub->handler(topic, reply, body, body_len, sub->user);
    c->dispatch_depth--;
    return 0;
}

static int process_client_buffer(vbus_client *c, int one_frame, int *dispatched) {
    while (c->rx.len >= sizeof(vbus_hdr)) {
        vbus_hdr h;
        uint8_t *frame = c->rx.data + c->rx_head;
        size_t need;
        const char *topic;
        const char *queue;
        const char *reply;
        const uint8_t *body;
        size_t off;
        memcpy(&h, frame, sizeof(h));
        if (h.magic != VBUS_MAGIC || h.version != VBUS_VERSION) return -1;
        if (frame_size_from_header(&h, &need) != 0 || need > c->rx_limit)
            return -1;
        if (c->rx.len < need) return 0;
        off = sizeof(h);
        topic = (const char *)(frame + off);
        off += h.topic_len;
        queue = (const char *)(frame + off);
        (void)queue;
        off += h.queue_len;
        reply = h.reply_len ? (const char *)(frame + off) : NULL;
        off += h.reply_len;
        body = frame + off;
        if (h.op == VBUS_OP_MSG) {
            char topic_z[256];
            char reply_z[256];
            if (h.topic_len == 0 || h.topic_len >= sizeof(topic_z) ||
                h.queue_len > sizeof(((vbus_sub *)0)->queue) ||
                h.reply_len >= sizeof(reply_z) ||
                (h.reply_len != 0 && memchr(reply, '\0', h.reply_len) != NULL)) return -1;
            memcpy(topic_z, topic, h.topic_len);
            topic_z[h.topic_len] = '\0';
            if (reply) {
                memcpy(reply_z, reply, h.reply_len);
                reply_z[h.reply_len] = '\0';
            }
            if (dispatch_msg(
                    c, h.subscription_id, topic_z, h.topic_len,
                    queue, h.queue_len,
                    reply ? reply_z : NULL, body, h.body_len) != 0) return -1;
        } else if (h.op == VBUS_OP_OK) {
            if (h.queue_len != 0 || h.reply_len != 0 || h.body_len != 0) return -1;
            if (h.topic_len == sizeof("hello") - 1u &&
                memcmp(topic, "hello", sizeof("hello") - 1u) == 0) {
                if (h.subscription_id != VBUS_SUBSCRIPTION_NONE) return -1;
                c->hello_acks++;
            } else if (h.topic_len == sizeof("sub") - 1u &&
                       memcmp(topic, "sub", sizeof("sub") - 1u) == 0) {
                if (h.subscription_id >= (uint32_t)c->nsubs) return -1;
                c->sub_acks++;
            } else {
                return -1;
            }
        } else {
            return -1;
        }
        c->rx.len -= need;
        c->rx_head += need;
        if (c->rx.len == 0) c->rx_head = 0;
        *dispatched = 1;
        if (one_frame) break;
    }
    /* Idle reclaim: shrink oversized receive buffer when drained. */
    if (c->rx.len == 0 && c->rx.cap > VBUS_RBUF_SOFT_MIN) {
        dynbuf_clear(&c->rx, VBUS_RBUF_SOFT_MIN);
    }
    return 0;
}

int vbus_poll_fd(const vbus_client *c) {
    return c ? c->fd : -1;
}

static int poll_client(vbus_client *c, int timeout_ms, int one_frame) {
    struct pollfd pfd;
    int pr;
    int dispatched = 0;
    if (!c || c->dispatch_depth != 0) return -1;
    if (process_client_buffer(c, one_frame, &dispatched) != 0) return -1;
    if (one_frame && dispatched) return 1;
    pfd.fd = c->fd;
    pfd.events = POLLIN;
    pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (pr == 0) return dispatched;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    {
        size_t read_need = c->rx_limit - c->rx.len;
        if (read_need > 4096) read_need = 4096;
        if (read_need == 0 || ensure_rbuf(c, read_need) != 0) return -1;
    }
    {
        size_t tail = c->rx_head + c->rx.len;
        size_t space = c->rx.cap - tail;
        size_t max_space = c->rx_limit - c->rx.len;
        if (space > max_space) space = max_space;
        ssize_t r = read(c->fd, c->rx.data + tail, space ? space : 1);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) return 0;
            return -1;
        }
        c->rx.len += (size_t)r;
    }
    if (process_client_buffer(c, one_frame, &dispatched) != 0) return -1;
    return dispatched;
}

int vbus_poll(vbus_client *c, int timeout_ms) {
    return poll_client(c, timeout_ms, 0) < 0 ? -1 : 0;
}

int vbus_poll_one(vbus_client *c, int timeout_ms) {
    return poll_client(c, timeout_ms, 1);
}

int vbus_run(vbus_client *c, const vbus_stop_flag *stop) {
    while (stop && atomic_load_explicit(stop, memory_order_relaxed) == 0) {
        if (vbus_poll(c, 200) != 0) return -1;
    }
    return 0;
}

/* ---------- broker ---------- */

typedef struct {
    char topic[256];
    char queue[128];
    uint32_t subscription_id;
    union {
        int next_exact;
        struct {
            int next;
            uint16_t prefix_length;
        } wildcard;
    } index;
    int next_queue;
    int queue_group_slot;
    int active;
    uint8_t topic_len;
    uint8_t queue_len;
} broker_sub;

typedef struct {
    const char *topic;
    const char *reply;
    const uint8_t *body;
    size_t topic_len;
    size_t reply_len;
    size_t body_len;
} broker_publication_view;

typedef struct {
    const uint8_t *data;
    size_t len;
} broker_span;

typedef struct {
    int head;
    uint32_t cursor;
    uint8_t state;
} broker_queue_group;

typedef struct {
    uint16_t client_idx;
    uint16_t sub_idx;
} broker_delivery_match;

typedef struct {
    uint32_t hash;
    uint16_t length;
} broker_topic_prefix;

typedef struct {
    /* The single broker event loop reuses this non-reentrant workspace. */
    broker_delivery_match matches[VBUS_MAX_CLIENTS * VBUS_MAX_SUBS];
    broker_topic_prefix prefixes[256];
    unsigned char delivered_flags[VBUS_MAX_CLIENTS * VBUS_MAX_SUBS];
    int group[VBUS_MAX_CLIENTS * VBUS_MAX_SUBS];
} broker_delivery_scratch;

typedef struct {
    int fd;
    dynbuf rx;
    size_t rx_head;
    dynbuf tx;
    size_t tx_head;
    int active;
    int failed;
    uint64_t accepted_at_ms;
    int hello_received;
    int flow_subscriber;
    uint64_t flow_blocked_at_ms;
    int publish_blocked;
    int input_hangup;
    broker_sub subs[VBUS_MAX_SUBS];
    int nsubs;
} broker_client;

typedef struct {
    broker_client clients[VBUS_MAX_CLIENTS];
    uint8_t active_clients[VBUS_MAX_CLIENTS];
    uint8_t active_positions[VBUS_MAX_CLIENTS];
    uint8_t free_clients[VBUS_MAX_CLIENTS];
    size_t active_client_count;
    size_t free_client_count;
    size_t pending_hello_count;
    size_t blocked_publish_count;
    size_t flow_subscriber_count;
    int exact_buckets[VBUS_EXACT_BUCKETS];
    int wildcard_buckets[VBUS_EXACT_BUCKETS];
    size_t wildcard_count;
    broker_queue_group queue_groups[VBUS_QUEUE_BUCKETS];
    broker_delivery_scratch delivery;
    size_t buffer_bytes;
    size_t max_buffer_bytes;
    int post_poll_maintenance;
} broker_state;

static int broker_activate_client(
    broker_state *st,
    int fd,
    uint64_t accepted_at_ms
) {
    size_t client_index;
    broker_client *client;
    if (!st || fd < 0 || accepted_at_ms == 0 ||
        st->active_client_count >= VBUS_MAX_CLIENTS ||
        st->free_client_count == 0u) return -1;
    client_index = st->free_clients[st->free_client_count - 1u];
    client = &st->clients[client_index];
    if (client->active) return -1;
    st->free_client_count--;
    client->fd = fd;
    client->active = 1;
    client->accepted_at_ms = accepted_at_ms;
    st->active_positions[client_index] =
        (uint8_t)st->active_client_count;
    st->active_clients[st->active_client_count++] = (uint8_t)client_index;
    st->pending_hello_count++;
    dynbuf_init(&client->rx);
    dynbuf_init(&client->tx);
    return (int)client_index;
}

static void broker_release_client_index(broker_state *st, int client_index) {
    size_t position;
    size_t last_position;
    uint8_t moved_client;
    if (!st || client_index < 0 || client_index >= VBUS_MAX_CLIENTS) return;
    position = st->active_positions[client_index];
    if (position >= st->active_client_count ||
        st->active_clients[position] != (uint8_t)client_index) {
        for (position = 0u; position < st->active_client_count; ++position) {
            if (st->active_clients[position] == (uint8_t)client_index) break;
        }
    }
    if (position < st->active_client_count) {
        last_position = st->active_client_count - 1u;
        moved_client = st->active_clients[last_position];
        st->active_clients[position] = moved_client;
        st->active_positions[moved_client] = (uint8_t)position;
        st->active_client_count = last_position;
    }
    if (st->free_client_count < VBUS_MAX_CLIENTS)
        st->free_clients[st->free_client_count++] = (uint8_t)client_index;
}

static size_t broker_buffer_limit(void) {
    const char *value = getenv("VBUS_BROKER_MAX_BUFFER_BYTES");
    char *end = NULL;
    unsigned long long parsed;

    if (!value || !value[0] || value[0] == '-')
        return VBUS_BROKER_DEFAULT_BUFFER_BYTES;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < VBUS_BROKER_MIN_BUFFER_BYTES ||
        parsed > VBUS_BROKER_MAX_BUFFER_BYTES)
        return VBUS_BROKER_DEFAULT_BUFFER_BYTES;
    return (size_t)parsed;
}

static int broker_buffer_reserve(
    broker_state *st,
    dynbuf *buffer,
    size_t *head,
    size_t need
) {
    size_t old_capacity;
    size_t available;
    size_t buffer_limit;
    if (!st || !buffer || !head || st->buffer_bytes > st->max_buffer_bytes) return -1;
    old_capacity = buffer->cap;
    available = st->max_buffer_bytes - st->buffer_bytes;
    if (old_capacity > SIZE_MAX - available) return -1;
    buffer_limit = old_capacity + available;
    if (receive_window_prepare_bounded(
            buffer, head, need, buffer_limit) != 0) return -1;
    st->buffer_bytes += buffer->cap - old_capacity;
    return 0;
}

static void broker_buffer_shrink(broker_state *st, dynbuf *buffer, size_t min_keep) {
    size_t old_capacity;
    size_t released;
    if (!st || !buffer) return;
    old_capacity = buffer->cap;
    dynbuf_clear(buffer, min_keep);
    if (old_capacity < buffer->cap) return;
    released = old_capacity - buffer->cap;
    if (released <= st->buffer_bytes) st->buffer_bytes -= released;
    else st->buffer_bytes = 0;
}

static void broker_buffer_free(broker_state *st, dynbuf *buffer) {
    if (!st || !buffer) return;
    if (buffer->cap <= st->buffer_bytes) st->buffer_bytes -= buffer->cap;
    else st->buffer_bytes = 0;
    dynbuf_free(buffer);
}

static uint64_t broker_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static uint32_t broker_topic_hash_n(const char *topic, size_t topic_len) {
    const unsigned char *p = (const unsigned char *)topic;
    uint32_t hash = UINT32_C(2166136261);
    size_t i;

    for (i = 0; i < topic_len; ++i) {
        hash ^= p[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static int broker_topic_is_wildcard(const char *topic, size_t topic_len) {
    return topic && topic_len > 0u && topic[topic_len - 1u] == '>';
}

static int broker_wildcard_prefix_length(
    const char *topic,
    size_t topic_len
) {
    if (!topic) return -1;
    if (topic_len == 1u && topic[0] == '>') return 0;
    if (topic_len >= 2u && topic[topic_len - 2u] == '.' &&
        topic[topic_len - 1u] == '>')
        return (int)(topic_len - 1u);
    return -1;
}

static int broker_sub_id(int client_idx, int sub_idx) {
    return client_idx * VBUS_MAX_SUBS + sub_idx;
}

static broker_sub *broker_sub_from_id(broker_state *st, int id) {
    int client_idx;
    int sub_idx;

    if (!st || id < 0 || id >= VBUS_MAX_CLIENTS * VBUS_MAX_SUBS) return NULL;
    client_idx = id / VBUS_MAX_SUBS;
    sub_idx = id % VBUS_MAX_SUBS;
    return &st->clients[client_idx].subs[sub_idx];
}

static int broker_queue_group_get(
    broker_state *st,
    const char *queue,
    size_t queue_len
) {
    size_t first_tombstone = VBUS_QUEUE_BUCKETS;
    size_t start;
    size_t probe;

    if (!st || !queue || queue_len == 0u) return VBUS_SUB_INDEX_NONE;
    start = (size_t)(
        broker_topic_hash_n(queue, queue_len) & (VBUS_QUEUE_BUCKETS - 1u));
    for (probe = 0; probe < VBUS_QUEUE_BUCKETS; ++probe) {
        size_t slot = (start + probe) & (VBUS_QUEUE_BUCKETS - 1u);
        broker_queue_group *group = &st->queue_groups[slot];

        if (group->state == BROKER_QUEUE_TOMBSTONE) {
            if (first_tombstone == VBUS_QUEUE_BUCKETS) first_tombstone = slot;
            continue;
        }
        if (group->state == BROKER_QUEUE_EMPTY) {
            if (first_tombstone != VBUS_QUEUE_BUCKETS) slot = first_tombstone;
            group = &st->queue_groups[slot];
            group->head = VBUS_SUB_INDEX_NONE;
            group->cursor = 0;
            group->state = BROKER_QUEUE_OCCUPIED;
            return (int)slot;
        }
        {
            broker_sub *representative = broker_sub_from_id(st, group->head);
            if (representative &&
                representative->queue_len == queue_len &&
                memcmp(representative->queue, queue, queue_len) == 0)
                return (int)slot;
        }
    }
    if (first_tombstone != VBUS_QUEUE_BUCKETS) {
        broker_queue_group *group = &st->queue_groups[first_tombstone];
        group->head = VBUS_SUB_INDEX_NONE;
        group->cursor = 0;
        group->state = BROKER_QUEUE_OCCUPIED;
        return (int)first_tombstone;
    }
    return VBUS_SUB_INDEX_NONE;
}

static int broker_index_queue(broker_state *st, broker_sub *sub, int id) {
    int slot;
    broker_queue_group *group;

    sub->next_queue = VBUS_SUB_INDEX_NONE;
    sub->queue_group_slot = VBUS_SUB_INDEX_NONE;
    if (sub->queue_len == 0u) return 0;
    slot = broker_queue_group_get(st, sub->queue, sub->queue_len);
    if (slot == VBUS_SUB_INDEX_NONE) return -1;
    group = &st->queue_groups[slot];
    sub->next_queue = group->head;
    sub->queue_group_slot = slot;
    group->head = id;
    return 0;
}

static void broker_unindex_queue(broker_state *st, broker_sub *sub, int id) {
    int slot;
    broker_queue_group *group;
    int *link;

    if (!st || !sub) return;
    slot = sub->queue_group_slot;
    if (slot < 0 || slot >= VBUS_QUEUE_BUCKETS) return;
    group = &st->queue_groups[slot];
    if (group->state != BROKER_QUEUE_OCCUPIED) return;
    link = &group->head;
    while (*link != VBUS_SUB_INDEX_NONE) {
        broker_sub *entry = broker_sub_from_id(st, *link);
        if (*link == id) {
            *link = entry ? entry->next_queue : VBUS_SUB_INDEX_NONE;
            break;
        }
        if (!entry) break;
        link = &entry->next_queue;
    }
    sub->next_queue = VBUS_SUB_INDEX_NONE;
    sub->queue_group_slot = VBUS_SUB_INDEX_NONE;
    if (group->head == VBUS_SUB_INDEX_NONE) {
        group->cursor = 0;
        group->state = BROKER_QUEUE_TOMBSTONE;
    }
}

static int broker_index_sub(broker_state *st, int client_idx, int sub_idx) {
    broker_sub *sub = &st->clients[client_idx].subs[sub_idx];
    int id = broker_sub_id(client_idx, sub_idx);

    if (broker_index_queue(st, sub, id) != 0) return -1;
    sub->index.next_exact = VBUS_SUB_INDEX_NONE;
    if (broker_topic_is_wildcard(sub->topic, sub->topic_len)) {
        int prefix_length = broker_wildcard_prefix_length(
            sub->topic,
            sub->topic_len);
        size_t bucket;
        if (prefix_length < 0) {
            broker_unindex_queue(st, sub, id);
            return -1;
        }
        bucket = (size_t)(
            broker_topic_hash_n(sub->topic, (size_t)prefix_length) &
            (VBUS_EXACT_BUCKETS - 1u));
        sub->index.wildcard.prefix_length = (uint16_t)prefix_length;
        sub->index.wildcard.next = st->wildcard_buckets[bucket];
        st->wildcard_buckets[bucket] = id;
        st->wildcard_count++;
        return 0;
    }
    {
        size_t bucket = (size_t)(
            broker_topic_hash_n(sub->topic, sub->topic_len) &
            (VBUS_EXACT_BUCKETS - 1u));
        sub->index.next_exact = st->exact_buckets[bucket];
        st->exact_buckets[bucket] = id;
    }
    return 0;
}

static void broker_unindex_sub(broker_state *st, int client_idx, int sub_idx) {
    broker_sub *sub = &st->clients[client_idx].subs[sub_idx];
    int id = broker_sub_id(client_idx, sub_idx);

    broker_unindex_queue(st, sub, id);
    if (broker_topic_is_wildcard(sub->topic, sub->topic_len)) {
        size_t prefix_length = sub->index.wildcard.prefix_length;
        size_t bucket = (size_t)(
            broker_topic_hash_n(sub->topic, prefix_length) &
            (VBUS_EXACT_BUCKETS - 1u));
        int *link = &st->wildcard_buckets[bucket];
        while (*link != VBUS_SUB_INDEX_NONE) {
            broker_sub *entry = broker_sub_from_id(st, *link);
            if (*link == id) {
                *link = entry ?
                    entry->index.wildcard.next : VBUS_SUB_INDEX_NONE;
                if (st->wildcard_count != 0u) st->wildcard_count--;
                break;
            }
            if (!entry) break;
            link = &entry->index.wildcard.next;
        }
        sub->index.wildcard.next = VBUS_SUB_INDEX_NONE;
        sub->index.wildcard.prefix_length = 0u;
        return;
    }
    {
        size_t bucket = (size_t)(
            broker_topic_hash_n(sub->topic, sub->topic_len) &
            (VBUS_EXACT_BUCKETS - 1u));
        int *link = &st->exact_buckets[bucket];
        while (*link != VBUS_SUB_INDEX_NONE) {
            broker_sub *entry = broker_sub_from_id(st, *link);
            if (*link == id) {
                *link = entry ? entry->index.next_exact : VBUS_SUB_INDEX_NONE;
                break;
            }
            if (!entry) break;
            link = &entry->index.next_exact;
        }
    }
    sub->index.next_exact = VBUS_SUB_INDEX_NONE;
}

static size_t broker_pending(const broker_client *cl) {
    return cl ? cl->tx.len : 0;
}

static int broker_tx_reserve(broker_state *st, broker_client *cl, size_t need) {
    uint8_t *next;
    size_t next_cap;
    size_t available;
    size_t client_limit;
    size_t first;
    if (!st || !cl || need > VBUS_MAX_PENDING ||
        st->buffer_bytes > st->max_buffer_bytes) return -1;
    if (need <= cl->tx.cap) return 0;
    available = st->max_buffer_bytes - st->buffer_bytes;
    if (cl->tx.cap > SIZE_MAX - available) return -1;
    client_limit = cl->tx.cap + available;
    if (client_limit > VBUS_MAX_PENDING) client_limit = VBUS_MAX_PENDING;
    if (need > client_limit) return -1;
    next_cap = cl->tx.cap ? cl->tx.cap : 256u;
    if (next_cap > client_limit) next_cap = client_limit;
    while (next_cap < need) {
        if (next_cap > client_limit / 2u) {
            next_cap = client_limit;
            break;
        }
        next_cap *= 2u;
    }
    if (next_cap < need) return -1;
    if (cl->tx_head == 0u) {
        next = (uint8_t *)realloc(cl->tx.data, next_cap);
        if (!next) return -1;
        cl->tx.data = next;
        st->buffer_bytes += next_cap - cl->tx.cap;
        cl->tx.cap = next_cap;
        return 0;
    }
    next = (uint8_t *)malloc(next_cap);
    if (!next) return -1;
    first = cl->tx.len;
    if (first > cl->tx.cap - cl->tx_head) first = cl->tx.cap - cl->tx_head;
    if (first != 0) memcpy(next, cl->tx.data + cl->tx_head, first);
    if (cl->tx.len > first) memcpy(next + first, cl->tx.data, cl->tx.len - first);
    free(cl->tx.data);
    cl->tx.data = next;
    st->buffer_bytes += next_cap - cl->tx.cap;
    cl->tx.cap = next_cap;
    cl->tx_head = 0;
    return 0;
}

static VBUS_NOINLINE int broker_tx_append_spans_reserved(
    broker_client *cl,
    const broker_span *spans,
    size_t span_count,
    size_t total_len
) {
    size_t tail;
    size_t contiguous;
    size_t i;
    if (!cl || (!spans && span_count != 0u) || span_count > 5u ||
        cl->tx.len > cl->tx.cap || total_len > cl->tx.cap - cl->tx.len)
        return -1;
    if (total_len == 0u) return 0;

    /* The caller has validated every nonempty span and their combined frame
     * length. Compute the circular tail once for the complete frame. */
    tail = cl->tx_head + cl->tx.len;
    if (tail >= cl->tx.cap) tail -= cl->tx.cap;
    contiguous = cl->tx.cap - tail;
    for (i = 0u; i < span_count; ++i) {
        size_t first;
        if (spans[i].len == 0u) continue;
        first = spans[i].len;
        if (first > contiguous) first = contiguous;
        memcpy(cl->tx.data + tail, spans[i].data, first);
        tail += first;
        contiguous -= first;
        if (tail == cl->tx.cap) {
            tail = 0u;
            contiguous = cl->tx.cap;
        }
        if (spans[i].len > first) {
            size_t second = spans[i].len - first;
            memcpy(cl->tx.data + tail, spans[i].data + first, second);
            tail += second;
            contiguous -= second;
        }
    }
    cl->tx.len += total_len;
    return 0;
}

static int broker_queue_frame(
    broker_state *st,
    broker_client *cl,
    uint32_t op,
    uint32_t subscription_id,
    const broker_publication_view *publication,
    const char *queue,
    size_t queue_len
) {
    vbus_hdr h;
    broker_span spans[5];
    size_t frame_len;
    size_t pending;
    if (!st || !cl || cl->failed || !publication ||
        (publication->topic_len != 0u && !publication->topic) ||
        (queue_len != 0u && !queue) ||
        (publication->reply_len != 0u && !publication->reply) ||
        (publication->body_len != 0u && !publication->body) ||
        frame_size(
            publication->topic_len,
            queue_len,
            publication->reply_len,
            publication->body_len,
            &frame_len) != 0) return -1;
    pending = broker_pending(cl);
    if (frame_len > VBUS_MAX_PENDING || pending > VBUS_MAX_PENDING - frame_len) return -1;
    if (pending + frame_len > cl->tx.cap &&
        broker_tx_reserve(st, cl, pending + frame_len) != 0) return -1;

    memset(&h, 0, sizeof(h));
    h.magic = VBUS_MAGIC;
    h.version = VBUS_VERSION;
    h.op = op;
    h.subscription_id = subscription_id;
    h.topic_len = (uint32_t)publication->topic_len;
    h.queue_len = (uint32_t)queue_len;
    h.reply_len = (uint32_t)publication->reply_len;
    h.body_len = (uint32_t)publication->body_len;
    spans[0].data = (const uint8_t *)&h;
    spans[0].len = sizeof(h);
    spans[1].data = (const uint8_t *)publication->topic;
    spans[1].len = publication->topic_len;
    spans[2].data = (const uint8_t *)queue;
    spans[2].len = queue_len;
    spans[3].data = (const uint8_t *)publication->reply;
    spans[3].len = publication->reply_len;
    spans[4].data = publication->body;
    spans[4].len = publication->body_len;
    if (broker_tx_append_spans_reserved(
            cl, spans, sizeof(spans) / sizeof(spans[0]), frame_len) != 0)
        return -1;
    st->post_poll_maintenance = 1;
    return 0;
}

static int broker_flush(broker_state *st, broker_client *cl) {
    while (cl && broker_pending(cl) != 0) {
        struct iovec vectors[2];
        struct msghdr message;
        size_t first = cl->tx.len;
        ssize_t n;

        if (first > cl->tx.cap - cl->tx_head)
            first = cl->tx.cap - cl->tx_head;
        memset(&message, 0, sizeof(message));
        vectors[0].iov_base = cl->tx.data + cl->tx_head;
        vectors[0].iov_len = first;
        message.msg_iov = vectors;
        message.msg_iovlen = 1;
        if (cl->tx.len > first) {
            vectors[1].iov_base = cl->tx.data;
            vectors[1].iov_len = cl->tx.len - first;
            message.msg_iovlen = 2;
        }
        n = sendmsg(cl->fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            size_t sent = (size_t)n;
            cl->tx_head = (cl->tx_head + sent) % cl->tx.cap;
            cl->tx.len -= sent;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    if (cl && cl->tx.len == 0) {
        cl->tx_head = 0;
        if (cl->tx.cap > VBUS_RBUF_SOFT_MIN)
            broker_buffer_shrink(st, &cl->tx, VBUS_RBUF_SOFT_MIN);
    }
    return 0;
}

static int broker_emit(
    broker_state *st,
    broker_client *cl,
    uint32_t op,
    uint32_t subscription_id,
    const broker_publication_view *publication,
    const char *queue,
    size_t queue_len
) {
    vbus_hdr h;
    struct iovec vectors[5];
    size_t frame_len;
    broker_span spans[5];
    size_t sent = 0;
    size_t i;
    size_t vector_count = 0;
    ssize_t result;
    if (!st || !cl || cl->failed || !publication ||
        (publication->topic_len != 0u && !publication->topic) ||
        (queue_len != 0u && !queue) ||
        (publication->reply_len != 0u && !publication->reply) ||
        (publication->body_len != 0u && !publication->body) ||
        frame_size(
            publication->topic_len,
            queue_len,
            publication->reply_len,
            publication->body_len,
            &frame_len) != 0) return -1;
    if (broker_pending(cl) != 0)
        return broker_queue_frame(
            st,
            cl,
            op,
            subscription_id,
            publication,
            queue,
            queue_len);

    /* Reserve fallback space before the first byte is sent. The common path
     * sends the complete frame without a copy or an extra system call. */
    if (frame_len > VBUS_MAX_PENDING ||
        (frame_len > cl->tx.cap &&
         broker_tx_reserve(st, cl, frame_len) != 0)) return -1;
    memset(&h, 0, sizeof(h));
    h.magic = VBUS_MAGIC;
    h.version = VBUS_VERSION;
    h.op = op;
    h.subscription_id = subscription_id;
    h.topic_len = (uint32_t)publication->topic_len;
    h.queue_len = (uint32_t)queue_len;
    h.reply_len = (uint32_t)publication->reply_len;
    h.body_len = (uint32_t)publication->body_len;
    spans[0].data = (const uint8_t *)&h;
    spans[0].len = sizeof(h);
    spans[1].data = (const uint8_t *)publication->topic;
    spans[1].len = publication->topic_len;
    spans[2].data = (const uint8_t *)queue;
    spans[2].len = queue_len;
    spans[3].data = (const uint8_t *)publication->reply;
    spans[3].len = publication->reply_len;
    spans[4].data = publication->body;
    spans[4].len = publication->body_len;
    for (i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        if (spans[i].len == 0) continue;
        vectors[vector_count].iov_base = (void *)spans[i].data;
        vectors[vector_count++].iov_len = spans[i].len;
    }
    do {
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = vectors;
#if defined(__APPLE__)
        /* At most five spans above; Darwin uses int for msg_iovlen. */
        message.msg_iovlen = (int)vector_count;
#else
        message.msg_iovlen = vector_count;
#endif
        result = sendmsg(cl->fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    if (result > 0) sent = (size_t)result;
    if (sent == frame_len) {
        if (cl->tx.cap > VBUS_RBUF_SOFT_MIN)
            broker_buffer_shrink(st, &cl->tx, VBUS_RBUF_SOFT_MIN);
        return 0;
    }
    frame_len -= sent;
    for (i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        if (sent >= spans[i].len) {
            sent -= spans[i].len;
            continue;
        }
        spans[i].data += sent;
        spans[i].len -= sent;
        if (broker_tx_append_spans_reserved(
                cl,
                spans + i,
                sizeof(spans) / sizeof(spans[0]) - i,
                frame_len) != 0) return -1;
        break;
    }
    st->post_poll_maintenance = 1;
    return 0;
}

static void broker_close_client(broker_state *st, int client_idx) {
    broker_client *cl;
    int i;

    if (!st || client_idx < 0 || client_idx >= VBUS_MAX_CLIENTS) return;
    cl = &st->clients[client_idx];
    if (!cl || !cl->active) return;
    for (i = 0; i < cl->nsubs; ++i) {
        if (cl->subs[i].active)
            broker_unindex_sub(st, client_idx, i);
    }
    close(cl->fd);
    broker_buffer_free(st, &cl->rx);
    broker_buffer_free(st, &cl->tx);
    if (!cl->hello_received && st->pending_hello_count != 0u)
        st->pending_hello_count--;
    if (cl->publish_blocked && st->blocked_publish_count != 0u)
        st->blocked_publish_count--;
    if (cl->flow_subscriber && st->flow_subscriber_count != 0u)
        st->flow_subscriber_count--;
    broker_release_client_index(st, client_idx);
    memset(cl, 0, sizeof(*cl));
}

static int broker_ensure(broker_state *st, broker_client *cl, size_t need) {
    if (!st || !cl) return -1;
    return broker_buffer_reserve(st, &cl->rx, &cl->rx_head, need);
}

static int broker_flow_ready(broker_state *st, size_t bucket,
    const broker_publication_view *publication) {
    int id = st->exact_buckets[bucket];
    size_t frame_len;
    if (frame_size(publication->topic_len, 0u, publication->reply_len,
            publication->body_len, &frame_len) != 0) return -1;
    /* Preflight before any fanout or queue-group cursor changes. Retrying a
     * held publisher frame therefore cannot duplicate another delivery. */
    while (id != VBUS_SUB_INDEX_NONE) {
        int client_idx = id / VBUS_MAX_SUBS;
        broker_client *cl = &st->clients[client_idx];
        broker_sub *sub = &cl->subs[id % VBUS_MAX_SUBS];
        int next = sub->index.next_exact;
        if (cl->active && !cl->failed && cl->flow_subscriber && sub->active &&
            sub->topic_len == publication->topic_len &&
            memcmp(sub->topic, publication->topic, publication->topic_len) == 0) {
            size_t pending = broker_pending(cl);
            if (pending <= VBUS_MAX_PENDING / 2u) cl->flow_blocked_at_ms = 0u;
            if (frame_len > VBUS_MAX_PENDING - pending) {
                uint64_t now = broker_monotonic_ms();
                if (cl->flow_blocked_at_ms == 0u) cl->flow_blocked_at_ms = now;
                if (now == 0u || now < cl->flow_blocked_at_ms ||
                    now - cl->flow_blocked_at_ms >= VBUS_FLOW_STALL_TIMEOUT_MS) {
                    broker_close_client(st, client_idx);
                } else {
                    return 1;
                }
            }
        }
        id = next;
    }
    return 0;
}

static int broker_deliver(
    broker_state *st,
    const broker_publication_view *publication
) {
    /* Collect matching subs; for each unique queue name, pick one client RR. */
    broker_delivery_scratch *scratch;
    uint32_t topic_hash = UINT32_C(2166136261);
    size_t prefix_count = 0;
    size_t topic_index;
    int nmatch = 0;
    int i;

    if (!st || !publication || !publication->topic ||
        publication->topic_len == 0u || publication->topic_len >= 256u ||
        (publication->reply_len != 0u && !publication->reply) ||
        (publication->body_len != 0u && !publication->body)) return -1;
    scratch = &st->delivery;
    if (st->wildcard_count != 0u) {
        scratch->prefixes[prefix_count].hash = topic_hash;
        scratch->prefixes[prefix_count++].length = 0u;
    }
    for (topic_index = 0; topic_index < publication->topic_len; ++topic_index) {
        topic_hash ^= (unsigned char)publication->topic[topic_index];
        topic_hash *= UINT32_C(16777619);
        if (st->wildcard_count != 0u &&
            publication->topic[topic_index] == '.') {
            scratch->prefixes[prefix_count].hash = topic_hash;
            scratch->prefixes[prefix_count++].length =
                (uint16_t)(topic_index + 1u);
        }
    }
    {
        size_t bucket = (size_t)(topic_hash & (VBUS_EXACT_BUCKETS - 1u));
        if (st->flow_subscriber_count != 0u) {
            int flow_status = broker_flow_ready(st, bucket, publication);
            if (flow_status != 0) return flow_status;
        }
        int id = st->exact_buckets[bucket];
        while (id != VBUS_SUB_INDEX_NONE) {
            int client_idx = id / VBUS_MAX_SUBS;
            int sub_idx = id % VBUS_MAX_SUBS;
            broker_client *cl = &st->clients[client_idx];
            broker_sub *sub = &cl->subs[sub_idx];
            int next = sub->index.next_exact;
            if (cl->active && sub->active &&
                sub->topic_len == publication->topic_len &&
                memcmp(
                    sub->topic,
                    publication->topic,
                    publication->topic_len) == 0) {
                if (sub->queue_len == 0u) {
                    if (broker_emit(
                            st, cl, VBUS_OP_MSG, sub->subscription_id,
                            publication, NULL, 0u) != 0) {
                        cl->failed = 1;
                        st->post_poll_maintenance = 1;
                    }
                } else if (nmatch <
                           (int)(sizeof(scratch->matches) /
                                 sizeof(scratch->matches[0]))) {
                    scratch->matches[nmatch].client_idx = (uint16_t)client_idx;
                    scratch->matches[nmatch].sub_idx = (uint16_t)sub_idx;
                    nmatch++;
                }
            }
            id = next;
        }
    }
    {
        size_t prefix_index;
        for (prefix_index = 0; prefix_index < prefix_count; ++prefix_index) {
            size_t bucket = (size_t)(
                scratch->prefixes[prefix_index].hash &
                (VBUS_EXACT_BUCKETS - 1u));
            int id = st->wildcard_buckets[bucket];
            while (id != VBUS_SUB_INDEX_NONE) {
                int client_idx = id / VBUS_MAX_SUBS;
                int sub_idx = id % VBUS_MAX_SUBS;
                broker_client *cl = &st->clients[client_idx];
                broker_sub *sub = &cl->subs[sub_idx];
                int next = sub->index.wildcard.next;
                size_t prefix_length = sub->index.wildcard.prefix_length;
                if (cl->active && sub->active &&
                    prefix_length == scratch->prefixes[prefix_index].length &&
                    memcmp(
                        sub->topic,
                        publication->topic,
                        prefix_length) == 0) {
                    if (sub->queue_len == 0u) {
                        if (broker_emit(
                                st, cl, VBUS_OP_MSG, sub->subscription_id,
                                publication, NULL, 0u) != 0) {
                            cl->failed = 1;
                            st->post_poll_maintenance = 1;
                        }
                    } else {
                        if (nmatch >=
                            (int)(sizeof(scratch->matches) /
                                  sizeof(scratch->matches[0]))) break;
                        scratch->matches[nmatch].client_idx =
                            (uint16_t)client_idx;
                        scratch->matches[nmatch].sub_idx = (uint16_t)sub_idx;
                        nmatch++;
                    }
                }
                id = next;
            }
            if (nmatch >=
                (int)(sizeof(scratch->matches) /
                      sizeof(scratch->matches[0]))) break;
        }
    }

    if (nmatch == 0) return 0;

    /* Fanout already delivered during lookup. Group queued matches and select
     * one member from each queue with that queue's own round-robin cursor. */
    {
        memset(scratch->delivered_flags, 0, (size_t)nmatch);
        for (i = 0; i < nmatch; ++i) {
            if (scratch->delivered_flags[i]) continue;
            broker_sub *match_sub =
                &st->clients[scratch->matches[i].client_idx]
                     .subs[scratch->matches[i].sub_idx];
            const char *match_queue = match_sub->queue;
            size_t match_queue_len = match_sub->queue_len;
            int ng = 0;
            int k;
            for (k = i; k < nmatch; ++k) {
                const broker_sub *candidate =
                    &st->clients[scratch->matches[k].client_idx]
                         .subs[scratch->matches[k].sub_idx];
                if (candidate->queue_group_slot == match_sub->queue_group_slot) {
                    scratch->group[ng++] = k;
                    scratch->delivered_flags[k] = 1;
                }
            }
            if (ng > 0) {
                broker_queue_group *queue_group =
                    &st->queue_groups[match_sub->queue_group_slot];
                int pick = (int)(queue_group->cursor++ % (uint32_t)ng);
                int idx = scratch->group[pick];
                broker_client *cl =
                    &st->clients[scratch->matches[idx].client_idx];
                broker_sub *selected =
                    &cl->subs[scratch->matches[idx].sub_idx];
                if (broker_emit(
                        st, cl, VBUS_OP_MSG, selected->subscription_id,
                        publication, match_queue, match_queue_len) != 0) {
                    cl->failed = 1;
                    st->post_poll_maintenance = 1;
                }
            }
        }
    }
    return 0;
}

static int broker_handle_frame(broker_state *st, int client_idx, const vbus_hdr *h,
                               const uint8_t *rest) {
    broker_client *cl = &st->clients[client_idx];
    const char *topic = (const char *)rest;
    const char *queue = (const char *)(rest + h->topic_len);
    const char *reply = (const char *)(rest + h->topic_len + h->queue_len);
    const uint8_t *body = rest + h->topic_len + h->queue_len + h->reply_len;
    size_t tl = h->topic_len;
    size_t ql = h->queue_len;
    size_t rl = h->reply_len;
    if (tl >= sizeof(cl->subs[0].topic) ||
        ql >= sizeof(cl->subs[0].queue) ||
        rl >= sizeof(cl->subs[0].topic)) return -1;
    if ((tl && memchr(topic, '\0', tl) != NULL) ||
        (ql && memchr(queue, '\0', ql) != NULL) ||
        (rl && memchr(reply, '\0', rl) != NULL)) return -1;

    if (h->op == VBUS_OP_HELLO) {
        const broker_publication_view acknowledgement = {
            "hello",
            NULL,
            NULL,
            sizeof("hello") - 1u,
            0u,
            0u,
        };
        if (h->subscription_id != VBUS_SUBSCRIPTION_NONE ||
            cl->hello_received || tl != sizeof("client") - 1u ||
            memcmp(topic, "client", sizeof("client") - 1u) != 0 ||
            ql != 0 || rl != 0 || h->body_len != 0) return -1;
        cl->hello_received = 1;
        if (st->pending_hello_count != 0u) st->pending_hello_count--;
        return broker_emit(
            st, cl, VBUS_OP_OK, VBUS_SUBSCRIPTION_NONE,
            &acknowledgement, NULL, 0u);
    }
    if (!cl->hello_received) return -1;
    if (cl->flow_subscriber) return -1;
    if (h->op == VBUS_OP_SUB || h->op == VBUS_OP_SUB_FLOW) {
        const broker_publication_view acknowledgement = {
            "sub",
            NULL,
            NULL,
            sizeof("sub") - 1u,
            0u,
            0u,
        };
        int sub_idx;
        if (tl == 0 || rl != 0 || h->body_len != 0 || cl->nsubs >= VBUS_MAX_SUBS ||
            h->subscription_id != (uint32_t)cl->nsubs)
            return -1;
        if (h->op == VBUS_OP_SUB_FLOW && (cl->nsubs != 0 || ql != 0 ||
                memchr(topic, '>', tl) != NULL || memchr(topic, '*', tl) != NULL))
            return -1;
        sub_idx = cl->nsubs++;
        broker_sub *s = &cl->subs[sub_idx];
        memset(s, 0, sizeof(*s));
        memcpy(s->topic, topic, tl);
        s->topic[tl] = '\0';
        s->topic_len = (uint8_t)tl;
        if (ql != 0u) memcpy(s->queue, queue, ql);
        s->queue[ql] = '\0';
        s->queue_len = (uint8_t)ql;
        s->subscription_id = h->subscription_id;
        s->active = 1;
        if (broker_index_sub(st, client_idx, sub_idx) != 0) {
            memset(s, 0, sizeof(*s));
            cl->nsubs--;
            return -1;
        }
        cl->flow_subscriber = h->op == VBUS_OP_SUB_FLOW;
        if (cl->flow_subscriber) st->flow_subscriber_count++;
        return broker_emit(
            st, cl, VBUS_OP_OK, h->subscription_id,
            &acknowledgement, NULL, 0u);
    }
    if (h->op == VBUS_OP_PUB) {
        broker_publication_view publication;
        if (h->subscription_id != VBUS_SUBSCRIPTION_NONE || tl == 0 || ql != 0)
            return -1;
        publication.topic = topic;
        publication.reply = rl != 0u ? reply : NULL;
        publication.body = body;
        publication.topic_len = tl;
        publication.reply_len = rl;
        publication.body_len = h->body_len;
        return broker_deliver(st, &publication);
    }
    return -1;
}

static int broker_process_client(broker_state *st, int client_idx) {
    broker_client *cl = &st->clients[client_idx];
    if (cl->publish_blocked) {
        cl->publish_blocked = 0;
        st->blocked_publish_count--;
    }
    while (cl->rx.len >= sizeof(vbus_hdr)) {
        vbus_hdr h;
        uint8_t *frame = cl->rx.data + cl->rx_head;
        size_t need;
        memcpy(&h, frame, sizeof(h));
        if (h.magic != VBUS_MAGIC || h.version != VBUS_VERSION) return -1;
        if (frame_size_from_header(&h, &need) != 0)
            return -1;
        if (cl->rx.len < need) return 0;
        {
            int handled = broker_handle_frame(st, client_idx, &h, frame + sizeof(h));
            if (handled < 0) return -1;
            if (handled > 0) {
                cl->publish_blocked = 1;
                st->blocked_publish_count++;
                return 0;
            }
        }
        if (cl->failed) return -1;
        cl->rx.len -= need;
        cl->rx_head += need;
        if (cl->rx.len == 0) cl->rx_head = 0;
    }
    if (cl->rx.len == 0 && cl->rx.cap > VBUS_RBUF_SOFT_MIN) {
        broker_buffer_shrink(st, &cl->rx, VBUS_RBUF_SOFT_MIN);
    }
    return 0;
}

static int broker_read_client(broker_state *st, int client_idx, int drain_to_eof) {
    broker_client *cl;
    if (!st || client_idx < 0 || client_idx >= VBUS_MAX_CLIENTS) return -1;
    cl = &st->clients[client_idx];
    for (;;) {
        size_t read_need = VBUS_MAX_FRAME - cl->rx.len;
        size_t tail;
        size_t space;
        size_t max_space;
        ssize_t received;
        if (read_need > 4096) read_need = 4096;
        if (read_need == 0 || broker_ensure(st, cl, read_need) != 0) return -1;
        tail = cl->rx_head + cl->rx.len;
        space = cl->rx.cap - tail;
        max_space = VBUS_MAX_FRAME - cl->rx.len;
        if (space > max_space) space = max_space;
        received = read(cl->fd, cl->rx.data + tail, space);
        if (received > 0) {
            cl->rx.len += (size_t)received;
            if (broker_process_client(st, client_idx) != 0) return -1;
            if (cl->publish_blocked) return 0;
            if (!drain_to_eof) return 0;
            continue;
        }
        if (received == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return drain_to_eof ? 1 : 0;
        return -1;
    }
}

static int broker_prepare_socket_path(const char *path) {
    struct sockaddr_un address;
    struct stat first;
    struct stat current;
    int probe_fd;
    int connect_error;
    int flags;

    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &first) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(first.st_mode)) {
        errno = EEXIST;
        return -1;
    }
    probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe_fd < 0) return -1;
    flags = fcntl(probe_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(probe_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(probe_fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(probe_fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        close(probe_fd);
        errno = EADDRINUSE;
        return -1;
    }
    connect_error = errno;
    close(probe_fd);
    if (connect_error == EINPROGRESS || connect_error == EAGAIN ||
        connect_error == EALREADY) {
        errno = EADDRINUSE;
        return -1;
    }
    if (connect_error == ENOENT) return 0;
    if (connect_error != ECONNREFUSED) {
        errno = connect_error;
        return -1;
    }
    if (lstat(path, &current) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(current.st_mode) || current.st_dev != first.st_dev ||
        current.st_ino != first.st_ino) {
        errno = EAGAIN;
        return -1;
    }
    return unlink(path);
}

static void broker_unlink_owned_socket(const char *path, const struct stat *owned) {
    struct stat current;
    if (!path || !owned || lstat(path, &current) != 0 ||
        !S_ISSOCK(current.st_mode) || current.st_dev != owned->st_dev ||
        current.st_ino != owned->st_ino) return;
    (void)unlink(path);
}

int vbus_broker_run(const char *unix_path, const vbus_stop_flag *stop) {
    broker_state *st;
    int listen_fd;
    struct sockaddr_un addr;
    struct stat bound_path;
    const char *path = unix_path && unix_path[0] ? unix_path : vbus_default_path();
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    /* Keep the fixed subscription tables off deployment-sensitive stacks. */
    st = (broker_state *)calloc(1u, sizeof(*st));
    if (!st) return -1;
    st->max_buffer_bytes = broker_buffer_limit();
    for (int client = 0; client < VBUS_MAX_CLIENTS; ++client)
        st->free_clients[client] =
            (uint8_t)(VBUS_MAX_CLIENTS - 1 - client);
    st->free_client_count = VBUS_MAX_CLIENTS;
    for (int bucket = 0; bucket < VBUS_EXACT_BUCKETS; ++bucket) {
        st->exact_buckets[bucket] = VBUS_SUB_INDEX_NONE;
        st->wildcard_buckets[bucket] = VBUS_SUB_INDEX_NONE;
    }
    signal(SIGPIPE, SIG_IGN);

    if (broker_prepare_socket_path(path) != 0) {
        free(st);
        return -1;
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        free(st);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        free(st);
        return -1;
    }
    if (lstat(path, &bound_path) != 0 || !S_ISSOCK(bound_path.st_mode)) {
        close(listen_fd);
        free(st);
        return -1;
    }
    if (listen(listen_fd, 32) != 0) {
        close(listen_fd);
        broker_unlink_owned_socket(path, &bound_path);
        free(st);
        return -1;
    }

    while (!stop || atomic_load_explicit(stop, memory_order_relaxed) == 0) {
        struct pollfd pfds[VBUS_MAX_CLIENTS + 1];
        uint8_t client_for_poll[VBUS_MAX_CLIENTS + 1];
        int np = 0;
        int i;
        pfds[np].fd = listen_fd;
        pfds[np].events = POLLIN;
        client_for_poll[np] = 0u;
        np++;
        for (size_t active = 0u; active < st->active_client_count; ++active) {
            i = st->active_clients[active];
            pfds[np].fd = st->clients[i].input_hangup && st->clients[i].publish_blocked ?
                -1 : st->clients[i].fd;
            pfds[np].events = (st->clients[i].publish_blocked ? 0 : POLLIN) |
                (broker_pending(&st->clients[i]) ? POLLOUT : 0);
            client_for_poll[np] = st->active_clients[active];
            np++;
        }
        if (poll(pfds, (nfds_t)np, 200) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int flags = fcntl(cfd, F_GETFL, 0);
                if (flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) != 0) {
                    close(cfd);
                    cfd = -1;
                }
                if (cfd >= 0) {
                    i = broker_activate_client(
                        st, cfd, broker_monotonic_ms());
                    if (i >= 0) {
                        if (broker_buffer_reserve(
                                st, &st->clients[i].rx,
                                &st->clients[i].rx_head, 8192) != 0 ||
                            broker_tx_reserve(st, &st->clients[i], 8192) != 0)
                            broker_close_client(st, i);
                        cfd = -1;
                    }
                }
                if (cfd >= 0) close(cfd);
            }
        }
        {
            int p;
            for (p = 1; p < np; ++p) {
                i = client_for_poll[p];
                if (!st->clients[i].active) continue;
                if (pfds[p].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                    int read_result = 0;
                    if (pfds[p].revents & (POLLHUP | POLLERR))
                        st->clients[i].input_hangup = 1;
                    if (!st->clients[i].publish_blocked &&
                        (pfds[p].revents & (POLLIN | POLLHUP | POLLERR)))
                        read_result = broker_read_client(
                            st, i, st->clients[i].input_hangup);
                    if (read_result != 0 ||
                        (pfds[p].revents & POLLNVAL)) {
                        broker_close_client(st, i);
                    }
                }
                if (st->clients[i].active && (pfds[p].revents & POLLOUT) &&
                    broker_flush(st, &st->clients[i]) != 0) broker_close_client(st, i);
            }
        }
        /* Frames queued while processing POLLIN were not represented in this
         * poll snapshot. A nonblocking flush preserves one-pass TTFA without
         * letting a slow subscriber stall the broker. */
        if (st->post_poll_maintenance) {
            size_t active = 0u;
            st->post_poll_maintenance = 0;
            while (active < st->active_client_count) {
                i = st->active_clients[active];
                if (st->clients[i].failed ||
                    (broker_pending(&st->clients[i]) != 0 &&
                     broker_flush(st, &st->clients[i]) != 0))
                    broker_close_client(st, i);
                else
                    active++;
            }
        }
        if (st->blocked_publish_count != 0u) {
            size_t active = 0u;
            while (active < st->active_client_count) {
                int result = 0;
                i = st->active_clients[active];
                if (st->clients[i].publish_blocked) {
                    result = broker_process_client(st, i);
                    if (result == 0 && st->clients[i].input_hangup &&
                        !st->clients[i].publish_blocked)
                        result = broker_read_client(st, i, 1);
                }
                if (result != 0)
                    broker_close_client(st, i);
                else
                    active++;
            }
        }
        if (st->pending_hello_count != 0u) {
            uint64_t now_ms = broker_monotonic_ms();
            size_t active = 0u;
            while (active < st->active_client_count) {
                broker_client *cl;
                i = st->active_clients[active];
                cl = &st->clients[i];
                if (!cl->hello_received &&
                    (now_ms == 0 || now_ms < cl->accepted_at_ms ||
                     now_ms - cl->accepted_at_ms >= VBUS_HANDSHAKE_TIMEOUT_MS))
                    broker_close_client(st, i);
                else
                    active++;
            }
        }
    }
    while (st->active_client_count != 0u)
        broker_close_client(
            st, st->active_clients[st->active_client_count - 1u]);
    close(listen_fd);
    broker_unlink_owned_socket(path, &bound_path);
    free(st);
    return 0;
}
