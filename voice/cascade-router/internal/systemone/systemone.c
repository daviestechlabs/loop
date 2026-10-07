#define _POSIX_C_SOURCE 200809L
#include "systemone.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char SYSTEMONE_PATH[] = "/v1/systemone";

const char *systemone_path_v1(void)
{
    return SYSTEMONE_PATH;
}

static void trim_copy(char *dst, size_t dst_cap, const char *src)
{
    size_t i;
    size_t start = 0;
    size_t end;
    size_t n;

    if (dst == NULL || dst_cap == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    while (src[start] != '\0' && isspace((unsigned char)src[start])) {
        start++;
    }
    end = strlen(src);
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }
    n = end - start;
    if (n >= dst_cap) {
        n = dst_cap - 1;
    }
    for (i = 0; i < n; i++) {
        dst[i] = src[start + i];
    }
    dst[n] = '\0';
}

void systemone_config_from_env_v1(systemone_config_v1 *out)
{
    const char *preferred;
    const char *alias;
    const char *gate;
    const char *k_env;
    char *end = NULL;
    float parsed;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->shortlist_k = SYSTEMONE_SHORTLIST_K_DEFAULT;

    preferred = getenv("SYSTEMONE_BASE_URL");
    alias = getenv("JEV_BASE_URL");
    if (preferred != NULL && preferred[0] != '\0') {
        trim_copy(out->base_url, sizeof(out->base_url), preferred);
    } else if (alias != NULL && alias[0] != '\0') {
        /* Alias only when preferred unset — document JEV_BASE_URL compatibility. */
        trim_copy(out->base_url, sizeof(out->base_url), alias);
    }

    out->enabled = (out->base_url[0] != '\0') ? 1 : 0;

    gate = getenv("SYSTEMONE_MIN_ENTROPY_CONFIDENCE");
    if (gate != NULL && gate[0] != '\0') {
        parsed = strtof(gate, &end);
        if (end != gate && isfinite(parsed)) {
            out->min_entropy_confidence_set = 1;
            out->min_entropy_confidence = parsed;
        }
    }

    k_env = getenv("SYSTEMONE_SHORTLIST_K");
    if (k_env != NULL && k_env[0] != '\0') {
        long k = strtol(k_env, &end, 10);
        if (end != k_env && k > 0 && k <= 255) {
            out->shortlist_k = (int)k;
        }
    }
}

int systemone_is_enabled_v1(const systemone_config_v1 *cfg)
{
    return (cfg != NULL && cfg->enabled && cfg->base_url[0] != '\0') ? 1 : 0;
}

systemone_status_v1 systemone_build_post_url_v1(const systemone_config_v1 *cfg,
                                                char *out_url,
                                                size_t out_cap)
{
    size_t base_len;
    size_t path_len;
    size_t i;
    int need_slash;

    if (cfg == NULL || out_url == NULL || out_cap == 0) {
        return SYSTEMONE_STATUS_ERROR_V1;
    }
    out_url[0] = '\0';
    if (!systemone_is_enabled_v1(cfg)) {
        return SYSTEMONE_STATUS_SKIPPED_UNSET_V1;
    }

    base_len = strlen(cfg->base_url);
    path_len = strlen(SYSTEMONE_PATH);
    need_slash = (base_len > 0 && cfg->base_url[base_len - 1] == '/') ? 0 : 1;
    /* If base already ends with '/', strip that slash and append path which starts with '/'. */
    if (!need_slash) {
        base_len -= 1;
    }
    if (base_len + path_len + 1 > out_cap) {
        return SYSTEMONE_STATUS_ERROR_V1;
    }
    for (i = 0; i < base_len; i++) {
        out_url[i] = cfg->base_url[i];
    }
    for (i = 0; i < path_len; i++) {
        out_url[base_len + i] = SYSTEMONE_PATH[i];
    }
    out_url[base_len + path_len] = '\0';
    return SYSTEMONE_STATUS_READY_V1;
}

size_t systemone_shortlist_topk_v1(const systemone_config_v1 *cfg,
                                   const uint32_t *ranked_in,
                                   size_t ranked_len,
                                   uint32_t *ranked_out,
                                   size_t ranked_out_cap)
{
    size_t k;
    size_t keep;
    size_t i;
    int shortlist_k;

    if (ranked_in == NULL || ranked_out == NULL || ranked_out_cap == 0) {
        return 0;
    }
    shortlist_k = (cfg != NULL && cfg->shortlist_k > 0)
                      ? cfg->shortlist_k
                      : SYSTEMONE_SHORTLIST_K_DEFAULT;
    k = (size_t)shortlist_k;
    keep = ranked_len < k ? ranked_len : k;
    if (keep > ranked_out_cap) {
        keep = ranked_out_cap;
    }
    for (i = 0; i < keep; i++) {
        ranked_out[i] = ranked_in[i];
    }
    return keep;
}

