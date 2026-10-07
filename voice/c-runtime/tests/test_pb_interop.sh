#!/usr/bin/env bash
set -euo pipefail

fixture=${1:-./c-pb-interop}
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
proto_root="$repo_root/contracts/handler-base/proto"
proto_file=messages/v1/messages.proto
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT

decode_fixture() {
  local fixture_kind=$1
  local message_type=$2
  "$fixture" "$fixture_kind" >"$tmp_dir/$fixture_kind.pb"
  protoc --proto_path="$proto_root" \
    --decode="$message_type" "$proto_file" \
    <"$tmp_dir/$fixture_kind.pb" >"$tmp_dir/$fixture_kind.txt"
}

decode_fixture turn messages.v1.TurnStartRequest
grep -Fq 'request_id: "r-c"' "$tmp_dir/turn.txt"
grep -Fq 'user_id: "u-c"' "$tmp_dir/turn.txt"
grep -Fq 'session_id: "s-c"' "$tmp_dir/turn.txt"
grep -Fq 'text: "hello"' "$tmp_dir/turn.txt"
grep -Fq 'response_subject: "ai.turn.events.r-c"' "$tmp_dir/turn.txt"
grep -Fq 'key: "interaction_profile"' "$tmp_dir/turn.txt"
grep -Fq 'value: "dnd_app"' "$tmp_dir/turn.txt"
grep -Fq 'key: "client_transport"' "$tmp_dir/turn.txt"
grep -Fq 'value: "c-interop"' "$tmp_dir/turn.txt"
grep -Fq 'key: "campaign_id"' "$tmp_dir/turn.txt"
grep -Fq 'value: "campaign-c"' "$tmp_dir/turn.txt"
grep -Fq 'key: "character_id"' "$tmp_dir/turn.txt"
grep -Fq 'value: "character-c"' "$tmp_dir/turn.txt"
grep -Fq 'key: "knowledge_scope"' "$tmp_dir/turn.txt"
grep -Fq 'value: "shared_rulebook"' "$tmp_dir/turn.txt"
grep -Fq 'key: "retrieval_force"' "$tmp_dir/turn.txt"
grep -Fq 'audio_committed_at_ms: 90' "$tmp_dir/turn.txt"
grep -Fq 'stt_request_received_at_ms: 91' "$tmp_dir/turn.txt"
grep -Fq 'stt_provider_request_started_at_ms: 92' "$tmp_dir/turn.txt"
grep -Fq 'stt_provider_ready_at_ms: 93' "$tmp_dir/turn.txt"
grep -Fq 'stt_transcript_published_at_ms: 94' "$tmp_dir/turn.txt"

decode_fixture lifecycle messages.v1.STTLifecycleEvent
grep -Fq 'session_id: "s-c"' "$tmp_dir/lifecycle.txt"
grep -Fq 'type: STT_LIFECYCLE_EVENT_TYPE_TRANSCRIPTION_FAILED' "$tmp_dir/lifecycle.txt"
grep -Fq 'timestamp: 12345' "$tmp_dir/lifecycle.txt"

decode_fixture transcription messages.v1.STTTranscription
grep -Fq 'session_id: "s-c"' "$tmp_dir/transcription.txt"
grep -Fq 'transcript: "hello from C STT"' "$tmp_dir/transcription.txt"
grep -Fq 'is_final: true' "$tmp_dir/transcription.txt"
grep -Fq 'has_voice_activity: true' "$tmp_dir/transcription.txt"
grep -Fq 'state: "final"' "$tmp_dir/transcription.txt"
grep -Fq 'commit_for_turn: true' "$tmp_dir/transcription.txt"
grep -Fq 'stream_state: STT_STREAM_STATE_LISTENING' "$tmp_dir/transcription.txt"
grep -Fq 'audio_committed_at_ms: 100' "$tmp_dir/transcription.txt"
grep -Fq 'stt_request_received_at_ms: 101' "$tmp_dir/transcription.txt"
grep -Fq 'stt_provider_request_started_at_ms: 102' "$tmp_dir/transcription.txt"
grep -Fq 'stt_provider_ready_at_ms: 104' "$tmp_dir/transcription.txt"

