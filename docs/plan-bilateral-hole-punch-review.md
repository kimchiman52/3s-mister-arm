# Review: Bilateral Hole Punch Plan

## Verdict

**Needs P-1 fixes before `/implement`.** The plan's architecture, state-machine extensions, step ordering, and non-goals are sound, and the vast majority of file/function/line references check out against the actual codebase. However, there are three material P-1 errors that would either cause the implementation to do duplicate work, make false assumptions about existing tooling, or create a genuine socket-concurrency bug on the host path. The most important fix is the SHA-256 finding: a production SHA-256 module already exists at `src/utils/sha256.{c,h}`, so vendoring `/tmp/3sxtra/src/netplay/sha256.{c,h}` is redundant (and creates a second implementation surface). The remaining P-1 items are a mis-claim about `tools/lobby-server/` existing in our repo and an under-specified socket-ownership contract during FALLBACK_BILATERAL_PUNCH. With those three fixed, the plan is shippable.

---

## P-1 findings

### P-1.1 SHA-256 already exists in-tree; do not vendor from 3sxtra

**Claim in plan:** §Decision 2 and §Step 3: "**We do NOT link SHA-256 from tf-psa-crypto**; instead we implement the tiny public-domain SHA-256 already present at `/tmp/3sxtra/src/netplay/sha256.{c,h}` (copy it in as Step 1)" and "Create `src/netplay/sha256.c` and `src/netplay/sha256.h` — direct copy of `/tmp/3sxtra/src/netplay/sha256.{c,h}`."

**Actual:** `src/utils/sha256.h:1-23` and `src/utils/sha256.c:1-17` already implement SHA-256 against tf-psa-crypto. The API is `sha256_init` / `sha256_append` / `sha256_finalize_bytes` / `sha256_finalize_hex`. `third_party/tf-psa-crypto` is already linked by `CMakeLists.txt:246`, and `src/port/resources.c:113,205` actively uses this module today (asset hashing). So the premise "we would need libcurl+OpenSSL cross-compiled to get SHA-256" is wrong — SHA-256 is already a solved build dependency we ship on MiSTer.

**Impact:** Vendoring a second SHA-256 implementation creates two code paths for the same primitive (inconsistent with the existing pattern), adds ~120 LOC of code we have to maintain (and re-audit for the public-domain license claim), and misses the chance to keep the netplay module consistent with the rest of the codebase. Step 3's "Do NOT" list also forbids `identity.c / lobby_server.c / discovery.c` — but `src/utils/sha256.c` is ours, not 3sxtra's, and is explicitly the right dependency.

**Fix:** Change §Decision 2 and §Step 3 to use the existing `src/utils/sha256.h` API instead of vendoring a new file. Drop the `src/netplay/sha256.{c,h}` creation from §Step 3's file list; replace "Create ... sha256.c/h" with "`#include "utils/sha256.h"` from `rendezvous.c`". Update `Rendezvous_DeriveSessionKey` to call `sha256_init` / `sha256_append(payload, 6)` / `sha256_finalize_bytes(out_bytes32)` and truncate to 16 bytes. The license-header audit step in §Step 3's "If it fails" also becomes moot.

---

### P-1.2 `tools/lobby-server/` does not exist in this repo

**Claim in plan:** §Step 1 "Node.js service is the cheapest path and matches the runtime we already use for `tools/lobby-server/` (per `docs/plan-netplay-port.md`)." and "Create `tools/rendezvous-server/deploy.sh` — same shape as `tools/lobby-server/deploy.sh`." and "Create `tools/rendezvous-server/README.md` — ... systemd unit similar to the existing `tools/lobby-server/lobby-server.service`."

**Actual:** `ls /Users/sb/Developer/3sx-mister/tools` shows only `compare_char_data.py`, `compare_states.py`, `mister`, `mister-wrapper`, `netplay`, `pll_search_*.py`, `requirements-*.txt`, `test_matchmaking_server.py`. There is **no `tools/lobby-server/` directory**. The lobby-server.js/.service/deploy.sh referenced in the plan live under `/tmp/3sxtra/tools/lobby-server/` (the upstream checkout), which is not part of our tree. Per `project-netplay-port-strategy.md` we deliberately chose to piggyback on 3sxtra's hosted lobby rather than host our own — that decision is why there's no local `tools/lobby-server/` tree to model the rendezvous server after.

**Impact:** Step 1's "read-first" and "same shape as" references point to files that don't exist in this repo. An implementer following the plan will be confused about where to mirror the deploy/systemd convention from. The pattern does exist in `/tmp/3sxtra/tools/lobby-server/` (I verified `deploy.sh`, `lobby-server.service`, `lobby-server.js`, `package.json` all exist there), but the plan calls them "existing" in our repo, which they are not.

**Fix:** In §Step 1, change "the runtime we already use for `tools/lobby-server/`" to "the runtime used by 3sxtra's upstream `/tmp/3sxtra/tools/lobby-server/` (we do not ship our own lobby server — see `project-netplay-port-strategy.md`)." Change "`same shape as tools/lobby-server/deploy.sh`" to "`same shape as /tmp/3sxtra/tools/lobby-server/deploy.sh`." Change "similar to the existing `tools/lobby-server/lobby-server.service`" to "similar to `/tmp/3sxtra/tools/lobby-server/lobby-server.service`." No change to actual work; just the citations become correct.

---

### P-1.3 Socket ownership during FALLBACK_BILATERAL_PUNCH is under-specified, creating a real read-race

**Claim in plan:** §Decision 3 mitigation: "all reads stay on the main thread. The rendezvous thread ONLY sends (REGISTERs, POLLs)." And §Step 5 (line 525): the rendezvous thread "shares `s_work.stun.socket` for sends only. Reads stay on the main thread."

**Actual:** `src/netplay/stun.c:424` shows that `Stun_HolePunch` internally calls `NET_ReceiveDatagram(sock, &dgram)` in a loop over `punch_duration_ms`. The plan's §Step 5 spawns `host_bilateral_punch_thread_fn` that calls `Stun_HolePunch(s_work.stun, ...)` — which by definition reads the STUN socket from the bilateral-punch thread while `DirectP2P_Tick` on the main thread may still run `host_tick_receive` (line 691 of direct_p2p.c calls it in the HOST_WAITING branch). The plan does add a new `DIRECT_P2P_FALLBACK_BILATERAL_PUNCH` tick case (§Step 5 bullet: "DirectP2P_Tick — add case for FALLBACK_BILATERAL_PUNCH on host: check if host_bilateral_punch_thread_fn has completed") which implicitly stops calling `host_tick_receive` in that state — but the plan never explicitly states "during FALLBACK_BILATERAL_PUNCH the main thread MUST NOT call `host_tick_receive` / `NET_ReceiveDatagram`." §Decision 3's summary ("all reads stay on the main thread") is therefore actually false during FALLBACK_BILATERAL_PUNCH on the host — reads migrate to the bilateral-punch thread.

