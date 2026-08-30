# shellcheck shell=bash
#
# Resolve and assert the identity of the Docker daemon a MiSTer build talks to.
# Sourced by tools/mister/build-game.sh and tools/mister/setup-build-container.sh.
#
# Why this exists
# ---------------
# On 2026-08-30 every MiSTer game build on this machine was running on the
# Colima `quartus2` VM -- an *emulated x86_64* VM that exists to host the FPGA
# (Quartus) toolchain, per AGENTS.md:23. Docker Desktop was simply not running,
# so the `docker` CLI fell through to the only live daemon in its context list
# and the builds succeeded. Slower, but green.
#
# Nothing in the build path asserted which daemon it was talking to, so the
# whole stack -- Apple Silicon -> emulated x86_64 VM -> Docker -> debian:11
# amd64 -> clang cross-compiling to armv7, i.e. two layers of translation --
# was invisible to every check we had. MIST_BUILD_HASH proved *what commit* was
# built and the task #53 stamp proved *the build completed*; neither said
# *where*, so a 3x slowdown on the wrong host produced a clean-looking success.
#
# That is the same defect class as task #53 (which tree was compiled) and task
# #116 (which config was compiled). This file adds the third question: which
# machine compiled it. The answer is printed alongside the other provenance
# lines so it is greppable in a build log after the fact.

# Normalise the many spellings of the two architectures we care about.
mister_norm_arch() {
    case "$1" in
    arm64 | aarch64) echo "arm64" ;;
    x86_64 | amd64) echo "x86_64" ;;
    *) echo "$1" ;;
    esac
}

# Populates:
#   MISTER_DOCKER_CONTEXT   active context name (or "<DOCKER_HOST>" when set)
#   MISTER_DOCKER_ENDPOINT  daemon endpoint URI
#   MISTER_DAEMON_OS        linux
#   MISTER_DAEMON_ARCH      raw daemon architecture
#   MISTER_DAEMON_NCPU      cores the daemon can use
#   MISTER_HOST_ARCH        raw host architecture
mister_resolve_docker_host() {
    MISTER_HOST_ARCH="$(uname -m)"

    if [ -n "${DOCKER_HOST:-}" ]; then
        MISTER_DOCKER_CONTEXT="<DOCKER_HOST>"
        MISTER_DOCKER_ENDPOINT="${DOCKER_HOST}"
    else
        MISTER_DOCKER_CONTEXT="$(docker context show 2>/dev/null || echo unknown)"
        MISTER_DOCKER_ENDPOINT="$(docker context inspect "${MISTER_DOCKER_CONTEXT}" \
            --format '{{.Endpoints.docker.Host}}' 2>/dev/null || echo unknown)"
    fi

    MISTER_DAEMON_OS=""
    MISTER_DAEMON_ARCH=""
    MISTER_DAEMON_NCPU=""

    # `docker info` against a dead daemon still prints a template-expanded line
    # -- the fields are simply empty -- and exits non-zero. Testing only for
    # empty output therefore misses the single most important case (Docker
    # Desktop not running), and the caller would go on to report it as an
    # architecture mismatch instead of "start Docker". Check the exit status
    # and require a non-empty architecture.
    local info rc
    info="$(docker info --format '{{.OSType}} {{.Architecture}} {{.NCPU}}' 2>/dev/null)"
    rc=$?
    [ "${rc}" -ne 0 ] && return 1

    read -r MISTER_DAEMON_OS MISTER_DAEMON_ARCH MISTER_DAEMON_NCPU <<<"${info}"
    if [ -z "${MISTER_DAEMON_ARCH}" ] || [ -z "${MISTER_DAEMON_OS}" ]; then
        MISTER_DAEMON_OS=""
        MISTER_DAEMON_ARCH=""
        MISTER_DAEMON_NCPU=""
        return 1
    fi
    return 0
}

# Print the greppable provenance line. Callers print this on every build.
mister_print_docker_host() {
    printf 'docker daemon: context=%s endpoint=%s os=%s arch=%s ncpu=%s host_arch=%s\n' \
        "${MISTER_DOCKER_CONTEXT}" "${MISTER_DOCKER_ENDPOINT}" \
        "${MISTER_DAEMON_OS:-unreachable}" "${MISTER_DAEMON_ARCH:-unreachable}" \
        "${MISTER_DAEMON_NCPU:-0}" "${MISTER_HOST_ARCH}"
}

