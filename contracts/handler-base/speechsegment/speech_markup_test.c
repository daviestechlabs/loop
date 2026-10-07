#include "speech_markup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct { const char *input, *expected; } fixture;
static const fixture cases[] = {
    {"Sneak Attack deals 1d6 damage.", "Sneak Attack deals 1d6 damage."},
    {"**Sneak Attack** deals 1d6 damage.", "Sneak Attack deals 1d6 damage."},
    {"Use *Steady Aim* on your turn.", "Use Steady Aim on your turn."},
    {"Use __Steady Aim__ on your turn.", "Use Steady Aim on your turn."},
    {"Use _Steady Aim_ on your turn.", "Use Steady Aim on your turn."},
    {"***Steady Aim*** requires no movement.", "Steady Aim requires no movement."},
    {"**A *nested* condition** remains.", "A nested condition remains."},
    {"**A __nested__ condition** remains.", "A nested condition remains."},
    {"`2 * 3` is six.", "2 * 3 is six."},
    {"``a `literal` marker`` remains.", "a `literal` marker remains."},
    {"**Keep `**literal**` markers**.", "Keep **literal** markers."},
    {"Roll 2 * 3 and use 2**3 literally.", "Roll 2 * 3 and use 2**3 literally."},
    {"record_id and some_word stay.", "record_id and some_word stay."},
    {"foo**bar**baz stays literal.", "foo**bar**baz stays literal."},
    {"**unfinished", "**unfinished"},
    {"Use * literal * marks.", "Use * literal * marks."},
    {"****literal****", "****literal****"},
    {"\\*literal\\* and \\_value\\_", "*literal* and _value_"},
    {"C:\\Users\\gale", "C:\\Users\\gale"},
    {"\\\\server\\share", "\\\\server\\share"},
    {"trailing\\", "trailing\\"},
    {"<la**ugh**> **word**", "<la**ugh**> word"},
    {"<la`ugh`> **word**", "<la`ugh`> word"},
    {"**<laugh> word**", "<laugh> word"},
    {"<unknown **tag**> **word**", "<unknown **tag**> word"},
    {"<unfinished **tag**", "<unfinished **tag**"},
    {"**Résumé** and *café*.", "Résumé and café."},
    {"**word**'s limit.", "word's limit."},
    {"# Heading\n* list item\n[label](url)", "# Heading\n* list item\n[label](url)"},
    {"", ""}
};

static void render(const char *input, size_t split, char output[2048]) {
    speech_markup_v1 state;
    speech_markup_init_v1(&state);
    size_t length = strlen(input), used = 0, written = 0;
    CHECK(split <= length);
    CHECK(!speech_markup_feed_v1(&state, input, split, 0, output, 2048, &written));
    used += written;
    CHECK(!speech_markup_feed_v1(&state, input + split, length - split, 1, output + used, 2048 - used, &written));
    used += written;
    output[used] = 0;
    CHECK(state.finished && !state.pending_len && state.source_bytes == length);
}

int main(void) {
    size_t checks = 0;
    char output[2048];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = strlen(cases[i].input);
        for (size_t split = 0; split <= length; ++split) {
            render(cases[i].input, split, output);
            if (strcmp(output, cases[i].expected))
                fprintf(stderr, "case=%zu split=%zu actual=[%s] expected=[%s]\n", i, split, output, cases[i].expected);
            CHECK(!strcmp(output, cases[i].expected));
            ++checks;
        }
    }
    speech_markup_v1 state, before;
    speech_markup_init_v1(&state);
    size_t written = 0;
    CHECK(!speech_markup_feed_v1(&state, "**word", 6, 0, output, sizeof(output), &written));
    CHECK(!written && state.pending_len == 6);
    before = state;
    CHECK(speech_markup_feed_v1(&state, "**", 2, 1, output, 1, &written) == SPEECH_MARKUP_ERR_CAPACITY);
    CHECK(!written && !memcmp(&state, &before, sizeof(state)));
    CHECK(!speech_markup_feed_v1(&state, "**", 2, 1, output, sizeof(output), &written));
    CHECK(written == 4 && !memcmp(output, "word", 4));
    CHECK(speech_markup_feed_v1(&state, "x", 1, 1, output, sizeof(output), &written) == SPEECH_MARKUP_ERR_ARGUMENT);
    speech_markup_init_v1(&state);
    CHECK(!speech_markup_feed_v1(&state, "clean", 5, 1, output, sizeof(output), &written));
    CHECK(written == 5 && !memcmp(output, "clean", 5));
    char long_span[400];
    memset(long_span, 'a', sizeof(long_span));
    long_span[0] = '*'; long_span[1] = '*';
    long_span[sizeof(long_span)-3] = '*'; long_span[sizeof(long_span)-2] = '*'; long_span[sizeof(long_span)-1] = 0;
    for (size_t split = 0; split < sizeof(long_span); ++split) {
        render(long_span, split, output);
        CHECK(!strcmp(output, long_span));
        ++checks;
    }
    printf("OK %zu speech markup partition cases\n", checks);
    return 0;
}
