# MiSTer Runbook (Offline-First)

## Scope

This runbook targets stock MiSTer Linux with the 3S-ARM MiSTer profile:

- no netplay
- no runtime ISO import
- no FFmpeg runtime dependency

## Canonical Docker Quick Start

Default command for fresh agents and most host machines:

```bash
tools/mister/build-game.sh --flavor telemetry
```

Common variants:

```bash
tools/mister/build-game.sh --flavor clean
tools/mister/build-game.sh --flavor both
```

If the task is simply "build the MiSTer runtime," stop here and use the helper above. Drop into the manual Docker commands later in this file only when debugging the container/toolchain flow or intentionally changing the build pipeline.

Why this is the default:

- It is the canonical MiSTer Docker build entry point for this repo.
- It defaults to the validated `linux/amd64` Docker cross-build path, so it still produces a real ARM MiSTer package on hosts that cannot execute `linux/arm/v7` containers locally.
- It builds in a container-local workdir, which avoids the stale host SDL and bind-mount ownership traps described later in this runbook.
- It copies the finished host-side outputs back to the standard paths under `build/`.

Expected outputs:

- `build/mister-telemetry-install` and `build/mister-telemetry-package`
- `build/mister-clean-install` and `build/mister-clean-package`

Use the manual Docker commands below only when debugging the Docker environment, validating a different container platform, or extending the build flow itself.

## Build

Toolchain note:

- Use `clang` for MiSTer builds. GCC toolchains (for example Debian `arm-linux-gnueabihf-gcc` 10.x) fail on legacy unnamed-parameter definitions in upstream sources.
- This repo requires CMake `>= 3.24`. Debian 11 `bullseye` main only ships `3.18.4`, so the validated Docker flow below installs `cmake 3.25.1` from `bullseye-backports`.
- As of March 20, 2026, the practical Bullseye LLVM repo pin is `clang-20`; the older `clang-15`/`clang-17` packages are no longer the dependable path on `apt.llvm.org` for this distro.

Validated Docker bootstrap on hosts with `linux/arm/v7` container support (validated with `clang-20` on March 20, 2026):

```bash
tools/mister/setup-build-container.sh --platform linux/arm/v7
```

Important:

- Set `JOBS=2` for this container. Letting the ARMv7 container auto-detect `10` jobs caused emulated clean builds to be OOM-killed around the halfway mark.
- Before creating a new Docker container, check whether `3s-mister-arm-build` already exists and reuse it. Only recreate it if the container is broken or its `/src` bind mount points at the wrong checkout.
- This path requires host `binfmt_misc`/QEMU support. If `docker run --platform linux/arm/v7 ...` fails immediately with `exec format error`, use the cross-build Docker path below instead.
- `tools/mister/setup-build-container.sh` installs the official LLVM Bullseye repo key, pins `clang-20`, and keeps CMake on `bullseye-backports`.
- Install `libasound2-dev` in the container. Without it SDL3 builds only `disk` and `dummy` audio backends, and MiSTer audio will not come up as `alsa`.
- The validated package set in this container now includes `clang-20 1:20.1.8~++20250708103407+25bcf1145fd7-1~exp1~20250708223526.135` from `apt.llvm.org`, plus `cmake 3.25.1-1~bpo11+1` from `bullseye-backports`.

Quick mount check for an existing container:

```bash
docker inspect -f '{{range .Mounts}}{{println .Source "->" .Destination}}{{end}}' 3s-mister-arm-build
```

Validated Docker build command:

```bash
docker exec 3s-mister-arm-build bash -lc '
set -euxo pipefail
cd /src
JOBS=2 bash build-deps.sh --profile mister
CC=clang-20 CXX=clang++-20 cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel 2
cmake --install build/mister --prefix build/mister-install
'
```

If this repo was previously used for a macOS or other non-Linux build, remove `third_party/sdl3/build` before the Docker build. `build-deps.sh` only checks whether that directory exists, so Docker can otherwise reuse a host-built SDL tree and fail near the final link step while looking for Linux `libSDL3.so`.

