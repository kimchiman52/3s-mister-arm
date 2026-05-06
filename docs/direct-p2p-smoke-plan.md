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
- Bilateral smoke: CONDITIONAL. Include in the commit message as either
  "completed — outcome X" or "deferred — rendezvous infra not deployed"
  or "deferred — second network not available".

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

---

## Bilateral smoke — two-home test (CONDITIONAL)

Validates the symmetric-NAT bilateral hole-punch fallback added per
`docs/plan-bilateral-hole-punch.md` (Steps 5b/5c). Joiner detects the
direct-punch failure, transitions through `FALLBACK_SIGNALING`
(REGISTER/POLL against the rendezvous server) into
`FALLBACK_BILATERAL_PUNCH` (simultaneous Stun_HolePunch against the
DELIVER-supplied peer endpoint), and on success arrives at HANDOFF.
Real network and real symmetric NAT only — the offline
`--test-bilateral-punch` harness covers the pure logic.

### Pre-requisites

- Two MiSTer units on **different residential networks** (different
  ISPs / public IPs). At least one peer must sit behind a confirmed
  symmetric NAT — verify ahead of time by running Smoke B on that
  unit and observing `DIRECT_P2P_FAILED_SYMMETRIC` on the joiner.
- Both peers running a build that includes the bilateral feature
  (post-Step-5c on `netplay-direct-only`). Confirm via
  `grep DIRECT_P2P_FALLBACK_SIGNALING` in the deployed build's log
  output, or by checking that `--test-bilateral-punch` exits 0 on
  both binaries.
- Kill-switch off (default): `netplay-direct-p2p-disable-bilateral`
  unset or `false` on both peers.
- Reachable rendezvous server. The default
  `udp://rendezvous.3s-arm.example:3478` is a placeholder sentinel —
  the operator MUST replace it with the real signal URL on both peers
  via `netplay-direct-p2p-signal-url` in
  `/media/fat/games/3s-arm/config/config.ini` before running this
  smoke. Confirm reachability with `nc -u <host> 3478` from a third
  machine; an open UDP port returns no error.

### Procedure

