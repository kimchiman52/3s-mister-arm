#!/usr/bin/env bash
#
# Derive, from the source, every top-level name the game persists inside its
# runtime root (/media/fat/games/3s-arm/ on MiSTer).
#
# Why this exists
# ---------------
# `misterctl.sh deploy` used to be an allowlist: `rsync --delete` scoped to the
# runtime root, with a hand-maintained `--exclude`/`--filter P` list naming the
# files that were allowed to survive. Anything on the device that was not on
# that list and not in the package was deleted. The list is a fixed enumeration
# racing a growing set of runtime writers, so every new persistent file was
# destroyed by default until somebody remembered to add it. That happened twice
# for real: libminiupnpc.so + replays/ + the ROM on 2026-07-25, and `training`
# (the user's training-mode settings) + balance.status on 2026-08-29, with no
# device backup either time.
#
# The deploy no longer works that way -- it deletes only what a previous deploy
# recorded as its own, see mister_deploy_* in mister-common.sh -- so a name
# missing from the inventory is no longer a data-loss path. This script is the
# tripwire on top of that: it reads the source, and `runtime-owned-paths.txt`
# records what it found. When they disagree, someone added a runtime writer,
# and tools/mister/tests/deploy-prune-test.sh fails until the inventory is
# updated. The deploy consults the inventory and refuses to delete anything it
# names.
#
# Usage:
#   tools/mister/derive-runtime-paths.sh          # print derived names, sorted
#   tools/mister/derive-runtime-paths.sh --check   # diff against the inventory
#
# Derivation rules (each is a literal-string rule; there is no attempt at real
# dataflow, and over-approximation is the safe direction here because every
# name this emits is a name the deploy refuses to delete):
#
#   A. Any string literal beginning "/media/fat/games/3s-arm/" -- the wrapper
#      (vendor/Main_MiSTer/thirdsarm_wrapper.cpp) hardcodes the runtime root
#      rather than calling into the port, e.g. kRecentJoinsDir at :2580.
#   B1. In any file referencing Paths_GetPrefPath, a literal of the form
#      "%s<name>" with no slash after the %s. Paths_GetPrefPath() is
#      normalised to a trailing slash (src/port/paths.c:26-30), so this is the
#      codebase's spelling for "a path directly under the runtime root":
#      "%straining" (training_config.c:157), "%sbalance.status"
#      (arcade_balance.c:96), "%slogs" (sdl_app.c:668).
#   B2. A line containing both Paths_GetPrefPath() and a "%s/<name>" literal.
#      One call site composes with an explicit slash -- imgui_wrapper.c:78 --
#      producing a harmless double slash before imgui.ini. Rule B1 cannot see
#      it, and restricting B2 to single lines keeps it from matching the
#      "%s/..." literals that compose against a logs/states directory variable
#      rather than against the pref path.
#   C. `#define <...>ROOT_DIR "<name>"` in a file referencing
#      Paths_OpenUserStorage. That API roots an SDL_Storage at the pref path
#      (paths.c:65), so storage-relative roots are runtime-root-relative:
#      savesub.c:42 defines the storage root as "saves"; savesub.c:412 reads
#      that macro and savesub.c:416 creates the directory.
#   D. SDL_CreateDirectory("<literal>"). Catches directories created relative
#      to the process CWD rather than through the pref path -- the
#      SDL_CreateDirectory call at game_state.c:2522 and the one at
#      netplay.c:1044 both create "states" this way.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
INVENTORY="${SCRIPT_DIR}/runtime-owned-paths.txt"

RUNTIME_ROOT_LITERAL="/media/fat/games/3s-arm/"

# Source trees that can contain a runtime writer. vendor/Main_MiSTer is in the
# list because the OSD wrapper writes config, balance.status, logs/ and state/
# without going through src/port/paths.c at all.
scan_roots=(src include vendor/Main_MiSTer)

existing_roots=()
for root in "${scan_roots[@]}"; do
    if [ -d "${ROOT_DIR}/${root}" ]; then
        existing_roots+=("${ROOT_DIR}/${root}")
    fi
done

if [ "${#existing_roots[@]}" -eq 0 ]; then
    echo "derive-runtime-paths: no source roots found under ${ROOT_DIR}" >&2
    exit 2
fi

