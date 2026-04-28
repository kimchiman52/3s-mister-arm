# Direct-P2P On-Device Smoke Plan

Step 12 of `docs/plan-stun-direct-p2p.md`. Four smoke scenarios validate
the Host/Join Direct-P2P pipeline end-to-end against real hardware and
real NAT. The compile-in test harness (`--test-stun-mock`) and the
room-code codec test (`--test-room-code`) cover the offline surface; the
scenarios below cover what only a real MiSTer + router can exercise.

## Prerequisites

Before running any of these, confirm the following once per session:

1. MiSTer reachable at `192.168.1.171` (or update `MISTER_HOST`).
2. A game build with `ENABLE_NETPLAY=ON` is deployed:
   ```bash
   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry
   ```
   Deploy per `docs/mister-runbook.md` (rsync the install tree into
   `/media/fat/games/3s-arm/`; NEVER use `--delete`).
3. Room-code codec has already been validated offline:
   ```bash
   "${HOST_BIN}" --test-room-code   # expect exit 0
   ```
4. STUN mock-server harness passes offline:
   ```bash
   "${HOST_BIN}" --test-stun-mock   # expect exit 0
   ```
   This requires `-DENABLE_NETPLAY_TESTS` in `CMAKE_C_FLAGS`. A build
   without the flag returns exit code 2 with a `"not compiled in"`
   diagnostic; that is the expected fallback behavior, not a failure.

Evidence captured per scenario: serial-over-SSH log excerpt, a screen
photo of any on-screen code display, and the router UPnP state where
relevant. Artifacts live in a per-session scratch dir; do NOT commit
them.

---

## Smoke A — LAN hairpin (REQUIRED)

Two MiSTers on the same home LAN. STUN reports the same public IP for
both, so the joiner's Direct-P2P state machine must rewrite the peer
IP to `127.0.0.1` (the host's internal_port from the room code). This
is the path that upstream's sdl_netplay_ui.cpp:719-720 uses for
same-LAN play.

### Setup

- MiSTer #1 (host): 192.168.1.171
- MiSTer #2 (join): a second MiSTer on the same LAN (identify its IP
  from the router admin page or `nmap -sn 192.168.1.0/24`).
- Router: default (UPnP enabled or disabled — either works for A;
  hairpin rewrite kicks in regardless).

### Steps

