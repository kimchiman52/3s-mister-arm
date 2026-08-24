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

# EXTRA_CMAKE_ARGS is forwarded as a fourth positional into the heredoc below.
# The heredoc uses <<'EOF' (single-quoted) so host-side variable expansion is
# disabled; positional args are the only way to smuggle values in.
docker exec -i "${container_name}" bash -s -- \
    "${platform}" "${flavor}" "${jobs}" "${EXTRA_CMAKE_ARGS:-}" "${git_short_sha}" \
    "${lane}" "${workdir}" "${lane_wait_secs}" "${build_nonce}" "${host_fingerprint}" <<'EOF'
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
# when the holding process dies. That matters here because builds on this
# machine have been killed by tool timeouts and background-task lifetime caps;
# a lock implemented as a mkdir/PID file would have survived those kills and
# wedged the lane permanently.
lock_file="/var/lock/3s-mister-build-lane-${lane}.lock"
owner_file="${lock_file}.owner"
mkdir -p /var/lock

# Print the recorded owner of the lane, indented, on stderr. Written as an
# if/else rather than `sed ... 2>/dev/null >&2 || echo ...` because in that
# form the redirections apply left to right: fd2 becomes /dev/null and then
# fd1 is dup'd from the *new* fd2, so the holder record is discarded and the
# operator sees an empty "current holder:" heading -- which is precisely the
# information they need to decide whether to wait or use another lane.
print_lane_holder() {
    if [ -f "${owner_file}" ]; then
        sed 's/^/         /' "${owner_file}" >&2
    else
        echo "         (no owner record)" >&2
    fi
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
rsync -a --delete \
    --exclude='.git/' \
    --exclude='.claude/' \
    --exclude='build/' \
    --exclude='third_party/sdl3/build/' \
    /src/ "${workdir}/"
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

    echo "cmake (final invocation): cmake -S . -B ${build_dir} -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=${telemetry_flag} ${cmake_target_args[*]-} ${hash_args[*]-} ${extra_args[*]-}"
    cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON \
        -DENABLE_PERF_TELEMETRY="${telemetry_flag}" \
        "${cmake_target_args[@]}" \
        "${hash_args[@]}" \
        "${extra_args[@]}"
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
