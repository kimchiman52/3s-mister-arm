# MiSTer Performance Memory

Last curated: 2026-03-10

## Purpose

- Use this file for durable MiSTer performance context that should survive beyond a single optimization loop.
- Keep loop journals, raw captures, and ad hoc experiment history in `artifacts/`, not here.

## When To Load This

- When changing MiSTer renderer or present-path code.
- When planning or interpreting MiSTer perf captures.
- When deciding whether a change belongs in the `telemetry` or `clean` package.
- Skip for launcher-only, packaging-only, or loop-bookkeeping changes.

## Fast Path

- Entry points: `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/sdl_app.c`, `src/port/sound/adx.c`, `tools/mister/perf-sampler.sh`, `tools/mister/misterctl.sh`.
- Build/deploy procedure: follow [docs/mister-runbook.md](../mister-runbook.md).
- Build flavors:
  - `telemetry` is the default developer flavor for perf iteration and parity checks.
  - `clean` is the player-facing runtime package and the final handoff build.
- Capture rule: start gameplay triage with `tools/mister/perf-sampler.sh --perf-basic`.
- Escalate to full telemetry only when you need task-family attribution or runtime wait-state debugging.

## Durable Decisions

- MiSTer keeps `software-frame-mode = on` by default because validated direct fbdev present outperformed the old readback path without accepted gameplay regressions.
- Keep both package flavors. `telemetry` preserves perf tooling and parity checks; `clean` strips those paths out for player validation.
- Prefer `tools/mister/misterctl.sh` for deploy, probe, and smoke checks, and prefer `tools/mister/perf-sampler.sh` for captures. They are the canonical MiSTer entry points.
- Treat `--perf-basic` as the default gameplay metric. Full per-frame telemetry is measurably intrusive on MiSTer and can create fake regressions.
- Keep the attract/demo runtime wait-state telemetry in tree. It is measurement support for the logo-overlay lane, not gameplay logic.

## Validated Hotspots

- Ibuki stage fallback lane:
  - Signature: `present_readback` spikes caused by three repeated `256x256` sheared textured background quads.
  - First place to inspect: the narrow `rect_uv_parallelogram` software-frame path in `src/port/sdl/sdl_game_renderer.c`.
- Attract/demo logo lane:
  - Signature: slowdown begins when the centered game logo overlays attract-mode gameplay.
  - First place to inspect: small affine or translated textured-quad handling in `src/port/sdl/sdl_game_renderer.c`, plus the runtime wait-state plumbing in `src/main.c` and `src/port/sdl/sdl_app.c`.
- Genei-Jin lane:
  - Signature: on the canonical non-Ibuki path, slowdown is render-heavy but still direct-presented, dominated by `software_frame_fast_non_integer` with a smaller `generic_textured` residue.
  - Caveat: on Ibuki stage, shared stage-background quads can dominate instead, so do not assume every Genei-Jin slowdown is the same lane.

## Known Pitfalls

- A broad `title` or `opening` capture is not the same as the attract-demo logo overlay. Use the dedicated runtime wait state before drawing conclusions.
- Full telemetry can distort gameplay measurements enough to look like a regression. Recheck with `--perf-basic` before changing runtime code.
- Stage-specific regressions can come from shared background geometry, not only from the player effect you are testing.
- Raw loop records in `artifacts/mister-port/` are useful research history, but they are not the canonical source for what should be merged to `mister`.

## Canonical References

- [docs/mister-runbook.md](../mister-runbook.md): build, container, deploy, and MiSTer launch flow.
- [docs/config.md](../config.md): shipped MiSTer-facing config defaults.
- [artifacts/mister-port/living-findings.md](../../artifacts/mister-port/living-findings.md): full loop-era research history and rejected experiment record.