On Docker Desktop/macOS bind mounts, do not unpack or build SDL directly inside `/src`. GNU `tar` will try to restore archive ownership into the bind mount, emit `Cannot change ownership ... Permission denied`, and abort the dependency build. Copy the repo into a container-local workdir first, for example:

```bash
docker exec 3s-mister-arm-build bash -lc '
set -euxo pipefail
rm -rf /work
mkdir -p /work
cd /src
tar --exclude=.git --exclude=build --exclude=third_party/sdl3/build -cf - . | tar --no-same-owner -xf - -C /work
'
```

Build flavors:

- `telemetry` is the developer/default flavor. It keeps `--perf-*`, `--software-frame-parity-check`, renderer/presenter breakdown capture, and the optimization workflow.
- `clean` is the player-facing flavor. It compiles out perf capture CLI/plumbing and the always-on renderer/presenter telemetry bookkeeping used only for measurement.

Netplay builds (experimental, `netplay` integration branch):

- On-device netplay builds need `-DENABLE_NETPLAY=ON` (and, once the Phase 6 RmlUi lobby cascade lands, `-DENABLE_RMLUI=ON`) passed to cmake. See `docs/plan-netplay-port.md` §15 #8 for the `CFG_KEY_NETPLAY_*` runtime-config key convention that goes with these builds.
- `tools/mister/build-game.sh` forwards an `EXTRA_CMAKE_ARGS` environment variable verbatim to the inner cmake configure step, so the canonical netplay-flavor MiSTer build is:

    ```bash
    EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON -DENABLE_RMLUI=ON" \
        tools/mister/build-game.sh --flavor telemetry
    ```

  The inner build prints the final cmake invocation (including these extra `-D...` flags) so you can confirm they landed in the container log. `PORT_MISTER=ON` still defaults `ENABLE_NETPLAY` and `ENABLE_RMLUI` to OFF (see `CMakeLists.txt:25-42`), so the explicit overrides are required — the opt-in gate is intentional.

Validated dual-flavor Docker build/package commands:

```bash
docker exec 3s-mister-arm-build bash -lc '
set -euxo pipefail
cd /src
CC=clang-20 CXX=clang++-20 cmake -S . -B build/mister-telemetry -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=ON
cmake --build build/mister-telemetry --parallel 2
cmake --install build/mister-telemetry --prefix build/mister-telemetry-install
tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package

CC=clang-20 CXX=clang++-20 cmake -S . -B build/mister-clean -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY=OFF
cmake --build build/mister-clean --parallel 2
cmake --install build/mister-clean --prefix build/mister-clean-install
tools/mister/package.sh build/mister-clean-install build/mister-clean-package
'
```

Native ARM Linux build:

```bash
bash build-deps.sh --profile mister
CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel
cmake --install build/mister --prefix build/mister-install
```

Cross-build from x86_64/arm64 host (Docker/VM):

This fallback was revalidated with `clang-20` on March 20, 2026 from an x86_64 Docker host that could not execute `linux/arm/v7` containers locally (`exec format error`). It produces an ARM hard-float package that deploys and probes successfully on MiSTer.

```bash
tools/mister/setup-build-container.sh --platform linux/amd64

docker exec 3s-mister-arm-build bash -lc '
set -euxo pipefail
rm -rf /work-arm
mkdir -p /work-arm
cd /src
tar --exclude=.git --exclude=build --exclude=third_party/sdl3/build -cf - . | tar --no-same-owner -xf - -C /work-arm
cd /work-arm

export CC=clang-20
export CXX=clang++-20
export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
export CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
export CXXFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
export LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr"

JOBS=2 bash build-deps.sh --profile mister
cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON \
  -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf \
  -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf
cmake --build build/mister --parallel 2
cmake --install build/mister --prefix build/mister-install
tools/mister/package.sh build/mister-install build/mister-package
readelf -h build/mister-install/bin/3s-arm | sed -n "1,20p"
'

docker cp 3s-mister-arm-build:/work-arm/build/mister-package ./build/mister-package-arm
```