**Impact:** Two readers on the same SDL3_net socket is exactly the race the plan says it is mitigating. If an implementer reads §Decision 3 literally and adds concurrent `host_tick_receive` calls, the main-thread read and the Stun_HolePunch read will both poll the socket and each may consume the other's expected datagrams. The implicit transition rule (only Stun_HolePunch reads during FALLBACK_BILATERAL_PUNCH) works, but only if the plan states it. Right now that's buried in a Tick-case bullet with no cross-reference.

**Fix:** Add a §Decision 3 clarification: "Once state transitions to FALLBACK_BILATERAL_PUNCH on the host, main-thread `DirectP2P_Tick` stops calling `host_tick_receive` and only polls the bilateral-punch thread status. The STUN socket is owned exclusively by `host_bilateral_punch_thread_fn` for the duration of that thread's `Stun_HolePunch` call; no concurrent reader on the main thread." Also explicitly document in §Step 5's DirectP2P_Tick modification that the new FALLBACK_BILATERAL_PUNCH case must replace (not coexist with) a `host_tick_receive` call.

---

## P-2 findings

### P-2.1 LAN bypass private-IP check misses the NAT-hairpin failure case the plan claims to handle

**Claim in plan:** §Hard requirement 3 and §Decision 11: "Offline / LAN-only must not touch the rendezvous server. `127.0.0.1` and RFC1918 ranges … skip bilateral-punch entirely."

**Actual:** The existing hairpin case in `direct_p2p.c:527-532` is triggered when `peer_public_ip == self_public_ip` — meaning two peers on the same LAN whose router reports the same public IP to both. In that scenario `peer_ip` is the *public* IP (never private/RFC1918), so `direct_p2p_is_lan_peer(peer_ip)` as defined in §Step 5 ("`127.0.0.1`, `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`") returns **false** for hairpin traffic. The plan's bypass only fires when the user typed a LAN address directly (the `--p2p-remote-ip` path, which §Hard requirement 3(a) correctly notes never enters the orchestrator).

**Impact:** Two peers behind the same router that doesn't support NAT loopback will (a) fail the 2.5s direct punch, (b) fall into FALLBACK_SIGNALING, (c) REGISTER at the rendezvous server with their shared public IP, and (d) both get DELIVER with each other's public endpoint — which the router still won't loop back. The bilateral punch will fail, landing in FAILED_BILATERAL after ~11s instead of today's ~2.5s FAILED_SYMMETRIC. Not security-critical, but worse UX than advertised, and it does hit the rendezvous server which Hard Requirement 3 tried to prevent.

**Fix:** Either (a) add a hairpin check in the fallback gate — skip bilateral if `s_work.peer_ip == s_work.stun.public_ip` on the joiner side (host knows only its own public IP until DELIVER, so the check happens at DELIVER-receive time: skip if delivered peer_ip matches own public_ip); or (b) accept the longer fail path and document it as a known limitation in §Rollout. Option (a) is cheap (one strcmp) and honors Hard Requirement 3 more faithfully.

### P-2.2 Session-key entropy is 48 bits, not 128

> *[Historical record — the quotes and the 48-bit arithmetic below were correct against the plan as reviewed and are deliberately left verbatim. Both numbers have since moved: the room code is 18 chars (v3, not 11), and its payload is 10 bytes — `ip[4] ‖ port_be[2] ‖ nonce_be[4]` — so the derivation input is **80 bits**, of which 32 are CSPRNG nonce. That is exactly the fix this finding pointed at: `(ip, port)` alone no longer determines the session key. See `docs/plan-netplay-connection.md` §6.3 and §6.8.]*

**Claim in plan:** §Decision 8 "Collision probability. SHA-256[0..16] is a 128-bit space. Two random 11-char room codes colliding is ~2^-128. Non-issue."

**Actual:** The SHA-256 output is 128 bits after truncation, but the *input* (the 6-byte room-code payload: 4-byte IPv4 + 2-byte port) is only 48 bits. The derived session_key inherits the input's entropy ceiling: collision probability is ~2^-24 for random pairs (birthday bound on 48-bit inputs), and a targeted attacker with a victim's IP range can brute-force all 2^16 port candidates in seconds. The "~2^-128" figure is wrong. The plan's own §Security subsection on rendezvous-key leakage correctly identifies the attack (anyone with the room code derives the key) — but the collision math contradicts the threat-model conclusion.

**Impact:** Not load-bearing for the decision (the security argument is "same attack as direct-P2P fast path" which remains valid), but the math is wrong in a review-visible way and a careful reader will question the rest of §Security.

**Fix:** Replace "SHA-256[0..16] is a 128-bit space. Two random 11-char room codes colliding is ~2^-128" with something like: "The input space is 48 bits (IP+port), so two distinct rooms collide only if they share the same (ip, port) tuple — which is the problem the room code itself already has. The 128-bit truncation is only to make the key non-typeable; it does not add entropy."

### P-2.3 §Testing assumes `direct_p2p.c` state machine is drivable in-process, but the module has no test seam today

**Claim in plan:** §Decision 7 Test 4 and §Step 6 Test 4: "Drive `direct_p2p.c`'s state machine with a mock `Stun_HolePunch`… This will need some test-only hooks; prefer lightweight function pointer injection via a `#ifdef NETPLAY_TEST_HOOKS` seam rather than rewriting the module."

**Actual:** `direct_p2p.c` today has no function-pointer seam for `Stun_HolePunch`. Calls are direct at lines 421 (`Stun_HolePunch(&s_work.stun, ...)`). Adding a seam is net-new code in `direct_p2p.c` that belongs to Step 6 but is physically in Step 5's file. The plan's dependency ordering (Step 6 depends on Step 5) is correct, but Step 5's `/implement` agent won't know to add the seam — which will force Step 6 to reopen `direct_p2p.c`. Fine in principle; Step 6's file list would need to include `src/netplay/direct_p2p.c` (currently implicit).

**Impact:** Minor coordination issue. Step 6's scope as written reads as purely additive (new test file, args, main dispatch), but in practice it touches `direct_p2p.c` to add the hook seam. An implementer who reads Step 6 as "do not touch direct_p2p.c" will not add the seam, and Test 4 won't be implementable.

**Fix:** In §Step 6 "Changes", explicitly list `src/netplay/direct_p2p.c` as a file modified to add the `NETPLAY_TEST_HOOKS` seam. Note which specific calls get the seam (`Stun_HolePunch` is the one named; `Rendezvous_Send` per the plan, though the `rendezvous.c` side of that is mostly pure already).

### P-2.4 `host_tick_receive` dispatch uses wrong minimum length

**Claim in plan:** §Step 4 "If `buflen >= 24` AND first 4 bytes == `'3SXR'` magic: hand to `try_handle_deliver()`"

