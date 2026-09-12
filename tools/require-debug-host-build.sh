# shellcheck shell=bash
#
# Guard for the shared build/host directory: it must carry the `#if DEBUG`
# harness code, or the harnesses that build it produce a binary that silently
# ignores every flag they pass.
#
# THE FAILURE THIS PREVENTS
# ------------------------
# Four scripts build and run $REPO_ROOT/build/host:
#
#   tools/frame-data/run.sh          tools/rollback-determinism/run.sh
#   tools/frame-data/run-suite.sh    tools/ldreq-timing/run.sh
#
# All four drive the binary with `--test-*` flags, and every one of those
# flags lives in src/test/test_runner.c and src/test/input_script.c, which are
# whole-file `#if defined(DEBUG)` (CMakeLists.txt's DEBUG_HOOKS_GENEX gates
# both the sources and the DEBUG compile define). All four also configure the
# directory Debug ONLY IF IT DOES NOT ALREADY EXIST -- so a build/host that
# somebody else configured Release is reused as-is, and the build succeeds.
#
# The binary that comes out has no `--test-*` hooks compiled in at all. It
# parses the flags into nothing, boots a normal game, never reaches the
# scripted exit, and gets killed by the wall-clock cap. The frame-data harness
# then reports
#
#     error: game hung and was killed by the timeout (Ns cap for N labels)
#
# for every corpus -- a message that points squarely at the engine. Two
# separate lanes lost roughly half an hour each to that misdiagnosis before
# the cause was traced to the build type. This guard converts it into a
# refusal that names the actual problem.
#
# WHY IT REFUSES RATHER THAN RECONFIGURING
# ----------------------------------------
# build/host is shared. Reconfiguring it would silently discard a directory
# the user built for another purpose, and the switch invalidates every object
# file, so a Release/Debug flip-flop between two harnesses costs a full
# rebuild each way. The tree's own convention is one directory per
# configuration -- tools/gates/run-gates.sh puts its Release host build in
# build/host-release and its Debug netplay-test build in build/host-nptest --
# so the fix is to move the Release build to where it belongs, which is a
# decision for whoever made it, not for this script.

# _rdhb_cache_value <cachefile> <entry-name>
#
# CMakeCache.txt lines are `NAME:TYPE=VALUE`. Prints the value, or nothing
# when the entry is absent.
_rdhb_cache_value() {
    sed -n "s/^$2:[^=]*=//p" "$1" 2>/dev/null | head -1
}

# require_debug_host_build <build_dir> <tag>
#
# Returns 0 when <build_dir> is safe to build a harness binary in: absent (the
# caller then configures it Debug itself), configured Debug, configured with
# -DENABLE_DEBUG_HOOKS=ON (which compiles the same DEBUG code into a non-Debug
# configuration -- see CMakeLists.txt's ENABLE_DEBUG_HOOKS), or configured with
# a multi-config generator, whose per-config build type is not in the cache to
# check and whose default `cmake --build` config is Debug anyway.
#
# Returns 1, after printing what is wrong and how to fix it, otherwise.
# Callers are `set -e` scripts and invoke this as `... || exit 1`.
require_debug_host_build() {
    local build_dir="$1"
    local tag="${2:-harness}"
    local cache="${build_dir}/CMakeCache.txt"

    [ -d "$build_dir" ] || return 0

    if [ ! -f "$cache" ]; then
        echo "error: [$tag] $build_dir exists but contains no CMakeCache.txt, so it is not a" >&2
        echo "       configured CMake build directory. This script only configures build/host when the" >&2
        echo "       directory is absent, so it will not configure this one." >&2
        echo "" >&2
        echo "       Fix: remove it and re-run, or configure it yourself:" >&2
        echo "           rm -rf $build_dir" >&2
        return 1
    fi

    local generator build_type debug_hooks
    generator="$(_rdhb_cache_value "$cache" CMAKE_GENERATOR)"
    build_type="$(_rdhb_cache_value "$cache" CMAKE_BUILD_TYPE)"
    debug_hooks="$(_rdhb_cache_value "$cache" ENABLE_DEBUG_HOOKS)"

    case "$(printf '%s' "$debug_hooks" | tr '[:lower:]' '[:upper:]')" in
        1|ON|YES|TRUE|Y) return 0 ;;
    esac

    case "$generator" in
        Xcode|"Ninja Multi-Config"|"Visual Studio"*) return 0 ;;
    esac

    [ "$build_type" = "Debug" ] && return 0

    local found="CMAKE_BUILD_TYPE=${build_type}"
    [ -n "$build_type" ] || found="an empty CMAKE_BUILD_TYPE"

    echo "error: [$tag] $build_dir is configured $found, but this harness" >&2
    echo "       needs CMAKE_BUILD_TYPE=Debug." >&2
    echo "" >&2
    echo "       The --test-* flags this harness drives the game with live in src/test/test_runner.c" >&2
    echo "       and src/test/input_script.c, which are whole-file '#if defined(DEBUG)'. A non-Debug" >&2
    echo "       build compiles none of them, so the build SUCCEEDS and produces a binary that" >&2
    echo "       silently ignores every flag, boots a normal game, never reaches the scripted exit," >&2
    echo "       and is killed by the wall-clock cap -- reported as 'game hung and was killed by the" >&2
    echo "       timeout', which reads like an engine bug and is not one. Refusing here instead." >&2
    echo "" >&2
    echo "       Fix -- reconfigure build/host as Debug (this is NOT done for you: build/host is" >&2
    echo "       shared by tools/frame-data/run.sh, tools/frame-data/run-suite.sh," >&2
    echo "       tools/rollback-determinism/run.sh and tools/ldreq-timing/run.sh, and converting a" >&2
    echo "       directory you configured for something else is its own surprise):" >&2
    echo "           cmake -B ${build_dir} -DCMAKE_BUILD_TYPE=Debug" >&2
    echo "" >&2
    echo "       If you wanted a Release host build, it belongs in build/host-release -- that is" >&2
    echo "       where tools/gates/run-gates.sh puts one, so both can exist at the same time:" >&2
    echo "           cmake -S . -B build/host-release -DCMAKE_BUILD_TYPE=Release" >&2
    echo "" >&2
    echo "       -DENABLE_DEBUG_HOOKS=ON also satisfies this check: it compiles the same DEBUG code" >&2
    echo "       into a non-Debug configuration (see CMakeLists.txt's ENABLE_DEBUG_HOOKS)." >&2
    return 1
}
