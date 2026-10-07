#define _POSIX_C_SOURCE 200809L

#include "../bus/vbus.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    BENCH_WARMUP = 256,
    BENCH_SAMPLES = 4096,
    BENCH_FILLER_CLIENTS = 6,
    BENCH_PAYLOAD_BYTES = 256,
    BENCH_TIMEOUT_MS = 5000,
};

typedef struct {
    uint64_t sent_at_ns;
    uint32_t sequence;
    uint8_t padding[BENCH_PAYLOAD_BYTES - sizeof(uint64_t) - sizeof(uint32_t)];
} bench_message;

_Static_assert(
    sizeof(bench_message) == BENCH_PAYLOAD_BYTES,
    "benchmark payload must keep its service-sized wire shape");

static vbus_stop_flag broker_stop;
static uint64_t latency_samples[BENCH_SAMPLES];
static size_t messages_received;
static int callback_failed;
static int acknowledgement_fd = -1;

static uint64_t monotonic_ns(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static void sleep_ms(long milliseconds) {
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int write_byte(int fd) {
    const uint8_t marker = 1;

    for (;;) {
        ssize_t written = write(fd, &marker, sizeof(marker));
        if (written == (ssize_t)sizeof(marker)) return 0;
        if (written < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int read_byte(int fd) {
    uint8_t marker;

    for (;;) {
        ssize_t received = read(fd, &marker, sizeof(marker));
        if (received == (ssize_t)sizeof(marker)) return 0;
        if (received < 0 && errno == EINTR) continue;
        return -1;
    }
}

static void broker_signal(int signal_number) {
    (void)signal_number;
    atomic_store_explicit(&broker_stop, 1, memory_order_relaxed);
}

static void on_latency_message(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    bench_message message;
    uint64_t received_at_ns;
    size_t expected_sequence = messages_received;

    (void)topic;
    (void)reply;
    (void)user;
    if (data_len != sizeof(message) || expected_sequence > UINT32_MAX) {
        callback_failed = 1;
        return;
    }
    memcpy(&message, data, sizeof(message));
    received_at_ns = monotonic_ns();
    if (message.sequence != (uint32_t)expected_sequence ||
        message.sent_at_ns == 0 || received_at_ns < message.sent_at_ns) {
        callback_failed = 1;
        return;
    }
    if (messages_received >= BENCH_WARMUP) {
        size_t sample_index = messages_received - BENCH_WARMUP;
        if (sample_index >= BENCH_SAMPLES) {
            callback_failed = 1;
            return;
        }
        latency_samples[sample_index] = received_at_ns - message.sent_at_ns;
    }
    messages_received++;
    if (write_byte(acknowledgement_fd) != 0) callback_failed = 1;
}

static vbus_client *connect_with_retry(const char *socket_path) {
    int attempt;

    for (attempt = 0; attempt < 200; ++attempt) {
        vbus_client *client = vbus_connect(socket_path);
        if (client) return client;
        sleep_ms(5);
    }
    return NULL;
}

static int run_publisher(
    const char *socket_path,
    int start_fd,
    int acknowledgement_read_fd
) {
    vbus_client *publisher = connect_with_retry(socket_path);
    size_t sequence;

    if (!publisher || read_byte(start_fd) != 0) {
        vbus_close(publisher);
        return 1;
    }
    for (sequence = 0; sequence < BENCH_WARMUP + BENCH_SAMPLES; ++sequence) {
        bench_message message;

        memset(&message, 0xa5, sizeof(message));
        message.sequence = (uint32_t)sequence;
        message.sent_at_ns = monotonic_ns();
        if (message.sent_at_ns == 0 ||
            vbus_publish(
                publisher,
                "bench.vbus.process-latency",
                (const uint8_t *)&message,
                sizeof(message)) != 0 ||
            read_byte(acknowledgement_read_fd) != 0) {
            vbus_close(publisher);
            return 1;
        }
    }
    vbus_close(publisher);
    return 0;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static void stop_child(pid_t child) {
    int status;

    if (child <= 0) return;
    (void)kill(child, SIGTERM);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

int main(void) {
    char socket_path[108];
    int start_pipe[2] = {-1, -1};
    int acknowledgement_pipe[2] = {-1, -1};
    vbus_client *subscriber = NULL;
    vbus_client *fillers[BENCH_FILLER_CLIENTS] = {0};
    pid_t broker_pid = -1;
    pid_t publisher_pid = -1;
    uint64_t deadline_ns;
    size_t expected_messages = BENCH_WARMUP + BENCH_SAMPLES;
    int publisher_status = 0;
    int result = 1;
    int i;
    int path_length = snprintf(
        socket_path,
        sizeof(socket_path),
        "/tmp/c-vbus-process-latency-%ld.sock",
        (long)getpid());

    if (path_length <= 0 || (size_t)path_length >= sizeof(socket_path) ||
        pipe(start_pipe) != 0 || pipe(acknowledgement_pipe) != 0) {
        fprintf(stderr, "process-latency setup failed\n");
        goto done;
    }

    atomic_init(&broker_stop, 0);
    broker_pid = fork();
    if (broker_pid < 0) goto done;
    if (broker_pid == 0) {
        signal(SIGTERM, broker_signal);
        signal(SIGINT, broker_signal);
        _exit(vbus_broker_run(socket_path, &broker_stop) == 0 ? 0 : 1);
    }

    subscriber = connect_with_retry(socket_path);
    if (!subscriber ||
        vbus_subscribe(
            subscriber,
            "bench.vbus.process-latency",
            NULL,
            on_latency_message,
            NULL) != 0) goto done;
    for (i = 0; i < BENCH_FILLER_CLIENTS; ++i) {
        fillers[i] = connect_with_retry(socket_path);
        if (!fillers[i]) goto done;
    }

    publisher_pid = fork();
    if (publisher_pid < 0) goto done;
    if (publisher_pid == 0) {
        int publisher_result;

        close(start_pipe[1]);
        close(acknowledgement_pipe[1]);
        for (i = 0; i < BENCH_FILLER_CLIENTS; ++i) vbus_close(fillers[i]);
        vbus_close(subscriber);
        publisher_result = run_publisher(
            socket_path,
            start_pipe[0],
            acknowledgement_pipe[0]);
        _exit(publisher_result);
    }
    close(start_pipe[0]);
    start_pipe[0] = -1;
    close(acknowledgement_pipe[0]);
    acknowledgement_pipe[0] = -1;
    acknowledgement_fd = acknowledgement_pipe[1];

    sleep_ms(20);
    if (write_byte(start_pipe[1]) != 0) goto done;
    deadline_ns = monotonic_ns() +
        (uint64_t)BENCH_TIMEOUT_MS * UINT64_C(1000000);
    while (!callback_failed && messages_received < expected_messages &&
           monotonic_ns() < deadline_ns) {
        if (vbus_poll(subscriber, 100) != 0) {
            callback_failed = 1;
            break;
        }
    }
    if (callback_failed || messages_received != expected_messages) {
        stop_child(publisher_pid);
        publisher_pid = -1;
        goto done;
    }
    while (waitpid(publisher_pid, &publisher_status, 0) < 0 && errno == EINTR) {
    }
    publisher_pid = -1;
    if (callback_failed || messages_received != expected_messages ||
        !WIFEXITED(publisher_status) || WEXITSTATUS(publisher_status) != 0)
        goto done;

    qsort(
        latency_samples,
        BENCH_SAMPLES,
        sizeof(latency_samples[0]),
        compare_u64);
    printf(
        "BenchmarkVBusProcessLatency\tconnections=%d\tpayload_bytes=%d\t"
        "samples=%d\tp50_ns=%llu\tp95_ns=%llu\tp99_ns=%llu\tmax_ns=%llu\n",
        BENCH_FILLER_CLIENTS + 2,
        BENCH_PAYLOAD_BYTES,
        BENCH_SAMPLES,
        (unsigned long long)latency_samples[BENCH_SAMPLES / 2],
        (unsigned long long)latency_samples[(BENCH_SAMPLES * 95) / 100],
        (unsigned long long)latency_samples[(BENCH_SAMPLES * 99) / 100],
        (unsigned long long)latency_samples[BENCH_SAMPLES - 1]);
    result = 0;

done:
    if (publisher_pid > 0) stop_child(publisher_pid);
    if (start_pipe[0] >= 0) close(start_pipe[0]);
    if (start_pipe[1] >= 0) close(start_pipe[1]);
    if (acknowledgement_pipe[0] >= 0) close(acknowledgement_pipe[0]);
    if (acknowledgement_pipe[1] >= 0) close(acknowledgement_pipe[1]);
    for (i = 0; i < BENCH_FILLER_CLIENTS; ++i) vbus_close(fillers[i]);
    vbus_close(subscriber);
    stop_child(broker_pid);
    if (result != 0) fprintf(stderr, "VBus process latency benchmark failed\n");
    return result;
}
