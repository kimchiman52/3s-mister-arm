#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

container_name="${MISTER_BUILD_CONTAINER:-3s-mister-arm-build}"
platform="${MISTER_DOCKER_PLATFORM:-linux/amd64}"
llvm_version="${MISTER_LLVM_VERSION:-20}"
cross_build_mode="auto"

usage() {
    cat <<EOF
Usage:
  tools/mister/setup-build-container.sh [options]

Options:
  --container <name>        Docker container name (default: ${container_name})
  --platform <value>        Docker platform (default: ${platform})
  --llvm-version <major>    LLVM/Clang major to install from apt.llvm.org (default: ${llvm_version})
  --cross-build             Install ARM cross-build packages even on non-amd64 platforms
  --no-cross-build          Skip ARM cross-build package install
  --help                    Show this message

Defaults:
  - Creates or reuses the Docker container bind-mounted at /src from:
      ${ROOT_DIR}
  - Pins Clang to the official Bullseye LLVM repo.
  - Installs ARM cross-build packages automatically when platform != linux/arm/v7.
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
    --container)
        container_name="$2"
        shift 2
        ;;
    --platform)
        platform="$2"
        shift 2
        ;;
    --llvm-version)
        llvm_version="$2"
        shift 2
        ;;
    --cross-build)
        cross_build_mode="on"
        shift
        ;;
    --no-cross-build)
        cross_build_mode="off"
        shift
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

require_cmd docker

if ! [[ "${llvm_version}" =~ ^[0-9]+$ ]]; then
    echo "--llvm-version must be a numeric major version" >&2
    exit 2
fi

cross_build=0
expected_dpkg_arch=""
case "${cross_build_mode}" in
auto)
    if [ "${platform}" != "linux/arm/v7" ]; then
        cross_build=1
    fi
    ;;
on)
    cross_build=1
    ;;
off)
    cross_build=0
    ;;
*)
    echo "internal error: unexpected cross-build mode '${cross_build_mode}'" >&2
    exit 2
    ;;
esac

case "${platform}" in
linux/amd64)
    expected_dpkg_arch="amd64"
    ;;
linux/arm/v7)
    expected_dpkg_arch="armhf"
    ;;
esac

if docker ps -a --format '{{.Names}}' | grep -qx "${container_name}"; then
    existing_src_mount="$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/src"}}{{.Source}}{{end}}{{end}}' "${container_name}")"
    if [ -n "${existing_src_mount}" ] && [ "${existing_src_mount}" != "${ROOT_DIR}" ]; then
        echo "existing container '${container_name}' is mounted from '${existing_src_mount}', expected '${ROOT_DIR}'" >&2
        echo "reuse that checkout or recreate the container deliberately before running this helper" >&2
        exit 1
    fi

    docker start "${container_name}" >/dev/null
else
    docker run -d --name "${container_name}" --platform "${platform}" -v "${ROOT_DIR}":/src -w /src debian:11 sleep infinity >/dev/null
fi

if [ -n "${expected_dpkg_arch}" ]; then
    actual_dpkg_arch="$(docker exec "${container_name}" dpkg --print-architecture | tr -d '\r')"
    if [ "${actual_dpkg_arch}" != "${expected_dpkg_arch}" ]; then
        echo "existing container '${container_name}' reports Debian architecture '${actual_dpkg_arch}', expected '${expected_dpkg_arch}' for platform '${platform}'" >&2
        echo "recreate the container deliberately if you need a different platform" >&2
        exit 1
    fi
fi

docker exec "${container_name}" bash -lc "
set -euxo pipefail
cat >/etc/apt/sources.list <<'EOF_APT'
deb http://deb.debian.org/debian bullseye main contrib non-free
deb http://deb.debian.org/debian bullseye-updates main contrib non-free
deb http://security.debian.org/debian-security bullseye-security main
deb http://archive.debian.org/debian bullseye-backports main contrib non-free
EOF_APT
apt-get update
apt-get install -y build-essential ca-certificates curl git gpg libasound2-dev make pkg-config rsync zlib1g-dev
install -d /usr/share/keyrings
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor --yes -o /usr/share/keyrings/apt.llvm.org.gpg
rm -f /etc/apt/sources.list.d/llvm-bullseye-*.list
cat >/etc/apt/sources.list.d/llvm-bullseye-${llvm_version}.list <<'EOF_LLVM'
deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-${llvm_version} main
EOF_LLVM
apt-get update
apt-get install -y -t bullseye-backports cmake
apt-get install -y clang-${llvm_version}
"

if [ "${cross_build}" -eq 1 ]; then
    docker exec "${container_name}" bash -lc "
set -euxo pipefail
dpkg --add-architecture armhf
apt-get update
apt-get install -y gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libc6-dev-armhf-cross libstdc++-10-dev-armhf-cross libasound2-dev:armhf zlib1g-dev:armhf libminiupnpc-dev:armhf
"
fi

docker exec "${container_name}" bash -lc "
set -euxo pipefail
cmake --version | head -n 1
clang-${llvm_version} --version | head -n 1
printf 'container=%s\n' '${container_name}'
printf 'platform=%s\n' '${platform}'
printf 'cross_build=%s\n' '${cross_build}'
printf 'src_mount=%s\n' '/src'
"