**Actual:** §Decision 2 specifies DELIVER as **32 bytes**, not 24. REGISTER and POLL are 24 bytes, but clients never receive those. The host receives DELIVERs (32B) from the server; the buflen check should be `>= 32` (or `>= 8` for magic+version+type+reserved, if you want to reject malformed traffic with a short-header error log). Currently `buflen >= 24` would happily accept a REGISTER-sized packet someone spoofed at the host, though it would then fail `Rendezvous_ParseDeliver`.

**Impact:** Minor defense-in-depth issue; also inconsistent with §Decision 2's sizes. Works in practice because the parser validates, but the buflen check is the first-line cheap filter.

**Fix:** Change §Step 4's criterion to `buflen >= 32` (DELIVER minimum), matching the §Decision 2 wire format.

### P-2.5 Kill-switch behavior on host side is under-specified

**Claim in plan:** §Decision 6 "Kill switch — forces direct-only behavior (today's `FAILED_SYMMETRIC` path)." §Step 5 for joiner: "If `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL` is true OR `direct_p2p_is_lan_peer(s_work.peer_ip)`: transition to `FAILED_SYMMETRIC` (unchanged)."

**Actual:** The plan tells us explicitly how the kill switch behaves on the joiner side. On the host side, §Step 5 says only: "Update `host_thread_fn` to spawn `host_rendezvous_thread_fn` after publishing `HOST_WAITING` (unless `CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL` is true…)." That's correct, but it doesn't state what happens when the passive receive path times out on the host. With kill switch off today, host stays on HOST_WAITING forever until user cancel. Under the new plan, if the rendezvous thread's 8-second budget expires with no DELIVER, §Decision 3 says "state stays at `DIRECT_P2P_HOST_WAITING` (the passive receive path keeps running in case a late-arriving direct punch still works)." Good. With kill switch **on**, there's no rendezvous thread and behavior is identical to today. The caller's request ("Behavior when kill switch is on + direct punch fails should exactly match today") is satisfied on both sides.

**Impact:** None — verified the kill switch matches today's behavior. Flagging so the reviewer doesn't re-discover the same thing.

**Fix:** No change needed; this is a verified-correct item. Listed here so the Fix agent skips it.

### P-2.6 Step 5's "do NOT start a signaling thread on the joiner side" claim is inconsistent with inline REGISTER/POLL loop semantics

**Claim in plan:** §Step 5 "Do NOT: Start a signaling thread on the joiner side — joiner's existing worker thread handles rendezvous inline."

**Actual:** §Step 5 also says the joiner runs "inline-run REGISTER/POLL loop for the configured budget" on the existing `join_thread_fn` worker thread. That's fine, but the plan says `join_thread_fn` runs STUN discover → hole-punch → HANDOFF and then exits (direct_p2p.c:579 is the return). To REGISTER/POLL inline the worker must stay alive longer, which means it also owns the bilateral Stun_HolePunch call. Currently §Step 5 says "call `Stun_HolePunch` with `CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS` budget; on DELIVER-never-arrived or second punch failure, `FAILED_BILATERAL`; on success, `HANDOFF`." Consistent, but not clearly bookkept against the current worker's "publish state then exit" contract (direct_p2p.c:577-578). The worker will now be alive until the second Stun_HolePunch returns, which may be ~11 seconds after the user pressed "Join."

**Impact:** The join-side thread lifetime grows significantly. The existing `DirectP2P_Cancel` logic waits up to 500ms for IDLE (direct_p2p.c:780-782). With a Stun_HolePunch in progress on the worker, the worker will see `s_cancel` and return from Stun_HolePunch (it passes `&s_cancel` as the cancel_flag — direct_p2p.c:543), so Cancel still works. But the plan should state that the joiner worker's lifetime is extended by up to `budget_ms + punch_ms` (8s + 3s = 11s), and that DirectP2P_Cancel's existing 500ms grace period is adequate because Stun_HolePunch honors the cancel flag.

**Fix:** Add a sentence to §Step 5 joiner changes noting the extended worker lifetime and confirming cancel-flag compatibility. No behavior change needed.

### P-2.7 Step 6 forward-declares `Netplay_Test_BilateralPunch` — verify struct name

**Claim in plan:** §Step 6 "Add field to `Configuration` / `NetplayTestingFlags` (whatever the struct name is; verify via grep)."

**Actual:** `src/args.c:265-286` shows the existing fields are on `configuration->test_netplay_event_queue`, `configuration->test_mist_handshake`, `configuration->test_room_code`, `configuration->test_stun_mock`. The field is a simple `bool` on the top-level `Configuration` struct, not a separate `NetplayTestingFlags`. The plan's hedge ("whatever the struct name is") is honest, but since this is a concrete concretely-implementable step, it would be cleaner to state the struct.

**Impact:** None for correctness; just a small ambiguity.

**Fix:** Replace "`Configuration` / `NetplayTestingFlags`" with "`Configuration` (the top-level struct; the existing tests use `configuration->test_stun_mock` etc. as simple bool fields)."

---

## Nits

- §Decision 2 wire format: "u16 my_public_port (BE)" — rest of the doc uses "host byte order" / "network byte order." Pick one convention. `StunResult.public_port` is host order per `stun.h:16`, so REGISTER writing `my_public_port` in network byte order requires an `htons`; flag this in the encoder spec.
- §Decision 3 line 134 says "putting it on the worker thread that publishes `HOST_WAITING` would require redesigning that worker's exit contract (today the worker exits immediately after publishing `HOST_WAITING`)." Verified at `direct_p2p.c:482-486`: host worker publishes HOST_WAITING then returns. Accurate.
- §Step 1 "Node.js/Go on the server, ~250 LOC of SDL3_net UDP client" — drop the Go alternative; §Decision 1 locked in Node.js for same-runtime rationale.
- §Step 5 mentions `docs/plan-bilateral-hole-punch.md` §Bilateral smoke added in Step 7 as a cross-reference for Step 5's success criteria. That creates a forward reference: Step 5 smoke-criteria refers to a doc section that doesn't exist yet until Step 7. Reorder so Step 7 lands before Step 5 smoke is cited, or accept the forward reference and note in Step 5: "(Step 7 will create this section; early Step 5 runs can log-grep manually.)"
- §Step 8 "Update `MEMORY.md` index with the new entry." MEMORY.md lives at `/Users/sb/.claude/projects/-Users-sb-Developer-3sx-mister/memory/MEMORY.md` — the plan references the directory but not the index file by name. Minor.
- Overlay mode-label mapping: plan §Decision 9 says "`FALLBACK_SIGNALING` and `FALLBACK_BILATERAL_PUNCH` map to `CONNECTING`." Verified `direct_p2p_overlay.c:45-67` uses a switch that returns `"CONNECTING"` today for `DIRECT_P2P_JOIN_PUNCHING`. Adding two more cases returning `"CONNECTING"` is trivial. Minor.

---

## Things verified correct

