# MiSTer Remote Safety Agent Memory

## Purpose

- Use this file when touching MiSTer deploy helpers, remote command wrappers, or docs that show file-mutating commands.
- It records the guardrails added after an accidental destructive sync against the MiSTer filesystem.

## When To Load This

- When editing `/Users/sb/Developer/3s-mister-arm/tools/mister/*`.
- When editing `/Users/sb/Developer/3s-mister-arm/tools/mister-wrapper/*` packaging or deploy flows.
- When adding docs/examples that show `rsync`, `scp`, `ssh`, `rm`, or remote config rewrites.
- Skip for local-only gameplay/render changes that never touch MiSTer automation.

## Fast Path

- Entry points: `/Users/sb/Developer/3s-mister-arm/tools/mister/mister-common.sh`, `/Users/sb/Developer/3s-mister-arm/tools/mister/misterctl.sh`, `/Users/sb/Developer/3s-mister-arm/tools/mister/perf-sampler.sh`.
- Check lock owner first with `tools/mister/misterctl.sh lock-status` when another agent/worktree may be using the same target.
- Check remote activity with `tools/mister/misterctl.sh busy-status` before deploy/probe/smoke work when another workflow may already own the live target state.
- Safe remote delete scope: `/media/fat/games/3s-arm/` only.
- Safe wrapper copy-only targets: `/media/fat/MiSTer_3S-ARM` and `/media/fat/_Other/3S-ARM.rbf`.
- Override valve: `MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1` plus `MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=<exact-path>` only for deliberate test trees.
- Collision valve: `MISTER_ALLOW_BUSY_TARGET=1` only when you deliberately accept running against a target that already shows active `3s-arm`, `MiSTer_3S-ARM`, transfer, or launcher processes.
- Raw remote shell valve: `MISTER_ALLOW_REMOTE_EXEC=1` is required before using `tools/mister/misterctl.sh exec`.
- Perf tags must stay simple basenames; do not derive remote temp or log paths from arbitrary user text.

## Durable Decisions

- Decision: delete-capable syncs must be scoped to the owned runtime subtree only. | Why: broad `/media/fat` deletes can wipe unrelated MiSTer content. | Date: `2026-03-10`.
- Decision: wrapper deploys may copy into `/media/fat`, but delete is only allowed inside `/media/fat/games/3s-arm/`. | Why: wrapper assets own only three targets and should not mutate the rest of the card. | Date: `2026-03-10`.
- Decision: nonstandard remote roots require an explicit unsafe override. | Why: path typos like `/media/fat` vs `/media/fat/games/3s-arm` are high-impact. | Date: `2026-03-10`.
- Decision: nonstandard remote roots require a typed exact-path confirmation in addition to the unsafe flag. | Why: a broad boolean override alone is too easy to leave set accidentally. | Date: `2026-03-10`.
- Decision: arbitrary `misterctl.sh exec` is opt-in only. | Why: it bypasses all path-scoping protections. | Date: `2026-03-10`.
- Decision: perf helper data staging must restore the live `SF33RD.AFS` on exit. | Why: perf capture is not allowed to leave the device in a mutated content state. | Date: `2026-03-10`.
- Decision: MiSTer deploy/probe/smoke commands must refuse to run when the target already appears busy unless an explicit override is set. | Why: the local shared lock only covers tooling that cooperates on the same host, but the live target can still be mutated by another workflow. | Date: `2026-03-10`.

## Known Pitfalls

- `rsync --delete` aimed at `/media/fat/` or another broad root -> reject it or narrow the destination to `/media/fat/games/3s-arm/`.
- Docs that say “sync to /media/fat root” without naming the owned targets -> rewrite them to list exact targets and delete scope.
- Generic helpers like `mister_rsync_expect` can hide delete semantics -> validate the destination path before calling them.
- Perf/probe scripts also mutate remote files -> validate `--remote-root` even when the command is “just sampling”.
- Parallel worktrees on the same machine only serialize if both use the shared tooling -> prefer `misterctl.sh` / `perf-sampler.sh` over ad hoc `ssh` or `rsync`.
- The local `/tmp` lock is not enough by itself when another host or ad hoc process is touching the MiSTer -> use `busy-status`, and keep the busy-process regex conservative unless you validate a narrower rule.
- `--copy-afs` that uploads over the live archive without a restore path -> back up the remote file first and restore it in cleanup.
- Path-like perf tags -> reject them before deriving remote filenames or temp paths.
- Local build cleanup like `rm -rf build/...` is acceptable only for repo-owned output dirs -> keep local and remote risk clearly separated.

## Update Rules

- Keep this file focused on durable remote-safety rules, not one-off recovery steps.
- If a new helper can delete or overwrite remote files, add its safe scope here.
- Last verified: `2026-03-10`.
