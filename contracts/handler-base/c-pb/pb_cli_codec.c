/* pb_cli_codec.c — shared kv-line encode/decode for c-pb + c-companions. */
#define _POSIX_C_SOURCE 200809L
#include "pb_cli_codec.h"
#include "pb_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jget_line(const char *blob, const char *key, char *out, size_t cap) {
    char pat[96];
    const char *p, *nl;
    size_t klen, n;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "%s=", key);
    klen = strlen(pat);
    p = blob;
    while (p && *p) {
        if (strncmp(p, pat, klen) == 0) {
            p += klen;
            nl = strchr(p, '\n');
            n = nl ? (size_t)(nl - p) : strlen(p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return;
        }
        nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
}

static int jget_i(const char *blob, const char *key) {
    char tmp[64];
    jget_line(blob, key, tmp, sizeof(tmp));
    return tmp[0] ? atoi(tmp) : 0;
}

static int64_t jget_ll(const char *blob, const char *key) {
    char tmp[64];
    jget_line(blob, key, tmp, sizeof(tmp));
    return tmp[0] ? (int64_t)strtoll(tmp, NULL, 10) : 0;
}

int pb_cli_encode(const char *kind, const char *kv_text, uint8_t *out, size_t out_cap, size_t *out_len) {
    const char *in = kv_text ? kv_text : "";
    size_t on = 0;
    if (!kind || !out || !out_len || out_cap == 0)
        return PB_CLI_ERR;
    *out_len = 0;
if (strcmp(kind, "turn-start") == 0) {
        pb_turn_start_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "session_id", m.session_id, sizeof(m.session_id));
        jget_line(in, "text", m.text, sizeof(m.text));
        jget_line(in, "response_subject", m.response_subject, sizeof(m.response_subject));
        m.premium = jget_i(in, "premium");
        m.enable_rag = jget_i(in, "enable_rag");
        m.enable_tts = jget_i(in, "enable_tts");
        on = pb_enc_turn_start_req(out, out_cap, &m);
    } else if (strcmp(kind, "turn-event") == 0) {
        pb_turn_event m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "text", m.text, sizeof(m.text));
        jget_line(in, "speech_text", m.speech_text, sizeof(m.speech_text));
        jget_line(in, "display_text", m.display_text, sizeof(m.display_text));
        jget_line(in, "error", m.error, sizeof(m.error));
        m.type = jget_i(in, "type");
        m.state = jget_i(in, "state");
        m.sequence = jget_i(in, "sequence");
        m.timestamp = jget_ll(in, "timestamp");
        on = pb_enc_turn_event(out, out_cap, &m);
    } else if (strcmp(kind, "tool-start") == 0) {
        pb_tool_start_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "tool_call_id", m.tool_call_id, sizeof(m.tool_call_id));
        jget_line(in, "tool_id", m.tool_id, sizeof(m.tool_id));
        jget_line(in, "agent_id", m.agent_id, sizeof(m.agent_id));
        jget_line(in, "input_json", m.input_json, sizeof(m.input_json));
        jget_line(in, "idempotency_key", m.idempotency_key, sizeof(m.idempotency_key));
        on = pb_enc_tool_start_req(out, out_cap, &m);
    } else if (strcmp(kind, "agent-start") == 0) {
        pb_agent_start_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "task_id", m.task_id, sizeof(m.task_id));
        jget_line(in, "agent_id", m.agent_id, sizeof(m.agent_id));
        jget_line(in, "input_json", m.input_json, sizeof(m.input_json));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        on = pb_enc_agent_start_req(out, out_cap, &m);
    } else if (strcmp(kind, "stt-lifecycle") == 0) {
        pb_stt_lifecycle m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "session_id", m.session_id, sizeof(m.session_id));
        jget_line(in, "utterance_id", m.utterance_id, sizeof(m.utterance_id));
        jget_line(in, "type", m.type, sizeof(m.type));
        m.timestamp_ms = jget_ll(in, "timestamp_ms");
        on = pb_enc_stt_lifecycle(out, out_cap, &m);
    } else if (strcmp(kind, "dnd-vision") == 0) {
        pb_dnd_vision_event m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "session_id", m.session_id, sizeof(m.session_id));
        jget_line(in, "policy_version", m.policy_version, sizeof(m.policy_version));
        m.sequence = (uint64_t)jget_ll(in, "sequence");
        m.observed_at_mono_ns = (uint64_t)jget_ll(in, "observed_at_mono_ns");
        m.published_at_unix_ms = jget_ll(in, "published_at_unix_ms");
        m.person_count = (uint32_t)jget_i(in, "person_count");
        m.gaze_cluster = jget_i(in, "gaze_cluster");
        m.scene_changed = jget_i(in, "scene_changed");
        m.frame_width = (uint32_t)jget_i(in, "frame_width");
        m.frame_height = (uint32_t)jget_i(in, "frame_height");
        {
            char buf[64];
            jget_line(in, "motion_energy", buf, sizeof(buf));
            if (buf[0])
                m.motion_energy = (float)atof(buf);
        }
        on = pb_enc_dnd_vision(out, out_cap, &m);
    } else if (strcmp(kind, "chat-req") == 0) {
        pb_chat_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "session_id", m.session_id, sizeof(m.session_id));
        jget_line(in, "message", m.message, sizeof(m.message));
        on = pb_enc_chat_req(out, out_cap, &m);
    } else if (strcmp(kind, "chat-resp") == 0) {
        pb_chat_resp m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "response", m.response, sizeof(m.response));
        jget_line(in, "response_text", m.response_text, sizeof(m.response_text));
        jget_line(in, "error", m.error, sizeof(m.error));
        on = pb_enc_chat_resp(out, out_cap, &m);
    } else if (strcmp(kind, "turn-cancel") == 0) {
        pb_turn_cancel_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "reason", m.reason, sizeof(m.reason));
        on = pb_enc_turn_cancel_req(out, out_cap, &m);
    } else if (strcmp(kind, "turn-start-resp") == 0) {
        pb_turn_start_resp m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "event_subject", m.event_subject, sizeof(m.event_subject));
        m.accepted = jget_i(in, "accepted");
        m.state = jget_i(in, "state");
        m.accepted_at = jget_ll(in, "accepted_at");
        on = pb_enc_turn_start_resp(out, out_cap, &m);
    } else if (strcmp(kind, "login") == 0) {
        pb_login_event m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "username", m.username, sizeof(m.username));
        jget_line(in, "nickname", m.nickname, sizeof(m.nickname));
        m.premium = jget_i(in, "premium");
        m.timestamp = jget_ll(in, "timestamp");
        on = pb_enc_login_event(out, out_cap, &m);
    } else if (strcmp(kind, "greeting-req") == 0) {
        pb_greeting_req m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "username", m.username, sizeof(m.username));
        jget_line(in, "nickname", m.nickname, sizeof(m.nickname));
        m.premium = jget_i(in, "premium");
        on = pb_enc_greeting_req(out, out_cap, &m);
    } else if (strcmp(kind, "greeting-resp") == 0) {
        pb_greeting_resp m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "user_id", m.user_id, sizeof(m.user_id));
        jget_line(in, "greeting", m.greeting, sizeof(m.greeting));
        jget_line(in, "error", m.error, sizeof(m.error));
        on = pb_enc_greeting_resp(out, out_cap, &m);
    } else if (strcmp(kind, "chat-stream") == 0) {
        pb_chat_stream_chunk m;
        memset(&m, 0, sizeof(m));
        jget_line(in, "request_id", m.request_id, sizeof(m.request_id));
        jget_line(in, "type", m.type, sizeof(m.type));
        jget_line(in, "content", m.content, sizeof(m.content));
        jget_line(in, "error", m.error, sizeof(m.error));
        m.done = jget_i(in, "done");
        m.timestamp = jget_ll(in, "timestamp");
        on = pb_enc_chat_stream_chunk(out, out_cap, &m);
    } else {
        return PB_CLI_UNKNOWN;
    }

    if (!on)
        return PB_CLI_ERR;
    *out_len = on;
    return PB_CLI_OK;
}