- **File paths and extensions**: `src/netplay/direct_p2p.{c,h}` exist at the cited paths; `src/netplay/stun.{c,h}`, `src/netplay/room_code.{c,h}`, `src/netplay/netplay.{c,h}` all exist. `src/port/config/config.{c,h}` exist. `src/netplay/test_stun_mock.c` and `src/netplay/test_mist_handshake.c` exist. `docs/direct-p2p-smoke-plan.md` exists. `docs/config.md` exists. `docs/STUN-PORT-STATUS.md` exists.
- **Function signatures** *(pointers refreshed to the current tree 2026-08-29; each review-time number is kept in its parenthetical, because the finding is about that number)*: `host_thread_fn` at `direct_p2p.c:2799` (was 288; plan cited the range correctly), `join_thread_fn` at `direct_p2p.c:3404` (was 372; plan said 372-457, actual at the time was 372-458 — off by one, nit), `host_tick_receive` at `direct_p2p.c:3993` (was 516; plan cited 516-552, verified exactly), `do_handoff(1, …)` at `direct_p2p.c:4125` (was 550) and `do_handoff(2, …)` at `direct_p2p.c:4133` (was 558), both verified exactly.
- **Stun API**: `Stun_Discover(StunResult*, uint16_t)`, `Stun_HolePunch(StunResult*, char*, uint16_t*, int, SDL_AtomicInt*)`, `Stun_CloseSocket(StunResult*)` all match plan-assumed signatures (`stun.h:25,28,41`).
- **Room-code API**: `RoomCode_Encode` / `RoomCode_Decode` / `RoomCode_NormalizeInput` at `room_code.h:96,104,113`; payload is 6 bytes (`ROOM_CODE_RAW_LEN` at `room_code.h:63`). Plan's derivation of the 6-byte payload from both sides is correct. *[No longer true — corrected here because these are file:line references a reader would follow. As of room code v3: `RoomCode_Encode` `room_code.h:195`, `RoomCode_Decode` `:204` (returns `RoomCodeDecodeResult`, not `bool`, and takes a `nonce` out-param), `RoomCode_GenerateNonce` `:215`, `RoomCode_NormalizeInput` `:251`. `ROOM_CODE_RAW_LEN` was deleted; the payload is **10 bytes** — `ip[4] ‖ port_be[2] ‖ nonce_be[4]` (`room_code_pack_payload`, `room_code.c:251`; `REND_KEY_PAYLOAD_LEN 10`, `rendezvous.c:48`). The rest of this section's line numbers were verified against the tree as it stood at review time and have drifted similarly.]*
- **Netplay API**: `Netplay_SetStunSocket(struct NET_DatagramSocket*)` at `netplay.h:43`, `Netplay_SetParams(int, const char*)` at `:24`, `Netplay_SetSessionTeardownCallback(void (*)(void))` at `:49`, `Netplay_SetRemotePort(unsigned short)` at `:29`. All match plan's assumptions.
- **Config API pattern**: `CFG_KEY_*` macros in `config.h:29-33` exist with the naming convention the plan follows. `default_entries[]` in `config.c:53-78` is additive — new entries can be appended. `CONFIG_ENTRIES_MAX` is 128 (`config.c:10`), current count is ~17 defaults plus dynamic entries; plenty of headroom. Plan's "If it fails" note about the 128-entry ceiling is accurate.
- **DirectP2PState enum**: `direct_p2p.h:59-69` has exactly the states the plan assumes, including `DIRECT_P2P_FAILED_SYMMETRIC`, `DIRECT_P2P_FAILED_STUN`, `DIRECT_P2P_FAILED_PUNCH`. Additive enum extension is safe.
- **Upstream reference validity**: `/tmp/3sxtra/src/netplay/lobby_server.c` exists and uses `#include <curl/curl.h>` at line 24 (plan cites `:19-24`, close enough). `/tmp/3sxtra/tools/lobby-server/lobby-server.js` exists; room_code filter at line 366 (plan cites 362 — off by a few, nit). `/tmp/3sxtra/src/netplay/sha256.{c,h}` exists with public-domain header (`sha256.c:4-8`, `sha256.h:6`).
- **Stun_EncodeEndpoint existence**: exists in our repo at `stun.c:39`, not just in 3sxtra's. Plan's claim that 3sxtra encodes `"ip|public_port|local_port"` via `Stun_EncodeEndpoint` is consistent with our own `stun.c:39`.
- **Build commands**: `tools/mister/build-game.sh --flavor telemetry` exists and accepts `--flavor telemetry` per `build-game.sh:10,24,64,92`. `cmake --build build/host` is the standard host build invocation documented elsewhere in the repo.
- **Test harness registration pattern**: `main.c:912-967` shows the exact forward-decl + dispatch pattern for existing test harnesses (`Netplay_Test_EventQueue`, `Netplay_Test_MistHandshake`, `Netplay_Test_RoomCode`, `Netplay_Test_StunMock`). `args.c:217-243` shows the `OPT_BOOLEAN` flag registration pattern. Plan's proposed `--test-bilateral-punch` fits identically.
- **Hard requirements met (except P-2.1 caveat)**: (1) fast path preserved — plan correctly keeps `host_tick_receive` passive path unchanged; (2) lean deps — correctly rejects libcurl+cJSON (see P-1.1 for the SHA-256 twist); (3) LAN bypass — RFC1918 check is present but hairpin-blind (see P-2.1); (4) no daemon — REGISTER/POLL phase budget is 8s with socket torn down after bilateral.
- **Phase ordering**: Dependency graph at §bottom is correct. Step 1 (server) + Step 2 (config) are independently deployable. Step 3 (client pure module) before Step 4 (host passive dispatch stub) before Step 5 (full integration) is correct. Step 6 tests depend on Step 5, Step 7 smoke needs Step 5 + deployed Step 1, Step 8 doc follows Step 5 — all consistent.
- **Success-criteria automatability (spot-check)**: Step 2's `grep -n 'netplay-direct-p2p-disable-bilateral\|...' src/port/config/config.{c,h}` — concrete, runnable, will return non-empty once the keys are added. Step 3's `grep -n 'Rendezvous_DeriveSessionKey\|...' src/netplay/rendezvous.h` — concrete. Step 4's `grep -n 'try_handle_deliver\|0x33535852' src/netplay/direct_p2p.c` — concrete. Step 6's `--test-bilateral-punch` exit-0 check — concrete. Step 7's `grep -n 'Bilateral smoke\|FALLBACK_SIGNALING' docs/direct-p2p-smoke-plan.md` — concrete. Automatability is good across the plan.
- **No scope creep detected**: Plan explicitly excludes TURN, ICE, DTLS, port prediction, matchmaking, 3sxtra lobby integration, cross-arch crossplay, telemetry, bracket play, leaderboards, RmlUi UI changes, `netplay_screen.c` changes, wrapper (`build-hps.sh`) changes, and UPnP changes. All those exclusions are internally consistent. No step sneaks in any excluded capability.
- **Scenario matrix accuracy**: §Rollout matrix is internally consistent. The "New host + Old joiner" row ("host's rendezvous thread REGISTERs but never gets a peer; after 8 s rendezvous exits, host stays on HOST_WAITING forever") matches the code's current HOST_WAITING behavior (no timeout on the passive receive loop). "Graceful degradation" is genuinely graceful — no session that works today would break.
- **Kill-switch behavior with direct-punch failure**: When the kill switch is on and the direct punch fails, the joiner transitions to FAILED_SYMMETRIC (plan §Step 5 explicit), which matches today's behavior exactly (direct_p2p.c:550-557). On the host side, no rendezvous thread spawned, so host stays on HOST_WAITING same as today. Requirement satisfied.
- **Socket handoff timing**: §Hard requirement 4 ("No long-lived daemon") is honored: all new threads exit after the orchestrator transitions to HANDOFF or FAILED_BILATERAL. `direct_p2p_on_teardown` at `direct_p2p.c:587-599` already resets state to IDLE on session end, which the plan correctly extends to also cancel the rendezvous/bilateral threads.