# Reduce "a/b/c" to "a", drop anything with a printf conversion left in it
# (e.g. "config.tmp.%d", "netplay-%llu.log" -- transient per-run names that no
# package ever ships), and drop empties.
emit_component() {
    local raw="$1"
    raw="${raw%%/*}"
    case "${raw}" in
    ""|*%*|.|..) return 0 ;;
    esac
    printf '%s\n' "${raw}"
}

derive() {
    local file line name

    # --- Rule A -----------------------------------------------------------
    while IFS= read -r name; do
        emit_component "${name}"
    done < <(
        grep -rhoE "\"${RUNTIME_ROOT_LITERAL}[A-Za-z0-9_][A-Za-z0-9_.%+-]*" \
            "${existing_roots[@]}" 2>/dev/null |
            sed "s|.*${RUNTIME_ROOT_LITERAL}||"
    )

    # --- Rules B1 / B2 ----------------------------------------------------
    while IFS= read -r file; do
        [ -n "${file}" ] || continue
        while IFS= read -r name; do
            emit_component "${name#\"%s}"
        done < <(grep -ohE '"%s[A-Za-z0-9_][A-Za-z0-9_.%+-]*' "${file}" 2>/dev/null)

        while IFS= read -r line; do
            while IFS= read -r name; do
                emit_component "${name#\"%s/}"
            done < <(printf '%s\n' "${line}" |
                grep -ohE '"%s/[A-Za-z0-9_][A-Za-z0-9_.%+-]*' 2>/dev/null)
        done < <(grep -hE 'Paths_GetPrefPath\(\)' "${file}" 2>/dev/null)
    done < <(grep -rlE 'Paths_GetPrefPath' "${existing_roots[@]}" 2>/dev/null)

    # --- Rule C -----------------------------------------------------------
    while IFS= read -r file; do
        [ -n "${file}" ] || continue
        while IFS= read -r name; do
            emit_component "${name}"
        done < <(
            grep -ohE '#[[:space:]]*define[[:space:]]+[A-Z_]*ROOT_DIR[[:space:]]+"[^"]+"' \
                "${file}" 2>/dev/null | sed -E 's/.*"([^"]+)".*/\1/'
        )
    done < <(grep -rlE 'Paths_OpenUserStorage' "${existing_roots[@]}" 2>/dev/null)

    # --- Rule D -----------------------------------------------------------
    while IFS= read -r name; do
        emit_component "${name}"
    done < <(
        grep -rhoE 'SDL_CreateDirectory\("[^"]+"' "${existing_roots[@]}" 2>/dev/null |
            sed -E 's/.*"([^"]+)"?$/\1/'
    )
}

derived="$(derive | LC_ALL=C sort -u)"

if [ -z "${derived}" ]; then
    echo "derive-runtime-paths: derived nothing; the scan is broken, not the source" >&2
    exit 3
fi

if [ "${1:-}" != "--check" ]; then
    printf '%s\n' "${derived}"
    exit 0
fi

if [ ! -f "${INVENTORY}" ]; then
    echo "derive-runtime-paths: inventory not found: ${INVENTORY}" >&2
    exit 3
fi

inventory="$(sed -E 's/[[:space:]]*#.*$//; s/^[[:space:]]+//; s/[[:space:]]+$//' \
    "${INVENTORY}" | grep -v '^$' | LC_ALL=C sort -u)"

missing="$(LC_ALL=C comm -23 <(printf '%s\n' "${derived}") <(printf '%s\n' "${inventory}"))"
extra="$(LC_ALL=C comm -13 <(printf '%s\n' "${derived}") <(printf '%s\n' "${inventory}"))"

rc=0
if [ -n "${missing}" ]; then
    echo "ERROR: the source persists names that ${INVENTORY} does not list:" >&2
    printf '  %s\n' ${missing} >&2
    echo "       Add them (with a file:LINE citation) before deploying." >&2
    rc=1
fi
if [ -n "${extra}" ]; then
    echo "ERROR: ${INVENTORY} lists names the source no longer persists:" >&2
    printf '  %s\n' ${extra} >&2
    echo "       Remove them, or fix the derivation rule that used to find them." >&2
    rc=1
fi

if [ "${rc}" -eq 0 ]; then
    echo "runtime-owned inventory matches the source ($(printf '%s\n' "${derived}" | wc -l | tr -d ' ') names)"
fi
exit "${rc}"
