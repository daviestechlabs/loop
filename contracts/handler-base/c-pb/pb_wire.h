/* pb_wire.h — pure-C protobuf wire helpers (no protoc, no Go).
 * Canonical codec home for handler-base messages.proto product path. */
#ifndef HANDLER_BASE_C_PB_WIRE_H
#define HANDLER_BASE_C_PB_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* The runtime also exports its optimized pb_min readers. Keep this contract
 * codec's linker symbols distinct while preserving existing source callers. */
#define pb_reader_init cpb_reader_init
#define pb_read_tag cpb_read_tag
#define pb_skip cpb_skip
#define pb_read_varint cpb_read_varint
#define pb_read_bytes cpb_read_bytes
#define pb_read_string cpb_read_string

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} pb_reader;

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t pos;
} pb_writer;

void pb_reader_init(pb_reader *r, const uint8_t *data, size_t len);
int pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire);
int pb_skip(pb_reader *r, uint32_t wire);
int pb_read_varint(pb_reader *r, uint64_t *out);
int pb_read_bytes(pb_reader *r, const uint8_t **out, size_t *out_len);
int pb_read_string(pb_reader *r, char *out, size_t out_cap);

void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap);
size_t pb_writer_len(const pb_writer *w);
int pb_write_varint(pb_writer *w, uint64_t v);
int pb_write_tag(pb_writer *w, uint32_t field, uint32_t wire);
int pb_write_string(pb_writer *w, uint32_t field, const char *s);
int pb_write_bytes(pb_writer *w, uint32_t field, const uint8_t *data, size_t len);
int pb_write_int64(pb_writer *w, uint32_t field, int64_t v); /* omit if 0 */
int pb_write_int64_force(pb_writer *w, uint32_t field, int64_t v);
int pb_write_bool(pb_writer *w, uint32_t field, int v); /* always emit */
int pb_write_enum(pb_writer *w, uint32_t field, int v); /* omit if 0 */
int pb_write_map_ss(pb_writer *w, uint32_t field, const char *key, const char *val);
/* fixed32 / float (wire type 5); omit float if 0.0 */
int pb_write_fixed32(pb_writer *w, uint32_t field, uint32_t bits);
int pb_write_float(pb_writer *w, uint32_t field, float f);
int pb_write_uint64(pb_writer *w, uint32_t field, uint64_t v); /* omit if 0 */

/* Scan message for first string field / int field. */
int pb_get_string(const uint8_t *in, size_t n, uint32_t field, char *out, size_t cap);
int64_t pb_get_int(const uint8_t *in, size_t n, uint32_t field);
int pb_get_bool(const uint8_t *in, size_t n, uint32_t field);
int pb_get_map_ss(const uint8_t *in, size_t n, uint32_t map_field, const char *key, char *out,
                  size_t cap);

#ifdef __cplusplus
}
#endif

#endif
