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
// mp:U41 (2026-09-18) REPLACED THE SCAN WITH A SEQUENCE-MERGED LANE PAIR, and its arms are the second
// half of this file. D24's sixteen checks are kept exactly as they were: `is_evictable` is still the
// router predicate the lanes classify with, and `choose_victim` is still compiled -- as the REFERENCE
// MODEL the equivalence arms below run the same arrival sequence through. The premise U41 was chosen
// on is that DELIVERY ORDER DOES NOT CHANGE, and a premise is worth nothing until something can watch
// it fail, so that is what the equivalence arms are: the old one-FIFO-plus-scan model and the new lane
// pair, fed the same frames, compared element for element.
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

// ================= mp:U41 fixtures: one arrival sequence, two models =============================
//
// A frame here is (tag, len) plus the ARRIVAL ID the models carry through, which is what makes the
// comparison an order comparison rather than a content comparison. The kinds are the five shapes the
// wire really produces plus the one the length test exists for.
enum Kind {
    K_ORDER,
    K_HORIZON,
    K_KEEPALIVE,
    K_CONTROL,
    K_CHAT,
    K_HORIZON_PLUS_ORDER, // a datagram whose FIRST message is a horizon and which carries more
};

constexpr int MAX_ARRIVALS = 256;

struct Arrivals {
    unsigned char tag[MAX_ARRIVALS];
    int           len[MAX_ARRIVALS];
    int           n = 0;

    void add(Kind k) {
        if (n >= MAX_ARRIVALS) return;
        switch (k) {
            case K_ORDER: tag[n] = MSG_ORDER, len[n] = 1 + 0x44; break;
            case K_HORIZON: tag[n] = MSG_HORIZON, len[n] = BARE_HORIZON_LEN; break;
            case K_KEEPALIVE: tag[n] = 3, len[n] = 1 + 4; break;
            case K_CONTROL: tag[n] = 4, len[n] = 1 + 1 + 4 + 8; break;
            case K_CHAT: tag[n] = 5, len[n] = 1 + 1 + 1 + 27; break;
            case K_HORIZON_PLUS_ORDER:
                tag[n] = MSG_HORIZON, len[n] = BARE_HORIZON_LEN + 1 + 0x44;
                break;
        }
        ++n;
    }
    bool is_must_keep(int i) const {
        const unsigned char b[1] = {tag[i]};
        return lane_of_game_frame(b, len[i]) == lane::must_keep;
    }
};

// What a model delivered: the arrival ids, in the order they came out.
struct Drained {
    int  id[MAX_ARRIVALS];
    int  n       = 0;
    long evicted = 0;
    long refused = 0;

    void take(int arrival_id) {
        if (n < MAX_ARRIVALS) id[n++] = arrival_id;
    }
    bool same_as(const Drained &o) const {
        if (n != o.n) return false;
        for (int i = 0; i < n; ++i)
            if (id[i] != o.id[i]) return false;
        return true;
    }
    bool is_strictly_increasing() const {
        for (int i = 1; i < n; ++i)
            if (id[i] <= id[i - 1]) return false;
        return true;
    }
    bool is_arrival_order() const {
        for (int i = 0; i < n; ++i)
            if (id[i] != i) return false;
        return true;
    }
    bool holds(int arrival_id) const {
        for (int i = 0; i < n; ++i)
            if (id[i] == arrival_id) return true;
        return false;
    }
    bool keeps_all_must_keep(const Arrivals &a) const {
        for (int i = 0; i < a.n; ++i)
            if (a.is_must_keep(i) && !holds(i)) return false;
        return true;
    }
    bool drops_only_horizons(const Arrivals &a) const {
        for (int i = 0; i < a.n; ++i)
            if (!holds(i) && a.is_must_keep(i)) return false;
        return true;
    }
};

// ---- MODEL A: the OLD single FIFO + choose_victim (what shipped between D24 and U41) ------------
struct OldFifo {
    unsigned char tag[MAX_ARRIVALS];
    int           len[MAX_ARRIVALS];
    int           id[MAX_ARRIVALS];
    int           head = 0, count = 0, cap = 0;
    long          evicted = 0;

