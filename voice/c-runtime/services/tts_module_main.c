/*
 * Pure-C turn TTS worker.
 *
 * The VBus callback only validates and queues canonical TurnTTSSegmentRequest
 * messages. Bounded worker lanes preserve ordering for each request_id while
 * leaving the event loop free to observe ai.turn.cancel during HTTP synthesis.
 */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../common/http_min.h"
#include "../common/openai_min.h"
#include "../common/pcm_frame.h"
#include "../common/vbus_subject.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include "speech_sanitize.h"
#include "pcm_parse.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TTS_WORKER_LANES 4
#define TTS_PIPELINE_DEPTH 2
#define TTS_QUEUE_PER_LANE 16
#define TTS_JOBS_PER_LANE (TTS_QUEUE_PER_LANE + 1)
#define TTS_ORDER_CAPACITY (TTS_WORKER_LANES * TTS_JOBS_PER_LANE)
#define TTS_ORDER_BUCKET_COUNT 128u
#define TTS_ORDER_NONE UINT8_MAX
#define TTS_ORDER_STALE_MS UINT64_C(600000)
#define TTS_STAGE_FUTURE_SKEW_MS INT64_C(60000)
#define TTS_PCM_RESPONSE_MAX (8u * 1024u * 1024u)
#define TTS_HTTP_PRECONNECT_TIMEOUT_MS 250
#define TTS_FINALITY_TIMEOUT_MS_DEFAULT 65000
#define TTS_FINALITY_TIMEOUT_MS_MAX 600000
#define TTS_STREAM_ERR_INVALID -1
#define TTS_STREAM_ERR_PUBLISH -2
#define TTS_STREAM_ERR_FINALITY -3

typedef struct tts_job tts_job;
typedef struct tts_state tts_state;
typedef struct tts_lane tts_lane;

typedef struct {
    char request_id[128];
    char event_subject[256];
    uint16_t request_id_len;
    uint16_t event_subject_len;
    uint32_t request_hash;
    int32_t next_admitted;
    int32_t next_to_publish;
    int32_t final_segment_index;
    size_t refs;
    uint64_t touched_ms;
    int in_use;
    int final_admitted;
    int stream_finality_deferred;
    int completed;
    int aborted;
    int terminal_reported;
    uint8_t bucket_next;
} tts_order;

struct tts_job {
    char request_id[128];
    char event_subject[256];
    char spoken[2048];
    char voice_id[128];
    uint16_t request_id_len;
    uint16_t event_subject_len;
    uint16_t spoken_len;
    uint16_t voice_id_len;
    int spoken_json_plain;
    int32_t segment_index;
    int is_final;
    int stream_finality_deferred;
    uint32_t query_hash;
    uint64_t request_received_mono_ms;
    turn_stage_timestamps_c stages;
    size_t lane_index;
    size_t order_index;
    atomic_int canceled;
};

struct tts_lane {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    uint8_t queue[TTS_QUEUE_PER_LANE];
    size_t queue_head;
    tts_job *active;
    size_t queued;
    tts_job jobs[TTS_JOBS_PER_LANE];
    uint8_t free_stack[TTS_JOBS_PER_LANE];
    size_t free_count;
    int stop;
    int initialized;
    int thread_started;
    tts_state *owner;
    http_min_client http;
};

struct tts_state {
    vbus_client *nc;
    vbus_client *publish_nc;
    char tts_url[512];
    int timeout_ms;
    int finality_timeout_ms;
    tts_lane lanes[TTS_WORKER_LANES];
    pthread_mutex_t order_mutex;
    pthread_cond_t order_ready;
    tts_order orders[TTS_ORDER_CAPACITY];
    uint8_t order_bucket_heads[TTS_ORDER_BUCKET_COUNT];
    uint8_t order_free_stack[TTS_ORDER_CAPACITY];
    size_t order_free_count;
    int order_initialized;
    int publish_initialized;
    int stopping;
};

_Static_assert(sizeof(tts_job) <= 2720u, "TTS job storage exceeds its bound");
_Static_assert(
    TTS_ORDER_CAPACITY < TTS_ORDER_NONE,
    "TTS order index exceeds its compact representation");
_Static_assert(
    (TTS_ORDER_BUCKET_COUNT & (TTS_ORDER_BUCKET_COUNT - 1u)) == 0u,
    "TTS order bucket count must be a power of two");
_Static_assert(
    TTS_PCM_RESPONSE_MAX / 960u + 1u < 16384u,
    "bounded current PCM response can overflow its admitted sequence");
_Static_assert(
    TTS_PCM_RESPONSE_MAX / 2u + 1u < UINT32_C(0x7fffffff),
    "bounded TTS response can overflow its signed sequence");
_Static_assert(
    sizeof(((tts_job *)0)->request_id) <= UINT16_MAX &&
    sizeof(((tts_job *)0)->event_subject) <= UINT16_MAX &&
    sizeof(((tts_job *)0)->spoken) <= UINT16_MAX &&
    sizeof(((tts_job *)0)->voice_id) <= UINT16_MAX,
    "TTS job string capacity exceeds its stored length");

static int copy_c_string(char *out, size_t out_cap, const char *input) {
    const char *input_end;
    size_t input_len;
    if (!out || out_cap == 0 || !input) return -1;
    input_end = (const char *)memchr(input, '\0', out_cap);
    if (!input_end) return -1;
    input_len = (size_t)(input_end - input);
    memcpy(out, input, input_len + 1u);
    return 0;
}

static int publish_worker_event(
    tts_state *st,
    const char *subject,
    const char *request_id,
    const char *type,
    const char *text
) {
    uint8_t wire[4096];
    size_t wire_len;
    int result;
    if (!st || !st->publish_initialized || !st->publish_nc) return -1;
    wire_len = pb_encode_turn_event(
        wire, sizeof(wire), request_id, type, text ? text : "");
    if (wire_len == 0u) return -1;
    pthread_mutex_lock(&st->order_mutex);
    result = vbus_publish(st->publish_nc, subject, wire, wire_len);
    pthread_mutex_unlock(&st->order_mutex);
    return result;
}

static int event_subject_for(
    const char *request_id,
    size_t request_id_len,
    const char *response_subject,
    size_t response_subject_len,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    static const char prefix[] = SUBJ_TURN_EVENTS_PFX ".";
    const size_t prefix_len = sizeof(prefix) - 1u;
    size_t length;
    int derived;
    if (!request_id || request_id_len == 0u || !out || out_cap == 0u || !out_len)
        return -1;
    *out_len = 0u;
    if (response_subject_len != 0u) {
        length = response_subject_len;
        if (!response_subject || length >= out_cap) return -1;
        derived = length == prefix_len + request_id_len &&
            memcmp(response_subject, prefix, prefix_len) == 0 &&
            memcmp(
                response_subject + prefix_len,
                request_id,
                request_id_len) == 0;
        memcpy(out, response_subject, length);
        out[length] = '\0';
        if (!derived && !vbus_publish_subject_span_valid(out, length)) return -1;
    } else {
        length = prefix_len + request_id_len;
        if (length >= out_cap) return -1;
        memcpy(out, prefix, prefix_len);
        memcpy(out + prefix_len, request_id, request_id_len);
        out[length] = '\0';
    }
    *out_len = length;
    return 0;
}

