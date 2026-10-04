//
// udp_origin_selftest.cpp -- mp:U59 (HM-M1): the end-to-end exactly-once layer for broadcast game
// frames (mh_net_udp/origin_seq.h + its wiring in udp_endpoint.cpp). Called from `udploopbacktest`
// (run_origin_arms) so it rides the existing gate row; no new suite name.
//
// WHY THE LAYER EXISTS. Host migration (the mp:U57 design plan) needs every survivor to hold
// the SAME prefix of every origin's frame stream once the hub dies. Per-connection reliable streams
// cannot give that -- the connection is what died -- so each origin numbers its broadcast frames, every
// peer dedupes and delivers per origin in order, and every peer retains the last 30 s so the
// survivors can top each other up to the highest frontier any of them holds.
//
// FOUR GROUPS
//   A. the pure layer (no sockets): prefix + codecs, the retention ring against a reference model,
//      dedupe / ordering / holes / incarnation / gap timeout, bare-horizon coalescing and its memory
//      bound, and the reconcile planner's table.
//   B. THE ACCEPTANCE ARMS (3 and 4 endpoints): hub + N clients over real loopback sockets at 5 %
//      synthetic loss, every peer broadcasting a numbered run; the hub is killed MID-BURST and the
//      survivors keep sending into the void (those frames are exactly the "in flight through a dead
//      hub" population). The survivors then run the test-only reconcile (osq_report -> osq::plan ->
//      osq_serve -> osq_ingest, no election, no re-dial -- that is M2/M3). Asserted: per-origin
//      delivered sequences are gapless, duplicate-free and IDENTICAL across survivors; every survivor
//      origin's frames all arrived; a second, redundant reconcile changes nothing.
//   C. THE MUTATION. The same arm with the reconcile SKIPPED must go red (the detection is itself
//      asserted, so a vacuous arm cannot pass). `MH_OSQ_MUTATE=skip_reconcile` skips it in the real
//      arms too, which is how the suite is shown to fail on a source mutation.
//   D. THE VERSION GATE (user decision Q5): an endpoint pretending to be a pre-layer build is refused
//      at join, in both directions.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

#include "../mh_net_udp/udp_endpoint.h"

namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;
namespace osq = mh::netudp::osq;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}
void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[400];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

// ================================================================================================
// A. the pure layer
// ================================================================================================
uint32_t g_rng = 0x2545F491u;
uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

struct Got {
    int      origin;
    uint32_t tag; // first 4 payload bytes
    int      len;
};
struct Collector {
    std::vector<Got> got;
    void             operator()(int origin, const uint8_t *d, int n) {
        Got g{origin, 0, n};
        if (n >= 4) memcpy(&g.tag, d, 4);
        got.push_back(g);
    }
};
void put_tag(uint8_t *b, uint32_t tag, int len) {
    memset(b, 0xEE, (size_t)len);
    if (len >= 4) memcpy(b, &tag, 4);
}

// The Layer is ~1.7 MB: heap, never a local.
osq::Layer *new_layer(uint16_t inc) {
    osq::Layer *l = (osq::Layer *)calloc(1, sizeof(osq::Layer));
    l->reset(inc);
    return l;
}

void test_prefix_and_codecs() {
    uint8_t p[osq::PREFIX_BYTES];
    osq::prefix_encode(p, 0xBEEF, 0x01020304u);
    uint16_t inc = 0;
    uint32_t seq = 0;
    check("prefix round-trips", osq::prefix_decode(p, sizeof(p), inc, seq) && inc == 0xBEEF && seq == 0x01020304u);
    check("a short payload is not a prefix", !osq::prefix_decode(p, osq::PREFIX_BYTES - 1, inc, seq));
    uint8_t legacy[16] = {0x01, 0x02, 0x03}; // a retail lockstep frame opens with a type byte 1..5
    check("a pre-layer frame (no magic) is detected", !osq::prefix_decode(legacy, sizeof(legacy), inc, seq));

    osq::Report r{}, back{};
    for (int i = 0; i < osq::ORIGINS; ++i) {
        r.front[i]  = 1000u * (i + 1);
        r.oldest[i] = 7u * (i + 1);
        r.inc[i]    = (uint16_t)(0x1111 * (i + 1));
    }
    uint8_t wire[osq::REPORT_WIRE_BYTES];
    check("report encodes to its fixed size", osq::report_encode(r, wire) == osq::REPORT_WIRE_BYTES);
    check("report round-trips", osq::report_decode(wire, sizeof(wire), back) && memcmp(&r, &back, sizeof(r)) == 0);
    check("a truncated report is refused", !osq::report_decode(wire, sizeof(wire) - 1, back));
    wire[0] ^= 0xff;
    check("a report of another version is refused", !osq::report_decode(wire, sizeof(wire), back));

    uint8_t buf[64], data[20];
    put_tag(data, 77, 20);
    const int      n = osq::range_encode(buf, sizeof(buf), 3, 0xABCD, 10, 12, data, 20);
    int            o = -1, len = 0;
    uint16_t       ri = 0;
    uint32_t       lo = 0, hi = 0;
    const uint8_t *dp = nullptr;
    check("range encodes", n == osq::RANGE_HDR_BYTES + 20);
    check("range round-trips", osq::range_decode(buf, n, o, ri, lo, hi, dp, len) == n && o == 3 && ri == 0xABCD &&
                                   lo == 10 && hi == 12 && len == 20 && memcmp(dp, data, 20) == 0);
    check("a truncated range is refused", osq::range_decode(buf, n - 1, o, ri, lo, hi, dp, len) == 0);
    buf[0] = 9; // origin out of range
    check("a range for a bad origin is refused", osq::range_decode(buf, n, o, ri, lo, hi, dp, len) == 0);
}