Verify ARM hard-float/NEON codegen in the final binary:

```bash
readelf -A build/mister-install/bin/3s-arm | rg -i "Tag_CPU_name|Tag_ABI_VFP_args|Tag_Advanced_SIMD_arch"
```

## FPGA Core Build (3S-ARM.rbf)

Quartus 17.0 Lite is installed **natively inside the Colima `quartus2` VM** — not in a Docker
container. Do not search Docker images or Docker Desktop for Quartus; it lives in the VM itself.

Prerequisites:

- Colima profile `quartus2` must be running: `colima list` to check, `colima start --profile quartus2` if stopped
- Quartus install: `/home/sb.linux/intelFPGA_lite/17.0/` (8.5 GB, inside the VM)
- The Mac project directory is bind-mounted into the VM by Colima

**IMPORTANT:** The `colima ssh` session will time out and kill the build if you
run Quartus directly via SSH. Always use `nohup` to detach the build inside the
VM, then poll for completion.

Quick build (detached, survives SSH disconnect):

```bash
# 1. Prepare source (run on the Mac side)
tools/mister-wrapper/build-core.sh --prepare-source

# 2. Launch Quartus compile detached with nohup
colima ssh --profile quartus2 -- bash -c '
  nohup bash -c "
    export PATH=\"/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:\$PATH\"
    cd /Users/sb/Developer/3s-mister-arm/build/mister-wrapper-core/src
    quartus_sh --flow compile 3S-ARM -c 3S-ARM > /tmp/quartus_build.log 2>&1
    if [ ! -f output_files/3S-ARM.rbf ] && [ -f output_files/3S-ARM.sof ]; then
      quartus_cpf -c output_files/3S-ARM.sof output_files/3S-ARM.rbf
    fi
    echo BUILD_DONE >> /tmp/quartus_build.log
  " &
  echo "Build launched, PID=$!"
'

# 3. Poll for completion (check every 60s)
while colima ssh --profile quartus2 -- pgrep -f "quartus_sh.*compile" > /dev/null 2>&1; do
  echo "$(date +%H:%M:%S) - still building..."
  sleep 60
done
echo "Done! Check result:"
colima ssh --profile quartus2 -- tail -5 /tmp/quartus_build.log
```

**Do NOT** run Quartus directly via `colima ssh -- quartus_sh ...` — the SSH
session will time out after ~2 minutes and kill the build. Do NOT launch multiple
concurrent Quartus builds (they corrupt the `db/` directory). If a build was
killed, clean stale artifacts from the Mac side before retrying:

```bash
rm -rf build/mister-wrapper-core/src/db build/mister-wrapper-core/src/output_files
```

Output: `build/mister-wrapper-core/src/output_files/3S-ARM.rbf` (visible from both inside the VM and on the Mac host).

Previous builds are also cached inside the VM at `/home/sb.linux/build/mister-wrapper-core/`.

Build time: ~30-60 minutes (x86_64-emulated Colima QEMU VM).

Deploy the FPGA core to MiSTer using `misterctl.sh`:

```bash
MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 \
  tools/mister/misterctl.sh deploy-wrapper --src build/mister-wrapper-core --artifacts-only
```

Or manually (core only):

```bash
sshpass -p 1 scp build/mister-wrapper-core/src/output_files/3S-ARM.rbf \
  root@192.168.1.171:/media/fat/_Other/3S-ARM.rbf
```

## Package

Telemetry package:

```bash
tools/mister/package.sh build/mister-telemetry-install build/mister-telemetry-package
```

Clean package:

```bash
tools/mister/package.sh build/mister-clean-install build/mister-clean-package
```

Player-facing runtime package:

```bash
tools/mister/build-runtime-package.sh
```

Output layout:

