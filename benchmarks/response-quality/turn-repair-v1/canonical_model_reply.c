/* Offline replay of the production presentation kernels; no model call. */
#include "speech_markup.h"
#include "speech_sanitize.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    char input[SPEECH_MARKUP_INPUT_CAP], rendered[SPEECH_MARKUP_INPUT_CAP], output[SPEECH_MARKUP_INPUT_CAP];
    size_t length = fread(input, 1, sizeof(input), stdin), rendered_length = 0, output_length = 0;
    speech_markup_v1 state;
    if (ferror(stdin) || !length || length >= sizeof(input) || memchr(input, 0, length) ||
        !utf8_validate_v1((const unsigned char *)input, length)) return 1;
    speech_markup_init_v1(&state);
    /* Completed markup output is invariant to the original delta partition. */
    if (speech_markup_feed_v1(&state, input, length, 1, rendered, sizeof(rendered), &rendered_length) ||
        speech_sanitize_v1(rendered, rendered_length, 0, output, sizeof(output), &output_length) ||
        !output_length || output_length >= sizeof(output)) return 1;
    return fwrite(output, 1, output_length, stdout) == output_length && !fflush(stdout) ? 0 : 1;
}
