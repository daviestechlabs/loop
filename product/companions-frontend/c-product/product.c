/* product.c — pure-C companions product policy. */
#include "product.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define PROD_PRINTF_LIKE(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define PROD_PRINTF_LIKE(format_index, first_arg)
#endif

static int is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_oauth_user_rest(char c) {
    return is_alnum(c) || c == '.' || c == '_' || c == '@' || c == '+' || c == '-';
}

static int is_pkce(char c) {
    return is_alnum(c) || c == '.' || c == '_' || c == '~' || c == '-';
}

static int is_provider_rest(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

int prod_oauth_username_ok(const char *username) {
    size_t n, i;
    if (!username || !username[0])
        return 0;
    if (!is_alnum(username[0]))
        return 0;
    n = strlen(username);
    if (n < 3 || n > 50)
        return 0;
    for (i = 1; i < n; i++) {
        if (!is_oauth_user_rest(username[i]))
            return 0;
    }
    return 1;
}

int prod_oauth_pkce_ok(const char *verifier) {
    size_t n, i;
    if (!verifier)
        return 0;
    n = strlen(verifier);
    if (n < 43 || n > 128)
        return 0;
    for (i = 0; i < n; i++) {
        if (!is_pkce(verifier[i]))
            return 0;
    }
    return 1;
}

int prod_oauth_provider_ok(const char *name) {
    size_t n, i;
    if (!name || !name[0])
        return 0;
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')))
        return 0;
    n = strlen(name);
    if (n < 1 || n > 50)
        return 0;
    for (i = 1; i < n; i++) {
        if (!is_provider_rest(name[i]))
            return 0;
    }
    return 1;
}

int prod_oauth_callback_code_ok(const char *code) {
    size_t n, i;
    if (!code || !code[0])
        return 0;
    n = strlen(code);
    if (n > PROD_MAX_OAUTH_CALLBACK_CODE)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)code[i];
        if (c == 0 || c == '\r' || c == '\n')
            return 0;
    }
    return 1;
}

int prod_oauth_identity_field_ok(const char *value, int min_bytes, int max_bytes) {
    size_t n, i;
    const char *p;
    if (!value || min_bytes < 0 || max_bytes < min_bytes)
        return 0;
    n = strlen(value);
    if ((int)n < min_bytes || (int)n > max_bytes)
        return 0;
    /* must equal trim (no leading/trailing space) */
    if (n > 0 && (value[0] == ' ' || value[0] == '\t' || value[n - 1] == ' ' || value[n - 1] == '\t'))
        return 0;
    /* UTF-8 well-formed + no C0/DEL */
    p = value;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f)
            return 0;
        if (c < 0x80) {
            p++;
            continue;
        }
        /* multi-byte UTF-8 */
        if ((c & 0xe0) == 0xc0) {
            if ((p[1] & 0xc0) != 0x80 || c < 0xc2)
                return 0;
            p += 2;
        } else if ((c & 0xf0) == 0xe0) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
                return 0;
            p += 3;
        } else if ((c & 0xf8) == 0xf0) {
            if (c > 0xf4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80)
                return 0;
            p += 4;
        } else {
            return 0;
        }
    }
    (void)i;
    return 1;
}

int prod_oauth_state_decoded_len_ok(size_t n) {
    return n == PROD_OAUTH_STATE_DECODED_LEN;
}

static const char *const avatar_models[] = {
    "models/free/Dungeon-Master.vrm",
    "models/legend/Lynn.vrm",
    "models/legend/Volo.vrm",
    "models/legend/Midori.vrm",
};

int prod_avatar_ok(const char *model) {
    size_t i;
    if (!model)
        return 0;
    for (i = 0; i < sizeof(avatar_models) / sizeof(avatar_models[0]); i++) {
        if (strcmp(model, avatar_models[i]) == 0)
            return 1;
    }
    return 0;
}

int prod_avatar_default(char *out, size_t cap) {
    size_t n = strlen(PROD_DEFAULT_AVATAR);
    if (!out || cap <= n)
        return PROD_ERR;
    memcpy(out, PROD_DEFAULT_AVATAR, n + 1);
    return PROD_OK;
}

int prod_avatar_list(char *out, size_t cap) {
    size_t i, pos = 0;
    if (!out || cap == 0)
        return PROD_ERR;
    out[0] = '\0';
    for (i = 0; i < sizeof(avatar_models) / sizeof(avatar_models[0]); i++) {
        size_t n = strlen(avatar_models[i]);
        if (pos + n + 2 > cap)
            return PROD_ERR;
        memcpy(out + pos, avatar_models[i], n);
        pos += n;
        out[pos++] = '\n';
        out[pos] = '\0';
    }
    return PROD_OK;
}

static const char *const mission_activities[] = {
    "login_day",
    "rules_question",
    "combat_started",
    "nat20_rolled",
    "dm_interrupted",
    "chat_exported",
    "nickname_set",
};

int prod_mission_activity_ok(const char *activity_key) {
    size_t i;
    if (!activity_key || !activity_key[0])
        return 0;
    for (i = 0; i < sizeof(mission_activities) / sizeof(mission_activities[0]); i++) {
        if (strcmp(activity_key, mission_activities[i]) == 0)
            return 1;
    }
    return 0;
}

/* lowercase copy into buf; returns 0 on overflow */
static int lower_copy(const char *in, char *buf, size_t cap) {
    size_t i, n;
    /* skip leading space */
    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r')
        in++;
    n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\n' || in[n - 1] == '\r'))
        n--;
    if (n + 1 > cap)
        return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        buf[i] = (char)tolower(c);
    }
    buf[n] = '\0';
    return 0;
}

static int contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

int prod_looks_like_rules_question(const char *message) {
    char text[4096];
    int question_like;
    size_t i;
    static const char *const phrases[] = {
        "how does", "what happens if", "can i ", "can you ", "when do", "do i need to", "does ",
        "is it legal",
    };
    static const char *const terms[] = {
        "initiative", "bonus action", "action", "reaction", "saving throw", "spell", "concentration",
        "armor class", "hit points", "advantage", "disadvantage", "attack", "combat", "turn", "d20",
    };

    if (!message)
        return 0;
    if (lower_copy(message, text, sizeof(text)) != 0)
        return 0;
    if (text[0] == '\0')
        return 0;
    if (contains(text, "rule") || contains(text, "rules"))
        return 1;

    question_like = contains(text, "?");
    for (i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (contains(text, phrases[i])) {
            question_like = 1;
            break;
        }
    }
    if (!question_like)
        return 0;
    for (i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        if (contains(text, terms[i]))
            return 1;
    }
    return 0;
}

static int copy_out(const char *src, char *out, size_t cap) {
    size_t n;
    if (!src || !out || cap == 0)
        return PROD_ERR;
    n = strlen(src);
    if (n + 1 > cap)
        return PROD_ERR;
    memcpy(out, src, n + 1);
    return PROD_OK;
}

/* trim leading/trailing ASCII space like Go strings.TrimSpace for common cases */
static const char *trim_span(const char *s, size_t *n_out) {
    size_t n;
    if (!s) {
        *n_out = 0;
        return "";
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f')
        s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == '\v' || s[n - 1] == '\f'))
        n--;
    *n_out = n;
    return s;
}

int prod_oauth_provider_error_norm(const char *raw, char *out, size_t cap) {
    char buf[128];
    size_t n, i;
    const char *p;
    if (!out || cap == 0)
        return PROD_ERR;
    p = trim_span(raw ? raw : "", &n);
    if (n + 1 > sizeof(buf))
        return copy_out("provider_error", out, cap);
    for (i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)p[i]);
    buf[n] = '\0';
    if (strcmp(buf, "access_denied") == 0 || strcmp(buf, "temporarily_unavailable") == 0 ||
        strcmp(buf, "server_error") == 0)
        return copy_out(buf, out, cap);
    return copy_out("provider_error", out, cap);
}

