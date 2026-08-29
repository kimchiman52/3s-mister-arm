#!/usr/bin/env bash
#
# Acceptance test for the OSD launcher step of `misterctl.sh deploy` (task #95).
#
# The step used to begin with a hardcoded delete list:
#
#   rm -f /media/fat/Scripts/3S-ARM.sh \
#         '/media/fat/Scripts/3S-ARM_Training_Yun_Ryu_Ryu_Stage.sh' \
#         '/media/fat/Scripts/3S-ARM Training Yun Ryu Ryu Stage.sh'
#
# Nothing in this repo has ever created the second or third name -- `git log -S`
# finds no writer, and `grep -rn` finds them only in that rm. They are
# hand-authored per-stage training shortcuts, and every deploy destroyed them.
#
# The load-bearing cases here are therefore O1/O1b: files that exist in the OSD
# scripts directory, that this deploy did NOT install, and that the pre-fix code
# named explicitly. A test that only checked "the installer's own launcher gets
# replaced" would be vacuous -- it is exactly the test that passes on the
# destructive code. O6 keeps O1/O1b honest by asserting the fixtures are not in
# the set of names the deploy installs, and by asserting they really are the
# names the pre-fix code hardcoded.
#
# The test drives the real `misterctl.sh deploy`. It does not reimplement the
# policy: `rsync` and `ssh` are shimmed on PATH to repoint /media/fat/Scripts
# and the runtime root at a sandbox, and everything above the transport --
# manifest, plan, veto, remote script -- is production code. Because the shim
# rewrites strings rather than functions, the same test runs unchanged against
# the pre-fix tree, which is how it was proven red:
#
#   tools/mister/tests/osd-launcher-test.sh
#   MISTERCTL=/path/to/prefix/tools/mister/misterctl.sh \
#     tools/mister/tests/osd-launcher-test.sh
#
# No part of this test can reach a MiSTer: MISTER_HOST is set to an .invalid
# name, and the ssh shim refuses any command that still mentions /media/fat
# after the sandbox rewrite.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

MISTERCTL="${MISTERCTL:-${TOOLS_DIR}/misterctl.sh}"
MISTERCTL_DIR="$(cd "$(dirname "${MISTERCTL}")" && pwd)"
MISTER_COMMON="${MISTER_COMMON:-${MISTERCTL_DIR}/mister-common.sh}"

REMOTE_ROOT='/media/fat/games/3s-arm'
REMOTE_SCRIPTS='/media/fat/Scripts'

# The two names the pre-fix `rm -f` destroyed on every deploy. Neither is
# created anywhere in this repo.
USER_SCRIPT_A='3S-ARM_Training_Yun_Ryu_Ryu_Stage.sh'
USER_SCRIPT_B='3S-ARM Training Yun Ryu Ryu Stage.sh'
# A user script the pre-fix code did not name, to show the scope of the fix is
# the whole directory and not just those two.
USER_SCRIPT_C='zzz-user-authored.sh'
# A bystander from another core.
OTHER_CORE_SCRIPT='Update_All.sh'

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/osd-launcher-test.XXXXXX")"
WORK="$(cd "${WORK}" && pwd -P)"
trap 'rm -rf "${WORK}"' EXIT

export MISTER_TEST_DEVICE_ROOT="${WORK}/device"
export MISTER_TEST_SCRIPTS_DIR="${WORK}/device-scripts"

# ---------------------------------------------------------------------------
# Transport shims
# ---------------------------------------------------------------------------

mkdir -p "${WORK}/bin"

REAL_RSYNC="$(command -v rsync)"
cat >"${WORK}/bin/rsync" <<SHIM
#!/usr/bin/env bash
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
case "\${dst}" in
"\${MISTER_TEST_DEVICE_ROOT}"*) ;;
*)
    echo "rsync shim: destination '\${dst}' escaped the test sandbox" >&2
    exit 97
    ;;
esac
args[\${last}]="\${dst}"
exec "${REAL_RSYNC}" "\${args[@]}"
SHIM

cat >"${WORK}/bin/ssh" <<SHIM
#!/usr/bin/env bash
# The remote command is the last argument. Repoint both /media/fat paths the
# deploy touches at the sandbox, then refuse outright if any /media/fat
# reference survived -- a rewrite miss must abort, never fall through to a real
# device path.
cmd="\${!#}"
cmd="\$(printf '%s' "\${cmd}" | sed "s|${REMOTE_SCRIPTS}|\${MISTER_TEST_SCRIPTS_DIR}|g; s|${REMOTE_ROOT}|\${MISTER_TEST_DEVICE_ROOT}|g")"
case "\${cmd}" in
*/media/fat*)
    echo "ssh shim: command still references /media/fat after rewrite; refusing" >&2
    printf '%s\n' "\${cmd}" >&2
    exit 96
    ;;
