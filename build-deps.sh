#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
THIRD_PARTY="$ROOT_DIR/third_party"

mkdir -p "$THIRD_PARTY"

PROFILE="desktop"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --profile)
            PROFILE="${2:-}"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [--profile desktop|mister]"
            exit 1
            ;;
    esac
done

if [ "$PROFILE" != "desktop" ] && [ "$PROFILE" != "mister" ] && [ "$PROFILE" != "miyoo" ]; then
    echo "Invalid profile: $PROFILE"
    echo "Expected one of: desktop, mister, miyoo"
    exit 1
fi

# -----------------------------------------------------------------------
# Dependency set
# -----------------------------------------------------------------------
#
# Built by this script, in the order the sections appear below:
#
#   FFmpeg          desktop only            (ADX decode/encode)
#   SDL3            desktop, mister, miyoo
#   GekkoNet        desktop, mister         (rollback netcode; miyoo Cut 1
#                                            builds ENABLE_NETPLAY=OFF)
#   SDL3_net        desktop, mister         (same miyoo exclusion)
#   libcdio         desktop only            (ISO import)
#   minizip-ng      desktop, mister, miyoo
#   tf-psa-crypto   desktop, mister, miyoo
#
# Every one of these is consumed by CMakeLists.txt: see the *_ROOT set()
# lines and the link lists in the PORT_* blocks.
#
# NOTE: this script must run with the repo root as CWD. tf-psa-crypto is
# configured with TF_PSA_CRYPTO_CONFIG_FILE="configs/crypto-config-ccm-aes-
# sha256.h", a relative path that CMake resolves against the CWD, not
# against $ROOT_DIR.
#
# REMOVED — do not re-add without a consumer:
#   FreeType 2.13.3 and RmlUi 6.2 were added for the 3sxtra RmlUi lobby
#   port (docs/plan-netplay-port.md Phase 4, TRACK_B_BLOCKED.md), which was
#   abandoned in April 2026. Neither library was ever referenced by
#   CMakeLists.txt or cmake/, so neither ever reached the binary; FreeType
#   existed only because RmlUi needed it. They were cross-compiled on every
#   `--profile mister` run regardless, and the FreeType tarball fetch from
#   savannah.gnu.org later started failing (166-byte HTML error body
#   2026-08-24, hard 502 2026-08-25), blocking ARM dependency builds outright.
#   Both recipes were deleted 2026-08-29. If RmlUi is ever revived, recover
#   the recipes from git history rather than rewriting them.

# -----------------------------
# ARM cross-toolchain guard (F5 — netplay Track B review finding)
# -----------------------------
#
# The `mister` profile cross-compiles every dep for ARMv7 hard-float.
# It relies on $CC / $CXX pointing at an ARM-aware clang or gcc (we
# use `clang-20 --target=arm-linux-gnueabihf` inside the cross-build
# Docker container; see tools/mister/build-game.sh). Running this
# script on a developer host with the host compiler (e.g. macOS
# clang, Linux x86_64 gcc) silently builds x86_64 libs into
# third_party/, which then fail to link against the ARM binary in
# a confusing way deep inside the cmake link step.
#
# Guard: require $CC to be set and to look like an ARM cross-compiler
# (either an arm-linux-gnueabihf-prefixed binary, or clang with an
# ARM target in $CFLAGS). Override with MISTER_CC_IS_CROSS=1 if you
# know what you are doing.
if { [ "$PROFILE" = "mister" ] || [ "$PROFILE" = "miyoo" ]; } && [ "${MISTER_CC_IS_CROSS:-0}" != "1" ]; then
    if [ -z "${CC:-}" ]; then
        echo "ERROR: build-deps.sh --profile $PROFILE requires CC to be set to an ARM cross-compiler." >&2
        if [ "$PROFILE" = "mister" ]; then
            echo "       Run inside the Docker cross-build container:" >&2
            echo "         tools/mister/setup-build-container.sh" >&2
            echo "         docker exec 3s-mister-arm-build bash -lc 'CC=clang-20 CXX=clang++-20 bash build-deps.sh --profile mister'" >&2
        else
            echo "       Run inside the Docker cross-build container:" >&2
            echo "         tools/miyoo/setup-build-container.sh" >&2
            echo "         docker exec 3s-miyoo-arm-build bash -lc 'CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ bash build-deps.sh --profile miyoo'" >&2
        fi
        echo "       Or set MISTER_CC_IS_CROSS=1 to bypass this guard." >&2
        exit 1
    fi

    cc_version_output="$($CC --version 2>&1 || true)"
    cflags_combined="${CFLAGS:-} ${CC:-}"
    if ! echo "$cc_version_output" | grep -q -E 'arm-linux-gnueabihf|aarch64-linux' \
       && ! echo "$cflags_combined" | grep -q -E 'arm-linux-gnueabihf|aarch64-linux'; then
        echo "ERROR: build-deps.sh --profile $PROFILE expected CC to target arm-linux-gnueabihf (or aarch64-linux)." >&2
        echo "       Detected: CC='${CC}', version output did not match and \$CFLAGS lacks --target=arm-linux-gnueabihf." >&2
        echo "       Run inside the Docker cross-build container, or set MISTER_CC_IS_CROSS=1 to bypass this guard." >&2
        exit 1
    fi
fi

# -----------------------------------------------------------------------
# Task #52 — target-architecture guard for cached dependency artifacts
# -----------------------------------------------------------------------
#
# Every "already built" check below used to test only for *presence* -- a
# directory existing, or a file existing. Presence says nothing about what
# architecture the artifact was compiled for. A host-arch artifact sitting
# where an ARM one belongs therefore satisfied the guard and the ARM link
# step later failed in a confusing way (or, worse, an agent reported a
# successful "ARM" build off a cache that was never ARM).
#
# Observed instance: a host `build-deps.sh --profile desktop` run left
# third_party/tf-psa-crypto/build/lib/libtfpsacrypto.a as a Mach-O 64-bit
# arm64 object, while the container's copy of the same path is ELF EM_ARM.
# `[ -d "$TF_PSA_CRYPTO_BUILD" ]` accepted both.
#
# The guard below inspects the artifact for real, via readelf. Note that
# `file(1)` is NOT installed in the build container (debian:11 base image
# from tools/mister/setup-build-container.sh installs neither file nor
# its magic db), so readelf is the only inspection tool we can rely on.
#
# readelf -h on a static archive prints one ELF header per member, so the
# checks below require that EVERY header present agrees with the target and
# that at least one header exists. A Mach-O or an ar archive of Mach-O
# members yields no "Class:"/"Machine:" lines at all and is rejected by the
# final "at least one" test.
dep_expected_elf_machine=""
case "$PROFILE" in
    mister|miyoo)
        # Both cross profiles target 32-bit ARM hard-float; readelf spells
        # EM_ARM (0x28) as "ARM" and EM_AARCH64 as "AArch64", so an aarch64
        # artifact does not substring-match "ARM" and is correctly rejected.
        dep_expected_elf_machine="ARM"
        ;;