int prod_oauth_local_failure_norm(const char *raw, char *out, size_t cap) {
    static const char *const allow[] = {
        "access_denied",
        "temporarily_unavailable",
        "server_error",
        "provider_error",
        "invalid_state",
        "state_mismatch",
        "database_error",
        "no_code",
        "token_exchange_failed",
        "userinfo_failed",
        "user_creation_failed",
        "identity_conflict",
    };
    size_t i;
    if (!out || cap == 0)
        return PROD_ERR;
    if (raw) {
        for (i = 0; i < sizeof(allow) / sizeof(allow[0]); i++) {
            if (strcmp(raw, allow[i]) == 0)
                return copy_out(raw, out, cap);
        }
    }
    return copy_out("provider_error", out, cap);
}

static int dim_char_ok(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    if (c == '-' || c == '_' || c == ':' || c == '.' || c == '@')
        return 1;
    /* non-ASCII UTF-8 lead/continuation — accept as letter-like (Go unicode.IsLetter class) */
    if (c >= 0x80)
        return 1;
    return 0;
}

int prod_bounded_dimension(const char *value, int limit, char *out, size_t cap) {
    size_t n, i;
    const char *p;
    if (!out || cap == 0 || limit < 0)
        return PROD_ERR;
    out[0] = '\0';
    p = trim_span(value ? value : "", &n);
    if (n == 0 || (int)n > limit)
        return PROD_ERR;
    for (i = 0; i < n; i++) {
        if (!dim_char_ok((unsigned char)p[i]))
            return PROD_ERR;
    }
    if (n + 1 > cap)
        return PROD_ERR;
    memcpy(out, p, n);
    out[n] = '\0';
    return PROD_OK;
}

int prod_default_dimension(const char *value, const char *fallback, int limit, char *out, size_t cap) {
    if (prod_bounded_dimension(value, limit, out, cap) == PROD_OK)
        return PROD_OK;
    return copy_out(fallback ? fallback : "", out, cap);
}

int prod_bounded_path(const char *value, char *out, size_t cap) {
    size_t n, i;
    const char *p;
    if (!out || cap == 0)
        return PROD_ERR;
    out[0] = '\0';
    p = trim_span(value ? value : "", &n);
    if (n == 0 || n > 256 || p[0] != '/')
        return PROD_ERR;
    for (i = 0; i < n; i++) {
        if (p[i] == '?') {
            n = i;
            break;
        }
    }
    if (n == 0 || n + 1 > cap)
        return PROD_ERR;
    memcpy(out, p, n);
    out[n] = '\0';
    return PROD_OK;
}

int prod_default_transports(const char *service_type, char *out, size_t cap) {
    const char *list = NULL;
    if (!service_type || !out || cap == 0)
        return PROD_ERR;
    if (strcmp(service_type, "llm") == 0)
        list = "http-chat,websocket-stream";
    else if (strcmp(service_type, "stt") == 0)
        list = "browser-audio,http-transcription";
    else if (strcmp(service_type, "tts") == 0)
        list = "http-wav,http-sse";
    else {
        out[0] = '\0';
        return PROD_ERR;
    }
    return copy_out(list, out, cap);
}

int prod_infer_provider(const char *raw_url, char *out, size_t cap) {
    char buf[2048];
    size_t n, i;
    const char *p;
    if (!out || cap == 0)
        return PROD_ERR;
    p = trim_span(raw_url ? raw_url : "", &n);
    if (n == 0)
        return copy_out("", out, cap);
    if (n + 1 > sizeof(buf))
        n = sizeof(buf) - 1;
    for (i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)p[i]);
    buf[n] = '\0';
    if (strstr(buf, "vllm"))
        return copy_out("vLLM", out, cap);
    if (strstr(buf, "orpheus"))
        return copy_out("Orpheus", out, cap);
    if (strstr(buf, "whisper"))
        return copy_out("Whisper", out, cap);
    if (strstr(buf, "ray"))
        return copy_out("Ray Serve", out, cap);
    return copy_out("custom", out, cap);
}

int prod_health_mode(int llm_ok, int stt_ok, int tts_ok, char *out, size_t cap) {
    if (!out || cap == 0)
        return PROD_ERR;
    if (llm_ok && stt_ok && tts_ok)
        return copy_out("healthy", out, cap);
    if (llm_ok)
        return copy_out("degraded", out, cap);
    return copy_out("down", out, cap);
}

int prod_gateway_turns_path(const char *path, char *out, size_t cap) {
    size_t n;
    const char *p;
    char tmp[512];
    if (!out || cap == 0)
        return PROD_ERR;
    p = path ? path : "";
    n = strlen(p);
    /* TrimRight(path, "/") */
    while (n > 0 && p[n - 1] == '/')
        n--;
    if (n + 1 > sizeof(tmp))
        return PROD_ERR;
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    if (tmp[0] == '\0' || strcmp(tmp, "/v1/voice/turns") == 0)
        return copy_out("/v1/voice/turns", out, cap);
    return PROD_ERR;
}

int prod_sanitize_endpoint(const char *raw_url, char *out, size_t cap) {
    const char *scheme_end, *auth, *host_start, *path_start, *q, *f;
    size_t scheme_len, host_len, path_len, need;
    if (!out || cap == 0)
        return PROD_ERR;
    if (!raw_url || !raw_url[0]) {
        out[0] = '\0';
        return PROD_OK;
    }
    /* scheme:// */
    scheme_end = strstr(raw_url, "://");
    if (!scheme_end || scheme_end == raw_url)
        return copy_out(raw_url, out, cap);
    scheme_len = (size_t)(scheme_end - raw_url);
    auth = scheme_end + 3;
    /* path starts at first / after authority, or end */
    path_start = strchr(auth, '/');
    host_start = auth;
    /* strip userinfo: if @ before path, host starts after last @ in authority */
    {
        const char *at;
        const char *auth_end = path_start ? path_start : auth + strlen(auth);
        at = memchr(auth, '@', (size_t)(auth_end - auth));
        if (at)
            host_start = at + 1;
    }
    if (path_start) {
        path_len = strlen(path_start);
        q = memchr(path_start, '?', path_len);
        f = memchr(path_start, '#', path_len);
        if (q && (!f || q < f))
            path_len = (size_t)(q - path_start);
        else if (f)
            path_len = (size_t)(f - path_start);
    } else {
        path_len = 0;
        /* host may still have ? or # */
        q = strchr(host_start, '?');
        f = strchr(host_start, '#');
        if (q || f) {
            const char *cut = q;
            if (f && (!cut || f < cut))
                cut = f;
            host_len = (size_t)(cut - host_start);
        } else {
            host_len = strlen(host_start);
        }
        need = scheme_len + 3 + host_len + 1;
        if (need > cap)
            return PROD_ERR;
        memcpy(out, raw_url, scheme_len);
        memcpy(out + scheme_len, "://", 3);
        memcpy(out + scheme_len + 3, host_start, host_len);
        out[scheme_len + 3 + host_len] = '\0';
        return PROD_OK;
    }
    host_len = (size_t)(path_start - host_start);
    /* host may include port; strip ? # already handled via path */
    need = scheme_len + 3 + host_len + path_len + 1;
    if (need > cap)
        return PROD_ERR;
    memcpy(out, raw_url, scheme_len);
    memcpy(out + scheme_len, "://", 3);
    memcpy(out + scheme_len + 3, host_start, host_len);
    memcpy(out + scheme_len + 3 + host_len, path_start, path_len);
    out[scheme_len + 3 + host_len + path_len] = '\0';
    return PROD_OK;
}

int prod_export_format(const char *format, char *mime_out, size_t mime_cap, char *ext_out,
                       size_t ext_cap) {
    const char *mime, *ext;
    if (!mime_out || !ext_out || mime_cap == 0 || ext_cap == 0)
        return PROD_ERR;
    if (format && strcmp(format, "markdown") == 0) {
        mime = "text/markdown; charset=utf-8";
        ext = "md";
    } else if (format && strcmp(format, "txt") == 0) {
        mime = "text/plain; charset=utf-8";
        ext = "txt";
    } else if (format && strcmp(format, "html") == 0) {
        mime = "text/html; charset=utf-8";
        ext = "html";
    } else {
        mime = "application/json; charset=utf-8";
        ext = "json";
    }
    if (copy_out(mime, mime_out, mime_cap) != PROD_OK)
        return PROD_ERR;
    return copy_out(ext, ext_out, ext_cap);
}

