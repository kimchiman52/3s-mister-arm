#!/usr/bin/env bash
set -euo pipefail

# Provision a Buildroot-glibc cross-compile container for the Miyoo
# Mini Plus / OnionOS port. Built on top of shauninman's
# union-miyoomini-toolchain image, which ships the Buildroot Buster
# glibc + arm-linux-gnueabihf-gcc toolchain matching Onion's runtime.
#
# We DON'T reuse the MiSTer Bullseye/Clang container because Bullseye
# glibc is newer than Onion's Buildroot glibc; symbol-version mismatch
# is the established failure mode for cross-targeting Onion.
#
# Usage:
#   tools/miyoo/setup-build-container.sh
#
# Env vars (with defaults):
#   MIYOO_BUILD_CONTAINER     Docker container name (default: 3s-miyoo-arm-build).
#   MIYOO_DOCKER_PLATFORM     Docker platform (default: linux/amd64).
#   MIYOO_TOOLCHAIN_SRC_DIR   Local clone of union-miyoomini-toolchain
#                             (default: /tmp/union-miyoomini-toolchain).
#   MIYOO_TOOLCHAIN_GIT_REF   Git ref / commit to pin (default: a verified
#                             commit SHA on shauninman/union-miyoomini-toolchain
#                             `main` — see vendor/sigmastar.UPSTREAM.md).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

container_name="${MIYOO_BUILD_CONTAINER:-3s-miyoo-arm-build}"
platform="${MIYOO_DOCKER_PLATFORM:-linux/amd64}"
toolchain_src="${MIYOO_TOOLCHAIN_SRC_DIR:-/tmp/union-miyoomini-toolchain}"
# Pinned SHA on shauninman/union-miyoomini-toolchain `main` (snapshot
# 2026-05-03). Pinning to a SHA — not a moving branch — guarantees the
# next contributor builds against the toolchain that produced any
# already-shipped binary. To re-pin, run:
#   gh api repos/shauninman/union-miyoomini-toolchain/commits/main --jq .sha
# and update both this default and vendor/sigmastar.UPSTREAM.md.
toolchain_git_ref="${MIYOO_TOOLCHAIN_GIT_REF:-cfcae3083679de59d6600d4ba05676c654b6d24e}"
image_tag="${MIYOO_IMAGE_TAG:-local/union-miyoomini-toolchain:cut1}"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 2
    fi
}

require_cmd docker

if docker ps -a --format '{{.Names}}' | grep -qx "${container_name}"; then
    existing_src_mount="$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/src"}}{{.Source}}{{end}}{{end}}' "${container_name}")"
    if [ -n "${existing_src_mount}" ] && [ "${existing_src_mount}" != "${ROOT_DIR}" ]; then
        echo "existing container '${container_name}' is mounted from '${existing_src_mount}', expected '${ROOT_DIR}'" >&2
        echo "reuse that checkout or recreate the container deliberately before running this helper" >&2
        exit 1
    fi

    docker start "${container_name}" >/dev/null
else
    # Build the union-miyoomini-toolchain image locally from upstream.
    # NOTE: there is no verified public Docker registry path for this
    # image. We clone the upstream repo and `docker build` it locally.
    if ! docker image inspect "${image_tag}" >/dev/null 2>&1; then
        if [ ! -d "${toolchain_src}/.git" ]; then
            echo "Cloning union-miyoomini-toolchain into ${toolchain_src}..." >&2
            rm -rf "${toolchain_src}"
            # Full clone (no --depth, no --branch) so we can check out a
            # pinned SHA. SHAs are not directly fetchable as refs on
            # GitHub HTTPS; cloning the full repo and then checking out
            # the SHA is the portable way to honor MIYOO_TOOLCHAIN_GIT_REF
            # whether it's a SHA, branch, or tag.
            git clone https://github.com/shauninman/union-miyoomini-toolchain \
                "${toolchain_src}"
            git -C "${toolchain_src}" checkout --detach "${toolchain_git_ref}"
        fi

        echo "Building Docker image ${image_tag} from ${toolchain_src}..." >&2
        docker build --platform "${platform}" -t "${image_tag}" "${toolchain_src}"
    fi

    docker run -d --name "${container_name}" --platform "${platform}" \
        -v "${ROOT_DIR}":/src -w /src \
        "${image_tag}" sleep infinity >/dev/null
fi

# Install build prerequisites that the toolchain image doesn't ship by
# default: curl (used by build-deps.sh's tf-psa-crypto fetch and FFmpeg
# tarball downloads), rsync (used by tools/miyoo/build-game.sh's
# in-container workdir sync from /src), pkg-config, ca-certificates,
# clang (modern compiler — Buildroot's gcc 8.3 is too old for the
# project's C23 features like `enum Character : uint16_t`).
docker exec "${container_name}" bash -lc '
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    curl ca-certificates rsync pkg-config make gnupg

# Install clang from apt.llvm.org buster repo. We pin clang-13 which
# is the latest LLVM release line that ships Buster packages and which
# fully supports C23 typed-enum underlying types.
if ! command -v clang-13 >/dev/null 2>&1; then
    install -d /usr/share/keyrings
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor --yes -o /usr/share/keyrings/apt.llvm.org.gpg
    cat >/etc/apt/sources.list.d/llvm-buster-13.list <<EOF_LLVM
deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] http://apt.llvm.org/buster/ llvm-toolchain-buster-13 main
EOF_LLVM
    apt-get update
    apt-get install -y --no-install-recommends clang-13
fi
'

# union-miyoomini-toolchain ships with cmake 3.13 (Buster). SDL3 release-3.4.x
# requires cmake >= 3.16. Install Kitware's official cmake binary into
# /opt/cmake-3.27.x and prepend to PATH for build tooling. We pin a version
# that's still distributed as a self-contained tarball with no glibc
# requirements newer than Buster.
docker exec "${container_name}" bash -lc '
set -euxo pipefail
if [ ! -x /opt/cmake/bin/cmake ]; then
    cmake_ver="3.27.9"
    cmake_arch="linux-x86_64"
    cmake_tar="cmake-${cmake_ver}-${cmake_arch}.tar.gz"
    cd /tmp
    if [ ! -f "${cmake_tar}" ]; then
        if command -v curl >/dev/null 2>&1; then
            curl -fsSLO "https://github.com/Kitware/CMake/releases/download/v${cmake_ver}/${cmake_tar}"
        else
            wget -q "https://github.com/Kitware/CMake/releases/download/v${cmake_ver}/${cmake_tar}"
        fi
    fi
    rm -rf /opt/cmake
    mkdir -p /opt/cmake
    tar -xzf "${cmake_tar}" --strip-components=1 -C /opt/cmake
    rm -f "${cmake_tar}"
fi
ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake
ln -sf /opt/cmake/bin/ctest /usr/local/bin/ctest
ln -sf /opt/cmake/bin/cpack /usr/local/bin/cpack
'

docker exec "${container_name}" bash -lc "
set -euxo pipefail
arm-linux-gnueabihf-gcc --version | head -n 1
cmake --version | head -n 1
printf 'container=%s\n' '${container_name}'
printf 'platform=%s\n' '${platform}'
printf 'src_mount=%s\n' '/src'
"