1. Host side (MiSTer #1): OSD -> Netplay -> Direct P2P -> Host. Wait
   for STATUS_READY. Code renders on-screen.
2. Photograph the code; read it manually (or copy via SSH if the
   handoff file has been written to `/tmp/3s-arm-netplay.handoff` —
   see Step 9).
3. Join side (MiSTer #2): OSD -> Netplay -> Direct P2P -> Join ->
   enter code.
4. Observe on MiSTer #2's log (via `tools/mister/misterctl.sh logs`
   or direct serial): a line containing
   `"hairpin"` or similar indicating the peer IP was rewritten to
   loopback. The direct_p2p.c state machine should transition through
   `DIRECT_P2P_STUN_DISCOVER` -> `DIRECT_P2P_HOLE_PUNCH` ->
   `DIRECT_P2P_SESSION_STARTING`.
5. netplay.c should log `"starting a session"` and Gekko reaches
   CONNECTING state.

### Pass criteria

- Both MiSTers reach Gekko `CONNECTING`.
- Joiner log shows the hairpin rewrite diagnostic.
- No `DIRECT_P2P_FAILED_*` terminal state.

### Notes

- If only one MiSTer is available, Smoke A can substitute a host-desktop
  game build for the joiner (macOS or Linux). The hairpin path depends
  on public-IP equality, which still holds when both devices NAT out
  through the same home router.

---

## Smoke B — Two networks (CONDITIONAL, not required)

Two MiSTers (or one MiSTer + laptop) on genuinely different public
networks. Exercises the "real internet" path: UPnP mapping on the
host side, STUN discover on both, hole punch through both NATs.

### Setup (when available)

- Host: MiSTer on home LAN with UPnP-enabled router.
- Joiner: second device tethered to a phone hotspot (LTE/5G, commonly
  Symmetric NAT or CGNAT). Alternative: a friend's home network
  behind a different ISP.
- Confirm both public IPs differ via `curl ifconfig.me` on each side.

### Steps

1. Host: OSD -> Direct P2P -> Host. Wait for STATUS_READY.
2. Confirm UPnP succeeded (host log: `UPnP mapping OK (external
   <port> -> internal <port>)`). If UPnP fails, note it; the test
   still proceeds via STUN-hairpin-less direct connection.
3. Joiner: enter code. Observe hole-punch attempt.
4. Expected outcomes (either counts as pass):
   a. Connection succeeds — host + joiner both reach Gekko
      `CONNECTING`. This is the canonical UPnP + STUN success path.
   b. Clean symmetric-NAT failure — joiner transitions to
      `DIRECT_P2P_FAILED_SYMMETRIC_NAT` and renders a user-visible
      "peer appears to be behind symmetric NAT" message. This is
      valid when the hotspot performs Symmetric NAT (most LTE
      networks do).
5. Unacceptable outcome: crash, hang, or silent timeout without
   reaching either of (a)/(b).

### When to defer

This scenario is CONDITIONAL. If no phone hotspot or second network is
available during implementation, mark the smoke as
**deferred — external network not available** in the commit message
and flag it for post-MVP validation. Do NOT block Step 12 on this.

---

## Smoke C — UPnP-disabled router (REQUIRED)

Forces the state machine down the STUN-only branch. Validates that
`CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP` correctly short-circuits the
UPnP phase and that STUN-hairpin still reaches a connection on the LAN.

### Setup

Pick one of the two equivalent approaches:

**(Option 1 — runtime override, preferred for iteration speed.)**
Edit `/media/fat/games/3s-arm/config/config.ini` on both MiSTers:
```
netplay-direct-p2p-disable-upnp = 1
```
Re-launch the game. No router changes needed.

**(Option 2 — router-level, for realism.)** Log into the router admin
page (e.g., `http://192.168.1.1`) and disable UPnP/IGD. Leave the game
config default. Capture a screenshot of the router page as evidence.

### Steps

1. Host (MiSTer #1): OSD -> Direct P2P -> Host.
2. Observe log: `"UPnP unavailable or refused; falling back to STUN"`
   (direct_p2p.c:209). The state machine skips the UPnP success path
   and proceeds to `DIRECT_P2P_STUN_DISCOVER` directly.
3. Join (MiSTer #2 or host build): enter code.
4. Same-LAN hairpin rewrite still applies (Smoke A's condition), so
   the connection completes over 127.0.0.1.

### Pass criteria

- Log shows UPnP was skipped (with the reason: either the runtime
  config key or miniupnpc's own discovery failure).
- Gekko session reaches `CONNECTING`.
- No `DIRECT_P2P_FAILED_*` state.

### Notes

- If the `disable-upnp` config key was used, revert it after the
  test (delete or set to `0`) so default-config behavior is preserved
  for later smoke runs.

---

## Smoke D — Base32 room-code round-trip (REQUIRED, offline)

Covered entirely by `--test-room-code`. Runs in ~10 ms, no network.

### Steps

```bash
"${HOST_BIN}" --test-room-code
```

### Pass criteria

- Exit code 0.
- stderr lists "OK" for each case (all-zeros, all-ones, loopback,
  dns-google, lan, high-port, asymmetric, typo detection, bogus-input
  rejection, normalization sanity).

### Notes

- If the binary was built without `-DENABLE_NETPLAY_TESTS`, the flag
  returns exit 2 with `"not compiled in"`. That is the
  intentional-stub path and does NOT satisfy Smoke D. Rebuild with
  `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"`
  and retry.

---

## Commit gating

- Smoke A, Smoke C, Smoke D: REQUIRED before committing Step 12
  follow-on work. Paste log evidence into the commit message.
- Smoke B: CONDITIONAL. Include in the commit message as either
  "completed — outcome X" or "deferred — hotspot not available".

## What NOT to do

- Do not run any scenario behind a VPN (Tailscale, Wireguard, etc.).
  Per plan locked decision #7, Direct-P2P is validated on the real
  internet only. Disable any VPN before testing.
- Do not commit smoke artifacts (logs, screenshots, config dumps)
  into git. Keep them in a per-session scratch directory.
- Do not use `rsync --delete` on any deploy during smoke iteration.
  Per `feedback-no-rsync-delete.md`, this destroys game data.
- Do not edit `menu.rbf`. Core RBF goes to
  `/media/fat/_Other/3S-ARM.rbf` per `docs/mister-runbook.md`.
