/* Security and edge-case tests for the pure-C WebTransport protocol core. */

#include "gateway_webtransport_core.h"
#include "turn_response.h"
#include "cmp_turn_metadata_policy.h"
#include "cmp_json.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int parse_text(const char *json, gw_wt_turn_request *request) {
    return gw_wt_turn_request_parse((const uint8_t *)json, strlen(json), request);
}

static int turn_requests_equal(
    const gw_wt_turn_request *left,
    const gw_wt_turn_request *right
) {
    return left && right &&
        strcmp(left->request_id, right->request_id) == 0 &&
        strcmp(left->session_id, right->session_id) == 0 &&
        strcmp(left->text, right->text) == 0 &&
        strcmp(left->identity_token, right->identity_token) == 0 &&
        left->enable_rag == right->enable_rag &&
        left->enable_tts == right->enable_tts &&
        left->audio_first == right->audio_first &&
        left->audio_stream == right->audio_stream &&
        memcmp(&left->metadata, &right->metadata, sizeof(left->metadata)) == 0 &&
        strcmp(left->meta_budget_ms, right->meta_budget_ms) == 0 &&
        strcmp(left->meta_deadline_unix_ms, right->meta_deadline_unix_ms) == 0;
}

static int count_metadata_field(const char *key, const char *value, void *user) {
    int *count = user;
    if (!key || !value || !key[0] || !value[0]) return -1;
    (*count)++;
    return 0;
}

static void test_loop_page_admission(void) {
    static const char *const kernels[] = {"wasm-simd", "scalar", "recorded_pcm16", "live_c_worklet"};
    static const char conversation[] =
        "{\"request_id\":\"loop-turn\",\"identity_token\":\"vat1.payload.signature\","
        "\"text\":\"\",\"enable_tts\":true,\"metadata\":{"
        "\"interaction_profile\":\"realtime_voice\",\"client_surface\":\"loop\","
        "\"product\":\"loop\",\"turn_kind\":\"voice\","
        "\"client_transport\":\"webtransport-turn-stream\","
        "\"input_mode\":\"webtransport_audio\","
        "\"client_audio_datagram_protocol\":\"dtvp1\","
        "\"client_audio_kernel\":\"live_c_worklet\",\"client_audio_packet_ms\":\"20\"}}";
    gw_wt_turn_request request;
    gw_wt_audio_reorder reorder;
    gw_wt_audio_delivery delivery;
    size_t kernel;
    uint32_t sequence;
    enum { utterance_frames = 8u };
    for (kernel = 0; kernel < sizeof(kernels) / sizeof(kernels[0]); ++kernel) {
        char json[1024];
        int length = snprintf(json, sizeof(json),
            "{\"request_id\":\"loop-turn\",\"identity_token\":\"vat1.payload.signature\","
            "\"text\":\"\",\"enable_tts\":true,\"metadata\":{"
            "\"interaction_profile\":\"realtime_voice\",\"client_surface\":\"loop\","
            "\"product\":\"loop\",\"turn_kind\":\"voice\","
            "\"client_transport\":\"webtransport-turn-stream\","
            "\"input_mode\":\"webtransport_audio\","
            "\"client_audio_datagram_protocol\":\"dtvp1\","
            "\"client_audio_kernel\":\"%s\",\"client_audio_packet_ms\":\"20\"}}",
            kernels[kernel]);
        expect(length > 0 && (size_t)length < sizeof(json) &&
                   parse_text(json, &request) == GW_WT_OK && request.audio_first &&
                   request.text[0] == '\0' && request.enable_tts,
               "Loop page voice payload is admitted as audio-first");
    }
    expect(parse_text(conversation, &request) == GW_WT_OK && request.audio_first &&
               request.text[0] == '\0' && request.enable_tts,
           "live conversation body with client_audio_kernel live_c_worklet is not malformed");
    memset(&reorder, 0, sizeof(reorder));
    memset(&delivery, 0, sizeof(delivery));
    for (sequence = 0; sequence < utterance_frames; ++sequence) {
        uint8_t packet[1u + GW_WT_AUDIO_HEADER_BYTES + 640u];
        gw_wt_datagram datagram;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        memset(packet, (int)(0x20u + sequence), sizeof(packet));
        packet[0] = 0x01;
        memcpy(packet + 1u, GW_WT_AUDIO_PREFIX, sizeof(GW_WT_AUDIO_PREFIX) - 1u);
        packet[6] = (uint8_t)(sequence >> 24);
        packet[7] = (uint8_t)(sequence >> 16);
        packet[8] = (uint8_t)(sequence >> 8);
        packet[9] = (uint8_t)sequence;
        expect(gw_wt_datagram_parse(packet, sizeof(packet), &datagram) == GW_WT_OK &&
                   datagram.is_audio && datagram.payload_len == 640u &&
                   gw_wt_audio_reorder_insert(&reorder, sequence, datagram.payload, datagram.payload_len) ==
                       GW_WT_AUDIO_INSERTED,
               "admitted session accepts one 640-byte microphone frame");
        expect(gw_wt_audio_reorder_peek(&reorder, &payload, &payload_len) && payload_len == 640u &&
                   gw_wt_audio_reorder_pop(&reorder) &&
                   gw_wt_audio_delivery_record(&delivery, 1, payload_len) == GW_WT_OK,
               "contiguous microphone audio is forwarded, not left idle");
    }
    expect(gw_wt_audio_reorder_commit_state(&reorder, utterance_frames, utterance_frames * 640u) ==
               GW_WT_AUDIO_COMMIT_READY &&
               gw_wt_audio_delivery_matches(&delivery, utterance_frames, utterance_frames * 640u) &&
               gw_wt_audio_reorder_commit_state(&reorder, 0u, 0u) == GW_WT_AUDIO_COMMIT_MISMATCH,
           "end-of-input commit matches delivered audio and rejects an empty stream");
    if (failures == 0) {
        printf("Loop page voice payload is not malformed\n");
        printf("end-of-input commit accepted 640-byte frames and rejected an empty idle stream\n");
    }
}

static void test_metadata_admission(void) {
    static const struct { const char *metadata; int accepted; } cases[] = {
        {"{\"input_mode\":\"text\",\"client_surface\":\"companions\"}", 1},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"scene_id\":\"hall\"}", 1},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"scene_id\":\"hall\","
         "\"knowledge_scope\":\"shared_rulebook\",\"retrieval_force\":\"true\"}", 1},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"scene_id\":\"hall\","
         "\"retrieval_force\":\"true\"}", 0},
        {"{\"interaction_profile\":\"dnd_app\",\"scene_id\":\"hall\"}", 0},
        {"{\"interaction_profile\":\"text_chat\",\"campaign_id\":\"table\",\"scene_id\":\"hall\"}", 0},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"scene_id\":\"../hall\"}", 0},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"campaign-a\","
         "\"knowledge_scope\":\"shared_rulebook\",\"retrieval_force\":\"true\"}", 1},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"campaign-a\","
         "\"knowledge_scope\":\"character_memory\",\"character_id\":\"character-a\"}", 1},
        {"{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"campaign-a\","
         "\"knowledge_scope\":\"character_memory\"}", 0},
        {"{\"interaction_profile\":\"dnd_app\",\"knowledge_scope\":\"campaign_canon\"}", 0},
        {"{\"interaction_profile\":\"text_chat\",\"retrieval_force\":\"true\"}", 0},
        {"{\"premium\":\"true\"}", 0},
        {"{\"user_id\":\"another-owner\"}", 0},
        {"{\"governed_user_id\":\"another-owner\"}", 0},
        {"{\"knowledge_scope\":{\"scope\":\"shared_rulebook\"}}", 0},
        {"{\"client_surface\":\"first\",\"client_surface\":\"second\"}", 0},
        {"{\"audio_group_session\":\"true\"}", 0},
        {"{\"audio_group_session\":\"true\",\"audio_participant_id\":\"player-a\","
         "\"audio_participant_label\":\"Mira 🐉\"}", 1},
        {"{\"turn_budget_ms\":\"45000\",\"turn_deadline_unix_ms\":\"1788690000000\"}", 1},
        {"{\"turn_budget_ms\":\"600001\"}", 0},
        {"{\"turn_budget_ms\":45000}", 0},
        {"{\"client_audio_datagram_protocol\":\"dtvp1\",\"client_audio_kernel\":\"wasm-simd\","
         "\"client_audio_packet_ms\":\"20\"}", 1},
        {"{\"client_audio_kernel\":\"scalar\"}", 1},
        {"{\"client_audio_kernel\":\"recorded_pcm16\"}", 1},
        {"{\"product\":\"loop\"}", 1},
        {"{\"client_audio_datagram_protocol\":\"other\"}", 0},
        {"{\"client_audio_kernel\":\"unknown\"}", 0},
        {"{\"client_audio_packet_ms\":\"200\"}", 0},
        {"{\"client_transport\":\"websocket\"}", 0},
        {"{\"audio_participant_label\":\"broken\\u0000label\"}", 0},
    };
    gw_wt_turn_request request;
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char json[4096];
        cmp_json_object object;
        int visited = 0;
        cmp_turn_metadata_policy policy;
        int length = snprintf(json, sizeof(json),
            "{\"request_id\":\"metadata-turn\",\"text\":\"Which rule applies?\","
            "\"identity_token\":\"vat1.payload.signature\","
            "\"client_transport\":\"webtransport-turn-stream\",\"metadata\":%s}", cases[i].metadata);
        int product_ok = length > 0 && (size_t)length < sizeof(json) &&
            cmp_json_object_parse(json, &object) &&
            cmp_turn_metadata_visit(&object, count_metadata_field, &visited, &policy, NULL, NULL, NULL) == 0;
        int transport_ok = parse_text(json, &request) == GW_WT_OK;
        expect(product_ok == cases[i].accepted && transport_ok == product_ok,
            "WebTransport and product use the same metadata admission policy");
        expect(product_ok ? visited > 0 : visited == 0,
            "rejected metadata never reaches the admitted-field callback");
    }
    expect(parse_text(
        "{\"request_id\":\"metadata-audio\",\"text\":\"\",\"identity_token\":\"vat1.payload.signature\","
        "\"metadata\":{\"input_mode\":\"webtransport_audio\",\"client_audio_datagram_protocol\":\"dtvp1\","
        "\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"campaign-a\",\"character_id\":\"character-a\","
        "\"scene_id\":\"hall\","
        "\"knowledge_scope\":\"character_memory\",\"turn_budget_ms\":\"45000\","
        "\"turn_deadline_unix_ms\":\"1788690000000\"}}", &request) == GW_WT_OK && request.audio_first &&
        strcmp(request.metadata.interaction_profile, "dnd_app") == 0 &&
        strcmp(request.metadata.campaign_id, "campaign-a") == 0 &&
        strcmp(request.metadata.scene_id, "hall") == 0 &&
        strcmp(request.metadata.character_id, "character-a") == 0 &&
        strcmp(request.metadata.knowledge_scope, "character_memory") == 0 &&
        strcmp(request.meta_budget_ms, "45000") == 0 &&
        strcmp(request.meta_deadline_unix_ms, "1788690000000") == 0,
        "audio admission preserves governed scope and timing fields");
    {
        static const char plain[] = "{\"request_id\":\"next-turn\",\"text\":\"hello\",\"identity_token\":\"vat1.payload.signature\"}";
        expect(gw_wt_turn_request_parse_active((const uint8_t *)plain, sizeof(plain) - 1u, &request) == GW_WT_OK &&
            !request.metadata.campaign_id[0] && !request.metadata.scene_id[0] && !request.metadata.character_id[0] &&
            !request.meta_budget_ms[0] && !request.meta_deadline_unix_ms[0],
            "a reused parser cannot retain the previous D&D scope");
    }
}