int pb_cli_decode(const char *kind, const uint8_t *wire, size_t wire_len, char *out, size_t out_cap) {
    char *mem = NULL;
    size_t mem_sz = 0;
    FILE *fp;
    int rc = PB_CLI_ERR;
    const uint8_t *in;
    size_t n;
    if (!kind || !out || out_cap == 0)
        return PB_CLI_ERR;
    out[0] = '\0';
    in = wire ? wire : (const uint8_t *)"";
    n = wire ? wire_len : 0;
    fp = open_memstream(&mem, &mem_sz);
    if (!fp)
        return PB_CLI_ERR;

    if (strcmp(kind, "turn-start") == 0) {
        pb_turn_start_req m;
        if (pb_dec_turn_start_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\nuser_id=%s\nsession_id=%s\ntext=%s\nresponse_subject=%s\n",
                m.request_id, m.user_id, m.session_id, m.text, m.response_subject);
    } else if (strcmp(kind, "turn-event") == 0) {
        pb_turn_event m;
        if (pb_dec_turn_event(in, n, &m) != 0)
            goto done;
        fprintf(fp,
                "request_id=%s\ntype=%d\nstate=%d\ntext=%s\nspeech_text=%s\ndisplay_text=%s\nerror=%s\n"
                "sequence=%d\ntimestamp=%lld\n",
                m.request_id, m.type, m.state, m.text, m.speech_text, m.display_text, m.error, m.sequence,
                (long long)m.timestamp);
    } else if (strcmp(kind, "tool-start") == 0) {
        pb_tool_start_req m;
        if (pb_dec_tool_start_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "tool_call_id=%s\ntool_id=%s\nagent_id=%s\ninput_json=%s\n", m.tool_call_id, m.tool_id,
                m.agent_id, m.input_json);
    } else if (strcmp(kind, "tool-start-resp") == 0) {
        pb_tool_start_resp m;
        if (pb_dec_tool_start_resp(in, n, &m) != 0)
            goto done;
        fprintf(fp, "tool_call_id=%s\naccepted=%d\nstate=%d\nevent_subject=%s\nerror=%s\n", m.tool_call_id,
                m.accepted, m.state, m.event_subject, m.error);
    } else if (strcmp(kind, "agent-start") == 0) {
        pb_agent_start_req m;
        if (pb_dec_agent_start_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "task_id=%s\nagent_id=%s\ninput_json=%s\n", m.task_id, m.agent_id, m.input_json);
    } else if (strcmp(kind, "dnd-vision") == 0) {
        pb_dnd_vision_event m;
        if (pb_dec_dnd_vision(in, n, &m) != 0)
            goto done;
        fprintf(fp,
                "session_id=%s\nsequence=%llu\nobserved_at_mono_ns=%llu\npublished_at_unix_ms=%lld\n"
                "person_count=%u\nmotion_energy=%g\ngaze_cluster=%d\nscene_changed=%d\n"
                "policy_version=%s\nframe_width=%u\nframe_height=%u\n",
                m.session_id, (unsigned long long)m.sequence, (unsigned long long)m.observed_at_mono_ns,
                (long long)m.published_at_unix_ms, m.person_count, (double)m.motion_energy, m.gaze_cluster,
                m.scene_changed, m.policy_version, m.frame_width, m.frame_height);
    } else if (strcmp(kind, "chat-req") == 0) {
        pb_chat_req m;
        if (pb_dec_chat_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\nuser_id=%s\nsession_id=%s\nmessage=%s\n", m.request_id, m.user_id,
                m.session_id, m.message);
    } else if (strcmp(kind, "chat-resp") == 0) {
        pb_chat_resp m;
        if (pb_dec_chat_resp(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\nuser_id=%s\nresponse=%s\nresponse_text=%s\nerror=%s\n", m.request_id,
                m.user_id, m.response, m.response_text, m.error);
    } else if (strcmp(kind, "turn-cancel") == 0) {
        pb_turn_cancel_req m;
        if (pb_dec_turn_cancel_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\nuser_id=%s\nreason=%s\n", m.request_id, m.user_id, m.reason);
    } else if (strcmp(kind, "turn-start-resp") == 0) {
        pb_turn_start_resp m;
        if (pb_dec_turn_start_resp(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\naccepted=%d\nstate=%d\naccepted_at=%lld\nevent_subject=%s\n",
                m.request_id, m.accepted, m.state, (long long)m.accepted_at, m.event_subject);
    } else if (strcmp(kind, "login") == 0) {
        pb_login_event m;
        if (pb_dec_login_event(in, n, &m) != 0)
            goto done;
        fprintf(fp, "user_id=%s\nusername=%s\nnickname=%s\npremium=%d\ntimestamp=%lld\n", m.user_id,
                m.username, m.nickname, m.premium, (long long)m.timestamp);
    } else if (strcmp(kind, "greeting-req") == 0) {
        pb_greeting_req m;
        if (pb_dec_greeting_req(in, n, &m) != 0)
            goto done;
        fprintf(fp, "user_id=%s\nusername=%s\nnickname=%s\npremium=%d\n", m.user_id, m.username, m.nickname,
                m.premium);
    } else if (strcmp(kind, "greeting-resp") == 0) {
        pb_greeting_resp m;
        if (pb_dec_greeting_resp(in, n, &m) != 0)
            goto done;
        fprintf(fp, "user_id=%s\ngreeting=%s\nerror=%s\n", m.user_id, m.greeting, m.error);
    } else if (strcmp(kind, "chat-stream") == 0) {
        pb_chat_stream_chunk m;
        if (pb_dec_chat_stream_chunk(in, n, &m) != 0)
            goto done;
        fprintf(fp, "request_id=%s\ntype=%s\ncontent=%s\ndone=%d\ntimestamp=%lld\nerror=%s\n", m.request_id,
                m.type, m.content, m.done, (long long)m.timestamp, m.error);
    } else {
        fclose(fp);
        free(mem);
        return PB_CLI_UNKNOWN;
    }
    rc = PB_CLI_OK;
done:
    fclose(fp);
    if (rc == PB_CLI_OK && mem) {
        size_t copy = mem_sz;
        if (copy >= out_cap)
            copy = out_cap - 1;
        memcpy(out, mem, copy);
        out[copy] = '\0';
    }
    free(mem);
    return rc;
}
