/* prompt.h — pure-C prompt-library load + template render (no Go). */
#ifndef PROMPT_LIBRARY_C_H
#define PROMPT_LIBRARY_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PROMPT_OK = 0,
    PROMPT_ERR_ARGUMENT = 1,
    PROMPT_ERR_NOT_FOUND = 2,
    PROMPT_ERR_IO = 3,
    PROMPT_ERR_JSON = 4,
    PROMPT_ERR_CAPACITY = 5
};

#define PROMPT_STR 256
#define PROMPT_PATH 512
#define PROMPT_TEXT (256 * 1024)
#define PROMPT_SHA 65

typedef struct prompt_entry {
    char id[PROMPT_STR];
    char version[64];
    char type[64];
    char path[PROMPT_PATH];
    char text[PROMPT_TEXT];
    char sha256[PROMPT_SHA];
    char origin[32]; /* "external" | "embedded" */
} prompt_entry;

/* family from id: "rag.foo" → "rag"; "dnd-x" → "dnd". */
int prompt_family_from_id(const char *prompt_id, char *family, size_t cap);

/* Replace {{key}} occurrences; trims result. replacements as alternating key/value NULL-terminated pairs. */
int prompt_render_template(const char *template_text, const char *const *kv_pairs, char *out, size_t cap);

/*
 * Companion persona system-prompt render (matches residual Go BuildCompanionSystemPrompt):
 * for each non-empty trimmed line, skip if any optional placeholder has empty value
 * (custom_traits / speaking_style / catchphrases); always apply {{preset}};
 * join kept lines with a single space.
 * NULL field values treated as "".
 */
int prompt_render_companion_persona(const char *template_text, const char *preset,
                                    const char *custom_traits, const char *speaking_style,
                                    const char *catchphrases, char *out, size_t cap);

#define PROMPT_COMPANION_PERSONA_ID "companion.persona_profile.template"

/*
 * Load companion.persona_profile.template then render companion persona system prompt.
 * root/root_is_env same semantics as prompt_load (NULL → monorepo discover).
 */
int prompt_build_companion_system(const char *root, int root_is_env, const char *preset,
                                  const char *custom_traits, const char *speaking_style,
                                  const char *catchphrases, char *out, size_t cap);

/*
 * Load prompt text by id under root (directory containing "prompts/").
 * origin_out: "external" if root_is_env, else "embedded".
 * If root is NULL/empty, discover monorepo contracts/prompt-library.
 */
int prompt_load(const char *root, int root_is_env, const char *prompt_id, prompt_entry *out);

/* Encode entry as JSON for host binding. */
int prompt_entry_encode_json(const prompt_entry *e, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
