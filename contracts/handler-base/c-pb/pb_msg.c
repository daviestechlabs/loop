#include "pb_msg.h"

#include <string.h>

static void zero(void *p, size_t n) {
    memset(p, 0, n);
}

static size_t done(pb_writer *w) {
    return w->pos > 0 || w->cap > 0 ? w->pos : 0;
}

/* ── TurnStartRequest ── */
static int decode_turn_metadata(const uint8_t *entry, size_t len, pb_meta_pair *pair) {
    pb_reader reader;
    unsigned seen = 0u;
    pb_reader_init(&reader, entry, len);
    while (reader.pos < reader.len) {
        uint32_t field, wire;
        if (pb_read_tag(&reader, &field, &wire) != 0) return -1;
        if (field == 1u || field == 2u) {
            unsigned bit = 1u << (field - 1u);
            char *dst = field == 1u ? pair->key : pair->val;
            size_t cap = field == 1u ? sizeof(pair->key) : sizeof(pair->val);
            if (wire != 2u || (seen & bit) != 0u ||
                pb_read_string(&reader, dst, cap) != 0) return -1;
            seen |= bit;
        } else if (pb_skip(&reader, wire) != 0) {
            return -1;
        }
    }
    return seen == 3u && pair->key[0] ? 0 : -1;
}

size_t pb_enc_turn_start_req(uint8_t *out, size_t cap, const pb_turn_start_req *m) {
    pb_writer w;
    int i;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->username) != 0) return 0;
    if (pb_write_string(&w, 5, m->text) != 0) return 0;
    if (m->premium && pb_write_bool(&w, 6, 1) != 0) return 0;
    if (m->enable_rag && pb_write_bool(&w, 7, 1) != 0) return 0;
    if (m->enable_tts && pb_write_bool(&w, 8, 1) != 0) return 0;
    if (pb_write_string(&w, 9, m->system_prompt) != 0) return 0;
    if (pb_write_string(&w, 10, m->voice_id) != 0) return 0;
    if (pb_write_string(&w, 11, m->response_subject) != 0) return 0;
    if (m->n_meta < 0 || m->n_meta > PB_TURN_META_PAIRS) return 0;
    for (i = 0; i < m->n_meta; i++)
        if (pb_write_map_ss(&w, 12, m->meta[i].key, m->meta[i].val) != 0) return 0;
    if (m->dnd_initiative.count) {
        if (m->dnd_campaign.operation || m->dnd_encounter_action.operation) return 0;
        uint8_t request[DND_INITIATIVE_REQUEST_MAX];
        size_t length = dnd_initiative_request_encode(request, sizeof(request), &m->dnd_initiative);
        if (!length || pb_write_bytes(&w, 18u, request, length) != 0) return 0;
    }
    if (m->dnd_campaign.operation) {
        if (m->dnd_encounter_action.operation) return 0;
        uint8_t request[DND_CAMPAIGN_REQUEST_MAX];
        size_t length = dnd_campaign_request_encode(request, sizeof(request), &m->dnd_campaign);
        if (!length || pb_write_bytes(&w, 19u, request, length) != 0) return 0;
    }
    if (m->dnd_encounter_action.operation) {
        uint8_t request[DND_ENCOUNTER_ACTION_MAX];
        size_t length = dnd_encounter_action_encode(request, sizeof(request), &m->dnd_encounter_action);
        if (!length || pb_write_bytes(&w, 20u, request, length) != 0) return 0;
    }
    return done(&w);
}

