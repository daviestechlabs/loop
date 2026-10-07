#include "pb_msg.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "FAIL %s\n", m);
        fails++;
    }
}

int main(void) {
    uint8_t buf[8192];
    size_t n;
    pb_turn_start_req a, b;
    pb_tool_start_req t1, t2;
    pb_tool_approval_req ap1, ap2;
    pb_stt_stream s1, s2;
    uint8_t pcm[4] = {1, 2, 3, 4};

    {
        const uint8_t oversized[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 2};
        const uint8_t wrapped_len[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1};
        const uint8_t oversized_tag[] = {0x88, 0x80, 0x80, 0x80, 0x10};
        pb_reader reader;
        uint64_t value;
        uint32_t field, wire;
        const uint8_t *bytes;
        size_t length;
        pb_reader_init(&reader, oversized, sizeof(oversized));
        expect(pb_read_varint(&reader, &value) != 0, "reject uint64 overflow");
        pb_reader_init(&reader, wrapped_len, sizeof(wrapped_len));
        expect(pb_read_bytes(&reader, &bytes, &length) != 0, "reject wrapped byte length");
        pb_reader_init(&reader, oversized_tag, sizeof(oversized_tag));
        expect(pb_read_tag(&reader, &field, &wire) != 0, "reject truncated field number");
        pb_reader_init(&reader, (const uint8_t *)"\0", 1u);
        expect(pb_read_tag(&reader, &field, &wire) != 0, "reject zero field number");
    }

    memset(&a, 0, sizeof(a));
    snprintf(a.request_id, sizeof(a.request_id), "r1");
    snprintf(a.text, sizeof(a.text), "hi");
    a.enable_tts = 1;
    n = pb_enc_turn_start_req(buf, sizeof(buf), &a);
    expect(n > 0, "enc turn");
    expect(pb_dec_turn_start_req(buf, n, &b) == 0, "dec turn");
    expect(strcmp(b.request_id, "r1") == 0, "id");
    expect(strcmp(b.text, "hi") == 0, "text");
    expect(b.enable_tts == 1, "tts flag");
    {
        int i;
        pb_writer writer;
        memset(&a, 0, sizeof(a));
        snprintf(a.request_id, sizeof(a.request_id), "metadata-cap");
        a.n_meta = PB_TURN_META_PAIRS;
        for (i = 0; i < PB_TURN_META_PAIRS; ++i) {
            snprintf(a.meta[i].key, sizeof(a.meta[i].key), "key-%02d", i);
            snprintf(a.meta[i].val, sizeof(a.meta[i].val), "value-%02d", i);
        }
        n = pb_enc_turn_start_req(buf, sizeof(buf), &a);
        expect(n > 0 && pb_dec_turn_start_req(buf, n, &b) == 0,
               "turn metadata cap roundtrip");
        expect(b.n_meta == PB_TURN_META_PAIRS &&
                   strcmp(b.meta[PB_TURN_META_PAIRS - 1].val, "value-31") == 0,
               "turn metadata cap preserves final entry");
        a.n_meta = PB_TURN_META_PAIRS + 1;
        expect(pb_enc_turn_start_req(buf, sizeof(buf), &a) == 0,
               "turn metadata encoder rejects overflow");

        pb_writer_init(&writer, buf, sizeof(buf));
        for (i = 0; i < PB_TURN_META_PAIRS + 1; ++i) {
            char key[32];
            snprintf(key, sizeof(key), "wire-key-%02d", i);
            expect(pb_write_map_ss(&writer, 12, key, "value") == 0,
                   "encode oversized wire metadata");
        }
        n = pb_writer_len(&writer);
        expect(n > 0 && pb_dec_turn_start_req(buf, n, &b) != 0,
               "turn metadata decoder rejects overflow");

        pb_writer_init(&writer, buf, sizeof(buf));
        expect(pb_write_map_ss(&writer, 12, "campaign_id", "one") == 0 &&
                   pb_write_map_ss(&writer, 12, "campaign_id", "two") == 0,
               "encode duplicate wire metadata");
        expect(pb_dec_turn_start_req(buf, pb_writer_len(&writer), &b) != 0,
               "turn metadata decoder rejects duplicate keys");
        {
            static const char duplicate_key[] = "\x62\x09\x0a\x01x\x12\x01y\x0a\x01z";
            static const char duplicate_value[] = "\x62\x09\x0a\x01x\x12\x01y\x12\x01z";
            static const char nul_key[] = "\x62\x07\x0a\x02x\0\x12\x01y";
            static const char nul_value[] = "\x62\x07\x0a\x01x\x12\x02y\0";
            static const char malformed_tail[] = "\x62\x07\x0a\x01x\x12\x01y\xff";
            static const char wrong_wire[] = "\x62\x05\x08\x01\x12\x01y";
#define REJECT_META_WIRE(value) \
            expect(pb_dec_turn_start_req((const uint8_t *)(value), sizeof(value) - 1u, &b) != 0, \
                   "turn metadata rejects " #value)
            REJECT_META_WIRE(duplicate_key);
            REJECT_META_WIRE(duplicate_value);
            REJECT_META_WIRE(nul_key);
            REJECT_META_WIRE(nul_value);
            REJECT_META_WIRE(malformed_tail);
            REJECT_META_WIRE(wrong_wire);
#undef REJECT_META_WIRE
        }
        {
            char long_key[65];
            memset(long_key, 'k', sizeof(long_key) - 1u);
            long_key[sizeof(long_key) - 1u] = '\0';
            pb_writer_init(&writer, buf, sizeof(buf));
            expect(pb_write_map_ss(&writer, 12, long_key, "value") == 0,
                   "encode oversized wire metadata key");
            expect(pb_dec_turn_start_req(buf, pb_writer_len(&writer), &b) != 0,
                   "turn metadata decoder rejects truncating a key");
        }
    }

    memset(&t1, 0, sizeof(t1));
    snprintf(t1.tool_call_id, sizeof(t1.tool_call_id), "c1");
    snprintf(t1.tool_id, sizeof(t1.tool_id), "workspace-read");
    snprintf(t1.input_json, sizeof(t1.input_json), "{\"paths\":[\"a\"]}");
    n = pb_enc_tool_start_req(buf, sizeof(buf), &t1);
    expect(n > 0, "enc tool");
    expect(pb_dec_tool_start_req(buf, n, &t2) == 0, "dec tool");
    expect(strcmp(t2.tool_id, "workspace-read") == 0, "tool id");
    expect(strstr(t2.input_json, "paths") != NULL, "input");

    memset(&ap1, 0, sizeof(ap1));
    snprintf(ap1.tool_call_id, sizeof(ap1.tool_call_id), "c2");
    ap1.approved = 1;
    snprintf(ap1.approval_id, sizeof(ap1.approval_id), "apr-9");
    n = pb_enc_tool_approval_req(buf, sizeof(buf), &ap1);
    expect(n > 0, "enc approval");
    expect(pb_dec_tool_approval_req(buf, n, &ap2) == 0, "dec approval");
    expect(ap2.approved == 1, "approved");
    expect(strcmp(ap2.approval_id, "apr-9") == 0, "approval_id map");

    memset(&s1, 0, sizeof(s1));
    snprintf(s1.type, sizeof(s1.type), "audio");
    s1.audio = pcm;
    s1.audio_len = 4;
    s1.sample_rate = 16000;
    s1.channels = 1;
    s1.bit_depth = 16;
    n = pb_enc_stt_stream(buf, sizeof(buf), &s1);
    expect(n > 0, "enc stt");
    expect(pb_dec_stt_stream(buf, n, &s2) == 0, "dec stt");
    expect(s2.sample_rate == 16000, "sr");
    expect(s2.audio_len == 4, "audio len");
    expect(s2.audio && s2.audio[0] == 1, "audio data");

    {
        pb_login_event l1, l2;
        pb_greeting_req g1, g2;
        pb_greeting_resp gr1, gr2;
        pb_chat_stream_chunk c1, c2;
        pb_chat_req cr1, cr2;
        pb_turn_cancel_req tc1, tc2;

        memset(&l1, 0, sizeof(l1));
        snprintf(l1.user_id, sizeof(l1.user_id), "u1");
        snprintf(l1.username, sizeof(l1.username), "alice");
        l1.premium = 1;
        l1.timestamp = 1700000000;
        n = pb_enc_login_event(buf, sizeof(buf), &l1);
        expect(n > 0, "enc login");
        expect(pb_dec_login_event(buf, n, &l2) == 0, "dec login");
        expect(strcmp(l2.username, "alice") == 0, "login user");
        expect(l2.premium == 1 && l2.timestamp == 1700000000, "login flags");

        memset(&g1, 0, sizeof(g1));
        snprintf(g1.user_id, sizeof(g1.user_id), "u1");
        snprintf(g1.nickname, sizeof(g1.nickname), "Al");
        n = pb_enc_greeting_req(buf, sizeof(buf), &g1);
        expect(n > 0 && pb_dec_greeting_req(buf, n, &g2) == 0, "greeting req roundtrip");
        expect(strcmp(g2.nickname, "Al") == 0, "greeting nick");

        memset(&gr1, 0, sizeof(gr1));
        snprintf(gr1.greeting, sizeof(gr1.greeting), "hello");
        n = pb_enc_greeting_resp(buf, sizeof(buf), &gr1);
        expect(n > 0 && pb_dec_greeting_resp(buf, n, &gr2) == 0, "greeting resp roundtrip");
        expect(strcmp(gr2.greeting, "hello") == 0, "greeting text");

        memset(&c1, 0, sizeof(c1));
        snprintf(c1.content, sizeof(c1.content), "tok");
        c1.done = 0;
        c1.timestamp = 42;
        n = pb_enc_chat_stream_chunk(buf, sizeof(buf), &c1);
        expect(n > 0 && pb_dec_chat_stream_chunk(buf, n, &c2) == 0, "stream roundtrip");
        expect(strcmp(c2.content, "tok") == 0 && c2.timestamp == 42, "stream fields");

        memset(&cr1, 0, sizeof(cr1));
        snprintf(cr1.request_id, sizeof(cr1.request_id), "r9");
        snprintf(cr1.message, sizeof(cr1.message), "ping");
        n = pb_enc_chat_req(buf, sizeof(buf), &cr1);
        expect(n > 0 && pb_dec_chat_req(buf, n, &cr2) == 0, "chat req");
        expect(strcmp(cr2.message, "ping") == 0, "chat msg");

        memset(&tc1, 0, sizeof(tc1));
        snprintf(tc1.request_id, sizeof(tc1.request_id), "r9");
        snprintf(tc1.reason, sizeof(tc1.reason), "barge");
        n = pb_enc_turn_cancel_req(buf, sizeof(buf), &tc1);
        expect(n > 0 && pb_dec_turn_cancel_req(buf, n, &tc2) == 0, "cancel");
        expect(strcmp(tc2.reason, "barge") == 0, "cancel reason");
    }

    if (fails) {
        fprintf(stderr, "%d fails\n", fails);
        return 1;
    }
    printf("ALL PASS c-pb unit\n");
    return 0;
}
