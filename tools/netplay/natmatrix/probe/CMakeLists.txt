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
