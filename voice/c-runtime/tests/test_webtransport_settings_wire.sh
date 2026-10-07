#!/usr/bin/env bash
set -euo pipefail
source_dir=${1:?pinned LSQUIC source directory required}
test_root=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
# Compile the actual patched upstream writer with an in-memory output sink.
{ echo int; sed -n '/^lsquic_hcso_write_settings (/,/^}/p' "$source_dir/src/liblsquic/lsquic_hcso_writer.c"; } > "$scratch/settings_writer_under_test.inc"
for mode in debug release; do
  mode_flag=
  if [[ $mode == release ]]; then mode_flag=-DNDEBUG; fi
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${mode_flag:+"$mode_flag"} \
    -I"$source_dir/src/liblsquic" -I"$scratch" \
    "$test_root/webtransport_settings_wire_test.c" -o "$scratch/test"
  "$scratch/test"
done
