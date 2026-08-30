cmake_minimum_required(VERSION 3.24)
project(3s-arm LANGUAGES C CXX)

include(${PROJECT_SOURCE_DIR}/cmake/imgui_sources.cmake)

# ======================================
# Compiler cache
# ======================================
#
# ccache is wired as a compiler *launcher* rather than by pointing
# CMAKE_C_COMPILER at a ccache shim. That distinction is what keeps it
# compatible with this project's provenance guards:
#
#   - The compiler CMake probes, records in CMakeCache.txt and reports in the
#     build log is still the real clang, so nothing about the toolchain
#     identity becomes indirect.
#   - Every -D stays on the command line ccache hashes. MIST_BUILD_HASH is
#     attached to src/netplay/mist_handshake.c and src/netplay/netplay.c as a
#     COMPILE_DEFINITION (see the MIST_BUILD_HASH_OVERRIDE block below), so a
#     build of a different commit presents a different command line for
#     exactly those two files and cannot hit an entry cached for another
#     commit. Files that do not carry the hash are content-identical between
#     those commits and reusing their objects is correct.
#   - ccache's default sloppiness is deliberately left alone. In particular
#     `time_macros` is NOT set, so a translation unit using __DATE__/__TIME__
#     is never served from cache.
#
# The generated cmake/port_build_config.h.in carries feature flags only and no
# build identity, so it does not force a full rebuild between commits.
#
# Must be set before the first add_executable(): a target captures
# CMAKE_<LANG>_COMPILER_LAUNCHER at creation time.
option(ENABLE_CCACHE "Use ccache as the compiler launcher when it is available" ON)
if(ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "ccache: requested but not found; building uncached")
    endif()
else()
    message(STATUS "ccache: disabled by -DENABLE_CCACHE=OFF")
endif()

# ======================================
# Options and platform setup
# ======================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PORT_MISTER "Enable MiSTer-oriented build profile" OFF)
option(ENABLE_PERF_TELEMETRY "Enable perf telemetry capture and perf-focused CLI surface" ON)

# Task #64 measurement instrument (src/test/texgroup_window_probe.c).
# OFF by default and it must stay that way: the probe keeps file-scope
# counters that land inside the rollback-determinism harness's hashed
# data image, so an ON build reports them as DIVERGENT symbols and turns
# `tools/rollback-determinism/run.sh` red for reasons that have nothing
# to do with the simulation. Turn it on only for a measurement run:
#   cmake -B build/host -DENABLE_TEXGROUP_WINDOW_PROBE=ON
option(ENABLE_TEXGROUP_WINDOW_PROBE
       "Instrument the texgroup case-2 reclaim window and getObjectHeight ok==0 returns (task #64)" OFF)

set(_ENABLE_NETPLAY_DEFAULT ON)
set(_ENABLE_ISO_IMPORT_DEFAULT ON)
set(_ENABLE_FFMPEG_ADX_DEFAULT ON)
set(_ENABLE_SDL_DIALOGS_DEFAULT ON)
set(_ENABLE_MISTER_ARM_HARDENING_DEFAULT OFF)

if(PORT_MISTER)
    # Netplay stays ON for MiSTer. Direct-P2P + bilateral hole-punch is a
    # shipping feature, and a netplay-OFF MiSTer package silently omits
    # libminiupnpc — which a subsequent `--delete` deploy then removes from the
    # device (the 2026-07-25 incident). We never want a netplay-off MiSTer
    # build; the exceptional case must pass -DENABLE_NETPLAY=OFF explicitly.
    set(_ENABLE_ISO_IMPORT_DEFAULT OFF)
    set(_ENABLE_FFMPEG_ADX_DEFAULT OFF)
    set(_ENABLE_SDL_DIALOGS_DEFAULT OFF)
    set(_ENABLE_MISTER_ARM_HARDENING_DEFAULT ON)
endif()

