#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEST_DIR="${MISTER_QUARTUS_INSTALLER_DIR:-${ROOT_DIR}/build/quartus17-installer}"
EDITION="${MISTER_QUARTUS_EDITION:-lite}"
PRINT_URLS=0
FORCE=0

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/fetch-quartus17-installer.sh [--dest-dir <dir>] [--edition <lite|standard>] [--print-urls] [--force]

Purpose:
  Download the verified official Intel Quartus 17.0 Linux installer plus the
  Cyclone and Cyclone V device packs into one local directory for
  tools/mister-wrapper/build-quartus-image.sh.

Defaults:
  dest_dir=${DEST_DIR}
  edition=${EDITION}

Files:
  QuartusLiteSetup-17.0.0.595-linux.run (default) or QuartusSetup-17.0.0.595-linux.run
  cyclone-17.0.0.595.qdz
  cyclonev-17.0.0.595.qdz
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        return 1
    }
}

download_if_needed() {
    local url="$1"
    local dst_path="$2"

    if [ -f "${dst_path}" ] && [ "${FORCE}" -ne 1 ]; then
        echo "present=${dst_path}"
        return 0
    fi

    local tmp_path="${dst_path}.part"
    rm -f "${tmp_path}"
    curl --fail --location --progress-bar --output "${tmp_path}" "${url}"
    mv "${tmp_path}" "${dst_path}"
    echo "downloaded=${dst_path}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --dest-dir)
        DEST_DIR="$2"
        shift 2
        ;;
    --edition)
        EDITION="$2"
        shift 2
        ;;
    --print-urls)
        PRINT_URLS=1
        shift
        ;;
    --force)
        FORCE=1
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

require_cmd curl

case "${EDITION}" in
lite)
    QUARTUS_FILE="QuartusLiteSetup-17.0.0.595-linux.run"
    ;;
standard)
    QUARTUS_FILE="QuartusSetup-17.0.0.595-linux.run"
    ;;
*)
    echo "unsupported edition: ${EDITION} (expected lite or standard)" >&2
    exit 2
    ;;
esac

QUARTUS_URL="https://downloads.intel.com/akdlm/software/acdsinst/17.0std/595/ib_installers/${QUARTUS_FILE}"
CYCLONE_URL="https://downloads.intel.com/akdlm/software/acdsinst/17.0std/595/ib_installers/cyclone-17.0.0.595.qdz"
CYCLONEV_URL="https://downloads.intel.com/akdlm/software/acdsinst/17.0std/595/ib_installers/cyclonev-17.0.0.595.qdz"

if [ "${PRINT_URLS}" -eq 1 ]; then
    printf '%s=%s\n' "${QUARTUS_FILE}" "${QUARTUS_URL}"
    printf 'cyclone-17.0.0.595.qdz=%s\n' "${CYCLONE_URL}"
    printf 'cyclonev-17.0.0.595.qdz=%s\n' "${CYCLONEV_URL}"
    exit 0
fi

mkdir -p "${DEST_DIR}"

download_if_needed "${QUARTUS_URL}" "${DEST_DIR}/${QUARTUS_FILE}"
download_if_needed "${CYCLONE_URL}" "${DEST_DIR}/cyclone-17.0.0.595.qdz"
download_if_needed "${CYCLONEV_URL}" "${DEST_DIR}/cyclonev-17.0.0.595.qdz"

echo "installer_dir=${DEST_DIR}"
