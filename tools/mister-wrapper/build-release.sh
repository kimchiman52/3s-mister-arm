#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_RUNTIME_SCRIPT="${ROOT_DIR}/tools/mister/build-runtime-package.sh"
PACKAGE_WRAPPER_SCRIPT="${ROOT_DIR}/tools/mister-wrapper/package-wrapper.sh"
INSTALL_README_TEMPLATE="${ROOT_DIR}/tools/mister/release-readme.txt"
RUNTIME_INSTALL_PREFIX="${RUNTIME_INSTALL_PREFIX:-${ROOT_DIR}/build/mister-clean-install}"
RUNTIME_PACKAGE="${RUNTIME_PACKAGE:-${ROOT_DIR}/build/mister-runtime-package}"
HPS_BINARY="${HPS_BINARY:-${ROOT_DIR}/build/mister-wrapper-hps/MiSTer_3S-ARM}"
CORE_RBF="${CORE_RBF:-${ROOT_DIR}/build/mister-wrapper-core/3S-ARM.rbf}"
WORK_DIR="${WORK_DIR:-${ROOT_DIR}/build/mister-release}"
STAGE_DIR="${STAGE_DIR:-${WORK_DIR}/stage}"
RELEASE_DATE="${RELEASE_DATE:-$(date +%Y-%m-%d)}"
OUTPUT_ZIP="${OUTPUT_ZIP:-${WORK_DIR}/3s-mister-arm-${RELEASE_DATE}.zip}"
README_BASENAME="README.txt"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/build-release.sh --check
  tools/mister-wrapper/build-release.sh [options]

Options:
  --runtime-install-prefix <dir>  Clean MiSTer install prefix to package
  --runtime-package <dir>         Runtime package output directory
  --hps-binary <file>             MiSTer_3S-ARM binary to include
  --core-rbf <file>               3S-ARM.rbf to include
  --work-dir <dir>                Working directory for stage and zip
  --stage-dir <dir>               FAT-rooted staging directory
  --output-zip <file>             Final FAT-rooted release zip path
  --help                          Show this help

Defaults:
  runtime_install_prefix=${RUNTIME_INSTALL_PREFIX}
  runtime_package=${RUNTIME_PACKAGE}
  hps_binary=${HPS_BINARY}
  core_rbf=${CORE_RBF}
  work_dir=${WORK_DIR}
  stage_dir=${STAGE_DIR}
  output_zip=${OUTPUT_ZIP}
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "required command not found: $1" >&2
        exit 1
    }
}

require_inputs() {
    [ -x "${BUILD_RUNTIME_SCRIPT}" ] || { echo "runtime build script not found: ${BUILD_RUNTIME_SCRIPT}" >&2; return 1; }
    [ -x "${PACKAGE_WRAPPER_SCRIPT}" ] || { echo "wrapper package script not found: ${PACKAGE_WRAPPER_SCRIPT}" >&2; return 1; }
    [ -d "${RUNTIME_INSTALL_PREFIX}" ] || { echo "runtime install prefix not found: ${RUNTIME_INSTALL_PREFIX}" >&2; return 1; }
    [ -f "${HPS_BINARY}" ] || { echo "HPS wrapper binary not found: ${HPS_BINARY}" >&2; return 1; }
    [ -f "${CORE_RBF}" ] || { echo "wrapper core RBF not found: ${CORE_RBF}" >&2; return 1; }
    [ -f "${INSTALL_README_TEMPLATE}" ] || { echo "install README template not found: ${INSTALL_README_TEMPLATE}" >&2; return 1; }
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

    [ -f "${stage_root}/MiSTer_3S-ARM" ] || { echo "missing staged HPS binary: ${stage_root}/MiSTer_3S-ARM" >&2; return 1; }
    [ -f "${stage_root}/_Other/3S-ARM.rbf" ] || { echo "missing staged core: ${stage_root}/_Other/3S-ARM.rbf" >&2; return 1; }
    [ -f "${stage_root}/games/3s-arm/bin/3s-arm" ] || { echo "missing staged runtime binary: ${stage_root}/games/3s-arm/bin/3s-arm" >&2; return 1; }
    [ -d "${stage_root}/games/3s-arm/resources" ] || { echo "missing staged resources directory: ${stage_root}/games/3s-arm/resources" >&2; return 1; }
    [ -f "${stage_root}/${README_BASENAME}" ] || { echo "missing staged install README: ${stage_root}/${README_BASENAME}" >&2; return 1; }

    assert_absent "${stage_root}/games/3s-arm/resources/SF33RD.AFS"
    assert_absent "${stage_root}/games/3s-arm/config"
    assert_absent "${stage_root}/games/3s-arm/keymap"
    assert_absent "${stage_root}/games/3s-arm/logs"
}

build_runtime_package() {
    "${BUILD_RUNTIME_SCRIPT}" \
        --install-prefix "${RUNTIME_INSTALL_PREFIX}" \
        --output-dir "${RUNTIME_PACKAGE}"
}

stage_release() {
    rm -rf "${STAGE_DIR}"
    mkdir -p "${STAGE_DIR}"

    OUTPUT_DIR="${STAGE_DIR}" "${PACKAGE_WRAPPER_SCRIPT}" \
        --runtime-package "${RUNTIME_PACKAGE}" \
        --hps-binary "${HPS_BINARY}" \
        --core-rbf "${CORE_RBF}"

    mkdir -p "${STAGE_DIR}/games/3s-arm/resources"
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
        zip -rq "${OUTPUT_ZIP}" "MiSTer_3S-ARM" "_Other" "games" "${README_BASENAME}"
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
    --hps-binary)
        HPS_BINARY="$2"
        shift 2
        ;;
    --core-rbf)
        CORE_RBF="$2"
        shift 2
        ;;
    --work-dir)
        WORK_DIR="$2"
        STAGE_DIR="${WORK_DIR}/stage"
        OUTPUT_ZIP="${WORK_DIR}/3s-mister-arm-${RELEASE_DATE}.zip"
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
require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "runtime_install_prefix=${RUNTIME_INSTALL_PREFIX}"
    echo "runtime_package=${RUNTIME_PACKAGE}"
    echo "hps_binary=${HPS_BINARY}"
    echo "core_rbf=${CORE_RBF}"
    echo "stage_dir=${STAGE_DIR}"
    echo "output_zip=${OUTPUT_ZIP}"
    exit 0
fi

build_runtime_package
stage_release
build_zip

echo "release_stage=${STAGE_DIR}"
echo "release_zip=${OUTPUT_ZIP}"