static uint32_t request_hash_span(const char *request_id, size_t request_id_len) {
    const unsigned char *p = (const unsigned char *)request_id;
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0u; i < request_id_len; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)(now.tv_nsec / 1000000L);
}

static int64_t realtime_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) return 0;
    return (int64_t)now.tv_sec * INT64_C(1000) +
        (int64_t)now.tv_nsec / INT64_C(1000000);
}

static int64_t job_timestamp_ms(const tts_job *job) {
    uint64_t now;
    uint64_t elapsed;
    if (!job || job->stages.tts_request_received_at_ms <= 0 ||
        job->request_received_mono_ms == 0) return 0;
    now = monotonic_ms();
    if (now < job->request_received_mono_ms) return 0;
    elapsed = now - job->request_received_mono_ms;
    if (elapsed >
        (uint64_t)(INT64_MAX - job->stages.tts_request_received_at_ms)) return 0;
    return job->stages.tts_request_received_at_ms + (int64_t)elapsed;
}

static size_t order_bucket_for_hash(uint32_t request_hash) {
    return (size_t)(request_hash & (TTS_ORDER_BUCKET_COUNT - 1u));
}

/* The order mutex protects every helper in this group. */
static size_t order_find_locked(
    const tts_state *st,
    const char *request_id,
    size_t request_id_len,
    uint32_t request_hash
) {
    uint8_t cursor;
    if (!st || !request_id) return TTS_ORDER_CAPACITY;
    cursor = st->order_bucket_heads[order_bucket_for_hash(request_hash)];
    while (cursor != TTS_ORDER_NONE) {
        const tts_order *order = &st->orders[cursor];
        if (order->in_use && order->request_hash == request_hash &&
            order->request_id_len == request_id_len &&
            memcmp(order->request_id, request_id, request_id_len) == 0)
            return (size_t)cursor;
        cursor = order->bucket_next;
    }
    return TTS_ORDER_CAPACITY;
}

static int order_unlink_locked(tts_state *st, size_t order_index) {
    tts_order *order;
    uint8_t *link;
    if (!st || order_index >= TTS_ORDER_CAPACITY) return -1;
    order = &st->orders[order_index];
    if (!order->in_use) return -1;
    link = &st->order_bucket_heads[order_bucket_for_hash(order->request_hash)];
    while (*link != TTS_ORDER_NONE) {
        if ((size_t)*link == order_index) {
            *link = order->bucket_next;
            return 0;
        }
        link = &st->orders[*link].bucket_next;
    }
    return -1;
}

static size_t order_take_slot_locked(tts_state *st, uint64_t now) {
    size_t i;
    if (!st) return TTS_ORDER_CAPACITY;
    if (st->order_free_count != 0u)
        return (size_t)st->order_free_stack[--st->order_free_count];
    for (i = 0u; i < TTS_ORDER_CAPACITY; ++i) {
        tts_order *order = &st->orders[i];
        if (order->in_use && order->refs == 0u && now >= order->touched_ms &&
            now - order->touched_ms >= TTS_ORDER_STALE_MS) {
            if (order_unlink_locked(st, i) != 0) return TTS_ORDER_CAPACITY;
            memset(order, 0, sizeof(*order));
            return i;
        }
    }
    return TTS_ORDER_CAPACITY;
}

static int order_recycle_locked(tts_state *st, size_t order_index) {
    if (!st || order_index >= TTS_ORDER_CAPACITY ||
        st->order_free_count >= TTS_ORDER_CAPACITY ||
        order_unlink_locked(st, order_index) != 0) return -1;
    memset(&st->orders[order_index], 0, sizeof(st->orders[order_index]));
    st->order_free_stack[st->order_free_count++] = (uint8_t)order_index;
    return 0;
}

/* The caller holds the order mutex. Workers release their lane before they
 * acquire that mutex, so this order-to-lane lock sequence cannot cycle. */
static int order_cancel_workers_locked(
    tts_state *st,
    const char *request_id,
    size_t request_id_len
) {
    int matched = 0;
    int i;
    if (!st || !request_id || request_id_len == 0u) return 0;
    for (i = 0; i < TTS_WORKER_LANES; ++i) {
        tts_lane *lane = &st->lanes[i];
        size_t offset;
        pthread_mutex_lock(&lane->mutex);
        if (lane->active &&
            lane->active->request_id_len == request_id_len &&
            memcmp(lane->active->request_id, request_id, request_id_len) == 0) {
            atomic_store_explicit(
                &lane->active->canceled, 1, memory_order_relaxed);
            matched = 1;
        }
        for (offset = 0; offset < lane->queued; ++offset) {
            size_t position =
                (lane->queue_head + offset) % TTS_QUEUE_PER_LANE;
            tts_job *job = &lane->jobs[lane->queue[position]];
            if (job->request_id_len == request_id_len &&
                memcmp(job->request_id, request_id, request_id_len) == 0) {
                atomic_store_explicit(
                    &job->canceled, 1, memory_order_relaxed);
                matched = 1;
            }
        }
        pthread_mutex_unlock(&lane->mutex);
    }
    return matched;
}

static int order_cancel_indexed(
    tts_state *st,
    const char *request_id,
    size_t request_id_len,
    uint32_t request_hash,
    int claim_terminal,
    int *matched_out
) {
    size_t order_index;
    int present = 0;
    if (matched_out) *matched_out = 0;
    if (!st || !st->order_initialized || !request_id || request_id_len == 0u ||
        request_id_len >= sizeof(((tts_order *)0)->request_id) || !matched_out)
        return 0;
    pthread_mutex_lock(&st->order_mutex);
    order_index = order_find_locked(
        st, request_id, request_id_len, request_hash);
    if (order_index != TTS_ORDER_CAPACITY) {
        tts_order *order = &st->orders[order_index];
        present = 1;
        if (order->refs == 0u && order_recycle_locked(st, order_index) == 0) {
            pthread_mutex_unlock(&st->order_mutex);
            return present;
        }
        if (order->refs != 0u)
            *matched_out = order_cancel_workers_locked(
                st, request_id, request_id_len);
        order->aborted = 1;
        order->completed = 1;
        order->touched_ms = monotonic_ms();
        if (claim_terminal) order->terminal_reported = 1;
        pthread_cond_broadcast(&st->order_ready);
    }
    pthread_mutex_unlock(&st->order_mutex);
    return present;
}

static int order_state_start(tts_state *st) {
    size_t i;
    if (!st || pthread_mutex_init(&st->order_mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&st->order_ready, NULL) != 0) {
        pthread_mutex_destroy(&st->order_mutex);
        return -1;
    }
    memset(st->orders, 0, sizeof(st->orders));
    memset(st->order_bucket_heads, TTS_ORDER_NONE, sizeof(st->order_bucket_heads));
    for (i = 0u; i < TTS_ORDER_CAPACITY; ++i)
        st->order_free_stack[i] = (uint8_t)(TTS_ORDER_CAPACITY - 1u - i);
    st->order_free_count = TTS_ORDER_CAPACITY;
    st->order_initialized = 1;
    return 0;
}

static void order_state_stop(tts_state *st) {
    size_t i;
    if (!st || !st->order_initialized) return;
    pthread_mutex_lock(&st->order_mutex);
    st->stopping = 1;
    for (i = 0; i < TTS_ORDER_CAPACITY; ++i) {
        if (st->orders[i].in_use) {
            st->orders[i].aborted = 1;
            st->orders[i].completed = 1;
        }
    }
    pthread_cond_broadcast(&st->order_ready);
    pthread_mutex_unlock(&st->order_mutex);
}