// The retention ring against a reference model: random payload sizes (so the arena wraps and
// evicts), random bare-horizon runs (so entries coalesce), a clock that advances past the window.
void test_ring_model() {
    osq::Ring *r = (osq::Ring *)calloc(1, sizeof(osq::Ring));
    r->clear();
    std::vector<std::vector<uint8_t>> model(1); // model[seq] = the newest payload that covers seq
    uint32_t                          seq = 0, now = 1000;
    bool                              ok        = true;
    int                               max_count = 0;
    uint32_t                          max_bytes = 0;
    for (int it = 0; it < 60000 && ok; ++it) {
        now += rnd() % 5;
        const bool sup = (rnd() % 3) == 0;
        int        len = sup ? 9 : (int)(1 + rnd() % 300);
        if (rnd() % 400 == 0) len = 1 + (int)(rnd() % osq::PAYLOAD_MAX); // an occasional huge one
        uint8_t b[osq::PAYLOAD_MAX];
        ++seq;
        for (int i = 0; i < len; ++i) b[i] = (uint8_t)(rnd());
        model.emplace_back(b, b + len);
        r->add(seq, seq, now, b, len, sup);
        if (r->count > max_count) max_count = r->count;
        const uint32_t used = r->wrapped ? (osq::RING_POOL - r->ho) + r->te : r->te - r->ho;
        if (used > max_bytes) max_bytes = used;
        if (it % 97 == 0 || it > 59990) {
            // contiguity + content: every retained entry's bytes are the newest payload of its range
            uint32_t prev_hi = 0;
            for (int i = 0; i < r->count; ++i) {
                const osq::Ring::Ent &e = r->at(i);
                if (i > 0 && e.lo != prev_hi + 1) ok = false;
                prev_hi                       = e.hi;
                const std::vector<uint8_t> &m = model[e.hi];
                if (e.len != m.size() || (e.len && memcmp(r->pool + e.off, m.data(), e.len) != 0)) ok = false;
                if (e.off + (e.len ? e.len : 1u) > osq::RING_POOL) ok = false;
            }
            if (r->count && r->newest_hi() != seq) ok = false;
        }
    }
    check("ring: contiguous, content-exact, in bounds across 60000 random frames (wrap, evict, coalesce)", ok);
    checkf(max_count <= osq::RING_ENTRIES && max_bytes <= osq::RING_POOL,
           "ring: bounded (peak %d entries / %u bytes of %d / %u)", max_count, max_bytes, osq::RING_ENTRIES,
           osq::RING_POOL);
    // expiry: a frame older than the window goes, a younger one stays
    r->clear();
    uint8_t b[16] = {1};
    r->add(1, 1, 1000, b, 16, false);
    r->add(2, 2, 1000 + osq::WINDOW_MS - 1, b, 16, false);
    check("ring: a frame younger than the window is kept", r->oldest_lo() == 1);
    r->add(3, 3, 1000 + osq::WINDOW_MS + 5, b, 16, false);
    check("ring: a frame older than the window expires", r->oldest_lo() == 2);
    r->add(4, 4, 0xFFFFFFF0u, b, 16, false); // the tick wraps; the diff stays signed-correct
    r->add(5, 5, 0x00000010u, b, 16, false);
    check("ring: expiry is wrap-safe across a tick rollover", r->count >= 1 && r->newest_hi() == 5);
    free(r);
}