int prod_export_role_name(const char *sender, char *out, size_t cap) {
    if (!out || cap == 0)
        return PROD_ERR;
    if (sender && strcmp(sender, "user") == 0)
        return copy_out("You", out, cap);
    return copy_out("Companion", out, cap);
}

static int append_str(char *out, size_t cap, size_t *len, const char *s) {
    size_t n;
    if (!s)
        s = "";
    n = strlen(s);
    if (*len + n + 1 > cap)
        return PROD_ERR;
    memcpy(out + *len, s, n + 1);
    *len += n;
    return PROD_OK;
}

static int append_fmt(char *out, size_t cap, size_t *len, const char *fmt, ...)
    PROD_PRINTF_LIKE(4, 5);

static int append_fmt(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return PROD_ERR;
    return append_str(out, cap, len, tmp);
}

static int append_html_esc(char *out, size_t cap, size_t *len, const char *s) {
    if (!s)
        s = "";
    for (; *s; s++) {
        const char *rep = NULL;
        char one[2];
        if (*s == '&')
            rep = "&amp;";
        else if (*s == '<')
            rep = "&lt;";
        else if (*s == '>')
            rep = "&gt;";
        else if (*s == '"')
            rep = "&quot;";
        else if (*s == '\'')
            rep = "&#39;";
        if (rep) {
            if (append_str(out, cap, len, rep) != PROD_OK)
                return PROD_ERR;
        } else {
            one[0] = *s;
            one[1] = '\0';
            if (append_str(out, cap, len, one) != PROD_OK)
                return PROD_ERR;
        }
    }
    return PROD_OK;
}

static int append_json_esc(char *out, size_t cap, size_t *len, const char *s) {
    if (!s)
        s = "";
    for (; *s; s++) {
        char buf[8];
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[0] = '\\';
            buf[1] = (char)c;
            buf[2] = '\0';
            if (append_str(out, cap, len, buf) != PROD_OK)
                return PROD_ERR;
        } else if (c == '\n') {
            if (append_str(out, cap, len, "\\n") != PROD_OK)
                return PROD_ERR;
        } else if (c == '\r') {
            if (append_str(out, cap, len, "\\r") != PROD_OK)
                return PROD_ERR;
        } else if (c == '\t') {
            if (append_str(out, cap, len, "\\t") != PROD_OK)
                return PROD_ERR;
        } else if (c < 0x20) {
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            if (append_str(out, cap, len, buf) != PROD_OK)
                return PROD_ERR;
        } else {
            buf[0] = (char)c;
            buf[1] = '\0';
            if (append_str(out, cap, len, buf) != PROD_OK)
                return PROD_ERR;
        }
    }
    return PROD_OK;
}

static int export_json(const char *export_date, const prod_export_msg *msgs, size_t nmsgs,
                       char *out, size_t cap) {
    size_t len = 0;
    size_t i;
    if (append_str(out, cap, &len, "{\n  \"exportDate\": \"") != PROD_OK)
        return PROD_ERR;
    if (append_json_esc(out, cap, &len, export_date ? export_date : "") != PROD_OK)
        return PROD_ERR;
    if (append_str(out, cap, &len,
                   "\",\n  \"format\": \"companions-export-v1\",\n  \"messages\": [") != PROD_OK)
        return PROD_ERR;
    for (i = 0; i < nmsgs; i++) {
        if (i == 0) {
            if (append_str(out, cap, &len, "\n") != PROD_OK)
                return PROD_ERR;
        } else {
            if (append_str(out, cap, &len, ",\n") != PROD_OK)
                return PROD_ERR;
        }
        if (append_str(out, cap, &len, "    {\n      \"role\": \"") != PROD_OK)
            return PROD_ERR;
        if (append_json_esc(out, cap, &len, msgs[i].sender) != PROD_OK)
            return PROD_ERR;
        if (append_str(out, cap, &len, "\",\n      \"content\": \"") != PROD_OK)
            return PROD_ERR;
        if (append_json_esc(out, cap, &len, msgs[i].text) != PROD_OK)
            return PROD_ERR;
        if (append_str(out, cap, &len, "\"\n    }") != PROD_OK)
            return PROD_ERR;
    }
    if (nmsgs > 0) {
        if (append_str(out, cap, &len, "\n  ") != PROD_OK)
            return PROD_ERR;
    }
    return append_str(out, cap, &len, "]\n}");
}

static int export_markdown(const char *export_date, const prod_export_msg *msgs, size_t nmsgs,
                           char *out, size_t cap) {
    size_t len = 0, i;
    char role[32];
    if (append_str(out, cap, &len, "---\n") != PROD_OK)
        return PROD_ERR;
    if (append_fmt(out, cap, &len, "title: Companions Chat Export %s\n",
                   export_date ? export_date : "") != PROD_OK)
        return PROD_ERR;
    if (append_str(out, cap, &len, "source: companions\n") != PROD_OK)
        return PROD_ERR;
    if (append_fmt(out, cap, &len, "exported_on: %s\n", export_date ? export_date : "") != PROD_OK)
        return PROD_ERR;
    if (append_fmt(out, cap, &len, "message_count: %zu\n", nmsgs) != PROD_OK)
        return PROD_ERR;
    if (append_str(out, cap, &len,
                   "tags:\n  - companions\n  - chat-export\n  - obsidian\n---\n\n"
                   "# Companions Chat Export\n\n"
                   "This markdown export is structured to import cleanly into note tools such as "
                   "Obsidian and Affine.\n\n## Conversation\n\n") != PROD_OK)
        return PROD_ERR;
    for (i = 0; i < nmsgs; i++) {
        if (prod_export_role_name(msgs[i].sender, role, sizeof(role)) != PROD_OK)
            return PROD_ERR;
        if (append_fmt(out, cap, &len, "### %s\n\n%s\n\n", role, msgs[i].text) != PROD_OK)
            return PROD_ERR;
    }
    return PROD_OK;
}

static int export_text(const char *export_date, const prod_export_msg *msgs, size_t nmsgs,
                       char *out, size_t cap) {
    size_t len = 0, i;
    char role[32];
    if (append_str(out, cap, &len, "COMPANIONS CHAT EXPORT\n") != PROD_OK)
        return PROD_ERR;
    if (append_fmt(out, cap, &len, "Exported: %s\n", export_date ? export_date : "") != PROD_OK)
        return PROD_ERR;
    if (append_str(out, cap, &len, "==================================================\n\n") != PROD_OK)
        return PROD_ERR;
    for (i = 0; i < nmsgs; i++) {
        if (prod_export_role_name(msgs[i].sender, role, sizeof(role)) != PROD_OK)
            return PROD_ERR;
        if (append_fmt(out, cap, &len, "[%s]\n%s\n\n", role, msgs[i].text) != PROD_OK)
            return PROD_ERR;
    }
    return PROD_OK;
}

static int export_html(const char *export_date, const prod_export_msg *msgs, size_t nmsgs, char *out,
                       size_t cap) {
    size_t len = 0, i;
    char role[32];
    static const char head[] =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
        "<title>Companions Chat Export</title>\n"
        "<style>\n"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;\n"
        "max-width:800px;margin:0 auto;padding:20px;background:#1a1a2e;color:#e4e4e7}\n"
        "h1{color:#818cf8;border-bottom:2px solid #4338ca;padding-bottom:10px}\n"
        ".meta{color:#71717a;font-size:.9em;margin-bottom:20px}\n"
        ".msg{padding:15px;margin:10px 0;border-radius:8px}\n"
        ".msg.user{background:#1e40af;margin-left:40px}\n"
        ".msg.assistant{background:#374151;margin-right:40px}\n"
        ".role{font-weight:bold;margin-bottom:5px}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<h1>Companions Chat Export</h1>\n"
        "<p class=\"meta\">Exported on ";
    if (append_str(out, cap, &len, head) != PROD_OK)
        return PROD_ERR;
    if (append_html_esc(out, cap, &len, export_date ? export_date : "") != PROD_OK)
        return PROD_ERR;
    if (append_str(out, cap, &len, "</p>\n") != PROD_OK)
        return PROD_ERR;
    for (i = 0; i < nmsgs; i++) {
        const char *cls = "assistant";
        if (strcmp(msgs[i].sender, "user") == 0)
            cls = "user";
        if (prod_export_role_name(msgs[i].sender, role, sizeof(role)) != PROD_OK)
            return PROD_ERR;
        if (append_fmt(out, cap, &len, "<div class=\"msg %s\"><div class=\"role\">", cls) !=
            PROD_OK)
            return PROD_ERR;
        if (append_html_esc(out, cap, &len, role) != PROD_OK)
            return PROD_ERR;
        if (append_str(out, cap, &len, "</div><div>") != PROD_OK)
            return PROD_ERR;
        if (append_html_esc(out, cap, &len, msgs[i].text) != PROD_OK)
            return PROD_ERR;
        if (append_str(out, cap, &len, "</div></div>\n") != PROD_OK)
            return PROD_ERR;
    }
    return append_str(out, cap, &len, "</body></html>");
}

