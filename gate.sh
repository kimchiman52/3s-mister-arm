#!/usr/bin/env bash
# Task #103 consolidation gate. NOT for commit — lane-private.
# Usage: ./gate.sh <label>
set -u
cd "$(dirname "$0")"
LABEL="${1:-gate}"
OUT="/tmp/t103-gate-$LABEL"
mkdir -p "$OUT"
FAILED=0

echo "############ GATE: $LABEL ############"

# ---- 1. Build the hooks-ON test config -------------------------------
cmake --build build/host-nptest -j8 > "$OUT/build-nptest.log" 2>&1
NP_BUILD=$?
echo "BUILD host-nptest (hooks ON,  -DENABLE_NETPLAY_TESTS) exit=$NP_BUILD"
[ $NP_BUILD -ne 0 ] && { FAILED=1; grep -E "error:" "$OUT/build-nptest.log" | head -25; }

# ---- 2. Build the SHIPPED config -------------------------------------
cmake --build build/host-release -j8 > "$OUT/build-release.log" 2>&1
REL_BUILD=$?
echo "BUILD host-release (SHIPPED: ENABLE_NETPLAY=ON, hooks OFF) exit=$REL_BUILD"
[ $REL_BUILD -ne 0 ] && { FAILED=1; grep -E "error:" "$OUT/build-release.log" | head -25; }

# ---- 3. Netplay harnesses, TRUE exit codes ---------------------------
BIN=build/host-nptest/3S-ARM.app/Contents/MacOS/3S-ARM
if [ $NP_BUILD -eq 0 ] && [ -x "$BIN" ]; then
  HARNESSES="test-netplay-event-queue test-mist-handshake test-room-code test-stun-mock \
test-sparse-effect-save test-bilateral-punch test-gs-coverage test-texcash-bounds"
  # 9th harness only exists once #36 has landed
  grep -q '"test-connect-observability"' src/args.c 2>/dev/null && \
    HARNESSES="$HARNESSES test-connect-observability"
  N=0
  for h in $HARNESSES; do
    N=$((N+1))
    "./$BIN" "--$h" > "$OUT/$h.log" 2>&1
    rc=$?
    note=""
    # exit 2 + "not compiled in" is a MISBUILD, never a pass
    if grep -q "not compiled in" "$OUT/$h.log"; then note="  <<< NOT COMPILED IN (misbuild, not a pass)"; fi
    printf "  %-32s exit=%-3s%s\n" "$h" "$rc" "$note"
    if [ $rc -ne 0 ] || [ -n "$note" ]; then FAILED=1; fi
  done
  echo "  harness count = $N (expect 8 pre-#36, 9 post-#36)"
else
  echo "  HARNESSES SKIPPED — nptest build failed"; FAILED=1
fi

# ---- 4. Rendezvous protocol tests (mainline gate) --------------------
if [ -f tools/rendezvous-server/__test_protocol.js ]; then
  node tools/rendezvous-server/__test_protocol.js > "$OUT/protocol.log" 2>&1
  rc=$?
  echo "__test_protocol.js exit=$rc  ($(grep -cE '^(ok|PASS|✓)' "$OUT/protocol.log" 2>/dev/null) ok-lines)"
  tail -3 "$OUT/protocol.log"
  [ $rc -ne 0 ] && FAILED=1
else
  echo "__test_protocol.js MISSING"; FAILED=1
fi

echo "############ GATE $LABEL RESULT: $([ $FAILED -eq 0 ] && echo GREEN || echo RED) ############"
exit $FAILED