void test_dedupe_order() {
    osq::Layer *l = new_layer(0x1234);
    l->set_local(0);
    Collector c;
    uint8_t   b[20];
    auto      rx = [&](int o, uint16_t inc, uint32_t s) {
        put_tag(b, s, 20);
        return l->rx(o, inc, s, s, b, 20, false, 5000, c);
    };
    // in order
    for (uint32_t s = 1; s <= 5; ++s) check("in-order frame delivered", rx(1, 7, s) == osq::Layer::Rx::Delivered);
    check("in-order: five delivered", c.got.size() == 5);
    check("a replayed frame is a duplicate", rx(1, 7, 3) == osq::Layer::Rx::Duplicate && c.got.size() == 5);
    check("the frontier frame again is a duplicate", rx(1, 7, 5) == osq::Layer::Rx::Duplicate);
    // a hole: 8 arrives before 6,7
    check("a frame above a hole waits", rx(1, 7, 8) == osq::Layer::Rx::Ahead && c.got.size() == 5);
    check("a second waiting frame", rx(1, 7, 7) == osq::Layer::Rx::Ahead && c.got.size() == 5);
    check("the same waiting frame twice is a duplicate", rx(1, 7, 7) == osq::Layer::Rx::Duplicate);
    check("filling the hole delivers 6, 7, 8 in order", rx(1, 7, 6) == osq::Layer::Rx::Delivered && c.got.size() == 8 &&
                                                            c.got[5].tag == 6 && c.got[6].tag == 7 &&
                                                            c.got[7].tag == 8);
    // origins are independent
    check("another origin starts its own sequence", rx(2, 9, 40) == osq::Layer::Rx::Delivered); // late joiner: base 40
    check("...and its next seq follows", rx(2, 9, 41) == osq::Layer::Rx::Delivered);
    check("an old seq of a late-joined origin is a duplicate", rx(2, 9, 39) == osq::Layer::Rx::Duplicate);
    // our own frame handed back
    check("our own frame handed back is a duplicate", rx(0, 7, 1) == osq::Layer::Rx::Duplicate);
    // a restarted origin (new incarnation) is not "all duplicates"
    const size_t before = c.got.size();
    check("a new incarnation restarts the origin", rx(1, 8, 1) == osq::Layer::Rx::Delivered && c.got.size() == before + 1);
    check("...and counts as a reset", l->counters().resets == 1);
    // bad inputs
    check("a bad origin is refused", rx(8, 7, 1) == osq::Layer::Rx::Dropped && rx(-1, 7, 1) == osq::Layer::Rx::Dropped);
    check("seq 0 is refused", l->rx(1, 8, 0, 0, b, 20, false, 5000, c) == osq::Layer::Rx::Dropped);

    // shuffled arrival of one origin's run: delivery is the run, in order, once each
    osq::Layer *m = new_layer(1);
    Collector   mc;
    bool        all_ok = true;
    for (int trial = 0; trial < 200 && all_ok; ++trial) {
        m->reset(1);
        mc.got.clear();
        // seq 1 establishes the origin's base (a first-seen frame IS the base -- the late-joiner rule);
        // then blocks of 12 shuffled frames, so a hole never exceeds the waiting capacity (AHEAD_SLOTS)
        put_tag(b, 1, 20);
        m->rx(3, 5, 1, 1, b, 20, false, 100, mc);
        for (int blk = 0; blk < 40; blk += 12) {
            uint32_t v[12];
            int      n = 0;
            for (int i = blk; i < blk + 12 && i < 40; ++i)
                if (i > 0) v[n++] = (uint32_t)i + 1;
            for (int i = n - 1; i > 0; --i) std::swap(v[i], v[rnd() % (i + 1)]);
            for (int i = 0; i < n; ++i) {
                put_tag(b, v[i], 20);
                m->rx(3, 5, v[i], v[i], b, 20, false, 100, mc);
            }
        }
        if (mc.got.size() != 40) all_ok = false;
        for (size_t i = 0; i < mc.got.size() && all_ok; ++i)
            if (mc.got[i].tag != (uint32_t)i + 1) all_ok = false;
    }
    check("200 shuffled arrivals: each delivered exactly once, in order", all_ok);

    // the ahead buffer is bounded and keeps the LOWER seqs
    m->reset(1);
    mc.got.clear();
    put_tag(b, 1, 20);
    m->rx(3, 5, 1, 1, b, 20, false, 100, mc);
    for (uint32_t s = 3; s < 3 + osq::AHEAD_SLOTS; ++s) m->rx(3, 5, s, s, b, 20, false, 100, mc);
    check("a full ahead buffer refuses the next", m->rx(3, 5, 500, 500, b, 20, false, 100, mc) == osq::Layer::Rx::Dropped &&
                                                      m->counters().ahead_dropped == 1);

    // gap timeout: skipped, counted, delivered; never when disabled
    m->reset(1);
    mc.got.clear();
    put_tag(b, 1, 20);
    m->rx(3, 5, 1, 1, b, 20, false, 100, mc);
    put_tag(b, 3, 20);
    m->rx(3, 5, 3, 3, b, 20, false, 100, mc);
    m->tick(100 + osq::GAP_FORCE_MS - 1, mc);
    check("gap: not skipped before the timeout", mc.got.size() == 1 && m->counters().gap_skipped == 0);
    m->tick(100 + osq::GAP_FORCE_MS + 1, mc);
    check("gap: skipped after the timeout, and counted", mc.got.size() == 2 && m->counters().gap_skipped == 1);
    m->reset(1);
    m->gap_force_ms = 0;
    mc.got.clear();
    put_tag(b, 1, 20);
    m->rx(3, 5, 1, 1, b, 20, false, 100, mc);
    put_tag(b, 3, 20);
    m->rx(3, 5, 3, 3, b, 20, false, 100, mc);
    m->tick(1000000, mc);
    check("gap: gap_force_ms = 0 never skips (the M2 migration switch)", mc.got.size() == 1 && m->counters().gap_skipped == 0);
    free(m);
    free(l);
}