1. Host side (MiSTer #1): OSD -> Netplay -> Direct P2P -> Host. Wait
   for STATUS_READY. Code renders on-screen. Overlay shows
   `Waiting for peer...`.
2. Photograph the code; transcribe.
3. Join side (MiSTer #2 — the symmetric-NAT peer): OSD -> Netplay ->
   Direct P2P -> Join -> enter code.
4. Joiner overlay observations (in order):
   - `Discovering public endpoint...`
   - `Connecting to peer...` (direct-punch attempt — expected to
     fail on symmetric NAT)
   - `Symmetric NAT - coordinating via rendezvous...`
   - `Symmetric NAT - simultaneous hole punch...`
   - `Connected via fallback. Starting session...`
5. Host overlay observations (in order):
   - `Waiting for peer...` (rendezvous resender spawns silently)
   - On bilateral SUCCESS: `Bilateral punch succeeded - transferring socket...`
6. Both MiSTers reach Gekko `CONNECTING` and the game starts.

### Expected log lines

Joiner (`tools/mister/misterctl.sh logs` on MiSTer #2):
```
[direct_p2p] joiner fallback: ...                      (intermediate, only on a transient failure path)
[direct_p2p] joiner DELIVER received peer=<ip>:<port>
[direct_p2p] joiner entering FALLBACK_BILATERAL_PUNCH peer=<ip>:<port> (budget=<n>ms)
```
(There is no separate `joiner bilateral punch SUCCESS` log; success is
implied when the joiner publishes `set_status("Connected via fallback.
Starting session...")` and transitions to `DIRECT_P2P_HANDOFF`.)

Host (`tools/mister/misterctl.sh logs` on MiSTer #1):
```
[direct_p2p] HOST_WAITING published. Code=<code> public=<ip>:<port> (via UPnP|STUN)
[direct_p2p] DELIVER received peer=<ip>:<port>
[direct_p2p] entering FALLBACK_BILATERAL_PUNCH peer=<ip>:<port> (budget=<n>ms)
[direct_p2p] bilateral punch SUCCESS - handoff pending
[direct_p2p] Handoff to netplay: player=<n> peer=<ip>:<port>
```
(The host emits `[direct_p2p] DELIVER received peer=...` from the
DELIVER handler; the joiner emits its own
`[direct_p2p] joiner DELIVER received peer=...` from the inline
REGISTER/POLL loop. Both strings are present in the source verbatim.)

### Pass criteria

- Both MiSTers reach Gekko `CONNECTING`.
- Joiner log shows `joiner DELIVER received peer=...` followed by
  `joiner entering FALLBACK_BILATERAL_PUNCH ...`.
- Host log shows `DELIVER received peer=...`, then
  `entering FALLBACK_BILATERAL_PUNCH ...`, then
  `bilateral punch SUCCESS - handoff pending`.
- No `DIRECT_P2P_FAILED_BILATERAL` terminal state on either peer.

### Failure diagnostics

- **`FAILED_BILATERAL` ("Could not reach peer after fallback. Try
  another network.")** — rendezvous paired the peers but the bilateral
  Stun_HolePunch did not converge within
  `netplay-direct-p2p-bilateral-punch-ms` (default 3000ms). Capture:
  - tcpdump on the rendezvous server (or its uplink) filtered to
    UDP/3478 to confirm both peers REGISTERed and the server emitted
    DELIVER to each.
  - tcpdump on each MiSTer's uplink filtered to the peer's public IP
    (read from the DELIVER log line) for the bilateral budget window.
  - Both peers' STUN-discovered public IPs (search log for
    `HOST_WAITING published. Code=... public=<ip>:<port>` on the host
    and the equivalent joiner-side STUN result). If either peer's
    public port shifted between STUN-discover and the DELIVER (port-
    reallocating symmetric NAT), bilateral cannot succeed without port
    prediction — out of scope per plan §11.
- **Hairpin / `FAILED_SYMMETRIC` ("Cannot reach peer on this network.
  (NAT loopback not supported.)")** — the host received a DELIVER
  whose peer IP equals its own public IP, indicating both peers are
  behind the same NAT. Bilateral is suppressed in this case (see
  `direct_p2p.c` DELIVER handler hairpin guard). Move one peer to a
  different network (LTE hotspot) and retry.
- **Stuck on `Symmetric NAT - coordinating via rendezvous...`** for
  longer than `netplay-direct-p2p-signal-budget-ms` (default 8000ms)
  without progressing — the joiner could not reach the rendezvous
  server. From a workstation: `dig <signal-host>` to confirm DNS,
  `nc -u <signal-host> <signal-port>` to confirm the UDP path is open.
  Server-side: confirm the listener is up
  (`ss -ulnp | grep 3478` on the rendezvous host).

### Kill-switch verification

Validates that `netplay-direct-p2p-disable-bilateral=true`
short-circuits the joiner and that the host's solo rendezvous loop
times out cleanly.

1. On the joiner: edit `/media/fat/games/3s-arm/config/config.ini`
   and set `netplay-direct-p2p-disable-bilateral = 1`. Re-launch.
2. Host: as in the main procedure, OSD -> Direct P2P -> Host.
3. Joiner: enter code. Expected: joiner lands on
   `DIRECT_P2P_FAILED_SYMMETRIC` promptly (within the direct-punch
   budget; no rendezvous attempt). Joiner overlay shows
   `Cannot reach peer. Possible Symmetric NAT.` Capture tcpdump on
   the joiner uplink filtered to UDP/3478 — there must be **zero**
   packets to the rendezvous server.
4. Host: rendezvous resender runs for the signal-budget window
   (default 8s), then host overlay flips to
   `Waiting for peer (no peer detected - check that they're using a
   recent build).` Host stays in `DIRECT_P2P_HOST_WAITING`; no
   terminal failure.
5. Revert: delete or set `netplay-direct-p2p-disable-bilateral = 0`
   on the joiner so default behavior is restored for later smokes.

### Notes

- The bilateral path uses a role-branched mode label internally
  (`joiner entering FALLBACK_BILATERAL_PUNCH` vs. plain
  `entering FALLBACK_BILATERAL_PUNCH` from the host worker thread).
  Both lines are valid; grep for the verbatim host string when
  triaging host logs and the verbatim joiner string when triaging
  joiner logs — do NOT collapse them.
- Strict-port-allocation NATs (a.k.a. fully port-reallocating
  symmetric NAT, where the public port changes per-destination) are
  **out of scope** for this smoke. Bilateral hole-punch succeeds only
  when each peer's NAT preserves the public port for the duration of
  the bilateral budget. Per plan §11, port prediction and TURN are
  not implemented; if both peers exhibit strict reallocation, the
  expected outcome is a clean `FAILED_BILATERAL`.
- This scenario is **CONDITIONAL** — it depends on a deployed
  rendezvous server and access to two genuinely separate networks.
  Defer with `deferred — rendezvous infra not deployed` or
  `deferred — second network not available` in the commit message
  if either is missing. Do NOT block bilateral feature commits on
  this; the offline `--test-bilateral-punch` harness is the gating
  test for code changes.
