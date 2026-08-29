#!/usr/bin/env bash
#
# Acceptance test for the MiSTer runtime deploy's deletion policy (task #93).
#
# The deploy destroyed real, unrecoverable user data twice -- libminiupnpc.so,
# replays/ and the ROM on 2026-07-25; the user's `training` settings and
# balance.status on 2026-08-29. Both times the mechanism was the same:
# `rsync --delete` scoped to the runtime root, shielding a fixed preserve-list,
# so anything on the device that was neither on that list nor in the package
# was deleted.
#
# The load-bearing case in this file is therefore T1/T2: a file and a directory
# that exist on the device, are not in the package, and are on NO preserve list
# and in NO inventory. A test that only checks that *listed* files survive
# proves nothing -- that is precisely the test that would have passed on
# 2026-07-24 and on 2026-08-28. T3 exists to keep T1/T2 honest: it fails if
# those names ever acquire list membership and quietly stop testing anything.
#
# The test drives the production functions in mister-common.sh. It does not
# reimplement the rsync flags or the delete policy; `rsync` and `ssh` are
# shimmed on PATH to redirect the remote path at a local directory, and
# everything above that -- argument vector, manifest, prune plan, veto -- is the
# real code.
#
# Usage:
#   tools/mister/tests/deploy-prune-test.sh
#   MISTER_COMMON=/path/to/other/mister-common.sh tools/mister/tests/deploy-prune-test.sh
#     (the second form is how this was proven red against the pre-fix code)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${TOOLS_DIR}/../.." && pwd)"

MISTER_COMMON="${MISTER_COMMON:-${TOOLS_DIR}/mister-common.sh}"

pass_count=0
fail_count=0

ok() {
    pass_count=$((pass_count + 1))
    printf 'PASS  %s\n' "$1"
}

bad() {
    fail_count=$((fail_count + 1))
    printf 'FAIL  %s\n' "$1"
}

check_exists() {
    if [ -e "$1" ]; then ok "$2"; else bad "$2 (missing: $1)"; fi
}

check_absent() {
    if [ ! -e "$1" ]; then ok "$2"; else bad "$2 (still present: $1)"; fi
}