esac

dep_artifact_ok() {
    local artifact="$1"

    # EXISTENCE IS NOT AN ARCHITECTURE QUESTION, so it is tested on every
    # profile, ahead of the desktop early-out below.
    #
    # Task #74: the early-out used to come FIRST. On `--profile desktop` the
    # function therefore returned 0 without ever looking at $artifact, so an
    # empty third_party/tf-psa-crypto/build/ satisfied the call site's
    # `[ -d "$TF_PSA_CRYPTO_BUILD" ] && dep_cache_valid ...` (line ~877),
    # build-deps.sh printed "tf-psa-crypto already built", and the game build
    # died later on a missing psa/crypto.h. That is the same shape as the
    # defect this guard was written to fix in #52 -- a check that tests the
    # wrong thing and reports success -- merely on the other profile.
    #
    # `-s` (exists AND non-empty) rather than `-f`, because a truncated or
    # zero-byte library is a broken cache too: the desktop-reachable SDL3
    # guard tests only `ls libSDL3.so*`, and SDL3_net/minizip-ng test only
    # that the build DIRECTORY exists, so a 0-byte file passes every one of
    # those call sites. Centralising the test here covers all of them.
    if [ ! -s "$artifact" ]; then
        return 1
    fi

    # Desktop/host profile: artifacts are host-arch by definition and there is
    # nothing to cross-check, so return success without inspecting the file's
    # ARCHITECTURE. `--profile desktop` still never runs readelf and can still
    # never be rejected on arch grounds -- only on the existence test above.
    [ -n "$dep_expected_elf_machine" ] || return 0

    if ! command -v readelf >/dev/null 2>&1; then
        echo "ERROR: readelf not found; cannot verify target architecture of" >&2
        echo "       $artifact -- refusing to trust the dependency cache." >&2
        return 1
    fi

    local hdr
    hdr="$(readelf -h "$artifact" 2>/dev/null || true)"

    # Counting rather than matching is deliberate. `grep -q` exits the moment
    # it finds a hit, which closes the pipe and SIGPIPEs the writer; this file
    # runs under `set -o pipefail` (line 2), so that turns into a non-zero
    # pipeline status and the guard rejects a perfectly good artifact. It only
    # bites once the header text exceeds the 64 KiB pipe buffer, i.e. for
    # archives with enough members -- libtfpsacrypto.a has 79 -- which made it
    # a size-dependent, intermittent false rejection. `grep -c` always drains
    # its input, so there is no early close and no SIGPIPE.
    #
    # `grep -c` exits 1 on a zero count, hence the `|| true`; the substitution
    # still captures "0".
    local n_class n_class_ok n_machine n_machine_ok
    n_class="$(printf '%s\n' "$hdr" | grep -cE '^[[:space:]]*Class:' || true)"
    n_class_ok="$(printf '%s\n' "$hdr" \
        | grep -cE '^[[:space:]]*Class:[[:space:]]+ELF32[[:space:]]*$' || true)"
    n_machine="$(printf '%s\n' "$hdr" | grep -cE '^[[:space:]]*Machine:' || true)"
    n_machine_ok="$(printf '%s\n' "$hdr" \
        | grep -cE "^[[:space:]]*Machine:[[:space:]]+${dep_expected_elf_machine}[[:space:]]*\$" || true)"

    # There must be at least one ELF header, so that a non-ELF file (Mach-O, an
    # HTML error page, an empty archive) cannot pass by vacuously satisfying
    # the "nothing disagrees" tests below.
    [ "$n_class" -gt 0 ] && [ "$n_machine" -gt 0 ] || return 1

    # And every header present must agree with the target, so a mixed archive
    # with one host-arch member is rejected too.
    [ "$n_class" -eq "$n_class_ok" ] || return 1
    [ "$n_machine" -eq "$n_machine_ok" ] || return 1

    return 0
}

# Reject-and-report wrapper: same predicate, but explains itself when the
# cache is rejected so the rebuild is attributable in the build log.
dep_cache_valid() {
    local name="$1" artifact="$2"
    if dep_artifact_ok "$artifact"; then
        return 0
    fi

    # Missing-or-empty is a cache MISS, not an architecture mismatch, and it
    # is the only way the guard can fail on --profile desktop (task #74).
    # Report it separately: the arch wording below would print the empty
    # $dep_expected_elf_machine as "is not ELF32/" on desktop, and would run
    # readelf, which is not installed on a macOS host.
    if [ ! -s "$artifact" ]; then
        if [ -e "$artifact" ]; then
            echo "NOTE: $name cache at $artifact exists but is empty" >&2
            echo "      (0 bytes). Discarding it and rebuilding." >&2
        else
            echo "NOTE: $name cache directory is present but $artifact" >&2
            echo "      is missing. Discarding it and rebuilding." >&2
        fi
        return 1
    fi

    if [ -e "$artifact" ]; then
        local detail
        # A wrong-arch ELF still has Class:/Machine: lines worth printing. A
        # Mach-O, an HTML error page or a truncated file has none, and readelf
        # explains itself on stderr instead ("Not an ELF file - it has the
        # wrong magic bytes at the start"), so fall back to that.
        detail="$(readelf -h "$artifact" 2>/dev/null \
            | grep -E '^[[:space:]]*(Class|Machine):' \
            | sed 's/^[[:space:]]*//' | sort -u | tr '\n' ';' | sed 's/;$//')"
        if [ -z "$detail" ]; then
            detail="$(readelf -h "$artifact" 2>&1 >/dev/null | head -n 1)"
            [ -n "$detail" ] || detail="$(wc -c <"$artifact" | tr -d ' ') bytes, unrecognised format"
        fi
        echo "NOTE: $name cache at $artifact" >&2
        echo "      is not ELF32/$dep_expected_elf_machine -- readelf reports: ${detail}" >&2
        echo "      Discarding it and rebuilding for the target." >&2
    fi
    return 1
}

