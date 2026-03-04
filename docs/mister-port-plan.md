# MiSTer Port Feasibility And Execution Plan (3SX)

Last updated: 2026-03-04  
Repository: `/Users/sb/Developer/3sx-mister`

## 1. Scope

This document defines:

- What is technically possible for running 3SX on MiSTer FPGA's Linux side (HPS userspace).
- What is not available on stock MiSTer Linux.
- What dependency and rendering-pipeline changes are required.
- A phased execution plan with explicit milestones and go/no-go criteria.

This is a technical planning document, not an implementation log.

## 2. Executive Summary

- A MiSTer build is feasible, but not as a direct "compile and run" of the current desktop pipeline.
- On stock MiSTer Linux, OpenGL/GLES/Vulkan userspace libraries are not present in the currently shipped `linux.img` payload.
- SDL3 itself is buildable for ARM Linux, but a working on-screen renderer on MiSTer depends on either:
  - adding a graphics userspace stack to MiSTer Linux, or
  - implementing a non-GL display path compatible with MiSTer's framebuffer model.
- Current 3SX has hard links against FFmpeg, libcdio/iso9660, GekkoNet, and SDL3_net. For a practical first MiSTer target, these must be feature-gated and mostly disabled/replaced.

Recommendation:

- Phase 1 target should be a local/offline build with:
  - no netplay (remove GekkoNet + SDL3_net),
  - no ISO dialog/path (require pre-extracted `SF33RD.AFS`),
  - no FFmpeg (replace ADX decode path or pre-decode assets),
  - simplified renderer path.

## 3. Verified Platform Facts (MiSTer)

All items below were verified from primary sources on 2026-03-04.

### 3.1 Hardware baseline

- MiSTer's base board is DE10-Nano (Cyclone V SoC + dual-core ARM Cortex-A9).
- DE10-Nano spec page indicates dual-core ARM Cortex-A9 and 1GB DDR3.

### 3.2 Linux role and system model

- MiSTer wiki describes Linux as the housekeeping environment (loading data, device support, etc).
- Network-access documentation confirms SSH/SFTP/FTP and `/media/fat` as SD root.

### 3.3 Active MiSTer Linux payload reference

- `Distribution_MiSTer` DB `linux` entry currently points to:
  - `release_20250402.7z`
  - `version: "250402"`
  - DB timestamp: `1772574932` => `2026-03-03 21:55:32 UTC`.

### 3.4 Linux updater behavior

- `Downloader_MiSTer` updater extracts `files/linux/*` from the installer archive and updates `/media/fat/linux`.
- It replaces kernel image/boot artifacts and `linux.img`.

### 3.5 Contents and markers from current `release_20250402.7z`

Confirmed archive members include:

- `files/linux/linux.img`
- `files/linux/zImage_dtb`
- `files/linux/uboot.img`
- `files/linux/updateboot`
- `files/linux/_user-startup.sh`

`linux.img` markers observed:

- `PRETTY_NAME="Buildroot 2021.02.4"`
- kernel tag `5.15.1-MiSTer`
- init hook lines including `::sysinit:/media/fat/MiSTer &`
- `USER_SCRIPT="/media/fat/linux/user-startup.sh"`

### 3.6 Library-level observations from current `linux.img`

Enumerating `/usr/lib` inside the ext4 image showed:

- Present: SDL2 (`libSDL2-2.0.so.0.14.0`, `libSDL2.so`)
- Not found: `libSDL3*`
- Not found: OpenGL/GLES/EGL/Vulkan userspace libs (`libGL*`, `libGLES*`, `libEGL*`, `libvulkan*`)
- Not found: KMSDRM userspace prerequisites (`libdrm*`, `libgbm*`)
- Not found: FFmpeg core libs (`libavcodec`, `libavformat`, `libavutil`, `libswresample`)
- Not found: `libcdio`/`libiso9660`

Interpretation:

- Stock image does not provide the graphics stack normally used for SDL KMSDRM/OpenGL/GLES/Vulkan rendering.
- Therefore OpenGL and SDL_gpu are not available on stock userspace without custom system work.

## 4. Verified SDL3 Constraints Relevant To MiSTer