check_contents() {
    local actual
    actual="$(cat "$1" 2>/dev/null || true)"
    if [ "${actual}" = "$2" ]; then ok "$3"; else bad "$3 (want '$2', got '${actual}')"; fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/deploy-prune-test.XXXXXX")"
WORK="$(cd "${WORK}" && pwd -P)"
trap 'rm -rf "${WORK}"' EXIT

REMOTE_ROOT='/media/fat/games/3s-arm'
export MISTER_TEST_DEVICE_ROOT="${WORK}/device"

# ---------------------------------------------------------------------------
# Transport shims
# ---------------------------------------------------------------------------
#
# The only thing these change is where "the device" is. Flags, filters,
# ordering and the remote scripts all come from the production code.

mkdir -p "${WORK}/bin"

REAL_RSYNC="$(command -v rsync)"
cat >"${WORK}/bin/rsync" <<SHIM
#!/usr/bin/env bash
# Rewrite the trailing user@host:<remote> destination to the local fake device,
# and drop the -e <ssh command> pair that a local copy has no use for.
args=()
skip_next=0
for a in "\$@"; do
    if [ "\${skip_next}" -eq 1 ]; then skip_next=0; continue; fi
    if [ "\${a}" = "-e" ]; then skip_next=1; continue; fi
    args+=("\${a}")
done
last=\$(( \${#args[@]} - 1 ))
dst="\${args[\${last}]}"
dst="\${dst#*:}"
dst="\$(printf '%s' "\${dst}" | sed "s|^${REMOTE_ROOT}|\${MISTER_TEST_DEVICE_ROOT}|")"
# If the rewrite ever fails to fire, rsync would write a media/fat/games/...
# tree into whatever the cwd happens to be -- which is the repo. Refuse.
case "\${dst}" in
"\${MISTER_TEST_DEVICE_ROOT}"*) ;;
*)
    echo "rsync shim: destination '\${dst}' escaped the test sandbox" >&2
    exit 97
    ;;
esac
args[\${last}]="\${dst}"
printf '%s\n' "\${args[*]}" >> "${WORK}/rsync-argv.log"
exec "${REAL_RSYNC}" "\${args[@]}"
SHIM

cat >"${WORK}/bin/ssh" <<SHIM
#!/usr/bin/env bash
# The remote command is the last argument; run it locally with the runtime root
# repointed at the fake device.
cmd="\${!#}"
cmd="\$(printf '%s' "\${cmd}" | sed "s|${REMOTE_ROOT}|\${MISTER_TEST_DEVICE_ROOT}|g")"
printf '%s\n---\n' "\${cmd}" >> "${WORK}/ssh-cmd.log"
exec /bin/sh -c "\${cmd}"
SHIM

chmod +x "${WORK}/bin/rsync" "${WORK}/bin/ssh"
export PATH="${WORK}/bin:${PATH}"

# shellcheck disable=SC1090
. "${MISTER_COMMON}"

# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------
#
# UNRECOGNISED_* are the point of this file: names invented here, present on
# the device, absent from the package, and absent from every list the tooling
# consults. They stand in for the next runtime writer nobody has remembered to
# register -- which is exactly what `training` was on 2026-08-29.
UNRECOGNISED_FILE='zzz-unregistered-runtime-file'
UNRECOGNISED_DIR='zzz-unregistered-runtime-dir'

seed_device() {
    local root="${MISTER_TEST_DEVICE_ROOT}"
    rm -rf "${root}"
    mkdir -p "${root}"/{bin,lib,licenses,scripts,resources,state,replays,saves,states,logs}
    mkdir -p "${root}/${UNRECOGNISED_DIR}"

    # Installed by a previous deploy.
    echo 'old binary' >"${root}/bin/3s-arm"
    echo 'sdl3' >"${root}/lib/libSDL3.so.0"
    echo 'miniupnpc' >"${root}/lib/libminiupnpc.so"
    echo 'launcher' >"${root}/scripts/run-3s-arm.sh"
    echo 'license' >"${root}/licenses/LICENSE"

    # Written by the game / the wrapper at runtime, on the preserve list.
    echo 'user config' >"${root}/config"
    echo 'user keymap' >"${root}/keymap"
    printf 'user training settings' >"${root}/training"
    echo 'Arcade (CPS3)' >"${root}/balance.status"
    echo 'ABCD 1' >"${root}/state/recent_joins.txt"
    echo 'replay' >"${root}/replays/match.3sr"
    echo 'the user ROM' >"${root}/resources/SF33RD.AFS"

    # Written by the game at runtime and NOT on the historical preserve list.
    # saves/ holds the actual save data (savesub.c:335,340) and was still
    # unprotected when this test was written.
    echo 'save data' >"${root}/saves/settings"
    echo 'sysdir' >"${root}/saves/sysdir"
    echo 'desync dump' >"${root}/states/desync_F1_pid2.txt"
    echo 'backend log' >"${root}/logs/backend.log"

    # On no list at all.
    echo 'nobody registered me' >"${root}/${UNRECOGNISED_FILE}"
    echo 'nor me' >"${root}/${UNRECOGNISED_DIR}/data.bin"
}

seed_package() {
    local pkg="${WORK}/package"
    rm -rf "${pkg}"
    mkdir -p "${pkg}"/{bin,lib,licenses,scripts,resources}
    # Deliberately a different length from the on-device copy: rsync's quick
    # check is size+mtime, and two same-size files written in the same second
    # would be skipped, making T7 pass or fail for reasons unrelated to policy.
    echo 'new binary, deliberately a different size' >"${pkg}/bin/3s-arm"
    echo 'sdl3' >"${pkg}/lib/libSDL3.so.0"
    echo 'launcher' >"${pkg}/scripts/run-3s-arm.sh"
    echo 'license' >"${pkg}/licenses/LICENSE"
    # Note: no lib/libminiupnpc.so -- it is the stale artifact this deploy
    # should clean up, and the reason a delete step exists at all.
}

# The manifest a previous deploy of the same package layout would have left.
seed_previous_manifest() {
    local root="${MISTER_TEST_DEVICE_ROOT}"
    cat >"${root}/.deploy-manifest" <<'MANIFEST'
d bin
d lib
d licenses
d resources
d scripts
f bin/3s-arm
f licenses/LICENSE
f lib/libSDL3.so.0
f lib/libminiupnpc.so
f scripts/run-3s-arm.sh
MANIFEST
}

run_deploy() {
    MISTER_PASSWORD='' mister_rsync_deploy \
        "${WORK}/package/" 'test-host' 'root' '' "${REMOTE_ROOT}/" \
        >"${WORK}/deploy.log" 2>&1
    printf '%s' "$?" >"${WORK}/deploy.rc"
}

# ===========================================================================
# Scenario 1 -- a normal deploy over a device with a previous manifest
# ===========================================================================
echo '--- scenario 1: deploy over a previously-deployed device ---'
seed_device
seed_package
seed_previous_manifest
run_deploy
deploy_rc="$(cat "${WORK}/deploy.rc")"
if [ "${deploy_rc}" = "0" ]; then
    ok 'S1 deploy exits 0'
else
    bad "S1 deploy exits 0 (rc=${deploy_rc})"
    sed 's/^/      | /' "${WORK}/deploy.log"
fi

D="${MISTER_TEST_DEVICE_ROOT}"

# --- T1/T2: the case that failed twice -------------------------------------
check_contents "${D}/${UNRECOGNISED_FILE}" 'nobody registered me' \
    'T1 an on-device file that is in no package and on no list SURVIVES'
check_contents "${D}/${UNRECOGNISED_DIR}/data.bin" 'nor me' \
    'T2 an on-device directory that is in no package and on no list SURVIVES'

# --- T3: keep T1/T2 from going vacuous -------------------------------------
vacuous=0
if mister_deploy_preserve_paths 2>/dev/null | grep -qxF "${UNRECOGNISED_FILE}"; then vacuous=1; fi
if mister_deploy_preserve_paths 2>/dev/null | grep -qxF "${UNRECOGNISED_DIR}"; then vacuous=1; fi
if [ -f "${TOOLS_DIR}/runtime-owned-paths.txt" ]; then
    if grep -qE "^[[:space:]]*(${UNRECOGNISED_FILE}|${UNRECOGNISED_DIR})\b" \
        "${TOOLS_DIR}/runtime-owned-paths.txt"; then vacuous=1; fi
fi
if [ "${vacuous}" -eq 0 ]; then
    ok 'T3 the T1/T2 fixtures are on no preserve list and in no inventory'
else
    bad 'T3 the T1/T2 fixtures gained list membership; T1/T2 no longer test anything'
fi

# --- T4: save data, unprotected by the historical list ----------------------
check_contents "${D}/saves/settings" 'save data' 'T4 saves/settings survives'
check_exists "${D}/saves/sysdir" 'T4b saves/sysdir survives'
check_exists "${D}/states/desync_F1_pid2.txt" 'T4c states/ survives'

# --- T5: the historically preserved names ----------------------------------
check_contents "${D}/training" 'user training settings' 'T5a training survives'
check_contents "${D}/balance.status" 'Arcade (CPS3)' 'T5b balance.status survives'
check_contents "${D}/config" 'user config' 'T5c config survives'
check_contents "${D}/keymap" 'user keymap' 'T5d keymap survives'
check_exists "${D}/state/recent_joins.txt" 'T5e state/ survives'
check_exists "${D}/replays/match.3sr" 'T5f replays/ survives'
check_contents "${D}/resources/SF33RD.AFS" 'the user ROM' 'T5g the ROM survives'
check_exists "${D}/logs/backend.log" 'T5h logs/ survives'

# --- T6: the delete step still does its job --------------------------------
check_absent "${D}/lib/libminiupnpc.so" \
    'T6 a stale artifact the previous deploy installed IS removed'

# --- T7: the package actually lands -----------------------------------------
check_contents "${D}/bin/3s-arm" 'new binary, deliberately a different size' 'T7 package content is installed'

# --- T8: the new manifest is recorded ---------------------------------------
if grep -qxF 'f bin/3s-arm' "${D}/.deploy-manifest" 2>/dev/null &&
    ! grep -qxF 'f lib/libminiupnpc.so' "${D}/.deploy-manifest" 2>/dev/null; then
    ok 'T8 a fresh manifest is written and no longer claims the pruned path'
else
    bad 'T8 a fresh manifest is written and no longer claims the pruned path'
fi
if grep -qE '(^| )(training|balance\.status|saves|config|keymap)$' \
    "${D}/.deploy-manifest" 2>/dev/null; then
    bad 'T8b the manifest must never claim ownership of a runtime-written path'
else
    ok 'T8b the manifest never claims ownership of a runtime-written path'
fi

# ===========================================================================
# Scenario 2 -- bootstrap: a device with no manifest owns nothing
# ===========================================================================
echo '--- scenario 2: device with no previous manifest ---'
seed_device
seed_package
rm -f "${MISTER_TEST_DEVICE_ROOT}/.deploy-manifest"
run_deploy
check_exists "${D}/lib/libminiupnpc.so" \
    'T9 with no manifest the deploy owns nothing and deletes nothing'
check_contents "${D}/${UNRECOGNISED_FILE}" 'nobody registered me' \
    'T9b unrecognised file still survives the bootstrap deploy'

# ===========================================================================
# Scenario 3 -- the tripwire: a manifest that claims a runtime-written path
# ===========================================================================
echo '--- scenario 3: manifest claims a runtime-written path ---'
seed_device
seed_package
seed_previous_manifest
printf 'f training\n' >>"${MISTER_TEST_DEVICE_ROOT}/.deploy-manifest"
run_deploy
deploy_rc="$(cat "${WORK}/deploy.rc")"
if [ "${deploy_rc}" != "0" ]; then
    ok 'T10 deploy refuses when the prune plan names a runtime-written path'
else
    bad 'T10 deploy refuses when the prune plan names a runtime-written path (exited 0)'
fi
check_contents "${D}/training" 'user training settings' \
    'T10b training is untouched by the refused deploy'
check_exists "${D}/lib/libminiupnpc.so" \
    'T10c the refused deploy deletes nothing at all'

# ===========================================================================
# Scenario 4 -- pruning a whole directory the package dropped
# ===========================================================================
#
# Removal is `rm -f <exact file>` then `rmdir` deepest-first, never `rm -rf`.
# The consequence worth proving is the second half: if anything the deploy does
# not own is sitting inside a directory it is entitled to remove, rmdir refuses
# and that content survives. A recursive delete would have taken it.
echo '--- scenario 4: a dropped directory, with and without a squatter ---'
seed_device
seed_package
mkdir -p "${MISTER_TEST_DEVICE_ROOT}/assets/fonts"
echo 'font' >"${MISTER_TEST_DEVICE_ROOT}/assets/fonts/ui.ttf"
seed_previous_manifest
cat >>"${MISTER_TEST_DEVICE_ROOT}/.deploy-manifest" <<'EXTRA'
d assets
d assets/fonts
f assets/fonts/ui.ttf
EXTRA
run_deploy
check_absent "${D}/assets" 'T15 a directory the package dropped is removed, deepest-first'

seed_device
seed_package
mkdir -p "${MISTER_TEST_DEVICE_ROOT}/assets/fonts"
echo 'font' >"${MISTER_TEST_DEVICE_ROOT}/assets/fonts/ui.ttf"
echo 'user put this here' >"${MISTER_TEST_DEVICE_ROOT}/assets/user-notes.txt"
seed_previous_manifest
cat >>"${MISTER_TEST_DEVICE_ROOT}/.deploy-manifest" <<'EXTRA'
d assets
d assets/fonts
f assets/fonts/ui.ttf
EXTRA
run_deploy
check_absent "${D}/assets/fonts/ui.ttf" 'T16a the owned file inside it is still removed'
check_contents "${D}/assets/user-notes.txt" 'user put this here' \
    'T16b unowned content inside a pruned directory SURVIVES (rmdir, never rm -rf)'

# ===========================================================================
# Static checks
# ===========================================================================
echo '--- static checks ---'

# The generated remote script must never contain a recursive delete, whatever
# the plan says. (`rm -rf` elsewhere in mister-common.sh cleans up the local
# temp dir and is not remote.)
if declare -f mister_deploy_prune_script >/dev/null 2>&1; then
    cat >"${WORK}/fake-plan" <<'PLAN'
prune f a/b/c.txt
prune d a/b
prune d a
PLAN
    mister_deploy_prune_script "${REMOTE_ROOT}" "${WORK}/fake-plan" >"${WORK}/fake-prune.sh"
    if grep -q 'rm -rf' "${WORK}/fake-prune.sh"; then
        bad 'T17 the remote prune script contains no recursive delete'
    elif grep -q 'rm -f --' "${WORK}/fake-prune.sh" && grep -q 'rmdir --' "${WORK}/fake-prune.sh"; then
        ok 'T17 the remote prune script removes files with rm -f and directories with rmdir'
    else
        bad 'T17 the remote prune script removes files with rm -f and directories with rmdir'
        sed 's/^/      | /' "${WORK}/fake-prune.sh"
    fi
    # Deepest-first, or rmdir would hit a non-empty parent first and give up.
    if [ "$(grep -n "rmdir" "${WORK}/fake-prune.sh" | head -1 | grep -c "'a/b'")" = "1" ]; then
        ok 'T18 directories are removed deepest-first'
    else
        bad 'T18 directories are removed deepest-first'
        sed 's/^/      | /' "${WORK}/fake-prune.sh"
    fi
else
    bad 'T17/T18 mister_deploy_prune_script is missing'
fi

if [ -x "${TOOLS_DIR}/derive-runtime-paths.sh" ]; then
    if "${TOOLS_DIR}/derive-runtime-paths.sh" --check >/dev/null 2>&1; then
        ok 'T11 runtime-owned inventory matches what the source persists'
    else
        bad 'T11 runtime-owned inventory matches what the source persists'
        "${TOOLS_DIR}/derive-runtime-paths.sh" --check 2>&1 | sed 's/^/      | /'
    fi
else
    bad 'T11 tools/mister/derive-runtime-paths.sh is missing'
fi

# Comment lines are stripped so the prose above the new implementation, which
# necessarily quotes the flag it removed, does not trip these checks.
code_only="${WORK}/mister-common.code"
grep -vE '^[[:space:]]*#' "${MISTER_COMMON}" >"${code_only}"

if grep -qE '(^|[^-])--delete' "${code_only}"; then
    bad 'T12 no executable --delete remains in the deploy path'
    grep -nE '(^|[^-])--delete' "${code_only}" | sed 's/^/      | /'
else
    ok 'T12 no executable --delete remains in the deploy path'
fi

# The password (expect) path and the key-only path must transfer with the same
# argument vector. They drifted before, and mister_rsync_deploy_wrapper drifted
# from both -- that drift is why the wrapper deploy still destroyed `training`
# after the 2026-08-29 fix.
if grep -q 'spawn rsync {\*}\$rsync_args' "${MISTER_COMMON}" 2>/dev/null; then
    ok 'T13 the expect path spawns rsync with the shared argument vector'
else
    bad 'T13 the expect path spawns rsync with the shared argument vector'
fi
if ! grep -q 'rsync -av --delete' "${code_only}"; then
    ok 'T14 no hand-copied delete-capable rsync invocation remains'
else
    bad 'T14 no hand-copied delete-capable rsync invocation remains'
fi

echo
printf 'passed %d, failed %d\n' "${pass_count}" "${fail_count}"
[ "${fail_count}" -eq 0 ]