option(ENABLE_NETPLAY "Enable netplay and networking dependencies" ${_ENABLE_NETPLAY_DEFAULT})
option(ENABLE_ISO_IMPORT "Enable runtime ISO import flow via libcdio" ${_ENABLE_ISO_IMPORT_DEFAULT})
option(ENABLE_FFMPEG_ADX "Enable FFmpeg-backed ADX decode backend" ${_ENABLE_FFMPEG_ADX_DEFAULT})
option(ENABLE_SDL_DIALOGS "Enable SDL native dialogs and message boxes" ${_ENABLE_SDL_DIALOGS_DEFAULT})
option(ENABLE_MISTER_ARM_HARDENING
       "Enable conservative ARMv7 hard-float/NEON flags for MiSTer-target builds"
       ${_ENABLE_MISTER_ARM_HARDENING_DEFAULT})

# Step 6 of docs/plan-bilateral-hole-punch.md — function-pointer seam in
# direct_p2p.c so test_bilateral_punch.c can mock Stun_HolePunch /
# Rendezvous_Send. OFF by default; turn ON for the test build alongside
# -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS. With OFF, direct_p2p.c emits
# byte-identical machine code to pre-Step-6 (no indirect calls).
option(NETPLAY_TEST_HOOKS "Compile in test seams for direct_p2p.c (Step 6)" OFF)

# pr-243 ARM Linux display backend (fbdev + DRM/KMS) for non-MiSTer ARM Linux
# targets (Pi-class, handheld Linux, etc.). Mutually exclusive with PORT_MISTER.
# libdrm-dev must be available at build time on the target sysroot (libdrm +
# xf86drm headers).
option(PORT_ARM_FBDEV "Build pr-243's ARM Linux display backend (fbdev + DRM)" OFF)

# Miyoo Mini Plus / OnionOS port profile (SigmaStar SSD202D, Cortex-A7
# ARMv7 hard-float, Buildroot Buster glibc). Activates the MI_GFX
# hardware-blitter presenter at src/platform/app/arm/arm_display_mi_gfx.c
# and a 16bpp RGB565 software canvas. Mutually exclusive with
# PORT_MISTER and PORT_ARM_FBDEV.
option(PORT_MIYOO_MINI_PLUS "Build for Miyoo Mini Plus / OnionOS (MI_GFX presenter)" OFF)

if(PORT_ARM_FBDEV AND PORT_MISTER)
    message(FATAL_ERROR "PORT_ARM_FBDEV and PORT_MISTER are mutually exclusive.")
endif()

if(PORT_MIYOO_MINI_PLUS AND (PORT_MISTER OR PORT_ARM_FBDEV))
    message(FATAL_ERROR "PORT_MIYOO_MINI_PLUS is mutually exclusive with PORT_MISTER and PORT_ARM_FBDEV.")
endif()

message(STATUS "PORT_MISTER=${PORT_MISTER}")
message(STATUS "ENABLE_PERF_TELEMETRY=${ENABLE_PERF_TELEMETRY}")
message(STATUS "ENABLE_TEXGROUP_WINDOW_PROBE=${ENABLE_TEXGROUP_WINDOW_PROBE}")
message(STATUS "ENABLE_NETPLAY=${ENABLE_NETPLAY}")
message(STATUS "ENABLE_ISO_IMPORT=${ENABLE_ISO_IMPORT}")
message(STATUS "ENABLE_FFMPEG_ADX=${ENABLE_FFMPEG_ADX}")
message(STATUS "ENABLE_SDL_DIALOGS=${ENABLE_SDL_DIALOGS}")
message(STATUS "ENABLE_MISTER_ARM_HARDENING=${ENABLE_MISTER_ARM_HARDENING}")
message(STATUS "NETPLAY_TEST_HOOKS=${NETPLAY_TEST_HOOKS}")
message(STATUS "PORT_ARM_FBDEV=${PORT_ARM_FBDEV}")
message(STATUS "PORT_MIYOO_MINI_PLUS=${PORT_MIYOO_MINI_PLUS}")

# ======================================
# Include directories
# ======================================

include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/imgui/imgui
    ${PROJECT_SOURCE_DIR}/src/imgui/dcimgui
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/sdk
)

