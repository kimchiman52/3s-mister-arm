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

if [ -d "$SDL_BUILD/lib" ] && ls "$SDL_BUILD/lib"/libSDL3.so* >/dev/null 2>&1; then
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
     perl -0ne 'exit(!(/u8 value = 0;\s*\n\s*while \(idx \+ 1 < length\)/))' \
        "$GEKKONET_BUILD/include/compression.h" 2>/dev/null; then
    # Self-healing: only treat the cache as valid if the RLEDecode OOB guard
    # is present in the cached header (see the security-patch block below).
    # A pre-patch / partial cache falls through and rebuilds automatically,
    # so a stale unpatched libGekkoNet.a can never be silently reused.
    echo "GekkoNet already built (RLEDecode patched) at $GEKKONET_BUILD"
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

    cmake -S "$GEKKONET_SRC" -B "$GEKKONET_SRC/cmake-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNO_ASIO_BUILD=ON \
        -DBUILD_SHARED_LIBS=OFF

    cmake --build "$GEKKONET_SRC/cmake-build" -j"$JOBS"

    mkdir -p "$GEKKONET_BUILD/include" "$GEKKONET_BUILD/lib"
    cp -r "$GEKKONET_SRC/GekkoLib/include/." "$GEKKONET_BUILD/include/"
    find "$GEKKONET_SRC" -name "*.a" -exec cp {} "$GEKKONET_BUILD/lib/libGekkoNet.a" \;

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
elif [ -d "$SDL3_NET_BUILD" ]; then
    echo "SDL3_net already built at $SDL3_NET_BUILD"
else
    echo "Building SDL3_net @ $SDL3_NET_REF..."

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

if [ -d "$MINIZIP_NG_BUILD" ]; then
    echo "minizip-ng already built at $MINIZIP_NG_BUILD"
else
    echo "Building minizip-ng @ $MINIZIP_NG_BUILD..."

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
TF_PSA_CRYPTO_DIR="$THIRD_PARTY/tf-psa-crypto"
TF_PSA_CRYPTO_BUILD="$TF_PSA_CRYPTO_DIR/build"

if [ -d "$TF_PSA_CRYPTO_BUILD" ]; then
    echo "tf-psa-crypto already built at $TF_PSA_CRYPTO_BUILD"
else
    echo "Building tf-psa-crypto @ $TF_PSA_CRYPTO_BUILD..."

    mkdir -p "$TF_PSA_CRYPTO_BUILD"
    TF_PSA_CRYPTO_SRC=$(mktemp -d)

    curl -L -o "$TF_PSA_CRYPTO_SRC/tf-psa-crypto.tar.bz2" "$TF_PSA_CRYPTO_URL"
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

# -----------------------------
# FreeType 2.13.3 (MiSTer profile only)
# -----------------------------
#
# Needed by RmlUi for glyph rasterisation. MiSTer's Buildroot shipped FreeType
# is older than the 2.13.3 pin RmlUi expects, so we static-link our own.
# Mirrors the Batocera Pi4 recipe at
# /tmp/3sxtra/tools/batocera/rpi4/download-deps_rpi4.sh:194-214 — main
# adaptation is CMAKE_SYSTEM_PROCESSOR=arm (not aarch64) and we inherit the
# toolchain environment from the Docker cross-build container rather than
# hard-coding aarch64-linux-gnu-gcc. HarfBuzz/brotli/bz2/PNG/zlib are disabled
# to keep the archive hermetic; RmlUi does not use them for our netplay set.