// A horizon storm must not grow the ring, and a range serves back as ONE latest-wins frame.
void test_coalesce_bound() {
    osq::Layer *a = new_layer(3), *b = new_layer(4);
    a->set_local(0);
    Collector sink;
    uint8_t   order[40];
    put_tag(order, 0xAAAA, 40);
    a->tx(0, order, 40, false, 1000); // seq 1: an order
    uint8_t        h[9]  = {2};
    const uint32_t STORM = 200000; // ~one minute of a 3500/s runaway advert
    uint32_t       t     = 1000;
    for (uint32_t i = 0; i < STORM; ++i) {
        memcpy(h + 1, &i, 4);
        if (i % 8 == 0) ++t; // 1 ms per 8 adverts -> 25 s at this rate: inside the window
        a->tx(0, h, 9, true, t);
    }
    order[0] = 0xBB;
    a->tx(0, order, 40, false, t); // the next order
    osq::Report ra;
    a->report(ra);
    check("storm: every seq is accounted for", ra.front[0] == 1 + STORM + 1);
    const int entries = a->o[0].ring.count;
    checkf(entries <= 3, "storm: %u bare horizons coalesced into %d ring entries", STORM, entries);
    // serve it all to a fresh peer
    struct S {
        osq::Layer *to;
        Collector  *sink;
        int         n;
    } ctx{b, &sink, 0};
    a->serve(0, 1, ra.front[0], [&](uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *d, int len, bool sup) {
        b->rx(0, inc, lo, hi, d, len, sup, t, sink);
        ++ctx.n;
    });
    osq::Report rb;
    b->report(rb);
    check("storm: the peer reaches the same frontier from a handful of ranges", rb.front[0] == ra.front[0] && ctx.n <= 3);
    check("storm: delivered as order, ONE latest advert, order", sink.got.size() == 3 && sink.got[1].len == 9);
    // sizeof: the memory bound
    printf("     memory: osq::Layer = %zu bytes (%d origins x [ring %u B + %d entries + %d waiting frames])\n",
           sizeof(osq::Layer), osq::ORIGINS, osq::RING_POOL, osq::RING_ENTRIES, osq::AHEAD_SLOTS);
    check("memory: the whole layer is bounded under 2 MiB", sizeof(osq::Layer) < 2u * 1024u * 1024u);
    free(a);
    free(b);
}

osq::Report mkrep(std::initializer_list<std::pair<int, std::pair<uint32_t, uint32_t>>> e, uint16_t inc = 5) {
    osq::Report r{};
    for (int i = 0; i < osq::ORIGINS; ++i) r.inc[i] = inc;
    for (auto &x : e) {
        r.front[x.first]  = x.second.first;
        r.oldest[x.first] = x.second.second;
    }
    return r;
}

void test_plan() {
    osq::Fetch f[32];
    uint32_t   target[osq::ORIGINS];
    bool       unrec = false;
    {
        // 3 survivors. Origin 0 (the dead hub): A holds 100, B 90, C 95. Origin 1 (A's own): A 50, B 50, C 40.
        osq::Report r[3] = {mkrep({{0, {100, 1}}, {1, {50, 1}}}), mkrep({{0, {90, 1}}, {1, {50, 1}}}),
                            mkrep({{0, {95, 1}}, {1, {40, 1}}})};
        const int   n    = osq::plan(r, 3, f, 32, &unrec, target);
        check("plan: targets are the max frontiers", target[0] == 100 && target[1] == 50);
        check("plan: three fetches (B and C for the hub's frames, C for A's)", n == 3 && !unrec);
        bool b0 = false, c0 = false, c1 = false;
        for (int i = 0; i < n; ++i) {
            if (f[i].origin == 0 && f[i].to_peer == 1 && f[i].holder == 0 && f[i].from == 91 && f[i].to == 100) b0 = true;
            if (f[i].origin == 0 && f[i].to_peer == 2 && f[i].holder == 0 && f[i].from == 96 && f[i].to == 100) c0 = true;
            if (f[i].origin == 1 && f[i].to_peer == 2 && f[i].from == 41 && f[i].to == 50) c1 = true;
        }
        check("plan: each fetch asks the max-holder for exactly the missing range", b0 && c0 && c1);
    }
    {
        osq::Report r[2] = {mkrep({{0, {50, 1}}}), mkrep({{0, {50, 1}}})};
        check("plan: equal frontiers need no fetch", osq::plan(r, 2, f, 32, &unrec, target) == 0 && !unrec);
    }
    {
        // the holder has retained only from 60 but the peer needs 41..: UNRECOVERABLE, never a guess
        osq::Report r[2] = {mkrep({{0, {100, 60}}}), mkrep({{0, {40, 1}}})};
        const int   n    = osq::plan(r, 2, f, 32, &unrec, target);
        check("plan: a needed seq below every holder's retention is UNRECOVERABLE", n == 0 && unrec);
    }
    {
        // a peer with nothing from an origin starts at the holder's oldest (it joined late)
        osq::Report r[2] = {mkrep({{0, {100, 70}}}), mkrep({})};
        const int   n    = osq::plan(r, 2, f, 32, &unrec, target);
        check("plan: a peer with no state fetches from the holder's oldest retained seq", n == 1 && !unrec &&
                                                                                              f[0].from == 70 && f[0].to == 100);
    }
    {
        osq::Report r[2] = {mkrep({{0, {100, 1}}}, 5), mkrep({{0, {40, 1}}}, 6)};
        osq::plan(r, 2, f, 32, &unrec, target);
        check("plan: disagreeing incarnations are UNRECOVERABLE", unrec);
    }
    {
        osq::Report r[2] = {mkrep({{0, {100, 1}}, {1, {100, 1}}, {2, {100, 1}}}), mkrep({})};
        osq::plan(r, 2, f, 2, &unrec, target);
        check("plan: a buffer too small to hold the plan is refused, not half-planned", unrec);
    }
}

