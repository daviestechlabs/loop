#include "vbus_subject.h"

#include <string.h>

int vbus_publish_subject_span_valid(const char *subject, size_t subject_len) {
    size_t i;
    unsigned char invalid = 0;
    if (!subject || subject_len == 0u) return 0;
    for (i = 0; i < subject_len; ++i) {
        unsigned char c = (unsigned char)subject[i];
        invalid |= (unsigned char)(c <= 0x20u) |
            (unsigned char)(c >= 0x7fu) |
            (unsigned char)(c == (unsigned char)'*') |
            (unsigned char)(c == (unsigned char)'>');
    }
    return invalid == 0;
}

int vbus_publish_subject_valid(const char *subject, size_t subject_cap) {
    const char *end;
    if (!subject || subject_cap < 2u) return 0;
    end = (const char *)memchr(subject, '\0', subject_cap);
    if (!end || end == subject) return 0;
    return vbus_publish_subject_span_valid(
        subject, (size_t)(end - subject));
}