systemone_status_v1 systemone_entropy_gate_v1(const systemone_config_v1 *cfg,
                                              float entropy_confidence)
{
    if (cfg == NULL || !isfinite(entropy_confidence)) {
        return SYSTEMONE_STATUS_ERROR_V1;
    }
    /* Unset gate => feature off with URL (URL-only enable). */
    if (!cfg->min_entropy_confidence_set) {
        return SYSTEMONE_STATUS_READY_V1;
    }
    if (entropy_confidence < cfg->min_entropy_confidence) {
        return SYSTEMONE_STATUS_REJECT_LOW_ENTROPY_CONFIDENCE_V1;
    }
    return SYSTEMONE_STATUS_READY_V1;
}

systemone_status_v1 systemone_lab_request_stub_v1(const systemone_config_v1 *cfg,
                                                  char *out_url,
                                                  size_t out_cap)
{
    char scratch[SYSTEMONE_BASE_URL_CAP + SYSTEMONE_PATH_CAP];
    char *dest = out_url;
    size_t cap = out_cap;
    systemone_status_v1 st;

    if (dest == NULL || cap == 0) {
        dest = scratch;
        cap = sizeof(scratch);
    }
    st = systemone_build_post_url_v1(cfg, dest, cap);
    /* TODO(lab): perform POST {url} with typed-decisions JSON after Infra /healthz. */
    return st;
}

#include "http_min.h"
#include <stdio.h>
#include <time.h>

#ifndef SYSTEMONE_MODEL_DEFAULT
#define SYSTEMONE_MODEL_DEFAULT "laya-typed-decisions"
#endif

static int append_str(char *out, size_t cap, size_t *len, const char *s)
{
    size_t n;
    if (!out || !len || !s) return -1;
    n = strlen(s);
    if (*len + n >= cap) return -1;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
    return 0;
}

static int json_escape_append(char *out, size_t cap, size_t *len, const char *in, size_t in_len)
{
    size_t i;
    if (!out || !len) return -1;
    for (i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        char esc[8];
        size_t elen = 0;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; elen = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n'; elen = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r'; elen = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't'; elen = 2; }
        else if (c < 0x20u) {
            if (snprintf(esc, sizeof(esc), "\\u%04x", c) != 6) return -1;
            elen = 6;
        } else { esc[0] = (char)c; elen = 1; }
        if (*len + elen >= cap) return -1;
        memcpy(out + *len, esc, elen);
        *len += elen;
    }
    if (*len < cap) out[*len] = '\0';
    return 0;
}

static const char *find_key(const char *json, size_t json_len, const char *key)
{
    size_t key_len, i;
    if (!json || !key) return NULL;
    key_len = strlen(key);
    if (!key_len || json_len < key_len + 2) return NULL;
    for (i = 0; i + key_len + 2 <= json_len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, key_len) == 0 &&
            json[i + 1 + key_len] == '"') {
            const char *p = json + i + 1 + key_len + 1;
            while (p < json + json_len && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p < json + json_len && *p == ':') {
                p++;
                while (p < json + json_len && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
                return p;
            }
        }
    }
    return NULL;
}

static int parse_json_string(const char *p, const char *end, char *out, size_t out_cap)
{
    size_t n = 0;
    if (!p || !end || !out || !out_cap || p >= end || *p != '"') return -1;
    p++;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= end || n + 1 >= out_cap) return -1;
            switch (*p) {
            case '"': case '\\': case '/': out[n++] = *p; break;
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            default: out[n++] = *p; break;
            }
            p++;
            continue;
        }
        if (n + 1 >= out_cap) return -1;
        out[n++] = *p++;
    }
    if (p >= end || *p != '"') return -1;
    out[n] = '\0';
    return 0;
}

static int parse_json_number(const char *p, const char *end, float *out)
{
    char buf[64];
    size_t n = 0;
    char *parse_end = NULL;
    float v;
    if (!p || !end || !out) return -1;
    if (p < end && (*p == '-' || *p == '+')) buf[n++] = *p++;
    while (p < end && n + 1 < sizeof(buf) &&
           ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
            *p == '+' || *p == '-')) buf[n++] = *p++;
    if (!n) return -1;
    buf[n] = '\0';
    v = strtof(buf, &parse_end);
    if (parse_end == buf || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static const char *object_span_end(const char *p, const char *end)
{
    int depth = 0, in_str = 0;
    if (!p || !end || p >= end || *p != '{') return NULL;
    for (; p < end; p++) {
        if (in_str) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) return p + 1;
        }
    }
    return NULL;
}

