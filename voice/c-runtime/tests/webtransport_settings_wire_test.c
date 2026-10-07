/* Exercise the pinned LSQUIC SETTINGS writer, not a copy of its encoder. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "lsquic_varint.h"
#include "lsquic_hq.h"
#define LSQUIC_WEBTRANSPORT_SERVER_SUPPORT 1
#define LSQ_INFO(...) ((void)0)
#define LSQ_DEBUG(...) ((void)0)
struct hcso_writer { int how_fral; void *how_stream; };
static unsigned char output[256];
static size_t output_size;
static unsigned random_width;
static unsigned hcso_setting_type2bits(struct hcso_writer *w, unsigned value) {
    (void)w;
    return random_width ? 3u : (unsigned)vint_val2bits(value);
}
static int lsquic_frab_list_empty(int *list) { (void)list; return 1; }
static int lsquic_frab_list_write(int *list, const void *data, size_t size) {
    (void)list;
    if (size > sizeof(output)) abort();
    memcpy(output, data, size); output_size = size; return 0;
}
static void lsquic_stream_wantwrite(void *stream, int value) { (void)stream; (void)value; }
#include "settings_writer_under_test.inc"
static void require(int value) { if (!value) { fputs("SETTINGS wire assertion failed\n", stderr); exit(1); } }
static uint64_t read_value(size_t *offset) {
    require(*offset < output_size);
    size_t size = (size_t)1 << (output[*offset] >> 6);
    require(size <= output_size - *offset);
    uint64_t value = output[(*offset)++] & 63u;
    for (size_t i = 1; i < size; ++i) value = (value << 8) | output[(*offset)++];
    return value;
}
static void check(int server, int enabled, unsigned sessions) {
    struct hcso_writer writer = {0};
    require(lsquic_hcso_write_settings(&writer, UINT_MAX, UINT_MAX, UINT_MAX,
                                     server, enabled, sessions) == 0);
    size_t offset = 0;
    require(read_value(&offset) == 4);
    uint64_t size = read_value(&offset);
    require(size == output_size - offset);
    unsigned draft07 = 0, legacy = 0, datagram = 0, connect = 0;
    while (offset < output_size) {
        uint64_t key = read_value(&offset), value = read_value(&offset);
        require(key != 0x14e9cd29); /* Draft 13 requires unimplemented RESET_STREAM_AT. */
        if (key == 0xc671706a) { require(value == 1); draft07++; }
        if (key == 0x2b603743) { require(value == sessions); legacy++; }
        if (key == 0x33) { require(value == 1); datagram++; }
        if (key == 8) { require(value == 1); connect++; }
    }
    unsigned expected = server && enabled && sessions;
    require(draft07 == expected && legacy == expected && datagram == expected && connect == expected);
}
int main(void) {
    unsigned widths = 1;
#ifndef NDEBUG
    widths = 2;
#endif
    for (random_width = 0; random_width < widths; ++random_width) {
        check(1, 1, 1); check(1, 1, 64); check(0, 1, 1); check(1, 0, 1); check(1, 1, 0);
    }
    puts("WebTransport SETTINGS: Safari, legacy, disabled, and frame bounds passed");
    return 0;
}
