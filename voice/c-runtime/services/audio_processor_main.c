/*
 * Pure-C audio-processor service.
 * Subscribes: ai.voice.stream.>
 * Product math: audio_engine_process on chunk PCM; no Go runtime.
 */
#define _POSIX_C_SOURCE 200809L

#include "../common/http_min.h"
#include "../common/openai_min.h"
#include "../common/service.h"
#include "../common/loop_scene.h"
#include "../common/stt_vocabulary.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"

#include "audio_engine_internal.h"
#include "pcm_condition.h"
#include "telemetry.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SESSIONS 256
#define AP_MAX_UTTERANCE_BYTES (4u * 1024u * 1024u)
#define AP_MAX_CHUNK_BYTES (64u * 1024u)
#define AP_STT_QUEUE_CAP 16u
#define AP_GENERATION_CAP 512u
#define AP_STT_RESPONSE_CAP (64u * 1024u)
#define AP_STT_CANONICAL_PATH "/v1/internal/transcribe_pcm_s16le"
#define AP_DEFAULT_STREAM_IDLE_TIMEOUT_MS 30000
#define AP_MAX_STREAM_IDLE_TIMEOUT_MS 300000
#define AP_DEFAULT_STORE_MAX_BYTES (32u * 1024u * 1024u)
#define AP_MIN_STORE_MAX_BYTES 4096u
#define AP_MAX_STORE_MAX_BYTES (64u * 1024u * 1024u)
#define AP_TELEMETRY_RING_CAPACITY 1024u
#define AP_SESSION_INDEX_CAPACITY (MAX_SESSIONS * 2u)
#define AP_GENERATION_INDEX_CAPACITY (AP_GENERATION_CAP * 2u)
#define AP_TRANSCRIPTION_SUBJECT_CAPACITY 256u
#define AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES \
    (sizeof(SUBJ_VOICE_TRANSCRIPTION_PFX ".") - 1u)
#define AP_PCM16_MONO_16KHZ_BYTES_PER_MS 32u
#define AP_DEFAULT_STT_SPEECH_CONTEXT_MS 200
#define AP_MAX_STT_SPEECH_CONTEXT_MS 1000

_Static_assert(
    (AP_SESSION_INDEX_CAPACITY & (AP_SESSION_INDEX_CAPACITY - 1u)) == 0u,
    "audio session index capacity must be a power of two");
_Static_assert(
    MAX_SESSIONS < UINT16_MAX,
    "audio session links must fit one-based slot indexes");
_Static_assert(
    (AP_GENERATION_INDEX_CAPACITY & (AP_GENERATION_INDEX_CAPACITY - 1u)) == 0u,
    "audio generation index capacity must be a power of two");
_Static_assert(
    AP_GENERATION_CAP < UINT16_MAX,
    "audio generation links must fit one-based slot indexes");

typedef struct {
    char session_id[128];
    audio_engine_session engine;
    uint64_t generation;
    uint64_t expires_ns;
    uint16_t index_next;
    int in_use;
} ap_session;

_Static_assert(
    sizeof(ap_session) <= 512u,
    "embedded audio session must remain within its fixed slot budget");

typedef struct {
    char session_id[128];
    uint8_t *pcm;
    size_t pcm_len;
    size_t pcm_capacity;
    uint64_t generation;
    turn_input_stage_timestamps_c input_stages;
    char user_id[128];
} ap_stt_job;

typedef struct {
    char session_id[128];
    uint64_t generation;
    uint16_t index_next;
    int in_use;
} ap_generation;

typedef struct {
    const char *text;
    size_t length;
    uint64_t hash;
} ap_session_key;

_Static_assert(
    AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES +
        sizeof(((ap_session *)0)->session_id) <=
        AP_TRANSCRIPTION_SUBJECT_CAPACITY,
    "maximum audio session must fit its transcription subject");

typedef struct {
    vbus_client *nc;
    vbus_client *worker_nc;
    ap_session sessions[MAX_SESSIONS];
    char session_owners[MAX_SESSIONS][128];
    uint16_t session_buckets[AP_SESSION_INDEX_CAPACITY];
    uint16_t recycled_session_slots[MAX_SESSIONS];
    size_t session_pool_high_water;
    size_t recycled_session_count;
    pthread_t worker;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    ap_stt_job jobs[AP_STT_QUEUE_CAP];
    size_t job_head;
    size_t job_count;
    ap_generation generations[AP_GENERATION_CAP];
    uint16_t generation_buckets[AP_GENERATION_INDEX_CAPACITY];
    uint16_t recycled_generation_slots[AP_GENERATION_CAP];
    size_t generation_pool_high_water;
    size_t recycled_generation_count;
    uint64_t next_generation;
    int shutting_down;
    int worker_active;
    char worker_session_id[128];
    uint64_t worker_generation;
    atomic_int http_cancel;
    http_min_client stt_http;
    char stt_url[768];
    char stt_language[16];
    int stt_timeout_ms;
    int stt_speech_context_ms;
    int stt_vocabulary_evaluation;
    uint64_t stream_idle_ns;
    uint64_t next_session_expiry_ns;
    size_t store_max_bytes;
    atomic_size_t store_bytes;
} ap_state;

static void audio_store_release(ap_state *st, size_t capacity) {
    size_t used;
    if (!st || capacity == 0) return;
    used = atomic_load_explicit(&st->store_bytes, memory_order_relaxed);
    for (;;) {
        size_t next = capacity < used ? used - capacity : 0u;
        if (atomic_compare_exchange_weak_explicit(
                &st->store_bytes,
                &used,
                next,
                memory_order_relaxed,
                memory_order_relaxed)) return;
    }
}