int pb_dec_turn_start_req(const uint8_t *in, size_t n, pb_turn_start_req *m) {
    pb_reader r;
    int initiative_seen = 0;
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field >= 18u && field <= 20u) {
            const uint8_t *request;
            size_t length;
            if (initiative_seen || wire != 2u || pb_read_bytes(&r, &request, &length) != 0 ||
                !(field == 18u ? dnd_initiative_request_decode(request, length, &m->dnd_initiative) :
                  field == 19u ? dnd_campaign_request_decode(request, length, &m->dnd_campaign) :
                  dnd_encounter_action_decode(request, length, &m->dnd_encounter_action))) return -1;
            initiative_seen = 1;
            continue;
        }
        if (wire == 2) {
            char *dst = NULL;
            size_t cap = 0;
            if (field == 1) {
                dst = m->request_id;
                cap = sizeof(m->request_id);
            } else if (field == 2) {
                dst = m->user_id;
                cap = sizeof(m->user_id);
            } else if (field == 3) {
                dst = m->session_id;
                cap = sizeof(m->session_id);
            } else if (field == 4) {
                dst = m->username;
                cap = sizeof(m->username);
            } else if (field == 5) {
                dst = m->text;
                cap = sizeof(m->text);
            } else if (field == 9) {
                dst = m->system_prompt;
                cap = sizeof(m->system_prompt);
            } else if (field == 10) {
                dst = m->voice_id;
                cap = sizeof(m->voice_id);
            } else if (field == 11) {
                dst = m->response_subject;
                cap = sizeof(m->response_subject);
            } else if (field == 12) {
                const uint8_t *entry;
                size_t elen;
                if (pb_read_bytes(&r, &entry, &elen) != 0) return -1;
                if (m->n_meta < PB_TURN_META_PAIRS) {
                    int prior;
                    if (decode_turn_metadata(entry, elen, &m->meta[m->n_meta]) != 0)
                        return -1;
                    for (prior = 0; prior < m->n_meta; ++prior)
                        if (strcmp(m->meta[prior].key,
                                   m->meta[m->n_meta].key) == 0) return -1;
                    m->n_meta++;
                } else {
                    return -1;
                }
                continue;
            }
            if (dst) {
                if (pb_read_string(&r, dst, cap) != 0) return -1;
                continue;
            }
        }
        if (wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0) return -1;
            if (field == 6) m->premium = (int)v;
            else if (field == 7) m->enable_rag = (int)v;
            else if (field == 8) m->enable_tts = (int)v;
            continue;
        }
        if (pb_skip(&r, wire) != 0) return -1;
    }
    return 0;
}

size_t pb_enc_turn_start_resp(uint8_t *out, size_t cap, const pb_turn_start_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_bool(&w, 2, m->accepted) != 0) return 0;
    if (pb_write_enum(&w, 3, m->state) != 0) return 0;
    if (pb_write_int64(&w, 4, m->accepted_at) != 0) return 0;
    if (pb_write_string(&w, 5, m->event_subject) != 0) return 0;
    return done(&w);
}

int pb_dec_turn_start_resp(const uint8_t *in, size_t n, pb_turn_start_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->request_id, sizeof(m->request_id));
    m->accepted = pb_get_bool(in, n, 2);
    m->state = (int)pb_get_int(in, n, 3);
    m->accepted_at = pb_get_int(in, n, 4);
    pb_get_string(in, n, 5, m->event_subject, sizeof(m->event_subject));
    return 0;
}

size_t pb_enc_turn_cancel_req(uint8_t *out, size_t cap, const pb_turn_cancel_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->reason) != 0) return 0;
    return done(&w);
}

int pb_dec_turn_cancel_req(const uint8_t *in, size_t n, pb_turn_cancel_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->request_id, sizeof(m->request_id));
    pb_get_string(in, n, 2, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 3, m->reason, sizeof(m->reason));
    return 0;
}

size_t pb_enc_turn_event(uint8_t *out, size_t cap, const pb_turn_event *m) {
    pb_writer w;
    int i;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_enum(&w, 3, m->type) != 0) return 0;
    if (pb_write_enum(&w, 4, m->state) != 0) return 0;
    if (pb_write_string(&w, 5, m->text) != 0) return 0;
    if (pb_write_int64(&w, 7, m->sample_rate) != 0) return 0;
    if (pb_write_int64(&w, 8, m->channels) != 0) return 0;
    if (pb_write_int64(&w, 9, m->bit_depth) != 0) return 0;
    if (pb_write_int64(&w, 10, m->sequence) != 0) return 0;
    if (pb_write_int64(&w, 11, m->segment_index) != 0) return 0;
    if (m->is_final && pb_write_bool(&w, 12, 1) != 0) return 0;
    if (pb_write_int64(&w, 13, m->timestamp) != 0) return 0;
    if (pb_write_string(&w, 14, m->error) != 0) return 0;
    for (i = 0; i < m->n_meta && i < PB_META_PAIRS; i++)
        if (pb_write_map_ss(&w, 16, m->meta[i].key, m->meta[i].val) != 0) return 0;
    if (pb_write_string(&w, 17, m->speech_text) != 0) return 0;
    if (pb_write_string(&w, 18, m->display_text) != 0) return 0;
    return done(&w);
}

