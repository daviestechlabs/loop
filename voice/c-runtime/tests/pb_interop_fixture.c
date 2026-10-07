#include "pb_min.h"

#include <stdio.h>
#include <string.h>

static int write_bytes(const uint8_t *data, size_t len) {
    return len > 0 && fwrite(data, 1, len, stdout) == len ? 0 : 1;
}

static size_t read_bytes(uint8_t *data, size_t cap) {
    size_t len = fread(data, 1, cap, stdin);
    if (ferror(stdin) || (!feof(stdin) && len == cap)) return 0;
    return len;
}

int main(int argc, char **argv) {
    uint8_t buf[8192];
    size_t len;
    if (argc != 2) return 2;

    if (strcmp(argv[1], "turn") == 0) {
        turn_start_c turn;
        memset(&turn, 0, sizeof(turn));
        memcpy(turn.request_id, "r-c", 4);
        memcpy(turn.user_id, "u-c", 4);
        memcpy(turn.session_id, "s-c", 4);
        memcpy(turn.text, "hello", 6);
        memcpy(turn.response_subject, "ai.turn.events.r-c", 19);
        memcpy(turn.metadata.interaction_profile, "dnd_app", 8);
        memcpy(turn.metadata.client_transport, "c-interop", 10);
        memcpy(turn.metadata.campaign_id, "campaign-c", 11);
        memcpy(turn.metadata.character_id, "character-c", sizeof("character-c"));
        memcpy(turn.metadata.knowledge_scope, "shared_rulebook", 16);
        memcpy(turn.metadata.retrieval_force, "true", 5);
        turn.input_stages.audio_committed_at_ms = 90;
        turn.input_stages.stt_request_received_at_ms = 91;
        turn.input_stages.stt_provider_request_started_at_ms = 92;
        turn.input_stages.stt_provider_ready_at_ms = 93;
        turn.input_stages.stt_transcript_published_at_ms = 94;
        return write_bytes(buf, pb_encode_turn_start(buf, sizeof(buf), &turn));
    }
    if (strcmp(argv[1], "lifecycle") == 0) {
        return write_bytes(
            buf,
            pb_encode_stt_lifecycle(
                buf, sizeof(buf), "s-c", "transcription_failed", 12345)
        );
    }
    if (strcmp(argv[1], "transcription") == 0) {
        stt_transcription_c transcript;
        memset(&transcript, 0, sizeof(transcript));
        memcpy(transcript.session_id, "s-c", 4);
        memcpy(transcript.transcript, "hello from C STT", 17);
        memcpy(transcript.state, "final", 6);
        transcript.sequence = 1;
        transcript.is_final = 1;
        transcript.timestamp_ms = 12345;
        transcript.has_voice_activity = 1;
        transcript.commit_for_turn = 1;
        transcript.stream_state = 1;
        transcript.input_stages.audio_committed_at_ms = 100;
        transcript.input_stages.stt_request_received_at_ms = 101;
        transcript.input_stages.stt_provider_request_started_at_ms = 102;
        transcript.input_stages.stt_provider_ready_at_ms = 104;
        transcript.input_stages.stt_transcript_published_at_ms = 12345;
        return write_bytes(
            buf, pb_encode_stt_transcription(buf, sizeof(buf), &transcript));
    }
    if (strcmp(argv[1], "event") == 0) {
        return write_bytes(
            buf,
            pb_encode_turn_text_event(
                buf, sizeof(buf), "r-c", "text_completed", "answer",
                "answer <laugh>", "answer", 0, 1)
        );
    }
    if (strcmp(argv[1], "tool-event") == 0) {
        turn_tool_result_c tool = {0};
        memcpy(tool.tool_id, "dnd-dice-roll", sizeof("dnd-dice-roll"));
        memcpy(tool.tool_call_id, "dice-", 5u);
        memset(tool.tool_call_id + 5u, 'a', 64u);
        memset(tool.output_sha256, 'b', 64u);
        tool.elapsed_ms = 12;
        tool.present = 1;
        len = pb_encode_turn_text_event(buf, sizeof(buf), "r-tool", "text_completed",
                                        "You rolled 20.", "You rolled 20.", "You rolled 20.", 0, 1);
        return write_bytes(buf, pb_append_turn_tool_result(buf, sizeof(buf), len, &tool));
    }
    if (strcmp(argv[1], "tts-segment") == 0) {
        turn_tts_segment_c segment;
        memset(&segment, 0, sizeof(segment));
        memcpy(segment.request_id, "r-c", 4);
        memcpy(segment.text, "speak", 6);
        memcpy(segment.response_subject, "ai.turn.events.r-c", 19);
        segment.segment_index = 2;
        segment.is_final = 1;
        segment.output_sample_rate = 24000;
        segment.output_channels = 1;
        segment.output_bit_depth = 16;
        segment.output_encoding = 1;
        segment.input_stages.audio_committed_at_ms = 94;
        segment.input_stages.stt_request_received_at_ms = 95;
        segment.input_stages.stt_provider_request_started_at_ms = 96;
        segment.input_stages.stt_provider_ready_at_ms = 97;
        segment.input_stages.stt_transcript_published_at_ms = 98;
        segment.first_text_at_ms = 99;
        segment.segment_emitted_at_ms = 100;
        segment.stream_finality_deferred = 1;
        return write_bytes(
            buf, pb_encode_turn_tts_segment(buf, sizeof(buf), &segment));
    }
    if (strcmp(argv[1], "audio") == 0) {
        static const uint8_t audio[] = {1, 2, 3, 4};
        turn_stage_timestamps_c stages;
        memset(&stages, 0, sizeof(stages));
        stages.input.audio_committed_at_ms = 94;
        stages.input.stt_request_received_at_ms = 95;
        stages.input.stt_provider_request_started_at_ms = 96;
        stages.input.stt_provider_ready_at_ms = 97;
        stages.input.stt_transcript_published_at_ms = 98;
        stages.first_text_at_ms = 99;
        stages.tts_segment_emitted_at_ms = 100;
        stages.tts_request_received_at_ms = 101;
        stages.tts_provider_request_started_at_ms = 102;
        stages.tts_provider_ready_at_ms = 103;
        stages.pcm_started_at_ms = 104;
        stages.pcm_first_chunk_at_ms = 105;
        return write_bytes(
            buf,
            pb_encode_turn_audio_event_stages(
                buf, sizeof(buf), "r-c", "pcm_chunk", "", audio, sizeof(audio),
                16000, 1, 16, 7, 3, 1, &stages)
        );
    }
    if (strcmp(argv[1], "rag-request") == 0) {
        rag_search_request_c req;
        memset(&req, 0, sizeof(req));
        memcpy(req.request_id, "r-c", 4);
        memcpy(req.user_id, "u-c", 4);
        memcpy(req.query, "dragon rules", 13);
        memcpy(req.collection, "dnd_rules", 10);
        memcpy(req.session_id, "s-c", 4);
        strcpy(req.knowledge_scope, "campaign_canon");
        strcpy(req.campaign_id, "campaign-c");
        strcpy(req.character_id, "character-c");
        req.premium = 1;
        req.deadline_unix_ms = 1788655000123LL;
        req.top_k = 4;
        req.rerank_top_k = 2;
        req.enable_rerank = 1;
        return write_bytes(buf, pb_encode_rag_search_request(buf, sizeof(buf), &req));
    }
    if (strcmp(argv[1], "rag-response") == 0 || strcmp(argv[1], "rag-response-bm25") == 0 ||
        strcmp(argv[1], "citation-event") == 0) {
        rag_search_response_c response;
        memset(&response, 0, sizeof(response));
        memcpy(response.request_id, "r-c", 4);
        memcpy(response.context_text, "bounded context", 16);
        response.used_rag = 1;
        response.documents.count = 1u;
        strcpy(response.documents.hits[0].content, "Sneak Attack can apply once per turn.");
        {
            dnd_rag_citation *citation = &response.documents.hits[0].citation;
            strcpy(citation->source, "book://players-handbook");
            strcpy(citation->book_slug, "players-handbook");
            strcpy(citation->collection, "reviewed_books");
            strcpy(citation->corpus_version, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
            strcpy(citation->embedding_model, "bge-m3");
            strcpy(citation->record_id, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
            strcpy(citation->document_id, "phb-document");
            strcpy(citation->content_hash, "3d5d9eb3ca84da74316b9fd72406305d69018ffc9e9df7916d99c345ed390425");
            strcpy(citation->source_sha256, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
            strcpy(citation->section, "Sneak Attack");
            citation->page_start = citation->page_end = 96;
            citation->score = 0.875;
            if (strcmp(argv[1], "rag-response-bm25") == 0) {
                citation->score = 42.25;
                citation->score_metric = DND_RAG_SCORE_BM25;
            }
        }
        if (strcmp(argv[1], "citation-event") == 0) {
            uint8_t provenance[DND_RAG_CITATIONS_WIRE_CAP];
            turn_retrieval_c view;
            dnd_grounding_identity identity = {DND_GROUNDING_PROMPT_ID, "v3",
                "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"};
            size_t length = pb_encode_grounded_retrieval_provenance(provenance, sizeof(provenance),
                &response.documents, &identity);
            if (!length || pb_decode_retrieval_provenance(provenance, length, &view) != 0) return 1;
            len = pb_encode_turn_text_event(buf, sizeof(buf), "r-c", "text_completed",
                "Rule [1].", "Rule [1].", "Rule [1].", 0, 1);
            return write_bytes(buf, pb_append_turn_retrieval(buf, sizeof(buf), len, &view));
        }
        return write_bytes(buf, pb_encode_rag_search_response(buf, sizeof(buf), &response));
    }
    if (strcmp(argv[1], "session-append") == 0) {
        session_append_request_c request;
        memset(&request, 0, sizeof(request));
        memcpy(request.session_id, "s-c", 4);
        memcpy(request.user_id, "u-c", 4);
        memcpy(request.message.role, "user", 5);
        memcpy(request.message.content, "remember", 9);
        memcpy(request.message.request_id, "r-c", 4);
        request.message.timestamp_ms = 12345;
        return write_bytes(
            buf, pb_encode_session_append_request(buf, sizeof(buf), &request));
    }
    if (strcmp(argv[1], "session-summary") == 0) {
        return write_bytes(
            buf, pb_encode_session_summary_response(
                buf, sizeof(buf), "s-c", "u-c", "bounded summary", "c-test", 12345));
    }
    if (strcmp(argv[1], "error-response") == 0) {
        return write_bytes(
            buf, pb_encode_error_response(
                buf, sizeof(buf), "session store capacity reached", "transient"));
    }

    len = read_bytes(buf, sizeof(buf));
    if (len == 0) return 2;
    if (strcmp(argv[1], "decode-citation-event") == 0) {
        turn_event_c event;
        dnd_rag_citation citation;
        dnd_grounding_identity identity;
        pb_reader reader;
        if (pb_decode_turn_event_bound(buf, len, "r-c", 3u, &event) != 0 ||
            event.type_id != 5 || !event.is_final || event.retrieval.count != 1u ||
            pb_retrieval_grounding(&event.retrieval, &identity) != 1 ||
            strcmp(identity.id, DND_GROUNDING_PROMPT_ID) != 0 || strcmp(identity.version, "v3") != 0 ||
            strcmp(identity.sha256, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee") != 0) return 1;
        pb_reader_init(&reader, event.retrieval.data, event.retrieval.length);
        return pb_retrieval_citation_next(&reader, &citation) == 1 &&
            strcmp(citation.source, "book://players-handbook") == 0 &&
            strcmp(citation.source_sha256, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0 &&
            citation.page_start == 96 && citation.page_end == 96 && citation.score == 0.875 ? 0 : 1;
    }
    if (strcmp(argv[1], "decode-turn") == 0) {
        turn_start_c turn;
        return pb_decode_turn_start(buf, len, &turn) == 0 &&
                       strcmp(turn.request_id, "r-in") == 0 &&
                       strcmp(turn.user_id, "u-in") == 0 &&
                       strcmp(turn.session_id, "s-in") == 0 &&
                       strcmp(turn.text, "hello from protoc") == 0 &&
                       strcmp(turn.response_subject, "ai.turn.events.r-in") == 0 &&
                       strcmp(turn.metadata.interaction_profile, "dnd_app") == 0 &&
                       strcmp(turn.metadata.client_transport, "protoc-interop") == 0 &&
                       strcmp(turn.metadata.campaign_id, "campaign-in") == 0 &&
                       strcmp(turn.metadata.character_id, "character-in") == 0 &&
                       strcmp(turn.metadata.knowledge_scope, "shared_rulebook") == 0 &&
                       strcmp(turn.metadata.retrieval_force, "true") == 0 &&
                       turn.input_stages.audio_committed_at_ms == 190 &&
                       turn.input_stages.stt_request_received_at_ms == 191 &&
                       turn.input_stages.stt_provider_request_started_at_ms == 192 &&
                       turn.input_stages.stt_provider_ready_at_ms == 193 &&
                       turn.input_stages.stt_transcript_published_at_ms == 194
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-transcript") == 0) {
        stt_transcription_c transcript;
        return pb_decode_stt_transcription(buf, len, &transcript) == 0 &&
                       strcmp(transcript.session_id, "s-in") == 0 &&
                       strcmp(transcript.transcript, "hello from stt") == 0 &&
                       transcript.sequence == 7 && transcript.is_final == 1 &&
                       transcript.has_voice_activity == 1 &&
                       strcmp(transcript.state, "final") == 0 &&
                       transcript.commit_for_turn == 1 && transcript.stream_state == 1 &&
                       transcript.input_stages.audio_committed_at_ms == 190 &&
                       transcript.input_stages.stt_request_received_at_ms == 191 &&
                       transcript.input_stages.stt_provider_request_started_at_ms == 192 &&
                       transcript.input_stages.stt_provider_ready_at_ms == 193 &&
                       transcript.input_stages.stt_transcript_published_at_ms == 12345
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-tts-segment") == 0) {
        turn_tts_segment_c segment;
        return pb_decode_turn_tts_segment(buf, len, &segment) == 0 &&
                       strcmp(segment.request_id, "r-in") == 0 &&
                       segment.request_id_len == strlen(segment.request_id) &&
                       strcmp(segment.text, "speak from protoc") == 0 &&
                       segment.text_len == strlen(segment.text) &&
                       segment.segment_index == 3 && segment.is_final == 1 &&
                       segment.input_stages.audio_committed_at_ms == 194 &&
                       segment.input_stages.stt_request_received_at_ms == 195 &&
                       segment.input_stages.stt_provider_request_started_at_ms == 196 &&
                       segment.input_stages.stt_provider_ready_at_ms == 197 &&
                       segment.input_stages.stt_transcript_published_at_ms == 198 &&
                       segment.first_text_at_ms == 199 &&
                       segment.segment_emitted_at_ms == 200 &&
                       segment.stream_finality_deferred == 1
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-lifecycle") == 0) {
        stt_lifecycle_c life;
        return pb_decode_stt_lifecycle(buf, len, &life) == 0 &&
                       strcmp(life.session_id, "s-in") == 0 &&
                       life.type_id == STT_LIFECYCLE_TRANSCRIPTION_FAILED &&
                       life.timestamp_ms == 12345
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-tool-event") == 0) {
        turn_event_c event;
        return pb_decode_turn_event(buf, len, &event) == 0 && event.type_id == 5 &&
            strcmp(event.request_id, "r-tool") == 0 && event.tool_result.present &&
            event.tool_result.elapsed_ms == 0 &&
            strcmp(event.tool_result.tool_id, "dnd-dice-roll") == 0 &&
            strcmp(event.tool_result.tool_call_id,
                "dice-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0 &&
            strcmp(event.tool_result.output_sha256,
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "decode-event") == 0) {
        turn_event_c event;
        return pb_decode_turn_event(buf, len, &event) == 0 &&
                       strcmp(event.request_id, "r-in") == 0 && event.type_id == 5 &&
                       strcmp(event.text, "answer from protoc") == 0 &&
                       strcmp(event.speech_text, "spoken from protoc") == 0 &&
                       strcmp(event.display_text, "display from protoc") == 0 &&
                       event.stages.input.audio_committed_at_ms == 194 &&
                       event.stages.input.stt_request_received_at_ms == 195 &&
                       event.stages.input.stt_provider_request_started_at_ms == 196 &&
                       event.stages.input.stt_provider_ready_at_ms == 197 &&
                       event.stages.input.stt_transcript_published_at_ms == 198 &&
                       event.stages.first_text_at_ms == 199 &&
                       event.stages.tts_segment_emitted_at_ms == 200 &&
                       event.stages.tts_request_received_at_ms == 201 &&
                       event.stages.tts_provider_request_started_at_ms == 202 &&
                       event.stages.tts_provider_ready_at_ms == 203 &&
                       event.stages.pcm_started_at_ms == 204 &&
                       event.stages.pcm_first_chunk_at_ms == 205
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-rag-request") == 0) {
        rag_search_request_c req;
        return pb_decode_rag_search_request(buf, len, &req) == 0 &&
                       strcmp(req.request_id, "r-in") == 0 &&
                       strcmp(req.query, "rules query") == 0 &&
                       strcmp(req.collection, "dnd_rules") == 0 &&
                       req.top_k == 6 && req.enable_rerank == 1 && req.premium == 1 &&
                       strcmp(req.knowledge_scope, "campaign_canon") == 0 &&
                       strcmp(req.campaign_id, "campaign-in") == 0 &&
                       strcmp(req.character_id, "character-in") == 0 &&
                       req.deadline_unix_ms == 1788655000123LL
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "roundtrip-citation") == 0) {
        dnd_rag_citation citation;
        uint8_t encoded[8192];
        if (pb_decode_retrieval_citation(buf, len, &citation)) return 1;
        size_t size = pb_encode_retrieval_citation(encoded, sizeof(encoded), &citation);
        return size ? write_bytes(encoded, size) : 1;
    }
    if (strcmp(argv[1], "decode-rag-response") == 0 || strcmp(argv[1], "decode-rag-response-bm25") == 0) {
        rag_search_response_c response;
        int bm25 = strcmp(argv[1], "decode-rag-response-bm25") == 0;
        return pb_decode_rag_search_response(buf, len, &response) == 0 &&
                       strcmp(response.request_id, "r-in") == 0 &&
                       strcmp(response.context_text, "context from protoc") == 0 &&
                       response.used_rag == 1 && response.documents.count == 1u &&
                       strcmp(response.documents.hits[0].content, "Sneak Attack can apply once per turn.") == 0 &&
                       strcmp(response.documents.hits[0].citation.collection, "reviewed_books") == 0 &&
                       strcmp(response.documents.hits[0].citation.source_sha256,
                           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0 &&
                       response.documents.hits[0].citation.page_start == 96 &&
                       response.documents.hits[0].citation.score == (bm25 ? 42.25 : 0.875) &&
                       response.documents.hits[0].citation.score_metric == (bm25 ? DND_RAG_SCORE_BM25 : DND_RAG_SCORE_COSINE)
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-session-append") == 0) {
        session_append_request_c request;
        return pb_decode_session_append_request(buf, len, &request) == 0 &&
                       strcmp(request.session_id, "s-in") == 0 &&
                       strcmp(request.user_id, "u-in") == 0 &&
                       strcmp(request.message.role, "assistant") == 0 &&
                       strcmp(request.message.content, "answer from protoc") == 0 &&
                       request.message.timestamp_ms == 12345
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-session-summary") == 0) {
        session_summary_response_c response;
        return pb_decode_session_summary_response(buf, len, &response) == 0 &&
                       strcmp(response.session_id, "s-in") == 0 &&
                       strcmp(response.user_id, "u-in") == 0 &&
                       strcmp(response.summary, "summary from protoc") == 0 &&
                       strcmp(response.source, "protoc") == 0 &&
                       response.updated_at_ms == 12345
                   ? 0
                   : 1;
    }
    if (strcmp(argv[1], "decode-error-response") == 0) {
        error_response_c response;
        return pb_decode_error_response(buf, len, &response) == 0 &&
                       response.error == 1 &&
                       strcmp(response.message, "invalid session append") == 0 &&
                       strcmp(response.type, "validation_error") == 0
                   ? 0
                   : 1;
    }
    return 2;
}
