/*
 * test_event_queue.c — Phase 6 Step 2 test harness for the netplay event
 * queue (docs/plan-netplay-phase6.md Step 2).
 *
 * Exercises the 8-slot queue by reaching the static push_event() through a
 * non-public trampoline Netplay_Test_PushEvent() exposed by netplay.c.
 * Both the trampoline and this test body are gated behind
 * ENABLE_NETPLAY_TESTS so release builds don't carry the symbol. Enable
 * with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS='-DENABLE_NETPLAY_TESTS -DNETPLAY_TEST_HOOKS'"
 * (NETPLAY_TEST_HOOKS is required because this build links every
 * src/netplay/test_*.c TU, including test_bilateral_punch.c, which needs
 * that macro to declare DirectP2P_TestHook_IsLanPeer.)
 * When the macro is undefined the TU still links but the entry point
 * prints a diagnostic and returns non-zero.
 */

#include "netplay.h"

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

// Thin trampoline exposed by netplay.c for test-only access to the static
// push_event(). See the end of src/netplay/netplay.c.
void Netplay_Test_PushEvent(NetplayEventType type);

static int fail(const char* why) {
    fprintf(stderr, "[test_event_queue] FAIL: %s\n", why);
    return 1;
}

int Netplay_Test_EventQueue(void) {
    NetplayEvent ev;

    // 1. Empty poll returns false.
    if (Netplay_PollEvent(&ev)) {
        return fail("poll on empty queue returned true");
    }

    // 2. Round-trip the three non-NONE event types in order.
    Netplay_Test_PushEvent(NETPLAY_EVENT_SYNCHRONIZING);
    Netplay_Test_PushEvent(NETPLAY_EVENT_CONNECTED);
    Netplay_Test_PushEvent(NETPLAY_EVENT_DISCONNECTED);

    if (!Netplay_PollEvent(&ev) || ev.type != NETPLAY_EVENT_SYNCHRONIZING) {
        return fail("expected SYNCHRONIZING first");
    }
    if (!Netplay_PollEvent(&ev) || ev.type != NETPLAY_EVENT_CONNECTED) {
        return fail("expected CONNECTED second");
    }
    if (!Netplay_PollEvent(&ev) || ev.type != NETPLAY_EVENT_DISCONNECTED) {
        return fail("expected DISCONNECTED third");
    }
    if (Netplay_PollEvent(&ev)) {
        return fail("expected empty after three polls");
    }

    // 3. Overflow: push 12 into an 8-slot queue; extras must be dropped
    //    silently. First 8 observed in FIFO order.
    const NetplayEventType order[12] = {
        NETPLAY_EVENT_SYNCHRONIZING, NETPLAY_EVENT_CONNECTED,
        NETPLAY_EVENT_DISCONNECTED,  NETPLAY_EVENT_SYNCHRONIZING,
        NETPLAY_EVENT_CONNECTED,     NETPLAY_EVENT_DISCONNECTED,
        NETPLAY_EVENT_SYNCHRONIZING, NETPLAY_EVENT_CONNECTED,
        NETPLAY_EVENT_DISCONNECTED,  NETPLAY_EVENT_SYNCHRONIZING,
        NETPLAY_EVENT_CONNECTED,     NETPLAY_EVENT_DISCONNECTED,
    };
    for (int i = 0; i < 12; i++) {
        Netplay_Test_PushEvent(order[i]);
    }

    for (int i = 0; i < 8; i++) {
        if (!Netplay_PollEvent(&ev)) {
            return fail("expected 8 events after overflow push");
        }
        if (ev.type != order[i]) {
            fprintf(stderr,
                    "[test_event_queue] FAIL: slot %d expected %d got %d\n",
                    i, (int)order[i], (int)ev.type);
            return 1;
        }
    }
    if (Netplay_PollEvent(&ev)) {
        return fail("queue should be empty; overflow not dropped");
    }

    fprintf(stderr, "[test_event_queue] OK\n");
    return 0;
}

#else  /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_EventQueue(void) {
    fprintf(stderr,
            "[test_event_queue] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