systemone_status_v1 systemone_parse_response_v1(const char *json, size_t json_len,
                                                systemone_decision_v1 *out)
{
    const char *end, *answers, *answers_end, *route_obj, *route_end, *p;
    char choice[SYSTEMONE_ROUTE_CHOICE_CAP];
    if (!out) return SYSTEMONE_STATUS_ERROR_V1;
    memset(out, 0, sizeof(*out));
    if (!json || !json_len) return SYSTEMONE_STATUS_ERROR_V1;
    end = json + json_len;
    answers = find_key(json, json_len, "answers");
    if (!answers || answers >= end || *answers != '{') return SYSTEMONE_STATUS_ERROR_V1;
    answers_end = object_span_end(answers, end);
    if (!answers_end) return SYSTEMONE_STATUS_ERROR_V1;

    route_obj = find_key(answers, (size_t)(answers_end - answers), "route");
    if (route_obj && route_obj < answers_end && *route_obj == '{') {
        route_end = object_span_end(route_obj, answers_end);
        if (route_end) {
            p = find_key(route_obj, (size_t)(route_end - route_obj), "choice");
            if (p && parse_json_string(p, route_end, choice, sizeof(choice)) == 0) {
                memcpy(out->route_choice, choice, strlen(choice) + 1);
                out->has_route = 1;
            }
            p = find_key(route_obj, (size_t)(route_end - route_obj), "confidence");
            if (p && parse_json_number(p, route_end, &out->route_confidence) == 0)
                out->has_route_confidence = 1;
            {
                const char *probs = find_key(route_obj, (size_t)(route_end - route_obj),
                                             "probabilities");
                const char *probs_end;
                float pa = 0.f, pe = 0.f, pr = 0.f;
                int got = 0;
                if (probs && probs < route_end && *probs == '{') {
                    probs_end = object_span_end(probs, route_end);
                    if (probs_end) {
                        const char *q;
                        q = find_key(probs, (size_t)(probs_end - probs), "answer");
                        if (q && parse_json_number(q, probs_end, &pa) == 0) got++;
                        q = find_key(probs, (size_t)(probs_end - probs), "escalate");
                        if (q && parse_json_number(q, probs_end, &pe) == 0) got++;
                        q = find_key(probs, (size_t)(probs_end - probs),
                                     "retrieve_then_escalate");
                        if (q && parse_json_number(q, probs_end, &pr) == 0) got++;
                        if (got > 0) {
                            out->route_prob_answer = pa;
                            out->route_prob_escalate = pe;
                            out->route_prob_retrieve_then_escalate = pr;
                            out->has_route_probabilities = 1;
                        }
                    }
                }
            }
        }
    }
    {
        const char *o = find_key(answers, (size_t)(answers_end - answers), "confidence");
        const char *e;
        if (o && o < answers_end && *o == '{') {
            e = object_span_end(o, answers_end);
            if (e) {
                p = find_key(o, (size_t)(e - o), "score");
                if (p && parse_json_number(p, e, &out->score) == 0) out->has_score = 1;
            }
        }
    }
    {
        const char *o = find_key(answers, (size_t)(answers_end - answers), "abstain");
        const char *e;
        if (o && o < answers_end && *o == '{') {
            e = object_span_end(o, answers_end);
            if (e) {
                p = find_key(o, (size_t)(e - o), "noul");
                if (p && parse_json_number(p, e, &out->noul) == 0) out->has_noul = 1;
            }
        }
    }
    return out->has_route ? SYSTEMONE_STATUS_READY_V1 : SYSTEMONE_STATUS_ERROR_V1;
}

