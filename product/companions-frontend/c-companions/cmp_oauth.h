/* cmp_oauth.h — bounded OAuth authorization-code and PKCE edge. */
#ifndef C_COMPANIONS_CMP_OAUTH_H
#define C_COMPANIONS_CMP_OAUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CMP_OAUTH_OK = 0,
    CMP_OAUTH_ERR = 1
};

#define CMP_OAUTH_STATE_CAP 64
#define CMP_OAUTH_STATE_TTL_SEC 600
#define CMP_OAUTH_LOCATION_CAP 4096
#define CMP_OAUTH_REASON_CAP 64

typedef struct {
    char provider[64];
    char subject[256];
    char username[64];
    char email[256];
    char name[256];
} cmp_oauth_identity;

typedef int (*cmp_oauth_http_fn)(const char *method, const char *url, const char *content_type,
                                 const char *authorization, const uint8_t *body, size_t body_len,
                                 uint8_t *out, size_t out_cap, size_t *out_len, void *user);

/* Load and validate OAuth configuration. An absent configuration is not an error. */
int cmp_oauth_init(void);
void cmp_oauth_cleanup(void);
int cmp_oauth_enabled(void);

/* Create one state/PKCE binding and its authorization redirect. */
int cmp_oauth_begin(const char *request_path, char *state, size_t state_cap,
                    char *location, size_t location_cap, char *reason, size_t reason_cap);

/* Consume one state and resolve the provider identity. */
int cmp_oauth_finish(const char *request_path, const char *state_cookie,
                     cmp_oauth_identity *identity, char *reason, size_t reason_cap);

/* Deterministic parser entry for unit tests and libFuzzer. */
int cmp_oauth_callback_query_valid(const char *request_path, size_t request_path_len);

/* Fetch public signing keys with the existing verified HTTPS transport. */
int cmp_oauth_public_keys_get(const char *url, uint8_t *out, size_t cap, size_t *length);

/* Bounded HTTPS session read. Caller must restrict the destination before forwarding cookies.
 * Verifies TLS, rejects redirects, and admits only successful JSON response framing. */
int cmp_oauth_session_get(const char *url, const char *cookie, uint8_t *out, size_t cap, size_t *length);

/* Forward a previously verified gateway ID token to a caller-restricted HTTPS authority.
 * Sends no browser cookies. The receiver must independently verify the token. */
int cmp_oauth_identity_get(const char *url, const char *token, uint8_t *out, size_t cap, size_t *length);

/* Replace outbound HTTPS only in process-local tests. NULL restores the real client. */
void cmp_oauth_set_http_for_tests(cmp_oauth_http_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif
