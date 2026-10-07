#define _POSIX_C_SOURCE 200809L
#include "product_analytics.h"
#include "cmp_json.h"
#include "ia_transport.h"
#include "gateway_http.h"
#include "http_min.h"
#include "voice_auth.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void pause_ms(long ms) {
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

static uint64_t clock_ns(void) {
    struct timespec now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static voice_analytics_turn observation(const char *owner, const char *request) {
    turn_start_c turn = {0};
    voice_analytics_turn result;
    snprintf(turn.user_id, sizeof(turn.user_id), "%s", owner);
    snprintf(turn.request_id, sizeof(turn.request_id), "%s", request);
    snprintf(turn.metadata.product_session_id, sizeof(turn.metadata.product_session_id), "browser-a");
    snprintf(turn.metadata.campaign_id, sizeof(turn.metadata.campaign_id), "campaign-original");
    snprintf(turn.metadata.interaction_profile, sizeof(turn.metadata.interaction_profile), "table");
    turn.enable_tts = 1;
    voice_analytics_begin(&result, &turn, 1, 1, 1000000000u);
    voice_analytics_audio(&result, 1123456000u);
    return result;
}

static void test_policy(void) {
    voice_analytics_turn turn = observation("user-a", "turn-original");
    char wire[IA_WIRE_CAP], value[256], other[IA_SESSION_CAP];
    cmp_json_object root, payload, labels, stages;
    voice_analytics_finish(NULL, &turn, VOICE_ANALYTICS_COMPLETED, 2000123000u);
    voice_analytics_finish(NULL, &turn, VOICE_ANALYTICS_FAILED, 3000000000u);
    CHECK(turn.outcome == VOICE_ANALYTICS_COMPLETED && turn.finished_ns == 2000123000u);
    CHECK(!voice_analytics_wire(&turn, 1700000000123LL, wire, sizeof(wire)));
    CHECK(cmp_json_object_parse(wire, &root));
    CHECK(cmp_json_object_object(&root, "payload", &payload));
    CHECK(cmp_json_object_object(&root, "labels", &labels));
    CHECK(cmp_json_object_object(&payload, "stage_ms", &stages));
    CHECK(cmp_json_object_str(&root, "session_id", value, sizeof(value)));
    CHECK(!strcmp(value, "ps1-47f12221eee4439e8e994aeef7bf4d9fd4364916b1917ad97dcb86725daedd47"));
    CHECK(!ia_product_session_id("user-b", "browser-a", other, sizeof(other)) && strcmp(other, value));
    CHECK(cmp_json_object_str(&root, "occurred_at", value, sizeof(value)) && !strcmp(value, "2023-11-14T22:13:20.123Z"));
    CHECK(cmp_json_object_str(&labels, "evidence_source", value, sizeof(value)) && !strcmp(value, "c_gateway"));
    CHECK(cmp_json_object_str(&labels, "latency_origin", value, sizeof(value)) && !strcmp(value, "gateway_admission"));
    CHECK(cmp_json_object_str(&payload, "campaign_id", value, sizeof(value)) && !strcmp(value, "campaign-original"));
    CHECK(strstr(wire, "\"first_audio_latency_ms\":123.456"));
    CHECK(strstr(wire, "\"completion_latency_ms\":1000.123"));
    CHECK(strstr(wire, "\"tts_first_audio\":123.456"));
    CHECK(!cmp_json_object_key_count(&root, "trace_id") && !cmp_json_object_key_count(&root, "model_id"));
    CHECK(!cmp_json_object_key_count(&labels, "route") && stages.field_count == 2u);
    CHECK(!strstr(wire, "browser-a") && !strstr(wire, "prompt"));
    turn = observation("user-a", "cancel");
    turn.client_interrupt = 1;
    voice_analytics_finish(NULL, &turn, VOICE_ANALYTICS_CANCELED, 2000000000u);
    CHECK(!voice_analytics_wire(&turn, 1, wire, sizeof(wire)) && strstr(wire, "\"barge_in\":true"));
    turn.outcome = VOICE_ANALYTICS_DISCONNECTED;
    CHECK(!voice_analytics_wire(&turn, 1, wire, sizeof(wire)) && strstr(wire, "\"barge_in\":false"));
    turn.outcome = VOICE_ANALYTICS_CANCELED;
    turn.first_audio_ns = 0;
    CHECK(!voice_analytics_wire(&turn, 1, wire, sizeof(wire)) && strstr(wire, "\"barge_in\":false"));
    turn.finished_ns = turn.admitted_ns - 1u;
    CHECK(voice_analytics_wire(&turn, 1, wire, sizeof(wire)) && !wire[0]);
    turn = observation("user-a", "invalid");
    turn.outcome = VOICE_ANALYTICS_COMPLETED;
    turn.finished_ns = 2000000000u;
    CHECK(voice_analytics_wire(&turn, 1, wire, 20u) && !wire[0]);
    strcpy(turn.campaign, "invalid\"campaign");
    CHECK(voice_analytics_wire(&turn, 1, wire, sizeof(wire)) && !wire[0]);
    turn = observation("invalid\nowner", "request");
    CHECK(!turn.admitted_ns);
    turn_start_c input = {0};
    voice_analytics_begin(&turn, &input, 1, 1, 1);
    CHECK(!turn.admitted_ns);
    CHECK(!voice_analytics_start(NULL, "c-webtransport-gateway"));
    CHECK(!voice_analytics_start("http://127.0.0.1:1/wrong", "c-webtransport-gateway"));
}

typedef struct {
    int fd;
    atomic_int arrived;
    atomic_int release;
    int reject;
    char request[8192];
} receiver;

static void *receive_one(void *arg) {
    receiver *server = arg;
    struct timeval timeout = {.tv_sec = 4, .tv_usec = 0};
    size_t len = 0;
    struct pollfd ready = {.fd = server->fd, .events = POLLIN};
    CHECK(poll(&ready, 1, 4000) == 1);
    int fd = accept(server->fd, NULL, NULL);
    CHECK(fd >= 0);
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    for (;;) {
        ssize_t count = recv(fd, server->request + len, sizeof(server->request) - len - 1u, 0);
        CHECK(count > 0);
        len += (size_t)count;
        server->request[len] = '\0';
        char *end = strstr(server->request, "\r\n\r\n");
        char *length = strstr(server->request, "Content-Length: ");
        if (end && length && len >= (size_t)(end + 4 - server->request) + strtoul(length + 16, NULL, 10)) break;
    }
    atomic_store(&server->arrived, 1);
    for (int i = 0; !atomic_load(&server->release) && i < 3000; ++i) pause_ms(1);
    const char *reply = server->reject ?
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n\x0a\x02\x08\x01" :
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    CHECK(send(fd, reply, strlen(reply), MSG_NOSIGNAL) == (ssize_t)strlen(reply));
    close(fd);
    return NULL;
}

static void test_delivery(int reject) {
    receiver server = {.fd = socket(AF_INET, SOCK_STREAM, 0), .reject = reject};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t address_len = sizeof(address);
    pthread_t thread;
    char url[128];
    atomic_init(&server.arrived, 0);
    atomic_init(&server.release, 0);
    CHECK(server.fd >= 0 && !bind(server.fd, (struct sockaddr *)&address, sizeof(address)));
    CHECK(!listen(server.fd, 32) && !getsockname(server.fd, (struct sockaddr *)&address, &address_len));
    CHECK(!pthread_create(&thread, NULL, receive_one, &server));
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1/logs", (unsigned)ntohs(address.sin_port));
    voice_analytics *worker = voice_analytics_start(url, "c-webtransport-gateway");
    CHECK(worker);
    voice_analytics_turn turn = observation("user-a", "immutable-turn");
    voice_analytics_finish(worker, &turn, VOICE_ANALYTICS_COMPLETED, 2000000000u);
    for (int i = 0; !atomic_load(&server.arrived) && i < 2000; ++i) pause_ms(1);
    CHECK(atomic_load(&server.arrived));
    CHECK(!voice_analytics_get_counts(worker).accepted);
    /* An active connection slot can be immediately scrubbed or reused. */
    memset(&turn, 0, sizeof(turn));
    if (!reject) {
        uint64_t start = clock_ns();
        for (int i = 0; i < 1000; ++i) {
            turn = observation("user-b", "new-slot");
            voice_analytics_finish(worker, &turn, VOICE_ANALYTICS_FAILED, 2000000000u);
        }
        CHECK(clock_ns() - start < 500000000u);
        voice_analytics_counts counts = voice_analytics_get_counts(worker);
        CHECK(counts.queued == 17u && counts.dropped == 984u);
    }
    atomic_store(&server.release, 1);
    CHECK(!pthread_join(thread, NULL));
    /* Binary protobuf contains the full JSON record after its resource fields. */
    int immutable = 0, wrong_owner = 0;
    for (size_t i = 0; i + 32u < sizeof(server.request); ++i) {
        if (!memcmp(server.request + i, "immutable-turn", 14u)) immutable = 1;
        if (!memcmp(server.request + i, "user-b", 6u)) wrong_owner = 1;
    }
    CHECK(immutable && !wrong_owner);
    for (int i = 0; i < 1000; ++i) {
        voice_analytics_counts counts = voice_analytics_get_counts(worker);
        if (counts.accepted + counts.failed) break;
        pause_ms(1);
    }
    voice_analytics_counts counts = voice_analytics_get_counts(worker);
    CHECK(reject ? counts.failed == 1u && !counts.accepted : counts.accepted == 1u);
    close(server.fd);
    voice_analytics_stop(worker);
}

static void test_idle_workers(void) {
    voice_analytics *workers[4];
    for (size_t i = 0; i < 4u; ++i) {
        workers[i] = voice_analytics_start("http://127.0.0.1:1/v1/logs",
            "c-webtransport-gateway");
        CHECK(workers[i]);
    }
    for (size_t i = 0; i < 4u; ++i) {
        voice_analytics_counts counts = voice_analytics_get_counts(workers[i]);
        CHECK(!counts.queued && !counts.accepted && !counts.failed && !counts.dropped);
        voice_analytics_stop(workers[i]);
    }
}

typedef struct {
    char path[108];
    int port;
    vbus_client *bus;
    vbus_stop_flag stop;
} gateway_fixture;

static void *broker_run(void *arg) {
    gateway_fixture *fixture = arg;
    CHECK(!vbus_broker_run(fixture->path, &fixture->stop));
    return NULL;
}

static void *http_run(void *arg) {
    gateway_fixture *fixture = arg;
    CHECK(!gateway_http_run(fixture->port, &fixture->stop));
    return NULL;
}

static void *peer_run(void *arg) {
    gateway_fixture *fixture = arg;
    CHECK(!vbus_run(fixture->bus, &fixture->stop));
    return NULL;
}

static void reply_turn(const char *subject, const char *reply, const uint8_t *data,
                        size_t length, void *arg) {
    gateway_fixture *fixture = arg;
    turn_start_c turn;
    uint8_t wire[1024];
    size_t size;
    (void)subject; (void)reply;
    CHECK(!pb_decode_turn_start(data, length, &turn));
    CHECK(!strcmp(turn.user_id, "user-a"));
    CHECK(!strcmp(turn.metadata.product_session_id, "browser-a"));
    size = pb_encode_turn_text_event(wire, sizeof(wire), turn.request_id,
        "text_completed", "fixture answer", "fixture answer", "fixture answer", 0, 1);
    CHECK(size && !vbus_publish(fixture->bus, turn.response_subject, wire, size));
    size = pb_encode_turn_event(wire, sizeof(wire), turn.request_id, "completed", "");
    CHECK(size && !vbus_publish(fixture->bus, turn.response_subject, wire, size));
}

static int listener(int *port) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0 && !bind(fd, (struct sockaddr *)&address, sizeof(address)));
    CHECK(!listen(fd, 4) && !getsockname(fd, (struct sockaddr *)&address, &len));
    *port = (int)ntohs(address.sin_port);
    return fd;
}