int prod_export_render(const char *format, const char *export_date, const prod_export_msg *msgs,
                       size_t nmsgs, char *out, size_t cap) {
    if (!out || cap == 0)
        return PROD_ERR;
    if (!msgs && nmsgs > 0)
        return PROD_ERR;
    out[0] = '\0';
    if (format && strcmp(format, "markdown") == 0)
        return export_markdown(export_date, msgs, nmsgs, out, cap);
    if (format && strcmp(format, "txt") == 0)
        return export_text(export_date, msgs, nmsgs, out, cap);
    if (format && strcmp(format, "html") == 0)
        return export_html(export_date, msgs, nmsgs, out, cap);
    return export_json(export_date, msgs, nmsgs, out, cap);
}

int prod_summarize_health(const char *mode, int llm_ok, int stt_ok, int tts_ok, char *out,
                          size_t cap) {
    char unavail[64];
    size_t u = 0;
    if (!out || cap == 0)
        return PROD_ERR;
    if (mode && strcmp(mode, "healthy") == 0)
        return copy_out("All companion services are ready.", out, cap);
    unavail[0] = '\0';
    if (!llm_ok) {
        memcpy(unavail + u, "LLM", 3);
        u += 3;
    }
    if (!stt_ok) {
        if (u) {
            unavail[u++] = ',';
            unavail[u++] = ' ';
        }
        memcpy(unavail + u, "STT", 3);
        u += 3;
    }
    if (!tts_ok) {
        if (u) {
            unavail[u++] = ',';
            unavail[u++] = ' ';
        }
        memcpy(unavail + u, "TTS", 3);
        u += 3;
    }
    unavail[u] = '\0';
    if (mode && strcmp(mode, "degraded") == 0) {
        snprintf(out, cap,
                 "Core chat is available, but some voice services are degraded: %s", unavail);
        return PROD_OK;
    }
    snprintf(out, cap, "Core companion services are unavailable: %s", unavail);
    return PROD_OK;
}

int prod_ws_control_kind(const char *type, char *out, size_t cap) {
    if (!out || cap == 0)
        return PROD_ERR;
    if (type && strcmp(type, "login") == 0)
        return copy_out("ignore", out, cap);
    if (type && (strcmp(type, "cancel_turn") == 0 || strcmp(type, "stop_tts") == 0))
        return copy_out("cancel", out, cap);
    return copy_out("unknown", out, cap);
}

int prod_vision_consent_action_ok(const char *action) {
    if (!action)
        return 0;
    return strcmp(action, "start") == 0 || strcmp(action, "revoke") == 0 ||
           strcmp(action, "stop") == 0;
}

#include <openssl/evp.h>

int prod_turn_identity_derive_key(const unsigned char *root, size_t root_len, char *out_hex,
                                  size_t cap) {
    unsigned char sum[32];
    unsigned int sum_len = 0;
    EVP_MD_CTX *ctx;
    size_t i;
    static const char *hexd = "0123456789abcdef";

    if (!out_hex || cap < 65)
        return PROD_ERR;
    out_hex[0] = '\0';
    if (!root || root_len == 0)
        return PROD_ERR;

    ctx = EVP_MD_CTX_new();
    if (!ctx)
        return PROD_ERR;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, root, root_len) != 1 ||
        EVP_DigestUpdate(ctx, PROD_TURN_IDENTITY_INFO, strlen(PROD_TURN_IDENTITY_INFO)) != 1 ||
        EVP_DigestFinal_ex(ctx, sum, &sum_len) != 1 || sum_len != 32) {
        EVP_MD_CTX_free(ctx);
        return PROD_ERR;
    }
    EVP_MD_CTX_free(ctx);
    for (i = 0; i < 32; i++) {
        out_hex[i * 2] = hexd[(sum[i] >> 4) & 0xf];
        out_hex[i * 2 + 1] = hexd[sum[i] & 0xf];
    }
    out_hex[64] = '\0';
    return PROD_OK;
}

int prod_turn_identity_fields_ok(const char *request_id, const char *user_id, const char *username) {
    size_t n;
    if (!request_id || !user_id || !username)
        return 0;
    n = strlen(request_id);
    if (n == 0 || n > PROD_TURN_IDENTITY_MAX_FIELD)
        return 0;
    n = strlen(user_id);
    if (n == 0 || n > PROD_TURN_IDENTITY_MAX_FIELD)
        return 0;
    n = strlen(username);
    if (n == 0 || n > PROD_TURN_IDENTITY_MAX_FIELD)
        return 0;
    return 1;
}

/* ── Default seed catalogs (pure-C; dual-run Go tables deleted) ── */

typedef struct {
    const char *id;
    const char *name;
    const char *type;
    const char *cr;
    const char *subtitle;
    int max_hp;
    int armor_class;
    const char *image_key;
    const char *tier;
} prod_monster_token_seed;

