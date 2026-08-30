#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SETUP_CONTAINER_SCRIPT="${ROOT_DIR}/tools/mister/setup-build-container.sh"

container_name="${MISTER_BUILD_CONTAINER:-3s-mister-arm-build}"
platform="${MISTER_DOCKER_PLATFORM:-linux/amd64}"
flavor="telemetry"
jobs="${JOBS:-2}"
# Task #53: the container workdir used to be the hardcoded string
# "/work-mister" for every invocation. See the --lane block below.
lane="${MISTER_BUILD_LANE:-mister}"
lane_wait_secs="${MISTER_BUILD_LANE_WAIT:-0}"

usage() {
    cat <<EOF
Usage:
  tools/mister/build-game.sh [options]

Purpose:
  Canonical Docker build for the MiSTer game runtime. This helper uses the
  validated Docker flow, builds in a container-local workdir, and copies ARM
  MiSTer outputs back into the host repo under build/.

Options:
  --flavor <telemetry|clean|both>   Build flavor to produce (default: ${flavor})
  --platform <docker-platform>      Docker platform for the build container
                                    (default: ${platform})
  --container <name>                Docker container name (default: ${container_name})
  --jobs <count>                    Parallel build jobs inside Docker (default: ${jobs})
  --lane <name>                     Build lane. Selects the container workdir and
                                    the exclusive lock that protects it
                                    (default: ${lane}). Lane 'mister' uses the
                                    historical /work-mister workdir; any other
                                    lane <n> uses /work-lane-<n>. Give a parallel
                                    build its own lane so it cannot rsync over a
                                    build already running in another lane.
  --wait-for-lane <seconds>         If the lane is busy, wait up to this many
                                    seconds for it instead of failing
                                    immediately (default: ${lane_wait_secs}).
  --help                            Show this message

Concurrency (task #53):
  Each lane is guarded by an exclusive flock held for the whole in-container
  run -- rsync, dependency build, compile, install and package. A second
  invocation of the same lane cannot start its rsync while the first is still
  building; it fails fast (exit 3) and prints who holds the lane. This replaces
  the previous convention where every invocation shared /work-mister and a
  concurrent run could, and did, overwrite another build's source mid-compile.

  Note that a new lane starts with an empty third_party/ dependency cache and
  will rebuild every dependency from scratch (tens of minutes, several GB of
  container overlay). Prefer sharing the default lane and letting the lock
  serialise the builds unless you specifically need them to run at once.

Worktrees (task #81):
  The build container has a single /src bind mount, fixed to whichever checkout
  created it. When this script runs from a different checkout -- any git
  worktree -- it stages that checkout into <mount>/.build-src/<nonce>/ and the
  container rsyncs from there, so a lane can verify its own branch on ARM
  without merging first. The staging directory is per invocation, not per lane,
  so it is never shared mutable state, and it is removed on exit. What was
  actually compiled is still proven by the task #53 fingerprint and nonce
  canary, which compare against this checkout, not against the mount.

Defaults:
  - The default platform is linux/amd64 because it is the most portable Docker
    path across macOS and other hosts that cannot execute linux/arm/v7
    containers locally.
  - linux/amd64 uses the validated ARM cross-build flow and still produces a
    real ARM hard-float MiSTer package.
  - linux/arm/v7 is supported when the host has binfmt_misc/QEMU support and you
    explicitly want a native ARM container build.

Environment:
  EXTRA_CMAKE_ARGS                  Space-separated extra -D... flags forwarded
                                    verbatim to the inner cmake configure step.
                                    Netplay is ON by default for MiSTer builds
                                    (CMakeLists.txt PORT_MISTER block); to build
                                    the rare netplay-off exception pass
                                    'EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=OFF"'.
                                    Values with embedded whitespace or shell
                                    quoting are not supported; pass each
                                    \`-Dkey=value\` as a separate whitespace-
                                    delimited token.

Outputs:
  telemetry -> build/mister-telemetry-install, build/mister-telemetry-package
  clean     -> build/mister-clean-install, build/mister-clean-package
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --flavor)
        flavor="$2"
        shift 2
        ;;
    --platform)
        platform="$2"
        shift 2
        ;;
    --container)
        container_name="$2"
        shift 2
        ;;
    --jobs)
        jobs="$2"
        shift 2
        ;;
    --lane)
        lane="$2"
        shift 2
        ;;
    --wait-for-lane)
        lane_wait_secs="$2"
        shift 2
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        echo "unknown option: $1" >&2
        exit 2
        ;;
    esac
done

case "${flavor}" in
telemetry|clean|both)
    ;;
*)
    echo "--flavor must be one of telemetry, clean, or both" >&2
    exit 2
    ;;
esac

if ! [[ "${jobs}" =~ ^[0-9]+$ ]] || [ "${jobs}" -le 0 ]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi

# The lane name is interpolated into container paths (/work-lane-<name>) and
# into a lock filename, so constrain it to characters that cannot escape a
# path component or need quoting.
if ! [[ "${lane}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
    echo "--lane must match ^[A-Za-z0-9][A-Za-z0-9._-]*$ (got '${lane}')" >&2
    exit 2
fi

if ! [[ "${lane_wait_secs}" =~ ^[0-9]+$ ]]; then
    echo "--wait-for-lane must be a non-negative integer number of seconds" >&2
    exit 2
fi

# Lane 'mister' keeps the historical /work-mister workdir so that the default
# single-user invocation reuses the dependency cache that is already there and
# behaves exactly as before. Every other lane gets its own tree.
if [ "${lane}" = "mister" ]; then
    workdir="/work-mister"
else
    workdir="/work-lane-${lane}"
fi

require_cmd docker

"${SETUP_CONTAINER_SCRIPT}" --container "${container_name}" --platform "${platform}"

# R-1: the rsync into the container excludes .git/, so CMake's configure-time
# git probe for the MIST handshake build_hash cannot work inside Docker.
# Derive the short SHA here on the host and forward it explicitly. Empty on
# failure (e.g. tarball checkout) -> CMake falls back to "0000000".
git_short_sha="$(git -C "${ROOT_DIR}" rev-parse --short=7 HEAD 2>/dev/null || true)"

# Task #53 clobber canary. This nonce is written into a stamp file inside the
# container workdir immediately after our rsync, and read back after the build
# finishes. The stamp lives at a path that does not exist in /src, so any
# competing `rsync -a --delete /src/ <workdir>/` -- the exact operation that
# overwrote a build's source mid-compile -- deletes or replaces it. A stamp
# that still carries our nonce at the end is positive evidence that the tree
# we compiled is the tree we synced.
build_nonce="$(date -u +%Y%m%dT%H%M%SZ)-$$-${RANDOM}"

# Host-side fingerprint of the source that we are about to ship into the
# container, recomputed in the container after the rsync and compared. cksum
# is POSIX and the CRC is specified by the standard, so macOS and GNU
# coreutils agree on the value for identical bytes. This answers "was the
# in-container source the source I meant to build?" with a measurement rather
# than an assumption.
source_fingerprint() {
    find src CMakeLists.txt -type f \
        -not -path '*/build/*' \
        -not -path '*/.git/*' \
        -not -path '*/.claude/*' 2>/dev/null \
        | LC_ALL=C sort \
        | tr '\n' '\0' \
        | xargs -0 cksum \
        | cksum \
        | awk '{print $1"-"$2}'
}
host_fingerprint="$(cd "${ROOT_DIR}" && source_fingerprint)"

# -------------------------------------------------------------------------
# Task #81 -- build the caller's checkout, not the container's bind mount
# -------------------------------------------------------------------------
#
# The container has exactly one bind mount, /src, fixed at creation time to
# whichever checkout created it. The in-container step below rsyncs its source
# from a path inside the container, so when ROOT_DIR is a git worktree the tree
# that actually reaches the compiler has to be put somewhere the container can
# see. Re-pointing the mount would mean recreating the container and losing the
# lane workdirs and their prebuilt ARM dependency trees.
#
# So: when ROOT_DIR is the mount source, nothing changes and the historical
# `/src/` path is used verbatim. Otherwise ROOT_DIR is staged into a directory
# underneath the mount and the container rsyncs from there instead.
#
# The staging directory is keyed by ${build_nonce}, which is unique per
# invocation, rather than by lane. That is deliberate: a lane-keyed staging
# directory would be shared mutable state that a second invocation of the same
# lane could overwrite while the first was still rsyncing out of it, which is a
# new instance of exactly the clobber that task #53 removed. Per-invocation
# staging is not shared at all, so no second process can write to the directory
# this build reads from, and the lane flock continues to be the only thing
# serialising access to the workdir.
mount_src="$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/src"}}{{.Source}}{{end}}{{end}}' "${container_name}" 2>/dev/null || true)"
staging_host_dir=""
container_src="/src/"

if [ -n "${mount_src}" ] && [ "${mount_src}" != "${ROOT_DIR}" ]; then
    staging_host_dir="${mount_src}/.build-src/${build_nonce}"
    container_src="/src/.build-src/${build_nonce}/"

    # Reap staging directories stranded by builds that died without running
    # their trap -- SIGKILL, or the host going down. Nothing else will ever
    # remove them: they are keyed by a nonce no later build reuses, so without
    # this they accumulate at ~600 MB each inside the container's overlay,
    # which has run out of space mid-compile before.
    #
    # An age threshold is safe rather than merely convenient, because the
    # container deletes its own staging directory immediately after rsyncing
    # out of it. A live build's staging therefore survives seconds, not hours,
    # and anything this old cannot belong to a build still using it. The
    # threshold is left generous so that even a build parked on
    # --wait-for-lane is never reaped out from under itself.
    staging_reap_minutes="${MISTER_BUILD_STAGING_REAP_MINUTES:-360}"
    if [ -d "${mount_src}/.build-src" ]; then
        while IFS= read -r stale; do
            [ -n "${stale}" ] || continue
            echo "reaping stranded staging directory: ${stale}" >&2
            rm -rf "${stale}"
        done < <(find "${mount_src}/.build-src" -mindepth 1 -maxdepth 1 -type d \
            -mmin "+${staging_reap_minutes}" 2>/dev/null)
    fi

    # Advisory only. Staging copies the whole checkout, so doing it before
    # discovering the lane is busy wastes a few hundred MB of writes and a good
    # chunk of wall time. This probe acquires and immediately releases the lane
    # lock just to answer "is it busy right now?". It is deliberately not
    # treated as ownership: the lane can be taken between this probe and the
    # real acquisition inside the container, so the authoritative check remains
    # the flock held across the actual build. Skipped when the caller asked to
    # wait, since then a busy lane is not an error.
    if [ "${lane_wait_secs}" -eq 0 ]; then
        if ! docker exec "${container_name}" \
            flock -n "/var/lock/3s-mister-build-lane-${lane}.lock" -c true 2>/dev/null; then
            echo "ERROR: build lane '${lane}' is already in use (workdir ${workdir})." >&2
            echo "       Refusing to stage source for a build that cannot start." >&2
            echo "       Either wait (--wait-for-lane <seconds>) or pick another lane" >&2
            echo "       (--lane <name>); note a fresh lane rebuilds all dependencies." >&2
            exit 3
        fi
    fi

    # Remove the staging copy however we exit, including on failure, so a
    # killed build cannot leave ~600 MB of source inside the shared checkout.
    #
    # This trap covers a normal exit, an error under `set -e`, SIGINT and
    # SIGTERM. It cannot cover SIGKILL or the machine losing power, because no
    # handler runs in those cases. That gap is not hypothetical here: long ARM
    # builds on this machine have been killed by tool timeouts, and the
    # container has run out of space mid-compile before, which presents as an
    # unrelated ENOSPC rather than as "a dead build left its staging behind".
    #
    # Two things narrow it. The container deletes the staging directory itself
    # as soon as it has rsynced out of it (see below), so the directory only
    # exists for the first few seconds of a build rather than for its whole
    # duration; and the reaper above sweeps anything a SIGKILL still managed to
    # strand.
    cleanup_staging() {
        if [ -n "${staging_host_dir}" ] && [ -d "${staging_host_dir}" ]; then
            rm -rf "${staging_host_dir}"
        fi
    }
    # INT and TERM must exit explicitly. Trapping them to a handler that only
    # cleans up leaves bash resuming the script afterwards, so a SIGTERM'd build
    # deletes its own staging directory and then carries on compiling out of a
    # tree it just removed -- measured, not theorised: with a plain
    # `trap cleanup_staging EXIT INT TERM` the build was still running two
    # minutes after the TERM was delivered.
    #
    # Note that bash cannot run either handler while a foreground command is in
    # progress, so a signal arriving mid-build is only serviced once the inner
    # `docker exec` returns. That is precisely why the staging directory is
    # released container-side seconds into the build rather than here: by the
    # time a mid-build signal can be serviced there is normally nothing left to
    # clean. Killing this process also does not stop the container-side build,
    # which keeps the lane locked -- see the lane-lock note above.
    trap cleanup_staging EXIT
    trap 'cleanup_staging; exit 130' INT
    trap 'cleanup_staging; exit 143' TERM

    echo "staging ${ROOT_DIR} -> ${staging_host_dir} (task #81)"
    mkdir -p "${staging_host_dir}"
    # Same exclude set as the in-container rsync below, so the staged tree is
    # byte-identical to what a same-checkout build would have shipped.
    #
    # Task #90: --copy-unsafe-links. See the matching comment on the
    # in-container rsync below; both copies of this command need it, because a
    # worktree's tree passes through both.
    rsync -a --delete --copy-unsafe-links \
        --exclude='.git/' \
        --exclude='.claude/' \
        --exclude='.build-src/' \
        --exclude='build/' \
        --exclude='third_party/sdl3/build/' \
        "${ROOT_DIR}/" "${staging_host_dir}/"
fi

# EXTRA_CMAKE_ARGS is forwarded as a fourth positional into the heredoc below.
# The heredoc uses <<'EOF' (single-quoted) so host-side variable expansion is
# disabled; positional args are the only way to smuggle values in.
docker exec -i "${container_name}" bash -s -- \
    "${platform}" "${flavor}" "${jobs}" "${EXTRA_CMAKE_ARGS:-}" "${git_short_sha}" \
    "${lane}" "${workdir}" "${lane_wait_secs}" "${build_nonce}" "${host_fingerprint}" \
    "${container_src}" <<'EOF'
set -euo pipefail

platform="$1"
flavor="$2"
jobs="$3"
extra_cmake_args="$4"
git_short_sha="$5"
lane="$6"
workdir="$7"
lane_wait_secs="$8"
build_nonce="$9"
host_fingerprint="${10}"
# Task #81: "/src/" for a build of the container's own checkout, or a
# per-invocation staging path underneath it for a build of another worktree.
container_src="${11}"
llvm_version="${MISTER_LLVM_VERSION:-20}"

# -----------------------------------------------------------------------
# Task #53 -- exclusive lane lock
# -----------------------------------------------------------------------
#
# Held from before the rsync until the build, install and package are all
# done. The lock file deliberately lives outside ${workdir}: the rsync we are
# about to run is `--delete`, so anything inside the workdir that is not in
# /src gets removed, and the lock must also exist before the workdir does.
#
# flock is an advisory lock on an open file descriptor, which the kernel drops
# when the last descriptor referring to it is closed. That matters here because
# builds on this machine have been killed by tool timeouts and background-task
# lifetime caps; a lock implemented as a mkdir/PID file would have survived
# those kills and wedged the lane permanently.
#
# One correction to the above, measured rather than assumed. The lock is NOT
# released merely because the shell below dies. fd 9 is inherited across fork
# and exec, so every descendant -- make, cc1, ld -- holds a copy, and the lock
# survives for as long as any of them does. Observed directly: with the holding
# shell SIGKILLed, a surviving child still had /proc/<pid>/fd/9 open on the lock
# file and `flock -n` from a third process still failed; it succeeded only once
# that child exited.
#
# That is the safer of the two behaviours and is deliberately left alone: an
# orphaned compiler is still writing into ${workdir}, so releasing the lane
# while it runs would re-admit exactly the `rsync --delete` clobber task #53
# exists to prevent. The cost is that a killed build can leave the lane held by
# a process no owner file names, so the busy-lane path below reports the pids
# actually holding the descriptor and not just the recorded owner.
lock_file="/var/lock/3s-mister-build-lane-${lane}.lock"
owner_file="${lock_file}.owner"
mkdir -p /var/lock

# Print the recorded owner of the lane, indented, on stderr. Written as an
# if/else rather than `sed ... 2>/dev/null >&2 || echo ...` because in that
# form the redirections apply left to right: fd2 becomes /dev/null and then
# fd1 is dup'd from the *new* fd2, so the holder record is discarded and the
# operator sees an empty "current holder:" heading -- which is precisely the
# information they need to decide whether to wait or use another lane.
#
# The owner file records the build that *acquired* the lane. It does not
# necessarily name the process still holding it: if that build was killed, the
# lane stays locked by whichever inherited descendant outlived it, and the owner
# record then describes a process that no longer exists. Walking /proc for the
# processes with the lock file actually open is what distinguishes "a build is
# genuinely running" from "a dead build left an orphan behind", which is the
# difference between waiting and killing something. Done by hand rather than
# with fuser/lsof because neither is installed in this container.
print_lock_fd_holders() {
    local found=0 p pid resolved
    # /proc/<pid>/fd/N shows the *resolved* target, and /var/lock is a symlink
    # to /run/lock in this image, so matching on ${lock_file} verbatim silently
    # finds nothing -- it reported "no live process" while nine processes held
    # the descriptor. Compare against the resolved path instead.
    resolved="$(readlink -f "${lock_file}" 2>/dev/null || echo "${lock_file}")"
    for p in /proc/[0-9]*; do
        pid="${p#/proc/}"
        if ls -l "${p}/fd" 2>/dev/null | grep -q " -> ${resolved}\$"; then
            found=1
            echo "         pid ${pid}: $(tr '\0' ' ' <"${p}/cmdline" 2>/dev/null | cut -c1-100)" >&2
        fi
    done
    if [ "${found}" -eq 0 ]; then
        echo "         (no live process holds the lock descriptor)" >&2
    fi
}

print_lane_holder() {
    if [ -f "${owner_file}" ]; then
        sed 's/^/         /' "${owner_file}" >&2
    else
        echo "         (no owner record)" >&2
    fi
    echo "       processes holding the lock descriptor:" >&2
    print_lock_fd_holders
}

exec 9>"${lock_file}"

if ! flock -n 9; then
    if [ "${lane_wait_secs}" -gt 0 ]; then
        echo "lane '${lane}' (${workdir}) is busy; waiting up to ${lane_wait_secs}s..." >&2
        if ! flock -w "${lane_wait_secs}" 9; then
            echo "ERROR: timed out after ${lane_wait_secs}s waiting for lane '${lane}'." >&2
            echo "       current holder:" >&2
            print_lane_holder
            exit 3
        fi
    else
        echo "ERROR: build lane '${lane}' is already in use (workdir ${workdir})." >&2
        echo "       Refusing to rsync over a build that is still running." >&2
        echo "       current holder:" >&2
        print_lane_holder
        echo "       Either wait (--wait-for-lane <seconds>) or pick another lane" >&2
        echo "       (--lane <name>); note a fresh lane rebuilds all dependencies." >&2
        exit 3
    fi
fi

cat >"${owner_file}" <<OWNER
lane=${lane}
workdir=${workdir}
pid=$$
git_short_sha=${git_short_sha}
flavor=${flavor}
nonce=${build_nonce}
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
OWNER

cross_build=0
if [ "${platform}" != "linux/arm/v7" ]; then
    cross_build=1
fi

mkdir -p "${workdir}"
# .claude/ holds this repo's agent worktrees -- entire parallel checkouts of
# other branches, each with its own build output dirs. None of it is a build
# input (CMake globs src/ under ${workdir} only), and the --exclude='build/'
# above does not catch their differently-named output dirs (build-host-debug,
# build-normal, ...). Left in, they dominate the copy: 1.4 GB of .claude vs
# ~0.6 GB of actual source, and the overflow is what filled this container's
# 20 GB overlay and killed the rsync mid-transfer with ENOSPC.
#
# Task #81: '.build-src/' is where a worktree build stages its source inside
# the mount. It is never a build input -- excluding it keeps a concurrent
# worktree build's staging tree out of this lane's workdir, and stops a build
# of the container's own checkout from copying it in.
#
# Task #90: --copy-unsafe-links. `third_party/` is gitignored, so a git
# worktree does not get one from the checkout and lanes create it as an
# absolute symlink into the main checkout instead. Plain `rsync -a` copies
# that symlink verbatim; the host path it names does not exist inside the
# container, so the link dangles and `mkdir -p third_party` in build-deps.sh
# fails with "File exists" -- a mkdir error naming a directory that visibly
# does exist, which reads as a permissions problem rather than a broken link.
# --copy-unsafe-links materialises the contents of any symlink pointing
# outside the transfer root, so a worktree build stages exactly the tree a
# main-checkout build stages. It also replaces a dangling symlink already
# sitting in the destination, so a lane workdir left broken by a pre-fix build
# repairs itself on the next run. Verified against both implementations this
# repo's builds pass through: host openrsync (macOS) and rsync 3.2.3 in the
# container; the `third_party/sdl3/build/` exclude still applies through the
# dereference in both.
rsync -a --delete --copy-unsafe-links \
    --exclude='.git/' \
    --exclude='.claude/' \
    --exclude='.build-src/' \
    --exclude='build/' \
    --exclude='third_party/sdl3/build/' \
    "${container_src}" "${workdir}/"
cd "${workdir}"

# Task #53 -- write the clobber canary immediately after the rsync, while we
# still hold the lane lock. `stamp_file` is not a path in /src, so a competing
# `rsync --delete` into this workdir removes it.
stamp_file="${workdir}/.mister-build-stamp"
cat >"${stamp_file}" <<STAMP
nonce=${build_nonce}
lane=${lane}
git_short_sha=${git_short_sha}
flavor=${flavor}
status=building
synced=$(date -u +%Y-%m-%dT%H:%M:%SZ)
STAMP

# Task #53 -- prove the tree we are about to compile is the tree the caller
# meant to ship, rather than whatever a previous run happened to leave here.
container_fingerprint="$(
    find src CMakeLists.txt -type f \
        -not -path '*/build/*' \
        -not -path '*/.git/*' \
        -not -path '*/.claude/*' 2>/dev/null \
        | LC_ALL=C sort \
        | tr '\n' '\0' \
        | xargs -0 cksum \
        | cksum \
        | awk '{print $1"-"$2}'
)"
if [ "${container_fingerprint}" != "${host_fingerprint}" ]; then
    echo "ERROR: in-container source does not match the host checkout." >&2
    echo "       host      fingerprint: ${host_fingerprint}" >&2
    echo "       container fingerprint: ${container_fingerprint}" >&2
    echo "       workdir: ${workdir}" >&2
    echo "       Refusing to build: the artifact's provenance could not be established." >&2
    exit 4
fi
echo "source fingerprint verified: ${container_fingerprint} (lane=${lane} workdir=${workdir})"

# Task #81 -- drop the staging copy the moment it is no longer needed.
#
# Everything below this point compiles out of ${workdir}; the staged tree has
# already been rsynced in and its contents proven by the fingerprint check
# above, so keeping it for the remaining half hour of the build only risks
# stranding ~600 MB in the container overlay if this process is SIGKILLed.
# Deleting it here rather than in the host-side trap means the window in which
# an abnormal exit can strand anything is the few seconds between staging and
# this line, instead of the whole build.
#
# The guard matters: container_src is exactly "/src/" for a build of the
# container's own checkout, and that is the bind mount of a real working tree.
case "${container_src}" in
/src/.build-src/*)
    rm -rf "${container_src}"
    echo "staging copy released: ${container_src}"
    ;;
esac

export CC="clang-${llvm_version}"
export CXX="clang++-${llvm_version}"

cmake_target_args=()
if [ "${cross_build}" -eq 1 ]; then
    export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
    export CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
    export CXXFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
    export LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr"
    cmake_target_args=(
        -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf
        -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf
    )
fi

# -------------------------------------------------------------------------
# Task #90 -- the host checkout must be unreachable from the ARM dependency
# build
# -------------------------------------------------------------------------
#
# build-deps.sh writes every dependency it builds into ${workdir}/third_party.
# It resolves that path through whatever `third_party` happens to be, so if
# `third_party` is a symlink leading out of the workdir, an ARM build writes
# ARM ELF objects wherever the link lands.
#
# That is not hypothetical. The staged-symlink bug this guard accompanies
# presents as `mkdir: cannot create directory 'third_party': File exists`, and
# the obvious way to "fix" a dangling link is to recreate the missing prefix
# inside the container so it resolves -- for the worktree case that means
# pointing the host checkout path at /src, e.g.
# `ln -sfn /src /Users/sb/Developer/3sx-mister`. /src is the bind mount of the
# host repo. The dependency build then writes straight through it. Observed
# 2026-08-29: host third_party/sdl3, GekkoNet, SDL_net, minizip-ng and
# tf-psa-crypto were all overwritten with ARM artifacts. Nothing failed at
# build time; it surfaced later as the host test harness aborting with
# `Library not loaded: @rpath/libSDL3.0.dylib` (SIGABRT, exit 134), and cost a
# full `build-deps.sh --profile desktop` to recover.
#
# So the workaround is not merely discouraged here, it is refused: the build
# will not start unless third_party resolves inside this lane's own workdir.
# The safe lane-private form of the same workaround -- a link into a
# container-local directory, e.g. `mkdir -p /armdeps/third_party &&
# ln -sfn /armdeps <host-checkout-path>` -- is what the message points at, but
# note that with --copy-unsafe-links above no workaround should be needed at
# all: third_party arrives as a real directory.
if [ -L third_party ]; then
    echo "ERROR: ${workdir}/third_party is a symlink -> $(readlink third_party)" >&2
    echo "       build-deps.sh would build this lane's ARM dependencies through it." >&2
    echo "       Refusing: third_party must be a real directory inside ${workdir}." >&2
    exit 6
fi
third_party_resolved="$(readlink -f third_party 2>/dev/null || true)"
case "${third_party_resolved}" in
"${workdir}"/third_party) ;;
*)
    echo "ERROR: third_party resolves outside this lane's workdir." >&2
    echo "       workdir:  ${workdir}" >&2
    echo "       resolves: ${third_party_resolved:-<unresolvable>}" >&2
    echo "       An ARM dependency build here would write outside the workdir;" >&2
    echo "       if that path is under /src it would overwrite the host checkout's" >&2
    echo "       desktop dependency tree with ARM objects. Refusing." >&2
    exit 6
    ;;
esac

JOBS="${jobs}" bash build-deps.sh --profile mister

build_one() {
    local flavor_name="$1"
    local telemetry_flag="$2"
    local build_dir="build/mister-${flavor_name}"
    local install_dir="build/mister-${flavor_name}-install"
    local package_dir="build/mister-${flavor_name}-package"
    local binary_path="${install_dir}/bin/3s-arm"

    # Split EXTRA_CMAKE_ARGS on whitespace into an array so each -D... token
    # is passed as a distinct argv element (avoids quoting surprises when the
    # string expands). An empty extra_cmake_args yields a zero-length array;
    # "${extra_args[@]}" expands to zero words under bash 4.4+ (container
    # ships bash 5.x), so the cmake invocation below stays well-formed even
    # when no extras are set.
    local extra_args=()
    if [ -n "${extra_cmake_args}" ]; then
        read -ra extra_args <<< "${extra_cmake_args}"
    fi

    # R-1: forward the host-derived git short SHA (5th positional) so the
    # MIST handshake advertises the real build hash despite .git being
    # excluded from the container rsync.
    local hash_args=()
    if [ -n "${git_short_sha}" ]; then
        hash_args=(-DMIST_BUILD_HASH_OVERRIDE="${git_short_sha}")
    fi

    # Task #116 -- restate ENABLE_DEBUG_HOOKS=OFF on every invocation.
    #
    # A cmake option()'s default applies only the FIRST time a build directory
    # is configured; after that the value lives in that directory's
    # CMakeCache.txt and an invocation that simply omits the flag INHERITS it.
    # The lane workdir here is long-lived and shared across invocations, so one
    # measurement build run as
    #
    #     EXTRA_CMAKE_ARGS="-DENABLE_DEBUG_HOOKS=ON" tools/mister/build-game.sh
    #
    # left ENABLE_DEBUG_HOOKS:BOOL=ON in /work-<lane>/build/mister-telemetry,
    # and the NEXT ordinary `--flavor telemetry` build silently produced a
    # -DDEBUG package with the ImGui sources linked in -- observed on
    # 2026-08-29, where the tools/gates/run-gates.sh --arm "shipped config"
    # gate compiled 30 .cpp objects and defined DEBUG while reporting itself as
    # the shipped build. That is a MiSTer package that is not the one anybody
    # asked for, produced with no diagnostic.
    #
    # Naming the OFF default explicitly makes the shipped value independent of
    # whatever the previous invocation left behind. It stays ahead of
    # "${extra_args[@]}", so an intentional EXTRA_CMAKE_ARGS override is the
    # later -D on the command line and still wins.
    local debug_hooks_args=(-DENABLE_DEBUG_HOOKS=OFF)

    echo "cmake (final invocation): cmake -S . -B ${build_dir} -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=${telemetry_flag} ${debug_hooks_args[*]-} ${cmake_target_args[*]-} ${hash_args[*]-} ${extra_args[*]-}"
    cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON \
        -DENABLE_PERF_TELEMETRY="${telemetry_flag}" \
        "${debug_hooks_args[@]}" \
        "${cmake_target_args[@]}" \
        "${hash_args[@]}" \
        "${extra_args[@]}"

    # Task #116 -- assert the resolved value rather than trusting the flag.
    # The point of the whole exercise above is that a stale cache can decide
    # what gets compiled, so read back what cmake actually settled on. Unless
    # the caller named ENABLE_DEBUG_HOOKS in EXTRA_CMAKE_ARGS, an ON here means
    # the package is a DEBUG build with ImGui linked in, and it must not be
    # handed back as a MiSTer package.
    local resolved_debug_hooks
    resolved_debug_hooks="$(sed -n 's/^ENABLE_DEBUG_HOOKS:BOOL=//p' "${build_dir}/CMakeCache.txt")"
    echo "resolved ENABLE_DEBUG_HOOKS=${resolved_debug_hooks:-<unset>}"
    case "${extra_cmake_args}" in
    *ENABLE_DEBUG_HOOKS*) ;;
    *)
        if [ "${resolved_debug_hooks}" != "OFF" ]; then
            echo "ERROR: ENABLE_DEBUG_HOOKS resolved to '${resolved_debug_hooks}' in ${build_dir}" >&2
            echo "       but no caller asked for it. This build would ship a DEBUG" >&2
            echo "       package (ImGui linked, #if DEBUG code compiled in)." >&2
            exit 7
        fi
        ;;
    esac

    cmake --build "${build_dir}" --parallel "${jobs}"
    cmake --install "${build_dir}" --prefix "${install_dir}"
    tools/mister/package.sh "${install_dir}" "${package_dir}"

    readelf -h "${binary_path}" | grep -q "Machine:.*ARM"
    readelf -A "${binary_path}" | grep -q "Tag_ABI_VFP_args"
}

