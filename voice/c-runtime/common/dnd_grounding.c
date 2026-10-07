#include "dnd_grounding.h"
#include "prompt.h"
#include "pb_min.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

int dnd_grounding_load(const char *root, dnd_grounding_prompt *out) {
    prompt_entry *entry;
    size_t length, version_len;
    int rc = -1;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    entry = malloc(sizeof(*entry));
    if (!entry) return -1;
    if (prompt_load(root, root && root[0], DND_GROUNDING_PROMPT_ID, entry) != PROMPT_OK ||
        strcmp(entry->id, DND_GROUNDING_PROMPT_ID) != 0 || strcmp(entry->type, "system") != 0)
        goto done;
    length = strlen(entry->text);
    version_len = strlen(entry->version);
    if (!length || length >= sizeof(out->text) || !version_len ||
        version_len >= sizeof(out->identity.version) ||
        !utf8_validate_v1((const uint8_t *)entry->text, length)) goto done;
    memcpy(out->identity.id, entry->id, strlen(entry->id) + 1u);
    memcpy(out->identity.version, entry->version, version_len + 1u);
    memcpy(out->identity.sha256, entry->sha256, sizeof(out->identity.sha256));
    if (!pb_grounding_identity_valid(&out->identity)) goto done;
    memcpy(out->text, entry->text, length + 1u);
    out->text_len = length;
    rc = 0;
done:
    if (rc != 0) memset(out, 0, sizeof(*out));
    free(entry);
    return rc;
}
