#define _POSIX_C_SOURCE 200809L

#include "vbus.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum {
    BENCH_CAPACITY = 4096,
    BENCH_HEAD = 4000,
    BENCH_BYTES = 256,
    BENCH_FRAME_HEADER_BYTES = VBUS_FRAME_HEADER_BYTES,
    BENCH_BATCHES = 1000,
    BENCH_OPS_PER_BATCH = 128,
};

typedef struct {
    uint8_t data[BENCH_CAPACITY];
    size_t head;
    size_t len;
} bench_ring;

typedef struct {
    double batches[BENCH_BATCHES];
    uint64_t send_calls;
    uint64_t checksum;
} bench_result;

#if defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static void ring_reset(bench_ring *ring, const uint8_t *payload)
{
    size_t first = BENCH_CAPACITY - BENCH_HEAD;

    memcpy(ring->data + BENCH_HEAD, payload, first);
    memcpy(ring->data, payload + first, BENCH_BYTES - first);
    ring->head = BENCH_HEAD;
    ring->len = BENCH_BYTES;
}

static BENCH_NOINLINE int flush_contiguous(
    int fd,
    bench_ring *ring,
    uint64_t *calls
)
{
    while (ring->len != 0) {
        size_t contiguous = ring->len;
        ssize_t sent;

        if (contiguous > BENCH_CAPACITY - ring->head)
            contiguous = BENCH_CAPACITY - ring->head;
        sent = send(fd, ring->data + ring->head, contiguous, MSG_NOSIGNAL);
        (*calls)++;
        if (sent > 0) {
            ring->head = (ring->head + (size_t)sent) % BENCH_CAPACITY;
            ring->len -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static BENCH_NOINLINE int flush_vectored(
    int fd,
    bench_ring *ring,
    uint64_t *calls
)
{
    while (ring->len != 0) {
        struct iovec vectors[2];
        struct msghdr message;
        size_t first = ring->len;
        ssize_t sent;

        if (first > BENCH_CAPACITY - ring->head)
            first = BENCH_CAPACITY - ring->head;
        memset(&message, 0, sizeof(message));
        vectors[0].iov_base = ring->data + ring->head;
        vectors[0].iov_len = first;
        message.msg_iov = vectors;
        message.msg_iovlen = 1;
        if (ring->len > first) {
            vectors[1].iov_base = ring->data;
            vectors[1].iov_len = ring->len - first;
            message.msg_iovlen = 2;
        }
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
        (*calls)++;
        if (sent > 0) {
            ring->head = (ring->head + (size_t)sent) % BENCH_CAPACITY;
            ring->len -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int receive_all(int fd, uint8_t *out, size_t length)
{
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(fd, out + received, length - received, 0);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int send_fragment(
    int fd,
    const uint8_t *data,
    size_t length,
    uint64_t *calls
)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);

        (*calls)++;
        if (result > 0) {
            sent += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static BENCH_NOINLINE int send_fragmented_frame(
    int fd,
    const uint8_t *header,
    const uint8_t *topic,
    size_t topic_length,
    const uint8_t *payload,
    uint64_t *calls
)
{
    return send_fragment(fd, header, BENCH_FRAME_HEADER_BYTES, calls) == 0 &&
        send_fragment(fd, topic, topic_length, calls) == 0 &&
        send_fragment(fd, payload, BENCH_BYTES, calls) == 0 ? 0 : -1;
}

static BENCH_NOINLINE int send_vectored_frame(
    int fd,
    const uint8_t *header,
    const uint8_t *topic,
    size_t topic_length,
    const uint8_t *payload,
    uint64_t *calls
)
{
    struct iovec vectors[3];
    struct msghdr message;
    ssize_t result;

    vectors[0].iov_base = (void *)header;
    vectors[0].iov_len = BENCH_FRAME_HEADER_BYTES;
    vectors[1].iov_base = (void *)topic;
    vectors[1].iov_len = topic_length;
    vectors[2].iov_base = (void *)payload;
    vectors[2].iov_len = BENCH_BYTES;
    memset(&message, 0, sizeof(message));
    message.msg_iov = vectors;
    message.msg_iovlen = sizeof(vectors) / sizeof(vectors[0]);
    do {
        result = sendmsg(fd, &message, MSG_NOSIGNAL);
        (*calls)++;
    } while (result < 0 && errno == EINTR);
    return result == (ssize_t)(BENCH_FRAME_HEADER_BYTES + topic_length + BENCH_BYTES) ?
        0 : -1;
}

static int run_candidate(
    int vectored,
    const uint8_t *payload,
    bench_result *result
)
{
    int sockets[2];
    size_t batch;
    bench_ring ring;
    uint8_t received[BENCH_BYTES];

    memset(result, 0, sizeof(*result));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return -1;
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPS_PER_BATCH; operation++) {
            size_t i;
            ring_reset(&ring, payload);
            if ((vectored ?
                    flush_vectored(sockets[0], &ring, &result->send_calls) :
                    flush_contiguous(sockets[0], &ring, &result->send_calls)) != 0 ||
                receive_all(sockets[1], received, sizeof(received)) != 0 ||
                memcmp(received, payload, sizeof(received)) != 0) {
                close(sockets[0]);
                close(sockets[1]);
                return -1;
            }
            for (i = 0; i < sizeof(received); i += 64)
                result->checksum += received[i];
        }
        result->batches[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPS_PER_BATCH;
    }
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

static int run_frame_candidate(
    int vectored,
    const uint8_t *payload,
    bench_result *result
)
{
    static const uint8_t topic[] = "ai.turn.tts.speak";
    uint8_t header[BENCH_FRAME_HEADER_BYTES];
    uint8_t received[BENCH_FRAME_HEADER_BYTES + sizeof(topic) - 1 + BENCH_BYTES];
    int sockets[2];
    size_t batch;

    memset(result, 0, sizeof(*result));
    memset(header, 0x31, sizeof(header));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return -1;
    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        uint64_t started = monotonic_ns();
        size_t operation;

        for (operation = 0; operation < BENCH_OPS_PER_BATCH; operation++) {
            size_t i;
            int send_result = vectored ?
                send_vectored_frame(
                    sockets[0], header, topic, sizeof(topic) - 1, payload,
                    &result->send_calls) :
                send_fragmented_frame(
                    sockets[0], header, topic, sizeof(topic) - 1, payload,
                    &result->send_calls);
            if (send_result != 0 ||
                receive_all(sockets[1], received, sizeof(received)) != 0 ||
                memcmp(received, header, sizeof(header)) != 0 ||
                memcmp(received + sizeof(header), topic, sizeof(topic) - 1) != 0 ||
                memcmp(
                    received + sizeof(header) + sizeof(topic) - 1,
                    payload,
                    BENCH_BYTES) != 0) {
                close(sockets[0]);
                close(sockets[1]);
                return -1;
            }
            for (i = 0; i < sizeof(received); i += 64)
                result->checksum += received[i];
        }
        result->batches[batch] =
            (double)(monotonic_ns() - started) / BENCH_OPS_PER_BATCH;
    }
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void print_result(const char *name, bench_result *result)
{
    size_t i;
    double total = 0.0;
    uint64_t operations =
        (uint64_t)BENCH_BATCHES * BENCH_OPS_PER_BATCH;

    for (i = 0; i < BENCH_BATCHES; i++)
        total += result->batches[i];
    qsort(
        result->batches,
        BENCH_BATCHES,
        sizeof(result->batches[0]),
        compare_double);
    printf(
        "%s avg_ns=%.2f p50_ns=%.2f p99_ns=%.2f sends_per_op=%.2f "
        "checksum=%llu\n",
        name,
        total / BENCH_BATCHES,
        result->batches[BENCH_BATCHES / 2],
        result->batches[(BENCH_BATCHES * 99) / 100],
        (double)result->send_calls / (double)operations,
        (unsigned long long)result->checksum);
}

int main(void)
{
    uint8_t payload[BENCH_BYTES];
    bench_result contiguous;
    bench_result vectored;
    bench_result fragmented_frame;
    bench_result vectored_frame;
    size_t i;

    for (i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)i;
    if (run_candidate(0, payload, &contiguous) != 0 ||
        run_candidate(1, payload, &vectored) != 0 ||
        run_frame_candidate(0, payload, &fragmented_frame) != 0 ||
        run_frame_candidate(1, payload, &vectored_frame) != 0) {
        fputs("VBus transmit benchmark failed\n", stderr);
        return 1;
    }
    print_result("contiguous", &contiguous);
    print_result("vectored", &vectored);
    print_result("fragmented_frame", &fragmented_frame);
    print_result("vectored_frame", &vectored_frame);
    return contiguous.checksum == vectored.checksum &&
        contiguous.send_calls == vectored.send_calls * 2 &&
        fragmented_frame.checksum == vectored_frame.checksum &&
        fragmented_frame.send_calls == vectored_frame.send_calls * 3 ? 0 : 1;
}
