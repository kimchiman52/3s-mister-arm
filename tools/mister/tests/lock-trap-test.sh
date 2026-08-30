#!/usr/bin/env bash
#
# The MiSTer lock must survive a caller's own EXIT trap.
#
# mister_lock_acquire installs `trap 'mister_lock_release' EXIT`. Bash has one
# EXIT trap slot, so any caller writing the ordinary
#
#     mister_lock_acquire
#     trap 'my_cleanup' EXIT
#
# used to overwrite the release and leak the lock directory. mister-common.sh
# now shadows `trap` with a function that chains the release onto whatever the
# caller installs. These tests pin both halves of that: the lock is released,
# AND the caller's handler still runs -- a chain that drops the caller's own
# cleanup would be a worse bug than the leak it fixes.
#
# Run:  tools/mister/tests/lock-trap-test.sh
# No device, no network, no build. Everything happens under a temp dir.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MISTER_COMMON="${MISTER_COMMON:-${TOOLS_DIR}/mister-common.sh}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/lock-trap-test.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

pass=0
fail=0

ok() { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
no() { printf 'FAIL  %s\n     %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

# Run a caller script body in its own bash, against its own lock dir.
# Echoes the caller's stdout, then "lock_after=<free|leaked>".
run_case() {
    local name="$1" body="$2"
    local lock_dir="${WORK}/${name}.lock"
    local script="${WORK}/${name}.sh"

    {
        printf '%s\n' '#!/usr/bin/env bash'
        printf '%s\n' 'set -uo pipefail'
        printf 'source %q\n' "${MISTER_COMMON}"
        printf '%s\n' "${body}"
    } >"${script}"
    chmod +x "${script}"

    MISTER_LOCK_DIR="${lock_dir}" MISTER_LOCK_TIMEOUT=5 bash "${script}" 2>&1
    if [ -d "${lock_dir}" ]; then
        printf 'lock_after=leaked\n'
    else
        printf 'lock_after=free\n'
    fi
}

expect_contains() {
    local name="$1" out="$2" needle="$3"
    case "${out}" in
        *"${needle}"*) ok "${name}: ${needle}" ;;
        *) no "${name}: expected ${needle}" "got: $(printf '%s' "${out}" | tr '\n' '|')" ;;
    esac
}

# ---------------------------------------------------------------- T1
# The reported defect: caller installs its own EXIT trap after the acquire.
# Both the release and the caller's handler must run.
out="$(run_case t1 '
mister_lock_acquire
trap "echo CALLER-EXIT-RAN" EXIT
exit 0
')"
expect_contains "T1 caller EXIT trap" "${out}" "CALLER-EXIT-RAN"
expect_contains "T1 caller EXIT trap" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T2
# Caller handler is a function, and the script dies under `set -e` rather than
# exiting cleanly.
out="$(run_case t2 '
set -e
my_cleanup() { echo "CALLER-CLEANUP-RAN status=$?"; }
mister_lock_acquire
trap my_cleanup EXIT
false
echo "UNREACHABLE"
')"
expect_contains "T2 set -e failure" "${out}" "CALLER-CLEANUP-RAN status=1"
expect_contains "T2 set -e failure" "${out}" "lock_after=free"
case "${out}" in *UNREACHABLE*) no "T2 set -e failure" "kept running past the failure";; *) ok "T2 set -e failure: stopped at the failure";; esac