case "${flavor}" in
telemetry)
    build_one telemetry ON
    ;;
clean)
    build_one clean OFF
    ;;
both)
    build_one telemetry ON
    build_one clean OFF
    ;;
esac

# Task #53 -- re-read the canary. If another invocation rsynced over this
# workdir while we were compiling, the stamp is gone or carries a different
# nonce, and the binaries we are about to copy out were linked from a source
# tree we did not verify. Fail rather than hand back an artifact of unknown
# provenance.
if [ ! -f "${stamp_file}" ]; then
    echo "ERROR: build stamp ${stamp_file} disappeared during the build." >&2
    echo "       Another process overwrote ${workdir}; the output is not trustworthy." >&2
    exit 5
fi
if ! grep -qx "nonce=${build_nonce}" "${stamp_file}"; then
    echo "ERROR: build stamp nonce changed during the build." >&2
    echo "       expected nonce=${build_nonce}, found:" >&2
    sed 's/^/         /' "${stamp_file}" >&2
    exit 5
fi

# Re-verify the source fingerprint too: an rsync that happened to restore an
# identical stamp would still be caught if it changed a single source file.
final_fingerprint="$(
    find src CMakeLists.txt -type f \
        -not -path '*/build/*' \
        -not -path '*/.git/*' \
        -not -path '*/.claude/*' 2>/dev/null \
        | LC_ALL=C sort \
        | tr '\n' '\0' \
        | xargs -0 cksum \
        | cksum \
        | awk '{print $1"-"$2}'
)"
if [ "${final_fingerprint}" != "${host_fingerprint}" ]; then
    echo "ERROR: source tree changed under the build." >&2
    echo "       fingerprint at sync: ${host_fingerprint}" >&2
    echo "       fingerprint now:     ${final_fingerprint}" >&2
    exit 5
