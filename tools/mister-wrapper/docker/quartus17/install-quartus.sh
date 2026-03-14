#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: install-quartus.sh <quartus-install-dir>" >&2
    exit 2
fi

INSTALL_DIR="$1"
INSTALLER_DIR="/tmp/quartus-installer"

QUARTUS_RUN="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'QuartusLiteSetup-17.0*.run' | sort | head -n 1)"
if [ -z "${QUARTUS_RUN}" ]; then
    QUARTUS_RUN="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'QuartusSetup-17.0*.run' | sort | head -n 1)"
fi
CYCLONE_QDZ="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'cyclone-17.0*.qdz' | sort | head -n 1)"
CYCLONEV_QDZ="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'cyclonev-17.0*.qdz' | sort | head -n 1)"

[ -n "${QUARTUS_RUN}" ] || { echo "missing Quartus Lite or Standard setup .run in ${INSTALLER_DIR}" >&2; exit 1; }
[ -n "${CYCLONE_QDZ}" ] || { echo "missing Cyclone .qdz in ${INSTALLER_DIR}" >&2; exit 1; }
[ -n "${CYCLONEV_QDZ}" ] || { echo "missing Cyclone V .qdz in ${INSTALLER_DIR}" >&2; exit 1; }

chmod +x "${QUARTUS_RUN}"
mkdir -p "${INSTALL_DIR}"

cd "${INSTALLER_DIR}"
"${QUARTUS_RUN}" --mode unattended --unattendedmodeui minimal --installdir "${INSTALL_DIR}" --accept_eula 1
