#define _POSIX_C_SOURCE 200809L
#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

static void test_family(void) {
    char fam[64];
    char tiny[2];
    expect(prompt_family_from_id("rag.context_fallback_assistant", fam, sizeof(fam)) == PROMPT_OK,
           "family ok");
    expect(strcmp(fam, "rag") == 0, "rag");
    expect(prompt_family_from_id("dnd-foo", fam, sizeof(fam)) == PROMPT_OK, "dnd prefix");
    expect(strcmp(fam, "dnd") == 0, "dnd");
    expect(prompt_family_from_id("dnd-foo", tiny, sizeof(tiny)) == PROMPT_ERR_CAPACITY,
           "dnd capacity");
}

static void test_render(void) {
    char out[256];
    char small[4];
    const char *kv[] = {"name", "Ava", "detail", "Ready", NULL};
    expect(prompt_render_template("Hello {{name}}\n{{detail}}", kv, out, sizeof(out)) == PROMPT_OK,
           "render");
    expect(strcmp(out, "Hello Ava\nReady") == 0, "rendered text");
    expect(prompt_render_template("Hello", NULL, small, sizeof(small)) == PROMPT_ERR_CAPACITY,
           "render capacity");
}

static void test_load_default(void) {
    prompt_entry e;
    int rc = prompt_load(NULL, 0, "rag.context_fallback_assistant", &e);
    expect(rc == PROMPT_OK, "load embedded path");
    if (rc == PROMPT_OK) {
        expect(strcmp(e.id, "rag.context_fallback_assistant") == 0, "id");
        expect(strcmp(e.origin, "embedded") == 0, "origin");
        expect(e.sha256[0] != '\0', "sha");
        expect(strstr(e.text, "provided context") != NULL, "text content");
    }
}

static void test_companion_persona(void) {
    char out[512];
    char oversized[1100];
    const char *tmpl =
        "Adopt the '{{preset}}' companion persona.\n"
        "Custom traits: {{custom_traits}}.\n"
        "Speaking style: {{speaking_style}}.\n"
        "Optional recurring phrases: {{catchphrases}}.\n";
    expect(prompt_render_companion_persona(tmpl, "friendly", "curious and upbeat",
                                           "warm and concise", "Let us roll; Onward", out,
                                           sizeof(out)) == PROMPT_OK,
           "persona full");
    expect(strcmp(out, "Adopt the 'friendly' companion persona. Custom traits: curious and upbeat. "
                        "Speaking style: warm and concise. Optional recurring phrases: Let us roll; "
                        "Onward.") == 0,
           "persona full text");
    expect(prompt_render_companion_persona(tmpl, "friendly", "", "", "", out, sizeof(out)) ==
               PROMPT_OK,
           "persona minimal");
    expect(strcmp(out, "Adopt the 'friendly' companion persona.") == 0, "persona minimal text");
    expect(prompt_build_companion_system(NULL, 0, "friendly", "", "", "", out, sizeof(out)) ==
               PROMPT_OK,
           "build system");
    expect(strcmp(out, "Adopt the 'friendly' companion persona.") == 0, "build text");
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    expect(prompt_render_companion_persona(oversized, "", "", "", "", out,
                                           sizeof(out)) == PROMPT_ERR_CAPACITY,
           "persona line capacity");
}

static void test_orpheus_vocal_policy(void) {
    static const char *const prompt_ids[] = {
        "voice.live_spoken_assistant.default",
        "voice.orchestrated_turn.spoken_assistant",
        "dnd-dm-system",
    };
    static const char allowlist[] =
        "live-admitted allowlist only: "
        "<laugh> <chuckle> <cough> <sniffle> <groan> <yawn> <gasp>";
    size_t i;

    for (i = 0; i < sizeof(prompt_ids) / sizeof(prompt_ids[0]); i++) {
        prompt_entry entry;
        int rc = prompt_load(NULL, 0, prompt_ids[i], &entry);
        expect(rc == PROMPT_OK, "load Orpheus vocal-policy prompt");
        if (rc != PROMPT_OK) continue;
        expect(strstr(entry.text, allowlist) != NULL,
               "Orpheus prompt locks live-admitted allowlist only: <laugh> <chuckle> <cough> <sniffle> <groan> <yawn> <gasp>");
        expect(strstr(entry.text, "Do not use <sigh>") != NULL,
               "Orpheus prompt locks Do not use <sigh>");
    }
}

