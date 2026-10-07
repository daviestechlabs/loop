/*
 * Integration test: VBus broker + stream-chunk (audio engine) + turn-start
 * (phatic/classifier) + request/reply RAG path without NATS server.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "vbus.h"
#include "pb_min.h"
#include "pcm_condition.h"
#include "audio_engine.h"
#include "route_classifier.h"
#include "phatic_policy.h"
#include "subjects.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static vbus_stop_flag g_broker_stop;
static char g_sock[256];

static void *broker_thread(void *arg) {
    (void)arg;
    (void)vbus_broker_run(g_sock, &g_broker_stop);
    return NULL;
}

typedef struct {
    char path[108];
    vbus_stop_flag stop;
    int result;
} path_broker_probe;

static void *path_broker_thread(void *user) {
    path_broker_probe *probe = (path_broker_probe *)user;
    probe->result = vbus_broker_run(probe->path, &probe->stop);
    return NULL;
}

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
} raw_vbus_header;
#pragma pack(pop)

_Static_assert(
    sizeof(raw_vbus_header) == VBUS_FRAME_HEADER_BYTES,
    "raw VBus test header must match the production wire header");

static int write_all(int fd, const uint8_t *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t written = write(fd, data + offset, len - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int raw_connect(const char *path) {
    struct sockaddr_un address;
    int fd;

    if (!path || strlen(path) >= sizeof(address.sun_path)) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_for_peer_close(int fd, int timeout_ms) {
    struct pollfd descriptor;
    uint8_t byte;
    int poll_result;
    ssize_t read_result;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    poll_result = poll(&descriptor, 1, timeout_ms);
    read_result = poll_result > 0 ? read(fd, &byte, 1) : -1;
    return poll_result > 0 && read_result == 0 ? 0 : -1;
}

static int raw_publish_without_hello_rejected(const char *path) {
    static const char topic[] = "test.pre-hello";
    raw_vbus_header header;
    uint8_t frame[sizeof(header) + sizeof(topic)];
    int fd = raw_connect(path);
    int result;

    if (fd < 0) return -1;
    memset(&header, 0, sizeof(header));
    header.magic = VBUS_MAGIC;
    header.version = VBUS_VERSION;
    header.op = VBUS_OP_PUB;
    header.topic_len = (uint32_t)(sizeof(topic) - 1u);
    header.body_len = 1;
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), topic, sizeof(topic) - 1u);
    frame[sizeof(frame) - 1u] = (uint8_t)'x';
    if (write_all(fd, frame, sizeof(frame)) != 0) {
        close(fd);
        return -1;
    }
    result = wait_for_peer_close(fd, 1000);
    close(fd);
    return result;
}

static int raw_idle_without_hello_rejected(const char *path) {
    int fd = raw_connect(path);
    int result;

    if (fd < 0) return -1;
    result = wait_for_peer_close(fd, 1600);
    close(fd);
    return result;
}

static int raw_overflowing_lengths_rejected(const char *path) {
    raw_vbus_header header;
    int fd = raw_connect(path);
    int result;

    if (fd < 0) return -1;
    memset(&header, 0, sizeof(header));
    header.magic = VBUS_MAGIC;
    header.version = VBUS_VERSION;
    header.op = VBUS_OP_HELLO;
    header.subscription_id = VBUS_SUBSCRIPTION_NONE;
    header.topic_len = UINT32_MAX;
    header.queue_len = UINT32_MAX;
    header.reply_len = UINT32_MAX;
    header.body_len = UINT32_MAX;
    if (write_all(fd, (const uint8_t *)&header, sizeof(header)) != 0) {
        close(fd);
        return -1;
    }
    result = wait_for_peer_close(fd, 1000);
    close(fd);
    return result;
}

static int broker_client_capacity_recycles(const char *path) {
    enum {
        BROKER_CLIENT_CAPACITY = 64,
        EXISTING_CLIENTS = 1,
        TEST_CLIENTS = BROKER_CLIENT_CAPACITY - EXISTING_CLIENTS
    };
    vbus_client *clients[TEST_CLIENTS];
    vbus_client *overflow = NULL;
    vbus_client *probe = NULL;
    int result = -1;
    int index;
    int attempt;

    memset(clients, 0, sizeof(clients));
    for (index = 0; index < TEST_CLIENTS; ++index) {
        clients[index] = vbus_connect(path);
        if (!clients[index]) goto done;
    }
    overflow = vbus_connect(path);
    if (overflow) goto done;
    for (index = 0; index < TEST_CLIENTS; index += 2) {
        vbus_close(clients[index]);
        clients[index] = NULL;
    }
    for (index = 0; index < TEST_CLIENTS; index += 2) {
        for (attempt = 0; attempt < 50 && !clients[index]; ++attempt) {
            clients[index] = vbus_connect(path);
            if (!clients[index]) usleep(2000);
        }
        if (!clients[index]) goto done;
    }
    result = 0;

done:
    if (overflow) vbus_close(overflow);
    for (index = 0; index < TEST_CLIENTS; ++index) {
        if (clients[index]) vbus_close(clients[index]);
    }
    for (attempt = 0; attempt < 50 && !probe; ++attempt) {
        probe = vbus_connect(path);
        if (!probe) usleep(2000);
    }
    if (!probe) result = -1;
    vbus_close(probe);
    return result;
}

static int broker_preserves_regular_file(void) {
    static const uint8_t marker[] = "keep";
    char path[] = "/tmp/vbus-regular-XXXXXX";
    struct stat status;
    vbus_stop_flag stopped = 1;
    uint8_t observed[sizeof(marker)];
    int fd = mkstemp(path);
    int run_result;
    int preserved = 0;

    if (fd < 0 || write_all(fd, marker, sizeof(marker)) != 0) {
        if (fd >= 0) close(fd);
        if (fd >= 0) unlink(path);
        return -1;
    }
    run_result = vbus_broker_run(path, &stopped);
    if (lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
        lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, observed, sizeof(observed)) == (ssize_t)sizeof(observed) &&
        memcmp(observed, marker, sizeof(marker)) == 0)
        preserved = 1;
    close(fd);
    unlink(path);
    return run_result != 0 && preserved ? 0 : -1;
}

static int broker_replaces_stale_socket(void) {
    char path[108];
    struct sockaddr_un address;
    struct stat status;
    vbus_stop_flag stopped = 1;
    int fd;
    int run_result;

    snprintf(path, sizeof(path), "/tmp/vbus-stale-%d.sock", (int)getpid());
    unlink(path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    run_result = vbus_broker_run(path, &stopped);
    if (lstat(path, &status) == 0) {
        unlink(path);
        return -1;
    }
    return run_result == 0 && errno == ENOENT ? 0 : -1;
}

static int broker_preserves_replacement_path(void) {
    static const uint8_t marker[] = "replacement";
    path_broker_probe probe;
    pthread_t thread;
    vbus_client *client = NULL;
    struct stat status;
    uint8_t observed[sizeof(marker)];
    int fd = -1;
    int attempt;
    int connected = 0;
    int preserved = 0;

    memset(&probe, 0, sizeof(probe));
    snprintf(probe.path, sizeof(probe.path), "/tmp/vbus-owned-%d.sock", (int)getpid());
    unlink(probe.path);
    atomic_store_explicit(&probe.stop, 0, memory_order_relaxed);
    if (pthread_create(&thread, NULL, path_broker_thread, &probe) != 0) return -1;
    for (attempt = 0; attempt < 50; ++attempt) {
        client = vbus_connect(probe.path);
        if (client) break;
        usleep(20000);
    }
    if (client) {
        connected = 1;
        vbus_close(client);
    }
    if (!connected || unlink(probe.path) != 0) goto done;
    fd = open(probe.path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write_all(fd, marker, sizeof(marker)) != 0) goto done;

done:
    atomic_store_explicit(&probe.stop, 1, memory_order_relaxed);
    pthread_join(thread, NULL);
    if (fd >= 0 && probe.result == 0 &&
        lstat(probe.path, &status) == 0 && S_ISREG(status.st_mode) &&
        lseek(fd, 0, SEEK_SET) == 0 &&
        read(fd, observed, sizeof(observed)) == (ssize_t)sizeof(observed) &&
        memcmp(observed, marker, sizeof(marker)) == 0)
        preserved = 1;
    if (fd >= 0) close(fd);
    unlink(probe.path);
    return preserved ? 0 : -1;
}

static volatile int g_got_chunk;
static volatile float g_chunk_energy;
static volatile int g_got_turn;
static char g_route_or_reply[128];
static volatile int g_got_speak;
static char g_speak_rid[128];
static volatile int g_fast_marker;

typedef struct {
    int count;
    int invalid;
} receive_window_probe;

typedef struct {
    int count;
    int invalid;
} queued_frame_probe;

typedef struct {
    int count;
    int invalid;
} publish_pair_probe;

typedef struct {
    int count;
    int invalid;
} prepared_publish_probe;

typedef struct {
    int count;
    int invalid;
} span_publish_probe;

typedef struct {
    const char *expected_reply;
    size_t expected_reply_len;
    int count;
    int invalid;
} reply_span_probe;

static void on_reply_span(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    reply_span_probe *probe = (reply_span_probe *)user;
    if (!probe) return;
    if (!topic || strcmp(topic, "test.reply-boundary") != 0 ||
        !reply || strlen(reply) != probe->expected_reply_len ||
        memcmp(reply, probe->expected_reply, probe->expected_reply_len) != 0 ||
        !data || data_len != 1u || data[0] != (uint8_t)'r')
        probe->invalid = 1;
    probe->count++;
}

static void on_prepared_publish(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    prepared_publish_probe *probe = (prepared_publish_probe *)user;
    uint8_t expected_data;
    if (!probe) return;
    expected_data = probe->count == 0 ? (uint8_t)'1' : (uint8_t)'2';
    if (probe->count >= 2 || !topic || strcmp(topic, "test.prepared") != 0 ||
        reply != NULL || !data || data_len != 1u || data[0] != expected_data)
        probe->invalid = 1;
    probe->count++;
}

static void on_span_publish(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    static const char *const topics[] = {
        "test.spans.single",
        "test.spans.single",
        "test.spans.first",
        "test.spans.second",
        "test.spans.first",
        "test.spans.second",
    };
    static const char *const bodies[] = {
        "abc", "abc", "123", "xy", "123", "xy",
    };
    span_publish_probe *probe = (span_publish_probe *)user;
    size_t index;
    if (!probe) return;
    index = (size_t)probe->count;
    if (index >= sizeof(topics) / sizeof(topics[0]) || !topic || reply ||
        strcmp(topic, topics[index]) != 0 || !data ||
        data_len != strlen(bodies[index]) ||
        memcmp(data, bodies[index], data_len) != 0)
        probe->invalid = 1;
    probe->count++;
}

static void on_publish_pair(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    publish_pair_probe *probe = (publish_pair_probe *)user;
    const char *expected_topic;
    uint8_t expected_data;
    if (!probe) return;
    expected_topic = (probe->count & 1) == 0 ?
        "test.pair.first" : "test.pair.second";
    expected_data = (uint8_t)('1' + probe->count);
    if (probe->count >= 4 || !topic || strcmp(topic, expected_topic) != 0 ||
        reply != NULL || !data || data_len != 1 || data[0] != expected_data) {
        probe->invalid = 1;
    }
    probe->count++;
}

static void on_receive_window(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    receive_window_probe *probe = (receive_window_probe *)user;
    size_t i;
    uint8_t expected;

    if (!probe) return;
    if (!topic || strcmp(topic, "test.rx-window") != 0 || reply != NULL || !data) {
        probe->invalid = 1;
    } else if (probe->count == 0) {
        if (data_len != 65536) {
            probe->invalid = 1;
        } else {
            for (i = 0; i < data_len; ++i) {
                if (data[i] != 0x5c) {
                    probe->invalid = 1;
                    break;
                }
            }
        }
    } else {
        expected = (uint8_t)(probe->count - 1);
        if (data_len != 1104 || data[0] != expected) {
            probe->invalid = 1;
        } else {
            for (i = 1; i < data_len; ++i) {
                if (data[i] != 0xa5) {
                    probe->invalid = 1;
                    break;
                }
            }
        }
    }
    probe->count++;
}

static void on_queued_frame(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    queued_frame_probe *probe = (queued_frame_probe *)user;
    uint32_t sequence = 0u;
    size_t i;

    if (!probe) return;
    if (!topic || strcmp(topic, "test.queued-frame") != 0 || reply != NULL ||
        !data || data_len != 4096u) {
        probe->invalid = 1;
    } else {
        memcpy(&sequence, data, sizeof(sequence));
        if (sequence != (uint32_t)probe->count) probe->invalid = 1;
        for (i = sizeof(sequence); i < data_len; ++i) {
            if (data[i] != (uint8_t)sequence) {
                probe->invalid = 1;
                break;
            }
        }
    }
    probe->count++;
}

static void on_count(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    int *count = (int *)user;
    (void)topic;
    (void)reply;
    (void)data;
    (void)data_len;
    if (count) (*count)++;
}

static void on_fast_marker(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    (void)topic;
    (void)reply;
    (void)data;
    (void)data_len;
    (void)user;
    g_fast_marker = 1;
}

static void on_orchestrate(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    vbus_client *pub = (vbus_client *)user;
    (void)topic;
    (void)reply;
    if (pub) (void)vbus_publish(pub, SUBJ_TURN_GENERATE, data, data_len);
}

static void on_stream(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    stt_stream_message_c msg;
    audio_engine_session *engine = (audio_engine_session *)user;
    audio_decision decision;
    audio_process_options options;
    (void)topic;
    (void)reply;
    if (pb_decode_stt_stream_message(data, data_len, &msg) != 0) return;
    if (strcmp(msg.type, "chunk") != 0 || msg.audio_len == 0) return;
    memset(&options, 0, sizeof(options));
    memset(&decision, 0, sizeof(decision));
    audio_engine_process(engine, msg.audio, msg.audio_len, 1, options, &decision);
    g_chunk_energy = decision.energy;
    g_got_chunk = 1;
}

static void on_turn(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_start_c turn;
    uint32_t phatic;
    vbus_client *pub = (vbus_client *)user;
    (void)topic;
    (void)reply;
    if (pb_decode_turn_start(data, data_len, &turn) != 0) return;
    phatic = phatic_policy_match_v1(turn.text, strlen(turn.text));
    if ((phatic & 0xff) != 0) {
        const char *reply_text = phatic_policy_reply_v1(phatic & 0xff);
        uint8_t speak_buf[512];
        size_t sn;
        snprintf(g_route_or_reply, sizeof(g_route_or_reply), "phatic");
        /* Product glue: phatic → ai.turn.tts.speak (cascade mirror). */
        if (pub && reply_text) {
            turn_tts_segment_c speak;
            memset(&speak, 0, sizeof(speak));
            snprintf(speak.request_id, sizeof(speak.request_id), "%s", turn.request_id);
            snprintf(speak.text, sizeof(speak.text), "%s", reply_text);
            speak.is_final = 1;
            sn = pb_encode_turn_tts_segment(speak_buf, sizeof(speak_buf), &speak);
            if (sn) (void)vbus_publish(pub, SUBJ_TURN_TTS_SPEAK, speak_buf, sn);
        }
    } else {
        uint64_t packed = route_classifier_packed_v1(turn.text, strlen(turn.text));
        snprintf(g_route_or_reply, sizeof(g_route_or_reply), "route-%llu", (unsigned long long)(packed & 3));
    }
    g_got_turn = 1;
}

