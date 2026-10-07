/* Measure canonical turn-event and STT-stream encoding and decoding. */
#define _POSIX_C_SOURCE 200809L

#include "pb_min.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BENCH_AUDIO_BYTES 640u
#define BENCH_TEXT_BYTES 1024u
#define BENCH_WIRE_BYTES 32768u

static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int run_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_event_c event;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        if (pb_decode_turn_event(wire, wire_len, &event) != 0) return 0;
        *checksum = *checksum * 131u + (uint8_t)event.request_id[0] +
            (uint64_t)event.audio_len + (uint8_t)event.display_text[0] +
            (uint64_t)event.stages.pcm_started_at_ms;
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_fixture(
    const char *name,
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    size_t samples
) {
    uint64_t total_ns = 0;
    uint64_t checksum = 0;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_decode(
                wire, wire_len, rounds, &elapsed_ns, &sample_checksum))
            return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnDecode fixture=%s wire_bytes=%zu rounds=%zu samples=%zu "
        "ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        name, wire_len, rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_active_decode(
    const uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_event_c event;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || !request_id || request_id_len == 0u ||
        rounds == 0u || !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        if (pb_decode_turn_event_active(
                wire, wire_len, request_id, request_id_len, &event) != 0)
            return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)request_id[0] + (uint64_t)event.audio_len +
            (uint8_t)event.display_text[0] +
            (uint64_t)event.stages.pcm_started_at_ms;
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_active_fixture(
    const char *name,
    const uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len,
    size_t rounds,
    size_t samples
) {
    uint64_t generic_total_ns = 0u;
    uint64_t active_total_ns = 0u;
    uint64_t checksum = 0u;
    turn_event_c active_event = {0};
    turn_event_c bound_event = {0};
    size_t sample;
    if (!name || !request_id || request_id_len == 0u ||
        rounds == 0u || samples == 0u) return 0;
    if (pb_decode_turn_event_bound(
            wire, wire_len, request_id, request_id_len, &bound_event) != 0 ||
        pb_decode_turn_event_active(
            wire, wire_len, request_id, request_id_len, &active_event) != 0 ||
        memcmp(&active_event, &bound_event, sizeof(bound_event)) != 0)
        return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t generic_ns;
        uint64_t generic_checksum;
        uint64_t active_ns;
        uint64_t active_checksum;
        int active_first = (sample & 1u) != 0u;
        if (active_first) {
            if (!run_active_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &active_ns, &active_checksum) ||
                !run_decode(
                    wire, wire_len, rounds,
                    &generic_ns, &generic_checksum))
                return 0;
        } else {
            if (!run_decode(
                    wire, wire_len, rounds,
                    &generic_ns, &generic_checksum) ||
                !run_active_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &active_ns, &active_checksum))
                return 0;
        }
        if (generic_checksum != active_checksum) return 0;
        generic_total_ns += generic_ns;
        active_total_ns += active_ns;
        checksum += active_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnActiveDecode fixture=%s wire_bytes=%zu rounds=%zu "
        "samples=%zu generic_ns_per_call=%" PRIu64
        " active_ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        name, wire_len, rounds, samples,
        generic_total_ns / (rounds * samples),
        active_total_ns / (rounds * samples), checksum);
    return 1;
}

static int public_event_matches_active(
    const turn_event_c *public_event,
    const turn_event_c *active_event
) {
    turn_event_c expected;
    turn_event_c normalized;
    if (!public_event || !active_event) return 0;
    expected = *active_event;
    memset(expected.text, 0, sizeof(expected.text));
    memset(expected.speech_text, 0, sizeof(expected.speech_text));
    normalized = *public_event;
    normalized.current_tts_stage_wire = NULL;
    if (public_event->display_text_borrowed != 0u) {
        if (public_event->display_text_borrowed != 1u ||
            !public_event->display_text_view ||
            public_event->display_text_len != active_event->display_text_len ||
            memcmp(
                public_event->display_text_view, active_event->display_text,
                public_event->display_text_len) != 0)
            return 0;
        memcpy(
            normalized.display_text, active_event->display_text,
            sizeof(active_event->display_text));
        normalized.display_text_borrowed = 0u;
    }
    return memcmp(&normalized, &expected, sizeof(expected)) == 0;
}

