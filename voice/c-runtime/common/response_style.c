#include "response_style.h"
#include "prompt.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

int voice_response_prompts_load(const char *root, const dnd_grounding_prompt *grounding,
                               voice_response_prompts *out) {
    prompt_entry *entry;
    size_t length, version_len;
    int rc = -1;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!grounding || !grounding->text_len || grounding->text_len >= sizeof(grounding->text) ||
        grounding->text[grounding->text_len] != '\0' ||
        memchr(grounding->text, '\0', grounding->text_len) ||
        !utf8_validate_v1((const uint8_t *)grounding->text, grounding->text_len)) return -1;
    entry = malloc(sizeof(*entry));
    if (!entry) return -1;
    if (prompt_load(root, root && root[0], VOICE_RESPONSE_STYLE_ID, entry) != PROMPT_OK ||
        strcmp(entry->id, VOICE_RESPONSE_STYLE_ID) != 0 || strcmp(entry->type, "system") != 0)
        goto done;
    length = strlen(entry->text);
    version_len = strlen(entry->version);
    if (!length || length >= sizeof(out->direct) || !version_len ||
        version_len >= sizeof(out->version) ||
        length + 2u + grounding->text_len >= sizeof(out->grounded) ||
        !utf8_validate_v1((const uint8_t *)entry->text, length)) goto done;
    memcpy(out->direct, entry->text, length + 1u);
    out->direct_len = length;
    memcpy(out->grounded, entry->text, length);
    memcpy(out->grounded + length, "\n\n", 2u);
    memcpy(out->grounded + length + 2u, grounding->text, grounding->text_len + 1u);
    out->grounded_len = length + 2u + grounding->text_len;
    memcpy(out->version, entry->version, version_len + 1u);
    memcpy(out->sha256, entry->sha256, sizeof(out->sha256));
    if (prompt_load(root, root && root[0], VOICE_DND_DIALOGUE_ID, entry) != PROMPT_OK ||
        strcmp(entry->id, VOICE_DND_DIALOGUE_ID) || strcmp(entry->type, "system")) goto done;
    length = strlen(entry->text);
    version_len = strlen(entry->version);
    if (!length || !version_len || version_len >= sizeof(out->dnd_version) ||
        out->direct_len + 2u + length + 2u + grounding->text_len >= sizeof(out->dnd_grounded) ||
        !utf8_validate_v1((const uint8_t *)entry->text, length)) goto done;
    memcpy(out->dnd_direct, out->direct, out->direct_len);
    memcpy(out->dnd_direct + out->direct_len, "\n\n", 2u);
    memcpy(out->dnd_direct + out->direct_len + 2u, entry->text, length + 1u);
    out->dnd_direct_len = out->direct_len + 2u + length;
    memcpy(out->dnd_grounded, out->dnd_direct, out->dnd_direct_len);
    memcpy(out->dnd_grounded + out->dnd_direct_len, "\n\n", 2u);
    memcpy(out->dnd_grounded + out->dnd_direct_len + 2u, grounding->text, grounding->text_len + 1u);
    out->dnd_grounded_len = out->dnd_direct_len + 2u + grounding->text_len;
    memcpy(out->dnd_version, entry->version, version_len + 1u);
    memcpy(out->dnd_sha256, entry->sha256, sizeof(out->dnd_sha256));
    if (prompt_load(root, root && root[0], VOICE_LOOP_DIALOGUE_ID, entry) != PROMPT_OK ||
        strcmp(entry->id, VOICE_LOOP_DIALOGUE_ID) || strcmp(entry->type, "system")) goto done;
    length = strlen(entry->text);
    version_len = strlen(entry->version);
    if (!length || !version_len || version_len >= sizeof(out->loop_version) ||
        out->direct_len + 2u + length + 2u + grounding->text_len >= sizeof(out->loop_grounded) ||
        !utf8_validate_v1((const uint8_t *)entry->text, length)) goto done;
    memcpy(out->loop_direct, out->direct, out->direct_len);
    memcpy(out->loop_direct + out->direct_len, "\n\n", 2u);
    memcpy(out->loop_direct + out->direct_len + 2u, entry->text, length + 1u);
    out->loop_direct_len = out->direct_len + 2u + length;
    memcpy(out->loop_grounded, out->loop_direct, out->loop_direct_len);
    memcpy(out->loop_grounded + out->loop_direct_len, "\n\n", 2u);
    memcpy(out->loop_grounded + out->loop_direct_len + 2u, grounding->text, grounding->text_len + 1u);
    out->loop_grounded_len = out->loop_direct_len + 2u + grounding->text_len;
    memcpy(out->loop_version, entry->version, version_len + 1u);
    memcpy(out->loop_sha256, entry->sha256, sizeof(out->loop_sha256));
    rc = 0;
done:
    if (rc != 0) memset(out, 0, sizeof(*out));
    free(entry);
    return rc;
}
