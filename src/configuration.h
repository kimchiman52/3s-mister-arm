#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "port/build_config.h"

#include <stdbool.h>

typedef struct NetplayConfiguration {
    int p2p_local_player;
    const char* p2p_remote_ip;
    const char* matchmaking_ip;
    int matchmaking_port;
    /* Step 9 of docs/plan-stun-direct-p2p.md: direct-P2P handoff path.
     * When `direct_p2p_handoff_set` is true, main() reads the file at
     * `direct_p2p_handoff_path` after SDL init + DirectP2P_Init and
     * dispatches BeginHost/BeginJoin based on its contents. The fixed
     * 256-byte buffer holds the CLI-supplied path; argparse populates
     * it via a temporary `const char*` pointer copied in
     * `verify_configuration`. When `direct_p2p_handoff_set` is false,
     * main() still probes the config-default handoff path
     * (CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH) — if that file exists
     * the same dispatch runs, so the wrapper can either pass --direct-
     * p2p-handoff explicitly or drop the file at the default path. */
    bool direct_p2p_handoff_set;
    char direct_p2p_handoff_path[256];
} NetplayConfiguration;

typedef struct TestRunnerConfiguration {
    bool enabled;
    const char* states_path;
    /* Phase 1 Step H1 (docs/plan-frame-data-harness.md section 1.3):
     * optional path to a line-oriented `.fdi` input script played back by
     * src/test/input_script.c once training-mode gameplay has started.
     * Mutually exclusive in practice with states_path (input_script takes
     * priority in TestRunner_Prologue's PHASE_GAME branch when set). */
    const char* input_script_path;
    const char* scene_preset;
    int characters[2];
    int super_arts[2];
    bool initial_super_full;
    /* EX/Supers program Step 1 (docs/plan-frame-data-completion.md's
     * exsuper plan): -1 (unset, the default) means "don't touch the
     * training S.A.GAUGE menu cell" — zero gameplay behavior. 0-3 mirror
     * the game's own training menu options (NORMAL/MAX START/INFINITY/
     * MAXIMUM, `effe3.c:95-123`'s switch on `Training[*].contents[0][1][0]`)
     * and are pinned every frame by apply_training_sa_gauge_overrides()
     * in test_runner.c, alongside a forced init_E3_flag re-latch. */
    int training_sa_gauge;
    int preserve_game_transition;
    int delay_gameplay_inputs_until_active;
    int stage;
    /* Phase 1 Step H3 (docs/plan-frame-data-harness.md section 1.5): when
     * true, the training-mode RNG reseed in game.c's Game01() zeroes
     * Random_ix16/32/_ex via Setup_Net_Random_ix() instead of seeding
     * Random_ix32/_ex from Interrupt_Timer, so two harness runs with the
     * same input script produce byte-identical FINAL annotation
     * sequences. Only takes effect in #if DEBUG builds. */
    bool pin_rng;
} TestRunnerConfiguration;

#if ENABLE_PERF_TELEMETRY
typedef struct PerfCaptureConfiguration {
    int frame_count;
    const char* output_path;
    const char* scene;
    bool basic_mode;
    bool basic_first_window_family_snapshots;
    bool basic_first_window_render_subphases;
    bool basic_first_window_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_exact_hot_family_alpha_offpath;
    bool basic_first_window_onset_cluster_alpha_offpath;
    bool fast_non_integer_disable_reuse_telemetry;
    bool fast_non_integer_enable_subrect_alpha_telemetry;
    bool wait_for_gameplay;
    const char* wait_for_test_phase;
    const char* wait_for_runtime_state;
    int gameplay_warmup_frames;
    bool software_frame_parity_check;
} PerfCaptureConfiguration;
#endif

typedef struct Configuration {
    NetplayConfiguration netplay;
    TestRunnerConfiguration test;
#if ENABLE_PERF_TELEMETRY
    PerfCaptureConfiguration perf;
#endif
    bool probe_renderer_only;
    bool headless;
    /* Phase 6 Step 2 (docs/plan-netplay-phase6.md): when true, main() runs
     * the netplay event-queue test harness and exits. Honors the CLI flag
     * --test-netplay-event-queue. The flag is parsed unconditionally so the
     * CLI always accepts it, but the actual test is only compiled in when
     * ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS is defined. OFF builds and
     * tests-disabled builds print a diagnostic and exit 2 (see main.c
     * dispatch and src/netplay/test_event_queue.c fallback). */
    bool test_netplay_event_queue;
    /* Phase 6 Step 8 (docs/plan-netplay-phase6.md): when true, main() runs
     * the MIST handshake test harness and exits. Honors the CLI flag
     * --test-mist-handshake. Parsed unconditionally; the real test body
     * is only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test uses a
     * localhost UDP socket pair; no external network dependency. */
    bool test_mist_handshake;
    /* Step 2 of docs/plan-stun-direct-p2p.md: when true, main() runs
     * the room-code codec test harness and exits. Honors the CLI flag
     * --test-room-code. Parsed unconditionally; the real test body is
     * only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test is pure
     * in-process — no socket or network dependency. */
    bool test_room_code;
    /* Step 12 of docs/plan-stun-direct-p2p.md: when true, main() runs
     * the STUN mock-server test harness and exits. Honors the CLI flag
     * --test-stun-mock. Parsed unconditionally; the real test body is
     * only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test spins up a
     * localhost UDP listener that speaks a canned STUN Binding
     * Response with XOR-MAPPED-ADDRESS; no external network dep. */
    bool test_stun_mock;
    /* Sparse effect-pool save Option A — round-trip parity tests for the
     * pack/unpack helpers in src/netplay/game_state.c. Honors the CLI flag
     * --test-sparse-effect-save. Parsed unconditionally; real body is
     * gated by ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS, otherwise the
     * stub returns 2. Pure in-process — no GekkoNet session required. */
    bool test_sparse_effect_save;
    /* Step 6 of docs/plan-bilateral-hole-punch.md: when true, main() runs
     * the bilateral hole-punch test harness and exits. Honors the CLI flag
     * --test-bilateral-punch. Parsed unconditionally; the real test body
     * is only compiled in when ENABLE_NETPLAY=ON && ENABLE_NETPLAY_TESTS
     * is defined (otherwise the stub returns 2). The test exercises the
     * rendezvous wire codec, session-key derivation, LAN-bypass table,
     * and the kill-switch config gate; uses only localhost UDP sockets. */
    bool test_bilateral_punch;
} Configuration;

#endif