static int run_public_decode(
    const uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_event_c event;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || !request_id || request_id_len == 0u ||
        rounds == 0u || !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        if (pb_decode_turn_event_public_active(
                wire, wire_len, request_id, request_id_len, &event) != 0)
            return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)request_id[0] + (uint64_t)event.audio_len +
            (uint8_t)(event.display_text_borrowed ?
                event.display_text_view[0] : event.display_text[0]) +
            (uint64_t)event.stages.pcm_started_at_ms;
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_public_fixture(
    const char *name,
    const uint8_t *wire,
    size_t wire_len,
    const char *request_id,
    size_t request_id_len,
    size_t rounds,
    size_t samples
) {
    uint64_t active_total_ns = 0u;
    uint64_t public_total_ns = 0u;
    uint64_t checksum = 0u;
    turn_event_c active_event = {0};
    turn_event_c public_event = {0};
    size_t sample;
    if (!name || !request_id || request_id_len == 0u ||
        rounds == 0u || samples == 0u) return 0;
    if (pb_decode_turn_event_active(
            wire, wire_len, request_id, request_id_len, &active_event) != 0 ||
        pb_decode_turn_event_public_active(
            wire, wire_len, request_id, request_id_len, &public_event) != 0 ||
        !public_event_matches_active(&public_event, &active_event))
        return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t active_ns;
        uint64_t active_checksum;
        uint64_t public_ns;
        uint64_t public_checksum;
        int public_first = (sample & 1u) != 0u;
        if (public_first) {
            if (!run_public_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &public_ns, &public_checksum) ||
                !run_active_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &active_ns, &active_checksum))
                return 0;
        } else {
            if (!run_active_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &active_ns, &active_checksum) ||
                !run_public_decode(
                    wire, wire_len, request_id, request_id_len, rounds,
                    &public_ns, &public_checksum))
                return 0;
        }
        if (active_checksum != public_checksum) return 0;
        active_total_ns += active_ns;
        public_total_ns += public_ns;
        checksum += public_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnPublicDecode fixture=%s wire_bytes=%zu rounds=%zu "
        "samples=%zu active_ns_per_call=%" PRIu64
        " public_ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        name, wire_len, rounds, samples,
        active_total_ns / (rounds * samples),
        public_total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_stt_stream_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    stt_stream_message_c message;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        if (pb_decode_stt_stream_message(wire, wire_len, &message) != 0)
            return 0;
        *checksum = *checksum * UINT64_C(131) +
            (uint8_t)message.type[0] + (uint64_t)message.audio_len +
            (uint64_t)(uint32_t)message.sample_rate +
            (uint64_t)message.timestamp_ms + message.audio[0];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_stt_stream_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    size_t samples
) {
    uint64_t total_ns = 0;
    uint64_t checksum = 0;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_stt_stream_decode(
                wire, wire_len, rounds, &elapsed_ns, &sample_checksum))
            return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkSTTStreamDecode wire_bytes=%zu rounds=%zu samples=%zu "
        "ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        wire_len, rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_stage_encode(
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    uint8_t wire[2048];
    turn_stage_timestamps_c stages;
    uint64_t started;
    size_t i;
    if (rounds == 0u || !elapsed_ns || !checksum) return 0;
    memset(&stages, 0, sizeof(stages));
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        int64_t base = INT64_C(1787864000000) +
            (int64_t)(i % 900000u);
        size_t length;
        stages.input.audio_committed_at_ms = base - 9;
        stages.input.stt_request_received_at_ms = base - 8;
        stages.input.stt_provider_request_started_at_ms = base - 7;
        stages.input.stt_provider_ready_at_ms = base - 6;
        stages.input.stt_transcript_published_at_ms = base - 5;
        stages.first_text_at_ms = base - 4;
        stages.tts_segment_emitted_at_ms = base - 3;
        stages.tts_request_received_at_ms = base - 2;
        stages.tts_provider_request_started_at_ms = base - 1;
        stages.tts_provider_ready_at_ms = base;
        stages.pcm_started_at_ms = base + 1;
        stages.pcm_first_chunk_at_ms = base + 2;
        length = pb_encode_turn_event_stages(
            wire, sizeof(wire), "req-bench", "pcm_started", "", &stages);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) + length + wire[length - 1u];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_stage_encode(size_t rounds, size_t samples) {
    uint64_t total_ns = 0;
    uint64_t checksum = 0;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_stage_encode(rounds, &elapsed_ns, &sample_checksum)) return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnStageEncode rounds=%zu samples=%zu ns_per_call=%" PRIu64
        " checksum=%" PRIu64 "\n",
        rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int verify_current_tts_stage_preparation(
    const turn_stage_timestamps_c *stages
) {
    uint8_t generic[2048];
    uint8_t prepared_event[2048];
    turn_stage_wire_c prepared;
    size_t generic_len;
    size_t prepared_len;
    if (!stages || pb_prepare_turn_stage_wire(&prepared, stages) != 0 ||
        !prepared.current_tts_template) return 0;
    generic_len = pb_encode_turn_event_stages(
        generic, sizeof(generic), "req-stage-prepare", "pcm_started", "",
        stages);
    prepared_len = pb_encode_turn_event_stage_wire(
        prepared_event, sizeof(prepared_event), "req-stage-prepare",
        "pcm_started", "", &prepared);
    return generic_len != 0u && prepared_len == generic_len &&
        memcmp(prepared_event, generic, generic_len) == 0;
}

static int run_current_tts_stage_preparation(
    const turn_stage_timestamps_c *stages,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_stage_wire_c prepared;
    uint64_t started;
    size_t i;
    if (!stages || rounds == 0u || !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        if (pb_prepare_turn_stage_wire(&prepared, stages) != 0 ||
            !prepared.current_tts_template) return 0;
        *checksum += prepared.len + prepared.data[(i * 17u) % prepared.len];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_current_tts_stage_preparation(
    size_t rounds,
    size_t samples
) {
    turn_stage_timestamps_c stages;
    uint64_t total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    memset(&stages, 0, sizeof(stages));
    stages.first_text_at_ms = INT64_C(1787864000010);
    stages.tts_segment_emitted_at_ms = INT64_C(1787864000011);
    stages.tts_request_received_at_ms = INT64_C(1787864000012);
    stages.tts_provider_request_started_at_ms = INT64_C(1787864000013);
    stages.tts_provider_ready_at_ms = INT64_C(1787864000014);
    stages.pcm_started_at_ms = INT64_C(1787864000015);
    if (rounds == 0u || samples == 0u ||
        !verify_current_tts_stage_preparation(&stages)) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_current_tts_stage_preparation(
                &stages, rounds, &elapsed_ns, &sample_checksum)) return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnCurrentTTSStagePrepare rounds=%zu samples=%zu "
        "ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_prepared_text_encode(
    const char *text,
    size_t text_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    uint8_t wire[BENCH_WIRE_BYTES];
    uint64_t started;
    size_t i;
    if (!text || text_len == 0u || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t length = pb_encode_turn_text_event_prepared(
            wire, sizeof(wire), "req-encode-text", sizeof("req-encode-text") - 1u,
            4, text, text_len, text, text_len, text, text_len,
            (int32_t)(i & 7u), 0);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) + length + wire[length - 1u];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_prepared_text_encode(
    const char *name,
    const char *text,
    size_t text_len,
    size_t rounds,
    size_t samples
) {
    uint64_t total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (!name || rounds == 0u || samples == 0u) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_prepared_text_encode(
                text, text_len, rounds, &elapsed_ns, &sample_checksum))
            return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnTextPreparedEncode fixture=%s text_bytes=%zu rounds=%zu "
        "samples=%zu ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        name, text_len, rounds, samples,
        total_ns / (rounds * samples), checksum);
    return 1;
}

static int verify_route_event_encode_equivalence(void) {
    static const char *const routes[] = {
        "answer",
        "escalate",
        "retrieve_then_escalate"
    };
    uint8_t generic[256];
    uint8_t prepared[256];
    size_t route_index;
    for (route_index = 0u;
         route_index < sizeof(routes) / sizeof(routes[0]);
         ++route_index) {
        const char *route = routes[route_index];
        size_t route_len = strlen(route);
        size_t generic_len = pb_encode_turn_event(
            generic, sizeof(generic), "req-route", "route", route);
        size_t capacity;
        if (generic_len == 0u) return 0;
        for (capacity = 0u; capacity <= generic_len + 1u; ++capacity) {
            size_t current_generic_len = pb_encode_turn_event(
                generic, capacity, "req-route", "route", route);
            size_t prepared_len = pb_encode_turn_route_event_prepared(
                prepared,
                capacity,
                "req-route",
                sizeof("req-route") - 1u,
                route,
                route_len);
            if (current_generic_len != prepared_len ||
                (current_generic_len != 0u &&
                 memcmp(generic, prepared, current_generic_len) != 0))
                return 0;
        }
    }
    return 1;
}