static void test_initiative_admission(void) {
    char json[GW_WT_CONTROL_FRAME_CAP];
    gw_wt_turn_request request;
    dnd_initiative_request_c product;
    for (size_t count = 1u; count <= 65u; count += count == 1u ? 63u : 1u) {
        size_t used = (size_t)snprintf(json, sizeof(json),
            "{\"request_id\":\"initiative\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"\","
            "\"metadata\":{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"encounter_id\":\"battle\","
            "\"input_mode\":\"webtransport_audio\",\"client_audio_datagram_protocol\":\"dtvp1\"},"
            "\"dnd_initiative\":{\"operation_id\":\"roll-1\",\"campaign_version\":4,\"expected_version\":0,\"selections\":[");
        for (size_t i = 0; i < count; ++i)
            used += (size_t)snprintf(json + used, sizeof(json) - used,
                "%s{\"character_id\":\"pc-%zu\",\"expression\":\"2d20kl1-2\"}", i ? "," : "", i);
        (void)snprintf(json + used, sizeof(json) - used, "]}}");
        cmp_json_object object;
        cmp_turn_metadata_policy policy;
        int visited = 0;
        int product_ok = cmp_json_object_parse(json, &object) &&
            cmp_turn_metadata_visit(&object, count_metadata_field, &visited, &policy, &product, NULL, NULL) == 0;
        int transport_ok = parse_text(json, &request) == GW_WT_OK;
        expect(product_ok == (count <= 64u) && product_ok == transport_ok,
            "HTTP and WebTransport share initiative selection admission through the maximum count");
        if (transport_ok) expect(request.audio_first && request.dnd_initiative.count == count &&
            !memcmp(&request.dnd_initiative, &product, sizeof(product)),
            "audio-first WebTransport keeps every admitted initiative choice and version");
    }
    memset(&request, 0x7f, sizeof(request));
    static const char plain[] = "{\"request_id\":\"ordinary\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"hello\"}";
    expect(gw_wt_turn_request_parse_active((const uint8_t *)plain, sizeof(plain) - 1u, &request) == GW_WT_OK &&
        !request.dnd_initiative.count, "reused WebTransport request clears prior initiative choices");
}

static void test_campaign_admission(void) {
    static const char *const cases[] = {
        "{\"operation\":\"create\",\"operation_id\":\"save-create\",\"expected_version\":0,\"campaign\":{\"name\":\"Table 🐉\",\"ruleset\":\"5e\"}}",
        "{\"operation\":\"add_character\",\"operation_id\":\"save-add\",\"expected_version\":1,\"character\":{\"id\":\"pc\",\"name\":\"Aria\",\"kind\":\"player\",\"species\":\"elf\",\"class_name\":\"rogue\",\"level\":3,\"armor_class\":14,\"max_hp\":23,\"ability_scores\":{\"strength\":8,\"dexterity\":16,\"constitution\":12,\"intelligence\":13,\"wisdom\":11,\"charisma\":14}}}",
        "{\"operation\":\"create\",\"operation_id\":\"save\",\"expected_version\":0,\"campaign\":{\"name\":\"Table\",\"ruleset\":\"5e\",\"owner\":\"victim\"}}",
        "{\"operation\":\"create\",\"operation_id\":\"save\",\"expected_version\":1,\"campaign\":{\"name\":\"Table\",\"ruleset\":\"5e\"}}",
        "{}", "null"
    };
    gw_wt_turn_request request;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char json[2048];
        cmp_json_object object;
        cmp_turn_metadata_policy policy;
        dnd_campaign_request_c product;
        int visited = 0;
        (void)snprintf(json, sizeof(json), "{\"request_id\":\"setup\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"\","
            "\"metadata\":{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"input_mode\":\"webtransport_audio\","
            "\"client_audio_datagram_protocol\":\"dtvp1\"},\"dnd_campaign\":%s}", cases[i]);
        int product_ok = cmp_json_object_parse(json, &object) &&
            cmp_turn_metadata_visit(&object, count_metadata_field, &visited, &policy, NULL, &product, NULL) == 0;
        int transport_ok = parse_text(json, &request) == GW_WT_OK;
        expect(product_ok == (i < 2u) && product_ok == transport_ok, "HTTP and WebTransport agree on campaign setup admission");
        if (transport_ok) expect(request.audio_first && !memcmp(&request.dnd_campaign, &product, sizeof(product)),
            "prepared audio retains every authored campaign field");
        else expect(!visited, "invalid setup never visits admitted metadata");
    }
    memset(&request, 0x7f, sizeof(request));
    static const char plain[] = "{\"request_id\":\"ordinary\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"hello\"}";
    expect(gw_wt_turn_request_parse_active((const uint8_t *)plain, sizeof(plain) - 1u, &request) == GW_WT_OK &&
        !request.dnd_campaign.operation, "ordinary session reuse clears campaign setup");
}

static void test_action_admission(void) {
    static const char *const cases[] = {
        "{\"operation\":\"advance\",\"operation_id\":\"save\",\"expected_version\":1}",
        "{\"operation\":\"damage\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"amount\":7,\"damage_type\":\"fire\"}",
        "{\"operation\":\"heal\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"amount\":7}",
        "{\"operation\":\"condition_add\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"condition\":\"prone\"}",
        "{\"operation\":\"condition_remove\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"condition\":\"prone\"}",
        "{\"operation\":\"end\",\"operation_id\":\"save\",\"expected_version\":1}",
        "{\"operation\":\"advance\",\"operation_id\":\"save\",\"expected_version\":0}",
        "{\"operation\":\"damage\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"amount\":7}",
        "{\"operation\":\"heal\",\"operation_id\":\"save\",\"expected_version\":1,\"participant_id\":\"aria\",\"amount\":0}",
        "{\"operation\":\"advance\",\"operation_id\":\"save\",\"expected_version\":1,\"owner_user_id\":\"victim\"}",
        "{}", "null"
    };
    gw_wt_turn_request request;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char json[2048];
        cmp_json_object object;
        cmp_turn_metadata_policy policy;
        dnd_encounter_action_c product;
        int visited = 0;
        (void)snprintf(json, sizeof(json), "{\"request_id\":\"change\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"\","
            "\"metadata\":{\"interaction_profile\":\"dnd_app\",\"campaign_id\":\"table\",\"encounter_id\":\"battle\","
            "\"input_mode\":\"webtransport_audio\",\"client_audio_datagram_protocol\":\"dtvp1\"},\"dnd_encounter_action\":%s}", cases[i]);
        int product_ok = cmp_json_object_parse(json, &object) &&
            cmp_turn_metadata_visit(&object, count_metadata_field, &visited, &policy, NULL, NULL, &product) == 0;
        int transport_ok = parse_text(json, &request) == GW_WT_OK;
        expect(product_ok == (i < 6u) && product_ok == transport_ok, "HTTP and WebTransport agree on encounter action admission");
        if (transport_ok) expect(request.audio_first && !memcmp(&request.dnd_encounter_action, &product, sizeof(product)),
            "prepared WebTransport retains every explicit encounter choice");
        else expect(!visited, "invalid action never visits metadata");
    }
    memset(&request, 0x7f, sizeof(request));
    static const char plain[] = "{\"request_id\":\"ordinary\",\"identity_token\":\"vat1.payload.signature\",\"text\":\"hello\"}";
    expect(gw_wt_turn_request_parse_active((const uint8_t *)plain, sizeof(plain) - 1u, &request) == GW_WT_OK &&
        !request.dnd_encounter_action.operation, "ordinary session reuse clears encounter action");
}

static int token_limit_request(
    char *out,
    size_t out_cap,
    size_t ignored_fields
) {
    size_t used = 0u;
    size_t field;
    int written;
    if (!out || out_cap == 0u) return -1;
    written = snprintf(out, out_cap, "{");
    if (written != 1) return -1;
    used = 1u;
    for (field = 0u; field < ignored_fields; ++field) {
        written = snprintf(out + used, out_cap - used,
                           "\"ignored_%02zu\":0,", field);
        if (written < 0 || (size_t)written >= out_cap - used) return -1;
        used += (size_t)written;
    }
    written = snprintf(
        out + used, out_cap - used,
        "\"request_id\":\"token-boundary\",\"text\":\"hello\","
        "\"identity_token\":\"vat1.payload.signature\"}");
    if (written < 0 || (size_t)written >= out_cap - used) return -1;
    used += (size_t)written;
    return used <= (size_t)INT32_MAX ? (int)used : -1;
}

static int depth_limit_request(
    char *out,
    size_t out_cap,
    size_t nested_objects
) {
    size_t used = 0u;
    size_t level;
    int written;
    if (!out || out_cap == 0u) return -1;
    written = snprintf(out, out_cap, "{\"ignored\":");
    if (written < 0 || (size_t)written >= out_cap) return -1;
    used = (size_t)written;
    for (level = 0u; level < nested_objects; ++level) {
        written = snprintf(out + used, out_cap - used, "{\"level\":");
        if (written < 0 || (size_t)written >= out_cap - used) return -1;
        used += (size_t)written;
    }
    written = snprintf(out + used, out_cap - used, "\"leaf\"");
    if (written < 0 || (size_t)written >= out_cap - used) return -1;
    used += (size_t)written;
    for (level = 0u; level < nested_objects; ++level) {
        if (used + 1u >= out_cap) return -1;
        out[used++] = '}';
    }
    written = snprintf(
        out + used, out_cap - used,
        ",\"request_id\":\"depth-boundary\",\"text\":\"hello\","
        "\"identity_token\":\"vat1.payload.signature\"}");
    if (written < 0 || (size_t)written >= out_cap - used) return -1;
    used += (size_t)written;
    return used <= (size_t)INT32_MAX ? (int)used : -1;
}

static size_t string_scan_request(
    uint8_t *out,
    size_t out_cap,
    const uint8_t *content,
    size_t content_len
) {
    static const char prefix[] = "{\"ignored\":\"";
    static const char suffix[] =
        "\",\"request_id\":\"string-scan\",\"text\":\"hello\","
        "\"identity_token\":\"vat1.payload.signature\"}";
    const size_t prefix_len = sizeof(prefix) - 1u;
    const size_t suffix_len = sizeof(suffix) - 1u;
    size_t used;
    if (!out || (!content && content_len != 0u) || prefix_len > out_cap ||
        content_len > out_cap - prefix_len ||
        suffix_len > out_cap - prefix_len - content_len)
        return 0u;
    memcpy(out, prefix, prefix_len);
    if (content_len != 0u) memcpy(out + prefix_len, content, content_len);
    used = prefix_len + content_len;
    memcpy(out + used, suffix, suffix_len);
    return used + suffix_len;
}

static void set_event(turn_event_c *event, int type_id, const char *type) {
    memset(event, 0, sizeof(*event));
    memcpy(event->request_id, "req-event", sizeof("req-event"));
    memcpy(event->type, type, strlen(type) + 1u);
    event->type_id = type_id;
}

