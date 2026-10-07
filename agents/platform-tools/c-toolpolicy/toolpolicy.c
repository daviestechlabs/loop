#define _POSIX_C_SOURCE 200809L
#include "toolpolicy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_builtins[] = {
    "workspace-read",
    "workspace-edit",
    "dnd-dice-roll",
    "dnd-encounter-state",
    "dnd-campaign-state",
    "dnd-scene-presence",
    NULL,
};

typedef struct tp_agent_tool {
    const char *agent_id;
    const char *tool_id;
} tp_agent_tool;

static const tp_agent_tool k_agent_tools[] = {
    {"waterdeep-coder", "workspace-read"},
    {"waterdeep-coder", "workspace-edit"},
    {"dnd-agent", "dnd-dice-roll"},
    {"dnd-agent", "dnd-encounter-state"},
    {"dnd-agent", "dnd-campaign-state"},
    {"dnd-agent", "dnd-scene-presence"},
    {NULL, NULL},
};

int tp_agent_allows_tool(const char *agent_id, const char *tool_id) {
    int i;
    if (!agent_id || !agent_id[0] || !tool_id || !tool_id[0]) return 0;
    for (i = 0; k_agent_tools[i].agent_id; i++) {
        if (strcmp(k_agent_tools[i].agent_id, agent_id) == 0 &&
            strcmp(k_agent_tools[i].tool_id, tool_id) == 0)
            return 1;
    }
    return 0;
}

static void trim_inplace(char *s) {
    char *a, *b;
    size_t n;
    if (!s) return;
    a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) {
        n = strlen(a);
        memmove(s, a, n + 1);
    }
    n = strlen(s);
    b = s + n;
    while (b > s && isspace((unsigned char)b[-1])) b--;
    *b = '\0';
}

static void lower_inplace(char *s) {
    for (; s && *s; s++) *s = (char)tolower((unsigned char)*s);
}

int tp_is_builtin_tool(const char *id) {
    int i;
    if (!id) return 0;
    for (i = 0; k_builtins[i]; i++)
        if (strcmp(k_builtins[i], id) == 0) return 1;
    return 0;
}

const char *tp_parse_risk(const char *raw) {
    static char buf[32];
    if (!raw) return "unspecified";
    snprintf(buf, sizeof(buf), "%s", raw);
    trim_inplace(buf);
    lower_inplace(buf);
    if (!strcmp(buf, "low")) return "low";
    if (!strcmp(buf, "medium")) return "medium";
    if (!strcmp(buf, "high")) return "high";
    if (!strcmp(buf, "destructive")) return "destructive";
    if (!strcmp(buf, "external")) return "external";
    return "unspecified";
}

static int json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[128];
    const char *p;
    size_t o = 0;
    if (!json || !key || !out || !cap) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        out[0] = '\0';
        return -1;
    }
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

static int json_int(const char *json, const char *key, long long *out, int *found) {
    char pat[128];
    const char *p;
    if (found) *found = 0;
    if (!json || !key || !out) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return -1;
    *out = strtoll(p, NULL, 10);
    if (found) *found = 1;
    return 0;
}