# -----------------------------------------------------------------------
# Task #52 — integrity-checked downloads
# -----------------------------------------------------------------------
#
# `curl -L -O <url>` without -f writes the server's error body to disk and
# exits 0. The motivating incident was a savannah.gnu.org hiccup that returned
# a 166-byte HTTP error page in place of a source tarball; tar then failed deep
# in the build with an unrelated-looking message. (That particular fetch — the
# FreeType recipe — has since been removed along with RmlUi, its only consumer;
# see the "Dependency set" note at the top of this file. The hazard applies to
# every remaining download.) -f makes curl fail the transfer on HTTP >= 400, and the
# sha256 pin catches every other way a fetch can be wrong (truncated body,
# mirror serving a different release, MITM).
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "ERROR: no sha256sum/shasum available to verify $1" >&2
        return 1
    fi
}

fetch_verified() {
    local url="$1" dest="$2" want="$3"
    local tmp="${dest}.part"

    rm -f "$tmp"
    if ! curl -fL --retry 3 --retry-delay 2 --retry-all-errors -o "$tmp" "$url"; then
        rm -f "$tmp"
        echo "ERROR: download failed: $url" >&2
        return 1
    fi

    local got
    got="$(sha256_of "$tmp")" || { rm -f "$tmp"; return 1; }

    if [ "$got" != "$want" ]; then
        local sz
        sz="$(wc -c < "$tmp" | tr -d ' ')"
        echo "ERROR: sha256 mismatch for $url" >&2
        echo "       expected: $want" >&2
        echo "       actual:   $got" >&2
        echo "       size:     ${sz} bytes" >&2
        rm -f "$tmp"
        return 1
    fi

    mv "$tmp" "$dest"
}

if [ -n "${JOBS:-}" ]; then
    JOBS="$JOBS"
elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"
else
    JOBS=4
fi

# Detect OS
OS="$(uname -s)"
echo "Detected OS: $OS"
echo "Dependency profile: $PROFILE"
echo "Parallel jobs: $JOBS"

echo "Using cmake from: $(which cmake)"
cmake --version

# -----------------------------
# FFmpeg
# -----------------------------

if [ "$PROFILE" = "desktop" ]; then
    FFMPEG="ffmpeg-8.0"
    FFMPEG_DIR="$THIRD_PARTY/ffmpeg"
    FFMPEG_BUILD="$FFMPEG_DIR/build"

    if [ -d "$FFMPEG_BUILD" ]; then
        echo "FFmpeg already built at $FFMPEG_BUILD"
    else
        echo "Building FFmpeg..."
        mkdir -p "$FFMPEG_DIR"
        cd "$FFMPEG_DIR"

        if [ ! -d "$FFMPEG" ]; then
            curl -L -O "https://ffmpeg.org/releases/$FFMPEG.tar.xz"
            tar xf "$FFMPEG.tar.xz"
        fi

        cd "$FFMPEG"

        mkdir -p build
        cd build

        case "$OS" in
            Darwin)
                ../configure \
                    --prefix=$FFMPEG_BUILD \
                    --disable-all --disable-autodetect \
                    --disable-static --enable-shared \
                    --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
                    --enable-decoder=adpcm_adx --enable-parser=adx --enable-muxer=adx \
                    --enable-pic \
                    --extra-cflags="-fPIC" \
                    --extra-ldflags="-Wl,-rpath,@loader_path/../Frameworks" \
                    --install-name-dir="@rpath"
                ;;
            Linux)
                ../configure \
                    --prefix=$FFMPEG_BUILD \
                    --disable-all --disable-autodetect \
                    --disable-static --enable-shared \
                    --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
                    --enable-decoder=adpcm_adx --enable-parser=adx --enable-muxer=adx \
                    --enable-pic \
                    --extra-cflags="-fPIC" \
                    --extra-ldflags="-Wl,-rpath,\$ORIGIN/../lib" \
                    --install-name-dir=\$ORIGIN
                ;;
            MINGW*|MSYS*|CYGWIN*)
                ../configure \
                    --prefix=$FFMPEG_BUILD \
                    --disable-all --disable-autodetect \
                    --disable-static --enable-shared \
                    --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
                    --enable-decoder=adpcm_adx --enable-parser=adx --enable-muxer=adx \
                    --extra-cflags="-I/mingw64/include" \
                    --extra-ldflags="-L/mingw64/lib"
                ;;
            *)
                echo "Unsupported OS: $OS"
                exit 1
                ;;
        esac

        make -j"$JOBS"
        make install
        echo "FFmpeg installed to $FFMPEG_BUILD"

        cd ../..
        rm -rf "$FFMPEG"
        rm "$FFMPEG.tar.xz"
        cd "$ROOT_DIR"
    fi
else
    echo "Skipping FFmpeg for profile '$PROFILE'"
fi

# -----------------------------
# SDL3
# -----------------------------

SDL_TAG="release-3.4.4"
if [ "$PROFILE" = "miyoo" ]; then
    # Miyoo uses a Buildroot Buster glibc cross-build of SDL3 separate
    # from the MiSTer Bullseye build at third_party/sdl3/. CMakeLists.txt
    # branches SDL3_ROOT to this path under PORT_MIYOO_MINI_PLUS.
    SDL_DIR="$THIRD_PARTY/sdl3-miyoo"
else
    SDL_DIR="$THIRD_PARTY/sdl3"
fi
SDL_BUILD="$SDL_DIR/build"

if [ -d "$SDL_BUILD/lib" ] && ls "$SDL_BUILD/lib"/libSDL3.so* >/dev/null 2>&1 \
   && dep_cache_valid "SDL3" "$SDL_BUILD/lib/libSDL3.so"; then
    echo "SDL3 already built at $SDL_BUILD"
