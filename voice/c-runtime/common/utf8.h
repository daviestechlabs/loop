/* Strict bounded UTF-8 validation for product-visible text. */
#ifndef VOICE_C_UTF8_H
#define VOICE_C_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Return one only when the complete byte span is canonical UTF-8. */
int utf8_validate_v1(const uint8_t *text, size_t len);

#endif