static int json_bool(const char *json, const char *key, int *out) {
    char pat[128];
    const char *p;
    if (!json || !key || !out) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int meta_eq(const char *json, const char *key, const char *want) {
    const char *meta = strstr(json, "\"metadata\"");
    char val[TP_STR];
    if (!meta) return 0;
    if (json_str(meta, key, val, sizeof(val)) != 0) return 0;
    return strcmp(val, want) == 0;
}

static int extract_object_after_key(const char *json, const char *key, char *out, size_t cap) {
    char pat[128];
    const char *p, *br;
    int depth = 0;
    size_t n;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    br = strchr(p + strlen(pat), '{');
    if (!br) return -1;
    p = br;
    do {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    } while (*p && depth > 0);
    n = (size_t)(p - br);
    if (n + 1 > cap) return -1;
    memcpy(out, br, n);
    out[n] = '\0';
    return 0;
}

int tp_validate_tool_json(const char *tool_json, char *err, size_t err_cap) {
    char id[TP_STR], name[TP_STR], version[TP_STR], in_s[TP_PATH], out_s[TP_PATH];
    char auth[TP_STR], risk_raw[64];
    long long timeout = 0;
    int found = 0, sandbox_req = 0;
    char sandbox[2048];

    if (!tool_json) {
        if (err && err_cap) snprintf(err, err_cap, "null tool");
        return TP_ERR;
    }
    id[0] = name[0] = version[0] = in_s[0] = out_s[0] = auth[0] = risk_raw[0] = '\0';
    (void)json_str(tool_json, "id", id, sizeof(id));
    (void)json_str(tool_json, "name", name, sizeof(name));
    (void)json_str(tool_json, "version", version, sizeof(version));
    (void)json_str(tool_json, "inputSchema", in_s, sizeof(in_s));
    (void)json_str(tool_json, "outputSchema", out_s, sizeof(out_s));
    (void)json_str(tool_json, "authScope", auth, sizeof(auth));
    (void)json_str(tool_json, "riskLevel", risk_raw, sizeof(risk_raw));
    (void)json_int(tool_json, "timeoutMs", &timeout, &found);
    trim_inplace(id);
    trim_inplace(name);
    trim_inplace(version);
    trim_inplace(in_s);
    trim_inplace(out_s);
    trim_inplace(auth);
    trim_inplace(risk_raw);

    if (!id[0]) {
        if (err && err_cap) snprintf(err, err_cap, "tool id is required");
        return TP_ERR;
    }
    if (!tp_is_builtin_tool(id)) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" is not a compiled no-shell builtin", id);
        return TP_ERR;
    }
    if (!name[0] || !version[0] || !in_s[0] || !out_s[0] || !auth[0] || !found || timeout <= 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" has an incomplete governed definition", id);
        return TP_ERR;
    }
    if (!strcmp(tp_parse_risk(risk_raw), "unspecified")) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" has unsupported risk level \"%s\"", id, risk_raw);
        return TP_ERR;
    }
    if (!meta_eq(tool_json, "execution_class", "builtin_no_shell")) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" execution_class must be \"builtin_no_shell\"", id);
        return TP_ERR;
    }
    const char *egress = !strcmp(id, "dnd-scene-presence") ? "none" : "http_only";
    if (!meta_eq(tool_json, "network_egress", egress)) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" network_egress must be \"%s\"", id, egress);
        return TP_ERR;
    }
    /* secrets: only reject non-empty array immediately after the key. */
    {
        const char *sp = strstr(tool_json, "\"secrets\"");
        if (sp) {
            const char *p = sp + strlen("\"secrets\"");
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == ':') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == '[') {
                    const char *q = p + 1;
                    while (*q && isspace((unsigned char)*q)) q++;
                    if (*q != ']') {
                        if (err && err_cap)
                            snprintf(err, err_cap,
                                     "tool \"%s\" cannot mount secrets in the built-in runtime", id);
                        return TP_ERR;
                    }
                }
            }
        }
    }

    sandbox[0] = '\0';
    if (extract_object_after_key(tool_json, "sandbox", sandbox, sizeof(sandbox)) == 0) {
        (void)json_bool(sandbox, "required", &sandbox_req);
    }

    if (!strcmp(id, "workspace-edit")) {
        char runtime[TP_STR], ns[TP_STR], sa[TP_STR], np[TP_STR];
        runtime[0] = ns[0] = sa[0] = np[0] = '\0';
        (void)json_str(sandbox, "runtime", runtime, sizeof(runtime));
        (void)json_str(sandbox, "namespace", ns, sizeof(ns));
        (void)json_str(sandbox, "serviceAccount", sa, sizeof(sa));
        (void)json_str(sandbox, "networkPolicy", np, sizeof(np));
        trim_inplace(runtime);
        trim_inplace(ns);
        trim_inplace(sa);
        trim_inplace(np);
        if (!sandbox_req || strcmp(runtime, "in_process_builtin") != 0 || strcmp(ns, "ai-ml") != 0 ||
            strcmp(sa, "platform-tools") != 0 || strcmp(np, "platform-tools-http-only") != 0) {
            if (err && err_cap)
                snprintf(err, err_cap,
                         "tool \"%s\" sandbox must describe the restricted platform-tools runtime",
                         id);
            return TP_ERR;
        }
    } else if (sandbox_req) {
        if (err && err_cap)
            snprintf(err, err_cap, "tool \"%s\" cannot claim an unimplemented sandbox", id);
        return TP_ERR;
    }
    return TP_OK;
}

