# STUN + UPnP Direct-P2P Port — Status Snapshot

> **ARCHIVED 2026-08-30** — true at c803dfbb, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


Wake-time status report. All plan steps implemented and committed on
branch `netplay`. The only outstanding action is deploying to the
MiSTer at `192.168.1.171`, which was offline at session end (device
not responding to ping).

> **Historical snapshot — do not use as current reference.** Everything
> below describes the branch/artifact state at the end of that one
> session. The room code has since gone 11 → 14 (v2) → **18** chars
> (v3: version char `'3'` + 80-bit `ip(32)|port(16)|nonce(32)` in 16
> Crockford chars + ISO 7064 check digit, displayed
> `XXXXXX-XXXXXX-XXXXXX`). Wherever this file says 11 or 14 chars, read
> it as a record of what was true then. Current spec:
> `docs/plan-netplay-connection.md` §6.3, with the S4 adversarial-review
> fixes in §6.8.

## What shipped tonight

All 12 steps of `docs/plan-stun-direct-p2p.md` plus the plan itself
plus fix commits plus the glue commits plus a UX polish commit
(`7afbeefd`). Branch `netplay` head after this session ended at
`7afbeefd`.

## UX polish bundle (`7afbeefd`, final commit of the session)

Three changes, one commit, all review-driven:

1. **Stripped "Host Port" + "Disable UPnP" OSD toggles.** Power
   users still have them as config.ini keys; the OSD menu is now
   a two-option submenu (Host Game / Join Game / Exit).
2. **UPnP ~3s wall-clock timeout.** `try_upnp()` in `direct_p2p.c`
   now wraps `Upnp_AddMapping` in an `SDL_Thread` with a 3000 ms
   deadline, detaches on timeout, and falls through to STUN. Prevents
   a broken IGD router from stalling the Host Game flow.
3. **Room code 14 → 11 chars.** Dropped `local_port` from the
   payload (6-byte payload, 10 payload chars + 1 check digit).
   Hairpin routing now relies on NAT loopback instead of the
   `127.0.0.1:local_port` shortcut. `room_code.h` documents the
   rationale. *(That 11-char form was superseded twice — v2 at 14 chars,
   v3 at 18. `local_port` stayed dropped; the growth is the nonce.)*

The `main-mister-full-menu.patch` hunk headers were re-validated
against pinned Main_MiSTer ref `3380931329b8acb442bd3d35a24d89f88641b7cf`
(`git apply --check` clean).

Plan: `b1ad8b16` (docs/plan-stun-direct-p2p.md, 695 LOC, reviewed via
the 3-agent loop).

Step commits, in order:

| Step | Commit | What landed |
|---|---|---|
| 1 | `89ec3ef6` | SDL3_net struct-layout gate + `net_tuning.h` port (89 LOC) |
| 2 | `2b89937a` | Base32 codec + ISO 7064 MOD 37,36 check digit + `--test-room-code` (695 LOC) |
| 3 | `4708a65e` | STUN client verbatim port (506 LOC) |
| 4 | `a0512a0f` | UPnP client + miniupnpc cross-compile (247 LOC) |
| 5 | `0cc09890` | Five new `CFG_KEY_NETPLAY_DIRECT_P2P_*` config keys + docs (53 LOC) |
| 6 | `b6939363` | `Netplay_SetStunSocket` + `Netplay_SetSessionTeardownCallback` (77 LOC) |
| 7 | `fff34228` | Direct-P2P orchestrator state machine (754 LOC) |
| 8 | `eee6ed9e` | In-game status overlay via `SSPutStrPro` (120 LOC) |
| 9 | `13e8b064` | `--direct-p2p-handoff` CLI + handoff file reader (402 LOC) |
| 10 | `81b853c8` | Wrapper OSD Direct-P2P submenu + 6×7 peer-code OSK (485 LOC) |
| 11 | `75034eed` | Wrapper exec-arg injection for handoff-armed relaunch (27 LOC) |
| 12 | `53fc2dfc` | STUN mock test harness + smoke plan (728 LOC) |

Plus two glue commits that landed during the session from earlier
threads: `23954b90` (RmlUi per-screen init/update hooks — fixes the
"template text shows raw" bug from the Phase 6 on-device smoke; also
strips post-smoke debug traces).

## Build artifacts (ready to deploy)

- `build/mister-telemetry-package/bin/3s-arm` — 8,008,684 bytes
  (8.01 MB) ARM v7 hard-float ELF. Fresh build with
  `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON"`.
  Contains STUN, UPnP (3s-timeout wrapped), direct_p2p orchestrator,
  11-char room code codec, handoff reader. `libminiupnpc.so.17` is
  in the `NEEDED` list.
- `build/mister-wrapper-hps/MiSTer_3S-ARM` — 1,035,012 bytes
  (1.01 MB) ARM EABI5 hard-float ELF. OSD Direct-P2P submenu
  (Host Game / Join Game / Exit — power-user toggles stripped)
  + 6×7 OSK for 11-char code entry + handoff writer + exec-arg
  injection.

## What to run when the MiSTer is back online

Device is at `192.168.1.171`. Check state first:

```
MISTER_PASSWORD=1 tools/mister/misterctl.sh busy-status
MISTER_PASSWORD=1 tools/mister/misterctl.sh lock-status
```

Then deploy both the game runtime and the wrapper:

```
# Game runtime (assets, binary, launcher, scripts)
MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy \
    --src build/mister-telemetry-package

# Wrapper (OSD menu binary)
MISTER_PASSWORD=1 tools/mister/misterctl.sh deploy-wrapper \
    --src build/mister-wrapper-hps --wrapper-only
```

The `--wrapper-only` flag keeps the 3S-ARM.rbf and games/3s-arm
directories untouched — only `MiSTer_3S-ARM` at `/media/fat/` gets
refreshed.

## Smoke tests to run

Per `docs/direct-p2p-smoke-plan.md`:

- **Smoke A (hairpin / single MiSTer — doable solo):** Boot core
  → F12 → System → Direct P2P → Host Game. STUN discovers the
  device's public IP. Since you're Hosting alone, just verify the
  OSD transitions through UPNP_PROBE → STUN_DISCOVER → HOST_WAITING
  and a room code appears on-screen. Exit cleanly.

  Then try Join Game with a fabricated code — should render the
  OSK, let you type **18** chars (`DP2P_CODE_CHAR_LEN`, dashes after
  chars 6 and 12), and then fail with "Invalid room code" or "Cannot
  reach peer". Tests the full OSD + handoff + game-side flow end-to-end
  without a partner. *(This line said "14 chars" and contradicted the
  "11-char code entry" artifact note above it even at the time; 18 is the
  current v3 length.)*

- **Smoke C (UPnP toggle — doable solo):** Host Game with `Disable
  UPnP` = Off, then again with = On. First run logs
  `Opening port forward via UPnP...`; second skips straight to
  STUN. Verify via `last-run.log` that the orchestrator's state
  machine follows the two paths.

- **Smoke B (two home networks — requires partner):** your buddy
  needs a MiSTer, same-build 3s-arm + MiSTer_3S-ARM, `ping
  <your-public-ip>` is not required. Exchange codes over text, one
  hosts, the other joins. Conditional — document the outcome but
  not blocking.

## Known limitations (ship-as-is)

1. `CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS` is declared but not
   honored: `Stun_Discover` has its own 2 s/server internal cap.
   Wiring the config key would require a `Stun_Discover` signature
   change (upstream-incompatible). Flagged in `direct_p2p.c:150`.
2. `Netplay_SetParams(player, ip)` hardcodes `remote_port=50000`;
   STUN-negotiated peer port is currently overridden. A proper fix
   needs `Netplay_SetRemotePort` setters on `netplay.c`.
3. Host Port and Disable UPnP OSD toggles persist only in
   process-local statics; the handoff file schema today carries
   only `mode=host|join` + `peer_code=`. Extending to `port=` /
   `disable_upnp=` requires matching parser/writer updates.
4. Symmetric NAT: the orchestrator detects and fails gracefully
   with `DIRECT_P2P_FAILED_SYMMETRIC` (status overlay renders
   "Cannot reach peer. Possible Symmetric NAT."). No TURN fallback
   per locked decision.

## Bilateral hole-punch fallback (post-snapshot)

A bilateral hole-punch fallback now layers on top of `FAILED_SYMMETRIC`:
both peers register with a lightweight rendezvous server and fire
`Stun_HolePunch` simultaneously, adding `DIRECT_P2P_RENDEZVOUS_REGISTER`,
`DIRECT_P2P_RENDEZVOUS_WAIT`, `DIRECT_P2P_BILATERAL_PUNCH`, and
`DIRECT_P2P_FAILED_BILATERAL` states. Kill switch:
`netplay-direct-p2p-disable-bilateral=true` restores the legacy
`FAILED_SYMMETRIC` terminal. See `docs/plan-bilateral-hole-punch.md` and
`docs/direct-p2p-smoke-plan.md`.

## Commit log tail

```
53fc2dfc feat(netplay): STUN mock test harness + on-device smoke plan (Step 12)
75034eed feat(wrapper): exec-arg injection for handoff-armed relaunch (Step 11)
81b853c8 feat(wrapper): OSD Direct-P2P submenu + 6x7 peer-code keyboard (Step 10)
13e8b064 feat(netplay): --direct-p2p-handoff CLI flag + handoff file reader (Step 9)
eee6ed9e feat(netplay): Direct-P2P status overlay (Step 8)
fff34228 feat(netplay): Direct-P2P orchestrator (Step 7)
b6939363 feat(netplay): socket handoff + session teardown callback (Step 6)
0cc09890 feat(config): direct-P2P config keys + defaults (Step 5)
a0512a0f feat(netplay): port UPnP client with miniupnpc cross-compile (Step 4)
4708a65e feat(netplay): port STUN client verbatim from 3sxtra (Step 3)
2b89937a feat(netplay): base32 room-code codec + ISO 7064 check digit (Step 2)
89ec3ef6 feat(netplay): port net_tuning.h + SDL3_net struct-layout gate (Step 1)
b1ad8b16 docs(netplay): tier-3 plan for STUN + UPnP direct-P2P port
23954b90 fix(rmlui): wire per-screen init/update hooks; strip debug traces
```

13 commits on branch `netplay` plus the plan, ~4200 LOC of new code
and docs. All ARM cross-compile builds green (`-DENABLE_NETPLAY=ON`,
`-DENABLE_NETPLAY=OFF`, and `+ENABLE_NETPLAY_TESTS` variants).
Wrapper build green via `tools/mister-wrapper/build-hps.sh`.

Push was deliberately not done — branch stays local on `netplay`
until you review in the morning.