# ---------------------------------------------------------------- T3
# Caller clears the EXIT trap outright. The release is not the caller's to
# remove while the lock is held.
out="$(run_case t3 '
mister_lock_acquire
trap - EXIT
echo CLEARED
exit 0
')"
expect_contains "T3 trap - EXIT" "${out}" "CLEARED"
expect_contains "T3 trap - EXIT" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T4
# Caller installs one handler across several signals at once, the shape
# perf-sampler.sh uses. The non-EXIT signals must be installed verbatim.
out="$(run_case t4 '
mister_lock_acquire
trap "echo MULTI" EXIT INT TERM HUP
trap -p INT
trap -p TERM
exit 0
')"
expect_contains "T4 multi-signal" "${out}" "trap -- 'echo MULTI' SIGINT"
expect_contains "T4 multi-signal" "${out}" "trap -- 'echo MULTI' SIGTERM"
expect_contains "T4 multi-signal" "${out}" "MULTI"
expect_contains "T4 multi-signal" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T5
# Non-EXIT traps pass through untouched: the wrapper must not append a release
# to a signal it was not asked about.
out="$(run_case t5 '
mister_lock_acquire
trap "echo INTONLY" INT
trap -p INT
trap -p EXIT
exit 0
')"
expect_contains "T5 non-EXIT passthrough" "${out}" "trap -- 'echo INTONLY' SIGINT"
expect_contains "T5 non-EXIT passthrough" "${out}" "mister_lock_release' EXIT"
expect_contains "T5 non-EXIT passthrough" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T6
# Before the acquire, `trap` is the plain builtin -- no chaining, nothing
# appended. (A pre-acquire EXIT trap is then clobbered by the acquire, which is
# the acquire's documented behaviour and unchanged by this fix.)
out="$(run_case t6 '
trap "echo EARLY" EXIT
trap -p EXIT
mister_lock_acquire
exit 0
')"
expect_contains "T6 pre-acquire passthrough" "${out}" "trap -- 'echo EARLY' EXIT"
case "${out}" in *"EARLY
mister_lock_release"*) no "T6 pre-acquire passthrough" "chained before the lock was held";; *) ok "T6 pre-acquire passthrough: not chained";; esac
expect_contains "T6 pre-acquire passthrough" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T7
# A subshell must never drop the parent's lock. Its EXIT trap gets the chained
# release like any other, and the BASH_SUBSHELL guard in mister_lock_release is
# what stops it deleting a lock the parent is still using.
out="$(run_case t7 '
mister_lock_acquire
( trap "echo SUBSHELL-EXIT" EXIT; true )
if [ -d "${MISTER_LOCK_DIR}" ]; then echo "STILL-HELD-AFTER-SUBSHELL"; else echo "LOCK-STOLEN-BY-SUBSHELL"; fi
exit 0
')"
expect_contains "T7 subshell guard" "${out}" "SUBSHELL-EXIT"
expect_contains "T7 subshell guard" "${out}" "STILL-HELD-AFTER-SUBSHELL"
expect_contains "T7 subshell guard" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T8
# After an explicit release the wrapper is inert again: no chaining, and the
# caller's trap is whatever the caller wrote.
out="$(run_case t8 '
mister_lock_acquire
mister_lock_release
trap "echo LATE" EXIT
trap -p EXIT
exit 0
')"
expect_contains "T8 post-release passthrough" "${out}" "trap -- 'echo LATE' EXIT"
expect_contains "T8 post-release passthrough" "${out}" "lock_after=free"

# ---------------------------------------------------------------- T10
# The real in-tree caller shape: perf-sampler.sh acquires, installs four traps
# (perf-sampler.sh:904-907), and its handler disarms all four
# (perf-sampler.sh:787) before releasing explicitly and exiting
# (perf-sampler.sh:811-812). No double release, no recursion, no leak.
out="$(run_case t10 '
cleanup_local() {
    local status="${1:-$?}"
    trap - EXIT INT TERM HUP
    echo "CLEANUP status=${status}"
    mister_lock_release || true
    echo "RELEASE-RC=$?"
    exit "${status}"
}
mister_lock_acquire
trap "cleanup_local \$?" EXIT
trap "cleanup_local 130" INT
trap "cleanup_local 129" HUP
trap "cleanup_local 143" TERM
exit 3
')"
expect_contains "T10 perf-sampler shape" "${out}" "CLEANUP status=3"
expect_contains "T10 perf-sampler shape" "${out}" "RELEASE-RC=0"
expect_contains "T10 perf-sampler shape" "${out}" "lock_after=free"
case "${out}" in *"CLEANUP status"*"CLEANUP status"*) no "T10 perf-sampler shape" "handler re-entered";; *) ok "T10 perf-sampler shape: handler ran once";; esac

# ---------------------------------------------------------------- T9
# Negative control: the old behaviour, reproduced against a copy of
# mister-common.sh with the wrapper removed. This must LEAK -- if it does not,
# the positive cases above are proving nothing.
legacy="${WORK}/legacy-common.sh"
python3 - "${MISTER_COMMON}" "${legacy}" <<'PY'
import io, re, sys
src = io.open(sys.argv[1], encoding="utf-8").read()
start = src.index("\ntrap() {\n")
end = src.index("\n}\n", start) + len("\n}\n")
out = src[:start] + "\n" + src[end:]
out = out.replace("builtin trap 'mister_lock_release' EXIT INT TERM HUP",
                  "trap 'mister_lock_release' EXIT INT TERM HUP")
assert "\ntrap() {\n" not in out
io.open(sys.argv[2], "w", encoding="utf-8").write(out)
PY
if bash -n "${legacy}"; then
    out="$(MISTER_COMMON="${legacy}" run_case t9 '
mister_lock_acquire
trap "echo CALLER-EXIT-RAN" EXIT
exit 0
')"
    expect_contains "T9 negative control (wrapper removed)" "${out}" "CALLER-EXIT-RAN"
    expect_contains "T9 negative control (wrapper removed)" "${out}" "lock_after=leaked"
    rm -rf "${WORK}/t9.lock"
else
    no "T9 negative control" "could not build the wrapper-less copy"
fi

printf '\n%d passed, %d failed\n' "${pass}" "${fail}"
[ "${fail}" -eq 0 ]