From SDL official docs/source:

- SDL3 Linux support is highly feature-conditional and can dynamically disable unavailable runtime features.
- KMSDRM backend requires `libdrm` + `gbm` + EGL at build/runtime.
- SDL GPU render backend on Linux is Vulkan-based.
- `SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER` controls DRM-master requirement for KMSDRM rendering behavior.
- `SDL_HINT_VIDEO_DRIVER` and `SDL_HINT_RENDER_DRIVER` can force backend selection order.

Implication for MiSTer:

- Even if SDL3 compiles, practical render backends on stock MiSTer are constrained by missing graphics userspace libs.

## 5. Current 3SX State (Dependency/Rendering)

### 5.1 Build-time links (current CMake)

Current CMake links:

- FFmpeg (`libavcodec/libavformat/libavutil/libswresample`)
- SDL3
- libcdio (`libiso9660`, `libcdio`)
- GekkoNet
- SDL3_net

This is currently unconditional by target platform except for filename variants per OS.

### 5.2 Resource flow

- Startup requires `SF33RD.AFS` in resources path.
- If missing, code enters an SDL file-dialog + ISO extraction flow (`SDL_ShowOpenFileDialog` + libcdio).

### 5.3 Rendering flow

- Current path uses SDL3 render API with textures, render targets, geometry sorting, and multiple passes.
- This is not direct OpenGL; it depends on SDL renderer backend availability.

### 5.4 Audio decode flow

- ADX decoding currently uses FFmpeg (`AV_CODEC_ID_ADPCM_ADX`) in `src/port/sound/adx.c`.

## 6. Availability Of OpenGL And SDL_gpu On MiSTer

### 6.1 OpenGL

Status on stock MiSTer Linux: **not available by default** (no GL/EGL/GLES libs discovered in current shipped image).

### 6.2 SDL_gpu

Status on stock MiSTer Linux: **not available by default** for practical use.

Reason:

- SDL GPU renderer on Linux depends on Vulkan backend support.
- `libvulkan` and related userspace stack were not found in current stock image.

## 7. Port Strategy Options

## Option A: Stock MiSTer Linux, software-first display strategy

Summary:

- Keep stock MiSTer Linux unchanged.
- Build app/deps to avoid GL/Vulkan requirements.
- Implement a display path that can present frames without OpenGL/KMSDRM userspace stack.

Pros:

- Avoids maintaining custom MiSTer OS image.
- Lowest operational friction for users.

Cons:

- Requires custom rendering output integration for actual on-screen presentation.
- Highest engineering burden in app/runtime integration.

## Option B: Custom MiSTer Linux image with graphics userspace stack

Summary:

- Add libdrm/gbm/EGL/GLES (or Vulkan) stack to a custom Linux image.
- Use SDL3 KMSDRM/OpenGLES (or Vulkan-based paths) conventionally.

Pros:

- Uses more standard SDL rendering architecture.
- Less app-specific display backend code.

Cons:

- Ongoing maintenance burden across MiSTer updates.
- Higher install complexity and support burden.

## Option C: SDL2 pivot on stock libs

Summary:

- Port app from SDL3 API surface to SDL2 and use stock `libSDL2`.

Pros:

- Removes need to ship SDL3 runtime.

Cons:

- Major API migration effort.
- Does not solve GL/Vulkan availability by itself.
- Still constrained by video backend reality.

## Strategy recommendation

Recommended sequence:

1. Start with Option A-style dependency minimization and feature gating.
2. Run targeted runtime probes on-device to identify viable display method.
3. Choose between:
   - custom framebuffer presentation path, or
   - Option B custom Linux image if conventional SDL rendering is required.

## 8. Required Codebase Changes

## 8.1 Build-system feature gates

Introduce explicit CMake options for MiSTer profile:

- `PORT_MISTER=ON`
- `ENABLE_NETPLAY=OFF` (default OFF for MiSTer)
- `ENABLE_ISO_IMPORT=OFF` (default OFF for MiSTer)
- `ENABLE_FFMPEG_ADX=OFF` (default OFF for MiSTer)
- `ENABLE_SDL_DIALOGS=OFF` (default OFF for MiSTer)