/* Iterate objects in array under key. */
typedef int (*obj_cb)(const char *obj, size_t len, int index, void *ud);

static int for_each_array_object(const char *json, const char *key, obj_cb cb, void *ud) {
    char pat[128];
    const char *p, *arr, *end;
    int depth, in_str, esc, index = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '[') return 0;
    arr = p;
    depth = 0;
    in_str = 0;
    esc = 0;
    for (end = arr; *end; end++) {
        if (in_str) {
            if (esc) esc = 0;
            else if (*end == '\\') esc = 1;
            else if (*end == '"') in_str = 0;
            continue;
        }
        if (*end == '"') {
            in_str = 1;
            continue;
        }
        if (*end == '[') depth++;
        else if (*end == ']') {
            depth--;
            if (depth == 0) break;
        }
    }
    if (!*end) return -1;
    p = arr + 1;
    while (p < end) {
        const char *obj;
        int d, is, es;
        while (p < end && (*p == ',' || isspace((unsigned char)*p))) p++;
        if (p >= end || *p != '{') break;
        obj = p;
        d = 0;
        is = 0;
        es = 0;
        for (; p < end; p++) {
            char c = *p;
            if (is) {
                if (es) es = 0;
                else if (c == '\\') es = 1;
                else if (c == '"') is = 0;
                continue;
            }
            if (c == '"') {
                is = 1;
                continue;
            }
            if (c == '{') d++;
            else if (c == '}') {
                d--;
                if (d == 0) {
                    p++;
                    if (cb(obj, (size_t)(p - obj), index++, ud) != 0) return -1;
                    break;
                }
            }
        }
    }
    return 0;
}

typedef struct {
    char ids[TP_MAX_TOOLS][TP_STR];
    int n;
    char err[TP_MSG];
    int failed;
} tool_pass;

static int tool_cb(const char *obj, size_t len, int index, void *ud) {
    tool_pass *tp = (tool_pass *)ud;
    char buf[16384];
    char id[TP_STR];
    int i;
    (void)index;
    if (len + 1 > sizeof(buf)) {
        snprintf(tp->err, sizeof(tp->err), "tool object too large");
        tp->failed = 1;
        return -1;
    }
    memcpy(buf, obj, len);
    buf[len] = '\0';
    if (tp_validate_tool_json(buf, tp->err, sizeof(tp->err)) != TP_OK) {
        tp->failed = 1;
        return -1;
    }
    id[0] = '\0';
    (void)json_str(buf, "id", id, sizeof(id));
    trim_inplace(id);
    for (i = 0; i < tp->n; i++) {
        if (!strcmp(tp->ids[i], id)) {
            snprintf(tp->err, sizeof(tp->err), "tool id \"%s\" must be unique", id);
            tp->failed = 1;
            return -1;
        }
    }
    if (tp->n < TP_MAX_TOOLS) {
        snprintf(tp->ids[tp->n], TP_STR, "%s", id);
        tp->n++;
    }
    return 0;
}

typedef struct {
    tool_pass *tools;
    char agent_ids[TP_MAX_AGENTS][TP_STR];
    int n_agents;
    char err[TP_MSG];
    int failed;
} agent_pass;

typedef struct {
    agent_pass *agents;
    char agent_id[TP_STR];
    char (*seen)[TP_STR];
    int *seen_count;
} agent_tool_ctx;

