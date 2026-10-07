// reflex_kernel.c
// Standalone C test. Runs the real engine on fake audio and verifies its public
// monitor/snapshot boundary plus process-local ring integrity.
// Usage: ./reflex_kernel --frames 6 --seed 123
// Build: gcc ... (see test.sh)

#include "telemetry.h"
#include "audio_engine.h"   // real ported hot path for deeper integration test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>

static void kernel_consumer(int is_frame, const void* data, size_t size, void* user) {
    (void)size;
    unsigned* counts = (unsigned*)user;
    if (is_frame) {
        const frame_telemetry_t* f = (const frame_telemetry_t*)data;
        if (f->turn_id_lo == 0 && f->features.energy < 0.001f) return;
        if (counts) counts[0]++;
        static unsigned frame_c = 0;
        frame_c++;
        if ((frame_c % 8) == 0) {
            printf("  [kernel] FRAME t=%llx-%llx e=%.2f rtf=%.3f\n",
                   (unsigned long long)f->turn_id_hi, (unsigned long long)f->turn_id_lo,
                   f->features.energy, f->rtf_this_frame);
        }
    } else {
        const stage_event_t* s = (const stage_event_t*)data;
        if (s->stage_id == 0 && s->turn_id_lo == 0) return;
        if (counts && s->stage_id == STAGE_KIND_VAD) counts[1]++;
        if (s->stage_id == STAGE_KIND_VAD) {
            printf("  [kernel] VAD kind=%u turn=%llx latency=%.3fms\n",
                   s->stage_id, (unsigned long long)s->turn_id_lo, s->latency_ms);
        }
    }
}