static void test_http_gateway(void) {
    char directory[] = "/tmp/c-analytics-http.XXXXXX";
    char endpoint[128], url[128], health[128], timestamp[32];
    const char secret[] = "analytics-disposable-gateway-secret-0123456789";
    gateway_fixture broker = {0}, gateway = {0}, peer = {0};
    pthread_t broker_thread, gateway_thread, peer_thread, receiver_thread;
    receiver server = {0};
    int port, status = 0;
    uint8_t output[8192], wire[8192];
    size_t output_len, wire_len;
    char nonce[VOICE_AUTH_NONCE_HEX_LEN + 1u], signature[VOICE_AUTH_SIGNATURE_HEX_LEN + 1u];
    CHECK(mkdtemp(directory));
    snprintf(broker.path, sizeof(broker.path), "%s/vbus.sock", directory);
    atomic_init(&broker.stop, 0);
    atomic_init(&gateway.stop, 0);
    atomic_init(&peer.stop, 0);
    atomic_init(&server.arrived, 0);
    atomic_init(&server.release, 1);
    server.fd = listener(&port);
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d/v1/logs", port);
    int unused = listener(&gateway.port);
    close(unused);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/voice/turns", gateway.port);
    snprintf(health, sizeof(health), "http://127.0.0.1:%d/healthz", gateway.port);
    CHECK(!setenv("VBUS_PATH", broker.path, 1) && !setenv("VOICE_GATEWAY_TOKEN", secret, 1));
    CHECK(!setenv("GATEWAY_HTTP_BIND_ADDRESS", "127.0.0.1", 1));
    CHECK(!setenv("VOICE_ANALYTICS_OTLP_URL", endpoint, 1));
    CHECK(!pthread_create(&broker_thread, NULL, broker_run, &broker));
    for (int i = 0; !peer.bus && i < 200; ++i) {
        peer.bus = vbus_connect(broker.path);
        if (!peer.bus) pause_ms(10);
    }
    CHECK(peer.bus && !vbus_subscribe(peer.bus, "ai.turn.start", NULL, reply_turn, &peer));
    CHECK(!pthread_create(&peer_thread, NULL, peer_run, &peer));
    CHECK(!setenv("GATEWAY_HTTP_BIND_ADDRESS", "invalid-address", 1));
    CHECK(gateway_http_run(gateway.port, &gateway.stop) == -1);
    CHECK(!setenv("GATEWAY_HTTP_BIND_ADDRESS", "127.0.0.1", 1));
    CHECK(!pthread_create(&gateway_thread, NULL, http_run, &gateway));
    for (int i = 0; i < 200; ++i) {
        if (!http_min_get(health, output, sizeof(output), &output_len, &status, 100) && status == 200) break;
        pause_ms(10);
    }
    CHECK(status == 200);
    turn_start_c turn = {0};
    strcpy(turn.user_id, "user-a");
    strcpy(turn.request_id, "authenticated-http-terminal");
    strcpy(turn.text, "This text must never enter analytics.");
    strcpy(turn.metadata.product_session_id, "browser-a");
    strcpy(turn.metadata.campaign_id, "campaign-original");
    strcpy(turn.metadata.interaction_profile, "table");
    wire_len = pb_encode_turn_start(wire, sizeof(wire), &turn);
    CHECK(wire_len);
    CHECK(http_min_post(url, "application/x-protobuf", wire, wire_len,
        output, sizeof(output), &output_len, &status, 1000) == HTTP_MIN_ERR_STATUS && status == 401);
    CHECK(!atomic_load(&server.arrived));
    int64_t now = (int64_t)time(NULL);
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)now);
    CHECK(voice_auth_random_nonce(nonce) == VOICE_AUTH_OK);
    CHECK(voice_auth_sign(secret, strlen(secret), "POST", "/v1/voice/turns", "user-a",
        now, nonce, wire, wire_len, signature) == VOICE_AUTH_OK);
    http_min_header headers[] = {{"X-Voice-User", "user-a"}, {"X-Voice-Timestamp", timestamp},
        {"X-Voice-Nonce", nonce}, {"X-Voice-Signature", signature}};
    CHECK(!pthread_create(&receiver_thread, NULL, receive_one, &server));
    CHECK(!http_min_post_headers(url, "application/x-protobuf", headers, 4u, wire, wire_len,
        output, sizeof(output) - 1u, &output_len, &status, 3000) && status == 200);
    output[output_len] = 0;
    CHECK(strstr((char *)output, "\"type\":\"completed\""));
    CHECK(!pthread_join(receiver_thread, NULL));
    CHECK(atomic_load(&server.arrived));
    int valid = 0, leaked = 0;
    for (size_t i = 0; i + 80u < sizeof(server.request); ++i) {
        if (!memcmp(server.request + i, "\"turn_id\":\"authenticated-http-terminal\"", 39u)) valid = 1;
        if (!memcmp(server.request + i, "This text must never", 20u)) leaked = 1;
    }
    CHECK(valid && !leaked);
    for (int i = 0; i < 100; ++i) {
        CHECK(!http_min_get(health, output, sizeof(output) - 1u, &output_len, &status, 1000));
        output[output_len] = 0;
        if (strstr((char *)output, "\"accepted\":1")) break;
        pause_ms(5);
    }
    CHECK(strstr((char *)output, "\"enabled\":true"));
    CHECK(strstr((char *)output, "\"queued\":1") && strstr((char *)output, "\"accepted\":1"));
    CHECK(strstr((char *)output, "\"failed\":0"));
    atomic_store(&gateway.stop, 1);
    CHECK(!pthread_join(gateway_thread, NULL));
    atomic_store(&peer.stop, 1);
    CHECK(!pthread_join(peer_thread, NULL));
    vbus_close(peer.bus);
    atomic_store(&broker.stop, 1);
    CHECK(!pthread_join(broker_thread, NULL));
    close(server.fd);
    unlink(broker.path);
    CHECK(!rmdir(directory));
    unsetenv("VBUS_PATH");
    unsetenv("VOICE_GATEWAY_TOKEN");
    unsetenv("GATEWAY_HTTP_BIND_ADDRESS");
    unsetenv("VOICE_ANALYTICS_OTLP_URL");
}

int main(int argc, char **argv) {
    if (argc == 2) {
        voice_analytics *worker = voice_analytics_start(argv[1], "c-webtransport-gateway");
        CHECK(worker);
        voice_analytics_turn turn = observation("user-a", "vector-terminal-turn");
        voice_analytics_finish(worker, &turn, VOICE_ANALYTICS_COMPLETED, 2000000000u);
        for (int i = 0; i < 4000; ++i) {
            voice_analytics_counts counts = voice_analytics_get_counts(worker);
            if (counts.accepted + counts.failed) break;
            pause_ms(1);
        }
        voice_analytics_counts counts = voice_analytics_get_counts(worker);
        voice_analytics_stop(worker);
        CHECK(counts.accepted == 1u && !counts.failed);
        puts("ALL PASS C terminal event delivered");
        return 0;
    }
    CHECK(argc == 1);
    test_policy();
    test_idle_workers();
    test_delivery(0);
    test_delivery(1);
    test_http_gateway();
    printf("analytics snapshot bytes=%zu queue slots=16\n", sizeof(voice_analytics_turn));
    puts("ALL PASS C gateway analytics policy, ownership, queue pressure, OTLP ACK, and authenticated HTTP");
    return 0;
}