else
    echo "Building SDL3 at $SDL_BUILD..."

    rm -rf "$SDL_BUILD"
    mkdir -p "$SDL_BUILD"
    SDL_SRC=$(mktemp -d)

    git clone \
        --branch "$SDL_TAG" \
        --single-branch \
        https://github.com/libsdl-org/SDL \
        "$SDL_SRC"

    if [ "$PROFILE" = "mister" ]; then
        cmake -S "$SDL_SRC" -B "$SDL_SRC/cmake-build" \
            -DCMAKE_INSTALL_PREFIX="$SDL_BUILD" \
            -DBUILD_SHARED_LIBS=ON \
            -DSDL_STATIC=OFF \
            -DSDL_TESTS=OFF \
            -DSDL_TEST_LIBRARY=OFF \
            -DSDL_INSTALL_TESTS=OFF \
            -DSDL_EXAMPLES=OFF \
            -DSDL_UNIX_CONSOLE_BUILD=ON \
            -DSDL_X11=OFF \
            -DSDL_WAYLAND=OFF
    elif [ "$PROFILE" = "miyoo" ]; then
        # Miyoo Mini Plus runs OnionOS (Buildroot Buster) without
        # X11/Wayland/KMSDRM. Video uses SDL_VIDEODRIVER=dummy at
        # runtime; MI_GFX bypasses SDL for present. Audio uses the
        # OSS /dev/dsp backend.
        cmake -S "$SDL_SRC" -B "$SDL_SRC/cmake-build" \
            -DCMAKE_INSTALL_PREFIX="$SDL_BUILD" \
            -DBUILD_SHARED_LIBS=ON \
            -DSDL_STATIC=OFF \
            -DSDL_TESTS=OFF \
            -DSDL_TEST_LIBRARY=OFF \
            -DSDL_INSTALL_TESTS=OFF \
            -DSDL_EXAMPLES=OFF \
            -DSDL_UNIX_CONSOLE_BUILD=ON \
            -DSDL_X11=OFF \
            -DSDL_WAYLAND=OFF \
            -DSDL_KMSDRM=OFF \
            -DSDL_OPENGL=OFF \
            -DSDL_OPENGLES=OFF \
            -DSDL_VULKAN=OFF \
            -DSDL_OSS=ON \
            -DSDL_ALSA=OFF \
            -DSDL_PIPEWIRE=OFF \
            -DSDL_PULSEAUDIO=OFF \
            -DSDL_JACK=OFF
    else
        cmake -S "$SDL_SRC" -B "$SDL_SRC/cmake-build" \
            -DCMAKE_INSTALL_PREFIX="$SDL_BUILD" \
            -DBUILD_SHARED_LIBS=ON \
            -DSDL_STATIC=OFF
    fi

    cmake --build "$SDL_SRC/cmake-build" -j"$JOBS"
    cmake --install "$SDL_SRC/cmake-build"

    rm -rf "$SDL_SRC"
    echo "SDL3 installed to $SDL_BUILD"
fi

# -----------------------------
# GekkoNet
# -----------------------------

GEKKONET_REF="7be848c"
GEKKONET_DIR="$THIRD_PARTY/GekkoNet"
GEKKONET_BUILD="$GEKKONET_DIR/build"

if [ "$PROFILE" = "miyoo" ]; then
    echo "Skipping GekkoNet for profile '$PROFILE' (Cut 1 has ENABLE_NETPLAY=OFF)"
elif [ -d "$GEKKONET_BUILD" ] && \
     [ -s "$GEKKONET_BUILD/lib/libGekkoNet.a" ] && \
     dep_cache_valid "GekkoNet" "$GEKKONET_BUILD/lib/libGekkoNet.a" && \
     perl -0ne 'exit(!(/u8 value = 0;\s*\n\s*while \(idx \+ 1 < length\)/))' \
        "$GEKKONET_BUILD/include/compression.h" 2>/dev/null && \
     grep -q '3s-arm M-4: cap RLE-decompressed output' \
        "$GEKKONET_BUILD/include/compression.h" 2>/dev/null; then
    # Self-healing: only treat the cache as valid if the cached libGekkoNet.a
    # exists non-empty AND both the RLEDecode OOB guard and the R-2 hardening
    # marker are present in the cached header (see the security-patch block
    # below). The R-2 marker in compression.h is the version sentinel for the
    # whole R-2 patch set: the C-1/C-2/L-2 guards live in src/backend.cpp and
    # the M-4 resize guard in thirdparty/zpp/serializer.h — neither file is
    # copied into the cached include tree, but all patches are applied in the
    # same rebuild block, which populates the cache by staging headers + .a
    # into a temp dir and atomically renaming it into place. A kill at any
    # point therefore leaves either (a) the previous cache untouched — which
    # still fails whichever check sent us down the rebuild path, (b) a
    # partially deleted or absent cache — rejected by the -d / -s / header
    # checks, or (c) the complete new cache. The marker can never sit next to
    # a stale unpatched libGekkoNet.a, so it is never silently reused.
    echo "GekkoNet already built (RLEDecode + R-2 hardening patched) at $GEKKONET_BUILD"
