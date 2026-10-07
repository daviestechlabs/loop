#define _POSIX_C_SOURCE 200809L
#include "dice.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

static void test_parse_good(void) {
    dice_spec s;
    char err[160];
    expect(dice_parse(" 2D20 KH1 +5 ", &s, err, sizeof(err)) == DICE_OK, "parse adv");
    expect(s.count == 2 && s.sides == 20 && strcmp(s.keep, "kh1") == 0 && s.modifier == 5, "fields");
    expect(strcmp(s.expression, "2d20kh1+5") == 0, "norm expr");
    expect(strcmp(dice_mode(s.keep), "advantage") == 0, "mode");
    expect(dice_parse("100d2", &s, err, sizeof(err)) == DICE_OK, "100d2");
    expect(s.count == 100 && s.sides == 2, "100d2 fields");
    expect(dice_parse("1d20-3", &s, err, sizeof(err)) == DICE_OK, "mod neg");
    expect(s.modifier == -3, "mod -3");
    expect(dice_parse("3d6kl1", &s, err, sizeof(err)) == DICE_OK, "kl1");
    expect(strcmp(dice_mode(s.keep), "disadvantage") == 0, "disadv");
}

static void test_parse_bad(void) {
    dice_spec s;
    char err[160];
    expect(dice_parse("101d6", &s, err, sizeof(err)) == DICE_ERR, "101d6");
    expect(dice_parse("1d1", &s, err, sizeof(err)) == DICE_ERR, "1d1");
    expect(dice_parse("1d20kh1", &s, err, sizeof(err)) == DICE_ERR, "kh1 needs 2");
    expect(dice_parse("0d20", &s, err, sizeof(err)) == DICE_ERR, "0d20");
    expect(dice_parse("2d20kh2", &s, err, sizeof(err)) == DICE_ERR, "kh2");
    expect(dice_parse("", &s, err, sizeof(err)) == DICE_ERR, "empty");
}

static void test_select(void) {
    int rolls[] = {3, 18, 7};
    int idx[8];
    int n = 0;
    expect(dice_select_indexes(rolls, 3, "kh1", idx, &n) == DICE_OK && n == 1 && idx[0] == 1, "kh1");
    expect(dice_select_indexes(rolls, 3, "kl1", idx, &n) == DICE_OK && n == 1 && idx[0] == 0, "kl1");
    expect(dice_select_indexes(rolls, 3, "", idx, &n) == DICE_OK && n == 3 && idx[0] == 0 && idx[2] == 2,
           "all");
}

static void test_roll(void) {
    dice_spec s;
    int rolls[100];
    int i;
    char err[160];
    expect(dice_parse("4d6", &s, err, sizeof(err)) == DICE_OK, "4d6");
    expect(dice_secure_roll(&s, rolls, 100) == DICE_OK, "roll");
    for (i = 0; i < 4; i++)
        expect(rolls[i] >= 1 && rolls[i] <= 6, "range");
}

static void test_encode(void) {
    dice_spec s;
    char err[160];
    char out[512];
    int rolls[] = {4, 17};
    int kept[] = {1};
    expect(dice_parse("2d20kh1+5", &s, err, sizeof(err)) == DICE_OK, "enc parse");
    expect(dice_encode_spec_json(&s, out, sizeof(out)) == DICE_OK, "spec json");
    expect(strstr(out, "\"expression\":\"2d20kh1+5\"") != NULL, "spec expr");
    expect(dice_encode_roll_json(&s, rolls, 2, kept, 1, 22, "getrandom", out, sizeof(out)) == DICE_OK,
           "roll json");
    expect(strstr(out, "\"total\":22") != NULL, "total");
    expect(strstr(out, "\"mode\":\"advantage\"") != NULL, "mode json");
    expect(strstr(out, "getrandom") != NULL, "entropy");
}

int main(void) {
    test_parse_good();
    test_parse_bad();
    test_select();
    test_roll();
    test_encode();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ALL PASS c-dice unit\n");
    return 0;
}