static void order_state_destroy(tts_state *st) {
    if (!st || !st->order_initialized) return;
    pthread_cond_destroy(&st->order_ready);
    pthread_mutex_destroy(&st->order_mutex);
    st->order_initialized = 0;
}

static int order_acquire(
    tts_state *st,
    const char *request_id,
    size_t request_id_len,
    uint32_t request_hash,
    uint64_t now,
    const char *event_subject,
    size_t event_subject_len,
    int32_t segment_index,
    int is_final,
    int stream_finality_deferred,
    size_t *order_index
) {
    size_t match;
    int created = 0;

    if (!st || !st->order_initialized || !request_id || request_id_len == 0u ||
        request_id_len >= sizeof(((tts_order *)0)->request_id) ||
        request_id[request_id_len] != '\0' || now == 0u || !event_subject ||
        event_subject_len == 0u ||
        event_subject_len >= sizeof(((tts_order *)0)->event_subject) ||
        event_subject[event_subject_len] != '\0' || !order_index ||
        segment_index < 0 || segment_index == INT32_MAX) return -1;
    pthread_mutex_lock(&st->order_mutex);
    if (st->stopping) {
        pthread_mutex_unlock(&st->order_mutex);
        return -1;
    }
    match = order_find_locked(st, request_id, request_id_len, request_hash);
    if (match == TTS_ORDER_CAPACITY) {
        tts_order *order;
        if (segment_index != 0) {
            pthread_mutex_unlock(&st->order_mutex);
            return -1;
        }
        match = order_take_slot_locked(st, now);
        if (match == TTS_ORDER_CAPACITY) {
            pthread_mutex_unlock(&st->order_mutex);
            return -1;
        }
        order = &st->orders[match];
        memset(order, 0, sizeof(*order));
        memcpy(order->request_id, request_id, request_id_len + 1u);
        memcpy(order->event_subject, event_subject, event_subject_len + 1u);
        order->request_id_len = (uint16_t)request_id_len;
        order->event_subject_len = (uint16_t)event_subject_len;
        order->request_hash = request_hash;
        order->bucket_next =
            st->order_bucket_heads[order_bucket_for_hash(request_hash)];
        st->order_bucket_heads[order_bucket_for_hash(request_hash)] =
            (uint8_t)match;
        order->final_segment_index = -1;
        order->stream_finality_deferred = stream_finality_deferred != 0;
        order->in_use = 1;
        created = 1;
    }
    {
        tts_order *order = &st->orders[match];
        if (order->aborted || order->completed || order->final_admitted ||
            (!created && order->stream_finality_deferred !=
                (stream_finality_deferred != 0)) ||
            order->event_subject_len != event_subject_len ||
            memcmp(order->event_subject, event_subject, event_subject_len) != 0 ||
            order->next_admitted != segment_index) {
            pthread_mutex_unlock(&st->order_mutex);
            return -1;
        }
        order->next_admitted++;
        order->final_admitted = is_final != 0;
        if (is_final) order->final_segment_index = segment_index;
        order->refs++;
        order->touched_ms = now;
        /* A successor proves the preceding segment is non-final. Wake its
         * deferred tail without waiting for the model's final marker. */
        pthread_cond_broadcast(&st->order_ready);
    }
    *order_index = match;
    pthread_mutex_unlock(&st->order_mutex);
    return 0;
}

static int order_admit_final_marker(
    tts_state *st,
    const char *request_id,
    size_t request_id_len,
    uint32_t request_hash,
    const char *event_subject,
    size_t event_subject_len,
    int32_t marker_index,
    uint64_t now
) {
    size_t match;
    tts_order *order;
    int result = -1;
    if (!st || !st->order_initialized || !request_id || request_id_len == 0u ||
        !event_subject || event_subject_len == 0u || marker_index <= 0 ||
        marker_index == INT32_MAX || now == 0u) return -1;
    pthread_mutex_lock(&st->order_mutex);
    match = order_find_locked(st, request_id, request_id_len, request_hash);
    if (match != TTS_ORDER_CAPACITY) {
        order = &st->orders[match];
        if (!st->stopping && !order->aborted && !order->completed &&
            order->stream_finality_deferred && !order->final_admitted &&
            order->refs != 0u && order->next_admitted == marker_index &&
            order->event_subject_len == event_subject_len &&
            memcmp(order->event_subject, event_subject, event_subject_len) == 0) {
            order->next_admitted++;
            order->final_admitted = 1;
            order->final_segment_index = marker_index - 1;
            order->touched_ms = now;
            pthread_cond_broadcast(&st->order_ready);
            result = 0;
        }
    }
    pthread_mutex_unlock(&st->order_mutex);
    return result;
}

static int order_matches_job(const tts_order *order, const tts_job *job) {
    return order && job && order->in_use &&
        order->request_id_len == job->request_id_len &&
        memcmp(order->request_id, job->request_id, job->request_id_len) == 0;
}

static int order_wait_turn(tts_state *st, const tts_job *job) {
    tts_order *order;
    int result = -1;
    if (!st || !job || job->order_index >= TTS_ORDER_CAPACITY) return -1;
    pthread_mutex_lock(&st->order_mutex);
    order = &st->orders[job->order_index];
    while (order_matches_job(order, job) && !st->stopping && !order->aborted &&
           !order->completed && order->next_to_publish < job->segment_index &&
           atomic_load_explicit(&job->canceled, memory_order_relaxed) == 0)
        pthread_cond_wait(&st->order_ready, &st->order_mutex);
    if (order_matches_job(order, job) && !st->stopping && !order->aborted &&
        !order->completed && order->next_to_publish == job->segment_index &&
        atomic_load_explicit(&job->canceled, memory_order_relaxed) == 0)
        result = 0;
    pthread_mutex_unlock(&st->order_mutex);
    return result;
}

static int order_resolve_segment_finality(
    tts_state *st,
    const tts_job *job,
    int *is_final
) {
    struct timespec deadline;
    tts_order *order;
    int wait_rc = 0;
    int result = -1;
    if (!st || !job || !is_final || job->order_index >= TTS_ORDER_CAPACITY)
        return -1;
    if (!job->stream_finality_deferred) {
        *is_final = job->is_final != 0;
        return 0;
    }
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += st->finality_timeout_ms / 1000;
    deadline.tv_nsec +=
        (long)(st->finality_timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&st->order_mutex);
    order = &st->orders[job->order_index];
    while (order_matches_job(order, job) && !st->stopping && !order->aborted &&
           !order->completed &&
           atomic_load_explicit(&job->canceled, memory_order_relaxed) == 0) {
        if (order->final_admitted &&
            order->final_segment_index == job->segment_index) {
            *is_final = 1;
            result = 0;
            break;
        }
        if (order->next_admitted > job->segment_index + 1) {
            *is_final = 0;
            result = 0;
            break;
        }
        wait_rc = pthread_cond_timedwait(
            &st->order_ready, &st->order_mutex, &deadline);
        if (wait_rc == ETIMEDOUT) break;
        if (wait_rc != 0) break;
    }
    pthread_mutex_unlock(&st->order_mutex);
    return result;
}

