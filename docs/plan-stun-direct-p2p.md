# STUN + UPnP Direct-P2P — Tier-3 Implementation Plan

Follow-on phase to netplay Phase 6. Adds direct peer-to-peer over the public internet without manual port forwarding.

- **Parent branch:** `netplay`, many Phase 6 commits on top of `mister`.
- **Scope:** UPnP-first, STUN hole-punch fallback, lazy discovery on Host/Join press, base32 room codes, graceful symmetric-NAT failure. No lobby integration, no TURN.
- **Upstream source-of-truth:** `/tmp/3sxtra/` at the Phase 6 pinned commit.
- **Target total:** ~1100–1500 LOC new, ~150–250 LOC modified across 12 steps. XL — 8–12 focused days, excluding external test coordination (two-home-network testing is the tail risk).
- **Biggest risk identified:** SDL3_net struct-layout drift (Step 1, gating prereq) combined with the socket-ownership contract between STUN, hole-punch, UPnP-fallback, and GekkoNet. If any one of those four holders mis-manages the UDP handle, sessions either fail to punch, leak fds, or double-close at teardown. The plan serializes those through `Netplay_SetStunSocket` exactly as upstream does. Secondary risk: wrapper-OSD vs. game-side split for the room-code display/entry widget — the user's "OSD, not in-game RmlUi" decision forces a minor IPC between `MiSTer_3S-ARM` and `3s-arm` that does not exist today.

## Locked user decisions (repeated for each step's context)

