#include "product_analytics.h"
#include "ia_transport.h"
#include "cmp_json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    voice_analytics_turn observation = {0};
    char wire[IA_WIRE_CAP];
    uint8_t otlp[IA_OTLP_CAP];
    size_t length;
    cmp_json_object object;
    if (size) memcpy(&observation, data, size < sizeof(observation) ? size : sizeof(observation));
    /* Test hostile snapshot fields, including missing string terminators.
     * Successful admission is also exercised independently of random layout. */
    (void)ia_otlp_ack(data, size);
    if (!voice_analytics_wire(&observation, 1700000000123LL, wire, sizeof(wire))) {
        if (!cmp_json_object_parse(wire, &object) ||
            ia_otlp_encode("c-webtransport-gateway", wire, 1700000000123LL, otlp, sizeof(otlp), &length) ||
            !length || length > sizeof(otlp)) abort();
    }
    turn_start_c turn = {0};
    if (size) memcpy(&turn.metadata, data, size < sizeof(turn.metadata) ? size : sizeof(turn.metadata));
    strcpy(turn.user_id, "user-a");
    strcpy(turn.request_id, "fuzz-turn");
    strcpy(turn.metadata.product_session_id, "fuzz-browser");
    if (size && (data[0] & 1u)) turn.metadata.campaign_id[0] = '\0';
    voice_analytics_begin(&observation, &turn, 1, 1, 1000000000u);
    voice_analytics_audio(&observation, 1500000000u);
    voice_analytics_finish(NULL, &observation, VOICE_ANALYTICS_COMPLETED, 2000000000u);
    (void)voice_analytics_wire(&observation, 1700000000123LL, wire, sizeof(wire));
    return 0;
}
