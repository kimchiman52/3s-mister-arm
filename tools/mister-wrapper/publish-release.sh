#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TAG_NAME="${TAG_NAME:-rolling-pre-release}"
ZIP_PATH="${ZIP_PATH:-${ROOT_DIR}/build/mister-release/3S-ARM-mister-rolling-pre-release.zip}"
REMOTE_NAME="${REMOTE_NAME:-origin}"
ASSET_GLOB="${ASSET_GLOB:-3S-ARM-mister-*.zip}"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/publish-release.sh --check
  tools/mister-wrapper/publish-release.sh [--zip <file>] [--tag <tag>] [--remote <name>]

Purpose:
  Move the rolling prerelease tag to the current commit and upload the local
  MiSTer release zip to GitHub Releases using gh.

Defaults:
  zip=${ZIP_PATH}
  tag=${TAG_NAME}
  remote=${REMOTE_NAME}
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "required command not found: $1" >&2
        exit 1
    }
}

require_inputs() {
    [ -f "${ZIP_PATH}" ] || { echo "release zip not found: ${ZIP_PATH}" >&2; return 1; }
    git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "not inside a git repository" >&2; return 1; }
}

write_notes() {
    local notes_path="$1"
    local full_sha="$2"
    local asset_name="$3"

    cat > "${notes_path}" <<EOF
Automatic release for commit ${full_sha}

Updated MiSTer asset: ${asset_name}

This is a pre-release build. Expect bugs and instability.
EOF
}

delete_matching_assets() {
    local asset_name
    while IFS= read -r asset_name; do
        case "${asset_name}" in
        ${ASSET_GLOB})
            gh release delete-asset "${TAG_NAME}" "${asset_name}" --yes
            ;;
        esac
    done < <(gh release view "${TAG_NAME}" --json assets --jq '.assets[].name')
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
    --zip)
        ZIP_PATH="$2"
        shift 2
        ;;
    --tag)
        TAG_NAME="$2"
        shift 2
        ;;
    --remote)
        REMOTE_NAME="$2"
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

require_cmd gh
require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "zip=${ZIP_PATH}"
    echo "tag=${TAG_NAME}"
    echo "remote=${REMOTE_NAME}"
    exit 0
fi

full_sha="$(git rev-parse HEAD)"
short_sha="$(git rev-parse --short HEAD)"
asset_name="$(basename "${ZIP_PATH}")"
notes_file="$(mktemp "${TMPDIR:-/tmp}/3s-arm-release-notes.XXXXXX")"
trap 'rm -f "${notes_file}"' EXIT

write_notes "${notes_file}" "${full_sha}" "${asset_name}"

git tag -f "${TAG_NAME}" "${full_sha}"
git push "${REMOTE_NAME}" "refs/tags/${TAG_NAME}" --force

if gh release view "${TAG_NAME}" >/dev/null 2>&1; then
    gh release edit "${TAG_NAME}" \
        --title "${short_sha}" \
        --prerelease \
        --notes-file "${notes_file}"
    delete_matching_assets
    gh release upload "${TAG_NAME}" "${ZIP_PATH}" --clobber
else
    gh release create "${TAG_NAME}" "${ZIP_PATH}" \
        --verify-tag \
        --title "${short_sha}" \
        --prerelease \
        --notes-file "${notes_file}"
fi

echo "published_tag=${TAG_NAME}"
echo "published_asset=${asset_name}"