int pb_dec_turn_event(const uint8_t *in, size_t n, pb_turn_event *m) {
    pb_reader r;
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (wire == 2) {
            if (field == 1) {
                if (pb_read_string(&r, m->request_id, sizeof(m->request_id)) != 0) return -1;
            } else if (field == 2) {
                if (pb_read_string(&r, m->user_id, sizeof(m->user_id)) != 0) return -1;
            } else if (field == 5) {
                if (pb_read_string(&r, m->text, sizeof(m->text)) != 0) return -1;
            } else if (field == 14) {
                if (pb_read_string(&r, m->error, sizeof(m->error)) != 0) return -1;
            } else if (field == 16) {
                const uint8_t *entry;
                size_t elen;
                if (pb_read_bytes(&r, &entry, &elen) != 0) return -1;
                if (m->n_meta < PB_META_PAIRS) {
                    pb_get_string(entry, elen, 1, m->meta[m->n_meta].key, sizeof(m->meta[0].key));
                    pb_get_string(entry, elen, 2, m->meta[m->n_meta].val, sizeof(m->meta[0].val));
                    m->n_meta++;
                }
            } else if (field == 17) {
                if (pb_read_string(&r, m->speech_text, sizeof(m->speech_text)) != 0) return -1;
            } else if (field == 18) {
                if (pb_read_string(&r, m->display_text, sizeof(m->display_text)) != 0) return -1;
            } else if (pb_skip(&r, wire) != 0)
                return -1;
            continue;
        }
        if (wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0) return -1;
            if (field == 3) m->type = (int)v;
            else if (field == 4) m->state = (int)v;
            else if (field == 7) m->sample_rate = (int32_t)v;
            else if (field == 8) m->channels = (int32_t)v;
            else if (field == 9) m->bit_depth = (int32_t)v;
            else if (field == 10) m->sequence = (int32_t)v;
            else if (field == 11) m->segment_index = (int32_t)v;
            else if (field == 12) m->is_final = (int)v;
            else if (field == 13) m->timestamp = (int64_t)v;
            continue;
        }
        if (pb_skip(&r, wire) != 0) return -1;
    }
    return 0;
}

/* ChatRequest fields approximate: 1 request_id, 2 user_id, 3 session, 4 message */
size_t pb_enc_chat_req(uint8_t *out, size_t cap, const pb_chat_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->message) != 0) return 0;
    return done(&w);
}

int pb_dec_chat_req(const uint8_t *in, size_t n, pb_chat_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->request_id, sizeof(m->request_id));
    pb_get_string(in, n, 2, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 3, m->session_id, sizeof(m->session_id));
    pb_get_string(in, n, 4, m->message, sizeof(m->message));
    return 0;
}

size_t pb_enc_chat_resp(uint8_t *out, size_t cap, const pb_chat_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->response) != 0) return 0;
    if (pb_write_string(&w, 4, m->response_text) != 0) return 0;
    if (pb_write_string(&w, 5, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_chat_resp(const uint8_t *in, size_t n, pb_chat_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->request_id, sizeof(m->request_id));
    pb_get_string(in, n, 2, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 3, m->response, sizeof(m->response));
    pb_get_string(in, n, 4, m->response_text, sizeof(m->response_text));
    pb_get_string(in, n, 5, m->error, sizeof(m->error));
    return 0;
}

/* LoginEvent: 1 user_id, 2 username, 3 nickname, 4 premium, 5 timestamp */
size_t pb_enc_login_event(uint8_t *out, size_t cap, const pb_login_event *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->username) != 0) return 0;
    if (pb_write_string(&w, 3, m->nickname) != 0) return 0;
    if (pb_write_bool(&w, 4, m->premium) != 0) return 0;
    if (pb_write_int64(&w, 5, m->timestamp) != 0) return 0;
    return done(&w);
}

