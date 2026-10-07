#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
source "$root/benchmarks/retrieval-quality/scripts/native-env.sh"
if [[ $# != 1 ]]; then
  echo "usage: build-c-probe.sh OUTPUT_BINARY" >&2
  exit 2
fi
runtime="$root/voice/c-runtime"
product="$root/product/companions-frontend"
prompts="$root/contracts/prompt-library"
speech="$root/contracts/handler-base/speechsegment"
read -r -a compiler <<< "${CC:-cc}"
read -r -a extra_flags <<< "${CFLAGS:--O2}"
"${compiler[@]}" -std=c11 -O2 -Wall -Wextra -Werror -Wshadow -Wconversion \
  -Wsign-conversion -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wvla \
  -ffunction-sections -fdata-sections "${extra_flags[@]}" \
  -I"$runtime/bus" -I"$runtime/wire" -I"$runtime/common" \
  -I"$product/c-companions" -I"$product/c-entitlements" \
  -I"$prompts/c-prompt" -I"$speech" \
  "$root/benchmarks/retrieval-quality/c_rag_probe.c" \
  "$runtime/bus/vbus.c" "$runtime/common/vbus_subject.c" \
  "$runtime/wire/pb_min.c" "$runtime/common/utf8.c" \
  "$runtime/common/dnd_retrieval.c" "$root/voice/audio-processor/dsp/dynbuf.c" \
  "$runtime/common/dnd_grounding.c" "$runtime/common/openai_min.c" \
  "$runtime/common/http_min.c" "$prompts/c-prompt/prompt.c" \
  "$speech/speech_display_stream.c" "$speech/speech_sanitize.c" \
  "$product/c-companions/cmp_json.c" "$product/c-entitlements/ent_books.c" \
  "$native_gc_flag" -lcrypto -lm -o "$1"
