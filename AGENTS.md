# Agent Notes

## Safety

- Remote MiSTer filesystem mutations are high risk. Treat `rsync --delete`, `rm -rf`, broad `scp` copies, and remote config rewrites as dangerous until the destination scope is proven.
- Never target delete-capable syncs at `/media/fat`, `/media`, `/`, or another shared remote root. Limit destructive syncs to owned subtrees such as `/media/fat/games/3sx/`.
- For wrapper deploys, only `/media/fat/MiSTer_3SX`, `/media/fat/_Utility/3SX.rbf`, and `/media/fat/games/3sx/` are owned targets. Delete scope belongs only inside `/media/fat/games/3sx/`.
- When the MiSTer may be shared with another agent or worktree, check `tools/mister/misterctl.sh lock-status` and `tools/mister/misterctl.sh busy-status` before deploy/probe/smoke work. Prefer local-only progress until the target is clearly idle.
- If a task truly requires a nonstandard remote root, require both a boolean unsafe override and a typed exact-path confirmation. Do not accept a bare “unsafe mode” toggle for delete-capable operations.
- When touching MiSTer deploy helpers or docs, add path validation and dry-run guidance before adding convenience shortcuts.
- Do not use `tools/mister/misterctl.sh exec` unless the task truly requires raw remote shell access; safer purpose-built subcommands are preferred.

## Workflow

- For MiSTer implementation/iteration work, prefer [`mister-ivrfc-loop`](/Users/sb/.codex/skills/mister-ivrfc-loop/SKILL.md) over the generic [`start-implementation`](/Users/sb/.codex/skills/start-implementation/SKILL.md) flow when the MiSTer-specific loop fits the task.
- Use [`start-implementation`](/Users/sb/.codex/skills/start-implementation/SKILL.md) for non-MiSTer work or for MiSTer tasks that clearly fall outside the `mister-ivrfc-loop` scope.
- For MiSTer runtime builds on fresh agents, do not start with a host-local `cmake -B build/mister` flow. Default to `tools/mister/build-game.sh --flavor telemetry` unless the task explicitly needs the clean package or the host is already native ARM Linux.
- For mature MiSTer perf queues, use [docs/agent-memory/mister-ralph-loop-v2.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-ralph-loop-v2.md) to choose the right loop type (`runtime`, `measurement`, or `workload-fidelity`) before starting another Ralph pass.

## Memory Index

- Load [docs/building.md](/Users/sb/Developer/3sx-mister/docs/building.md) when you need baseline host build commands, MiSTer profile setup, or the desktop-vs-MiSTer build split.
- Load [docs/mister-runbook.md](/Users/sb/Developer/3sx-mister/docs/mister-runbook.md) when building, packaging, deploying, probing, or perf-sampling the MiSTer runtime on device.
- For MiSTer Docker runtime builds, default immediately to `tools/mister/build-game.sh --flavor telemetry` unless the task explicitly needs the player-facing clean package. That helper is the canonical path for fresh agents because it produces real ARM MiSTer outputs on common host setups instead of a host-arch `PORT_MISTER=ON` build.
- Load [docs/mister-wrapper.md](/Users/sb/Developer/3sx-mister/docs/mister-wrapper.md) when working on the `3SX.rbf` + `MiSTer_3SX` wrapper-core path, wrapper packaging, or wrapper deploy/smoke commands.
- Load [docs/agent-memory/mister-remote-safety.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-remote-safety.md) when touching MiSTer deploy helpers, remote command wrappers, or docs that show remote file mutation.
- Load [docs/agent-memory/mister-wrapper-quartus.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-wrapper-quartus.md) when touching `3SX.rbf`, Quartus setup, Apple Silicon host strategy, wrapper-core build failures, or rebuilding the Colima VM. The "VM Recreation" section has the step-by-step rebuild procedure and explains why agents must invoke `build-hps.sh`/`build-core.sh` rather than `make`/`quartus_sh` directly.
- Load [docs/agent-memory/mister-native-analog-crt.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-native-analog-crt.md) when revisiting scaler-off analog CRT output, `svideo`/`cvbs` color loss, or native analog wrapper/video-path cleanup.
- Load [docs/agent-memory/mister-ralph-loop-v2.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-ralph-loop-v2.md) when planning, reranking, or repairing the Ralph perf process itself.
- Load [docs/agent-memory/mister-ralph-working-brief.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-ralph-working-brief.md) first when starting a new Ralph perf loop on the active queue.
- Load [docs/agent-memory/mister-ralph-working-brief-template.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-ralph-working-brief-template.md) when creating or refreshing the small working brief for the active Ralph queue.
- Load [docs/agent-memory/mister-perf-deep-research-2026-03-21.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-perf-deep-research-2026-03-21.md) when reranking native super-art/Yun/Genei Ralph loops, validating whether a candidate is actually new, or revisiting the post-loop-149 deep-research claims.
- Load [docs/config.md](/Users/sb/Developer/3sx-mister/docs/config.md) when changing config keys, defaults, or user-facing scale/software-frame behavior.
- Load [artifacts/mister-port/living-findings.md](/Users/sb/Developer/3sx-mister/artifacts/mister-port/living-findings.md) when you need archived Ralph loop evidence, exact rejection history, or old closeout details; do not treat it as the default working brief for new perf loops.
- Load [docs/mister-port-plan.md](/Users/sb/Developer/3sx-mister/docs/mister-port-plan.md) when re-evaluating stock MiSTer platform constraints, dependency strategy, or custom-image vs stock-image architecture decisions.
- Load [docs/agent-memory/mister-geneijin-rendering.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-geneijin-rendering.md) when working on Yun SA3 (Genei-Jin) rendering, effect reduction, burst-window optimization, or investigating what renders the activation visual effects. Permanent reference with game code pointers, confirmed findings, and perf data.
- Load [docs/agent-memory/mister-sa3-effect-reduction-handoff.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-sa3-effect-reduction-handoff.md) when picking up SA3 effect reduction work mid-session. Temporary handoff doc with dirty code state, what's deployed, and immediate next steps. Delete when diagnostic phase is complete.
