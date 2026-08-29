#!/usr/bin/env bash
#
# run-gates -- the declared verification gate set.
#
# WHY THIS FILE IS COMMITTED
# ==========================
#
# The runner this replaces was lane-private scratch (`gate.sh`, deleted by
# ed37cb42 as "throwaway task #103 tooling"). That is precisely the problem it
# was meant to solve: the gate set lived in one agent's temp file and in prose
# inside task briefs, so "the gates" meant whatever the last person happened to
# run. Two defects have now been traced to that shape:
#
#   #76   parked work built only with NETPLAY_TEST_HOOKS=ON and failed the
#         SHIPPED config with three errors, because nothing gated the shipped
#         config.
#   #106  the ed37cb42 merge passed nine harnesses, the shipped-config build
#         and frame-data 94 GREEN, then failed to cross-compile for ARM,
#         because nothing gated the ARM target.
#
# Same shape both times: a configuration nobody gates on goes quietly broken
# while every gate that does run stays green. So the gate set is written down
# here, and -- this is the load-bearing part -- a gate that did NOT run is
# named in the summary rather than omitted from it. A green line that silently
# excludes the ARM target is how #106 happened.
#
# USAGE
#   tools/gates/run-gates.sh              # fast set (no Docker, ~2-4 min)
#   tools/gates/run-gates.sh --arm        # also cross-compile for ARM (slow)
#   tools/gates/run-gates.sh --list       # print the gate set and exit
#
# Exit codes: 0 all run gates green, 1 at least one red, 2 harness error.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${ROOT_DIR}"

run_arm="${GATES_ARM:-0}"
jobs="${GATES_JOBS:-8}"
out_dir="${GATES_OUT:-$(mktemp -d)}"
list_only=0

while [ $# -gt 0 ]; do
    case "$1" in
    --arm) run_arm=1; shift ;;
    --no-arm) run_arm=0; shift ;;
    --jobs) jobs="$2"; shift 2 ;;
    --out) out_dir="$2"; shift 2 ;;
    --list) list_only=1; shift ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
mkdir -p "${out_dir}"

GATES=(
    "shipped-config-build|build the SHIPPED config (ENABLE_NETPLAY=ON, NETPLAY_TEST_HOOKS=OFF) -- #76"
    "nptest-build|build the hooks-ON test config the harnesses need"
    "netplay-harnesses|run every --test-* harness with true exit codes"
    "rendezvous-protocol|node tools/rendezvous-server/__test_protocol.js"
    "host-diagnostic-parity|diagnostics the host's fortified libc headers hide -- #106"
    "doc-citation-baselines|tools/doc-citations/check_baselines.py (breach AND slack)"
    "arm-cross-build|cross-compile the shipped config for ARM -- #106 (needs --arm)"
)

if [ "${list_only}" -eq 1 ]; then
    printf '%s\n' "${GATES[@]}" | sed 's/|/\t/'
    exit 0
fi

declare -a NAMES=() STATES=()
FAILED=0

record() {  # record <name> <state>
    NAMES+=("$1"); STATES+=("$2")
    case "$2" in RED|ERROR) FAILED=1 ;; esac
    printf '  %-24s %s\n' "$1" "$2"
}

echo "############ GATE RUN ############"
echo "logs: ${out_dir}"
echo

# ---------------------------------------------------------------------------
# 1/2. The two host configurations. Configure as well as build: a gate that
#      assumes somebody already ran cmake is a gate that silently tests a stale
#      configuration, which is the same class of defect as not running at all.
# ---------------------------------------------------------------------------
echo "=== host builds ==="
cmake -S . -B build/host-release -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=OFF \
    > "${out_dir}/configure-release.log" 2>&1 \
 && cmake --build build/host-release -j "${jobs}" \
    > "${out_dir}/build-release.log" 2>&1
rc=$?
[ $rc -eq 0 ] && record shipped-config-build GREEN || {
    record shipped-config-build RED
    grep -E "error:" "${out_dir}/build-release.log" | head -15
}

cmake -S . -B build/host-nptest -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON \
    "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" \
    > "${out_dir}/configure-nptest.log" 2>&1 \
 && cmake --build build/host-nptest -j "${jobs}" \
    > "${out_dir}/build-nptest.log" 2>&1
np_rc=$?
[ $np_rc -eq 0 ] && record nptest-build GREEN || {
    record nptest-build RED
    grep -E "error:" "${out_dir}/build-nptest.log" | head -15
}
echo

# ---------------------------------------------------------------------------
# 3. Harnesses. Discovered from src/args.c rather than hardcoded, so a new
#    harness joins the gate by existing rather than by somebody remembering to
#    add it here.
# ---------------------------------------------------------------------------
echo "=== netplay harnesses ==="
BIN=""
for cand in build/host-nptest/3S-ARM.app/Contents/MacOS/3S-ARM \
            build/host-nptest/3s-arm build/host-nptest/3S-ARM; do
    [ -x "${cand}" ] && { BIN="${cand}"; break; }
done

