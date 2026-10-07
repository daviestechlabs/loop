#include "dnd_source_encode.h"
#include "dnd_source_artifact_fixture.h"
#include <stdlib.h>

static source_fixture f;
static artifact_fixture a;
int main(void) {
    fixture_make(&f); artifact_make(&a, &f);
    unsigned char *wire = NULL;
    size_t length = 0;
    assert(!dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length));
    /* The older test encoder predates this producer. Check every wire byte. */
    assert(length == a.length && !memcmp(wire, a.bytes, length));
    unsigned char *preserved = wire;
    size_t preserved_length = length;
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length) == -1);
    assert(wire == preserved && length == preserved_length);
    free(wire); wire = NULL; length = 0;
    assert(dnd_source_encode(NULL, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length) == -1);
    assert(dnd_source_encode(&f.data, NULL, a.expected.compiler_sha256, &wire, &length) == -1);
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, NULL, &wire, &length) == -1);
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, NULL, &length) == -1);
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, NULL) == -1);
    a.expected.compiler_sha256[0] = 'X';
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length) == -1);
    artifact_make(&a, &f);
    f.entries[0].heading.spans[0].record = UINT32_MAX;
    assert(dnd_source_encode(&f.data, a.expected.manifest_sha256, a.expected.compiler_sha256, &wire, &length) == -1);
    assert(!wire && !length);
    puts("dnd_source_encode: independent wire bytes and rejected inputs passed");
    return 0;
}
