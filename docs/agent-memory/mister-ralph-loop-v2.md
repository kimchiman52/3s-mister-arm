# MiSTer Ralph Loop V2

## Purpose

- Use this file to run MiSTer performance loops with better throughput once the easy wins are gone.
- Treat it as the operational process layer above the historical loop log and below scene-specific research memos.

## When To Load This

- When planning or reranking a Ralph loop.
- When deciding whether the next loop should be runtime, measurement, or workload/fidelity work.
- When a queue has logged multiple rejects or docs-only closeouts and needs a pivot rule.
- Skip for non-performance feature work, wrapper-core work, or one-off device transport fixes.

## Fast Path

- Load order for a fresh loop: `docs/agent-memory/mister-ralph-working-brief.md`, `artifacts/mister-port/stock-image-software-frame-loop-series/todo.md`, `docs/agent-memory/mister-perf-deep-research-2026-03-21.md`, `artifacts/mister-port/living-findings.md` only for archive lookup, then `tools/mister/perf-sampler.sh`.
- Default capture rule: start with `tools/mister/perf-sampler.sh --perf-basic`.
- Minimal device verification: `tools/mister/misterctl.sh lock-status`, `tools/mister/misterctl.sh busy-status`, `tools/mister/misterctl.sh health`, `deploy`, `probe`, bounded `smoke`.

## Loop Types

- `runtime`: tests a runtime code change against trusted lanes. Do not add new telemetry unless that telemetry path is already proven low-distortion on the deciding lane.
- `measurement`: improves or validates instrumentation only. Judge it on distortion budget, selector correctness, and future leverage, not on runtime speedup.
- `workload-fidelity`: reduces or reshapes burst workload without changing gameplay timing, logic, determinism, or input semantics. Treat this as first-class work, not a fallback.

## Durable Decisions

- Decision: keep IVRFC safety, rollback, and on-device verification discipline. | Why: those rules produced the big accepted wins and keep risk bounded. | Date: `2026-03-22`
- Decision: stop treating every telemetry gap as permission to invent new in-band collectors on the runtime critical path. | Why: recent collectors can dominate the deciding lane and invalidate the loop itself. | Date: `2026-03-22`
- Decision: split Ralph work into explicit `runtime`, `measurement`, and `workload-fidelity` loops. | Why: mature queues need different success criteria than early broad-win exploration. | Date: `2026-03-22`
- Decision: after two failed runtime bets in the same idea family, force a queue pivot to a different lever class. | Why: repeated branch-layout or micro-reland retries consume loop budget without widening the search space. | Date: `2026-03-22`
- Decision: prefer workload-level or user-approved fidelity reductions over another helper micro-opt when the remaining lane is still far from stable target performance. | Why: the bigger remaining gap is often workload size, not one last inner-loop branch. | Date: `2026-03-22`
- Decision: keep a small Ralph working brief and treat `living-findings.md` as archive/history, not default working context. | Why: the log is too large to serve as the routine prompt payload without anchoring and token drag. | Date: `2026-03-22`

## Ralph V2 Rules

- Start every loop by declaring its type: `runtime`, `measurement`, or `workload-fidelity`.
- Name one deciding lane, at least one non-regression gameplay guard, and one stop condition before editing.
- Each loop must close one bounded diff or one bounded docs-only pivot. Do not let the same dirty runtime implementation drift across multiple loop IDs.
- If a dirty runtime diff already exists at kickoff, either revert it before planning or explicitly adopt it as the sole diff under test for that loop.
- For `runtime` loops, prefer one bounded code change over a family of adjacent tweaks.
- For `measurement` loops, define an explicit distortion budget before implementation.
- Do not solve a telemetry gap by silently widening a `runtime` loop into `measurement` work. Close or pivot into a separate `measurement` loop instead.
- If a measurement mode materially changes route, frame-time class, or first-window shape on the deciding lane, reject it immediately and do not use it for reranking.
- If the queue records two straight rejects or docs-only closures in the same idea family, pivot to a different lever class before another reland.
- Do not spend a runtime loop reopening a rejected family unless the new evidence is genuinely different, not a narrower label on the same bet.
- When a user-approved fidelity tradeoff exists, rank it against micro-opts honestly instead of treating it as a last resort.

## Distortion Budget

- `--perf-basic` is the default comparator.
- Any richer capture mode must prove it stays acceptably close to the trusted `--perf-basic` lane before it can guide runtime decisions.
- Compare richer capture modes against an unchanged-tree baseline first.
- Reject a collector if its own bookkeeping becomes a dominant reported subphase or if it more than modestly perturbs the deciding lane.
- Prefer off-path or post-window exports over in-band per-pixel or per-row accounting on hot lanes.

## Statistics Protocol

- Keep a small set of trusted comparator lanes and reuse them.
- For close calls, run at least two unchanged reruns and two candidate reruns on the deciding lane.
- Judge close calls on median frame time plus worst-frame behavior, not a single narrative mean.
- Keep a standing A/A rerun note for each trusted lane so later agents know the normal variance band.
- Treat large wins or large losses as decisive without over-spending device time on extra confirmation.

## Review Protocol

- Keep the independent review pass, but scope it narrowly to the authored diff.
- If `codex review --uncommitted` stalls, fall back immediately to a bounded diff-only review path rather than burning loop time on repo traversal.
- For docs-only closures, review the closure diff for factual consistency, final hashes, and selector/capture claims.
- For measurement loops, review selector scope and metric semantics before chasing runtime interpretation.

## Working Set Policy

- Maintain one small Ralph working brief with the current queue, trusted baselines, deciding lanes, banned families, current top candidates, and recent decisive wins and rejects.
- Update the working brief after each meaningful keep/reject/pivot.
- Keep `living-findings.md` as append-only evidence and historical lookup.
- If a point only matters for archive lookup, it belongs in `living-findings.md`, not the brief.

## Candidate Ranking Heuristics

- Prefer candidates that change the largest remaining cost center, not just the easiest helper to patch.
- Prefer candidates that attack a whole workload class over candidates that shave a tiny subfamily.
- Prefer candidates that preserve or improve measurement clarity.
- Demote candidates that require new in-band metadata on the hottest path.
- Promote candidates with user approval when they trade non-gameplay visual fidelity for stable speed.

## Known Pitfalls

- In-band collectors on hot lanes can become the experiment instead of measuring it -> validate collectors on unchanged runtime first.
- A “new” micro-opt can still be the same rejected family under a narrower label -> check the deep-research memo and no-retry list first.
- Giant loop history is useful archive, but poor default context -> load a brief first and drill into history only when needed.
- Manual review is acceptable when automation stalls, but only after constraining the review scope to the actual diff.
- Repeated docs-only closures can feel productive while the search space stagnates -> treat them as a signal to pivot, not as proof the process is still exploring well.

## Update Rules

- Last verified: `2026-03-22`.
- Keep this file short and operational.
- Replace stale process rules instead of appending contradictory ones.
- Update this file only when the Ralph process itself changes, not for every loop outcome.
