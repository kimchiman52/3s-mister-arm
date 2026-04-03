#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT_DIR}/build/mister-wrapper-core}"
BUILD_SRC_DIR="${OUTPUT_DIR}/src"
PROJECT_NAME="${MISTER_WRAPPER_CORE_NAME:-3SX}"
CORE_SEED="${MISTER_WRAPPER_CORE_SEED:-menu}"
DOCKER_IMAGE="${MISTER_WRAPPER_CORE_IMAGE:-3sx-mister-wrapper-quartus17}"
DOCKER_PLATFORM="${MISTER_WRAPPER_CORE_DOCKER_PLATFORM:-linux/amd64}"
DOCKER_BUILD_SCRIPT="${ROOT_DIR}/tools/mister-wrapper/build-quartus-image.sh"
CONTAINER_ROOT="${MISTER_WRAPPER_CORE_CONTAINER_ROOT:-/workspaces/3sx-mister}"
CONTAINER_BUILD_SRC_DIR="${CONTAINER_ROOT}/build/mister-wrapper-core/src"
MISTER_QUARTUS_INSTALLER_DIR="${MISTER_QUARTUS_INSTALLER_DIR:-}"
QUARTUS_LICENSE_SPEC="${MISTER_QUARTUS_LICENSE_FILE:-${LM_LICENSE_FILE:-}}"
SOURCE_DIR=""
UPSTREAM_FILE=""
TEMPLATE_BASENAME=""
CONF_STR_TOKEN=""

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/build-core.sh [--seed menu] --check-env
  tools/mister-wrapper/build-core.sh [--seed menu] --prepare-source
  tools/mister-wrapper/build-core.sh [--seed menu] --build-image
  tools/mister-wrapper/build-core.sh [--seed menu]

Purpose:
  Build the minimal FPGA wrapper core that produces ${PROJECT_NAME}.rbf from the
  pinned wrapper-core template seed.

Planned output:
  ${OUTPUT_DIR}/${PROJECT_NAME}.rbf

Default seed:
  ${CORE_SEED}
EOF
}

configure_seed() {
    case "${CORE_SEED}" in
        menu)
            SOURCE_DIR="${ROOT_DIR}/vendor/Menu_MiSTer"
            UPSTREAM_FILE="${ROOT_DIR}/vendor/Menu_MiSTer.UPSTREAM.md"
            TEMPLATE_BASENAME="menu"
            CONF_STR_TOKEN="MENU;UART31250,MIDI;"
            ;;
        *)
            echo "unsupported core seed: ${CORE_SEED}" >&2
            return 1
            ;;
    esac
}

have_command() {
    command -v "$1" >/dev/null 2>&1
}

require_base_tools() {
    have_command rsync || { echo "missing required command: rsync" >&2; return 1; }
    have_command ruby || { echo "missing required command: ruby" >&2; return 1; }
    configure_seed || return 1
    [ -d "${SOURCE_DIR}" ] || { echo "missing pinned wrapper-core source (${CORE_SEED}): ${SOURCE_DIR}" >&2; return 1; }
}

quartus_available_locally() {
    have_command quartus_sh && have_command quartus_cpf
}

docker_available() {
    have_command docker && [ -f "${DOCKER_BUILD_SCRIPT}" ]
}

docker_image_exists() {
    docker image inspect "${DOCKER_IMAGE}" >/dev/null 2>&1
}

quartus_edition() {
    quartus_sh --version 2>/dev/null | awk '
        /Standard Edition/ { print "standard"; found=1; exit }
        /Lite Edition/ { print "lite"; found=1; exit }
        END { if (!found) print "unknown" }
    '
}

docker_build_possible() {
    [ -n "${MISTER_QUARTUS_INSTALLER_DIR}" ]
}

selected_build_mode() {
    if quartus_available_locally; then
        echo "local"
        return 0
    fi

    if docker_available && (docker_image_exists || docker_build_possible); then
        echo "docker"
        return 0
    fi

    echo "missing"
    return 1
}