else
    echo "Building GekkoNet @ $GEKKONET_REF..."

    GEKKONET_SRC=$(mktemp -d)
    git clone https://github.com/HeatXD/GekkoNet.git "$GEKKONET_SRC"
    git -C "$GEKKONET_SRC" -c advice.detachedHead=false checkout "$GEKKONET_REF"

    # 3s-arm security patch — GekkoNet RLEDecode 1-byte OOB heap read.
    # compression.h RLEDecode() reads data[idx+1] with only an `idx < length`
    # loop guard, so an odd-length (hostile/corrupt) compressed InputMsg
    # payload reads one byte past the buffer. Patch the fetched source BEFORE
    # the cmake build so the fix compiles into libGekkoNet.a (third_party/ is
    # gitignored, so this script is the only durable place for the fix).
    # The substitution is anchored on `u8 value = 0;` (a local unique to
    # RLEDecode) so RLEEncode's identical `while (idx < length)` is untouched.
    # Fails loudly if upstream compression.h drifts from the expected form.
    GEKKONET_COMPRESSION_H="$GEKKONET_SRC/GekkoLib/include/compression.h"
    if ! perl -0ne 'exit(!(/u8 value = 0;\s*\n\s*while \(idx < length\)/))' "$GEKKONET_COMPRESSION_H"; then
        echo "ERROR: GekkoNet RLEDecode not in expected pre-patch form at ref $GEKKONET_REF;" >&2
        echo "       upstream compression.h changed. Refusing to build unpatched." >&2
        exit 1
    fi
    perl -0pi -e 's/(u8 value = 0;\s*\n\s*)while \(idx < length\)/${1}while (idx + 1 < length)/' "$GEKKONET_COMPRESSION_H"
    if ! perl -0ne 'exit(!(/u8 value = 0;\s*\n\s*while \(idx \+ 1 < length\)/))' "$GEKKONET_COMPRESSION_H"; then
        echo "ERROR: GekkoNet RLEDecode OOB guard failed to apply." >&2
        exit 1
    fi
    echo "GekkoNet: applied RLEDecode odd-length OOB guard"

    # ---------------------------------------------------------------------
    # 3s-arm security patch (R-2) — GekkoNet remote-crash hardening.
    #
    # Beyond the RLEDecode OOB above, the shipped libGekkoNet.a has three
    # remote-triggerable memory-safety bugs; a malicious/buggy peer can crash
    # or OOM the console. Patch the fetched source BEFORE the cmake build so
    # the fixes compile into libGekkoNet.a (third_party/ is gitignored, so
    # this script is the only durable place for the fix). Every patch is
    # FAIL-CLOSED: on a hostile/malformed packet the message is DROPPED
    # (return) or the deserialize throws std::out_of_range (already caught in
    # MessageSystem::HandleData) — never an assert/crash in release. Each
    # patch asserts its vulnerable pre-condition source form first and its
    # guard post-condition after, so a future GEKKONET_REF bump that drifts
    # the source fails loudly and forces re-review.
    # ---------------------------------------------------------------------
    GEKKONET_BACKEND_CPP="$GEKKONET_SRC/GekkoLib/src/backend.cpp"
    GEKKONET_SERIALIZER_H="$GEKKONET_SRC/GekkoLib/thirdparty/zpp/serializer.h"

    # --- C-2 (type confusion) --------------------------------------------
    # ParsePacket dispatches on the wire header.type, but the polymorphic
    # body's concrete type is decided independently by its own wire
    # serialization id. Each On* handler C-casts pkt.body.get() to the type
    # implied by header.type WITHOUT checking the body's real runtime type,
    # so a peer sending header.type=Inputs with e.g. a SyncMsg body makes
    # OnInputs read a fabricated std::vector (wild ptr+size) -> OOB. Replace
    # each unchecked C-cast with a dynamic_cast (the lib is built with RTTI;
    # MsgBody is polymorphic) and drop the packet on a nullptr mismatch.
    # NOTE: the two SyncMsg casts (OnSyncRequest/OnSyncResponse) are patched
    # with one global substitution; the other four are unique.
    for c2_pre in \
        '    auto body = (SyncMsg*)pkt.body.get();' \
        '    auto body = (InputMsg*)pkt.body.get();' \
        '    auto body = (InputAckMsg*)pkt.body.get();' \
        '    auto body = (SessionHealthMsg*)pkt.body.get();' \
        '    auto body = (NetworkHealthMsg*)pkt.body.get();'; do
        if ! grep -qF "$c2_pre" "$GEKKONET_BACKEND_CPP"; then
            echo "ERROR: GekkoNet backend.cpp missing expected C-cast pre-patch form at ref $GEKKONET_REF (C-2):" >&2
            echo "         $c2_pre" >&2
            echo "       backend.cpp drifted. Refusing to build unpatched." >&2
            exit 1
        fi
    done
    perl -0pi -e 's/    auto body = \(SyncMsg\*\)pkt\.body\.get\(\);/    auto body = dynamic_cast<SyncMsg*>(pkt.body.get());\n    if (!body) { return; } \/\/ 3s-arm C-2: drop type-confused packet/g' "$GEKKONET_BACKEND_CPP"
    perl -0pi -e 's/    auto body = \(InputMsg\*\)pkt\.body\.get\(\);/    auto body = dynamic_cast<InputMsg*>(pkt.body.get());\n    if (!body) { return; } \/\/ 3s-arm C-2: drop type-confused packet/' "$GEKKONET_BACKEND_CPP"
    perl -0pi -e 's/    auto body = \(InputAckMsg\*\)pkt\.body\.get\(\);/    auto body = dynamic_cast<InputAckMsg*>(pkt.body.get());\n    if (!body) { return; } \/\/ 3s-arm C-2: drop type-confused packet/' "$GEKKONET_BACKEND_CPP"
    perl -0pi -e 's/    auto body = \(SessionHealthMsg\*\)pkt\.body\.get\(\);/    auto body = dynamic_cast<SessionHealthMsg*>(pkt.body.get());\n    if (!body) { return; } \/\/ 3s-arm C-2: drop type-confused packet/' "$GEKKONET_BACKEND_CPP"
    perl -0pi -e 's/    auto body = \(NetworkHealthMsg\*\)pkt\.body\.get\(\);/    auto body = dynamic_cast<NetworkHealthMsg*>(pkt.body.get());\n    if (!body) { return; } \/\/ 3s-arm C-2: drop type-confused packet/' "$GEKKONET_BACKEND_CPP"
    if grep -qE '\((SyncMsg|InputMsg|InputAckMsg|SessionHealthMsg|NetworkHealthMsg)\*\)pkt\.body\.get\(\)' "$GEKKONET_BACKEND_CPP"; then
        echo "ERROR: GekkoNet C-2 type-check left an unchecked body C-cast in backend.cpp." >&2
        exit 1
    fi
    if [ "$(grep -c 'dynamic_cast<' "$GEKKONET_BACKEND_CPP")" -ne 6 ]; then
        echo "ERROR: GekkoNet C-2 expected 6 dynamic_cast body checks, found a different count." >&2
        exit 1
    fi
    echo "GekkoNet: applied C-2 body type-confusion guards (6 handlers)"

    # --- C-1 (OnInputs OOB read) -----------------------------------------
    # OnInputs indexes body->inputs[] (and memcpys _input_size bytes per
    # entry via AddInput) using the wire-controlled u16 input_count with NO
    # check against the actual, post-decompression inputs.size(). A peer
    # sending input_count=65535 with a tiny inputs vector reads ~256KB past
    # the buffer -> SIGSEGV. Insert a size invariant right before the
    # indexing loops: inputs.size() must be >= input_count * players *
    # _input_size, where players = _num_players for spectator packets, else
    # the number of remote handles bound to the sender address (u64 math to
    # avoid overflow). On violation, drop the packet. The handles vector is
    # fetched ONCE here and reused by the non-spectator loop below (L-4:
    # upstream fetched it a second time there — one wasted vector alloc per
    # input packet).
    # The anchor must be UNIQUE (exactly 1 occurrence): the substitution is
    # first-occurrence, so if a future ref bump introduced an earlier
    # same-text line the guard would silently land in the wrong place while
    # pre- and post-conditions still passed. Fail loud instead.
    if [ "$(grep -cF '    if (is_spectator) {' "$GEKKONET_BACKEND_CPP")" -ne 1 ]; then
        echo "ERROR: GekkoNet OnInputs not in expected pre-patch form at ref $GEKKONET_REF (C-1);" >&2
        echo "       expected exactly 1 '    if (is_spectator) {' anchor. Refusing to build unpatched." >&2
        exit 1
    fi
    C1_ANCHOR='    if (is_spectator) {'
    C1_REPL='    // 3s-arm C-1: bound wire-declared input_count against the actual
    // decompressed buffer before indexing (OOB read / SIGSEGV guard).
    std::vector<Handle> c1_handles;
    if (!is_spectator) {
        c1_handles = GetRemoteHandlesForAddress(&addr);
    }
    {
        const u32 c1_players = is_spectator
            ? (u32)_num_players
            : (u32)c1_handles.size();
        const u64 c1_required =
            (u64)input_count * (u64)c1_players * (u64)_input_size;
        if (c1_required > (u64)body->inputs.size()) {
            return; // drop malformed / hostile input packet
        }
    }

    if (is_spectator) {'
    export C1_ANCHOR C1_REPL
    perl -0pi -e 's/\Q$ENV{C1_ANCHOR}\E/$ENV{C1_REPL}/' "$GEKKONET_BACKEND_CPP"
    if ! grep -qF '3s-arm C-1: bound wire-declared input_count' "$GEKKONET_BACKEND_CPP"; then
        echo "ERROR: GekkoNet C-1 OnInputs bounds guard failed to apply." >&2
        exit 1
    fi
    echo "GekkoNet: applied C-1 OnInputs input_count bounds guard"

    # --- L-4 (redundant handles fetch) -----------------------------------
    # Reuse the handles vector the C-1 guard just computed instead of
    # re-fetching it in the non-spectator loop. Anchored on the unique
    # two-line fetch/count pair so the substitution cannot relocate.
    C1_HOIST_ANCHOR='        auto handles = GetRemoteHandlesForAddress(&addr);
        const u32 player_count = (u32)handles.size();'
    export C1_HOIST_ANCHOR
    if ! perl -0ne 'my $c = () = /\Q$ENV{C1_HOIST_ANCHOR}\E/g; exit($c != 1)' "$GEKKONET_BACKEND_CPP"; then
        echo "ERROR: GekkoNet OnInputs handles fetch not in expected pre-patch form at ref $GEKKONET_REF (L-4);" >&2
        echo "       expected exactly 1 handles/player_count pair. Refusing to build unpatched." >&2
        exit 1
    fi
    C1_HOIST_REPL='        auto handles = std::move(c1_handles); // 3s-arm L-4: reuse C-1 fetch
        const u32 player_count = (u32)handles.size();'
    export C1_HOIST_REPL
    perl -0pi -e 's/\Q$ENV{C1_HOIST_ANCHOR}\E/$ENV{C1_HOIST_REPL}/' "$GEKKONET_BACKEND_CPP"
    if ! grep -qF '3s-arm L-4: reuse C-1 fetch' "$GEKKONET_BACKEND_CPP" || \
       [ "$(grep -cF 'GetRemoteHandlesForAddress(&addr);' "$GEKKONET_BACKEND_CPP")" -ne 2 ]; then
        echo "ERROR: GekkoNet L-4 handles hoist failed to apply cleanly" >&2
        echo "       (expected exactly 2 remaining GetRemoteHandlesForAddress(&addr) call sites:" >&2
        echo "       the C-1 guard fetch and the one in OnNetworkHealth)." >&2
        exit 1
    fi
    echo "GekkoNet: applied L-4 handles hoist (single fetch per input packet)"

    # --- L-2 (Debug-build remote abort on unknown packet type) -----------
    # ParsePacket's default case hits assert(false) on an unknown wire
    # header.type. Release (NDEBUG) compiles the assert out (falls through
    # to return -> packet dropped), but a Debug build hands any
    # session-magic-valid peer a one-packet remote abort. Replace the assert
    # with an explicit drop so Debug and Release behave identically.
    L2_ANCHOR='        default:
            assert(false && "cannot process an unknown event!");
            return;'
    export L2_ANCHOR
    if ! perl -0ne 'my $c = () = /\Q$ENV{L2_ANCHOR}\E/g; exit($c != 1)' "$GEKKONET_BACKEND_CPP"; then
        echo "ERROR: GekkoNet ParsePacket default-assert not in expected pre-patch form at ref $GEKKONET_REF (L-2);" >&2
        echo "       expected exactly 1 occurrence. Refusing to build unpatched." >&2
        exit 1
    fi
    L2_REPL='        default:
            // 3s-arm L-2: unknown wire header.type -> drop the packet.
            // (Upstream assert(false) is a remote abort in Debug builds.)
            return;'
    export L2_REPL
    perl -0pi -e 's/\Q$ENV{L2_ANCHOR}\E/$ENV{L2_REPL}/' "$GEKKONET_BACKEND_CPP"
    if ! grep -qF '3s-arm L-2: unknown wire header.type' "$GEKKONET_BACKEND_CPP" || \
       grep -qF 'cannot process an unknown event' "$GEKKONET_BACKEND_CPP"; then
        echo "ERROR: GekkoNet L-2 default-case drop failed to apply." >&2
        exit 1
    fi
    echo "GekkoNet: applied L-2 ParsePacket unknown-type drop (Debug-safe)"

    # --- M-4 (unbounded deserialize resize) ------------------------------
    # zpp's resizable-container loader reads a wire-declared u32 element count
    # and resize()s the container to it BEFORE bounds-checking the input
    # bytes, so a hostile length (up to ~4G) forces a ~1GB transient
    # allocation per poll on a ~1GB MiSTer -> OOM DoS. Cap the declared size
    # to a single-UDP-datagram ceiling (65536 elements) BEFORE resizing; on
    # violation throw out_of_range (already caught in HandleData -> packet
    # dropped). Two identical resize sites (class-type and fundamental-type
    # loaders); both guarded via one global substitution. Only InputMsg's
    # inputs vector is wire-deserialized, whose legitimate size is <= the
    # sender's MAX_INPUT_SIZE (512), so 65536 never rejects real traffic.
    if [ "$(grep -c 'container.resize(size);' "$GEKKONET_SERIALIZER_H")" -ne 2 ]; then
        echo "ERROR: GekkoNet zpp serializer not in expected pre-patch form at ref $GEKKONET_REF (M-4);" >&2
        echo "       expected exactly 2 'container.resize(size);' sites. Refusing to build unpatched." >&2
        exit 1
    fi
    SZ_REPL='    if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(65536u)) {
        // 3s-arm M-4: refuse wire-declared container sizes beyond a single
        // UDP datagram ceiling BEFORE allocating; prevents ~1GB transient OOM
        // from a hostile u32 length. Caught in HandleData -> packet dropped.
        throw out_of_range("3s-arm: deserialize size exceeds sane message bound");
    }
    container.resize(size);'
    export SZ_REPL
    perl -0pi -e 's/    container\.resize\(size\);/$ENV{SZ_REPL}/g' "$GEKKONET_SERIALIZER_H"
    if [ "$(grep -c '3s-arm M-4' "$GEKKONET_SERIALIZER_H")" -ne 2 ]; then
        echo "ERROR: GekkoNet M-4 serializer resize guard failed to apply to both sites." >&2
        exit 1
    fi
    echo "GekkoNet: applied M-4 serializer resize bound (both loaders)"

    # --- M-4 (RLEDecode decompression bomb) ------------------------------
    # RLEDecode expands each (count,value) pair by up to 255x with no output
    # cap. Even with the bounded input above, a full buffer could expand ~127x.
    # Cap the decoded output at the same single-datagram ceiling; an over-cap
    # buffer is returned empty and then dropped by the OnInputs C-1 size check.
    # Anchored on the push loop (untouched by the RLE odd-length OOB patch
    # above), so the two patches are order-independent. RLEEncode untouched.
    # The anchor must be UNIQUE (exactly 1 occurrence) — the substitution is
    # first-occurrence, so a duplicate introduced by a ref bump would
    # silently misplace the cap. Fail loud instead.
    if ! perl -0ne 'my $c = () = /                for \(i32 x = 0; x < count; x\+\+\) \{\n                    result\.push_back\(value\);\n                \}/g; exit($c != 1)' "$GEKKONET_COMPRESSION_H"; then
        echo "ERROR: GekkoNet RLEDecode push loop not in expected form at ref $GEKKONET_REF (M-4);" >&2
        echo "       expected exactly 1 occurrence. compression.h drifted. Refusing to build unpatched." >&2
        exit 1
    fi
    CP_REPL='                // 3s-arm M-4: cap RLE-decompressed output. Bounds the
                // decompression-bomb expansion (~127x) to a single-datagram
                // ceiling; an over-cap buffer is dropped by OnInputs C-1.
                if ((u64)result.size() + (u64)count > (u64)65536u) {
                    return {};
                }
                for (i32 x = 0; x < count; x++) {
                    result.push_back(value);
                }'
    export CP_REPL
    perl -0pi -e 's/                for \(i32 x = 0; x < count; x\+\+\) \{\n                    result\.push_back\(value\);\n                \}/$ENV{CP_REPL}/' "$GEKKONET_COMPRESSION_H"
    if ! grep -qF '3s-arm M-4: cap RLE-decompressed output' "$GEKKONET_COMPRESSION_H"; then
        echo "ERROR: GekkoNet M-4 RLEDecode output cap failed to apply." >&2
        exit 1
    fi
    echo "GekkoNet: applied M-4 RLEDecode output cap"

    cmake -S "$GEKKONET_SRC" -B "$GEKKONET_SRC/cmake-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNO_ASIO_BUILD=ON \
        -DBUILD_SHARED_LIBS=OFF

    cmake --build "$GEKKONET_SRC/cmake-build" -j"$JOBS"

    # Populate the cache CRASH-SAFELY: stage the complete artifact set
    # (patched headers + libGekkoNet.a) into a temp dir beside the final
    # path, then atomically rename() it into place. Copying headers straight
    # into $GEKKONET_BUILD would write the R-2 sentinel next to whatever .a
    # was already cached; a kill before the .a copy would then leave a cache
    # that PASSES the marker check while shipping a stale unpatched library.
    # With the stage + rename, a kill at any point leaves the previous cache
    # untouched (still invalid, still rebuilt) or the complete new one.
    GEKKONET_STAGE="$GEKKONET_BUILD.staging"
    rm -rf "$GEKKONET_STAGE"
    mkdir -p "$GEKKONET_STAGE/include" "$GEKKONET_STAGE/lib"
    cp -r "$GEKKONET_SRC/GekkoLib/include/." "$GEKKONET_STAGE/include/"
    find "$GEKKONET_SRC" -name "*.a" -exec cp {} "$GEKKONET_STAGE/lib/libGekkoNet.a" \;
    if [ ! -s "$GEKKONET_STAGE/lib/libGekkoNet.a" ]; then
        echo "ERROR: GekkoNet build produced no libGekkoNet.a to stage." >&2
        exit 1
    fi
    rm -rf "$GEKKONET_BUILD"
    mv "$GEKKONET_STAGE" "$GEKKONET_BUILD"

    rm -rf "$GEKKONET_SRC"
    echo "GekkoNet installed to $GEKKONET_BUILD"
