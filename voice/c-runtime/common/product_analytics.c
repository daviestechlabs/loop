#define _POSIX_C_SOURCE 200809L
#include "product_analytics.h"
#include "http_min.h"
#include "ia_transport.h"
#include "iaevents.h"
#include "service.h"

#include <inttypes.h>
#include <errno.h>
#ifdef __APPLE__
#include <fcntl.h>
#endif
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ANALYTICS_QUEUE_CAP 16u
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "analytics counters must be lock-free");
_Static_assert((ANALYTICS_QUEUE_CAP & (ANALYTICS_QUEUE_CAP - 1u)) == 0u,
    "ring capacity must divide the unsigned sequence-number range");

struct voice_analytics {
    pthread_t thread;
#ifdef __APPLE__
    sem_t *ready;
#else
    sem_t ready;
#endif
    atomic_int stop;
    atomic_uint dropped;
    atomic_uint queued;
    atomic_uint accepted;
    atomic_uint failed;
    voice_analytics_turn queue[ANALYTICS_QUEUE_CAP];
    atomic_uint head;
    atomic_uint tail;
    char url[800];
    char service[64];
};

/* macOS does not implement unnamed POSIX semaphores. Unlink a private named
 * semaphore immediately; only this worker retains its handle. Linux keeps
 * the existing unnamed semaphore and the producer never waits on either. */
static int ready_init(voice_analytics *worker) {
#ifdef __APPLE__
    char event[33], name[32];
    if (ia_new_event_id(event, sizeof(event)) != IA_OK) return -1;
    int n = snprintf(name, sizeof(name), "/dtl-va-%.20s", event);
    if (n <= 0 || (size_t)n >= sizeof(name)) return -1;
    worker->ready = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (worker->ready == SEM_FAILED) return -1;
    if (sem_unlink(name)) {
        (void)sem_close(worker->ready);
        worker->ready = SEM_FAILED;
        return -1;
    }
    return 0;
#else
    return sem_init(&worker->ready, 0, 0);
#endif
}

static sem_t *ready_handle(voice_analytics *worker) {
#ifdef __APPLE__
    return worker->ready;
#else
    return &worker->ready;
#endif
}

static void ready_destroy(voice_analytics *worker) {
#ifdef __APPLE__
    (void)sem_close(worker->ready);
#else
    (void)sem_destroy(&worker->ready);
#endif
}

static int identifier(const char *text, size_t cap, int owner) {
    size_t i;
    if (!text || !cap || !text[0]) return 0;
    for (i = 0; i < cap; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!c) return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == ':' || (owner && c == '@'))) return 0;
    }
    return 0;
}

void voice_analytics_begin(voice_analytics_turn *o, const turn_start_c *turn,
                           int audio_input, int webtransport, uint64_t admitted_ns) {
    const char *profile;
    if (!o) return;
    memset(o, 0, sizeof(*o));
    if (!turn || !admitted_ns ||
        !identifier(turn->user_id, sizeof(o->owner), 1) ||
        !identifier(turn->request_id, sizeof(o->request_id), 0) ||
        !identifier(turn->metadata.product_session_id, sizeof(o->nonce), 0) ||
        (turn->metadata.campaign_id[0] &&
         !identifier(turn->metadata.campaign_id, sizeof(o->campaign), 0))) return;
    profile = turn->metadata.interaction_profile;
    if (!identifier(profile, sizeof(o->profile), 0)) profile = "unknown";
    memcpy(o->owner, turn->user_id, strlen(turn->user_id) + 1u);
    memcpy(o->request_id, turn->request_id, strlen(turn->request_id) + 1u);
    memcpy(o->nonce, turn->metadata.product_session_id, strlen(turn->metadata.product_session_id) + 1u);
    memcpy(o->campaign, turn->metadata.campaign_id, strlen(turn->metadata.campaign_id) + 1u);
    memcpy(o->profile, profile, strlen(profile) + 1u);
    o->admitted_ns = admitted_ns;
    o->audio_input = audio_input != 0;
    o->voice_output = turn->enable_tts != 0;
    o->webtransport = webtransport != 0;
}

void voice_analytics_audio(voice_analytics_turn *o, uint64_t now_ns) {
    if (o && o->admitted_ns && !o->outcome && !o->first_audio_ns &&
        now_ns >= o->admitted_ns) o->first_audio_ns = now_ns;
}