- `build/mister-telemetry-package/bin/3s-arm`
- `build/mister-clean-package/bin/3s-arm`
- `build/mister-telemetry-package/lib/*`
- `build/mister-clean-package/lib/*`
- `build/mister-telemetry-package/scripts/run-3s-arm.sh`
- `build/mister-clean-package/scripts/run-3s-arm.sh`
- `build/mister-telemetry-package/scripts/launch-osd.sh`
- `build/mister-clean-package/scripts/launch-osd.sh`
- `build/mister-telemetry-package/run-3s-arm.sh` and `build/mister-telemetry-package/launch-osd.sh` are compatibility wrappers
- `build/mister-clean-package/run-3s-arm.sh` and `build/mister-clean-package/launch-osd.sh` are compatibility wrappers

## Player Release Zip

Build the player-facing FAT-rooted MiSTer release zip after the clean runtime install, HPS wrapper,
and wrapper core artifacts already exist:

```bash
tools/mister-wrapper/build-release.sh
```

Default inputs:

- `build/mister-clean-install`
- `build/mister-wrapper-hps/MiSTer_3S-ARM`
- `build/mister-wrapper-core/3S-ARM.rbf`

Default outputs:

- staged FAT root: `build/mister-release/stage`
- release zip: `build/mister-release/3S-ARM-mister-rolling-pre-release.zip`

The release zip is ready to extract directly onto a MiSTer SD card root. The build fails if the
staged release contains any of the player-local or excluded content below:

- `games/3s-arm/resources/SF33RD.AFS`
- `games/3s-arm/config`
- `games/3s-arm/keymap`
- `games/3s-arm/logs`

The staged ZIP root always includes `README.txt` (the release readme), and the archive intentionally leaves
`games/3s-arm/resources/SF33RD.AFS` empty so end users must add their own copy manually after install.

## Publish Rolling Pre-Release

After `tools/mister-wrapper/build-release.sh` succeeds, publish the MiSTer zip to the existing
rolling pre-release tag with:

```bash
tools/mister-wrapper/publish-release.sh
```

Current behavior:

- move `rolling-pre-release` to the current `HEAD`
- keep the release marked as a pre-release
- update the release title to the current short SHA
- replace only existing `3S-ARM-mister-*.zip` assets on that release, leaving other platform assets alone

This remains a local maintainer flow. GitHub-hosted CI does not build or publish the MiSTer release
zip yet.

## Deploy To MiSTer

Copy package contents to:

- `/media/fat/games/3s-arm/`

Preferred remote entry point:

- `tools/mister/misterctl.sh`

The MiSTer SSH path is fragile on this target. Use `tools/mister/misterctl.sh` for deploy, probe, smoke, and ad hoc remote commands, and use `tools/mister/perf-sampler.sh` for captures. Both tools take a shared local lock so only one MiSTer remote workflow runs at a time.
`misterctl.sh deploy` also refreshes the visible MiSTer OSD wrapper at `/media/fat/Scripts/3S-ARM.sh`.

Auth note:

- On the stock box, export `MISTER_PASSWORD=1` before remote commands unless you have intentionally configured working SSH key auth.
- When `MISTER_PASSWORD` is unset, the tooling now uses a key-only, non-interactive SSH path that ignores the local agent. That avoids accidental `Too many authentication failures`, but it will fail fast instead of prompting for a password.

When multiple agents/worktrees are active on the same machine, inspect the shared lock before starting a remote step:

```bash
tools/mister/misterctl.sh lock-status
```

The lock lives under `/tmp` by default, so repo-driven MiSTer operations in sibling worktrees will serialize as long as they go through `misterctl.sh` or `perf-sampler.sh`.

Before any deploy/probe/smoke step that could collide with another workflow, inspect the remote side too:

```bash
tools/mister/misterctl.sh busy-status
```

`misterctl.sh` now runs that busy preflight automatically before `deploy`, `deploy-wrapper`, `probe`, `smoke`, `probe-wrapper`, `smoke-wrapper`, and `run-wrapper`. If the target already shows an active `3s-arm`, `MiSTer_3S-ARM`, `launch-osd.sh`, `run-3s-arm.sh`, `rsync`, `scp`, or `sftp-server` process, the command aborts unless `MISTER_ALLOW_BUSY_TARGET=1` is set deliberately.

Remote mutation safety:

