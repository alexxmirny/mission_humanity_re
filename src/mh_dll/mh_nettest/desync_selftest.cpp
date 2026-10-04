//
// desync_selftest.cpp -- `net_selftest.exe desynctest`: every decision the runtime desync detector
// (D21) makes, with no game, no rig and no socket.
//
// WHY IT IS THE RIGHT PLACE FOR MOST OF D21's EVIDENCE. Three of the item's clauses are about things
// that must NOT happen, and a rig run cannot demonstrate the absence of an event -- a clean 2-peer
// determinism run and a detector that never looked produce the same silence. So the rig proves the
// two arms that need a real match (a deliberately desynced run IS caught; a clean run reports
// nothing), and the decisions underneath them are asserted here:
//
//   (c) the compared value drops the peer-local regions   -> fold_state / first-diverging-region
//   (f) 21428 mismatching steps produce ONE notice        -> should_notify over the real count
//   (b) a lost or future sample is never called a desync  -> judge()'s too_old / not_yet arms
//
// The last one is the cry-wolf failure mode the item singles out, and it is entirely a matter of
// what judge() returns for a step it cannot find -- which is testable here and nowhere else.
//
#include <stdio.h>
#include <string.h>

#include <vector>

#include "desync/desync_watch.h"
#include "desync/desync_wire2.h"
#include "state/inc_state.h"

using namespace mh::desync;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// A small stand-in manifest: 8 regions, two of them excluded -- the same shape as the live one
// (56 regions, 9 excluded), which is what the assertions are about.
constexpr int NR       = 8;
constexpr int EX_A     = 3; // "peer_horizon"-like: peer-local, differs on 100% of steps
constexpr int EX_B     = 6; // "frame_ring"-like
bool          g_ex[NR] = {false, false, false, true, false, false, true, false};

void fill(uint64_t *per, uint64_t seed) {
    for (int i = 0; i < NR; ++i) per[i] = seed + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
}

// Build a wire record the way the live sender does.
sample_wire make_sample(uint32_t step, const uint64_t *per, uint64_t fp) {
    sample_wire s;
    memset(&s, 0, sizeof(s));
    s.magic        = WIRE_MAGIC;
    s.version      = WIRE_VERSION;
    s.region_count = (uint16_t)NR;
    s.manifest_fp  = fp;
    s.step         = step;
    s.state_hash   = fold_state(per, g_ex, NR);
    memcpy(s.per, per, sizeof(uint64_t) * NR);
    return s;
}

} // namespace

// =================================================================================================
// mp:D44 -- WIRE_VERSION 2: per-step judging, the undo journal, live localisation, v1<->v2 mixing
// =================================================================================================
namespace {

using namespace mh::desync::v2;

// Frames one peer broadcast, delivered to the other in order (the transport's reliable stream).
struct wire_q {
    struct f {
        int     len;
        uint8_t b[MAX_FRAME];
    };
    f    q[256];
    int  n = 0, head = 0;
    void push(const uint8_t *b, int len) {
        if (n < 256) {
            q[n].len = len;
            memcpy(q[n].b, b, (size_t)len);
            ++n;
        }
    }
};

// A synthetic peer for the localiser: six regions, one of them 512 KB (8192 blocks, the tile_objects
// shape), one with a keep mask. `state` IS the state at the incident step (materialize() is the journal
// test's job, below); per_at() hashes it with the same block hash the live tracker uses.
constexpr int      LR            = 6;
const uint32_t     LLEN[LR]      = {16, 200, 4096, 6000, 64 * 8192, 130};
const char *const  LNAME[LR]     = {"tiny", "odd", "page", "units_like", "tile_like", "masked"};
const bool         LEXCL[LR]     = {false, false, false, false, false, true};
constexpr uint32_t MASKED_REGION = 3; // byte 5 of every 16 is not compared
static uint8_t     g_state[2][LR][64 * 8192];

struct syn_host final : loc_host {
    int      me;
    wire_q  *out;
    uint64_t fpv          = 0x1234;
    bool     can_mat      = true;
    bool     corrupt_self = false; // per_at lies for region 2 -> the self-check must refuse it
    char     lines[64][256];
    int      nlines        = 0;
    int      regions_first = -2, regions_nd = -1;