    void init(int c) { head = count = 0, cap = c, evicted = 0; }
    void push(unsigned char t, int l, int arrival_id) {
        if (count >= cap) {
            // The shipped caller: the scan's victim, or the head when it says -1.
            const int v    = choose_victim(head, count, cap, [this](int slot) {
                return frame_view{&tag[slot], len[slot]};
            });
            const int kill = (v >= 0) ? v : head;
            ++evicted;
            // close the gap by shifting the OLDER entries up, exactly as net_transport.cpp did
            const int p = (kill - head + cap) % cap;
            for (int i = p; i > 0; --i) {
                const int d = (head + i) % cap, sidx = (head + i - 1) % cap;
                tag[d] = tag[sidx], len[d] = len[sidx], id[d] = id[sidx];
            }
            head = (head + 1) % cap;
            --count;
        }
        const int slot = (head + count) % cap;
        tag[slot] = t, len[slot] = l, id[slot] = arrival_id;
        ++count;
    }
    bool pop(int *out_id) {
        if (count <= 0) return false;
        *out_id = id[head];
        head    = (head + 1) % cap;
        --count;
        return true;
    }
};

// ---- MODEL B: the lane pair ---------------------------------------------------------------------
// Run an arrival sequence through the lane pair at the given caps and drain it completely.
template <int CAP_H, int CAP_M>
void lane_pair_run(const Arrivals &a, Drained &out, int drain_after = -1) {
    mh::net::queue_policy::lane_queue<CAP_H, CAP_M> q;
    q.reset();
    int id_h[CAP_H] = {}, id_m[CAP_M] = {};
    for (int i = 0; i < a.n; ++i) {
        if (drain_after >= 0 && i == drain_after) {
            for (;;) {
                const pop_result r = q.pop();
                if (!r.ok) break;
                out.take(r.which == lane::supersedable ? id_h[r.pos] : id_m[r.pos]);
            }
        }
        const unsigned char b[1] = {a.tag[i]};
        const push_result   r    = q.push(lane_of_game_frame(b, a.len[i]));
        if (!r.accepted) continue;
        if (r.which == lane::supersedable) id_h[r.pos] = i;
        else id_m[r.pos] = i;
    }
    for (;;) {
        const pop_result r = q.pop();
        if (!r.ok) break;
        out.take(r.which == lane::supersedable ? id_h[r.pos] : id_m[r.pos]);
    }
    out.evicted = q.evicted();
    out.refused = q.refused();
}

