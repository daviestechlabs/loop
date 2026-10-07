/* pb_cli_codec.h — pure-C kv-line encode/decode facade over pb_msg (no Go). */
#ifndef HANDLER_BASE_C_PB_CLI_CODEC_H
#define HANDLER_BASE_C_PB_CLI_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PB_CLI_OK = 0, PB_CLI_ERR = 1, PB_CLI_UNKNOWN = 2 };

int pb_cli_encode(const char *kind, const char *kv_text, uint8_t *out, size_t out_cap, size_t *out_len);
int pb_cli_decode(const char *kind, const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif
