#!/usr/bin/env bash
set -euo pipefail

rag=${1:?RAG binary is required}
broker=${2:?broker binary is required}
test_dir=$(mktemp -d /tmp/c-rag-startup.XXXXXXXX)
broker_pid=
rag_pid=
cleanup() {
  local status=$?
  if [[ -n "$rag_pid" ]]; then kill "$rag_pid" 2>/dev/null || true; wait "$rag_pid" 2>/dev/null || true; fi
  if [[ -n "$broker_pid" ]]; then kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; fi
  if (( status != 0 )); then
    for log in "$test_dir"/*.log; do
      [[ -f "$log" ]] || continue
      printf 'RAG startup failure: %s\n' "${log##*/}" >&2
      tail -30 "$log" >&2
    done
  fi
  rm -rf "$test_dir"
}
trap cleanup EXIT

# Keep developer backend settings out of this local startup proof.
clean_env=(env)
for name in EMBED_HTTP_URL MILVUS_SEARCH_URL EMBEDDING_MODEL_ID EMBEDDING_DIMENSIONS \
  RAG_SHARED_RULEBOOK_COLLECTION RAG_OWNED_RULEBOOK_COLLECTION RAG_CAMPAIGN_CANON_COLLECTION \
  RAG_SESSION_TRANSCRIPT_COLLECTION RAG_CHARACTER_MEMORY_COLLECTION MILVUS_HTTP_AUTH_TOKEN \
  RAG_HTTP_TIMEOUT_MS RAG_SHARED_RULEBOOK_SEARCH RAG_SHARED_RULEBOOK_RULESET; do
  clean_env+=(-u "$name")
done
clean_env+=("VBUS_PATH=$test_dir/vbus.sock")
config=(EMBED_HTTP_URL=http://127.0.0.1:1/embeddings
  MILVUS_SEARCH_URL=http://127.0.0.1:1/v2/vectordb/entities/search
  EMBEDDING_MODEL_ID=bge-m3 EMBEDDING_DIMENSIONS=1024 RAG_SHARED_RULEBOOK_COLLECTION=reviewed_books)

reject() {
  local result=0
  timeout 5s "${clean_env[@]}" "${config[@]}" "$@" "$rag" >"$test_dir/rejected.log" 2>&1 || result=$?
  [[ "$result" == 1 ]]
  grep -q 'invalid retrieval backend configuration' "$test_dir/rejected.log"
}
for field in EMBED_HTTP_URL MILVUS_SEARCH_URL EMBEDDING_MODEL_ID EMBEDDING_DIMENSIONS RAG_SHARED_RULEBOOK_COLLECTION; do
  reject "$field="
done
reject EMBEDDING_DIMENSIONS=0
reject EMBEDDING_DIMENSIONS=1025
reject EMBEDDING_DIMENSIONS=three
reject RAG_HTTP_TIMEOUT_MS=0
reject RAG_HTTP_TIMEOUT_MS=60001
reject RAG_HTTP_TIMEOUT_MS=three
reject 'RAG_CAMPAIGN_CANON_COLLECTION=books or true'
reject $'EMBEDDING_MODEL_ID=\xff'
reject $'MILVUS_HTTP_AUTH_TOKEN=bad\r\nheader'
reject EMBED_HTTP_URL=not-a-url
reject RAG_SHARED_RULEBOOK_SEARCH=unknown
reject RAG_SHARED_RULEBOOK_SEARCH=BM25
reject RAG_SHARED_RULEBOOK_RULESET=DND-5e-2014
reject 'RAG_SHARED_RULEBOOK_RULESET=dnd" or true'
reject RAG_SHARED_RULEBOOK_RULESET=dnd_5e
reject RAG_SHARED_RULEBOOK_RULESET=dnd-5e-2014 EMBED_HTTP_URL=
reject "RAG_SHARED_RULEBOOK_RULESET=$(printf 'a%.0s' {1..128})"

VBUS_PATH="$test_dir/vbus.sock" "$broker" >"$test_dir/broker.log" 2>&1 &
broker_pid=$!
for _ in {1..100}; do
  [[ -S "$test_dir/vbus.sock" ]] && break
  kill -0 "$broker_pid"
  sleep 0.02
done
[[ -S "$test_dir/vbus.sock" ]]

start_and_stop() {
  local enabled=$1
  shift
  # Clear the previous readiness message before the background child can run.
  : >"$test_dir/ready.log"
  "${clean_env[@]}" "$@" "$rag" >"$test_dir/ready.log" 2>&1 &
  rag_pid=$!
  for _ in {1..100}; do
    grep -q "pure-C scoped retrieval enabled=$enabled" "$test_dir/ready.log" && break
    kill -0 "$rag_pid"
    sleep 0.02
  done
  grep -q "pure-C scoped retrieval enabled=$enabled" "$test_dir/ready.log"
  kill "$rag_pid"
  wait "$rag_pid"
  rag_pid=
}
start_and_stop 0
start_and_stop 1 "${config[@]}" RAG_CHARACTER_MEMORY_COLLECTION=character_memory MILVUS_HTTP_AUTH_TOKEN=local-fixture-token
start_and_stop 1 "${config[@]}" RAG_SHARED_RULEBOOK_SEARCH=bm25
start_and_stop 1 "${config[@]}" RAG_SHARED_RULEBOOK_RULESET=dnd-5e-2014
start_and_stop 1 "${config[@]}" RAG_SHARED_RULEBOOK_SEARCH=bm25 RAG_SHARED_RULEBOOK_RULESET=dnd-5e-2014
echo 'PASS C RAG startup: disabled, valid, and rejected partial or invalid backend settings'
