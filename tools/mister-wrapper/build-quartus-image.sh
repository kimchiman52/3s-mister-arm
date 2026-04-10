#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKER_TEMPLATE_DIR="${SCRIPT_DIR}/docker/quartus17"
DOCKER_IMAGE="${MISTER_WRAPPER_CORE_IMAGE:-3s-mister-arm-wrapper-quartus17}"
DOCKER_PLATFORM="${MISTER_WRAPPER_CORE_DOCKER_PLATFORM:-linux/amd64}"
INSTALLER_DIR="${MISTER_QUARTUS_INSTALLER_DIR:-}"
BASE_IMAGE="${MISTER_QUARTUS_BASE_IMAGE:-ubuntu:20.04}"
QUARTUS_INSTALL_DIR="${MISTER_QUARTUS_INSTALL_DIR:-}"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/build-quartus-image.sh --installer-dir <dir>
  tools/mister-wrapper/build-quartus-image.sh

Purpose:
  Build the dedicated x86_64 Quartus Docker image used by
  tools/mister-wrapper/build-core.sh.

Expected installer dir contents:
  - QuartusLiteSetup-17.0*.run or QuartusSetup-17.0*.run
  - cyclone-17.0*.qdz
  - cyclonev-17.0*.qdz

Defaults:
  docker_image=${DOCKER_IMAGE}
  docker_platform=${DOCKER_PLATFORM}
  base_image=${BASE_IMAGE}
  quartus_install_dir=derived from installer edition unless MISTER_QUARTUS_INSTALL_DIR is set
EOF
}

find_quartus_run() {
    local lite_run
    local standard_run

    lite_run="$(find "$1" -maxdepth 1 -type f -name 'QuartusLiteSetup-17.0*.run' | sort | head -n 1)"
    if [ -n "${lite_run}" ]; then
        printf '%s\n' "${lite_run}"
        return 0
    fi

    standard_run="$(find "$1" -maxdepth 1 -type f -name 'QuartusSetup-17.0*.run' | sort | head -n 1)"
    if [ -n "${standard_run}" ]; then
        printf '%s\n' "${standard_run}"
    fi
}

find_cyclone_qdz() {
    find "$1" -maxdepth 1 -type f -name 'cyclone-17.0*.qdz' | sort | head -n 1
}

find_cyclonev_qdz() {
    find "$1" -maxdepth 1 -type f -name 'cyclonev-17.0*.qdz' | sort | head -n 1
}

link_or_copy() {
    local src="$1"
    local dst="$2"
    ln "${src}" "${dst}" 2>/dev/null || cp "${src}" "${dst}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --installer-dir)
        INSTALLER_DIR="$2"
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

if [ -z "${INSTALLER_DIR}" ]; then
    echo "missing required installer dir: set --installer-dir or MISTER_QUARTUS_INSTALLER_DIR" >&2
    exit 1
fi

[ -d "${INSTALLER_DIR}" ] || { echo "installer dir not found: ${INSTALLER_DIR}" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "missing required command: docker" >&2; exit 1; }

QUARTUS_RUN="$(find_quartus_run "${INSTALLER_DIR}")"
CYC_QDZ="$(find_cyclone_qdz "${INSTALLER_DIR}")"
CYCV_QDZ="$(find_cyclonev_qdz "${INSTALLER_DIR}")"

[ -n "${QUARTUS_RUN}" ] || { echo "missing Quartus 17.0 Lite or Standard setup .run in ${INSTALLER_DIR}" >&2; exit 1; }
[ -n "${CYC_QDZ}" ] || { echo "missing Cyclone device support .qdz in ${INSTALLER_DIR}" >&2; exit 1; }
[ -n "${CYCV_QDZ}" ] || { echo "missing Cyclone V device support .qdz in ${INSTALLER_DIR}" >&2; exit 1; }

if [ -z "${QUARTUS_INSTALL_DIR}" ]; then
    case "$(basename "${QUARTUS_RUN}")" in
    QuartusLiteSetup-*)
        QUARTUS_INSTALL_DIR="/opt/intelFPGA_lite/17.0"
        ;;
    *)
        QUARTUS_INSTALL_DIR="/opt/intelFPGA_std/17.0"
        ;;
    esac
fi

BUILD_CONTEXT="$(mktemp -d "${TMPDIR:-/tmp}/3s-arm-quartus-image.XXXXXX")"
trap 'rm -rf "${BUILD_CONTEXT}"' EXIT

mkdir -p "${BUILD_CONTEXT}/installer"
cp "${DOCKER_TEMPLATE_DIR}/Dockerfile" "${BUILD_CONTEXT}/Dockerfile"
cp "${DOCKER_TEMPLATE_DIR}/install-quartus.sh" "${BUILD_CONTEXT}/install-quartus.sh"
link_or_copy "${QUARTUS_RUN}" "${BUILD_CONTEXT}/installer/$(basename "${QUARTUS_RUN}")"
link_or_copy "${CYC_QDZ}" "${BUILD_CONTEXT}/installer/$(basename "${CYC_QDZ}")"
link_or_copy "${CYCV_QDZ}" "${BUILD_CONTEXT}/installer/$(basename "${CYCV_QDZ}")"

docker build \
    --platform "${DOCKER_PLATFORM}" \
    --build-arg BASE_IMAGE="${BASE_IMAGE}" \
    --build-arg QUARTUS_INSTALL_DIR="${QUARTUS_INSTALL_DIR}" \
    -t "${DOCKER_IMAGE}" \
    "${BUILD_CONTEXT}"

echo "docker_image=${DOCKER_IMAGE}"
echo "quartus_installer=$(basename "${QUARTUS_RUN}")"
echo "quartus_install_dir=${QUARTUS_INSTALL_DIR}"
