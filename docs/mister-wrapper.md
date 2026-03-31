# MiSTer Wrapper Core

This document tracks the operator-facing shape of the 3SX wrapper-core work.

## Phase 1 Goal

Phase 1 adds a real MiSTer core identity for 3SX without changing gameplay logic:

- `3SX.rbf` provides the real MiSTer core name `3SX`
- `MiSTer_3SX` is a Main_MiSTer-derived HPS executable launched through `[3SX] main=MiSTer_3SX`
- the existing `/media/fat/games/3sx` runtime remains the actual game binary and data path

The current HPS build path fetches the pinned full `Main_MiSTer` upstream tree into `build/`,
then overlays the local 3SX-specific files from the curated `vendor/Main_MiSTer` subset before
building. That keeps the checked-in vendor footprint small without depending on the old stub-heavy
slim-source build.

## Planned Install Layout

```text
/media/fat/
  MiSTer_3SX
  _Other/
    3SX.rbf
  games/
    3sx/
      bin/3sx
      lib/*
      resources/SF33RD.AFS
      launch-osd.sh
      run-3sx.sh
      logs/
```

## Planned Config Snippet

```ini
[3SX]
main=MiSTer_3SX
```

The `[3SX]` section only needs `main`:

```ini
[3SX]
main=MiSTer_3SX
```

If the user's global MiSTer settings have `vga_scaler=1`, override it to 0 in the
`[3SX]` section. When `vga_scaler=1`, the FPGA routes the HDMI scaler's plain RGB
to the VGA DAC, bypassing the core video path and YC encoder — causing grayscale
S-Video and wrong aspect ratio on CRT. Do not add `video_mode` overrides — native
video timing is controlled by the core.

## Launch Contract

The core-driven path is not allowed to hand-wave away the current MiSTer launcher behavior.
`MiSTer_3SX` preserves the important parts of the existing
`/media/fat/games/3sx/launch-osd.sh` contract while directly `execve()`-launching
`/media/fat/games/3sx/bin/3sx`:

- force SDL onto dummy video plus software renderer for stock MiSTer startup
- keep startup and exit logs under `/media/fat/games/3sx/logs`
- preserve and restore the active console on exit/failure
- handle `INT`, `HUP`, and `TERM` cleanly
- avoid manual `openvt`/`chvt` launch hops in the core-driven path
- keep `launch-osd.sh` on disk as the fallback launcher, but do not invoke it from the core-driven path

The shell script itself remains as the fallback runtime launcher during phase 1.

## Current Hardware Finding

Real-device validation has now ruled out the HPS wrapper as the primary cause of the current
black-screen failure on CRT. The latest wrapper builds successfully proved all of the expected
handoff conditions on hardware:

- `set_vga_fb=1`
- `video_chvt=2`
- `video_fb_enable=1`
- `video_fb_state=1`
- active VT `tty2`
- changing `/dev/fb0`
- `MiSTer_fb` mode `8888 1 384 240 1536`

Despite that, the old MemTest-derived `3SX.rbf` still produced a black CRT image. The next
wrapper-core step is therefore core-side, not HPS-side. The wrapper-core build path now standardizes
on a `Menu_MiSTer`-derived seed so the core can inherit the same framebuffer/CRT substrate as the
known-good menu launch path.

## Wrapper Script Entrypoints

These reserved names are intentionally created early so the operator surface is fixed before the
implementation-heavy chunks land:

- `tools/mister-wrapper/build-hps.sh`
- `tools/mister-wrapper/build-core.sh`
- `tools/mister-wrapper/build-quartus-image.sh`
- `tools/mister-wrapper/package-wrapper.sh`
- `tools/mister-wrapper/build-release.sh`
- `tools/mister-wrapper/publish-release.sh`

Chunk 1 only establishes these entrypoints. Their real build/package logic lands in later chunks.

## Pinned HPS Foundation

Phase 1 uses a tracked local overlay subset derived from upstream `Main_MiSTer`:

- local path: `vendor/Main_MiSTer`
- pinned commit: `3380931329b8acb442bd3d35a24d89f88641b7cf`
- metadata file: `vendor/Main_MiSTer.UPSTREAM.md`
- overlay file manifest: `tools/mister-wrapper/main-mister-overlay.files`

The full upstream source now stays out of git and is fetched on demand during the HPS build. The
checked-in subset exists only to carry the 3SX-specific overlays and the Docker fallback context.

