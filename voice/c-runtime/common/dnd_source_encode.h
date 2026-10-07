#ifndef VOICE_C_DND_SOURCE_ENCODE_H
#define VOICE_C_DND_SOURCE_ENCODE_H

#include "dnd_source_artifact.h"

/* Offline serialization after source-index admission. No native pointers or
 * padding enter the portable DNDSIDX1 bytes. Caller-owned input stays immutable
 * for this call. *out and *length must initially be zero; failure preserves
 * them. On success the caller frees *out. Pins identify an externally reviewed
 * manifest and compiler bundle; this encoder does not certify their contents. */
int dnd_source_encode(const dnd_source_data *data, const char manifest_sha256[65],
    const char compiler_sha256[65], unsigned char **out, size_t *length);

/* DNDSIDX2 adds nonempty, canonically ordered passage definitions. The encoder
 * rebuilds each passage from original records and stores its assembly hash.
 * The legacy encoder above continues to produce byte-identical DNDSIDX1. */
int dnd_source_encode_passages(const dnd_source_data *data,
    const dnd_source_passage_definition *definitions, size_t count,
    const char manifest_sha256[65], const char compiler_sha256[65],
    unsigned char **out, size_t *length);

#endif