void run_old_fifo(const Arrivals &a, int cap, Drained &out, int drain_after = -1) {
    OldFifo f;
    f.init(cap);
    for (int i = 0; i < a.n; ++i) {
        if (drain_after >= 0 && i == drain_after) {
            int got = 0;
            while (f.pop(&got)) out.take(got);
        }
        f.push(a.tag[i], a.len[i], i);
    }
    int got = 0;
    while (f.pop(&got)) out.take(got);
    out.evicted = f.evicted;
}

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

    // ================= mp:U41 -- the sequence-merged lane pair ===================================

    // ---- 3. the lane router: the SAME predicate, decided once -----------------------------------
    {
        unsigned char buf[16] = {0};
        buf[0]                = MSG_HORIZON;
        check("a bare horizon routes to lane H",
              lane_of_game_frame(buf, BARE_HORIZON_LEN) == lane::supersedable);
        check("a horizon with an order behind it routes to lane M",
              lane_of_game_frame(buf, BARE_HORIZON_LEN + 1 + 0x44) == lane::must_keep);
        buf[0] = MSG_ORDER;
        check("an order routes to lane M", lane_of_game_frame(buf, 1 + 0x44) == lane::must_keep);
        check("only lane H may lose a frame", lane_evictable(lane::supersedable) &&
                                                  !lane_evictable(lane::must_keep) &&
                                                  !lane_evictable(lane::bulk));
    }

    // ---- 4. THE EQUIVALENCE ARMS ----------------------------------------------------------------
    //
    // The whole reason this variant was chosen over coalescing the horizon lane: what comes out must
    // be what used to come out. Both models are driven from one recorded arrival sequence and their
    // drained id lists are compared.
    {
        // ARM (a): NOTHING OVERFLOWS -- the strong claim, element for element, against the OLD model.
        {
            Arrivals a;
            a.add(K_ORDER);
            a.add(K_HORIZON);
            a.add(K_HORIZON);
            a.add(K_CONTROL);
            a.add(K_HORIZON);
            a.add(K_CHAT);
            a.add(K_KEEPALIVE);
            a.add(K_HORIZON_PLUS_ORDER);
            a.add(K_HORIZON);
            a.add(K_ORDER);
            Drained old_out, new_out;
            run_old_fifo(a, 32, old_out);
            lane_pair_run<32, 32>(a, new_out);
            check("no overflow: the OLD single-FIFO drains in arrival order", old_out.is_arrival_order());
            check("no overflow: the LANE PAIR drains identically to the old model",
                  new_out.same_as(old_out));
        }

        // ARM (b): LANE H EVICTS, and the two models still agree EXACTLY. The setup is what makes
        // that a real comparison rather than a coincidence: every must_keep frame arrives first and
        // is drained before the flood, so when the old ring overflows its head IS a horizon and its
        // scan picks the same frame lane H's head eviction does. Same victims, same survivors, same
        // order -- which is precisely the case the premise had to be tested in.
        {
            Arrivals a;
            a.add(K_ORDER);
            a.add(K_CONTROL);
            Drained old_out, new_out;
            // drain the must_keep prefix out of both models, then flood
            const int flood = 20; // > cap 6, so both models shed
            for (int i = 0; i < flood; ++i) a.add(K_HORIZON);
            run_old_fifo(a, 6, old_out, 2);
            lane_pair_run<6, 6>(a, new_out, 2);
            check("lane H evicted in this arm", new_out.evicted > 0);
            check("eviction: the LANE PAIR drains identically to the old model",
                  new_out.same_as(old_out));
        }

        // ARM (c): a MIXED flood with a small lane H. The models legitimately part here (they shed at
        // different moments), so the assertion is the invariant instead of the identity -- and it is
        // the invariant the sim actually depends on.
        {
            Arrivals a;
            for (int i = 0; i < 24; ++i) {
                a.add(K_HORIZON);
                a.add(K_HORIZON);
                if ((i % 4) == 0) a.add(K_ORDER);
                if ((i % 7) == 0) a.add(K_HORIZON_PLUS_ORDER);
                if ((i % 11) == 0) a.add(K_CONTROL);
            }
            Drained out;
            lane_pair_run<4, 32>(a, out);
            check("mixed flood: lane H really did evict", out.evicted > 0);
            check("mixed flood: the merge NEVER reorders (ids strictly increase)",
                  out.is_strictly_increasing());
            check("mixed flood: every must_keep frame is delivered", out.keeps_all_must_keep(a));
            check("mixed flood: everything missing is a bare horizon", out.drops_only_horizons(a));
        }

        // ARM (d): LANE M REFUSES rather than evicting. The `-1` arm of the scan, in its new shape.
        {
            Arrivals a;
            for (int i = 0; i < 10; ++i) a.add(K_ORDER);
            Drained out;
            lane_pair_run<32, 4>(a, out);
            check("lane M full: the arrivals are REFUSED, not evicted", out.refused == 6);
            check("lane M full: nothing already accepted is destroyed", out.n == 4 && out.evicted == 0);
            check("lane M full: the four survivors are the FIRST four (no reordering)",
                  out.id[0] == 0 && out.id[1] == 1 && out.id[2] == 2 && out.id[3] == 3);
        }

        // ARM (e): lane H evicts its HEAD, O(1) -- the survivors are the newest CAP_H, in order.
        {
            Arrivals a;
            for (int i = 0; i < 10; ++i) a.add(K_HORIZON);
            Drained out;
            lane_pair_run<4, 4>(a, out);
            check("lane H full: it evicted its own head six times", out.evicted == 6);
            check("lane H full: the survivors are the NEWEST four, in arrival order",
                  out.n == 4 && out.id[0] == 6 && out.id[3] == 9);
        }
    }

    // ---- 5. the PER-MATCH reset (the pure half of U41's rollup clause) ---------------------------
    {
        mh::net::queue_policy::lane_queue<4, 2> q;
        q.reset();
        for (int i = 0; i < 8; ++i) q.push(lane::supersedable);
        for (int i = 0; i < 4; ++i) q.push(lane::must_keep);
        check("before reset: the match has numbers to report",
              q.evicted() == 4 && q.refused() == 2 && q.high_water() == 6);
        q.reset();
        check("after reset: depth, high-water, evicted and refused are ALL back to zero",
              q.depth() == 0 && q.high_water() == 0 && q.evicted() == 0 && q.refused() == 0);
        // mp:U41d -- reset() is the TRANSPORT boundary and must NOT move the epoch; only
        // reset_counters() (the MATCH boundary, checked in block 6 below) may.
        check("reset() does not touch the epoch", q.epoch() == 0);
        // And the merge still works from a clean counter -- a reset that left `next_seq_` behind
        // would still order correctly, but one that left a LANE half-full would not.
        const push_result h = q.push(lane::supersedable);
        const push_result m = q.push(lane::must_keep);
        check("after reset: both lanes start empty", h.pos == 0 && m.pos == 0);
        const pop_result first = q.pop();
        check("after reset: the merge still delivers the earlier arrival first",
              first.ok && first.which == lane::supersedable);
    }

    // ---- 6. the MATCH-BOUNDARY reset (mp:U41b): counters restart, queued frames survive ----------
    // host_rematch keeps the link up, so the second match's rollup must not inherit the first's
    // high-water or counts -- but nothing already queued may be dropped to get there.
    {
        mh::net::queue_policy::lane_queue<4, 2> q;
        q.reset();
        for (int i = 0; i < 8; ++i) q.push(lane::supersedable); // 4 evicted, H full
        for (int i = 0; i < 4; ++i) q.push(lane::must_keep);    // 2 refused, M full
        for (int i = 0; i < 5; ++i) q.pop();                    // drain to depth 1
        check("match 1: high-water 6, evicted 4, refused 2, depth 1 left queued",
              q.high_water() == 6 && q.evicted() == 4 && q.refused() == 2 && q.depth() == 1);
        const unsigned epoch0 = q.epoch();
        q.reset_counters();
        check("boundary: evicted and refused restart at zero",
              q.evicted() == 0 && q.refused() == 0);
        check("boundary: the high-water restarts at the depth ACTUALLY queued (1), not 0 and not 6",
              q.high_water() == 1 && q.high_water_h() + q.high_water_m() == 1);
        check("boundary: the queued frame SURVIVES (a match boundary is not a link teardown)",
              q.depth() == 1);
        // mp:U41d -- THE MARKER. Only reset_counters() may move this; check_queue_rollups.py's
        // verdict (and net_selftest.exe qmatchtest's mutation arm) both key off it.
        check("boundary: the epoch advanced by exactly 1", q.epoch() == epoch0 + 1);
        const pop_result left = q.pop();
        check("boundary: ...and drains normally", left.ok && q.depth() == 0);
        q.push(lane::supersedable);
        q.push(lane::supersedable);
        check("match 2: its rollup reports ITS OWN high-water (2), not match 1's 6",
              q.high_water() == 2 && q.evicted() == 0 && q.refused() == 0);
        const unsigned epoch1 = q.epoch();
        q.reset_counters();
        check("a second boundary advances the epoch again", q.epoch() == epoch1 + 1);
    }

    printf("=== queuetest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
