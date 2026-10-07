#!/usr/bin/env bash
set -euo pipefail

package_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=$(mktemp -d)
trap 'rm -rf -- "$output"' EXIT
for kind in scalar simd; do
  flags=()
  if [[ "$kind" == simd ]]; then flags+=(-msimd128); fi
  "${CLANG:-clang}" --target=wasm32 -std=c11 -O3 -ffp-contract=off \
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
    -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wundef -Wvla \
    -nostdlib -Wl,--no-entry -Wl,--export-memory \
    -Wl,--initial-memory=131072 -Wl,--max-memory=131072 -Wl,--strip-all \
    "${flags[@]}" "$package_root/wasm/audio_kernel.c" -o "$output/$kind.wasm"
done
bun "$package_root/scripts/audio-kernel-asset.mjs" "$output/scalar.wasm" "$output/simd.wasm"
