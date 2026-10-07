/* dice.h — pure-C NdS / kh1 / kl1 dice parse + keep + roll (no Go). */
#ifndef PLATFORM_TOOLS_DICE_H
#define PLATFORM_TOOLS_DICE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DICE_OK = 0,
    DICE_ERR = 1
};

#define DICE_MAX_COUNT 100
#define DICE_MAX_SIDES 1000
#define DICE_EXPR 64
#define DICE_MSG 160
#define DICE_KEEP 8

typedef struct dice_spec {
    int count;
    int sides;
    char keep[DICE_KEEP]; /* "" | "kh1" | "kl1" */
    int modifier;
    char expression[DICE_EXPR];
} dice_spec;

/* Normalize + parse. On error fills err (if non-NULL). */
int dice_parse(const char *raw, dice_spec *out, char *err, size_t err_cap);

/* "advantage" | "disadvantage" | "normal" */
const char *dice_mode(const char *keep);

/*
 * Select kept indexes into out_idx (cap >= count).
 * *n_kept set to number selected (count for normal, 1 for kh1/kl1).
 */
int dice_select_indexes(const int *rolls, int n_rolls, const char *keep, int *out_idx, int *n_kept);

/* Secure roll via getrandom: fills rolls[0..count-1] with values in 1..sides. */
int dice_secure_roll(const dice_spec *spec, int *rolls, int rolls_cap);

/*
 * Encode result JSON for pure dice outcome (no roll_id/reason/campaign).
 * out receives compact JSON.
 */
int dice_encode_roll_json(const dice_spec *spec, const int *rolls, int n_rolls, const int *kept_idx,
                          int n_kept, int total, const char *entropy_source, char *out, size_t cap);

/* Encode parse-only JSON. */
int dice_encode_spec_json(const dice_spec *spec, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