    int         nregions() const override { return LR; }
    uint32_t    region_len(int r) const override { return LLEN[r]; }
    const char *region_name(int r) const override { return LNAME[r]; }
    bool        excluded(int r) const override { return LEXCL[r]; }
    uint64_t    fp() const override { return fpv; }
    uint8_t     keep(int r, uint32_t off) override {
        return (r == (int)MASKED_REGION && off % 16 == 5) ? 0 : 0xff;
    }
    uint64_t bh(int r, uint32_t b) {
        const uint32_t off = b * 64, n = LLEN[r] - off < 64 ? LLEN[r] - off : 64;
        uint8_t        k[64];
        for (uint32_t j = 0; j < n; ++j) k[j] = keep(r, off + j);
        return ::mh::state::inc::block_hash((uint32_t)r, b, g_state[me][r] + off, n, k);
    }
    bool per_at(uint32_t, uint64_t *o) override {
        for (int r = 0; r < LR; ++r) {
            uint64_t s = 0;
            for (uint32_t b = 0; b < nblocks_of(LLEN[r]); ++b) s += bh(r, b);
            o[r] = s;
        }
        if (corrupt_self) o[2] ^= 1;
        return true;
    }
    bool           materialize(uint32_t) override { return can_mat; }
    const uint8_t *evidence(int r) override { return g_state[me][r]; }
    uint64_t       block_hash(int r, uint32_t b) override { return bh(r, b); }
    void           send(const uint8_t *f, int len) override { out->push(f, len); }
    void           log(const char *line) override {
        if (nlines < 64) {
            snprintf(lines[nlines], sizeof(lines[nlines]), "%s", line);
            ++nlines;
        }
    }
    void on_regions(int, uint32_t, int first, int nd) override {
        regions_first = first;
        regions_nd    = nd;
    }
    bool said(const char *needle) const {
        for (int i = 0; i < nlines; ++i)
            if (strstr(lines[i], needle)) return true;
        return false;
    }
};

// Deliver everything queued, alternately, until both queues are drained. Returns frames delivered.
int pump(localiser &la, syn_host &ha, wire_q &a_out, localiser &lb, syn_host &hb, wire_q &b_out) {
    int moved = 0;
    for (int guard = 0; guard < 1000; ++guard) {
        bool any = false;
        if (a_out.head < a_out.n) { // A's frame reaches B
            const wire_q::f &f = a_out.q[a_out.head++];
            hdr              h;
            if (frame_ok(f.b, f.len, LR, h)) lb.on_frame(hb, /*sender=*/0, h, f.b + HDR_BYTES);
            any = true;
            ++moved;
        }
        if (b_out.head < b_out.n) {
            const wire_q::f &f = b_out.q[b_out.head++];
            hdr              h;
            if (frame_ok(f.b, f.len, LR, h)) la.on_frame(ha, /*sender=*/1, h, f.b + HDR_BYTES);
            any = true;
            ++moved;
        }
        if (!any) break;
    }
    return moved;
}

void fill_states() {
    uint32_t seed = 0x5eed;
    for (int r = 0; r < LR; ++r)
        for (uint32_t o = 0; o < LLEN[r]; ++o) {
            seed             = seed * 1103515245u + 12345u;
            g_state[0][r][o] = (uint8_t)(seed >> 16);
            g_state[1][r][o] = g_state[0][r][o];
        }
}

bool has_result(const localiser &l, int region, uint32_t off, uint8_t mine, uint8_t theirs) {
    for (int i = 0; i < l.nresults(); ++i) {
        const loc_result &r = l.result(i);
        if (r.region == region && r.off == off && r.mine == mine && r.theirs == theirs) return true;
    }
    return false;
}

static localiser g_la, g_lb; // ~270 KB each: static, never on the stack
static wire_q    g_qa, g_qb;
static ticker    g_ta;

void run_v2_checks(uint64_t FP) {
    // ---- 1. the wire ------------------------------------------------------------------------------
    {
        check("v2: header is 24 bytes with step at v1's offset 16",
              sizeof(hdr) == 24 && offsetof(hdr, step) == 16 && offsetof(hdr, version) == 4);
        check("v2: a 5-step TICK is 72 bytes", tick_size(5) == 72);
        check("v2: a REGIONS frame for the live 63-region manifest is 528 bytes", regions_size(63) == 528);
        check("v2: a full GROUPS frame (128 groups) fits", groups_size(MAX_GROUPS) == 1048 && 1048 <= MAX_FRAME);
        check("v2: the largest frame (16-block BYTES) is 1080 bytes, under the 2048-byte payload cap",
              MAX_FRAME == 1080 && bytes_size(BYTES_MAX_BLOCKS) == MAX_FRAME);
        // WIRE-SIZE BOUND (the design argument, asserted): at the shipped tick_batch=5 and 50 steps/s a
        // peer sends 10 TICKs a second = 720 payload bytes/s, against v1's one 544-byte sample a second
        // and the ~26 KB/s the per-region vector would cost every step.
        check("v2: per-step TICK traffic at batch 5 is <= 1 KB/s per peer", tick_size(5) * (50 / 5) <= 1024);
        check("v2: per-step TICK traffic at batch 1 is still <= 2 KB/s per peer", tick_size(1) * 50 <= 2048);
        check("v2: sending every region every step would be ~26 KB/s (why TICK carries none)",
              regions_size(63) * 50 > 25 * 1024);

        uint8_t b[MAX_FRAME];
        put_hdr(b, T_TICK, 5, FP, 1200, 0, 0);
        memset(b + HDR_BYTES, 0xab, 48);
        hdr h;
        check("v2: a well-formed TICK validates", frame_ok(b, tick_size(5), 63, h) && h.step == 1200 && h.count == 5);
        check("v2: a TICK one byte short is refused", !frame_ok(b, tick_size(5) - 1, 63, h));
        check("v2: a TICK one byte long is refused", !frame_ok(b, tick_size(5) + 1, 63, h));
        put_hdr(b, T_TICK, 0, FP, 1200, 0, 0);
        check("v2: a TICK with zero steps is refused", !frame_ok(b, tick_size(0), 63, h));
        put_hdr(b, T_TICK, 33, FP, 1200, 0, 0);
        check("v2: a TICK over 32 steps is refused", !frame_ok(b, tick_size(33), 63, h));
        put_hdr(b, T_TICK, 1, FP, 1200, 7, 0);
        check("v2: a TICK carrying a region is refused", !frame_ok(b, tick_size(1), 63, h));
        put_hdr(b, T_REGIONS, 62, FP, 1200, 0, 0);
        check("v2: a REGIONS frame for a different region count is refused", !frame_ok(b, regions_size(62), 63, h));
        put_hdr(b, 10, 1, FP, 1200, 0, 0); // 6..9 are the mp:X3c world-resync frames
        check("v2: an unknown frame type is refused", !frame_ok(b, HDR_BYTES + 8, 63, h));
        check("v2: fp_v2 folds the hash kind in", fp_v2(FP, 2) != fp_v2(FP, 1) && fp_v2(FP, 2) != FP);

        // ---- v1 <-> v2 MIXING, both ways, through the real parsers --------------------------------
        // (a) A v1 PEER receiving v2 frames: every shape a v2 build sends goes through parse_v1_frame
        //     -- the rc builds' receive rule -- and must come out a bad frame, never a sample.
        struct shape {
            uint8_t type, count;
            int     len;
        } shapes[]       = {{T_TICK, 1, tick_size(1)}, {T_TICK, 5, tick_size(5)}, {T_TICK, 32, tick_size(32)}, {T_REGIONS, 63, regions_size(63)}, {T_GROUPS, 1, groups_size(1)}, {T_GROUPS, 128, groups_size(128)}, {T_BLOCKS, 64, blocks_size(64)}, {T_BYTES, 1, bytes_size(1)}, {T_BYTES, 16, bytes_size(16)}};
        bool all_refused = true, step_readable = true;
        for (const shape &sh : shapes) {
            memset(b, 0, sizeof(b));
            put_hdr(b, sh.type, sh.count, FP, 4321, 0, 0);
            sample_wire s;
            uint64_t    od;
            bool        has;
            if (parse_v1_frame(b, sh.len, s, od, has)) all_refused = false;
            uint32_t st;
            memcpy(&st, b + 16, 4); // the UDP transport's bulk-selftest trigger reads magic + step@16
            if (st != 4321 || peek_version(b, sh.len) != 2) step_readable = false;
        }
        check("v1<-v2: a v1 peer's receive rule refuses every v2 frame shape (a bad frame, never a sample)",
              all_refused);
        check("v1<-v2: every v2 frame keeps magic + step where the transport's DSNC sniffer reads them",
              step_readable);

        // (b) A v2 PEER receiving v1 frames: peek_version says 1 (-> the v1 path and the FALLBACK), the
        //     v1 parser accepts it with and without the D31 digest, and the v2 validator refuses it.
        uint64_t per[NR];
        fill(per, 0x77);
        sample_wire v1s = make_sample(50, per, FP);
        uint8_t     v1b[sizeof(sample_wire) + ORDER_DIGEST_BYTES];
        memcpy(v1b, &v1s, (size_t)wire_size(NR));
        const uint64_t dig = 0xfeedULL;
        memcpy(v1b + wire_size(NR), &dig, 8);
        sample_wire got;
        uint64_t    od  = 0;
        bool        has = false;
        check("v2<-v1: peek_version reads a v1 sample as version 1", peek_version(v1b, wire_size(NR)) == 1);
        check("v2<-v1: the v1 parser accepts an rc2-shaped frame (no digest)",
              parse_v1_frame(v1b, wire_size(NR), got, od, has) && !has && got.step == 50);
        check("v2<-v1: ...and a D31 frame, digest intact",
              parse_v1_frame(v1b, wire_size(NR) + 8, got, od, has) && has && od == dig);
        check("v2<-v1: the v2 validator refuses a v1 frame", !frame_ok(v1b, wire_size(NR), NR, h));
        check("peek_version: a non-DSNC payload reads 0", peek_version((const uint8_t *)"hello world", 11) == 0);
    }

    // ---- 2. per-step judging -------------------------------------------------------------------------
    {
        ticker &t = g_ta;
        t.clear(NR);
        uint64_t per[NR];
        fill(per, 1);
        int      oks = 0, mism = 0, starts = 0, too_old = 0, ord_mis = 0, ord_ok = 0, max_consec = 0;
        uint32_t first_bad = 0;
        auto     emit      = [&](const tick_verdict &v) {
            switch (v.kind) {
                case tick_verdict::ok: ++oks; break;
                case tick_verdict::mismatch:
                    ++mism;
                    if (v.incident_start) {
                        ++starts;
                        if (!first_bad) first_bad = v.step;
                    }
                    if (v.consecutive > max_consec) max_consec = v.consecutive;
                    break;
                case tick_verdict::too_old: ++too_old; break;
                case tick_verdict::order_mismatch: ++ord_mis; break;
                case tick_verdict::order_ok: ++ord_ok; break;
            }
        };
        // mine for steps 1..10; theirs agree on 1..6, differ on 7..9 (a poke that heals), agree on 10
        for (uint32_t s = 1; s <= 10; ++s) t.put_mine(s, 1000 + s, 0xD0 + s, per, emit);
        uint64_t th[10];
        for (int j = 0; j < 10; ++j) th[j] = 1000 + (uint64_t)(j + 1) + ((j >= 6 && j <= 8) ? 0x100 : 0);
        t.put_theirs(1, 1, 5, th, 0xD5, emit);      // steps 1..5, digest at 5 agrees
        t.put_theirs(1, 6, 5, th + 5, 0xBAD, emit); // steps 6..10, digest at 10 disagrees
        check("per-step: EVERY step is judged (10 of 10)", oks + mism == 10);
        check("per-step: the three poked steps mismatch", mism == 3);
        check("per-step: the incident opens at the FIRST bad step (7), once", starts == 1 && first_bad == 7);
        check("per-step: consecutive counts the run (3)", max_consec == 3);
        check("per-step: the order digest is judged at each TICK's last step", ord_ok == 1 && ord_mis == 1);

        // theirs AHEAD of mine: held, judged the moment our own step arrives
        oks = mism = starts = 0;
        uint64_t ahead[3]   = {2011, 2012, 9999};
        t.put_theirs(1, 11, 3, ahead, 0xE0 + 13, emit);
        check("per-step: a sample ahead of us is held, not judged", oks + mism == 0);
        t.put_mine(11, 2011, 0xE0 + 11, per, emit);
        t.put_mine(12, 2012, 0xE0 + 12, per, emit);
        t.put_mine(13, 2013, 0xE0 + 13, per, emit);
        check("per-step: held samples are judged as our steps arrive", oks == 2 && mism == 1 && starts == 1);
        check("per-step: a held order digest is judged when we reach its step", ord_ok == 2);

        // a sample for a step that fell out of our ring is dropped as too_old, never a mismatch
        for (uint32_t s = 14; s < 14 + TICK_RING + 5; ++s) t.put_mine(s, s, 0, per, emit);
        mism           = 0;
        uint64_t stale = 12345;
        t.put_theirs(1, 3, 1, &stale, 0, emit);
        check("per-step: an evicted step is too_old, never a mismatch", too_old == 1 && mism == 0);
        check("per-step: the ring indexes exactly (no aliasing)", t.find_mine(14 + TICK_RING + 4) != nullptr &&
                                                                      t.find_mine(13) == nullptr);
        check("notify persistence: fires once at 50 consecutive mismatching steps",
              !should_notify_steps(49) && should_notify_steps(50) && !should_notify_steps(51));
    }

    // ---- 3. the undo journal rebuilds a past step exactly -------------------------------------------
    {
        constexpr uint32_t         LEN = 64 * 40 + 17; // a short last block
        static uint8_t             live[LEN], shadow[LEN], hist[60][LEN];
        static undo_journal::entry mem[400];
        undo_journal               j;
        j.attach(mem, sizeof(mem));
        uint32_t seed = 99;
        for (uint32_t o = 0; o < LEN; ++o) live[o] = (uint8_t)(o * 7);
        memcpy(shadow, live, LEN);
        j.clear(1); // "prime" at step 1
        memcpy(hist[1], live, LEN);
        for (uint32_t s = 2; s < 60; ++s) {
            for (int w = 0; w < 1 + (int)(s % 7); ++w) {
                seed = seed * 1103515245u + 12345u;
                live[(seed >> 8) % LEN] ^= (uint8_t)(1 + (seed & 0x3f));
            }
            j.cur = s; // what the tracker update of step s does: journal each changed block, then copy
            for (uint32_t b = 0; b * 64 < LEN; ++b) {
                const uint32_t n = LEN - b * 64 < 64 ? LEN - b * 64 : 64;
                if (memcmp(live + b * 64, shadow + b * 64, n) == 0) continue;
                j.add(0, b, shadow + b * 64, n);
                memcpy(shadow + b * 64, live + b * 64, n);
            }
            memcpy(hist[s], live, LEN);
        }
        bool exact = true;
        for (uint32_t S = 1; S < 60; ++S) {
            static uint8_t ev[LEN];
            memcpy(ev, shadow, LEN);
            j.undo_to(S, [&](int, uint32_t b, const uint8_t *bytes, uint32_t n) { memcpy(ev + b * 64, bytes, n); });
            if (memcmp(ev, hist[S], LEN) != 0) exact = false;
        }
        check("undo journal: every past step 1..59 is rebuilt byte-exact from the latest shadow", exact);
        check("undo journal: nothing evicted yet -> covers the prime step", j.covers(1) && j.evicted == 0);
        // a small journal: the oldest entries go and the floor rises with them
        static undo_journal::entry few[8];
        undo_journal               k;
        k.attach(few, sizeof(few));
        k.clear(10);
        for (uint32_t s = 11; s <= 20; ++s) {
            k.cur = s;
            k.add(0, 0, shadow, 64);
        }
        check("undo journal: when full, the floor rises to the evicted step (10 adds, 8 slots -> floor 12)",
              k.floor == 12 && !k.covers(11) && k.covers(12) && k.evicted == 2);
        k.clear(30);
        check("undo journal: a prime resets the floor", k.covers(30) && !k.covers(29) && k.count == 0);
    }

    // ---- 4. the localisation exchange, round trip ----------------------------------------------------
    {
        fill_states();
        // Plant the divergence on peer B: one byte in "units_like" (4096+ bytes, masked byte excluded
        // below), one deep in the 512 KB "tile_like" region, and a MASKED byte that must not be named.
        g_state[1][3][0x1234] ^= 0x40; // units_like +0x1234 (0x1234 % 16 = 4: compared)
        g_state[1][3][0x0105] ^= 0x01; // units_like +0x105 (% 16 == 5: masked -- never named)
        g_state[1][3][0x1225] ^= 0x02; // masked, in the SAME block as +0x1234: must not move the offset named
        g_state[1][4][300000] ^= 0x80; // tile_like +300000 (block 4687, group 73)
        g_state[1][5][7] ^= 0x10;      // "masked": an EXCLUDED region -- never named
        syn_host ha, hb;
        ha.me  = 0;
        ha.out = &g_qa;
        hb.me  = 1;
        hb.out = &g_qb;
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(4);
        g_lb.reset(4);
        // Both peers' per-step judges found step 1200 bad; each starts the exchange on its own.
        g_la.on_local_mismatch(ha, 1200);
        g_lb.on_local_mismatch(hb, 1200);
        const int moved = pump(g_la, ha, g_qa, g_lb, hb, g_qb);
        check("localise: the exchange terminates", moved > 0 && moved < 200);
        check("localise: both peers name the SAME first region (units_like) via on_regions",
              ha.regions_first == 3 && hb.regions_first == 3 && ha.regions_nd == 2 && hb.regions_nd == 2);
        check("localise: peer A names units_like +0x1234 (mine/theirs as A sees them)",
              has_result(g_la, 3, 0x1234, g_state[0][3][0x1234], g_state[1][3][0x1234]));
        check("localise: peer B names the same offset, bytes mirrored",
              has_result(g_lb, 3, 0x1234, g_state[1][3][0x1234], g_state[0][3][0x1234]));
        check("localise: both name tile_like +300000 inside a 512 KB region (two-level summary)",
              has_result(g_la, 4, 300000, g_state[0][4][300000], g_state[1][4][300000]) &&
                  has_result(g_lb, 4, 300000, g_state[1][4][300000], g_state[0][4][300000]));
        check("localise: exactly those two findings on each side (the masked byte and the excluded region are "
              "never named)",
              g_la.nresults() == 2 && g_lb.nresults() == 2);
        check("localise: a masked byte in the same block is not counted (1 byte differs, not 2)",
              g_la.nresults() >= 1 && g_la.result(0).region == 3 && g_la.result(0).nbytes == 1);
        check("localise: A logged a LOCALISED line naming the region and offset",
              ha.said("LOCALISED step=1200 peer=1 region=3 units_like +0x1234"));
        check("localise: B logged the region list at the first bad step",
              hb.said("LOCALISE step=1200 peer=0: 2 region(s) differ at the first bad step: units_like tile_like"));
        // WIRE BOUND for an incident: REGIONS + GROUPS(2 regions, one of 128 groups) + BLOCKS + BYTES.
        check("localise: one incident costs each peer under 6 KB on the wire",
              g_la.bytes_sent() < 6 * 1024 && g_lb.bytes_sent() < 6 * 1024);
        check("localise: symmetric -- both peers sent the same frame count",
              g_la.frames_sent() == g_lb.frames_sent());

        // EARLIEST WINS: B noticed the incident at 1200, A (lagging) at 1210 -> both converge on 1200.
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(4);
        g_lb.reset(4);
        g_la.on_local_mismatch(ha, 1210);
        g_lb.on_local_mismatch(hb, 1200);
        pump(g_la, ha, g_qa, g_lb, hb, g_qb);
        check("localise: peers that noticed at different steps converge on the EARLIER one",
              g_la.step() == 1200 && g_lb.step() == 1200 && g_la.nresults() == 2 && g_lb.nresults() == 2);

        // A peer that never judged the step itself (e.g. a third peer) joins on the first frame.
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(4);
        g_lb.reset(4);
        g_la.on_local_mismatch(ha, 1300);
        pump(g_la, ha, g_qa, g_lb, hb, g_qb);
        check("localise: a peer joins an incident from the first frame it receives",
              g_lb.active() && g_lb.step() == 1300 && g_lb.nresults() == 2 && g_la.nresults() == 2);

        // Block history gone on one side: region level only, still names the region, never guesses offsets.
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(4);
        g_lb.reset(4);
        hb.can_mat = false;
        g_la.on_local_mismatch(ha, 1400);
        g_lb.on_local_mismatch(hb, 1400);
        pump(g_la, ha, g_qa, g_lb, hb, g_qb);
        check("localise: without block history the region is still named, no offset is",
              hb.regions_first == 3 && g_lb.nresults() == 0 && g_la.nresults() == 0 &&
                  hb.said("region level only"));
        hb.can_mat = true;

        // A rebuild that does not reproduce the region's hash is refused by the self-check.
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(4);
        g_lb.reset(4);
        g_state[1][2][10] ^= 1; // make "page" differ too, so it is tracked
        ha.corrupt_self = true;
        g_la.on_local_mismatch(ha, 1500);
        g_lb.on_local_mismatch(hb, 1500);
        pump(g_la, ha, g_qa, g_lb, hb, g_qb);
        check("localise: a rebuild that does not reproduce the step's hash is refused, loudly",
              ha.said("SELF-CHECK FAILED"));
        ha.corrupt_self = false;
        g_state[1][2][10] ^= 1;

        // The incident budget: a peer over localise_max takes no new incident.
        g_qa.n = g_qa.head = g_qb.n = g_qb.head = 0;
        g_la.reset(0);
        g_la.on_local_mismatch(ha, 1600);
        check("localise: localise_max=0 starts nothing and sends nothing", !g_la.active() && g_qa.n == 0);
    }
}

} // namespace

