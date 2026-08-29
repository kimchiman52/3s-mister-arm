#!/usr/bin/env bash
set -euo pipefail

# Single entry point for the loader-timing invariance check (task #66):
# build build/host (Debug) -> run the 2x2 latency/barrier matrix -> verdict.
#
# Exit 0 = the simulation is invariant to loader timing WITH the barrier
#          on, and provably NOT invariant with it off (the built-in
#          neutralization control).
#      1 = the barrier did not hold: traces still diverge.
#      2 = harness plumbing failure, or the control failed to diverge (in
#          which case the experiment has no signal and a green result
#          would be meaningless).
#
# Why this exists rather than tools/rollback-determinism: see the module
# docstring in check_ldreq_timing.py and known limit 9 in
# docs/rollback-determinism-harness.md.
#
# LDT_SKIP_BUILD (opt-in, unset by default): skip the configure+build
# block, mirroring the rbd and frame-data harnesses' protocol.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build/host"
BIN_PATH="${BUILD_DIR}/3S-ARM.app/Contents/MacOS/3S-ARM"

# Source-level gate, run before the build so it fails in seconds rather than
# after a full compile. gd3rd.h derives LDREQ_BARRIER_BUDGET_MS from
# GekkoNet's NetStats::DISCONNECT_TIMEOUT, but net.h is C++ and every
# consumer of the budget is C, so no translation unit can see both constants
# and a _Static_assert is unavailable. DISCONNECT_TIMEOUT also lives in
# vendored code that build-deps.sh re-fetches, so it can change without a
# commit to this repo. check_barrier_budget.py reads both values from their
# real definitions and fails if the barrier could outlast the peer's
# disconnect timeout -- the failure that would otherwise turn a slow disk
# into a dropped match with the comment still asserting it cannot happen.
python3 "${SCRIPT_DIR}/check_barrier_budget.py" --repo-root "${REPO_ROOT}"

if [ -z "${LDT_SKIP_BUILD:-}" ]; then
    if [ ! -d "$BUILD_DIR" ]; then
        echo "[ldreq-timing/run.sh] build/host missing, configuring..." >&2
        CC=clang cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    fi
    echo "[ldreq-timing/run.sh] building 3S-ARM (build/host)..." >&2
    cmake --build "$BUILD_DIR" --parallel 8
fi

if [ ! -x "$BIN_PATH" ]; then
    echo "error: expected binary not found at $BIN_PATH after build" >&2
    exit 2
fi

exec python3 "${SCRIPT_DIR}/check_ldreq_timing.py" --binary "$BIN_PATH" "$@"