# ======================================
# Source collection
# ======================================

file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS src/*.c)
list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/netplay/netplay_stub\\.c$")
list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/netplay/matchmaking_stub\\.c$")

if(NOT ENABLE_NETPLAY)
    list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/netplay/.*\\.c$")
    list(APPEND GAME_SRC
        "${CMAKE_SOURCE_DIR}/src/netplay/netplay_stub.c"
        "${CMAKE_SOURCE_DIR}/src/netplay/matchmaking_stub.c"
    )
endif()

add_executable(3s-arm MACOSX_BUNDLE
    ${GAME_SRC}
    $<$<CONFIG:Debug>:${IMGUI_SOURCES}>
)

# The rollback-determinism capture (src/test/rollback_determinism.c) hashes
# the entire ~17 MB writable data/bss image once per frame. At Debug -O0 the
# byte loop is ~10x slower, which multiplies the whole harness runtime, so
# compile just this TU with -O2 in every configuration. Semantics are
# unaffected: the TU is self-contained bookkeeping, not simulation code.
set_source_files_properties(src/test/rollback_determinism.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# R-1: wire the MIST netplay handshake's build_hash to the real git short
# SHA, derived at configure time. Falls back to mist_handshake.c's baked-in
# "0000000" when git or the repo is unavailable (tarball builds, containers
# without .git access). Scoped as a per-source define so a SHA change only
# recompiles mist_handshake.c, not the whole tree. The extra -c
# safe.directory covers builds where the repo is owned by a different uid
# ("dubious ownership").
#
# MIST_BUILD_HASH_OVERRIDE: the MiSTer Docker cross-build rsyncs the source
# tree into the container WITHOUT .git (tools/mister/build-game.sh), so the
# git probe below cannot work there. build-game.sh derives the SHA on the
# host and passes it in explicitly via this cache variable.
set(MIST_BUILD_HASH_OVERRIDE "" CACHE STRING
    "Explicit git short SHA for the MIST handshake build_hash (for builds without .git access)")
set(MIST_BUILD_HASH "${MIST_BUILD_HASH_OVERRIDE}")
if(MIST_BUILD_HASH STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${CMAKE_SOURCE_DIR}"
                    -C "${CMAKE_SOURCE_DIR}" rev-parse --short=7 HEAD
            OUTPUT_VARIABLE MIST_BUILD_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _mist_git_rc
            ERROR_QUIET)
        if(NOT _mist_git_rc EQUAL 0)
            set(MIST_BUILD_HASH "")
        endif()
    endif()
endif()
if(MIST_BUILD_HASH STREQUAL "")
    message(STATUS "MIST_BUILD_HASH: git unavailable, using fallback 0000000")
else()
    message(STATUS "MIST_BUILD_HASH=${MIST_BUILD_HASH}")
    # #44: netplay.c stamps the same hash into the header of
    # <PrefPath>logs/netplay-report.txt, the one file a tester sends back.
    # It must be THIS macro and not a second source of build identity: a
    # report that disagrees with the MIST handshake's build_hash about
    # which binary produced it is worse than no report. netplay.c keeps an
    # #ifdef fallback so removing it here degrades to build=unknown rather
    # than failing to compile.
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/netplay/mist_handshake.c"
                        "${CMAKE_SOURCE_DIR}/src/netplay/netplay.c"
                 APPEND PROPERTY COMPILE_DEFINITIONS
                 MIST_BUILD_HASH="${MIST_BUILD_HASH}")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/port_build_config.h.in"
    "${CMAKE_BINARY_DIR}/generated/port/build_config.h"
)

target_include_directories(3s-arm PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
)

# ======================================
# Compiler and linker flags
# ======================================

target_compile_definitions(3s-arm PRIVATE
    $<$<CONFIG:Debug>:DEBUG>

    $<$<CONFIG:Release>:RELEASE>
    # CHECKSUM = SHA256 verify SF33RD.AFS at startup. Skip on PORT_MISTER
    # (Cortex-A9 stock 800MHz is borderline) AND on PORT_MIYOO_MINI_PLUS
    # (Cortex-A7 @ 1.2GHz on slow SD takes 60+ seconds, looks like a hang).
    $<$<AND:$<CONFIG:Release>,$<NOT:$<OR:$<BOOL:${PORT_MISTER}>,$<BOOL:${PORT_MIYOO_MINI_PLUS}>>>>:CHECKSUM>

    _POSIX_C_SOURCE=200809L
)

# Feature toggles
target_compile_definitions(3s-arm PRIVATE
    # CPS3
)

if(PORT_MISTER)
    target_compile_definitions(3s-arm PRIVATE PORT_MISTER)
    target_include_directories(3s-arm PRIVATE ${PROJECT_SOURCE_DIR}/vendor/Main_MiSTer)
endif()

if(ENABLE_NETPLAY)
    target_compile_definitions(3s-arm PRIVATE NETPLAY_ENABLED GEKKONET_STATIC GEKKONET_NO_ASIO ENABLE_NETPLAY)
endif()

if(NETPLAY_TEST_HOOKS)
    target_compile_definitions(3s-arm PRIVATE NETPLAY_TEST_HOOKS)
endif()

if(ENABLE_ISO_IMPORT)
    target_compile_definitions(3s-arm PRIVATE ENABLE_ISO_IMPORT)
endif()

if(ENABLE_FFMPEG_ADX)
    target_compile_definitions(3s-arm PRIVATE ENABLE_FFMPEG_ADX)
endif()

if(ENABLE_SDL_DIALOGS)
    target_compile_definitions(3s-arm PRIVATE ENABLE_SDL_DIALOGS)
endif()

target_compile_definitions(3s-arm PRIVATE
    CRS_VIDEO_DRIVER_SOFTWARE=1
    CRS_SW_INDEXED8_KERNEL_DEFAULT='b')
if(PORT_MISTER)
    # 16bpp RGB565 canvas — matches the FPGA DDR3 native_video_writer
    # 16bpp scanout. Other targets (Mac, PORT_ARM_FBDEV) use the default
    # 32bpp ARGB8888 canvas.
    target_compile_definitions(3s-arm PRIVATE CRS_SW_CANVAS_16BPP=1)
endif()

if(PORT_ARM_FBDEV)
    # pr-243 ARM Linux display backend. Files live under src/platform/app/arm/
    # and are gated by `#if CRS_APP_DRIVER_ARM` — they compile to empty TUs
    # on every other build profile.
    target_compile_definitions(3s-arm PRIVATE
        CRS_APP_DRIVER_ARM=1
        CRS_ARM_HAVE_DRM=1)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
    target_link_libraries(3s-arm PRIVATE PkgConfig::LIBDRM)
    message(STATUS "PORT_ARM_FBDEV=ON (pr-243 ARM Linux display backend; fbdev + DRM)")
endif()

if(PORT_MIYOO_MINI_PLUS)
    # Miyoo Mini Plus profile: SigmaStar MI_GFX hardware-blitter presenter
    # at src/platform/app/arm/arm_display_mi_gfx.c. Source canvas is
    # RGB565 to match SDL3's giblet 16bpp canvas (CRS_SW_CANVAS_16BPP=1
    # is set here, NOT in the shared block above — that block is
    # PORT_MISTER-only). The SigmaStar BSP headers and prebuilt libs
    # are vendored at vendor/sigmastar/{inc,lib}/. The deployed package
    # does NOT bundle libmi_*.so; runtime resolves them from the
    # device's /config/lib/ provided by the OnionOS firmware BSP.
    target_compile_definitions(3s-arm PRIVATE
        CRS_APP_DRIVER_ARM=1
        CRS_ARM_HAVE_MI_GFX=1
        CRS_SW_CANVAS_16BPP=1
        PORT_MIYOO_MINI_PLUS)
    target_include_directories(3s-arm PRIVATE
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/inc")
    # Link against the bare `libmi_*.so` filenames. The Miyoo Mini Plus
    # OnionOS BSP exposes the runtime libs at `/config/lib/libmi_*.so`
    # (verified on a v0.4-class Onion install via on-device probe) — no
    # `.so.0` SONAME suffix on either the on-disk filenames or the
    # working community ports' NEEDED entries. The vendored link-time
    # stubs in vendor/sigmastar/lib/ have no DT_SONAME, so the linker
    # writes the file basename `libmi_sys.so` (etc.) into NEEDED, which
    # matches what the runtime loader will find under /config/lib/.
    # libmi_ao.so is added for the direct MI_AO audio backend at
    # src/port/sound/mi_ao_backend.c. Same link-time/runtime split as
    # the other libmi_*.so: the vendored stub satisfies the link
    # against MI_AO_* symbols and the runtime loader resolves
    # libmi_ao.so from /config/lib/ on the device.
    target_link_libraries(3s-arm PRIVATE
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_sys.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_gfx.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_common.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_ao.so")
    # pthread for the MI_AO audio backend's dedicated send thread.
    # SDL3 already pulls libpthread transitively, but we link
    # explicitly so the dependency is reflected in NEEDED entries
    # regardless of SDL3 build options.
    find_package(Threads REQUIRED)
    target_link_libraries(3s-arm PRIVATE Threads::Threads)
    message(STATUS "PORT_MIYOO_MINI_PLUS=ON (MI_GFX presenter; direct MI_AO audio backend; SDL3 dummy video; SigmaStar SDK vendored at vendor/sigmastar/)")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(DISABLED_WARNINGS
        -Wpointer-to-int-cast
        -Wno-deprecated-non-prototype
        -Wno-tautological-overlap-compare
        -Wno-gnu-folding-constant
        -Wno-c2x-extensions
        -Wno-incompatible-pointer-types
        -Wno-pointer-sign
    )
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(DISABLED_WARNINGS
        -Wno-unused-but-set-variable
        -Wno-pointer-sign
        -Wno-parentheses
        -Wno-incompatible-pointer-types
        -Wno-discarded-qualifiers
        -Wno-format-overflow
        -Wno-comment
        -Wno-maybe-uninitialized
        -Wno-array-bounds
        -Wno-aggressive-loop-optimizations
        -Wno-stringop-overflow
        -Wno-restrict
        -Wno-stringop-truncation
    )
endif()

target_compile_options(3s-arm PRIVATE
    -Wall
    -Werror

    # An undefined identifier in an #if is not an error in C -- it folds to 0
    # silently, so a feature gate that is misspelled, renamed, or simply never
    # reached its compile definition goes permanently dead with no diagnostic
    # at all. Two shipped bugs in this tree had exactly that shape: task #62
    # (ENABLE_PERF_TELEMETRY blocks dead in mtrans.c/pls03.c) and task #58
    # (Screen_Mode left UI-unreachable). -Wundef is the warning that catches
    # the class, and under -Werror above it is a build failure rather than a
    # line of scrollback. Task #67.
    #
    # Consequence for new code: test an OPTIONAL macro with #ifdef/defined(),
    # and test a macro that is always defined to 0 or 1 -- everything coming
    # out of cmake/port_build_config.h.in via #cmakedefine01 -- with a bare
    # #if, making sure the translation unit actually includes the header that
    # defines it. The two forms are not interchangeable; see the comment at
    # the top of src/sf33rd/Source/Game/io/gd3rd.h for the case where getting
    # that wrong silently dropped a declaration.
    -Wundef

    -fno-strict-aliasing

    ${DISABLED_WARNINGS}

    $<$<CONFIG:Debug>:-Wno-unused-function>
    $<$<CONFIG:Debug>:-Wno-unused-variable>
    $<$<CONFIG:Debug>:-Wno-unused-but-set-variable>
)

if(PORT_MISTER AND ENABLE_MISTER_ARM_HARDENING)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mister_system_processor_lc)
    string(TOLOWER "${CMAKE_C_COMPILER_TARGET}" _mister_compiler_target_lc)
    set(_mister_armv7_target_detected OFF)

    if((_mister_compiler_target_lc MATCHES "armv7") OR (_mister_compiler_target_lc MATCHES "arm-linux-gnueabihf"))
        set(_mister_armv7_target_detected ON)
    elseif((_mister_system_processor_lc MATCHES "armv7") OR (_mister_system_processor_lc MATCHES "armhf"))
        set(_mister_armv7_target_detected ON)
    endif()

    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang") OR
       (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
        if(_mister_armv7_target_detected)
            target_compile_options(3s-arm PRIVATE
                -mcpu=cortex-a9
                -mfpu=neon-vfpv3
                -mfloat-abi=hard
            )
            target_link_options(3s-arm PRIVATE
                -mcpu=cortex-a9
                -mfpu=neon-vfpv3
                -mfloat-abi=hard
            )
            message(STATUS "MiSTer ARM hardening flags enabled: -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")
        else()
            message(WARNING
                    "PORT_MISTER hardening requested but ARMv7 hard-float target not detected "
                    "(system='${CMAKE_SYSTEM_PROCESSOR}', "
                    "compiler_target='${CMAKE_C_COMPILER_TARGET}'). Skipping ARM hardening flags.")
        endif()
    else()
        message(WARNING "PORT_MISTER ARM hardening supports Clang/GNU compilers only. Skipping hardening flags.")
    endif()
endif()

# Miyoo Mini Plus hardening — Cortex-A7 (vs MiSTer's Cortex-A9) with
# VFPv4 NEON. The FPU spec differs from MiSTer because Cortex-A7's
# integrated FPU is VFPv4-D32, not VFPv3 like Cortex-A9. -mfloat-abi=hard
# is required to link against the Buildroot hardfloat sysroot. These
# flags are unconditional under PORT_MIYOO_MINI_PLUS (no
# ENABLE_MIYOO_HARDENING toggle in Cut 1).
if(PORT_MIYOO_MINI_PLUS)
    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang") OR
       (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
        target_compile_options(3s-arm PRIVATE
            -mcpu=cortex-a7
            -mfpu=neon-vfpv4
            -mfloat-abi=hard
        )
        target_link_options(3s-arm PRIVATE
            -mcpu=cortex-a7
            -mfpu=neon-vfpv4
            -mfloat-abi=hard
        )
        message(STATUS "Miyoo ARM hardening flags enabled: -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
    endif()

    # The Miyoo container ships clang-13 (no `-Wdeprecated-non-prototype`).
    # Scope the meta-suppression to the Miyoo profile so Mac/MiSTer
    # builds still surface unknown-warning typos as errors. Both Clang
    # and AppleClang variants accept the option silently.
    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang"))
        target_compile_options(3s-arm PRIVATE -Wno-unknown-warning-option)
    endif()
endif()

# MiSTer / Miyoo Release: aggressive optimisation — -O3 (cross-build).
# Note: -flto=thin was removed because it triggers a linker segfault in
# clang-20 during the ARM cross-build (Debian bullseye toolchain).
# Cortex-A7 is slower than Cortex-A9; -O3 is desired on Miyoo too.
if(PORT_MISTER OR PORT_MIYOO_MINI_PLUS)
    target_compile_options(3s-arm PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
    target_link_options(3s-arm PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
endif()

set_target_properties(3s-arm PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist"
)

if(APPLE)
    set_target_properties(3s-arm PROPERTIES
        OUTPUT_NAME "3S-ARM"
        MACOSX_BUNDLE_BUNDLE_NAME "3S-ARM"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(3s-arm PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
endif()

# ======================================
# Dependencies
# ======================================

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
find_package(ZLIB REQUIRED)

set(FFMPEG_ROOT        "${THIRD_PARTY_DIR}/ffmpeg/build")
if(PORT_MIYOO_MINI_PLUS)
    # Miyoo Mini Plus uses a Buildroot Buster glibc cross-build of SDL3
    # at third_party/sdl3-miyoo/ (separate from the MiSTer Bullseye
    # cross-build at third_party/sdl3/). Built by build-deps.sh
    # --profile miyoo.
    set(SDL3_ROOT      "${THIRD_PARTY_DIR}/sdl3-miyoo/build")
else()
    set(SDL3_ROOT      "${THIRD_PARTY_DIR}/sdl3/build")
endif()
set(LIBCDIO_ROOT       "${THIRD_PARTY_DIR}/libcdio/build")
set(GEKKONET_ROOT      "${THIRD_PARTY_DIR}/GekkoNet/build")
set(SDL3_NET_ROOT      "${THIRD_PARTY_DIR}/SDL_net/build")
set(MINIZIP_NG_ROOT    "${THIRD_PARTY_DIR}/minizip-ng/build")
set(TF_PSA_CRYPTO_ROOT "${THIRD_PARTY_DIR}/tf-psa-crypto/build")

include_directories(
    ${SDL3_ROOT}/include
    ${MINIZIP_NG_ROOT}/include
    ${TF_PSA_CRYPTO_ROOT}/include
)

if(ENABLE_FFMPEG_ADX)
    include_directories(${FFMPEG_ROOT}/include)
endif()

if(ENABLE_ISO_IMPORT)
    include_directories(${LIBCDIO_ROOT}/include)
endif()

if(ENABLE_NETPLAY)
    include_directories(
        ${GEKKONET_ROOT}/include
        ${SDL3_NET_ROOT}/include
    )
    # Direct-P2P Step 4 (docs/plan-stun-direct-p2p.md): miniupnpc for UPnP
    # port forwarding. pkg-config in the armhf cross sysroot provides the
    # right -I / -l paths via PkgConfig::MINIUPNPC. HAVE_UPNP is set here
    # so src/netplay/upnp.c compiles the real backend (not the no-op stub).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MINIUPNPC REQUIRED IMPORTED_TARGET miniupnpc)
endif()

target_link_libraries(3s-arm PRIVATE
    m
    "${MINIZIP_NG_ROOT}/lib/libminizip-ng.a"
    "${TF_PSA_CRYPTO_ROOT}/lib/libtfpsacrypto.a"
    ZLIB::ZLIB
    stdc++
)

if(ENABLE_ISO_IMPORT)
    target_link_libraries(3s-arm PRIVATE
        "${LIBCDIO_ROOT}/lib/libiso9660.a"
        "${LIBCDIO_ROOT}/lib/libcdio.a"
    )
endif()

if(ENABLE_NETPLAY)
    target_link_libraries(3s-arm PRIVATE
        "${GEKKONET_ROOT}/lib/libGekkoNet.a"
        "${SDL3_NET_ROOT}/lib/libSDL3_net.a"
        PkgConfig::MINIUPNPC
    )
    # Direct-P2P Step 4: HAVE_UPNP flips src/netplay/upnp.c from the no-op
    # stubs to the miniupnpc-backed implementation. Gated inside the
    # ENABLE_NETPLAY block so a non-netplay build neither links miniupnpc
    # nor compiles upnp.c (glob filter excludes src/netplay/*.c above).
    target_compile_definitions(3s-arm PRIVATE HAVE_UPNP)

    # S7 (docs/plan-netplay-connection.md §9): src/netplay/natpmp.c is the
    # second mapping backend (NAT-PMP RFC 6886 / PCP RFC 6887). It is
    # picked up by the same GLOB_RECURSE CONFIGURE_DEPENDS src/*.c above
    # as upnp.c is, and there is DELIBERATELY nothing to link and nothing
    # to add to build-deps.sh: it is raw UDP on the existing socket API,
    # not libnatpmp. Please keep it that way.

endif()

if(APPLE)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.dylib
            ${FFMPEG_ROOT}/lib/libavformat.dylib
            ${FFMPEG_ROOT}/lib/libavutil.dylib
            ${FFMPEG_ROOT}/lib/libswresample.dylib
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.0.dylib
        iconv
    )
elseif(WIN32)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.dll.a
            ${FFMPEG_ROOT}/lib/libavformat.dll.a
            ${FFMPEG_ROOT}/lib/libavutil.dll.a
            ${FFMPEG_ROOT}/lib/libswresample.dll.a
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.dll.a
        iconv
        bcrypt
        dbghelp
        ws2_32
        iphlpapi
    )
    target_link_options(3s-arm PRIVATE
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:--for-linker>"
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:--pdb=3s-arm.pdb>"
    )
elseif(UNIX)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.so
            ${FFMPEG_ROOT}/lib/libavformat.so
            ${FFMPEG_ROOT}/lib/libavutil.so
            ${FFMPEG_ROOT}/lib/libswresample.so
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.so
    )
endif()

# ======================================
# Installation
# ======================================

set(PROJECT_LICENSE_FILES
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
)

if(APPLE)
    install(TARGETS 3s-arm
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION .
    )

    if(ENABLE_FFMPEG_ADX)
        file(GLOB FFMPEG_DYLIBS "${FFMPEG_ROOT}/lib/*.dylib")
        install(FILES ${FFMPEG_DYLIBS} DESTINATION 3S-ARM.app/Contents/Frameworks)
    endif()

    install(FILES
        ${SDL3_ROOT}/lib/libSDL3.0.dylib
        DESTINATION 3S-ARM.app/Contents/Frameworks
    )

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION 3S-ARM.app/Contents/Resources/licenses
    )
elseif(WIN32)
    install(TARGETS 3s-arm
        RUNTIME_DEPENDENCY_SET deps
        RUNTIME DESTINATION bin
    )

    execute_process(COMMAND cygpath -m /mingw64/bin
        OUTPUT_VARIABLE binPath
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(_runtime_dep_dirs ${binPath} "${SDL3_ROOT}/bin")
    if(ENABLE_FFMPEG_ADX)
        list(APPEND _runtime_dep_dirs "${FFMPEG_ROOT}/bin")
    endif()

    install(RUNTIME_DEPENDENCY_SET deps
        DIRECTORIES ${_runtime_dep_dirs}
        PRE_EXCLUDE_REGEXES "^api"
        POST_EXCLUDE_REGEXES ".*system32/.*\\.dll"
        DESTINATION bin
    )

    install(FILES
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:${CMAKE_CURRENT_BINARY_DIR}/3s-arm.pdb>"
        DESTINATION bin
    )

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION bin/licenses
    )
elseif(UNIX)
    install(TARGETS 3s-arm
        RUNTIME DESTINATION bin
    )

    if(ENABLE_FFMPEG_ADX)
        file(GLOB FFMPEG_SO "${FFMPEG_ROOT}/lib/*.so*")
        install(FILES ${FFMPEG_SO} DESTINATION lib)
    endif()

    file(GLOB SDL3_SO "${SDL3_ROOT}/lib/libSDL3.so*")
    install(FILES ${SDL3_SO} DESTINATION lib)

    if(ENABLE_NETPLAY)
        # Direct-P2P Step 4 follow-up (docs/plan-stun-direct-p2p.md): the
        # linked binary has libminiupnpc.so.17 in its NEEDED list and the
        # MiSTer rootfs does not ship it. pkg_check_modules populated
        # MINIUPNPC_LIBRARY_DIRS with the armhf cross sysroot libdir
        # (e.g. /usr/lib/arm-linux-gnueabihf). Glob the SONAME symlink
        # plus versioned real file and stage both under lib/ so the
        # runtime sits next to libSDL3.so*.
        foreach(_mu_dir IN LISTS MINIUPNPC_LIBRARY_DIRS)
            file(GLOB _mu_so "${_mu_dir}/libminiupnpc.so*")
            if(_mu_so)
                install(FILES ${_mu_so} DESTINATION lib)
                break()
            endif()
        endforeach()
    endif()

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION share/3s-arm/licenses
    )
endif()

# ======================================
# Packaging
# ======================================

set(CPACK_PACKAGE_NAME "3S-ARM")
set(CPACK_PACKAGE_VENDOR "CrowdedStreet")
# set(CPACK_PACKAGE_VERSION "1.0.0")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