decode_fixture event messages.v1.TurnEvent
grep -Fq 'type: TURN_EVENT_TEXT_COMPLETED' "$tmp_dir/event.txt"
grep -Fq 'text: "answer"' "$tmp_dir/event.txt"
grep -Fq 'is_final: true' "$tmp_dir/event.txt"
grep -Fq 'speech_text: "answer <laugh>"' "$tmp_dir/event.txt"
grep -Fq 'display_text: "answer"' "$tmp_dir/event.txt"

decode_fixture tool-event messages.v1.TurnEvent
grep -Fq 'tool_id: "dnd-dice-roll"' "$tmp_dir/tool-event.txt"
grep -Fq 'tool_call_id: "dice-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' "$tmp_dir/tool-event.txt"
grep -Fq 'output_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' "$tmp_dir/tool-event.txt"
grep -Fq 'elapsed_ms: 12' "$tmp_dir/tool-event.txt"

decode_fixture citation-event messages.v1.TurnEvent
grep -Fq 'retrieval {' "$tmp_dir/citation-event.txt"
grep -Fq 'citations {' "$tmp_dir/citation-event.txt"
grep -Fq 'source: "book://players-handbook"' "$tmp_dir/citation-event.txt"
grep -Fq 'source_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' "$tmp_dir/citation-event.txt"
grep -Fq 'grounding_prompt {' "$tmp_dir/citation-event.txt"
grep -Fq 'id: "rag.answer.grounded_response"' "$tmp_dir/citation-event.txt"
grep -Fq 'version: "v3"' "$tmp_dir/citation-event.txt"
grep -Fq 'sha256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"' "$tmp_dir/citation-event.txt"
protoc --proto_path="$proto_root" --encode=messages.v1.TurnEvent "$proto_file" \
  <"$tmp_dir/citation-event.txt" | "$fixture" decode-citation-event

printf '%s\n' \
  'request_id: "r-tool"' \
  'type: TURN_EVENT_TEXT_COMPLETED' \
  'tool_result {' \
  '  tool_id: "dnd-dice-roll"' \
  '  tool_call_id: "dice-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' \
  '  output_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' \
  '}' |
  protoc --proto_path="$proto_root" --encode=messages.v1.TurnEvent "$proto_file" |
  "$fixture" decode-tool-event

decode_fixture tts-segment messages.v1.TurnTTSSegmentRequest
grep -Fq 'request_id: "r-c"' "$tmp_dir/tts-segment.txt"
grep -Fq 'text: "speak"' "$tmp_dir/tts-segment.txt"
grep -Fq 'segment_index: 2' "$tmp_dir/tts-segment.txt"
grep -Fq 'is_final: true' "$tmp_dir/tts-segment.txt"
grep -Fq 'first_text_at_ms: 99' "$tmp_dir/tts-segment.txt"
grep -Fq 'segment_emitted_at_ms: 100' "$tmp_dir/tts-segment.txt"
grep -Fq 'audio_committed_at_ms: 94' "$tmp_dir/tts-segment.txt"
grep -Fq 'stt_request_received_at_ms: 95' "$tmp_dir/tts-segment.txt"
grep -Fq 'stt_provider_request_started_at_ms: 96' "$tmp_dir/tts-segment.txt"
grep -Fq 'stt_provider_ready_at_ms: 97' "$tmp_dir/tts-segment.txt"
grep -Fq 'stt_transcript_published_at_ms: 98' "$tmp_dir/tts-segment.txt"
grep -Fq 'stream_finality_deferred: true' "$tmp_dir/tts-segment.txt"