**Correction (2026-04-26):** Two entries in this list — `/tmp/3sxtra/src/netplay/lobby_server.c exists and uses #include <curl/curl.h> at line 24` and `/tmp/3sxtra/src/netplay/sha256.{c,h} exists with public-domain header` (both within the "Upstream reference validity" bullet at line 141) — are no longer reproducible. The local 3sxtra checkout has been cleared (verified 2026-04-26: zero `.c`/`.h` files anywhere under `/tmp/3sxtra/`); these claims should be treated as historical artifacts of the original review pass, not as current verifications. The conclusions they supported (use in-tree `src/utils/sha256.h`; treat upstream as reference-only) remain correct on first-principles grounds.

---

## Re-verification 2026-04-26

A second deep verification pass produced 12 additional findings, prioritized below. Each was applied as a revision to `plan-bilateral-hole-punch.md`; cross-references point to the affected plan section. New findings are numbered NEW-N to distinguish them from the original P-1/P-2/Nit set.

### P-1 (new)

#### NEW-1 — Wire-format byte counts are internally inconsistent; IPv4 encoding ambiguous

**Claim in plan (pre-revision):** §Decision 2 declared REGISTER and POLL as "24 bytes" and DELIVER as "32 bytes", with `peer_ip[16]` documented as "IPv4-mapped IPv6 (`::ffff:a.b.c.d`) or raw IPv4 at `[12..15]` with `[0..11]=0`."

**Actual:** Summing the declared field widths:
- REGISTER: 4 (magic) + 1 (version) + 1 (type) + 2 (reserved) + 16 (session_key) + 2 (port) + 2 (reserved2) = **28 bytes** (not 24).
- POLL: 4 + 1 + 1 + 2 + 16 + 4 (reserved3) = **28 bytes** (not 24).
- DELIVER: 4 + 1 + 1 + 2 + 16 + 16 (peer_ip) + 2 (port) + 2 (reserved2) = **44 bytes** (not 32).

The 16-byte `peer_ip` field with the IPv4-mapped-IPv6-OR-raw-IPv4 alternation is also genuinely ambiguous. Node.js `dgram.rinfo.address` returns either dotted-quad (`"192.168.1.1"`, on udp4 sockets) or IPv4-mapped (`"::ffff:192.168.1.1"`, on udp6 dual-stack), depending on bind type. Our `s_work.stun.public_ip` is always dotted-quad. The hairpin gate (`strcmp` against `s_work.stun.public_ip`) silently fails open if forms disagree.

**Impact:** A reader cannot resolve the wire-format math from the document alone. The 16-byte `peer_ip` field doubles DELIVER size for no IPv6 benefit (IPv6 is out of scope per `reference-mister-network-stack.md`). Step 4's `buflen >= 32` check (correct against the new sizes) doesn't match the original §Decision 2 figure.

**Disposition:** Addressed in §Decision 2 (rewrite to declared offsets, REGISTER/POLL = 28 bytes, DELIVER = 32 bytes; `peer_ip` = 4 raw bytes IPv4 in network byte order; explicit server bind/parse spec via `dgram.createSocket('udp4')` + `inet_pton`/`inet_ntop`); §Step 3 `Rendezvous_BuildRegister/Poll` updated to `out_pkt[28]`; `Rendezvous_ParseDeliver` adds the 32-byte length check and `inet_ntop` formatting.

---

#### NEW-2 — Threading model: SDL3_net's `pending_output` queue is unsynchronized; "rendezvous thread sends only" is unsafe

**Claim in plan (pre-revision):** §Decision 3 stated "the rendezvous thread ONLY sends through the same socket — SDL3_net tolerates cross-thread send while the main thread is the sole reader."

**Actual:** `NET_SendDatagram` and `NET_ReceiveDatagram` both call `PumpDatagramSocket` (`/private/tmp/sdl_net_ref/src/SDL_net.c:2015`), which mutates `sock->pending_output`, `sock->pending_output_len`, and `sock->pending_output_allocation` from any thread that calls send or recv, with no socket-level locks. Concurrent send (from the rendezvous thread) and recv (from the main thread) can corrupt the heap pointer queue, not just lose packets. None of our existing callers (`stun.c`, `direct_p2p.c`, `netplay.c`, `sdl_net_adapter.c`, `matchmaking.c`) does cross-thread send/recv.

**Impact:** Race on the heap-allocated send queue. POSIX UDP socket-level guarantees do not apply — the bug is at the SDL3_net library layer above the kernel.

**Disposition:** Addressed in §Decision 3 (rewritten to specify a single-producer/single-consumer atomic ring `s_rendezvous_send_q` drained inline by `DirectP2P_Tick`; rendezvous thread enqueues only and never touches the socket); §Step 5 `host_rendezvous_thread_fn` reflects the queue. §Step 5 "If it fails" updated to address queue-overflow rather than fallback. §Open question 1 (cross-thread send safety) struck and marked resolved.

---

#### NEW-3 — Hairpin gate normalization: `strcmp` fails open on form mismatch

**Claim in plan (pre-revision):** §Step 5 DELIVER handler used `strcmp(peer_ip, s_work.stun.public_ip) == 0` for the hairpin short-circuit.

**Actual:** With NEW-1 ensuring DELIVER `peer_ip` is dotted-quad, `strcmp` works in the steady state, but defense in depth matters: any future code path that produces an IPv4-mapped form (e.g., a STUN server returning `::ffff:1.2.3.4` in a different code path) silently disables the hairpin gate.

**Impact:** Latent failure-open bug; cheap to fix.

**Disposition:** Addressed in §Step 5 (replace `strcmp` call with `direct_p2p_ip_eq_normalized` helper that uses `inet_pton(AF_INET, ...)` on both sides and compares the resulting `uint32_t`); §Hard requirement 3(c) updated with "compare normalized — never `strcmp` on possibly-prefixed string forms."

---

#### NEW-4 — Cancel semantics: `SDL_DetachThread` + spin-for-IDLE has a write-after-free race