// ================================================================================================
// B. the acceptance arms
// ================================================================================================
const unsigned char TEST_PSK[32] = {0x4d, 0x48, 0x74, 0x65, 0x73, 0x74, 0x6b, 0x65, 0x79, 0x20, 0x75, 0x64, 0x70, 0x20, 0x6c, 0x6f,
                                    0x6f, 0x70, 0x62, 0x61, 0x63, 0x6b, 0x20, 0x54, 0x31, 0x20, 0x66, 0x69, 0x78, 0x65, 0x64, 0x21};

void ep_log(void *, const char *line) {
    if (getenv("MH_UDP_LOOPBACK_VERBOSE")) printf("    | %s\n", line);
}

void fill_cfg(Config &c, int role, int port, int player, unsigned short bind_port) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port          = port;
    c.net.player_id     = player;
    c.net.log           = 1;
    c.net.host_assign   = 1;
    c.net.ping_ms       = 200;
    c.net.rx_timeout_ms = -1; // a suite that stops to assert must not have a watchdog dropping its peers
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(5);
    }
}

Endpoint g_o[4]; // hub + up to 3 clients; BSS, ~4 MB of stream rings each plus the layer

struct Ledger {
    std::vector<uint32_t> seq[osq::ORIGINS]; // per origin, the record counters in DELIVERY order
    void                  clear() {
        for (auto &v : seq) v.clear();
    }
};

void drain(Endpoint &ep, Ledger &led) {
    for (;;) {
        int           sender = -1;
        unsigned char buf[2048];
        int           len = (int)sizeof(buf);
        if (!ep.recv(&sender, buf, &len)) return;
        if (len < 4 || sender < 0 || sender >= osq::ORIGINS) continue;
        led.seq[sender].push_back((uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16));
    }
}

struct Served {
    int      origin;
    uint16_t inc;
    uint32_t lo, hi;
    int      len;
    uint8_t  data[osq::PAYLOAD_MAX];
};
struct ServeCtx {
    std::vector<Served> *out;
    int                  origin;
};
void serve_cb(void *ctx, uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *d, int len, bool) {
    ServeCtx *c = (ServeCtx *)ctx;
    Served    s;
    s.origin = c->origin;
    s.inc    = inc;
    s.lo     = lo;
    s.hi     = hi;
    s.len    = len;
    memcpy(s.data, d, (size_t)len);
    c->out->push_back(s);
}

// Execute a plan between the live survivor endpoints: serve from each holder, ingest at each peer,
// draining between ingests (osq_ingest answers -1 while the game queue has no room -- the same
// backpressure the live path obeys). Returns false if a serve found a seq below the retention.
bool run_reconcile(Endpoint **surv, Ledger *led, int n, bool *unrecoverable, int *fetched_ranges) {
    osq::Report rep[3];
    for (int i = 0; i < n; ++i) surv[i]->osq_report(rep[i]);
    osq::Fetch f[64];
    uint32_t   target[osq::ORIGINS];
    const int  nf = osq::plan(rep, n, f, 64, unrecoverable, target);
    bool       ok = !*unrecoverable;
    for (int k = 0; k < nf; ++k) {
        std::vector<Served> served;
        ServeCtx            sc{&served, f[k].origin};
        const int           sn = surv[f[k].holder]->osq_serve(f[k].origin, f[k].from, f[k].to, serve_cb, &sc);
        if (sn < 0) {
            ok = false;
            continue;
        }
        for (const Served &s : served) {
            ++*fetched_ranges;
            for (int tries = 0; tries < 400; ++tries) {
                const int r = surv[f[k].to_peer]->osq_ingest(s.origin, s.inc, s.lo, s.hi, s.data, s.len);
                if (r >= 0) break;
                drain(*surv[f[k].to_peer], led[f[k].to_peer]);
                Sleep(1);
            }
            drain(*surv[f[k].to_peer], led[f[k].to_peer]);
        }
    }
    return ok;
}