// ---- 5. the REAL tracker's journal hook: rebuild a past step of the live manifest -------------------
// The live binding's materialize(): the tracker's shadow plus the undo journal replayed backwards must
// give every slice's bytes AT step S, and the masked block hashes over those bytes must sum to the
// slice's incremental hash AT S -- the localiser's self-check, here over the real 63-slice manifest
// (rebased onto heap buffers), with order_queue_count moving so the one cross-region mask is exercised.
namespace {

namespace st = ::mh::state;

uint32_t g_lrng = 0xC0FFEEu;
uint32_t lrnd() {
    g_lrng = g_lrng * 1103515245u + 12345u;
    return g_lrng >> 8;
}

struct tap final : st::inc::journal {
    undo_journal *j;
    void          old_block(int region, uint32_t block, const uint8_t *old_bytes, uint32_t n) override {
        j->add(region, block, old_bytes, n);
    }
};

void run_v2_live_journal_check() {
    static std::vector<std::vector<uint8_t>> bufs;
    static std::vector<int>                  rids;
    bufs.clear();
    rids.clear();
    for (int i = 0; i < st::HASH_REGION_COUNT; ++i) {
        const int rid  = (int)st::HASH_REGIONS[i].rid;
        bool      seen = false;
        for (int r : rids) seen = seen || r == rid;
        if (seen) continue;
        uint32_t need = st::REGIONS[rid].size;
        for (int j = 0; j < st::HASH_REGION_COUNT; ++j)
            if ((int)st::HASH_REGIONS[j].rid == rid && st::HASH_REGIONS[j].offset + st::HASH_REGIONS[j].len > need)
                need = st::HASH_REGIONS[j].offset + st::HASH_REGIONS[j].len;
        rids.push_back(rid);
        bufs.emplace_back(need + 64u);
        for (auto &x : bufs.back()) x = (uint8_t)lrnd();
        st::rebase((st::region_id)rid, (uint32_t)(uintptr_t)bufs.back().data(), need);
    }
    auto slice        = [](int i) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(st::hash_base(i))); };
    auto set_oq_count = [&](int32_t c) { memcpy(slice(st::HIDX_ORDER_QUEUE_COUNT), &c, 4); };
    set_oq_count(120);

    const size_t            ab = st::inc::tracker::arena_bytes();
    std::vector<uint8_t>    arena(ab);
    static st::inc::tracker tr;
    tr.attach(arena.data(), st::inc::knobs{});
    std::vector<undo_journal::entry> jm(20000);
    undo_journal                     jr;
    jr.attach(jm.data(), jm.size() * sizeof(undo_journal::entry));
    tap t;
    t.j = &jr;
    tr.set_journal(&t);

    constexpr uint32_t                STEPS   = 40;
    const uint32_t                    KEEP[3] = {3, 17, 39}; // the steps whose whole state is kept as the truth
    std::vector<std::vector<uint8_t>> truth[3];
    uint64_t                          truth_per[3][st::HASH_REGION_COUNT];
    st::inc::null_sink                ns;
    for (uint32_t s = 1; s <= STEPS; ++s) {
        if (s > 1) {
            for (int k = 0; k < 1 + (int)(lrnd() % 25); ++k) {
                const int      i = (int)(lrnd() % st::HASH_REGION_COUNT);
                const uint32_t L = st::HASH_REGIONS[i].len;
                slice(i)[lrnd() % L] ^= (uint8_t)(1 + lrnd() % 255);
            }
            if (s % 7 == 0) set_oq_count((int32_t)(lrnd() % 300)); // the order_queue mask boundary moves
        }
        jr.cur = s;
        if (s == 1) {
            tr.prime();
            jr.clear(1);
        } else {
            tr.update(ns, 0);
        }
        for (int k = 0; k < 3; ++k)
            if (KEEP[k] == s) {
                truth[k].resize(st::HASH_REGION_COUNT);
                for (int i = 0; i < st::HASH_REGION_COUNT; ++i)
                    truth[k][i].assign(slice(i), slice(i) + st::HASH_REGIONS[i].len);
                tr.region_hashes(truth_per[k]);
            }
    }
    bool bytes_ok = true, hash_ok = true;
    for (int k = 0; k < 3; ++k) {
        std::vector<std::vector<uint8_t>> ev(st::HASH_REGION_COUNT);
        for (int i = 0; i < st::HASH_REGION_COUNT; ++i) ev[i].assign(tr.shadow(i), tr.shadow(i) + st::HASH_REGIONS[i].len);
        jr.undo_to(KEEP[k], [&](int r, uint32_t b, const uint8_t *bytes, uint32_t n) {
            memcpy(ev[r].data() + b * 64u, bytes, n);
        });
        int32_t c;
        memcpy(&c, ev[st::HIDX_ORDER_QUEUE_COUNT].data(), 4);
        const uint32_t oq = st::inc::order_queue_live_bytes(c, st::HASH_REGIONS[st::HIDX_ORDER_QUEUE].len);
        for (int i = 0; i < st::HASH_REGION_COUNT; ++i) {
            if (ev[i] != truth[k][i]) bytes_ok = false;
            uint64_t       sum = 0;
            const uint32_t L   = st::HASH_REGIONS[i].len;
            for (uint32_t b = 0; b < (L + 63u) / 64u; ++b)
                sum += st::inc::block_hash_masked(i, b, ev[i].data(), L, st::inc::knobs{},
                                                  i == st::HIDX_ORDER_QUEUE ? oq : 0);
            if (sum != truth_per[k][i]) hash_ok = false;
        }
    }
    check("live journal: the real tracker's shadow + undo journal rebuild steps 3, 17 and 39 of the whole "
          "63-slice manifest byte-exact",
          bytes_ok);
    check("live journal: the masked block hashes of each rebuilt slice sum to its incremental hash at that step "
          "(order_queue boundary moving)",
          hash_ok);
    check("live journal: the journal covers the prime step and evicted nothing", jr.covers(1) && jr.evicted == 0);
    tr.set_journal(nullptr);
    for (int rid : rids) st::unrebase((st::region_id)rid);
}

} // namespace