if [ "${np_rc}" -ne 0 ] || [ -z "${BIN}" ]; then
    record netplay-harnesses ERROR
    echo "  no test binary; harnesses cannot run"
else
    mapfile -t HARNESSES < <(grep -oE '"test-[a-z0-9-]+"' src/args.c \
        | tr -d '"' | sort -u)
    if [ "${#HARNESSES[@]}" -eq 0 ]; then
        record netplay-harnesses ERROR
        echo "  discovered zero harnesses in src/args.c -- refusing to pass vacuously"
    else
        h_failed=0
        for h in "${HARNESSES[@]}"; do
            "./${BIN}" "--${h}" > "${out_dir}/${h}.log" 2>&1
            rc=$?
            note=""
            # exit 2 + "not compiled in" is a MISBUILD, never a pass.
            if grep -q "not compiled in" "${out_dir}/${h}.log"; then
                note="  <<< NOT COMPILED IN (misbuild, not a pass)"
            fi
            printf '    %-34s exit=%-3s%s\n' "${h}" "${rc}" "${note}"
            { [ $rc -ne 0 ] || [ -n "${note}" ]; } && h_failed=1
        done
        echo "    harness count = ${#HARNESSES[@]}"
        [ "${h_failed}" -eq 0 ] && record netplay-harnesses GREEN \
                                || record netplay-harnesses RED
    fi
fi
echo

# ---------------------------------------------------------------------------
# 4. Rendezvous protocol.
# ---------------------------------------------------------------------------
echo "=== rendezvous protocol ==="
if [ -f tools/rendezvous-server/__test_protocol.js ] && command -v node >/dev/null; then
    node tools/rendezvous-server/__test_protocol.js > "${out_dir}/protocol.log" 2>&1
    [ $? -eq 0 ] && record rendezvous-protocol GREEN || {
        record rendezvous-protocol RED; tail -5 "${out_dir}/protocol.log"; }
else
    record rendezvous-protocol ERROR
    echo "  __test_protocol.js or node is missing"
fi
echo

# ---------------------------------------------------------------------------
# 5. Host diagnostic parity -- task #106.
# ---------------------------------------------------------------------------
echo "=== host diagnostic parity ==="
"${SCRIPT_DIR}/host-diagnostic-parity.sh" > "${out_dir}/diag-parity.log" 2>&1
case $? in
0) record host-diagnostic-parity GREEN ;;
1) record host-diagnostic-parity RED
   grep -E "warning:|error:" "${out_dir}/diag-parity.log" | head -10 ;;
*) record host-diagnostic-parity ERROR
   tail -5 "${out_dir}/diag-parity.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 6. Doc-citation ceilings. Fails on breach AND on slack, so a scope cannot
#    quietly un-clean itself and an improvement cannot go unrecorded.
# ---------------------------------------------------------------------------
echo "=== doc-citation baselines ==="
python3 tools/doc-citations/check_baselines.py > "${out_dir}/baselines.log" 2>&1
case $? in
0) record doc-citation-baselines GREEN ;;
1) record doc-citation-baselines RED
   grep -E "BREACH|SLACK|BAD" "${out_dir}/baselines.log" | head -10 ;;
*) record doc-citation-baselines ERROR; tail -5 "${out_dir}/baselines.log" ;;
esac
echo

# ---------------------------------------------------------------------------
# 7. ARM cross-build -- task #106.
# ---------------------------------------------------------------------------
echo "=== ARM cross-build ==="
if [ "${run_arm}" -eq 1 ]; then
    tools/mister/build-game.sh --flavor telemetry \
        --lane "${GATES_ARM_LANE:-mister}" \
        --wait-for-lane "${GATES_ARM_WAIT:-1800}" \
        > "${out_dir}/arm-build.log" 2>&1
    case $? in
    0) record arm-cross-build GREEN ;;
    *) record arm-cross-build RED
       grep -E "error:|Refusing|No space" "${out_dir}/arm-build.log" | head -10 ;;
    esac
else
    record arm-cross-build "NOT RUN"
    echo "  pass --arm (or GATES_ARM=1) to cross-compile. Until then this run"
    echo "  says nothing about the ARM target -- see the summary line."
fi
echo

# ---------------------------------------------------------------------------
# Summary. Every gate is named, including the ones that did not run.
# ---------------------------------------------------------------------------
echo "############ SUMMARY ############"
skipped=""
for i in "${!NAMES[@]}"; do
    printf '  %-24s %s\n' "${NAMES[$i]}" "${STATES[$i]}"
    [ "${STATES[$i]}" = "NOT RUN" ] && skipped="${skipped} ${NAMES[$i]}"
done
verdict=$([ "${FAILED}" -eq 0 ] && echo GREEN || echo RED)
if [ -n "${skipped}" ]; then
    echo "GATES RESULT: ${verdict} -- NOT RUN:${skipped}"
    echo "  This result does not cover the gate(s) listed as NOT RUN."
else
    echo "GATES RESULT: ${verdict} (full set)"
fi
exit "${FAILED}"
