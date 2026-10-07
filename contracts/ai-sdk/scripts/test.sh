#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

if command -v bun >/dev/null 2>&1; then
  if [[ ! -d node_modules/typescript ]]; then
    bun install
  fi
  bun scripts/turn-provenance-asset.mjs
  bun scripts/audio-kernel-asset.mjs
  bun run build
  bun tests/audio.test.mjs
  bun tests/audioKernel.test.mjs
  bun tests/interactionRuntime.test.mjs
  bun tests/metadata.test.mjs
  bun tests/productContext.test.mjs
  bun tests/turnStream.test.mjs
  bun tests/turnProvenance.test.mjs
elif command -v npm >/dev/null 2>&1; then
  if [[ ! -d node_modules/typescript ]]; then
    npm install
  fi
  node scripts/turn-provenance-asset.mjs
  node scripts/audio-kernel-asset.mjs
  npm run build
  node tests/audio.test.mjs
  node tests/audioKernel.test.mjs
  node tests/metadata.test.mjs
  node tests/productContext.test.mjs
  node tests/turnStream.test.mjs
  node tests/turnProvenance.test.mjs
else
  echo "ai-sdk test requires bun or npm" >&2
  exit 1
fi