static size_t audio_store_session_limit(const ap_state *st, size_t current_capacity) {
    size_t used;
    size_t available;
    if (!st) return current_capacity;
    if (current_capacity >= AP_MAX_UTTERANCE_BYTES)
        return AP_MAX_UTTERANCE_BYTES;
    used = atomic_load_explicit(&st->store_bytes, memory_order_relaxed);
    available = used < st->store_max_bytes ? st->store_max_bytes - used : 0u;
    if (available > AP_MAX_UTTERANCE_BYTES - current_capacity)
        return AP_MAX_UTTERANCE_BYTES;
    return current_capacity + available;
}

static int session_key_init(ap_session_key *key, const char *sid) {
    const unsigned char *cursor;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t length = 0;
    if (!key || !sid) return -1;
    cursor = (const unsigned char *)sid;
    while (*cursor) {
        if (length >= sizeof(((ap_session *)0)->session_id) - 1u) return -1;
        hash ^= *cursor++;
        hash *= UINT64_C(1099511628211);
        length++;
    }
    if (length == 0u) return -1;
    key->text = sid;
    key->length = length;
    key->hash = hash;
    return 0;
}

static size_t transcription_subject_write(
    char *out,
    size_t out_cap,
    const ap_session_key *key
) {
    static const char prefix[] = SUBJ_VOICE_TRANSCRIPTION_PFX ".";
    size_t subject_len;
    if (!out || out_cap == 0u || !key || !key->text || key->length == 0u ||
        AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES >= out_cap ||
        key->length >= out_cap - AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES)
        return 0u;
    subject_len = AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES + key->length;
    memcpy(out, prefix, AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES);
    memcpy(
        out + AP_TRANSCRIPTION_SUBJECT_PREFIX_BYTES,
        key->text,
        key->length);
    out[subject_len] = '\0';
    return subject_len;
}

static size_t generation_bucket(uint64_t hash) {
    return (size_t)hash & (AP_GENERATION_INDEX_CAPACITY - 1u);
}

static ap_generation *generation_find_locked(
    ap_state *st,
    const ap_session_key *key,
    size_t *slot_out,
    uint16_t **link_out
) {
    uint16_t *link;
    if (slot_out) *slot_out = AP_GENERATION_CAP;
    if (link_out) *link_out = NULL;
    if (!st || !key) return NULL;
    link = &st->generation_buckets[generation_bucket(key->hash)];
    while (*link != 0u) {
        size_t slot = (size_t)*link - 1u;
        ap_generation *entry;
        if (slot >= st->generation_pool_high_water) return NULL;
        entry = &st->generations[slot];
        if (!entry->in_use) return NULL;
        if (strcmp(entry->session_id, key->text) == 0) {
            if (slot_out) *slot_out = slot;
            if (link_out) *link_out = link;
            return entry;
        }
        link = &entry->index_next;
    }
    if (link_out) *link_out = link;
    return NULL;
}

static size_t generation_slot_acquire_locked(ap_state *st) {
    size_t slot;
    if (st->recycled_generation_count != 0u)
        slot = st->recycled_generation_slots[--st->recycled_generation_count];
    else {
        if (st->generation_pool_high_water >= AP_GENERATION_CAP)
            return AP_GENERATION_CAP;
        slot = st->generation_pool_high_water++;
    }
    return slot;
}

static void generation_slot_release_locked(ap_state *st, size_t slot) {
    if (slot >= st->generation_pool_high_water) return;
    memset(&st->generations[slot], 0, sizeof(st->generations[slot]));
    if (st->recycled_generation_count >= AP_GENERATION_CAP) return;
    st->recycled_generation_slots[st->recycled_generation_count++] =
        (uint16_t)slot;
}

static int generation_current_locked(
    ap_state *st,
    const ap_session_key *key,
    uint64_t generation
) {
    ap_generation *entry = generation_find_locked(st, key, NULL, NULL);
    return entry && entry->generation == generation;
}

static void generation_remove_locked(
    ap_state *st,
    const ap_session_key *key,
    uint64_t generation,
    int match_generation
) {
    size_t slot;
    uint16_t *link;
    ap_generation *entry = generation_find_locked(st, key, &slot, &link);
    if (!entry || !link ||
        (match_generation && entry->generation != generation)) return;
    *link = entry->index_next;
    generation_slot_release_locked(st, slot);
}

static uint64_t generation_begin(
    ap_state *st,
    const ap_session_key *key
) {
    size_t slot;
    uint16_t *link;
    ap_generation *entry;
    uint64_t generation = 0;
    pthread_mutex_lock(&st->mu);
    entry = generation_find_locked(st, key, &slot, &link);
    if (st->worker_active && strcmp(st->worker_session_id, key->text) == 0)
        atomic_store_explicit(&st->http_cancel, 1, memory_order_relaxed);
    if (!entry && link) {
        slot = generation_slot_acquire_locked(st);
        if (slot < AP_GENERATION_CAP) {
            entry = &st->generations[slot];
            memcpy(entry->session_id, key->text, key->length + 1u);
            entry->in_use = 1;
            *link = (uint16_t)(slot + 1u);
        }
    }
    if (entry) {
        st->next_generation++;
        if (st->next_generation == 0) st->next_generation++;
        generation = st->next_generation;
        entry->generation = generation;
    }
    pthread_mutex_unlock(&st->mu);
    return generation;
}

static void generation_invalidate(
    ap_state *st,
    const ap_session_key *key,
    uint64_t generation
) {
    pthread_mutex_lock(&st->mu);
    if (st->worker_active && st->worker_generation == generation &&
        strcmp(st->worker_session_id, key->text) == 0)
        atomic_store_explicit(&st->http_cancel, 1, memory_order_relaxed);
    generation_remove_locked(st, key, generation, 1);
    pthread_mutex_unlock(&st->mu);
}

