#!/usr/bin/env bash
set -euo pipefail

# Miyoo Mini Plus / OnionOS cross-build entry point. Mirrors the
# MiSTer build-game.sh structure but targets the Buildroot-glibc
# toolchain (gcc) instead of clang, and packages into the OnionOS
# layout under build/miyoo-package/.
#
# Cut 1 ships a single flavor (no telemetry/clean split). Telemetry
# is left ON by default to match MiSTer's behavior; toggle by
# overriding ENABLE_PERF_TELEMETRY via EXTRA_CMAKE_ARGS.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SETUP_CONTAINER_SCRIPT="${ROOT_DIR}/tools/miyoo/setup-build-container.sh"

container_name="${MIYOO_BUILD_CONTAINER:-3s-miyoo-arm-build}"
platform="${MIYOO_DOCKER_PLATFORM:-linux/amd64}"
jobs="${JOBS:-2}"

usage() {
    cat <<EOF
Usage:
  tools/miyoo/build-game.sh [options]

Purpose:
  Cross-compile the 3s-arm binary for the Miyoo Mini Plus / OnionOS
  inside a Buildroot-glibc Docker container, then package into
  build/miyoo-package/ as a tree mirroring the SD card root.
  Deploy via:
      tools/miyoo/deploy.sh                  # SSH/rsync to 192.168.1.190
      rsync -a build/miyoo-package/ /Volumes/<SD>/   # SD-card fallback
  Resulting on-SD layout:
      Roms/PORTS/Games/3s-arm/                    (binary, lib, launch.sh, ...)
      Roms/PORTS/Shortcuts/Action/3s-arm.port     (OnionOS Ports menu entry)
      Roms/PORTS/Imgs/PUT_3S_ARM_PNG_HERE.txt     (optional icon placeholder)

Options:
  --platform <docker-platform>   Docker platform (default: ${platform})
  --container <name>             Docker container name (default: ${container_name})
  --jobs <count>                 Parallel build jobs inside Docker (default: ${jobs})
  --help                         Show this message

Environment:
  EXTRA_CMAKE_ARGS               Extra -D... flags for the inner cmake
                                 configure step.

Outputs:
  build/miyoo-install     -- cmake install prefix
  build/miyoo-package     -- SD-card-root layout (Roms/PORTS/...) ready to rsync
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

if ! [[ "${jobs}" =~ ^[0-9]+$ ]] || [ "${jobs}" -le 0 ]; then
    echo "--jobs must be a positive integer" >&2
    exit 2
fi

require_cmd docker

MIYOO_BUILD_CONTAINER="${container_name}" MIYOO_DOCKER_PLATFORM="${platform}" \
    "${SETUP_CONTAINER_SCRIPT}"

# Forward EXTRA_CMAKE_ARGS as a positional into the heredoc to dodge
# host-side shell expansion (matches mister/build-game.sh pattern).
docker exec -i "${container_name}" bash -s -- \
    "${platform}" "${jobs}" "${EXTRA_CMAKE_ARGS:-}" <<'EOF'
set -euo pipefail

# union-miyoomini-toolchain sets PATH/CROSS_COMPILE/PREFIX via
# /root/setup-env.sh sourced from .bashrc on login. `docker exec ...
# bash -s` runs a non-login shell so we need to source it explicitly.
if [ -f /root/setup-env.sh ]; then
    # shellcheck source=/dev/null
    . /root/setup-env.sh
fi

platform="$1"
jobs="$2"
extra_cmake_args="$3"
workdir="/work-miyoo"

mkdir -p "${workdir}"
rsync -a --delete \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='third_party/sdl3/build/' \
    --exclude='third_party/sdl3-miyoo/build/' \
    /src/ "${workdir}/"
cd "${workdir}"

# Use clang-13 with the union-miyoomini-toolchain Buildroot sysroot.
# We can't use the toolchain's own gcc 8.3: this codebase relies on C23
# typed-enum underlying types (e.g. `enum Character : uint16_t` in
# src/constants.h) which gcc didn't gain support for until gcc-13.
# clang-13 supports it via -Wno-c2x-extensions like the MiSTer build.
TOOLCHAIN_ROOT=/opt/miyoomini-toolchain/usr
TOOLCHAIN_SYSROOT="${TOOLCHAIN_ROOT}/arm-linux-gnueabihf/sysroot"

export CC="clang-13"
export CXX="clang++-13"
export CFLAGS="--target=arm-linux-gnueabihf --sysroot=${TOOLCHAIN_SYSROOT} --gcc-toolchain=${TOOLCHAIN_ROOT} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
export CXXFLAGS="--target=arm-linux-gnueabihf --sysroot=${TOOLCHAIN_SYSROOT} --gcc-toolchain=${TOOLCHAIN_ROOT} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
export LDFLAGS="--target=arm-linux-gnueabihf --sysroot=${TOOLCHAIN_SYSROOT} --gcc-toolchain=${TOOLCHAIN_ROOT}"

cmake_target_args=(
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR=arm
    -DCMAKE_C_COMPILER="${CC}"
    -DCMAKE_CXX_COMPILER="${CXX}"
    -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf
    -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf
    -DCMAKE_SYSROOT="${TOOLCHAIN_SYSROOT}"
)

JOBS="${jobs}" MISTER_CC_IS_CROSS=1 bash build-deps.sh --profile miyoo

build_dir="build/miyoo"
install_dir="build/miyoo-install"
package_dir="build/miyoo-package"
binary_path="${install_dir}/bin/3s-arm"

# Split EXTRA_CMAKE_ARGS on whitespace into an array.
extra_args=()
if [ -n "${extra_cmake_args}" ]; then
    read -ra extra_args <<< "${extra_cmake_args}"
fi

echo "cmake (final invocation): cmake -S . -B ${build_dir} -DCMAKE_BUILD_TYPE=Release -DPORT_MIYOO_MINI_PLUS=ON -DENABLE_NETPLAY=OFF -DENABLE_ISO_IMPORT=OFF -DENABLE_FFMPEG_ADX=OFF -DENABLE_SDL_DIALOGS=OFF -DENABLE_MISTER_ARM_HARDENING=OFF -DENABLE_PERF_TELEMETRY=OFF ${cmake_target_args[*]-} ${extra_args[*]-}"

cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORT_MIYOO_MINI_PLUS=ON \
    -DENABLE_NETPLAY=OFF \
    -DENABLE_ISO_IMPORT=OFF \
    -DENABLE_FFMPEG_ADX=OFF \
    -DENABLE_SDL_DIALOGS=OFF \
    -DENABLE_MISTER_ARM_HARDENING=OFF \
    -DENABLE_PERF_TELEMETRY=OFF \
    "${cmake_target_args[@]}" \
    "${extra_args[@]}"

cmake --build "${build_dir}" --parallel "${jobs}"
cmake --install "${build_dir}" --prefix "${install_dir}"

tools/miyoo/package.sh "${install_dir}" "${package_dir}"

readelf -h "${binary_path}" | grep -q "Machine:.*ARM"
readelf -A "${binary_path}" | grep -q "Tag_ABI_VFP_args"
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

copy_out_dir /work-miyoo/build/miyoo-install
copy_out_dir /work-miyoo/build/miyoo-package

echo "container=${container_name}"
echo "platform=${platform}"
echo "install_prefix=${ROOT_DIR}/build/miyoo-install"
echo "package_dir=${ROOT_DIR}/build/miyoo-package"
