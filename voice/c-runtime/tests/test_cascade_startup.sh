#!/usr/bin/env bash
set -euo pipefail

cascade=${1:?cascade binary is required}
broker=${2:?broker binary is required}
test_dir=$(mktemp -d /tmp/c-cascade-startup.XXXXXXXX)
broker_pid=
cascade_pid=
cleanup() {
  local status=$?
  if [[ -n "$cascade_pid" ]]; then kill "$cascade_pid" 2>/dev/null || true; wait "$cascade_pid" 2>/dev/null || true; fi
  if [[ -n "$broker_pid" ]]; then kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; fi
  if (( status != 0 )); then
    for log in "$test_dir"/*.log; do
      [[ -f "$log" ]] || continue
      printf 'Cascade startup failure: %s\n' "${log##*/}" >&2
      tail -30 "$log" >&2
    done
  fi
  rm -rf "$test_dir"
}
trap cleanup EXIT
clean_env=(env -u TOOL_HTTP_URL -u TOOL_HTTP_AUTH_SECRET -u LLM_GROUNDED_MAX_COMPLETION_TOKENS
  -u PROMPT_LIBRARY_ROOT "VBUS_PATH=$test_dir/vbus.sock" LLM_HTTP_URL=http://127.0.0.1:1/v1/chat/completions)

VBUS_PATH="$test_dir/vbus.sock" "$broker" >"$test_dir/broker.log" 2>&1 &
broker_pid=$!
for _ in {1..100}; do
  [[ -S "$test_dir/vbus.sock" ]] && break
  kill -0 "$broker_pid"
  sleep 0.02
done
[[ -S "$test_dir/vbus.sock" ]]

for value in 0 257 4096 1000000 1000001 01 +8 '8 ' invalid; do
  result=0
  timeout 5s "${clean_env[@]}" "LLM_GROUNDED_MAX_COMPLETION_TOKENS=$value" "$cascade" \
    >"$test_dir/rejected.log" 2>&1 || result=$?
  [[ "$result" == 1 ]]
  grep -q 'invalid grounded completion token configuration' "$test_dir/rejected.log"
done

for value in '' 1 128 256; do
  # Clear the previous readiness message before the background child can run.
  ready_log="$test_dir/ready-${value:-default}.log"
  : >"$ready_log"
  "${clean_env[@]}" "LLM_GROUNDED_MAX_COMPLETION_TOKENS=$value" "$cascade" >"$ready_log" 2>&1 &
  cascade_pid=$!
  for _ in {1..100}; do
    grep -q 'bounded cancelable OpenAI SSE workers' "$ready_log" && break
    kill -0 "$cascade_pid"
    sleep 0.02
  done
  grep -q 'bounded cancelable OpenAI SSE workers' "$ready_log"
  kill "$cascade_pid"
  wait "$cascade_pid"
  cascade_pid=
done
# The model must never start with only part of the canonical prompt bundle.
cp -R ../../contracts/prompt-library "$test_dir/prompts"
for mutation in missing oversized wrong-type; do
  cp ../../contracts/prompt-library/prompts/voice/manifest.json "$test_dir/prompts/prompts/voice/manifest.json"
  cp ../../contracts/prompt-library/prompts/voice/product-response-style.system.txt "$test_dir/prompts/prompts/voice/product-response-style.system.txt"
  case "$mutation" in
    missing) rm "$test_dir/prompts/prompts/voice/product-response-style.system.txt" ;;
    oversized) python3 -c 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text("x" * 4096)' "$test_dir/prompts/prompts/voice/product-response-style.system.txt" ;;
    wrong-type) python3 -c 'import pathlib,sys; p=pathlib.Path(sys.argv[1]); p.write_text(p.read_text().replace("system", "fragment"))' "$test_dir/prompts/prompts/voice/manifest.json" ;;
  esac
  result=0
  timeout 5s "${clean_env[@]}" "PROMPT_LIBRARY_ROOT=$test_dir/prompts" "$cascade" >"$test_dir/style-$mutation.log" 2>&1 || result=$?
  [[ "$result" == 1 ]]
  grep -q 'canonical response style unavailable' "$test_dir/style-$mutation.log"
done
echo 'PASS C cascade startup: completion budgets and complete canonical prompt bundle'