## Verified HPS Build Path

The wrapper HPS entrypoint is `tools/mister-wrapper/build-hps.sh`.

Current verified behavior:

- it clones the pinned upstream `Main_MiSTer` tree into `build/mister-wrapper-hps/src`
- it overlays the local 3SX-customized files listed in
  `tools/mister-wrapper/main-mister-overlay.files`
- it applies the wrapper menu bridge patch from
  `tools/mister-wrapper/main-mister-full-menu.patch`
- it copies `tools/mister-wrapper/Makefile.full.3sx` into the staged tree and builds with
  `make -f Makefile.3sx`
- it writes the final artifact to `build/mister-wrapper-hps/MiSTer_3SX`
- it prefers a local `arm-none-linux-gnueabihf` toolchain when present
- otherwise it falls back to Docker and builds inside an image derived from
  `vendor/Main_MiSTer/.devcontainer/Dockerfile`

Current local overlay boundary:

- wrapper-local sources:
  - `threesx_main.cpp`
  - `threesx_wrapper.cpp`
  - `threesx_wrapper.h`
  - `threesx_core_context.cpp`
  - `threesx_core_context.h`
- locally carried upstream deltas:
  - `fpga_io.cpp`
  - `fpga_io.h`
  - `user_io.cpp`
  - `user_io.h`
  - `video.cpp`
  - `video.h`
- build-time patch-only delta:
  - `menu.cpp`
  - `menu.h`

Verified commands for Chunk 2:

```sh
tools/mister-wrapper/build-hps.sh --check-env
tools/mister-wrapper/build-hps.sh
```

On this host, the validated path is the Docker fallback because the MiSTer cross-toolchain is not
installed locally.

## Intended Fork Surface

The default rule for `vendor/Main_MiSTer` is "overlay-only unless 3SX handoff requires otherwise."

Current local customization boundary:

- build-time fetch, overlay, and patch application stay in `tools/mister-wrapper/build-hps.sh`
- the checked-in `vendor/Main_MiSTer` tree keeps only the files listed in
  `tools/mister-wrapper/main-mister-overlay.files`
- if a future wrapper need proves another upstream delta is required, prefer either:
  - one more overlay file in the local subset, or
  - one small build-time patch against fetched upstream source

The goal is to keep future upstream sync possible while avoiding both a full vendored upstream tree
and the broken old slim-source build.

## Current Runtime Handoff

The current wrapper implementation lives in `vendor/Main_MiSTer/threesx_wrapper.cpp` and is
entered through `vendor/Main_MiSTer/threesx_main.cpp`, but it now builds against fetched full
upstream `Main_MiSTer` source plus the local overlay set.

Current implemented behavior:

- seed the MiSTer-side `3SX` core identity explicitly before `cfg`/video init so `[3SX]`
  `MiSTer.ini` video overrides still apply without running the full `user_io_init()` path
- validate `/media/fat/games/3sx/bin/3sx` and `/media/fat/games/3sx/resources/SF33RD.AFS` before launch
- launch the runtime as a direct `execve()` child instead of invoking `launch-osd.sh`
- set `THREESX_HOME`, `LD_LIBRARY_PATH`, `SDL_VIDEODRIVER`, `SDL_VIDEO_DRIVER`, and
  `SDL_RENDER_DRIVER` explicitly for the child
- write wrapper lifecycle events to `/media/fat/games/3sx/logs/osd-wrapper.log`
- capture runtime stdout/stderr in `/media/fat/games/3sx/logs/last-run.log`
- forward `INT`, `HUP`, and `TERM` to the child and restore the active console before exit
- on normal child exit or startup failure, restart back to `MiSTer` with `menu.rbf`
- on missing files or `execve()` failure, show a simple OSD error before returning to menu
- keep `launch-osd.sh` on disk strictly as a fallback shell path outside the core-driven launch

## Core Template Seed

The wrapper-core build path now supports one pinned official seed:

- supported seed: `Menu_MiSTer`
  - local path: `vendor/Menu_MiSTer`
  - pinned commit: `b0a2b9298d7a7a355e4e0a97277d3d4218eb2f55`
  - metadata file: `vendor/Menu_MiSTer.UPSTREAM.md`

Why `Menu_MiSTer` is the supported path:

- hardware validation ruled out the HPS wrapper as the primary blocker
- the earlier MemTest-derived core still produced a black CRT image
- `Menu_MiSTer` is the known-good MiSTer framebuffer/CRT family already used by the working
  menu/script path
- the staged Menu project still keeps the visible core identity at a simple top-level `CONF_STR`,
  which makes the `3SX` rename straightforward

## Verified Core Build Prep Path

The wrapper core entrypoint is `tools/mister-wrapper/build-core.sh`.

Current verified behavior:

- it accepts `--seed menu` (default) and `--fast`
- `--fast` appends `vendor/Menu_MiSTer/menu-fast.qsf` to the project QSF, overriding
  aggressive optimization defaults with faster-compile settings (~50-70% compile time
  reduction, ~10-20% fMAX trade-off). Suitable for dev iteration; omit for release builds.
- it stages the selected seed into `build/mister-wrapper-core/src`
- it renames the Quartus project from the seed basename (`menu`) to `3SX`
- it patches:
  - `3SX.qpf` project revision
  - `files.qip` top-level source references
  - `3SX.sv` `CONF_STR` so the core identifies as `3SX`
- it reserves `build/mister-wrapper-core/3SX.rbf` as the final artifact path
- it can use either:
  - a local Quartus install when `quartus_sh` and `quartus_cpf` are on `PATH`
  - a separate `linux/amd64` Docker image for Quartus 17 when the MiSTer runtime container is the
    wrong architecture for FPGA builds

The dedicated Quartus image entrypoint is `tools/mister-wrapper/build-quartus-image.sh`.
It expects an installer directory containing:

- `QuartusLiteSetup-17.0*.run` or `QuartusSetup-17.0*.run`
- `cyclone-17.0*.qdz`
- `cyclonev-17.0*.qdz`

Current Docker defaults for the Quartus image path:

- image name: `3sx-mister-wrapper-quartus17`
- platform: `linux/amd64`
- base image: `ubuntu:20.04`
- install dir: derived from the selected installer edition unless overridden

Future agents doing Quartus host setup or `.rbf` failure diagnosis should load
`docs/agent-memory/mister-wrapper-quartus.md` before changing this path.

Verified commands on this host:

```sh
tools/mister-wrapper/build-core.sh --seed menu --prepare-source
tools/mister-wrapper/build-core.sh --fast --seed menu --prepare-source
tools/mister-wrapper/build-quartus-image.sh --help
```

Validated full-build path on this Apple Silicon host:

```sh
# Dev iteration (fast compile, ~50-70% faster):
colima --profile quartus2 ssh -- bash -lc '
  export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C &&
  cd /Users/sb/Developer/3sx-mister &&
  OUTPUT_DIR=/home/sb.linux/build/mister-wrapper-core \
  bash tools/mister-wrapper/build-core.sh --fast --seed menu
'

# Release (full optimization):
colima --profile quartus2 ssh -- bash -lc '
  export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C &&
  cd /Users/sb/Developer/3sx-mister &&
  OUTPUT_DIR=/home/sb.linux/build/mister-wrapper-core \
  bash tools/mister-wrapper/build-core.sh --seed menu
'
```

Current verified Quartus findings on this host:

- A clean Quartus Prime Lite 17.0 install with both `cyclone-17.0*.qdz` and
  `cyclonev-17.0*.qdz` now completes the full `build-core.sh` flow through
  `quartus_map`, `quartus_fit`, `quartus_asm`, and `quartus_sta`, which disproves the earlier
  `Error (20004)` conclusion. The original Lite failure was caused by an incomplete device-package
  install, not by Lite Edition itself.
- Quartus Prime Standard 17.0 also gets past the device-family gate, but if that path is used it
  may require `LM_LICENSE_FILE` / `MISTER_QUARTUS_LICENSE_FILE` depending on the installed
  feature set and license environment.
- On Apple Silicon macOS, Docker Desktop `linux/amd64` image builds hit a Rosetta-side installer
  failure (`rosetta error: bss_size overflow`). The validated workaround is an x86_64 Linux VM
  (`colima --arch x86_64 --vm-type qemu`) or another real Linux/x86_64 machine.

That means the preferred path for a real `3SX.rbf` build is Quartus Lite 17.0 plus both Cyclone
device packs on an x86_64 Linux host or VM. The remaining wrapper gates are now on-device deploy
and launch validation, not Quartus build viability.

