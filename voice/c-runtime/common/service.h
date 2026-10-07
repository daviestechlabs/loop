#ifndef VOICE_C_SERVICE_H
#define VOICE_C_SERVICE_H

#include "../bus/vbus.h"

#include <stddef.h>

const char *svc_env(const char *key, const char *fallback);
int svc_env_int(const char *key, int fallback);
int svc_env_int_range(const char *key, int fallback, int min_value, int max_value);

/* Connect to VBus (default path from VBUS_PATH). No NATS. */
vbus_client *svc_connect_bus(void);

void svc_install_signals(vbus_stop_flag *stop);

#if defined(__GNUC__) || defined(__clang__)
#define SVC_PRINTF_LIKE(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define SVC_PRINTF_LIKE(format_index, first_arg)
#endif

void svc_log(const char *svc, const char *fmt, ...) SVC_PRINTF_LIKE(2, 3);

/* Emit two caller-bounded byte spans as one message. */
void svc_log_join_n(
    const char *svc,
    const char *literal,
    size_t literal_len,
    const char *value,
    size_t value_len);

#undef SVC_PRINTF_LIKE

#endif