decode_fixture audio messages.v1.TurnEvent
grep -Fq 'type: TURN_EVENT_PCM_CHUNK' "$tmp_dir/audio.txt"
grep -Fq 'sample_rate: 16000' "$tmp_dir/audio.txt"
grep -Fq 'channels: 1' "$tmp_dir/audio.txt"
grep -Fq 'bit_depth: 16' "$tmp_dir/audio.txt"
grep -Fq 'sequence: 7' "$tmp_dir/audio.txt"
grep -Fq 'segment_index: 3' "$tmp_dir/audio.txt"
grep -Fq 'is_final: true' "$tmp_dir/audio.txt"
grep -Fq 'audio_encoding: AUDIO_ENCODING_PCM_S16LE' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_audio_committed_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_stt_request_received_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_stt_provider_request_started_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_stt_provider_ready_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_stt_transcript_published_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_first_text_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_tts_segment_emitted_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_tts_request_received_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_tts_provider_request_started_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_tts_provider_ready_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_pcm_started_at_ms"' "$tmp_dir/audio.txt"
grep -Fq 'key: "stage_pcm_first_chunk_at_ms"' "$tmp_dir/audio.txt"

decode_fixture rag-request messages.v1.RAGSearchRequest
grep -Fq 'request_id: "r-c"' "$tmp_dir/rag-request.txt"
grep -Fq 'query: "dragon rules"' "$tmp_dir/rag-request.txt"
grep -Fq 'collection: "dnd_rules"' "$tmp_dir/rag-request.txt"
grep -Fq 'top_k: 4' "$tmp_dir/rag-request.txt"
grep -Fq 'enable_rerank: true' "$tmp_dir/rag-request.txt"

decode_fixture rag-response messages.v1.RAGSearchResponse
grep -Fq 'request_id: "r-c"' "$tmp_dir/rag-response.txt"
grep -Fq 'context_text: "bounded context"' "$tmp_dir/rag-response.txt"
grep -Fq 'used_rag: true' "$tmp_dir/rag-response.txt"
grep -Fq 'knowledge_scope: "campaign_canon"' "$tmp_dir/rag-request.txt"
grep -Fq 'campaign_id: "campaign-c"' "$tmp_dir/rag-request.txt"
grep -Fq 'character_id: "character-c"' "$tmp_dir/rag-request.txt"
grep -Fq 'premium: true' "$tmp_dir/rag-request.txt"
grep -Fq 'deadline_unix_ms: 1788655000123' "$tmp_dir/rag-request.txt"
grep -Fq 'provenance {' "$tmp_dir/rag-response.txt"
grep -Fq 'collection: "reviewed_books"' "$tmp_dir/rag-response.txt"
grep -Fq 'source_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' "$tmp_dir/rag-response.txt"
grep -Fq 'page_start: 96' "$tmp_dir/rag-response.txt"
grep -Fq 'score: 0.875' "$tmp_dir/rag-response.txt"
decode_fixture rag-response-bm25 messages.v1.RAGSearchResponse
grep -Fq 'score: 42.25' "$tmp_dir/rag-response-bm25.txt"
grep -Fq 'score_metric: RETRIEVAL_SCORE_METRIC_BM25' "$tmp_dir/rag-response-bm25.txt"
# Protoc re-encodes the BM25 response before the native decoder checks its metric.
sed -e 's/request_id: "r-c"/request_id: "r-in"/' \
  -e 's/context_text: "bounded context"/context_text: "context from protoc"/' \
  "$tmp_dir/rag-response-bm25.txt" |
  protoc --proto_path="$proto_root" --encode=messages.v1.RAGSearchResponse "$proto_file" |
  "$fixture" decode-rag-response-bm25

decode_fixture session-append messages.v1.SessionAppendRequest
grep -Fq 'session_id: "s-c"' "$tmp_dir/session-append.txt"
grep -Fq 'user_id: "u-c"' "$tmp_dir/session-append.txt"
grep -Fq 'role: "user"' "$tmp_dir/session-append.txt"
grep -Fq 'content: "remember"' "$tmp_dir/session-append.txt"
grep -Fq 'timestamp: 12345' "$tmp_dir/session-append.txt"