fi

sed -i 's/^status=building$/status=complete/' "${stamp_file}"
echo "build stamp verified: nonce=${build_nonce} fingerprint=${final_fingerprint}"
EOF

mkdir -p "${ROOT_DIR}/build"

copy_out_dir() {
    local container_src="$1"
    local host_dst_parent="${ROOT_DIR}/build"
    local host_dst_name

    host_dst_name="$(basename "${container_src}")"
    rm -rf "${host_dst_parent}/${host_dst_name}"
    docker cp "${container_name}:${container_src}" "${host_dst_parent}/"
}

# Task #53: these were hardcoded to /work-mister. With lanes that would have
# copied another lane's binaries out of the container while reporting them as
# this invocation's -- the same class of mistake the lane split exists to
# prevent -- so they follow ${workdir}.
case "${flavor}" in
telemetry)
    copy_out_dir "${workdir}/build/mister-telemetry-install"
    copy_out_dir "${workdir}/build/mister-telemetry-package"
    ;;
clean)
    copy_out_dir "${workdir}/build/mister-clean-install"
    copy_out_dir "${workdir}/build/mister-clean-package"
    ;;
both)
    copy_out_dir "${workdir}/build/mister-telemetry-install"
    copy_out_dir "${workdir}/build/mister-telemetry-package"
    copy_out_dir "${workdir}/build/mister-clean-install"
    copy_out_dir "${workdir}/build/mister-clean-package"
    ;;
esac

echo "container=${container_name}"
echo "lane=${lane}"
echo "workdir=${workdir}"
echo "build_hash=${git_short_sha}"
echo "source_fingerprint=${host_fingerprint}"
echo "platform=${platform}"
echo "flavor=${flavor}"
if [ "${platform}" = "linux/arm/v7" ]; then
    echo "mode=native-arm-container"
else
    echo "mode=arm-cross-build"
fi

case "${flavor}" in
telemetry)
    echo "install_prefix=${ROOT_DIR}/build/mister-telemetry-install"
    echo "package_dir=${ROOT_DIR}/build/mister-telemetry-package"
    ;;
clean)
    echo "install_prefix=${ROOT_DIR}/build/mister-clean-install"
    echo "package_dir=${ROOT_DIR}/build/mister-clean-package"
    ;;
both)
    echo "install_prefix_telemetry=${ROOT_DIR}/build/mister-telemetry-install"
    echo "package_dir_telemetry=${ROOT_DIR}/build/mister-telemetry-package"
    echo "install_prefix_clean=${ROOT_DIR}/build/mister-clean-install"
    echo "package_dir_clean=${ROOT_DIR}/build/mister-clean-package"
    ;;
esac
