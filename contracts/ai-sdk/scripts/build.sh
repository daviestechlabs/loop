#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

if command -v bun >/dev/null 2>&1; then
  if [[ ! -d node_modules/typescript ]]; then
    bun install
  fi
  bun run build
elif command -v npm >/dev/null 2>&1; then
  if [[ ! -d node_modules/typescript ]]; then
    npm install
  fi
  npm run build
else
  echo "ai-sdk build requires bun or npm" >&2
  exit 1
fi