/* Pure-C bounded in-memory session store using canonical session protobufs. */
#define _POSIX_C_SOURCE 200809L

#include "../common/service.h"
#include "../common/dynbuf.h"
#include "../common/voice_ascii.h"
#include "../wire/pb_min.h"
#include "../wire/subjects.h"
#include "turn_error.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SM_MAX_SESSIONS 1024u
#define SM_INDEX_CAPACITY 2048u
#define SM_MAX_SESSION_BYTES (4u * 1024u * 1024u)
#define SM_DEFAULT_STORE_BYTES (32u * 1024u * 1024u)
#define SM_MIN_STORE_BYTES 4096u
#define SM_MAX_STORE_BYTES (128u * 1024u * 1024u)

_Static_assert(
    (SM_INDEX_CAPACITY & (SM_INDEX_CAPACITY - 1u)) == 0,
    "session index capacity must be a power of two");
_Static_assert(
    SM_INDEX_CAPACITY >= SM_MAX_SESSIONS * 2u,
    "session index load must not exceed 50 percent");
_Static_assert(
    SM_MAX_SESSIONS <= UINT16_MAX,
    "session pool indexes must fit in the recycle stack");

typedef struct {
    char session_id[128];
    char user_id[128];
    /* [u32 framed_len][field-2 tag + varint length + SessionMessage]... */
    dynbuf messages;
    size_t nmsg;
} sm_session;

typedef struct {
    vbus_client *nc;
    sm_session *sessions[SM_INDEX_CAPACITY];
    uint8_t slot_state[SM_INDEX_CAPACITY];
    /* The static slab is demand-paged. The recycle stack keeps churn O(1). */
    sm_session session_pool[SM_MAX_SESSIONS];
    uint16_t recycled_session_slots[SM_MAX_SESSIONS];
    size_t session_pool_high_water;
    size_t recycled_session_count;
    size_t session_count;
    size_t allocated_bytes;
    size_t max_bytes;
} sm_state;

enum sm_slot_state {
    SM_SLOT_EMPTY = 0,
    SM_SLOT_USED = 1,
    SM_SLOT_TOMBSTONE = 2,
};

static int64_t realtime_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int valid_id(const char *id) {
    const unsigned char *p = (const unsigned char *)id;
    if (!p || !*p) return 0;
    while (*p) {
        if (!(voice_ascii_is_alnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
              *p == ':'))
            return 0;
        p++;
    }
    return 1;
}

static int valid_session_id_hash(const char *id, size_t *hash_out) {
    const unsigned char *p = (const unsigned char *)id;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!p || !*p || !hash_out) return 0;
    while (*p) {
        if (!(voice_ascii_is_alnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
              *p == ':'))
            return 0;
        hash ^= *p++;
        hash *= UINT64_C(1099511628211);
    }
    *hash_out = (size_t)hash;
    return 1;
}

static sm_session *acquire_session_slot(sm_state *st) {
    size_t slot;
    sm_session *session;
    if (!st) return NULL;
    if (st->recycled_session_count != 0) {
        slot = st->recycled_session_slots[--st->recycled_session_count];
    } else {
        if (st->session_pool_high_water >= SM_MAX_SESSIONS) return NULL;
        slot = st->session_pool_high_water++;
    }
    session = &st->session_pool[slot];
    session->session_id[0] = '\0';
    session->user_id[0] = '\0';
    dynbuf_init(&session->messages);
    session->nmsg = 0;
    return session;
}

static void release_session_slot(sm_state *st, sm_session *session) {
    size_t slot;
    if (!st || !session || st->recycled_session_count >= SM_MAX_SESSIONS) return;
    slot = (size_t)(session - st->session_pool);
    if (slot >= st->session_pool_high_water) return;
    session->session_id[0] = '\0';
    session->user_id[0] = '\0';
    st->recycled_session_slots[st->recycled_session_count++] = (uint16_t)slot;
}