static int order_abort_job(tts_state *st, const tts_job *job) {
    tts_order *order;
    int report = 0;
    if (!st || !job || job->order_index >= TTS_ORDER_CAPACITY) return 1;
    pthread_mutex_lock(&st->order_mutex);
    order = &st->orders[job->order_index];
    if (order_matches_job(order, job)) {
        order->aborted = 1;
        order->completed = 1;
        order->touched_ms = monotonic_ms();
        if (!order->terminal_reported) {
            order->terminal_reported = 1;
            report = 1;
        }
        pthread_cond_broadcast(&st->order_ready);
    }
    pthread_mutex_unlock(&st->order_mutex);
    return report;
}

static int order_abort_request(
    tts_state *st,
    const char *request_id,
    int claim_terminal,
    char *event_subject,
    size_t event_subject_cap
) {
    size_t request_id_len;
    size_t order_index;
    uint32_t request_hash;
    int report = claim_terminal != 0;
    if (event_subject && event_subject_cap != 0) event_subject[0] = '\0';
    if (!st || !request_id || !st->order_initialized) return report;
    request_id_len = strnlen(request_id, sizeof(((tts_order *)0)->request_id));
    if (request_id_len == 0u ||
        request_id_len >= sizeof(((tts_order *)0)->request_id)) return report;
    request_hash = request_hash_span(request_id, request_id_len);
    pthread_mutex_lock(&st->order_mutex);
    order_index = order_find_locked(
        st, request_id, request_id_len, request_hash);
    if (order_index != TTS_ORDER_CAPACITY) {
        tts_order *order = &st->orders[order_index];
        if (event_subject && event_subject_cap != 0)
            (void)copy_c_string(
                event_subject, event_subject_cap, order->event_subject);
        order->aborted = 1;
        order->completed = 1;
        if (order->refs != 0u)
            (void)order_cancel_workers_locked(
                st, request_id, request_id_len);
        order->touched_ms = monotonic_ms();
        if (claim_terminal) {
            if (order->terminal_reported) report = 0;
            else order->terminal_reported = 1;
        }
        if (order->refs == 0u)
            (void)order_recycle_locked(st, order_index);
        pthread_cond_broadcast(&st->order_ready);
    }
    pthread_mutex_unlock(&st->order_mutex);
    return report;
}

static void abort_and_publish_failure(
    tts_state *st,
    const char *request_id,
    const char *fallback_subject,
    const char *message
) {
    char trusted_subject[256];
    const char *subject;
    if (!order_abort_request(
            st, request_id, 1, trusted_subject, sizeof(trusted_subject))) return;
    subject = trusted_subject[0] ? trusted_subject : fallback_subject;
    if (vbus_publish_subject_valid(subject, sizeof(trusted_subject)))
        (void)publish_worker_event(st, subject, request_id, "failed", message);
}

static void abort_and_publish_failure_span(
    tts_state *st,
    const char *request_id,
    size_t request_id_len,
    const char *fallback_subject,
    const char *message
) {
    char owned_request_id[sizeof(((tts_job *)0)->request_id)];
    if (!request_id || request_id_len == 0u ||
        request_id_len >= sizeof(owned_request_id)) return;
    memcpy(owned_request_id, request_id, request_id_len);
    owned_request_id[request_id_len] = '\0';
    abort_and_publish_failure(st, owned_request_id, fallback_subject, message);
}

static int publish_job_pcm_started_if_current(
    tts_state *st,
    const tts_job *job,
    const turn_stage_wire_c *stage_wire
) {
    tts_order *order;
    int result = -1;
    if (!st || !job || !st->publish_initialized || !st->publish_nc ||
        job->order_index >= TTS_ORDER_CAPACITY) return -1;
    pthread_mutex_lock(&st->order_mutex);
    order = &st->orders[job->order_index];
    if (order_matches_job(order, job) && !st->stopping && !order->aborted &&
        !order->completed && order->next_to_publish == job->segment_index &&
        atomic_load_explicit(&job->canceled, memory_order_relaxed) == 0)
        {
            uint8_t wire[TURN_STAGE_WIRE_CAPACITY + sizeof(job->request_id) + 4u];
            size_t wire_len = pb_encode_turn_pcm_boundary_prepared(
                wire, sizeof(wire), job->request_id, job->request_id_len,
                7, stage_wire);
            result = wire_len ? vbus_publish_prepared(
                st->publish_nc,
                job->event_subject,
                job->event_subject_len,
                wire,
                wire_len) : -1;
        }
    pthread_mutex_unlock(&st->order_mutex);
    return result;
}

static void order_release_job(tts_state *st, const tts_job *job) {
    tts_order *order;
    if (!st || !job || job->order_index >= TTS_ORDER_CAPACITY) return;
    pthread_mutex_lock(&st->order_mutex);
    order = &st->orders[job->order_index];
    if (order_matches_job(order, job) && order->refs != 0) {
        order->refs--;
        order->touched_ms = monotonic_ms();
        if (order->refs == 0 && (order->completed || order->aborted))
            (void)order_recycle_locked(st, job->order_index);
    }
    pthread_mutex_unlock(&st->order_mutex);
}

static size_t lane_index_for_segment(uint32_t request_hash, int32_t segment_index) {
    uint32_t offset = (uint32_t)segment_index % TTS_PIPELINE_DEPTH;
    return (request_hash + offset) % TTS_WORKER_LANES;
}

static tts_lane *lane_for_segment(
    tts_state *st,
    uint32_t request_hash,
    int32_t segment_index,
    size_t *lane_index_out
) {
    size_t lane_index;
    if (!st || segment_index < 0 || !lane_index_out)
        return NULL;
    lane_index = lane_index_for_segment(request_hash, segment_index);
    *lane_index_out = lane_index;
    return &st->lanes[lane_index];
}

static tts_job *lane_acquire_job(tts_lane *lane) {
    tts_job *job = NULL;
    if (!lane || !lane->initialized) return NULL;
    pthread_mutex_lock(&lane->mutex);
    if (!lane->stop && lane->free_count != 0) {
        size_t index = lane->free_stack[--lane->free_count];
        job = &lane->jobs[index];
        /* Lane initialization and every release scrub slots before reuse. */
        atomic_init(&job->canceled, 0);
    }
    pthread_mutex_unlock(&lane->mutex);
    return job;
}

static int lane_job_index(
    const tts_lane *lane,
    const tts_job *job,
    size_t *index_out
) {
    size_t index;

    if (!lane || !job || !index_out) return -1;
    for (index = 0; index < TTS_JOBS_PER_LANE; ++index) {
        if (job == &lane->jobs[index]) {
            *index_out = index;
            return 0;
        }
    }
    return -1;
}

static void lane_release_job_locked(tts_lane *lane, tts_job *job) {
    size_t index;
    if (lane_job_index(lane, job, &index) != 0 ||
        lane->free_count >= TTS_JOBS_PER_LANE) return;
    memset(job, 0, sizeof(*job));
    lane->free_stack[lane->free_count++] = (uint8_t)index;
}

static void lane_release_job(tts_lane *lane, tts_job *job) {
    if (!lane || !job) return;
    pthread_mutex_lock(&lane->mutex);
    lane_release_job_locked(lane, job);
    pthread_mutex_unlock(&lane->mutex);
}