static void generation_invalidate_session(
    ap_state *st,
    const ap_session_key *key
) {
    pthread_mutex_lock(&st->mu);
    if (st->worker_active && strcmp(st->worker_session_id, key->text) == 0)
        atomic_store_explicit(&st->http_cancel, 1, memory_order_relaxed);
    generation_remove_locked(st, key, 0u, 0);
    pthread_mutex_unlock(&st->mu);
}

static int valid_language(const char *language) {
    size_t i;
    size_t n;
    if (!language) return 0;
    n = strlen(language);
    if (n < 2 || n > 8) return 0;
    for (i = 0; i < n; ++i) {
        if (language[i] < 'a' || language[i] > 'z') return 0;
    }
    return 1;
}

static int build_stt_url(const char *base, char *out, size_t out_cap) {
    http_min_url parsed;
    size_t base_len;
    int n;
    if (!base || !out || out_cap == 0 || http_min_parse_url(base, &parsed) != HTTP_MIN_OK)
        return -1;
    if (strcmp(parsed.path, AP_STT_CANONICAL_PATH) == 0) {
        n = snprintf(out, out_cap, "%s", base);
        return n > 0 && (size_t)n < out_cap ? 0 : -1;
    }
    if (strcmp(parsed.path, "/") != 0) return -1;
    base_len = strlen(base);
    while (base_len > 7 && base[base_len - 1] == '/') base_len--;
    n = snprintf(out, out_cap, "%.*s%s", (int)base_len, base, AP_STT_CANONICAL_PATH);
    return n > 0 && (size_t)n < out_cap ? 0 : -1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int64_t realtime_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static uint64_t session_deadline(const ap_state *st, uint64_t now_ns) {
    if (!st || now_ns == 0 || st->stream_idle_ns == 0) return 0;
    if (now_ns > UINT64_MAX - st->stream_idle_ns) return UINT64_MAX;
    return now_ns + st->stream_idle_ns;
}

static void session_touch(ap_state *st, ap_session *session, uint64_t now_ns) {
    uint64_t deadline;
    if (!st || !session) return;
    deadline = session_deadline(st, now_ns);
    session->expires_ns = deadline;
    if (deadline != 0u &&
        (st->next_session_expiry_ns == 0u ||
         deadline < st->next_session_expiry_ns))
        st->next_session_expiry_ns = deadline;
}

static size_t session_bucket(uint64_t hash) {
    return (size_t)hash & (AP_SESSION_INDEX_CAPACITY - 1u);
}

static size_t session_slot_acquire(ap_state *st) {
    size_t slot;
    if (st->recycled_session_count != 0u)
        slot = st->recycled_session_slots[--st->recycled_session_count];
    else {
        if (st->session_pool_high_water >= MAX_SESSIONS) return MAX_SESSIONS;
        slot = st->session_pool_high_water++;
    }
    return slot;
}

static void session_slot_release(ap_state *st, size_t slot) {
    if (slot >= st->session_pool_high_water) return;
    memset(&st->sessions[slot], 0, sizeof(st->sessions[slot]));
    if (st->recycled_session_count >= MAX_SESSIONS) return;
    st->recycled_session_slots[st->recycled_session_count++] = (uint16_t)slot;
}

static ap_session *session_get(
    ap_state *st,
    const ap_session_key *key,
    int create,
    uint64_t touch_ns
) {
    size_t bucket;
    size_t slot;
    uint16_t link;
    if (!st || !key) return NULL;
    bucket = session_bucket(key->hash);
    link = st->session_buckets[bucket];
    while (link != 0u) {
        ap_session *session;
        slot = (size_t)link - 1u;
        if (slot >= st->session_pool_high_water) return NULL;
        session = &st->sessions[slot];
        if (session->in_use &&
            strcmp(session->session_id, key->text) == 0) {
            if (touch_ns != 0u) session_touch(st, session, touch_ns);
            return session;
        }
        link = session->index_next;
    }
    if (!create) return NULL;
    slot = session_slot_acquire(st);
    if (slot == MAX_SESSIONS) return NULL;
    memset(st->session_owners[slot], 0, sizeof(st->session_owners[slot]));
    memcpy(
        st->sessions[slot].session_id,
        key->text,
        key->length + 1u);
    if (!audio_engine_session_init_zeroed(
            &st->sessions[slot].engine,
            0.01f, 0.985, 1, 1, 0.0025, 0.2, 1.75, 0.12)) {
        session_slot_release(st, slot);
        return NULL;
    }
    st->sessions[slot].generation = generation_begin(st, key);
    if (st->sessions[slot].generation == 0) {
        audio_engine_session_deinit(&st->sessions[slot].engine);
        session_slot_release(st, slot);
        return NULL;
    }
    st->sessions[slot].index_next = st->session_buckets[bucket];
    st->sessions[slot].in_use = 1;
    st->session_buckets[bucket] = (uint16_t)(slot + 1u);
    if (touch_ns != 0u) session_touch(st, &st->sessions[slot], touch_ns);
    return &st->sessions[slot];
}

static void session_drop(
    ap_state *st,
    const ap_session_key *key,
    int invalidate_generation
) {
    size_t bucket;
    const char *sid;
    uint16_t *link;
    if (!st || !key) return;
    bucket = session_bucket(key->hash);
    sid = key->text;
    link = &st->session_buckets[bucket];
    while (*link != 0u) {
        size_t slot = (size_t)*link - 1u;
        ap_session *session;
        if (slot >= st->session_pool_high_water) return;
        session = &st->sessions[slot];
        if (session->in_use && strcmp(session->session_id, sid) == 0) {
            uint64_t generation = session->generation;
            *link = session->index_next;
            size_t capacity =
                audio_engine_utterance_capacity(&session->engine);
            audio_engine_session_deinit(&session->engine);
            audio_store_release(st, capacity);
            memset(st->session_owners[slot], 0, sizeof(st->session_owners[slot]));
            session_slot_release(st, slot);
            if (invalidate_generation)
                generation_invalidate(st, key, generation);
            return;
        }
        link = &session->index_next;
    }
}

static int publish_lifecycle_to(
    vbus_client *client,
    const ap_session_key *key,
    int type_id
) {
    uint8_t buf[512];
    char subject[AP_TRANSCRIPTION_SUBJECT_CAPACITY];
    size_t n;
    size_t subject_len;
    int64_t ts = realtime_ms();
    if (!client || !key) return -1;
    n = pb_encode_stt_lifecycle_prepared(
        buf, sizeof(buf), key->text, key->length, type_id, ts);
    subject_len = transcription_subject_write(subject, sizeof(subject), key);
    if (n == 0 || subject_len == 0u) return -1;
    return vbus_publish_prepared(client, subject, subject_len, buf, n);
}

static void publish_lifecycle(
    ap_state *st,
    const ap_session_key *key,
    int type_id
) {
    if (!st || !key || publish_lifecycle_to(st->nc, key, type_id) != 0)
        svc_log(
            "audio-processor",
            "lifecycle publish failed session=%s",
            key ? key->text : "unknown");
}

static void session_expire(ap_state *st, uint64_t now_ns) {
    size_t i;
    if (!st || now_ns == 0u || st->next_session_expiry_ns == 0u ||
        st->next_session_expiry_ns > now_ns)
        return;
    st->next_session_expiry_ns = 0u;
    for (i = 0; i < st->session_pool_high_water; ++i) {
        ap_session *session = &st->sessions[i];
        ap_session_key key;
        char sid[sizeof(session->session_id)];
        if (!session->in_use || session->expires_ns == 0u) continue;
        if (session->expires_ns > now_ns) {
            if (st->next_session_expiry_ns == 0u ||
                session->expires_ns < st->next_session_expiry_ns)
                st->next_session_expiry_ns = session->expires_ns;
            continue;
        }
        memcpy(sid, session->session_id, sizeof(sid));
        if (session_key_init(&key, sid) != 0) continue;
        publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
        svc_log_join_n(
            "audio-processor",
            "expire idle stream session=",
            sizeof("expire idle stream session=") - 1u,
            sid,
            key.length);
        session_drop(st, &key, 1);
    }
}

static void publish_interrupt(ap_state *st, const ap_session_key *key) {
    uint8_t buf[512];
    char subject[AP_TRANSCRIPTION_SUBJECT_CAPACITY];
    size_t n;
    size_t subject_len;
    if (!st || !key) return;
    n = pb_encode_stt_interrupt(
        buf, sizeof(buf), key->text, realtime_ms());
    subject_len = transcription_subject_write(subject, sizeof(subject), key);
    if (n == 0 || subject_len == 0u) return;
    if (vbus_publish_prepared(st->nc, subject, subject_len, buf, n) != 0)
        svc_log(
            "audio-processor",
            "interrupt publish failed session=%s",
            key->text);
}

static int enqueue_stt(
    ap_state *st,
    const ap_session_key *key,
    uint64_t generation,
    uint8_t *pcm,
    size_t pcm_len,
    size_t pcm_capacity,
    const turn_input_stage_timestamps_c *input_stages,
    const char *user_id
) {
    size_t index;
    int queued = 0;
    if (!st || !key) return 0;
    pthread_mutex_lock(&st->mu);
    if (input_stages && !st->shutting_down && st->job_count < AP_STT_QUEUE_CAP &&
        generation_current_locked(st, key, generation)) {
        index = (st->job_head + st->job_count) % AP_STT_QUEUE_CAP;
        memset(&st->jobs[index], 0, sizeof(st->jobs[index]));
        memcpy(
            st->jobs[index].session_id,
            key->text,
            key->length + 1u);
        st->jobs[index].generation = generation;
        st->jobs[index].pcm = pcm;
        st->jobs[index].pcm_len = pcm_len;
        st->jobs[index].pcm_capacity = pcm_capacity;
        st->jobs[index].input_stages = *input_stages;
        if (user_id && st->stt_vocabulary_evaluation) {
            size_t n = strlen(user_id);
            if (n < sizeof(st->jobs[index].user_id))
                memcpy(st->jobs[index].user_id, user_id, n + 1u);
        }
        st->job_count++;
        queued = 1;
        pthread_cond_signal(&st->cond);
    }
    pthread_mutex_unlock(&st->mu);
    return queued;
}

static void trim_transcript(char *text, size_t *text_len) {
    size_t begin = 0;
    size_t end;
    if (!text || !text_len) return;
    end = *text_len;
    while (begin < end &&
           (text[begin] == ' ' || text[begin] == '\t' ||
            text[begin] == '\r' || text[begin] == '\n')) begin++;
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r' || text[end - 1] == '\n')) end--;
    if (begin != 0 && end > begin) memmove(text, text + begin, end - begin);
    *text_len = end - begin;
    text[*text_len] = '\0';
}

static int publish_transcription_locked(
    ap_state *st,
    const ap_stt_job *job,
    const ap_session_key *key,
    const char *text
) {
    stt_transcription_c transcript;
    uint8_t wire[4096];
    char subject[AP_TRANSCRIPTION_SUBJECT_CAPACITY];
    size_t wire_len;
    size_t subject_len;
    memset(&transcript, 0, sizeof(transcript));
    if (!key || strlen(text) >= sizeof(transcript.transcript)) return -1;
    memcpy(transcript.session_id, key->text, key->length + 1u);
    memcpy(transcript.transcript, text, strlen(text) + 1);
    memcpy(transcript.state, "final", sizeof("final"));
    transcript.sequence = 1;
    transcript.is_final = 1;
    transcript.timestamp_ms = realtime_ms();
    transcript.has_voice_activity = 1;
    transcript.commit_for_turn = 1;
    transcript.stream_state = 2;
    transcript.input_stages = job->input_stages;
    transcript.input_stages.stt_transcript_published_at_ms =
        transcript.timestamp_ms;
    wire_len = pb_encode_stt_transcription(wire, sizeof(wire), &transcript);
    subject_len = transcription_subject_write(subject, sizeof(subject), key);
    if (wire_len == 0 || subject_len == 0u) return -1;
    return vbus_publish_prepared(
        st->worker_nc,
        subject,
        subject_len,
        wire,
        wire_len);
}

static void generation_finish_locked(
    ap_state *st,
    const ap_session_key *key,
    uint64_t generation
) {
    generation_remove_locked(st, key, generation, 1);
}

static int read_stt_vocabulary(ap_state *st, const ap_stt_job *job,
    char *out, size_t capacity) {
    session_get_request_c request = {0};
    session_history_response_c history;
    uint8_t wire[512], reply[SESSION_HISTORY_MAX * 4608u + 256u];
    size_t n, reply_len = 0;
    out[0] = '\0';
    if (!st->stt_vocabulary_evaluation || !job->user_id[0]) return 0;
    memcpy(request.session_id, job->session_id, strlen(job->session_id) + 1u);
    memcpy(request.user_id, job->user_id, strlen(job->user_id) + 1u);
    request.last_n = (int32_t)SESSION_HISTORY_MAX;
    n = pb_encode_session_get_request(wire, sizeof(wire), &request);
    if (!n || vbus_request_cancel(st->worker_nc, SUBJ_SESSION_GET,
            wire, n, reply, sizeof(reply), &reply_len, 100, &st->http_cancel) ||
        pb_decode_session_history_response(reply, reply_len, &history) ||
        strcmp(history.session_id, job->session_id)) return 0;
    return loop_scene_vocabulary(&history, out, capacity) == 1;
}

static void *stt_worker_main(void *arg) {
    ap_state *st = (ap_state *)arg;
    static const char sample_rate[] = "16000";
    static const char pcm_format[] = "pcm_s16le";
    http_min_header headers[4];
    headers[0] = (http_min_header){"X-Sample-Rate", sample_rate};
    headers[1] = (http_min_header){"X-Language", st->stt_language};
    headers[2] = (http_min_header){"X-PCM-Format", pcm_format};

    for (;;) {
        ap_stt_job job;
        ap_session_key job_key;
        uint8_t response[AP_STT_RESPONSE_CAP];
        char transcript[2048];
        char vocabulary[65];
        size_t header_count = 3u;
        size_t response_len = 0;
        size_t transcript_len = 0;
        int status_code = 0;
        int http_rc;
        int result_ok = 0;
        int current;
        int shutting_down;
        int published = 0;

        memset(&job, 0, sizeof(job));
        pthread_mutex_lock(&st->mu);
        while (st->job_count == 0 && !st->shutting_down)
            pthread_cond_wait(&st->cond, &st->mu);
        if (st->shutting_down) {
            while (st->job_count > 0) {
                ap_stt_job *queued = &st->jobs[st->job_head];
                audio_engine_free_buffer(queued->pcm);
                audio_store_release(st, queued->pcm_capacity);
                memset(queued, 0, sizeof(*queued));
                st->job_head = (st->job_head + 1) % AP_STT_QUEUE_CAP;
                st->job_count--;
            }
            pthread_mutex_unlock(&st->mu);
            return NULL;
        }
        job = st->jobs[st->job_head];
        memset(&st->jobs[st->job_head], 0, sizeof(st->jobs[st->job_head]));
        st->job_head = (st->job_head + 1) % AP_STT_QUEUE_CAP;
        st->job_count--;
        if (session_key_init(&job_key, job.session_id) != 0 ||
            !generation_current_locked(st, &job_key, job.generation)) {
            pthread_mutex_unlock(&st->mu);
            audio_engine_free_buffer(job.pcm);
            audio_store_release(st, job.pcm_capacity);
            continue;
        }
        atomic_store_explicit(&st->http_cancel, 0, memory_order_relaxed);
        st->worker_active = 1;
        memcpy(
            st->worker_session_id,
            job_key.text,
            job_key.length + 1u);
        st->worker_generation = job.generation;
        pthread_mutex_unlock(&st->mu);

        if (read_stt_vocabulary(st, &job, vocabulary, sizeof(vocabulary))) {
            headers[3] = (http_min_header){"X-Whisper-Vocabulary", vocabulary};
            header_count = 4u;
        }
        job.input_stages.stt_provider_request_started_at_ms = realtime_ms();
        http_rc = http_min_client_post_headers_cancel(
            &st->stt_http,
            "application/octet-stream",
            headers,
            header_count,
            job.pcm,
            job.pcm_len,
            response,
            sizeof(response),
            &response_len,
            &status_code,
            st->stt_timeout_ms,
            &st->http_cancel);
        if (http_rc == HTTP_MIN_OK &&
            stt_json_extract_transcript(
                (const char *)response, response_len,
                transcript, sizeof(transcript), &transcript_len) == 0) {
            trim_transcript(transcript, &transcript_len);
            result_ok = transcript_len > 0;
        }
        job.input_stages.stt_provider_ready_at_ms = realtime_ms();
        if (job.input_stages.stt_provider_request_started_at_ms <= 0 ||
            job.input_stages.stt_provider_ready_at_ms <
                job.input_stages.stt_provider_request_started_at_ms)
            result_ok = 0;

        pthread_mutex_lock(&st->mu);
        current = generation_current_locked(st, &job_key, job.generation);
        shutting_down = st->shutting_down;
        st->worker_active = 0;
        st->worker_session_id[0] = '\0';
        st->worker_generation = 0;
        if (current && !st->shutting_down && result_ok &&
            publish_transcription_locked(
                st, &job, &job_key, transcript) == 0) published = 1;
        if (current) generation_finish_locked(st, &job_key, job.generation);
        pthread_mutex_unlock(&st->mu);

        if (current && !shutting_down) {
            if (published) {
                svc_log(
                    "audio-processor", "final transcript session=%s chars=%zu",
                    job.session_id, transcript_len);
            } else if (http_rc != HTTP_MIN_ERR_CANCELED) {
                if (publish_lifecycle_to(
                        st->worker_nc, &job_key,
                        STT_LIFECYCLE_TRANSCRIPTION_FAILED) != 0)
                    svc_log(
                        "audio-processor",
                        "STT failure lifecycle publish failed session=%s",
                        job.session_id);
                svc_log(
                    "audio-processor",
                    "STT failed closed session=%s http_rc=%d status=%d response_bytes=%zu",
                    job.session_id, http_rc, status_code, response_len);
            }
        }
        audio_engine_free_buffer(job.pcm);
        audio_store_release(st, job.pcm_capacity);
    }
}

static void on_stream(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    ap_state *st = (ap_state *)user;
    stt_stream_message_c msg;
    ap_session_key key;
    const char *sid;
    ap_session *sess;
    (void)reply;

    /* subject: ai.voice.stream.{session_id} */
    if (!subject || strncmp(
            subject,
            SUBJ_VOICE_STREAM_PFX ".",
            sizeof(SUBJ_VOICE_STREAM_PFX ".") - 1u) != 0) return;
    sid = subject + sizeof(SUBJ_VOICE_STREAM_PFX ".") - 1u;
    if (session_key_init(&key, sid) != 0) {
        svc_log("audio-processor", "reject oversized session id");
        return;
    }

    if (pb_decode_stt_stream_message(data, data_len, &msg) != 0) {
        svc_log_join_n(
            "audio-processor",
            "decode STTStreamMessage failed session=",
            sizeof("decode STTStreamMessage failed session=") - 1u,
            sid,
            key.length);
        return;
    }

    if (strcmp(msg.type, "start") == 0) {
        uint64_t now_ns = monotonic_ns();
        session_drop(st, &key, 1);
        sess = session_get(st, &key, 1, now_ns);
        if (sess) {
            if (st->stt_vocabulary_evaluation)
                memcpy(st->session_owners[(size_t)(sess - st->sessions)],
                    msg.user_id, strlen(msg.user_id) + 1u);
            svc_log_join_n(
                "audio-processor",
                "start session=",
                sizeof("start session=") - 1u,
                sid,
                key.length);
            publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_STARTED);
        }
        return;
    }
    if (strcmp(msg.type, "end") == 0) {
        int queued = 0;
        size_t captured_bytes = 0u;
        size_t stt_bytes = 0u;
        size_t trimmed_prefix_bytes = 0u;
        size_t trimmed_suffix_bytes = 0u;
        sess = session_get(st, &key, 0, 0u);
        if (!sess) {
            svc_log_join_n(
                "audio-processor",
                "reject end without start session=",
                sizeof("reject end without start session=") - 1u,
                sid,
                key.length);
            return;
        }
        {
            audio_finalized_utterance finalized;
            size_t finalized_capacity;
            uint64_t generation = sess->generation;
            turn_input_stage_timestamps_c input_stages;
            int64_t received_at_ms = realtime_ms();
            memset(&input_stages, 0, sizeof(input_stages));
            input_stages.audio_committed_at_ms =
                msg.timestamp_ms > 0 ? msg.timestamp_ms : received_at_ms;
            input_stages.stt_request_received_at_ms = received_at_ms;
            audio_engine_mark_complete(&sess->engine, monotonic_ns());
            finalized_capacity =
                audio_engine_utterance_capacity(&sess->engine);
            if (st->stt_speech_context_ms >= 0) {
                const size_t context_bytes =
                    (size_t)st->stt_speech_context_ms *
                    AP_PCM16_MONO_16KHZ_BYTES_PER_MS;
                finalized = audio_engine_finalize_speech_window(
                    &sess->engine, context_bytes, context_bytes);
            } else {
                finalized = audio_engine_finalize(&sess->engine);
            }
            captured_bytes = finalized.captured_length;
            stt_bytes = finalized.length;
            trimmed_prefix_bytes = finalized.trimmed_prefix_length;
            trimmed_suffix_bytes = finalized.trimmed_suffix_length;
            if (finalized.buffer && finalized.length > 0 &&
                finalized.length <= AP_MAX_UTTERANCE_BYTES &&
                (finalized.length & 1u) == 0u &&
                (finalized.flags & AUDIO_DECISION_COMPLETE) != 0u &&
                (finalized.flags & AUDIO_DECISION_VOICE_SEEN) != 0u &&
                input_stages.audio_committed_at_ms > 0 &&
                input_stages.stt_request_received_at_ms >=
                    input_stages.audio_committed_at_ms) {
                queued = enqueue_stt(
                    st,
                    &key,
                    generation,
                    finalized.buffer,
                    finalized.length,
                    finalized_capacity,
                    &input_stages,
                    st->session_owners[(size_t)(sess - st->sessions)]);
            }
            if (!queued && finalized.buffer) {
                audio_engine_free_buffer(finalized.buffer);
                audio_store_release(st, finalized_capacity);
            }
            session_drop(st, &key, queued ? 0 : 1);
        }
        publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
        if (!queued)
            publish_lifecycle(
                st, &key, STT_LIFECYCLE_TRANSCRIPTION_FAILED);
        svc_log(
            "audio-processor",
            "end session=%s stt=%s captured_bytes=%zu stt_bytes=%zu "
            "trimmed_prefix_bytes=%zu trimmed_suffix_bytes=%zu",
            sid,
            queued ? "queued" : "skipped",
            captured_bytes,
            stt_bytes,
            trimmed_prefix_bytes,
            trimmed_suffix_bytes);
        return;
    }
    if (strcmp(msg.type, "cancel") == 0) {
        int had_session;
        sess = session_get(st, &key, 0, 0u);
        had_session = sess != NULL;
        if (had_session)
            session_drop(st, &key, 1);
        else
            generation_invalidate_session(st, &key);
        if (had_session)
            publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
        svc_log_join_n(
            "audio-processor",
            "cancel session=",
            sizeof("cancel session=") - 1u,
            sid,
            key.length);
        return;
    }
    if (strcmp(msg.type, "chunk") == 0) {
        audio_decision decision;
        audio_process_options options;
        audio_monitor_state state;
        size_t capacity_before;
        size_t capacity_after;
        size_t session_capacity_limit;
        uint64_t frame_now_ns;
        if (msg.audio_len == 0 || (msg.audio_len & 1u) != 0u ||
            msg.sample_rate != 16000 || msg.channels != 1 || msg.bit_depth != 16) {
            svc_log(
                "audio-processor",
                "reject chunk session=%s bytes=%zu rate=%d ch=%d bits=%d",
                sid, msg.audio_len, msg.sample_rate, msg.channels, msg.bit_depth);
            return;
        }
        frame_now_ns = monotonic_ns();
        sess = session_get(st, &key, 0, frame_now_ns);
        if (!sess) {
            svc_log_join_n(
                "audio-processor",
                "reject chunk without start session=",
                sizeof("reject chunk without start session=") - 1u,
                sid,
                key.length);
            return;
        }
        if (msg.audio_len > AP_MAX_CHUNK_BYTES) {
            publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
            svc_log(
                "audio-processor",
                "reject oversized chunk session=%s bytes=%zu",
                sid,
                msg.audio_len);
            session_drop(st, &key, 1);
            return;
        }
        memset(&state, 0, sizeof(state));
        audio_engine_get_monitor_state(&sess->engine, &state);
        if (msg.audio_len > AP_MAX_UTTERANCE_BYTES ||
            state.utterance_bytes > AP_MAX_UTTERANCE_BYTES - msg.audio_len) {
            publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
            svc_log_join_n(
                "audio-processor",
                "utterance overflow session=",
                sizeof("utterance overflow session=") - 1u,
                sid,
                key.length);
            session_drop(st, &key, 1);
            return;
        }
        memset(&options, 0, sizeof(options));
        memset(&decision, 0, sizeof(decision));
        capacity_before = audio_engine_utterance_capacity(&sess->engine);
        session_capacity_limit = audio_store_session_limit(st, capacity_before);
        audio_engine_process_bounded(
            &sess->engine,
            msg.audio,
            msg.audio_len,
            frame_now_ns,
            options,
            session_capacity_limit,
            &decision
        );
        capacity_after = audio_engine_utterance_capacity(&sess->engine);
        if (capacity_after > capacity_before)
            atomic_fetch_add_explicit(
                &st->store_bytes,
                capacity_after - capacity_before,
                memory_order_relaxed);
        if ((decision.flags & AUDIO_DECISION_ERROR) != 0u) {
            publish_lifecycle(st, &key, STT_LIFECYCLE_STREAM_ENDED);
            svc_log_join_n(
                "audio-processor",
                "audio engine rejected chunk session=",
                sizeof("audio engine rejected chunk session=") - 1u,
                sid,
                key.length);
            session_drop(st, &key, 1);
            return;
        }
        if (decision.flags & AUDIO_DECISION_INTERRUPT) {
            publish_interrupt(st, &key);
        }
        return;
    }
    svc_log("audio-processor", "reject unknown stream type=%s session=%s", msg.type, sid);
}

