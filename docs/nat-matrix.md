# NAT traversal matrix — measured results

The numbers the relay-removal decision rests on. Until this file existed they
lived only in task briefs and agent reports, which is why superseded grids are
still quoted around the tree (see *Superseded grids* at the bottom).

This is a **permanent** record, not a work note: it sits next to the harness
that produces it (`tools/netplay/natmatrix/`) so a future reader can re-derive
it rather than trust it.

---

## What was measured

Four emulated NAT types on each side, every ordered pair — **16 cells**. The
type list is the harness default, `tools/netplay/natmatrix/run_matrix.sh:39`:

```
fullcone   addr-restricted   port-restricted   symmetric
```

Two independent instruments, deliberately separate:

| instrument | what it answers | entry point |
|---|---|---|
| **cascade** (`p2p_probe`) | did this pairing CONNECT | `tools/netplay/natmatrix/run_matrix.sh` |
| **mechanism** (raw sockets, no SDL, no state machine) | was the DATAGRAM admitted by the NAT | `tools/netplay/natmatrix/mech_matrix.sh`, modelling `tools/netplay/natmatrix/rig/punch_mech.py` |

The split is the point, and `mech_matrix.sh:6-9` states it: *"a failure here is
a NAT property and a failure only there is a code property."*

---

## Result — cascade, `--reps 3`

**13 / 16 cells connect.** The three that do not:

| host | joiner | terminal state |
|---|---|---|
| `port-restricted` | `symmetric` | `FAILED_BILATERAL` |
| `symmetric` | `port-restricted` | `FAILED_BILATERAL` |
| `symmetric` | `symmetric` | `FAILED_BILATERAL` |

All three land in `DIRECT_P2P_FAILED_BILATERAL`, i.e. the joiner reached a real
cascade terminal state rather than hanging or dying earlier — which is what
makes the negative meaningful rather than a broken rig.

## Result — mechanism (raw sockets)

The raw-socket grid is **strictly weaker** than the cascade: it fails two cells
the cascade recovers — `fullcone × symmetric` and `addr-restricted ×
symmetric`.

**This is the bilateral punch earning its place.** Both peers punching, with
the joiner's DELIVERed endpoint armed the instant the DELIVER parses
(`src/netplay/direct_p2p.c:2089`, `race_arm_punch` leg 1; the wire
order is spelled out in `tools/netplay/natmatrix/rig/punch_mech.py:11-22`), opens a path that a
single-sided raw punch cannot. It was invisible until rig and product were
measured **separately** — with one instrument, a cell that the cascade rescues
and the NAT would otherwise refuse simply reads as "connected", and the credit
goes nowhere.

---

## Non-vacuity — why the grid is evidence at all

`tools/netplay/natmatrix/nonvacuity.sh` runs **first** in `run_all.sh`
(`run_all.sh:34`), because a matrix where only the easy cells connect cannot
distinguish "the cascade succeeded" from "the rig never carried a packet". Four
checks, each closing one way this harness could be a lie
(`nonvacuity.sh:10-26`):

- **A — positive control.** `port-restricted × port-restricted` must CONNECT.
- **B — true negative.** `symmetric × symmetric` must NOT connect, *and* STUN
  must still resolve, so a rig that dropped all traffic is caught rather than
  scored as a NAT result.
- **C — sabotage discrimination.** Take the cell that passed (A), blackhole
  only the peer-to-peer path, leave the servers reachable: it must flip to
  `NOT_CONNECTED`.
- **D — build guard.** Compiling the probe without `-DNETPLAY_TEST_HOOKS` must
  FAIL, so the exit-2 "not compiled in" false pass is structurally impossible.

Plus the instrument-freshness rule: `assert_probe_fresh`
(`tools/netplay/natmatrix/nonvacuity.sh:59`, and the identical copy at
`tools/netplay/natmatrix/run_matrix.sh:84`) refuses a probe older than any
source it links, **with no override**.

That rule exists because it happened. The probe binary failed to link on this
branch from `d207ef1e` (2026-08-29 12:39) to `fd1fa3cc` (2026-08-30 04:38),
and every driver kept running the stale one and exiting 0 for sixteen hours.

---

## Reproducing

Linux with `ip netns` and passwordless `sudo -n` (the drivers call
`sudo -n ip netns exec`). Not reproducible on macOS.

```sh
# 1. build the probe (must be newer than every src/netplay source it links)
cmake -S tools/netplay/natmatrix/probe -B /tmp/probe-build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/probe-build -j8

# 2. the full set: non-vacuity, then the matrix, then the impairment sweeps
tools/netplay/natmatrix/run_all.sh \
    --probe /tmp/probe-build/p2p_probe --reps 3 --out /tmp/s8results

# 3. the raw-socket control, same grid, no SDL and no cascade
tools/netplay/natmatrix/mech_matrix.sh --reps 3 --out /tmp/s8mech/results.jsonl

# 4. summarize
python3 tools/netplay/natmatrix/summarize.py /tmp/s8results/baseline.jsonl
```

`run_all.sh` propagates the worst stage rc (`run_all.sh:24-31`, restored
after it too used to run off its end). **`mech_matrix.sh`
does not** — it runs off the end at `mech_matrix.sh:112` and exits 0 regardless
of per-rep rc. Read its JSONL, do not trust its exit code. (Open item; see
`docs/queue.md`.)

---

## Provenance, and what is NOT verified here

- **Tree.** Branch `upstream-engine-fixes`, at the tip carrying the task #126
  S8 probe work. The last commits that can move these numbers are
  `d3dcb13a` ("give port-restricted and symmetric an inbound path"),
  `ef50a268`, `b3f7114a` and `50b81965`; `c99bb4b1` is a citation repoint and
  cannot.
- **COULD NOT VERIFY in the session that wrote this file:** the run's own
  output artifacts (`/tmp/s8results/*.jsonl`) were not available to re-read,
  and neither driver stamps a commit id into its JSONL. The grid above is
  transcribed from the run report, not re-derived. **Anyone re-running it
  should record the `git rev-parse HEAD` of the tree they built the probe from
  in this section** — the freshness guard proves the probe matches the tree, but
  nothing yet records *which* tree.
- The "five no-gateway baselines" quoted elsewhere could not be located in the
  harness as a named set; `nonvacuity.sh` defines four checks (A–D). Treat any
  count other than four as unsourced until someone points at the code.

## Superseded grids

Earlier 16-cell grids appear in task briefs. Their `port-restricted` rows were
**rig artifacts** — the emulated port-restricted NAT had no inbound path until
`d3dcb13a` — so those rows understate the cascade. Do not quote them; this file
is the current record.
