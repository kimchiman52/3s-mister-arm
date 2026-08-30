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
    "key-rate-budget|server per-key cap vs the CLIENT cadences it is derived from -- #123"
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
    # DISCOVERY, not a hardcoded list, so a new harness joins this gate by
    # existing rather than by somebody remembering to add it here.
    #
    # The discriminator is precise on purpose. `grep '"test-[a-z0-9-]+"'` over
    # args.c looks like it works and does not: it returns 23 flags, because most
    # `--test-*` options CONFIGURE the interactive test runner (--test-stage
    # takes an integer, --test-p1-character takes a name, --test-enable launches
    # the game) rather than being self-contained harnesses. Running those as if
    # they were harnesses starts the game and hangs the gate -- observed.
    #
    # A harness is an OPT_BOOLEAN whose help text says it runs and EXITS. That
    # is exactly the nine the pre-existing lane-private runner listed by hand.
    #
    # NOT mapfile: macOS ships bash 3.2, which has no `mapfile`. It failed
    # silently, and under `set -u` the next line died on an unbound HARNESSES,
    # so this gate recorded NO state and the summary still printed GREEN -- the
    # very defect this runner exists to prevent, reproduced inside it. The
    # completeness assertion at the bottom is the structural fix for that class;
    # this loop is the proximate one.
    HARNESSES=()
    while IFS= read -r h; do
        [ -n "${h}" ] && HARNESSES+=("${h}")
    done < <(python3 - src/args.c <<'DISCOVER'
import re, sys
src = open(sys.argv[1]).read()
pat = re.compile(
    r'OPT_BOOLEAN\(\s*0\s*,\s*"(test-[a-z0-9-]+)"\s*,\s*[^,]+,\s*((?:"[^"]*"\s*)+)',
    re.S)
for m in pat.finditer(src):
    if "and exit" in m.group(2).replace("\n", " "):
        print(m.group(1))
DISCOVER
)
    # A discovery rule that silently matches nothing would turn this gate into a
    # no-op that reports GREEN. Refuse instead.
    if [ "${#HARNESSES[@]}" -lt 8 ]; then
        record netplay-harnesses ERROR
        echo "  discovered ${#HARNESSES[@]} harnesses in src/args.c (expected >= 8)"
        echo "  -- refusing to pass vacuously on a discovery rule that broke"
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
# 4b. Cross-repo constant coupling -- task #123.
#
# rendezvous-server.js sizes KEY_RATE_LIMIT_PER_WINDOW from constants in the C
# CLIENT, and the two deploy INDEPENDENTLY: the server is a long-lived VPS
# process, the client ships in a release ZIP. No build can catch the drift (no
# TU and no module sees both a C literal and a JS const), and the symptom is
# not a crash -- it is a production room whose host liveness REGISTERs get
# rate-dropped until the code on the host's screen stops working. Same shape,
# and the same remedy, as tools/ldreq-timing/check_barrier_budget.py.
# ---------------------------------------------------------------------------
echo "=== key-rate budget ==="
python3 tools/rendezvous-server/check_key_rate_budget.py \
    > "${out_dir}/key-rate-budget.log" 2>&1
case $? in
0) record key-rate-budget GREEN ;;
1) record key-rate-budget RED
   grep -E "FAIL|legit peak|required cap" "${out_dir}/key-rate-budget.log" | head -10 ;;
*) record key-rate-budget ERROR; tail -8 "${out_dir}/key-rate-budget.log" ;;
esac
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

# COMPLETENESS ASSERTION.
#
# Every gate declared in GATES must have recorded a state. Without this, a gate
# that dies before calling `record` -- a missing shell builtin, an unbound
# variable under `set -u`, an early `exit` in a helper -- simply vanishes from
# the summary, and the verdict is computed over the gates that survived. That
# is not a hypothetical: the harness gate did exactly this on macOS bash 3.2
# (`mapfile: command not found`) and the run still printed GREEN.
#
# A gate that produced no state is a harness error, never a pass.
for spec in "${GATES[@]}"; do
    want="${spec%%|*}"
    found=0
    for n in "${NAMES[@]:-}"; do
        [ "${n}" = "${want}" ] && { found=1; break; }
    done
    if [ "${found}" -eq 0 ]; then
        NAMES+=("${want}"); STATES+=("NO RESULT")
        FAILED=1
    fi
done

skipped=""
for i in "${!NAMES[@]}"; do
    printf '  %-24s %s\n' "${NAMES[$i]}" "${STATES[$i]}"
    [ "${STATES[$i]}" = "NOT RUN" ] && skipped="${skipped} ${NAMES[$i]}"
    [ "${STATES[$i]}" = "NO RESULT" ] && \
        echo "      ^ this gate recorded nothing -- it did not run to completion"
done
verdict=$([ "${FAILED}" -eq 0 ] && echo GREEN || echo RED)
if [ -n "${skipped}" ]; then
    echo "GATES RESULT: ${verdict} -- NOT RUN:${skipped}"
    echo "  This result does not cover the gate(s) listed as NOT RUN."
else
    echo "GATES RESULT: ${verdict} (full set)"
fi
exit "${FAILED}"
