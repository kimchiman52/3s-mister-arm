#!/usr/bin/env bash
set -euo pipefail

# Sets up the 'quartus2' Colima VM used for:
#   - Quartus 17.0 FPGA builds (3S-ARM.rbf)
#   - The 3s-mister-arm-build Docker container (ARM/HPS builds)
#
# Safe to re-run on an existing running VM — skips steps already done.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
INSTALLER_DIR="${ROOT_DIR}/build/quartus17-installer"
QUARTUS_INSTALL_DIR="/home/sb.linux/intelFPGA_lite/17.0"

PROFILE="quartus2"
DISK_GB=20
CPUS=4
MEMORY_GB=8
SKIP_QUARTUS=0
SKIP_CONTAINER=0
BACKUP_QUARTUS=""
RESTORE_QUARTUS=""

usage() {
    cat <<EOF
Usage:
  tools/mister/setup-colima-vm.sh [options]

Purpose:
  Create or verify the '${PROFILE}' Colima/QEMU VM and install build tooling
  inside it. Safe to re-run — skips steps already in place.

Options:
  --profile <name>          Colima profile name (default: ${PROFILE})
  --disk <GiB>              Data disk GiB when creating new VM (default: ${DISK_GB})
  --skip-quartus            Skip Quartus 17.0 install step
  --skip-container          Skip Docker build container setup step
  --backup-quartus <path>   Export Quartus install from VM to a .tar.gz file, then exit
  --restore-quartus <path>  Restore Quartus install into VM from a .tar.gz file
  --help                    Show this message

Steps performed (default):
  1. Start the VM (creates it with --disk ${DISK_GB} if absent).
  2. Install Quartus 17.0 Lite into the VM at ${QUARTUS_INSTALL_DIR}.
  3. Set up the 3s-mister-arm-build Docker container (ARM/HPS builds).

Recreating the VM (shrinking disk):
  # 1. Backup Quartus from old VM to external drive
  tools/mister/setup-colima-vm.sh --backup-quartus /Volumes/KimchDrive/quartus17-backup.tar.gz
  # 2. Delete old VM
  colima stop -p quartus2 && colima delete -p quartus2 --force
  # 3. Remove orphaned data disk if needed:
  #    limactl disk list  — if colima-quartus2 is not listed, remove manually:
  #    rm -rf ~/.colima/_lima/_disks/colima-quartus2
  # 4. Create new VM and restore
  tools/mister/setup-colima-vm.sh --restore-quartus /Volumes/KimchDrive/quartus17-backup.tar.gz

Quartus installer prerequisite (fresh install):
  Installer files must be present at build/quartus17-installer/ before running
  this script without --restore-quartus. Fetch them with:
    tools/mister-wrapper/fetch-quartus17-installer.sh

Notes:
  - Disk size can only be set at VM creation time.
  - --backup-quartus verifies the archive with gzip -t before exiting.
  - The VM mounts your Mac home directory at /Users/\$USER via sshfs, but
    installer files are copied to local VM storage before running for speed.
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || { echo "missing required command: $1" >&2; exit 2; }
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --profile)
        PROFILE="$2"; shift 2 ;;
    --disk)
        DISK_GB="$2"; shift 2 ;;
    --skip-quartus)
        SKIP_QUARTUS=1; shift ;;
    --skip-container)
        SKIP_CONTAINER=1; shift ;;
    --backup-quartus)
        BACKUP_QUARTUS="$2"; shift 2 ;;
    --restore-quartus)
        RESTORE_QUARTUS="$2"; shift 2 ;;
    --help|-h)
        usage; exit 0 ;;
    *)
        echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

require_cmd colima
require_cmd docker

# VM-level packages needed by build-core.sh (rsync, ruby) — installed in the
# VM OS itself, not in the Docker container.
install_vm_packages() {
    colima ssh --profile "${PROFILE}" -- sudo apt-get update -qq
    colima ssh --profile "${PROFILE}" -- sudo apt-get install -y rsync ruby
}

# ---------------------------------------------------------------------------
# --backup-quartus: export Quartus from a running VM, verify, then exit
# ---------------------------------------------------------------------------