int pb_dec_login_event(const uint8_t *in, size_t n, pb_login_event *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 2, m->username, sizeof(m->username));
    pb_get_string(in, n, 3, m->nickname, sizeof(m->nickname));
    m->premium = pb_get_bool(in, n, 4);
    m->timestamp = pb_get_int(in, n, 5);
    return 0;
}

/* GreetingRequest: 1 user_id, 2 username, 3 nickname, 4 premium */
size_t pb_enc_greeting_req(uint8_t *out, size_t cap, const pb_greeting_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->username) != 0) return 0;
    if (pb_write_string(&w, 3, m->nickname) != 0) return 0;
    if (pb_write_bool(&w, 4, m->premium) != 0) return 0;
    return done(&w);
}

int pb_dec_greeting_req(const uint8_t *in, size_t n, pb_greeting_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 2, m->username, sizeof(m->username));
    pb_get_string(in, n, 3, m->nickname, sizeof(m->nickname));
    m->premium = pb_get_bool(in, n, 4);
    return 0;
}

/* GreetingResponse: 1 user_id, 2 greeting; dual-run error as field 8 (mirrors ChatResponse.error) */
size_t pb_enc_greeting_resp(uint8_t *out, size_t cap, const pb_greeting_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->greeting) != 0) return 0;
    if (pb_write_string(&w, 8, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_greeting_resp(const uint8_t *in, size_t n, pb_greeting_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 2, m->greeting, sizeof(m->greeting));
    pb_get_string(in, n, 8, m->error, sizeof(m->error));
    return 0;
}

/* ChatStreamChunk: 1 request_id, 2 type, 3 content, 4 done, 5 timestamp; dual-run error=6 */
size_t pb_enc_chat_stream_chunk(uint8_t *out, size_t cap, const pb_chat_stream_chunk *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->request_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->type) != 0) return 0;
    if (pb_write_string(&w, 3, m->content) != 0) return 0;
    if (pb_write_bool(&w, 4, m->done) != 0) return 0;
    if (pb_write_int64(&w, 5, m->timestamp) != 0) return 0;
    if (pb_write_string(&w, 6, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_chat_stream_chunk(const uint8_t *in, size_t n, pb_chat_stream_chunk *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->request_id, sizeof(m->request_id));
    pb_get_string(in, n, 2, m->type, sizeof(m->type));
    pb_get_string(in, n, 3, m->content, sizeof(m->content));
    m->done = pb_get_bool(in, n, 4);
    m->timestamp = pb_get_int(in, n, 5);
    pb_get_string(in, n, 6, m->error, sizeof(m->error));
    return 0;
}

/* Agent task — field numbers match messages.proto */
size_t pb_enc_agent_start_req(uint8_t *out, size_t cap, const pb_agent_start_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->task_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->idempotency_key) != 0) return 0;
    if (pb_write_string(&w, 3, m->parent_turn_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 5, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 6, m->profile) != 0) return 0;
    if (pb_write_string(&w, 7, m->agent_id) != 0) return 0;
    if (pb_write_string(&w, 8, m->input_json) != 0) return 0;
    if (pb_write_int64(&w, 9, m->deadline_unix_ms) != 0) return 0;
    return done(&w);
}

int pb_dec_agent_start_req(const uint8_t *in, size_t n, pb_agent_start_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->task_id, sizeof(m->task_id));
    pb_get_string(in, n, 2, m->idempotency_key, sizeof(m->idempotency_key));
    pb_get_string(in, n, 3, m->parent_turn_id, sizeof(m->parent_turn_id));
    pb_get_string(in, n, 4, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 5, m->session_id, sizeof(m->session_id));
    pb_get_string(in, n, 6, m->profile, sizeof(m->profile));
    pb_get_string(in, n, 7, m->agent_id, sizeof(m->agent_id));
    pb_get_string(in, n, 8, m->input_json, sizeof(m->input_json));
    m->deadline_unix_ms = pb_get_int(in, n, 9);
    return 0;
}