typedef struct {
    tts_state *owner;
    tts_job *job;
    turn_stage_wire_c stage_wire;
    turn_pcm_chunk_current_wire_c current_wire;
    uint8_t header_bytes[8];
    size_t header_have;
    pcm_header_c header;
    size_t frame_bytes;
    uint8_t frames[2][4096];
    size_t frame_have;
    size_t pending_len;
    uint8_t fill_slot;
    uint8_t pending_slot;
    int32_t sequence;
    int started;
    int current_wire_prepared;
} tts_pcm_stream;

_Static_assert(
    VBUS_FRAME_HEADER_BYTES + sizeof(((tts_job *)0)->event_subject) +
        sizeof(((tts_pcm_stream *)0)->frames[0]) +
        sizeof(((tts_job *)0)->request_id) + 16u +
        TURN_STAGE_WIRE_CAPACITY + 64u <= VBUS_MAX_FRAME_BYTES,
    "bounded TTS PCM event exceeds the admitted VBus frame");
_Static_assert(
    VBUS_FRAME_HEADER_BYTES + sizeof(((tts_job *)0)->event_subject) + 512u <=
        VBUS_MAX_FRAME_BYTES,
    "bounded TTS PCM boundary exceeds the admitted VBus frame");

/* A rejected HTTP body callback ends synchronous response delivery.
 * Mono-s16 admission also fixes sample alignment at two bytes. */

static int publish_stream_frame_data(
    tts_pcm_stream *stream,
    const uint8_t *pcm,
    size_t pcm_len,
    int is_final,
    int end_segment
) {
    uint8_t event_prefix[sizeof(stream->job->request_id) + 16u];
    uint8_t event_suffix[TURN_STAGE_WIRE_CAPACITY + 64u];
    uint8_t end_wire[512];
    vbus_publish_span event_spans[3];
    const uint8_t *event_prefix_data = event_prefix;
    size_t event_len;
    size_t event_prefix_len;
    size_t event_suffix_len;
    size_t end_len = 0u;
    tts_order *order;
    int first_frame;
    int result;
    /* Stream start proves the owner, job, publisher, order slot, and bounded
     * response sequence once. Every caller publishes only after that proof. */
    if (!stream || !stream->started || !pcm || pcm_len == 0u)
        return TTS_STREAM_ERR_INVALID;
    first_frame = stream->job->stages.pcm_first_chunk_at_ms == 0;
    if (first_frame) {
        stream->job->stages.pcm_first_chunk_at_ms = job_timestamp_ms(stream->job);
        if (stream->job->stages.pcm_first_chunk_at_ms == 0 ||
            pb_complete_turn_stage_wire(
                &stream->stage_wire,
                stream->job->stages.pcm_first_chunk_at_ms) != 0)
            return TTS_STREAM_ERR_INVALID;
    }
    if (stream->current_wire_prepared &&
        pcm_len == (size_t)stream->current_wire.audio_len) {
        /* Preparation owns every invariant field. The response bound keeps
         * sequence inside the admitted two-byte protobuf range. */
        event_len = pb_encode_turn_pcm_chunk_current_suffix_admitted(
            &stream->current_wire,
            event_suffix,
            sizeof(event_suffix),
            &event_suffix_len,
            stream->sequence,
            is_final,
            first_frame ? &stream->stage_wire : NULL
        );
        event_prefix_data = stream->current_wire.prefix;
        event_prefix_len = stream->current_wire.prefix_len;
    } else {
        event_len = pb_encode_turn_pcm_chunk_spans_prepared(
            event_prefix,
            sizeof(event_prefix),
            &event_prefix_len,
            event_suffix,
            sizeof(event_suffix),
            &event_suffix_len,
            stream->job->request_id,
            stream->job->request_id_len,
            pcm,
            pcm_len,
            (int32_t)stream->header.sample_rate,
            (int32_t)stream->header.channels,
            (int32_t)stream->header.bit_depth,
            stream->sequence,
            stream->job->segment_index,
            is_final,
            first_frame ? &stream->stage_wire : NULL
        );
    }
    if (event_len == 0) return TTS_STREAM_ERR_INVALID;
    event_spans[0].data = event_prefix_data;
    event_spans[0].len = event_prefix_len;
    event_spans[1].data = pcm;
    event_spans[1].len = pcm_len;
    event_spans[2].data = event_suffix;
    event_spans[2].len = event_suffix_len;
    if (end_segment) {
        end_len = pb_encode_turn_pcm_boundary_prepared(
            end_wire,
            sizeof(end_wire),
            stream->job->request_id,
            stream->job->request_id_len,
            9,
            NULL);
        if (end_len == 0u) return TTS_STREAM_ERR_INVALID;
    }
    pthread_mutex_lock(&stream->owner->order_mutex);
    order = &stream->owner->orders[stream->job->order_index];
    /* The admitted job holds an order reference until this worker releases
     * it. A referenced slot cannot recycle, so its request ID stays bound. */
    if (order->in_use && order->refs != 0u && !stream->owner->stopping &&
        !order->aborted && !order->completed &&
        order->next_to_publish == stream->job->segment_index &&
        atomic_load_explicit(&stream->job->canceled, memory_order_relaxed) == 0) {
        result = end_segment ?
            vbus_publish_spans3_pair_admitted(
                stream->owner->publish_nc,
                stream->job->event_subject,
                stream->job->event_subject_len,
                event_spans,
                stream->job->event_subject,
                stream->job->event_subject_len,
                end_wire,
                end_len) :
            vbus_publish_spans3_admitted(
                stream->owner->publish_nc,
                stream->job->event_subject,
                stream->job->event_subject_len,
                event_spans);
        if (result == 0 && end_segment) {
            order->next_to_publish++;
            order->touched_ms = monotonic_ms();
            if (is_final) order->completed = 1;
            pthread_cond_broadcast(&stream->owner->order_ready);
        }
    } else {
        result = -1;
    }
    pthread_mutex_unlock(&stream->owner->order_mutex);
    if (result != 0) return TTS_STREAM_ERR_PUBLISH;
    stream->sequence++;
    return 0;
}

static int publish_stream_frame(
    tts_pcm_stream *stream,
    int is_final,
    int end_segment
) {
    int result;
    if (!stream || stream->pending_len == 0u)
        return TTS_STREAM_ERR_INVALID;
    result = publish_stream_frame_data(
        stream,
        stream->frames[stream->pending_slot],
        stream->pending_len,
        is_final,
        end_segment);
    if (result != 0) return result;
    stream->pending_len = 0;
    return 0;
}

static int start_pcm_stream(tts_pcm_stream *stream, size_t available_pcm_bytes) {
    if (!stream || !stream->owner || !stream->job || stream->started ||
        stream->frame_bytes == 0 || available_pcm_bytes < 2u)
        return TTS_STREAM_ERR_INVALID;
    stream->job->stages.tts_provider_ready_at_ms =
        job_timestamp_ms(stream->job);
    if (stream->job->stages.tts_provider_ready_at_ms == 0)
        return TTS_STREAM_ERR_INVALID;
    if (order_wait_turn(stream->owner, stream->job) != 0)
        return TTS_STREAM_ERR_PUBLISH;
    stream->job->stages.pcm_started_at_ms = job_timestamp_ms(stream->job);
    if (stream->job->stages.pcm_started_at_ms == 0 ||
        pb_prepare_turn_stage_wire(
            &stream->stage_wire, &stream->job->stages) != 0 ||
        publish_job_pcm_started_if_current(
            stream->owner,
            stream->job,
            &stream->stage_wire) != 0)
        return TTS_STREAM_ERR_PUBLISH;
    stream->started = 1;
    return 0;
}