esac
printf '%s\n---\n' "\${cmd}" >> "${WORK}/ssh-cmd.log"
exec /bin/sh -c "\${cmd}"
SHIM

chmod +x "${WORK}/bin/rsync" "${WORK}/bin/ssh"
export PATH="${WORK}/bin:${PATH}"

# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

# The launcher execs <runtime root>/scripts/launch-osd.sh. The ssh shim rewrites
# the runtime root inside the generated script, so the sandbox sees its own root
# here; on a device this is /media/fat/games/3s-arm.
EXPECTED_LAUNCHER="$(printf '#!/bin/sh\nset -eu\nexec %s/scripts/launch-osd.sh "$@"' "${MISTER_TEST_DEVICE_ROOT}")"

seed_device() {
    rm -rf "${MISTER_TEST_DEVICE_ROOT}" "${MISTER_TEST_SCRIPTS_DIR}"
    mkdir -p "${MISTER_TEST_DEVICE_ROOT}"/{bin,lib,scripts}
    echo 'old binary' >"${MISTER_TEST_DEVICE_ROOT}/bin/3s-arm"
    echo 'launcher' >"${MISTER_TEST_DEVICE_ROOT}/scripts/launch-osd.sh"
    printf 'user training settings' >"${MISTER_TEST_DEVICE_ROOT}/training"

    mkdir -p "${MISTER_TEST_SCRIPTS_DIR}"
    printf 'installer launcher, previous deploy' >"${MISTER_TEST_SCRIPTS_DIR}/3S-ARM.sh"
    printf 'user wrote this A' >"${MISTER_TEST_SCRIPTS_DIR}/${USER_SCRIPT_A}"
    printf 'user wrote this B' >"${MISTER_TEST_SCRIPTS_DIR}/${USER_SCRIPT_B}"
    printf 'user wrote this C' >"${MISTER_TEST_SCRIPTS_DIR}/${USER_SCRIPT_C}"
    printf 'another core owns this' >"${MISTER_TEST_SCRIPTS_DIR}/${OTHER_CORE_SCRIPT}"
}

seed_package() {
    local pkg="${WORK}/package"
    rm -rf "${pkg}"
    mkdir -p "${pkg}"/{bin,lib,scripts}
    echo 'new binary, deliberately a different size' >"${pkg}/bin/3s-arm"
    echo 'launcher' >"${pkg}/scripts/launch-osd.sh"
}

run_deploy() {
    # MISTER_HOST is deliberately unresolvable: if a shim ever fails to be
    # picked up, the command fails to connect rather than reaching a MiSTer.
    env \
        MISTER_HOST='osd-launcher-test.invalid' \
        MISTER_USER='root' \
        MISTER_PASSWORD='' \
        MISTER_LOCK_DIR="${WORK}/lock" \
        MISTER_ALLOW_BUSY_TARGET=1 \
        bash "${MISTERCTL}" deploy --src "${WORK}/package" \
        >"${WORK}/deploy.log" 2>&1
    printf '%s' "$?" >"${WORK}/deploy.rc"
}

S="${MISTER_TEST_SCRIPTS_DIR}"
D="${MISTER_TEST_DEVICE_ROOT}"

# ===========================================================================
# Scenario 1 -- bootstrap: the device has no OSD launcher manifest yet
# ===========================================================================
#
# This is the state of every real device at the moment this change lands.
echo '--- scenario 1: deploy onto a device with no OSD launcher manifest ---'
seed_device
seed_package
run_deploy
deploy_rc="$(cat "${WORK}/deploy.rc")"
if [ "${deploy_rc}" = "0" ]; then
    ok 'O0 deploy exits 0'
else
    bad "O0 deploy exits 0 (rc=${deploy_rc})"
    sed 's/^/      | /' "${WORK}/deploy.log"
fi

# --- O1/O1b: the destroyed-by-name case ------------------------------------
check_contents "${S}/${USER_SCRIPT_A}" 'user wrote this A' \
    "O1 a script the deploy did NOT install SURVIVES (${USER_SCRIPT_A})"
check_contents "${S}/${USER_SCRIPT_B}" 'user wrote this B' \
    "O1b the spaced variant SURVIVES (${USER_SCRIPT_B})"
check_contents "${S}/${USER_SCRIPT_C}" 'user wrote this C' \
    "O1c an unrelated user script SURVIVES (${USER_SCRIPT_C})"
check_contents "${S}/${OTHER_CORE_SCRIPT}" 'another core owns this' \
    "O1d another core's script SURVIVES (${OTHER_CORE_SCRIPT})"