decode_fixture session-summary messages.v1.SessionSummaryResponse
grep -Fq 'session_id: "s-c"' "$tmp_dir/session-summary.txt"
grep -Fq 'summary: "bounded summary"' "$tmp_dir/session-summary.txt"
grep -Fq 'source: "c-test"' "$tmp_dir/session-summary.txt"

decode_fixture error-response messages.v1.ErrorResponse
grep -Fq 'error: true' "$tmp_dir/error-response.txt"
grep -Fq 'message: "session store capacity reached"' "$tmp_dir/error-response.txt"
grep -Fq 'type: "transient"' "$tmp_dir/error-response.txt"

printf '%s\n' \
  'request_id: "r-in"' \
  'user_id: "u-in"' \
  'session_id: "s-in"' \
  'text: "hello from protoc"' \
  'response_subject: "ai.turn.events.r-in"' \
  'metadata { key: "interaction_profile" value: "dnd_app" }' \
  'metadata { key: "client_transport" value: "protoc-interop" }' \
  'metadata { key: "campaign_id" value: "campaign-in" }' \
  'metadata { key: "character_id" value: "character-in" }' \
  'metadata { key: "knowledge_scope" value: "shared_rulebook" }' \
  'metadata { key: "retrieval_force" value: "true" }' \
  'audio_committed_at_ms: 190' \
  'stt_request_received_at_ms: 191' \
  'stt_provider_request_started_at_ms: 192' \
  'stt_provider_ready_at_ms: 193' \
  'stt_transcript_published_at_ms: 194' |
  protoc --proto_path="$proto_root" --encode=messages.v1.TurnStartRequest "$proto_file" |
  "$fixture" decode-turn

printf '%s\n' \
  'session_id: "s-in"' \
  'transcript: "hello from stt"' \
  'sequence: 7' \
  'is_final: true' \
  'timestamp: 12345' \
  'has_voice_activity: true' \
  'state: "final"' \
  'stream_state: STT_STREAM_STATE_LISTENING' \
  'commit_for_turn: true' \
  'audio_committed_at_ms: 190' \
  'stt_request_received_at_ms: 191' \
  'stt_provider_request_started_at_ms: 192' \
  'stt_provider_ready_at_ms: 193' |
  protoc --proto_path="$proto_root" --encode=messages.v1.STTTranscription "$proto_file" |
  "$fixture" decode-transcript

printf '%s\n' \
  'session_id: "s-in"' \
  'type: STT_LIFECYCLE_EVENT_TYPE_TRANSCRIPTION_FAILED' \
  'timestamp: 12345' |
  protoc --proto_path="$proto_root" --encode=messages.v1.STTLifecycleEvent "$proto_file" |
  "$fixture" decode-lifecycle

printf '%s\n' \
  'request_id: "r-in"' \
  'text: "speak from protoc"' \
  'segment_index: 3' \
  'is_final: true' \
  'audio_committed_at_ms: 194' \
  'stt_request_received_at_ms: 195' \
  'stt_provider_request_started_at_ms: 196' \
  'stt_provider_ready_at_ms: 197' \
  'stt_transcript_published_at_ms: 198' \
  'first_text_at_ms: 199' \
  'segment_emitted_at_ms: 200' \
  'stream_finality_deferred: true' |
  protoc --proto_path="$proto_root" --encode=messages.v1.TurnTTSSegmentRequest "$proto_file" |
  "$fixture" decode-tts-segment