Link dependencies conditionally by option, not globally.

## 8.2 Resource import pipeline

For MiSTer target:

- Remove runtime ISO dialog flow from critical path.
- Require `SF33RD.AFS` to exist at configured resources path.
- If missing: show plain text guidance + deterministic exit code.

## 8.3 Audio pipeline (ADX)

Replace FFmpeg dependency with one of:

- minimal in-tree ADX decoder implementation, or
- pre-conversion pipeline for BGM assets into a decode format already supported by lightweight runtime.

Target:

- remove runtime link to FFmpeg on MiSTer profile.

## 8.4 Networking/netplay

For first working build:

- compile out netplay UI and runtime hooks.
- remove GekkoNet and SDL3_net linkage.

## 8.5 Rendering pipeline

Mandatory rendering refactor tasks:

- add renderer capability probe at startup:
  - enumerate SDL video/render drivers,
  - log selected backend,
  - fail fast with actionable error if none usable.
- add explicit backend hint order for MiSTer profile.
- reduce render passes/copies where possible:
  - minimize full-frame target switches and texture blits.

If stock backend cannot present to screen:

- implement a MiSTer-compatible presentation backend (framebuffer path) for final composited frame.

## 9. Phased Execution Plan

## Phase 0: Fork and planning baseline

Deliverables:

- `3sx-mister` fork created in `~/Developer`.
- This planning document checked in.

Exit criteria:

- baseline established and reviewed.

## Phase 1: Build profile and dependency cut

Tasks:

- add MiSTer CMake options listed above.
- decouple unconditional links to FFmpeg/libcdio/GekkoNet/SDL3_net.
- keep desktop targets unchanged.

Exit criteria:

- `PORT_MISTER=ON` builds without FFmpeg/libcdio/netplay libs.

## Phase 2: Resource and startup behavior

Tasks:

- disable dialog/ISO path for MiSTer profile.
- require pre-staged `SF33RD.AFS`.
- add robust error messages and deterministic failures.

Exit criteria:

- app starts to main loop when AFS present, and fails cleanly when absent.

## Phase 3: Audio replacement

Tasks:

- implement or integrate lightweight ADX decode path.
- verify loop/seamless behavior parity with existing logic.

Exit criteria:

- no FFmpeg linkage in MiSTer build; BGM/ADX behavior acceptable.

## Phase 4: Renderer capability probing

Tasks:

- add startup diagnostics for SDL video/render drivers.
- add configurable hint ordering (`SDL_HINT_VIDEO_DRIVER`, `SDL_HINT_RENDER_DRIVER`).
- capture logs on MiSTer target hardware.

Exit criteria:

- backend viability confirmed empirically on device.

## Phase 5: Presentation backend implementation

Decision gate:

- If SDL can present reliably with available backend: keep SDL path.
- If not: implement custom presentation path compatible with MiSTer framebuffer model.

Exit criteria:

- stable in-game rendering on target display.

## Phase 6: Performance and stability hardening

Tasks:

- profile frame pacing, render cost, texture churn.
- reduce CPU overhead in hot paths.
- verify long-session stability (memory usage, no leaks/crashes).

Exit criteria:

- sustained acceptable performance and stable long-run behavior.

## Phase 7: Packaging and operational docs

Tasks:

- define deployment layout under `/media/fat`.
- add user-facing setup instructions.
- add troubleshooting matrix (missing assets/backend failures/input issues).

Exit criteria:

- repeatable install/run flow for testers.

## 10. Go/No-Go Gates

Go gate A (after Phase 1):

- Can we build cleanly without FFmpeg/libcdio/netplay libs?

Go gate B (after Phase 4):

- Is there a reliable on-screen backend without custom Linux image changes?

Go gate C (after Phase 6):

- Is frame pacing/performance sufficient on target hardware?

If gate B fails:

- either accept custom Linux-image maintenance (Option B),
- or pause project.

## 11. Risk Register

1. Display backend incompatibility on stock MiSTer Linux  
Impact: Critical  
Mitigation: early runtime probing + explicit gate B.