1. Direct-P2P only this phase. No lobby STUN.
2. Ship UPnP by default — **UPnP first**, STUN hole-punch as fallback. (This is the intentional inverse of upstream's order, which runs STUN first and falls back to UPnP; see §"Ordering note" below.)
3. Symmetric-NAT failure path: on-screen message, clean session exit, no TURN, no relay infra.
4. Discovery fires on the Host / Join button press, not on menu entry.
5. Host/Join asymmetric: Host → UPnP (or STUN) → display code → socket stays bound listening. Join → enter code → UPnP (or STUN) → punch toward host. Host learns joiner's addr from first inbound datagram.
6. Base32 room codes with checksum digit. Must be human-typeable on a gamepad.
7. No Tailscale mention anywhere — code, docs, commits, release notes.

### Ordering note (upstream vs. our fork)

Upstream `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:1141-1209` runs **STUN-discover → hole-punch → UPnP-on-punch-failure**. The user's locked decision #2 inverts this: **UPnP-first, STUN fallback**. The reasoning is that when UPnP succeeds on both endpoints, no STUN request, no public servers, and no hole-punch packets are required — a strict superset of upstream's reliability for UPnP-enabled routers. When UPnP fails on either side the pipeline degrades to the upstream order. Plan steps encode this ordering in Step 7 (`direct_p2p.c` orchestrator).

## Ground rules for every step

- Sub-agents MUST invoke `/implement` — per `feedback-enforce-skill-invocation.md`. Each step is one full implement → review → fix → verify → commit loop. Review is non-skippable.
- Canonical game build: `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` (Phase 6 Step 1 added `EXTRA_CMAKE_ARGS` pass-through). Do not reintroduce direct `cmake -B build/mister` flows.
- Canonical wrapper build: `tools/mister-wrapper/build-hps.sh` (produces `build/mister-wrapper-hps/MiSTer_3S-ARM`). **Not** `tools/mister/build-game.sh`. Per `feedback-build-terminology.md`, "wrapper" means `MiSTer_3S-ARM`, not `3s-arm`.
- Wrapper source of truth is `vendor/Main_MiSTer/` + `tools/mister-wrapper/main-mister-full-menu.patch` + `tools/mister-wrapper/Makefile.full.3s-arm`. **Never edit `build/mister-wrapper-hps/src/`** (AGENTS.md §Source of Truth). If a step appears to need wrapper changes, the step edits the overlay sources and/or the patch file.
- No `rsync --delete` (per `feedback-no-rsync-delete.md`). Deploy targets per `docs/mister-runbook.md`: RBF → `/media/fat/_Other/3S-ARM.rbf` (NEVER `menu.rbf`); wrapper → `/media/fat/MiSTer_3S-ARM`; game → `/media/fat/games/3s-arm/bin/3s-arm`.
- Before any probe/deploy: `tools/mister/misterctl.sh lock-status && tools/mister/misterctl.sh busy-status`.
- No `--no-verify`, no commit-signing bypass.
- No emojis in code, docs, commit messages.
- No Tailscale references anywhere.
- Cite `file:line` for non-obvious claims in each commit message.

## Architectural decision: where does STUN run?

**Context.** The user's OSD preference conflicts with a process-level concern: the UDP socket that STUN binds must remain bound from discovery through hole-punch through GekkoNet. If STUN runs in the wrapper (`MiSTer_3S-ARM`) and then exec's `3s-arm`, the bound socket is lost at exec. Hole punching cannot tolerate the bind-discover-unbind-rebind cycle — the NAT mapping on the public port only stays alive while traffic keeps flowing. See `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:1164` (`Netplay_SetStunSocket(stun_result.socket); stun_result.socket = NULL;` — socket ownership transferred intact into the game-session code).

**Decision (codified in Step 6, Step 9).** STUN, hole-punch, UPnP port-mapping, and the socket hand-off all run **inside the game binary** (`src/netplay/`). The wrapper OSD's role is strictly:

- Collect user intent (Host vs. Join).
- For Join: accept the 12-character base32 room code (gamepad-navigable widget). *[Superseded — and already inconsistent with Step 2's own 14 at the time of writing. As shipped the code is **18** chars, `XXXXXX-XXXXXX-XXXXXX` (`DP2P_CODE_CHAR_LEN 18` in the wrapper OSK). See `docs/plan-netplay-connection.md` §6.3.]*
- Write a single handoff file (`/tmp/3s-arm-netplay.handoff`, mode 0600) containing one of `mode=host\n` or `mode=join\ncode=AB3F-9K2L-MN7P\n`.
- Re-exec `/media/fat/games/3s-arm/bin/3s-arm` — existing wrapper pathway already forwards its own `argv[2..]`; we add `--direct-p2p-handoff /tmp/3s-arm-netplay.handoff` to the child argv inside the wrapper (see `vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2765-2770`).

The game binary reads the handoff, runs the STUN/UPnP orchestrator (Step 7), and renders progress/code-display using the same 384x224 canvas path `src/port/sdl/netplay_screen.c` already uses (`SSPutStrPro`). This keeps the socket in one address space, honors the user's "OSD for entry, not in-game RmlUi" preference (the Host/Join button + text entry is in the wrapper OSD), and avoids adding miniupnpc / SDL3_net to the wrapper link.

This split is load-bearing for the plan. Steps 10 and 11 implement the wrapper side and game side of the handoff respectively.

---

## Step index (12 steps)

1. **[GATE]** SDL3_net `NET_DatagramSocket` struct-layout verification. Plan halts if this fails.
2. Base32 codec with checksum digit (`src/netplay/room_code.{c,h}`).
3. Port STUN client (`src/netplay/stun.{c,h}` + `src/netplay/net_tuning.h`).
4. Port UPnP client (`src/netplay/upnp.{c,h}`) with miniupnpc cross-compile.
5. Config keys + defaults (`CFG_KEY_NETPLAY_DIRECT_P2P_*`).
6. Netplay.c socket handoff (`Netplay_SetStunSocket` + `configure_gekko` path).
7. Direct-P2P orchestrator (`src/netplay/direct_p2p.{c,h}`) — the state machine that runs UPnP-first → STUN → punch → hand off.
8. Game-side Host/Join in-flight status rendering (`src/port/sdl/netplay_screen.c` extensions).
9. Handoff file reader + CLI arg wiring (`src/args.c`, `src/main.c`).
10. Wrapper OSD Direct-P2P submenu (overlay + patch edits under `vendor/Main_MiSTer/` and `tools/mister-wrapper/`).
11. Wrapper handoff writer + exec-arg injection.
12. Test harnesses (base32 round-trip, STUN mock, UPnP integration) + on-device smoke plan.

### Dependency graph

- Step 1 gates everything else.
- Step 2 is independent of Steps 3–11 (small, isolated).
- Step 3 depends on Step 1.
- Step 4 depends on Step 1 (for the build-deps surface) and is otherwise independent.
- Step 5 is independent.
- Step 6 depends on Step 3 (for the socket type). Not on Step 5 — Step 6 does not read any Direct-P2P config keys.
- Step 7 depends on Steps 2, 3, 4, 5, 6 — this is the integration step.
- Step 8 depends on Step 7 (reads orchestrator status).
- Step 9 depends on Steps 2, 7.
- Step 10 depends on Step 2 (base32 display/entry uses the codec).
- Step 11 depends on Steps 9, 10.
- Step 12 depends on all prior steps.

### Build matrix per step

| Step | `build-game.sh` needed? | `build-hps.sh` needed? | On-device? |
|------|--------------------------|--------------------------|-------------|
| 1 | No (grep + diff) | No | No |
| 2 | Yes | No | No |
| 3 | Yes | No | No |
| 4 | Yes | No | No |
| 5 | Yes | No | No |
| 6 | Yes | No | No |
| 7 | Yes | No | No |
| 8 | Yes | No | No |
| 9 | Yes | No | No |
| 10 | No | Yes | No |
| 11 | No | Yes | No |
| 12 | Yes + Yes | Yes | Yes (final) |

---

## Step 1 — SDL3_net struct-layout verification gate

**Why it matters.** `net_tuning.h` (upstream `/tmp/3sxtra/src/netplay/net_tuning.h:45-56`) casts a `NET_DatagramSocket*` to a `NetTuningDgramMirror*` and reads `num_handles` and `handles[i].handle` to reach the platform socket fd. This is used in two places: (a) `NetTuning_SetRecvBuf` to bump SO_RCVBUF to 256KB, (b) STUN's `getsockname` fallback path to discover the actual OS-assigned local port when the public port differs from the local port. If the upstream SDL3_net struct layout has drifted from what `net_tuning.h` mirrors, both call sites silently misbehave — on a best-case layout mismatch you only get a failed `setsockopt` (ugly but survivable); on a worst-case you read an invalid `num_handles`, step off the end of an array, and blast whatever memory follows. The mirror has not been re-verified since 3sxtra pinned its SDL3_net ref. Our fork's pin is `92022dc` per `build-deps.sh:253`. This step verifies layout before we port any code that depends on it.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/build-deps.sh:249-278` — our SDL3_net pin (`SDL3_NET_REF="92022dc"`).
- `/tmp/3sxtra/src/netplay/net_tuning.h:14-77` — full mirror contract.
- `/tmp/3sxtra/src/netplay/net_tuning.h:45-56` — the partial struct mirror this step validates.
- Upstream SDL3_net source at ref `92022dc`: re-clone to `/tmp/sdl_net_verify` and inspect `src/SDL_net.c:1853-1875`. `build-deps.sh:262-276` builds SDL3_net from a temp clone and then `rm -rf`'s the source tree, so only `third_party/SDL_net/build/include/SDL3_net/SDL_net.h` lives in-tree — the `.c` source is not vendored. This step's verification must therefore clone fresh to `/tmp/sdl_net_verify`. Known content (verified while writing this plan):
  - `typedef struct NET_DatagramSocketHandle { Socket handle; int family; int protocol; } NET_DatagramSocketHandle;`
  - `struct NET_DatagramSocket { NET_SocketType socktype; NET_Address *addr; Uint16 port; int percent_loss; Uint8 recv_buffer[64*1024]; NET_Address *latest_recv_addrs[64]; int latest_recv_addrs_idx; int num_handles; NET_DatagramSocketHandle *handles; NET_DatagramSocketHandle handle_pool[4]; NET_Datagram **pending_output; int pending_output_len; int pending_output_allocation; };`
  - This matches `NetTuningDgramMirror` exactly for the prefix fields the mirror covers. Confirm against the fresh `/tmp/sdl_net_verify` clone.

**Files to create/modify.**
- None yet. This is a verification step — output is a short report in the step's commit message, not a code change.
- If the layout has drifted, patch `src/netplay/net_tuning.h` (a fork of upstream's file — which Step 3 introduces) to match. Do not re-pin SDL3_net; keep `92022dc` stable with Phase 6 link-tested binaries. A mirror patch is strictly safer than a version bump.

**Success criteria.**
- `rm -rf /tmp/sdl_net_verify && git clone https://github.com/libsdl-org/SDL_net.git /tmp/sdl_net_verify && git -C /tmp/sdl_net_verify checkout 92022dc` prints the detached-HEAD banner.
- `grep -n 'struct NET_DatagramSocket' /tmp/sdl_net_verify/src/SDL_net.c` returns `1860:struct NET_DatagramSocket` (line number exactly — drift on line number is OK if field order matches; re-read fields).
- For each field in `NetTuningDgramMirror` (socktype, addr, port, percent_loss, recv_buffer[64*1024], latest_recv_addrs[64], latest_recv_addrs_idx, num_handles, handles), verify presence and order in the upstream struct via visual diff.
- `sizeof(NetTuningSockType)` vs. `sizeof(NET_SocketType)`: both are enum with ≤3 values; on every LP64 and ILP32 platform the compiler reduces these to `int`. The plan treats them as ABI-compatible; the gate treats a mismatch as a hard stop.
- The ported `src/netplay/net_tuning.h` (landed by Step 3) must include a real compile-time size gate: `_Static_assert(sizeof(NetTuningSockType) == sizeof(int), "enum size drift breaks mirror");` — this pattern is already used in `src/netplay/game_state.c:66,73`. Step 1's verdict is informational; the static_assert is the enforceable backstop against future drift.
- Write a commit message that lists each field pair and the verdict (match / differ / size change). No code change required in Step 1 itself if everything matches; the static_assert ships with Step 3.

**Dependencies.** None. Branch state: current `netplay` tip.

**What NOT to do.**
- Do not re-pin SDL3_net to a newer commit to "fix" a drift. That drags in unrelated upstream changes and breaks Phase 6's linkage. Patch the mirror instead.
- Do not copy `net_tuning.h` into our tree yet — Step 3 does that. This step is read-only.
- Do not modify `build-deps.sh`.

**What to do if it fails.**
- If a field type or size differs (e.g., `uint16_t port` became `uint32_t`): update the local mirror (which Step 3 will drop into `src/netplay/net_tuning.h`) with the drifted types before Step 3 lands. Document the divergence in a comment on the mirror.
- If a field has been inserted ahead of `num_handles` or `handles`: the mirror must grow to match, and Step 3 must ship the updated mirror as its first commit.
- If a field was removed: Step 3 is blocked until the feature the missing field served is understood. Escalate.
- If the upstream struct is now fully opaque (no prefix mirror possible): the plan is blocked. Escalate. Options to consider: use `getsockopt(SOL_SOCKET, SO_RCVBUF, ...)` to set SO_RCVBUF via an SDL3_net accessor if one has been added; use `getsockname` without the handle mirror (requires SDL3_net to expose the fd).

---

## Step 2 — Base32 room-code codec

> **Superseded design — the format below never shipped in this shape.**
> This step specifies an 8-byte payload (`ip | public_port |
> local_port`), a 14-char code with no version character and a 17-char
> display form. What actually landed, in three steps: `local_port` was
> dropped before the first release (6-byte payload, **11** chars — see
> `docs/STUN-PORT-STATUS.md`); S4b added a version char and a nonce
> (**14** chars, v2); the S4 adversarial review widened the nonce to 32
> bits (**18** chars, v3: `'3'` + 80-bit `ip(32)|port(16)|nonce(32)` in
> 16 Crockford chars + 1 ISO 7064 MOD 37,36 check digit, displayed
> `XXXXXX-XXXXXX-XXXXXX` = 20 visible). Treat every payload size,
> character count and display form in this step as the original plan of
> record, not as current fact. Current spec:
> `docs/plan-netplay-connection.md` §6.3; the v3 rationale and the
> check-digit defect that forced it are in §6.8 and in the `room_code.h`
> header comment. The base32 alphabet choice (Crockford) and the check
> scheme (ISO 7064 MOD 37,36) *did* survive unchanged — the arithmetic
> for computing the check character did not (§6.8, MEDIUM-1).

**Why it matters.** Upstream's `Stun_EncodeEndpoint` (`/tmp/3sxtra/src/netplay/stun.c:39-43`) emits `"ip|public_port|local_port"` — verbose, error-prone over voice chat, and unsuitable for gamepad entry. The user's locked decision #6 requires a short base32-alphabet, human-typeable code with a checksum. Typical payload: 4 bytes IPv4 + 2 bytes public_port + 2 bytes local_port = 8 bytes = 13 base32 chars; with a checksum digit → 14 chars → displayed as `ABCD-EFGH-IJKL-MN` (three dashes, 17 printable chars total).

**Files to read first.**
- `/tmp/3sxtra/src/netplay/stun.c:39-70` — upstream encode/decode (the "before").
- `/Users/sb/Developer/3sx-mister/src/port/config/config.c` — for the `Config_SetString` / `Config_GetString` pattern (the last-peer code may be persisted so a typo doesn't force full re-entry).
- RFC 4648 §6 (base32 alphabet, padding rules). Use the Crockford variant: 32-char alphabet (`0-9A-HJ-NP-TV-Z`, no I/L/O/U to avoid confusion with 1/0). Crockford is not RFC but widely used and more gamepad-friendly than RFC 4648 base32.
- ISO 7064 MOD 37,36 (Wikipedia: "ISO/IEC 7064") — a native alphanumeric check-character scheme. Unlike Damm-10 (which requires decimal digits and can only check a decimal-digit-bearing code), MOD 37,36 operates directly over Crockford base-32's alphanumeric alphabet. Damm would require either (a) a 32×32 quasigroup table (1024 bytes, and requires a published/designed anti-symmetric quasigroup — not free), or (b) appending a separate decimal check digit to the 13 base-32 chars and decoding each half differently (heterogeneous, fragile). MOD 37,36 is the cleanest fit for a base-32 payload and is used by this plan.

**Files to create/modify.**
- `src/netplay/room_code.h` (new, ~50 LOC):
  - `#define ROOM_CODE_PAYLOAD_BYTES 8` (4B IPv4 + 2B public_port BE + 2B local_port BE).
  - `#define ROOM_CODE_CHAR_LEN 14` (13 base32 chars + 1 check digit).
  - `#define ROOM_CODE_DISPLAY_LEN 17` (14 chars + 3 dashes).
  - Struct `RoomCodePayload { uint32_t ipv4_be; uint16_t public_port; uint16_t local_port; };`.
  - `bool RoomCode_Encode(const RoomCodePayload* in, char out_code[ROOM_CODE_CHAR_LEN + 1]);`
  - `bool RoomCode_Decode(const char* code, RoomCodePayload* out);` — accepts with or without dashes, lowercase or uppercase, tolerates Crockford's loose-alias mappings (I→1, L→1, O→0, U→V).
  - `void RoomCode_Format(const char code[ROOM_CODE_CHAR_LEN + 1], char out_display[ROOM_CODE_DISPLAY_LEN + 1]);` — inserts dashes every 4 chars for display.
- `src/netplay/room_code.c` (new, ~180 LOC):
  - Crockford alphabet table + reverse LUT.
  - ISO 7064 MOD 37,36 check-character implementation (no runtime table — a short loop; if a table speeds it up, size is 37 bytes, far less than a Damm quasigroup).
  - IPv4 + 2× uint16_t → 64-bit → 13 base32 chars → 14-char code (13 payload + 1 MOD 37,36 check character drawn from the Crockford alphabet).
  - Unit-test-only test entry points hidden behind `ENABLE_NETPLAY_TESTS` so `test_room_code.c` can reach internals without making them public.
- `src/netplay/test_room_code.c` (new, ~150 LOC, compiled only when `ENABLE_NETPLAY_TESTS` is defined): round-trip tests for 10k random endpoints, single-character-error detection tests, adjacent-transposition detection tests, case-folding tests, dash-insensitivity tests.
- `CMakeLists.txt`: add the new `.c` files under the existing `ENABLE_NETPLAY` block at `:90-95` (same pattern the Phase 6 `test_*.c` files use).
- `src/configuration.h`: add a `bool test_room_code;` field next to `test_lobby_client_compile` at line ~76.
- `src/args.c`: add `--test-room-code` in the same style as `--test-mist-handshake`.
- `src/main.c`: route `--test-room-code` to a dispatch that calls into the test TU and exits.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" tools/mister/build-game.sh --flavor telemetry` succeeds. The cross-compiled binary lands at `build/mister-telemetry/3s-arm` (per `tools/mister/build-game.sh:157`).
- **The two flags are spelled differently and both are required.** `NETPLAY_TEST_HOOKS` is a genuine CMake `option()` (`CMakeLists.txt:57`, default OFF) and must be passed as `-DNETPLAY_TEST_HOOKS=ON`; `ENABLE_NETPLAY_TESTS` is *not* a CMake option and only reaches the compiler via `CMAKE_C_FLAGS`. Passing `CMAKE_C_FLAGS` alone configures cleanly and then fails the build with ~20 compile errors in the netplay test TUs. Note also that `EXTRA_CMAKE_ARGS` is split on whitespace by `tools/mister/build-game.sh:576-584`, so each token must be whitespace-free — pass the hooks flag as its own `-D`, do not fold it into a quoted `CMAKE_C_FLAGS` string.
- Run the test binary via one of: (a) inside the Docker cross-build container (`docker exec ... build/mister-telemetry/3s-arm --test-room-code`), (b) host build via `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug -DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" && cmake --build build/host --target 3s-arm` followed by `./build/host/3S-ARM.app/Contents/MacOS/3S-ARM --test-room-code` (macOS; on Linux the binary is `build/host/3s-arm`), or (c) `tools/mister/misterctl.sh` deploy + run on the device. The harness must exit 0; its final line is `[test_room_code] OK — all cases passed`. (A cross-compiled ARM `3s-arm` cannot be executed directly on the macOS dev host.)
- `grep -c 'RoomCode_Encode\|RoomCode_Decode' src/netplay/room_code.c` returns at least 2.
- Manual spot-check: 192.168.1.171 + public_port 55123 + local_port 55123 encodes to a 14-char string, decodes back to the same bytes, display form has exactly 3 dashes.

**Dependencies.** Step 1 (gate only — no code-level dependency).

**What NOT to do.**
- Do not use RFC 4648 base32 with `I`/`L`/`O` in the alphabet. Users over voice chat will misread every time.
- Do not use a CRC-16 checksum — ISO 7064 MOD 37,36 catches the errors humans actually make (single-char typos, adjacent transpositions) and operates natively over the base-32 alphabet.
- Do not allocate on the hot path. Encode/decode are stack-only.
- Do not depend on `snprintf` for the encode path (it pulls in locale machinery the binary already has but the test path should be pure).
- Do not introduce IPv6 support. MiSTer kernel has `CONFIG_IPV6 is not set` (`reference-mister-network-stack.md`).

**What to do if it fails.**
- If ISO 7064 MOD 37,36 check-character math is wrong: compare against the published test vector (`ISO7064` Wikipedia reference implementations). The alphabet ordering for the 37-wide modulus must match the ISO table: 0–9, A–Z, then the check-only character `*`. When restricted to the Crockford 32 characters, the missing slots (I/L/O/U) are simply unreachable by encode; decode rejects them as invalid input.
- If round-trip fails for edge endpoints (port 0, port 65535): fix endianness handling. IPv4 is stored big-endian throughout; ports are converted to big-endian before encoding.
- If the test harness hangs: the random seed loop is running forever. Use `SDL_rand_bits()` not `rand()` — Phase 6 Step 7 decision.
- If the code is >14 chars: the payload exceeds 8 bytes. Verify you aren't packing anything else in (no flags byte, no version — room code carries only the endpoint tuple). *[Superseded, and this one inverted: the shipped code deliberately carries **both** a version character and a 32-bit nonce on top of the endpoint tuple (10-byte payload, 18 chars). The nonce is what stops the punch token and rendezvous session key from being derivable by anyone who can guess `(ip, port)` — `docs/plan-netplay-connection.md` §6.3. Do not "fix" a long code by stripping them.]*

---

## Step 3 — Port STUN client (`src/netplay/stun.{c,h}` + `src/netplay/net_tuning.h`)

**Why it matters.** STUN is the fallback path when UPnP is unavailable (the locked decision #2 order is UPnP-first, STUN-second, but the code has to exist either way). Upstream's `stun.c` is 458 LOC of RFC 5389 binding-request + XOR-MAPPED-ADDRESS parsing + hole-punch loop with a 2.5s window, a 200ms send interval, and a hairpin detection that's critical for same-LAN tests. Four hardcoded STUN servers in `stun_servers[]` (`/tmp/3sxtra/src/netplay/stun.c:194-199`): Google, Google (secondary), Cloudflare, Nextcloud. Any one can be unreachable; the client walks the list.

**Files to read first.**
- `/tmp/3sxtra/src/netplay/stun.h:1-48` (full, 48 LOC).
- `/tmp/3sxtra/src/netplay/stun.c:1-458` (full, 458 LOC).
- `/tmp/3sxtra/src/netplay/net_tuning.h:1-77` (full, 77 LOC).
- Step 1 report for any mirror-layout divergences.
- `src/netplay/sdl_net_adapter.c:82-88` for `SDLNetAdapter_Create` — the adapter accepts *any* `NET_DatagramSocket*`, so handing it a STUN-punched socket works as-is.

**Files to create/modify.**
- `src/netplay/stun.h` (new, wholesale copy of `/tmp/3sxtra/src/netplay/stun.h` minus the upstream header guards if they collide). The `Stun_EncodeEndpoint` / `Stun_DecodeEndpoint` functions from upstream are **retained** (we will not call them on the Host/Join path — Step 7 uses `RoomCode_Encode` / `RoomCode_Decode` instead — but leaving upstream's encoders in the file is lower risk than surgical removal, and they compile to dead code in a minimal sense).
- `src/netplay/stun.c` (new, wholesale copy, ~458 LOC). Adjustments on the way in:
  - Drop the `#include "stun.h"` → replace with `#include "netplay/stun.h"` to match our include-style convention (Phase 6 Step 7 set this pattern; confirm at `src/netplay/netplay.c:1-7` or `src/netplay/matchmaking.c:1-5` — `lobby_server.c` is the sole exception and should not be used as the exemplar).
  - Drop the `#ifndef _WIN32 #define _GNU_SOURCE` sentinel only if it conflicts with our build flags. If the existing build already defines `_GNU_SOURCE` globally, leave upstream's guard in; it's harmless.
  - Retain all four STUN server entries. Do NOT reduce to three. If a server is unreachable the client walks on; removing the redundancy makes the failure mode worse.
- `src/netplay/net_tuning.h` (new, copy of `/tmp/3sxtra/src/netplay/net_tuning.h`, plus any Step-1 diverged fields). Upstream's `net_tuning.h:11` has a comment referencing `third_party/sdl3_net/SDL_net/src/SDL_net.c` — that path does not exist in our tree (we only vendor `third_party/SDL_net/build/include/SDL3_net/SDL_net.h`). Update the comment to point at Step 1's verification recipe (`/tmp/sdl_net_verify/src/SDL_net.c` @ ref `92022dc`), or drop the note entirely. Add the `_Static_assert(sizeof(NetTuningSockType) == sizeof(int), "enum size drift breaks mirror");` per Step 1's size-gate requirement.
- `CMakeLists.txt`: add the two new `.c` files to `GAME_SRC` via the existing `ENABLE_NETPLAY` block — they land under `src/netplay/` and are picked up automatically by the existing `aux_source_directory`/glob. Verify by inspection that the Phase 6 `sdl_net_adapter.c` is not individually listed; if it is, list `stun.c` too.
- No other files change. `stun.c` does not call into game code.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `grep -c 'bool Stun_Discover' src/netplay/stun.c` returns 1.
- `grep -c 'stun_servers\[\]' src/netplay/stun.c` returns at least 1 (server list is present).
- `nm build/mister-telemetry/3s-arm | grep -E 'Stun_Discover|Stun_HolePunch|Stun_CloseSocket'` returns 3 T (text-segment) symbols.
- Host desktop build: `cmake -S . -B build/host -DENABLE_NETPLAY=ON && cmake --build build/host --target 3s-arm` also succeeds (Step 3 must keep the host build green for local iteration). On macOS the built binary lands under `build/host/3S-ARM.app/Contents/MacOS/3S-ARM`; on Linux it lands under `build/host/3s-arm`. There is no `build/desktop` tree.

**Dependencies.** Step 1 (for the mirror-layout gate).

**What NOT to do.**
- Do not call `Stun_Discover` from Step 3. This step is a pure port — no call sites. Step 7 wires it in.
- Do not "improve" the hardcoded STUN server list. Adding new servers without operator confirmation is a user-facing change.
- Do not port the `NetTuning_SetRecvBuf` call into `configure_gekko` yet — that also happens in Step 6/7.
- Do not adopt any RFC 8489 (STUN successor) feature (fingerprints, MESSAGE-INTEGRITY, software attribute). Upstream's 5389-only implementation is sufficient; extending it is scope creep.
- Do not port `Stun_EncodeEndpoint` or `Stun_DecodeEndpoint` callsites into our netplay state machine. We ship them as compiled-in dead code (see §Files). Actual encoding is via `RoomCode_Encode` from Step 2.

**What to do if it fails.**
- If `NET_CreateDatagramSocket(NULL, 0)` returns NULL: SDL3_net is not initialized. The Phase 6 init sequence calls `NET_Init()` before matchmaking; ensure Step 7's orchestrator does the same before `Stun_Discover`.
- If the compile fails on `SDL_rand`: the SDL3 version in our tree predates `SDL_rand` (added mid-3.2.x). Substitute with `rand() & 0xff` — this is a transaction ID, cryptographic quality is not required.
- If `inet_ntop` is undeclared on ARM: our glibc target is 2.31+, so `#include <arpa/inet.h>` should be sufficient. Double-check `-std=gnu11` is in effect (not `-std=c11`).
- If a linker error complains about `getaddrinfo` missing: ensure `libc` is linked (it is — this would indicate an SDL_net internal-API conflict where `NET_ResolveHostname` is expected instead). The upstream code path falls back to `NET_ResolveHostname(host)` directly; the `getaddrinfo` preference is a Windows-dual-stack workaround.

---

## Step 4 — Port UPnP client with miniupnpc cross-compile

**Why it matters.** UPnP is the happy-path for the vast majority of home routers (verified for consumer ISP hardware over the last decade). When UPnP works on both peers, **no STUN request is sent, no hole punching is attempted, no public STUN server is touched** — users get a clean, immediate connection. Upstream's `upnp.c` (195 LOC) is a thin wrapper over miniupnpc's discovery → GetValidIGD → AddPortMapping → GetExternalIPAddress. Miniupnpc is widely packaged (`libminiupnpc-dev:armhf` is in Debian bullseye `main`; the container's apt sources at `tools/mister/setup-build-container.sh:133-137` cover bullseye main, bullseye-updates, and bullseye-security).

**Files to read first.**
- `/tmp/3sxtra/src/netplay/upnp.h:1-39` (full).
- `/tmp/3sxtra/src/netplay/upnp.c:1-195` (full).
- `/Users/sb/Developer/3sx-mister/tools/mister/setup-build-container.sh:140-158` — the apt-install section where `libminiupnpc-dev:armhf` lands.
- `/Users/sb/Developer/3sx-mister/build-deps.sh:249-278` — the SDL3_net fetch block, as a template for possibly vendoring miniupnpc if apt doesn't work.
- `/Users/sb/Developer/3sx-mister/docs/mister-runbook.md` for how the Docker cross-build container is constructed.

**Files to create/modify.**
- `src/netplay/upnp.h` (new, wholesale copy of `/tmp/3sxtra/src/netplay/upnp.h`).
- `src/netplay/upnp.c` (new, wholesale copy, ~195 LOC). The `#ifdef HAVE_UPNP` / `#else` split at `upnp.c:11` / `upnp.c:266` is retained — on builds without miniupnpc, all entry points become no-ops returning `false`. This is the degrade-gracefully path per locked decision #3.
- `tools/mister/setup-build-container.sh:157`: extend the apt install list — append `libminiupnpc-dev:armhf`.
- `CMakeLists.txt`:
  - Under the existing `if(ENABLE_NETPLAY)` block at `:296-316`, add `find_package(PkgConfig QUIET)` and then `pkg_check_modules(MINIUPNPC miniupnpc)`. If found, `add_compile_definitions(HAVE_UPNP)` and add the include dirs; link `${MINIUPNPC_LIBRARIES}` inside the `target_link_libraries` block at `:341-351`.
  - If not found, log `message(WARNING "miniupnpc not found — UPnP port forwarding will be unavailable; STUN-only path will be used")`. Set no `HAVE_UPNP` define.
- `tools/mister/build-game.sh` — no change (the container step handles the dep).

**Success criteria.**
- `tools/mister/setup-build-container.sh` runs clean (re-provisioning an existing container is idempotent).
- Inside the container, `dpkg -l | grep libminiupnpc` shows `libminiupnpc-dev:armhf` as installed.
- `pkg-config --cflags --libs miniupnpc` (inside the container, with `PKG_CONFIG_PATH` pointing at armhf libs) returns a `-I.../include` and a `-lminiupnpc`.
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- On a `HAVE_UPNP` build: verify the define is propagated by inspecting the compile invocation or by searching for `HAVE_UPNP` in `build/mister-telemetry/CMakeFiles/3s-arm.dir/flags.make` — a bare `grep -c 'HAVE_UPNP' CMakeCache.txt` is not informative (any grep returns ≥ 0). The authoritative gate is: `nm build/mister-telemetry/3s-arm | grep Upnp_AddMapping` must resolve to the real miniupnpc-backed body (T symbol from `upnp.c`'s `#ifdef HAVE_UPNP` branch), not the no-op stub. On a non-HAVE_UPNP build, `nm build/mister-telemetry/3s-arm | grep -E 'Upnp_AddMapping|Upnp_RemoveMapping|Upnp_GetExternalIP'` still returns 3 T symbols — pointing at the stub bodies.
- Host desktop build (`-DENABLE_NETPLAY=ON` on host): same behavior — if the host has `libminiupnpc-dev`, `HAVE_UPNP` is set; otherwise it falls through to stubs. macOS: Homebrew `miniupnpc` also works.

**Dependencies.** Step 1 (gate).

**What NOT to do.**
- Do not vendor miniupnpc into `third_party/` unless apt-based install fails on the cross-build container. The vendor path costs 300+ LOC of `build-deps.sh` changes and adds a second third-party cross-compile target. Only reach for it as a fallback (see "What to do if it fails" below).
- Do not add `miniupnpc` as a dep of the wrapper (`MiSTer_3S-ARM`). UPnP runs in the game, not the wrapper.
- Do not remove the `#ifdef HAVE_UPNP` / `#else` guard — degrading gracefully to the no-op path is the user's locked decision #3 when UPnP is unavailable (router disabled it, package missing, etc.).
- Do not hardcode the miniupnpc API version. `upnp.c:21-24` already handles `MINIUPNPC_API_VERSION` drift; preserve that.
- Do not alter the UPNP_AddPortMapping description string — `"3SX Netplay"` is what shows up in router admin UIs. We keep the string for user debuggability.

**What to do if it fails.**
- If `libminiupnpc-dev:armhf` is unavailable in bullseye main / bullseye-updates / bullseye-security (it is, as of plan date, but package state changes): fall back to vendoring. Add a new block to `build-deps.sh` mirroring the SDL3_net block (`:249-278`): clone `https://github.com/miniupnp/miniupnp`, pin to a stable tag (v2.2.8+), cross-compile the `miniupnpc/` subdirectory to a static lib, install to `third_party/miniupnpc/build/`. CMake picks up `third_party/miniupnpc/build/include/miniupnpc` then.
- If the pkg-config probe returns a path that includes `/usr/lib/x86_64-linux-gnu/`: `PKG_CONFIG_PATH` isn't set for armhf in the container. Prepend `/usr/lib/arm-linux-gnueabihf/pkgconfig/` to `PKG_CONFIG_PATH` in `tools/mister/build-game.sh`'s cmake invocation.
- If link fails with `undefined reference to UPNP_GetValidIGD`: the installed miniupnpc is older than API v18 but the `#if MINIUPNPC_API_VERSION >= 18` block is being taken. Check the package version; if <v2.2.8, force the pre-v18 call signature by `#define MINIUPNPC_API_VERSION 17` before including.

---

## Step 5 — Config keys + defaults for Direct-P2P

**Why it matters.** Several parameters need persistence: (a) the UDP port the host wants to bind for Direct-P2P (if user wants a stable port for their router's UPnP history; `0` means OS-assigned), (b) the last-used peer code (for quick rejoin on "Match lost, rejoin" kind of flow — not MVP but header-reserved), (c) a toggle to disable UPnP attempts (some routers misbehave and fail slowly, users should be able to skip straight to STUN). The Phase 6 config plumbing (`src/port/config/config.c`) already supports string/int/bool; this step extends it, nothing new.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/src/port/config/config.h:30-35` — existing netplay keys set in Phase 6 Step 7.
- `/Users/sb/Developer/3sx-mister/src/port/config/config.c` — `default_entries` table pattern and `Config_SetString` implementation.
- `/Users/sb/Developer/3sx-mister/docs/config.md` — for user-facing docs convention.

**Files to create/modify.**
- `src/port/config/config.h`: add
  - `#define CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT "netplay-direct-p2p-host-port"` (int, default 0 = OS-assigned).
  - `#define CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP "netplay-direct-p2p-disable-upnp"` (bool, default false).
  - `#define CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE "netplay-direct-p2p-last-peer-code"` (string, no default — populated at runtime by Step 7 on successful Join).
  - Two more keys reserved for wrapper-handoff plumbing and orchestrator tuning: `#define CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH "netplay-direct-p2p-handoff-path"` (string, default `"/tmp/3s-arm-netplay.handoff"`) — **read in Step 9** (`load_direct_p2p_handoff` uses this as the default fallback when `--direct-p2p-handoff` is omitted but the config sets a non-default path). And `#define CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS "netplay-direct-p2p-stun-timeout-ms"` (int, default 4000) — **read in Step 7** (orchestrator clamps its STUN discovery timeout to this value; upstream default is 2000 ms, but a congested public STUN server sometimes needs more). If either key is not wired in its consuming step, remove the define in Step 5 rather than leaving orphaned keys.
- `src/port/config/config.c`: extend `default_entries[]` with the two default-bearing keys. The other three are populated via `Config_SetString` at runtime.
- `docs/config.md`: document the new keys in the existing "Netplay" section. Note that `netplay-direct-p2p-host-port` is advisory — if the port is already bound, the OS falls back to ephemeral and the public port is whatever the NAT maps it to.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `grep -cE '^#define CFG_KEY_NETPLAY_DIRECT_P2P' src/port/config/config.h` returns exactly 5 (anchored to `#define` so a header-comment mention of the family does not inflate the count).
- `grep -c 'CFG_KEY_NETPLAY_DIRECT_P2P' src/port/config/config.c` returns at least 2 (default entries) — the runtime-set keys are mentioned in code comments, not in `default_entries`.
- `grep -c 'netplay-direct-p2p' docs/config.md` returns at least 5.

**Dependencies.** None beyond the base branch.

**What NOT to do.**
- Do not add a "STUN server list override" config key. The hardcoded four servers are correct; users who want to override them can modify `src/netplay/stun.c:194-199` and rebuild. This is the same policy as 3sxtra and we are not adding operator surface area.
- Do not add a "TURN relay server" key — locked decision #3 is no TURN.
- Do not share a config key with the wrapper. The wrapper reads `MiSTer.ini`, the game reads our own config file in the user's pref-path. Step 11's handoff file is the interop.

**What to do if it fails.**
- If `Config_Save` is the no-op stub it is in Phase 6 (config.h:68-72): the last-peer-code key won't persist across runs. That's acceptable for this phase — the wrapper-OSD Join widget re-prompts on each session; the key is only used within a running game instance.

---

## Step 6 — Netplay.c socket handoff (`Netplay_SetStunSocket` + `configure_gekko`)

**Why it matters.** This is the load-bearing seam between the STUN/punch orchestrator and GekkoNet. Upstream stores a single static `NET_DatagramSocket* stun_socket = NULL;` (`/tmp/3sxtra/src/netplay/netplay.c:79`) and has `configure_gekko` branch between (a) reusing that socket, (b) creating a fresh fallback socket, (c) using GekkoNet's default adapter. Our fork's `configure_gekko` at `src/netplay/netplay.c:465-520` already has a branch for "matchmaking socket vs. direct-P2P socket" — this step extends it to add a third branch for "pre-punched STUN socket passed in by Step 7's orchestrator". The contract matches upstream exactly: caller transfers ownership to netplay.c, netplay.c destroys on session teardown.

**Files to read first.**
- `/tmp/3sxtra/src/netplay/netplay.c:79-80` — `stun_socket` / `fallback_socket` statics.
- `/tmp/3sxtra/src/netplay/netplay.c:422-494` — `configure_gekko` in full.
- `/tmp/3sxtra/src/netplay/netplay.c:808-814` — `Netplay_SetStunSocket`.
- `/tmp/3sxtra/src/netplay/netplay.c:987-1019` — session teardown, including STUN socket destroy.
- `src/netplay/netplay.c:61` — our current `static NET_DatagramSocket* p2p_sock = NULL;`.
- `src/netplay/netplay.c:465-520` — our `configure_gekko` before.
- `src/netplay/netplay.c:880-898` — our session teardown before.
- `src/netplay/netplay.c:843-871` — the `NETPLAY_SESSION_TRANSITIONING` → `CONNECTING` seam; MIST handshake lives there (`:856`). Direct-P2P is lobby-less so MIST is skipped on this path (gated on `s_lobby_session`).
- `src/netplay/netplay.h:20-29` — public API before.

**Files to create/modify.**
- `src/netplay/netplay.h`: declare `void Netplay_SetStunSocket(struct NET_DatagramSocket* socket);` near the existing `Netplay_BeginDirectP2P` / `Netplay_SetMatchmakingParams` declarations. Forward-declare `struct NET_DatagramSocket;` in the header — do NOT include `<SDL3_net/SDL_net.h>` from netplay.h (that pulls SDL3_net into every TU that only wants the event queue).
- `src/netplay/netplay.c`:
  - Add `static NET_DatagramSocket* stun_socket = NULL;` next to `p2p_sock` at `:61` (keep both — `p2p_sock` is the legacy no-discovery LAN path that Step 9 of Phase 6 shipped; it stays).
  - Implement `Netplay_SetStunSocket` bodily at the tail of the file (near `Netplay_SetLobbySession`), mirroring upstream's guard: "if we already hold a STUN socket, close it first".
  - Modify `configure_gekko`: change the branch at `:485-495` to prefer, in order, (1) `stun_socket` if non-NULL, (2) `Matchmaking_GetSocket()` if non-NULL, (3) fallback to creating `p2p_sock`. When using `stun_socket`, also call `NetTuning_SetRecvBuf(stun_socket, 256 * 1024)` mirroring upstream `/tmp/3sxtra/src/netplay/netplay.c:440-444`.
  - Modify session teardown at `:886-890`: destroy whichever of `stun_socket`, `p2p_sock`, `fallback_socket` (if any) is non-NULL. Set all back to NULL.
  - Add `#include "netplay/net_tuning.h"` alongside the existing SDL3_net include at line ~31.
- `src/netplay/netplay_stub.c`: add a no-op `Netplay_SetStunSocket` stub so the `ENABLE_NETPLAY=OFF` build still links. Mirror the pattern of the other stubs there.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=OFF" tools/mister/build-game.sh --flavor telemetry` succeeds (the stub path still links).
- `grep -c 'Netplay_SetStunSocket' src/netplay/netplay.c src/netplay/netplay.h src/netplay/netplay_stub.c` returns at least 3.
- `grep -c 'stun_socket' src/netplay/netplay.c` returns at least 4 (declaration, `configure_gekko` branch, `Netplay_SetStunSocket` body, teardown).
- Running the existing Phase 6 MIST-handshake regression still passes. Invoke via (a) the Docker container's `build/mister-telemetry/3s-arm --test-mist-handshake`, (b) the host desktop build (`build/host/3S-ARM.app/Contents/MacOS/3S-ARM --test-mist-handshake` on macOS; `build/host/3s-arm --test-mist-handshake` on Linux), or (c) `tools/mister/misterctl.sh` deploy + run on the device. Same for `--test-netplay-event-queue`. (A cross-compiled ARM `3s-arm` cannot run directly on a macOS dev host.)

**Dependencies.** Step 3 (for `NET_DatagramSocket` type). Step 5 is NOT a dependency here: Step 6 does not read any of the Direct-P2P config keys — those are consumed by Step 7 (STUN timeout) and Step 9 (handoff path). The socket-ownership seam in this step is orthogonal to configuration.

**What NOT to do.**
- Do not destroy `stun_socket` in the middle of `run_netplay()` — ownership is transferred into `configure_gekko` which hands the socket to `SDLNetAdapter`. Only teardown destroys it.
- Do not call `NET_Init()` from `Netplay_SetStunSocket`. The caller (Step 7 orchestrator) must have done `NET_Init()` before running STUN.
- Do not modify the MIST handshake gate at `:856`. Direct-P2P sessions never set `s_lobby_session = true`, so the gate naturally skips MIST. This is correct per the task spec.
- Do not add a `Netplay_SetUpnpMapping` API in this step. The UPnP mapping lifecycle is orchestrator-owned (Step 7) — netplay.c only sees the socket, not the mapping metadata.
- Do not change the call order of `SDLNetAdapter_Destroy()` at `:883`. That already runs before any socket destruction, releasing cached DNS refs first.

**What to do if it fails.**
- If `configure_gekko` compiles but `SDLNetAdapter_Create` is called with NULL on the STUN path: `stun_socket` was set to NULL between the branch check and the adapter creation. Add a defensive assert and re-order.
- If teardown double-frees: two sources are destroying the same socket. Check that Step 7's orchestrator calls `Netplay_SetStunSocket(NULL)` or stops touching the socket after transfer.
- If `NetTuning_SetRecvBuf` crashes at runtime: the Step 1 mirror-verification was incomplete. Re-run Step 1's check and inspect actual runtime layout with `gdb` on the ARM target.

---

## Step 7 — Direct-P2P orchestrator (`src/netplay/direct_p2p.{c,h}`)

**Why it matters.** This is the state machine that runs the user-facing flow end-to-end. It replaces upstream's `sdl_netplay_ui.cpp:696-749` (which is Lobby-UI-bound and pulls in RmlUi state) with a self-contained headless orchestrator driven entirely from `Netplay_Run` / the main-loop pump. It owns: (a) Host/Join role selection, (b) UPnP-first attempt with timeout, (c) STUN-discover on UPnP failure, (d) code encode/decode and hand-off to Step 8 for display, (e) hairpin detection, (f) hole-punch, (g) socket-ownership transfer to `Netplay_SetStunSocket`, (h) `Netplay_BeginDirectP2P` call, (i) UPnP mapping release on session teardown.

**Files to read first.**
- `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:696-749` — upstream's STUN/punch/UPnP thread functions (pattern reference).
- `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:711-726` — hairpin detection logic (exact port).
- `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:1141-1209` — punch-done handler, including the dual-stack-socket bypass at `:1155-1162` and the STUN-socket ownership transfer at `:1164-1165` (the most important five lines of the entire upstream direct-P2P pathway — must be replicated).
- `/tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:1213-1240` — UPnP-done handler.
- `src/netplay/netplay.c:770-784` — our `Netplay_TickDirectP2P` (the current path from menu press to session start).
- `src/netplay/stun.h`, `src/netplay/upnp.h` (new from Steps 3, 4).
- `src/netplay/room_code.h` (new from Step 2).

**Files to create/modify.**
- `src/netplay/direct_p2p.h` (new, ~70 LOC):
  - Enum `DirectP2PState { DIRECT_P2P_IDLE, DIRECT_P2P_UPNP_PROBING, DIRECT_P2P_UPNP_SUCCESS, DIRECT_P2P_STUN_DISCOVERING, DIRECT_P2P_AWAITING_PEER_CODE, DIRECT_P2P_PUNCHING, DIRECT_P2P_HANDING_OFF, DIRECT_P2P_FAILED_SYMMETRIC_NAT, DIRECT_P2P_FAILED_OTHER };`
  - `void DirectP2P_BeginHost(void);` — starts the "Host" flow (discover, compute room code, wait for first inbound datagram).
  - `void DirectP2P_BeginJoin(const char* room_code_14);` — starts the "Join" flow (discover, decode code, punch toward host).
  - `void DirectP2P_Cancel(void);` — user-cancel; tears down in-flight threads and any UPnP mapping.
  - `void DirectP2P_Tick(void);` — called once per frame from `Netplay_Run` or `main.c` similar to how Phase 6 `Netplay_TickMatchmaking` works.
  - `DirectP2PState DirectP2P_GetState(void);` — for the status-render path (Step 8).
  - `const char* DirectP2P_GetStatusMessage(void);` — short user-facing string for the status overlay.
  - `bool DirectP2P_GetHostRoomCode(char out_code[ROOM_CODE_DISPLAY_LEN + 1]);` — populated once `DIRECT_P2P_STUN_DISCOVERING` transitions to `DIRECT_P2P_AWAITING_PEER_CODE` (Host side).
- `src/netplay/direct_p2p.c` (new, ~450 LOC):
  - State-machine implementation.
  - Worker thread (SDL_CreateThread) for UPnP probe → STUN discover → punch. Keep all SDL3_net calls on the worker thread; main thread only reads `SDL_AtomicInt` state markers. This mirrors upstream's `lobby_thread` pattern.
  - Cancel flag (`SDL_AtomicInt`) passed into `Stun_HolePunch`.
  - Hairpin detection: compare discovered `public_ip` against peer's `public_ip`; if equal, rewrite peer to `127.0.0.1` and use peer's `local_port` — exactly upstream's `sdl_netplay_ui.cpp:711-726`. The game binary running on a single MiSTer with a phone on the same LAN behind the same public IP will trigger this path.
  - Dual-stack bypass on hairpin: if peer rewrote to `127.0.0.1`, destroy the STUN socket (so `Netplay_BeginDirectP2P` creates a fresh bound socket on the local_port) — upstream `sdl_netplay_ui.cpp:1155-1162`. This is defensive cross-platform hygiene for host desktop builds where IPv4/IPv6 dual-stack binds divergent ports and localhost routing fails. On MiSTer this is not strictly required (kernel has `CONFIG_IPV6 is not set` per `reference-mister-network-stack.md`), but we keep the bypass so the same path compiles and runs identically across host and MiSTer.
  - On success, calls `Netplay_SetRemoteIP`, `Netplay_SetRemotePort`, `Netplay_SetLocalPort`, `Netplay_SetStunSocket`, `Netplay_SetPlayerNumber` (Host = 0, Join = 1, per upstream `:1197`), clears `stun_result.socket = NULL` to prevent double-close, and calls `Netplay_BeginDirectP2P`.
  - On UPnP success: do NOT run STUN. Build the room code from the LAN addr + UPnP mapping's external_port + internal_port. Publish.
  - On UPnP failure: fall through to STUN.
  - On STUN failure after N retries (N=2, like upstream `STUN_RETRY_INTERVAL_MS` pattern): transition to `DIRECT_P2P_FAILED_OTHER`.
  - On punch failure after the 2.5s window: if we reached this through UPnP-failure-then-STUN, the user is behind what may be Symmetric NAT. Transition to `DIRECT_P2P_FAILED_SYMMETRIC_NAT`. Status message: `"Cannot connect through your router. Try enabling UPnP or use a Lobby (Phase 6)."` Render the failure string for 4 seconds, then call `Soft_Reset_Sub()` to return to the title screen (reuses the Phase 6 netplay-teardown exit path; the wrapper OSD is not involved — the game handles the visual recovery in-process).
  - UPnP mapping is released on session teardown via a new callback API. Add `void Netplay_SetSessionTeardownCallback(void (*cb)(void));` to `src/netplay/netplay.h` (~10 LOC new in `netplay.c`: store the fn ptr in a static, invoke it from the existing teardown path near `src/netplay/netplay.c:886-890` alongside the socket destroy). Step 7's orchestrator registers `Upnp_RemoveMapping`-wrapping callback once at init. Polling `GetSessionState` from a free-running tick is the alternative but leaves a race where teardown finishes before the poll observes the transition; the callback path is cleaner.
- `src/main.c:476-477`: add `DirectP2P_Tick();` alongside the existing `Netplay_TickMatchmaking();` and `Netplay_TickDirectP2P();` calls. The orchestrator's frame-pumped state machine runs from the same game-loop location as the other netplay ticks. Do NOT add the tick call from inside `Netplay_Run` — Phase 6 ticks all run from `main.c`'s frame dispatcher, not from within `Netplay_Run`.
- `CMakeLists.txt`: `direct_p2p.c` is picked up by the existing `src/netplay/*.c` glob.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `grep -c 'DirectP2P_Begin\|DirectP2P_Tick\|DirectP2P_GetState' src/netplay/direct_p2p.h` returns at least 4.
- `nm build/mister-telemetry/3s-arm | grep -E 'DirectP2P_BeginHost|DirectP2P_BeginJoin|DirectP2P_Tick'` returns 3 T symbols.
- `grep -c 'DirectP2P_Tick' src/main.c` returns at least 1 (the frame-dispatch call site at lines 476-477).
- Host desktop build: start the binary with a special `--direct-p2p-force-hairpin-test` flag that writes a fake UPnP result of `127.0.0.1:55000/55000` and calls `DirectP2P_BeginJoin("ABCDEFGHJKMN1")`; harness must reach `DIRECT_P2P_HANDING_OFF` within 5 seconds. Invoke via `build/host/3S-ARM.app/Contents/MacOS/3S-ARM` on macOS or `build/host/3s-arm` on Linux. (Harness is a Step 12 artifact; for Step 7 verification, the flag stub is sufficient.)
- `nm build/mister-telemetry/3s-arm | grep Upnp_RemoveMapping` returns 1 T symbol (orchestrator linked the remover).

**Dependencies.** Steps 2, 3, 4, 5, 6.

**What NOT to do.**
- Do not block `DirectP2P_Tick` on thread work — it must return within one frame. Long-running STUN / UPnP calls go on the worker thread; Tick only inspects atomics.
- Do not integrate with the lobby — locked decision #1. This module is lobby-unaware. Any future lobby integration happens in a follow-on phase.
- Do not start a TURN relay fallback — locked decision #3. Symmetric-NAT failure is final, user-visible, clean-exit.
- Do not reuse `p2p_sock` from `configure_gekko`. The STUN socket and the legacy-LAN `p2p_sock` are distinct — `p2p_sock` stays the fallback for users who use CLI args directly (no STUN, no discovery, just a hardcoded peer IP). Direct-P2P through the OSD always takes the `stun_socket` path.
- Do not call `SDL_Log` at frame rate — orchestrator status goes through `DirectP2P_GetStatusMessage` for the overlay, not the log. Only log on state transitions.
- Do not alter the STUN server list. Editing `stun.c` in this step is out of scope.

**What to do if it fails.**
- If UPnP-first appears to succeed on a router that's actually lying about creating the mapping: the STUN fallback will eventually trigger through the punch-failure path. To verify UPnP-actually-working, after `Upnp_AddMapping` returns true, immediately poll `UPNP_GetSpecificPortMappingEntry` via miniupnpc to confirm the router lists the mapping we just requested (same external_port, same protocol, our internal IP). If it returns `UPNPCOMMAND_SUCCESS` and the internal-client matches, treat the mapping as valid. If the entry is not listed or the internal-client differs, treat as UPnP failure and fall through to STUN. Do NOT attempt a self-ping from the LAN to our own external IP — that requires the router to support NAT hairpinning, which many consumer routers explicitly do not.
- If hairpin rewrite doesn't resolve connectivity on same-LAN test: the NAT loopback config on the router actually supports hairpin (unusual, but some consumer routers do this). In that case skip the rewrite — log at debug level and let the punch proceed to the public IP. The test harness in Step 12 must cover both variants.
- If the worker thread leaks on `DirectP2P_Cancel`: the thread must observe the `cancel_flag` and return. Make sure `Stun_HolePunch` is the only potentially-long blocking call; STUN discovery has its own 2s timeout baked in (`stun.c:281-284`), UPnP discovery has its own 2s cap (`upnp.c:26`).

---

## Step 8 — Game-side status overlay for Host/Join

**Why it matters.** Once the wrapper OSD closes and the game binary starts, the user needs to see something while STUN/UPnP run. "STUN-in-progress", "Your code is: AB3F-9K2L-MN7P", "Waiting for peer...", "Connecting (success)", "Cannot connect (symmetric NAT)" — these must all render in the ~2–5 second window between `3s-arm` startup and `Netplay_BeginDirectP2P`. The existing `src/port/sdl/netplay_screen.c:35-76` matchmaking-status pattern is exactly right for this — `SSPutStrPro` into the 384x224 canvas that native_video_writer blits. No RmlUi dependency.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/src/port/sdl/netplay_screen.c:1-76` — full.
- `/Users/sb/Developer/3sx-mister/src/port/sdl/netplay_screen.h:1-7`.
- `src/netplay/direct_p2p.h` (new from Step 7).
- `feedback-rmlui-render-target.md` — the render target must be the 384x224 canvas the native video writer blits, not `SDL_GetRenderer(window)`. `SSPutStrPro` targets the game canvas directly; using it is correct.
- `feedback-fbdev-not-used.md` — do NOT consider fbdev. The overlay is the same 384x224 SSPutStrPro path the rest of the game uses.
- `src/sf33rd/Source/Game/ui/sc_sub.h` — for the `SSPutStrPro` API (existing, used by netplay_screen.c).

**Files to create/modify.**
- `src/port/sdl/netplay_screen.c`:
  - Extend `NetplayScreen_Render` to inspect `DirectP2P_GetState()` when `Netplay_GetSessionState() == NETPLAY_SESSION_IDLE`. If the orchestrator is running, render its state/message/code. Render order:
    - If state is `DIRECT_P2P_UPNP_PROBING`: show "Opening port forward..." at top.
    - If `DIRECT_P2P_STUN_DISCOVERING`: show "Discovering public endpoint..." at top.
    - If `DIRECT_P2P_AWAITING_PEER_CODE` (Host): show "Room Code:" on the label line, the formatted 17-char code (`AB3F-9K2L-MN7P-XY`) centered on a second line. `SSPutStrPro` (`src/sf33rd/Source/Game/ui/sc_sub.h:44`) has no scale parameter — its signature is `s32 SSPutStrPro(u16 flag, u16 x, u16 y, u8 atr, u32 vtxcol, const char* str);` where `atr` is a palette bank, not a scale. Use single-size text and center horizontally on the 384-wide canvas (text width ≈ 8 px per character = 136 px for 17 chars; `x = (384 - 136) / 2 = 124`). `SSPutStr_Bigger` accepts an `f32 sc` parameter but its call path is not what netplay_screen.c currently uses; introducing it here is scope creep. *[Superseded twice. The code is **20 visible** chars now (`XXXXXX-XXXXXX-XXXXXX`, v3), not 17, so the hand-computed `x = 124` would be wrong; and the as-built overlay does not hand-compute `x` at all — `src/netplay/direct_p2p_overlay.c` calls `SSPutStrProP(1, DP2P_OVL_CANVAS_W /* 384 */, ...)`, which self-centers the string within `[0, width]`. The layout constants live in that file, not in `netplay_screen.c`.]*
    - If `DIRECT_P2P_PUNCHING`: show "Connecting to peer..." at top.
    - If `DIRECT_P2P_HANDING_OFF`: show "Connected!" briefly.
    - If `DIRECT_P2P_FAILED_SYMMETRIC_NAT`: show "Cannot connect through your router." and below "Enable UPnP or use Lobby." — hold for 4 seconds then trigger a clean exit (return to the title).
    - If `DIRECT_P2P_FAILED_OTHER`: show "Network discovery failed." and below "Try again or check your internet." — same 4s hold.
  - All `SSPutStrPro` calls use white (`0xFFFFFFFF`) on the existing canvas.
- `src/port/sdl/netplay_screen.h`: no API change. `NetplayScreen_Render` continues to be the single entry point.
- `src/port/sdl/sdl_app.c`: no change — `NetplayScreen_Render` is already called from the main render loop (per Phase 6 Step 11/12 wiring). Verify with a grep.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `grep -c 'DirectP2P_GetState\|DirectP2P_GetStatusMessage' src/port/sdl/netplay_screen.c` returns at least 2.
- Host desktop build: stub the Direct-P2P state via a dev flag (`--direct-p2p-mock-state=UPNP_PROBING`) and verify `NetplayScreen_Render` draws the overlay in the host canvas. Invoke via `build/host/3S-ARM.app/Contents/MacOS/3S-ARM` on macOS or `build/host/3s-arm` on Linux.
- `grep -n 'NetplayScreen_Render' src/port/sdl/sdl_app.c` still returns at least 1 line (the call site is intact from Phase 6).

**Dependencies.** Step 7.

**What NOT to do.**
- Do not target `SDL_GetRenderer(window)` for the overlay — per `feedback-rmlui-render-target.md`. Use `SSPutStrPro`.
- Do not bring up RmlUi for this overlay — locked on "OSD, not in-game RmlUi".
- Do not make the overlay modal. The user should be able to press the wrapper OSD hotkey during any state to cancel. The orchestrator's `DirectP2P_Cancel` handles this, and netplay_screen.c only renders.
- Do not blink the text. Fighting-game community reads static text fast enough; animation adds CPU and interrupts the frame budget we spent Phase 1–4 lowering.

**What to do if it fails.**
- If the formatted 17-character code runs off the right edge at x=384: measure the string width at runtime (SSPutStrPro returns width on the `s32` return path) and adjust `x` accordingly, or drop the dashes on the screen display and let users reconstruct (mild UX degradation). *[Superseded: the display form is now 20 visible characters (v3), and the as-built overlay lets `SSPutStrProP` centre within the 384-wide canvas rather than computing an `x`. The v3 bump added 4 payload chars and one dash group — worth re-checking against the CRT if this ever regresses.]*
- If the state machine flashes through a state too fast to read: add a minimum 500 ms hold timer on each state in the orchestrator (Step 7) rather than in the renderer.
- If native_video_writer doesn't pick up the overlay when main menu is visible: `NetplayScreen_Render` is called after the menu draw in sdl_app.c. Move the call site after the menu render block.

---

## Step 9 — Handoff file reader + CLI arg wiring

**Why it matters.** The wrapper OSD writes a handoff file, re-execs the game, and the game needs to (a) detect the flag, (b) parse the file, (c) call `DirectP2P_BeginHost` or `DirectP2P_BeginJoin` before the normal menu loop. Everything after the parse is Step 7's problem. The parse is a one-shot on startup, and the flag is `--direct-p2p-handoff /path/to/file`.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/src/args.c:178-195` — existing `OPT_STRING` pattern for `--p2p-remote-ip`, `--matchmaking-ip`. New flag follows the same style.
- `/Users/sb/Developer/3sx-mister/src/main.c:184-192` — existing `set_netplay_params`, which is where the dispatch to `Netplay_SetParams`/`Netplay_SetMatchmakingParams` happens today.
- `/Users/sb/Developer/3sx-mister/src/configuration.h:8-14` — `NetplayConfiguration` struct.
- `src/netplay/room_code.h` (new from Step 2) for decoding.

**Files to create/modify.**
- `src/configuration.h`: add `const char* direct_p2p_handoff_path;` to `NetplayConfiguration` (at line ~13). Add a sibling `enum { DIRECT_P2P_MODE_NONE = 0, DIRECT_P2P_MODE_HOST, DIRECT_P2P_MODE_JOIN } direct_p2p_mode;` and `char direct_p2p_peer_code[ROOM_CODE_CHAR_LEN + 1];` — populated by the reader in `main.c`, not directly by argparse.
- `src/args.c`:
  - Add `OPT_STRING(0, "direct-p2p-handoff", &configuration->netplay.direct_p2p_handoff_path, ...)` in the option list near the existing `p2p-remote-ip` entry (~line 181).
  - Extend the mutual-exclusion check at `:138-139` to reject combinations of `--direct-p2p-handoff` with the other two modes.
- `src/main.c`:
  - New static function `load_direct_p2p_handoff(const char* path, NetplayConfiguration* netplay)`: opens the file, reads up to 256 bytes, parses line-by-line looking for `mode=host`, `mode=join`, `code=AB3F-9K2L-MN7P` (the `code=` line is only valid with `mode=join`). Validates via `RoomCode_Decode` — if the checksum fails, print an error and exit non-zero. Unlink the file on success (one-shot hand-off, prevents stale data on subsequent launches).
  - Extend `set_netplay_params` to: if `configuration.netplay.direct_p2p_handoff_path != NULL`, call `load_direct_p2p_handoff`, then dispatch `DirectP2P_BeginHost()` or `DirectP2P_BeginJoin(netplay.direct_p2p_peer_code)` accordingly.
- `src/netplay/netplay.h`: no change (DirectP2P API is in `direct_p2p.h`).

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`.
- `grep -c 'direct-p2p-handoff' src/args.c` returns at least 1.
- `grep -c 'load_direct_p2p_handoff' src/main.c` returns at least 2.
- Host desktop: let `${HOST_BIN}` be `build/host/3S-ARM.app/Contents/MacOS/3S-ARM` on macOS or `build/host/3s-arm` on Linux. Then `printf 'mode=host\n' > /tmp/handoff.txt && "${HOST_BIN}" --direct-p2p-handoff /tmp/handoff.txt` starts, orchestrator transitions to `DIRECT_P2P_UPNP_PROBING`, and `/tmp/handoff.txt` is gone after startup. Graceful exit after the 4s timeout in the degenerate "no router" case.
- Invalid code: `printf 'mode=join\ncode=INVALID\n' > /tmp/handoff.txt && "${HOST_BIN}" --direct-p2p-handoff /tmp/handoff.txt` exits 1 with `[direct_p2p_handoff] invalid peer code` in stderr.

**Dependencies.** Steps 2, 7.

**What NOT to do.**
- Do not parse anything beyond `mode=` and `code=` keys in v1 of the handoff format. The wrapper writes exactly those. Extending the format schema now is speculative.
- Do not leave the handoff file on disk after a successful read. Unlink it — if the user relaunches without the wrapper OSD step, stale code would trigger bad behavior.
- Do not read the handoff from `/media/fat/` — it's a one-shot transient, `/tmp` is correct. The MiSTer root filesystem is read-only except for specific paths (`/tmp` is tmpfs, safe).
- Do not add a `--direct-p2p-mode=host` / `--direct-p2p-mode=join` / `--direct-p2p-peer-code=XYZ` trio of flags as an "easier" alternative. Rationale for file-based handoff over CLI args: (a) **one-shot semantics** — `unlink` after read guarantees a fresh code on every Host/Join press; a flag on argv persists for the full child lifetime and leaks via `/proc/<pid>/cmdline`, (b) **security** — the peer code is tmpfs-local at mode 0600 rather than world-visible in process listings, (c) **wrapper simplicity** — the wrapper already forwards `argv[2..]` unmodified at `vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2765-2770`; the handoff flag injects exactly one pair (`--direct-p2p-handoff /tmp/...`), leaving user-supplied argv intact. Test-only CLI flags under `ENABLE_NETPLAY_TESTS` still use direct argv (see Step 12).
- Do not error if the handoff file is missing. Print a warning and fall through to the normal menu — the wrapper may have failed to write it, and the user should still be able to enter the game.

**What to do if it fails.**
- If `load_direct_p2p_handoff` reads the file but the mode line is garbled: treat as the "file missing" case — warn, ignore, continue to title. Do NOT force an exit; the user can still use the wrapper OSD to retry.
- If there's a race where the game reads the file before the wrapper finishes writing (it shouldn't — the wrapper's ordering is: parent writes the handoff → parent `fsync`s → parent `fork()`s → the child `execve`s the game, so the handoff is on-disk before the child even exists): add or strengthen `fsync` on the wrapper write side (Step 11).
- If `unlink` fails (read-only filesystem fallback): warn and move on; the handoff file is small enough that staleness on the next run is an acceptable UX issue once — the stale `mode=host` will re-run orchestrator, discover a new endpoint, and display a fresh code.

---

## Step 10 — Wrapper OSD Direct-P2P submenu

**Why it matters.** This is the only user-visible part of the phase that lives outside the game binary. The user's decision is clear: the entry point (Host/Join selection, peer-code entry) is in the wrapper's OSD menu. The wrapper OSD's existing pattern is a state-machine of `MENU_*` enum values dispatched from `HandleUI`, with `OsdWrite(line, text)` as the output primitive. The Phase 6-adjacent `MENU_BTNCHECK` patch (`tools/mister-wrapper/main-mister-full-menu.patch:65-172`) is the template for adding a new MENU state with custom rendering.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/main-mister-full-menu.patch:1-176` — full patch.
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/main-mister-overlay.files` — 14-entry keep-list (fpga_io.cpp/.h, input.cpp/.h, mister_joy_shm.h, thirdsarm_core_context.cpp/.h, thirdsarm_main.cpp, thirdsarm_wrapper.cpp/.h, user_io.cpp/.h, video.cpp/.h). **`menu.h`, `menu.cpp`, `osd.h`, `osd.cpp` are NOT in the overlay.** Those files in `vendor/Main_MiSTer/` (where `menu.h`, `osd.h`, `osd.cpp` happen to exist on disk — `menu.cpp` does not) are reference-only; they are not rsynced over the upstream clone by `tools/mister-wrapper/build-hps.sh:91`. The only mechanism that modifies menu.cpp / menu.h / osd.cpp / osd.h in the actual build tree is `main-mister-full-menu.patch` applied at `build-hps.sh:92`.
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/osd.h` — reference for `OsdWrite`, `OsdSetTitle`, `OsdSetSize` APIs (but edits to this copy do NOT reach the build tree).
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/build-hps.sh:85-94` — prepare_source sequence: clone upstream → rsync overlay files → apply patch → drop Makefile.3s-arm. Shows exactly why overlay-listed vs. not-listed distinction matters.
- Upstream `Main_MiSTer/menu.cpp` (not in our vendored copy — fetched by build-hps.sh at the pinned commit `3380931329b8acb442bd3d35a24d89f88641b7cf`; read it by `git clone https://github.com/MiSTer-devel/Main_MiSTer.git /tmp/main_mister_verify && git -C /tmp/main_mister_verify checkout 3380931329b8acb442bd3d35a24d89f88641b7cf`). Specifically, identify the MENU state enum declaration near line 120 and the HandleUI dispatch loop.
- **KNOWN REQUIREMENT (no longer an open question)**: `grep -iE 'keyboard|osk|password|rename|editname' build/mister-wrapper-hps/src/menu.cpp` returns only keyboard-scan-code matches and `DISABLE_KEYBOARD` — upstream has no existing on-screen-keyboard / multi-character text entry widget. The 4x4 OSK is therefore **required in-scope for Step 10**, not a fallback. Build a Crockford-alphabet on-screen keyboard (32 chars + backspace + done in a 6x6 grid) using `OsdWrite` with dpad + A selection. Estimated total Step 10 patch delta: ~280 LOC (MENU states + HandleUI cases + OSK rendering + per-frame selection state), not ~200 LOC.

**Files to create/modify.**
- `tools/mister-wrapper/main-mister-full-menu.patch`: extend by inserting new `MENU_DIRECT_P2P_ROOT`, `MENU_DIRECT_P2P_HOST`, `MENU_DIRECT_P2P_JOIN`, `MENU_DIRECT_P2P_JOIN_ENTRY`, `MENU_DIRECT_P2P_CONFIRM` enum entries near line 120, the corresponding `case MENU_DIRECT_P2P_ROOT:` / etc. blocks inside `HandleUI` near line 6860, and a Crockford-32 OSK renderer + selection state (6x6 grid over 32 alphabet chars + backspace + done + 2 blanks) drawn via `OsdWrite` from inside the `MENU_DIRECT_P2P_JOIN_ENTRY` case. Estimated patch delta: ~280 LOC (BTNCHECK patch is ~110 LOC; direct-p2p menu + OSK roughly 2.5× larger).
- `tools/mister-wrapper/Makefile.full.3s-arm`: no change expected. The patch + overlay are what matters.
- `vendor/Main_MiSTer/thirdsarm_wrapper.cpp`: add new constant `constexpr const char *kDirectP2PHandoffPath = "/tmp/3s-arm-netplay.handoff";`. This file IS in the overlay (`tools/mister-wrapper/main-mister-overlay.files:9`), so the edit reaches the build tree via rsync. No functional change here yet — Step 11 uses it.
- The OSK lives inside the patched menu.cpp (one TU, no new files). A separate `osd_keyboard.cpp`/`.h` would require either (a) adding those filenames to `main-mister-overlay.files` AND placing the sources in `vendor/Main_MiSTer/` (they'd get rsynced over upstream clone each build), or (b) creating them via the patch as all-new files. Option (a) is simpler and matches how every other overlay file works; option (b) bloats the patch with file-creation hunks. Only split into a separate TU if the in-patch OSK exceeds ~150 LOC and becomes hard to review.
- **Overlay addition**: if new files go into `vendor/Main_MiSTer/` (e.g., a standalone OSK TU per option (a) above), append them to `tools/mister-wrapper/main-mister-overlay.files`.

**Wrapper-edit mechanism (concrete rule):**
- (a) File IS in `main-mister-overlay.files`: edit the copy in `vendor/Main_MiSTer/<file>` — it gets rsynced over the upstream clone at `build-hps.sh:91`. Examples: `thirdsarm_wrapper.cpp`, `user_io.cpp`, `video.cpp`.
- (b) File is NOT in `main-mister-overlay.files`: the only mechanism is the patch at `tools/mister-wrapper/main-mister-full-menu.patch` applied at `build-hps.sh:92`. Examples: `menu.cpp`, `menu.h`, `osd.cpp`, `osd.h`. Editing the `vendor/Main_MiSTer/` copy of these files has NO build effect — they're reference-only.
- Never edit anything under `build/mister-wrapper-hps/src/` (AGENTS.md §Source of Truth; the tree is regenerated on every `prepare_source` run).

**Success criteria.**
- `tools/mister-wrapper/build-hps.sh --check-env` returns 0 (env is OK).
- `tools/mister-wrapper/build-hps.sh` builds `build/mister-wrapper-hps/MiSTer_3S-ARM`. `file build/mister-wrapper-hps/MiSTer_3S-ARM` shows `ELF 32-bit LSB executable, ARM, EABI5, hard-float`.
- `strings build/mister-wrapper-hps/MiSTer_3S-ARM | grep -E 'Direct P2P|Room Code|Host|Join'` returns at least 2 lines (confirms our new menu text is embedded).
- Lint-grep: `grep -c 'MENU_DIRECT_P2P' tools/mister-wrapper/main-mister-full-menu.patch` returns at least 5 (enum entries + case labels).
- No changes under `build/` tree committed (AGENTS.md §Source of Truth).

**Dependencies.** Step 2 (for the base32 display; the wrapper side reads the code as 14 chars but doesn't need to decode — that's the game's job). *[Superseded: **18** chars as shipped (`DP2P_CODE_CHAR_LEN 18`). "Doesn't need to decode" still holds, but the wrapper's buffers are NOT length-agnostic — they are sized from `RECENT_JOIN_CODE_BUF 24` and must be updated in lockstep with `room_code.h` whenever the format grows; the v3 bump would have overflowed the previous 16-byte buffers.]*

**What NOT to do.**
- Do not link `src/netplay/room_code.c` into the wrapper. The wrapper takes a typed string and forwards verbatim; the game validates. Avoids a cross-TU dep.
- Do not put the menu under `MENU_CORE_SETTINGS` or any existing menu — add a dedicated `Direct P2P` submenu at the top level. Discovery matters; burying it loses users.
- Do not edit files under `build/mister-wrapper-hps/src/` (AGENTS.md §Source of Truth). All wrapper-side edits go through the overlay + patch mechanism — see the "Wrapper-edit mechanism" rule above.
- Do not implement STUN/UPnP logic in the wrapper. Wrapper only collects intent + forwards to game.
- Do not change `MiSTer.ini` defaults — that's the main MiSTer system config, not ours.

**What to do if it fails.**
- If multi-character text entry proves impractical in the wrapper OSD (no existing keyboard widget, no easy upstream reuse): reduce Join to "paste code from clipboard" via a shared filesystem location. The host writes its code to `/media/fat/games/3s-arm/last-host-code.txt`; the joiner reads it from the same path when both MiSTers share a filesystem (e.g., via SMB mount). This is a strictly worse UX; use only if the OSD-keyboard build proves a tar-pit. Better: retreat to a **4x4 on-screen keyboard** rendered via `OsdWrite` with dpad+A to pick chars — this is what every console uses for alphanumeric entry, and for Crockford 32-char alphabet fits exactly in a 6x6 grid (32 chars + backspace + done + 2 empties).
- If the patch fails to apply at the pinned upstream commit: update the patch context lines. Upstream Main_MiSTer sees periodic refactoring of menu.cpp; rebasing the patch is routine.

---

## Step 11 — Wrapper handoff writer + exec-arg injection

**Why it matters.** Step 10 collects intent; Step 11 is where the wrapper writes the handoff file and passes `--direct-p2p-handoff /path/to/file` to the game.

**Files to read first.**
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2765-2770` — the `execve` call site. New args must be injected into `child_argv` before this.
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/thirdsarm_wrapper.cpp:2417-2770` — the run block around `execve` for context.
- The patch additions from Step 10 for where the `MENU_DIRECT_P2P_CONFIRM` state transitions to "launch game" — the handoff write happens in that transition.

**Files to create/modify.**
- `vendor/Main_MiSTer/thirdsarm_wrapper.cpp`:
  - Two new static functions: `write_direct_p2p_handoff_host()` and `write_direct_p2p_handoff_join(const char* code)`. Both open `kDirectP2PHandoffPath` with O_WRONLY|O_CREAT|O_TRUNC, mode 0600; write the two/three lines; `fsync`; close. Unlink on failure to avoid stale data.
  - At the `execve` call site: a static bool `g_direct_p2p_handoff_armed = false;` (set by Step 10's menu state-machine when the user confirms Host/Join). If true: inject `--direct-p2p-handoff /tmp/3s-arm-netplay.handoff` into `child_argv` before the `argv[2..]` forward.
- `tools/mister-wrapper/main-mister-full-menu.patch`: Step 10's patch is extended to call `write_direct_p2p_handoff_*` and set `g_direct_p2p_handoff_armed` in the `MENU_DIRECT_P2P_CONFIRM` transition.

**Success criteria.**
- `tools/mister-wrapper/build-hps.sh` succeeds.
- `strings build/mister-wrapper-hps/MiSTer_3S-ARM | grep -c 'direct-p2p-handoff'` returns at least 1.
- On-device smoke (Step 12 will drive this more rigorously): navigate OSD → Direct P2P → Host, press confirm. Observe `/tmp/3s-arm-netplay.handoff` created with `mode=host\n`. Game launches. After launch, `ls /tmp/3s-arm-netplay.handoff` returns no-such-file (game unlinked it).

**Dependencies.** Steps 9, 10.

**What NOT to do.**
- Do not write the handoff to `/media/fat/games/3s-arm/`. `/tmp` is correct (tmpfs, no wear, no persistence). Writing game data into `/media/fat` risks interaction with the game's own config layer.
- Do not write the handoff before the user confirms. `MENU_DIRECT_P2P_CONFIRM` is the commit point.
- Do not forget to `fsync` before `fork()` — write + fsync happens in the parent; only then does the parent `fork()` and the child `execve` the game. Linux tmpfs is memory-backed and the race would be benign (tmpfs writes are synchronous on the current data), but keep fsync explicit. Removing it saves <1ms and obscures the handoff semantics.
- Do not hardcode the path string in multiple places. Single definition in a header (`kDirectP2PHandoffPath`), consumed by both the writer (this step) and the menu state machine (Step 10).

**What to do if it fails.**
- If `execve` argv injection breaks the existing forwarding (e.g., the game sees the flag but also stale args from the wrapper): verify `child_argv.push_back(argv[i])` for `i >= 2` runs after the flag injection. Order matters: inject flag + its value, then forward user's argv[2..].
- If the game exits before reading the handoff: it's the flag parse failing. Verify the game binary version is the Step 9 build; Phase 6 builds don't know the flag.

---

## Step 12 — Test harnesses and on-device smoke plan

**Why it matters.** Per the task spec: same-LAN hairpin, two-separate-homes, UPnP-enabled vs UPnP-disabled, base32 round-trip, STUN fallback. The tricky one is two-separate-homes — without physical hardware, use a phone hotspot behind CGNAT as the second endpoint, which is also the canonical test for Symmetric NAT detection.

**Files to read first.**
- `src/netplay/test_room_code.c` (new from Step 2) — pattern template.
- Existing Phase 6 test harnesses (`test_event_queue.c`, `test_lobby_client_compile.c`, `test_mist_handshake.c`) for the test-runner style.

**Files to create/modify.**
- `src/netplay/test_stun_mock.c` (new, ~200 LOC): a link-time harness that spins up a mock STUN server on `127.0.0.1:19302` (using an SDL3_net datagram socket), a client calling `Stun_Discover`, and verifies the client correctly extracts the mapped address when the server replies. Stand-in for network-independent CI.
- `src/netplay/test_upnp_discover.c` (new, ~100 LOC): a manual-run test (not CI — UPnP depends on a router). Prints whether the local network has an IGD, external IP, and a test mapping creation/removal.
- `src/netplay/test_direct_p2p_flow.c` (new, ~250 LOC): exercises Step 7's orchestrator with mocked STUN and UPnP backends (stubs the `Stun_*` / `Upnp_*` entry points via a compile-time seam — define `DIRECT_P2P_ENABLE_TEST_HOOKS` and override).
- `src/main.c`: add dispatchers for `--test-stun-mock`, `--test-upnp-discover`, `--test-direct-p2p-flow` — same pattern as Phase 6.
- `src/args.c`: add the new test flags.
- `src/configuration.h`: add the corresponding booleans.
- **New doc**: `docs/on-device-smoke-direct-p2p.md` (~40 lines) — checklist for the three smoke scenarios below.

### Smoke scenario A: Same-LAN hairpin (single MiSTer, iPhone hotspot)

1. MiSTer on home LAN, phone on same LAN (via Wi-Fi, same public IP).
2. On MiSTer: OSD → Direct P2P → Host. Confirm. Code displays on game screen.
3. On a second test device (host desktop build with Direct-P2P): read the code from a photo, enter via CLI: `"${HOST_BIN}" --direct-p2p-mode=join --direct-p2p-peer-code=XXXX`. Note: these CLI flags do not exist in the game yet — they would be added for test purposes only behind `ENABLE_NETPLAY_TESTS`. Alternative: use the wrapper OSD on a second MiSTer if available.
4. Expect: hairpin rewrite kicks in, connection completes over 127.0.0.1.
5. Pass: gekko session reaches CONNECTING state, netplay.c logs "starting a session".

### Smoke scenario B: Two home networks (MiSTer on home LAN + phone hotspot for partner device)

1. Primary: MiSTer on home LAN (with UPnP-enabled router — check in Step 10's initial config audit).
2. Secondary: laptop tethered to phone hotspot (LTE/5G CGNAT — Symmetric NAT typical).
3. MiSTer: OSD → Host. Expect UPnP success (router maps port, code uses external IP).
4. Laptop: game build with Direct-P2P (host desktop build, `${HOST_BIN}`), `--direct-p2p-mode=join` + code.
5. Expect: MiSTer side's UPnP works; Laptop behind CGNAT may fail at hole-punch. If CGNAT is truly symmetric, Step 7's `DIRECT_P2P_FAILED_SYMMETRIC_NAT` state should fire.
6. Pass: either (a) connection succeeds (some CGNAT is semi-symmetric and punches through) or (b) clean symmetric-NAT failure message on laptop side.

### Smoke scenario C: UPnP-disabled router

1. On router, disable UPnP.
2. Two devices on the LAN.
3. Expect: UPnP fails fast, STUN runs, hairpin rewrite applies.
4. Pass: gekko session reaches CONNECTING through the STUN-hairpin path.

### Smoke scenario D: Base32 round-trip (pure offline)

1. `"${HOST_BIN}" --test-room-code` (or the Docker-container / on-device equivalent) — covered by Step 2.
2. Pass: test exits 0.

**Success criteria.**
- `EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DNETPLAY_TEST_HOOKS=ON -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS" tools/mister/build-game.sh --flavor telemetry` succeeds. Cross-compiled output lands at `build/mister-telemetry/3s-arm`. Both flags are required — see the note in Step 2's success criteria for why the spellings differ.
- Let `${HOST_BIN}` be `build/host/3S-ARM.app/Contents/MacOS/3S-ARM` on macOS or `build/host/3s-arm` on Linux; or use the Docker cross-build container's `build/mister-telemetry/3s-arm`; or deploy to MiSTer and invoke via `tools/mister/misterctl.sh`. `"${HOST_BIN}" --test-stun-mock` passes (runs in ~1 second, no network dep). **`--test-stun-mock` needs `NETPLAY_TEST_HOOKS`, not just `ENABLE_NETPLAY_TESTS`.** Its four `run_discover_*` cases (parallel probe, all-dead diagnosis, retransmit, port-disagreement) are compiled out without the hooks. Historically that skip printed one quiet line and the harness **still exited 0**, so "expect exit 0" from a bare-flags build validated nothing for those four; it now prints `[test_stun_mock] INCOMPLETE: ... DID NOT RUN` and fails (`test_stun_mock.c:1257-1275`). Either way, build with `-DNETPLAY_TEST_HOOKS=ON` — a bare-flags build was never sufficient.
- `"${HOST_BIN}" --test-direct-p2p-flow` passes.
- Smoke A (same-LAN hairpin) completed end-to-end with evidence (log snippet in commit message showing `configure_gekko` reached and the hairpin log line from Step 7). **REQUIRED.**
- Smoke C (UPnP-disabled router) completed with evidence (STUN-hairpin log line, router UPnP confirmed disabled via admin page screenshot noted in commit message). **REQUIRED.**
- Smoke B (two home networks + phone hotspot for symmetric NAT) is **CONDITIONAL**: mark as optional for commit gating. If the implementer has a phone hotspot + second test device, document the outcome (success or clean symmetric-NAT failure — both are valid). If not available, note "deferred — hotspot not available during implementation" in the commit message and flag for post-MVP validation. Do NOT block the Step 12 commit on Smoke B.
- `tools/mister-wrapper/build-hps.sh && tools/mister/build-game.sh --flavor telemetry` together produce both binaries; deploy to MiSTer per `docs/mister-runbook.md`. Post-deploy: boot, OSD Direct-P2P → Host, confirm Step 10's menu is present and the code renders on-screen.

**Dependencies.** Steps 1–11.

**What NOT to do.**
- Do not skip Smoke B. Two-home-network is the only real verification that the STUN/UPnP combo works outside the LAN. The CGNAT test is the one that catches "oh, the code path compiles but crashes on real NAT".
- Do not use `rsync --delete` on the deploy — `feedback-no-rsync-delete.md`. Use `rsync -av --no-owner --no-group` into `/media/fat/games/3s-arm/bin/` only for the game binary.
- Do not commit the screenshots or log files from smoke tests into git. Reference them in commit messages; keep artifacts in a per-session scratch dir.
- Do not flip upstream network behavior by running the smoke test behind a real VPN (Tailscale, Wireguard, etc). Per locked decision #7, no Tailscale. If a test device happens to have a VPN running, disable it before the smoke test — the "real internet" is what we're validating.
- Do not assume Step 10's OSD text entry works without validation. If text entry is impractical (see Step 10 open question), Smoke B requires a second MiSTer, not a laptop — and the plan must flag this as a known limitation.

**What to do if it fails.**
- If Smoke A works but the code display is unreadable on CRT (thin fonts on low-bandwidth composite): add a `--direct-p2p-room-code-large` flag that doubles the font scale in `netplay_screen.c`.
- If Smoke B consistently fails even on UPnP-enabled routers: check that UPnP mapping was actually created (log the miniupnpc response). Consumer routers sometimes report success but silently drop the mapping. Router-level debugging is out of scope for this phase.
- If the game crashes on handoff read: verify Step 9's code-validation path. A stale handoff with the wrong mode string should not crash — it should fall through to the normal menu.
- If deploy destroys runtime data (SF33RD.AFS et al): you used `rsync --delete`. Restore from a golden MiSTer build and re-read `feedback-no-rsync-delete.md`.

---

## Open questions flagged during planning

1. **Wrapper OSD text-entry for 14-character code.** *[Answered, and the length moved: the OSK is a 6×7 grid entering an **18**-character code (`DP2P_CODE_CHAR_LEN 18`, dashes rendered after chars 6 and 12, buffers sized from `RECENT_JOIN_CODE_BUF 24`). An 18-char code overflows the original 16-byte buffers, so the wrapper constants are kept in lockstep with `room_code.h` — see `docs/plan-netplay-connection.md` §6.3.]* The BTNCHECK patch pattern shows how to add a new MENU state, but does not demonstrate multi-character text entry. Before implementing Step 10, a 1–2 hour research spike on `vendor/Main_MiSTer/...` (or the upstream Main_MiSTer menu.cpp at the pinned commit) must enumerate existing upstream menu patterns that collect multi-character strings (e.g., SD card file selection, hostname entry). If none exist upstream, Step 10's fallback is a 4x4 on-screen keyboard built from `OsdWrite`.
2. **miniupnpc:armhf availability.** Confirmed available via Debian bullseye main (the `tools/mister/setup-build-container.sh` apt sources cover bullseye main + bullseye-updates + bullseye-security). If the package gets moved to unstable/testing only, Step 4's fallback is vendoring miniupnpc into `third_party/` similar to SDL3_net (add ~60 LOC to `build-deps.sh`). Verify at Step 4 kickoff.
3. **Host's internal IP in the room code.** When UPnP succeeds, the external IP from UPnP maps to the LAN IP via the router's NAT. The joiner needs the external IP+external_port. But on hairpin (same LAN), the joiner needs the internal IP+internal_port. Resolution: encode the external IP in the code; on the joiner's side, run STUN too and compare public IPs — if same, rewrite to the host's internal_port (which the code also carries). This is exactly upstream's pattern (`sdl_netplay_ui.cpp:719-720` uses `lobby_punch_peer_local_port` for the hairpin port). The 8-byte payload in Step 2 already accommodates both port fields. *[Resolved differently — this resolution was NOT implemented. `local_port` was dropped from the payload before the first release (`docs/STUN-PORT-STATUS.md`), so the shipped code does **not** carry `internal_port` at any version. Hairpin relies on NAT loopback instead. The payload's second port slot became the nonce.]*
4. **Session restart after symmetric-NAT failure.** Answered: the game renders the failure string for 4 seconds, then calls `Soft_Reset_Sub()` — the existing Phase 6 netplay-teardown return-to-title path. The wrapper OSD is not involved (the user re-enters it through the normal OSD hotkey once back at the title). Step 7 and Step 8 encode this behavior. Post-MVP refinement path: if UX feedback requests auto-return to wrapper OSD, add a game-to-wrapper exit signal (likely a distinct exit code the wrapper interprets).
5. **Doc/code mismatch: wrapper source.** AGENTS.md §Source of Truth warns that `build/mister-wrapper-hps/src/` is NOT the source of truth, and points at `vendor/Menu_MiSTer/`. But `Menu_MiSTer` is the **FPGA core menu** (SystemVerilog); the **OSD wrapper** (ARM-side C++) source is `vendor/Main_MiSTer/` per `tools/mister-wrapper/build-hps.sh:7`. Plan uses `vendor/Main_MiSTer/` for wrapper edits. Flagged here for agent safety — do not edit `vendor/Menu_MiSTer/` when the user asks for OSD changes.
6. **`r_no[1]=6` routing.** Per the task spec and `docs/plan-netplay-phase6.md:433-437`, our fork's menu slot 5 routes to `MENU_SCREEN_NETWORK_LOBBY`. Direct-P2P does NOT collide because Direct-P2P is entered via the **wrapper OSD**, not via an in-game menu slot. No new in-game menu slot is allocated. Documented in Step 10.

## Cross-cutting risk notes

- **SDL3_net ABI drift (gated by Step 1).** If SDL_net upstream bumps the pinned ref after this phase but before Phase N+1, re-run Step 1 at Phase N+1 kickoff.
- **Symmetric NAT prevalence.** Typical estimate: 5–10% of home routers present symmetric NAT, up to 40% for mobile CGNAT. Smoke B's CGNAT path is a realistic failure mode and the graceful-degrade UX in Step 7/8 is the mitigation.
- **miniupnpc cross-compile surface.** apt-based install is the happy path; vendoring adds ~1h of work. Flagged in Step 4.
- **UPnP router-lies-about-success.** Mitigation in Step 7 "what to do if it fails": verify the mapping with `UPNP_GetSpecificPortMappingEntry` after `Upnp_AddMapping` returns true. Do NOT attempt a self-ping to our own external IP — that requires router hairpin NAT support, which is often absent.
- **CGNAT test-plan availability.** Requires a phone with hotspot. If not available during implementation, Smoke B gets deferred to post-MVP validation — reflected in Step 12's success criteria (documented outcome, not mandatory pass).
- **Socket-ownership correctness.** Three potential owners (Direct-P2P orchestrator, Netplay via `Netplay_SetStunSocket`, SDLNetAdapter). Step 6 + Step 7 encode the serial transfer. Any fourth toucher (e.g., a future telemetry probe attaching to the socket) would break this — call out in code comments.

## Reviewer notes resolved

All P-1 and P-2 findings were verified against the real code and applied to the plan. No findings were skipped — each was either corrected directly in the referenced step or reflected in Step 3 / cross-cutting notes where the issue applied plan-wide (e.g., build-output paths, host-binary location). Cross-step consistency fixes made beyond the listed findings: removed the `./build/3s-arm` invocations inside Smoke A scenario 3 and Smoke B scenario 4 to use the `${HOST_BIN}` abstraction introduced for Step 12 (follow-on to P-1 #4); propagated `build/mister/` → `build/mister-telemetry/` in every Step's success criteria (P-1 #2); updated the dependency graph line for Step 6 to match the corrected dependency set (P-1 #7); updated Open Question #2 and #4 and the UPnP cross-cutting risk note to reflect the answered/corrected findings (P-2 #11, #14, #23).