size_t pb_enc_agent_start_resp(uint8_t *out, size_t cap, const pb_agent_start_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->task_id) != 0) return 0;
    if (pb_write_bool(&w, 2, m->accepted) != 0) return 0;
    if (pb_write_enum(&w, 3, m->state) != 0) return 0;
    if (pb_write_string(&w, 4, m->event_subject) != 0) return 0;
    if (pb_write_int64(&w, 5, m->accepted_at) != 0) return 0;
    if (pb_write_string(&w, 6, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_agent_start_resp(const uint8_t *in, size_t n, pb_agent_start_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->task_id, sizeof(m->task_id));
    m->accepted = pb_get_bool(in, n, 2);
    m->state = (int)pb_get_int(in, n, 3);
    pb_get_string(in, n, 4, m->event_subject, sizeof(m->event_subject));
    m->accepted_at = pb_get_int(in, n, 5);
    pb_get_string(in, n, 6, m->error, sizeof(m->error));
    return 0;
}

size_t pb_enc_agent_cancel_req(uint8_t *out, size_t cap, const pb_agent_cancel_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->task_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->reason) != 0) return 0;
    return done(&w);
}

int pb_dec_agent_cancel_req(const uint8_t *in, size_t n, pb_agent_cancel_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->task_id, sizeof(m->task_id));
    pb_get_string(in, n, 2, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 3, m->reason, sizeof(m->reason));
    return 0;
}

size_t pb_enc_agent_event(uint8_t *out, size_t cap, const pb_agent_event *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->task_id) != 0) return 0;
    if (pb_write_string(&w, 5, m->agent_id) != 0) return 0;
    if (pb_write_enum(&w, 6, m->state) != 0) return 0;
    if (pb_write_enum(&w, 7, m->type) != 0) return 0;
    if (pb_write_int64(&w, 8, m->sequence) != 0) return 0;
    if (pb_write_string(&w, 9, m->text) != 0) return 0;
    if (pb_write_string(&w, 12, m->error) != 0) return 0;
    if (pb_write_int64(&w, 13, m->timestamp) != 0) return 0;
    return done(&w);
}

int pb_dec_agent_event(const uint8_t *in, size_t n, pb_agent_event *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->task_id, sizeof(m->task_id));
    pb_get_string(in, n, 5, m->agent_id, sizeof(m->agent_id));
    m->state = (int)pb_get_int(in, n, 6);
    m->type = (int)pb_get_int(in, n, 7);
    m->sequence = (int32_t)pb_get_int(in, n, 8);
    pb_get_string(in, n, 9, m->text, sizeof(m->text));
    pb_get_string(in, n, 12, m->error, sizeof(m->error));
    m->timestamp = pb_get_int(in, n, 13);
    return 0;
}

size_t pb_enc_agent_dispatch_req(uint8_t *out, size_t cap, const pb_agent_dispatch_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->task_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->agent_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->input_json) != 0) return 0;
    if (pb_write_int64(&w, 4, m->deadline_unix_ms) != 0) return 0;
    if (pb_write_int64(&w, 5, m->attempt) != 0) return 0;
    return done(&w);
}