fi

# -----------------------------
# SDL3_net
# -----------------------------

SDL3_NET_REF="92022dc"
SDL3_NET_DIR="$THIRD_PARTY/SDL_net"
SDL3_NET_BUILD="$SDL3_NET_DIR/build"

if [ "$PROFILE" = "miyoo" ]; then
    echo "Skipping SDL3_net for profile '$PROFILE' (Cut 1 has ENABLE_NETPLAY=OFF)"
elif [ -d "$SDL3_NET_BUILD" ] \
     && dep_cache_valid "SDL3_net" "$SDL3_NET_BUILD/lib/libSDL3_net.a"; then
    echo "SDL3_net already built at $SDL3_NET_BUILD"
else
    echo "Building SDL3_net @ $SDL3_NET_REF..."

    # Purge any rejected/partial prefix so a wrong-arch libSDL3_net.a cannot
    # survive alongside the new one, and so the stale cmake/ and pkgconfig/
    # files it installed cannot be picked up by the consuming cmake run.
    rm -rf "$SDL3_NET_BUILD"

    SDL3_NET_SRC=$(mktemp -d)
    git clone https://github.com/libsdl-org/SDL_net.git "$SDL3_NET_SRC"
    git -C "$SDL3_NET_SRC" -c advice.detachedHead=false checkout "$SDL3_NET_REF"

    cmake -S "$SDL3_NET_SRC" -B "$SDL3_NET_SRC/cmake-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL3_NET_BUILD" \
        -DCMAKE_PREFIX_PATH="$SDL_BUILD" \
        -DBUILD_SHARED_LIBS=OFF \
        -DSDLNET_INSTALL=ON

    cmake --build "$SDL3_NET_SRC/cmake-build" -j"$JOBS"
    cmake --install "$SDL3_NET_SRC/cmake-build"

    rm -rf "$SDL3_NET_SRC"
    echo "SDL3_net installed to $SDL3_NET_BUILD"
