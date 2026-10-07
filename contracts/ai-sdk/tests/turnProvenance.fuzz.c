#include "turn_provenance.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const char request[] = "req-browser-dnd";
    uint32_t result;
    if (size > DTL_PROVENANCE_INPUT_CAPACITY) return 0;
    memcpy(dtl_provenance_input(), data, size);
    memcpy(dtl_provenance_request(), request, sizeof(request));
    result = dtl_provenance_project((uint32_t)size, sizeof(request) - 1u);
    for (uint32_t kind = 0u; kind < 3u; kind++) {
        const uint32_t count = dtl_provenance_count(kind);
        const uint32_t strings = kind == DTL_PROVENANCE_CITATION ? 12u : 3u;
        assert(count <= (kind == DTL_PROVENANCE_CITATION ? 4u : 1u));
        if (!result) assert(count == 0u);
        for (uint32_t index = 0u; index < count; index++) {
            for (uint32_t field = 0u; field < strings; field++) {
                const char *value = dtl_provenance_string(kind, index, field);
                assert(value != NULL);
                assert(strlen(value) < DTL_PROVENANCE_INPUT_CAPACITY);
            }
            for (uint32_t field = 0u; field < 4u; field++)
                assert(isfinite(dtl_provenance_number(kind, index, field)));
            if (kind == DTL_PROVENANCE_CITATION) {
                double witness_count = dtl_provenance_number(kind, index, 21u);
                assert(witness_count >= 0.0 && witness_count <= 16.0);
                for (uint32_t witness = 0u; witness < 16u; ++witness) {
                    uint32_t flat = index * 16u + witness;
                    for (uint32_t field = 0u; field < 2u; ++field) {
                        const char *value = dtl_provenance_string(DTL_PROVENANCE_WITNESS, flat, field);
                        if (witness < (uint32_t)witness_count) assert(value && strlen(value) == 64u);
                        else assert(value == NULL);
                    }
                    assert(dtl_provenance_string(DTL_PROVENANCE_WITNESS, flat, 2u) == NULL);
                    for (uint32_t field = 0u; field < 5u; ++field)
                        assert(isfinite(dtl_provenance_number(DTL_PROVENANCE_WITNESS, flat, field)));
                }
            }
        }
        assert(dtl_provenance_string(kind, count, 0u) == NULL);
        assert(dtl_provenance_string(kind, 0u, strings) == NULL);
    }
    /* Rejection must revoke prior views, even without a caller clear. */
    assert(dtl_provenance_project(0u, sizeof(request) - 1u) == 0u);
    for (uint32_t kind = 0u; kind < 3u; kind++) assert(dtl_provenance_count(kind) == 0u);
    for (uint32_t index = 0u; index <= 64u; ++index) {
        assert(dtl_provenance_string(DTL_PROVENANCE_WITNESS, index, 0u) == NULL);
        assert(dtl_provenance_number(DTL_PROVENANCE_WITNESS, index, 0u) == 0.0);
    }
    dtl_provenance_clear();
    for (size_t i = 0u; i < size; i++) assert(dtl_provenance_input()[i] == 0u);
    return 0;
}