static sm_session *find_validated(
    sm_state *st,
    const char *session_id,
    const char *user_id,
    size_t hash,
    int create
) {
    size_t first_tombstone = SM_INDEX_CAPACITY;
    size_t start;
    size_t probe;

    if (!st) return NULL;
    start = hash & (SM_INDEX_CAPACITY - 1u);
    for (probe = 0; probe < SM_INDEX_CAPACITY; ++probe) {
        size_t index = (start + probe) & (SM_INDEX_CAPACITY - 1u);
        sm_session *session;

        if (st->slot_state[index] == SM_SLOT_EMPTY) {
            if (first_tombstone == SM_INDEX_CAPACITY)
                first_tombstone = index;
            break;
        }
        if (st->slot_state[index] == SM_SLOT_TOMBSTONE) {
            if (first_tombstone == SM_INDEX_CAPACITY)
                first_tombstone = index;
            continue;
        }
        session = st->sessions[index];
        if (session && strcmp(session->session_id, session_id) == 0) {
            return strcmp(session->user_id, user_id) == 0 ? session : NULL;
        }
    }
    if (!create || st->session_count >= SM_MAX_SESSIONS ||
        first_tombstone == SM_INDEX_CAPACITY) return NULL;
    {
        sm_session *session;
        size_t session_id_len;
        size_t user_id_len;
        session_id_len = strlen(session_id);
        user_id_len = strlen(user_id);
        if (session_id_len >= sizeof(((sm_session *)0)->session_id) ||
            user_id_len >= sizeof(((sm_session *)0)->user_id)) return NULL;
        session = acquire_session_slot(st);
        if (!session) return NULL;
        memcpy(session->session_id, session_id, session_id_len + 1u);
        memcpy(session->user_id, user_id, user_id_len + 1u);
        st->sessions[first_tombstone] = session;
        st->slot_state[first_tombstone] = SM_SLOT_USED;
        st->session_count++;
        return session;
    }
}

static int drop_session_validated(
    sm_state *st,
    const char *session_id,
    const char *user_id,
    size_t hash
) {
    size_t start;
    size_t probe;

    if (!st) return 0;
    start = hash & (SM_INDEX_CAPACITY - 1u);
    for (probe = 0; probe < SM_INDEX_CAPACITY; ++probe) {
        size_t index = (start + probe) & (SM_INDEX_CAPACITY - 1u);
        sm_session *session;

        if (st->slot_state[index] == SM_SLOT_EMPTY) return 0;
        if (st->slot_state[index] != SM_SLOT_USED) continue;
        session = st->sessions[index];
        if (!session || strcmp(session->session_id, session_id) != 0) continue;
        if (strcmp(session->user_id, user_id) != 0) return 0;
        if (session->messages.cap <= st->allocated_bytes)
            st->allocated_bytes -= session->messages.cap;
        else
            st->allocated_bytes = 0;
        dynbuf_free(&session->messages);
        st->sessions[index] = NULL;
        st->slot_state[index] = SM_SLOT_TOMBSTONE;
        st->session_count--;
        release_session_slot(st, session);
        return 1;
    }
    return 0;
}

static size_t put_varint(uint8_t *out, size_t out_cap, uint64_t value) {
    size_t pos = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        if (value) byte |= 0x80u;
        if (pos == out_cap) return 0;
        out[pos++] = byte;
    } while (value);
    return pos;
}

static int append_message(
    sm_state *st,
    sm_session *session,
    const uint8_t *wire,
    size_t wire_len
) {
    uint8_t header[16];
    size_t varint_len;
    size_t framed_len;
    uint32_t stored_len;
    size_t need;
    size_t old_capacity;
    size_t available;
    size_t buffer_limit;
    if (!st || !session || !wire || wire_len == 0 || wire_len > UINT32_MAX) return -1;
    header[0] = (uint8_t)((2u << 3) | 2u);
    varint_len = put_varint(header + 1, sizeof(header) - 1, wire_len);
    if (varint_len == 0 || wire_len > SIZE_MAX - 1u - varint_len) return -1;
    framed_len = 1u + varint_len + wire_len;
    if (framed_len > UINT32_MAX || framed_len > SM_MAX_SESSION_BYTES - sizeof(stored_len) ||
        session->messages.len > SM_MAX_SESSION_BYTES - sizeof(stored_len) - framed_len)
        return -1;
    need = session->messages.len + sizeof(stored_len) + framed_len;
    old_capacity = session->messages.cap;
    if (st->allocated_bytes > st->max_bytes) return -1;
    available = st->max_bytes - st->allocated_bytes;
    if (old_capacity > SIZE_MAX - available) return -1;
    buffer_limit = old_capacity + available;
    if (dynbuf_reserve_bounded(&session->messages, need, buffer_limit) != 0) return -1;
    st->allocated_bytes += session->messages.cap - old_capacity;
    stored_len = (uint32_t)framed_len;
    memcpy(session->messages.data + session->messages.len, &stored_len, sizeof(stored_len));
    memcpy(
        session->messages.data + session->messages.len + sizeof(stored_len),
        header, 1u + varint_len);
    memcpy(
        session->messages.data + session->messages.len + sizeof(stored_len) + 1u + varint_len,
        wire, wire_len);
    session->messages.len = need;
    session->nmsg++;
    return 0;
}

