#!/usr/bin/env bash
#
# toolchain-diagnostic-delta -- enumerate what the host compiler and the ARM
# cross compiler disagree about.
#
# Task #106. "Both are clang, so they agree" was load-bearing in this project
# and had never been measured. It is now, and it is only half true:
#
#   * The two toolchains are DIFFERENT CLANG MAJORS. The ARM container installs
#     clang-20 from apt.llvm.org (`llvm_version` default,
#     tools/mister/setup-build-container.sh:13; the apt.llvm.org source and
#     install at :202-209);
#     the host is whatever Xcode ships. So a version-driven delta is possible in
#     principle and this script measures it.
#
#   * But the delta that actually bit us -- the -Wformat-truncation that failed
#     the first ARM build of ed37cb42 after every host gate passed -- is NOT in
#     that set. Both compilers implement that warning and both enable it under
#     -Wall. The blindness came from Darwin's libc headers macro-rewriting
#     snprintf out from under the check. See fortify_blind_sweep.py for the
#     measurement, and host-diagnostic-parity.sh for the gate that closes it.
#
# So this script answers "which diagnostics does only one of them KNOW ABOUT",
# and host-diagnostic-parity.sh answers "which diagnostics does only one of them
# actually RAISE on our code". Both questions are real; neither subsumes the
# other. This one is the cheap, offline half.
#
# Each compiler is asked to enumerate its own warning groups via
# `--autocomplete=-W`, which is the shell-completion interface clang exposes for
# exactly this and is therefore a primary source rather than a scrape of docs.
#
# Usage:
#   tools/gates/toolchain-diagnostic-delta.sh [--out DIR]
#
# Exit codes: 0 delta enumerated, 2 harness error (a compiler was unreachable).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

out_dir=""
while [ $# -gt 0 ]; do
    case "$1" in
    --out) out_dir="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [ -z "${out_dir}" ]; then
    out_dir="$(mktemp -d)"
    trap 'rm -rf "${out_dir}"' EXIT
fi
mkdir -p "${out_dir}"

container="${MISTER_BUILD_CONTAINER:-3s-mister-arm-build}"
llvm_version="${MISTER_LLVM_VERSION:-20}"
host_cc="${CC:-cc}"

groups() {  # groups <label> <command...>
    local label="$1"; shift
    if ! "$@" --autocomplete=-W 2>/dev/null \
        | sed 's/\t.*//' | grep -v '^-W$' | sort -u > "${out_dir}/${label}.groups"; then
        return 1
    fi
    [ -s "${out_dir}/${label}.groups" ]
}

echo "=== host toolchain ==="
if ! "${host_cc}" --version | head -1; then
    echo "harness error: host compiler '${host_cc}' is not runnable" >&2
    exit 2
fi
if ! groups host "${host_cc}"; then
    echo "harness error: '${host_cc} --autocomplete=-W' produced nothing." >&2
    echo "  This compiler does not expose its warning-group list, so the delta" >&2
    echo "  cannot be enumerated. Do not read that as 'no delta'." >&2
    exit 2
fi
echo "  warning groups: $(wc -l < "${out_dir}/host.groups")"
echo

echo "=== ARM cross toolchain (container ${container}, clang-${llvm_version}) ==="
if ! docker exec "${container}" clang-"${llvm_version}" --version 2>/dev/null | head -1; then
    echo "harness error: cannot reach clang-${llvm_version} in container '${container}'." >&2
    echo "  Start it with tools/mister/setup-build-container.sh, or set" >&2
    echo "  MISTER_BUILD_CONTAINER. Refusing to report a delta measured against" >&2
    echo "  only one compiler." >&2
    exit 2
fi
if ! docker exec "${container}" clang-"${llvm_version}" --autocomplete=-W 2>/dev/null \
    | sed 's/\t.*//' | grep -v '^-W$' | sort -u > "${out_dir}/arm.groups"; then
    echo "harness error: could not enumerate the cross compiler's warning groups" >&2
    exit 2
fi
if [ ! -s "${out_dir}/arm.groups" ]; then
    echo "harness error: the cross compiler's warning-group list came back empty" >&2
    exit 2
fi
echo "  warning groups: $(wc -l < "${out_dir}/arm.groups")"
echo

echo "=== groups the ARM cross compiler knows and the host does NOT ==="
comm -13 "${out_dir}/host.groups" "${out_dir}/arm.groups" | grep -v '^-Wno-' || true
echo
echo "=== groups the host knows and the ARM cross compiler does NOT ==="
comm -23 "${out_dir}/host.groups" "${out_dir}/arm.groups" | grep -v '^-Wno-' || true
echo

only_arm=$(comm -13 "${out_dir}/host.groups" "${out_dir}/arm.groups" | grep -vc '^-Wno-' || true)
only_host=$(comm -23 "${out_dir}/host.groups" "${out_dir}/arm.groups" | grep -vc '^-Wno-' || true)
echo "DELTA SUMMARY: only_arm=${only_arm} only_host=${only_host} (group names, not firings)"
echo
echo "NOTE: a group present in both lists can still fire on only one of them --"
echo "      that is what host-diagnostic-parity.sh measures. A group missing"
echo "      from one list cannot fire there at all."