Most recent local validation:

- the Menu-derived core build completed on the validated `quartus2` x86_64 VM and produced
  `build/mister-wrapper-core/3SX.menu.rbf`
- the HPS wrapper rebuild completed locally and updated `build/mister-wrapper-hps/MiSTer_3SX`
- a local wrapper package assembled successfully at `build/mister-wrapper-package-menu`
- an artifacts-only deploy of that Menu package updated `/media/fat/MiSTer_3SX` and
  `/media/fat/_Other/3SX.rbf` on the MiSTer without touching `/media/fat/games/3sx`
- forced wrapper probe still passes on the new artifacts, but it continues to inherit the SSH-side
  `320x240` fbdev path, so the next meaningful gate remains a real OSD launch on the CRT

Most recent device validation:

- real OSD/core launch is now proven through the wrapper path, not just the forced probe path
- `wrapper-status` captured a normal wrapper launch with:
  - `forced_mode=0`
  - `core_name=3SX`
  - `rbf_name=3SX`
  - `FBDEV: active (384x240 stride=1536 bpp=32)`
- that confirms the `[3SX] main=MiSTer_3SX` handoff on the actual device launch path
- current caveat: the game's in-game "Exit Game" behavior appears to be a soft reset back into the
  title/game flow, not a process exit, so leaving the wrapper back to MiSTer still needs a
  deliberate runtime or wrapper-level exit path

When installer files are available, the expected Quartus Docker flow is:

```sh
export MISTER_QUARTUS_INSTALLER_DIR=/path/to/quartus17-installer-files
tools/mister-wrapper/build-core.sh --build-image
tools/mister-wrapper/build-core.sh
```

If you choose the Standard installer instead, set `LM_LICENSE_FILE` or
`MISTER_QUARTUS_LICENSE_FILE` as needed and `build-core.sh` will forward it into the container.

## Wrapper Package And Deploy Path

The wrapper package entrypoint is `tools/mister-wrapper/package-wrapper.sh`.

Current implemented behavior:

- assemble a `/media/fat`-rooted tree at `build/mister-wrapper-package`
- place `MiSTer_3SX` at the package root
- place `3SX.rbf` under `_Other/`
- copy the existing runtime package into `games/3sx/`

The player-facing release entrypoint is `tools/mister-wrapper/build-release.sh`.

Current implemented behavior:

- rebuild the player-facing runtime package from `build/mister-clean-install` via `tools/mister/build-runtime-package.sh`
- assemble a FAT-rooted stage at `build/mister-release/stage`
- copy `README-3SX-INSTALL.txt` to the ZIP root
- fail if the staged release contains `games/3sx/resources/SF33RD.AFS`, `games/3sx/config`,
  `games/3sx/keymap`, or `games/3sx/logs`
- write the final player ZIP to `build/mister-release/3SX-mister-rolling-pre-release.zip`

Player install flow:

- extract the ZIP directly onto the MiSTer SD card root
- add a legally obtained `SF33RD.AFS` at `/media/fat/games/3sx/resources/SF33RD.AFS`
- add `[3SX] main=MiSTer_3SX` to `MiSTer.ini` if the section is not already present

The local release publish entrypoint is `tools/mister-wrapper/publish-release.sh`.

Current implemented behavior:

- move the `rolling-pre-release` tag to the current commit
- create or update that GitHub pre-release with `gh`
- replace only the existing `3SX-mister-*.zip` asset set on that release

Current remote tooling additions:

- `tools/mister/misterctl.sh busy-status`
- `tools/mister/misterctl.sh configure-3sx-ini`
- `tools/mister/misterctl.sh deploy-wrapper --src <wrapper-package-dir>`
- `tools/mister/misterctl.sh deploy-wrapper --src <wrapper-package-dir> --artifacts-only`
- `tools/mister/misterctl.sh probe-wrapper`
- `tools/mister/misterctl.sh smoke-wrapper`
- `tools/mister/misterctl.sh run-wrapper --runtime-arg <arg>...`
- global `--remote-fat-root` override for wrapper deploys

Wrapper deploys now sync only the wrapper-owned targets:

- `/media/fat/MiSTer_3SX`
- `/media/fat/_Other/3SX.rbf`
- `/media/fat/games/3sx/`