static void test_transcript_control(void) {
    turn_start_c turn = {0};
    char control[GW_WT_CONTROL_FRAME_CAP], decoded[sizeof(turn.text)];
    size_t length = 99u;
    int final = 0;
    strcpy(turn.request_id, "transcript-request");
    strcpy(turn.session_id, "transcript-session");
    strcpy(turn.user_id, "transcript-user");
    strcpy(turn.text, "Mira says \"bonjour\".\nBefore you cast the spell…");
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_OK,
        "bound committed transcript projects to browser JSON");
    expect(length == strlen(control) && cmp_json_bool(control, "is_final", &final) == 1 && final,
        "transcript control is final and has its exact byte length");
    expect(cmp_json_str(control, "text", decoded, sizeof(decoded)) == 1 && strcmp(decoded, turn.text) == 0,
        "transcript JSON preserves quotes, controls, and UTF-8");
    expect(cmp_json_str(control, "request_id", decoded, sizeof(decoded)) == 1 && strcmp(decoded, turn.request_id) == 0,
        "transcript control carries the admitted request identity");
    expect(gw_wt_transcript_control(&turn, "other-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED && length == 0u,
        "transcript rejects another request");
    expect(gw_wt_transcript_control(&turn, "transcript-request", "other-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED,
        "transcript rejects another audio session");
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "other-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED,
        "transcript rejects another owner");
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, 16u, &length) == GW_WT_ERR_CAPACITY && !control[0] && length == 0u,
        "short transcript output capacity cannot publish partial JSON");
    strcpy(turn.text, " \t\n");
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED,
        "empty speech is not a final transcript");
    turn.text[0] = (char)0xff; turn.text[1] = 0;
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED,
        "malformed UTF-8 cannot enter the transcript control");
    memset(turn.text, 1, sizeof(turn.text) - 1u); turn.text[sizeof(turn.text) - 1u] = 0;
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_OK && length < sizeof(control),
        "maximum escaped transcript fits the bounded control frame");
    memset(turn.text, 'x', sizeof(turn.text));
    expect(gw_wt_transcript_control(&turn, "transcript-request", "transcript-session",
        "transcript-user", control, sizeof(control), &length) == GW_WT_ERR_MALFORMED,
        "unterminated transcript fails within its bound");
}

static void test_progressing_audio_gaps(void) {
    static const uint32_t sequences[] = {0u, 2u, 4u, 1u, 3u};
    static const uint64_t times[] = {1000u, 1020u, 1180u, 1200u, 1360u};
    static const uint8_t pcm[] = {1u, 2u};
    gw_wt_audio_reorder window = {0};
    uint64_t deadline = 0u;
    size_t i;
    for (i = 0u; i < sizeof(sequences) / sizeof(sequences[0]); ++i) {
        const uint8_t *payload;
        size_t length;
        uint32_t previous = window.next_sequence;
        /* A real event loop checks expiry between arrivals. */
        if (deadline && times[i] >= deadline) break;
        expect(gw_wt_audio_reorder_insert(&window, sequences[i], pcm, sizeof(pcm)) ==
                   GW_WT_AUDIO_INSERTED, "timed reorder admits a unique packet");
        while (gw_wt_audio_reorder_peek(&window, &payload, &length)) {
            expect(length == sizeof(pcm) && memcmp(payload, pcm, length) == 0,
                   "timed reorder preserves PCM");
            expect(gw_wt_audio_reorder_pop(&window), "timed reorder advances its prefix");
        }
        deadline = gw_wt_audio_gap_deadline(deadline, previous, window.next_sequence,
                                           window.buffered_datagrams != 0u, times[i]);
    }
    expect(i == sizeof(sequences) / sizeof(sequences[0]),
           "successive repaired gaps do not inherit the first gap timeout");
    expect(deadline == 0u && gw_wt_audio_reorder_commit_state(&window, 5u, 10u) ==
               GW_WT_AUDIO_COMMIT_READY, "timed reorder commits only complete PCM");

    deadline = gw_wt_audio_gap_deadline(0u, 1u, 1u, 1, 1020u);
    expect(deadline == 1270u, "first missing packet retains the 250 ms bound");
    expect(gw_wt_audio_gap_deadline(deadline, 1u, 1u, 1, 1260u) == deadline,
           "later and duplicate packets cannot extend the same missing prefix");
    expect(gw_wt_audio_gap_deadline(deadline, 1u, 1u, 0, 1260u) == 0u,
           "complete input clears the gap deadline");
    expect(gw_wt_audio_gap_deadline(0u, 1u, 1u, 1, UINT64_MAX - 1u) == UINT64_MAX,
           "deadline arithmetic cannot wrap");
}

static void test_reliable_audio_input(void) {
    gw_wt_turn_request request;
    gw_wt_datagram packet;
    uint8_t audio[GW_WT_AUDIO_HEADER_BYTES + 640u] = {'D', 'T', 'V', 'P', '1', 0, 0, 0, 7};
    static const char stream[] = "{\"request_id\":\"stream-turn\",\"identity_token\":\"test\",\"text\":\"\","
        "\"metadata\":{\"input_mode\":\"webtransport_audio_stream_v1\",\"client_audio_datagram_protocol\":\"dtvp1\"}}";
    static const char legacy[] = "{\"request_id\":\"legacy-turn\",\"identity_token\":\"test\",\"text\":\"\","
        "\"metadata\":{\"input_mode\":\"webtransport_audio\",\"client_audio_datagram_protocol\":\"dtvp1\"}}";
    static const char end[] = "{\"type\":\"end\",\"request_id\":\"stream-turn\",\"packet_count\":8,\"audio_bytes\":5120}";
    static const char unbound[] = "{\"type\":\"end\",\"packet_count\":8,\"audio_bytes\":5120}";
    expect(parse_text(stream, &request) == GW_WT_OK && request.audio_first && request.audio_stream,
           "reliable input requires explicit stream mode");
    expect(gw_wt_turn_request_parse_active((const uint8_t *)legacy, sizeof(legacy) - 1u, &request) == GW_WT_OK &&
           request.audio_first && !request.audio_stream, "active parser clears prior stream negotiation");
    expect(parse_text("{\"request_id\":\"text\",\"identity_token\":\"test\",\"text\":\"hello\","
           "\"metadata\":{\"input_mode\":\"webtransport_audio_stream_v1\"}}", &request) == GW_WT_ERR_MALFORMED,
           "text cannot negotiate an audio stream");
    expect(gw_wt_stream_input_parse(3u, audio, sizeof(audio), &packet) == GW_WT_OK &&
           packet.is_audio && !packet.is_control && packet.sequence == 7u && packet.payload_len == 640u &&
           packet.payload == audio + 9u, "stream audio preserves exact sequence and PCM bytes");
    expect(gw_wt_stream_input_parse(3u, audio, sizeof(audio) - 1u, &packet) == GW_WT_ERR_MALFORMED,
           "odd stream PCM rejects before admission");
    for (size_t length = 0; length <= GW_WT_AUDIO_HEADER_BYTES; length++)
        expect(gw_wt_stream_input_parse(3u, audio, length, &packet) == GW_WT_ERR_MALFORMED,
               "truncated stream audio rejects");
    expect(gw_wt_stream_input_parse(2u, audio, sizeof(audio), &packet) == GW_WT_ERR_MALFORMED,
           "response frames cannot become input PCM");
    expect(gw_wt_stream_input_parse(1u, (const uint8_t *)end, sizeof(end) - 1u, &packet) == GW_WT_OK &&
           packet.is_control && packet.control.kind == GW_WT_CONTROL_END && packet.control.packet_count == 8u &&
           packet.control.audio_bytes == 5120u && !strcmp(packet.control.request_id, "stream-turn"),
           "reliable end retains turn binding and exact totals");
    expect(gw_wt_stream_input_parse(1u, (const uint8_t *)unbound, sizeof(unbound) - 1u, &packet) == GW_WT_ERR_MALFORMED,
           "stream control without a request binding rejects");
}