int main(void) {
    vbus_stop_flag stop = 0;
    ap_state *st;
    const char *backend;
    const char *base_url;
    const char *language;
    const char *queue;
    int run_rc = -1;
    int worker_started = 0;
    int mutex_ready = 0;
    int cond_ready = 0;
    int stt_http_ready = 0;
    int telemetry_ready = 0;
    int exit_code = 1;
    st = (ap_state *)calloc(1, sizeof(*st));
    if (!st) {
        svc_log("audio-processor", "state allocation failed");
        return 1;
    }
    atomic_init(&st->http_cancel, 0);
    atomic_init(&st->store_bytes, 0u);
    if (pthread_mutex_init(&st->mu, NULL) != 0) {
        free(st);
        return 1;
    }
    mutex_ready = 1;
    if (pthread_cond_init(&st->cond, NULL) != 0) goto done;
    cond_ready = 1;
    telemetry_init_global_ring(AP_TELEMETRY_RING_CAPACITY);
    telemetry_ready = telemetry_get_global_ring() != NULL;
    if (!telemetry_ready) {
        svc_log("audio-processor", "telemetry ring allocation failed");
        goto done;
    }

    backend = svc_env("STT_BACKEND", "internal-pcm-s16le");
    base_url = svc_env(
        "STT_BACKEND_URL", "http://stt.ai-ml.svc.cluster.local:8000");
    language = svc_env("STT_LANGUAGE", "en");
    if (strcmp(backend, "internal-pcm-s16le") != 0 ||
        !valid_language(language) ||
        strlen(language) >= sizeof(st->stt_language) ||
        build_stt_url(base_url, st->stt_url, sizeof(st->stt_url)) != 0) {
        svc_log(
            "audio-processor",
            "invalid STT config backend=%s base_url=%s language=%s",
            backend, base_url, language);
        goto done;
    }
    memcpy(st->stt_language, language, strlen(language) + 1);
    st->stt_timeout_ms = svc_env_int_range("STT_TIMEOUT_MS", 120000, 100, 300000);
    st->stt_speech_context_ms = svc_env_int_range(
        "STT_SPEECH_CONTEXT_MS",
        AP_DEFAULT_STT_SPEECH_CONTEXT_MS,
        -1,
        AP_MAX_STT_SPEECH_CONTEXT_MS);
    st->stt_vocabulary_evaluation = svc_env_int_range(
        "STT_VOCABULARY_EVALUATION", 0, 0, 1);
    if (st->stt_vocabulary_evaluation) {
        char health_url[768];
        uint8_t health[4096];
        size_t health_len = 0;
        int status = 0;
        size_t prefix = strlen(st->stt_url) - strlen(AP_STT_CANONICAL_PATH);
        int n = snprintf(health_url, sizeof(health_url), "%.*s/health",
            (int)prefix, st->stt_url);
        if (n <= 0 || (size_t)n >= sizeof(health_url) ||
            http_min_get(health_url, health, sizeof(health) - 1u,
                &health_len, &status, 1000) != HTTP_MIN_OK || status != 200) {
            svc_log("audio-processor", "STT vocabulary evaluator is not ready");
            goto done;
        }
        health[health_len] = '\0';
        if (memchr(health, '\0', health_len) ||
            !stt_vocabulary_evaluator_ready((const char *)health)) {
            svc_log("audio-processor", "STT vocabulary requires the admitted evaluation engine");
            goto done;
        }
    }
    st->store_max_bytes = (size_t)svc_env_int_range(
        "AUDIO_STORE_MAX_BYTES",
        (int)AP_DEFAULT_STORE_MAX_BYTES,
        (int)AP_MIN_STORE_MAX_BYTES,
        (int)AP_MAX_STORE_MAX_BYTES);
    st->stream_idle_ns =
        (uint64_t)svc_env_int_range(
            "AUDIO_STREAM_IDLE_TIMEOUT_MS",
            AP_DEFAULT_STREAM_IDLE_TIMEOUT_MS,
            1000,
            AP_MAX_STREAM_IDLE_TIMEOUT_MS) * UINT64_C(1000000);
    if (http_min_client_init(&st->stt_http, st->stt_url) != HTTP_MIN_OK) {
        svc_log("audio-processor", "invalid STT URL url=%s", st->stt_url);
        goto done;
    }
    stt_http_ready = 1;

    svc_install_signals(&stop);
    st->nc = svc_connect_bus();
    if (!st->nc) goto done;
    st->worker_nc = svc_connect_bus();
    if (!st->worker_nc) goto done;
    if (pthread_create(&st->worker, NULL, stt_worker_main, st) != 0) {
        svc_log("audio-processor", "STT worker start failed");
        goto done;
    }
    worker_started = 1;
    queue = svc_env("VBUS_QUEUE_GROUP", "audio-processors");
    if (vbus_subscribe(st->nc, SUBJ_VOICE_STREAM_ALL, queue, on_stream, st) != 0) {
        svc_log("audio-processor", "subscribe failed");
        goto done;
    }
    svc_log(
        "audio-processor",
        "pure-C service running queue=%s stt=%s language=%s store_max_bytes=%zu "
        "stt_speech_context_ms=%d "
        "(single-inflight)",
        queue,
        st->stt_url,
        st->stt_language,
        st->store_max_bytes,
        st->stt_speech_context_ms);
    run_rc = 0;
    while (atomic_load_explicit(&stop, memory_order_relaxed) == 0) {
        if (vbus_poll(st->nc, 200) != 0) {
            run_rc = -1;
            break;
        }
        session_expire(st, monotonic_ns());
    }
    exit_code = run_rc == 0 ? 0 : 1;

done:
    if (worker_started) {
        pthread_mutex_lock(&st->mu);
        st->shutting_down = 1;
        atomic_store_explicit(&st->http_cancel, 1, memory_order_relaxed);
        pthread_cond_broadcast(&st->cond);
        pthread_mutex_unlock(&st->mu);
        pthread_join(st->worker, NULL);
    }
    {
        size_t i;
        for (i = 0; i < st->session_pool_high_water; ++i) {
            if (st->sessions[i].in_use) {
                audio_engine_session_deinit(&st->sessions[i].engine);
            }
        }
    }
    if (st->worker_nc) vbus_close(st->worker_nc);
    if (st->nc) vbus_close(st->nc);
    if (stt_http_ready) http_min_client_destroy(&st->stt_http);
    if (telemetry_ready) telemetry_free_global_ring();
    if (cond_ready) pthread_cond_destroy(&st->cond);
    if (mutex_ready) pthread_mutex_destroy(&st->mu);
    free(st);
    return exit_code;
}
