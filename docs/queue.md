# Work queue — TEMPORARY, dated 2026-08-30

> **DELETE THIS FILE WHEN THE QUEUE IS EMPTY.** It exists only because the
> session's task tooling was withdrawn mid-flight and the queue had nowhere
> else to live. A stale tasklist checked into a repo is worse than none: it
> reads as current long after it stops being true.
>
> **Staleness test, runnable:** every line number below was verified against
> `dcf7631a` on `upstream-engine-fixes`. Run
> `git log --oneline dcf7631a..HEAD -- src/netplay tools/rendezvous-server`
> — if that prints anything, re-verify before trusting a citation here.
> If every entry is closed, `git rm docs/queue.md`.

Findings and citations only. No narrative, no status prose.

---

## #130 — slot-reclaim staleness (MEDIUM, auth)

Both port-reclaim arms fire on same-IP **plus** `portReclaims < MAX_PORT_RECLAIMS`
with **no staleness precondition**, so they repoint a *live* slot — the host's
included:

- slot B arm — `tools/rendezvous-server/rendezvous-server.js:1184-1186`
- slot A arm — `tools/rendezvous-server/rendezvous-server.js:1198-1200`
- the cap — `tools/rendezvous-server/rendezvous-server.js:171` (`MAX_PORT_RECLAIMS = 8`)

What a reclaimer actually proves: same public IP, knowledge of the session key,
and return-routability at **its own new endpoint**. `cookieForSlot`
(`rendezvous-server.js:612`) does **not** mix the session key — stated in the
file's own comment at `:154` and `:449` — so it does **not** prove
original-party identity.

The comment at `rendezvous-server.js:160-164` understates this: it says the cap
"bounds the churn rather than carrying a security property" and frames the
residual as denial-of-room, not slot steering.

Off-path attackers stay fully blocked. Composed with late-punch's own
8-relearn cap, a CGNAT-co-located room-code holder can steer both halves —
inside the trust boundary the room code already defines, but the two caps are
**independent counters, neither gating the other**.

---

## #131 — review batch (five items)

1. **`ORCH_HOST_LADDER_MS` undercounts the ladder by 600 ms.**
   It adds `WORKER_STARTUP_DELAY_MS` **once**
   (`src/netplay/direct_p2p.c:6151`), but the S2 ladder **respawns**
   `host_thread_fn` (`src/netplay/direct_p2p.c:5854`), whose first statement
   is that same delay (`src/netplay/direct_p2p.c:3516`, definition at
   `:3507`). Four spawns ⇒ **+600 ms**. Assert `[D]`'s `86450`
   (`src/netplay/direct_p2p.c:6280`, derivation at `:6260`) should be
   `87050`. Absorbed by the 5000 ms nav margin, so **not user-facing** — but
   it contradicts the block's own charter, which says the number "must be
   COMPUTED from the legs the code enforces"
   (`src/netplay/direct_p2p.c:6033-6036`) and lists the delay as occurring at
   "both sites" (`:6040-6041`).
2. **Late-punch borrows a socket with no invalidation hook.**
   `s_sock` is borrowed and never closed (`src/netplay/late_punch.c:26`, set
   at `:63`, used at `:163` and `:178`). `Netplay_SetStunSocket`
   (`src/netplay/netplay.c:2727-2733`) destroys the previous socket, which
   could dangle it. **No trace was constructed — judgment only.** Cheap
   defensive disarm.
3. **`check_key_rate_budget.py` is looser than the prose it guards.**
   It enforces `per-key cap >= 23`
   (`tools/rendezvous-server/check_key_rate_budget.py:9`, constraint at
   `:61-62`), while the server's own derivation claims a 3:1 design giving 30
   (`tools/rendezvous-server/rendezvous-server.js:326-336`). Green for any cap
   in [23, 29].
4. **`notePushLost` logs unthrottled.**
   `tools/rendezvous-server/rendezvous-server.js:1043` calls `logWarn`
   directly, unlike every other hot path in the file, which routes through
   `noteThrottled`.
5. **Doc drift, three sites.**
   - `src/netplay/netplay_nav.c:133` says "120.2 s"; the enforced host bound
     is 86,450 ms (`src/netplay/direct_p2p.c:6260`).
   - `src/netplay/direct_p2p.c:2643` says "10 s ceiling"; the enforced
     `PORTMAP_PROBE_BUDGET_MS` is 11,250 ms (`:382`).
   - `tools/netplay/gen_plw_canon_fields.py:21-24` inverts its own
     descend/emit rule in the docstring.

---

## #132 — coverage plan

**Priority 1**
- `classify_peer_payload` (`src/netplay/mist_handshake.c:314`) and the four
  payload readers it uses — `read_cstr` `:236`, `read_u8` `:255`, `read_u16be`
  `:263`, `read_u64be` `:271`, plus `parse_header` `:210`. This is the
  compat/desync gate and has **zero direct tests**. Blocked by `static` + a
  file global; extract via the `mist_handshake_gate_next` trampoline precedent
  at `src/netplay/mist_handshake.c:894`.
- `Stun_IsBindingResponse` (`src/netplay/stun.c:1028`).
- Late-punch pure cases: cap saturation, wrong-IP-no-answer, answer targets the
  learned port.

**Priority 2**
- The `ORCH_*` cascade — `ORCH_HOST_WORST_CASE_MS`
  (`src/netplay/direct_p2p.c:6162`) — table-driven **from independently listed
  constants, never importing the macro under test**.
- natpmp timeout math, refactored to take a caller-supplied clock.

**Priority 3**
- Extract ~13 pure blocks out of `src/netplay/test_bilateral_punch.c`.

**Non-goals, deliberately:** the punch race, split brain, the PROMISED
LISTENING INTERVAL, the reclaim boundary, GekkoNet/rollback determinism, UPnP.
All interaction bugs, where a mocked unit passes vacuously.

---

## Smaller open items

- **#129** — ROM search trim + a legible miss message.
- **#125** — `--test-connect-observability` flakes **only under concurrent gate
  runs**, always at `test6-byte-budget`; shared logs directory.
- **#128** — five reproducible file-load failures (file numbers 9, 10, 1454,
  1456, 1458) plus 379 ms startup outliers. Pre-existing.
- **#108** — `--test-enable` pins PS2, so no corpus run exercises the shipping
  engine; `Present_Mode` 4/5 hides the select screen.

---

## Open structural gaps

- `tools/netplay/natmatrix/mech_matrix.sh:112` falls off the end of the file
  and exits 0 regardless of per-rep `rc` — the last statement is an `echo`,
  with no stage-rc propagation of the kind `tools/netplay/natmatrix/run_all.sh:27-31` grew.

**Closed 2026-08-30 (task #122 test commit), recorded so the list does not
re-open them:**

- `src/netplay/test_sparse_effect_save.c` printed "not compiled **with**" where
  `tools/gates/run-gates.sh:179` greps "not compiled **in**". Fixed. The same
  evasion was found and fixed in `src/test/test_texcash_bounds.c`.

---

## Needs the user at the machine

- TV + pad confirmation of the shortened select countdown — measured **at the
  source of the rendered digits, never at the glass**.

---

*Verified at `dcf7631a` (`upstream-engine-fixes`), i.e. with task #122's
client-half (`bfa20e56`) and test (`dcf7631a`) commits applied. Re-verify every
line number if the tree has moved.*
