#!/usr/bin/env bash
set -euo pipefail
repo_root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$repo_root"
: "${CLANG:=clang}"
: "${WASI_SYSROOT:=/usr}"
if [[ ! -f "${WASI_SYSROOT}/lib/wasm32-wasi/libc.a" ]]; then
  echo "Set WASI_SYSROOT to a wasi-libc sysroot (headers and static libc)." >&2
  exit 1
fi
output=$(mktemp -d)
trap 'rm -rf -- "$output"' EXIT
sources=()
while IFS= read -r source; do
  case "$source" in *.c) sources+=("$source");; esac
done < contracts/ai-sdk/wasm/turn_provenance.sources
"$CLANG" --target=wasm32-wasi --sysroot="$WASI_SYSROOT" \
  -std=c11 -O3 -DNDEBUG -Wall -Wextra -Werror -Wshadow -Wconversion \
  -Wsign-conversion -Wstrict-prototypes -Wmissing-prototypes \
  -ffunction-sections -fdata-sections -nostdlib \
  -Wl,--no-entry -Wl,--gc-sections -Wl,--export-memory \
  -Wl,--initial-memory=393216 -Wl,--max-memory=393216 -Wl,--strip-all \
  -Ivoice/c-runtime/wire -Ivoice/c-runtime/common \
  "${sources[@]}" "$WASI_SYSROOT/lib/wasm32-wasi/libc.a" \
  -o "$output/turn_provenance.wasm"
bun contracts/ai-sdk/scripts/turn-provenance-asset.mjs "$output/turn_provenance.wasm"
