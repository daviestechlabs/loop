/* Offline probe. IO and formatting stay outside the pure C kernel. */
#include "common/dnd_claim_evidence.h"
#include <stdio.h>

static size_t decode_size(const unsigned char *p) {
    return ((size_t)p[0] << 24u) | ((size_t)p[1] << 16u) |
           ((size_t)p[2] << 8u) | (size_t)p[3];
}

int main(void) {
    unsigned char text[DND_CLAIM_TEXT_CAP + 1u];
    unsigned char context[DND_CLAIM_CONTEXT_CAP + 1u];
    unsigned char quote[DND_CLAIM_QUOTE_CAP + 1u];
    unsigned char sizes[12];
    for (;;) {
        size_t got = fread(sizes, 1u, sizeof(sizes), stdin);
        if (!got) return ferror(stdin) ? 2 : 0;
        if (got != sizeof(sizes)) return 2;
        size_t length = decode_size(sizes);
        size_t context_length = decode_size(sizes + 4u);
        size_t quote_length = decode_size(sizes + 8u);
        if (length > sizeof(text) || context_length > sizeof(context) || quote_length > sizeof(quote) ||
            fread(text, 1u, length, stdin) != length ||
            fread(context, 1u, context_length, stdin) != context_length ||
            fread(quote, 1u, quote_length, stdin) != quote_length) return 2;
        dnd_claim_evidence_span out;
        memset(&out, 0xff, sizeof(out));
        dnd_claim_evidence_status status = dnd_claim_evidence_resolve(
            text, length, context, context_length, quote, quote_length, &out);
        printf("%d %zu %zu %zu %zu\n", (int)status, out.context_begin, out.context_end,
               out.quote_begin, out.quote_end);
    }
}