# --- O2: the launcher the deploy does own is installed ----------------------
check_contents "${S}/3S-ARM.sh" "${EXPECTED_LAUNCHER}" \
    'O2 the OSD launcher this deploy owns is (re)written'
if [ -x "${S}/3S-ARM.sh" ]; then
    ok 'O2b the OSD launcher is executable'
else
    bad 'O2b the OSD launcher is executable'
fi

# --- O3: ownership is recorded ----------------------------------------------
if [ -f "${D}/.osd-scripts-manifest" ]; then
    if [ "$(cat "${D}/.osd-scripts-manifest")" = '3S-ARM.sh' ]; then
        ok 'O3 the manifest records exactly the launcher the deploy installed'
    else
        bad "O3 the manifest records exactly the launcher the deploy installed (got '$(cat "${D}/.osd-scripts-manifest")')"
    fi
else
    bad 'O3 an OSD launcher manifest is written to the runtime root'
fi

# ===========================================================================
# Scenario 2 -- a launcher a previous deploy installed and this one dropped
# ===========================================================================
#
# The delete step exists to stop renamed/dropped launchers accumulating. That
# capability must survive the inversion, or the fix is just "delete nothing".
echo '--- scenario 2: a stale launcher a previous deploy owned ---'
seed_device
seed_package
printf 'stale installer launcher' >"${S}/3S-ARM-legacy.sh"
printf '3S-ARM.sh\n3S-ARM-legacy.sh\n' >"${D}/.osd-scripts-manifest"
run_deploy
deploy_rc="$(cat "${WORK}/deploy.rc")"
if [ "${deploy_rc}" = "0" ]; then
    ok 'O4 deploy exits 0'
else
    bad "O4 deploy exits 0 (rc=${deploy_rc})"
    sed 's/^/      | /' "${WORK}/deploy.log"
fi
check_absent "${S}/3S-ARM-legacy.sh" \
    'O4b a launcher the previous deploy owned and this one dropped IS removed'
check_contents "${S}/${USER_SCRIPT_A}" 'user wrote this A' \
    'O4c the user script still survives a deploy that does delete something'
check_contents "${S}/${OTHER_CORE_SCRIPT}" 'another core owns this' \
    "O4d another core's script still survives that deploy"
if [ "$(cat "${D}/.osd-scripts-manifest" 2>/dev/null)" = '3S-ARM.sh' ]; then
    ok 'O4e the refreshed manifest no longer claims the pruned launcher'
else
    bad 'O4e the refreshed manifest no longer claims the pruned launcher'
fi

# ===========================================================================
# Scenario 3 -- a malformed manifest must abort, not delete
# ===========================================================================
echo '--- scenario 3: a manifest entry that is not a plain basename ---'
seed_device
seed_package
printf 'sentinel, must not be rewritten' >"${S}/3S-ARM.sh"
printf '3S-ARM.sh\n../../games/3s-arm/training\n' >"${D}/.osd-scripts-manifest"
run_deploy
deploy_rc="$(cat "${WORK}/deploy.rc")"
if [ "${deploy_rc}" != "0" ]; then
    ok 'O5 deploy refuses when the OSD manifest names a non-basename entry'
else
    bad 'O5 deploy refuses when the OSD manifest names a non-basename entry (exited 0)'
fi
check_contents "${D}/training" 'user training settings' \
    'O5b the escaping path is untouched by the refused deploy'
check_contents "${S}/3S-ARM.sh" 'sentinel, must not be rewritten' \
    'O5c the refused deploy touches nothing in the scripts directory'
check_contents "${S}/${USER_SCRIPT_A}" 'user wrote this A' \
    'O5d the user script survives the refused deploy'

# ===========================================================================
# Scenario 4 -- anti-vacuity
# ===========================================================================
echo '--- scenario 4: keep O1/O1b honest ---'

# The fixtures must be names the deploy never installs. If they ever join the
# installed set, O1/O1b stop testing anything and this fails.
if [ -r "${MISTER_COMMON}" ]; then
    # shellcheck disable=SC1090
    . "${MISTER_COMMON}"
fi
if declare -f mister_osd_launcher_names >/dev/null 2>&1; then
    vacuous=0
    for n in "${USER_SCRIPT_A}" "${USER_SCRIPT_B}" "${USER_SCRIPT_C}" "${OTHER_CORE_SCRIPT}"; do
        if mister_osd_launcher_names | grep -qxF "${n}"; then vacuous=1; fi
    done
    if [ "${vacuous}" -eq 0 ]; then
        ok 'O6 the O1 fixtures are not names this deploy installs'
    else
        bad 'O6 a fixture joined the installed set; O1/O1b no longer test anything'
    fi
else
    bad 'O6 mister_osd_launcher_names is missing'
fi

