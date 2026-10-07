#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "dice.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <sys/random.h>
#endif
#include <sys/types.h>

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || cap == 0)
        return;
    snprintf(err, cap, "%s", msg ? msg : "error");
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

int dice_parse(const char *raw, dice_spec *out, char *err, size_t err_cap) {
    char norm[DICE_EXPR];
    size_t i, j;
    const char *p;
    int count, sides, modifier;
    char keep[DICE_KEEP];

    if (!out) {
        set_err(err, err_cap, "null spec");
        return DICE_ERR;
    }
    memset(out, 0, sizeof(*out));
    if (!raw) {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }
    /* lower + strip spaces */
    j = 0;
    for (i = 0; raw[i] && j + 1 < sizeof(norm); i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        norm[j++] = (char)tolower(c);
    }
    norm[j] = '\0';
    if (j == 0 || j >= DICE_EXPR - 1) {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }

    p = norm;
    /* count: 1-9, 10-99, or 100  → regex ([1-9][0-9]?|100) */
    if (p[0] == '1' && p[1] == '0' && p[2] == '0' && (p[3] == 'd' || p[3] == '\0')) {
        count = 100;
        p += 3;
    } else if (p[0] >= '1' && p[0] <= '9') {
        count = p[0] - '0';
        p++;
        if (is_digit(*p)) {
            count = count * 10 + (*p - '0');
            p++;
            if (is_digit(*p) && *p != 'd') {
                /* three-digit other than 100 invalid for this grammar */
                set_err(err, err_cap,
                        "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
                return DICE_ERR;
            }
        }
    } else {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }
    if (*p != 'd') {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }
    p++;
    /* sides: [1-9][0-9]{0,3} → 1..9999 but we bound 2..1000 later; leading zero invalid */
    if (!is_digit(*p) || *p == '0') {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }
    sides = 0;
    {
        int digits = 0;
        while (is_digit(*p) && digits < 4) {
            sides = sides * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (is_digit(*p)) {
            set_err(err, err_cap,
                    "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
            return DICE_ERR;
        }
    }
    keep[0] = '\0';
    if (p[0] == 'k' && p[1] == 'h' && p[2] == '1') {
        snprintf(keep, sizeof(keep), "kh1");
        p += 3;
    } else if (p[0] == 'k' && p[1] == 'l' && p[2] == '1') {
        snprintf(keep, sizeof(keep), "kl1");
        p += 3;
    }
    modifier = 0;
    if (*p == '+' || *p == '-') {
        int sign = (*p == '+') ? 1 : -1;
        int mag = 0;
        int digits = 0;
        p++;
        if (!is_digit(*p)) {
            set_err(err, err_cap,
                    "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
            return DICE_ERR;
        }
        while (is_digit(*p) && digits < 5) {
            mag = mag * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (is_digit(*p) || digits == 0 || digits > 5) {
            set_err(err, err_cap,
                    "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
            return DICE_ERR;
        }
        modifier = sign * mag;
    }
    if (*p != '\0') {
        set_err(err, err_cap, "expression must match NdS, NdSkh1, or NdSkl1 with an optional +/- modifier");
        return DICE_ERR;
    }
    if (count < 1 || count > 100) {
        set_err(err, err_cap, "dice count must be between 1 and 100");
        return DICE_ERR;
    }
    if (sides < 2 || sides > 1000) {
        set_err(err, err_cap, "die sides must be between 2 and 1000");
        return DICE_ERR;
    }
    if ((strcmp(keep, "kh1") == 0 || strcmp(keep, "kl1") == 0) && count < 2) {
        set_err(err, err_cap, "keep-highest/lowest requires at least two dice");
        return DICE_ERR;
    }
    if (modifier < -10000 || modifier > 10000) {
        set_err(err, err_cap, "modifier must be between -10000 and 10000");
        return DICE_ERR;
    }
    out->count = count;
    out->sides = sides;
    snprintf(out->keep, sizeof(out->keep), "%s", keep);
    out->modifier = modifier;
    snprintf(out->expression, sizeof(out->expression), "%s", norm);
    return DICE_OK;
}

const char *dice_mode(const char *keep) {
    if (keep && strcmp(keep, "kh1") == 0)
        return "advantage";
    if (keep && strcmp(keep, "kl1") == 0)
        return "disadvantage";
    return "normal";
}

int dice_select_indexes(const int *rolls, int n_rolls, const char *keep, int *out_idx, int *n_kept) {
    int i, selected;
    if (!rolls || n_rolls < 1 || !out_idx || !n_kept)
        return DICE_ERR;
    if (!keep || !keep[0]) {
        for (i = 0; i < n_rolls; i++)
            out_idx[i] = i;
        *n_kept = n_rolls;
        return DICE_OK;
    }
    if (strcmp(keep, "kh1") != 0 && strcmp(keep, "kl1") != 0)
        return DICE_ERR;
    selected = 0;
    for (i = 1; i < n_rolls; i++) {
        if ((strcmp(keep, "kh1") == 0 && rolls[i] > rolls[selected]) ||
            (strcmp(keep, "kl1") == 0 && rolls[i] < rolls[selected]))
            selected = i;
    }
    out_idx[0] = selected;
    *n_kept = 1;
    return DICE_OK;
}

/* Rejection sampling for unbiased 1..sides. */
static int roll_one(int sides, int *out) {
    unsigned int limit, r;
    unsigned char buf[4];
    if (sides < 2)
        return DICE_ERR;
    /* largest multiple of sides below 2^32 */
    limit = (unsigned int)(0xffffffffu / (unsigned int)sides) * (unsigned int)sides;
    for (;;) {
#ifdef __APPLE__
        arc4random_buf(buf, sizeof(buf));
#else
        if (getrandom(buf, sizeof(buf), 0) != (ssize_t)sizeof(buf))
            return DICE_ERR;
#endif
        r = ((unsigned int)buf[0] << 24) | ((unsigned int)buf[1] << 16) | ((unsigned int)buf[2] << 8) |
            (unsigned int)buf[3];
        if (r < limit) {
            *out = (int)(r % (unsigned int)sides) + 1;
            return DICE_OK;
        }
    }
}

int dice_secure_roll(const dice_spec *spec, int *rolls, int rolls_cap) {
    int i;
    if (!spec || !rolls || rolls_cap < spec->count)
        return DICE_ERR;
    for (i = 0; i < spec->count; i++) {
        if (roll_one(spec->sides, &rolls[i]) != DICE_OK)
            return DICE_ERR;
    }
    return DICE_OK;
}

static int append_int_array(char *out, size_t cap, size_t *pos, const int *vals, int n) {
    int i, w;
    if (*pos + 1 >= cap)
        return -1;
    out[(*pos)++] = '[';
    for (i = 0; i < n; i++) {
        if (i) {
            if (*pos + 1 >= cap)
                return -1;
            out[(*pos)++] = ',';
        }
        w = snprintf(out + *pos, cap - *pos, "%d", vals[i]);
        if (w < 0 || (size_t)w >= cap - *pos)
            return -1;
        *pos += (size_t)w;
    }
    if (*pos + 1 >= cap)
        return -1;
    out[(*pos)++] = ']';
    out[*pos] = '\0';
    return 0;
}

int dice_encode_spec_json(const dice_spec *spec, char *out, size_t cap) {
    int w;
    if (!spec || !out || cap < 8)
        return DICE_ERR;
    w = snprintf(out, cap,
                 "{\"count\":%d,\"sides\":%d,\"keep\":\"%s\",\"modifier\":%d,\"expression\":\"%s\","
                 "\"mode\":\"%s\"}",
                 spec->count, spec->sides, spec->keep, spec->modifier, spec->expression, dice_mode(spec->keep));
    if (w < 0 || (size_t)w >= cap)
        return DICE_ERR;
    return DICE_OK;
}

int dice_encode_roll_json(const dice_spec *spec, const int *rolls, int n_rolls, const int *kept_idx,
                          int n_kept, int total, const char *entropy_source, char *out, size_t cap) {
    size_t pos = 0;
    int w, i;
    int kept_rolls[DICE_MAX_COUNT];
    if (!spec || !rolls || !kept_idx || !out || !entropy_source)
        return DICE_ERR;
    if (n_kept > DICE_MAX_COUNT || n_rolls > DICE_MAX_COUNT)
        return DICE_ERR;
    for (i = 0; i < n_kept; i++)
        kept_rolls[i] = rolls[kept_idx[i]];

    w = snprintf(out, cap,
                 "{\"expression\":\"%s\",\"rolls\":", spec->expression);
    if (w < 0 || (size_t)w >= cap)
        return DICE_ERR;
    pos = (size_t)w;
    if (append_int_array(out, cap, &pos, rolls, n_rolls) != 0)
        return DICE_ERR;
    w = snprintf(out + pos, cap - pos, ",\"kept_indexes\":");
    if (w < 0 || (size_t)w >= cap - pos)
        return DICE_ERR;
    pos += (size_t)w;
    if (append_int_array(out, cap, &pos, kept_idx, n_kept) != 0)
        return DICE_ERR;
    w = snprintf(out + pos, cap - pos, ",\"kept_rolls\":");
    if (w < 0 || (size_t)w >= cap - pos)
        return DICE_ERR;
    pos += (size_t)w;
    if (append_int_array(out, cap, &pos, kept_rolls, n_kept) != 0)
        return DICE_ERR;
    w = snprintf(out + pos, cap - pos,
                 ",\"modifier\":%d,\"total\":%d,\"mode\":\"%s\",\"entropy_source\":\"%s\"}",
                 spec->modifier, total, dice_mode(spec->keep), entropy_source);
    if (w < 0 || (size_t)w >= cap - pos)
        return DICE_ERR;
    return DICE_OK;
}
