#include "pt_pb.h"

#include <stdio.h>
#include <string.h>

static size_t put_varint(uint8_t *out, size_t cap, uint64_t v) {
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        if (v) b |= 0x80u;
        if (n >= cap) return 0;
        out[n++] = b;
    } while (v);
    return n;
}

static size_t put_key(uint8_t *out, size_t cap, uint32_t field, uint32_t wire) {
    return put_varint(out, cap, ((uint64_t)field << 3) | wire);
}

static size_t put_string(uint8_t *out, size_t cap, uint32_t field, const char *s) {
    size_t sl, n = 0, k;
    if (!s) s = "";
    sl = strlen(s);
    if (!sl) return 0;
    k = put_key(out + n, cap - n, field, 2);
    if (!k) return 0;
    n += k;
    k = put_varint(out + n, cap - n, sl);
    if (!k) return 0;
    n += k;
    if (n + sl > cap) return 0;
    memcpy(out + n, s, sl);
    return n + sl;
}

static size_t put_int64(uint8_t *out, size_t cap, uint32_t field, int64_t v) {
    size_t n = 0, k;
    if (v == 0) return 0;
    k = put_key(out + n, cap - n, field, 0);
    if (!k) return 0;
    n += k;
    k = put_varint(out + n, cap - n, (uint64_t)v);
    if (!k) return 0;
    return n + k;
}

static size_t put_bool(uint8_t *out, size_t cap, uint32_t field, int v) {
    size_t n = 0, k;
    /* always emit accepted/approved so false is visible */
    k = put_key(out + n, cap - n, field, 0);
    if (!k) return 0;
    n += k;
    if (n >= cap) return 0;
    out[n++] = v ? 1 : 0;
    return n;
}

static size_t put_enum(uint8_t *out, size_t cap, uint32_t field, int v) {
    return put_int64(out, cap, field, v);
}

static int read_varint(const uint8_t *in, size_t n, size_t *off, uint64_t *v) {
    uint64_t x = 0;
    int shift = 0;
    while (*off < n) {
        uint8_t b = in[(*off)++];
        x |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *v = x;
            return 0;
        }
        shift += 7;
        if (shift > 63) return -1;
    }
    return -1;
}

static int get_string_field(const uint8_t *in, size_t n, uint32_t want, char *out, size_t cap) {
    size_t off = 0;
    out[0] = '\0';
    while (off < n) {
        uint64_t key, len;
        uint32_t field, wire;
        if (read_varint(in, n, &off, &key) != 0) return -1;
        field = (uint32_t)(key >> 3);
        wire = (uint32_t)(key & 7);
        if (wire == 0) {
            if (read_varint(in, n, &off, &len) != 0) return -1;
            continue;
        }
        if (wire == 1) {
            off += 8;
            continue;
        }
        if (wire == 5) {
            off += 4;
            continue;
        }
        if (wire != 2) return -1;
        if (read_varint(in, n, &off, &len) != 0) return -1;
        if (off + len > n) return -1;
        if (field == want) {
            size_t copy = (size_t)len;
            if (copy >= cap) copy = cap - 1;
            memcpy(out, in + off, copy);
            out[copy] = '\0';
            return 0;
        }
        off += (size_t)len;
    }
    return -1;
}

static int64_t get_int_field(const uint8_t *in, size_t n, uint32_t want) {
    size_t off = 0;
    while (off < n) {
        uint64_t key, val, len;
        uint32_t field, wire;
        if (read_varint(in, n, &off, &key) != 0) return 0;
        field = (uint32_t)(key >> 3);
        wire = (uint32_t)(key & 7);
        if (wire == 0) {
            if (read_varint(in, n, &off, &val) != 0) return 0;
            if (field == want) return (int64_t)val;
            continue;
        }
        if (wire == 1) {
            off += 8;
            continue;
        }
        if (wire == 5) {
            off += 4;
            continue;
        }
        if (wire != 2) return 0;
        if (read_varint(in, n, &off, &len) != 0) return 0;
        if (off + len > n) return 0;
        off += (size_t)len;
    }
    return 0;
}