static void test_json_capacity(void) {
    prompt_entry e = {0};
    char out[8];
    expect(prompt_entry_encode_json(&e, out, sizeof(out)) == PROMPT_ERR_CAPACITY,
           "json capacity");
}

static void test_manifest_field_capacity(void) {
    char root[] = "/tmp/c-prompt-capacity-XXXXXX";
    char prompts[256];
    char family[256];
    char manifest[256];
    char prompt_path[256];
    prompt_entry entry;
    FILE *file;
    int i;

    if (!mkdtemp(root)) {
        expect(0, "manifest capacity temp directory");
        return;
    }
    snprintf(prompts, sizeof(prompts), "%s/prompts", root);
    snprintf(family, sizeof(family), "%s/rag", prompts);
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", family);
    snprintf(prompt_path, sizeof(prompt_path), "%s/oversized.txt", family);
    if (mkdir(prompts, 0700) != 0 || mkdir(family, 0700) != 0 ||
        !(file = fopen(manifest, "wb"))) {
        expect(0, "manifest capacity fixture setup");
        goto cleanup;
    }
    fputs("{\"prompts\":[{\"id\":\"rag.oversized\",\"version\":\"", file);
    for (i = 0; i < 80; i++) fputc('v', file);
    fputs("\",\"type\":\"system\",\"path\":\"prompts/rag/oversized.txt\"}]}", file);
    fclose(file);
    file = fopen(prompt_path, "wb");
    if (!file) {
        expect(0, "manifest capacity prompt setup");
        goto cleanup;
    }
    fputs("text", file);
    fclose(file);
    expect(prompt_load(root, 1, "rag.oversized", &entry) == PROMPT_ERR_NOT_FOUND,
           "manifest field capacity");

    file = fopen(manifest, "wb");
    if (!file) { expect(0, "binary asset manifest setup"); goto cleanup; }
    fputs("{\"prompts\":[{\"id\":\"rag.oversized\",\"version\":\"v1\","
          "\"type\":\"system\",\"path\":\"prompts/rag/oversized.txt\"}]}", file);
    fclose(file);
    file = fopen(prompt_path, "wb");
    if (!file) { expect(0, "binary prompt setup"); goto cleanup; }
    expect(fwrite("visible\0hidden", 1u, 14u, file) == 14u, "write embedded NUL prompt");
    fclose(file);
    expect(prompt_load(root, 1, "rag.oversized", &entry) != PROMPT_OK && !entry.text[0],
           "embedded NUL prompt cannot be shortened into a valid prompt");
    file = fopen(prompt_path, "wb");
    if (!file) { expect(0, "restore text prompt"); goto cleanup; }
    fputs("valid text", file);
    fclose(file);
    expect(prompt_load(root, 1, "rag.oversized", &entry) == PROMPT_OK,
           "valid text loads before binary manifest check");
    file = fopen(manifest, "ab");
    if (!file) { expect(0, "binary manifest setup"); goto cleanup; }
    fputc('\0', file);
    fclose(file);
    expect(prompt_load(root, 1, "rag.oversized", &entry) != PROMPT_OK && !entry.text[0],
           "embedded NUL manifest cannot hide trailing bytes");

cleanup:
    unlink(prompt_path);
    unlink(manifest);
    rmdir(family);
    rmdir(prompts);
    rmdir(root);
}

int main(void) {
    test_family();
    test_render();
    test_load_default();
    test_companion_persona();
    test_orpheus_vocal_policy();
    test_json_capacity();
    test_manifest_field_capacity();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-prompt unit\n");
    return 0;
}
