/* Test adapter only. Fourteen bytes per case; no native struct wire layout. */
#include "spell_budget.h"
#include <stdio.h>

int main(void) {
    uint8_t bytes[14];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), stdin)) != 0) {
        if (count != sizeof(bytes)) return 2;
        spell_budget_input input = {
            bytes[0], bytes[1], bytes[2], {bytes[3], bytes[4]},
            {bytes[5], bytes[6]}, {bytes[7], bytes[8]}, bytes[9],
            {{bytes[10], bytes[11]}, {bytes[12], bytes[13]}}
        };
        spell_budget_result result = spell_budget_evaluate(&input);
        printf("%u %u %u %u %u\n", result.status, result.possible_worlds,
            result.fitting_worlds, result.rejecting_reasons, result.universal_reasons);
    }
    return ferror(stdin) ? 2 : 0;
}