/* Tool call */
size_t pb_enc_tool_start_req(uint8_t *out, size_t cap, const pb_tool_start_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->idempotency_key) != 0) return 0;
    if (pb_write_string(&w, 3, m->parent_task_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->parent_turn_id) != 0) return 0;
    if (pb_write_string(&w, 5, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 6, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 7, m->agent_id) != 0) return 0;
    if (pb_write_string(&w, 8, m->tool_id) != 0) return 0;
    if (pb_write_string(&w, 9, m->input_json) != 0) return 0;
    if (pb_write_int64(&w, 10, m->deadline_unix_ms) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_start_req(const uint8_t *in, size_t n, pb_tool_start_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    pb_get_string(in, n, 2, m->idempotency_key, sizeof(m->idempotency_key));
    pb_get_string(in, n, 3, m->parent_task_id, sizeof(m->parent_task_id));
    pb_get_string(in, n, 4, m->parent_turn_id, sizeof(m->parent_turn_id));
    pb_get_string(in, n, 5, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 6, m->session_id, sizeof(m->session_id));
    pb_get_string(in, n, 7, m->agent_id, sizeof(m->agent_id));
    pb_get_string(in, n, 8, m->tool_id, sizeof(m->tool_id));
    pb_get_string(in, n, 9, m->input_json, sizeof(m->input_json));
    m->deadline_unix_ms = pb_get_int(in, n, 10);
    return 0;
}

size_t pb_enc_tool_start_resp(uint8_t *out, size_t cap, const pb_tool_start_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_bool(&w, 2, m->accepted) != 0) return 0;
    if (pb_write_enum(&w, 3, m->state) != 0) return 0;
    if (pb_write_string(&w, 4, m->event_subject) != 0) return 0;
    if (pb_write_int64(&w, 5, m->accepted_at) != 0) return 0;
    if (pb_write_string(&w, 6, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_start_resp(const uint8_t *in, size_t n, pb_tool_start_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    m->accepted = pb_get_bool(in, n, 2);
    m->state = (int)pb_get_int(in, n, 3);
    pb_get_string(in, n, 4, m->event_subject, sizeof(m->event_subject));
    m->accepted_at = pb_get_int(in, n, 5);
    pb_get_string(in, n, 6, m->error, sizeof(m->error));
    return 0;
}

size_t pb_enc_tool_cancel_req(uint8_t *out, size_t cap, const pb_tool_cancel_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->reason) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_cancel_req(const uint8_t *in, size_t n, pb_tool_cancel_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    pb_get_string(in, n, 2, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 3, m->reason, sizeof(m->reason));
    return 0;
}

size_t pb_enc_tool_approval_req(uint8_t *out, size_t cap, const pb_tool_approval_req *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_bool(&w, 2, m->approved) != 0) return 0;
    if (pb_write_string(&w, 3, m->approver_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->reason) != 0) return 0;
    if (pb_write_int64(&w, 5, m->decided_at) != 0) return 0;
    if (m->approval_id[0] && pb_write_map_ss(&w, 6, "approval_id", m->approval_id) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_approval_req(const uint8_t *in, size_t n, pb_tool_approval_req *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    m->approved = pb_get_bool(in, n, 2);
    pb_get_string(in, n, 3, m->approver_id, sizeof(m->approver_id));
    pb_get_string(in, n, 4, m->reason, sizeof(m->reason));
    m->decided_at = pb_get_int(in, n, 5);
    pb_get_map_ss(in, n, 6, "approval_id", m->approval_id, sizeof(m->approval_id));
    return 0;
}

size_t pb_enc_tool_dispatch_resp(uint8_t *out, size_t cap, const pb_tool_dispatch_resp *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_bool(&w, 2, m->accepted) != 0) return 0;
    if (pb_write_string(&w, 3, m->output_json) != 0) return 0;
    if (pb_write_string(&w, 5, m->summary) != 0) return 0;
    if (pb_write_string(&w, 6, m->error) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_dispatch_resp(const uint8_t *in, size_t n, pb_tool_dispatch_resp *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    m->accepted = pb_get_bool(in, n, 2);
    pb_get_string(in, n, 3, m->output_json, sizeof(m->output_json));
    pb_get_string(in, n, 5, m->summary, sizeof(m->summary));
    pb_get_string(in, n, 6, m->error, sizeof(m->error));
    return 0;
}

size_t pb_enc_tool_event(uint8_t *out, size_t cap, const pb_tool_event *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->tool_call_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->parent_task_id) != 0) return 0;
    if (pb_write_string(&w, 3, m->parent_turn_id) != 0) return 0;
    if (pb_write_string(&w, 4, m->user_id) != 0) return 0;
    if (pb_write_string(&w, 5, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 6, m->agent_id) != 0) return 0;
    if (pb_write_string(&w, 7, m->tool_id) != 0) return 0;
    if (pb_write_enum(&w, 8, m->state) != 0) return 0;
    if (pb_write_enum(&w, 9, m->type) != 0) return 0;
    if (pb_write_int64(&w, 11, m->sequence) != 0) return 0;
    if (pb_write_string(&w, 12, m->text) != 0) return 0;
    if (pb_write_string(&w, 15, m->error) != 0) return 0;
    if (pb_write_int64(&w, 16, m->timestamp) != 0) return 0;
    return done(&w);
}

int pb_dec_tool_event(const uint8_t *in, size_t n, pb_tool_event *m) {
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->tool_call_id, sizeof(m->tool_call_id));
    pb_get_string(in, n, 2, m->parent_task_id, sizeof(m->parent_task_id));
    pb_get_string(in, n, 3, m->parent_turn_id, sizeof(m->parent_turn_id));
    pb_get_string(in, n, 4, m->user_id, sizeof(m->user_id));
    pb_get_string(in, n, 5, m->session_id, sizeof(m->session_id));
    pb_get_string(in, n, 6, m->agent_id, sizeof(m->agent_id));
    pb_get_string(in, n, 7, m->tool_id, sizeof(m->tool_id));
    m->state = (int)pb_get_int(in, n, 8);
    m->type = (int)pb_get_int(in, n, 9);
    m->sequence = (int32_t)pb_get_int(in, n, 11);
    pb_get_string(in, n, 12, m->text, sizeof(m->text));
    pb_get_string(in, n, 15, m->error, sizeof(m->error));
    m->timestamp = pb_get_int(in, n, 16);
    return 0;
}

/* STT — field numbers match messages.proto STTStreamMessage / STTLifecycleEvent */
size_t pb_enc_stt_stream(uint8_t *out, size_t cap, const pb_stt_stream *m) {
    pb_writer w;
    if (!m) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->type) != 0) return 0;
    if (m->audio && m->audio_len && pb_write_bytes(&w, 2, m->audio, m->audio_len) != 0) return 0;
    if (pb_write_int64(&w, 5, m->sample_rate) != 0) return 0;
    if (pb_write_int64(&w, 6, m->channels) != 0) return 0;
    if (pb_write_int64(&w, 7, m->bit_depth) != 0) return 0;
    if (pb_write_string(&w, 3, m->state) != 0) return 0;
    if (pb_write_string(&w, 4, m->speaker_id) != 0) return 0;
    if (pb_write_string(&w, 11, m->utterance_id) != 0) return 0;
    return done(&w);
}