int main(void) {
    test_reliable_audio_input();
    test_progressing_audio_gaps();
    static const char identity[] = "vat1.payload.signature";
    gw_wt_turn_request request;
    gw_wt_datagram datagram;
    gw_wt_audio_reorder reorder;
    gw_wt_audio_delivery delivery;
    gw_wt_byte_ring ring;
    static uint8_t bulk[GW_WT_RESPONSE_QUEUE_CAP - 2u];
    uint8_t audio[1u + GW_WT_AUDIO_HEADER_BYTES + 640u];
    uint8_t control[256];
    uint64_t value = 0;
    size_t consumed = 0;
    int written;

    test_transcript_control();
    test_loop_page_admission();
    test_metadata_admission();
    test_initiative_admission();
    test_campaign_admission();
    test_action_admission();

    expect(sizeof(turn_response_event) <= 192u,
           "public response view stays compact");
    {
        static const char nonce[] = "0123456789abcdef0123456789abcdef";
        char subject[GW_WT_EVENT_SUBJECT_LENGTH + 2u];
        char changed_nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u];
        char unterminated_nonce[GW_WT_EVENT_NONCE_HEX_LEN + 1u];
        size_t slot = 999u;
        memset(subject, 0x5a, sizeof(subject));
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 63u, nonce) == GW_WT_OK &&
                   strcmp(subject,
                          "ai.turn.events.cwt.3f.0123456789abcdef0123456789abcdef") == 0,
               "response capability writes a canonical slot and nonce");
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_OK && slot == 63u,
               "response capability resolves its routing slot");
        expect(gw_wt_response_subject_write(
                   subject, GW_WT_EVENT_SUBJECT_LENGTH, 0u, nonce) ==
                   GW_WT_ERR_ARGUMENT &&
                   gw_wt_response_subject_write(
                       subject, sizeof(subject), 256u, nonce) ==
                   GW_WT_ERR_ARGUMENT,
               "response capability writer enforces output and slot bounds");
        expect(gw_wt_response_subject_write(
                   NULL, sizeof(subject), 0u, nonce) == GW_WT_ERR_ARGUMENT &&
                   gw_wt_response_subject_write(
                       subject, sizeof(subject), 0u, NULL) == GW_WT_ERR_ARGUMENT,
               "response capability writer rejects missing arguments");
        memcpy(changed_nonce, nonce, sizeof(changed_nonce));
        changed_nonce[0] = 'A';
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 0u, changed_nonce) ==
                   GW_WT_ERR_MALFORMED,
               "response capability rejects a noncanonical nonce");
        memset(unterminated_nonce, '0', sizeof(unterminated_nonce));
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 0u, unterminated_nonce) ==
                   GW_WT_ERR_MALFORMED,
               "response capability rejects an unterminated nonce");
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 255u, nonce) == GW_WT_OK &&
                   gw_wt_response_subject_slot(subject, &slot) == GW_WT_OK &&
                   slot == 255u,
               "response capability supports the complete byte slot range");
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 0u, nonce) == GW_WT_OK,
               "response capability rewrites after rejected input");
        subject[0] = 'A';
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_ERR_MALFORMED,
               "response capability rejects a changed prefix");
        subject[0] = 'a';
        subject[sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u] = 'g';
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_ERR_MALFORMED,
               "response capability rejects a non-hex slot");
        subject[sizeof(GW_WT_EVENT_SUBJECT_PREFIX) - 1u] = '0';
        subject[sizeof(GW_WT_EVENT_SUBJECT_PREFIX) + 1u] = '-';
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_ERR_MALFORMED,
               "response capability rejects a missing slot separator");
        subject[sizeof(GW_WT_EVENT_SUBJECT_PREFIX) + 1u] = '.';
        subject[GW_WT_EVENT_SUBJECT_LENGTH - 1u] = '\0';
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_ERR_MALFORMED,
               "response capability rejects a truncated nonce");
        expect(gw_wt_response_subject_write(
                   subject, sizeof(subject), 0u, nonce) == GW_WT_OK,
               "response capability restores its canonical length");
        {
            size_t cut;
            for (cut = 0u; cut < GW_WT_EVENT_SUBJECT_LENGTH; ++cut) {
                char saved = subject[cut];
                subject[cut] = '\0';
                expect(
                    gw_wt_response_subject_slot(subject, &slot) ==
                        GW_WT_ERR_MALFORMED,
                    "response capability rejects every truncated layout");
                subject[cut] = saved;
            }
        }
        subject[GW_WT_EVENT_SUBJECT_LENGTH] = '0';
        subject[GW_WT_EVENT_SUBJECT_LENGTH + 1u] = '\0';
        expect(gw_wt_response_subject_slot(subject, &slot) == GW_WT_ERR_MALFORMED,
               "response capability rejects an extended nonce");
        expect(gw_wt_response_subject_slot(NULL, &slot) == GW_WT_ERR_ARGUMENT &&
                   gw_wt_response_subject_slot(subject, NULL) == GW_WT_ERR_ARGUMENT,
               "response capability parser rejects missing arguments");
    }
    {
        turn_response_event empty_event;
        uint8_t wire[16];
        memset(&empty_event, 0, sizeof(empty_event));
        expect(turn_response_protobuf_encode(
                   wire, sizeof(wire), &empty_event) == 0,
               "public response encoder rejects incomplete views");
    }
    expect(parse_text(
               "{\"request_id\":\"req-1\",\"session_id\":\"session-1\","
               "\"text\":\"Roll \\\"initiative\\\" \\u2694\","
               "\"identity_token\":\"vat1.payload.signature\","
               "\"enable_rag\":false,\"enable_tts\":true,"
               "\"metadata\":{\"input_mode\":\"text\"},\"ignored\":[1,true,null]}",
               &request) == GW_WT_OK,
           "text request parses");
    expect(strcmp(request.request_id, "req-1") == 0, "text request id decodes");
    expect(strcmp(request.session_id, "session-1") == 0, "text session id decodes");
    expect(strcmp(request.text, "Roll \"initiative\" ⚔") == 0,
           "escaped text and Unicode decode");
    expect(strcmp(request.identity_token, identity) == 0, "identity token decodes");
    expect(request.enable_rag == 0 && request.enable_tts == 1 && request.audio_first == 0,
           "text request policy flags decode");
    {
        static const char active_json[] =
            "{\"request_id\":\"active-request\",\"text\":\"hello\","
            "\"identity_token\":\"vat1.payload.signature\"}";
        gw_wt_turn_request checked_request;
        gw_wt_turn_request active_request;
        gw_wt_turn_request fresh_active_request;
        memset(&checked_request, 0x5a, sizeof(checked_request));
        memset(&active_request, 0xa5, sizeof(active_request));
        expect(gw_wt_turn_request_parse(
                   (const uint8_t *)active_json, sizeof(active_json) - 1u,
                   &checked_request) == GW_WT_OK &&
                   gw_wt_turn_request_parse_active(
                       (const uint8_t *)active_json, sizeof(active_json) - 1u,
                       &active_request) == GW_WT_OK &&
                   turn_requests_equal(&checked_request, &active_request),
               "active request parser matches every logical field");
        expect(gw_wt_turn_request_parse_active(
                   (const uint8_t *)active_json, sizeof(active_json) - 1u,
                   &fresh_active_request) == GW_WT_OK &&
                   turn_requests_equal(
                       &checked_request, &fresh_active_request),
               "active request parser initializes fresh logical output");
        expect((unsigned char)active_request.request_id[
                   sizeof("active-request")] == 0xa5u &&
                   (unsigned char)active_request.text[sizeof("hello")] == 0xa5u,
               "active request parser leaves unreachable array tails untouched");
        expect(gw_wt_turn_request_parse_active(
                   (const uint8_t *)"{", 1u, &active_request) ==
                   GW_WT_ERR_MALFORMED &&
                   gw_wt_turn_request_parse_active(
                       (const uint8_t *)active_json, sizeof(active_json) - 1u,
                       &active_request) == GW_WT_OK &&
                   turn_requests_equal(&checked_request, &active_request),
               "active request parser recovers after rejected input");
        expect(gw_wt_turn_request_parse_active(
                   (const uint8_t *)active_json, sizeof(active_json) - 1u,
                   NULL) == GW_WT_ERR_ARGUMENT,
               "active request parser rejects a missing output");
    }
    expect(parse_text(
               "{\"request_id\":\"plain-utf8\",\"text\":\"Café 🐉\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_OK && strcmp(request.text, "Café 🐉") == 0,
           "plain multibyte text uses the exact decoded bytes");
    {
        static const uint8_t invalid_text[] =
            "{\"request_id\":\"invalid-utf8\",\"text\":\"bad "
            "\xc3\x28"
            "\",\"identity_token\":\"vat1.payload.signature\"}";
        static const uint8_t invalid_ignored[] =
            "{\"ignored\":\"bad "
            "\xe2\x28\xa1"
            "\",\"request_id\":\"invalid-ignored\",\"text\":\"hello\","
            "\"identity_token\":\"vat1.payload.signature\"}";
        expect(gw_wt_turn_request_parse(
                   invalid_text, sizeof(invalid_text) - 1u, &request) ==
                   GW_WT_ERR_MALFORMED,
               "invalid raw UTF-8 in text is rejected");
        expect(gw_wt_turn_request_parse(
                   invalid_ignored, sizeof(invalid_ignored) - 1u, &request) ==
                   GW_WT_ERR_MALFORMED,
               "invalid raw UTF-8 in an ignored field is rejected");
    }
    {
        uint8_t storage[512u];
        uint8_t content[32u];
        size_t alignment;
        int plain_ok = 1;
        int escape_ok = 1;
        int control_ok = 1;
        int utf8_ok = 1;
        for (alignment = 0u; alignment < sizeof(uint64_t); ++alignment) {
            size_t length;
            for (length = 0u; length < sizeof(content); ++length) {
                uint8_t *json = storage + alignment;
                size_t json_len;
                memset(content, 'a', length);
                json_len = string_scan_request(
                    json, sizeof(storage) - alignment, content, length);
                if (json_len == 0u ||
                    gw_wt_turn_request_parse(json, json_len, &request) != GW_WT_OK)
                    plain_ok = 0;
            }
        }
        expect(plain_ok,
               "plain string scan accepts every word alignment and tail");
        for (alignment = 0u; alignment < sizeof(uint64_t); ++alignment) {
            size_t position;
            for (position = 0u; position < 16u; ++position) {
                uint8_t *json = storage + alignment;
                size_t json_len;
                memset(content, 'a', 24u);
                content[position] = '\\';
                content[position + 1u] = 'n';
                json_len = string_scan_request(
                    json, sizeof(storage) - alignment, content, 24u);
                if (json_len == 0u ||
                    gw_wt_turn_request_parse(json, json_len, &request) != GW_WT_OK)
                    escape_ok = 0;
                memset(content, 'a', 24u);
                content[position] = 0xc3u;
                content[position + 1u] = 0xa9u;
                json_len = string_scan_request(
                    json, sizeof(storage) - alignment, content, 24u);
                if (json_len == 0u ||
                    gw_wt_turn_request_parse(json, json_len, &request) != GW_WT_OK)
                    utf8_ok = 0;
                content[position + 1u] = '(';
                json_len = string_scan_request(
                    json, sizeof(storage) - alignment, content, 24u);
                if (json_len == 0u ||
                    gw_wt_turn_request_parse(json, json_len, &request) !=
                        GW_WT_ERR_MALFORMED)
                    utf8_ok = 0;
            }
        }
        expect(escape_ok,
               "escaped string scan accepts every word alignment");
        expect(utf8_ok,
               "raw UTF-8 scan validates every word alignment");
        for (alignment = 0u; alignment < sizeof(uint64_t); ++alignment) {
            size_t position;
            for (position = 0u; position < 16u; ++position) {
                unsigned control_byte;
                for (control_byte = 0u; control_byte < 0x20u; ++control_byte) {
                    uint8_t *json = storage + alignment;
                    size_t json_len;
                    memset(content, 'a', 24u);
                    content[position] = (uint8_t)control_byte;
                    json_len = string_scan_request(
                        json, sizeof(storage) - alignment, content, 24u);
                    if (json_len == 0u ||
                        gw_wt_turn_request_parse(json, json_len, &request) !=
                            GW_WT_ERR_MALFORMED)
                        control_ok = 0;
                }
            }
        }
        expect(control_ok,
               "string scan rejects every raw control byte at every word alignment");
    }
    expect(parse_text(
               "{\"request_id\":\"surrogate-pair\","
               "\"text\":\"dragon \\ud83d\\udc09\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_OK && strcmp(request.text, "dragon 🐉") == 0,
           "escaped surrogate pair keeps the slow decoder path");
    {
        char bounded_json[4096];
        char bounded_text[2049];
        int bounded_len;
        memset(bounded_text, 'a', sizeof(bounded_text));
        bounded_text[2047] = '\0';
        bounded_len = snprintf(
            bounded_json, sizeof(bounded_json),
            "{\"request_id\":\"plain-boundary\",\"text\":\"%s\","
            "\"identity_token\":\"vat1.payload.signature\"}",
            bounded_text);
        expect(bounded_len > 0 && (size_t)bounded_len < sizeof(bounded_json) &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_json, (size_t)bounded_len,
                       &request) == GW_WT_OK && strlen(request.text) == 2047u,
               "plain text accepts its exact output boundary");
        bounded_text[2047] = 'a';
        bounded_text[2048] = '\0';
        bounded_len = snprintf(
            bounded_json, sizeof(bounded_json),
            "{\"request_id\":\"plain-overflow\",\"text\":\"%s\","
            "\"identity_token\":\"vat1.payload.signature\"}",
            bounded_text);
        expect(bounded_len > 0 && (size_t)bounded_len < sizeof(bounded_json) &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_json, (size_t)bounded_len,
                       &request) == GW_WT_ERR_MALFORMED,
               "plain text rejects one byte beyond its output boundary");
    }
    expect(parse_text(
               "{\"request_id\":\"req-audio\",\"session_id\":\"voice-session\","
               "\"text\":\"\",\"identity_token\":\"vat1.payload.signature\","
               "\"metadata\":{\"input_mode\":\"webtransport_audio\","
               "\"client_audio_datagram_protocol\":\"dtvp1\"}}",
               &request) == GW_WT_OK && request.audio_first == 1,
           "audio-first request parses");
    expect(parse_text(
               "{\"request_id\":\"req-audio-old\",\"text\":\"\","
               "\"identity_token\":\"vat1.payload.signature\","
               "\"metadata\":{\"input_mode\":\"webtransport_audio\"}}",
               &request) == GW_WT_ERR_MALFORMED,
           "audio-first request without sequenced protocol is rejected");
    expect(parse_text(
               "{\"request_id\":\"req-default\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_OK && strcmp(request.session_id, "req-default") == 0,
           "missing session uses request capability");
    expect(parse_text(
               "{\"request_ix\":\"forged\",\"session_ix\":\"forged\","
               "\"texx\":\"forged\",\"identity_tokex\":\"forged\","
               "\"enable_raw\":true,\"enable_ttx\":true,"
               "\"metadatx\":{\"input_mode\":\"webtransport_audio\"},"
               "\"request_id\":\"schema-exact\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_OK &&
               strcmp(request.request_id, "schema-exact") == 0 &&
               strcmp(request.session_id, "schema-exact") == 0 &&
               strcmp(request.text, "hello") == 0 &&
               strcmp(request.identity_token, identity) == 0 &&
               request.enable_rag == 0 && request.enable_tts == 0 &&
               request.audio_first == 0,
           "request schema ignores exact-length near-match keys");
    expect(parse_text(
               "{\"request_id\":\"same\",\"request_id\":\"other\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "duplicate top-level field rejected");
    expect(parse_text(
               "{\"\":1,\"\":2,\"request_id\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "duplicate empty object key rejected");
    expect(parse_text(
               "{\"x\":1,\"ac\":2,\"request_id\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_OK,
           "distinct fingerprint collision keys accepted");
    expect(parse_text(
               "{\"x\":1,\"ac\":2,\"x\":3,\"request_id\":\"same\","
               "\"text\":\"hello\",\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "duplicate key after fingerprint collision rejected");
    expect(parse_text("[]", &request) == GW_WT_ERR_MALFORMED &&
               parse_text("0", &request) == GW_WT_ERR_MALFORMED &&
               parse_text("true", &request) == GW_WT_ERR_MALFORMED &&
               parse_text("null", &request) == GW_WT_ERR_MALFORMED &&
               parse_text("\"root\"", &request) == GW_WT_ERR_MALFORMED,
           "top-level JSON must be an object");
    expect(parse_text(
               "{\"request_id\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\","
               "\"metadata\":{\"input_mode\":\"text\",\"input_mode\":\"webtransport_audio\"}}",
               &request) == GW_WT_ERR_MALFORMED,
           "duplicate metadata field rejected");
    expect(parse_text(
               "{\"request_id\":\"same\" \"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "missing object comma rejected");
    expect(parse_text(
               "{\"request_id\" \"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "missing object colon rejected");
    expect(parse_text(
               "{\"request\\u005fid\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "escaped security field name rejected");
    expect(parse_text(
               "{\"request_id\":\"bad/id\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "unsafe request id rejected");
    expect(parse_text(
               "{\"request_id\":\"same\",\"text\":\"\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "empty non-audio turn rejected");
    expect(parse_text(
               "{\"request_id\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\",\"enable_tts\":\"true\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "string boolean rejected");
    expect(parse_text(
               "{\"request_id\":\"same\",\"text\":\"bad \\ud800\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "unpaired surrogate rejected");
    expect(parse_text(
               " { \"request_id\" : \"spaced\", \"text\" : \"hello\", "
               "\"identity_token\" : \"vat1.payload.signature\" } \n",
               &request) == GW_WT_OK,
           "legal JSON whitespace accepted");
    expect(parse_text(
               "{\"ignored\":{\"request_id\":\"nested\","
               "\"array\":[{\"identity_token\":\"nested\"},[1,2,3]]},"
               "\"metadata\":{\"input_mode\":\"text\"},"
               "\"identity_token\":\"vat1.payload.signature\","
               "\"text\":\"hello\",\"request_id\":\"outer\"}",
               &request) == GW_WT_OK &&
               strcmp(request.request_id, "outer") == 0 &&
               strcmp(request.identity_token, identity) == 0,
           "root lookup skips complete nested subtrees");
    expect(parse_text(
               "{\"ignored\":{\"same\":1,\"same\":2},"
               "\"request_id\":\"same\",\"text\":\"hello\","
               "\"identity_token\":\"vat1.payload.signature\"}",
               &request) == GW_WT_ERR_MALFORMED,
           "duplicate field in an ignored nested object is rejected");
    {
        char bounded_request[4096];
        int bounded_len = token_limit_request(
            bounded_request, sizeof(bounded_request), 252u);
        expect(bounded_len > 0 &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_request, (size_t)bounded_len,
                       &request) == GW_WT_OK &&
                   strcmp(request.request_id, "token-boundary") == 0,
               "request at the fixed token boundary parses");
        bounded_len = token_limit_request(
            bounded_request, sizeof(bounded_request), 253u);
        expect(bounded_len > 0 &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_request, (size_t)bounded_len,
                       &request) == GW_WT_ERR_MALFORMED,
               "request beyond the fixed token boundary is rejected");
        expect(parse_text(
                   "{\"request_id\":\"after-token-limit\",\"text\":\"hello\","
                   "\"identity_token\":\"vat1.payload.signature\"}",
                   &request) == GW_WT_OK &&
                   strcmp(request.request_id, "after-token-limit") == 0 &&
                   strcmp(request.session_id, "after-token-limit") == 0 &&
                   strcmp(request.text, "hello") == 0 &&
                   strcmp(request.identity_token, identity) == 0,
               "request parser resets count after token-capacity rejection");
    }
    {
        char bounded_request[1024];
        int bounded_len = depth_limit_request(
            bounded_request, sizeof(bounded_request), 7u);
        expect(bounded_len > 0 &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_request, (size_t)bounded_len,
                       &request) == GW_WT_OK,
               "request at the fixed nesting boundary parses");
        bounded_len = depth_limit_request(
            bounded_request, sizeof(bounded_request), 8u);
        expect(bounded_len > 0 &&
                   gw_wt_turn_request_parse(
                       (const uint8_t *)bounded_request, (size_t)bounded_len,
                       &request) == GW_WT_ERR_MALFORMED,
               "request beyond the fixed nesting boundary is rejected");
    }

    {
        static const uint8_t one[] = {0x25};
        static const uint8_t two[] = {0x7b, 0xcd};
        static const uint8_t four[] = {0x80, 0x01, 0x00, 0x00};
        static const uint8_t eight[] = {0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00};
        static const uint8_t truncated[] = {0x40};
        static const uint8_t noncanonical[] = {0x40, 0x01};
        expect(gw_wt_quic_varint_decode(one, sizeof(one), &value, &consumed) == GW_WT_OK &&
                   value == 37u && consumed == 1u,
               "one-byte QUIC varint");
        expect(gw_wt_quic_varint_decode(two, sizeof(two), &value, &consumed) == GW_WT_OK &&
                   value == 15309u && consumed == 2u,
               "two-byte QUIC varint");
        expect(gw_wt_quic_varint_decode(four, sizeof(four), &value, &consumed) == GW_WT_OK &&
                   value == 65536u && consumed == 4u,
               "four-byte QUIC varint");
        expect(gw_wt_quic_varint_decode(eight, sizeof(eight), &value, &consumed) == GW_WT_OK &&
                   value == 1073741824u && consumed == 8u,
               "eight-byte QUIC varint");
        expect(gw_wt_quic_varint_decode(truncated, sizeof(truncated), &value, &consumed) ==
                   GW_WT_ERR_MALFORMED,
               "truncated QUIC varint rejected");
        expect(gw_wt_quic_varint_decode(noncanonical, sizeof(noncanonical), &value, &consumed) ==
                   GW_WT_ERR_MALFORMED,
               "noncanonical QUIC varint rejected");
    }

    memset(audio, 0x55, sizeof(audio));
    audio[0] = 0x01; /* Quarter Stream ID 1 maps to CONNECT stream 4. */
    memcpy(audio + 1u, GW_WT_AUDIO_PREFIX, sizeof(GW_WT_AUDIO_PREFIX) - 1u);
    audio[6] = 0x01;
    audio[7] = 0x02;
    audio[8] = 0x03;
    audio[9] = 0x04;
    expect(gw_wt_datagram_parse(audio, sizeof(audio), &datagram) == GW_WT_OK &&
               datagram.session_stream_id == 4u && !datagram.is_control &&
               datagram.is_audio && datagram.sequence == 0x01020304u &&
               datagram.payload_len == 640u &&
               datagram.payload == audio + 1u + GW_WT_AUDIO_HEADER_BYTES,
           "sequenced 20ms PCM datagram maps to WebTransport session");
    audio[1] = 0x00;
    expect(gw_wt_datagram_parse(audio, sizeof(audio), &datagram) == GW_WT_ERR_MALFORMED,
           "unframed PCM datagram rejected");
    audio[1] = 'D';
    expect(gw_wt_datagram_parse(audio, sizeof(audio) - 1u, &datagram) ==
               GW_WT_ERR_MALFORMED,
           "odd sequenced PCM payload rejected");

    written = snprintf((char *)control, sizeof(control),
                       "\x01%s{\"type\":\"cancel\",\"request_id\":\"req-1\","
                       "\"reason\":\"client_interrupt\"}",
                       "DTVA1:");
    expect(written > 0 && (size_t)written < sizeof(control) &&
               gw_wt_datagram_parse(control, (size_t)written, &datagram) == GW_WT_OK &&
               datagram.is_control && datagram.control.kind == GW_WT_CONTROL_CANCEL &&
               strcmp(datagram.control.request_id, "req-1") == 0 &&
               strcmp(datagram.control.reason, "client_interrupt") == 0,
           "cancel control datagram parses");
    written = snprintf((char *)control, sizeof(control),
                       "\x01%s{\"ignored\":{\"type\":\"end\"},"
                       "\"reason\":\"nested_skip\",\"request_id\":\"req-2\","
                       "\"type\":\"cancel\"}", "DTVA1:");
    expect(written > 0 && (size_t)written < sizeof(control) &&
               gw_wt_datagram_parse(control, (size_t)written, &datagram) == GW_WT_OK &&
               datagram.control.kind == GW_WT_CONTROL_CANCEL &&
               strcmp(datagram.control.request_id, "req-2") == 0 &&
               strcmp(datagram.control.reason, "nested_skip") == 0,
           "control lookup skips a nested subtree");
    written = snprintf((char *)control, sizeof(control),
                       "\x01%s{\"type\":\"end\",\"packet_count\":500,"
                       "\"audio_bytes\":320000}", "DTVA1:");
    expect(written > 0 && (size_t)written < sizeof(control) &&
               gw_wt_datagram_parse(control, (size_t)written, &datagram) == GW_WT_OK &&
               datagram.control.kind == GW_WT_CONTROL_END &&
               datagram.control.packet_count == 500u &&
               datagram.control.audio_bytes == 320000u,
           "bounded end receipt control parses");
    written = snprintf((char *)control, sizeof(control),
                       "\x01%s{\"type\":\"end\"}", "DTVA1:");
    expect(written > 0 && (size_t)written < sizeof(control) &&
               gw_wt_datagram_parse(control, (size_t)written, &datagram) ==
                   GW_WT_ERR_MALFORMED,
           "end control without delivery totals is rejected");
    written = snprintf((char *)control, sizeof(control),
                       "\x01%s{\"type\":\"unknown\"}", "DTVA1:");
    expect(written > 0 && (size_t)written < sizeof(control) &&
               gw_wt_datagram_parse(control, (size_t)written, &datagram) ==
                   GW_WT_ERR_MALFORMED,
           "unknown control datagram rejected");

    memset(&reorder, 0, sizeof(reorder));
    memset(audio, 0x11, 640u);
    expect(gw_wt_audio_reorder_insert(&reorder, 1u, audio, 640u) ==
               GW_WT_AUDIO_INSERTED && reorder.reordered_datagrams == 1u,
           "future PCM packet enters bounded reorder window");
    memset(audio, 0x22, 640u);
    expect(gw_wt_audio_reorder_insert(&reorder, 0u, audio, 640u) ==
               GW_WT_AUDIO_INSERTED && reorder.unique_datagrams == 2u &&
               reorder.unique_audio_bytes == 1280u,
           "missing leading PCM packet closes gap");
    {
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        expect(gw_wt_audio_reorder_peek(&reorder, &payload, &payload_len) &&
                   payload_len == 640u && payload[0] == 0x22,
               "reorder window exposes sequence zero first");
        expect(gw_wt_audio_reorder_pop(&reorder) &&
                   gw_wt_audio_reorder_peek(&reorder, &payload, &payload_len) &&
                   payload[0] == 0x11 && gw_wt_audio_reorder_pop(&reorder),
               "reorder window drains contiguous PCM in sequence");
    }
    expect(reorder.next_sequence == 2u && reorder.buffered_datagrams == 0u,
           "reorder cursor advances after drain");
    expect(gw_wt_audio_reorder_commit_state(&reorder, 2u, 1280u) ==
               GW_WT_AUDIO_COMMIT_READY &&
               gw_wt_audio_reorder_commit_state(&reorder, 2u, 1278u) ==
                   GW_WT_AUDIO_COMMIT_MISMATCH,
           "commit state binds exact packet and byte totals");
    expect(gw_wt_audio_reorder_insert(&reorder, 1u, audio, 640u) ==
               GW_WT_AUDIO_DUPLICATE && reorder.duplicate_datagrams == 1u,
           "late duplicate PCM is counted and ignored");

    memset(&delivery, 0, sizeof(delivery));
    expect(gw_wt_audio_delivery_should_forward(&delivery) == 1 &&
               gw_wt_audio_delivery_record(&delivery, 1, 640u) == GW_WT_OK &&
               delivery.forwarded_datagrams == 1u &&
               delivery.forwarded_audio_bytes == 640u,
           "audio delivery forwards PCM before the server endpoint");
    expect(gw_wt_audio_delivery_mark_endpoint(&delivery) == 1 &&
               gw_wt_audio_delivery_mark_endpoint(&delivery) == 0 &&
               gw_wt_audio_delivery_should_forward(&delivery) == 0,
           "audio delivery accepts one idempotent server endpoint");
    expect(gw_wt_audio_delivery_record(&delivery, 0, 640u) == GW_WT_OK &&
               delivery.drained_datagrams == 1u &&
               delivery.drained_audio_bytes == 640u &&
               gw_wt_audio_delivery_matches(&delivery, 2u, 1280u),
           "audio delivery drains trailing PCM without losing receipt totals");
    expect(!gw_wt_audio_delivery_matches(&delivery, 1u, 640u) &&
               gw_wt_audio_delivery_record(&delivery, 1, 640u) ==
                   GW_WT_ERR_MALFORMED &&
               gw_wt_audio_delivery_record(&delivery, 0, 0u) ==
                   GW_WT_ERR_ARGUMENT,
           "audio delivery rejects incomplete totals and route drift");
    memset(&delivery, 0, sizeof(delivery));
    expect(gw_wt_audio_delivery_mark_endpoint(NULL) == -1 &&
               gw_wt_audio_delivery_should_forward(NULL) == 0 &&
               gw_wt_audio_delivery_record(NULL, 1, 640u) ==
                   GW_WT_ERR_ARGUMENT &&
               gw_wt_audio_delivery_record(&delivery, 2, 640u) ==
                   GW_WT_ERR_ARGUMENT &&
               gw_wt_audio_delivery_record(&delivery, 0, 640u) ==
                   GW_WT_ERR_MALFORMED &&
               !gw_wt_audio_delivery_matches(NULL, 1u, 640u),
           "audio delivery rejects invalid state and pre-endpoint drain");
    delivery.forwarded_datagrams = UINT32_MAX;
    expect(gw_wt_audio_delivery_record(&delivery, 1, 640u) ==
               GW_WT_ERR_CAPACITY,
           "audio delivery rejects a datagram counter overflow");
    memset(&delivery, 0, sizeof(delivery));
    delivery.forwarded_audio_bytes = GW_WT_MAX_AUDIO_BYTES;
    expect(gw_wt_audio_delivery_record(&delivery, 1, 640u) ==
               GW_WT_ERR_CAPACITY,
           "audio delivery rejects an audio byte counter overflow");
    memset(&delivery, 0, sizeof(delivery));
    delivery.forwarded_audio_bytes = GW_WT_MAX_AUDIO_BYTES;
    delivery.drained_audio_bytes = 2u;
    expect(!gw_wt_audio_delivery_matches(
               &delivery, 1u, GW_WT_MAX_AUDIO_BYTES),
           "audio delivery rejects an overflowing aggregate receipt");
    memset(audio, 0x33, 640u);
    expect(gw_wt_audio_reorder_insert(&reorder, 3u, audio, 640u) ==
               GW_WT_AUDIO_INSERTED &&
               gw_wt_audio_reorder_commit_state(&reorder, 4u, 2560u) ==
                   GW_WT_AUDIO_COMMIT_WAIT,
           "end receipt waits for a missing in-window sequence");
    audio[0] = 0x44;
    expect(gw_wt_audio_reorder_insert(&reorder, 3u, audio, 640u) ==
               GW_WT_AUDIO_CONFLICT,
           "conflicting duplicate PCM is rejected");
    memset(audio, 0x55, 640u);
    expect(gw_wt_audio_reorder_insert(&reorder, 2u, audio, 640u) ==
               GW_WT_AUDIO_INSERTED,
           "missing packet closes a later gap");
    {
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        expect(gw_wt_audio_reorder_peek(&reorder, &payload, &payload_len) &&
                   gw_wt_audio_reorder_pop(&reorder) &&
                   gw_wt_audio_reorder_peek(&reorder, &payload, &payload_len) &&
                   gw_wt_audio_reorder_pop(&reorder) &&
                   gw_wt_audio_reorder_commit_state(&reorder, 4u, 2560u) ==
                       GW_WT_AUDIO_COMMIT_READY,
               "reordered window becomes commit-ready after contiguous drain");
    }
    expect(gw_wt_audio_reorder_insert(
               &reorder, reorder.next_sequence + GW_WT_AUDIO_REORDER_WINDOW,
               audio, 640u) == GW_WT_AUDIO_OUTSIDE_WINDOW,
           "packet outside bounded reorder window is rejected");

    {
        turn_response_state response;
        turn_event_c event;
        turn_response_event safe;
        turn_event_c decoded;
        static uint8_t event_audio[640];
        uint8_t wire[TURN_RESPONSE_MAX_AUDIO_EVENT + 8192u];
        size_t wire_len;
        memset(&response, 0, sizeof(response));
        response.wait_for_pcm = 1;

        set_event(&event, 2, "thinking_started");
        memcpy(event.text, "retrieve_then_escalate", sizeof("retrieve_then_escalate"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   safe.text[0] == '\0' && safe.display_text[0] == '\0',
               "response edge strips internal route text");

        set_event(&event, 2, "thinking_started");
        memcpy(event.text, "internal RAG hits", sizeof("internal RAG hits"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_DROP,
               "response edge drops repeated thinking start");

        set_event(&event, 6, "tts_segment");
        memcpy(event.text, "internal speech", sizeof("internal speech"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_DROP,
               "response edge drops internal TTS segments");

        set_event(&event, 5, "text_completed");
        memcpy(event.text, "speech <laugh>", sizeof("speech <laugh>"));
        memcpy(event.display_text, "Safe answer.", sizeof("Safe answer."));
        memcpy(event.speech_text, "speech <laugh>", sizeof("speech <laugh>"));
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   strcmp(safe.text, "Safe answer.") == 0 &&
                   strcmp(safe.display_text, "Safe answer.") == 0 &&
                   safe.text_len == sizeof("Safe answer.") - 1u &&
                   safe.text_json_raw == 1u &&
                   safe.speech_text[0] == '\0' && safe.is_final,
               "response edge exposes only display-safe text");
        wire_len = turn_response_protobuf_encode(wire, sizeof(wire), &safe);
        expect(wire_len != 0 && pb_decode_turn_event(wire, wire_len, &decoded) == 0 &&
                   strcmp(decoded.text, "Safe answer.") == 0 &&
                   strcmp(decoded.display_text, "Safe answer.") == 0 &&
                   decoded.speech_text[0] == '\0' && decoded.is_final,
               "public text event round trip remains display-safe");

        set_event(&event, 10, "completed");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_HOLD_COMPLETED && !response.terminal,
               "model completion waits for final PCM");

        set_event(&event, 7, "pcm_started");
        event.stages.input.audio_committed_at_ms = 93;
        event.stages.input.stt_request_received_at_ms = 94;
        event.stages.input.stt_provider_request_started_at_ms = 95;
        event.stages.input.stt_provider_ready_at_ms = 96;
        event.stages.input.stt_transcript_published_at_ms = 97;
        event.stages.first_text_at_ms = 98;
        event.stages.tts_segment_emitted_at_ms = 99;
        event.stages.tts_request_received_at_ms = 100;
        event.stages.tts_provider_request_started_at_ms = 101;
        event.stages.tts_provider_ready_at_ms = 102;
        event.stages.pcm_started_at_ms = 103;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   safe.stages == &event.stages &&
                   safe.stages->pcm_started_at_ms == 103,
               "PCM start opens one response stream");

        memset(event_audio, 0x33, sizeof(event_audio));
        set_event(&event, 8, "pcm_chunk");
        event.audio = event_audio;
        event.audio_len = sizeof(event_audio) - 1u;
        event.sample_rate = 16000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        event.stages.input.audio_committed_at_ms = 93;
        event.stages.input.stt_request_received_at_ms = 94;
        event.stages.input.stt_provider_request_started_at_ms = 95;
        event.stages.input.stt_provider_ready_at_ms = 96;
        event.stages.input.stt_transcript_published_at_ms = 97;
        event.stages.first_text_at_ms = 98;
        event.stages.tts_segment_emitted_at_ms = 99;
        event.stages.tts_request_received_at_ms = 100;
        event.stages.tts_provider_request_started_at_ms = 101;
        event.stages.tts_provider_ready_at_ms = 102;
        event.stages.pcm_started_at_ms = 103;
        event.stages.pcm_first_chunk_at_ms = 104;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "odd PCM response is rejected");

        event.audio_len = sizeof(event_audio);
        event.stages.tts_provider_ready_at_ms = 99;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "out-of-order PCM stage metadata is rejected");
        event.stages.tts_provider_ready_at_ms = 102;
        event.stages.tts_segment_emitted_at_ms = 97;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "out-of-order upstream stage metadata is rejected");
        event.stages.tts_segment_emitted_at_ms = 99;
        event.stages.input.stt_provider_ready_at_ms = 0;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "partial STT stage metadata is rejected");
        event.stages.input.stt_provider_ready_at_ms = 96;
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   safe.audio == event_audio && safe.audio_len == sizeof(event_audio) &&
                   safe.audio_encoding == 1 && response.final_pcm_seen,
               "canonical final PCM response passes intact");
        wire_len = turn_response_protobuf_encode(wire, sizeof(wire), &safe);
        expect(wire_len != 0 && pb_decode_turn_event(wire, wire_len, &decoded) == 0 &&
                   decoded.audio_len == sizeof(event_audio) &&
                   memcmp(decoded.audio, event_audio, sizeof(event_audio)) == 0 &&
                   decoded.sample_rate == 16000 && decoded.channels == 1 &&
                   decoded.bit_depth == 16 && decoded.audio_encoding == 1 &&
                   decoded.is_final &&
                   decoded.stages.input.audio_committed_at_ms == 93 &&
                   decoded.stages.input.stt_request_received_at_ms == 94 &&
                   decoded.stages.input.stt_provider_request_started_at_ms == 95 &&
                   decoded.stages.input.stt_provider_ready_at_ms == 96 &&
                   decoded.stages.input.stt_transcript_published_at_ms == 97 &&
                   decoded.stages.first_text_at_ms == 98 &&
                   decoded.stages.tts_segment_emitted_at_ms == 99 &&
                   decoded.stages.tts_request_received_at_ms == 100 &&
                   decoded.stages.tts_provider_request_started_at_ms == 101 &&
                   decoded.stages.tts_provider_ready_at_ms == 102 &&
                   decoded.stages.pcm_started_at_ms == 103 &&
                   decoded.stages.pcm_first_chunk_at_ms == 104,
               "public PCM event round trip preserves canonical format");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "PCM after the final chunk is rejected");

        set_event(&event, 9, "pcm_ended");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD_AND_COMPLETE && response.terminal &&
                   response.final_pcm_ended,
               "final PCM end releases the held completion");
        set_event(&event, 11, "canceled");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "response events after a terminal are rejected");
    }

    {
        static const char *const types[] = {
            "", "started", "thinking_started", "thinking_ended", "text_delta",
            "text_completed", "tts_segment", "pcm_started", "pcm_chunk", "pcm_ended",
            "completed", "canceled", "failed"
        };
        uint8_t wire[256];
        int type_id;

        for (type_id = 1; type_id <= 12; type_id++) {
            turn_response_state checked_state;
            turn_response_state decoded_state;
            turn_response_state bound_state;
            turn_response_state active_state;
            turn_response_event checked_safe;
            turn_response_event decoded_safe;
            turn_response_event bound_safe;
            turn_response_event active_safe;
            turn_event_c decoded;
            enum turn_response_action checked_action;
            enum turn_response_action decoded_action;
            enum turn_response_action bound_action;
            enum turn_response_action active_action;
            size_t wire_len = pb_encode_turn_event(
                wire, sizeof(wire), "req-decoded", types[type_id], "");

            memset(&checked_state, 0, sizeof(checked_state));
            memset(&decoded_state, 0, sizeof(decoded_state));
            memset(&bound_state, 0, sizeof(bound_state));
            memset(&active_state, 0, sizeof(active_state));
            memset(&checked_safe, 0, sizeof(checked_safe));
            memset(&decoded_safe, 0, sizeof(decoded_safe));
            memset(&bound_safe, 0, sizeof(bound_safe));
            memset(&active_safe, 0, sizeof(active_safe));
            expect(wire_len != 0 &&
                       pb_decode_turn_event(wire, wire_len, &decoded) == 0,
                   "canonical event decodes for response filter equivalence");
            checked_action = turn_response_filter(
                &checked_state, &decoded, &checked_safe);
            decoded_action = turn_response_filter_decoded(
                &decoded_state, &decoded, &decoded_safe);
            bound_action = turn_response_filter_bound_n(
                &bound_state, &decoded, decoded.request_id,
                strlen(decoded.request_id), &bound_safe);
            active_action = turn_response_filter_active_n(
                &active_state, &decoded, decoded.request_id,
                strlen(decoded.request_id), &active_safe);
            expect(checked_action == decoded_action &&
                       memcmp(&checked_state, &decoded_state,
                              sizeof(checked_state)) == 0 &&
                       memcmp(&checked_safe, &decoded_safe,
                              sizeof(checked_safe)) == 0,
                   "decoded response filter matches checked filter");
            expect(decoded_action == bound_action &&
                       memcmp(&decoded_state, &bound_state,
                              sizeof(decoded_state)) == 0 &&
                       memcmp(&decoded_safe, &bound_safe,
                              sizeof(decoded_safe)) == 0,
                   "bound response filter matches decoded filter");
            expect(bound_action == active_action &&
                       memcmp(&bound_state, &active_state,
                              sizeof(bound_state)) == 0 &&
                       memcmp(&bound_safe, &active_safe,
                              sizeof(bound_safe)) == 0,
                   "active response filter matches bound filter");
            if (bound_action != TURN_RESPONSE_DROP) {
                expect(
                    bound_safe.request_id_len == strlen(decoded.request_id) &&
                        bound_safe.type_len == strlen(types[type_id]),
                    "bound response filter carries validated string lengths");
            }
        }
    }

    {
        static const char active_id[] = "req-bound-view";
        turn_response_state response;
        turn_response_event safe;
        turn_event_c event;
        turn_event_c round_trip;
        uint8_t internal_wire[128];
        uint8_t public_wire[128];
        size_t internal_len = pb_encode_turn_event(
            internal_wire, sizeof(internal_wire), active_id, "started", "");
        size_t public_len;
        turn_event_c checked_event;

        memset(&response, 0, sizeof(response));
        memset(&event, 0, sizeof(event));
        memset(&safe, 0, sizeof(safe));
        expect(
            turn_response_filter_bound_n(
                &response, &event, active_id, 0u, &safe) ==
                TURN_RESPONSE_REJECT &&
            turn_response_filter_bound_n(
                &response, &event, active_id, 128u, &safe) ==
                TURN_RESPONSE_REJECT,
            "bound response view rejects invalid prepared lengths");
        expect(
            internal_len != 0u && pb_decode_turn_event_bound(
                internal_wire, internal_len, active_id, sizeof(active_id) - 1u,
                &event) == 0 &&
            turn_response_filter_bound_n(
                &response, &event, active_id, sizeof(active_id) - 1u, &safe) ==
                TURN_RESPONSE_FORWARD &&
            safe.request_id == active_id &&
            safe.request_id_len == sizeof(active_id) - 1u &&
            safe.type_len == sizeof("started") - 1u,
            "bound response view borrows the validated active identifier");
        memset(&checked_event, 0, sizeof(checked_event));
        expect(
            pb_decode_turn_event_active(
                internal_wire, internal_len, active_id,
                sizeof(active_id) - 1u, &checked_event) == 0 &&
            memcmp(&checked_event, &event, sizeof(event)) == 0,
            "active event decoder matches checked request binding");
        public_len = turn_response_protobuf_encode(
            public_wire, sizeof(public_wire), &safe);
        expect(
            public_len != 0u &&
                pb_decode_turn_event(public_wire, public_len, &round_trip) == 0 &&
                strcmp(round_trip.request_id, active_id) == 0,
            "bound response identifier survives synchronous public encoding");
    }

    {
        turn_response_state response;
        turn_response_state initial_response;
        turn_response_event safe;
        turn_response_event initial_safe;
        turn_event_c decoded;
        uint8_t wire[128];
        size_t wire_len = pb_encode_turn_event(
            wire, sizeof(wire), "req-drop", "tts_segment", "internal speech");

        memset(&response, 0, sizeof(response));
        memset(&safe, 0x5a, sizeof(safe));
        initial_response = response;
        initial_safe = safe;
        expect(wire_len != 0 &&
                   pb_decode_turn_event(wire, wire_len, &decoded) == 0 &&
                   turn_response_filter_decoded(&response, &decoded, &safe) ==
                       TURN_RESPONSE_DROP &&
                   memcmp(&response, &initial_response, sizeof(response)) == 0 &&
                   memcmp(&safe, &initial_safe, sizeof(safe)) == 0,
               "decoded internal event drops before public view initialization");

        wire_len = pb_encode_turn_event(
            wire, sizeof(wire), "bad/id", "thinking_started", "");
        memset(&response, 0, sizeof(response));
        expect(wire_len != 0 &&
                   pb_decode_turn_event(wire, wire_len, &decoded) == 0 &&
                   turn_response_filter_decoded(&response, &decoded, &safe) ==
                       TURN_RESPONSE_REJECT,
               "decoded response filter retains identifier validation");
    }

    {
        turn_response_state response;
        turn_event_c event;
        turn_response_event safe;
        turn_event_c decoded;
        static uint8_t event_audio[640];
        uint8_t wire[512];
        size_t wire_len;
        memset(&response, 0, sizeof(response));
        response.wait_for_pcm = 1;

        set_event(&event, 8, "pcm_chunk");
        event.audio = event_audio;
        event.audio_len = sizeof(event_audio);
        event.sample_rate = 16000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "PCM without a start is rejected");

        set_event(&event, 4, "text_delta");
        memcpy(event.text, "legacy speech <laugh>", sizeof("legacy speech <laugh>"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "text without a display-safe channel is rejected");

        set_event(&event, 4, "text_delta");
        event.display_text[0] = (char)0xc3;
        event.display_text[1] = (char)0x28;
        event.display_text[2] = '\0';
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "invalid display UTF-8 is rejected");

        set_event(&event, 4, "text_delta");
        memcpy(event.display_text, "Caf\xc3\xa9.", sizeof("Caf\xc3\xa9."));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   strcmp(safe.display_text, "Caf\xc3\xa9.") == 0 &&
                   safe.text_validated == 1u,
               "valid multibyte display UTF-8 is accepted");

        set_event(&event, 4, "text_delta");
        memcpy(event.display_text, "A\"\\B", sizeof("A\"\\B"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   safe.text_len == sizeof("A\"\\B") - 1u &&
                   safe.text_json_raw == 0u && safe.text_validated == 1u,
               "response edge carries JSON escape proof");

        wire_len = pb_encode_turn_text_event(
            wire, sizeof(wire), "req-public", "text_delta", "A\"\\B", "A\"\\B",
            "A\"\\B", 0, 0);
        memset(&response, 0, sizeof(response));
        expect(
            wire_len != 0u && pb_decode_turn_event_public_active(
                wire, wire_len, "req-public", sizeof("req-public") - 1u,
                &decoded) == 0 && decoded.display_text_borrowed == 1u &&
                decoded.display_text_view != NULL &&
                turn_response_filter_public_active_n(
                    &response, &decoded, "req-public",
                    sizeof("req-public") - 1u, &safe) == TURN_RESPONSE_FORWARD &&
                safe.text_len == sizeof("A\"\\B") - 1u &&
                safe.text_json_raw == 0u,
            "public borrowed text preserves escaped text policy");

        wire_len = pb_encode_turn_text_event(
            wire, sizeof(wire), "req-public", "text_delta", "Caf\xc3\xa9", "Caf\xc3\xa9",
            "Caf\xc3\xa9", 0, 0);
        memset(&response, 0, sizeof(response));
        expect(
            wire_len != 0u && pb_decode_turn_event_public_active(
                wire, wire_len, "req-public", sizeof("req-public") - 1u,
                &decoded) == 0 && decoded.display_text_borrowed == 1u &&
                turn_response_filter_public_active_n(
                    &response, &decoded, "req-public",
                    sizeof("req-public") - 1u, &safe) == TURN_RESPONSE_FORWARD &&
                safe.text_json_raw == 1u,
            "public decoder falls back for valid UTF-8 text");

        wire_len = pb_encode_turn_text_event(
            wire, sizeof(wire), "req-public", "text_delta", "bad\ntext",
            "bad\ntext", "bad\ntext", 0, 0);
        memset(&response, 0, sizeof(response));
        expect(
            wire_len != 0u && pb_decode_turn_event_public_active(
                wire, wire_len, "req-public", sizeof("req-public") - 1u,
                &decoded) == 0 && decoded.display_text_borrowed == 1u &&
                turn_response_filter_public_active_n(
                    &response, &decoded, "req-public",
                    sizeof("req-public") - 1u, &safe) == TURN_RESPONSE_REJECT,
            "public borrowed text preserves control rejection");

        set_event(&event, 4, "text_delta");
        event.display_text_view = "Safe answer.";
        event.display_text_len = sizeof("Safe answer.") - 1u;
        event.display_text_len_known = 1;
        event.display_text_borrowed = 1u;
        memset(&response, 0, sizeof(response));
        expect(
            turn_response_filter_active_n(
                &response, &event, event.request_id,
                sizeof("req-event") - 1u, &safe) == TURN_RESPONSE_REJECT,
            "generic response filter rejects borrowed decoder storage");

        safe.text_validated = 1u;
        set_event(&event, 1, "started");
        memset(&response, 0, sizeof(response));
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD && safe.text_validated == 0u,
               "response edge clears stale text validation proof");

        set_event(&event, 4, "text_delta");
        memcpy(event.display_text, "unsafe\177text", sizeof("unsafe\177text"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "display control bytes are rejected");

        set_event(&event, 4, "text_delta");
        memset(event.display_text, 'A', sizeof(event.display_text));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "unterminated display text is rejected within its bound");

        set_event(&event, 4, "text_delta");
        memset(event.display_text, 'A', sizeof(event.display_text) - 1u);
        event.display_text[sizeof(event.display_text) - 1u] = '\0';
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   safe.text_len == sizeof(event.display_text) - 1u &&
                   safe.text_json_raw == 1u && safe.text_validated == 1u,
               "maximum terminated display text is accepted");

        {
            int lengths_ok = 1;
            size_t length;
            set_event(&event, 4, "text_delta");
            memset(event.display_text, 'A', sizeof(event.display_text));
            for (length = 1u; length < sizeof(event.display_text); ++length) {
                enum turn_response_action action;
                event.display_text[length] = '\0';
                event.display_text_len = length;
                event.display_text_len_known = 1;
                memset(&response, 0, sizeof(response));
                action = turn_response_filter(&response, &event, &safe);
                if (action != TURN_RESPONSE_FORWARD ||
                    safe.text_len != length || safe.text_json_raw != 1u ||
                    safe.text_validated != 1u) {
                    lengths_ok = 0;
                    break;
                }
                event.display_text[length] = 'A';
            }
            expect(lengths_ok,
                   "display text accepts every bounded ASCII length");
        }

        {
            enum { TEXT_SWEEP_LENGTH = 24 };
            int bytes_ok = 1;
            size_t offset;
            unsigned byte;
            for (offset = 0u; offset < TEXT_SWEEP_LENGTH && bytes_ok; ++offset) {
                for (byte = 0u; byte <= 255u; ++byte) {
                    enum turn_response_action action;
                    int expected_forward =
                        byte >= 0x20u && byte < 0x7fu;
                    int expected_raw = byte != (unsigned)'"' &&
                        byte != (unsigned)'\\';
                    set_event(&event, 4, "text_delta");
                    memset(event.display_text, 'A', TEXT_SWEEP_LENGTH);
                    event.display_text[TEXT_SWEEP_LENGTH] = '\0';
                    event.display_text[offset] = (char)byte;
                    event.display_text_len = TEXT_SWEEP_LENGTH;
                    event.display_text_len_known = 1;
                    memset(&response, 0, sizeof(response));
                    action = turn_response_filter(&response, &event, &safe);
                    if ((action == TURN_RESPONSE_FORWARD) != expected_forward ||
                        (expected_forward &&
                         (safe.text_len != TEXT_SWEEP_LENGTH ||
                          safe.text_json_raw != (uint8_t)expected_raw))) {
                        bytes_ok = 0;
                        break;
                    }
                }
            }
            expect(bytes_ok,
                   "display text classifies every byte in every word lane");
        }

        {
            static const char utf8_dragon[] = "\xf0\x9f\x90\x89";
            enum { TEXT_UTF8_LENGTH = 32 };
            int utf8_lanes_ok = 1;
            size_t offset;
            for (offset = 0u;
                 offset + sizeof(utf8_dragon) - 1u <= TEXT_UTF8_LENGTH;
                 ++offset) {
                enum turn_response_action action;
                set_event(&event, 4, "text_delta");
                memset(event.display_text, 'A', TEXT_UTF8_LENGTH);
                memcpy(
                    event.display_text + offset,
                    utf8_dragon,
                    sizeof(utf8_dragon) - 1u);
                event.display_text[TEXT_UTF8_LENGTH] = '\0';
                event.display_text_len = TEXT_UTF8_LENGTH;
                event.display_text_len_known = 1;
                memset(&response, 0, sizeof(response));
                action = turn_response_filter(&response, &event, &safe);
                if (action != TURN_RESPONSE_FORWARD ||
                    safe.text_len != TEXT_UTF8_LENGTH ||
                    safe.text_json_raw != 1u || safe.text_validated != 1u) {
                    utf8_lanes_ok = 0;
                    break;
                }
                event.display_text[offset + sizeof(utf8_dragon) - 2u] = 'A';
                memset(&response, 0, sizeof(response));
                if (turn_response_filter(&response, &event, &safe) !=
                    TURN_RESPONSE_REJECT) {
                    utf8_lanes_ok = 0;
                    break;
                }
            }
            expect(utf8_lanes_ok,
                   "display text validates UTF-8 across every word lane");
        }

        set_event(&event, 4, "text_delta");
        memcpy(event.display_text, "Bounded", sizeof("Bounded"));
        event.display_text_len = sizeof("Bounded") - 2u;
        event.display_text_len_known = 1;
        memset(&response, 0, sizeof(response));
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_REJECT,
               "display text rejects a short cached length");

        event.display_text_len = sizeof("Bounded");
        memset(&response, 0, sizeof(response));
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_REJECT,
               "display text rejects a long cached length");

        set_event(&event, 10, "failed");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "mismatched response type name and enum are rejected");

        set_event(&event, 1, "started");
        memcpy(event.request_id, "player@example", sizeof("player@example"));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "HTTP-compatible request identifiers pass the shared edge");

        set_event(&event, 12, "failed");
        memcpy(event.text, "backend host detail", sizeof("backend host detail"));
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD_TERMINAL && response.terminal &&
                   strcmp(safe.text, "upstream_failed") == 0,
               "failed response hides backend detail");
        wire_len = turn_response_protobuf_encode(wire, sizeof(wire), &safe);
        expect(wire_len != 0 && pb_decode_turn_event(wire, wire_len, &decoded) == 0 &&
                   strcmp(decoded.text, "upstream_failed") == 0 &&
                   strstr(decoded.text, "backend") == NULL,
               "public failure event round trip remains generic");
    }

    {
        turn_response_state response;
        turn_event_c event;
        turn_response_event safe;
        static uint8_t event_audio[640];
        memset(&response, 0, sizeof(response));
        response.wait_for_pcm = 1;
        set_event(&event, 5, "text_completed");
        memcpy(event.display_text, "Two segments.", sizeof("Two segments."));
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "multi-segment response completes public text");
        set_event(&event, 10, "completed");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_HOLD_COMPLETED,
               "multi-segment completion waits for TTS");
        set_event(&event, 7, "pcm_started");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "first PCM segment starts");
        set_event(&event, 8, "pcm_chunk");
        event.audio = event_audio;
        event.audio_len = sizeof(event_audio);
        event.sample_rate = 16000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "non-final PCM segment passes");
        set_event(&event, 9, "pcm_ended");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   !response.final_pcm_ended && !response.terminal,
               "non-final PCM end keeps the turn open");
        set_event(&event, 7, "pcm_started");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "final PCM segment starts");
        set_event(&event, 8, "pcm_chunk");
        event.audio = event_audio;
        event.audio_len = sizeof(event_audio);
        event.sample_rate = 16000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        event.segment_index = 1;
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "sequence may restart in a later PCM segment");
        set_event(&event, 9, "pcm_ended");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD_AND_COMPLETE && response.terminal,
               "final PCM segment releases model completion");
    }

    {
        turn_response_state response;
        turn_event_c event;
        turn_response_event safe;
        static uint8_t event_audio[640];
        memset(&response, 0, sizeof(response));
        response.wait_for_pcm = 1;
        set_event(&event, 5, "text_completed");
        memcpy(event.display_text, "Alternate order.", sizeof("Alternate order."));
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "alternate response completes public text");
        set_event(&event, 7, "pcm_started");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "alternate order starts PCM");
        set_event(&event, 8, "pcm_chunk");
        event.audio = event_audio;
        event.audio_len = sizeof(event_audio);
        event.sample_rate = 16000;
        event.channels = 1;
        event.bit_depth = 16;
        event.audio_encoding = 1;
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "alternate order accepts final PCM");
        set_event(&event, 9, "pcm_ended");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD &&
                   response.final_pcm_ended && !response.terminal,
               "PCM may finish before model completion");
        set_event(&event, 10, "completed");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD_TERMINAL && response.terminal,
               "later model completion closes an ended PCM stream");
    }

    {
        turn_response_state response;
        turn_event_c event;
        turn_response_event safe;
        memset(&response, 0, sizeof(response));
        set_event(&event, 10, "completed");
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "completion without public text is rejected");
        memset(&response, 0, sizeof(response));
        set_event(&event, 5, "text_completed");
        memcpy(event.display_text, "Text only.", sizeof("Text only."));
        event.is_final = 1;
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_FORWARD,
               "text-only response completes public text");
        set_event(&event, 4, "text_delta");
        memcpy(event.display_text, "Late text.", sizeof("Late text."));
        expect(turn_response_filter(&response, &event, &safe) == TURN_RESPONSE_REJECT,
               "text after public text completion is rejected");
        set_event(&event, 10, "completed");
        expect(turn_response_filter(&response, &event, &safe) ==
                   TURN_RESPONSE_FORWARD_TERMINAL && response.terminal,
               "text-only completion remains immediate");
    }

    memset(&ring, 0, sizeof(ring));
    memset(bulk, 0x7a, sizeof(bulk));
    expect(GW_WT_RESPONSE_QUEUE_CAP == 4u * VOICE_BYTE_RING_CAPACITY,
           "WebTransport response ring provides four HTTP queue windows");
    expect(gw_wt_byte_ring_dirty_after_write(
               &ring, 0u, GW_WT_RESPONSE_QUEUE_CAP + 1u) ==
               GW_WT_RESPONSE_QUEUE_CAP,
           "WebTransport response ring tracks its complete scrub bound");
    ring.head = GW_WT_RESPONSE_QUEUE_CAP - 2u;
    ring.size = 1u;
    expect(gw_wt_byte_ring_dirty_after_write(&ring, 0u, 2u) ==
               GW_WT_RESPONSE_QUEUE_CAP,
           "WebTransport wrapped writes require a complete scrub");
    memset(&ring, 0, sizeof(ring));
    expect(gw_wt_byte_ring_write_pair(&ring, bulk, sizeof(bulk), NULL, 0u),
           "response ring accepts a bounded span");
    expect(gw_wt_byte_ring_consume(&ring, sizeof(bulk) - 2u),
           "response ring advances without compaction");
    {
        static const uint8_t first[] = {1u, 2u, 3u, 4u};
        static const uint8_t second[] = {5u, 6u, 7u, 8u};
        const uint8_t *span = NULL;
        size_t span_len = 0;
        size_t size_before;
        expect(gw_wt_byte_ring_write_pair(&ring, first, sizeof(first),
                                           second, sizeof(second)),
               "response ring atomically wraps a frame pair");
        expect(gw_wt_byte_ring_peek(&ring, &span, &span_len) && span_len == 4u &&
                   span[0] == 0x7a && span[1] == 0x7a &&
                   span[2] == 1u && span[3] == 2u,
               "response ring exposes the tail span first");
        expect(gw_wt_byte_ring_consume(&ring, span_len) &&
                   gw_wt_byte_ring_peek(&ring, &span, &span_len) && span_len == 6u &&
                   span[0] == 3u && span[1] == 4u && span[2] == 5u && span[5] == 8u,
               "response ring exposes the wrapped head span second");
        size_before = ring.size;
        expect(!gw_wt_byte_ring_write_pair(&ring, bulk, sizeof(bulk),
                                            second, sizeof(second)) &&
                   ring.size == size_before,
               "response ring preserves state after atomic capacity failure");
        expect(gw_wt_byte_ring_consume(&ring, span_len) && ring.size == 0u &&
                   ring.head == 0u,
               "empty response ring resets its cursor");
    }
    {
        static const uint8_t payload[] = {0xaau, 0xbbu};
        const uint8_t *span = NULL;
        size_t span_len = 0;
        expect(gw_wt_byte_ring_write_frame(&ring, 2u, payload, sizeof(payload)) &&
                   gw_wt_byte_ring_peek(&ring, &span, &span_len) && span_len == 7u &&
                   span[0] == 2u && span[1] == 0u && span[2] == 0u &&
                   span[3] == 0u && span[4] == 2u &&
                   span[5] == 0xaau && span[6] == 0xbbu,
               "response ring writes a canonical frame without a staging copy");
        expect(gw_wt_byte_ring_consume(&ring, span_len) && ring.size == 0u,
               "response ring consumes a directly encoded frame");
    }

    if (failures != 0) return 1;
    printf("ALL PASS WebTransport protocol core\n");
    return 0;
}