static const prod_monster_token_seed monster_token_seeds[] = {
    /* free */
    {"goblin", "Goblin", "enemy", "CR 1/4", "Small Humanoid", 7, 15, "tokens/free/goblin.webp",
     "free"},
    {"kobold", "Kobold", "enemy", "CR 1/8", "Small Humanoid", 5, 12, "tokens/free/kobold.webp",
     "free"},
    {"skeleton", "Skeleton", "enemy", "CR 1/4", "Medium Undead", 13, 13, "tokens/free/skeleton.webp",
     "free"},
    {"zombie", "Zombie", "enemy", "CR 1/4", "Medium Undead", 22, 8, "tokens/free/zombie.webp",
     "free"},
    {"giant_rat", "Giant Rat", "enemy", "CR 1/8", "Small Beast", 7, 12, "tokens/free/giant_rat.webp",
     "free"},
    {"wolf", "Wolf", "enemy", "CR 1/4", "Medium Beast", 11, 13, "tokens/free/wolf.webp", "free"},
    {"bandit", "Bandit", "enemy", "CR 1/8", "Medium Humanoid", 11, 12, "tokens/free/bandit.webp",
     "free"},
    /* adventurer */
    {"goblin_boss", "Goblin Boss", "enemy", "CR 1", "Small Humanoid", 21, 17,
     "tokens/adventurer/goblin_boss.webp", "adventurer"},
    {"hobgoblin", "Hobgoblin", "enemy", "CR 1/2", "Medium Humanoid", 11, 18,
     "tokens/adventurer/hobgoblin.webp", "adventurer"},
    {"orc", "Orc", "enemy", "CR 1/2", "Medium Humanoid", 15, 13, "tokens/adventurer/orc.webp",
     "adventurer"},
    {"bugbear", "Bugbear", "enemy", "CR 1", "Medium Humanoid", 27, 16,
     "tokens/adventurer/bugbear.webp", "adventurer"},
    {"ghoul", "Ghoul", "enemy", "CR 1", "Medium Undead", 22, 12, "tokens/adventurer/ghoul.webp",
     "adventurer"},
    {"dire_wolf", "Dire Wolf", "enemy", "CR 1", "Large Beast", 37, 14,
     "tokens/adventurer/dire_wolf.webp", "adventurer"},
    {"giant_spider", "Giant Spider", "enemy", "CR 1", "Large Beast", 26, 14,
     "tokens/adventurer/giant_spider.webp", "adventurer"},
    {"thug", "Thug", "enemy", "CR 1/2", "Medium Humanoid", 32, 11, "tokens/adventurer/thug.webp",
     "adventurer"},
    {"imp", "Imp", "enemy", "CR 1", "Tiny Fiend", 10, 13, "tokens/adventurer/imp.webp",
     "adventurer"},
    {"animated_armor", "Animated Armor", "enemy", "CR 1", "Medium Construct", 33, 18,
     "tokens/adventurer/animated_armor.webp", "adventurer"},
    /* hero */
    {"bandit_captain", "Bandit Captain", "enemy", "CR 2", "Medium Humanoid", 65, 15,
     "tokens/hero/bandit_captain.webp", "hero"},
    {"cult_fanatic", "Cult Fanatic", "enemy", "CR 2", "Medium Humanoid", 33, 13,
     "tokens/hero/cult_fanatic.webp", "hero"},
    {"ogre", "Ogre", "enemy", "CR 2", "Large Giant", 59, 11, "tokens/hero/ogre.webp", "hero"},
    {"mimic", "Mimic", "enemy", "CR 2", "Medium Monstrosity", 58, 12, "tokens/hero/mimic.webp",
     "hero"},
    {"gelatinous_cube", "Gelatinous Cube", "enemy", "CR 2", "Large Ooze", 84, 6,
     "tokens/hero/gelatinous_cube.webp", "hero"},
    {"dragon_wyrmling_green", "Green Dragon Wyrmling", "enemy", "CR 2", "Medium Dragon", 38, 17,
     "tokens/hero/dragon_wyrmling_green.webp", "hero"},
    {"owlbear", "Owlbear", "enemy", "CR 3", "Large Monstrosity", 59, 13, "tokens/hero/owlbear.webp",
     "hero"},
    {"ghost", "Ghost", "enemy", "CR 4", "Medium Undead", 45, 11, "tokens/hero/ghost.webp", "hero"},
    {"orc_war_chief", "Orc War Chief", "enemy", "CR 4", "Medium Humanoid", 93, 16,
     "tokens/hero/orc_war_chief.webp", "hero"},
    {"dragon_wyrmling_red", "Red Dragon Wyrmling", "enemy", "CR 4", "Medium Dragon", 75, 17,
     "tokens/hero/dragon_wyrmling_red.webp", "hero"},
    {"troll", "Troll", "enemy", "CR 5", "Large Giant", 84, 15, "tokens/hero/troll.webp", "hero"},
    {"hill_giant", "Hill Giant", "enemy", "CR 5", "Huge Giant", 105, 13, "tokens/hero/hill_giant.webp",
     "hero"},
    /* legend */
    {"young_dragon_green", "Young Green Dragon", "enemy", "CR 8", "Large Dragon", 136, 18,
     "tokens/legend/young_dragon_green.webp", "legend"},
    {"young_dragon_red", "Young Red Dragon", "enemy", "CR 10", "Large Dragon", 178, 18,
     "tokens/legend/young_dragon_red.webp", "legend"},
    {"giant_fire", "Fire Giant", "enemy", "CR 9", "Huge Giant", 162, 18,
     "tokens/legend/giant_fire.webp", "legend"},
    {"giant_frost", "Frost Giant", "enemy", "CR 8", "Huge Giant", 138, 15,
     "tokens/legend/giant_frost.webp", "legend"},
    {"mind_flayer", "Mind Flayer", "enemy", "CR 7", "Medium Aberration", 71, 15,
     "tokens/legend/mind_flayer.webp", "legend"},
    {"beholder", "Beholder", "enemy", "CR 13", "Large Aberration", 180, 18,
     "tokens/legend/beholder.webp", "legend"},
    {"lich", "Lich", "enemy", "CR 21", "Medium Undead", 135, 17, "tokens/legend/lich.webp", "legend"},
    {"adult_dragon_red", "Adult Red Dragon", "enemy", "CR 17", "Huge Dragon", 256, 19,
     "tokens/legend/adult_dragon_red.webp", "legend"},
};

typedef struct {
    const char *id;
    const char *title;
    const char *description;
    const char *activity_key;
    int target_count;
    int reward_xp;
    const char *type;
    const char *reset_period;
    const char *tier;
    const char *icon;
    int sort_order;
    int active; /* 0|1 */
} prod_daily_mission_seed;

static const prod_daily_mission_seed daily_mission_seeds[] = {
    {"login_streak_7", "Adventurer's Discipline",
     "Log in on 7 different days to build your streak.", "login_day", 7, 150, "streak", "streak",
     "free", "flame", 10, 1},
    {"rules_question", "Rules Lawyer",
     "Ask the DM one question about rules, rulings, or mechanics.", "rules_question", 1, 35,
     "activity", "once", "free", "book-open", 20, 1},
    {"combat_started", "Into The Fray", "Start a combat encounter from the tracker.",
     "combat_started", 1, 40, "activity", "once", "free", "swords", 30, 1},
    {"nat20_rolled", "Critical Hit", "Roll a natural 20 on a d20.", "nat20_rolled", 1, 50,
     "activity", "once", "free", "dices", 40, 1},
    {"dm_interrupted", "Hold That Thought",
     "Interrupt the DM once with voice barge-in while audio is playing.", "dm_interrupted", 1, 45,
     "activity", "once", "free", "mic-off", 50, 1},
    {"chat_exported", "Campaign Archivist",
     "Export a chat in a notes-friendly format for Obsidian or Affine.", "chat_exported", 1, 30,
     "activity", "once", "free", "scroll-text", 60, 1},
    {"nickname_set", "Known By Name", "Set a nickname for your account.", "nickname_set", 1, 20,
     "activity", "once", "free", "badge-info", 70, 1},
};

size_t prod_monster_token_seed_count(void) {
    return sizeof(monster_token_seeds) / sizeof(monster_token_seeds[0]);
}

size_t prod_daily_mission_seed_count(void) {
    return sizeof(daily_mission_seeds) / sizeof(daily_mission_seeds[0]);
}

/* Append formatted piece; returns 0 on success, -1 on overflow. */
static int seed_appendf(char *out, size_t cap, size_t *pos, const char *fmt, ...)
    PROD_PRINTF_LIKE(4, 5);

static int seed_appendf(char *out, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list ap;
    int n;
    size_t rem;
    if (!out || !pos || cap == 0 || *pos >= cap)
        return -1;
    rem = cap - *pos;
    va_start(ap, fmt);
    n = vsnprintf(out + *pos, rem, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= rem)
        return -1;
    *pos += (size_t)n;
    return 0;
}

#undef PROD_PRINTF_LIKE

int prod_monster_token_seeds_json(char *out, size_t cap) {
    size_t i, pos = 0;
    size_t n = prod_monster_token_seed_count();
    if (!out || cap < 3)
        return PROD_ERR;
    if (seed_appendf(out, cap, &pos, "[") != 0)
        return PROD_ERR;
    for (i = 0; i < n; i++) {
        const prod_monster_token_seed *t = &monster_token_seeds[i];
        if (i > 0 && seed_appendf(out, cap, &pos, ",") != 0)
            return PROD_ERR;
        if (seed_appendf(out, cap, &pos,
                         "{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"cr\":\"%s\","
                         "\"subtitle\":\"%s\",\"max_hp\":%d,\"armor_class\":%d,"
                         "\"image_key\":\"%s\",\"tier\":\"%s\"}",
                         t->id, t->name, t->type, t->cr, t->subtitle, t->max_hp, t->armor_class,
                         t->image_key, t->tier) != 0)
            return PROD_ERR;
    }
    if (seed_appendf(out, cap, &pos, "]") != 0)
        return PROD_ERR;
    return PROD_OK;
}