static int run_route_event_encode(
    size_t rounds,
    int use_prepared,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    static const char *const routes[] = {
        "answer",
        "escalate",
        "retrieve_then_escalate"
    };
    static const size_t route_lengths[] = {
        sizeof("answer") - 1u,
        sizeof("escalate") - 1u,
        sizeof("retrieve_then_escalate") - 1u
    };
    uint8_t wire[256];
    uint64_t started;
    size_t i;
    if (rounds == 0u || !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t route_index = i % (sizeof(routes) / sizeof(routes[0]));
        size_t length = use_prepared ?
            pb_encode_turn_route_event_prepared(
                wire,
                sizeof(wire),
                "req-route",
                sizeof("req-route") - 1u,
                routes[route_index],
                route_lengths[route_index]) :
            pb_encode_turn_event(
                wire, sizeof(wire), "req-route", "route", routes[route_index]);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) + length +
            wire[(i * 17u) % length];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_route_event_encode(size_t rounds, size_t samples) {
    uint64_t generic_total_ns = 0u;
    uint64_t prepared_total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (rounds == 0u || samples == 0u ||
        !verify_route_event_encode_equivalence()) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t generic_ns;
        uint64_t generic_checksum;
        uint64_t prepared_ns;
        uint64_t prepared_checksum;
        int first_prepared = (sample & 1u) != 0u;
        if (!run_route_event_encode(
                rounds,
                first_prepared,
                first_prepared ? &prepared_ns : &generic_ns,
                first_prepared ? &prepared_checksum : &generic_checksum) ||
            !run_route_event_encode(
                rounds,
                !first_prepared,
                first_prepared ? &generic_ns : &prepared_ns,
                first_prepared ? &generic_checksum : &prepared_checksum) ||
            generic_checksum != prepared_checksum)
            return 0;
        generic_total_ns += generic_ns;
        prepared_total_ns += prepared_ns;
        checksum += prepared_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnRoutePreparedEncode rounds=%zu samples=%zu "
        "generic_ns_per_call=%" PRIu64 " prepared_ns_per_call=%" PRIu64
        " checksum=%" PRIu64 "\n",
        rounds,
        samples,
        generic_total_ns / (rounds * samples),
        prepared_total_ns / (rounds * samples),
        checksum);
    return 1;
}

static int benchmark_turn_start_decode(
    const turn_start_c *input, int with_metadata, size_t rounds, size_t samples
) {
    static const char *const names[] = {"ordinary", "dnd", "initiative1", "initiative64", "campaign_create", "character_add",
        "encounter_advance", "encounter_damage", "encounter_condition"};
    turn_start_c fixture = *input, decoded;
    uint8_t wire[BENCH_WIRE_BYTES];
    size_t length, sample, i;
    uint64_t total_ns = 0, checksum = 0;
    if (with_metadata) {
        strcpy(fixture.metadata.interaction_profile, "dnd_app");
        strcpy(fixture.metadata.campaign_id, "campaign-bench");
        strcpy(fixture.metadata.knowledge_scope, "shared_rulebook");
        strcpy(fixture.metadata.retrieval_force, "true");
    }
    if (with_metadata == 2 || with_metadata == 3) {
        strcpy(fixture.metadata.encounter_id, "encounter-bench");
        fixture.dnd_initiative.campaign_version = 2;
        fixture.dnd_initiative.count = with_metadata == 2 ? 1u : DND_INITIATIVE_SELECTIONS_MAX;
        strcpy(fixture.dnd_initiative.operation_id, "saved-benchmark-roll");
        for (i = 0; i < fixture.dnd_initiative.count; ++i) {
            snprintf(fixture.dnd_initiative.selections[i].character_id,
                sizeof(fixture.dnd_initiative.selections[i].character_id), "pc-%zu", i);
            strcpy(fixture.dnd_initiative.selections[i].expression, "2d20kh1+5");
        }
    }
    if (with_metadata == 4 || with_metadata == 5) {
        dnd_campaign_request_c *request = &fixture.dnd_campaign;
        strcpy(request->operation_id, "saved-benchmark-setup");
        if (with_metadata == 4) {
            request->operation = DND_CAMPAIGN_CREATE;
            strcpy(request->data.campaign.name, "Waterdeep");
            strcpy(request->data.campaign.ruleset, "5e");
        } else {
            request->operation = DND_CAMPAIGN_ADD_CHARACTER;
            request->expected_version = 1;
            dnd_character_create_c *character = &request->data.character;
            strcpy(character->id, "aria");
            strcpy(character->name, "Aria");
            strcpy(character->species, "elf");
            strcpy(character->class_name, "rogue");
            character->kind = 1;
            character->level = 3;
            character->armor_class = 14;
            character->max_hp = 23;
            for (i = 0; i < 6u; ++i) character->abilities[i] = 10;
        }
    }
    if (with_metadata >= 6) {
        dnd_encounter_action_c *action = &fixture.dnd_encounter_action;
        strcpy(fixture.metadata.encounter_id, "battle");
        strcpy(action->operation_id, "saved-benchmark-action");
        action->expected_version = 2;
        action->operation = with_metadata == 6 ? DND_ACTION_ADVANCE : with_metadata == 7 ? DND_ACTION_DAMAGE : DND_ACTION_CONDITION_ADD;
        if (with_metadata != 6) strcpy(action->participant_id, "aria");
        if (with_metadata == 7) { action->amount = 7; action->damage_type = 4; }
        if (with_metadata == 8) action->condition = 11;
    }
    length = pb_encode_turn_start(wire, sizeof(wire), &fixture);
    if (!length || !rounds || !samples) return 0;
    memset(&decoded, 0xa5, sizeof(decoded));
    if (pb_decode_turn_start(wire, length, &decoded) != 0 ||
        strcmp(decoded.text, fixture.text) != 0 || decoded.text_len != fixture.text_len ||
        strcmp(decoded.metadata.campaign_id, fixture.metadata.campaign_id) != 0 ||
        decoded.dnd_initiative.count != fixture.dnd_initiative.count ||
        (fixture.dnd_initiative.count &&
         memcmp(&decoded.dnd_initiative, &fixture.dnd_initiative, sizeof(decoded.dnd_initiative))) ||
        decoded.dnd_campaign.operation != fixture.dnd_campaign.operation ||
        (fixture.dnd_campaign.operation &&
         memcmp(&decoded.dnd_campaign, &fixture.dnd_campaign, sizeof(decoded.dnd_campaign))) ||
        decoded.dnd_encounter_action.operation != fixture.dnd_encounter_action.operation ||
        (fixture.dnd_encounter_action.operation && memcmp(&decoded.dnd_encounter_action,
            &fixture.dnd_encounter_action, sizeof(decoded.dnd_encounter_action)))) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t started = monotonic_ns();
        for (i = 0; i < rounds; ++i) {
            if (pb_decode_turn_start(wire, length, &decoded) != 0) return 0;
            checksum = checksum * UINT64_C(131) + decoded.text_len +
                (uint8_t)decoded.metadata.campaign_id[0];
        }
        total_ns += monotonic_ns() - started;
    }
    printf("BenchmarkTurnStartDecode fixture=%s struct_bytes=%zu wire_bytes=%zu "
        "rounds=%zu samples=%zu ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        names[with_metadata], sizeof(decoded), length, rounds, samples,
        total_ns / (rounds * samples), checksum);
    return total_ns != 0;
}

