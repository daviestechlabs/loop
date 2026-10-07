#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
build_dir=$(mktemp -d)
binary=${build_dir}/telemetry-audio-timing-test

cleanup() {
  rm -rf "${build_dir}"
}
trap cleanup EXIT

objects=()
if [[ "$(uname -m)" == "x86_64" ]]; then
  cc -std=gnu11 -O2 -Wall -Wextra -Werror -mavx2 \
    -I "${repo_root}/voice/audio-processor/dsp" \
    -c "${repo_root}/voice/audio-processor/dsp/pcm_condition_avx2.c" \
    -o "${build_dir}/pcm_condition_avx2.o"
  objects+=("${build_dir}/pcm_condition_avx2.o")
fi

cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
  -DTELEMETRY_NATIVE_TEST_API=1 \
  -I "${repo_root}/contracts/telemetry-c" \
  -I "${repo_root}/voice/audio-processor/dsp" \
  "${repo_root}/contracts/telemetry-c/tests/timing_test.c" \
  "${repo_root}/contracts/telemetry-c/telemetry.c" \
  "${repo_root}/voice/audio-processor/dsp/audio_engine.c" \
  "${repo_root}/voice/audio-processor/dsp/dynbuf.c" \
  "${repo_root}/voice/audio-processor/dsp/pcm_condition.c" \
  "${objects[@]}" \
  -lm \
  -o "${binary}"

"${binary}"