static void on_speak(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    turn_tts_segment_c req;
    (void)topic;
    (void)reply;
    (void)user;
    if (pb_decode_turn_tts_segment(data, data_len, &req) != 0) return;
    snprintf(g_speak_rid, sizeof(g_speak_rid), "%s", req.request_id);
    g_got_speak = 1;
}

/* Mock rag-gateway: canonical protobuf request/reply. */
static vbus_stop_flag g_rag_stop;
static vbus_client *g_rag_client;

static void on_rag_search(
    const char *topic,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    vbus_client *c = (vbus_client *)user;
    rag_search_request_c request;
    rag_search_response_c response;
    uint8_t wire[512];
    size_t wire_len;
    (void)topic;
    if (pb_decode_rag_search_request(data, data_len, &request) != 0) return;
    memset(&response, 0, sizeof(response));
    snprintf(response.request_id, sizeof(response.request_id), "%s", request.request_id);
    snprintf(
        response.context_text, sizeof(response.context_text),
        "{\"hits\":[{\"text\":\"Elminster appears in books\"}]}");
    response.used_rag = 1;
    wire_len = pb_encode_rag_search_response(wire, sizeof(wire), &response);
    if (reply && reply[0] && wire_len)
        (void)vbus_publish(c, reply, wire, wire_len);
}