static int tts_pcm_body(const uint8_t *data, size_t len, void *user) {
    tts_pcm_stream *stream = (tts_pcm_stream *)user;
    size_t offset = 0;
    /* Request aborts set the active job flag while they hold the order lock.
     * Stream start and publication revalidate the order under that lock. */
    if (!stream || !stream->owner || !stream->job || (!data && len != 0) ||
        atomic_load_explicit(&stream->job->canceled, memory_order_relaxed) != 0)
        return -1;
    if (len == 0) return 0;

    if (stream->header_have < sizeof(stream->header_bytes)) {
        const uint8_t *header_data = stream->header_bytes;
        if (stream->header_have == 0u && len >= sizeof(stream->header_bytes)) {
            header_data = data;
            stream->header_have = sizeof(stream->header_bytes);
            offset = sizeof(stream->header_bytes);
        } else {
            size_t take = sizeof(stream->header_bytes) - stream->header_have;
            if (take > len) take = len;
            memcpy(stream->header_bytes + stream->header_have, data, take);
            stream->header_have += take;
            offset += take;
        }
        if (stream->header_have == sizeof(stream->header_bytes)) {
            if (pcm_parse_mono_s16_header_v1(
                    header_data,
                    sizeof(stream->header_bytes),
                    &stream->header) != PCM_PARSE_OK) {
                return -1;
            }
            /* Twenty milliseconds contains sample_rate / 50 complete samples. */
            stream->frame_bytes =
                ((size_t)stream->header.sample_rate / 50u) * 2u;
            if (stream->frame_bytes > sizeof(stream->frames[0])) {
                return -1;
            }
            if (stream->header.sample_rate == 24000u &&
                stream->header.channels == 1u &&
                stream->header.bit_depth == 16u) {
                if (pb_prepare_turn_pcm_chunk_current_wire(
                        &stream->current_wire,
                        stream->job->request_id,
                        stream->job->request_id_len,
                        stream->frame_bytes,
                        stream->job->segment_index) != 0) return -1;
                stream->current_wire_prepared = 1;
            }
        }
    }

    while (offset < len) {
        size_t take;
        if (stream->frame_bytes == 0) return -1;
        /* A complete successor proves this frame is not the final frame.
         * The synchronous VBus send consumes the callback span before return. */
        if (stream->frame_have == 0u &&
            len - offset >= stream->frame_bytes * 2u) {
            if (!stream->started &&
                start_pcm_stream(stream, len - offset) != 0) return -1;
            if (stream->pending_len != 0u &&
                publish_stream_frame(stream, 0, 0) != 0) return -1;
            if (publish_stream_frame_data(
                    stream,
                    data + offset,
                    stream->frame_bytes,
                    0,
                    0) != 0) return -1;
            offset += stream->frame_bytes;
            continue;
        }
        take = stream->frame_bytes - stream->frame_have;
        if (take > len - offset) take = len - offset;
        memcpy(
            stream->frames[stream->fill_slot] + stream->frame_have,
            data + offset,
            take);
        stream->frame_have += take;
        offset += take;
        if (!stream->started &&
            stream->frame_have >= 2u &&
            start_pcm_stream(stream, stream->frame_have) != 0) {
            return -1;
        }
        if (stream->frame_have == stream->frame_bytes) {
            if (stream->pending_len != 0 &&
                publish_stream_frame(stream, 0, 0) != 0) {
                return -1;
            }
            stream->pending_slot = stream->fill_slot;
            stream->pending_len = stream->frame_bytes;
            stream->fill_slot ^= 1u;
            stream->frame_have = 0;
        }
    }
    return 0;
}

static int finish_pcm_stream(tts_pcm_stream *stream) {
    int is_final;
    if (!stream || !stream->started)
        return TTS_STREAM_ERR_INVALID;
    if ((stream->frame_have % 2u) != 0)
        return TTS_STREAM_ERR_INVALID;
    if (stream->frame_have != 0) {
        if (stream->pending_len != 0) {
            int rc = publish_stream_frame(stream, 0, 0);
            if (rc != 0) return rc;
        }
        stream->pending_slot = stream->fill_slot;
        stream->pending_len = stream->frame_have;
        stream->frame_have = 0;
    }
    if (stream->pending_len == 0) return TTS_STREAM_ERR_INVALID;
    if (order_resolve_segment_finality(
            stream->owner, stream->job, &is_final) != 0)
        return TTS_STREAM_ERR_FINALITY;
    return publish_stream_frame(stream, is_final, 1);
}

static void publish_job_terminal(
    tts_state *st,
    tts_job *job,
    const char *type,
    const char *text
) {
    if (order_abort_job(st, job))
        (void)publish_worker_event(st, job->event_subject, job->request_id, type, text);
}

static void process_job(tts_lane *lane, tts_job *job) {
    tts_state *st = lane ? lane->owner : NULL;
    tts_pcm_stream stream;
    char request_json[14336];
    size_t request_len;
    size_t pcm_len = 0;
    int status = 0;
    int rc;

    if (!st || !job) return;
    if (atomic_load_explicit(&job->canceled, memory_order_relaxed) != 0) {
        publish_job_terminal(st, job, "canceled", "barge_in");
        return;
    }
    if (!st->tts_url[0]) {
        publish_job_terminal(
            st, job, "failed", "TTS_HTTP_URL is not configured");
        return;
    }
    /* Admission proves the request ID uses the JSON-plain subject alphabet.
     * Sanitization carries the separate text proof. */
    request_len = tts_pcm_request_json_prepared_spans(
        request_json,
        sizeof(request_json),
        job->spoken,
        job->spoken_len,
        job->voice_id,
        job->voice_id_len,
        job->request_id,
        job->request_id_len,
        job->query_hash,
        job->spoken_json_plain);
    if (request_len == 0) {
        publish_job_terminal(st, job, "failed", "TTS JSON encoding failed");
        return;
    }
    /* The HTTP callback fills both PCM buffers before their lengths expose
     * bytes. Initialize only independent state, not derived stream state. */
    stream.owner = st;
    stream.job = job;
    stream.header_have = 0;
    stream.frame_bytes = 0;
    stream.frame_have = 0;
    stream.pending_len = 0;
    stream.fill_slot = 0u;
    stream.pending_slot = 0u;
    stream.sequence = 0;
    stream.started = 0;
    stream.current_wire_prepared = 0;
    job->stages.tts_provider_request_started_at_ms = job_timestamp_ms(job);
    if (job->stages.tts_provider_request_started_at_ms == 0) {
        publish_job_terminal(st, job, "failed", "TTS timing clock failed");
        return;
    }
    rc = http_min_client_post_stream_headers_cancel(
        &lane->http,
        "application/json",
        NULL,
        0,
        (const uint8_t *)request_json,
        request_len,
        tts_pcm_body,
        &stream,
        TTS_PCM_RESPONSE_MAX,
        &pcm_len,
        &status,
        st->timeout_ms,
        &job->canceled
    );
    if (rc == HTTP_MIN_ERR_CANCELED ||
        atomic_load_explicit(&job->canceled, memory_order_relaxed) != 0) {
        publish_job_terminal(st, job, "canceled", "barge_in");
        return;
    }
    if (rc != HTTP_MIN_OK || status < 200 || status >= 300 || pcm_len == 0) {
        publish_job_terminal(st, job, "failed", "TTS backend request failed");
        svc_log(
            "tts-module", "backend miss request=%s status=%d rc=%d",
            job->request_id, status, rc);
        return;
    }

    rc = finish_pcm_stream(&stream);
    if (rc != 0) {
        if (atomic_load_explicit(&job->canceled, memory_order_relaxed) != 0)
            publish_job_terminal(st, job, "canceled", "barge_in");
        else if (rc == TTS_STREAM_ERR_PUBLISH)
            publish_job_terminal(st, job, "failed", "TTS segment ordering failed");
        else if (rc == TTS_STREAM_ERR_FINALITY)
            publish_job_terminal(st, job, "failed", "TTS stream finality timed out");
        else
            publish_job_terminal(st, job, "failed", "invalid or incomplete TTS PCM");
        return;
    }
}