fi

# -----------------------------
# libcdio
# -----------------------------

if [ "$PROFILE" = "desktop" ]; then
    LIBCDIO_VERSION="2.3.0"
    LIBCDIO="libcdio-$LIBCDIO_VERSION"
    LIBCDIO_DIR="$THIRD_PARTY/libcdio"
    LIBCDIO_BUILD="$LIBCDIO_DIR/build"

    if [ -d "$LIBCDIO_DIR" ]; then
        echo "libcdio already built at $LIBCDIO_BUILD"
    else
        echo "Building libcdio..."
        mkdir -p "$LIBCDIO_DIR"
        cd "$LIBCDIO_DIR"

        if [ ! -d "$LIBCDIO" ]; then
            curl -L -O "https://github.com/libcdio/libcdio/releases/download/$LIBCDIO_VERSION/$LIBCDIO.tar.gz"
            tar xf "$LIBCDIO.tar.gz"
        fi

        cd "$LIBCDIO"

        mkdir -p build
        cd build

        sh ../configure MAKE=make \
            --prefix=$LIBCDIO_BUILD \
            --enable-static \
            --disable-shared \
            --disable-cxx \
            --disable-example-progs

        make
        make install
        echo "libcdio installed to $LIBCDIO_BUILD"

        cd ../..
        rm -rf "$LIBCDIO"
        rm "$LIBCDIO.tar.gz"
        cd "$ROOT_DIR"
    fi
