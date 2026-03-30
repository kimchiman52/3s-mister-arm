#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SETUP_CONTAINER_SCRIPT="${ROOT_DIR}/tools/mister/setup-build-container.sh"

container_name="${MISTER_BUILD_CONTAINER:-3sx-mister-build}"
platform="${MISTER_DOCKER_PLATFORM:-linux/amd64}"
flavor="telemetry"
jobs="${JOBS:-2}"

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
  --help                            Show this message

Defaults:
  - The default platform is linux/amd64 because it is the most portable Docker
    path across macOS and other hosts that cannot execute linux/arm/v7
    containers locally.
  - linux/amd64 uses the validated ARM cross-build flow and still produces a
    real ARM hard-float MiSTer package.
  - linux/arm/v7 is supported when the host has binfmt_misc/QEMU support and you
    explicitly want a native ARM container build.

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

require_cmd docker

"${SETUP_CONTAINER_SCRIPT}" --container "${container_name}" --platform "${platform}"

docker exec -i "${container_name}" bash -s -- "${platform}" "${flavor}" "${jobs}" <<'EOF'
set -euo pipefail

platform="$1"
flavor="$2"
jobs="$3"
llvm_version="${MISTER_LLVM_VERSION:-20}"
workdir="/work-mister"

cross_build=0
if [ "${platform}" != "linux/arm/v7" ]; then
    cross_build=1
fi

mkdir -p "${workdir}"
rsync -a --delete \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='third_party/sdl3/build/' \
    /src/ "${workdir}/"
cd "${workdir}"

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
    local binary_path="${install_dir}/bin/3sx"

    cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON \
        -DENABLE_PERF_TELEMETRY="${telemetry_flag}" \
        "${cmake_target_args[@]}"
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

case "${flavor}" in
telemetry)
    copy_out_dir /work-mister/build/mister-telemetry-install
    copy_out_dir /work-mister/build/mister-telemetry-package
    ;;
clean)
    copy_out_dir /work-mister/build/mister-clean-install
    copy_out_dir /work-mister/build/mister-clean-package
    ;;
both)
    copy_out_dir /work-mister/build/mister-telemetry-install
    copy_out_dir /work-mister/build/mister-telemetry-package
    copy_out_dir /work-mister/build/mister-clean-install
    copy_out_dir /work-mister/build/mister-clean-package
    ;;
esac

echo "container=${container_name}"
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