printf '%s\n' \
  'request_id: "r-in"' \
  'type: TURN_EVENT_TEXT_COMPLETED' \
  'text: "answer from protoc"' \
  'speech_text: "spoken from protoc"' \
  'display_text: "display from protoc"' \
  'metadata { key: "stage_audio_committed_at_ms" value: "194" }' \
  'metadata { key: "stage_stt_request_received_at_ms" value: "195" }' \
  'metadata { key: "stage_stt_provider_request_started_at_ms" value: "196" }' \
  'metadata { key: "stage_stt_provider_ready_at_ms" value: "197" }' \
  'metadata { key: "stage_stt_transcript_published_at_ms" value: "198" }' \
  'metadata { key: "stage_first_text_at_ms" value: "199" }' \
  'metadata { key: "stage_tts_segment_emitted_at_ms" value: "200" }' \
  'metadata { key: "stage_tts_request_received_at_ms" value: "201" }' \
  'metadata { key: "stage_tts_provider_request_started_at_ms" value: "202" }' \
  'metadata { key: "stage_tts_provider_ready_at_ms" value: "203" }' \
  'metadata { key: "stage_pcm_started_at_ms" value: "204" }' \
  'metadata { key: "stage_pcm_first_chunk_at_ms" value: "205" }' |
  protoc --proto_path="$proto_root" --encode=messages.v1.TurnEvent "$proto_file" \
  >"$tmp_dir/staged-event.bin"
"$fixture" decode-event <"$tmp_dir/staged-event.bin"

printf '%s\n' \
  'metadata { key: "stage_tts_request_received_at_ms" value: "201" }' |
  protoc --proto_path="$proto_root" --encode=messages.v1.TurnEvent "$proto_file" \
  >"$tmp_dir/duplicate-stage.bin"
if cat "$tmp_dir/staged-event.bin" "$tmp_dir/duplicate-stage.bin" |
    "$fixture" decode-event; then
  echo "FAIL duplicate stage key accepted" >&2
  exit 1
fi

printf '%s\n' \
  'request_id: "r-in"' \
  'query: "rules query"' \
  'collection: "dnd_rules"' \
  'top_k: 6' \
  'knowledge_scope: "campaign_canon"' \
  'campaign_id: "campaign-in"' \
  'character_id: "character-in"' \
  'premium: true' \
  'deadline_unix_ms: 1788655000123' \
  'enable_rerank: true' |
  protoc --proto_path="$proto_root" --encode=messages.v1.RAGSearchRequest "$proto_file" |
  "$fixture" decode-rag-request

printf '%s\n' \
  'request_id: "r-in"' \
  'context_text: "context from protoc"' \
  'documents {' \
  '  content: "Sneak Attack can apply once per turn."' \
  '  source: "book://players-handbook"' \
  '  score: 0.875' \
  '  provenance {' \
  '    source: "book://players-handbook"' \
  '    book_slug: "players-handbook"' \
  '    collection: "reviewed_books"' \
  '    corpus_version: "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' \
  '    embedding_model: "bge-m3"' \
  '    record_id: "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"' \
  '    document_id: "phb-document"' \
  '    content_hash: "3d5d9eb3ca84da74316b9fd72406305d69018ffc9e9df7916d99c345ed390425"' \
  '    source_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' \
  '    section: "Sneak Attack"' \
  '    page_start: 96' \
  '    page_end: 96' \
  '    score: 0.875' \
  '  }' \
  '}' \
  'used_rag: true' |
  protoc --proto_path="$proto_root" --encode=messages.v1.RAGSearchResponse "$proto_file" |
  "$fixture" decode-rag-response

printf '%s\n' \
  'session_id: "s-in"' \
  'user_id: "u-in"' \
  'message {' \
  '  role: "assistant"' \
  '  content: "answer from protoc"' \
  '  timestamp: 12345' \
  '  request_id: "r-in"' \
  '}' |
  protoc --proto_path="$proto_root" --encode=messages.v1.SessionAppendRequest "$proto_file" |
  "$fixture" decode-session-append

printf '%s\n' \
  'session_id: "s-in"' \
  'user_id: "u-in"' \
  'summary: "summary from protoc"' \
  'source: "protoc"' \
  'updated_at: 12345' |
  protoc --proto_path="$proto_root" --encode=messages.v1.SessionSummaryResponse "$proto_file" |
  "$fixture" decode-session-summary

printf '%s\n' \
  'error: true' \
  'message: "invalid session append"' \
  'type: "validation_error"' |
  protoc --proto_path="$proto_root" --encode=messages.v1.ErrorResponse "$proto_file" |
  "$fixture" decode-error-response

printf 'PASS protobuf interoperability (C <-> canonical messages.proto)\n'