bool is_run(const std::vector<uint32_t> &v) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != (uint32_t)i) return false;
    return true;
}

// One acceptance arm. `clients` = 2 -> a 3-endpoint arm, 3 -> a 4-endpoint arm. Returns true when
// every property held. `reconcile` false = the MUTATION (the caller asserts the arm then goes red).
bool hub_kill_arm(const char *name, int clients, int base_port, bool reconcile, bool quiet) {
    const int ROUNDS = 260, KILL_AT = 120, LOSS_PM = 50, TAIL = 12;
    if (!quiet)
        printf("  -- %s (hub + %d clients, %d rounds each, hub killed at round %d, loss %d/1000%s)\n", name, clients,
               ROUNDS, KILL_AT, LOSS_PM, reconcile ? "" : ", RECONCILE SKIPPED");
    Endpoint &hub = g_o[0];
    for (int i = 0; i <= clients; ++i) new (&g_o[i]) Endpoint();
    Ledger led[3];
    for (auto &l : led) l.clear();

    Config ch, cc[3];
    fill_cfg(ch, 0, base_port, 0, (unsigned short)base_port);
    for (int i = 0; i < clients; ++i) fill_cfg(cc[i], 1, base_port, 1, (unsigned short)(base_port + 1 + i));
    for (int i = 0; i <= clients; ++i) {
        g_o[i].set_log(ep_log, nullptr);
        g_o[i].set_rx_loss(LOSS_PM, 0x1234abcdu + 0x9e3779b9u * (unsigned)i);
    }
    bool ok = true;
    ok &= hub.start(ch, TEST_PSK, true);
    for (int i = 0; i < clients; ++i) ok &= g_o[1 + i].start(cc[i], TEST_PSK, true);
    check("endpoints started", ok);
    const bool joined = wait_for([&] { return hub.peer_count() == clients; }, 8000);
    bool       ids    = joined && wait_for(
                             [&] {
                                 for (int i = 0; i < clients; ++i)
                                     if (!g_o[1 + i].id_assigned()) return false;
                                 return true;
                             },
                             4000);
    checkf(joined && ids, "%s: all clients admitted and assigned ids", name);
    if (!(joined && ids)) {
        for (int i = 0; i <= clients; ++i) g_o[i].stop();
        return false;
    }
    int id[3];
    for (int i = 0; i < clients; ++i) id[i] = g_o[1 + i].local_player_id();

    // THE BURST. No pacing beyond a 1 ms breath every 8 rounds; the hub dies mid-run.
    int hub_sent = 0, sent[3] = {0, 0, 0};
    for (int r = 0; r < ROUNDS; ++r) {
        unsigned char rec[40];
        auto          mk = [&](int who) {
            memset(rec, 0xA0, sizeof(rec));
            rec[0] = (unsigned char)who;
            rec[1] = (unsigned char)(r & 0xff);
            rec[2] = (unsigned char)((r >> 8) & 0xff);
            rec[3] = 0;
        };
        // THE HUB'S OWN TAIL IS LOST IN FLIGHT FOR ONE SURVIVOR: the last client takes 90 % inbound loss
        // for the final TAIL rounds before the kill, so some of the hub's last frames reach the other
        // survivors and not it -- "the hub sent order O to A and died before sending it to B" (plan 7.2).
        if (r == KILL_AT - TAIL) g_o[clients].set_rx_loss(900, 0x7a11u);
        if (r == KILL_AT) {
            hub.stop(); // abrupt: nothing flushed, nothing forwarded after this line
            g_o[clients].set_rx_loss(LOSS_PM, 0x7a12u);
        }
        if (r < KILL_AT) {
            mk(0);
            if (hub.send(MH_NET_BROADCAST, rec, sizeof(rec))) ++hub_sent;
        }
        for (int i = 0; i < clients; ++i) {
            mk(id[i]);
            if (g_o[1 + i].send(MH_NET_BROADCAST, rec, sizeof(rec))) ++sent[i];
            drain(g_o[1 + i], led[i]);
        }
        if ((r % 8) == 7) Sleep(1);
    }
    // settle: whatever the survivors can still receive from each other's streams (nothing: the only
    // route was the hub) and from datagrams already in flight has arrived
    for (int k = 0; k < 40; ++k) {
        for (int i = 0; i < clients; ++i) drain(g_o[1 + i], led[i]);
        Sleep(10);
    }
    long simlost = 0;
    for (int i = 0; i < clients; ++i) {
        Counters k;
        g_o[1 + i].counters(k);
        simlost += k.dgram_dropped_sim;
    }

    // BEFORE: the survivors disagree, or the arm proves nothing
    osq::Report rep0[3];
    long        missing_before        = 0;
    uint32_t    target0[osq::ORIGINS] = {0};
    for (int i = 0; i < clients; ++i) {
        g_o[1 + i].osq_report(rep0[i]);
        for (int o = 0; o < osq::ORIGINS; ++o)
            if (rep0[i].front[o] > target0[o]) target0[o] = rep0[i].front[o];
    }
    for (int i = 0; i < clients; ++i)
        for (int o = 0; o < osq::ORIGINS; ++o) missing_before += (long)(target0[o] - rep0[i].front[o]);
    long hub_missing_before = 0; // the dead hub's OWN stream: how far the laggard survivor is behind the leader
    for (int i = 0; i < clients; ++i) hub_missing_before += (long)(target0[0] - rep0[i].front[0]);

    bool unrec  = false;
    int  ranges = 0;
    if (reconcile) {
        Endpoint *surv[3];
        for (int i = 0; i < clients; ++i) surv[i] = &g_o[1 + i];
        run_reconcile(surv, led, clients, &unrec, &ranges);
        // a SECOND, REDUNDANT reconcile (and a full re-serve of everything to everyone): exactly-once
        // means this changes nothing
        Ledger snap[3];
        for (int i = 0; i < clients; ++i) snap[i] = led[i];
        run_reconcile(surv, led, clients, &unrec, &ranges);
        for (int a = 0; a < clients; ++a)
            for (int b = 0; b < clients; ++b)
                if (a != b) {
                    for (int o = 0; o < osq::ORIGINS; ++o) {
                        std::vector<Served> served;
                        ServeCtx            sc{&served, o};
                        surv[a]->osq_serve(o, 1, 0x7fffffffu, serve_cb, &sc);
                        for (const Served &s : served) {
                            surv[b]->osq_ingest(s.origin, s.inc, s.lo, s.hi, s.data, s.len);
                            drain(*surv[b], led[b]);
                        }
                    }
                }
        bool unchanged = true;
        for (int i = 0; i < clients; ++i)
            for (int o = 0; o < osq::ORIGINS; ++o)
                if (led[i].seq[o] != snap[i].seq[o]) unchanged = false;
        if (!quiet) checkf(unchanged, "%s: a redundant reconcile and a full re-serve change nothing (exactly once)", name);
        ok &= unchanged;
    }
    for (int i = 0; i < clients; ++i) drain(g_o[1 + i], led[i]);

    // AFTER: the properties
    bool run_ok = true, same_ok = true, all_in = true;
    for (int i = 0; i < clients; ++i) {
        for (int o = 0; o < osq::ORIGINS; ++o) {
            if (o == id[i]) continue;
            if (!is_run(led[i].seq[o])) run_ok = false; // gapless, in order, duplicate-free
        }
        for (int j = 0; j < clients; ++j) {
            if (j == i) continue;
            // origin j's frames: every one of them must have reached survivor i
            if ((int)led[i].seq[id[j]].size() != sent[j]) all_in = false;
        }
    }
    {
        // compare ledgers pairwise for every origin both survivors deliver (neither is its own origin)
        for (int a = 0; a < clients; ++a)
            for (int b = a + 1; b < clients; ++b)
                for (int o = 0; o < osq::ORIGINS; ++o)
                    if (o != id[a] && o != id[b] && led[a].seq[o] != led[b].seq[o]) same_ok = false;
    }
    const bool hub_len_ok = (long)led[0].seq[0].size() == (long)target0[0];

    if (quiet) {
        // the mutation probe: the caller asserts these properties do NOT all hold without a reconcile
        // (and a vacuous arm -- survivors that never disagreed -- would hold them, and so go red)
        for (int i = 0; i <= clients; ++i) g_o[i].stop();
        return run_ok && same_ok && all_in && hub_len_ok;
    }
    printf("     hub sent %d, survivors sent", hub_sent);
    for (int i = 0; i < clients; ++i) printf(" %d", sent[i]);
    printf(" | synthetic loss %ld | frames missing across survivors BEFORE reconcile: %ld | ranges fetched %d\n",
           simlost, missing_before, ranges);
    for (int i = 0; i < clients; ++i) {
        const osq::Counters oc = g_o[1 + i].osq_counters();
        printf("     survivor %d: delivered %ld dup %ld ahead %ld gap-skipped %ld legacy %ld\n", id[i], oc.delivered,
               oc.dup, oc.ahead, oc.gap_skipped, oc.legacy_refused);
        checkf(oc.gap_skipped == 0, "%s: survivor %d never skipped a hole (normal play: zero)", name, id[i]);
    }
    checkf(simlost > 0, "%s: the 5%% loss injection actually fired (%ld datagrams)", name, simlost);
    checkf(missing_before > 0, "%s: NON-VACUOUS -- the survivors really disagreed before reconcile (%ld frames missing)", name,
           missing_before);
    checkf(hub_missing_before > 0, "%s: NON-VACUOUS -- survivors held different parts of the DEAD HUB's own stream (%ld frames missing)",
           name, hub_missing_before);
    checkf(hub_sent == KILL_AT, "%s: the hub sent its %d frames before dying", name, KILL_AT);
    checkf(!unrec, "%s: the reconcile plan was recoverable (nothing below any retention)", name);
    checkf(run_ok, "%s: every origin's delivered sequence is a gapless, duplicate-free run at every survivor", name);
    checkf(same_ok, "%s: per-origin delivered sequences are IDENTICAL across the survivors", name);
    checkf(all_in, "%s: every frame a surviving origin sent reached every other survivor", name);
    checkf(hub_len_ok, "%s: the dead hub's stream ends at the max frontier any survivor held (%u)", name, target0[0]);
    for (int i = 0; i <= clients; ++i) g_o[i].stop();
    return ok && run_ok && same_ok && all_in && hub_len_ok && !unrec;
}

