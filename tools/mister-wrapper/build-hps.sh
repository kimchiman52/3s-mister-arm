#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_DIR="${ROOT_DIR}/vendor/Main_MiSTer"
UPSTREAM_FILE="${ROOT_DIR}/vendor/Main_MiSTer.UPSTREAM.md"
OVERLAY_MANIFEST="${ROOT_DIR}/tools/mister-wrapper/main-mister-overlay.files"
BUILD_MANIFEST="${ROOT_DIR}/tools/mister-wrapper/Makefile.full.3s-arm"
MENU_PATCH="${ROOT_DIR}/tools/mister-wrapper/main-mister-full-menu.patch"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/build/mister-wrapper-hps}"
BUILD_SRC_DIR="${OUTPUT_DIR}/src"
TOOLCHAIN_PREFIX="${MISTER_TOOLCHAIN_PREFIX:-arm-none-linux-gnueabihf}"
CC_BIN="${TOOLCHAIN_PREFIX}-gcc"
LD_BIN="${TOOLCHAIN_PREFIX}-ld"
STRIP_BIN="${TOOLCHAIN_PREFIX}-strip"
UPSTREAM_URL="${MISTER_WRAPPER_HPS_UPSTREAM_URL:-https://github.com/MiSTer-devel/Main_MiSTer.git}"
UPSTREAM_COMMIT="${MISTER_WRAPPER_HPS_UPSTREAM_COMMIT:-3380931329b8acb442bd3d35a24d89f88641b7cf}"
DOCKER_CONTEXT_DIR="${ROOT_DIR}/vendor/Main_MiSTer/.devcontainer"
DOCKERFILE_PATH="${DOCKER_CONTEXT_DIR}/Dockerfile"
DOCKER_IMAGE="${MISTER_WRAPPER_HPS_IMAGE:-3s-mister-arm-wrapper-hps}"
DOCKER_PLATFORM="${MISTER_WRAPPER_HPS_DOCKER_PLATFORM:-linux/amd64}"
CONTAINER_ROOT="${MISTER_WRAPPER_HPS_CONTAINER_ROOT:-/workspaces/3s-mister-arm}"
CONTAINER_BUILD_SRC_DIR="${CONTAINER_ROOT}/build/mister-wrapper-hps/src"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/build-hps.sh --check-env
  tools/mister-wrapper/build-hps.sh --prepare-source
  tools/mister-wrapper/build-hps.sh --build-image
  tools/mister-wrapper/build-hps.sh

Purpose:
  Build the Main_MiSTer-derived HPS wrapper binary MiSTer_3S-ARM from the pinned
  full upstream tree plus the local 3S-ARM overlay files.

Planned output:
  ${OUTPUT_DIR}/MiSTer_3S-ARM

Pinned source:
  ${UPSTREAM_URL} @ ${UPSTREAM_COMMIT}

Status:
  Full-source fetch + overlay build entrypoint with local-toolchain or Docker fallback.
EOF
}

have_command() {
    command -v "$1" >/dev/null 2>&1
}

have_local_toolchain() {
    have_command "${CC_BIN}" && have_command "${LD_BIN}" && have_command "${STRIP_BIN}"
}

require_base_host_tools() {
    have_command rsync || { echo "missing required command: rsync" >&2; return 1; }
    have_command git || { echo "missing required command: git" >&2; return 1; }
    [ -d "${SOURCE_DIR}" ] || { echo "missing pinned Main_MiSTer source: ${SOURCE_DIR}" >&2; return 1; }
    [ -f "${BUILD_MANIFEST}" ] || { echo "missing wrapper build manifest: ${BUILD_MANIFEST}" >&2; return 1; }
    [ -f "${OVERLAY_MANIFEST}" ] || { echo "missing Main_MiSTer overlay keep-list: ${OVERLAY_MANIFEST}" >&2; return 1; }
    [ -f "${MENU_PATCH}" ] || { echo "missing Main_MiSTer menu patch: ${MENU_PATCH}" >&2; return 1; }
}

docker_available() {
    have_command docker && [ -f "${DOCKERFILE_PATH}" ]
}

selected_build_mode() {
    if have_local_toolchain && have_command make; then
        echo "local"
        return 0
    fi

    if docker_available; then
        echo "docker"
        return 0
    fi

    echo "missing"
    return 1
}

prepare_source() {
    mkdir -p "${OUTPUT_DIR}"
    rm -rf "${BUILD_SRC_DIR}"

    git clone --filter=blob:none --no-checkout "${UPSTREAM_URL}" "${BUILD_SRC_DIR}"
    git -C "${BUILD_SRC_DIR}" checkout "${UPSTREAM_COMMIT}"
    rsync -a --files-from="${OVERLAY_MANIFEST}" "${SOURCE_DIR}/" "${BUILD_SRC_DIR}/"
    git -C "${BUILD_SRC_DIR}" apply --whitespace=nowarn "${MENU_PATCH}"
    cp "${BUILD_MANIFEST}" "${BUILD_SRC_DIR}/Makefile.3s-arm"
}