int prod_daily_mission_seeds_json(char *out, size_t cap) {
    size_t i, pos = 0;
    size_t n = prod_daily_mission_seed_count();
    if (!out || cap < 3)
        return PROD_ERR;
    if (seed_appendf(out, cap, &pos, "[") != 0)
        return PROD_ERR;
    for (i = 0; i < n; i++) {
        const prod_daily_mission_seed *m = &daily_mission_seeds[i];
        if (i > 0 && seed_appendf(out, cap, &pos, ",") != 0)
            return PROD_ERR;
        if (seed_appendf(out, cap, &pos,
                         "{\"id\":\"%s\",\"title\":\"%s\",\"description\":\"%s\","
                         "\"activity_key\":\"%s\",\"target_count\":%d,\"reward_xp\":%d,"
                         "\"type\":\"%s\",\"reset_period\":\"%s\",\"tier\":\"%s\","
                         "\"icon\":\"%s\",\"sort_order\":%d,\"active\":%s}",
                         m->id, m->title, m->description, m->activity_key, m->target_count,
                         m->reward_xp, m->type, m->reset_period, m->tier, m->icon, m->sort_order,
                         m->active ? "true" : "false") != 0)
            return PROD_ERR;
    }
    if (seed_appendf(out, cap, &pos, "]") != 0)
        return PROD_ERR;
    return PROD_OK;
}

/* ── Progression pure-CPU ── */

/* Integer sqrt for non-negative n (floor). */
static unsigned prod_isqrt(unsigned n) {
    unsigned x = n, y, b;
    if (n == 0)
        return 0;
    y = 0;
    b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b != 0) {
        if (x >= y + b) {
            x -= y + b;
            y = (y >> 1) + b;
        } else {
            y >>= 1;
        }
        b >>= 2;
    }
    return y;
}

int prod_xp_required_for_level(int level) {
    long base;
    if (level <= 1)
        return 0;
    base = (long)level - 1;
    /* (level-1)^2 * 100 — stay in int range for practical levels */
    if (base > 46340) /* avoid overflow on 32-bit for base*base */
        base = 46340;
    return (int)(base * base * 100L);
}

int prod_level_for_xp(int total_xp) {
    unsigned u;
    int level;
    if (total_xp < 0)
        total_xp = 0;
    u = (unsigned)total_xp;
    /* floor(sqrt(total_xp/100))+1  via integer division + isqrt */
    level = (int)prod_isqrt(u / 100u) + 1;
    if (level < 1)
        level = 1;
    return level;
}

int prod_rank_for_level(int level, char *out, size_t cap) {
    const char *rank;
    size_t n;
    if (!out || cap == 0)
        return PROD_ERR;
    if (level >= 9)
        rank = "Realmwalker";
    else if (level >= 7)
        rank = "Mythic Legend";
    else if (level >= 5)
        rank = "Dungeon Hero";
    else if (level >= 3)
        rank = "Seasoned Adventurer";
    else
        rank = "Wandering Adventurer";
    n = strlen(rank);
    if (n + 1 > cap)
        return PROD_ERR;
    memcpy(out, rank, n + 1);
    return PROD_OK;
}

int prod_progression_for_xp(int total_xp, int *level_out, int *xp_into_out, int *xp_for_next_out,
                            int *next_level_xp_out, int *total_xp_out, char *rank_out,
                            size_t rank_cap) {
    int level, level_start, next_level_xp, xp_into, xp_for_next;
    if (total_xp < 0)
        total_xp = 0;
    level = prod_level_for_xp(total_xp);
    level_start = prod_xp_required_for_level(level);
    next_level_xp = prod_xp_required_for_level(level + 1);
    xp_into = total_xp - level_start;
    if (xp_into < 0)
        xp_into = 0;
    xp_for_next = next_level_xp - level_start;
    if (level_out)
        *level_out = level;
    if (xp_into_out)
        *xp_into_out = xp_into;
    if (xp_for_next_out)
        *xp_for_next_out = xp_for_next;
    if (next_level_xp_out)
        *next_level_xp_out = next_level_xp;
    if (total_xp_out)
        *total_xp_out = total_xp;
    if (rank_out) {
        if (prod_rank_for_level(level, rank_out, rank_cap) != PROD_OK)
            return PROD_ERR;
    }
    return PROD_OK;
}

int prod_progression_json(int total_xp, char *out, size_t cap) {
    int level, xp_into, xp_for_next, next_level_xp, tot;
    char rank[PROD_RANK_CAP];
    if (!out || cap < 32)
        return PROD_ERR;
    if (prod_progression_for_xp(total_xp, &level, &xp_into, &xp_for_next, &next_level_xp, &tot, rank,
                                sizeof(rank)) != PROD_OK)
        return PROD_ERR;
    if (snprintf(out, cap,
                 "{\"total_xp\":%d,\"level\":%d,\"rank\":\"%s\",\"xp_into_level\":%d,"
                 "\"xp_for_next_level\":%d,\"next_level_xp\":%d}",
                 tot, level, rank, xp_into, xp_for_next, next_level_xp) >= (int)cap)
        return PROD_ERR;
    return PROD_OK;
}

/* ── OAuth provider config tables ── */

typedef struct {
    const char *name;
    const char *auth_url;     /* may be empty for env-driven OIDC */
    const char *token_url;
    const char *userinfo_url;
    const char *scopes_csv;
} prod_oauth_provider_row;

static const prod_oauth_provider_row oauth_providers[] = {
    {"github", "https://github.com/login/oauth/authorize",
     "https://github.com/login/oauth/access_token", "https://api.github.com/user",
     "read:user,user:email"},
    {"google", "https://accounts.google.com/o/oauth2/auth", "https://oauth2.googleapis.com/token",
     "https://www.googleapis.com/oauth2/v2/userinfo", "openid,profile,email"},
    {"authentik", "", "", "", "openid,profile,email"},
    {"oidc", "", "", "", "openid,profile,email"},
};

int prod_oauth_provider_config(const char *provider, char *auth_url, size_t auth_cap,
                               char *token_url, size_t token_cap, char *userinfo_url,
                               size_t userinfo_cap, char *scopes_csv, size_t scopes_cap) {
    size_t i;
    char name[64];
    size_t n, j;
    if (!provider)
        return PROD_ERR;
    /* lowercase copy of provider name (trimmed) */
    while (*provider == ' ' || *provider == '\t')
        provider++;
    n = strlen(provider);
    while (n > 0 && (provider[n - 1] == ' ' || provider[n - 1] == '\t'))
        n--;
    if (n == 0 || n >= sizeof(name))
        return PROD_ERR;
    for (j = 0; j < n; j++) {
        unsigned char c = (unsigned char)provider[j];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        name[j] = (char)c;
    }
    name[n] = '\0';
    for (i = 0; i < sizeof(oauth_providers) / sizeof(oauth_providers[0]); i++) {
        if (strcmp(name, oauth_providers[i].name) == 0) {
            if (auth_url && copy_out(oauth_providers[i].auth_url, auth_url, auth_cap) != PROD_OK)
                return PROD_ERR;
            if (token_url && copy_out(oauth_providers[i].token_url, token_url, token_cap) != PROD_OK)
                return PROD_ERR;
            if (userinfo_url &&
                copy_out(oauth_providers[i].userinfo_url, userinfo_url, userinfo_cap) != PROD_OK)
                return PROD_ERR;
            if (scopes_csv &&
                copy_out(oauth_providers[i].scopes_csv, scopes_csv, scopes_cap) != PROD_OK)
                return PROD_ERR;
            return PROD_OK;
        }
    }
    return PROD_ERR;
}

/* ── OAuth endpoint URL validation ── */

static int is_loopback_host(const char *host, size_t n) {
    char buf[64];
    size_t i;
    if (n == 0 || n >= sizeof(buf))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        buf[i] = (char)c;
    }
    buf[n] = '\0';
    if (strcmp(buf, "localhost") == 0)
        return 1;
    if (strcmp(buf, "127.0.0.1") == 0)
        return 1;
    if (strcmp(buf, "::1") == 0 || strcmp(buf, "[::1]") == 0)
        return 1;
    /* 127.x.x.x */
    if (n >= 4 && buf[0] == '1' && buf[1] == '2' && buf[2] == '7' && buf[3] == '.')
        return 1;
    return 0;
}

