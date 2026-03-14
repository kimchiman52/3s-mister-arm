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

## Memory Index

- Load [docs/building.md](/Users/sb/Developer/3sx-mister/docs/building.md) when you need baseline host build commands, MiSTer profile setup, or the desktop-vs-MiSTer build split.
- Load [docs/mister-runbook.md](/Users/sb/Developer/3sx-mister/docs/mister-runbook.md) when building, packaging, deploying, probing, or perf-sampling the MiSTer runtime on device.
- Load [docs/mister-wrapper.md](/Users/sb/Developer/3sx-mister/docs/mister-wrapper.md) when working on the `3SX.rbf` + `MiSTer_3SX` wrapper-core path, wrapper packaging, or wrapper deploy/smoke commands.
- Load [docs/agent-memory/mister-remote-safety.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-remote-safety.md) when touching MiSTer deploy helpers, remote command wrappers, or docs that show remote file mutation.
- Load [docs/agent-memory/mister-wrapper-quartus.md](/Users/sb/Developer/3sx-mister/docs/agent-memory/mister-wrapper-quartus.md) when touching `3SX.rbf`, Quartus setup, Apple Silicon host strategy, or wrapper-core build failures.
- Load [docs/config.md](/Users/sb/Developer/3sx-mister/docs/config.md) when changing config keys, defaults, or user-facing scale/software-frame behavior.
- Load [artifacts/mister-port/living-findings.md](/Users/sb/Developer/3sx-mister/artifacts/mister-port/living-findings.md) when doing MiSTer performance work or revisiting previously-tested optimization ideas; skip it for non-performance feature work.
- Load [docs/mister-port-plan.md](/Users/sb/Developer/3sx-mister/docs/mister-port-plan.md) when re-evaluating stock MiSTer platform constraints, dependency strategy, or custom-image vs stock-image architecture decisions.