static void *lane_worker(void *arg) {
    tts_lane *lane = (tts_lane *)arg;
    if (!lane) return NULL;
    if (lane->owner && lane->owner->tts_url[0])
        (void)http_min_client_warm(
            &lane->http, TTS_HTTP_PRECONNECT_TIMEOUT_MS);
    for (;;) {
        tts_job *job;
        pthread_mutex_lock(&lane->mutex);
        while (lane->queued == 0 && !lane->stop)
            pthread_cond_wait(&lane->ready, &lane->mutex);
        if (lane->queued == 0 && lane->stop) {
            pthread_mutex_unlock(&lane->mutex);
            break;
        }
        job = &lane->jobs[lane->queue[lane->queue_head]];
        lane->queue_head = (lane->queue_head + 1u) % TTS_QUEUE_PER_LANE;
        lane->queued--;
        lane->active = job;
        pthread_mutex_unlock(&lane->mutex);

        process_job(lane, job);

        order_release_job(lane->owner, job);

        pthread_mutex_lock(&lane->mutex);
        lane->active = NULL;
        lane_release_job_locked(lane, job);
        pthread_mutex_unlock(&lane->mutex);
    }
    return NULL;
}

static int start_workers(tts_state *st) {
    int i;
    if (!st) return -1;
    st->publish_nc = svc_connect_bus();
    if (!st->publish_nc) return -1;
    st->publish_initialized = 1;
    /* main owns one zeroed state. Job release still scrubs every reused slot. */
    for (i = 0; i < TTS_WORKER_LANES; ++i) {
        tts_lane *lane = &st->lanes[i];
        lane->owner = st;
        if (pthread_mutex_init(&lane->mutex, NULL) != 0) return -1;
        if (pthread_cond_init(&lane->ready, NULL) != 0) {
            pthread_mutex_destroy(&lane->mutex);
            return -1;
        }
        lane->initialized = 1;
        if (st->tts_url[0] &&
            http_min_client_init(&lane->http, st->tts_url) != HTTP_MIN_OK)
            return -1;
        lane->free_count = TTS_JOBS_PER_LANE;
        {
            size_t j;
            for (j = 0; j < TTS_JOBS_PER_LANE; ++j)
                lane->free_stack[j] = (uint8_t)(TTS_JOBS_PER_LANE - 1u - j);
        }
        if (pthread_create(&lane->thread, NULL, lane_worker, lane) != 0) return -1;
        lane->thread_started = 1;
    }
    return 0;
}

static void stop_workers(tts_state *st) {
    int i;
    order_state_stop(st);
    for (i = 0; i < TTS_WORKER_LANES; ++i) {
        tts_lane *lane = &st->lanes[i];
        size_t offset;
        if (!lane->initialized) continue;
        pthread_mutex_lock(&lane->mutex);
        lane->stop = 1;
        if (lane->active)
            atomic_store_explicit(&lane->active->canceled, 1, memory_order_relaxed);
        for (offset = 0; offset < lane->queued; ++offset) {
            size_t position = (lane->queue_head + offset) % TTS_QUEUE_PER_LANE;
            tts_job *job = &lane->jobs[lane->queue[position]];

            atomic_store_explicit(&job->canceled, 1, memory_order_relaxed);
        }
        pthread_cond_broadcast(&lane->ready);
        pthread_mutex_unlock(&lane->mutex);
    }
    for (i = 0; i < TTS_WORKER_LANES; ++i) {
        tts_lane *lane = &st->lanes[i];
        if (!lane->initialized) continue;
        if (lane->thread_started) pthread_join(lane->thread, NULL);
        http_min_client_destroy(&lane->http);
        pthread_cond_destroy(&lane->ready);
        pthread_mutex_destroy(&lane->mutex);
        lane->initialized = 0;
    }
    if (st->publish_nc) {
        vbus_close(st->publish_nc);
        st->publish_nc = NULL;
    }
    if (st->publish_initialized) {
        st->publish_initialized = 0;
    }
}

static int queue_job(tts_state *st, tts_job *job) {
    tts_lane *lane;
    size_t index;
    size_t position;
    int needs_signal;
    if (!st || !job || job->lane_index >= TTS_WORKER_LANES) return -1;
    lane = &st->lanes[job->lane_index];
    pthread_mutex_lock(&lane->mutex);
    if (lane->stop || lane->queued >= TTS_QUEUE_PER_LANE ||
        lane_job_index(lane, job, &index) != 0) {
        pthread_mutex_unlock(&lane->mutex);
        return -1;
    }
    needs_signal = lane->queued == 0;
    position = (lane->queue_head + lane->queued) % TTS_QUEUE_PER_LANE;
    lane->queue[position] = (uint8_t)index;
    lane->queued++;
    pthread_mutex_unlock(&lane->mutex);
    if (needs_signal) pthread_cond_signal(&lane->ready);
    return 0;
}

