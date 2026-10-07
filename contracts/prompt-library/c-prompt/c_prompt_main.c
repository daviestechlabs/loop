/*
 * c-prompt — pure-C prompt-library CLI.
 *   c-prompt family <id>
 *   c-prompt load <id>            # JSON entry; env PROMPT_LIBRARY_ROOT
 *   c-prompt load-text <id>       # raw text only
 *   c-prompt render               # stdin: {"template":"...","vars":{"k":"v"}}
 */
#define _POSIX_C_SOURCE 200809L
#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(void) {
    size_t cap = 8192, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        size_t r;
        if (n + 4096 > cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) return NULL;
        }
        r = fread(buf + n, 1, cap - n - 1, stdin);
        n += r;
        if (r == 0) break;
    }
    buf[n] = '\0';
    return buf;
}

static int json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[128];
    const char *p;
    size_t o = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        if (out && cap) out[0] = '\0';
        return -1;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n') c = '\n';
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

int main(int argc, char **argv) {
    const char *root;
    int root_is_env = 0;
    prompt_entry entry;
    char out[PROMPT_TEXT * 2];
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: c-prompt family|load|load-text|render ...\n");
        return 2;
    }
    root = getenv("PROMPT_LIBRARY_ROOT");
    if (root && root[0]) root_is_env = 1;
    else root = NULL;

    if (strcmp(argv[1], "family") == 0) {
        char fam[PROMPT_STR];
        if (argc < 3) return 2;
        if (prompt_family_from_id(argv[2], fam, sizeof(fam)) != PROMPT_OK) return 1;
        puts(fam);
        return 0;
    }
    if (strcmp(argv[1], "load") == 0 || strcmp(argv[1], "load-text") == 0) {
        if (argc < 3) return 2;
        rc = prompt_load(root, root_is_env, argv[2], &entry);
        if (rc == PROMPT_ERR_NOT_FOUND) {
            fprintf(stderr, "prompt id \"%s\" not found\n", argv[2]);
            return 1;
        }
        if (rc != PROMPT_OK) return 1;
        if (strcmp(argv[1], "load-text") == 0) {
            fputs(entry.text, stdout);
            return 0;
        }
        if (prompt_entry_encode_json(&entry, out, sizeof(out)) != PROMPT_OK) return 1;
        puts(out);
        return 0;
    }
    if (strcmp(argv[1], "companion-system") == 0) {
        /* companion-system <preset> [custom_traits] [speaking_style] [catchphrases] */
        const char *preset = (argc >= 3) ? argv[2] : "";
        const char *traits = (argc >= 4) ? argv[3] : "";
        const char *style = (argc >= 5) ? argv[4] : "";
        const char *phrases = (argc >= 6) ? argv[5] : "";
        rc = prompt_build_companion_system(root, root_is_env, preset, traits, style, phrases, out,
                                           sizeof(out));
        if (rc != PROMPT_OK)
            return 1;
        fputs(out, stdout);
        return 0;
    }
    if (strcmp(argv[1], "render") == 0) {
        char *body = read_all();
        char template[PROMPT_TEXT];
        char rendered[PROMPT_TEXT];
        const char *pairs[64];
        int np = 0;
        const char *vars;
        if (!body) return 1;
        template[0] = '\0';
        (void)json_str(body, "template", template, sizeof(template));
        /* extract simple vars object string values */
        vars = strstr(body, "\"vars\"");
        if (vars) {
            const char *p = strchr(vars, '{');
            if (p) {
                p++;
                while (*p && *p != '}' && np + 2 < 64) {
                    char key[PROMPT_STR], val[PROMPT_STR];
                    while (*p && *p != '"') p++;
                    if (*p != '"') break;
                    p++;
                    {
                        size_t o = 0;
                        while (*p && *p != '"' && o + 1 < sizeof(key)) key[o++] = *p++;
                        key[o] = '\0';
                    }
                    if (*p == '"') p++;
                    while (*p && *p != ':') p++;
                    if (*p == ':') p++;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p != '"') break;
                    p++;
                    {
                        size_t o = 0;
                        while (*p && *p != '"' && o + 1 < sizeof(val)) {
                            char c = *p++;
                            if (c == '\\' && *p) c = *p++;
                            val[o++] = c;
                        }
                        val[o] = '\0';
                    }
                    if (*p == '"') p++;
                    pairs[np++] = strdup(key);
                    pairs[np++] = strdup(val);
                    while (*p && *p != '"' && *p != '}') p++;
                }
            }
        }
        pairs[np] = NULL;
        free(body);
        if (prompt_render_template(template, pairs, rendered, sizeof(rendered)) != PROMPT_OK) return 1;
        {
            int i;
            for (i = 0; i < np; i++) free((void *)pairs[i]);
        }
        fputs(rendered, stdout);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