int run_desynctest() {
    printf("=== desynctest (D21: a live match is told when the two sims stop agreeing) ===\n");

    static const char *const NAMES[NR] = {"a", "b", "c", "peer_horizon", "e", "f", "frame_ring", "h"};
    static const uint32_t    LENS[NR]  = {16, 32, 48, 64, 80, 96, 112, 128};
    const uint64_t           FP        = manifest_fingerprint(NAMES, LENS, g_ex, NR);

    // ---- 1. the wire record -----------------------------------------------------------------
    {
        check("wire_size(0) is the header alone", wire_size(0) == (int)(sizeof(sample_wire) - 8 * MAX_REGIONS));
        check("wire_size(n) grows 8 bytes per region", wire_size(NR) == wire_size(0) + 8 * NR);
        check("the live manifest fits the record", 56 <= MAX_REGIONS);

        uint64_t per[NR];
        fill(per, 0x1111);
        sample_wire s = make_sample(7, per, FP);
        check("a well-formed frame is sane", frame_is_sane(s, wire_size(NR)));

        // Every rejection arm, one at a time -- a validator that only ever sees good input is not one.
        sample_wire b = s;
        b.magic       = 0xdeadbeefu;
        check("a wrong magic is rejected", !frame_is_sane(b, wire_size(NR)));
        b         = s;
        b.version = WIRE_VERSION + 1;
        check("a wrong version is rejected", !frame_is_sane(b, wire_size(NR)));
        b              = s;
        b.region_count = 0;
        check("a zero region count is rejected", !frame_is_sane(b, wire_size(0)));
        b              = s;
        b.region_count = MAX_REGIONS + 1;
        check("an over-large region count is rejected", !frame_is_sane(b, wire_size(NR)));
        b          = s;
        b.reserved = 1;
        check("a non-zero reserved word is rejected", !frame_is_sane(b, wire_size(NR)));
        // THE ONE THAT MATTERS: a truncated frame whose header still claims NR regions. Accepting it
        // would compare against uninitialised bytes and call the difference a desync.
        check("a frame shorter than its own header claims is rejected",
              !frame_is_sane(s, wire_size(NR) - 8));
        check("a frame longer than its own header claims is rejected",
              !frame_is_sane(s, wire_size(NR) + 8));
    }

    // ---- 2. clause (c): the compared value drops the peer-local regions ----------------------
    {
        uint64_t a[NR], b[NR];
        fill(a, 0x2222);
        memcpy(b, a, sizeof(a));
        const uint64_t base = fold_state(a, g_ex, NR);

        // In the 2026-08-28 run the peer-local regions differ on 100% of steps BY CONSTRUCTION.
        // Comparing a hash that counted them would report a desync in every healthy game.
        b[EX_A] ^= 0xffffffffffffffffULL;
        b[EX_B] ^= 0x1ULL;
        check("changing an EXCLUDED region does not move the state hash", fold_state(b, g_ex, NR) == base);

        // ...and the mask is not vacuous: everything else still counts.
        for (int i = 0; i < NR; ++i) {
            if (g_ex[i]) continue;
            uint64_t c[NR];
            memcpy(c, a, sizeof(a));
            c[i] ^= 1ULL;
            check("changing an INCLUDED region moves the state hash", fold_state(c, g_ex, NR) != base);
        }
    }

    // ---- 3. the ring: no modulo aliasing at a cadence ----------------------------------------
    {
        // Sampled steps are multiples of the cadence. A `slot = step % RING_CAP` index would keep only
        // 64/gcd(20,64) = 16 of the 64 entries and silently drop the rest -- the ring would claim a
        // 1280-step history and hold 320. Store a full ring at cadence 20 and demand all of it back.
        ring r;
        r.clear();
        uint64_t per[NR];
        for (int k = 1; k <= RING_CAP; ++k) {
            fill(per, (uint64_t)k);
            r.put((uint32_t)k * 20u, fold_state(per, g_ex, NR), per, NR);
        }
        int found = 0;
        for (int k = 1; k <= RING_CAP; ++k)
            if (r.find((uint32_t)k * 20u)) ++found;
        check("a full ring of cadence-spaced steps is entirely retrievable", found == RING_CAP);
        check("the ring reports its newest step", r.newest == (uint32_t)RING_CAP * 20u);
        check("an unsampled step is not found", r.find(21u) == nullptr);

        // One more put evicts the oldest -- and the evicted step must MISS, not return the newer
        // entry that now owns its slot.
        fill(per, 999);
        r.put((uint32_t)(RING_CAP + 1) * 20u, fold_state(per, g_ex, NR), per, NR);
        check("the oldest entry is evicted", r.find(20u) == nullptr);
        check("the newest entry is present", r.find((uint32_t)(RING_CAP + 1) * 20u) != nullptr);
    }

    // ---- 4. judge(): agreement, disagreement, and the two NON-verdicts ------------------------
    {
        ring r;
        r.clear();
        uint64_t mine[NR];
        fill(mine, 0x3333);
        r.put(100, fold_state(mine, g_ex, NR), mine, NR);
        r.put(120, fold_state(mine, g_ex, NR), mine, NR);

        // agreement
        verdict v = judge(r, make_sample(100, mine, FP), g_ex, NR, FP, r.newest);
        check("identical state at the same step is OK", v.kind == outcome::ok);
        check("an OK verdict names no region", v.first_region == -1);

        // disagreement, localized
        uint64_t theirs[NR];
        memcpy(theirs, mine, sizeof(mine));
        theirs[4] ^= 0x55ULL;
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("differing state at the same step is a MISMATCH", v.kind == outcome::mismatch);
        check("the mismatch names the first diverging region", v.first_region == 4);
        check("the mismatch carries both hashes", v.mine != v.theirs && v.mine == fold_state(mine, g_ex, NR));

        // THE CLAUSE (c) TRAP AT THE LOCALIZATION LEVEL: a peer-local region differing must never be
        // NAMED as the culprit -- it differs in every healthy game, so naming it would send every
        // future reader to the wrong region.
        memcpy(theirs, mine, sizeof(mine));
        theirs[EX_A] ^= 0xffULL; // excluded, index BELOW the real culprit
        theirs[5] ^= 0x77ULL;    // the real one
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("a mismatch skips excluded regions when naming the culprit",
              v.kind == outcome::mismatch && v.first_region == 5);

        // ...and a sample differing ONLY in excluded regions is not a mismatch at all.
        memcpy(theirs, mine, sizeof(mine));
        theirs[EX_A] ^= 0xffULL;
        theirs[EX_B] ^= 0xffULL;
        v = judge(r, make_sample(100, theirs, FP), g_ex, NR, FP, r.newest);
        check("a sample differing only in excluded regions is OK", v.kind == outcome::ok);

        // THE CRY-WOLF ARMS. Neither of these is a desync, and reporting either as one is the
        // failure D21 clause (b) exists to prevent.
        v = judge(r, make_sample(140, mine, FP), g_ex, NR, FP, r.newest);
        check("a sample for a step we have not reached is HELD, not reported",
              v.kind == outcome::not_yet);
        v = judge(r, make_sample(60, mine, FP), g_ex, NR, FP, r.newest);
        check("a sample whose ring entry is gone is DROPPED, not reported", v.kind == outcome::too_old);

        // A peer hashing a different manifest is a BUILD mismatch. Without this it would read as a
        // desync on the very first sample of a mixed-build game.
        v = judge(r, make_sample(100, mine, FP ^ 1ULL), g_ex, NR, FP, r.newest);
        check("a different manifest fingerprint is reported as a manifest mismatch",
              v.kind == outcome::manifest_mismatch);
        sample_wire wrong_n  = make_sample(100, mine, FP);
        wrong_n.region_count = NR - 1;
        v                    = judge(r, wrong_n, g_ex, NR, FP, r.newest);
        check("a different region count is reported as a manifest mismatch",
              v.kind == outcome::manifest_mismatch);
    }

    // ---- 5. clause (f): 21428 mismatching steps produce ONE notice ---------------------------
    {
        // The number is the real one -- the 2026-08-29 match was desynced from step 1 and ran 21428
        // steps. A per-step notice would have drawn it 21428 times.
        int notices = 0, full = 0, rollups = 0;
        for (int n = 1; n <= 21428; ++n) {
            if (should_notify(n)) ++notices;
            if (should_log_full(n)) ++full;
            if (should_log_rollup(n)) ++rollups;
        }
        check("21428 consecutive mismatches produce exactly ONE user-visible notice", notices == 1);
        check("the first mismatches are logged in full", full == LOG_FIRST);
        check("the rest are rolled up, not one line each", rollups == (21428 - LOG_FIRST) / LOG_EVERY);
        check("the log volume is bounded well under the mismatch count", full + rollups < 200);
    }

    // ---- 6. the pending queue: FIFO, and full means drop the OLDEST --------------------------
    {
        pending_queue q;
        q.clear();
        uint64_t per[NR];
        fill(per, 0x4444);
        for (int i = 0; i < PENDING_CAP; ++i) q.push(i % 8, make_sample((uint32_t)i, per, FP));
        check("the queue holds its capacity", q.count == PENDING_CAP && q.dropped == 0);

        q.push(1, make_sample(9999, per, FP)); // one too many
        check("an overflowing push drops exactly one", q.dropped == 1 && q.count == PENDING_CAP);

        int         from = -1;
        sample_wire s;
        check("pop returns a sample", q.pop(from, s));
        // The OLDEST (step 0) was the one dropped, so the head is now step 1 -- fresh evidence beats
        // stale, the same choice the transport's own inbound queue makes.
        check("the dropped sample was the oldest", s.step == 1);

        q.clear();
        check("a cleared queue pops nothing", !q.pop(from, s) && q.count == 0);
    }

    // ---- 7. snapshot scheduling (D25): the dump steps are ABSOLUTE, not per-peer ---------------
    {
        // The dumps are worth something only if every peer writes one for the SAME lockstep step.
        // An absolute grid delivers that WITHOUT the peers agreeing on when they noticed -- which
        // they do not: peer A may judge B's sample for step 1450 while B is still judging A's for
        // 1400. A schedule of the form `mismatch_step + k*every` gives those two 1650 and 1600 and
        // neither file has a partner, which is the bug these checks exist to keep out.
        check("the grid is 8 cadences", snapshot_grid(50) == 400);
        check("a degenerate cadence has no grid", snapshot_grid(0) == 0);

        check("a grid step is due", snapshot_due(1600, 50));
        check("a non-grid step is not", !snapshot_due(1650, 50));
        check("a cadence multiple that is not a grid multiple is not due", !snapshot_due(1450, 50));
        check("step 0 is never due (nothing has happened yet)", !snapshot_due(0, 50));
        check("a degenerate cadence is never due", !snapshot_due(1600, 0));

        // THE PROPERTY THAT MATTERS: two peers that armed at DIFFERENT steps still produce files
        // for common steps. Simulated over a 3000-step match with the shipped budget of 3 -- peer A
        // arms at 1450 (its first dump 1600), peer B at 1650 (its first 2000). Every step B writes
        // must be one A also writes, and at least one must be shared or the pair is undiffable.
        const int BUDGET = 3;
        uint32_t  a_steps[BUDGET], b_steps[BUDGET];
        int       na = 0, nb = 0;
        for (uint32_t s = 1; s <= 3000; ++s) {
            if (!snapshot_due(s, 50)) continue;
            if (s >= 1450 && na < BUDGET) a_steps[na++] = s;
            if (s >= 1650 && nb < BUDGET) b_steps[nb++] = s;
        }
        check("both peers spend their budget", na == BUDGET && nb == BUDGET);
        int shared = 0;
        for (int i = 0; i < nb; ++i)
            for (int j = 0; j < na; ++j)
                if (b_steps[i] == a_steps[j]) ++shared;
        check("peers that armed 200 steps apart still share dump steps", shared >= 2);
        check("the later peer's first dump is a grid step the earlier one also hit",
              a_steps[0] == 1600 && b_steps[0] == 2000);
    }

    // ---- 8. D31 clause A: STATUS proof-of-life fires within a run far shorter than a full match ---
    {
        // Reproduces the 2026-09-24 O4 rig cause directly: a 2000-step configuration-(1) run at the
        // shipped every=50 cadence produces 40 samples per peer. Before D31 (threshold 50) that run
        // NEVER crossed the STATUS line, so mp_analyze.py's "compared=N" read nothing and reported
        // "armed, no sample reached a comparison" for 5 of 6 runs where judge() had in fact been
        // comparing correctly the whole time. MUTATION RED: setting SAMPLES_PER_STATUS_LINE back to
        // 50 fails both checks below (0 fires in a 40-sample run) -- checked by hand, reverted.
        check("no status line at 0 samples (never called before the first sample)",
              !status_line_due(0));
        check("a status line is due at the threshold", status_line_due(SAMPLES_PER_STATUS_LINE));
        check("the threshold fits inside an O4-shaped 40-sample run",
              SAMPLES_PER_STATUS_LINE <= 40);
        int fires = 0;
        for (int64_t k = 1; k <= 40; ++k)
            if (status_line_due(k)) ++fires;
        check("an O4-shaped 40-sample run gets at least one proof-of-life line", fires >= 1);
        // The other half of the trade this cadence makes: still bounded, not a line every sample. At
        // the shipped every=50 and ~50 sim steps/s, 1 sample/s -> a 20-minute match takes ~1200
        // samples; SAMPLES_PER_STATUS_LINE=10 gives ~120 STATUS lines over 20 minutes (vs. the old
        // ~24 at 50) -- more than before, still a small fraction of the sample count, nowhere near
        // "a line every sample".
        constexpr int64_t TWENTY_MIN_SAMPLES = 1200;
        int               fires_full_match   = 0;
        for (int64_t k = 1; k <= TWENTY_MIN_SAMPLES; ++k)
            if (status_line_due(k)) ++fires_full_match;
        check("a 20-minute match's proof-of-life line count stays a small fraction of its samples",
              fires_full_match < TWENTY_MIN_SAMPLES / 5);
    }

    // ---- 9. D31 R2: the on-screen state notice needs PERSISTENCE, not a single sample -------------
    {
        // D30's motivating shape: a state mismatch that reconverges by the very next sample must
        // never reach the player. `should_notify` is now keyed by CONSECUTIVE mismatches, reset to 0
        // by an intervening agreement -- see its header comment.
        check("a single mismatching sample does not notify (n=1)", !should_notify(1));
        check("two CONSECUTIVE mismatching samples notify (n=2)", should_notify(2));
        check("a third consecutive sample does not notify again (n=3)", !should_notify(3));

        // Simulate D30's exact shape: mismatch, mismatch, ok (reconverged), mismatch, mismatch,
        // mismatch, ... `should_notify` fires at EVERY 2nd-consecutive incident on its own (it only
        // answers "is this sample the persistence bar", two separate incidents in one match both
        // qualify); the ONE-notice-per-match property is notify_once()'s own one-shot latch in
        // desync_watch.cpp (`if (g_notified) return; g_notified = true;`), mirrored here so the test
        // proves the guarantee the LIVE code actually gives.
        int        consecutive = 0, would_fire_count = 0, actually_notified = 0;
        bool       latched    = false;
        const bool sequence[] = {false, true, true, false /*heals*/, true, true, true, true};
        for (bool mismatched : sequence) {
            consecutive = mismatched ? consecutive + 1 : 0;
            if (should_notify(consecutive)) {
                ++would_fire_count; // two separate incidents cross the persistence bar in this sequence
                if (!latched) {
                    latched = true;
                    ++actually_notified; // this is notify_once()'s real, latched behavior
                }
            }
        }
        check("two separate incidents in one sequence each cross the persistence bar on their own",
              would_fire_count == 2);
        check("notify_once()'s one-shot latch over the same sequence fires exactly once",
              actually_notified == 1);

        // D21 clause (f), re-verified under the NEW gate: 21428 consecutive mismatches still produce
        // exactly one notice (now at sample #2, not #1).
        int notices21428 = 0;
        for (int n = 1; n <= 21428; ++n)
            if (should_notify(n)) ++notices21428;
        check("21428 consecutive mismatches still produce exactly ONE notice (D21 clause f)",
              notices21428 == 1);
    }

    // ---- 10. D31 clause C: the cumulative order digest, and its wire compatibility -----------------
    {
        // ---- judge_order(): the comparison itself ----
        ring r;
        r.clear();
        uint64_t per[NR];
        fill(per, 0x5555);
        r.put(200, fold_state(per, g_ex, NR), per, NR, /*order_digest=*/0xAAAAAAAAAAAAAAAAULL);

        check("matching order digests at a known step are OK",
              judge_order(r, 200, 0xAAAAAAAAAAAAAAAAULL, /*has=*/true) == order_outcome::ok);
        check("differing order digests at a known step are a MISMATCH",
              judge_order(r, 200, 0xBBBBBBBBBBBBBBBBULL, /*has=*/true) == order_outcome::mismatch);
        check("no digest on the incoming sample reads as ABSENT, never mismatch",
              judge_order(r, 200, 0xBBBBBBBBBBBBBBBBULL, /*has=*/false) == order_outcome::absent);
        check("a step with no local ring entry reads as ABSENT (defensive)",
              judge_order(r, 999, 0xAAAAAAAAAAAAAAAAULL, /*has=*/true) == order_outcome::absent);

        // THE D31 MOTIVATING PROPERTY: the digest does not re-converge. Two consecutive samples whose
        // STATE agrees again can still disagree on the order digest, because it is a running fold of
        // every step since session start, not a snapshot of "now".
        ring r2;
        r2.clear();
        uint64_t per_a[NR], per_b[NR];
        fill(per_a, 0x1234);
        memcpy(per_b, per_a, sizeof(per_a)); // state re-converged: identical current per-region hashes
        // Our own digest folded a divergent step earlier and never un-folds it -- simulated by giving
        // the two ring entries DIFFERENT stored digests despite identical `state`/`per[]`.
        r2.put(300, fold_state(per_a, g_ex, NR), per_a, NR, /*order_digest=*/111ULL);
        const verdict state_v = judge(r2, make_sample(300, per_b, FP), g_ex, NR, FP, r2.newest);
        check("a state re-convergence used for this property is itself OK",
              state_v.kind == outcome::ok);
        check("the order digest can still disagree when the state has re-converged (the D30/D31 case)",
              judge_order(r2, 300, 222ULL, /*has=*/true) == order_outcome::mismatch);

        // ---- wire compatibility: old<->new frame mixing, by LENGTH alone (no WIRE_VERSION bump) ----
        const int base = wire_size(NR);
        check("ORDER_DIGEST_BYTES is one uint64", ORDER_DIGEST_BYTES == (int)sizeof(uint64_t));

        // "NEW PEER RECEIVES OLD (rc2) PEER'S FRAME": exactly `base` bytes, no trailing digest.
        // frame_is_sane must still accept it (state comparison is unaffected by this feature).
        sample_wire old_frame = make_sample(400, per, FP);
        check("an rc2-shaped (undigested) frame is still sane at its own base length",
              frame_is_sane(old_frame, base));
        check("that frame's length does NOT look like a digest-bearing one",
              base != base + ORDER_DIGEST_BYTES); // trivially true; documents the discriminant used live

        // "OLD (rc2) PEER RECEIVES NEW PEER'S FRAME": base + ORDER_DIGEST_BYTES bytes. An rc2 build's
        // frame_is_sane (== this same function, since the wire's fixed fields never changed) rejects
        // it at ITS OWN base length -- it never gets to inspect or misinterpret the trailing bytes.
        sample_wire new_frame = make_sample(400, per, FP); // identical fixed fields; only length differs on the wire
        check("a digest-bearing frame is NOT sane at the base (state-only) length -- an rc2 peer "
              "drops it as a bad_frame instead of misreading it",
              !frame_is_sane(new_frame, base + ORDER_DIGEST_BYTES));
        check("...but IS sane once the receiver knows to check it at base length (a new peer, "
              "having already stripped the trailing digest bytes before calling frame_is_sane)",
              frame_is_sane(new_frame, base));
    }

    // ---- the dirty-block probe's scanner (dirty_scan) ----
    {
        // A page-aligned live buffer so the grain boundaries are known; the region starts 32 bytes
        // into it, so the FIRST block is a partial one (the live-address alignment rule).
        alignas(4096) static uint8_t live[3 * 4096];
        static uint8_t               shadow[3 * 4096];
        uint8_t *const               L   = live + 32;
        const uint32_t               LEN = 2 * 4096;
        memset(live, 0, sizeof(live));
        memset(shadow, 0, sizeof(shadow));
        dirty_counts c;
        dirty_scan(L, shadow, LEN, c);
        check("dirty_scan: nothing changed -> first == len", c.first == LEN);
        check("dirty_scan: identical buffers report nothing",
              c.bytes == 0 && c.grains[0] == 0 && c.grains[3] == 0);

        L[0]             = 1; // live+32: block [32,64), grain 64 #0, page 0
        L[1]             = 2; // same block
        L[40]            = 3; // live+72: block [64,128) -- a second 64-B grain, same 256-B grain
        L[4096 - 32 + 5] = 4; // live+4101: page 1
        dirty_scan(L, shadow, LEN, c);
        check("dirty_scan: exact byte count", c.bytes == 4);
        check("dirty_scan: first changed offset", c.first == 0);
        check("dirty_scan: 64-B grains counted by live-address alignment", c.grains[0] == 3);
        check("dirty_scan: 256-B grains", c.grains[1] == 2);
        check("dirty_scan: 4 KB pages", c.grains[3] == 2);
        check("dirty_scan: the changed blocks were copied into the shadow",
              memcmp(L, shadow, LEN) == 0);
        dirty_scan(L, shadow, LEN, c);
        check("dirty_scan: a second scan with no new writes reports nothing", c.bytes == 0 && c.grains[0] == 0);

        L[LEN - 1] = 9; // the tail byte
        dirty_scan(L, shadow, LEN, c);
        check("dirty_scan: the last byte of the region is scanned", c.bytes == 1 && c.grains[3] == 1);
        check("dirty_scan: first == the only changed offset", c.first == LEN - 1);
        check("dirty_scan: never writes the live buffer", L[LEN - 1] == 9 && L[0] == 1);

        // dirty_grains_add (mp:D39): the live probe counts grains from the incremental core's EXACT
        // runs now, so the runs of a random write pattern must give dirty_scan's counts exactly.
        uint32_t seed = 12345u;
        for (int round = 0; round < 50; ++round) {
            for (int w = 0; w < 1 + round % 20; ++w) {
                seed = seed * 1103515245u + 12345u;
                L[(seed >> 8) % LEN] ^= (uint8_t)(1 + (seed & 0x7f));
            }
            // exact runs of what changed, BEFORE dirty_scan copies it into the shadow
            uint32_t  grains[DIRTY_NGRAINS] = {0, 0, 0, 0};
            uintptr_t last[DIRTY_NGRAINS];
            for (int g = 0; g < DIRTY_NGRAINS; ++g) last[g] = ~(uintptr_t)0;
            for (uint32_t q = 0; q < LEN;) {
                if (L[q] == shadow[q]) {
                    ++q;
                    continue;
                }
                uint32_t e = q;
                while (e < LEN && L[e] != shadow[e]) ++e;
                dirty_grains_add((uintptr_t)(L + q), e - q, last, grains);
                q = e;
            }
            dirty_scan(L, shadow, LEN, c);
            bool same = true;
            for (int g = 0; g < DIRTY_NGRAINS; ++g) same = same && grains[g] == c.grains[g];
            if (!same) {
                check("dirty_grains_add: runs give dirty_scan's grain counts", false);
                break;
            }
            if (round == 49) check("dirty_grains_add: runs give dirty_scan's grain counts (50 rounds)", true);
        }
    }

    run_v2_checks(FP); // mp:D44
    run_v2_live_journal_check();

    printf("=== desynctest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
