#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
src_dir="${repo_root}/voice/voice-session-gateway/reflex-c"
dsp_dir="${repo_root}/voice/audio-processor/dsp"
telemetry_dir="${repo_root}/contracts/telemetry-c"
build_dir="$(mktemp -d)"
trap 'rm -rf "${build_dir}"' EXIT

common=(
  -std=c11
  -O3
  -Wall
  -Wextra
  -Werror
  -pedantic
  -D_POSIX_C_SOURCE=200809L
  -I"${src_dir}"
  -I"${dsp_dir}"
  -I"${telemetry_dir}"
)
sources=(
  "${src_dir}/reflex_session_test.c"
  "${src_dir}/reflex_session.c"
  "${dsp_dir}/audio_engine.c"
  "${dsp_dir}/dynbuf.c"
  "${dsp_dir}/pcm_condition.c"
  "${telemetry_dir}/telemetry.c"
)
native_objects=()
sanitize_objects=()
if [[ "$(uname -m)" == "x86_64" ]]; then
  cc "${common[@]}" -mavx2 -c "${dsp_dir}/pcm_condition_avx2.c" \
    -o "${build_dir}/pcm_condition_avx2.o"
  cc "${common[@]}" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -mavx2 \
    -c "${dsp_dir}/pcm_condition_avx2.c" \
    -o "${build_dir}/pcm_condition_avx2-sanitize.o"
  native_objects+=("${build_dir}/pcm_condition_avx2.o")
  sanitize_objects+=("${build_dir}/pcm_condition_avx2-sanitize.o")
fi

if rg -n 'audio_engine_(vad_endpoint|decide_process_reason|update_last_chunk_ns|update_silence_started_ns|set_has_voice)' \
  "${src_dir}"; then
  echo "Gateway parity proofs must use the public Process + MonitorTick lifecycle boundary." >&2
  exit 1
fi

cc "${common[@]}" "${sources[@]}" "${native_objects[@]}" \
  -lm -pthread -o "${build_dir}/reflex-gate"
"${build_dir}/reflex-gate"
python3 "${src_dir}/captured_pcm_replay.py" \
  --self-test \
  --runner "${build_dir}/reflex-gate"

cc "${common[@]}" -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined "${sources[@]}" "${sanitize_objects[@]}" \
  -lm -pthread \
  -o "${build_dir}/reflex-gate-sanitize"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  "${build_dir}/reflex-gate-sanitize"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  python3 "${src_dir}/captured_pcm_replay.py" \
    --self-test \
    --runner "${build_dir}/reflex-gate-sanitize"

if command -v valgrind >/dev/null 2>&1; then
  REFLEX_SKIP_LATENCY_ASSERT=1 valgrind --quiet --error-exitcode=1 \
    --leak-check=full --show-leak-kinds=definite,indirect,possible \
    "${build_dir}/reflex-gate"
  python3 "${src_dir}/captured_pcm_replay.py" \
    --self-test \
    --runner "${build_dir}/reflex-gate" \
    --under-valgrind
fi
