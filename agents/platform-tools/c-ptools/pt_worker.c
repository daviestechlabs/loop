#include "pt_worker.h"

#include "../c-dice/dice.h"
#include "../c-toolstore/toolstore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void pt_worker_init(pt_worker *w, const char *ws_root, const char *artifact_dir, const char *dnd_dir) {
    memset(w, 0, sizeof(*w));
    snprintf(w->workspace_root, sizeof(w->workspace_root), "%s",
             ws_root && ws_root[0] ? ws_root : "/tmp/platform-tools-ws");
    snprintf(w->artifact_dir, sizeof(w->artifact_dir), "%s",
             artifact_dir && artifact_dir[0] ? artifact_dir : "/tmp/platform-tools-artifacts");
    snprintf(w->dnd_state_dir, sizeof(w->dnd_state_dir), "%s",
             dnd_dir && dnd_dir[0] ? dnd_dir : "/tmp/platform-tools-dnd");
    mkdir(w->workspace_root, 0755);
    mkdir(w->artifact_dir, 0755);
    mkdir(w->dnd_state_dir, 0755);
}

static int jget_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[80];
    const char *p, *q1, *q2;
    size_t n;
    out[0] = '\0';
    if (!json || !key) return -1;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    q1 = p + 1;
    for (q2 = q1; *q2 && *q2 != '"'; q2++)
        if (*q2 == '\\' && q2[1]) q2++;
    if (*q2 != '"') return -1;
    n = (size_t)(q2 - q1);
    if (n >= cap) n = cap - 1;
    memcpy(out, q1, n);
    out[n] = '\0';
    return 0;
}

static int run_dice(pt_call *call) {
    char expr[64], err[160];
    dice_spec spec;
    int rolls[DICE_MAX_COUNT], kept[DICE_MAX_COUNT], n_kept = 0, total = 0, i;
    if (jget_str(call->input_json, "expression", expr, sizeof(expr)) != 0 &&
        jget_str(call->input_json, "dice", expr, sizeof(expr)) != 0) {
        if (call->input_json[0] == '"') {
            size_t n = strlen(call->input_json);
            if (n >= 2 && n - 2 < sizeof(expr)) {
                memcpy(expr, call->input_json + 1, n - 2);
                expr[n - 2] = '\0';
            } else {
                snprintf(call->error, sizeof(call->error), "missing expression");
                return -1;
            }
        } else {
            snprintf(call->error, sizeof(call->error), "missing expression");
            return -1;
        }
    }
    if (dice_parse(expr, &spec, err, sizeof(err)) != DICE_OK) {
        snprintf(call->error, sizeof(call->error), "%s", err);
        return -1;
    }
    if (dice_secure_roll(&spec, rolls, DICE_MAX_COUNT) != DICE_OK) {
        snprintf(call->error, sizeof(call->error), "roll failed");
        return -1;
    }
    if (dice_select_indexes(rolls, spec.count, spec.keep, kept, &n_kept) != DICE_OK) {
        snprintf(call->error, sizeof(call->error), "select failed");
        return -1;
    }
    for (i = 0; i < n_kept; i++) total += rolls[kept[i]];
    total += spec.modifier;
    if (dice_encode_roll_json(&spec, rolls, spec.count, kept, n_kept, total, "getrandom", call->output_json,
                              sizeof(call->output_json)) != DICE_OK) {
        snprintf(call->error, sizeof(call->error), "encode failed");
        return -1;
    }
    snprintf(call->summary, sizeof(call->summary), "rolled %s = %d", spec.expression, total);
    {
        size_t used = strlen(call->output_json);
        int written = snprintf(call->output_json + used - 1u, sizeof(call->output_json) - used + 1u,
                                ",\"roll_id\":\"%s\",\"visibility\":\"public\"}", call->tool_call_id);
        if (written <= 0 || (size_t)written >= sizeof(call->output_json) - used + 1u) return -1;
    }
    return 0;
}

static void jesc_append(char *out, size_t cap, size_t *off, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n && *off + 2 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out[(*off)++] = '\\';
            out[(*off)++] = (char)c;
        } else if (c >= 32 && c < 127) {
            out[(*off)++] = (char)c;
        }
    }
}

