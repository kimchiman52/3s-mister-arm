#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the rollback-determinism harness
# (docs/rollback-determinism-harness.md): build build/host (Debug) ->
# run baseline + rollback captures per scenario -> diff, symbolize,
# report. Exit 0 = no unexplained divergence; 1 = divergence found
# (symbols escaped the rollback save set — see the report); 2 = harness
# plumbing failure.
#
# Usage: tools/rollback-determinism/run.sh [fast|thorough] [extra driver args...]
#   fast (default): 2 scenarios x 3 runs — see the driver for runtimes.
#   thorough:       21 scenarios x 3 runs (every selectable character).
#
# RBD_SKIP_BUILD (opt-in, unset by default): skip the configure+build
# block, mirroring tools/frame-data/run.sh's FDH_SKIP_BUILD protocol.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MODE="${1:-fast}"
if [ "$MODE" != "fast" ] && [ "$MODE" != "thorough" ]; then
    echo "usage: $0 [fast|thorough] [extra driver args...]" >&2
    exit 2
fi
shift || true

BUILD_DIR="${REPO_ROOT}/build/host"
BIN_PATH="${BUILD_DIR}/3S-ARM.app/Contents/MacOS/3S-ARM"

if [ -z "${RBD_SKIP_BUILD:-}" ]; then
    if [ ! -d "$BUILD_DIR" ]; then
        echo "[rbd/run.sh] build/host missing, configuring..." >&2
        CC=clang cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    fi
    echo "[rbd/run.sh] building 3S-ARM (build/host)..." >&2
    cmake --build "$BUILD_DIR" --parallel 8
fi

if [ ! -x "$BIN_PATH" ]; then
    echo "error: expected binary not found at $BIN_PATH after build" >&2
    exit 2
fi

exec python3 "${SCRIPT_DIR}/check_rollback_determinism.py" \
    --binary "$BIN_PATH" --mode "$MODE" "$@"
