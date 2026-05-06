#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_RUNTIME_SCRIPT="${ROOT_DIR}/tools/miyoo/build-runtime-package.sh"
INSTALL_README_TEMPLATE="${ROOT_DIR}/tools/miyoo/release-readme.txt"
RUNTIME_INSTALL_PREFIX="${RUNTIME_INSTALL_PREFIX:-${ROOT_DIR}/build/miyoo-install}"
RUNTIME_PACKAGE="${RUNTIME_PACKAGE:-${ROOT_DIR}/build/miyoo-runtime-package}"
WORK_DIR="${WORK_DIR:-${ROOT_DIR}/build/miyoo-release}"
STAGE_DIR="${STAGE_DIR:-${WORK_DIR}/stage}"
RELEASE_DATE="${RELEASE_DATE:-$(date +%Y-%m-%d)}"
OUTPUT_ZIP="${OUTPUT_ZIP:-${WORK_DIR}/3s-miyoo-arm-${RELEASE_DATE}.zip}"
README_BASENAME="README.txt"

usage() {
    cat <<EOF
Usage:
  tools/miyoo/build-release.sh --check
  tools/miyoo/build-release.sh [options]

Options:
  --runtime-install-prefix <dir>  Miyoo install prefix to package
  --runtime-package <dir>         Runtime package output directory
  --work-dir <dir>                Working directory for stage and zip
  --stage-dir <dir>               SD-rooted staging directory
  --output-zip <file>             Final SD-rooted release zip path
  --help                          Show this help

Defaults:
  runtime_install_prefix=${RUNTIME_INSTALL_PREFIX}
  runtime_package=${RUNTIME_PACKAGE}
  work_dir=${WORK_DIR}
  stage_dir=${STAGE_DIR}
  output_zip=${OUTPUT_ZIP}

The output zip mirrors the SD card root: extracting onto the root of a
Miyoo Mini Plus / OnionOS SD card lands files at the right paths
(Roms/PORTS/Games/3s-arm/, Roms/PORTS/Shortcuts/Action/3s-arm.port,
Roms/PORTS/Imgs/, plus README.txt at the root).
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "required command not found: $1" >&2
        exit 1
    }
}

require_inputs() {
    [ -x "${BUILD_RUNTIME_SCRIPT}" ]      || { echo "runtime build script not found: ${BUILD_RUNTIME_SCRIPT}" >&2; return 1; }
    [ -d "${RUNTIME_INSTALL_PREFIX}" ]    || { echo "runtime install prefix not found: ${RUNTIME_INSTALL_PREFIX}" >&2; return 1; }
    [ -f "${INSTALL_README_TEMPLATE}" ]   || { echo "install README template not found: ${INSTALL_README_TEMPLATE}" >&2; return 1; }
}

assert_absent() {
    local path="$1"

    if [ -e "${path}" ]; then
        echo "forbidden staged release content present: ${path}" >&2
        return 1
    fi
}

validate_release_stage() {
    local stage_root="$1"
    local app_dir="${stage_root}/Roms/PORTS/Games/3s-arm"
    local shortcut="${stage_root}/Roms/PORTS/Shortcuts/Action/3s-arm.port"

    [ -f "${app_dir}/3s-arm" ]               || { echo "missing staged binary: ${app_dir}/3s-arm" >&2; return 1; }
    [ -f "${app_dir}/launch.sh" ]            || { echo "missing staged launcher: ${app_dir}/launch.sh" >&2; return 1; }
    [ -f "${app_dir}/keymap" ]               || { echo "missing staged keymap: ${app_dir}/keymap" >&2; return 1; }
    [ -d "${app_dir}/lib" ]                  || { echo "missing staged lib dir: ${app_dir}/lib" >&2; return 1; }
    [ -d "${app_dir}/resources" ]            || { echo "missing staged resources dir: ${app_dir}/resources" >&2; return 1; }
    [ -f "${shortcut}" ]                     || { echo "missing staged OnionOS shortcut: ${shortcut}" >&2; return 1; }
    [ -f "${stage_root}/${README_BASENAME}" ] || { echo "missing staged install README: ${stage_root}/${README_BASENAME}" >&2; return 1; }

    assert_absent "${app_dir}/resources/SF33RD.AFS"
    assert_absent "${app_dir}/config"
    assert_absent "${app_dir}/launch.log"
    assert_absent "${app_dir}/boot.log"
    assert_absent "${app_dir}/logs"
    assert_absent "${app_dir}/.local"

    if find "${app_dir}/lib" -maxdepth 1 -name 'libmi_*.so*' 2>/dev/null | grep -q .; then
        echo "forbidden libmi_*.so present in staged package lib/" >&2
        return 1
    fi
}

build_runtime_package() {
    "${BUILD_RUNTIME_SCRIPT}" \
        --install-prefix "${RUNTIME_INSTALL_PREFIX}" \
        --output-dir "${RUNTIME_PACKAGE}"
}

stage_release() {
    rm -rf "${STAGE_DIR}"
    mkdir -p "${STAGE_DIR}"

    # Copy the runtime package tree (already SD-rooted: Roms/PORTS/...)
    # into the stage. -a preserves perms; --copy-links was already
    # applied inside package.sh so the .so files in lib/ are real
    # files (exFAT cannot follow symlinks).
    rsync -a "${RUNTIME_PACKAGE}/" "${STAGE_DIR}/"

    # Drop the install README at the SD root so users see it after
    # extracting the zip.
    cp "${INSTALL_README_TEMPLATE}" "${STAGE_DIR}/${README_BASENAME}"

    validate_release_stage "${STAGE_DIR}"
}

build_zip() {
    local zip_parent

    zip_parent="$(dirname "${OUTPUT_ZIP}")"
    mkdir -p "${zip_parent}"
    rm -f "${OUTPUT_ZIP}"

    (
        cd "${STAGE_DIR}"
        zip -rq "${OUTPUT_ZIP}" "Roms" "${README_BASENAME}"
    )
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

check_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
    --check)
        check_only=1
        shift
        ;;
    --runtime-install-prefix)
        RUNTIME_INSTALL_PREFIX="$2"
        shift 2
        ;;
    --runtime-package)
        RUNTIME_PACKAGE="$2"
        shift 2
        ;;
    --work-dir)
        WORK_DIR="$2"
        STAGE_DIR="${WORK_DIR}/stage"
        OUTPUT_ZIP="${WORK_DIR}/3s-miyoo-arm-${RELEASE_DATE}.zip"
        shift 2
        ;;
    --stage-dir)
        STAGE_DIR="$2"
        shift 2
        ;;
    --output-zip)
        OUTPUT_ZIP="$2"
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

require_cmd zip
require_cmd rsync
require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "runtime_install_prefix=${RUNTIME_INSTALL_PREFIX}"
    echo "runtime_package=${RUNTIME_PACKAGE}"
    echo "stage_dir=${STAGE_DIR}"
    echo "output_zip=${OUTPUT_ZIP}"
    exit 0
fi

build_runtime_package
stage_release
build_zip

echo "release_stage=${STAGE_DIR}"
echo "release_zip=${OUTPUT_ZIP}"
