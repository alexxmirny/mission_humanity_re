//
// net_queue_selftest.cpp -- `net_selftest.exe queuetest`: WHICH inbound frame a full transport
// queue is allowed to destroy (MP D24), with no game, no rig and no socket.
//
// WHY THIS SUITE EXISTS. The defect it guards is a SILENT one: the transport's inbound ring used to
// evict the oldest frame when full, and on 2026-09-02 that was measured destroying 15 replicated
// ORDERS on a peer whose main thread had frozen for 1.8 s while the other two spun at the lockstep
// horizon re-advertising at frame rate. Nothing crashed, nothing logged, and the only symptom was an
// order_queue divergence 1400 steps into a 3000-step run, once in thirty runs.
//
// A rig run cannot demonstrate the fix: the failure needs a freeze that happens by luck, so a green
// campaign is equally consistent with "fixed" and "did not happen to freeze". The decision itself is
// a pure function, so it is asserted here -- including the arm that matters most, that an ORDER is
// never the victim even when it is the oldest thing in the ring.
//
#include <stdio.h>
#include <string.h>

#include "include/mh_net_queue_policy.h"

using namespace mh::net::queue_policy;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// A stand-in ring: fixed slots, each holding one frame's first bytes and its length.
constexpr int CAP = 8;
struct Slot {
    unsigned char data[16];
    int           len;
};
Slot g_ring[CAP];

void put(int slot, unsigned char tag, int len) {
    memset(&g_ring[slot], 0, sizeof(g_ring[slot]));
    g_ring[slot].data[0] = tag;
    g_ring[slot].len     = len;
}

frame_view get(int slot) { return frame_view{g_ring[slot].data, g_ring[slot].len}; }

// A bare horizon as the wire really carries it, and an order as the wire really carries it.
void put_horizon(int slot) { put(slot, MSG_HORIZON, BARE_HORIZON_LEN); }
void put_order(int slot) { put(slot, MSG_ORDER, 1 + 0x44); }

} // namespace

int run_queuetest() {
    printf("=== queuetest (D24: a full inbound queue may only destroy a superseded horizon) ===\n");

    // ---- 1. the frame classifier ---------------------------------------------------------------
    {
        unsigned char buf[16] = {0};

        buf[0] = MSG_HORIZON;
        check("a bare horizon is evictable", is_evictable(buf, BARE_HORIZON_LEN));

        buf[0] = MSG_ORDER;
        check("an order is NOT evictable", !is_evictable(buf, 1 + 0x44));

        // THE CASE THE LENGTH TEST EXISTS FOR. The retail dispatcher walks a datagram with a cursor,
        // so a frame whose FIRST message is a horizon may carry an order right behind it. Judging by
        // the tag alone would hand that frame to the shredder.
        buf[0] = MSG_HORIZON;
        check("a horizon followed by more bytes is NOT evictable",
              !is_evictable(buf, BARE_HORIZON_LEN + 1 + 0x44));
        check("a horizon one byte short is NOT evictable", !is_evictable(buf, BARE_HORIZON_LEN - 1));

        buf[0] = 3; // MSG_KEEPALIVE -- drives the resync trigger; not ours to discard
        check("a keepalive is NOT evictable", !is_evictable(buf, BARE_HORIZON_LEN));
        buf[0] = 4; // MSG_CONTROL -- one-shot state transitions
        check("a control frame is NOT evictable", !is_evictable(buf, BARE_HORIZON_LEN));

        check("a null frame is NOT evictable", !is_evictable(nullptr, BARE_HORIZON_LEN));
        check("an empty frame is NOT evictable", !is_evictable(buf, 0));
    }

    // ---- 2. victim selection: the oldest EVICTABLE, not the oldest ------------------------------
    {
        // head=0, the ring full of horizons: the oldest is the head, i.e. exactly what the old
        // policy did. The fix must not make the common case more expensive or different.
        for (int i = 0; i < CAP; ++i) put_horizon(i);
        check("a ring of horizons evicts the head", choose_victim(0, CAP, CAP, get) == 0);
        check("...and respects a rotated head", choose_victim(5, CAP, CAP, get) == 5);

        // THE ARM THAT MATTERS: the oldest frame is an ORDER. The old policy destroyed it. This one
        // must skip past it to the first horizon behind it.
        put_order(0);
        put_order(1);
        put_horizon(2);
        for (int i = 3; i < CAP; ++i) put_horizon(i);
        check("an order at the head is skipped, not destroyed", choose_victim(0, CAP, CAP, get) == 2);

        // Rotated, with the orders straddling the wrap -- the arithmetic is modular and the test has
        // to exercise that or it is only testing the easy layout.
        for (int i = 0; i < CAP; ++i) put_horizon(i);
        put_order(6);
        put_order(7);
        put_order(0);
        check("victim selection walks the wrap", choose_victim(6, CAP, CAP, get) == 1);

        // NOTHING SAFE: every frame is an input. -1 is a real answer, and the caller is required to
        // treat it as the correctness event it is rather than picking something anyway.
        for (int i = 0; i < CAP; ++i) put_order(i);
        check("a ring with nothing superseded returns -1", choose_victim(0, CAP, CAP, get) == -1);

        // Degenerate shapes.
        check("an empty ring returns -1", choose_victim(0, 0, CAP, get) == -1);
        check("a zero-capacity ring returns -1", choose_victim(0, 4, 0, get) == -1);

        // Only `count` entries are live; stale slots past the count must not be considered, or the
        // queue would "evict" a frame it had already delivered.
        for (int i = 0; i < CAP; ++i) put_horizon(i);
        put_order(0);
        put_order(1);
        check("a partially-filled ring does not look past its count",
              choose_victim(0, 2, CAP, get) == -1);
    }

    printf("=== queuetest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