# The fixtures must be the names the destructive code actually named, or O1/O1b
# would be testing an invented scenario rather than the real regression.
PREFIX_REF="$(git -C "${TOOLS_DIR}" show 8dd6ae89:tools/mister/misterctl.sh 2>/dev/null || true)"
if [ -n "${PREFIX_REF}" ]; then
    if printf '%s' "${PREFIX_REF}" | grep -qF "${USER_SCRIPT_A}" &&
        printf '%s' "${PREFIX_REF}" | grep -qF "${USER_SCRIPT_B}"; then
        ok 'O6b the O1/O1b fixtures are the exact names the pre-fix rm -f destroyed'
    else
        bad 'O6b the O1/O1b fixtures are the exact names the pre-fix rm -f destroyed'
    fi
else
    printf 'SKIP  O6b (could not read 8dd6ae89:tools/mister/misterctl.sh)\n'
fi

# ===========================================================================
# Static checks
# ===========================================================================
echo '--- static checks ---'

code_only="${WORK}/misterctl.code"
grep -vE '^[[:space:]]*#' "${MISTERCTL}" >"${code_only}"
common_code="${WORK}/common.code"
grep -vE '^[[:space:]]*#' "${MISTER_COMMON}" >"${common_code}" 2>/dev/null || : >"${common_code}"

# The point of the change: no executable line may name a script to delete.
if grep -qF '3S-ARM_Training' "${code_only}" || grep -qF '3S-ARM Training' "${code_only}" ||
    grep -qF '3S-ARM_Training' "${common_code}" || grep -qF '3S-ARM Training' "${common_code}"; then
    bad 'O7 no hardcoded training-script name remains in executable code'
    grep -nF '3S-ARM_Training' "${code_only}" "${common_code}" | sed 's/^/      | /'
    grep -nF '3S-ARM Training' "${code_only}" "${common_code}" | sed 's/^/      | /'
else
    ok 'O7 no hardcoded training-script name remains in executable code'
fi

# No glob may ever reach an rm inside the shared scripts directory.
if grep -qE 'rm .*Scripts/.*\*' "${code_only}" "${common_code}"; then
    bad 'O8 no globbed delete targets the OSD scripts directory'
else
    ok 'O8 no globbed delete targets the OSD scripts directory'
fi

if declare -f mister_osd_remote_script >/dev/null 2>&1; then
    cat >"${WORK}/fake-plan" <<'PLAN'
prune 3S-ARM-legacy.sh
prune weird name with spaces.sh
PLAN
    mister_osd_remote_script "${REMOTE_ROOT}" "${REMOTE_SCRIPTS}" "${WORK}/fake-plan" \
        >"${WORK}/fake-osd.sh"
    if grep -q 'rm -rf' "${WORK}/fake-osd.sh"; then
        bad 'O9 the generated OSD script contains no recursive delete'
    elif grep -q "rm -f -- '3S-ARM-legacy.sh' 'weird name with spaces.sh'" "${WORK}/fake-osd.sh"; then
        ok 'O9 the generated OSD script deletes exact quoted basenames with rm -f --'
    else
        bad 'O9 the generated OSD script deletes exact quoted basenames with rm -f --'
        sed 's/^/      | /' "${WORK}/fake-osd.sh"
    fi
    # The rm runs after a cd into the scripts dir, so no delete argument may
    # itself be an absolute path.
    if grep -q "rm -f -- '/" "${WORK}/fake-osd.sh"; then
        bad 'O10 delete arguments are basenames, not absolute paths'
    else
        ok 'O10 delete arguments are basenames, not absolute paths'
    fi
else
    bad 'O9/O10 mister_osd_remote_script is missing'
fi

if declare -f mister_osd_name_is_sane >/dev/null 2>&1; then
    sane_ok=1
    for bad_name in '../x.sh' '/etc/passwd' 'a/b.sh' '*.sh' '-rf' '.hidden' 'x$y.sh' "x'y.sh"; do
        if mister_osd_name_is_sane "${bad_name}"; then
            bad "O11 rejects unsafe manifest entries (accepted '${bad_name}')"
            sane_ok=0
            break
        fi
    done
    if [ "${sane_ok}" -eq 1 ]; then
        if mister_osd_name_is_sane '3S-ARM.sh' &&
            mister_osd_name_is_sane '3S-ARM Training Yun Ryu Ryu Stage.sh'; then
            ok 'O11 rejects unsafe manifest entries and accepts plain basenames'
        else
            bad 'O11 rejects a legitimate plain basename'
        fi
    fi
else
    bad 'O11 mister_osd_name_is_sane is missing'
fi

echo
printf 'passed %d, failed %d\n' "${pass_count}" "${fail_count}"
[ "${fail_count}" -eq 0 ]
