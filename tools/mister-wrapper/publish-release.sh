#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RELEASE_DATE="${RELEASE_DATE:-$(date +%Y-%m-%d)}"
TAG_NAME="${TAG_NAME:-v${RELEASE_DATE//-/}}"
ZIP_PATH="${ZIP_PATH:-${ROOT_DIR}/build/mister-release/3s-mister-arm-${RELEASE_DATE}.zip}"
REMOTE_NAME="${REMOTE_NAME:-origin}"
RELEASE_REPO="${RELEASE_REPO:-kimchiman52/3s-mister-arm}"

usage() {
    cat <<EOF
Usage:
  tools/mister-wrapper/publish-release.sh --check
  tools/mister-wrapper/publish-release.sh [options]

Options:
  --date <YYYY-MM-DD>   Release date (default: today)
  --tag <tag>           Override tag (default: v\${RELEASE_DATE//-/})
  --zip <file>          Override zip path
  --remote <name>       Git remote for tag push (default: origin)
  --repo <owner/name>   GitHub repo for release (default: kimchiman52/3s-mister-arm)
  --check               Print computed inputs and exit
  --help                Show this help

Publishes the MiSTer release zip to GitHub Releases using gh:
  - tags current HEAD with vYYYYMMDD (dated, not semver)
  - pushes that tag to the fork remote (origin = kimchiman52/3s-mister-arm)
  - creates a normal GitHub release marked --latest
  - no --prerelease flag, no rolling-pre-release tag

Fails if the tag or release already exists. Pick a different --date or clean
up the prior one manually; this script will not force-move tags or clobber
an existing release.

Defaults:
  date=${RELEASE_DATE}
  tag=${TAG_NAME}
  zip=${ZIP_PATH}
  remote=${REMOTE_NAME}
  repo=${RELEASE_REPO}
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

check_tag_free() {
    if git rev-parse -q --verify "refs/tags/${TAG_NAME}" >/dev/null; then
        echo "tag already exists locally: ${TAG_NAME} — delete it or choose a different --date" >&2
        return 1
    fi
    if git ls-remote --tags "${REMOTE_NAME}" "refs/tags/${TAG_NAME}" 2>/dev/null | grep -q "refs/tags/${TAG_NAME}$"; then
        echo "tag already exists on remote ${REMOTE_NAME}: ${TAG_NAME} — delete it or choose a different --date" >&2
        return 1
    fi
    if gh release view "${TAG_NAME}" --repo "${RELEASE_REPO}" >/dev/null 2>&1; then
        echo "GitHub release already exists: ${RELEASE_REPO} ${TAG_NAME} — delete it or choose a different --date" >&2
        return 1
    fi
}

write_notes() {
    local notes_path="$1"
    local full_sha="$2"
    local asset_name="$3"

    cat > "${notes_path}" <<EOF
Build for commit ${full_sha}

Asset: ${asset_name}
EOF
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
    --date)
        RELEASE_DATE="$2"
        TAG_NAME="v${RELEASE_DATE//-/}"
        ZIP_PATH="${ROOT_DIR}/build/mister-release/3s-mister-arm-${RELEASE_DATE}.zip"
        shift 2
        ;;
    --tag)
        TAG_NAME="$2"
        shift 2
        ;;
    --zip)
        ZIP_PATH="$2"
        shift 2
        ;;
    --remote)
        REMOTE_NAME="$2"
        shift 2
        ;;
    --repo)
        RELEASE_REPO="$2"
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
require_cmd git
require_inputs || exit 1

if [ "${check_only}" -eq 1 ]; then
    echo "date=${RELEASE_DATE}"
    echo "tag=${TAG_NAME}"
    echo "zip=${ZIP_PATH}"
    echo "remote=${REMOTE_NAME}"
    echo "repo=${RELEASE_REPO}"
    exit 0
fi

check_tag_free || exit 1

full_sha="$(git rev-parse HEAD)"
asset_name="$(basename "${ZIP_PATH}")"
notes_file="$(mktemp "${TMPDIR:-/tmp}/3s-arm-release-notes.XXXXXX")"
trap 'rm -f "${notes_file}"' EXIT

write_notes "${notes_file}" "${full_sha}" "${asset_name}"

git tag "${TAG_NAME}" "${full_sha}"
git push "${REMOTE_NAME}" "refs/tags/${TAG_NAME}"

gh release create "${TAG_NAME}" "${ZIP_PATH}" \
    --repo "${RELEASE_REPO}" \
    --verify-tag \
    --title "${TAG_NAME}" \
    --latest \
    --notes-file "${notes_file}"

echo "published_tag=${TAG_NAME}"
echo "published_asset=${asset_name}"
echo "published_repo=${RELEASE_REPO}"