static void *rag_thread(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_rag_stop, memory_order_relaxed) == 0) {
        if (g_rag_client) (void)vbus_poll(g_rag_client, 20);
    }
    return NULL;
}

static size_t pb_put_varint(uint8_t *out, size_t cap, size_t pos, uint64_t v) {
    while (v >= 0x80) {
        if (pos >= cap) return (size_t)-1;
        out[pos++] = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    if (pos >= cap) return (size_t)-1;
    out[pos++] = (uint8_t)v;
    return pos;
}

static size_t encode_chunk(uint8_t *out, size_t cap, const uint8_t *audio, size_t alen) {
    size_t pos = 0;
    if (cap < 32 + alen) return 0;
    out[pos++] = (1 << 3) | 2;
    pos = pb_put_varint(out, cap, pos, 5);
    memcpy(out + pos, "chunk", 5);
    pos += 5;
    out[pos++] = (2 << 3) | 2;
    pos = pb_put_varint(out, cap, pos, alen);
    if (pos == (size_t)-1 || pos + alen > cap) return 0;
    memcpy(out + pos, audio, alen);
    pos += alen;
    return pos;
}

static size_t encode_turn(uint8_t *out, size_t cap, const char *rid, const char *text) {
    size_t pos = 0;
    size_t rl = strlen(rid);
    size_t tl = strlen(text);
    if (cap < 32 + rl + tl) return 0;
    out[pos++] = (1 << 3) | 2;
    out[pos++] = (uint8_t)rl;
    memcpy(out + pos, rid, rl);
    pos += rl;
    out[pos++] = (5 << 3) | 2;
    out[pos++] = (uint8_t)tl;
    memcpy(out + pos, text, tl);
    pos += tl;
    return pos;
}

static int failures;

static void expect(const char *name, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    } else {
        printf("PASS %s\n", name);
    }
}

#include "bus_flow_test.h"