int pb_dec_stt_stream(const uint8_t *in, size_t n, pb_stt_stream *m) {
    pb_reader r;
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        if (pb_read_tag(&r, &field, &wire) != 0) return -1;
        if (field == 1 && wire == 2) {
            if (pb_read_string(&r, m->type, sizeof(m->type)) != 0) return -1;
        } else if (field == 2 && wire == 2) {
            if (pb_read_bytes(&r, &m->audio, &m->audio_len) != 0) return -1;
        } else if (wire == 0) {
            uint64_t v;
            if (pb_read_varint(&r, &v) != 0) return -1;
            if (field == 5) m->sample_rate = (int32_t)v;
            else if (field == 6) m->channels = (int32_t)v;
            else if (field == 7) m->bit_depth = (int32_t)v;
        } else if (field == 3 && wire == 2) {
            if (pb_read_string(&r, m->state, sizeof(m->state)) != 0) return -1;
        } else if (field == 4 && wire == 2) {
            if (pb_read_string(&r, m->speaker_id, sizeof(m->speaker_id)) != 0) return -1;
        } else if (field == 11 && wire == 2) {
            if (pb_read_string(&r, m->utterance_id, sizeof(m->utterance_id)) != 0) return -1;
        } else if (pb_skip(&r, wire) != 0)
            return -1;
    }
    return 0;
}

size_t pb_enc_stt_lifecycle(uint8_t *out, size_t cap, const pb_stt_lifecycle *m) {
    pb_writer w;
    int type_id;
    if (!m) return 0;
    type_id = m->type_id;
    if (type_id == 0 && strcmp(m->type, "stream_started") == 0) type_id = 1;
    else if (type_id == 0 && strcmp(m->type, "speech_started") == 0) type_id = 2;
    else if (type_id == 0 && strcmp(m->type, "speech_ended") == 0) type_id = 3;
    else if (type_id == 0 && strcmp(m->type, "stream_ended") == 0) type_id = 4;
    if (type_id < 1 || type_id > 4 || m->timestamp_ms <= 1) return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->session_id) != 0) return 0;
    if (pb_write_string(&w, 2, m->utterance_id) != 0) return 0;
    if (pb_write_enum(&w, 3, type_id) != 0) return 0;
    if (pb_write_int64(&w, 5, m->timestamp_ms) != 0) return 0;
    return done(&w);
}