int main(int argc, char **argv) {
    int num_frames = 28;
    int num_turns = 1;
    int verbose = 1;
    int seed = 42;  // default for reproducibility in integration tests
    int ring_cap = 512;

    struct option long_opts[] = {
        {"frames", required_argument, 0, 'f'},
        {"turns", required_argument, 0, 't'},
        {"quiet", no_argument, 0, 'q'},
        {"seed", required_argument, 0, 's'},
        {"ring-cap", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:t:qs:c:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'f': num_frames = atoi(optarg); break;
            case 't': num_turns = atoi(optarg); break;
            case 'q': verbose = 0; break;
            case 's': seed = atoi(optarg); break;
            case 'c': ring_cap = atoi(optarg); break;
            default: break;
        }
    }

    srand((unsigned)seed);
    printf("=== C Reflex Kernel (real audio_engine + ring integration) ===\n");
    printf("CLI: --frames=%d --turns=%d --seed=%d --ring-cap=%d\n", num_frames, num_turns, seed, ring_cap);
    printf("Hardened integration test exercising full real C paths + monitor decisions.\n\n");

    telemetry_init_global_ring(ring_cap);
    telemetry_ring_t* gring = telemetry_get_global_ring();
    if (!gring) {
        fprintf(stderr, "global ring init failed\n");
        return 1;
    }

    ring_stats_t rs = {0};
    if (telemetry_get_ring_stats(gring, &rs) == 0) {
        printf("Initial ring: cap=%zu occ=%zu pressure=%.2f\n", rs.capacity, rs.occupancy, rs.pressure);
    }

    audio_engine_session* sess = audio_engine_create(
        0.28f, 0.93, 1, 1, 0.007, 0.07, 1.6, 0.52
    );
    if (!sess) {
        fprintf(stderr, "audio_engine_create failed\n");
        return 1;
    }

    unsigned counts[2] = {0};
    int live_triggers = 0;
    int process_actions = 0;
    int pub_triggers = 0;
    int live_pub_triggers = 0;
    int monitor_failures = 0;
    int snapshot_mismatches = 0;
    const audio_process_options process_options = {
        .interrupt_threshold = 0.15f,
        .interrupt_duration_s = 0.8f,
        .flags = AUDIO_PROCESS_INTERRUPT_ENABLED,
    };

    for (int turn = 0; turn < num_turns; turn++) {
        if (verbose) printf("--- TURN %d ---\n", turn);

        size_t fake_utt = 0;
        uint64_t now_ns = 0;

        for (int f = 0; f < num_frames; f++) {
            now_ns += 20000000ULL;

            uint8_t pcm[640];
            int16_t* s16 = (int16_t*)pcm;
            int type = (f + (rand() % 3)) % 4;
            float base_amp = (f % 8 < 5) ? 10500.0f : 2200.0f;
            float amp = base_amp * (0.7f + ((rand() % 6) / 10.0f));
            for (int j = 0; j < 320; j++) {
                int16_t val = 0;
                if (type == 0) val = 0;
                else if (type == 1) val = (j % 2 ? (int16_t)amp : -(int16_t)amp);
                else if (type == 2) val = ((j / 5) % 2 ? (int16_t)amp : -(int16_t)amp);
                else val = (int16_t)((rand() % (int)(amp * 2)) - amp);
                s16[j] = val;
            }

            audio_decision dec = {0};
            audio_engine_process(
                sess, pcm, sizeof(pcm), now_ns, process_options, &dec);

            const int speech = (dec.flags & AUDIO_DECISION_SPEECH) != 0u;
            if (speech) fake_utt += 640; else fake_utt = 0;

            telemetry_consume(gring, kernel_consumer, counts);

            audio_monitor_state mst = {0};
            audio_engine_get_monitor_state(sess, &mst);

            audio_monitor_tick tick = {0};
            if (!audio_engine_monitor_tick(
                    sess,
                    4096,
                    16384,
                    4.0f,
                    now_ns,
                    200000000ULL,
                    1,
                    300000000ULL,
                    800000000ULL,
                    5000000000ULL,
                    1,
                    1,
                    100000000ULL,
                    30000000ULL,
                    &tick)) {
                monitor_failures++;
            }
            if (tick.pending_audio != mst.pending_audio ||
                tick.live_partial_eligible != mst.live_partial_eligible ||
                tick.utterance_bytes != mst.utterance_bytes ||
                tick.is_complete != mst.is_complete ||
                tick.has_voice != mst.has_voice) {
                snapshot_mismatches++;
            }
            size_t utt_b = tick.utterance_bytes;

            int intr = (dec.flags & AUDIO_DECISION_INTERRUPT) != 0u;
            int should_part = audio_engine_should_do_partial(sess, now_ns, fake_utt, 1024, 512, 150000000ULL);

            if ((f % 3) == 0 && fake_utt > 0) {
                audio_engine_record_partial(sess, now_ns, fake_utt);
            }

            // Publication eligibility belongs to the caller after the C
            // session returns its snapshot. Keep the proof expectation local
            // instead of restoring a stateless production C export.
            int should_pub = speech && fake_utt > 0;
            int should_live_pub = should_pub;

            if (mst.live_partial_eligible) live_triggers++;
            if (tick.reason != 0) process_actions++;
            if (should_pub) pub_triggers++;
            if (mst.live_partial_eligible && should_live_pub) live_pub_triggers++;

            if (verbose) {
                printf("  [C-DECISIONS f=%d] mon: pend=%d live=%d sugg=%lluns | action=%d intr=%d reason=%d part=%d pub=%d live_pub=%d trig=%d | utt=%zu\n",
                       f, mst.pending_audio, mst.live_partial_eligible,
                       (unsigned long long)tick.monitor_interval_ns,
                       tick.reason != 0, intr, tick.reason, should_part, should_pub,
                       should_live_pub, tick.live_partial_trigger,
                       utt_b);
            }
        }

        (void)audio_engine_finish_processing(sess, 1);
    }

    telemetry_consume(gring, kernel_consumer, counts);

    int saw_frames = counts[0] > 0;
    int saw_vad = counts[1] > 0;
    int gate_ok = saw_frames && saw_vad && monitor_failures == 0 && snapshot_mismatches == 0;

    printf("\n=== FINAL STATS ===\n");
    printf("frames=%u vad=%u emitted=%llu\n", counts[0], counts[1],
           (unsigned long long)telemetry_emitted_count(gring));
    printf("C decisions: live=%d actions=%d pub=%d live_pub=%d\n",
           live_triggers, process_actions, pub_triggers, live_pub_triggers);

    printf("\n=== INTEGRATION GATES ===\n");
    printf("FRAME events seen:                  %s\n", saw_frames ? "PASS" : "FAIL");
    printf("VAD stage events seen:              %s\n", saw_vad ? "PASS" : "FAIL");
    printf("Monitor aggregate returned:         %s\n", monitor_failures == 0 ? "PASS" : "FAIL");
    printf("Monitor/snapshot state agrees:      %s\n", snapshot_mismatches == 0 ? "PASS" : "FAIL");
    printf("Overall integration: %s\n", gate_ok ? "PASS - C ring + product monitor boundary agree" : "FAIL");

    audio_engine_destroy(sess);
    telemetry_free_global_ring();
    printf("\nReflex kernel complete (real engine calls, CLI, seed, gates).\n");
    return gate_ok ? 0 : 1;
}
