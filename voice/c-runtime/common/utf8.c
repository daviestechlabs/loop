/* Strict bounded UTF-8 validation for product-visible text. */
#include "utf8.h"

int utf8_validate_v1(const uint8_t *text, size_t len) {
    size_t offset = 0;
    if (!text && len != 0) return 0;
    while (offset < len) {
        const uint8_t *p = text + offset;
        size_t remaining = len - offset;
        if (p[0] <= 0x7fu) {
            offset++;
        } else if (remaining >= 2u && p[0] >= 0xc2u && p[0] <= 0xdfu &&
                   p[1] >= 0x80u && p[1] <= 0xbfu) {
            offset += 2u;
        } else if (remaining >= 3u && p[0] == 0xe0u &&
                   p[1] >= 0xa0u && p[1] <= 0xbfu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu) {
            offset += 3u;
        } else if (remaining >= 3u &&
                   ((p[0] >= 0xe1u && p[0] <= 0xecu) ||
                    (p[0] >= 0xeeu && p[0] <= 0xefu)) &&
                   p[1] >= 0x80u && p[1] <= 0xbfu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu) {
            offset += 3u;
        } else if (remaining >= 3u && p[0] == 0xedu &&
                   p[1] >= 0x80u && p[1] <= 0x9fu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu) {
            offset += 3u;
        } else if (remaining >= 4u && p[0] == 0xf0u &&
                   p[1] >= 0x90u && p[1] <= 0xbfu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu &&
                   p[3] >= 0x80u && p[3] <= 0xbfu) {
            offset += 4u;
        } else if (remaining >= 4u && p[0] >= 0xf1u && p[0] <= 0xf3u &&
                   p[1] >= 0x80u && p[1] <= 0xbfu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu &&
                   p[3] >= 0x80u && p[3] <= 0xbfu) {
            offset += 4u;
        } else if (remaining >= 4u && p[0] == 0xf4u &&
                   p[1] >= 0x80u && p[1] <= 0x8fu &&
                   p[2] >= 0x80u && p[2] <= 0xbfu &&
                   p[3] >= 0x80u && p[3] <= 0xbfu) {
            offset += 4u;
        } else {
            return 0;
        }
    }
    return 1;
}
