#ifndef VOICE_C_VBUS_SUBJECT_H
#define VOICE_C_VBUS_SUBJECT_H

#include <stddef.h>

/* A publish subject is non-empty printable ASCII without subscription wildcards.
 * subject_cap includes the required terminating NUL byte. */
int vbus_publish_subject_valid(const char *subject, size_t subject_cap);

/* Validate an exact, nonempty subject span without searching for a terminator. */
int vbus_publish_subject_span_valid(const char *subject, size_t subject_len);

#endif