int pb_dec_stt_lifecycle(const uint8_t *in, size_t n, pb_stt_lifecycle *m) {
    static const char *const names[] = {
        "", "stream_started", "speech_started", "speech_ended", "stream_ended"
    };
    if (!m) return -1;
    zero(m, sizeof(*m));
    pb_get_string(in, n, 1, m->session_id, sizeof(m->session_id));
    pb_get_string(in, n, 2, m->utterance_id, sizeof(m->utterance_id));
    m->type_id = (int)pb_get_int(in, n, 3);
    m->timestamp_ms = pb_get_int(in, n, 5);
    if (m->type_id < 1 || m->type_id > 4 || m->timestamp_ms <= 1) return -1;
    memcpy(m->type, names[m->type_id], strlen(names[m->type_id]) + 1);
    return 0;
}

size_t pb_enc_dnd_vision(uint8_t *out, size_t cap, const pb_dnd_vision_event *m) {
    pb_writer w;
    if (!m)
        return 0;
    pb_writer_init(&w, out, cap);
    if (pb_write_string(&w, 1, m->session_id) != 0)
        return 0;
    if (pb_write_uint64(&w, 2, m->sequence) != 0)
        return 0;
    if (pb_write_uint64(&w, 3, m->observed_at_mono_ns) != 0)
        return 0;
    if (pb_write_int64(&w, 4, m->published_at_unix_ms) != 0)
        return 0;
    if (m->person_count && pb_write_uint64(&w, 5, m->person_count) != 0)
        return 0;
    if (pb_write_float(&w, 6, m->motion_energy) != 0)
        return 0;
    if (pb_write_enum(&w, 7, m->gaze_cluster) != 0)
        return 0;
    if (m->scene_changed && pb_write_bool(&w, 8, 1) != 0)
        return 0;
    if (pb_write_string(&w, 9, m->policy_version) != 0)
        return 0;
    if (m->frame_width && pb_write_uint64(&w, 10, m->frame_width) != 0)
        return 0;
    if (m->frame_height && pb_write_uint64(&w, 11, m->frame_height) != 0)
        return 0;
    return done(&w);
}

int pb_dec_dnd_vision(const uint8_t *in, size_t n, pb_dnd_vision_event *m) {
    pb_reader r;
    if (!m)
        return -1;
    zero(m, sizeof(*m));
    pb_reader_init(&r, in, n);
    while (r.pos < r.len) {
        uint32_t field, wire;
        uint64_t v;
        if (pb_read_tag(&r, &field, &wire) != 0)
            return -1;
        if (wire == 2) {
            if (field == 1) {
                if (pb_read_string(&r, m->session_id, sizeof(m->session_id)) != 0)
                    return -1;
            } else if (field == 9) {
                if (pb_read_string(&r, m->policy_version, sizeof(m->policy_version)) != 0)
                    return -1;
            } else if (pb_skip(&r, wire) != 0)
                return -1;
            continue;
        }
        if (wire == 0) {
            if (pb_read_varint(&r, &v) != 0)
                return -1;
            if (field == 2)
                m->sequence = v;
            else if (field == 3)
                m->observed_at_mono_ns = v;
            else if (field == 4)
                m->published_at_unix_ms = (int64_t)v;
            else if (field == 5)
                m->person_count = (uint32_t)v;
            else if (field == 7)
                m->gaze_cluster = (int32_t)v;
            else if (field == 8)
                m->scene_changed = v ? 1 : 0;
            else if (field == 10)
                m->frame_width = (uint32_t)v;
            else if (field == 11)
                m->frame_height = (uint32_t)v;
            continue;
        }
        if (wire == 5) {
            uint32_t bits = 0;
            size_t i;
            if (r.pos + 4 > r.len)
                return -1;
            for (i = 0; i < 4; i++)
                bits |= ((uint32_t)r.data[r.pos++]) << (8 * i);
            if (field == 6)
                memcpy(&m->motion_energy, &bits, sizeof(float));
            continue;
        }
        if (pb_skip(&r, wire) != 0)
            return -1;
    }
    return 0;
}
