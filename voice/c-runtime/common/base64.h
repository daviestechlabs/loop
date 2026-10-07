/* Bounded RFC 4648 Base64 encoding for public voice edges. */
#ifndef VOICE_C_COMMON_BASE64_H
#define VOICE_C_COMMON_BASE64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the encoded byte count, excluding the trailing NUL. */
size_t base64_encoded_size_v1(size_t input_len);

/* Encode one bounded byte span. A successful output includes a trailing NUL. */
size_t base64_encode_v1(
    const uint8_t *input,
    size_t input_len,
    char *output,
    size_t output_cap
);

/* Scalar entry point exposed for differential measurement. */
size_t base64_encode_scalar_v1(
    const uint8_t *input,
    size_t input_len,
    char *output,
    size_t output_cap
);

int base64_host_has_ssse3_v1(void);
int base64_using_ssse3_v1(void);
int base64_host_has_avx2_v1(void);
int base64_using_avx2_v1(void);

#ifdef __cplusplus
}
#endif

#endif