static int run_turn_start_encode(
    const turn_start_c *turn,
    size_t rounds,
    int use_prepared,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    uint8_t wire[BENCH_WIRE_BYTES];
    uint64_t started;
    size_t i;
    if (!turn || rounds == 0u || !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t length = use_prepared ?
            pb_encode_turn_start_prepared(wire, sizeof(wire), turn) :
            pb_encode_turn_start(wire, sizeof(wire), turn);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) + length +
            wire[(i * 17u) % length];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int verify_turn_start_encode_equivalence(const turn_start_c *turn) {
    uint8_t generic[BENCH_WIRE_BYTES];
    uint8_t prepared[BENCH_WIRE_BYTES];
    size_t capacity;
    size_t generic_len;
    if (!turn) return 0;
    generic_len = pb_encode_turn_start(generic, sizeof(generic), turn);
    if (generic_len == 0u) return 0;
    for (capacity = 0u; capacity <= generic_len + 1u; ++capacity) {
        size_t current_generic_len = pb_encode_turn_start(
            generic, capacity, turn);
        size_t prepared_len = pb_encode_turn_start_prepared(
            prepared, capacity, turn);
        if (current_generic_len != prepared_len ||
            (current_generic_len != 0u &&
             memcmp(generic, prepared, current_generic_len) != 0))
            return 0;
    }
    return 1;
}

static int benchmark_turn_start_encode(
    const turn_start_c *turn,
    size_t rounds,
    size_t samples
) {
    uint64_t generic_total_ns = 0u;
    uint64_t prepared_total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (!turn || rounds == 0u || samples == 0u ||
        !verify_turn_start_encode_equivalence(turn)) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t generic_ns;
        uint64_t generic_checksum;
        uint64_t prepared_ns;
        uint64_t prepared_checksum;
        int first_prepared = (sample & 1u) != 0u;
        if (!run_turn_start_encode(
                turn, rounds, first_prepared,
                first_prepared ? &prepared_ns : &generic_ns,
                first_prepared ? &prepared_checksum : &generic_checksum) ||
            !run_turn_start_encode(
                turn, rounds, !first_prepared,
                first_prepared ? &generic_ns : &prepared_ns,
                first_prepared ? &generic_checksum : &prepared_checksum) ||
            generic_checksum != prepared_checksum)
            return 0;
        generic_total_ns += generic_ns;
        prepared_total_ns += prepared_ns;
        checksum += prepared_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnStartPreparedEncode text_bytes=%zu rounds=%zu "
        "samples=%zu generic_ns_per_call=%" PRIu64
        " prepared_ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        turn->text_len, rounds, samples,
        generic_total_ns / (rounds * samples),
        prepared_total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_pcm_chunk_encode(
    const uint8_t *audio,
    size_t audio_len,
    const turn_stage_wire_c *stages,
    size_t rounds,
    int use_prepared,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    uint8_t wire[BENCH_WIRE_BYTES];
    uint64_t started;
    size_t i;
    if (!audio || audio_len == 0u || !stages || rounds == 0u ||
        !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        int32_t sequence = (int32_t)(i & 127u);
        int32_t segment_index = (int32_t)((i >> 7u) & 7u);
        int is_final = (i & 31u) == 31u;
        size_t length = use_prepared ?
            pb_encode_turn_pcm_chunk_prepared(
                wire, sizeof(wire),
                "req-encode-pcm", sizeof("req-encode-pcm") - 1u,
                audio, audio_len, 24000, 1, 16,
                sequence, segment_index, is_final, stages) :
            pb_encode_turn_audio_event_stage_wire(
                wire, sizeof(wire), "req-encode-pcm", "pcm_chunk", "",
                audio, audio_len, 24000, 1, 16,
                sequence, segment_index, is_final, stages);
        if (length == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) + length + wire[length - 1u];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_pcm_chunk_encode(
    const uint8_t *audio,
    size_t audio_len,
    const turn_stage_wire_c *stages,
    size_t rounds,
    size_t samples
) {
    uint64_t generic_total_ns = 0u;
    uint64_t prepared_total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t generic_ns;
        uint64_t generic_checksum;
        uint64_t prepared_ns;
        uint64_t prepared_checksum;
        int first_prepared = (sample & 1u) != 0u;
        if (!run_pcm_chunk_encode(
                audio, audio_len, stages, rounds, first_prepared,
                first_prepared ? &prepared_ns : &generic_ns,
                first_prepared ? &prepared_checksum : &generic_checksum) ||
            !run_pcm_chunk_encode(
                audio, audio_len, stages, rounds, !first_prepared,
                first_prepared ? &generic_ns : &prepared_ns,
                first_prepared ? &generic_checksum : &prepared_checksum) ||
            generic_checksum != prepared_checksum) return 0;
        generic_total_ns += generic_ns;
        prepared_total_ns += prepared_ns;
        checksum += prepared_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnPCMPreparedEncode audio_bytes=%zu rounds=%zu samples=%zu "
        "generic_ns_per_call=%" PRIu64 " prepared_ns_per_call=%" PRIu64
        " checksum=%" PRIu64 "\n",
        audio_len, rounds, samples,
        generic_total_ns / (rounds * samples),
        prepared_total_ns / (rounds * samples), checksum);
    return 1;
}

static int run_current_pcm_suffix_encode(
    const turn_pcm_chunk_current_wire_c *wire,
    const turn_stage_wire_c *stages,
    size_t rounds,
    int use_admitted,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    uint8_t suffix[1024];
    uint64_t started;
    size_t i;
    if (!wire || !stages || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        size_t suffix_len;
        int32_t sequence = (int32_t)(i & 16383u);
        int is_final = (i & 31u) == 31u;
        const turn_stage_wire_c *frame_stages = i == 0u ? stages : NULL;
        size_t length = use_admitted ?
            pb_encode_turn_pcm_chunk_current_suffix_admitted(
                wire,
                suffix,
                sizeof(suffix),
                &suffix_len,
                sequence,
                is_final,
                frame_stages) :
            pb_encode_turn_pcm_chunk_current_suffix_prepared(
                wire,
                suffix,
                sizeof(suffix),
                &suffix_len,
                sequence,
                is_final,
                frame_stages);
        if (length == 0u || suffix_len == 0u) return 0;
        *checksum = *checksum * UINT64_C(131) +
            length + suffix_len + suffix[suffix_len - 1u];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_current_pcm_suffix_encode(
    const turn_pcm_chunk_current_wire_c *wire,
    const turn_stage_wire_c *stages,
    size_t rounds,
    size_t samples
) {
    uint64_t prepared_total_ns = 0u;
    uint64_t admitted_total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t prepared_ns;
        uint64_t prepared_checksum;
        uint64_t admitted_ns;
        uint64_t admitted_checksum;
        int first_admitted = (sample & 1u) != 0u;
        if (!run_current_pcm_suffix_encode(
                wire, stages, rounds, first_admitted,
                first_admitted ? &admitted_ns : &prepared_ns,
                first_admitted ? &admitted_checksum : &prepared_checksum) ||
            !run_current_pcm_suffix_encode(
                wire, stages, rounds, !first_admitted,
                first_admitted ? &prepared_ns : &admitted_ns,
                first_admitted ? &prepared_checksum : &admitted_checksum) ||
            prepared_checksum != admitted_checksum) return 0;
        prepared_total_ns += prepared_ns;
        admitted_total_ns += admitted_ns;
        checksum += admitted_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTurnCurrentPCMSuffixEncode rounds=%zu samples=%zu "
        "prepared_ns_per_call=%" PRIu64 " admitted_ns_per_call=%" PRIu64
        " checksum=%" PRIu64 "\n",
        rounds,
        samples,
        prepared_total_ns / (rounds * samples),
        admitted_total_ns / (rounds * samples),
        checksum);
    return 1;
}

static int run_tts_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_tts_segment_c segment;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        if (pb_decode_turn_tts_segment(wire, wire_len, &segment) != 0) return 0;
        *checksum = *checksum * UINT64_C(131) + segment.query_hash +
            (uint8_t)segment.request_id[0] + (uint8_t)segment.text[0];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int run_tts_view_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_tts_segment_view_c segment;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || rounds == 0u || !elapsed_ns || !checksum)
        return 0;
    *checksum = 0;
    started = monotonic_ns();
    for (i = 0; i < rounds; ++i) {
        if (pb_decode_turn_tts_segment_view(wire, wire_len, &segment) != 0)
            return 0;
        *checksum = *checksum * UINT64_C(131) + segment.query_hash +
            (uint8_t)segment.request_id[0] + (uint8_t)segment.text[0];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_tts_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    size_t samples
) {
    uint64_t total_ns = 0;
    uint64_t checksum = 0;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_tts_decode(
                wire, wire_len, rounds, &elapsed_ns, &sample_checksum))
            return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTTSSegmentDecode wire_bytes=%zu rounds=%zu samples=%zu "
        "ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        wire_len, rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int benchmark_tts_view_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    size_t samples
) {
    uint64_t total_ns = 0;
    uint64_t checksum = 0;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t elapsed_ns;
        uint64_t sample_checksum;
        if (!run_tts_view_decode(
                wire, wire_len, rounds, &elapsed_ns, &sample_checksum))
            return 0;
        total_ns += elapsed_ns;
        checksum += sample_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTTSSegmentViewDecode wire_bytes=%zu rounds=%zu samples=%zu "
        "ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        wire_len, rounds, samples, total_ns / (rounds * samples), checksum);
    return 1;
}

static int benchmark_tts_request_hash(
    const char *request_id,
    size_t request_id_len,
    uint32_t *hash_out
) {
    uint32_t hash = 2166136261u;
    size_t i;
    if (!request_id || request_id_len == 0u || !hash_out) return 0;
    for (i = 0u; i < request_id_len; ++i) {
        uint8_t c = (uint8_t)request_id[i];
        if (!((c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
              (c >= (uint8_t)'A' && c <= (uint8_t)'Z') ||
              (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
              c == (uint8_t)'-' || c == (uint8_t)'_' ||
              c == (uint8_t)'.' || c == (uint8_t)':')) return 0;
        hash ^= c;
        hash *= 16777619u;
    }
    *hash_out = hash;
    return 1;
}

static int run_tts_dispatch_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    int use_admitted,
    uint64_t *elapsed_ns,
    uint64_t *checksum
) {
    turn_tts_segment_view_c segment;
    uint64_t started;
    size_t i;
    if (!wire || wire_len == 0u || rounds == 0u ||
        !elapsed_ns || !checksum) return 0;
    *checksum = 0u;
    started = monotonic_ns();
    for (i = 0u; i < rounds; ++i) {
        uint32_t request_hash;
        if (use_admitted) {
            if (pb_decode_turn_tts_segment_view_admitted(
                    wire, wire_len, &segment, &request_hash) != 0) return 0;
        } else if (pb_decode_turn_tts_segment_view(
                       wire, wire_len, &segment) != 0 ||
                   !benchmark_tts_request_hash(
                       segment.request_id,
                       segment.request_id_len,
                       &request_hash)) {
            return 0;
        }
        *checksum = *checksum * UINT64_C(131) + segment.query_hash +
            request_hash + (uint8_t)segment.text[0];
    }
    *elapsed_ns = monotonic_ns() - started;
    return *elapsed_ns != 0u;
}

static int benchmark_tts_dispatch_decode(
    const uint8_t *wire,
    size_t wire_len,
    size_t rounds,
    size_t samples
) {
    uint64_t generic_total_ns = 0u;
    uint64_t admitted_total_ns = 0u;
    uint64_t checksum = 0u;
    size_t sample;
    if (rounds == 0u || samples == 0u) return 0;
    for (sample = 0u; sample < samples; ++sample) {
        uint64_t generic_ns;
        uint64_t generic_checksum;
        uint64_t admitted_ns;
        uint64_t admitted_checksum;
        int first_admitted = (sample & 1u) != 0u;
        if (!run_tts_dispatch_decode(
                wire, wire_len, rounds, first_admitted,
                first_admitted ? &admitted_ns : &generic_ns,
                first_admitted ? &admitted_checksum : &generic_checksum) ||
            !run_tts_dispatch_decode(
                wire, wire_len, rounds, !first_admitted,
                first_admitted ? &generic_ns : &admitted_ns,
                first_admitted ? &generic_checksum : &admitted_checksum) ||
            generic_checksum != admitted_checksum) return 0;
        generic_total_ns += generic_ns;
        admitted_total_ns += admitted_ns;
        checksum += admitted_checksum * (uint64_t)(sample + 1u);
    }
    printf(
        "BenchmarkTTSSegmentAdmittedViewDecode wire_bytes=%zu rounds=%zu "
        "samples=%zu generic_ns_per_call=%" PRIu64
        " admitted_ns_per_call=%" PRIu64 " checksum=%" PRIu64 "\n",
        wire_len, rounds, samples,
        generic_total_ns / (rounds * samples),
        admitted_total_ns / (rounds * samples), checksum);
    return 1;
}

static int tts_view_string_matches(
    const char *view,
    size_t view_len,
    const char *owned,
    size_t owned_len
) {
    if (!owned || view_len != owned_len) return 0;
    if (view_len == 0u) return owned[0] == '\0';
    return view && memcmp(view, owned, view_len) == 0;
}

static int tts_view_matches_owned(
    const turn_tts_segment_view_c *view,
    const turn_tts_segment_c *owned
) {
    if (!view || !owned ||
        !tts_view_string_matches(
            view->request_id, view->request_id_len,
            owned->request_id, owned->request_id_len) ||
        !tts_view_string_matches(
            view->user_id, view->user_id_len,
            owned->user_id, owned->user_id_len) ||
        !tts_view_string_matches(
            view->text, view->text_len, owned->text, owned->text_len) ||
        !tts_view_string_matches(
            view->voice_id, view->voice_id_len,
            owned->voice_id, owned->voice_id_len) ||
        !tts_view_string_matches(
            view->response_subject, view->response_subject_len,
            owned->response_subject, owned->response_subject_len)) return 0;
    return view->segment_index == owned->segment_index &&
        view->is_final == owned->is_final &&
        view->output_sample_rate == owned->output_sample_rate &&
        view->output_channels == owned->output_channels &&
        view->output_bit_depth == owned->output_bit_depth &&
        view->output_encoding == owned->output_encoding &&
        view->query_hash == owned->query_hash &&
        view->first_text_at_ms == owned->first_text_at_ms &&
        view->segment_emitted_at_ms == owned->segment_emitted_at_ms &&
        view->stream_finality_deferred == owned->stream_finality_deferred &&
        view->input_stages.audio_committed_at_ms ==
            owned->input_stages.audio_committed_at_ms &&
        view->input_stages.stt_request_received_at_ms ==
            owned->input_stages.stt_request_received_at_ms &&
        view->input_stages.stt_provider_request_started_at_ms ==
            owned->input_stages.stt_provider_request_started_at_ms &&
        view->input_stages.stt_provider_ready_at_ms ==
            owned->input_stages.stt_provider_ready_at_ms &&
        view->input_stages.stt_transcript_published_at_ms ==
            owned->input_stages.stt_transcript_published_at_ms;
}

int main(int argc, char **argv) {
    printf("BenchmarkTurnStructSize turn_start=%zu turn_event=%zu\n", sizeof(turn_start_c), sizeof(turn_event_c));
    static uint8_t audio[BENCH_AUDIO_BYTES];
    static char text[BENCH_TEXT_BYTES + 1u];
    static uint8_t audio_wire[BENCH_WIRE_BYTES];
    static uint8_t text_wire[BENCH_WIRE_BYTES];
    static uint8_t lifecycle_wire[BENCH_WIRE_BYTES];
    static uint8_t stage_wire[BENCH_WIRE_BYTES];
    static uint8_t current_stage_start_wire[BENCH_WIRE_BYTES];
    static uint8_t current_stage_chunk_wire[BENCH_WIRE_BYTES];
    static uint8_t stream_wire[BENCH_AUDIO_BYTES + 64u];
    static uint8_t tts_wire[BENCH_WIRE_BYTES];
    int verify = argc > 1 && strcmp(argv[1], "--verify") == 0;
    size_t rounds = verify ? 200u : 200000u;
    size_t samples = verify ? 2u : 6u;
    size_t audio_wire_len;
    size_t text_wire_len;
    size_t lifecycle_wire_len;
    size_t stage_wire_len;
    size_t current_stage_start_wire_len;
    size_t current_stage_chunk_wire_len;
    size_t stream_wire_len;
    size_t tts_wire_len;
    turn_event_c decoded;
    turn_start_c start_input;
    turn_stage_timestamps_c stages;
    turn_stage_timestamps_c current_stages;
    turn_stage_wire_c pcm_prepared;
    turn_stage_wire_c current_prepared;
    turn_pcm_chunk_current_wire_c current_pcm_wire;
    stt_stream_message_c stream_decoded;
    turn_tts_segment_c tts_input;
    turn_tts_segment_c tts_decoded;
    turn_tts_segment_view_c tts_view;
    size_t i;
    for (i = 0; i < sizeof(audio); ++i) audio[i] = (uint8_t)(i * 131u + 17u);
    memset(text, 'x', BENCH_TEXT_BYTES);
    text[BENCH_TEXT_BYTES] = '\0';
    memset(&start_input, 0, sizeof(start_input));
    memcpy(
        start_input.request_id,
        "req-encode-start",
        sizeof("req-encode-start"));
    start_input.request_id_len = sizeof("req-encode-start") - 1u;
    memcpy(start_input.user_id, "user-bench", sizeof("user-bench"));
    start_input.user_id_len = sizeof("user-bench") - 1u;
    memcpy(start_input.session_id, "session-bench", sizeof("session-bench"));
    start_input.session_id_len = sizeof("session-bench") - 1u;
    memcpy(start_input.text, text, sizeof(text));
    start_input.text_len = BENCH_TEXT_BYTES;
    memcpy(
        start_input.response_subject,
        "ai.turn.events.req-encode-start",
        sizeof("ai.turn.events.req-encode-start"));
    start_input.response_subject_len =
        sizeof("ai.turn.events.req-encode-start") - 1u;
    start_input.enable_rag = 1;
    start_input.enable_tts = 1;
    audio_wire_len = pb_encode_turn_audio_event(
        audio_wire, sizeof(audio_wire), "req-decode-audio", "pcm_chunk", "",
        audio, sizeof(audio), 24000, 1, 16, 7, 2, 0);
    text_wire_len = pb_encode_turn_text_event(
        text_wire, sizeof(text_wire), "req-decode-text", "text_delta", text,
        text, text, 2, 0);
    lifecycle_wire_len = pb_encode_turn_event(
        lifecycle_wire,
        sizeof(lifecycle_wire),
        "req-decode-lifecycle",
        "completed",
        "");
    memset(&current_stages, 0, sizeof(current_stages));
    current_stages.first_text_at_ms = INT64_C(1787864000010);
    current_stages.tts_segment_emitted_at_ms = INT64_C(1787864000011);
    current_stages.tts_request_received_at_ms = INT64_C(1787864000012);
    current_stages.tts_provider_request_started_at_ms = INT64_C(1787864000013);
    current_stages.tts_provider_ready_at_ms = INT64_C(1787864000014);
    current_stages.pcm_started_at_ms = INT64_C(1787864000015);
    if (pb_prepare_turn_stage_wire(&current_prepared, &current_stages) != 0 ||
        !current_prepared.current_tts_template) {
        fprintf(stderr, "FAIL prepare current PCM benchmark stages\n");
        return 1;
    }
    current_stage_start_wire_len = pb_encode_turn_event_stage_wire(
        current_stage_start_wire, sizeof(current_stage_start_wire),
        "req-current-stage", "pcm_started", "", &current_prepared);
    if (pb_complete_turn_stage_wire(
            &current_prepared, INT64_C(1787864000016)) != 0) {
        fprintf(stderr, "FAIL complete current PCM benchmark stages\n");
        return 1;
    }
    if (pb_prepare_turn_pcm_chunk_current_wire(
            &current_pcm_wire,
            "req-current-stage",
            sizeof("req-current-stage") - 1u,
            960u,
            2) != 0) {
        fprintf(stderr, "FAIL prepare current PCM benchmark wire\n");
        return 1;
    }
    current_stage_chunk_wire_len = pb_encode_turn_audio_event_stage_wire(
        current_stage_chunk_wire, sizeof(current_stage_chunk_wire),
        "req-current-stage", "pcm_chunk", "", audio, sizeof(audio),
        24000, 1, 16, 0, 0, 0, &current_prepared);
    memset(&stages, 0, sizeof(stages));
    stages.input.audio_committed_at_ms = INT64_C(1787863999991);
    stages.input.stt_request_received_at_ms = INT64_C(1787863999992);
    stages.input.stt_provider_request_started_at_ms = INT64_C(1787863999993);
    stages.input.stt_provider_ready_at_ms = INT64_C(1787863999994);
    stages.input.stt_transcript_published_at_ms = INT64_C(1787863999995);
    stages.first_text_at_ms = INT64_C(1787863999996);
    stages.tts_segment_emitted_at_ms = INT64_C(1787863999997);
    stages.tts_request_received_at_ms = INT64_C(1787863999998);
    stages.tts_provider_request_started_at_ms = INT64_C(1787863999999);
    stages.tts_provider_ready_at_ms = INT64_C(1787864000000);
    stages.pcm_started_at_ms = INT64_C(1787864000001);
    stages.pcm_first_chunk_at_ms = 0;
    if (pb_prepare_turn_stage_wire(&pcm_prepared, &stages) != 0 ||
        pb_complete_turn_stage_wire(
            &pcm_prepared, INT64_C(1787864000002)) != 0) {
        fprintf(stderr, "FAIL prepare PCM benchmark stages\n");
        return 1;
    }
    stages.pcm_first_chunk_at_ms = INT64_C(1787864000002);
    stage_wire_len = pb_encode_turn_event_stages(
        stage_wire, sizeof(stage_wire), "req-decode-stage", "pcm_started", "",
        &stages);
    stream_wire_len = pb_encode_stt_stream_message_at(
        stream_wire,
        sizeof(stream_wire),
        "chunk",
        audio,
        sizeof(audio),
        16000,
        1,
        16,
        INT64_C(1787864000000));
    memset(&tts_input, 0, sizeof(tts_input));
    memcpy(tts_input.request_id, "req-decode-tts", sizeof("req-decode-tts"));
    memset(tts_input.text, 'x', 128u);
    tts_input.text[128] = '\0';
    memcpy(tts_input.voice_id, "orpheus", sizeof("orpheus"));
    memcpy(
        tts_input.response_subject,
        "ai.turn.events.req-decode-tts",
        sizeof("ai.turn.events.req-decode-tts"));
    tts_input.segment_index = 2;
    tts_input.is_final = 1;
    tts_input.output_sample_rate = 24000;
    tts_input.output_channels = 1;
    tts_input.output_bit_depth = 16;
    tts_input.output_encoding = 1;
    tts_input.query_hash = UINT32_C(0x12345678);
    tts_input.input_stages.audio_committed_at_ms = INT64_C(1787863999995);
    tts_input.input_stages.stt_request_received_at_ms = INT64_C(1787863999996);
    tts_input.input_stages.stt_provider_request_started_at_ms =
        INT64_C(1787863999997);
    tts_input.input_stages.stt_provider_ready_at_ms = INT64_C(1787863999998);
    tts_input.input_stages.stt_transcript_published_at_ms =
        INT64_C(1787863999999);
    tts_input.first_text_at_ms = INT64_C(1787864000000);
    tts_input.segment_emitted_at_ms = INT64_C(1787864000001);
    tts_wire_len = pb_encode_turn_tts_segment(
        tts_wire, sizeof(tts_wire), &tts_input);
    if (argc > 1 && strcmp(argv[1], "--tts-admission") == 0) {
        return benchmark_tts_dispatch_decode(
            tts_wire, tts_wire_len, rounds * 5u, samples) ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "--public-events") == 0) {
        if (audio_wire_len == 0u || text_wire_len == 0u ||
            stage_wire_len == 0u ||
            !benchmark_public_fixture(
                "pcm640", audio_wire, audio_wire_len,
                "req-decode-audio", sizeof("req-decode-audio") - 1u,
                rounds, samples) ||
            !benchmark_public_fixture(
                "text1024x3", text_wire, text_wire_len,
                "req-decode-text", sizeof("req-decode-text") - 1u,
                rounds, samples) ||
            !benchmark_public_fixture(
                "stages12", stage_wire, stage_wire_len,
                "req-decode-stage", sizeof("req-decode-stage") - 1u,
                rounds * 5u, samples)) {
            fprintf(stderr, "FAIL public turn decode benchmark\n");
            return 1;
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--public-lifecycle") == 0) {
        if (lifecycle_wire_len == 0u ||
            !benchmark_public_fixture(
                "lifecycle", lifecycle_wire, lifecycle_wire_len,
                "req-decode-lifecycle",
                sizeof("req-decode-lifecycle") - 1u,
                rounds * 5u, samples)) {
            fprintf(stderr, "FAIL public lifecycle decode benchmark\n");
            return 1;
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--public-current-stages") == 0) {
        if (current_stage_start_wire_len == 0u ||
            current_stage_chunk_wire_len == 0u ||
            !benchmark_public_fixture(
                "current-stage-start", current_stage_start_wire,
                current_stage_start_wire_len, "req-current-stage",
                sizeof("req-current-stage") - 1u,
                rounds * 5u, samples) ||
            !benchmark_public_fixture(
                "current-stage-chunk", current_stage_chunk_wire,
                current_stage_chunk_wire_len, "req-current-stage",
                sizeof("req-current-stage") - 1u,
                rounds * 5u, samples)) {
            fprintf(stderr, "FAIL current stage public decode benchmark\n");
            return 1;
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--stage-preparation") == 0) {
        return benchmark_current_tts_stage_preparation(
            rounds * 5u, samples) ? 0 : 1;
    }
    if (audio_wire_len == 0u || text_wire_len == 0u || stage_wire_len == 0u ||
        current_stage_start_wire_len == 0u ||
        current_stage_chunk_wire_len == 0u || stream_wire_len == 0u ||
        tts_wire_len == 0u ||
        pb_decode_turn_event(audio_wire, audio_wire_len, &decoded) != 0 ||
        strcmp(decoded.request_id, "req-decode-audio") != 0 ||
        decoded.audio_len != sizeof(audio) || decoded.sample_rate != 24000 ||
        decoded.channels != 1 || decoded.bit_depth != 16 ||
        decoded.sequence != 7 || decoded.segment_index != 2 ||
        decoded.audio_encoding != 1 ||
        memcmp(decoded.audio, audio, sizeof(audio)) != 0 ||
        pb_decode_turn_event(text_wire, text_wire_len, &decoded) != 0 ||
        strcmp(decoded.request_id, "req-decode-text") != 0 ||
        strcmp(decoded.text, text) != 0 || strcmp(decoded.speech_text, text) != 0 ||
        strcmp(decoded.display_text, text) != 0 || decoded.segment_index != 2 ||
        decoded.is_final != 0 ||
        pb_decode_turn_event(stage_wire, stage_wire_len, &decoded) != 0 ||
        strcmp(decoded.request_id, "req-decode-stage") != 0 ||
        decoded.stages.input.audio_committed_at_ms !=
            INT64_C(1787863999991) ||
        decoded.stages.tts_request_received_at_ms !=
            INT64_C(1787863999998) ||
        decoded.stages.pcm_started_at_ms != INT64_C(1787864000001) ||
        decoded.stages.pcm_first_chunk_at_ms != INT64_C(1787864000002) ||
        pb_decode_stt_stream_message(
            stream_wire, stream_wire_len, &stream_decoded) != 0 ||
        strcmp(stream_decoded.type, "chunk") != 0 ||
        stream_decoded.audio_len != sizeof(audio) ||
        memcmp(stream_decoded.audio, audio, sizeof(audio)) != 0 ||
        stream_decoded.sample_rate != 16000 || stream_decoded.channels != 1 ||
        stream_decoded.bit_depth != 16 ||
        stream_decoded.timestamp_ms != INT64_C(1787864000000) ||
        pb_decode_turn_tts_segment(tts_wire, tts_wire_len, &tts_decoded) != 0 ||
        pb_decode_turn_tts_segment_view(tts_wire, tts_wire_len, &tts_view) != 0 ||
        !tts_view_matches_owned(&tts_view, &tts_decoded) ||
        (const uint8_t *)tts_view.request_id < tts_wire ||
        (const uint8_t *)tts_view.request_id >= tts_wire + tts_wire_len ||
        (const uint8_t *)tts_view.text < tts_wire ||
        (const uint8_t *)tts_view.text >= tts_wire + tts_wire_len ||
        strcmp(tts_decoded.request_id, "req-decode-tts") != 0 ||
        tts_decoded.request_id_len != strlen(tts_decoded.request_id) ||
        strcmp(tts_decoded.text, tts_input.text) != 0 ||
        tts_decoded.text_len != strlen(tts_decoded.text) ||
        tts_decoded.user_id_len != strlen(tts_decoded.user_id) ||
        tts_decoded.voice_id_len != strlen(tts_decoded.voice_id) ||
        tts_decoded.response_subject_len != strlen(tts_decoded.response_subject) ||
        tts_decoded.segment_index != 2 || !tts_decoded.is_final ||
        tts_decoded.output_sample_rate != 24000 ||
        tts_decoded.output_channels != 1 ||
        tts_decoded.output_bit_depth != 16 ||
        tts_decoded.output_encoding != 1 ||
        tts_decoded.query_hash != UINT32_C(0x12345678) ||
        tts_decoded.input_stages.audio_committed_at_ms !=
            INT64_C(1787863999995) ||
        tts_decoded.input_stages.stt_request_received_at_ms !=
            INT64_C(1787863999996) ||
        tts_decoded.input_stages.stt_provider_request_started_at_ms !=
            INT64_C(1787863999997) ||
        tts_decoded.input_stages.stt_provider_ready_at_ms !=
            INT64_C(1787863999998) ||
        tts_decoded.input_stages.stt_transcript_published_at_ms !=
            INT64_C(1787863999999) ||
        !benchmark_fixture(
            "pcm640", audio_wire, audio_wire_len, rounds, samples) ||
        !benchmark_fixture(
            "text1024x3", text_wire, text_wire_len, rounds, samples) ||
        !benchmark_fixture(
            "stages12", stage_wire, stage_wire_len,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_active_fixture(
            "pcm640", audio_wire, audio_wire_len,
            "req-decode-audio", sizeof("req-decode-audio") - 1u,
            rounds, samples) ||
        !benchmark_active_fixture(
            "text1024x3", text_wire, text_wire_len,
            "req-decode-text", sizeof("req-decode-text") - 1u,
            rounds, samples) ||
        !benchmark_active_fixture(
            "stages12", stage_wire, stage_wire_len,
            "req-decode-stage", sizeof("req-decode-stage") - 1u,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_public_fixture(
            "pcm640", audio_wire, audio_wire_len,
            "req-decode-audio", sizeof("req-decode-audio") - 1u,
            rounds, samples) ||
        !benchmark_public_fixture(
            "text1024x3", text_wire, text_wire_len,
            "req-decode-text", sizeof("req-decode-text") - 1u,
            rounds, samples) ||
        !benchmark_public_fixture(
            "stages12", stage_wire, stage_wire_len,
            "req-decode-stage", sizeof("req-decode-stage") - 1u,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_public_fixture(
            "current-stage-start", current_stage_start_wire,
            current_stage_start_wire_len, "req-current-stage",
            sizeof("req-current-stage") - 1u,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_public_fixture(
            "current-stage-chunk", current_stage_chunk_wire,
            current_stage_chunk_wire_len, "req-current-stage",
            sizeof("req-current-stage") - 1u,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_prepared_text_encode(
            "delta64", text, 64u,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_prepared_text_encode(
            "final1024", text, BENCH_TEXT_BYTES,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_route_event_encode(
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_turn_start_encode(
            &start_input, verify ? rounds : rounds * 5u, samples) ||
        !benchmark_turn_start_decode(&start_input, 0, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 1, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 2, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 3, verify ? rounds : rounds / 16u, samples) ||
        !benchmark_turn_start_decode(&start_input, 4, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 5, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 6, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 7, rounds, samples) ||
        !benchmark_turn_start_decode(&start_input, 8, rounds, samples) ||
        !benchmark_pcm_chunk_encode(
            audio, sizeof(audio), &pcm_prepared,
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_current_pcm_suffix_encode(
            &current_pcm_wire,
            &current_prepared,
            verify ? rounds : rounds * 5u,
            samples) ||
        !benchmark_current_tts_stage_preparation(
            verify ? rounds : rounds * 5u, samples) ||
        !benchmark_stage_encode(verify ? rounds : rounds * 5u, samples) ||
        !benchmark_stt_stream_decode(
            stream_wire, stream_wire_len, rounds, samples) ||
        !benchmark_tts_decode(
            tts_wire, tts_wire_len, verify ? rounds : rounds * 5u, samples) ||
        !benchmark_tts_view_decode(
            tts_wire, tts_wire_len, verify ? rounds : rounds * 5u, samples) ||
        !benchmark_tts_dispatch_decode(
            tts_wire, tts_wire_len,
            verify ? rounds : rounds * 5u, samples)) {
        fprintf(stderr, "FAIL turn decode benchmark\n");
        return 1;
    }
    return 0;
}