if [ -n "${BACKUP_QUARTUS}" ]; then
    echo "==> Backing up Quartus install from VM '${PROFILE}' to ${BACKUP_QUARTUS}"
    tmp_backup="${BACKUP_QUARTUS}.part"
    colima ssh --profile "${PROFILE}" -- tar czf - -C /home/sb.linux intelFPGA_lite > "${tmp_backup}"
    echo "    Verifying archive integrity..."
    gzip -t "${tmp_backup}"
    mv "${tmp_backup}" "${BACKUP_QUARTUS}"
    echo "    backup_path=${BACKUP_QUARTUS}"
    echo "    backup_size=$(du -sh "${BACKUP_QUARTUS}" | cut -f1)"
    echo "==> Backup complete. Safe to delete the VM and restore with --restore-quartus."
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 1: Start VM (create if absent)
# ---------------------------------------------------------------------------

echo "==> Checking Colima VM profile '${PROFILE}'"

vm_status="$(colima list --profile "${PROFILE}" --json 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('status',''))" 2>/dev/null || echo "")"

if [ -z "${vm_status}" ]; then
    echo "    VM not found — creating with disk=${DISK_GB}GiB cpu=${CPUS} memory=${MEMORY_GB}GiB"
    colima start \
        --profile "${PROFILE}" \
        --arch x86_64 \
        --vm-type qemu \
        --cpu "${CPUS}" \
        --memory "${MEMORY_GB}" \
        --disk "${DISK_GB}" \
        --runtime docker \
        --mount-type sshfs \
        --mount-inotify
elif [ "${vm_status}" = "Stopped" ]; then
    echo "    VM exists but stopped — starting"
    colima start --profile "${PROFILE}"
else
    echo "    VM is ${vm_status}"
fi

echo "    vm_profile=${PROFILE}"
colima list --profile "${PROFILE}" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Step 1b: Install VM-level packages (rsync, ruby — needed by build-core.sh)
# ---------------------------------------------------------------------------

echo "==> Installing VM-level packages (rsync, ruby)"
if colima ssh --profile "${PROFILE}" -- bash -lc 'command -v rsync && command -v ruby' >/dev/null 2>&1; then
    echo "    already installed"
else
    install_vm_packages
fi

# ---------------------------------------------------------------------------
# Step 2: Install Quartus 17.0 in the VM
# ---------------------------------------------------------------------------

if [ "${SKIP_QUARTUS}" -eq 1 ]; then
    echo "==> Skipping Quartus install (--skip-quartus)"
