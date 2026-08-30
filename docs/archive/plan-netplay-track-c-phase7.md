# Track C (Deps) Phase 7 — Implementation Plan

> **ARCHIVED 2026-08-30** — true at a752e2ca, not maintained.
> Read for rationale and for what was tried and failed. Do not read for current facts; read the code.


**Scope:** Move GekkoNet and SDL3_net ARM cross-compile recipes out of the
desktop-only profile gate in `build-deps.sh`, verify armhf linkage, and confirm
an end-to-end MiSTer build with `-DENABLE_NETPLAY=ON` produces a working armhf
binary that links both libraries. **Do not** flip `_ENABLE_NETPLAY_DEFAULT` for
PORT_MISTER (Phase-10 concern). **Do not** add RmlUi / FreeType / cJSON (Track
B / Phase 10).

**Authoritative inputs:**
- `docs/research-3sxtra-netplay-port.md` §13, §14, §18 (dependency matrix, ARM
  cross-compile recipes, build-system changes, §18.3 states
  `PORT_MISTER + ENABLE_MISTER_ARM_HARDENING + ENABLE_NETPLAY` are orthogonal).
- `docs/plan-netplay-port.md` Phase 7 section.
- Current `build-deps.sh` lines 182-246 (gated recipes) and lines 130-176
  (SDL3 mister recipe pattern).
- Current `CMakeLists.txt` lines 22-40, 112-114, 169-215, 262-267, 284-289.

**Parallel-worktree risk:** Track B (`feat/netplay-phase-4-6-track-b` at
`agent-a5e24d84`) may also edit `build-deps.sh` and `CMakeLists.txt`. Keep our
diff tight and additive.

---

## Step 1 — Refactor `build-deps.sh`: un-gate GekkoNet + SDL3_net, add mister cross-compile pattern

**Why it matters:** These are the only two remaining netplay deps that are
currently skipped on `PROFILE=mister`. Without this, there is no MiSTer
netplay build at all.

