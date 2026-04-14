cmake_minimum_required(VERSION 3.24)
project(3s-arm LANGUAGES C CXX)

include(${PROJECT_SOURCE_DIR}/cmake/imgui_sources.cmake)

# ======================================
# Options and platform setup
# ======================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PORT_MISTER "Enable MiSTer-oriented build profile" OFF)
option(ENABLE_PERF_TELEMETRY "Enable perf telemetry capture and perf-focused CLI surface" ON)

set(_ENABLE_NETPLAY_DEFAULT ON)
set(_ENABLE_ISO_IMPORT_DEFAULT ON)
set(_ENABLE_FFMPEG_ADX_DEFAULT ON)
set(_ENABLE_SDL_DIALOGS_DEFAULT ON)
set(_ENABLE_MISTER_ARM_HARDENING_DEFAULT OFF)

if(PORT_MISTER)
    set(_ENABLE_NETPLAY_DEFAULT OFF)
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

message(STATUS "PORT_MISTER=${PORT_MISTER}")
message(STATUS "ENABLE_PERF_TELEMETRY=${ENABLE_PERF_TELEMETRY}")
message(STATUS "ENABLE_NETPLAY=${ENABLE_NETPLAY}")
message(STATUS "ENABLE_ISO_IMPORT=${ENABLE_ISO_IMPORT}")
message(STATUS "ENABLE_FFMPEG_ADX=${ENABLE_FFMPEG_ADX}")
message(STATUS "ENABLE_SDL_DIALOGS=${ENABLE_SDL_DIALOGS}")
message(STATUS "ENABLE_MISTER_ARM_HARDENING=${ENABLE_MISTER_ARM_HARDENING}")

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
    $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${PORT_MISTER}>>>:CHECKSUM>

    _POSIX_C_SOURCE=200809L
    MEMCARD_DISABLED
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

if(ENABLE_ISO_IMPORT)
    target_compile_definitions(3s-arm PRIVATE ENABLE_ISO_IMPORT)
endif()

if(ENABLE_FFMPEG_ADX)
    target_compile_definitions(3s-arm PRIVATE ENABLE_FFMPEG_ADX)
endif()

if(ENABLE_SDL_DIALOGS)
    target_compile_definitions(3s-arm PRIVATE ENABLE_SDL_DIALOGS)
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

# MiSTer Release: aggressive optimisation — -O3 (Clang cross-build)
# Note: -flto=thin was removed because it triggers a linker segfault in
# clang-20 during the ARM cross-build (Debian bullseye toolchain).
if(PORT_MISTER)
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
set(SDL3_ROOT          "${THIRD_PARTY_DIR}/sdl3/build")
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
    )
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