static int run_workspace_read(pt_worker *w, pt_call *call) {
    char path[512], abs[1024];
    char out[PT_OUT];
    size_t off = 0;
    const char *paths_key;
    int nfiles = 0;

    paths_key = strstr(call->input_json, "\"paths\"");
    off = (size_t)snprintf(out, sizeof(out), "{\"workspace_id\":\"default\",\"files\":[");
    if (paths_key) {
        const char *p = strchr(paths_key, '[');
        if (p) {
            p++;
            while (*p && nfiles < 25) {
                const char *q1, *q2;
                size_t n;
                while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t')) p++;
                if (*p == ']') break;
                if (*p != '"') break;
                q1 = p + 1;
                q2 = strchr(q1, '"');
                if (!q2) break;
                n = (size_t)(q2 - q1);
                if (n >= sizeof(path)) n = sizeof(path) - 1;
                memcpy(path, q1, n);
                path[n] = '\0';
                p = q2 + 1;

                if (ts_bounded_path(w->workspace_root, path, abs, sizeof(abs)) != TS_OK) {
                    snprintf(call->error, sizeof(call->error), "path outside workspace: %s", path);
                    return -1;
                }
                {
                    FILE *f = fopen(abs, "rb");
                    char *data = NULL;
                    long sz = 0;
                    if (!f) {
                        snprintf(call->error, sizeof(call->error), "read %s: %s", path, strerror(errno));
                        return -1;
                    }
                    fseek(f, 0, SEEK_END);
                    sz = ftell(f);
                    rewind(f);
                    if (sz < 0) sz = 0;
                    if (sz > 65536) sz = 65536;
                    data = (char *)malloc((size_t)sz + 1);
                    if (!data) {
                        fclose(f);
                        return -1;
                    }
                    if (sz > 0) {
                        if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
                            free(data);
                            fclose(f);
                            snprintf(call->error, sizeof(call->error), "short read %s", path);
                            return -1;
                        }
                    }
                    data[sz] = '\0';
                    fclose(f);
                    if (nfiles) {
                        if (off + 1 < sizeof(out)) out[off++] = ',';
                    }
                    {
                        int wn = snprintf(out + off, sizeof(out) - off,
                                          "{\"path\":\"%s\",\"size_bytes\":%ld,\"content\":\"", path, sz);
                        if (wn > 0) off += (size_t)wn;
                    }
                    jesc_append(out, sizeof(out), &off, data, (size_t)sz);
                    if (off + 2 < sizeof(out)) {
                        out[off++] = '"';
                        out[off++] = '}';
                    }
                    free(data);
                    nfiles++;
                }
            }
        }
    }
    if (off + 3 < sizeof(out)) {
        out[off++] = ']';
        out[off++] = '}';
        out[off] = '\0';
    }
    snprintf(call->output_json, sizeof(call->output_json), "%s", out);
    snprintf(call->summary, sizeof(call->summary), "read %d file(s)", nfiles);
    return 0;
}

static int run_workspace_edit(pt_call *call) {
    char approval[128];
    if (jget_str(call->input_json, "approval_id", approval, sizeof(approval)) != 0 || !approval[0]) {
        snprintf(call->error, sizeof(call->error), "approval_id is required for workspace-edit");
        return -1;
    }
    snprintf(call->output_json, sizeof(call->output_json),
             "{\"approval_id\":\"%s\",\"dry_run\":true,\"changed_files\":[]}", approval);
    snprintf(call->summary, sizeof(call->summary), "workspace-edit accepted (dry_run)");
    return 0;
}

int pt_worker_dispatch(pt_worker *w, pt_call *call) {
    if (!w || !call) return -1;
    call->error[0] = '\0';
    call->output_json[0] = '\0';
    call->output_sha256[0] = '\0';
    call->output_artifact[0] = '\0';
    call->summary[0] = '\0';
    if (strcmp(call->tool_id, "dnd-dice-roll") == 0) return run_dice(call);
    if (strcmp(call->tool_id, "workspace-read") == 0) return run_workspace_read(w, call);
    if (strcmp(call->tool_id, "workspace-edit") == 0) return run_workspace_edit(call);
    /* Both D&D state transactions belong to the manager, which owns the store. */
    snprintf(call->error, sizeof(call->error), "unknown local tool");
    return -1;
}
