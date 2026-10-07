#!/usr/bin/env bash
set -euo pipefail
repo_root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$repo_root"
: "${CC:=cc}"
: "${JS_RUNTIME:=node}"
output=$(mktemp -d)
trap 'rm -rf -- "$output"' EXIT
flags=(-std=c11 -O2 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
  -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef -Wvla)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
sources=()
while IFS= read -r source; do
  case "$source" in *.c) sources+=("$source");; esac
done < contracts/ai-sdk/wasm/turn_provenance.sources
"$CC" "${flags[@]}" -Icontracts/ai-sdk/wasm -Ivoice/c-runtime/wire -Ivoice/c-runtime/common \
  contracts/ai-sdk/tests/turnProvenance.native.c "${sources[@]}" \
  voice/c-runtime/common/turn_response_json.c voice/c-runtime/common/stage_json.c \
  voice/c-runtime/common/base64.c -lm -o "$output/native-oracle"
"$JS_RUNTIME" contracts/ai-sdk/scripts/turn-provenance-asset.mjs
PROVENANCE_NATIVE_ORACLE="$output/native-oracle" \
  PROVENANCE_SDK_BUNDLE="$repo_root/product/companions-frontend/static/dist/js/ai-sdk-bridge.js" \
  "$JS_RUNTIME" contracts/ai-sdk/tests/turnProvenance.test.mjs
if [[ "${FUZZ_RUNS:-0}" != 0 ]]; then
  "${FUZZ_CC:-clang}" "${flags[@]}" -fsanitize=fuzzer,address,undefined \
    -Icontracts/ai-sdk/wasm -Ivoice/c-runtime/wire -Ivoice/c-runtime/common \
    contracts/ai-sdk/tests/turnProvenance.fuzz.c "${sources[@]}" -lm -o "$output/fuzz"
  mkdir "$output/corpus"
  for kind in tool citation passage encounter roster initiative; do
    protoc --proto_path=contracts/handler-base/proto --encode=messages.v1.TurnEvent \
      messages/v1/messages.proto < "contracts/ai-sdk/tests/fixtures/${kind}-provenance.textproto" \
      > "$output/corpus/$kind"
  done
  sed 's/score: 0.875/score: 42.25 score_metric: RETRIEVAL_SCORE_METRIC_BM25/' \
    contracts/ai-sdk/tests/fixtures/citation-provenance.textproto |
    protoc --proto_path=contracts/handler-base/proto --encode=messages.v1.TurnEvent \
      messages/v1/messages.proto > "$output/corpus/citation-bm25"
  sed 's/score: 0.875/score: 0 score_metric: RETRIEVAL_SCORE_METRIC_NONE/' \
    contracts/ai-sdk/tests/fixtures/citation-provenance.textproto |
    protoc --proto_path=contracts/handler-base/proto --encode=messages.v1.TurnEvent \
      messages/v1/messages.proto > "$output/corpus/citation-none"
  sed 's/score: 0.875/score: 0.875 excerpt_spans { end: 12 } excerpt_spans { begin: 24 end: 40 }/' \
    contracts/ai-sdk/tests/fixtures/citation-provenance.textproto |
    protoc --proto_path=contracts/handler-base/proto --encode=messages.v1.TurnEvent \
      messages/v1/messages.proto > "$output/corpus/citation-spans"
  "$output/fuzz" -runs="$FUZZ_RUNS" -max_len=131072 -print_funcs=0 "$output/corpus"
fi
