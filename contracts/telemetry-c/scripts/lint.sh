#!/usr/bin/env bash
# C lint for the process-local reflex and telemetry layer.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if command -v clang-format >/dev/null 2>&1; then
  echo ">>> clang-format check (telemetry-c/)"
  find . -name '*.c' -o -name '*.h' | grep -v '/\.' | sort | xargs clang-format --dry-run --Werror || {
    echo "Formatting issues. Fix with: clang-format -i *.c *.h"
    exit 1
  }
else
  echo ">>> clang-format not installed, skipping (install: apt-get install clang-format)"
fi

if command -v clang-tidy >/dev/null 2>&1; then
  echo ">>> clang-tidy (telemetry.c)"
  clang-tidy --quiet --header-filter='.*telemetry-c/.*' \
    telemetry.c \
    -- -I. -std=c11 -D_GNU_SOURCE 2>&1 | cat || true
else
  echo ">>> clang-tidy not installed, skipping (install: apt-get install clang-tidy)"
fi

echo "C lint for telemetry-c complete."
echo "Tip: run from audio-processor dir too (its test.sh also calls into the canonical telemetry-c sources)."