// ================================================================================================
// D. the version gate
// ================================================================================================
void legacy_arm(int base_port) {
    printf("  -- version gate (a pre-U59 build is refused at join, both directions)\n");
    Endpoint &host = g_o[0], &cli = g_o[1];
    {
        new (&host) Endpoint();
        new (&cli) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base_port, 0, (unsigned short)base_port);
        fill_cfg(cc, 1, base_port, 1, (unsigned short)(base_port + 1));
        host.set_log(ep_log, nullptr);
        cli.set_log(ep_log, nullptr);
        cli.osq_test_legacy(true); // an OLD client: empty HELLO, unprefixed frames
        host.start(ch, TEST_PSK, true);
        cli.start(cc, TEST_PSK, true);
        const bool refused = wait_for([&] { return host.osq_counters().legacy_refused > 0; }, 6000);
        checkf(refused, "a client without the capability is refused by the host (HELLO)");
        Sleep(100);
        check("...and holds no seat afterwards", host.peer_count() == 0);
        host.stop();
        cli.stop();
    }
    {
        new (&host) Endpoint();
        new (&cli) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base_port + 2, 0, (unsigned short)(base_port + 2));
        fill_cfg(cc, 1, base_port + 2, 1, (unsigned short)(base_port + 3));
        host.set_log(ep_log, nullptr);
        cli.set_log(ep_log, nullptr);
        host.osq_test_legacy(true); // an OLD host: empty WELCOME
        host.start(ch, TEST_PSK, true);
        cli.start(cc, TEST_PSK, true);
        const bool refused = wait_for([&] { return cli.osq_counters().legacy_refused > 0; }, 6000);
        checkf(refused, "a host without the capability is refused by the client (WELCOME)");
        host.stop();
        cli.stop();
    }
    {
        // and a legacy-looking DATA frame alone (no HELLO involvement) is refused too
        new (&host) Endpoint();
        new (&cli) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base_port + 4, 0, (unsigned short)(base_port + 4));
        fill_cfg(cc, 1, base_port + 4, 1, (unsigned short)(base_port + 5));
        host.set_log(ep_log, nullptr);
        cli.set_log(ep_log, nullptr);
        host.start(ch, TEST_PSK, true);
        cli.start(cc, TEST_PSK, true);
        wait_for([&] { return host.peer_count() == 1 && cli.id_assigned(); }, 6000);
        cli.osq_test_legacy(true); // from here its game frames carry no prefix
        unsigned char rec[16] = {1, 2, 3, 4};
        cli.send(MH_NET_BROADCAST, rec, sizeof(rec));
        const bool refused = wait_for([&] { return host.osq_counters().legacy_refused > 0; }, 4000);
        checkf(refused, "an unprefixed DATA frame is refused by the transport (the first-frame gate)");
        host.stop();
        cli.stop();
    }
}

} // namespace