2. ADX replacement quality/parity risk  
Impact: High  
Mitigation: targeted regression test cases for looping and transitions.

3. Ongoing maintenance burden from custom Linux image (if chosen)  
Impact: High  
Mitigation: automate image-build/version checks and pin known-good releases.

4. CPU headroom limits on ARM A9 for current render flow  
Impact: High  
Mitigation: simplify passes, reduce texture churn, profile early.

5. Scope creep from netplay and non-essential features  
Impact: Medium  
Mitigation: keep first milestone strictly offline/local.

## 12. Test Plan (Minimum)

Functional:

- boot with valid `SF33RD.AFS`
- fail behavior with missing/corrupt AFS
- menu navigation and match start
- BGM start/stop/loop transitions
- controller input and remapping basics

Rendering:

- verify output aspect/letterbox modes
- verify no persistent artifacts after scene transitions
- verify stable frame pacing over 30+ minute session

Stability:

- memory growth check over 60+ minute idle + gameplay mix
- restart/relaunch resilience

Deployment:

- clean install from empty app directory
- update flow over existing install

## 13. Repository/Workflow Plan For `3sx-mister`

Suggested structure additions:

- `docs/mister-port-plan.md` (this file)
- `docs/mister-runbook.md` (deployment/tester instructions)
- `cmake/PlatformMiSTer.cmake` (profile defaults)
- optional `tools/mister/` scripts for packaging and log collection

Branching:

- keep small, single-purpose branches per phase.
- merge only after phase exit criteria are met.

## 14. Immediate Next Actions

1. Implement Phase 1 CMake feature gates.
2. Implement Phase 2 resource-flow simplification.
3. Decide ADX replacement approach and execute Phase 3.
4. Run first on-device backend probe build (Phase 4).

## 15. Source Index

MiSTer sources:

- MiSTer wiki home: <https://github.com/MiSTer-devel/Wiki_MiSTer/wiki>
- MiSTer wiki network access: <https://github.com/MiSTer-devel/Wiki_MiSTer/wiki/Network-access>
- Distribution DB (`db.json.zip`): <https://raw.githubusercontent.com/MiSTer-devel/Distribution_MiSTer/main/db.json.zip>
- Downloader Linux updater: <https://github.com/MiSTer-devel/Downloader_MiSTer/blob/main/src/downloader/linux_updater.py>
- SD installer payload (referenced Linux archive): <https://raw.githubusercontent.com/MiSTer-devel/SD-Installer-Win64_MiSTer/b8531c7848526d9a8227841923cc4a493cb6e631/release_20250402.7z>
- DE10-Nano spec page: <https://www.terasic.com.tw/cgi-bin/page/archive.pl?Language=English&CategoryNo=167&No=1046>

SDL sources:

- SDL Linux README: <https://github.com/libsdl-org/SDL/blob/main/docs/README-linux.md>
- SDL CMake options: <https://github.com/libsdl-org/SDL/blob/main/CMakeLists.txt>
- SDL KMSDRM checks: <https://github.com/libsdl-org/SDL/blob/main/cmake/sdlchecks.cmake>
- SDL video driver hint: <https://github.com/libsdl-org/sdlwiki/blob/main/SDL3/SDL_HINT_VIDEO_DRIVER.md>
- SDL render driver hint: <https://github.com/libsdl-org/sdlwiki/blob/main/SDL3/SDL_HINT_RENDER_DRIVER.md>
- SDL KMSDRM DRM-master hint: <https://github.com/libsdl-org/sdlwiki/blob/main/SDL3/SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER.md>
- SDL open file dialog notes: <https://github.com/libsdl-org/sdlwiki/blob/main/SDL3/SDL_ShowOpenFileDialog.md>

Project references (current codebase):

- `/Users/sb/Developer/3sx-mister/CMakeLists.txt`
- `/Users/sb/Developer/3sx-mister/src/port/resources.c`
- `/Users/sb/Developer/3sx-mister/src/port/sound/adx.c`
- `/Users/sb/Developer/3sx-mister/src/port/sdl/sdl_app.c`
- `/Users/sb/Developer/3sx-mister/src/port/sdl/sdl_game_renderer.c`
