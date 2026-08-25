#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the rollback-determinism harness
# (docs/rollback-determinism-harness.md): build build/host (Debug) ->
# run baseline + rollback captures per scenario -> diff, symbolize,
# report. Exit 0 = no unexplained divergence; 1 = divergence found
# (symbols escaped the rollback save set — see the report); 2 = harness
# plumbing failure.
#
# Usage: tools/rollback-determinism/run.sh [fast|thorough|select] [extra driver args...]
#   fast (default): 2 scenarios x 3 runs — see the driver for runtimes.
#   thorough:       21 scenarios x 3 runs (every selectable character).
#   select:         2 scenarios x 3 runs at PRODUCTION character-select depth.
#
# The three profiles differ in what they are for, not just in size.
#
# `fast` and `thorough` are the SHARED GATE. As of 2026-08-25 (task #69) they
# run character select at PRODUCTION depth 8 (input_prediction_window,
# netplay.c:903-905). The depth is still passed explicitly and echoed in the
# RBD SUMMARY line (select_period=/select_depth=) so it can never be inherited
# silently from a compiled-in default — that invisibility was the substance of
# #63. The select PERIOD stays at 8; see known limit 1 in the harness doc for
# why period 1 is tracked separately.
#
# Depth matters and is not a formality: the task-50 duplicate-load leak
# changes WHICH guard is load-bearing between depth 2 and depth >= 3, because
# by depth 3 the head request has drained and the enqueue-side dedupe no
# longer sees it. A depth-2 matrix passes with the texgroup.c reclaim
# reverted, which is exactly the shape of a test that cannot fail.
#
# `select` therefore now differs from `fast` only in name; it is kept as a
# stable entry point for character-select regression work and so that older
# runbooks and commit messages that name it keep working.
#
# RBD_SKIP_BUILD (opt-in, unset by default): skip the configure+build
# block, mirroring tools/frame-data/run.sh's FDH_SKIP_BUILD protocol.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

MODE="${1:-fast}"
case "$MODE" in
    fast|thorough)
        # Shared-gate cadence. Passed EXPLICITLY rather than inherited from
        # src/main.c: the gate's select-phase depth has to be visible at the
        # call site and in the summary line, not hidden in a compiled-in
        # initializer.
        #
        # RAISED FROM 2 TO 8 on 2026-08-25 (task #69). Production predicts 8
        # frames ahead (input_prediction_window, netplay.c:903-905), so the
        # gate now probes character select at the depth the field runs at.
        # The single finding that used to make depth 8 permanently red --
        # plt_req -- was closed by measurement and carries its reachability
        # proof in allowlist.txt; nothing else diverges at depth 8 that did
        # not already diverge at depth 2. Do NOT lower this back to 2 to make
        # a new finding go away: a new DIVERGENT row here is a real escapee
        # at production prediction depth.
        DRIVER_MODE="$MODE"
        SELECT_ARGS=(--select-rollback-depth 8)
        ;;
    select)
        # Production-depth profile. Same two scenarios as `fast`; only the
        # select-phase depth changes, so a diff against a `fast` run isolates
        # the depth variable and nothing else.
        DRIVER_MODE="fast"
        SELECT_ARGS=(--select-rollback-depth 8)
        ;;
    *)
        echo "usage: $0 [fast|thorough|select] [extra driver args...]" >&2
        exit 2
        ;;
esac
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
    --binary "$BIN_PATH" --mode "$DRIVER_MODE" "${SELECT_ARGS[@]}" "$@"