int prod_oauth_endpoint_url(const char *raw, int callback, char *out, size_t cap) {
    const char *p, *host_start, *host_end, *path_start;
    size_t scheme_len, host_len, path_len, raw_len;
    int https = 0, http = 0;
    char path_buf[PROD_OAUTH_URL_CAP];
    const char *seg, *slash;

    if (!raw || !out || cap == 0)
        return PROD_ERR;
    /* must equal trim (no leading/trailing space) and non-empty */
    raw_len = strlen(raw);
    if (raw_len == 0)
        return PROD_ERR;
    if (raw[0] == ' ' || raw[0] == '\t' || raw[raw_len - 1] == ' ' || raw[raw_len - 1] == '\t')
        return PROD_ERR;

    /* scheme:// */
    p = strstr(raw, "://");
    if (!p || p == raw)
        return PROD_ERR;
    scheme_len = (size_t)(p - raw);
    if (scheme_len == 5 && strncmp(raw, "https", 5) == 0)
        https = 1;
    else if (scheme_len == 4 && strncmp(raw, "http", 4) == 0)
        http = 1;
    else
        return PROD_ERR;

    host_start = p + 3;
    if (*host_start == '\0')
        return PROD_ERR;
    /* reject userinfo: host must not contain @ before path */
    for (p = host_start; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (*p == '@')
            return PROD_ERR;
    }
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0)
        return PROD_ERR;
    /* strip brackets optional; port allowed in host */
    /* no query/fragment */
    if (strchr(host_end, '?') || strchr(host_end, '#'))
        return PROD_ERR;
    /* path required */
    if (*host_end != '/')
        return PROD_ERR;
    path_start = host_end;
    path_len = strlen(path_start);
    if (path_len == 0 || path_len >= sizeof(path_buf))
        return PROD_ERR;
    if (strchr(path_start, '\\') || strstr(path_start, "//"))
        return PROD_ERR;
    memcpy(path_buf, path_start, path_len + 1);
    /* segment check for . and .. */
    seg = path_buf;
    if (*seg == '/')
        seg++;
    while (*seg) {
        slash = strchr(seg, '/');
        if (!slash) {
            if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0)
                return PROD_ERR;
            break;
        }
        if ((size_t)(slash - seg) == 1 && seg[0] == '.')
            return PROD_ERR;
        if ((size_t)(slash - seg) == 2 && seg[0] == '.' && seg[1] == '.')
            return PROD_ERR;
        seg = slash + 1;
    }
    /* scheme policy */
    if (https) {
        /* ok */
    } else if (http) {
        /* host without port for loopback check */
        const char *colon = memchr(host_start, ':', host_len);
        size_t hlen = colon ? (size_t)(colon - host_start) : host_len;
        /* IPv6 in brackets */
        if (hlen >= 2 && host_start[0] == '[') {
            const char *rb = memchr(host_start, ']', hlen);
            if (!rb)
                return PROD_ERR;
            if (!is_loopback_host(host_start, (size_t)(rb - host_start + 1)) &&
                !is_loopback_host(host_start + 1, (size_t)(rb - host_start - 1)))
                return PROD_ERR;
        } else if (!is_loopback_host(host_start, hlen)) {
            return PROD_ERR;
        }
    } else {
        return PROD_ERR;
    }
    if (callback) {
        if (strcmp(path_start, "/api/oauth/callback") != 0)
            return PROD_ERR;
    }
    if (raw_len + 1 > cap)
        return PROD_ERR;
    memcpy(out, raw, raw_len + 1);
    return PROD_OK;
}

/* ── JWT product-token constants ── */

int prod_jwt_min_key_bytes(void) { return PROD_JWT_MIN_KEY_BYTES; }

int prod_jwt_key_len_ok(size_t n) { return n >= (size_t)PROD_JWT_MIN_KEY_BYTES ? 1 : 0; }

int prod_jwt_issuer(char *out, size_t cap) { return copy_out(PROD_JWT_ISSUER, out, cap); }

int prod_jwt_audience(char *out, size_t cap) { return copy_out(PROD_JWT_AUDIENCE, out, cap); }

/* ── Storage asset path policy ── */

static int ends_with(const char *s, const char *suf) {
    size_t n, m;
    if (!s || !suf)
        return 0;
    n = strlen(s);
    m = strlen(suf);
    if (m == 0 || n < m)
        return 0;
    return memcmp(s + n - m, suf, m) == 0;
}

int prod_asset_is_token_image(const char *key) {
    if (!key || !key[0])
        return 0;
    return ends_with(key, ".webp") || ends_with(key, ".svg") || ends_with(key, ".png");
}

int prod_asset_is_vrm(const char *key) {
    if (!key || !key[0])
        return 0;
    return ends_with(key, ".vrm");
}

int prod_asset_is_vrma(const char *key) {
    if (!key || !key[0])
        return 0;
    return ends_with(key, ".vrma");
}

int prod_asset_key_safe(const char *key) {
    if (!key || !key[0])
        return 0;
    if (strstr(key, "..") != NULL)
        return 0;
    return 1;
}

int prod_asset_extract_name(const char *key, char *out, size_t cap) {
    const char *base, *slash, *dot;
    size_t n;
    if (!out || cap == 0)
        return PROD_ERR;
    out[0] = '\0';
    if (!key) {
        return PROD_OK;
    }
    /* last path segment */
    base = key;
    for (slash = key; *slash; slash++) {
        if (*slash == '/')
            base = slash + 1;
    }
    /* strip final extension */
    dot = strrchr(base, '.');
    if (dot && dot != base)
        n = (size_t)(dot - base);
    else
        n = strlen(base);
    if (n + 1 > cap)
        return PROD_ERR;
    if (n > 0)
        memcpy(out, base, n);
    out[n] = '\0';
    return PROD_OK;
}

int prod_token_image_fallback(char *out, size_t cap) {
    return copy_out(PROD_TOKEN_IMAGE_FALLBACK, out, cap);
}

int prod_avatar_upload_ext_ok(const char *filename_or_ext) {
    const char *p;
    char buf[16];
    size_t n, i;
    if (!filename_or_ext || !filename_or_ext[0])
        return 0;
    /* Prefer last '.' segment as extension */
    p = strrchr(filename_or_ext, '.');
    if (p)
        p++; /* skip '.' */
    else
        p = filename_or_ext;
    n = strlen(p);
    if (n == 0 || n + 1 > sizeof(buf))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        buf[i] = (char)c;
    }
    buf[n] = '\0';
    return strcmp(buf, "jpg") == 0 || strcmp(buf, "jpeg") == 0 || strcmp(buf, "png") == 0 ||
           strcmp(buf, "gif") == 0 || strcmp(buf, "webp") == 0;
}

int prod_avatar_upload_content_type_ok(const char *content_type) {
    char buf[64];
    size_t n, i, j = 0;
    if (!content_type)
        return 0;
    /* trim + lowercase */
    while (*content_type == ' ' || *content_type == '\t')
        content_type++;
    n = strlen(content_type);
    while (n > 0 && (content_type[n - 1] == ' ' || content_type[n - 1] == '\t'))
        n--;
    if (n == 0 || n >= sizeof(buf))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)content_type[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        /* strip parameters after ';' */
        if (c == ';')
            break;
        buf[j++] = (char)c;
    }
    while (j > 0 && (buf[j - 1] == ' ' || buf[j - 1] == '\t'))
        j--;
    buf[j] = '\0';
    return strcmp(buf, "image/jpeg") == 0 || strcmp(buf, "image/png") == 0 ||
           strcmp(buf, "image/gif") == 0 || strcmp(buf, "image/webp") == 0;
}

int prod_avatar_upload_default_ext(char *out, size_t cap) {
    return copy_out(".jpg", out, cap);
}

int prod_asset_tier_from_key(const char *key, char *out, size_t cap) {
    const char *p;
    if (!out || cap < 8)
        return PROD_ERR;
    if (!key || !key[0])
        return copy_out("free", out, cap);
    /* models/{tier}/… or tokens/{tier}/… */
    if (strncmp(key, "models/", 7) == 0)
        p = key + 7;
    else if (strncmp(key, "tokens/", 7) == 0)
        p = key + 7;
    else
        return copy_out("free", out, cap);
    if (strncmp(p, "legend/", 7) == 0)
        return copy_out("legend", out, cap);
    if (strncmp(p, "hero/", 5) == 0)
        return copy_out("hero", out, cap);
    if (strncmp(p, "adventurer/", 11) == 0)
        return copy_out("adventurer", out, cap);
    if (strncmp(p, "free/", 5) == 0)
        return copy_out("free", out, cap);
    return copy_out("free", out, cap);
}