- Delete-capable syncs are only safe inside `/media/fat/games/3s-arm/`.
- Never retarget a `--delete` sync at `/media/fat/`, `/media/`, or `/`.
- `misterctl.sh` now rejects nonstandard remote roots unless both `MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1` and `MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=<exact-path>` are set deliberately.
- Raw `misterctl.sh exec` is disabled by default; set `MISTER_ALLOW_REMOTE_EXEC=1` only for intentional one-off maintenance.
- `tools/mister/perf-sampler.sh --copy-afs` now restores the previous remote `SF33RD.AFS` on exit instead of leaving a replacement behind.
- `tools/mister/perf-sampler.sh --tag` now accepts only safe basenames; do not use path-like tags.

Recommended sync command (preserves staged `SF33RD.AFS` plus on-device `config`/`keymap`):

```bash
MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 \
  tools/mister/misterctl.sh deploy --src build/mister-clean-package
```

Use `build/mister-clean-package/` for normal play and `build/mister-telemetry-package/` when you need perf capture or parity tooling on the device.

Low-level `rsync` still works, but do not prefer it in automation now that `misterctl.sh` exists. If you bypass `misterctl.sh`, dry-run first and keep the destination exactly `/media/fat/games/3s-arm/`:

```bash
rsync -avn --itemize-changes --delete --omit-dir-times --no-perms --no-owner --no-group \
  --exclude 'resources/SF33RD.AFS' \
  --filter 'P resources/SF33RD.AFS' \
  --exclude 'config' \
  --filter 'P config' \
  --exclude 'keymap' \
  --filter 'P keymap' \
  build/mister-clean-package/ root@192.168.1.171:/media/fat/games/3s-arm/
```

Remove `-n` only after reviewing the itemized path list and confirming every changed file belongs to `games/3s-arm`.

Required game data:

- `/media/fat/games/3s-arm/resources/SF33RD.AFS`

If `SF33RD.AFS` is missing, 3S-ARM exits with code `20` and prints the expected path.

## Probe Backends

```bash
MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 \
  tools/mister/misterctl.sh probe
```

Probe log path:

- `/media/fat/games/3s-arm/logs/backend.log`

## Run

```bash
/media/fat/games/3s-arm/run-3s-arm.sh
```

For OSD launchers (`/media/fat/Scripts/*.sh`), call:

```bash
/media/fat/Scripts/3S-ARM.sh
```

This keeps stdout/stderr out of the text console and writes logs to `/media/fat/games/3s-arm/logs/last-run.log`.
`/media/fat/Scripts/3S-ARM.sh` delegates to the packaged OSD launcher, which forces SDL dummy video + software renderer for stable stock-MiSTer OSD startup; fbdev presenter handles on-screen output. The top-level `run-3s-arm.sh` and `launch-osd.sh` remain the user-facing app-local wrappers.

Generated OSD wrapper:

```sh
#!/bin/sh
set -eu
exec /media/fat/Scripts/3S-ARM.sh "$@"
```

Do not wrap `/media/fat/Scripts/3S-ARM.sh` in `openvt`, `chvt`, or another manual VT hop. On this MiSTer target that path can hang before the launcher starts, leaving the OSD frozen and producing no fresh `last-run.log`.

## Performance Sampling

Preferred quick steady-state gate:

```bash
MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 \
  tools/mister/perf-sampler.sh --scene training --frames 300 --tag quick-gate
```

`tools/mister/perf-sampler.sh` now keeps one shared MiSTer lock for the whole capture workflow and returns the JSON plus remote log over the same SSH session that ran the capture. That keeps each perf sample to one remote command session instead of SSH plus separate SCP round-trips.

Perf sampling requires the `telemetry` flavor on the device. Deploy `build/mister-telemetry-package/` before running `tools/mister/perf-sampler.sh`.

Long-window gate (diagnostic, currently noisy):

```bash
MISTER_HOST=192.168.1.171 MISTER_USER=root MISTER_PASSWORD=1 \
  tools/mister/perf-sampler.sh --scene training --frames 600 --tag long-gate
```