systemone_status_v1 systemone_map_to_cascade_route_v1(
    const systemone_config_v1 *cfg, const systemone_decision_v1 *decision,
    const char **out_name, size_t *out_len, uint32_t *out_enum)
{
    systemone_status_v1 gate;
    float conf;
    if (out_name) *out_name = "escalate";
    if (out_len) *out_len = sizeof("escalate") - 1u;
    if (out_enum) *out_enum = 1u;
    if (!decision || !decision->has_route) return SYSTEMONE_STATUS_ERROR_V1;
    if (decision->has_noul && decision->noul >= 0.5f) return SYSTEMONE_STATUS_ERROR_V1;
    conf = decision->has_route_confidence ? decision->route_confidence
           : (decision->has_score ? decision->score : 1.0f);
    gate = systemone_entropy_gate_v1(cfg, conf);
    if (gate != SYSTEMONE_STATUS_READY_V1) return gate;
    if (strcmp(decision->route_choice, "retrieve_then_escalate") == 0) {
        if (out_name) *out_name = "retrieve_then_escalate";
        if (out_len) *out_len = sizeof("retrieve_then_escalate") - 1u;
        if (out_enum) *out_enum = 2u;
        return SYSTEMONE_STATUS_READY_V1;
    }
    if (strcmp(decision->route_choice, "answer") == 0) {
        if (out_name) *out_name = "answer";
        if (out_len) *out_len = sizeof("answer") - 1u;
        if (out_enum) *out_enum = 1u;
        return SYSTEMONE_STATUS_READY_V1;
    }
    if (strcmp(decision->route_choice, "escalate") == 0) return SYSTEMONE_STATUS_READY_V1;
    return SYSTEMONE_STATUS_ERROR_V1;
}

static int resolve_timeout_ms(int timeout_ms)
{
    const char *env;
    char *end = NULL;
    long parsed;
    if (timeout_ms > 0) return timeout_ms;
    env = getenv("SYSTEMONE_TIMEOUT_MS");
    if (env && env[0]) {
        parsed = strtol(env, &end, 10);
        if (end != env && parsed > 0 && parsed <= 60000) return (int)parsed;
    }
    return SYSTEMONE_DEFAULT_TIMEOUT_MS;
}

static int build_request_json(const char *state, size_t state_len,
                              char *out, size_t out_cap, size_t *out_len)
{
    size_t len = 0;
    const char *model;
    const char *tail =
        "\",\"questions\":{"
        "\"route\":{\"type\":\"choice\","
        "\"instructions\":\"Choose how SystemOne should handle this state.\","
        "\"criteria\":{"
        "\"answer\":\"Give a short direct answer without tools.\","
        "\"escalate\":\"Escalate to the generative model.\","
        "\"retrieve_then_escalate\":\"Retrieve knowledge then escalate.\"}},"
        "\"confidence\":{\"type\":\"score\","
        "\"instructions\":\"Score confidence from guessing to certain.\","
        "\"criteria\":[\"guessing\",\"certain\"]},"
        "\"abstain\":{\"type\":\"noul\","
        "\"instructions\":\"Whether to abstain from answering.\"}}}";
    if (!out || !out_cap || !out_len) return -1;
    out[0] = '\0';
    model = getenv("SYSTEMONE_MODEL");
    if (!model || !model[0]) model = SYSTEMONE_MODEL_DEFAULT;
    if (!state) { state = ""; state_len = 0; }
    if (append_str(out, out_cap, &len, "{\"state\":\"") ||
        json_escape_append(out, out_cap, &len, state, state_len) ||
        append_str(out, out_cap, &len, "\",\"model\":\"") ||
        json_escape_append(out, out_cap, &len, model, strlen(model)) ||
        append_str(out, out_cap, &len, tail)) return -1;
    *out_len = len;
    return 0;
}

systemone_status_v1 systemone_lab_request_v1(const systemone_config_v1 *cfg,
                                             const char *state_text, size_t state_len,
                                             systemone_decision_v1 *out_decision,
                                             int timeout_ms)
{
    char url[SYSTEMONE_BASE_URL_CAP + SYSTEMONE_PATH_CAP];
    char req[SYSTEMONE_REQUEST_BODY_CAP];
    uint8_t resp[SYSTEMONE_RESPONSE_BODY_CAP];
    size_t req_len = 0, resp_len = 0;
    int status = 0, rc;
    systemone_status_v1 st;
    if (out_decision) memset(out_decision, 0, sizeof(*out_decision));
    st = systemone_build_post_url_v1(cfg, url, sizeof(url));
    if (st != SYSTEMONE_STATUS_READY_V1) return st;
    if (!out_decision) return SYSTEMONE_STATUS_ERROR_V1;
    if (build_request_json(state_text, state_len, req, sizeof(req), &req_len))
        return SYSTEMONE_STATUS_ERROR_V1;
    timeout_ms = resolve_timeout_ms(timeout_ms);
    rc = http_min_post(url, "application/json", (const uint8_t *)req, req_len,
                       resp, sizeof(resp) - 1u, &resp_len, &status, timeout_ms);
    if (rc != HTTP_MIN_OK || status < 200 || status >= 300 || resp_len == 0)
        return SYSTEMONE_STATUS_ERROR_V1;
    resp[resp_len] = '\0';
    return systemone_parse_response_v1((const char *)resp, resp_len, out_decision);
}

