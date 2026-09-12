#!/usr/bin/env bash
#
# Unit test for tools/require-debug-host-build.sh's predicate.
#
# The guard decides whether the shared build/host directory can produce a
# binary with the `#if DEBUG` test hooks compiled in. Getting that decision
# wrong is expensive in both directions: a false accept restores the
# misdiagnosis the guard exists to kill ("game hung and was killed by the
# timeout" for a build-type problem), and a false refuse blocks a perfectly
# good directory. Both directions are checked here against synthetic
# CMakeCache.txt files, so neither needs a real CMake configure to test.
#
# Run: bash tools/tests/require-debug-host-build-test.sh
# Exit 0 = all cases as expected.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck source=../require-debug-host-build.sh
. "${REPO_ROOT}/tools/require-debug-host-build.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

pass=0
fail=0

# mkcache <name> <build_type> <debug_hooks> <generator>
mkcache() {
    mkdir -p "${TMP}/$1"
    printf 'CMAKE_BUILD_TYPE:STRING=%s\nENABLE_DEBUG_HOOKS:BOOL=%s\nCMAKE_GENERATOR:INTERNAL=%s\n' \
        "$2" "$3" "$4" > "${TMP}/$1/CMakeCache.txt"
}

# expect <accept|refuse> <dir> <what>
expect() {
    local want="$1" dir="$2" what="$3" rc
    require_debug_host_build "${TMP}/${dir}" test >/dev/null 2>&1
    rc=$?
    local got="accept"
    [ "${rc}" -eq 0 ] || got="refuse"
    if [ "${got}" = "${want}" ]; then
        pass=$((pass + 1))
        printf 'ok   %-44s %s\n' "${what}" "${got}"
    else
        fail=$((fail + 1))
        printf 'FAIL %-44s want=%s got=%s (rc=%d)\n' "${what}" "${want}" "${got}" "${rc}"
    fi
}

# --- accepted: the configurations that really do compile the DEBUG hooks ---

mkcache debug Debug OFF "Unix Makefiles"
expect accept debug "CMAKE_BUILD_TYPE=Debug"

# ENABLE_DEBUG_HOOKS compiles the same DEBUG code into a non-Debug config
# (CMakeLists.txt's DEBUG_HOOKS_GENEX is $<OR:$<CONFIG:Debug>,$<BOOL:...>>),
# so a Release directory with it ON is a legitimate harness build.
mkcache relhooks Release ON "Unix Makefiles"
expect accept relhooks "Release + ENABLE_DEBUG_HOOKS=ON"

mkcache relhooks1 Release 1 "Unix Makefiles"
expect accept relhooks1 "Release + ENABLE_DEBUG_HOOKS=1"

mkcache relhookstrue Release true "Unix Makefiles"
expect accept relhookstrue "Release + ENABLE_DEBUG_HOOKS=true (lowercase)"

# A multi-config generator keeps no CMAKE_BUILD_TYPE in the cache, so the
# cache cannot answer the question; `cmake --build` there defaults to Debug.
mkcache multiconfig "" OFF "Ninja Multi-Config"
expect accept multiconfig "multi-config generator, empty build type"

mkcache xcode "" OFF "Xcode"
expect accept xcode "Xcode generator"

# Absent directory: the caller configures it Debug itself.
expect accept absent-dir "directory does not exist"

# --- refused: everything that would silently build without the hooks ---

mkcache release Release OFF "Unix Makefiles"
expect refuse release "CMAKE_BUILD_TYPE=Release"

mkcache relwithdeb RelWithDebInfo OFF "Unix Makefiles"
expect refuse relwithdeb "CMAKE_BUILD_TYPE=RelWithDebInfo"

mkcache minsize MinSizeRel OFF "Unix Makefiles"
expect refuse minsize "CMAKE_BUILD_TYPE=MinSizeRel"

# Single-config generator with no build type is the same trap: $<CONFIG:Debug>
# is false, so the DEBUG define and the harness sources are both absent.
mkcache notype "" OFF "Unix Makefiles"
expect refuse notype "single-config generator, empty build type"

# Exists but was never configured: `cmake --build` would fail confusingly.
mkdir -p "${TMP}/nocache"
expect refuse nocache "directory exists with no CMakeCache.txt"

# --- the message itself carries the three things a reader needs ---

msg="$(require_debug_host_build "${TMP}/release" test 2>&1)"
for needle in "CMAKE_BUILD_TYPE=Release" "needs CMAKE_BUILD_TYPE=Debug" \
              "-DCMAKE_BUILD_TYPE=Debug" "build/host-release"; do
    if printf '%s' "${msg}" | grep -qF -- "${needle}"; then
        pass=$((pass + 1))
        printf 'ok   %-44s present\n' "message names ${needle}"
    else
        fail=$((fail + 1))
        printf 'FAIL %-44s missing\n' "message names ${needle}"
    fi
done

echo
echo "require-debug-host-build-test: ${pass} passed, ${fail} failed"
[ "${fail}" -eq 0 ]