prepare_source() {
    mkdir -p "${OUTPUT_DIR}"
    rm -rf "${BUILD_SRC_DIR}"
    rsync -a --delete --exclude='.git' "${SOURCE_DIR}/" "${BUILD_SRC_DIR}/"

    mv "${BUILD_SRC_DIR}/${TEMPLATE_BASENAME}.qpf" "${BUILD_SRC_DIR}/${PROJECT_NAME}.qpf"
    mv "${BUILD_SRC_DIR}/${TEMPLATE_BASENAME}.qsf" "${BUILD_SRC_DIR}/${PROJECT_NAME}.qsf"
    mv "${BUILD_SRC_DIR}/${TEMPLATE_BASENAME}.sv" "${BUILD_SRC_DIR}/${PROJECT_NAME}.sv"

    if [ -f "${BUILD_SRC_DIR}/${TEMPLATE_BASENAME}.sdc" ]; then
        mv "${BUILD_SRC_DIR}/${TEMPLATE_BASENAME}.sdc" "${BUILD_SRC_DIR}/${PROJECT_NAME}.sdc"
    fi


    ruby -e '
project = ARGV[4]
template = ARGV[5]
conf_str_token = ARGV[6]

qpf_path = ARGV[0]
qpf = File.read(qpf_path)
qpf.sub!(/PROJECT_REVISION = ".*?"/, %{PROJECT_REVISION = "#{project}"}) or
  abort("failed to patch PROJECT_REVISION in #{qpf_path}")
File.write(qpf_path, qpf)

qip_path = ARGV[1]
qip = File.read(qip_path)
qip.gsub!("#{template}.sdc", "#{project}.sdc")
qip.gsub!("#{template}.sv", "#{project}.sv") or
  abort("failed to patch #{template}.sv reference in #{qip_path}")
File.write(qip_path, qip)

sv_path = ARGV[2]
sv = File.read(sv_path)
sv.sub!(%("#{conf_str_token}"), %("#{project};;")) or
  abort("failed to patch CONF_STR token #{conf_str_token.inspect} in #{sv_path}")
File.write(sv_path, sv)

current_project_path = ARGV[3]
if File.exist?(current_project_path)
  File.write(current_project_path, "#{project}\n")
end
' "${BUILD_SRC_DIR}/${PROJECT_NAME}.qpf" \
   "${BUILD_SRC_DIR}/files.qip" \
   "${BUILD_SRC_DIR}/${PROJECT_NAME}.sv" \
   "${BUILD_SRC_DIR}/CURRENT_PROJECT" \
   "${PROJECT_NAME}" \
   "${TEMPLATE_BASENAME}" \
   "${CONF_STR_TOKEN}"

}

build_project() {
    (
        cd "${BUILD_SRC_DIR}"
        quartus_sh --flow compile "${PROJECT_NAME}" -c "${PROJECT_NAME}"
    )

    local staged_rbf="${BUILD_SRC_DIR}/output_files/${PROJECT_NAME}.rbf"
    local staged_sof="${BUILD_SRC_DIR}/output_files/${PROJECT_NAME}.sof"

    if [ ! -f "${staged_rbf}" ] && [ -f "${staged_sof}" ]; then
        have_command quartus_cpf || { echo "missing required command: quartus_cpf" >&2; return 1; }
        quartus_cpf -c "${staged_sof}" "${staged_rbf}"
    fi

    [ -f "${staged_rbf}" ] || { echo "missing built RBF: ${staged_rbf}" >&2; return 1; }
    cp "${staged_rbf}" "${OUTPUT_DIR}/${PROJECT_NAME}.rbf"
}

build_docker_image() {
    docker_available || { echo "docker build path unavailable" >&2; return 1; }
    [ -n "${MISTER_QUARTUS_INSTALLER_DIR}" ] || { echo "missing MISTER_QUARTUS_INSTALLER_DIR for Quartus image build" >&2; return 1; }
    "${DOCKER_BUILD_SCRIPT}" --installer-dir "${MISTER_QUARTUS_INSTALLER_DIR}"
}