Delete-scoped sync is only allowed inside `/media/fat/games/3sx/`, and `games/3sx/resources/SF33RD.AFS`
plus the on-device `games/3sx/config` and `games/3sx/keymap` files are preserved the same way the
existing runtime-only deploy path preserves `resources/SF33RD.AFS`.

When another workflow is actively updating `/media/fat/games/3sx`, use `deploy-wrapper --artifacts-only`
to stage just the wrapper-owned root artifacts (`MiSTer_3SX` and `_Other/3SX.rbf`) without replacing
the runtime tree.

Remote safety guardrails:

- `deploy-wrapper` now rejects nonstandard `--remote-fat-root` values unless both `MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1` and `MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=<exact-path>` are set deliberately.
- Even with that override, do not target `/`, `/media`, or another shared root. The wrapper owns only the three paths listed above.
- `configure-3sx-ini` only accepts `/media/fat/MiSTer.ini` and `/media/fat/MiSTer_*.ini` by default, takes a timestamped backup before editing, and still checks `busy-status` before it writes.
- `deploy-wrapper`, `probe-wrapper`, `smoke-wrapper`, and `run-wrapper` now perform a remote busy preflight and refuse to run if the target already shows an active `3sx`, `MiSTer_3SX`, `launch-osd.sh`, `run-3sx.sh`, `rsync`, `scp`, or `sftp-server` process. Use `busy-status` first when another agent may be active, and set `MISTER_ALLOW_BUSY_TARGET=1` only when you intentionally accept that collision risk.
- Use `probe-wrapper` and `smoke-wrapper` for validation; do not reach for raw `misterctl.sh exec` unless `MISTER_ALLOW_REMOTE_EXEC=1` is intentionally enabled for one-off maintenance.

`probe-wrapper` runs the existing runtime `--probe-renderer-only` self-check through
`MiSTer_3SX` by invoking:

- `/media/fat/MiSTer_3SX /media/fat/_Other/3SX.rbf '' --probe-renderer-only`

with `THREESX_WRAPPER_FORCE=1` set on the remote side so the wrapper path can be exercised from SSH
without going through OSD core selection first.

Validated on device as of `2026-03-10`:

- `tools/mister/misterctl.sh probe-wrapper` returned `__WRAPPER_PROBE_RC__=0`
- `osd-wrapper.log` showed `forced_mode=1` and `child_exit=0`
- `last-run.log` showed the expected SDL dummy/software + fbdev probe path
- a forced missing-resource test returned `__WRAPPER_MISSING_AFS_RC__=1` and logged
  `error=Missing /media/fat/games/3sx/resources/SF33RD.AFS`, and the archive was restored
  afterward
- `tools/mister/misterctl.sh wrapper-status` after the successful probe and after the forced
  missing-resource test showed no lingering `3sx` or `MiSTer_3SX` process

Known caveat from the same validation pass:

- SSH-launched normal-mode `MiSTer` / `MiSTer_3SX` attempts are not yet a reliable proxy for the
  real `[3SX] main=MiSTer_3SX` handoff. The tested commands emitted only `ttyS1: 31250`, never
  reached the wrapper logger, and therefore did not validate the non-forced OSD/core launch path.

`smoke-wrapper` uses the same entrypoint without extra runtime args and tails:

- `/media/fat/games/3sx/logs/osd-wrapper.log`
- `/media/fat/games/3sx/logs/last-run.log`

`run-wrapper` is the safe path for deterministic wrapper-driven checks that need explicit runtime
arguments without falling back to raw `misterctl.sh exec`. Example:

```sh
tools/mister/misterctl.sh run-wrapper \
  --runtime-arg --probe-renderer-only \
  --runtime-arg --software-frame-parity-check
```

`configure-3sx-ini` is the safe path for installing the minimal core-specific INI block without
hand-editing over SSH. Example:

```sh
tools/mister/misterctl.sh configure-3sx-ini \
  --ini /media/fat/MiSTer.ini \
  --main MiSTer_3SX \
  --video-mode 384,240,60 \
  --vga-scaler 1
```

## Fallback Status

Phase 1 does not replace the current runtime-only path:

- `tools/mister/package.sh` remains the legacy runtime package builder
- `/media/fat/games/3sx/launch-osd.sh` remains the fallback launcher
- `/media/fat/games/3sx/run-3sx.sh` remains the direct runtime entrypoint

The wrapper-core path must coexist with those tools until the new path is proven on device.
