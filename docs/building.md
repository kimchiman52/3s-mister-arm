# Build guide

## Setup

### Windows

1. Install [MSYS2](https://www.msys2.org/).
	* Steps after #4 on the official instructions can be skipped.
2. Launch the MinGW64 shell (there should be a start menu entry for it).
3. Install the required packages:

    ```bash
    pacman -S make mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-nasm mingw-w64-x86_64-clang mingw-w64-x86_64-headers-git mingw-w64-x86_64-git
    ```

### Linux

#### Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y $(cat tools/requirements-ubuntu-sdl3.txt)
sudo apt-get install -y clang curl nasm
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

## Building

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

## MiSTer profile (offline-first)

For MiSTer-oriented builds (no netplay, no ISO import flow, no FFmpeg ADX backend):

Important:
- Use `clang`/`clang++` for MiSTer builds.
- Do not use `gcc`/`g++` (for example `arm-linux-gnueabihf-gcc`) for this target.
- For Docker-based MiSTer builds, prefer the pinned LLVM repo setup in [docs/mister-runbook.md](mister-runbook.md); as of March 20, 2026 that flow installs `clang-20` from `apt.llvm.org` instead of Debian 11's system `clang` 11.

```bash
sh build-deps.sh --profile mister
CC=clang CXX=clang++ cmake -S . -B build/mister -DCMAKE_BUILD_TYPE=Release -DPORT_MISTER=ON
cmake --build build/mister --parallel
```
