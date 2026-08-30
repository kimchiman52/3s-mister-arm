# Standalone build for p2p_probe, the S8 NAT-matrix instrument.
#
# Deliberately SEPARATE from the top-level CMakeLists.txt: several lanes are
# editing that file concurrently, and the probe needs no part of the game
# (no renderer, no ROM, no audio). It compiles the production netplay cascade
# sources from ../../../src unmodified and links a headless SDL3 + SDL3_net.
#
# Configure:
#   cmake -S tools/netplay/natmatrix/probe -B <builddir> \
#         -DCMAKE_PREFIX_PATH=<sdl3 install prefix>
#
# ENABLE_NETPLAY and NETPLAY_TEST_HOOKS are forced ON below and are NOT options:
# p2p_probe.c has an #error for each, because a probe built without them would
# exit cleanly while testing nothing.
cmake_minimum_required(VERSION 3.16)
project(p2p_probe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(SRC "${REPO_ROOT}/src")

find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_net REQUIRED CONFIG)

add_executable(p2p_probe
    p2p_probe.c
    netplay_probe_stub.c
    # --- production cascade, compiled unmodified ---
    ${SRC}/netplay/direct_p2p.c
    ${SRC}/netplay/direct_p2p_handoff.c
    ${SRC}/netplay/stun.c
    ${SRC}/netplay/rendezvous.c
    ${SRC}/netplay/room_code.c
    ${SRC}/netplay/natpmp.c
    ${SRC}/netplay/upnp.c
    ${SRC}/netplay/connect_fail.c
    # Task #119: the late-punch rescue layer. No GekkoNet dependency, so it
    # is compiled UNCONDITIONALLY — a probe built without the session phase
    # still catches compile breakage in the production module.
    ${SRC}/netplay/late_punch.c
    # --- supporting port/util layers the cascade genuinely uses ---
    ${SRC}/port/config/config.c
    ${SRC}/port/config/config_helpers.c
    ${SRC}/port/paths.c
    ${SRC}/utils/sha256.c
    ${SRC}/utils/csprng.c
    # SHA-256 backend. third_party/tf-psa-crypto ships prebuilt for macOS/arm64
    # only, so on Linux we supply the small PSA subset sha256.c needs. Proven
    # byte-identical to the real backend by sha256_equiv_test.c.
    psa_shim.c
)

# psa_shim must come FIRST so <psa/crypto.h> resolves to the shim.
target_include_directories(p2p_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/psa_shim
    ${SRC}
    ${SRC}/port
)

# The equivalence proof, built against the same shim the probe uses.
add_executable(sha256_equiv_test sha256_equiv_test.c ${SRC}/utils/sha256.c psa_shim.c)
target_include_directories(sha256_equiv_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/psa_shim ${SRC})
enable_testing()
add_test(NAME sha256_equiv COMMAND sha256_equiv_test)

target_compile_definitions(p2p_probe PRIVATE
    ENABLE_NETPLAY
    NETPLAY_TEST_HOOKS
    ENABLE_NETPLAY_TESTS
)

target_link_libraries(p2p_probe PRIVATE SDL3_net::SDL3_net SDL3::SDL3)

# ---------------------------------------------------------------------------
# Task #119 -- optional post-handoff GekkoNet session phase (session_phase.c).
#
# The split-brain band under test ends in FAILED_HANDSHAKE, which lives PAST
# the handoff boundary the pre-#119 probe stopped at. When the vendored
# GekkoNet build is available (build-deps.sh installs it to
# third_party/GekkoNet/build; override with -DGEKKONET_ROOT for a
# platform-local build, e.g. the Linux netns container), the probe gains
# --session: a real gekko_create/sync loop on the punched socket through the
# production SDLNetAdapter + late_punch stack.
#
# Without the library the probe still builds and behaves exactly as before,
# and --session REFUSES with a RIG_ERROR rather than pretending (exit-code
# doctrine at the top of p2p_probe.c).
# ---------------------------------------------------------------------------
set(GEKKONET_ROOT "${REPO_ROOT}/third_party/GekkoNet/build" CACHE PATH
    "GekkoNet install prefix (include/ + lib/libGekkoNet.a) for the session phase")
if(EXISTS "${GEKKONET_ROOT}/lib/libGekkoNet.a")
    enable_language(CXX) # libGekkoNet.a is C++; the link needs the C++ runtime
    message(STATUS "p2p_probe: session phase ON (GekkoNet at ${GEKKONET_ROOT})")
    target_sources(p2p_probe PRIVATE
        session_phase.c
        ${SRC}/netplay/sdl_net_adapter.c
    )
    target_compile_definitions(p2p_probe PRIVATE PROBE_HAS_SESSION)
    target_include_directories(p2p_probe PRIVATE ${GEKKONET_ROOT}/include)
    target_link_libraries(p2p_probe PRIVATE ${GEKKONET_ROOT}/lib/libGekkoNet.a)
    set_target_properties(p2p_probe PROPERTIES LINKER_LANGUAGE CXX)
else()
    message(STATUS "p2p_probe: session phase OFF (no libGekkoNet.a under ${GEKKONET_ROOT})")
endif()

# ---------------------------------------------------------------------------
# natpmp_probe -- acceptance instrument for rig/natpmp_mock.py.
#
# Links the PRODUCTION NAT-PMP/PCP client (${SRC}/netplay/natpmp.c) unmodified
# and aims it at a gateway address via Natpmp_TestHook_SetGateway. Used to prove
# the mock speaks a dialect the shipped client actually accepts, without having
# to stand up the whole cascade.
#
# No SDL3_net and no cascade sources, so it also builds standalone:
#   cc -DNETPLAY_TEST_HOOKS -I<src> $(pkg-config --cflags sdl3) \
#      natpmp_probe.c <src>/netplay/natpmp.c <src>/utils/csprng.c \
#      $(pkg-config --libs sdl3) -o natpmp_probe
# ---------------------------------------------------------------------------
add_executable(natpmp_probe
    natpmp_probe.c
    ${SRC}/netplay/natpmp.c
    ${SRC}/utils/csprng.c
)
target_include_directories(natpmp_probe PRIVATE ${SRC} ${SRC}/port)
# NOTE: deliberately NOT ENABLE_NETPLAY_TESTS. With both defined, natpmp.c:764
# refuses to fall back to the real default route when no hook is set -- which is
# the right posture for the suite, but here it would hide a bug where the hook
# silently failed to take. Only NETPLAY_TEST_HOOKS is needed for the hook itself.
target_compile_definitions(natpmp_probe PRIVATE NETPLAY_TEST_HOOKS)
target_link_libraries(natpmp_probe PRIVATE SDL3::SDL3)
