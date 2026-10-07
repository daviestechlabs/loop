#!/usr/bin/env bash
# Standalone source checks. No lab endpoint, private registry, or microphone.
set -euo pipefail
loop_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$loop_repo"
python3 product/loop/open-source/test_prepare.py
python3 product/loop/open-source/test_dependencies.py
python3 product/loop/open-source/fetch-dependencies.py
cd product/loop
bun scripts/install-vendor.ts
bun node_modules/typescript/bin/tsc --noEmit -p tsconfig.json
bun scripts/build.ts
loop_tests=()
for loop_test in tests/*.test.ts; do
    # This monorepo deployment test is outside the source publication boundary.
    [[ "$loop_test" == tests/build.test.ts ]] || loop_tests+=("$loop_test")
done
bun test "${loop_tests[@]}"
make -C server all test sanitize loop-test-http
python3 open-source/test_http.py
cd "$loop_repo"
make -C voice/c-runtime -j2 test-source