int prod_mission_type_ok(const char *type) {
    if (!type || !type[0])
        return 0;
    return strcmp(type, "streak") == 0 || strcmp(type, "activity") == 0;
}

int prod_mission_type_is_streak(const char *type) {
    return type && strcmp(type, "streak") == 0;
}

/* ── OAuth userinfo field mapping (provider tables live here, not in Go) ── */

static const char *ui_find_key(const char *json, const char *key) {
    char pat[160];
    const char *p;
    size_t klen;
    if (!json || !key || !key[0])
        return NULL;
    klen = strlen(key);
    if (klen + 4 >= sizeof(pat))
        return NULL;
    pat[0] = '"';
    memcpy(pat + 1, key, klen);
    pat[1 + klen] = '"';
    pat[2 + klen] = '\0';
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + 2 + klen;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        if (*q == ':')
            return q + 1;
        p += 1;
    }
    return NULL;
}

static const char *ui_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int ui_json_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p;
    size_t i = 0;
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    p = ui_find_key(json, key);
    if (!p)
        return 0;
    p = ui_skip_ws(p);
    if (*p != '"')
        return 0;
    p++;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')
                out[i++] = '\n';
            else if (*p == 't')
                out[i++] = '\t';
            else if (*p == 'r')
                out[i++] = '\r';
            else
                out[i++] = *p;
            p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 1 : 0;
}

static int ui_json_i64(const char *json, const char *key, int64_t *out) {
    const char *p;
    char *end = NULL;
    if (!out)
        return 0;
    p = ui_find_key(json, key);
    if (!p)
        return 0;
    p = ui_skip_ws(p);
    if (!isdigit((unsigned char)*p) && *p != '-' && *p != '+')
        return 0;
    *out = (int64_t)strtoll(p, &end, 10);
    return end && end != p;
}

static int ui_json_bool(const char *json, const char *key, int *out) {
    const char *p;
    if (!out)
        return 0;
    p = ui_find_key(json, key);
    if (!p)
        return 0;
    p = ui_skip_ws(p);
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int ui_email_prefix(const char *email, char *out, size_t cap) {
    const char *at;
    size_t n;
    if (!email || !out || cap == 0)
        return PROD_ERR;
    at = strchr(email, '@');
    if (!at || at == email)
        return PROD_ERR;
    n = (size_t)(at - email);
    if (n + 1 > cap)
        return PROD_ERR;
    memcpy(out, email, n);
    out[n] = '\0';
    return PROD_OK;
}

static void ui_err(char *err, size_t err_cap, const char *tok) {
    if (!err || err_cap == 0)
        return;
    if (!tok)
        tok = "invalid";
    snprintf(err, err_cap, "%s", tok);
}

static int ui_provider_name(const char *provider, char *name, size_t cap) {
    size_t n, j;
    if (!provider || !name || cap == 0)
        return PROD_ERR;
    while (*provider == ' ' || *provider == '\t')
        provider++;
    n = strlen(provider);
    while (n > 0 && (provider[n - 1] == ' ' || provider[n - 1] == '\t'))
        n--;
    if (n == 0 || n >= cap)
        return PROD_ERR;
    for (j = 0; j < n; j++) {
        unsigned char c = (unsigned char)provider[j];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        name[j] = (char)c;
    }
    name[n] = '\0';
    return PROD_OK;
}

int prod_oauth_parse_userinfo(const char *provider, const char *body, char *id, size_t id_cap,
                              char *username, size_t username_cap, char *email, size_t email_cap,
                              char *name, size_t name_cap, char *err, size_t err_cap) {
    char pname[64];
    char login[PROD_OAUTH_USERINFO_FIELD_CAP];
    char sub[PROD_OAUTH_USERINFO_FIELD_CAP];
    char pref[PROD_OAUTH_USERINFO_FIELD_CAP];
    char em[PROD_OAUTH_USERINFO_FIELD_CAP];
    char nm[PROD_OAUTH_USERINFO_FIELD_CAP];
    char user[PROD_OAUTH_USERINFO_FIELD_CAP];
    char idbuf[PROD_OAUTH_USERINFO_FIELD_CAP];
    int64_t gh_id = 0;
    int email_verified = 0;

    if (id && id_cap)
        id[0] = '\0';
    if (username && username_cap)
        username[0] = '\0';
    if (email && email_cap)
        email[0] = '\0';
    if (name && name_cap)
        name[0] = '\0';
    if (!body)
        body = "";
    if (ui_provider_name(provider, pname, sizeof(pname)) != PROD_OK ||
        !prod_oauth_provider_ok(pname)) {
        ui_err(err, err_cap, "unknown_provider");
        return PROD_ERR;
    }
    em[0] = nm[0] = login[0] = sub[0] = pref[0] = user[0] = idbuf[0] = '\0';
    (void)ui_json_str(body, "email", em, sizeof(em));
    (void)ui_json_str(body, "name", nm, sizeof(nm));

    if (strcmp(pname, "github") == 0) {
        if (!ui_json_i64(body, "id", &gh_id) || !ui_json_str(body, "login", login, sizeof(login)) ||
            !login[0]) {
            ui_err(err, err_cap, "github_fields");
            return PROD_ERR;
        }
        if (snprintf(idbuf, sizeof(idbuf), "%lld", (long long)gh_id) >= (int)sizeof(idbuf)) {
            ui_err(err, err_cap, "github_id");
            return PROD_ERR;
        }
        memcpy(user, login, strlen(login) + 1);
    } else if (strcmp(pname, "google") == 0) {
        if (!ui_json_str(body, "sub", sub, sizeof(sub)) || !sub[0]) {
            ui_err(err, err_cap, "google_sub");
            return PROD_ERR;
        }
        if (ui_email_prefix(em, user, sizeof(user)) != PROD_OK) {
            ui_err(err, err_cap, "google_email");
            return PROD_ERR;
        }
        memcpy(idbuf, sub, strlen(sub) + 1);
    } else if (strcmp(pname, "authentik") == 0 || strcmp(pname, "oidc") == 0) {
        if (!ui_json_str(body, "sub", sub, sizeof(sub)) || !sub[0]) {
            ui_err(err, err_cap, "oidc_sub");
            return PROD_ERR;
        }
        (void)ui_json_str(body, "preferred_username", pref, sizeof(pref));
        (void)ui_json_bool(body, "email_verified", &email_verified);
        if (pref[0]) {
            memcpy(user, pref, strlen(pref) + 1);
        } else if (email_verified) {
            if (ui_email_prefix(em, user, sizeof(user)) != PROD_OK) {
                ui_err(err, err_cap, "oidc_email");
                return PROD_ERR;
            }
        } else {
            ui_err(err, err_cap, "oidc_username");
            return PROD_ERR;
        }
        memcpy(idbuf, sub, strlen(sub) + 1);
    } else {
        ui_err(err, err_cap, "unknown_provider");
        return PROD_ERR;
    }

    if (!prod_oauth_identity_field_ok(idbuf, 1, 255)) {
        ui_err(err, err_cap, "invalid_subject");
        return PROD_ERR;
    }
    if (!prod_oauth_username_ok(user)) {
        ui_err(err, err_cap, "invalid_username");
        return PROD_ERR;
    }
    if (em[0] && !prod_oauth_identity_field_ok(em, 3, 255)) {
        ui_err(err, err_cap, "invalid_email");
        return PROD_ERR;
    }
    if (!prod_oauth_identity_field_ok(nm, 0, 255)) {
        ui_err(err, err_cap, "invalid_name");
        return PROD_ERR;
    }
    if (copy_out(idbuf, id, id_cap) != PROD_OK || copy_out(user, username, username_cap) != PROD_OK ||
        copy_out(em, email, email_cap) != PROD_OK || copy_out(nm, name, name_cap) != PROD_OK) {
        ui_err(err, err_cap, "buffer");
        return PROD_ERR;
    }
    if (err && err_cap)
        err[0] = '\0';
    return PROD_OK;
}