int main(void) {
    pthread_t thr;
    pthread_t thr_rag;
    vbus_client *pub;
    vbus_client *sub_stream;
    vbus_client *sub_orch;
    vbus_client *sub_turn;
    vbus_client *sub_speak;
    vbus_client *rag;
    vbus_client *sub_all;
    vbus_client *overlap_sub;
    vbus_client *queue_identity_sub;
    vbus_client *slow_sub;
    vbus_client *fast_sub;
    vbus_client *burst_sub;
    vbus_client *queued_sub;
    vbus_client *queue_a1;
    vbus_client *queue_a2;
    vbus_client *queue_b;
    vbus_client *poll_sub;
    audio_engine_session *engine;
    uint8_t pcm[640];
    uint8_t frame[800];
    size_t n;
    int i;
    int waits;
    int slow_disconnected;

    failures = 0;
    expect("flow reader recovers with exact fanout and independent traffic", flow_control_case(1) == 0);
    expect("flow reader timeout releases its publisher without duplicate fanout", flow_control_case(0) == 0);
    expect(
        "broker preserves non-socket path",
        broker_preserves_regular_file() == 0);
    expect("broker replaces stale socket", broker_replaces_stale_socket() == 0);
    expect(
        "broker preserves replacement path on shutdown",
        broker_preserves_replacement_path() == 0);
    snprintf(g_sock, sizeof(g_sock), "/tmp/vbus-test-%d.sock", (int)getpid());
    setenv("VBUS_PATH", g_sock, 1);
    setenv("VBUS_BROKER_MAX_BUFFER_BYTES", "1048576", 1);
    atomic_store_explicit(&g_broker_stop, 0, memory_order_relaxed);
    if (pthread_create(&thr, NULL, broker_thread, NULL) != 0) {
        fprintf(stderr, "broker thread failed\n");
        return 1;
    }
    for (i = 0; i < 50; i++) {
        pub = vbus_connect(g_sock);
        if (pub) break;
        usleep(20000);
    }
    expect("broker connect", pub != NULL);
    if (!pub) {
        atomic_store_explicit(&g_broker_stop, 1, memory_order_relaxed);
        pthread_join(thr, NULL);
        return 1;
    }
    expect("poll fd rejects null", vbus_poll_fd(NULL) == -1);
    expect("poll fd exposes connected socket", vbus_poll_fd(pub) >= 0);
    expect("single-frame poll rejects null", vbus_poll_one(NULL, 0) == -1);
    expect("bounded connection rejects tiny receive limit", vbus_connect_bounded(g_sock, 8191u) == NULL);
    expect("bounded connection rejects huge receive limit", vbus_connect_bounded(g_sock, SIZE_MAX) == NULL);
    expect("bounded reader rejects complete oversized frames and wildcard flow", bounded_reader_case(g_sock, pub) == 0);
    {
        static const char *const cases[] = {
            "broker rejects star flow subscription", "broker rejects tail flow subscription",
            "broker rejects flow queue group", "broker rejects flow reply subject",
            "broker rejects flow body", "broker rejects flow after ordinary subscription",
            "broker rejects publish from flow reader", "broker rejects second subscription from flow reader"
        };
        for (size_t variant = 0u; variant < sizeof(cases) / sizeof(cases[0]); ++variant)
            expect(cases[variant], raw_flow_rejected(g_sock, (int)variant) == 0);
    }
    expect(
        "broker fills and recycles exact client capacity",
        broker_client_capacity_recycles(g_sock) == 0);
    {
        vbus_stop_flag stopped = 1;
        vbus_client *after_collision;
        expect(
            "second broker rejects active socket",
            vbus_broker_run(g_sock, &stopped) != 0);
        after_collision = vbus_connect(g_sock);
        expect("active broker socket remains owned", after_collision != NULL);
        vbus_close(after_collision);
    }
    expect(
        "broker rejects publish before hello",
        raw_publish_without_hello_rejected(g_sock) == 0);
    expect(
        "broker expires idle pre-hello client",
        raw_idle_without_hello_rejected(g_sock) == 0);
    expect(
        "broker rejects overflowing frame lengths",
        raw_overflowing_lengths_rejected(g_sock) == 0);

    engine = audio_engine_create(0.01f, 0.985, 1, 1, 0.0025, 0.2, 1.75, 0.12);
    expect("engine", engine != NULL);
    sub_stream = vbus_connect(g_sock);
    sub_orch = vbus_connect(g_sock);
    sub_turn = vbus_connect(g_sock);
    sub_speak = vbus_connect(g_sock);
    expect("sub connects", sub_stream && sub_orch && sub_turn && sub_speak);
    expect("sub stream", vbus_subscribe(sub_stream, SUBJ_VOICE_STREAM_ALL, "audio-processors", on_stream, engine) == 0);
    expect(
        "sub orchestrator",
        vbus_subscribe(
            sub_orch, SUBJ_TURN_START, "conversation-orchestrators", on_orchestrate, pub) == 0);
    /* on_turn user=pub so phatic path can publish ai.turn.tts.speak glue */
    expect("sub turn", vbus_subscribe(sub_turn, SUBJ_TURN_GENERATE, "cascade-routers", on_turn, pub) == 0);
    expect("sub speak", vbus_subscribe(sub_speak, SUBJ_TURN_TTS_SPEAK, "tts-modules", on_speak, NULL) == 0);

    /* A returned subscription is broker-active. The immediate publish also
     * proves an outer service loop wakes without a timer tick. */
    poll_sub = vbus_connect(g_sock);
    expect("poll fd subscriber connects", poll_sub != NULL);
    if (poll_sub) {
        struct pollfd descriptor;
        int poll_count = 0;
        int poll_result;

        expect(
            "poll fd subscriber subscribes",
            vbus_subscribe(
                poll_sub,
                "test.poll-fd",
                NULL,
                on_count,
                &poll_count) == 0
        );
        descriptor.fd = vbus_poll_fd(poll_sub);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        expect(
            "poll fd marker publishes",
            vbus_publish(
                pub,
                "test.poll-fd",
                (const uint8_t *)"x",
                1) == 0
        );
        poll_result = poll(&descriptor, 1, 100);
        expect(
            "poll fd wakes outer loop",
            poll_result == 1 && (descriptor.revents & POLLIN) != 0
        );
        expect("poll fd drains through VBus", vbus_poll(poll_sub, 0) == 0);
        for (waits = 0; waits < 8 && poll_count == 0; ++waits) {
            descriptor.revents = 0;
            poll_result = poll(&descriptor, 1, 100);
            if (poll_result != 1 || (descriptor.revents & POLLIN) == 0 ||
                vbus_poll(poll_sub, 0) != 0) break;
        }
        expect("poll fd delivers marker", poll_count == 1);
        {
            int close_count = 0;
            vbus_client *close_pub;
            expect(
                "immediate-close subscriber activates",
                vbus_subscribe(
                    poll_sub,
                    "test.immediate-close",
                    NULL,
                    on_count,
                    &close_count) == 0
            );
            close_pub = vbus_connect(g_sock);
            expect("immediate-close publisher connects", close_pub != NULL);
            expect(
                "immediate-close publisher sends final frame",
                close_pub && vbus_publish(
                    close_pub,
                    "test.immediate-close",
                    (const uint8_t *)"x",
                    1) == 0
            );
            vbus_close(close_pub);
            for (waits = 0; waits < 8 && close_count == 0; ++waits) {
                if (vbus_poll(poll_sub, 100) != 0) break;
            }
            expect("immediate-close final frame is delivered", close_count == 1);
        }
        {
            static const char prepared_topic[] = "test.prepared.trailing";
            prepared_publish_probe prepared_probe = {0};
            expect(
                "prepared publish subscribes",
                vbus_subscribe(
                    poll_sub,
                    "test.prepared",
                    NULL,
                    on_prepared_publish,
                    &prepared_probe) == 0
            );
            expect(
                "prepared publish rejects null client",
                vbus_publish_prepared(
                    NULL,
                    prepared_topic,
                    sizeof("test.prepared") - 1u,
                    (const uint8_t *)"x",
                    1u) != 0
            );
            expect(
                "prepared publish rejects null topic",
                vbus_publish_prepared(
                    pub,
                    NULL,
                    sizeof("test.prepared") - 1u,
                    (const uint8_t *)"x",
                    1u) != 0
            );
            expect(
                "prepared publish rejects empty span",
                vbus_publish_prepared(
                    pub,
                    prepared_topic,
                    0u,
                    (const uint8_t *)"x",
                    1u) != 0
            );
            expect(
                "prepared publish rejects null body",
                vbus_publish_prepared(
                    pub,
                    prepared_topic,
                    sizeof("test.prepared") - 1u,
                    NULL,
                    1u) != 0
            );
            expect(
                "prepared publish rejects overflowing frame lengths",
                vbus_publish_prepared(
                    pub,
                    prepared_topic,
                    SIZE_MAX,
                    (const uint8_t *)"x",
                    1u) != 0 &&
                vbus_publish_prepared(
                    pub,
                    prepared_topic,
                    sizeof("test.prepared") - 1u,
                    (const uint8_t *)"x",
                    SIZE_MAX) != 0
            );
            expect(
                "string publish sends reference frame",
                vbus_publish(
                    pub,
                    "test.prepared",
                    (const uint8_t *)"1",
                    1u) == 0
            );
            expect(
                "prepared publish honors exact span",
                vbus_publish_prepared(
                    pub,
                    prepared_topic,
                    sizeof("test.prepared") - 1u,
                    (const uint8_t *)"2",
                    1u) == 0
            );
            for (waits = 0; waits < 8 && prepared_probe.count < 2; ++waits) {
                if (vbus_poll(poll_sub, 100) != 0) break;
            }
            expect(
                "prepared publish matches string wire contract",
                prepared_probe.count == 2 && prepared_probe.invalid == 0
            );
        }
        {
            static const uint8_t a[] = "a";
            static const uint8_t b[] = "b";
            static const uint8_t c[] = "c";
            static const uint8_t one[] = "1";
            static const uint8_t two[] = "2";
            static const uint8_t three[] = "3";
            static const uint8_t x[] = "x";
            static const uint8_t y[] = "y";
            const vbus_publish_span single_spans[] = {
                {a, sizeof(a) - 1u},
                {b, sizeof(b) - 1u},
                {c, sizeof(c) - 1u},
            };
            const vbus_publish_span first_spans[] = {
                {one, sizeof(one) - 1u},
                {two, sizeof(two) - 1u},
                {three, sizeof(three) - 1u},
            };
            const vbus_publish_span invalid_spans[] = {
                {a, sizeof(a) - 1u},
                {NULL, 1u},
                {c, sizeof(c) - 1u},
            };
            const vbus_publish_span empty_spans[] = {
                {a, sizeof(a) - 1u},
                {b, 0u},
                {c, sizeof(c) - 1u},
            };
            const vbus_publish_span overflow_spans[] = {
                {x, SIZE_MAX},
                {y, 1u},
                {a, 1u},
            };
            const vbus_publish_span oversize_spans[] = {
                {x, (size_t)UINT32_C(1) << 21u},
                {y, (size_t)UINT32_C(1) << 21u},
                {a, (size_t)UINT32_C(1) << 21u},
            };
            span_publish_probe span_probe = {0};
            expect(
                "span publish subscribes single",
                vbus_subscribe(
                    poll_sub, "test.spans.single", NULL,
                    on_span_publish, &span_probe) == 0);
            expect(
                "span publish subscribes first",
                vbus_subscribe(
                    poll_sub, "test.spans.first", NULL,
                    on_span_publish, &span_probe) == 0);
            expect(
                "span publish subscribes second",
                vbus_subscribe(
                    poll_sub, "test.spans.second", NULL,
                    on_span_publish, &span_probe) == 0);
            expect(
                "span publish rejects invalid vector contracts",
                vbus_publish_spans3_prepared(
                    NULL, "test.spans.single", sizeof("test.spans.single") - 1u,
                    single_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, NULL, sizeof("test.spans.single") - 1u,
                    single_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single", 0u, single_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u, NULL) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    invalid_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    empty_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    overflow_spans) != 0 &&
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    oversize_spans) != 0);
            expect(
                "span publish sends one concatenated body",
                vbus_publish_spans3_prepared(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    single_spans) == 0);
            expect(
                "admitted span publish preserves checked body",
                vbus_publish_spans3_admitted(
                    pub, "test.spans.single",
                    sizeof("test.spans.single") - 1u,
                    single_spans) == 0);
            expect(
                "span pair validates both frames before sending",
                vbus_publish_spans3_pair_prepared(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    first_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    NULL, 1u) != 0 &&
                vbus_publish_spans3_pair_prepared(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    oversize_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    (const uint8_t *)"x", 1u) != 0 &&
                vbus_publish_spans3_pair_prepared(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    first_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    (const uint8_t *)"x", (size_t)UINT32_C(1) << 22u) != 0 &&
                vbus_publish_spans3_pair_prepared(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    first_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    (const uint8_t *)"x", 0u) != 0);
            expect(
                "span pair sends concatenated bodies in order",
                vbus_publish_spans3_pair_prepared(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    first_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    (const uint8_t *)"xy", 2u) == 0);
            expect(
                "admitted span pair preserves checked order",
                vbus_publish_spans3_pair_admitted(
                    pub,
                    "test.spans.first", sizeof("test.spans.first") - 1u,
                    first_spans,
                    "test.spans.second", sizeof("test.spans.second") - 1u,
                    (const uint8_t *)"xy", 2u) == 0);
            for (waits = 0; waits < 12 && span_probe.count < 6; ++waits) {
                if (vbus_poll(poll_sub, 100) != 0) break;
            }
            expect(
                "span publications preserve bytes and order",
                span_probe.count == 6 && span_probe.invalid == 0);
        }
        {
            static const char first_topic[] = "test.pair.first.trailing";
            static const char second_topic[] = "test.pair.second.trailing";
            publish_pair_probe pair_probe = {0};
            expect(
                "publish pair subscribes first",
                vbus_subscribe(
                    poll_sub,
                    "test.pair.first",
                    NULL,
                    on_publish_pair,
                    &pair_probe) == 0
            );
            expect(
                "publish pair subscribes second",
                vbus_subscribe(
                    poll_sub,
                    "test.pair.second",
                    NULL,
                    on_publish_pair,
                    &pair_probe) == 0
            );
            expect(
                "publish pair rejects invalid second frame",
                vbus_publish_pair(
                    pub,
                    "test.pair.first",
                    (const uint8_t *)"1",
                    1,
                    "",
                    (const uint8_t *)"2",
                    1) != 0
            );
            expect(
                "publish pair sends both frames",
                vbus_publish_pair(
                    pub,
                    "test.pair.first",
                    (const uint8_t *)"1",
                    1,
                    "test.pair.second",
                    (const uint8_t *)"2",
                    1) == 0
            );
            expect(
                "prepared publish pair rejects empty second span",
                vbus_publish_pair_prepared(
                    pub,
                    first_topic,
                    sizeof("test.pair.first") - 1u,
                    (const uint8_t *)"3",
                    1u,
                    second_topic,
                    0u,
                    (const uint8_t *)"4",
                    1u) != 0
            );
            expect(
                "prepared publish pair honors exact spans",
                vbus_publish_pair_prepared(
                    pub,
                    first_topic,
                    sizeof("test.pair.first") - 1u,
                    (const uint8_t *)"3",
                    1u,
                    second_topic,
                    sizeof("test.pair.second") - 1u,
                    (const uint8_t *)"4",
                    1u) == 0
            );
            for (waits = 0; waits < 8 && pair_probe.count < 4; ++waits) {
                if (vbus_poll(poll_sub, 100) != 0) break;
            }
            expect(
                "publish pair preserves order",
                pair_probe.count == 4 && pair_probe.invalid == 0
            );
        }
        vbus_close(poll_sub);
    }

    /* Stream chunk path */
    for (i = 0; i < 320; i++) {
        int16_t s = (int16_t)(sin((double)i / 10.0) * 8000.0);
        pcm[i * 2] = (uint8_t)(s & 0xff);
        pcm[i * 2 + 1] = (uint8_t)((s >> 8) & 0xff);
    }
    n = encode_chunk(frame, sizeof(frame), pcm, 640);
    expect("pub chunk", vbus_publish(pub, "ai.voice.stream.sess1", frame, n) == 0);
    g_got_chunk = 0;
    for (waits = 0; waits < 100 && !g_got_chunk; waits++) {
        (void)vbus_poll(sub_stream, 20);
        (void)vbus_poll(sub_turn, 0);
    }
    expect("stream chunk delivered", g_got_chunk == 1);
    expect("stream energy finite", isfinite(g_chunk_energy));

    /* Gateway-shaped framing: start + chunk + end via pb_encode_stt_stream_message */
    {
        uint8_t wire[900];
        size_t wn;
        g_got_chunk = 0;
        wn = pb_encode_stt_stream_message(wire, sizeof(wire), "start", NULL, 0, 16000, 1, 16);
        expect("gw start enc", wn > 0);
        expect("gw pub start", vbus_publish(pub, "ai.voice.stream.gw1", wire, wn) == 0);
        wn = pb_encode_stt_stream_message(wire, sizeof(wire), "chunk", pcm, 640, 16000, 1, 16);
        expect("gw chunk enc", wn > 0);
        expect("gw pub chunk", vbus_publish(pub, "ai.voice.stream.gw1", wire, wn) == 0);
        for (waits = 0; waits < 100 && !g_got_chunk; waits++) {
            (void)vbus_poll(sub_stream, 20);
        }
        expect("gw stream chunk delivered", g_got_chunk == 1);
        wn = pb_encode_stt_stream_message(wire, sizeof(wire), "end", NULL, 0, 16000, 1, 16);
        expect("gw end enc", wn > 0);
        expect("gw pub end", vbus_publish(pub, "ai.voice.stream.gw1", wire, wn) == 0);
        for (waits = 0; waits < 20; waits++) (void)vbus_poll(sub_stream, 5);
    }

    /* Turn start + phatic → tts.speak glue (stream→route→speak proof) */
    n = encode_turn(frame, sizeof(frame), "req-1", "hello");
    g_got_turn = 0;
    g_got_speak = 0;
    g_speak_rid[0] = '\0';
    expect("pub turn", vbus_publish(pub, SUBJ_TURN_START, frame, n) == 0);
    for (waits = 0; waits < 100 && !g_got_turn; waits++) {
        (void)vbus_poll(sub_orch, 20);
        (void)vbus_poll(sub_turn, 20);
        (void)vbus_poll(sub_stream, 0);
        (void)vbus_poll(sub_speak, 0);
    }
    expect("turn delivered", g_got_turn == 1);
    expect("phatic path", strcmp(g_route_or_reply, "phatic") == 0);
    for (waits = 0; waits < 100 && !g_got_speak; waits++) {
        (void)vbus_poll(sub_speak, 20);
        (void)vbus_poll(sub_turn, 0);
    }
    expect("speak delivered", g_got_speak == 1);
    expect("speak rid", strcmp(g_speak_rid, "req-1") == 0);

    /* Turn start + classify */
    n = encode_turn(frame, sizeof(frame), "req-2", "Which NPC witnessed the pact beneath the old bridge?");
    g_got_turn = 0;
    g_route_or_reply[0] = '\0';
    expect("pub turn2", vbus_publish(pub, SUBJ_TURN_START, frame, n) == 0);
    for (waits = 0; waits < 100 && !g_got_turn; waits++) {
        (void)vbus_poll(sub_orch, 20);
        (void)vbus_poll(sub_turn, 20);
    }
    expect("turn2 delivered", g_got_turn == 1);
    expect("route path", strncmp(g_route_or_reply, "route-", 6) == 0);

    /* Request/reply RAG path (retrieve_then_escalate glue) */
    rag = vbus_connect(g_sock);
    expect("rag connect", rag != NULL);
    g_rag_client = rag;
    atomic_store_explicit(&g_rag_stop, 0, memory_order_relaxed);
    if (rag) {
        expect("rag sub", vbus_subscribe(rag, SUBJ_RAG_SEARCH, "rag-gateways", on_rag_search, rag) == 0);
        expect("rag thr", pthread_create(&thr_rag, NULL, rag_thread, NULL) == 0);
        {
            uint8_t out[256];
            size_t out_len = 0;
            uint8_t request_wire[512];
            rag_search_request_c request;
            rag_search_response_c response;
            size_t request_len;
            memset(&request, 0, sizeof(request));
            snprintf(request.request_id, sizeof(request.request_id), "rag-1");
            snprintf(request.query, sizeof(request.query), "who is elminster?");
            snprintf(request.collection, sizeof(request.collection), "books");
            request.top_k = 4;
            request_len = pb_encode_rag_search_request(
                request_wire, sizeof(request_wire), &request);
            int rc = vbus_request(
                pub,
                SUBJ_RAG_SEARCH,
                request_wire,
                request_len,
                out,
                sizeof(out),
                &out_len,
                2000
            );
            expect("rag request", rc == 0);
            expect(
                "rag response decode",
                pb_decode_rag_search_response(out, out_len, &response) == 0);
            expect("rag hits", response.used_rag && strstr(response.context_text, "hits") != NULL);
            expect("rag books", strstr(response.context_text, "books") != NULL);
        }
        atomic_store_explicit(&g_rag_stop, 1, memory_order_relaxed);
        pthread_join(thr_rag, NULL);
        vbus_close(rag);
        g_rag_client = NULL;
    }

    /* One read can contain several frames. Exercise receive-window rollover
     * in both the broker and the client without losing frame boundaries. */
    burst_sub = vbus_connect(g_sock);
    expect("receive-window client", burst_sub != NULL);
    if (burst_sub) {
        enum { BURST_MESSAGES = 257 };
        static uint8_t large_frame[65536];
        uint8_t burst[1104];
        receive_window_probe probe;
        int publish_ok = 1;

        memset(&probe, 0, sizeof(probe));
        memset(large_frame, 0x5c, sizeof(large_frame));
        memset(burst, 0xa5, sizeof(burst));
        expect(
            "receive-window subscribe",
            vbus_subscribe(burst_sub, "test.rx-window", NULL, on_receive_window, &probe) == 0
        );
        expect(
            "receive-window large frame publish",
            vbus_publish(
                pub,
                "test.rx-window",
                large_frame,
                sizeof(large_frame)) == 0
        );
        for (waits = 0; waits < 100 && probe.count < 1; ++waits)
            (void)vbus_poll(burst_sub, 20);
        expect("receive-window large frame preserved", probe.count == 1 && !probe.invalid);
        for (i = 0; i < BURST_MESSAGES; ++i) {
            burst[0] = (uint8_t)i;
            if (vbus_publish(pub, "test.rx-window", burst, sizeof(burst)) != 0) {
                publish_ok = 0;
                break;
            }
        }
        expect("receive-window burst accepted", publish_ok);
        for (waits = 0; waits < 500 && probe.count < BURST_MESSAGES + 1; ++waits)
            (void)vbus_poll(burst_sub, 20);
        expect(
            "receive-window frames preserved",
            probe.count == BURST_MESSAGES + 1 && !probe.invalid
        );
        vbus_close(burst_sub);
    }

    /* A small receive socket forces complete frames into the broker's
     * circular transmit queue. Drain that queue and verify every span after
     * its tail wraps. */
    queued_sub = vbus_connect(g_sock);
    expect("queued-frame client", queued_sub != NULL);
    if (queued_sub) {
        enum { QUEUED_MESSAGES = 96, QUEUED_BYTES = 4096 };
        uint8_t queued_body[QUEUED_BYTES];
        queued_frame_probe probe = {0};
        int receive_bytes = 4096;
        int publish_ok = 1;

        expect(
            "queued-frame subscribe",
            vbus_subscribe(
                queued_sub,
                "test.queued-frame",
                NULL,
                on_queued_frame,
                &probe) == 0
        );
        expect(
            "queued-frame bounds receive socket",
            setsockopt(
                vbus_poll_fd(queued_sub),
                SOL_SOCKET,
                SO_RCVBUF,
                &receive_bytes,
                sizeof(receive_bytes)) == 0
        );
        for (i = 0; i < QUEUED_MESSAGES; ++i) {
            uint32_t sequence = (uint32_t)i;
            memcpy(queued_body, &sequence, sizeof(sequence));
            memset(
                queued_body + sizeof(sequence),
                (unsigned char)sequence,
                sizeof(queued_body) - sizeof(sequence));
            if (vbus_publish(
                    pub,
                    "test.queued-frame",
                    queued_body,
                    sizeof(queued_body)) != 0) {
                publish_ok = 0;
                break;
            }
        }
        expect("queued-frame first burst accepted", publish_ok);
        for (waits = 0; waits < 500 && probe.count < 32; ++waits)
            (void)vbus_poll(queued_sub, 20);
        expect("queued-frame partial drain", probe.count >= 32 && !probe.invalid);
        for (i = QUEUED_MESSAGES; i < QUEUED_MESSAGES * 2; ++i) {
            uint32_t sequence = (uint32_t)i;
            memcpy(queued_body, &sequence, sizeof(sequence));
            memset(
                queued_body + sizeof(sequence),
                (unsigned char)sequence,
                sizeof(queued_body) - sizeof(sequence));
            if (vbus_publish(
                    pub,
                    "test.queued-frame",
                    queued_body,
                    sizeof(queued_body)) != 0) {
                publish_ok = 0;
                break;
            }
        }
        expect("queued-frame second burst accepted", publish_ok);
        for (waits = 0;
             waits < 1000 && probe.count < QUEUED_MESSAGES * 2;
             ++waits)
            (void)vbus_poll(queued_sub, 20);
        expect(
            "queued-frame wrap preserves spans",
            probe.count == QUEUED_MESSAGES * 2 && !probe.invalid
        );
        vbus_close(queued_sub);
    }

    /* A subscriber that never reads must not head-of-line block unrelated
     * delivery. The broker bounds its per-client queue and disconnects it. */
    slow_sub = vbus_connect(g_sock);
    fast_sub = vbus_connect(g_sock);
    expect("backpressure clients", slow_sub != NULL && fast_sub != NULL);
    if (slow_sub && fast_sub) {
        static uint8_t bulk[65536];
        int publish_ok = 1;
        memset(bulk, 0x5a, sizeof(bulk));
        expect(
            "slow sub",
            vbus_subscribe(slow_sub, "test.backpressure", NULL, on_fast_marker, NULL) == 0
        );
        expect(
            "fast sub",
            vbus_subscribe(fast_sub, "test.fast", NULL, on_fast_marker, NULL) == 0
        );
        for (i = 0; i < 32; ++i) {
            if (vbus_publish(pub, "test.backpressure", bulk, sizeof(bulk)) != 0) {
                publish_ok = 0;
                break;
            }
        }
        expect("slow burst accepted", publish_ok);
        g_fast_marker = 0;
        expect("fast marker publish", vbus_publish(pub, "test.fast", (const uint8_t *)"x", 1) == 0);
        for (waits = 0; waits < 100 && !g_fast_marker; ++waits)
            (void)vbus_poll(fast_sub, 20);
        expect("slow subscriber isolated", g_fast_marker == 1);
        slow_disconnected = 0;
        for (waits = 0; waits < 100; ++waits) {
            if (vbus_poll(slow_sub, 10) != 0) {
                slow_disconnected = 1;
                break;
            }
        }
        expect("broker aggregate buffer cap disconnects slow subscriber", slow_disconnected);
    }
    if (slow_sub) vbus_close(slow_sub);
    if (fast_sub) vbus_close(fast_sub);

    /* Bare terminal wildcard covers every subject and exercises unindexing. */
    sub_all = vbus_connect(g_sock);
    expect("bare wildcard connect", sub_all != NULL);
    if (sub_all) {
        expect(
            "bare wildcard subscribe",
            vbus_subscribe(sub_all, ">", NULL, on_fast_marker, NULL) == 0
        );
        g_fast_marker = 0;
        expect(
            "bare wildcard publish",
            vbus_publish(pub, "test.bare-wildcard", (const uint8_t *)"x", 1) == 0
        );
        for (waits = 0; waits < 100 && !g_fast_marker; ++waits)
            (void)vbus_poll(sub_all, 20);
        expect("bare wildcard delivered", g_fast_marker == 1);
        vbus_close(sub_all);
    }

    {
        static const char embedded_topic[] = {
            't', 'e', 's', 't', '.', '\0', 'e', 'v', 'i', 'l'
        };
        char maximum_topic[256];
        char maximum_queue[128];
        char maximum_wildcard[256];
        char maximum_wildcard_topic[256];
        char maximum_reply[256];
        char oversized_topic[257];
        char oversized_queue[129];
        char oversized_reply[257];
        int boundary_count = 0;
        int maximum_wildcard_count = 0;
        int embedded_topic_disconnected = 0;
        int oversized_reply_disconnected = 0;
        reply_span_probe reply_probe;
        vbus_client *boundary_sub = vbus_connect(g_sock);
        vbus_client *embedded_topic_pub = NULL;
        vbus_client *oversized_reply_pub = NULL;
        memset(maximum_topic, 't', sizeof(maximum_topic) - 1u);
        maximum_topic[sizeof(maximum_topic) - 1u] = '\0';
        memset(maximum_queue, 'q', sizeof(maximum_queue) - 1u);
        maximum_queue[sizeof(maximum_queue) - 1u] = '\0';
        memset(maximum_wildcard, 'w', sizeof(maximum_wildcard) - 3u);
        maximum_wildcard[sizeof(maximum_wildcard) - 3u] = '.';
        maximum_wildcard[sizeof(maximum_wildcard) - 2u] = '>';
        maximum_wildcard[sizeof(maximum_wildcard) - 1u] = '\0';
        memcpy(
            maximum_wildcard_topic,
            maximum_wildcard,
            sizeof(maximum_wildcard_topic));
        maximum_wildcard_topic[sizeof(maximum_wildcard_topic) - 2u] = 'x';
        memset(maximum_reply, 'r', sizeof(maximum_reply) - 1u);
        maximum_reply[sizeof(maximum_reply) - 1u] = '\0';
        memset(oversized_topic, 't', sizeof(oversized_topic) - 1u);
        oversized_topic[sizeof(oversized_topic) - 1u] = '\0';
        memset(oversized_queue, 'q', sizeof(oversized_queue) - 1u);
        oversized_queue[sizeof(oversized_queue) - 1u] = '\0';
        memset(oversized_reply, 'r', sizeof(oversized_reply) - 1u);
        oversized_reply[sizeof(oversized_reply) - 1u] = '\0';
        memset(&reply_probe, 0, sizeof(reply_probe));
        reply_probe.expected_reply = maximum_reply;
        reply_probe.expected_reply_len = sizeof(maximum_reply) - 1u;
        expect("subscription boundary client connects", boundary_sub != NULL);
        if (boundary_sub) {
            expect(
                "maximum subscription key lengths activate",
                vbus_subscribe(
                    boundary_sub,
                    maximum_topic,
                    maximum_queue,
                    on_count,
                    &boundary_count) == 0);
            expect(
                "maximum subscription topic publishes",
                vbus_publish(
                    pub,
                    maximum_topic,
                    (const uint8_t *)"x",
                    1u) == 0);
            for (waits = 0; waits < 20 && boundary_count == 0; ++waits)
                (void)vbus_poll(boundary_sub, 20);
            expect("maximum subscription key dispatches", boundary_count == 1);
            expect(
                "maximum wildcard prefix activates",
                vbus_subscribe(
                    boundary_sub,
                    maximum_wildcard,
                    NULL,
                    on_count,
                    &maximum_wildcard_count) == 0);
            expect(
                "maximum wildcard prefix topic publishes",
                vbus_publish(
                    pub,
                    maximum_wildcard_topic,
                    (const uint8_t *)"x",
                    1u) == 0);
            for (waits = 0; waits < 20 && maximum_wildcard_count == 0; ++waits)
                (void)vbus_poll(boundary_sub, 20);
            expect(
                "maximum wildcard prefix dispatches",
                maximum_wildcard_count == 1);
            expect(
                "oversized subscription topic rejects",
                vbus_subscribe(
                    boundary_sub,
                    oversized_topic,
                    NULL,
                    on_count,
                    &boundary_count) != 0);
            expect(
                "oversized subscription queue rejects",
                vbus_subscribe(
                    boundary_sub,
                    "test.oversized-queue",
                    oversized_queue,
                    on_count,
                    &boundary_count) != 0);
            expect(
                "maximum broker reply span subscribes",
                vbus_subscribe(
                    boundary_sub,
                    "test.reply-boundary",
                    NULL,
                    on_reply_span,
                    &reply_probe) == 0);
            expect(
                "maximum broker reply span publishes",
                vbus_publish_reply(
                    pub,
                    "test.reply-boundary",
                    maximum_reply,
                    (const uint8_t *)"r",
                    1u) == 0);
            for (waits = 0; waits < 20 && reply_probe.count == 0; ++waits)
                (void)vbus_poll(boundary_sub, 20);
            expect(
                "maximum broker reply span dispatches",
                reply_probe.count == 1 && !reply_probe.invalid);
            vbus_close(boundary_sub);
        }
        oversized_reply_pub = vbus_connect(g_sock);
        expect(
            "oversized broker reply publisher connects",
            oversized_reply_pub != NULL);
        if (oversized_reply_pub) {
            expect(
                "oversized broker reply span writes",
                vbus_publish_reply(
                    oversized_reply_pub,
                    "test.reply-boundary",
                    oversized_reply,
                    (const uint8_t *)"r",
                    1u) == 0);
            for (waits = 0;
                 waits < 20 && !oversized_reply_disconnected;
                 ++waits) {
                if (vbus_poll(oversized_reply_pub, 20) != 0)
                    oversized_reply_disconnected = 1;
            }
            expect(
                "oversized broker reply span disconnects publisher",
                oversized_reply_disconnected);
            vbus_close(oversized_reply_pub);
        }
        embedded_topic_pub = vbus_connect(g_sock);
        expect(
            "embedded-NUL broker topic publisher connects",
            embedded_topic_pub != NULL);
        if (embedded_topic_pub) {
            expect(
                "embedded-NUL broker topic span writes",
                vbus_publish_prepared(
                    embedded_topic_pub,
                    embedded_topic,
                    sizeof(embedded_topic),
                    (const uint8_t *)"n",
                    1u) == 0);
            for (waits = 0;
                 waits < 20 && !embedded_topic_disconnected;
                 ++waits) {
                if (vbus_poll(embedded_topic_pub, 20) != 0)
                    embedded_topic_disconnected = 1;
            }
            expect(
                "embedded-NUL broker topic span disconnects publisher",
                embedded_topic_disconnected);
            vbus_close(embedded_topic_pub);
        }
    }

    /* One broker delivery must target one local subscription. Overlapping
     * patterns on one client must not multiply either callback. */
    overlap_sub = vbus_connect(g_sock);
    expect("overlap subscriber connects", overlap_sub != NULL);
    if (overlap_sub) {
        int exact_count = 0;
        int wildcard_count = 0;
        expect(
            "overlap exact subscribes",
            vbus_subscribe(
                overlap_sub, "test.overlap.value", NULL,
                on_count, &exact_count) == 0);
        expect(
            "overlap wildcard subscribes",
            vbus_subscribe(
                overlap_sub, "test.overlap.>", NULL,
                on_count, &wildcard_count) == 0);
        expect(
            "overlap message publishes",
            vbus_publish(
                pub, "test.overlap.value", (const uint8_t *)"x", 1) == 0);
        for (waits = 0;
             waits < 100 && (exact_count < 1 || wildcard_count < 1);
             ++waits)
            (void)vbus_poll(overlap_sub, 20);
        (void)vbus_poll(overlap_sub, 20);
        expect("overlap exact dispatches once", exact_count == 1);
        expect("overlap wildcard dispatches once", wildcard_count == 1);
        vbus_close(overlap_sub);
    }

    /* Queue identity must survive delivery to a client with several local
     * queue subscriptions, including two members of the same group. */
    queue_identity_sub = vbus_connect(g_sock);
    expect("queue identity subscriber connects", queue_identity_sub != NULL);
    if (queue_identity_sub) {
        int first_count = 0;
        int second_count = 0;
        int other_count = 0;
        int fanout_exact_count = 0;
        int fanout_wildcard_count = 0;
        expect(
            "queue identity first subscribes",
            vbus_subscribe(
                queue_identity_sub, "test.queue-identity", "identity-shared",
                on_count, &first_count) == 0);
        expect(
            "queue identity second subscribes",
            vbus_subscribe(
                queue_identity_sub, "test.queue-identity", "identity-shared",
                on_count, &second_count) == 0);
        expect(
            "queue identity other subscribes",
            vbus_subscribe(
                queue_identity_sub, "test.queue-identity", "identity-other",
                on_count, &other_count) == 0);
        expect(
            "mixed exact fanout subscribes",
            vbus_subscribe(
                queue_identity_sub, "test.queue-identity", NULL,
                on_count, &fanout_exact_count) == 0);
        expect(
            "mixed wildcard fanout subscribes",
            vbus_subscribe(
                queue_identity_sub, "test.>", NULL,
                on_count, &fanout_wildcard_count) == 0);
        expect(
            "queue identity message one publishes",
            vbus_publish(
                pub, "test.queue-identity", (const uint8_t *)"1", 1) == 0);
        expect(
            "queue identity message two publishes",
            vbus_publish(
                pub, "test.queue-identity", (const uint8_t *)"2", 1) == 0);
        for (waits = 0;
             waits < 100 &&
             (first_count + second_count < 2 || other_count < 2 ||
              fanout_exact_count < 2 || fanout_wildcard_count < 2);
             ++waits)
            (void)vbus_poll(queue_identity_sub, 20);
        (void)vbus_poll(queue_identity_sub, 20);
        expect(
            "same-client queue members balance",
            first_count == 1 && second_count == 1);
        expect("same-client queue group dispatches once", other_count == 2);
        expect("mixed exact fanout dispatches", fanout_exact_count == 2);
        expect("mixed wildcard fanout dispatches", fanout_wildcard_count == 2);
        vbus_close(queue_identity_sub);
    }

    /* Each queue group owns its cursor. These names collide in the 8,192-slot
     * index. Interleaving must not make one group repeat a member. */
    queue_a1 = vbus_connect(g_sock);
    queue_a2 = vbus_connect(g_sock);
    queue_b = vbus_connect(g_sock);
    expect("queue-group connects", queue_a1 && queue_a2 && queue_b);
    if (queue_a1 && queue_a2 && queue_b) {
        int count_a1 = 0;
        int count_a2 = 0;
        int count_b = 0;

        expect(
            "queue group a1 subscribe",
            vbus_subscribe(
                queue_a1,
                "test.queue-groups",
                "queue-collision-118",
                on_count,
                &count_a1) == 0
        );
        expect(
            "queue group a2 subscribe",
            vbus_subscribe(
                queue_a2,
                "test.queue-groups",
                "queue-collision-118",
                on_count,
                &count_a2) == 0
        );
        expect(
            "queue group b subscribe",
            vbus_subscribe(
                queue_b,
                "test.queue-groups",
                "queue-collision-600",
                on_count,
                &count_b) == 0
        );
        expect(
            "queue publish one",
            vbus_publish(pub, "test.queue-groups", (const uint8_t *)"1", 1) == 0
        );
        expect(
            "queue publish two",
            vbus_publish(pub, "test.queue-groups", (const uint8_t *)"2", 1) == 0
        );
        for (waits = 0;
             waits < 100 && (count_a1 + count_a2 < 2 || count_b < 2);
             ++waits) {
            (void)vbus_poll(queue_a1, 10);
            (void)vbus_poll(queue_a2, 10);
            (void)vbus_poll(queue_b, 10);
        }
        expect("queue group a balanced", count_a1 == 1 && count_a2 == 1);
        expect("queue group b independent", count_b == 2);
    }
    if (queue_a1) vbus_close(queue_a1);
    if (queue_a2) vbus_close(queue_a2);
    if (queue_b) vbus_close(queue_b);

    /* Dual microbench: pub→sub latency same payload */
    {
        enum { ITER = 2000 };
        uint8_t payload[256];
        memset(payload, 0xab, sizeof(payload));
        struct timespec t0, t1;
        volatile int sink = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < ITER; i++) {
            (void)vbus_publish(pub, "ai.voice.stream.bench", payload, sizeof(payload));
            (void)vbus_poll(sub_stream, 1);
            sink += g_got_chunk;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        printf("BenchmarkVBus_PubPoll_256B\t%d\t%.2f ns/op\tsink=%d\n", ITER, ns / (double)ITER, sink);
    }

    vbus_close(pub);
    vbus_close(sub_stream);
    vbus_close(sub_orch);
    vbus_close(sub_turn);
    vbus_close(sub_speak);
    if (engine) audio_engine_destroy(engine);
    atomic_store_explicit(&g_broker_stop, 1, memory_order_relaxed);
    pthread_join(thr, NULL);
    unlink(g_sock);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ALL PASS vbus (no NATS)\n");
    return 0;
}
