/* Authenticated HTTP fallback edge for the pure-C voice runtime. */
#ifndef VOICE_C_GATEWAY_HTTP_H
#define VOICE_C_GATEWAY_HTTP_H

#include "../bus/vbus.h"

#define GATEWAY_HTTP_RX_CAP (8192u + 16384u) /* Header bound plus a complete typed turn. */

#ifdef __cplusplus
extern "C" {
#endif

/* Run the bounded HTTP edge until stop is set. */
int gateway_http_run(int port, vbus_stop_flag *stop);

#ifdef __cplusplus
}
#endif

#endif