// Entry from run_udploopbacktest (udp_loopback_selftest.cpp). Returns the number of failures.
int run_origin_arms() {
    const int fails0 = g_fails;
    printf("  -- mp:U59 exactly-once layer: the pure layer\n");
    test_prefix_and_codecs();
    test_ring_model();
    test_dedupe_order();
    test_coalesce_bound();
    test_plan();

    const bool skip = getenv("MH_OSQ_MUTATE") && strcmp(getenv("MH_OSQ_MUTATE"), "skip_reconcile") == 0;
    if (skip) printf("  !! MH_OSQ_MUTATE=skip_reconcile: the reconcile is SKIPPED in the real arms (expect red)\n");
    hub_kill_arm("3-endpoint hub-kill", 2, 39800, !skip, false);
    hub_kill_arm("4-endpoint hub-kill", 3, 39810, !skip, false);
    // THE MUTATION, asserted from inside: the same arm with the reconcile skipped must NOT hold.
    const bool held_without = hub_kill_arm("mutation probe", 2, 39820, false, true);
    check("MUTATION: skipping the reconcile is DETECTED (the properties do not hold without it)", !held_without);
    legacy_arm(39830);
    printf("     exactly-once layer: %d checks so far, %d failures in this block\n", g_checks, g_fails - fails0);
    return g_fails - fails0;
}
