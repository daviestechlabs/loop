#ifndef VOICE_C_RUNTIME_COMMON_VOICE_ASCII_H
#define VOICE_C_RUNTIME_COMMON_VOICE_ASCII_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int voice_ascii_is_digit(unsigned char value) {
    return (unsigned)(value - (unsigned char)'0') <= 9u;
}

static inline int voice_ascii_is_alnum(unsigned char value) {
    return voice_ascii_is_digit(value) ||
           (unsigned)((value | 0x20u) - (unsigned char)'a') <= 25u;
}

/* HTTP field lines accept visible ASCII and horizontal tabs. */
static inline int voice_ascii_http_line_valid(const char *line, size_t line_len) {
    static const uint64_t byte_ones = UINT64_C(0x0101010101010101);
    static const uint64_t byte_high_bits = UINT64_C(0x8080808080808080);
    static const uint64_t byte_spaces = UINT64_C(0x2020202020202020);
    const unsigned char *scan = (const unsigned char *)line;
    if (!line) return 0;
    while (line_len >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, scan, sizeof(word));
        if ((((word + byte_ones) | word) & byte_high_bits) != 0u ||
            (((word - byte_spaces) & ~word & byte_high_bits) != 0u))
            break;
        scan += sizeof(word);
        line_len -= sizeof(word);
    }
    while (line_len != 0u) {
        unsigned char byte = *scan++;
        if (byte != '\t' && (byte < 0x20u || byte > 0x7eu)) return 0;
        line_len--;
    }
    return 1;
}

#endif