static int has_field(const uint8_t *in, size_t n, uint32_t want) {
    size_t off = 0;
    while (off < n) {
        uint64_t key, value;
        uint32_t field, wire;
        if (read_varint(in, n, &off, &key) != 0) return 0;
        field = (uint32_t)(key >> 3);
        wire = (uint32_t)(key & 7);
        if (field == want) return 1;
        if (wire == 0) {
            if (read_varint(in, n, &off, &value) != 0) return 0;
        } else if (wire == 1) {
            if (off + 8 > n) return 0;
            off += 8;
        } else if (wire == 2) {
            if (read_varint(in, n, &off, &value) != 0 || off + value > n) return 0;
            off += (size_t)value;
        } else if (wire == 5) {
            if (off + 4 > n) return 0;
            off += 4;
        } else {
            return 0;
        }
    }
    return 0;
}

/* map<string,string> field: repeated message with key=1, value=2 */
static void get_map_string(const uint8_t *in, size_t n, uint32_t map_field, const char *want_key,
                           char *out, size_t cap) {
    size_t off = 0;
    out[0] = '\0';
    while (off < n) {
        uint64_t key, len;
        uint32_t field, wire;
        if (read_varint(in, n, &off, &key) != 0) return;
        field = (uint32_t)(key >> 3);
        wire = (uint32_t)(key & 7);
        if (wire == 0) {
            if (read_varint(in, n, &off, &len) != 0) return;
            continue;
        }
        if (wire == 1) {
            off += 8;
            continue;
        }
        if (wire == 5) {
            off += 4;
            continue;
        }
        if (wire != 2) return;
        if (read_varint(in, n, &off, &len) != 0) return;
        if (off + len > n) return;
        if (field == map_field) {
            char k[PT_ID], v[PT_ID];
            k[0] = v[0] = '\0';
            get_string_field(in + off, (size_t)len, 1, k, sizeof(k));
            get_string_field(in + off, (size_t)len, 2, v, sizeof(v));
            if (k[0] && want_key && strcmp(k, want_key) == 0) {
                snprintf(out, cap, "%s", v);
                return;
            }
        }
        off += (size_t)len;
    }
}

static size_t put_map_entry(uint8_t *out, size_t cap, uint32_t map_field, const char *k,
                            const char *v) {
    uint8_t entry[512];
    size_t en = 0, n = 0, kk;
    if (!k || !k[0] || !v) return 0;
    en += put_string(entry + en, sizeof(entry) - en, 1, k);
    en += put_string(entry + en, sizeof(entry) - en, 2, v);
    if (!en) return 0;
    kk = put_key(out + n, cap - n, map_field, 2);
    if (!kk) return 0;
    n += kk;
    kk = put_varint(out + n, cap - n, en);
    if (!kk) return 0;
    n += kk;
    if (n + en > cap) return 0;
    memcpy(out + n, entry, en);
    return n + en;
}

int pt_pb_dec_start_req(const uint8_t *in, size_t n, pt_start_req *out) {
    if (!in || !out) return -1;
    memset(out, 0, sizeof(*out));
    get_string_field(in, n, 1, out->tool_call_id, sizeof(out->tool_call_id));
    get_string_field(in, n, 2, out->idempotency_key, sizeof(out->idempotency_key));
    get_string_field(in, n, 3, out->parent_task_id, sizeof(out->parent_task_id));
    get_string_field(in, n, 4, out->parent_turn_id, sizeof(out->parent_turn_id));
    get_string_field(in, n, 5, out->user_id, sizeof(out->user_id));
    get_string_field(in, n, 6, out->session_id, sizeof(out->session_id));
    get_string_field(in, n, 7, out->agent_id, sizeof(out->agent_id));
    get_string_field(in, n, 8, out->tool_id, sizeof(out->tool_id));
    get_string_field(in, n, 9, out->input_json, sizeof(out->input_json));
    out->deadline_unix_ms = get_int_field(in, n, 10);
    out->caller_tool_present = has_field(in, n, 11);
    return 0;
}

size_t pt_pb_enc_start_req(uint8_t *out, size_t cap, const pt_start_req *r) {
    size_t n = 0;
    if (!out || !r) return 0;
    n += put_string(out + n, cap - n, 1, r->tool_call_id);
    n += put_string(out + n, cap - n, 2, r->idempotency_key);
    n += put_string(out + n, cap - n, 3, r->parent_task_id);
    n += put_string(out + n, cap - n, 4, r->parent_turn_id);
    n += put_string(out + n, cap - n, 5, r->user_id);
    n += put_string(out + n, cap - n, 6, r->session_id);
    n += put_string(out + n, cap - n, 7, r->agent_id);
    n += put_string(out + n, cap - n, 8, r->tool_id);
    n += put_string(out + n, cap - n, 9, r->input_json);
    n += put_int64(out + n, cap - n, 10, r->deadline_unix_ms);
    return n;
}