if [ "$PROFILE" = "mister" ]; then
    FREETYPE_VER="2.13.3"
    FREETYPE_DIR="$THIRD_PARTY/freetype"
    FREETYPE_BUILD="$FREETYPE_DIR/build"

    if [ -d "$FREETYPE_BUILD/lib" ] && [ -f "$FREETYPE_BUILD/lib/libfreetype.a" ]; then
        echo "FreeType already built at $FREETYPE_BUILD"
    else
        echo "Building FreeType $FREETYPE_VER at $FREETYPE_BUILD..."

        mkdir -p "$FREETYPE_DIR"
        if [ ! -d "$FREETYPE_DIR/src" ]; then
            curl -L -o "$FREETYPE_DIR/freetype-$FREETYPE_VER.tar.xz" \
                "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VER.tar.xz"
            mkdir -p "$FREETYPE_DIR/src"
            tar xf "$FREETYPE_DIR/freetype-$FREETYPE_VER.tar.xz" -C "$FREETYPE_DIR/src" --strip-components=1
            rm -f "$FREETYPE_DIR/freetype-$FREETYPE_VER.tar.xz"
        fi

        cmake -S "$FREETYPE_DIR/src" -B "$FREETYPE_DIR/cmake-build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SYSTEM_NAME=Linux \
            -DCMAKE_SYSTEM_PROCESSOR=arm \
            -DCMAKE_INSTALL_PREFIX="$FREETYPE_BUILD" \
            -DBUILD_SHARED_LIBS=OFF \
            -DFT_DISABLE_HARFBUZZ=ON \
            -DFT_DISABLE_BROTLI=ON \
            -DFT_DISABLE_BZIP2=ON \
            -DFT_DISABLE_PNG=ON \
            -DFT_DISABLE_ZLIB=ON \
            -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE \
            -DCMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE \
            -DCMAKE_DISABLE_FIND_PACKAGE_BZip2=TRUE \
            -DCMAKE_DISABLE_FIND_PACKAGE_HarfBuzz=TRUE \
            -DCMAKE_DISABLE_FIND_PACKAGE_BrotliDec=TRUE

        cmake --build "$FREETYPE_DIR/cmake-build" -j"$JOBS"
        cmake --install "$FREETYPE_DIR/cmake-build"

        rm -rf "$FREETYPE_DIR/cmake-build" "$FREETYPE_DIR/src"
        echo "FreeType installed to $FREETYPE_BUILD"
    fi
else
    echo "Skipping FreeType for profile '$PROFILE'"
fi

# -----------------------------
# RmlUi 6.2 (MiSTer profile only)
# -----------------------------
#
# Netplay UI is rendered via RmlUi. We pin to tag 6.2 (matches 3sxtra). Lua
# bindings, samples, and tests are all disabled; RmlUi's samples/tests pull in
# GLFW which is not available for our target. The FreeType font engine is
# chosen explicitly via RMLUI_FONT_ENGINE=freetype and its path is injected via
# CMAKE_PREFIX_PATH.

if [ "$PROFILE" = "mister" ]; then
    RMLUI_REF="6.2"
    RMLUI_DIR="$THIRD_PARTY/rmlui"
    RMLUI_BUILD="$RMLUI_DIR/build"

    if [ -d "$RMLUI_BUILD/lib" ] && [ -f "$RMLUI_BUILD/lib/librmlui.a" ]; then
        echo "RmlUi already built at $RMLUI_BUILD"
    else
        echo "Building RmlUi @ $RMLUI_REF at $RMLUI_BUILD..."

        mkdir -p "$RMLUI_DIR"
        RMLUI_SRC=$(mktemp -d)

        git clone --depth 1 --branch "$RMLUI_REF" \
            https://github.com/mikke89/RmlUi.git "$RMLUI_SRC"

        # Notes on flags:
        #   RMLUI_SAMPLES=OFF  : no sample binaries; avoids GLFW/SDL-shell deps.
        #   BUILD_TESTING=OFF  : CMake-standard gate for RmlUi's tests; also
        #                        prevents rmlui_core from being forced-static
        #                        via the TESTS_ENABLED path.
        #   RMLUI_LUA_BINDINGS=OFF : no Lua plugin per §15 decision.
        #   RMLUI_BACKEND intentionally NOT set — backend source is only added
        #     to the build tree when RMLUI_SHELL is auto-enabled, which
        #     requires RMLUI_SAMPLES or RMLUI_TESTS. We vendor
        #     Backends/RmlUi_Renderer_SDL.cpp into our tree in Phase 5.
        cmake -S "$RMLUI_SRC" -B "$RMLUI_SRC/cmake-build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SYSTEM_NAME=Linux \
            -DCMAKE_SYSTEM_PROCESSOR=arm \
            -DCMAKE_INSTALL_PREFIX="$RMLUI_BUILD" \
            -DCMAKE_PREFIX_PATH="$FREETYPE_BUILD" \
            -DBUILD_SHARED_LIBS=OFF \
            -DBUILD_TESTING=OFF \
            -DRMLUI_FONT_ENGINE=freetype \
            -DRMLUI_LUA_BINDINGS=OFF \
            -DRMLUI_SAMPLES=OFF \
            -DRMLUI_PRECOMPILED_HEADERS=OFF \
            -DCMAKE_DISABLE_FIND_PACKAGE_Lua=TRUE

        cmake --build "$RMLUI_SRC/cmake-build" -j"$JOBS"
        cmake --install "$RMLUI_SRC/cmake-build"

        rm -rf "$RMLUI_SRC"
        echo "RmlUi installed to $RMLUI_BUILD"
    fi
else
    echo "Skipping RmlUi for profile '$PROFILE'"
fi

echo "Dependencies for profile '$PROFILE' installed in $THIRD_PARTY"
