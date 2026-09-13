#!/usr/bin/env bash
# Native (host-arch) build + run of the maze_seq_core smoke test. No Docker,
# no cross toolchain — this only checks the ported sequencer logic still
# behaves, including the FORCE-ONLY state-path/save-load round-trip.
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# -D_DEFAULT_SOURCE: maze_seq_core.c's background save-worker thread calls
# usleep() (verbatim upstream code) — glibc only declares it under -std=c11
# when a feature-test macro enables XOPEN/BSD extensions.
$CC -O2 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
    -D_DEFAULT_SOURCE -Isrc -std=c11 \
    tests/test_smoke.c src/maze_seq_core.c -lm -lpthread \
    -o "$OUT/test_smoke"

# module_dir doesn't exist yet on purpose - exercises ensure_state_dir()'s
# own mkdir path, same as a freshly-installed addon directory would.
"$OUT/test_smoke" "$OUT/state" | tail -n 40
