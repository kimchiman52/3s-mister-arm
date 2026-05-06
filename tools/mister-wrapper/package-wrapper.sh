#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/build/mister-wrapper-package}"
RUNTIME_PACKAGE="${RUNTIME_PACKAGE:-${ROOT_DIR}/build/mister-clean-package}"
HPS_BINARY="${HPS_BINARY:-${ROOT_DIR}/build/mister-wrapper-hps/MiSTer_3S-ARM}"
# CORE_RBF defaults to the latest dated bitstream in build/mister-wrapper-core/.
# build-core.sh writes 3S-ARM_YYYYMMDD.rbf; "ls -t | head" picks the newest.
default_core_rbf=""
if [ -d "${ROOT_DIR}/build/mister-wrapper-core" ]; then
    default_core_rbf="$(ls -1t "${ROOT_DIR}"/build/mister-wrapper-core/3S-ARM_*.rbf 2>/dev/null | head -1)"
fi
CORE_RBF="${CORE_RBF:-${default_core_rbf}}"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/package-wrapper.sh --check
  tools/mister-wrapper/package-wrapper.sh [--runtime-package <dir>] [--hps-binary <file>] [--core-rbf <file>]
  tools/mister-wrapper/package-wrapper.sh

Purpose:
  Assemble the full /media/fat-rooted wrapper package:
  - /media/fat/_Other/3S-ARM_YYYYMMDD.rbf
  - /media/fat/MiSTer_3S-ARM
  - /media/fat/games/3s-arm/...

Defaults:
  runtime_package=${RUNTIME_PACKAGE}
  hps_binary=${HPS_BINARY}
  core_rbf=${CORE_RBF}
  output_dir=${OUTPUT_DIR}
EOF
}

require_inputs() {
    [ -d "${RUNTIME_PACKAGE}" ] || { echo "runtime package not found: ${RUNTIME_PACKAGE}" >&2; return 1; }
    [ -f "${HPS_BINARY}" ] || { echo "HPS wrapper binary not found: ${HPS_BINARY}" >&2; return 1; }
    [ -f "${CORE_RBF}" ] || { echo "wrapper core RBF not found: ${CORE_RBF}" >&2; return 1; }
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

mkdir -p "${OUTPUT_DIR}"
require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "runtime_package=${RUNTIME_PACKAGE}"
    echo "hps_binary=${HPS_BINARY}"
    echo "core_rbf=${CORE_RBF}"
    echo "planned_output=${OUTPUT_DIR}"
    exit 0
fi

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}/_Other" "${OUTPUT_DIR}/games/3s-arm"

cp "${HPS_BINARY}" "${OUTPUT_DIR}/MiSTer_3S-ARM"
core_basename="$(basename "${CORE_RBF}")"
case "${core_basename}" in
    3S-ARM_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9].rbf)
        core_dst="${OUTPUT_DIR}/_Other/${core_basename}"
        ;;
    *)
        # Fallback: source is un-dated (legacy build); stamp from mtime.
        core_date="$(date -r "${CORE_RBF}" +%Y%m%d)"
        core_dst="${OUTPUT_DIR}/_Other/3S-ARM_${core_date}.rbf"
        ;;
esac
cp "${CORE_RBF}" "${core_dst}"
rsync -a "${RUNTIME_PACKAGE%/}/" "${OUTPUT_DIR}/games/3s-arm/"

chmod +x "${OUTPUT_DIR}/MiSTer_3S-ARM"

echo "packaged_root=${OUTPUT_DIR}"
echo "packaged_hps=${OUTPUT_DIR}/MiSTer_3S-ARM"
echo "packaged_core=${core_dst}"