Current behavior on `training`: first ~300 frames are typically steady, while late-window outliers can appear. Use the 300-frame gate for iteration and track long-window outliers separately.

## Troubleshooting

### GCC toolchain errors during configure/build

Symptoms:

- parser/legacy-definition errors while using `gcc`/`g++`

Fix:

```bash
CC=clang-20 CXX=clang++-20 cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel
```

### Docker build reuses host-built SDL artifacts

Symptoms:

- Docker build reaches the final link step and fails with `No rule to make target ... third_party/sdl3/build/lib/libSDL3.so`
- `third_party/sdl3/build/lib/` contains a host artifact such as `libSDL3.0.dylib`

Cause:

- `build-deps.sh` treats any existing `third_party/sdl3/build` directory as a completed SDL build, even if it was produced on another OS

Fix:

```bash
rm -rf third_party/sdl3/build
docker exec 3s-mister-arm-build bash -lc '
set -euxo pipefail
cd /src
JOBS=2 bash build-deps.sh --profile mister
CC=clang-20 CXX=clang++-20 cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel 2
cmake --install build/mister --prefix build/mister-install
'
```

### Missing resources

Symptom:

- app exits immediately with code `20`

Fix:

- ensure `SF33RD.AFS` exists at `/media/fat/games/3s-arm/resources/SF33RD.AFS`

### macOS tar deploy leaves `._*` files or ownership errors on MiSTer

Symptoms:

- remote `tar` extraction prints `Cannot change ownership`
- remote `tar` extraction prints `Ignoring unknown extended header keyword 'LIBARCHIVE.xattr.*'`
- unexpected `._*` files appear under `/media/fat/games/3s-arm/`

Fix:

- prefer the `rsync` deploy path above on macOS hosts
- avoid packaging MiSTer deploy tarballs with default macOS metadata unless you explicitly disable AppleDouble/xattr output

### No renderer/video backend

Symptoms:

- startup fails while creating SDL window/renderer

Fix:

1. Run `--probe-renderer-only`
2. Inspect `logs/backend.log`
3. Adjust config keys in `/media/fat/games/3s-arm/config`:

```text
scale-mode = native
video-driver-order = dummy
render-driver-order = software
```

Use `/media/fat/Scripts/3S-ARM.sh` as-is for OSD boot. If you write a custom wrapper, mirror its `SDL_VIDEODRIVER`/`SDL_VIDEO_DRIVER`/`SDL_RENDER_DRIVER` exports.

### Frozen OSD after selecting 3S-ARM

Symptoms:

- the OSD stays on screen and stops responding normally after launching 3S-ARM
- no new `/media/fat/games/3s-arm/logs/last-run.log` appears
- stuck `openvt` processes may remain on the MiSTer

Cause:

- the OSD wrapper is trying to launch 3S-ARM through `openvt` or another explicit VT switch, and the handoff wedges before `/media/fat/Scripts/3S-ARM.sh` starts

Fix:

1. Use `/media/fat/Scripts/3S-ARM.sh` directly, or a wrapper that only execs `/media/fat/Scripts/3S-ARM.sh "$@"`.
2. Do not add `openvt`, `chvt`, or backgrounding around the launcher.
3. If you need to confirm the wrapper path ran, inspect `/media/fat/games/3s-arm/logs/osd-wrapper.log` and `/media/fat/games/3s-arm/logs/last-run.log`.

### Input not responding

1. Verify controller is recognized by MiSTer Linux.
2. Relaunch 3S-ARM after controller is connected.
3. Check 3S-ARM config/keymap files under `THIRDSARM_HOME`.

### Console still receiving input / terminal cursor visible

Symptoms:

- button presses produce terminal glyphs
- blinking shell cursor is visible between frames

Fix:

1. Launch through `/media/fat/Scripts/3S-ARM.sh` (or an OSD wrapper that calls it).
2. Use `/media/fat/Scripts/3S-ARM.sh` (or match its SDL env exports exactly) so video/backend selection is deterministic.
3. If startup says it failed to acquire Linux console/KD_GRAPHICS, run from MiSTer OSD/local console, not SSH.
