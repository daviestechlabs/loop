/* Compile each real turn codec separately; their internal wire types differ. */
#include <stdio.h>
#include <stdint.h>
#ifdef PRODUCT_WIRE
#include "../../../contracts/handler-base/c-pb/pb_msg.h"
#else
#include "../wire/pb_min.h"
#endif

int main(void) {
    uint8_t input[DND_TURN_START_WIRE_MAX + 1u], output[DND_TURN_START_WIRE_MAX];
    size_t size = fread(input, 1u, sizeof(input), stdin), length;
    if (ferror(stdin) || size > DND_TURN_START_WIRE_MAX) return 2;
#ifdef PRODUCT_WIRE
    pb_turn_start_req turn;
    if (pb_dec_turn_start_req(input, size, &turn) != 0) return 1;
    length = pb_enc_turn_start_req(output, sizeof(output), &turn);
#else
    turn_start_c turn;
    if (pb_decode_turn_start(input, size, &turn) != 0) return 1;
    length = pb_encode_turn_start(output, sizeof(output), &turn);
#endif
    if (!length || fwrite(output, 1u, length, stdout) != length) return 2;
    return 0;
}