static void on_speak(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    tts_state *st = (tts_state *)user;
    turn_tts_segment_view_c req;
    tts_job *job;
    tts_lane *lane;
    char event_subject[256];
    size_t event_subject_len = 0u;
    size_t lane_index = 0u;
    size_t spoken_len = 0;
    uint32_t request_hash;
    int64_t request_received_at_ms;
    uint64_t request_received_mono_ms;
    (void)subject;
    (void)reply;

    if (pb_decode_turn_tts_segment_view_admitted(
            data, data_len, &req, &request_hash) != 0) {
        svc_log("tts-module", "reject malformed speak request");
        return;
    }
    if (event_subject_for(
            req.request_id,
            req.request_id_len,
            req.response_subject,
            req.response_subject_len,
            event_subject,
            sizeof(event_subject),
            &event_subject_len) != 0) {
        abort_and_publish_failure_span(
            st,
            req.request_id,
            req.request_id_len,
            NULL,
            "invalid TTS response subject");
        svc_log(
            "tts-module",
            "reject invalid response subject request=%.*s",
            (int)req.request_id_len,
            req.request_id);
        return;
    }
    request_received_at_ms = realtime_ms();
    if (req.segment_emitted_at_ms > request_received_at_ms &&
        req.segment_emitted_at_ms - request_received_at_ms >
            TTS_STAGE_FUTURE_SKEW_MS) {
        abort_and_publish_failure_span(
            st,
            req.request_id,
            req.request_id_len,
            event_subject,
            "invalid future TTS stage timestamp");
        svc_log(
            "tts-module",
            "reject future TTS stage request=%.*s",
            (int)req.request_id_len,
            req.request_id);
        return;
    }
    if (req.segment_emitted_at_ms > request_received_at_ms)
        request_received_at_ms = req.segment_emitted_at_ms;
    if (req.segment_index < 0 || req.segment_index == INT32_MAX) {
        abort_and_publish_failure_span(
            st,
            req.request_id,
            req.request_id_len,
            event_subject,
            "invalid TTS segment index");
        return;
    }
    request_received_mono_ms = monotonic_ms();
    if (req.text_len == 0u) {
        if (order_admit_final_marker(
                st,
                req.request_id,
                req.request_id_len,
                request_hash,
                event_subject,
                event_subject_len,
                req.segment_index,
                request_received_mono_ms) != 0) {
            abort_and_publish_failure_span(
                st,
                req.request_id,
                req.request_id_len,
                event_subject,
                "invalid TTS final marker");
            svc_log(
                "tts-module",
                "reject final marker request=%.*s index=%d",
                (int)req.request_id_len,
                req.request_id,
                req.segment_index);
        }
        return;
    }
    lane = lane_for_segment(
        st,
        request_hash,
        req.segment_index,
        &lane_index);
    job = lane_acquire_job(lane);
    if (!job) {
        abort_and_publish_failure_span(
            st,
            req.request_id,
            req.request_id_len,
            event_subject,
            "TTS job capacity exhausted");
        return;
    }
    memcpy(job->request_id, req.request_id, req.request_id_len);
    job->request_id[req.request_id_len] = '\0';
    memcpy(job->event_subject, event_subject, event_subject_len + 1u);
    job->request_id_len = (uint16_t)req.request_id_len;
    job->event_subject_len = (uint16_t)event_subject_len;
    job->stages.input = req.input_stages;
    job->stages.first_text_at_ms = req.first_text_at_ms;
    job->stages.tts_segment_emitted_at_ms = req.segment_emitted_at_ms;
    job->stages.tts_request_received_at_ms = request_received_at_ms;
    job->request_received_mono_ms = request_received_mono_ms;
    if (job->stages.tts_request_received_at_ms == 0 ||
        job->request_received_mono_ms == 0) {
        abort_and_publish_failure(
            st, job->request_id, job->event_subject, "TTS timing clock failed");
        lane_release_job(lane, job);
        return;
    }
    if (speech_sanitize_json_plain_v1(
            req.text, req.text_len, 1,
            job->spoken, sizeof(job->spoken), &spoken_len,
            &job->spoken_json_plain) !=
            SPEECH_SANITIZE_OK || spoken_len == 0 || spoken_len >= sizeof(job->spoken)) {
        abort_and_publish_failure(
            st, job->request_id, job->event_subject, "invalid TTS request");
        lane_release_job(lane, job);
        return;
    }
    job->spoken[spoken_len] = '\0';
    job->spoken_len = (uint16_t)spoken_len;
    job->segment_index = req.segment_index;
    job->is_final = req.is_final;
    job->stream_finality_deferred = req.stream_finality_deferred;
    job->query_hash = req.query_hash;
    job->lane_index = lane_index;
    if (req.voice_id_len >= sizeof(job->voice_id)) {
        abort_and_publish_failure(
            st, job->request_id, job->event_subject, "invalid TTS voice identifier");
        lane_release_job(lane, job);
        return;
    }
    if (req.voice_id_len != 0u)
        memcpy(job->voice_id, req.voice_id, req.voice_id_len);
    job->voice_id[req.voice_id_len] = '\0';
    job->voice_id_len = (uint16_t)req.voice_id_len;
    if (order_acquire(
            st,
            job->request_id,
            job->request_id_len,
            request_hash,
            request_received_mono_ms,
            job->event_subject,
            job->event_subject_len,
            job->segment_index,
            job->is_final,
            job->stream_finality_deferred,
            &job->order_index) != 0) {
        abort_and_publish_failure(
            st, job->request_id, job->event_subject, "invalid TTS segment order");
        lane_release_job(lane, job);
        return;
    }
    if (queue_job(st, job) != 0) {
        publish_job_terminal(st, job, "failed", "TTS queue capacity exhausted");
        order_release_job(st, job);
        lane_release_job(lane, job);
    }
}

static void on_cancel(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    tts_state *st = (tts_state *)user;
    turn_cancel_c cancel;
    int matched = 0;
    int upstream_failure;
    size_t request_id_len;
    uint32_t request_hash;
    (void)subject;
    (void)reply;
    if (pb_decode_turn_cancel(data, data_len, &cancel) != 0) {
        svc_log("tts-module", "reject malformed cancel");
        return;
    }
    upstream_failure =
        strcmp(cancel.reason, TURN_CANCEL_REASON_UPSTREAM_FAILURE) == 0;
    request_id_len = strnlen(
        cancel.request_id, sizeof(cancel.request_id));
    request_hash = request_hash_span(cancel.request_id, request_id_len);
    /* VBus serializes speak and cancel callbacks. An absent or idle order
     * cannot gain a worker reference during this callback. */
    (void)order_cancel_indexed(
        st,
        cancel.request_id,
        request_id_len,
        request_hash,
        upstream_failure,
        &matched);
    svc_log("tts-module", "cancel request=%s matched=%d", cancel.request_id, matched);
}

int main(void) {
    vbus_stop_flag stop = 0;
    tts_state *st;
    const char *queue;
    int run_rc;
    st = (tts_state *)calloc(1, sizeof(*st));
    if (!st) {
        svc_log("tts-module", "state allocation failed");
        return 1;
    }
    svc_install_signals(&stop);
    {
        const char *url = svc_env("TTS_HTTP_URL", "");
        if (copy_c_string(st->tts_url, sizeof(st->tts_url), url) != 0) {
            svc_log("tts-module", "invalid TTS backend URL");
            free(st);
            return 1;
        }
        st->timeout_ms = svc_env_int_range(
            "TTS_HTTP_TIMEOUT_MS", 60000, 1, 600000);
        st->finality_timeout_ms = svc_env_int_range(
            "TTS_STREAM_FINALITY_TIMEOUT_MS",
            TTS_FINALITY_TIMEOUT_MS_DEFAULT,
            1,
            TTS_FINALITY_TIMEOUT_MS_MAX);
    }
    st->nc = svc_connect_bus();
    if (!st->nc) {
        free(st);
        return 1;
    }
    if (order_state_start(st) != 0 || start_workers(st) != 0) {
        stop_workers(st);
        order_state_destroy(st);
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    queue = svc_env("VBUS_QUEUE_GROUP", "tts-modules");
    if (vbus_subscribe(st->nc, SUBJ_TURN_TTS_SPEAK, queue, on_speak, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_TURN_CANCEL, NULL, on_cancel, st) != 0) {
        svc_log("tts-module", "subscribe failed");
        stop_workers(st);
        order_state_destroy(st);
        vbus_close(st->nc);
        free(st);
        return 1;
    }
    svc_log("tts-module", "pure-C pipelined ordered workers (canonical events + cancelable HTTP)");
    run_rc = vbus_run(st->nc, &stop);
    stop_workers(st);
    order_state_destroy(st);
    vbus_close(st->nc);
    free(st);
    return run_rc == 0 ? 0 : 1;
}
