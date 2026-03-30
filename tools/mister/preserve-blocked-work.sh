#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  tools/mister/preserve-blocked-work.sh --branch <name> --message <msg> [--base <branch>] -- <paths...>

Preserves the specified dirty paths on a dedicated branch, commits them there,
then switches back to the original branch.
EOF
}

branch_name=""
commit_message=""
base_branch=""

while [ "$#" -gt 0 ]; do
    case "$1" in
    --branch)
        branch_name="$2"
        shift 2
        ;;
    --message)
        commit_message="$2"
        shift 2
        ;;
    --base)
        base_branch="$2"
        shift 2
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    --)
        shift
        break
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage
        exit 2
        ;;
    esac
done

if [ -z "${branch_name}" ] || [ -z "${commit_message}" ] || [ "$#" -eq 0 ]; then
    usage
    exit 2
fi

if [ -z "${base_branch}" ]; then
    base_branch="$(git branch --show-current)"
fi

if [ -z "${base_branch}" ]; then
    echo "unable to determine base branch" >&2
    exit 1
fi

if git show-ref --verify --quiet "refs/heads/${branch_name}"; then
    git switch "${branch_name}"
else
    git switch -c "${branch_name}"
fi

git add -- "$@"
if git diff --cached --quiet -- "$@"; then
    echo "no staged changes for preserved paths" >&2
else
    git commit -m "${commit_message}"
fi

git switch "${base_branch}"
