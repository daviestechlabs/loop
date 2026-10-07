#ifndef VOICE_C_DND_SOURCE_SELECT_INTERNAL_H
#define VOICE_C_DND_SOURCE_SELECT_INTERNAL_H
#include "dnd_source_index.h"
/* Shared query admission and name policy for immutable admitted source views. */
int dnd_source_query_prepare(const dnd_source_index *index, const dnd_rag_scope *scope,
    const char *corpus, const char *query, size_t query_len,
    const dnd_source_anchor *anchors, size_t count,
    char normalized[DND_SOURCE_QUERY_CAP], uint8_t allowed[DND_SOURCE_DOCUMENTS_MAX]);
int dnd_source_spell_name_matches(const char *normalized, const char *name);
#endif
