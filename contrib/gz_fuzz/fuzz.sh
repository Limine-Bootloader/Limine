#!/bin/bash
# Usage:
#   ./fuzz.sh [duration]
# duration: afl-fuzz wall-clock budget (passed to -V).  Default: 60.
# Pass 0 to run until interrupted.  The following overrides are permissible:
#   CC              AFL compiler wrapper (default: afl-clang-lto)
#   AFL_USE_ASAN    Set to 1 to build with ASAN
#   OUTDIR          afl-fuzz output directory (default: findings)

set -euo pipefail
cd "$(dirname "$0")"
DURATION="${1:-60}"
OUTDIR="${OUTDIR:-findings}"
export CC="${CC:-afl-clang-fast}"
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "error: $CC not on PATH (install AFL++)" >&2
  exit 1
fi
if ! command -v afl-fuzz >/dev/null 2>&1; then
  echo "error: afl-fuzz not on PATH (install AFL++)" >&2
  exit 1
fi
make clean;  make
if [ ! -d corpus ] || [ -z "$(ls -A corpus 2>/dev/null)" ]; then
  echo "error: corpus/ is empty; add at least one .gz seed" >&2
  exit 1
fi
mkdir -p "$OUTDIR"
AFL_ARGS=(-i corpus -o "$OUTDIR")
if [ "$DURATION" != "0" ]; then
  AFL_ARGS+=(-V "$DURATION")
fi
# Muffle the kernel-knob warnings.
export AFL_SKIP_CPUFREQ=1
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
echo "=== Running afl-fuzz (budget: ${DURATION}s) ==="
afl-fuzz "${AFL_ARGS[@]}" -- ./fuzz_gzip
echo "=== Summary ==="
if [ -d "$OUTDIR/default" ]; then
  STATDIR="$OUTDIR/default"
else
  STATDIR="$OUTDIR"
fi
if [ -f "$STATDIR/fuzzer_stats" ]; then
  grep -E '^(execs_done|execs_per_sec|paths_total|unique_crashes|unique_hangs|corpus_count|saved_crashes|saved_hangs)' \
    "$STATDIR/fuzzer_stats" || true
fi
CRASHES=$(find "$OUTDIR" -path '*/crashes/id:*' 2>/dev/null | wc -l)
HANGS=$(find "$OUTDIR" -path '*/hangs/id:*' 2>/dev/null | wc -l)
echo "crashes: $CRASHES"
echo "hangs:   $HANGS"
if [ "$CRASHES" -gt 0 ]; then
  exit 1
fi
