#!/usr/bin/env bash
set -euo pipefail

# Compile and run the RGB565 partial-alpha solid-fill bit-exactness harness on
# the dev host. Needs an arm64 / __ARM_NEON host (Apple Silicon) so the NEON
# kernel in sw_blit_565_fill.h actually compiles and both paths run natively.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
CC="${CC:-clang}"

OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT
BIN="$OUTDIR/test_sw_blit_565_fill"

"$CC" -O2 -march=native -std=c11 -Wall -Wextra -Werror \
    -DCRS_SW_CANVAS_16BPP=1 -DCRS_VIDEO_DRIVER_SOFTWARE=1 \
    -I"$ROOT/src" \
    "$DIR/test_sw_blit_565_fill.c" -o "$BIN"

exec "$BIN"