**Claim in plan (pre-revision):** Cancel-semantics changes were not specified; the plan inherited the existing `SDL_DetachThread(s_thread)` at `direct_p2p.c` line 644 and the spin-for-IDLE loop at lines 659-661. *[Those two line numbers are as this review found them and have no live equivalent: S5a removed the detach outright, and `DirectP2P_Cancel` now sets the three cancel atomics and `SDL_WaitThread`s every worker handle — `src/netplay/direct_p2p.c:4354-4375`.]*

**Actual:** Today's `DirectP2P_Cancel` spins up to 500 ms for `get_state() == DIRECT_P2P_IDLE`, then unconditionally tears down `s_work` via `memset` (`direct_p2p.c:795`) and the STUN socket. If the worker is mid-`Stun_HolePunch` post-receive write at `stun.c:445-446` (the 3-punch follow-up after a successful receive) when the 500 ms grace expires, the worker writes freed memory after teardown. The bilateral path's worst-case 11-second worker lifetime makes this race more likely.

**Impact:** Pre-existing race that the bilateral feature aggravates. Worth fixing while we're in the file.

**Disposition:** Addressed in §Step 5 (drop `SDL_DetachThread` at `:644`, retain handle, switch `DirectP2P_Cancel` to set cancel atomic + `SDL_WaitThread` then teardown; same pattern for new rendezvous and bilateral threads). Worker-lifetime note cites `stun.c:407-411` cancel honor and `stun.c:454` 10 ms loop.

---

#### NEW-5 — Step 1 cites four files in `/tmp/3sxtra/tools/lobby-server/`; directory is empty

**Claim in plan (pre-revision):** §Step 1 "Read first" listed `/tmp/3sxtra/tools/lobby-server/lobby-server.js`, `lobby-server.service`, `deploy.sh`, `package.json`. §Step 1 "Changes" said "same shape as `/tmp/3sxtra/tools/lobby-server/deploy.sh`" and "similar to `/tmp/3sxtra/tools/lobby-server/lobby-server.service`."

**Actual:** `ls /tmp/3sxtra/tools/lobby-server/` returns no files (verified 2026-04-26 — directory exists but is empty). The `Stun_EncodeEndpoint` upstream pairing claim in §Decision 1 also cited `lobby-server.js:362`, which is unreachable.

**Impact:** Implementer cannot follow the "Read first" guidance and cannot inspect the "same shape as" reference.

**Disposition:** Addressed in §Step 1 (drop the four citations; replace with a note that the upstream tree is empty as of 2026-04-26 and design comes from §Decision 2 directly; concrete systemd-unit spec inlined: single ExecStart, Restart=always, User=rendezvous, NoNewPrivileges=true, PrivateTmp=true, ProtectSystem=strict). §Decision 1 (B-evaluation) softened to remove the upstream `lobby-server.js:362` citation.

---

### P-2 (new)

#### NEW-6 — DNS resolution failures should fast-fail, not consume the 8-second budget

**Claim in plan (pre-revision):** Neither §Step 5 host_rendezvous_thread_fn nor the joiner inline path specified hostname-resolve behavior.

**Actual:** The existing `stun.c:272-275` shows the canonical pattern: 100 ms-bounded poll on `NET_GetAddressStatus`. Without it the 8-second budget is silently spent on DNS retries.

**Impact:** Misconfigured `signal-url` (e.g., typo, dead host) causes 8-second wait before bilateral failure, instead of <1 s.

**Disposition:** Addressed in §Step 5 (resolve once at thread start using the existing 100 ms poll; on failure, host stays HOST_WAITING / joiner transitions to FAILED_BILATERAL immediately). §Decision 7 Test 6 added.

---

#### NEW-7 — Server rate-limit Map sweeper missing; per-IP counters leak

**Claim in plan (pre-revision):** §Step 1 "Changes" said "per-source-IP rate limit (10 pkt/s)" without specifying TTL or sweep cadence.

**Actual:** A naive `Map<ip, counter>` with no eviction grows unboundedly under any /16 scan. The session-key map already has a 60 s TTL sweep; the rate-limit map needs the same.

**Impact:** Server RAM bloats over time; trivially DoS-able by an attacker scanning IPv4 source IPs.

**Disposition:** Addressed in §Step 1 "Changes" (sliding-window counter; entries swept every 60 s, same TTL as the session map).

---

#### NEW-8 — User-facing status strings use em-dashes; SF3 glyph table renders them as missing-glyph boxes

**Claim in plan (pre-revision):** §Decision 9 status table used em-dashes (U+2014) in `"Symmetric NAT — coordinating via rendezvous..."` and `"Symmetric NAT — simultaneous hole punch..."`.

**Actual:** Existing ASCII-only status strings at `direct_p2p.c:546, :434` set the precedent. The native game text path (`SSPutStrPro`) renders SF3's bespoke glyph table; em-dashes are not in the glyph set.

**Impact:** Status text would render as missing-glyph boxes on the device.

**Disposition:** Addressed in §Decision 9 (em-dashes replaced with ASCII hyphens in both rows; new "Glyph constraint - ASCII only" subsection enforcing the rule for all status strings).

---

#### NEW-9 — No idle UX after rendezvous-thread budget expires on host

**Claim in plan (pre-revision):** §Step 5 stated host stays at HOST_WAITING after the 8-second rendezvous budget expires (correct) but did not change the displayed status. User sees "Waiting for peer..." indefinitely with no signal that the rendezvous attempt timed out.

**Actual:** The post-budget state is qualitatively different from the pre-budget state — we tried the fallback and the peer never showed up. Users on old builds would never trigger this; users on new builds would only trigger it if their peer is also new but doesn't run REGISTER.

**Impact:** Confusing UX; user may wait forever without realizing the fallback failed.

**Disposition:** Addressed in §Decision 9 (new row for post-budget HOST_WAITING with status `"Waiting for peer (no peer detected - check that they're using a recent build)."`); §Step 5 `host_rendezvous_thread_fn` task to set the status before exiting.

---

#### NEW-10 — Session-key derivation: missing zero-IP guard + ambiguous pack semantics

**Claim in plan (pre-revision):** §Step 3 `Rendezvous_DeriveSessionKey` signature was `(uint32_t ip_be, uint16_t public_port, uint8_t out_key[16])` with no specification of the 6-byte payload layout or input validation.