void systemone_ft_emit_jsonl_v1(const systemone_config_v1 *cfg,
                                const char *request_id, const char *state_text,
                                size_t state_len, const systemone_decision_v1 *decision,
                                systemone_status_v1 status, const char *c_route_name)
{
    const char *path, *model, *choice = "";
    FILE *fp;
    char state_esc[512];
    size_t esc_len = 0;
    time_t now;
    struct tm tm_utc;
    char ts[32];
    float score = 0.f, noul = 0.f, route_conf = 0.f;
    float pa = 0.f, pe = 0.f, pr = 0.f;
    int shortlist_k = SYSTEMONE_SHORTLIST_K_DEFAULT;
    int has_route = 0, has_score = 0, has_noul = 0, has_route_conf = 0;
    int has_probs = 0;
    const char *label_source = "none";

    path = getenv("SYSTEMONE_FT_JSONL_PATH");
    if (!path || !path[0]) return;
    if (!state_text) { state_text = ""; state_len = 0; }
    if (state_len > 240) state_len = 240;
    if (json_escape_append(state_esc, sizeof(state_esc), &esc_len, state_text, state_len))
        return;
    now = time(NULL);
    if (!gmtime_r(&now, &tm_utc)) return;
    if (!strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc)) return;
    model = getenv("SYSTEMONE_MODEL");
    if (!model || !model[0]) model = SYSTEMONE_MODEL_DEFAULT;
    if (decision) {
        has_route = decision->has_route;
        has_score = decision->has_score;
        has_noul = decision->has_noul;
        has_route_conf = decision->has_route_confidence;
        has_probs = decision->has_route_probabilities;
        if (has_route) choice = decision->route_choice;
        if (has_score) score = decision->score;
        if (has_noul) noul = decision->noul;
        if (has_route_conf) route_conf = decision->route_confidence;
        if (has_probs) {
            pa = decision->route_prob_answer;
            pe = decision->route_prob_escalate;
            pr = decision->route_prob_retrieve_then_escalate;
        }
    }
    if (!request_id) request_id = "";
    if (!c_route_name) c_route_name = "";
    if (cfg && cfg->shortlist_k > 0) shortlist_k = cfg->shortlist_k;
    /* Cascade Mix / canary: never invent noul. label.source stays none until human/c_agree. */
    fp = fopen(path, "a");
    if (!fp) return;
    (void)fprintf(
        fp,
        "{\"schema\":\"laya-systemone-decision/v1\",\"ts\":\"%s\",\"request_id\":\"%s\","
        "\"model\":\"%s\",\"state_redacted\":\"%s\",\"status\":%d,"
        "\"questions\":{\"route\":{\"type\":\"choice\",\"criteria_keys\":"
        "[\"answer\",\"escalate\",\"retrieve_then_escalate\"]},"
        "\"confidence\":{\"type\":\"score\"},\"abstain\":{\"type\":\"noul\"}},"
        "\"answers\":{\"route\":{\"choice\":%s%s%s,\"probabilities\":",
        ts, request_id, model, state_esc, (int)status,
        has_route ? "\"" : "",
        has_route ? choice : "null",
        has_route ? "\"" : "");
    if (has_probs)
        (void)fprintf(fp,
            "{\"answer\":%.6f,\"escalate\":%.6f,\"retrieve_then_escalate\":%.6f}",
            (double)pa, (double)pe, (double)pr);
    else
        (void)fprintf(fp, "{}");
    (void)fprintf(fp, ",\"confidence\":");
    if (has_route_conf)
        (void)fprintf(fp, "%.6f", (double)route_conf);
    else
        (void)fprintf(fp, "null");
    (void)fprintf(fp, "},\"confidence\":{\"score\":");
    if (has_score)
        (void)fprintf(fp, "%.6f", (double)score);
    else
        (void)fprintf(fp, "null");
    (void)fprintf(fp, "},\"abstain\":{\"noul\":");
    /* Zero fake noul: numeric only when real has_noul; else JSON null. */
    if (has_noul)
        (void)fprintf(fp, "%.6f", (double)noul);
    else
        (void)fprintf(fp, "null");
    (void)fprintf(
        fp,
        "}},\"policy\":{\"c_route\":\"%s\",\"shortlist_k\":%d,\"has_route\":%s,"
        "\"has_score\":%s,\"has_noul\":%s,\"has_route_probabilities\":%s},"
        "\"label\":{\"source\":\"%s\",\"route_gold\":null,\"notes\":null}}\n",
        c_route_name, shortlist_k,
        has_route ? "true" : "false",
        has_score ? "true" : "false",
        has_noul ? "true" : "false",
        has_probs ? "true" : "false",
        label_source);
    (void)fclose(fp);
}