size_t pt_pb_enc_start_resp(uint8_t *out, size_t cap, const pt_start_resp *r) {
    size_t n = 0;
    if (!out || !r) return 0;
    n += put_string(out + n, cap - n, 1, r->tool_call_id);
    n += put_bool(out + n, cap - n, 2, r->accepted);
    n += put_enum(out + n, cap - n, 3, r->state);
    n += put_string(out + n, cap - n, 4, r->event_subject);
    n += put_int64(out + n, cap - n, 5, r->accepted_at);
    n += put_string(out + n, cap - n, 6, r->error);
    return n;
}

int pt_pb_dec_cancel_req(const uint8_t *in, size_t n, pt_cancel_req *out) {
    if (!in || !out) return -1;
    memset(out, 0, sizeof(*out));
    get_string_field(in, n, 1, out->tool_call_id, sizeof(out->tool_call_id));
    get_string_field(in, n, 2, out->user_id, sizeof(out->user_id));
    get_string_field(in, n, 3, out->reason, sizeof(out->reason));
    return 0;
}

size_t pt_pb_enc_cancel_req(uint8_t *out, size_t cap, const pt_cancel_req *r) {
    size_t n = 0;
    if (!out || !r) return 0;
    n += put_string(out + n, cap - n, 1, r->tool_call_id);
    n += put_string(out + n, cap - n, 2, r->user_id);
    n += put_string(out + n, cap - n, 3, r->reason);
    return n;
}

int pt_pb_dec_approval_req(const uint8_t *in, size_t n, pt_approval_req *out) {
    if (!in || !out) return -1;
    memset(out, 0, sizeof(*out));
    get_string_field(in, n, 1, out->tool_call_id, sizeof(out->tool_call_id));
    out->approved = (int)get_int_field(in, n, 2);
    get_string_field(in, n, 3, out->approver_id, sizeof(out->approver_id));
    get_string_field(in, n, 4, out->reason, sizeof(out->reason));
    out->decided_at = get_int_field(in, n, 5);
    get_map_string(in, n, 6, "approval_id", out->approval_id, sizeof(out->approval_id));
    return 0;
}

size_t pt_pb_enc_approval_req(uint8_t *out, size_t cap, const pt_approval_req *r) {
    size_t n = 0;
    if (!out || !r) return 0;
    n += put_string(out + n, cap - n, 1, r->tool_call_id);
    n += put_bool(out + n, cap - n, 2, r->approved);
    n += put_string(out + n, cap - n, 3, r->approver_id);
    n += put_string(out + n, cap - n, 4, r->reason);
    n += put_int64(out + n, cap - n, 5, r->decided_at);
    if (r->approval_id[0])
        n += put_map_entry(out + n, cap - n, 6, "approval_id", r->approval_id);
    return n;
}

size_t pt_pb_enc_dispatch_resp(uint8_t *out, size_t cap, const pt_dispatch_resp *r) {
    size_t n = 0;
    if (!out || !r) return 0;
    n += put_string(out + n, cap - n, 1, r->tool_call_id);
    n += put_bool(out + n, cap - n, 2, r->accepted);
    n += put_string(out + n, cap - n, 3, r->output_json);
    n += put_string(out + n, cap - n, 5, r->summary);
    n += put_string(out + n, cap - n, 6, r->error);
    return n;
}

size_t pt_pb_enc_event(uint8_t *out, size_t cap, const pt_event *e) {
    size_t n = 0;
    if (!out || !e) return 0;
    n += put_string(out + n, cap - n, 1, e->tool_call_id);
    n += put_string(out + n, cap - n, 2, e->parent_task_id);
    n += put_string(out + n, cap - n, 3, e->parent_turn_id);
    n += put_string(out + n, cap - n, 4, e->user_id);
    n += put_string(out + n, cap - n, 5, e->session_id);
    n += put_string(out + n, cap - n, 6, e->agent_id);
    n += put_string(out + n, cap - n, 7, e->tool_id);
    n += put_enum(out + n, cap - n, 8, e->state);
    n += put_enum(out + n, cap - n, 9, e->type);
    n += put_int64(out + n, cap - n, 11, e->sequence);
    n += put_string(out + n, cap - n, 12, e->text);
    n += put_string(out + n, cap - n, 15, e->error);
    n += put_int64(out + n, cap - n, 16, e->timestamp);
    return n;
}