else
    echo "Skipping libcdio for profile '$PROFILE'"
fi

# -----------------------------
# minizip-ng
# -----------------------------

MINIZIP_NG_TAG="4.1.0"
MINIZIP_NG_DIR="$THIRD_PARTY/minizip-ng"
MINIZIP_NG_BUILD="$MINIZIP_NG_DIR/build"

if [ -d "$MINIZIP_NG_BUILD" ] \
   && dep_cache_valid "minizip-ng" "$MINIZIP_NG_BUILD/lib/libminizip-ng.a"; then
    echo "minizip-ng already built at $MINIZIP_NG_BUILD"
else
    echo "Building minizip-ng @ $MINIZIP_NG_BUILD..."

    rm -rf "$MINIZIP_NG_BUILD"
    mkdir -p "$MINIZIP_NG_BUILD"
    MINIZIP_NG_SRC=$(mktemp -d)

    git clone \
        --branch "$MINIZIP_NG_TAG" \
        --single-branch \
        https://github.com/zlib-ng/minizip-ng \
        "$MINIZIP_NG_SRC"

    cmake -S "$MINIZIP_NG_SRC" -B "$MINIZIP_NG_SRC/cmake-build" \
        -DCMAKE_INSTALL_PREFIX="$MINIZIP_NG_BUILD" \
        -DMZ_COMPAT=OFF \
        -DMZ_ZLIB_FLAVOR=zlib \
        -DMZ_BZIP2=OFF \
        -DMZ_LZMA=OFF \
        -DMZ_PPMD=OFF \
        -DMZ_ZSTD=OFF \
        -DMZ_LIBCOMP=OFF \
        -DMZ_PKCRYPT=OFF \
        -DMZ_WZAES=OFF \
        -DMZ_OPENSSL=OFF \
        -DMZ_LIBBSD=OFF \
        -DMZ_DECOMPRESS_ONLY=ON

    cmake --build "$MINIZIP_NG_SRC/cmake-build" -j"$JOBS"
    cmake --install "$MINIZIP_NG_SRC/cmake-build"

    rm -rf "$MINIZIP_NG_SRC"
    echo "minizip-ng installed to $MINIZIP_NG_BUILD"
fi

# -----------------------------
# tf-psa-crypto
# -----------------------------

TF_PSA_CRYPTO_VERSION="1.0.0"
TF_PSA_CRYPTO_URL="https://github.com/Mbed-TLS/TF-PSA-Crypto/releases/download/tf-psa-crypto-$TF_PSA_CRYPTO_VERSION/tf-psa-crypto-$TF_PSA_CRYPTO_VERSION.tar.bz2"
TF_PSA_CRYPTO_SHA256="31f0df2ca17897b5db2757cb0307dcde267292ba21ade831663d972a7a5b7d40"
TF_PSA_CRYPTO_DIR="$THIRD_PARTY/tf-psa-crypto"
TF_PSA_CRYPTO_BUILD="$TF_PSA_CRYPTO_DIR/build"

if [ -d "$TF_PSA_CRYPTO_BUILD" ] \
   && dep_cache_valid "tf-psa-crypto" "$TF_PSA_CRYPTO_BUILD/lib/libtfpsacrypto.a"; then
    echo "tf-psa-crypto already built at $TF_PSA_CRYPTO_BUILD"
else
    echo "Building tf-psa-crypto @ $TF_PSA_CRYPTO_BUILD..."

    rm -rf "$TF_PSA_CRYPTO_BUILD"
    mkdir -p "$TF_PSA_CRYPTO_BUILD"
    TF_PSA_CRYPTO_SRC=$(mktemp -d)

    # sha256 pinned from the GitHub release asset digest reported by
    # api.github.com/repos/Mbed-TLS/TF-PSA-Crypto/releases/tags/tf-psa-crypto-1.0.0
    # ("digest": "sha256:31f0df2c...7d40", size 4440036), which matches a
    # direct download of the URL above byte-for-byte.
    fetch_verified "$TF_PSA_CRYPTO_URL" \
        "$TF_PSA_CRYPTO_SRC/tf-psa-crypto.tar.bz2" \
        "$TF_PSA_CRYPTO_SHA256"
    tar xf "$TF_PSA_CRYPTO_SRC/tf-psa-crypto.tar.bz2" -C "$TF_PSA_CRYPTO_SRC"

    cmake -S "$TF_PSA_CRYPTO_SRC/tf-psa-crypto-$TF_PSA_CRYPTO_VERSION" -B "$TF_PSA_CRYPTO_SRC/cmake-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$TF_PSA_CRYPTO_BUILD" \
        -DENABLE_PROGRAMS=OFF \
        -DENABLE_TESTING=OFF \
        -DUSE_SHARED_TF_PSA_CRYPTO_LIBRARY=OFF \
        -DUSE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON \
        -DTF_PSA_CRYPTO_CONFIG_FILE="configs/crypto-config-ccm-aes-sha256.h"

    cmake --build "$TF_PSA_CRYPTO_SRC/cmake-build" -j"$JOBS"
    cmake --install "$TF_PSA_CRYPTO_SRC/cmake-build"

    rm -rf "$TF_PSA_CRYPTO_SRC"
    echo "tf-psa-crypto installed to $TF_PSA_CRYPTO_BUILD"
fi

echo "Dependencies for profile '$PROFILE' installed in $THIRD_PARTY"
