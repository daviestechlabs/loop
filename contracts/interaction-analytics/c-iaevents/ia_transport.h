#ifndef INTERACTION_ANALYTICS_TRANSPORT_H
#define INTERACTION_ANALYTICS_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define IA_SESSION_CAP 69u
#define IA_WIRE_CAP 4096u
#define IA_OTLP_CAP 4608u

/* The owner comes from verified authentication. Keep the raw browser nonce in
 * turn metadata; derive the warehouse key exactly once at each C emitter. */
int ia_product_session_id(const char *owner, const char *nonce, char *out, size_t cap);

/* One bounded canonical JSON event in an OTLP protobuf log record. The caller
 * supplies its server-owned service name and wall clock. Zero means success. */
int ia_otlp_encode(const char *service, const char *wire, long long now_ms,
                   uint8_t *out, size_t cap, size_t *len);
/* A successful protobuf ExportLogsServiceResponse rejects zero records. */
int ia_otlp_ack(const uint8_t *body, size_t len);

#endif
