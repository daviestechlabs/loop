#ifndef CMP_GATEWAY_IDENTITY_H
#define CMP_GATEWAY_IDENTITY_H
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#define CMP_GATEWAY_JWKS_CAP 65536
#define CMP_GATEWAY_TOKEN_CAP 16384
/* Configuration strings remain owned by the caller and immutable until close. */
typedef struct { const char *issuer, *audience, *required_group, *jwks_url; } cmp_gateway_policy;
typedef struct {
    cmp_gateway_policy policy;
    pthread_mutex_t cache_mutex, refresh_mutex;
    char keys[CMP_GATEWAY_JWKS_CAP+1];
    int64_t expires, attempted;
} cmp_gateway_identity;
/* HTTP status: 200 verified, 401 invalid, 403 group denied, 503 keys unavailable. */
int cmp_gateway_verify(const char *token, const char *jwks, const cmp_gateway_policy *policy, int64_t now, char subject[256]);
int cmp_gateway_identity_init(cmp_gateway_identity *identity, const cmp_gateway_policy *policy);
void cmp_gateway_identity_close(cmp_gateway_identity *identity);
int cmp_gateway_identity_verify(cmp_gateway_identity *identity, const char *token, char subject[256]);
#endif