**Actual:** `ipv4_str_to_be` at `direct_p2p.c:369-376` returns 0 on parse failure (and 0.0.0.0 isn't a routable public address either). If the caller passes `ip_be == 0`, every offline peer derives the same session key (`SHA-256(0x00000000:port)`), causing all of them to collide on the rendezvous server.

**Impact:** Real cross-talk hazard if STUN fails silently; needs an explicit guard in the caller.

**Disposition:** Addressed in §Step 3 (explicit pack semantics — `payload[0..3] = ip_be`, `payload[4..5] = htons(public_port)` — and caller responsibilities: host MUST abort REGISTER if `ipv4_str_to_be(public_ip) == 0`).

---

#### NEW-11 — §Decision 1 `Stun_EncodeEndpoint` upstream-pairing claim is unverifiable

**Claim in plan (pre-revision):** §Decision 1 (B-evaluation) said "their room code IS `"ip|public_port|local_port"` per `Stun_EncodeEndpoint`" with a citation to `/tmp/3sxtra/tools/lobby-server/lobby-server.js:362`.

**Actual:** `grep -r "Stun_EncodeEndpoint" /tmp/3sxtra/` returns zero hits (the function exists in our repo at `src/netplay/stun.c:40` but not upstream); the cited `lobby-server.js:362` is unreachable (NEW-5).

**Impact:** Conclusion ("we'd be coupling on undocumented schema") is correct but the cited evidence is wrong.

**Disposition:** Addressed in §Decision 1 (claim softened: "their server stores the room_code as an opaque string field; the format is not visible in the local checkout"; `lobby-server.js:362` citation dropped).

---

### Nits (new)

#### NEW-12 — Stale line-number citations

**Stale:**
- `Stun_CloseSocket` cited as `stun.h:41` (in this review file's "Stun API" verified-correct list); actual is `stun.h:28`.
- `main.c` test dispatch cited as `:912-967` (in plan §Step 6 and review's verified-correct list); actual is `:937-1000`.
- `join_thread_fn` cited as `:372-457` (in plan §Decision 4 and review's verified-correct list); actual is `:372-458`.

**Disposition:** Plan corrected for `main.c:937-1000` (§Step 6) and `join_thread_fn:372-458` (§Decision 4 and review-disposition table). The `Stun_CloseSocket stun.h:28` correction belongs to this review file's "Stun API" bullet under "Things verified correct"; flagging here for transparency rather than retroactively editing the original review (per the "keep existing review content untouched — append only" constraint).

**Additional nit:** Original review's Things-verified-correct list (line 136) labels `Stun_HolePunch` at `stun.h:28` and `Stun_CloseSocket` at `stun.h:41`; verified swapped — `Stun_CloseSocket` is at `stun.h:28`, `Stun_HolePunch` at `stun.h:41-42`. The plan §Decision 12 fix already corrected the canonical citation; this is just a footnote on the original review.

---

## Re-verification 2026-04-26 (round 2)

A third deep verification pass surfaced 19 additional findings against the post-round-1 plan: 4 P-1, 7 P-2, 5 consistency, 3 nits. Each was applied as a revision to `plan-bilateral-hole-punch.md`. Cross-references point to the affected plan section. Findings are labeled P-1.A..D / P-2.1..7 / IC-1..5 / Nit-1..3 to distinguish them from the original P-1/P-2/Nit set and the round-1 NEW-N set.

### P-1 (round 2)

#### P-1.A — Joiner-side hairpin gate missing from FALLBACK_SIGNALING entry

**Claim in plan (pre-round-2):** §Step 5 join_thread_fn fallback bullet at `plan-bilateral-hole-punch.md:551` only checked the kill-switch and `direct_p2p_is_lan_peer(s_work.peer_ip)` before transitioning to `FALLBACK_SIGNALING`.

**Actual:** On the joiner, `s_work.peer_ip` is the host's public IP decoded from the room code. If both peers are on the same LAN behind a router that doesn't NAT-loopback, `direct_p2p_is_lan_peer` returns false on a public IP and the joiner REGISTERs with the rendezvous server — violating Hard Requirement 3(c). The host-side gate at the DELIVER handler already covers the host; the joiner needs the symmetric gate.

**Disposition:** Addressed in §Step 5 (extended to a three-way OR including `direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)`); §Hard requirement 3(c) updated.

---

#### P-1.B — Host/join label swap on `SDL_DetachThread`; three-thread cleanup model under-specified

**Claim in plan (pre-round-2):** the §Step 5 cancel-semantics bullet (`docs/plan-bilateral-hole-punch.md:586`; cited as :554 at the time) said "Drop `SDL_DetachThread(s_thread)` at `direct_p2p.c` line 644 (host)". The NEW-4 disposition row (`docs/plan-bilateral-hole-punch.md:907`; cited as :773 at the time) repeated the same label. The `direct_p2p.c` numbers throughout this section are review-time positions — see §NEW-4 for where the code went.

**Actual:** Verified against `src/netplay/direct_p2p.c`:
- `:603` is `SDL_DetachThread(s_thread)` inside `DirectP2P_BeginHost` (host detach).
- `:604` is `s_thread = NULL` after the host detach.
- `:644` is `SDL_DetachThread(s_thread)` inside `DirectP2P_BeginJoin` (join detach).
- `:645` is `s_thread = NULL` after the join detach.
- `:117` declares `s_thread` as a single static handle.

The plan's labels for host/join were swapped, and the three-thread cleanup model (`s_thread`, `s_rendezvous_thread`, `s_bilateral_punch_thread` coexist on the host path during FALLBACK_BILATERAL_PUNCH) was implicit.

**Disposition:** Addressed in §Step 5 cancel-semantics bullet (corrected to `:603-604` host / `:644-645` join; spelled out three-thread cleanup model and disjoint cancel atomics); NEW-4 disposition row updated.

---

#### P-1.C — `Rendezvous_Send` test seam has no production interpose point

**Claim in plan (pre-round-2):** §Step 6 Test 4 described "function pointer for `Stun_HolePunch` override + a `Rendezvous_Send` override". After round-1's main-thread send-drain queue rework, no `Rendezvous_Send` function exists.

**Actual:** The host enqueues, the main thread drains via inline `NET_SendDatagram`, the joiner sends directly from its worker thread. Test 4 has no interpose point.

**Disposition:** Addressed in §Step 3 (`Rendezvous_Send` helper added to the `rendezvous.{c,h}` API list as a thin wrapper around `NET_SendDatagram`); §Step 5 host and join paths route through `Rendezvous_Send`; §Step 6 Test 4 updated.

---

#### P-1.D — Drop unverifiable `/tmp/3sxtra/src/netplay/lobby_server.{c,h}` citations

**Claim in plan (pre-round-2):** `plan-bilateral-hole-punch.md:44` cited `/tmp/3sxtra/src/netplay/lobby_server.{c,h}` as the verification source. `plan-bilateral-hole-punch-review.md:143` (Things verified correct) asserted both `lobby_server.c` and `sha256.{c,h}` exist there.

**Actual:** `find /tmp/3sxtra -type f` returns zero files (verified 2026-04-26). The directory exists but is empty everywhere underneath, including `tools/lobby-server/`, `src/netplay/`, and the entire upstream tree.

**Disposition:** Addressed in §Decision 1 B-evaluation block (reframed to cite the prior project memory note rather than unreadable files); review file gains an append-only correction in "Things verified correct" noting the lobby_server.c / sha256.{c,h} entries are no longer reproducible.

---

### P-2 (round 2)

#### P-2.1 — `s_rendezvous_send_q` spec under-defined

**Claim in plan (pre-round-2):** §Decision 3 described `s_rendezvous_send_q` loosely as "single-producer/single-consumer atomic ring" with "max 4 outstanding sends" elsewhere; slot type, atomics, overflow behavior, drain rate were not concrete.

**Disposition:** Addressed in §Decision 3 (concrete spec: 8 slots, slot type with `NET_Address* target` + `payload[28]` + ownership rules, `SDL_AtomicInt` head/tail, drop-on-overflow with `s_q_drops` telemetry, drain ≤4 slots per tick); §Step 5 "If it fails" reconciled to match.

---

#### P-2.2 — `stun.c:386` "same idiom" claim is wrong

**Claim in plan (pre-round-2):** §Step 5 host_rendezvous_thread_fn DNS-resolve bullet cited `stun.c:386` as "same idiom".

**Actual:**
- `stun.c:272-275` is `while (... < 100) { SDL_Delay(1); }` — 100ms-bounded.
- `stun.c:385-389` is `while (... < 300) { SDL_Delay(10); }` — 3000ms-bounded. Different idiom.

**Disposition:** Addressed in §Step 5 (parenthetical replaced with correct distinction).

---

#### P-2.3 — `src/configuration.h` missing from Step 6 file list

**Claim in plan (pre-round-2):** §Step 6 file list named `src/args.c`, `src/main.c`, `src/netplay/test_bilateral_punch.c`, and `src/netplay/direct_p2p.c`, but the new `bool test_bilateral_punch` field belongs on the `Configuration` struct in `src/configuration.h:61-99`.

**Disposition:** Addressed in §Step 6 "Changes" (new bullet for `src/configuration.h`).

---

#### P-2.4 — Bilateral-punch thread local-copy discipline missing

**Claim in plan (pre-round-2):** §Step 5 host_bilateral_punch_thread_fn bullet did not specify how peer_ip / peer_public_port are passed into `Stun_HolePunch`.

**Actual:** `direct_p2p.c:539-541` already shows the join-side worker copies peer_ip / peer_port into stack-locals before calling `Stun_HolePunch` so the in-place overwrite at `stun.c:438,442` doesn't race with main-thread reads. The bilateral-punch thread on the host needs the same pattern.

**Disposition:** Addressed in §Step 5 (note added to mirror `direct_p2p.c:539-541`).

---

#### P-2.5 — Teardown blocking analysis missing

**Claim in plan (pre-round-2):** §Step 5 `direct_p2p_on_teardown` bullet said "ensure the rendezvous thread and bilateral-punch thread are cancelled + joined before returning" without quantifying the worst-case blocking time.

**Disposition:** Addressed in §Step 5 (worst-case ~510ms documented: rendezvous between sends ~500ms + bilateral inside `Stun_HolePunch`'s 10ms loop; 1-second deadline fallback added).

---

#### P-2.6 — Step 1 systemd unit incomplete

**Claim in plan (pre-round-2):** §Step 1 systemd-unit description listed only `ExecStart`, `Restart`, `User`, `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem` directives without `[Unit]` or `[Install]` sections.

**Actual:** Without `[Install] WantedBy=multi-user.target` the deploy script's `systemctl enable` fails.

**Disposition:** Addressed in §Step 1 (full unit body inlined: `[Unit] After=network-online.target Wants=network-online.target`, `[Service]` body, `[Install] WantedBy=multi-user.target`).

---

#### P-2.7 — Step 3 spurious dependency on Step 2

**Claim in plan (pre-round-2):** §Step 3 "Depends on:" line said "Step 2 (for `CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL` — the URL parser needs to exist before Step 5 calls it)."

**Actual:** Step 3's deliverable (the pure `rendezvous.{c,h}` module) does not import from `config.h`; `Rendezvous_ParseSignalUrl` takes the URL as a `const char*` argument. Step 5 consumes the config key. The dependency is logical, not source-level.

**Disposition:** Addressed in §Step 3 ("Depends on: none"; clarification added).

---

### Consistency (round 2)

#### IC-1 / IC-2 — `/tmp/3sxtra/` empty recursively, not just `tools/lobby-server/`

**Disposition:** Covered by P-1.D — both plan and review edits explicitly note the empty state covers `/tmp/3sxtra/` recursively (zero `.c`/`.h` files anywhere underneath).

---

#### IC-3 — Drain-rate vs. queue-depth wording drift

**Disposition:** Covered by P-2.1's concrete spec (queue depth = 8, drain rate up to 4 per tick); §Step 5 "If it fails" line in the plan rewritten to drop "max 4 outstanding" framing.

---

#### IC-4 — Decision 3 revision metadata bleeding into operational text

**Claim in plan (pre-round-2):** §Decision 3 "Net effect" paragraph contained "Decision 3's earlier wording … was incomplete; the corrected rule is …" — revision history mixed into the operational rule.

**Disposition:** Addressed in §Decision 3 (revision-meta sentence stripped; corrected rule stands on its own; revision history remains in the disposition table).

---

#### IC-5 — Worker-lifetime note covered join only

**Claim in plan (pre-round-2):** §Step 5 "Worker-lifetime note" only documented the join-side worker lifetime extension.

**Disposition:** Addressed in §Step 5 (parallel sentence added for the host worker at `direct_p2p.c:381, exits at :361-365`; clarifies that rendezvous and bilateral-punch threads are independent of `s_thread`).

---

### Nits (round 2)

#### Nit-1 — tf-psa-crypto link-line citation conflated path with link

**Claim in plan (pre-round-2):** §Step 3 CMakeLists.txt bullet said "tf-psa-crypto is already linked (`CMakeLists.txt:246`)".

**Actual:** `CMakeLists.txt:451` is `set(TF_PSA_CRYPTO_ROOT ...)` — the path declaration. The link line is `CMakeLists.txt:483` (`target_link_libraries(... "${TF_PSA_CRYPTO_ROOT}/lib/libtfpsacrypto.a")`).

**Disposition:** Addressed in §Step 3 (citation split: `:451` declaration, `:483` link).

---

#### Nit-2 — Test URL reserved-TLD form

**Claim in plan (pre-round-2):** §Decision 7 Test 6 used `udp://invalid.tld.example:3478`.

**Actual:** `.example` is the RFC 2606 reserved TLD; `.tld.example` is just a subdomain. The shorter `udp://invalid.example:3478` is the canonical form.

**Disposition:** Addressed in §Decision 7 Test 6.

---

#### Nit-3 — Original review's Stun_HolePunch / Stun_CloseSocket swap

**Disposition:** Per append-only convention, original review entry at `plan-bilateral-hole-punch-review.md:138` is untouched. Appended one-line nit to the existing "Re-verification 2026-04-26" section: `Stun_CloseSocket` is at `stun.h:28`, `Stun_HolePunch` at `stun.h:41-42`.