check_compiler_stack() {
    command -v "${CC_BIN}" >/dev/null 2>&1 || { echo "missing required compiler: ${CC_BIN}" >&2; return 1; }
    command -v "${LD_BIN}" >/dev/null 2>&1 || { echo "missing required linker: ${LD_BIN}" >&2; return 1; }
    command -v "${STRIP_BIN}" >/dev/null 2>&1 || { echo "missing required stripper: ${STRIP_BIN}" >&2; return 1; }
}

build_docker_image() {
    docker build \
        --platform "${DOCKER_PLATFORM}" \
        -t "${DOCKER_IMAGE}" \
        -f "${DOCKERFILE_PATH}" \
        "${DOCKER_CONTEXT_DIR}"
}

run_make_in_docker() {
    local toolchain_bin_expr='$(echo /usr/local/bin/gcc-arm-*/bin)'
    docker run --rm \
        --platform "${DOCKER_PLATFORM}" \
        -u "$(id -u):$(id -g)" \
        -v "${ROOT_DIR}:${CONTAINER_ROOT}" \
        -w "${CONTAINER_ROOT}" \
        "${DOCKER_IMAGE}" \
        bash -lc "TOOLCHAIN_BIN=${toolchain_bin_expr}; export PATH=\"\$PATH:\${TOOLCHAIN_BIN}\"; make -C \"${CONTAINER_BUILD_SRC_DIR}\" -f Makefile.3s-arm BASE=\"${TOOLCHAIN_PREFIX}\""
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

if [ "${1:-}" = "--check-env" ]; then
    require_base_host_tools || exit 1
    mkdir -p "${OUTPUT_DIR}"
    mode="$(selected_build_mode || true)"
    if [ "${mode}" = "local" ]; then
        check_compiler_stack || exit 1
        echo "build_mode=local"
    elif [ "${mode}" = "docker" ]; then
        echo "build_mode=docker"
        echo "docker_image=${DOCKER_IMAGE}"
        echo "docker_platform=${DOCKER_PLATFORM}"
    else
        echo "missing required toolchain: install ${TOOLCHAIN_PREFIX} on PATH or use Docker with ${DOCKERFILE_PATH}" >&2
        exit 1
    fi
    echo "HPS wrapper build scaffold is present."
    echo "source_dir=${SOURCE_DIR}"
    echo "overlay_manifest=${OVERLAY_MANIFEST}"
    echo "build_manifest=${BUILD_MANIFEST}"
    echo "menu_patch=${MENU_PATCH}"
    echo "upstream_url=${UPSTREAM_URL}"
    echo "upstream_commit=${UPSTREAM_COMMIT}"
    if [ -f "${UPSTREAM_FILE}" ]; then
        echo "upstream_metadata=${UPSTREAM_FILE}"
    fi
    echo "toolchain_prefix=${TOOLCHAIN_PREFIX}"
    echo "planned_output=${OUTPUT_DIR}/MiSTer_3S-ARM"
    exit 0
fi

if [ "${1:-}" = "--prepare-source" ]; then
    require_base_host_tools || exit 1
    prepare_source
    echo "prepared_source=${BUILD_SRC_DIR}"
    echo "prepared_manifest=${BUILD_SRC_DIR}/Makefile.3s-arm"
    exit 0
fi

if [ "${1:-}" = "--build-image" ]; then
    docker_available || { echo "docker build prerequisites missing: docker or ${DOCKERFILE_PATH}" >&2; exit 1; }
    build_docker_image
    echo "docker_image=${DOCKER_IMAGE}"
    exit 0
fi

require_base_host_tools || exit 1

prepare_source
mode="$(selected_build_mode || true)"
if [ "${mode}" = "local" ]; then
    command -v make >/dev/null 2>&1 || { echo "missing required command: make" >&2; exit 1; }
    check_compiler_stack || exit 1
    make -C "${BUILD_SRC_DIR}" -f Makefile.3s-arm BASE="${TOOLCHAIN_PREFIX}"
elif [ "${mode}" = "docker" ]; then
    build_docker_image
    run_make_in_docker
else
    echo "missing required toolchain: install ${TOOLCHAIN_PREFIX} on PATH or use Docker with ${DOCKERFILE_PATH}" >&2
    exit 1
fi

cp "${BUILD_SRC_DIR}/bin/MiSTer_3S-ARM" "${OUTPUT_DIR}/MiSTer_3S-ARM"
echo "build_mode=${mode}"
echo "built_output=${OUTPUT_DIR}/MiSTer_3S-ARM"
