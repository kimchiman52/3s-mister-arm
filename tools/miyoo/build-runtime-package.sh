#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PACKAGE_SCRIPT="${ROOT_DIR}/tools/miyoo/package.sh"
INSTALL_PREFIX="${INSTALL_PREFIX:-${ROOT_DIR}/build/miyoo-install}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/build/miyoo-runtime-package}"

usage() {
    cat <<EOF
Usage:
  tools/miyoo/build-runtime-package.sh --check
  tools/miyoo/build-runtime-package.sh [--install-prefix <dir>] [--output-dir <dir>]

Purpose:
  Build the player-facing Miyoo Mini Plus / OnionOS runtime tree from an
  existing Miyoo install prefix. Output mirrors the SD card root so a
  single rsync drops everything into place.

Defaults:
  install_prefix=${INSTALL_PREFIX}
  output_dir=${OUTPUT_DIR}
EOF
}

require_inputs() {
    [ -x "${PACKAGE_SCRIPT}" ] || { echo "runtime package script not found: ${PACKAGE_SCRIPT}" >&2; return 1; }
    [ -d "${INSTALL_PREFIX}" ] || { echo "install prefix not found: ${INSTALL_PREFIX}" >&2; return 1; }
}

assert_absent() {
    local path="$1"

    if [ -e "${path}" ]; then
        echo "forbidden staged runtime content present: ${path}" >&2
        return 1
    fi
}

validate_runtime_package() {
    local pkg_root="$1"
    local app_dir="${pkg_root}/Roms/PORTS/Games/3s-arm"
    local shortcut="${pkg_root}/Roms/PORTS/Shortcuts/Action/3s-arm.port"

    [ -f "${app_dir}/3s-arm" ]                || { echo "runtime binary missing: ${app_dir}/3s-arm" >&2; return 1; }
    [ -f "${app_dir}/launch.sh" ]             || { echo "launch.sh missing: ${app_dir}/launch.sh" >&2; return 1; }
    [ -f "${app_dir}/keymap" ]                || { echo "default keymap missing: ${app_dir}/keymap" >&2; return 1; }
    [ -f "${app_dir}/lib/libSDL3.so.0" ]      || { echo "bundled SDL3 missing: ${app_dir}/lib/libSDL3.so.0" >&2; return 1; }
    [ -f "${shortcut}" ]                      || { echo "OnionOS shortcut missing: ${shortcut}" >&2; return 1; }
    [ -f "${app_dir}/resources/PUT_SF33RD_AFS_HERE.txt" ] || \
        { echo "asset placeholder missing: ${app_dir}/resources/PUT_SF33RD_AFS_HERE.txt" >&2; return 1; }

    # Per-user / runtime state must NOT ship.
    assert_absent "${app_dir}/resources/SF33RD.AFS"
    assert_absent "${app_dir}/config"
    assert_absent "${app_dir}/launch.log"
    assert_absent "${app_dir}/boot.log"
    assert_absent "${app_dir}/logs"
    assert_absent "${app_dir}/.local"

    # Vendored SigmaStar libs must resolve from /config/lib at runtime,
    # never bundled into the package's lib/ — they would shadow the
    # firmware BSP's libmi_*.so. The package.sh rsync explicitly excludes
    # libmi_*.so; double-check here.
    if find "${app_dir}/lib" -maxdepth 1 -name 'libmi_*.so*' 2>/dev/null | grep -q .; then
        echo "forbidden libmi_*.so present in package lib/" >&2
        return 1
    fi
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
    --install-prefix)
        INSTALL_PREFIX="$2"
        shift 2
        ;;
    --output-dir)
        OUTPUT_DIR="$2"
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

require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "install_prefix=${INSTALL_PREFIX}"
    echo "planned_output=${OUTPUT_DIR}"
    exit 0
fi

"${PACKAGE_SCRIPT}" "${INSTALL_PREFIX}" "${OUTPUT_DIR}"
validate_runtime_package "${OUTPUT_DIR}"

echo "runtime_package=${OUTPUT_DIR}"
