/*
 * c-dice — pure-C dice parse / keep / secure roll CLI.
 *   c-dice parse <expression>     → JSON spec
 *   c-dice roll <expression>      → JSON rolls + keep + total (getrandom)
 * Exit 0 ok, 1 error (message on stderr).
 */
#define _POSIX_C_SOURCE 200809L
#include "dice.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    dice_spec spec;
    char err[DICE_MSG];
    char out[4096];
    int rolls[DICE_MAX_COUNT];
    int kept[DICE_MAX_COUNT];
    int n_kept = 0;
    int total;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: c-dice parse|roll <expression>\n");
        return 2;
    }
    if (dice_parse(argv[2], &spec, err, sizeof(err)) != DICE_OK) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (strcmp(argv[1], "parse") == 0) {
        if (dice_encode_spec_json(&spec, out, sizeof(out)) != DICE_OK) {
            fprintf(stderr, "encode failed\n");
            return 1;
        }
        puts(out);
        return 0;
    }
    if (strcmp(argv[1], "roll") == 0) {
        if (dice_secure_roll(&spec, rolls, DICE_MAX_COUNT) != DICE_OK) {
            fprintf(stderr, "secure roll failed\n");
            return 1;
        }
        if (dice_select_indexes(rolls, spec.count, spec.keep, kept, &n_kept) != DICE_OK) {
            fprintf(stderr, "select failed\n");
            return 1;
        }
        total = spec.modifier;
        for (i = 0; i < n_kept; i++)
            total += rolls[kept[i]];
        if (dice_encode_roll_json(&spec, rolls, spec.count, kept, n_kept, total, "getrandom", out,
                                  sizeof(out)) != DICE_OK) {
            fprintf(stderr, "encode failed\n");
            return 1;
        }
        puts(out);
        return 0;
    }
    fprintf(stderr, "unknown op\n");
    return 2;
}
