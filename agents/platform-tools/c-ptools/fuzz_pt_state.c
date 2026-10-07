/* Journal codec and both pure domain transitions; no filesystem operations. */
#include "pt_dnd_state.c"
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    state_transaction *tx = NULL, *decoded = NULL;
    pt_call *original = NULL;
    pt_dnd_identity identity;
    char *input = NULL, *journal = NULL;
    if (size >= DND_JOURNAL_CAP || memchr(data, 0, size)) return 0;
    input = malloc(size + 1u);
    journal = malloc(DND_JOURNAL_CAP);
    tx = calloc(1, sizeof(*tx));
    decoded = calloc(1, sizeof(*decoded));
    original = calloc(1, sizeof(*original));
    if (!input || !journal || !tx || !decoded || !original) goto done;
    memcpy(input, data, size);
    input[size] = '\0';
    for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); ++i) {
        const state_ops *ops = &adapters[i];
        if (!transaction_decode(ops, input, tx)) continue;
        *original = tx->completed;
        original->state = PT_ST_QUEUED;
        original->updated_at = original->created_at;
        memset(original->output_json, 0, sizeof(original->output_json));
        memset(original->output_sha256, 0, sizeof(original->output_sha256));
        memset(original->output_artifact, 0, sizeof(original->output_artifact));
        memset(original->summary, 0, sizeof(original->summary));
        memset(original->error, 0, sizeof(original->error));
        if (transaction_valid(ops, tx, original, &identity)) {
            size_t length = transaction_encode(ops, journal, DND_JOURNAL_CAP, tx);
            if (!length || !transaction_decode(ops, journal, decoded) ||
                memcmp(tx, decoded, sizeof(*tx)) ||
                transaction_encode(ops, journal, length, tx) || journal[0]) abort();
        }
    }
done:
    free(original);
    free(decoded);
    free(tx);
    free(journal);
    free(input);
    return 0;
}
