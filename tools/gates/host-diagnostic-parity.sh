#!/usr/bin/env bash
#
# host-diagnostic-parity -- make the host build see the defect class its own
# libc headers hide.
#
# Task #106. The full rationale, with the measurements that establish it, is in
# the module docstring of fortify_blind_sweep.py next to this file. The short
# version:
#
#   AppleClang, Debian clang-20 and Homebrew clang-22 ALL implement
#   -Wformat-truncation and all enable it under -Wall. They do not disagree.
#   Darwin's <secure/_stdio.h> macro-rewrites snprintf() to
#   __builtin___snprintf_chk() under the default _FORTIFY_SOURCE, and clang's
#   check keys on the snprintf builtin, so the check never runs on macOS. glibc
#   performs no such rewrite, so the identical source warns on Linux.
#
#   There is therefore NO warning flag that closes this gap on the host. The
#   only lever is to compile the tree once with fortification off.
#
# This gate configures the SHIPPED config (ENABLE_NETPLAY=ON,
# NETPLAY_TEST_HOOKS=OFF -- the configuration #76 found nobody was gating on)
# purely to harvest a compile database, then re-runs every translation unit
# with -fsyntax-only and fortification disabled. No object file is produced and
# no shipped artifact changes: _FORTIFY_SOURCE stays on everywhere something is
# actually linked.
#
# Exit codes: 0 clean, 1 diagnostics found, 2 harness error.
#
# Environment:
#   HDP_BUILD_DIR    where to configure (default build/host-diag-parity)
#   HDP_SKIP_CONFIG  reuse an existing compile database instead of configuring
#   HDP_JOBS         parallelism for the sweep

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${ROOT_DIR}"

build_dir="${HDP_BUILD_DIR:-build/host-diag-parity}"
jobs="${HDP_JOBS:-}"
sweep="${SCRIPT_DIR}/fortify_blind_sweep.py"

if [ ! -f "${sweep}" ]; then
    echo "harness error: ${sweep} is missing" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Non-vacuity control, FIRST.
# ---------------------------------------------------------------------------
# Every other gate in this tree that can pass by doing nothing runs a control
# before it runs the real thing (tools/netplay/natmatrix/run_all.sh,
# tools/ldreq-timing/run.sh). This one can pass by doing nothing in at least
# three ways -- an empty compile database, a flag the compiler silently
# ignores, or a compiler that has dropped the check -- so it does the same.
echo "=== non-vacuity control ==="
if ! python3 "${sweep}" --control; then
    echo "RESULT: HARNESS ERROR (control failed)" >&2
    exit 2
fi
echo

# ---------------------------------------------------------------------------
# Configure the shipped config and harvest a compile database.
# ---------------------------------------------------------------------------
if [ -z "${HDP_SKIP_CONFIG:-}" ]; then
    echo "=== configuring shipped config (ENABLE_NETPLAY=ON, NETPLAY_TEST_HOOKS=OFF) ==="
    # A fresh worktree has no build/ at all, so the redirect below would fail
    # before cmake ever ran and report it as a configure failure.
    mkdir -p "$(dirname "${build_dir}")"
    if ! cmake -S . -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_NETPLAY=ON \
        -DNETPLAY_TEST_HOOKS=OFF \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "${build_dir}.configure.log" 2>&1; then
        echo "harness error: cmake configure failed; see ${build_dir}.configure.log" >&2
        tail -20 "${build_dir}.configure.log" >&2
        exit 2
    fi
    echo "configured into ${build_dir}"
    echo
fi

db="${build_dir}/compile_commands.json"
if [ ! -f "${db}" ]; then
    echo "harness error: no compile database at ${db}" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# The sweep.
# ---------------------------------------------------------------------------
echo "=== fortification-off diagnostic sweep ==="
sweep_args=(--compile-db "${db}" --baseline)
if [ -n "${jobs}" ]; then
    sweep_args+=(--jobs "${jobs}")
fi

set +e
python3 "${sweep}" "${sweep_args[@]}"
rc=$?
set -e

echo
case "${rc}" in
0) echo "RESULT: GREEN -- no diagnostic is being hidden by the host's headers" ;;
1) echo "RESULT: RED -- the host build is compiling code that a glibc target diagnoses" >&2 ;;
*) echo "RESULT: HARNESS ERROR" >&2 ;;
esac
exit "${rc}"