static int next_record(
    const sm_session *session,
    size_t *offset,
    const uint8_t **framed,
    size_t *framed_len
) {
    uint32_t stored_len;
    if (!session || !offset || !framed || !framed_len ||
        *offset > session->messages.len ||
        session->messages.len - *offset < sizeof(stored_len)) return 0;
    memcpy(&stored_len, session->messages.data + *offset, sizeof(stored_len));
    *offset += sizeof(stored_len);
    if ((size_t)stored_len > session->messages.len - *offset) return -1;
    *framed = session->messages.data + *offset;
    *framed_len = stored_len;
    *offset += stored_len;
    return 1;
}

static int framed_message_wire(
    const uint8_t *framed,
    size_t framed_len,
    const uint8_t **wire,
    size_t *wire_len
) {
    size_t pos = 1;
    uint64_t length = 0;
    unsigned shift = 0;
    if (!framed || framed_len < 2 || framed[0] != (uint8_t)((2u << 3) | 2u)) return -1;
    while (pos < framed_len && shift < 64) {
        uint8_t byte = framed[pos++];
        length |= (uint64_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            if (length > SIZE_MAX || (size_t)length != framed_len - pos) return -1;
            *wire = framed + pos;
            *wire_len = (size_t)length;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static void reply_wire(sm_state *st, const char *reply, const uint8_t *wire, size_t wire_len) {
    if (reply && reply[0] && wire_len && vbus_publish(st->nc, reply, wire, wire_len) != 0)
        svc_log("session-manager", "reply publish failed");
}

static void reply_error(
    sm_state *st,
    const char *reply,
    const char *message,
    const char *type
) {
    uint8_t wire[512];
    size_t wire_len = pb_encode_error_response(wire, sizeof(wire), message, type);
    reply_wire(st, reply, wire, wire_len);
}

static void on_append(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    sm_state *st = (sm_state *)user;
    session_append_view_c request;
    sm_session *session;
    size_t session_id_hash;
    size_t previous_count;
    int created = 0;
    uint8_t response[256];
    size_t response_len;
    (void)subject;
    if (pb_decode_session_append_view(data, data_len, &request) != 0 ||
        !valid_session_id_hash(request.session_id, &session_id_hash) ||
        !valid_id(request.user_id)) {
        svc_log("session-manager", "reject malformed append");
        reply_error(st, reply, "invalid session append", TURN_ERR_CLASS_VALIDATION);
        return;
    }
    previous_count = st->session_count;
    session = find_validated(
        st, request.session_id, request.user_id, session_id_hash, 1);
    created = session && st->session_count != previous_count;
    if (!session) {
        svc_log("session-manager", "append unavailable session=%s", request.session_id);
        reply_error(
            st, reply, "session store unavailable", TURN_ERR_CLASS_TRANSIENT);
        return;
    }
    if (append_message(
            st, session, request.message_wire, request.message_wire_len) != 0) {
        svc_log("session-manager", "append capacity/error session=%s", request.session_id);
        if (created)
            (void)drop_session_validated(
                st, request.session_id, request.user_id, session_id_hash);
        reply_error(
            st, reply, "session store capacity reached", TURN_ERR_CLASS_TRANSIENT);
        return;
    }
    response_len = pb_encode_session_append_response(
        response, sizeof(response), session->session_id, (int32_t)session->nmsg);
    reply_wire(st, reply, response, response_len);
}

static void on_get(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    sm_state *st = (sm_state *)user;
    session_get_request_c request;
    sm_session *session;
    dynbuf response;
    uint8_t prefix[256];
    size_t session_id_hash;
    size_t prefix_len;
    size_t offset = 0;
    size_t index = 0;
    size_t skip = 0;
    (void)subject;
    if (pb_decode_session_get_request(data, data_len, &request) != 0 ||
        !valid_session_id_hash(request.session_id, &session_id_hash) ||
        !valid_id(request.user_id)) return;
    session = find_validated(
        st, request.session_id, request.user_id, session_id_hash, 0);
    prefix_len = pb_encode_session_get_response_prefix(
        prefix, sizeof(prefix), request.session_id);
    dynbuf_init(&response);
    if (!prefix_len || dynbuf_append(&response, prefix, prefix_len) != 0) {
        dynbuf_free(&response);
        return;
    }
    if (session) {
        if (request.last_n > 0 && (size_t)request.last_n < session->nmsg)
            skip = session->nmsg - (size_t)request.last_n;
        while (offset < session->messages.len) {
            const uint8_t *framed;
            size_t framed_len;
            int rc = next_record(session, &offset, &framed, &framed_len);
            if (rc <= 0) {
                dynbuf_free(&response);
                return;
            }
            if (index++ >= skip && dynbuf_append(&response, framed, framed_len) != 0) {
                dynbuf_free(&response);
                return;
            }
        }
    }
    reply_wire(st, reply, response.data, response.len);
    dynbuf_free(&response);
}

static int materialize_summary(const sm_session *session, char *out, size_t out_cap) {
    session_message_c first;
    session_message_c last;
    size_t offset = 0;
    size_t count = 0;
    int n;
    memset(&first, 0, sizeof(first));
    memset(&last, 0, sizeof(last));
    if (session) {
        while (offset < session->messages.len) {
            const uint8_t *framed;
            const uint8_t *wire;
            size_t framed_len;
            size_t wire_len;
            session_message_c message;
            if (next_record(session, &offset, &framed, &framed_len) <= 0 ||
                framed_message_wire(framed, framed_len, &wire, &wire_len) != 0 ||
                pb_decode_session_message(wire, wire_len, &message) != 0) return -1;
            if (count == 0) first = message;
            last = message;
            count++;
        }
    }
    n = snprintf(
        out, out_cap, "nmsg=%zu first=%.80s | last=%.80s",
        count, first.content, last.content);
    return n >= 0 && (size_t)n < out_cap ? 0 : -1;
}

static void on_summary(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    sm_state *st = (sm_state *)user;
    session_id_request_c request;
    sm_session *session;
    char summary[256];
    uint8_t response[1024];
    size_t session_id_hash;
    size_t response_len;
    (void)subject;
    if (pb_decode_session_id_request(data, data_len, &request) != 0 ||
        !valid_session_id_hash(request.session_id, &session_id_hash) ||
        !valid_id(request.user_id)) return;
    session = find_validated(
        st, request.session_id, request.user_id, session_id_hash, 0);
    if (materialize_summary(session, summary, sizeof(summary)) != 0) return;
    response_len = pb_encode_session_summary_response(
        response, sizeof(response), request.session_id, request.user_id,
        summary, "c-bounded-span-v1", realtime_ms());
    reply_wire(st, reply, response, response_len);
}

static void on_delete(
    const char *subject,
    const char *reply,
    const uint8_t *data,
    size_t data_len,
    void *user
) {
    sm_state *st = (sm_state *)user;
    session_id_request_c request;
    uint8_t response[256];
    size_t session_id_hash;
    size_t response_len;
    int deleted;
    (void)subject;
    if (pb_decode_session_id_request(data, data_len, &request) != 0 ||
        !valid_session_id_hash(request.session_id, &session_id_hash) ||
        !valid_id(request.user_id)) return;
    deleted = drop_session_validated(
        st, request.session_id, request.user_id, session_id_hash);
    response_len = pb_encode_session_delete_response(
        response, sizeof(response), request.session_id, deleted);
    reply_wire(st, reply, response, response_len);
}

int main(void) {
    static sm_state state;
    vbus_stop_flag stop = 0;
    sm_state *st = &state;
    const char *queue;
    int run_rc;
    st->max_bytes = (size_t)svc_env_int_range(
        "SESSION_STORE_MAX_BYTES",
        (int)SM_DEFAULT_STORE_BYTES,
        (int)SM_MIN_STORE_BYTES,
        (int)SM_MAX_STORE_BYTES);
    svc_install_signals(&stop);
    st->nc = svc_connect_bus();
    if (!st->nc) return 1;
    queue = svc_env("VBUS_QUEUE_GROUP", "session-managers");
    if (vbus_subscribe(st->nc, SUBJ_SESSION_APPEND, queue, on_append, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_SESSION_GET, queue, on_get, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_SESSION_DELETE, queue, on_delete, st) != 0 ||
        vbus_subscribe(st->nc, SUBJ_SESSION_SUMMARY, queue, on_summary, st) != 0) {
        vbus_close(st->nc);
        return 1;
    }
    svc_log(
        "session-manager",
        "pure-C canonical bounded session store max_bytes=%zu",
        st->max_bytes);
    run_rc = vbus_run(st->nc, &stop);
    {
        size_t i;
        for (i = 0; i < st->session_pool_high_water; ++i)
            dynbuf_free(&st->session_pool[i].messages);
    }
    vbus_close(st->nc);
    return run_rc == 0 ? 0 : 1;
}
