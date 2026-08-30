# Build guide

## Choose the target first

Before running any build command, decide whether you are building the desktop app or the MiSTer runtime.

MiSTer default for fresh agents and common host machines:

```bash
tools/mister/build-game.sh --flavor telemetry
```

Important:
- Treat `tools/mister/build-game.sh` as the canonical MiSTer build entry point.
- Do not start a MiSTer task with the desktop `cmake -B build` flow below.
- Do not assume `PORT_MISTER=ON` on a non-ARM host produces a deployable MiSTer binary.
- Use the native ARM Linux MiSTer flow only when the host itself is already native ARM Linux or when you are intentionally debugging outside Docker.

## Setup

### Windows

1. Install [MSYS2](https://www.msys2.org/).
	* Steps after #4 on the official instructions can be skipped.
2. Launch the MinGW64 shell (there should be a start menu entry for it).
3. Install the required packages:

    ```bash
    pacman -S --needed $(cat tools/requirements-windows.txt)
    ```

### Linux

#### Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y $(cat tools/requirements-ubuntu.txt)
```

### macOS

You should be able to build the project with just Xcode Command Line Tools.

1. Check if Command Line Tools are installed:

    ```bash
    xcode-select -p
    ```

2. Install if needed:

    ```bash
    xcode-select --install
    ```

## Desktop Builds

1. Build dependencies

    ```bash
    sh build-deps.sh --profile desktop
    ```

2. Build the game

    ```bash
    CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel --config Release
    cmake --install build --prefix build/application
    ```

3. Copy from build/application to the desired location

These steps are for desktop builds only. They do not produce MiSTer ARM runtime outputs.

## MiSTer profile (offline-first)

For MiSTer-oriented builds (no netplay, no ISO import flow, no FFmpeg ADX backend):

Default Docker path for fresh agents and common host setups:

```bash
tools/mister/build-game.sh --flavor telemetry
```

Notes:
- This is the canonical MiSTer Docker build entry point.
- It defaults to `--platform auto`: the container platform native to whichever Docker daemon is resolved (`linux/arm64` on Apple Silicon, `linux/amd64` on an x86_64 host). Either way the compiler is `clang --target=arm-linux-gnueabihf`, so this chooses only what the compiler *runs* as, never what it emits; both produce a real ARM hard-float MiSTer package. Pass `--platform linux/amd64` to pin the old behaviour.
- It defaults to `--jobs auto`, which is the daemon's core count. The old default was 2.
- It refuses to run on a Docker daemon that is not the native host daemon — in particular the Colima `quartus2` VM, which exists for the FPGA toolchain. When Docker Desktop is not running the `docker` CLI silently falls through to whatever context is live, which is how MiSTer game builds came to run inside the emulated Quartus VM unnoticed. Start Docker Desktop (`open -a Docker`), or set `MISTER_ALLOW_FOREIGN_DOCKER_HOST=1` to build elsewhere deliberately. The resolved daemon is printed as a `docker daemon:` line with the other provenance output.
- Compilation is cached with `ccache` in a persistent Docker volume (`3s-mister-ccache`), wired via `CMAKE_C_COMPILER_LAUNCHER`. Pass `-DENABLE_CCACHE=OFF` to disable.
- Use `--flavor clean` for the player-facing package or `--flavor both` when you need both outputs.
- Use the manual runbook flow in [docs/mister-runbook.md](mister-runbook.md) only when debugging the Docker environment or intentionally choosing a different container platform.

Important:
- Use `clang`/`clang++` for MiSTer builds.
- Do not use `gcc`/`g++` (for example `arm-linux-gnueabihf-gcc`) for this target.
- For Docker-based MiSTer builds, prefer the pinned LLVM repo setup in [docs/mister-runbook.md](mister-runbook.md); as of March 20, 2026 that flow installs `clang-20` from `apt.llvm.org` instead of Debian 11's system `clang` 11.
- `PORT_MISTER=ON` alone does not guarantee an ARM MiSTer binary. On non-ARM hosts, the plain `cmake -B build/mister` snippet below will still build a host-arch binary unless you use the Docker helper or the explicit cross-build flags from the runbook.

Native ARM Linux only:

```bash
bash build-deps.sh --profile mister
CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel
cmake --install build/mister --prefix build/mister-install
```

## Giblet PR #243 software renderer (experimental)

The renderer at `src/platform/video/software/` (originally a port of
crowded-street/3sx#243 by gibletto / Paul Connolly) is the sole
rendering backend on every supported platform — Mac host build and
MiSTer cross build. The legacy `SDLGameRenderer` path was deleted in
Phase D (see `docs/plan-giblet-wholesale-rip.md`). No build flag is
required.