build_project_in_docker() {
    local docker_license_args=()

    docker_image_exists || build_docker_image

    if [ -n "${QUARTUS_LICENSE_SPEC}" ]; then
        if [ -f "${QUARTUS_LICENSE_SPEC}" ]; then
            local container_license_path="/tmp/quartus-license/$(basename "${QUARTUS_LICENSE_SPEC}")"
            docker_license_args+=(-v "${QUARTUS_LICENSE_SPEC}:${container_license_path}:ro")
            docker_license_args+=(-e "LM_LICENSE_FILE=${container_license_path}")
        else
            docker_license_args+=(-e "LM_LICENSE_FILE=${QUARTUS_LICENSE_SPEC}")
        fi
    fi

    docker run --rm \
        --platform "${DOCKER_PLATFORM}" \
        -u "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        "${docker_license_args[@]}" \
        -v "${ROOT_DIR}:${CONTAINER_ROOT}" \
        -w "${CONTAINER_BUILD_SRC_DIR}" \
        "${DOCKER_IMAGE}" \
        bash -lc "quartus_sh --flow compile '${PROJECT_NAME}' -c '${PROJECT_NAME}' && if [ ! -f output_files/${PROJECT_NAME}.rbf ] && [ -f output_files/${PROJECT_NAME}.sof ]; then quartus_cpf -c output_files/${PROJECT_NAME}.sof output_files/${PROJECT_NAME}.rbf; fi"

    local staged_rbf="${BUILD_SRC_DIR}/output_files/${PROJECT_NAME}.rbf"
    [ -f "${staged_rbf}" ] || { echo "missing built RBF after Docker compile: ${staged_rbf}" >&2; return 1; }
    cp "${staged_rbf}" "${OUTPUT_DIR}/${PROJECT_NAME}.rbf"
}

COMMAND=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h)
            usage
            exit 0
            ;;
        --seed)
            [ "$#" -ge 2 ] || { echo "missing value for --seed" >&2; exit 1; }
            CORE_SEED="$2"
            shift 2
            ;;
        --fast|--release)
            echo "note: $1 is no longer needed (fast settings are now the default)" >&2
            shift
            ;;
        --check-env|--prepare-source|--build-image)
            [ -z "${COMMAND}" ] || { echo "multiple commands specified" >&2; exit 1; }
            COMMAND="$1"
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [ "${COMMAND}" = "--check-env" ]; then
    require_base_tools || exit 1
    mkdir -p "${OUTPUT_DIR}"
    echo "core_seed=${CORE_SEED}"
    echo "source_dir=${SOURCE_DIR}"
    if [ -f "${UPSTREAM_FILE}" ]; then
        echo "upstream_metadata=${UPSTREAM_FILE}"
    fi
    echo "planned_project=${BUILD_SRC_DIR}/${PROJECT_NAME}.qpf"
    echo "planned_output=${OUTPUT_DIR}/${PROJECT_NAME}.rbf"
    mode="$(selected_build_mode || true)"
    if [ "${mode}" = "local" ]; then
        echo "build_mode=local"
        echo "quartus_sh=$(command -v quartus_sh)"
        echo "quartus_edition=$(quartus_edition)"
    elif [ "${mode}" = "docker" ]; then
        echo "build_mode=docker"
        echo "docker_image=${DOCKER_IMAGE}"
        echo "docker_platform=${DOCKER_PLATFORM}"
        if [ -n "${QUARTUS_LICENSE_SPEC}" ]; then
            if [ -f "${QUARTUS_LICENSE_SPEC}" ]; then
                echo "license_source=file:${QUARTUS_LICENSE_SPEC}"
            else
                echo "license_source=env:${QUARTUS_LICENSE_SPEC}"
            fi
        else
            echo "license_source=missing"
        fi
        if docker_image_exists; then
            echo "docker_image_present=1"
        else
            echo "docker_image_present=0"
            echo "installer_dir=${MISTER_QUARTUS_INSTALLER_DIR}"
        fi
    else
        echo "missing Quartus build environment: install Quartus Lite or Standard locally, or set MISTER_QUARTUS_INSTALLER_DIR for Docker image builds" >&2
        exit 1
    fi
    exit 0
fi

if [ "${COMMAND}" = "--build-image" ]; then
    require_base_tools || exit 1
    build_docker_image
    exit 0
fi

if [ "${COMMAND}" = "--prepare-source" ]; then
    require_base_tools || exit 1
    prepare_source
    echo "core_seed=${CORE_SEED}"
    echo "prepared_source=${BUILD_SRC_DIR}"
    echo "prepared_project=${BUILD_SRC_DIR}/${PROJECT_NAME}.qpf"
    exit 0
fi

require_base_tools || exit 1

prepare_source
mode="$(selected_build_mode || true)"
if [ "${mode}" = "local" ]; then
    build_project
elif [ "${mode}" = "docker" ]; then
    build_project_in_docker
else
    echo "missing Quartus build environment: install Quartus Lite or Standard locally, or set MISTER_QUARTUS_INSTALLER_DIR for Docker image builds" >&2
    exit 1
fi

echo "built_output=${OUTPUT_DIR}/${PROJECT_NAME}.rbf"