int voice_analytics_wire(const voice_analytics_turn *o, long long now_ms, char *out, size_t cap) {
    char session[IA_SESSION_CAP], event[33], timestamp[40], stages[96];
    const char *outcome;
    struct tm utc;
    time_t seconds;
    uint64_t completion_us, audio_us;
    int n;
    if (!out || !cap) return -1;
    out[0] = '\0';
    if (!o || now_ms <= 0 || !o->admitted_ns || o->finished_ns < o->admitted_ns ||
        (o->first_audio_ns && (o->first_audio_ns < o->admitted_ns ||
                               o->first_audio_ns > o->finished_ns)) ||
        !identifier(o->request_id, sizeof(o->request_id), 0) ||
        !identifier(o->profile, sizeof(o->profile), 0) ||
        (o->campaign[0] && !identifier(o->campaign, sizeof(o->campaign), 0)) ||
        ia_product_session_id(o->owner, o->nonce, session, sizeof(session)) ||
        ia_new_event_id(event, sizeof(event)) != IA_OK) return -1;
    switch (o->outcome) {
    case VOICE_ANALYTICS_COMPLETED: outcome = "completed"; break;
    case VOICE_ANALYTICS_CANCELED: outcome = "canceled"; break;
    case VOICE_ANALYTICS_FAILED: outcome = "failed"; break;
    case VOICE_ANALYTICS_DISCONNECTED: outcome = "disconnected"; break;
    default: return -1;
    }
    seconds = (time_t)(now_ms / 1000);
    if (!gmtime_r(&seconds, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc)) return -1;
    completion_us = (o->finished_ns - o->admitted_ns) / 1000u;
    audio_us = o->first_audio_ns ? (o->first_audio_ns - o->admitted_ns) / 1000u : 0u;
    if (o->first_audio_ns) {
        n = snprintf(stages, sizeof(stages),
            "{\"completion\":%" PRIu64 ".%03" PRIu64 ",\"tts_first_audio\":%" PRIu64 ".%03" PRIu64 "}",
            completion_us / 1000u, completion_us % 1000u, audio_us / 1000u, audio_us % 1000u);
    } else {
        n = snprintf(stages, sizeof(stages),
            "{\"completion\":%" PRIu64 ".%03" PRIu64 "}",
            completion_us / 1000u, completion_us % 1000u);
    }
    if (n < 0 || (size_t)n >= sizeof(stages)) {
        out[0] = '\0';
        return -1;
    }
    n = snprintf(out, cap,
        "{\"event_id\":\"%s\",\"type\":\"turn\",\"schema_version\":\"%s\","
        "\"occurred_at\":\"%s.%03lldZ\",\"session_id\":\"%s\",\"turn_id\":\"%s\","
        "\"user_id\":\"%s\",\"source_repo\":\"daviestechlabs-homelab\","
        "\"labels\":{\"surface\":\"companions-frontend\",\"auth_state\":\"%s\","
        "\"evidence_source\":\"c_gateway\",\"transport\":\"%s\","
        "\"latency_origin\":\"gateway_admission\",\"audio_observation\":\"edge_queue\"},"
        "\"payload\":{\"profile\":\"%s\",\"campaign_id\":\"%s\","
        "\"input_modality\":\"%s\",\"output_modality\":\"%s\",\"outcome\":\"%s\","
        "\"answer_completed\":%s,\"barge_in\":%s,"
        "\"first_audio_latency_ms\":%" PRIu64 ".%03" PRIu64 ","
        "\"completion_latency_ms\":%" PRIu64 ".%03" PRIu64 ","
        "\"stage_ms\":%s}}",
        event, IA_VERSION, timestamp, now_ms % 1000, session, o->request_id, o->owner,
        strncmp(o->owner, "guest_", 6u) ? "authenticated" : "guest",
        o->webtransport ? "webtransport" : "http", o->profile, o->campaign,
        o->audio_input ? "voice" : "text", o->voice_output ? "voice" : "text", outcome,
        o->outcome == VOICE_ANALYTICS_COMPLETED ? "true" : "false",
        o->outcome == VOICE_ANALYTICS_CANCELED && o->client_interrupt && o->first_audio_ns ? "true" : "false",
        audio_us / 1000u, audio_us % 1000u, completion_us / 1000u, completion_us % 1000u,
        stages);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static void *deliver(void *arg) {
    voice_analytics *worker = arg;
    unsigned reported_dropped = 0;
    for (;;) {
        voice_analytics_turn observation;
        struct timespec now;
        char wire[IA_WIRE_CAP];
        uint8_t otlp[IA_OTLP_CAP], ack[2048];
        size_t wire_len = 0, ack_len = 0;
        long long now_ms;
        int status = 0, ok = 0;
        int wait_status;
        do { wait_status = sem_wait(ready_handle(worker)); } while (wait_status && errno == EINTR);
        if (wait_status) break;
        if (atomic_load_explicit(&worker->stop, memory_order_relaxed)) {
            unsigned tail = atomic_load_explicit(&worker->tail, memory_order_acquire);
            unsigned head = atomic_load_explicit(&worker->head, memory_order_relaxed);
            atomic_fetch_add_explicit(&worker->dropped, tail - head, memory_order_relaxed);
            break;
        }
        {
            unsigned head = atomic_load_explicit(&worker->head, memory_order_relaxed);
            /* Acquire publishes the producer's owned snapshot. Release below
             * gives that slot back only after the consumer copies and scrubs. */
            (void)atomic_load_explicit(&worker->tail, memory_order_acquire);
            observation = worker->queue[head % ANALYTICS_QUEUE_CAP];
            memset(&worker->queue[head % ANALYTICS_QUEUE_CAP], 0, sizeof(observation));
            atomic_store_explicit(&worker->head, head + 1u, memory_order_release);
        }
        if (!clock_gettime(CLOCK_REALTIME, &now) && now.tv_sec > 0) {
            now_ms = (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
            if (!voice_analytics_wire(&observation, observation.finished_unix_ms, wire, sizeof(wire)) &&
                !ia_otlp_encode(worker->service, wire, now_ms, otlp, sizeof(otlp), &wire_len) &&
                http_min_post_headers_cancel(worker->url, "application/x-protobuf", NULL, 0u,
                    otlp, wire_len, ack, sizeof(ack), &ack_len, &status, 1500, &worker->stop) == HTTP_MIN_OK &&
                status == 200 && ia_otlp_ack(ack, ack_len)) ok = 1;
        }
        if (ok) atomic_fetch_add_explicit(&worker->accepted, 1u, memory_order_relaxed);
        else {
            unsigned failed = atomic_fetch_add_explicit(&worker->failed, 1u, memory_order_relaxed) + 1u;
            svc_log(worker->service, "analytics delivery failed total=%u dropped=%u", failed,
                atomic_load_explicit(&worker->dropped, memory_order_relaxed));
        }
        {
            unsigned dropped = atomic_load_explicit(&worker->dropped, memory_order_relaxed);
            if (dropped != reported_dropped) {
                svc_log(worker->service, "analytics queue dropped total=%u", dropped);
                reported_dropped = dropped;
            }
        }
        memset(&observation, 0, sizeof(observation));
    }
    return NULL;
}

voice_analytics *voice_analytics_start(const char *url, const char *service) {
    voice_analytics *worker;
    http_min_url parsed;
    if (!url || !url[0]) return NULL;
    if (strnlen(url, 800u) >= 800u || http_min_parse_url(url, &parsed) ||
        strcmp(parsed.path, "/v1/logs") || !identifier(service, 64u, 0)) {
        svc_log("product-analytics", "invalid analytics endpoint or service");
        return NULL;
    }
    worker = calloc(1, sizeof(*worker));
    if (!worker) return NULL;
    atomic_init(&worker->stop, 0);
    atomic_init(&worker->dropped, 0u);
    atomic_init(&worker->queued, 0u);
    atomic_init(&worker->accepted, 0u);
    atomic_init(&worker->failed, 0u);
    atomic_init(&worker->head, 0u);
    atomic_init(&worker->tail, 0u);
    memcpy(worker->url, url, strlen(url) + 1u);
    memcpy(worker->service, service, strlen(service) + 1u);
    if (ready_init(worker)) { free(worker); return NULL; }
    if (pthread_create(&worker->thread, NULL, deliver, worker)) {
        ready_destroy(worker);
        free(worker);
        return NULL;
    }
    return worker;
}

void voice_analytics_finish(voice_analytics *worker, voice_analytics_turn *o,
                            enum voice_analytics_outcome outcome, uint64_t now_ns) {
    struct timespec now;
    if (!o || !o->admitted_ns || o->outcome) return;
    o->outcome = outcome;
    o->finished_ns = now_ns;
    if (!worker) return;
    if (!clock_gettime(CLOCK_REALTIME, &now) && now.tv_sec > 0)
        o->finished_unix_ms = (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
    unsigned tail = atomic_load_explicit(&worker->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&worker->head, memory_order_acquire);
    if (tail - head == ANALYTICS_QUEUE_CAP ||
        atomic_load_explicit(&worker->stop, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&worker->dropped, 1u, memory_order_relaxed);
    } else {
        worker->queue[tail % ANALYTICS_QUEUE_CAP] = *o;
        atomic_store_explicit(&worker->tail, tail + 1u, memory_order_release);
        atomic_fetch_add_explicit(&worker->queued, 1u, memory_order_relaxed);
        /* At most 16 pending records plus the stop wakeup. No semaphore wait
         * or worker-owned lock runs on the event-loop producer. */
        (void)sem_post(ready_handle(worker));
    }
}

voice_analytics_counts voice_analytics_get_counts(const voice_analytics *worker) {
    voice_analytics_counts counts = {0};
    if (worker) {
        counts.queued = atomic_load_explicit(&worker->queued, memory_order_relaxed);
        counts.accepted = atomic_load_explicit(&worker->accepted, memory_order_relaxed);
        counts.failed = atomic_load_explicit(&worker->failed, memory_order_relaxed);
        counts.dropped = atomic_load_explicit(&worker->dropped, memory_order_relaxed);
    }
    return counts;
}

void voice_analytics_stop(voice_analytics *worker) {
    if (!worker) return;
    atomic_store_explicit(&worker->stop, 1, memory_order_relaxed);
    (void)sem_post(ready_handle(worker));
    pthread_join(worker->thread, NULL);
    svc_log(worker->service, "analytics stopped accepted=%u failed=%u dropped=%u",
        atomic_load_explicit(&worker->accepted, memory_order_relaxed),
        atomic_load_explicit(&worker->failed, memory_order_relaxed),
        atomic_load_explicit(&worker->dropped, memory_order_relaxed));
    ready_destroy(worker);
    memset(worker, 0, sizeof(*worker));
    free(worker);
}
