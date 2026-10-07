#!/usr/bin/env bash
set -euo pipefail
package_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$package_root"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
flags=(-std=c11 -O2 -ffp-contract=off -Wall -Wextra -Werror -Wshadow -Wconversion
  -Wsign-conversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef -Wvla -Iwasm)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CC:-cc}" "${flags[@]}" wasm/audio_kernel.c tests/audio_kernel_native.c -o "$scratch/oracle"
"${JS_RUNTIME:-node}" scripts/audio-kernel-asset.mjs
AUDIO_NATIVE_ORACLE="$scratch/oracle" \
  AUDIO_SDK_BUNDLE="$package_root/../../product/companions-frontend/static/dist/js/ai-sdk-bridge.js" \
  "${JS_RUNTIME:-node}" tests/audioKernel.test.mjs
if [[ ${FUZZ_RUNS:-0} != 0 ]]; then
  clang "${flags[@]}" -fsanitize=fuzzer,address,undefined \
    wasm/audio_kernel.c tests/fuzz_audio_kernel.c -o "$scratch/fuzz"
  mkdir "$scratch/corpus"
  "${JS_RUNTIME:-node}" --input-type=module - "$scratch/corpus" <<'JS'
import { writeFileSync } from 'node:fs';
const capture = Buffer.alloc(16385);
for (let i = 1; i < capture.length; i += 4) capture.writeFloatLE(0.25, i);
writeFileSync(`${process.argv[2]}/capture`, capture);
const playback = Buffer.alloc(16385, 127);
playback[0] = 1;
writeFileSync(`${process.argv[2]}/playback`, playback);
JS
  "$scratch/fuzz" -runs="${FUZZ_RUNS}" -max_len=16385 -print_funcs=0 "$scratch/corpus"
fi