else
    echo "==> Checking Quartus install in VM"

    quartus_ok=0
    if colima ssh --profile "${PROFILE}" -- bash -lc "test -x '${QUARTUS_INSTALL_DIR}/quartus/bin/quartus_sh'" 2>/dev/null; then
        quartus_ver="$(colima ssh --profile "${PROFILE}" -- bash -lc "
            '${QUARTUS_INSTALL_DIR}/quartus/bin/quartus_sh' --version 2>&1 | head -n 2
        " 2>/dev/null || echo "")"
        if [ -n "${quartus_ver}" ]; then
            echo "    Quartus already installed:"
            echo "    ${quartus_ver}"
            quartus_ok=1
        fi
    fi

    if [ "${quartus_ok}" -eq 0 ]; then
        if [ -n "${RESTORE_QUARTUS}" ]; then
            # Restore from backup tarball
            echo "    Restoring Quartus from ${RESTORE_QUARTUS}..."
            [ -f "${RESTORE_QUARTUS}" ] || { echo "ERROR: backup file not found: ${RESTORE_QUARTUS}" >&2; exit 1; }
            echo "    Verifying archive integrity..."
            gzip -t "${RESTORE_QUARTUS}" || { echo "ERROR: backup archive is corrupt: ${RESTORE_QUARTUS}" >&2; exit 1; }
            colima ssh --profile "${PROFILE}" -- mkdir -p /home/sb.linux
            cat "${RESTORE_QUARTUS}" | colima ssh --profile "${PROFILE}" -- tar xzf - -C /home/sb.linux
        else
            # Fresh install from downloaded installers
            echo "    Quartus not found — installing from ${INSTALLER_DIR}"

            quartus_run="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'QuartusLiteSetup-17.0*.run' | sort | head -n 1)"
            cyclone_qdz="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'cyclone-17.0*.qdz' | sort | head -n 1)"
            cyclonev_qdz="$(find "${INSTALLER_DIR}" -maxdepth 1 -type f -name 'cyclonev-17.0*.qdz' | sort | head -n 1)"

            if [ -z "${quartus_run}" ] || [ -z "${cyclone_qdz}" ] || [ -z "${cyclonev_qdz}" ]; then
                echo "" >&2
                echo "ERROR: Installer files missing from ${INSTALLER_DIR}" >&2
                echo "Run this first:" >&2
                echo "  tools/mister-wrapper/fetch-quartus17-installer.sh" >&2
                echo "" >&2
                exit 1
            fi

            echo "    Copying installer files to data disk staging area..."
            # Stage under /home/sb.linux (user-writable, on root disk), NOT /tmp.
            # /tmp is tmpfs (RAM-backed). The Quartus installer + device packs are
            # ~3.5 GB and can exhaust tmpfs, producing a silent partial install.
            # TMPDIR must also point to real disk so the installer's own extraction
            # doesn't fill RAM and fail mid-way.
            colima ssh --profile "${PROFILE}" -- bash -lc "
                set -euo pipefail
                STAGE=/home/sb.linux/quartus-installer-stage
                mkdir -p \"\${STAGE}\"
                cp '${quartus_run}' \"\${STAGE}/\"
                cp '${cyclone_qdz}' \"\${STAGE}/\"
                cp '${cyclonev_qdz}' \"\${STAGE}/\"
            "

            echo "    Running Quartus installer (10-20 min)..."
            colima ssh --profile "${PROFILE}" -- bash -lc "
                set -euo pipefail
                STAGE=/home/sb.linux/quartus-installer-stage
                export TMPDIR=\"\${STAGE}\"
                QUARTUS_RUN=\$(find \"\${STAGE}\" -maxdepth 1 -name 'QuartusLiteSetup-17.0*.run' | head -n 1)
                chmod +x \"\${QUARTUS_RUN}\"
                mkdir -p '${QUARTUS_INSTALL_DIR}'
                cd \"\${STAGE}\"
                \"\${QUARTUS_RUN}\" \
                    --mode unattended \
                    --unattendedmodeui minimal \
                    --installdir '${QUARTUS_INSTALL_DIR}' \
                    --accept_eula 1
                rm -rf \"\${STAGE}\"
            "
        fi

        echo "    Verifying Quartus install..."
        colima ssh --profile "${PROFILE}" -- bash -lc "
            '${QUARTUS_INSTALL_DIR}/quartus/bin/quartus_sh' --version 2>&1 | head -n 2
        "
        echo "    quartus_install_dir=${QUARTUS_INSTALL_DIR}"
    fi
fi

# ---------------------------------------------------------------------------
# Step 3: Set up the Docker build container
# ---------------------------------------------------------------------------

if [ "${SKIP_CONTAINER}" -eq 1 ]; then
    echo "==> Skipping build container setup (--skip-container)"
else
    echo "==> Setting up Docker build container"

    # Point docker at the colima VM
    export DOCKER_HOST="unix://${HOME}/.colima/${PROFILE}/docker.sock"

    "${SCRIPT_DIR}/setup-build-container.sh"
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------

echo ""
echo "==> Build environment ready"
echo ""
echo "Quartus (FPGA builds):"
echo "  colima ssh --profile ${PROFILE} -- bash -lc \\"
echo "    'PATH=${QUARTUS_INSTALL_DIR}/quartus/bin:\$PATH LC_ALL=C LANG=C \\"
echo "     bash /Users/\$USER/Developer/3s-mister-arm/tools/mister-wrapper/build-core.sh --fast --seed menu'"
echo ""
echo "ARM/HPS build container:"
echo "  DOCKER_HOST=unix://\$HOME/.colima/${PROFILE}/docker.sock \\"
echo "    tools/mister/build-game.sh --flavor telemetry"