**Files to read:**
- `build-deps.sh` (lines 130-246; the SDL3 `if [ "$PROFILE" = "mister" ]`
  recipe at 152-169 is the pattern to mirror — it relies on the exported
  `CC`, `CXX`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS` from `tools/mister/build-game.sh`
  for cross-compile).
- `tools/mister/build-game.sh` lines 100-137 (env exports for clang-20 +
  `--target=arm-linux-gnueabihf`).
- `docs/research-3sxtra-netplay-port.md` §14 (ARM cross-compile recipes).

**Files to modify:**
- `build-deps.sh`:
  1. Unwrap GekkoNet recipe at lines 182-212: delete the `if [ "$PROFILE" =
     "desktop" ]` wrapper and its matching `else; echo "Skipping..."; fi`
     block. Leave the inner recipe unchanged.
  2. Unwrap SDL3_net recipe at lines 218-246 the same way.
  3. The SDL3 recipe (lines 152-169) already proves the env-based cross-compile
     pattern works; no explicit `-DCMAKE_SYSTEM_NAME=Linux` or
     `-DCMAKE_C_COMPILER_TARGET=...` is needed because cmake inherits
     `CC`/`CXX` + `CFLAGS`/`CXXFLAGS`/`LDFLAGS` from the container env. Keep
     GekkoNet + SDL3_net recipes the same shape (no extra toolchain args);
     only ensure we pass `-DCMAKE_BUILD_TYPE=Release` (GekkoNet already has
     this; SDL3_net does not — add it).
  4. Keep pins exactly: `GEKKONET_REF="7be848c"`,
     `SDL3_NET_REF="92022dc"`.
  5. Keep flags exactly: `-DNO_ASIO_BUILD=ON -DBUILD_SHARED_LIBS=OFF` for
     GekkoNet; `-DCMAKE_PREFIX_PATH="$SDL_BUILD" -DBUILD_SHARED_LIBS=OFF
     -DSDLNET_INSTALL=ON` for SDL3_net.
  6. SDL3_net must point at the already-built (mister) SDL3 via
     `CMAKE_PREFIX_PATH="$SDL_BUILD"`. That was already true in the gated
     block and must remain true for both profiles.

**Success criteria:**
- `bash build-deps.sh --profile desktop` still builds GekkoNet + SDL3_net
  to `third_party/GekkoNet/build/lib/libGekkoNet.a` and
  `third_party/SDL_net/build/lib/libSDL3_net.a` on host (no regression; skip
  this on fresh runs if the artefacts already exist — the recipes are
  idempotent-by-dir-check).
- Script still exits 0 at end.
- No references to `"Skipping GekkoNet"` or `"Skipping SDL3_net"` remain in
  the file.

**Dependencies:** None.

**What NOT to do:**
- Do not change the pins.
- Do not add `CMAKE_TOOLCHAIN_FILE` or explicit
  `-DCMAKE_C_COMPILER_TARGET`; env-based cross-compile is the validated
  pattern.
- Do not touch FFmpeg, libcdio, SDL3, minizip-ng, tf-psa-crypto recipes.
- Do not touch RmlUi / FreeType (Track B).

**If it fails:**
- If the desktop build regresses: revert the specific recipe to the gated
  version and diagnose. Most common cause: accidental indentation or stray
  `fi`.

---

## Step 2 — End-to-end mister-profile `build-deps.sh` run + armhf verification

**Why it matters:** Proves the refactored recipes actually produce armhf
static libraries under the real Docker cross-compile flow.

**Files to read:**
- `tools/mister/build-game.sh` lines 100-137 (for the env pattern; we don't
  need to modify it — we'll invoke `build-deps.sh` inside the existing
  `3s-mister-arm-build` container by the same contract).
- `tools/mister/setup-build-container.sh` (for container setup, if the
  container does not yet exist).
- `docs/mister-runbook.md` "Canonical Docker Quick Start" section.

**Files to modify:** None.

**Actions:**
1. Ensure the Docker container exists / is up:
   `bash tools/mister/setup-build-container.sh`.
2. Pre-clean any stale MiSTer GekkoNet/SDL_net build artefacts so the recipe
   actually re-runs (they are skipped if the build dir exists):
   - `rm -rf third_party/GekkoNet/build third_party/SDL_net/build`
   - (Do not rm `third_party/sdl3/build`: SDL3 is still needed by SDL3_net.)
3. Invoke `build-deps.sh --profile mister` inside the container, exporting
   the same env that `build-game.sh` exports:
   ```bash
   docker exec -i 3s-mister-arm-build bash -s <<'EOF'
   set -euo pipefail
   cd /src
   export CC=clang-20
   export CXX=clang++-20
   export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
   export CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
   export CXXFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
   export LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr"
   JOBS=2 bash build-deps.sh --profile mister
   EOF
   ```
4. Verify artefacts:
   - `docker exec 3s-mister-arm-build bash -lc 'cd /src && ls -l third_party/GekkoNet/build/lib/libGekkoNet.a third_party/SDL_net/build/lib/libSDL3_net.a'`
   - `docker exec 3s-mister-arm-build bash -lc 'cd /src && arm-linux-gnueabihf-readelf -h $(ar t third_party/GekkoNet/build/lib/libGekkoNet.a | head -n1 | xargs -I{} sh -c "ar x third_party/GekkoNet/build/lib/libGekkoNet.a {} -o /tmp && echo /tmp/{}" ) | grep -E "Machine|Class|OS"'`
     (simpler: just `readelf -h` the first object in the archive.) Expect
     `Machine: ARM`, `Class: ELF32`.
   - Equivalent for `libSDL3_net.a`.
5. Missing-symbol scan vs Buildroot userland. Static libs can't have link-time
   "missing" symbols until they're linked into an executable; we'll do the
   real check in Step 4 via the full build. For now: confirm no obvious
   Boost/ASIO pull-in: `docker exec 3s-mister-arm-build bash -lc 'cd /src &&
   arm-linux-gnueabihf-nm third_party/GekkoNet/build/lib/libGekkoNet.a |
   grep -Ei "asio|boost" | head -20'` — expect empty output.

**Success criteria:**
- `libGekkoNet.a` exists at
  `third_party/GekkoNet/build/lib/libGekkoNet.a`, is ELF32 ARM.
- `libSDL3_net.a` exists at
  `third_party/SDL_net/build/lib/libSDL3_net.a`, is ELF32 ARM.
- No `asio`/`boost` symbols in the GekkoNet archive.
- `build-deps.sh --profile mister` exits 0.

**Dependencies:** Step 1.

**What NOT to do:**
- Do not run a full game build yet (Step 4 does that end-to-end).
- Do not flip `_ENABLE_NETPLAY_DEFAULT`.

**If it fails:**
- If `asio.hpp` is pulled in (research doc §14.4 / §21 #4): confirm by
  reading the compile error; patch by defining a stub `asio.hpp` in the
  GekkoNet src tree or by adding `-DGEKKO_NO_ASIO` / equivalent compile
  flag. Document the fix in the commit message.
- If cmake selects a different compiler than `clang-20`: verify
  `CC=clang-20 CXX=clang++-20` are actually exported in the docker exec
  environment. If cmake's `CMakeCache.txt` is stale, `rm -rf` the build
  dir and re-run.

---

## Step 3 — CMake gating audit: confirm `PORT_MISTER + ENABLE_NETPLAY + ENABLE_MISTER_ARM_HARDENING` are orthogonal

**Why it matters:** Before running the end-to-end build, confirm by code
inspection that setting `ENABLE_NETPLAY=ON` on top of `PORT_MISTER=ON` does
not conflict with the ARM hardening flags (research doc §18.3 claims this).
No code change unless a conflict is found; otherwise this is a
verification-only step whose output is a note in the commit message.

**Files to read:**
- `CMakeLists.txt` lines 13-44 (option declarations and defaults).
- `CMakeLists.txt` lines 62-72 (source glob + stub substitution).
- `CMakeLists.txt` lines 107-114 (PORT_MISTER defines + NETPLAY defines).
- `CMakeLists.txt` lines 169-215 (ARM hardening flags, `PORT_MISTER AND
  ENABLE_MISTER_ARM_HARDENING`).
- `CMakeLists.txt` lines 237-289 (THIRD_PARTY_DIR, include dirs, link libs).
- `src/netplay/*.c` — sample 2-3 files to confirm they do NOT conditionally
  disable on `PORT_MISTER` (should compile cleanly on either profile).

**Files to modify:** None (audit-only). If a real conflict is found, file
as a finding and stop — do not fix silently. Expected outcome is
"no conflicts, no change needed."

**Success criteria:**
- Audit report (recorded in commit body): no conflict between `PORT_MISTER`
  scope (`PORT_MISTER` define, `vendor/Main_MiSTer` include, optional ARM
  hardening flags, `-O3` for release) and `ENABLE_NETPLAY` scope
  (`NETPLAY_ENABLED GEKKONET_STATIC GEKKONET_NO_ASIO ENABLE_NETPLAY`
  defines, include-dirs, link libs).
- `_ENABLE_NETPLAY_DEFAULT=OFF` under `PORT_MISTER` stays put (Phase 10 work
  to flip).

**Dependencies:** Step 1 (but not Step 2).

**What NOT to do:**
- Do not flip `_ENABLE_NETPLAY_DEFAULT` to ON.
- Do not add new options (that's Phase 10: `ENABLE_NETPLAY_LOBBY`,
  `ENABLE_NETPLAY_STUN`, etc. — research doc §18.1).
- Do not touch `src/netplay/` source files.

**If it fails:**
- If a real conflict is found (e.g., a `#if PORT_MISTER` that disables a
  symbol the netplay code needs): stop, document, escalate — do not paper
  over.

---

## Step 4 — End-to-end MiSTer build with `ENABLE_NETPLAY=ON` + binary verification

**Why it matters:** Closes the Phase 7 loop: the armhf 3s-arm binary with
netplay enabled actually links against our cross-compiled GekkoNet and
SDL3_net and contains GekkoNet symbols.

**Files to read:**
- `tools/mister/build-game.sh` lines 125-156. Note that the script at line
  147 invokes `cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release
  -DPORT_MISTER=ON -DENABLE_PERF_TELEMETRY="${telemetry_flag}"
  "${cmake_target_args[@]}"` with no pass-through for extra cmake args.

**Options for passing `-DENABLE_NETPLAY=ON`:**
- **Option A (preferred):** In this step, invoke the cmake step manually in
  the Docker container (mirroring `build-game.sh`'s logic) with an extra
  `-DENABLE_NETPLAY=ON` flag appended. No edit to `build-game.sh` needed,
  keeping the diff small and orthogonal to Track B.
- **Option B (fallback):** Add a minimal `--extra-cmake-arg` pass-through
  to `tools/mister/build-game.sh`. Only do this if Option A is insufficient
  for the verification — e.g., if the user plans to run Phase-7-style builds
  repeatedly. Keep the change small and confined to argv plumbing.

**Start with Option A.** Only fall back to Option B if the caller instructs
that build-game.sh edits are in-scope for Phase 7 (the task brief says "Use
the extra cmake args pass-through if supported, otherwise edit the script
conditionally" — so Option B is allowed but should be minimal).

**Files to modify:** None (Option A) or, if needed, `tools/mister/build-game.sh`
with a single `--cmake-arg` / `--cmake-arg-append` style pass-through.

**Actions (Option A):**
1. After Step 2 succeeds (libs built):
   ```bash
   docker exec -i 3s-mister-arm-build bash -s <<'EOF'
   set -euo pipefail
   cd /work-mister   # build-game.sh uses /work-mister, not /src
   # If /work-mister is stale, just rsync /src again:
   rsync -a --delete --exclude='.git/' --exclude='build/' --exclude='third_party/sdl3/build/' /src/ /work-mister/
   cd /work-mister
   export CC=clang-20 CXX=clang++-20
   export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
   export CFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
   export CXXFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr -isystem /usr/arm-linux-gnueabihf/include"
   export LDFLAGS="--target=arm-linux-gnueabihf --gcc-toolchain=/usr"
   JOBS=2 bash build-deps.sh --profile mister
   cmake -S . -B build/mister-telemetry \
       -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON \
       -DENABLE_PERF_TELEMETRY=ON \
       -DENABLE_NETPLAY=ON \
       -DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf \
       -DCMAKE_CXX_COMPILER_TARGET=arm-linux-gnueabihf
   cmake --build build/mister-telemetry --parallel 2
   cmake --install build/mister-telemetry --prefix build/mister-telemetry-install
   EOF
   ```
2. Binary verification:
   - `readelf -h build/mister-telemetry-install/bin/3s-arm | grep Machine` →
     `Machine: ARM`.
   - `readelf -d build/mister-telemetry-install/bin/3s-arm` — list of
     dynamic deps. Should not include anything MiSTer Buildroot does not
     have. Observed NEEDED set (6 entries): `libm.so.6`, `libz.so.1`,
     `libstdc++.so.6`, `libSDL3.so.0`, `libgcc_s.so.1`, `libc.so.6`.
     `libpthread`/`libdl` are NOT in NEEDED because the only references
     to pthread are weak (`pthread_rwlock_*`, `__pthread_key_create`) and
     there are no `dlopen`/`dlsym` call sites — confirm against
     `docs/mister-runbook.md` Buildroot userland notes. GekkoNet and
     SDL3_net are STATIC, so they will NOT appear as dynamic deps — this
     is correct.
   - `nm -u build/mister-telemetry-install/bin/3s-arm | grep -iE
     "gekko|sdl_net|sdlnet"` — GekkoNet is static, so symbols should be
     defined (NOT undefined). Use `nm --defined-only ... | grep -iE
     "gekko|sdlnet"` to see the entry points (`gekko_session_create`,
     `gekko_start`, etc.) present in the binary.
   - ASIO/Boost leak scan: `nm -u build/mister-telemetry-install/bin/3s-arm |
     grep -iE "asio|boost"` → expect empty.

**Success criteria:**
- Build completes without errors; `build/mister-telemetry-install/bin/3s-arm`
  exists and is ELF32 ARM.
- GekkoNet entry-point symbols defined in the binary
  (`nm --defined-only ... | grep -i gekko_` is non-empty).
- No unresolved ASIO/Boost symbols.
- `_ENABLE_NETPLAY_DEFAULT` is still `OFF` for PORT_MISTER (unchanged — only
  the explicit `-DENABLE_NETPLAY=ON` override activates netplay).

**Dependencies:** Steps 1, 2, 3.

**What NOT to do:**
- Do not deploy to the MiSTer; binary-only verification is Phase 7 scope.
- Do not flip the default.
- Do not run `rsync --delete` against any path that is not a Docker
  container-internal workdir.

**If it fails:**
- Linker errors resolving netplay symbols: the netplay source files in
  `src/netplay/` need functions that GekkoNet provides; check
  `NO_ASIO_BUILD` didn't accidentally exclude too much.
- ARM hardening flag conflict: unlikely (research doc §18.3), but if ARM
  flags cause a link-time issue with the static libs, the libs should be
  re-built with the same `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard`
  — the clang default for `--target=arm-linux-gnueabihf` is usually
  compatible, but if needed, add those flags to the lib build as well.

---

## Step 5 — Commit and report

**Why it matters:** Phase 7 deliverable is a clean commit on a feature
branch, not a push. Final report captures verification evidence.

**Files to modify:**
- Commit the `build-deps.sh` refactor from Step 1 (and the plan document
  from this plan file).
- If Step 4 required a `build-game.sh` pass-through (Option B), commit it
  as a separate commit.

**Actions:**
1. `git status` to confirm only expected files changed.
2. `git add build-deps.sh docs/plan-netplay-track-c-phase7.md` (+ optional
   `tools/mister/build-game.sh`).
3. Commit with message:
   ```
   build(deps): Phase 7 — cross-compile GekkoNet + SDL3_net for MiSTer armhf

   - Move GekkoNet and SDL3_net recipes out of the desktop-only profile gate
     in build-deps.sh. Env-based cross-compile (CC=clang-20, CFLAGS with
     --target=arm-linux-gnueabihf) inherited from the Docker container
     matches the SDL3 mister pattern already in place.
   - Pins unchanged: GEKKONET_REF=7be848c, SDL3_NET_REF=92022dc.
     NO_ASIO_BUILD=ON, BUILD_SHARED_LIBS=OFF preserved.
   - End-to-end verified: explicit -DENABLE_NETPLAY=ON produces an armhf
     3s-arm that statically links GekkoNet + SDL3_net. _ENABLE_NETPLAY_DEFAULT
     for PORT_MISTER stays OFF (Phase 10 concern).
   - Orthogonality re-confirmed per research doc §18.3: PORT_MISTER,
     ENABLE_MISTER_ARM_HARDENING, ENABLE_NETPLAY are independent.

   Plan: docs/plan-netplay-track-c-phase7.md
   ```
4. Do NOT push. Local-only.

**Success criteria:**
- `git log` shows one (or two) new commit(s) on this worktree's branch.
- `git status` clean.
- Final report includes: git log, readelf evidence, nm evidence, merge-risk
  assessment with Track B.

**Dependencies:** Steps 1-4 (or partial, if blocked).

**What NOT to do:**
- Do not push.
- Do not squash or amend into any upstream commit.
- Do not use `--no-verify`.

**If it fails:**
- If Steps 1-4 did not all complete cleanly: commit WIP, drop a
  `TRACK_C_BLOCKED.md` at repo root describing exactly what stopped us,
  and report.

---

## Merge-conflict risk assessment vs Track B

**Files both tracks are expected to touch:**
- `build-deps.sh`: Track C removes 6 lines (the `if ... else ... fi`
  wrapping) and optionally adds 1 `-DCMAKE_BUILD_TYPE=Release` line inside
  SDL3_net recipe. Track B adds new recipes AFTER the libcdio block (lines
  ~295+) for FreeType + RmlUi + cJSON. **Overlap risk: LOW.** Different line
  ranges, purely additive from Track B.
- `CMakeLists.txt`: Track C adds **zero lines** (audit only). Track B will
  add `ENABLE_RMLUI` + related options and conditional include / link
  blocks. **Overlap risk: NONE from Track C side.**
- `tools/mister/build-game.sh`: Track C may add a minimal extra-args
  pass-through (Option B). Track B likely does not touch this. **Overlap
  risk: LOW.**

**Mitigation:** keep Track C's `build-deps.sh` diff minimal (6 lines deleted,
1 line added) and localized to lines 182-246. Any merge with Track B will
be a trivial textual non-conflict unless Track B also rewrites the same
block (unlikely since the scope is orthogonal).
