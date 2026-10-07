#ifndef VOICE_PRODUCT_ANALYTICS_H
#define VOICE_PRODUCT_ANALYTICS_H

#include <stddef.h>
#include <stdint.h>
#include "pb_min.h"

typedef struct voice_analytics voice_analytics;

enum voice_analytics_outcome {
    VOICE_ANALYTICS_COMPLETED = 1,
    VOICE_ANALYTICS_CANCELED,
    VOICE_ANALYTICS_FAILED,
    VOICE_ANALYTICS_DISCONNECTED
};

/* Owned snapshots only. No receive-buffer or reusable connection pointers
 * cross the worker queue. Times use CLOCK_MONOTONIC nanoseconds. */
typedef struct {
    char owner[64];
    char nonce[128];
    char request_id[128];
    char campaign[128];
    char profile[32];
    uint64_t admitted_ns;
    uint64_t first_audio_ns;
    uint64_t finished_ns;
    long long finished_unix_ms;
    int audio_input;
    int voice_output;
    int webtransport;
    int client_interrupt;
    enum voice_analytics_outcome outcome;
} voice_analytics_turn;

typedef struct {
    unsigned queued;
    unsigned accepted;
    unsigned failed;
    unsigned dropped;
} voice_analytics_counts;

/* NULL means disabled or unavailable. The URL is server configuration only.
 * Encoding, identity hashing, entropy, and HTTP run on one C worker thread. */
voice_analytics *voice_analytics_start(const char *url, const char *service);
void voice_analytics_stop(voice_analytics *worker);
voice_analytics_counts voice_analytics_get_counts(const voice_analytics *worker);

/* An invalid/missing product nonce leaves an inactive observation. This does
 * not reject a voice turn or certify a product session. */
void voice_analytics_begin(voice_analytics_turn *observation,
                           const turn_start_c *turn, int audio_input,
                           int webtransport, uint64_t admitted_ns);
void voice_analytics_audio(voice_analytics_turn *observation, uint64_t now_ns);

/* Single event-loop producer. A full queue drops this observation
 * and increments a counter; it never waits for the worker or retries a POST.
 * Terminal calls are idempotent even when delivery fails. */
void voice_analytics_finish(voice_analytics *worker, voice_analytics_turn *observation,
                            enum voice_analytics_outcome outcome, uint64_t now_ns);

/* Deterministic policy/format boundary, used by the worker and local tests.
 * Unknown route/model/stage data is omitted. No browser timings are consumed. */
int voice_analytics_wire(const voice_analytics_turn *observation, long long now_ms,
                         char *out, size_t cap);

#endif