static int validate_agent_tool_cb(const char *obj, size_t len, int index, void *ud) {
    agent_tool_ctx *ctx = (agent_tool_ctx *)ud;
    char buf[1024], tool_id[TP_STR];
    int i, known = 0;

    (void)index;
    if (len + 1 > sizeof(buf)) return 0;
    memcpy(buf, obj, len);
    buf[len] = '\0';
    tool_id[0] = '\0';
    (void)json_str(buf, "id", tool_id, sizeof(tool_id));
    trim_inplace(tool_id);
    for (i = 0; i < ctx->agents->tools->n; i++) {
        if (strcmp(ctx->agents->tools->ids[i], tool_id) == 0) known = 1;
    }
    if (!known) {
        snprintf(ctx->agents->err, sizeof(ctx->agents->err),
                 "agent \"%s\" references unknown tool \"%s\"", ctx->agent_id, tool_id);
        ctx->agents->failed = 1;
        return -1;
    }
    for (i = 0; i < *ctx->seen_count; i++) {
        if (strcmp(ctx->seen[i], tool_id) == 0) {
            snprintf(ctx->agents->err, sizeof(ctx->agents->err),
                     "agent \"%s\" repeats tool \"%s\"", ctx->agent_id, tool_id);
            ctx->agents->failed = 1;
            return -1;
        }
    }
    if (*ctx->seen_count < TP_MAX_AGENT_TOOLS) {
        snprintf(ctx->seen[*ctx->seen_count], TP_STR, "%s", tool_id);
        (*ctx->seen_count)++;
    }
    return 0;
}

static int agent_cb(const char *obj, size_t len, int index, void *ud) {
    agent_pass *ap = (agent_pass *)ud;
    char buf[8192];
    char id[TP_STR];
    int i;
    char seen[TP_MAX_AGENT_TOOLS][TP_STR];
    int n_seen = 0;
    (void)index;
    if (len + 1 > sizeof(buf)) {
        snprintf(ap->err, sizeof(ap->err), "agent object too large");
        ap->failed = 1;
        return -1;
    }
    memcpy(buf, obj, len);
    buf[len] = '\0';
    id[0] = '\0';
    (void)json_str(buf, "id", id, sizeof(id));
    trim_inplace(id);
    if (!id[0]) {
        snprintf(ap->err, sizeof(ap->err), "agent id is required");
        ap->failed = 1;
        return -1;
    }
    for (i = 0; i < ap->n_agents; i++) {
        if (!strcmp(ap->agent_ids[i], id)) {
            snprintf(ap->err, sizeof(ap->err), "agent id \"%s\" must be unique", id);
            ap->failed = 1;
            return -1;
        }
    }
    if (ap->n_agents < TP_MAX_AGENTS) {
        snprintf(ap->agent_ids[ap->n_agents], TP_STR, "%s", id);
        ap->n_agents++;
    }
    /* tools array of {id} */
    {
        agent_tool_ctx ctx;

        ctx.agents = ap;
        snprintf(ctx.agent_id, sizeof(ctx.agent_id), "%s", id);
        ctx.seen = seen;
        ctx.seen_count = &n_seen;
        if (for_each_array_object(buf, "tools", validate_agent_tool_cb, &ctx) != 0 || ap->failed)
            return -1;
    }
    return 0;
}

int tp_validate_policy_json(const char *json, char *err, size_t err_cap) {
    char version[TP_STR];
    tool_pass tools;
    agent_pass agents;

    if (!json) {
        if (err && err_cap) snprintf(err, err_cap, "null policy");
        return TP_ERR;
    }
    version[0] = '\0';
    (void)json_str(json, "version", version, sizeof(version));
    trim_inplace(version);
    if (!version[0]) {
        /* also try spec.version if wrapped */
        const char *spec = strstr(json, "\"spec\"");
        if (spec) (void)json_str(spec, "version", version, sizeof(version));
        trim_inplace(version);
    }
    if (!version[0]) {
        if (err && err_cap) snprintf(err, err_cap, "registry version is required");
        return TP_ERR;
    }

    memset(&tools, 0, sizeof(tools));
    /* tools may be under root or spec */
    if (for_each_array_object(json, "tools", tool_cb, &tools) != 0 || tools.failed) {
        if (err && err_cap) snprintf(err, err_cap, "%s", tools.err[0] ? tools.err : "tool validation failed");
        return TP_ERR;
    }
    memset(&agents, 0, sizeof(agents));
    agents.tools = &tools;
    if (for_each_array_object(json, "agents", agent_cb, &agents) != 0 || agents.failed) {
        if (err && err_cap)
            snprintf(err, err_cap, "%s", agents.err[0] ? agents.err : "agent validation failed");
        return TP_ERR;
    }
    return TP_OK;
}
