#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PACKAGE_SCRIPT="${ROOT_DIR}/tools/mister/package.sh"
INSTALL_PREFIX="${INSTALL_PREFIX:-${ROOT_DIR}/build/mister-clean-install}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/build/mister-runtime-package}"

usage() {
    cat <<EOF
Usage:
  tools/mister/build-runtime-package.sh --check
  tools/mister/build-runtime-package.sh [--install-prefix <dir>] [--output-dir <dir>]

Purpose:
  Build the player-facing MiSTer runtime package for /media/fat/games/3sx/
  from an existing clean MiSTer install prefix.

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
    local runtime_root="$1"

    [ -f "${runtime_root}/bin/3sx" ] || { echo "runtime binary missing from package: ${runtime_root}/bin/3sx" >&2; return 1; }

    assert_absent "${runtime_root}/resources/SF33RD.AFS"
    assert_absent "${runtime_root}/config"
    assert_absent "${runtime_root}/keymap"
    assert_absent "${runtime_root}/logs"
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