# Resolve, print, and refuse to build on a daemon that is not the native host
# Docker. Set MISTER_ALLOW_FOREIGN_DOCKER_HOST=1 to downgrade the refusals to
# warnings -- deliberate use is fine, the silent fallback is not.
mister_assert_docker_host() {
    if ! mister_resolve_docker_host; then
        mister_print_docker_host
        echo "ERROR: no reachable Docker daemon for context '${MISTER_DOCKER_CONTEXT}'." >&2
        echo "       endpoint: ${MISTER_DOCKER_ENDPOINT}" >&2
        echo "       Start Docker Desktop and retry:" >&2
        echo "           open -a Docker && docker context use default" >&2
        exit 8
    fi

    mister_print_docker_host

    local allow="${MISTER_ALLOW_FOREIGN_DOCKER_HOST:-0}"
    local problem=""

    # The Quartus VM is not a general-purpose build host. It is an emulated
    # x86_64 Colima profile whose disk lives on an external drive and whose job
    # is the FPGA toolchain (AGENTS.md:23). Building the game there is always a
    # mistake, even when the architectures happen to line up.
    case "${MISTER_DOCKER_ENDPOINT}" in
    *.colima/quartus*) problem="the Colima Quartus VM (FPGA toolchain host, emulated x86_64)" ;;
    esac

    # The general form of the same defect: any daemon whose architecture is not
    # the host's is a translated VM, and every compile inside it pays for that.
    if [ -z "${problem}" ]; then
        local host_norm daemon_norm
        host_norm="$(mister_norm_arch "${MISTER_HOST_ARCH}")"
        daemon_norm="$(mister_norm_arch "${MISTER_DAEMON_ARCH}")"
        if [ "${host_norm}" != "${daemon_norm}" ]; then
            problem="a ${MISTER_DAEMON_ARCH} daemon on a ${MISTER_HOST_ARCH} host (emulated VM)"
        fi
    fi

    if [ -n "${problem}" ]; then
        if [ "${allow}" = "1" ]; then
            echo "WARNING: building against ${problem}." >&2
            echo "         Allowed by MISTER_ALLOW_FOREIGN_DOCKER_HOST=1." >&2
        else
            echo "ERROR: the active Docker daemon is ${problem}." >&2
            echo "       context:  ${MISTER_DOCKER_CONTEXT}" >&2
            echo "       endpoint: ${MISTER_DOCKER_ENDPOINT}" >&2
            echo "       This is not the native host Docker, and a build here is" >&2
            echo "       silently translated -- the failure this check exists to catch." >&2
            echo "       Use the native daemon:" >&2
            echo "           open -a Docker && docker context use default" >&2
            echo "       To build here deliberately, set MISTER_ALLOW_FOREIGN_DOCKER_HOST=1." >&2
            exit 8
        fi
    fi
}

# Default container name for a platform. Platform-derived so an arm64 and an
# amd64 build container can coexist on one daemon: the container's Debian
# architecture is fixed at creation and setup-build-container.sh refuses to
# reuse one whose arch does not match the requested platform. Sharing a single
# name across platforms would turn every platform switch into that refusal and
# force a manual `docker rm`. The amd64 name is the historical one so existing
# containers and their lane workdirs keep working untouched.
mister_default_container_for_platform() {
    case "$1" in
    linux/arm64) echo "3s-mister-arm64-build" ;;
    linux/arm/v7) echo "3s-mister-armv7-build" ;;
    *) echo "3s-mister-arm-build" ;;
    esac
}

# The container platform that is native to the resolved daemon. Keeps
# linux/amd64 reachable on an x86_64 host and picks linux/arm64 on Apple
# Silicon, instead of hardcoding one and emulating on the other.
mister_native_platform() {
    case "$(mister_norm_arch "${MISTER_DAEMON_ARCH:-}")" in
    arm64) echo "linux/arm64" ;;
    x86_64) echo "linux/amd64" ;;
    *) echo "linux/amd64" ;;
    esac
}
