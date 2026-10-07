/* pt_manager.h — pure-C platform-tools call manager (no Go). */
#ifndef PT_MANAGER_H
#define PT_MANAGER_H

#include "pt_store.h"
#include "pt_worker.h"
#include "pt_pb.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct pt_manager {
    pt_store store;
    pt_worker worker;
    pthread_mutex_t mutex;
    int initialized;
} pt_manager;

int pt_manager_init(pt_manager *m, const char *store_path, const char *ws_root,
                    const char *artifact_dir, const char *dnd_dir);
void pt_manager_destroy(pt_manager *m);
int pt_manager_ready(pt_manager *m);

/* The transport must authenticate user before calling this owner check. */
int pt_read_owned_call(pt_manager *m, const char *id, const char *user, pt_call *out);

/* Start or reuse by idempotency; may execute immediately for known local tools. */
int pt_start_call_json(pt_manager *m, const char *req_json, char *out, size_t cap);

/* GET call record as JSON response. */
int pt_get_call_json(pt_manager *m, const char *id, char *out, size_t cap);

/* Cancel non-terminal call. */
int pt_cancel_call_json(pt_manager *m, const char *id, const char *reason, char *out, size_t cap);

/* Approval: approved=1 runs if awaiting; 0 rejects. */
int pt_approve_call_json(pt_manager *m, const char *id, int approved, const char *approval_id,
                         char *out, size_t cap);

/* Structured core (shared by JSON + protobuf). */
int pt_start_core(pt_manager *m, const pt_start_req *req, pt_start_resp *resp);
int pt_execute_core(pt_manager *m, const pt_start_req *req, pt_dispatch_resp *resp);
int pt_cancel_core(pt_manager *m, const pt_cancel_req *req, pt_start_resp *resp);
int pt_approve_core(pt_manager *m, const pt_approval_req *req, pt_start_resp *resp);

/* Protobuf NATS handlers: raw payload in → raw response out. */
int pt_handle_start_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                       size_t *out_len);
int pt_handle_execute_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                         size_t *out_len);
int pt_handle_cancel_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        size_t *out_len);
int pt_handle_approval_pb(pt_manager *m, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                          size_t *out_len);

int64_t pt_now_ms(void);

#endif
