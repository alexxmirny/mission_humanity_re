//
// udp_mesh_selftest.cpp -- `net_selftest.exe meshtest`: mp:U61 (HM-M3), the host-migration election
// machinery (mh_net_udp/mesh.h + udp_mesh.cpp). The host-migration plan (mp:U57) sections 3-5.
//
// WHAT IS PROVED
//   A. elect() -- the pure election rule -- as a TABLE: min-max costs, ties by the sum then by the lowest
//      id, an unmeasured edge makes a candidate ineligible, the relay tier (shortest leg), the no-data
//      tier (lowest id), empty/single/subset survivor sets, the one-direction edge, plus a randomised
//      comparison against an independent brute-force statement of the plan's formula
//      `max over i != j of d(i,c) + d(c,j)`.
//   B. the codecs and the keys: every FLAG_MESH frame round-trips and EVERY strict prefix / trailing byte
//      is refused (a truncation read as a short answer is a peer quietly ranking fewer candidates); the
//      probe keys are per sender and per match key; a sealed probe opens, a replay and a foreign key do not;
//      the digest moves with exactly what elect() reads.
//   C. the endpoints on 127.0.0.1 (host + 2 clients, host + 3 clients): every pair gets a matrix entry
//      within 5 s of start, the epoch ack round completes, EVERY endpoint holds the same epoch, digest
//      and ranking, the published ranking IS elect() of the published matrix, and the probe + frame
//      bandwidth is measured against plan section 3's estimate.
//   D. the negatives: a client that never answers a probe leaves its pairs unmeasured and the epoch
//      falls to the lowest-id tier (and still agrees); a client that never acks stalls e+1 (and is
//      re-sent e) until it does; relayed clients (simulated by the leg overhead) send no probes and are
//      ranked by their relay legs with a pre-minted room each; a peer that is not a mesh build (the U59/U60
//      capability byte) is refused at join in both directions; a stranger's datagram is not a probe.
//
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <initializer_list>
#include <new>
#include <string>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include "../mh_net_udp/mesh.h"
#include "../mh_net_udp/udp_endpoint.h"

namespace {

namespace M = mh::netudp::mesh;
namespace U = mh_net_proto::udp;
using mh::netudp::Config;
using mh::netudp::Endpoint;

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

const unsigned char TEST_PSK[32] = {0x4d, 0x48, 0x6d, 0x65, 0x73, 0x68, 0x74, 0x65, 0x73, 0x74, 0x20, 0x55, 0x36, 0x31, 0x20, 0x70,
                                    0x73, 0x6b, 0x20, 0x66, 0x69, 0x78, 0x65, 0x64, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x21, 0x21};

// mp:U63 -- the per-endpoint "hub elected" lines (the failover arms compare them across survivors). `ctx` is the
// endpoint's index in g_e[], or null for an arm that does not record.
std::vector<std::string> g_elected[8];
void                     ep_log(void *ctx, const char *line) {
    if (ctx && strstr(line, "hub elected ")) g_elected[*(int *)ctx & 7].push_back(line);
    if (getenv("MH_UDP_LOOPBACK_VERBOSE")) printf("    | %s\n", line);
}

template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(10);
    }
}

// ================================================================================================
// A. elect()
// ================================================================================================
// Symmetric edge table: set(m, a, b, d) writes both directions.
void set(M::Matrix &m, int a, int b, uint16_t d) {
    m.rtt[a][b] = d;
    m.rtt[b][a] = d;
}
uint32_t mask(std::initializer_list<int> ids) {
    uint32_t s = 0;
    for (int i : ids) s |= 1u << i;
    return s;
}
bool rank_is(const M::Election &e, std::initializer_list<int> want) {
    if (e.n != (int)want.size()) return false;
    int i = 0;
    for (int w : want)
        if (e.rank[i++] != w) return false;
    return true;
}
std::string rank_str(const M::Election &e) {
    std::string s;
    for (int i = 0; i < e.n; ++i) s += std::to_string((int)e.rank[i]) + (i + 1 < e.n ? ">" : "");
    return s;
}

void test_elect_table() {
    printf("  -- A. elect(): the table\n");
    M::Election e;
    // 1. min-max cost: 4 clients, client 2 is central.
    {
        M::Matrix m;
        set(m, 1, 2, 100);
        set(m, 1, 3, 300);
        set(m, 1, 4, 500);
        set(m, 2, 3, 100);
        set(m, 2, 4, 200);
        set(m, 3, 4, 100);
        M::elect(m, mask({1, 2, 3, 4}), e);
        checkf(rank_is(e, {2, 3, 4, 1}), "costs: ranking is 2>3>4>1 (got %s)", rank_str(e).c_str());
        check("costs: winner 2, tier DIRECT", e.winner == 2 && e.tier == M::TIER_DIRECT);
        check("costs: cost(2) = top1+top2 = 300, cost(3) = 400, cost(4) = 700, cost(1) = 800",
              e.cost[0] == 300 && e.cost[1] == 400 && e.cost[2] == 700 && e.cost[3] == 800);
        check("costs: sums are 400 / 500 / 800 / 900", e.sum[0] == 400 && e.sum[1] == 500 && e.sum[2] == 800 && e.sum[3] == 900);
        // 1b. min-max is not min-sum: client 1 has two near peers and one far one.
        M::Matrix n;
        set(n, 1, 2, 10);
        set(n, 1, 3, 10);
        set(n, 1, 4, 900); // sum 920, cost 910
        set(n, 2, 3, 200);
        set(n, 2, 4, 200);
        set(n, 3, 4, 200); // client 2: sum 410, cost 400
        M::elect(n, mask({1, 2, 3, 4}), e);
        check("min-max beats min-sum: 2 (cost 400, sum 410) is preferred to 1 (cost 910, sum 920)", e.winner == 2 || e.winner == 3);
        // 7. a subset: the hub and client 2 are gone.
        M::elect(m, mask({1, 3, 4}), e);
        checkf(rank_is(e, {3, 4, 1}), "subset {1,3,4}: ranking is 3>4>1 (got %s)", rank_str(e).c_str());
    }
    // 2. ties: by the sum, then by the lowest id.
    {
        M::Matrix m;
        set(m, 1, 2, 100);
        set(m, 1, 3, 100);
        set(m, 1, 4, 50); // cost(1)=200 sum=250
        set(m, 2, 3, 100);
        set(m, 2, 4, 10); // cost(2)=200 sum=210
        set(m, 3, 4, 400);
        M::elect(m, mask({1, 2, 3, 4}), e);
        checkf(rank_is(e, {2, 1, 4, 3}), "tie on cost -> the smaller sum wins: 2>1>4>3 (got %s)", rank_str(e).c_str());
        check("...and the two costs really tie", e.cost[0] == 200 && e.cost[1] == 200 && e.sum[0] < e.sum[1]);
        M::Matrix q;
        set(q, 2, 5, 100);
        set(q, 2, 7, 100);
        set(q, 5, 7, 100);
        M::elect(q, mask({2, 5, 7}), e);
        checkf(rank_is(e, {2, 5, 7}), "full tie (cost and sum) -> the lowest player id: 2>5>7 (got %s)", rank_str(e).c_str());
    }
    // 3. an unmeasured edge makes a candidate ineligible.
    {
        M::Matrix m;
        set(m, 1, 2, 100);
        set(m, 2, 3, 100); // 1-3 unmeasured
        M::elect(m, mask({1, 2, 3}), e);
        checkf(rank_is(e, {2, 1, 3}), "unmeasured edge 1-3: only 2 reaches everybody -> 2>1>3 (got %s)", rank_str(e).c_str());
        check("...tier DIRECT, ineligible candidates carry no cost", e.tier == M::TIER_DIRECT && e.cost[1] == 0xFFFFFFFFu && e.cost[2] == 0xFFFFFFFFu);
        // a cheap-but-unmeasured candidate never beats an expensive measured one
        M::Matrix n;
        set(n, 1, 2, 5);
        set(n, 2, 3, 900);
        set(n, 3, 4, 900);
        set(n, 2, 4, 900);
        set(n, 1, 3, 5); // 1-4 unmeasured: 1 is ineligible however cheap its other edges are
        M::elect(n, mask({1, 2, 3, 4}), e);
        check("a candidate with an unmeasured edge is not rank 1 even if its other edges are the cheapest", e.winner != 1 && e.tier == M::TIER_DIRECT);
    }
    // 4. the relay tier.
    {
        M::Matrix m;
        m.relay_ok = 1;
        m.leg[1]   = 300;
        m.leg[2]   = 100;
        m.leg[3]   = 100;
        M::elect(m, mask({1, 2, 3}), e);
        checkf(rank_is(e, {2, 3, 1}) && e.tier == M::TIER_RELAY, "relay tier: shortest leg first, ties by id -> 2>3>1 (got %s tier %d)",
               rank_str(e).c_str(), (int)e.tier);
        m.leg[3] = M::NONE; // one leg unmeasured -> the relay tier cannot vouch for everybody
        M::elect(m, mask({1, 2, 3}), e);
        checkf(rank_is(e, {1, 2, 3}) && e.tier == M::TIER_ID, "a missing leg drops to the id tier -> 1>2>3 (got %s tier %d)", rank_str(e).c_str(),
               (int)e.tier);
        m.leg[3]   = 100;
        m.relay_ok = 0; // legs without a relay carrying the match are not evidence
        M::elect(m, mask({1, 2, 3}), e);
        check("legs are ignored when no relay carries the match", e.tier == M::TIER_ID && rank_is(e, {1, 2, 3}));
        // some direct edges but nobody reaches everyone, relay carries the match
        M::Matrix n;
        n.relay_ok = 1;
        n.leg[1]   = 50;
        n.leg[2]   = 70;
        n.leg[3]   = 20;
        set(n, 1, 2, 10);
        M::elect(n, mask({1, 2, 3}), e);
        checkf(rank_is(e, {3, 1, 2}) && e.tier == M::TIER_RELAY, "nobody reaches everybody directly -> the relay tier ranks by leg: 3>1>2 (got %s tier %d)",
               rank_str(e).c_str(), (int)e.tier);
        // mixed: one eligible candidate, the others follow by leg
        M::Matrix o;
        o.relay_ok = 1;
        o.leg[1]   = 90;
        o.leg[2]   = 10;
        o.leg[3]   = 40;
        set(o, 1, 2, 100);
        set(o, 1, 3, 100);
        // 2-3 unmeasured -> only 1 reaches both
        M::elect(o, mask({1, 2, 3}), e);
        checkf(rank_is(e, {1, 2, 3}) && e.tier == M::TIER_DIRECT, "one eligible candidate leads, the rest by known leg: 1>2>3 (got %s)", rank_str(e).c_str());
    }
    // 5. no data, empty, single.
    {
        M::Matrix m;
        M::elect(m, mask({3, 1, 6}), e);
        checkf(rank_is(e, {1, 3, 6}) && e.tier == M::TIER_ID && e.winner == 1, "no data: the lowest id (1>3>6) (got %s)", rank_str(e).c_str());
        M::elect(m, 0, e);
        check("empty S: no winner, no tier", e.winner == -1 && e.n == 0 && e.tier == M::TIER_NONE);
        M::elect(m, mask({4}), e);
        check("single survivor: it wins at cost 0", e.winner == 4 && e.n == 1 && e.tier == M::TIER_DIRECT && e.cost[0] == 0);
    }
    // 6. edges: one direction is enough; both are averaged.
    {
        M::Matrix m;
        m.rtt[1][2] = 100;
        check("edge: one measured direction is the edge", M::edge(m, 1, 2) == 100 && M::edge(m, 2, 1) == 100);
        m.rtt[2][1] = 200;
        check("edge: both directions average (rounded up)", M::edge(m, 1, 2) == 150);
        m.rtt[2][1] = 201;
        check("edge: 100/201 -> 151", M::edge(m, 1, 2) == 151);
        check("edge: nothing -> NONE; an edge to oneself is 0", M::edge(m, 1, 3) == M::NONE && M::edge(m, 4, 4) == 0);
        M::Matrix r;
        r.relay_ok = 1;
        r.leg[1]   = 30;
        r.leg[2]   = 40;
        check("pair_rtt: no direct edge but both legs -> the relay estimate", M::pair_rtt(r, 1, 2) == 70);
        r.leg[2] = M::NONE;
        check("pair_rtt: a missing leg -> NONE", M::pair_rtt(r, 1, 2) == M::NONE);
    }
    // 8. the symmetric re-reading does not change the result.
    {
        M::Matrix a, b;
        set(a, 1, 2, 120);
        set(a, 1, 3, 340);
        set(a, 2, 3, 90);
        b.rtt[1][2] = 120;
        b.rtt[2][1] = 120;
        b.rtt[3][1] = 340; // only the other direction
        b.rtt[2][3] = 90;  // and only one direction here
        M::Election ea, eb;
        M::elect(a, mask({1, 2, 3}), ea);
        M::elect(b, mask({1, 2, 3}), eb);
        check("one-direction reports elect the same ranking as symmetric ones", ea.n == eb.n && memcmp(ea.rank, eb.rank, 3) == 0);
    }
    // 9. hysteresis.
    {
        check("beats_incumbent: 15 % is not enough", !M::beats_incumbent(1000, 850));
        check("beats_incumbent: 30 % and 30 ms is enough", M::beats_incumbent(1000, 700));
        check("beats_incumbent: 10 ms or less is not enough however large the ratio", !M::beats_incumbent(300, 250));
        check("beats_incumbent: 12 ms and 24 % is enough", M::beats_incumbent(500, 380));
        check("beats_incumbent: an unusable incumbent is replaced by anything usable", M::beats_incumbent(0xFFFFFFFFu, 5000));
        check("beats_incumbent: nothing usable on either side is no change", !M::beats_incumbent(0xFFFFFFFFu, 0xFFFFFFFFu));
        check("beats_incumbent: an equal cost is no change", !M::beats_incumbent(400, 400));
    }
}

// An independent statement of plan section 4, brute force over ordered pairs.
void test_elect_fuzz() {
    printf("  -- A. elect(): randomised vs the brute-force statement of the formula\n");
    uint32_t rng = 0x1234abcdu;
    auto     rnd = [&](uint32_t n) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng % n;
    };
    int bad = 0, cases = 0, direct = 0, relay = 0, byid = 0;
    for (int it = 0; it < 4000; ++it) {
        M::Matrix m;
        m.relay_ok                       = (uint8_t)rnd(2);
        static const uint32_t DENSITY[4] = {100, 80, 35, 10}; // percent of directed entries present
        const uint32_t        dens       = DENSITY[rnd(4)];
        for (int i = 0; i < 8; ++i) {
            if (rnd(4) != 0) m.leg[i] = (uint16_t)(1 + rnd(2000));
            for (int j = 0; j < 8; ++j)
                if (rnd(100) < dens) m.rtt[i][j] = (uint16_t)(1 + rnd(5000));
        }
        uint32_t S = rnd(256);
        if (rnd(3) == 0) S &= ~(1u << rnd(8));
        M::Election e;
        M::elect(m, S, e);
        ++cases;
        // permutation of S
        bool     perm = e.n == M::popcnt(S);
        uint32_t seen = 0;
        for (int i = 0; perm && i < e.n; ++i) {
            if (!(S >> e.rank[i] & 1u) || (seen >> e.rank[i] & 1u)) perm = false;
            seen |= 1u << e.rank[i];
        }
        if (!perm) {
            ++bad;
            continue;
        }
        if (e.n == 0) {
            if (e.winner != -1) ++bad;
            continue;
        }
        // brute force
        struct C {
            int      id;
            bool     ok;
            uint32_t cost, sum;
        };
        std::vector<C> cs;
        bool           any = false;
        for (int c = 0; c < 8; ++c) {
            if (!(S >> c & 1u)) continue;
            C x{c, true, 0, 0};
            for (int k = 0; k < 8 && x.ok; ++k)
                if (k != c && (S >> k & 1u) && M::edge(m, c, k) == M::NONE) x.ok = false;
            if (x.ok) {
                // max over ordered pairs (i,j), i != j, of d(i,c) + d(c,j), d(c,c) = 0
                for (int i = 0; i < 8; ++i)
                    for (int j = 0; j < 8; ++j) {
                        if (i == j || !(S >> i & 1u) || !(S >> j & 1u)) continue;
                        const uint32_t d1 = (i == c) ? 0 : M::edge(m, i, c);
                        const uint32_t d2 = (j == c) ? 0 : M::edge(m, c, j);
                        if (i != c && j != c && (M::edge(m, i, c) == M::NONE || M::edge(m, c, j) == M::NONE)) continue;
                        x.cost = std::max(x.cost, d1 + d2);
                    }
                for (int k = 0; k < 8; ++k)
                    if (k != c && (S >> k & 1u)) x.sum += M::edge(m, c, k);
                any = true;
            }
            cs.push_back(x);
        }
        if (any) {
            ++direct;
            std::vector<C> el;
            for (auto &x : cs)
                if (x.ok) el.push_back(x);
            std::sort(el.begin(), el.end(), [](const C &a, const C &b) {
                if (a.cost != b.cost) return a.cost < b.cost;
                if (a.sum != b.sum) return a.sum < b.sum;
                return a.id < b.id;
            });
            bool ok = e.tier == M::TIER_DIRECT;
            for (size_t i = 0; i < el.size() && ok; ++i) ok = e.rank[i] == el[i].id && e.cost[i] == el[i].cost && e.sum[i] == el[i].sum;
            if (!ok) ++bad;
        } else {
            bool all_legs = m.relay_ok != 0;
            for (auto &x : cs)
                if (m.leg[x.id] == M::NONE) all_legs = false;
            std::vector<int> ids;
            for (auto &x : cs) ids.push_back(x.id);
            if (all_legs) {
                ++relay;
                std::sort(ids.begin(), ids.end(), [&](int a, int b) { return m.leg[a] != m.leg[b] ? m.leg[a] < m.leg[b] : a < b; });
            } else {
                ++byid;
                std::sort(ids.begin(), ids.end());
            }
            bool ok = e.tier == (all_legs ? M::TIER_RELAY : M::TIER_ID);
            for (size_t i = 0; i < ids.size() && ok; ++i) ok = e.rank[i] == ids[i];
            if (!ok) ++bad;
        }
    }
    checkf(bad == 0, "elect() agrees with the brute-force statement on %d random matrices (%d direct, %d relay, %d by id), %d disagreements",
           cases, direct, relay, byid, bad);
    check("the fuzz reached every tier", direct > 100 && relay > 10 && byid > 10);
}

// ================================================================================================
// B. codecs + keys
// ================================================================================================
M::Epoch sample_epoch() {
    M::Epoch e;
    e.epoch = 7;
    e.hub   = 0;
    e.S     = (uint8_t)mask({1, 2, 3});
    M::Matrix m;
    set(m, 1, 2, 123);
    set(m, 1, 3, 456);
    m.rtt[2][3] = 78;
    m.leg[1]    = 50;
    m.relay_ok  = 1;
    e.M         = m;
    M::Election el;
    M::elect(m, e.S, el);
    e.n    = el.n;
    e.tier = el.tier;
    for (int i = 0; i < el.n; ++i) e.rank[i] = el.rank[i];
    e.digest = M::digest(m, e.S);
    for (int i = 1; i <= 3; ++i) {
        M::Addr4 a;
        a.ip[0] = 10;
        a.ip[1] = 0;
        a.ip[2] = (uint8_t)i;
        a.ip[3] = 7;
        a.port  = (uint16_t)(40000 + i);
        e.dial[i].add(a);
        a.ip[0] = 192;
        e.dial[i].add(a);
        e.room[i] = 1000u + (uint32_t)i;
    }
    return e;
}

void test_codecs() {
    printf("  -- B. the FLAG_MESH codecs, the probe keys and the digest\n");
    // mp:U62 -- HUB_LEAVING / LEAVE_ACK
    {
        M::Leaving l;
        l.epoch = 7;
        l.gen   = 3;
        l.hub   = 0;
        l.n     = 3;
        for (int i = 0; i < 3; ++i) {
            l.e[i].id      = (uint8_t)(i + 1);
            l.e[i].relayed = (i == 1);
            l.e[i].room    = 900u + (uint32_t)i;
            M::Addr4 a;
            a.ip[0] = 10;
            a.ip[1] = 0;
            a.ip[2] = 0;
            a.ip[3] = (uint8_t)(i + 1);
            a.port  = (uint16_t)(6000 + i);
            l.e[i].cands.add(a);
        }
        uint8_t   b[256];
        const int n = M::leaving_encode(l, b, sizeof(b));
        check("HUB_LEAVING encodes", n > 8);
        M::Leaving r;
        check("...and decodes to the same list",
              n > 0 && M::leaving_decode(b, n, r) && r.epoch == 7 && r.gen == 3 && r.hub == 0 && r.n == 3 && r.e[1].relayed && !r.e[0].relayed &&
                  r.e[2].room == 902 && r.e[0].cands.n == 1 && r.e[2].cands.a[0].port == 6002);
        check("a truncated HUB_LEAVING is refused", !M::leaving_decode(b, n - 1, r));
        b[n] = 0;
        check("a trailing byte is refused", !M::leaving_decode(b, n + 1, r));
        M::Leaving dup = l;
        dup.e[1].id    = dup.e[0].id;
        int n2         = M::leaving_encode(dup, b, sizeof(b));
        check("a duplicate successor id is a corrupt list", n2 > 0 && !M::leaving_decode(b, n2, r));
        M::Leaving self = l;
        self.e[0].id    = self.hub;
        n2              = M::leaving_encode(self, b, sizeof(b));
        check("the leaving hub listed as its own successor is refused", n2 > 0 && !M::leaving_decode(b, n2, r));
        M::Leaving none;
        check("an empty list does not encode", M::leaving_encode(none, b, sizeof(b)) == 0);
        uint8_t  a7[M::LEAVE_ACK_BYTES];
        uint32_t e2 = 0;
        uint8_t  g2 = 0, f2 = 0;
        M::leave_ack_encode(9, 4, M::LA_SUCCESSOR, a7);
        check("LEAVE_ACK round-trips", M::leave_ack_decode(a7, sizeof(a7), e2, g2, f2) && e2 == 9 && g2 == 4 && f2 == M::LA_SUCCESSOR);
        check("...and a wrong length is refused", !M::leave_ack_decode(a7, sizeof(a7) - 1, e2, g2, f2));
        check("the new hub's first epoch is held + 2", M::successor_first_epoch(5) == 7);
    }
    // candidates
    {
        M::CandList c;
        for (int i = 0; i < 4; ++i) {
            M::Addr4 a;
            a.ip[0] = 192;
            a.ip[1] = 168;
            a.ip[2] = 1;
            a.ip[3] = (uint8_t)(10 + i);
            a.port  = (uint16_t)(5000 + i);
            c.add(a);
        }
        check("CandList refuses a fifth and a duplicate", !c.add(c.a[0]) && c.n == 4);
        uint8_t     b[64];
        int         n = M::cand_frame_encode(true, c, b, sizeof(b));
        M::CandList d;
        bool        rel = false;
        check("MK_CAND round-trips", n > 0 && M::cand_frame_decode(b, n, rel, d) && rel && d.n == 4 && M::addr_eq(d.a[3], c.a[3]));
        bool prefixes = true;
        for (int k = 0; k < n; ++k)
            if (M::cand_frame_decode(b, k, rel, d)) prefixes = false;
        check("MK_CAND: every strict prefix is refused", prefixes);
        b[n] = 0;
        check("MK_CAND: a trailing byte is refused", !M::cand_frame_decode(b, n + 1, rel, d));
        b[2] = 5; // five candidates declared
        check("MK_CAND: more than MAX_CANDS is a refusal, never a shorter list", !M::cand_frame_decode(b, n, rel, d));
    }
    // broker
    {
        M::Broker b;
        memset(&b, 0, sizeof(b));
        for (int i = 0; i < 32; ++i) b.key[i] = (uint8_t)(i * 7 + 1);
        b.n            = 2;
        b.e[0].id      = 2;
        b.e[0].relayed = false;
        M::Addr4 a;
        memset(&a, 0, sizeof(a));
        a.ip[0] = 1;
        a.ip[1] = 2;
        a.ip[2] = 3;
        a.ip[3] = 4;
        a.port  = 999;
        b.e[0].cands.add(a);
        b.e[1].id      = 5;
        b.e[1].relayed = true;
        uint8_t   buf[256];
        const int n = M::broker_encode(b, buf, sizeof(buf));
        M::Broker d;
        check("MK_BROKER round-trips (key, ids, the relayed bit, candidates)",
              n > 0 && M::broker_decode(buf, n, d) && d.n == 2 && memcmp(d.key, b.key, 32) == 0 && d.e[0].id == 2 && d.e[0].cands.n == 1 &&
                  M::addr_eq(d.e[0].cands.a[0], a) && d.e[1].relayed && d.e[1].cands.n == 0);
        bool prefixes = true;
        for (int k = 0; k < n; ++k)
            if (M::broker_decode(buf, k, d)) prefixes = false;
        check("MK_BROKER: every strict prefix is refused", prefixes);
        buf[n] = 0;
        check("MK_BROKER: a trailing byte is refused", !M::broker_decode(buf, n + 1, d));
    }
    // row
    {
        M::Row r;
        memset(&r, 0, sizeof(r));
        r.n       = 2;
        r.peer[0] = 2;
        r.rtt[0]  = 321;
        r.peer[1] = 3;
        r.rtt[1]  = M::NONE;
        r.leg     = 77;
        r.relayed = true;
        uint8_t   buf[64];
        const int n = M::row_encode(r, buf, sizeof(buf));
        M::Row    d;
        check("MK_ROW round-trips", n > 0 && M::row_decode(buf, n, d) && d.n == 2 && d.rtt[0] == 321 && d.rtt[1] == M::NONE && d.leg == 77 && d.relayed);
        bool prefixes = true;
        for (int k = 0; k < n; ++k)
            if (M::row_decode(buf, k, d)) prefixes = false;
        check("MK_ROW: every strict prefix is refused", prefixes);
        buf[2] = 9; // a peer id out of range
        check("MK_ROW: a peer id outside 0..7 is refused", !M::row_decode(buf, n, d));
    }
    // epoch
    {
        const M::Epoch e = sample_epoch();
        uint8_t        buf[M::MAX_FRAME];
        const int      n = M::epoch_encode(e, buf, sizeof(buf));
        M::Epoch       d;
        check("MK_EPOCH round-trips", n > 0 && M::epoch_decode(buf, n, d));
        check("...epoch, digest, hub, S, tier, rank", d.epoch == 7 && d.digest == e.digest && d.hub == 0 && d.S == e.S && d.tier == e.tier && d.n == e.n &&
                                                          memcmp(d.rank, e.rank, e.n) == 0);
        bool same = true;
        for (int i = 1; i <= 3; ++i) {
            same = same && d.room[i] == e.room[i] && d.dial[i].n == e.dial[i].n && M::addr_eq(d.dial[i].a[1], e.dial[i].a[1]) && d.M.leg[i] == e.M.leg[i];
            for (int j = 1; j <= 3; ++j) same = same && d.M.rtt[i][j] == e.M.rtt[i][j];
        }
        check("...the matrix, the legs, the dial info and the rooms", same);
        check("...and the digest recomputed from the decoded matrix is the one it carried", M::digest(d.M, d.S) == d.digest);
        bool prefixes = true;
        for (int k = 0; k < n; ++k)
            if (M::epoch_decode(buf, k, d)) prefixes = false;
        check("MK_EPOCH: every strict prefix is refused", prefixes);
        buf[n] = 0;
        check("MK_EPOCH: a trailing byte is refused", !M::epoch_decode(buf, n + 1, d));
        uint8_t big[8];
        check("MK_EPOCH: an output buffer that is too small is a refusal", M::epoch_encode(e, big, sizeof(big)) == 0);
        buf[n]  = 0;
        buf[10] = 0x80; // S names a player the rank list does not
        check("MK_EPOCH: a ranking entry outside S is refused", !M::epoch_decode(buf, n, d));
    }
    // ack + probe body
    {
        uint8_t a[M::ACK_BYTES];
        M::ack_encode(0xdeadbeefu, 0x01020304u, a);
        uint32_t e = 0, d = 0;
        check("MK_ACK round-trips", M::ack_decode(a, sizeof(a), e, d) && e == 0xdeadbeefu && d == 0x01020304u);
        check("MK_ACK: a short or long frame is refused", !M::ack_decode(a, sizeof(a) - 1, e, d) && !M::ack_decode(a, sizeof(a) + 1, e, d));
        uint8_t body[M::PROBE_BODY];
        M::probe_body(M::PR_PROBE, 2, 5, 0x11223344u, 1, body);
        uint8_t  k = 0, fl = 0;
        int      f = 0, t = 0;
        uint32_t nn = 0;
        check("probe body round-trips", M::probe_body_decode(body, sizeof(body), k, f, t, nn, fl) && k == M::PR_PROBE && f == 2 && t == 5 && nn == 0x11223344u && fl == 1);
        body[0] = 9;
        check("probe body: an unknown kind is refused", !M::probe_body_decode(body, sizeof(body), k, f, t, nn, fl));
        body[0] = M::PR_ECHO;
        body[1] = 8;
        check("probe body: a sender id outside 0..7 is refused", !M::probe_body_decode(body, sizeof(body), k, f, t, nn, fl));
    }
    // keys + sealed probes
    {
        uint8_t key[32], key2[32];
        for (int i = 0; i < 32; ++i) {
            key[i]  = (uint8_t)(i + 1);
            key2[i] = (uint8_t)(i + 2);
        }
        M::ProbeKeys a1, a2, b1, a1b;
        M::probe_keys(key, 1, a1);
        M::probe_keys(key, 1, a1b);
        M::probe_keys(key, 2, a2);
        M::probe_keys(key2, 1, b1);
        check("probe keys are deterministic", memcmp(&a1, &a1b, sizeof(a1)) == 0);
        check("...differ per sender (cid, enc and mac)", memcmp(a1.cid, a2.cid, 8) != 0 && memcmp(a1.enc, a2.enc, 32) != 0 && memcmp(a1.mac, a2.mac, 32) != 0);
        check("...and per mesh key", memcmp(a1.cid, b1.cid, 8) != 0 && memcmp(a1.enc, b1.enc, 32) != 0);
        check("...and enc != mac", memcmp(a1.enc, a1.mac, 32) != 0);
        uint8_t body[M::PROBE_BODY];
        M::probe_body(M::PR_PROBE, 1, 2, 77, 0, body);
        uint8_t      pkt[U::MAX_DATAGRAM];
        U::Verdict   why = U::Verdict::Ok;
        const size_t n   = U::packet_encode(U::PKT_DATA, a1.cid, 5, a1.enc, a1.mac, body, sizeof(body), pkt, why);
        check("a probe seals to HDR + 8 + TAG = 42 bytes", n == U::HDR_SIZE + M::PROBE_BODY + U::TAG_SIZE && n == 42);
        U::Header ph;
        check("...and its connection id is readable without a key", U::hdr_peek(pkt, n, ph) && memcmp(ph.conn_id, a1.cid, 8) == 0);
        U::ReplayWindow win;
        uint8_t         copy[U::MAX_DATAGRAM];
        memcpy(copy, pkt, n);
        U::Header h;
        size_t    bl = 0;
        check("it opens under the sender's keys", U::packet_decode(copy, n, a1.cid, a1.enc, a1.mac, &win, h, &bl) == U::Verdict::Ok && bl == M::PROBE_BODY &&
                                                      memcmp(copy + U::HDR_SIZE, body, sizeof(body)) == 0);
        memcpy(copy, pkt, n);
        check("a replay of it is refused", U::packet_decode(copy, n, a1.cid, a1.enc, a1.mac, &win, h, &bl) == U::Verdict::Replay);
        U::ReplayWindow w2;
        memcpy(copy, pkt, n);
        check("under another sender's keys it does not authenticate", U::packet_decode(copy, n, a1.cid, a2.enc, a2.mac, &w2, h, &bl) == U::Verdict::BadMac);
        memcpy(copy, pkt, n);
        check("under another mesh key it does not authenticate", U::packet_decode(copy, n, a1.cid, b1.enc, b1.mac, &w2, h, &bl) == U::Verdict::BadMac);
        memcpy(copy, pkt, n);
        copy[U::HDR_SIZE] ^= 1;
        check("a flipped body bit does not authenticate", U::packet_decode(copy, n, a1.cid, a1.enc, a1.mac, &w2, h, &bl) == U::Verdict::BadMac);
    }
    // digest
    {
        M::Matrix m;
        set(m, 1, 2, 100);
        set(m, 1, 3, 200);
        m.leg[1]         = 5;
        const uint32_t S = mask({1, 2, 3}), d0 = M::digest(m, S);
        M::Matrix      n = m;
        n.rtt[1][2]      = 101;
        check("digest moves with an rtt entry of S", M::digest(n, S) != d0);
        n        = m;
        n.leg[2] = 9;
        check("...with a leg", M::digest(n, S) != d0);
        n          = m;
        n.relay_ok = 1;
        check("...with relay_ok", M::digest(n, S) != d0);
        n           = m;
        n.rtt[6][7] = 55;
        n.leg[6]    = 3;
        check("...and NOT with an entry outside S (elect() cannot read it)", M::digest(n, S) == d0);
        check("...but with S itself", M::digest(m, mask({1, 2})) != d0);
    }
}

// ================================================================================================
// C/D. endpoints on 127.0.0.1
// ================================================================================================
constexpr int MAXC = 4;
Endpoint      g_e[1 + MAXC]; // BSS: each is several MB of stream rings
// Test-only clock scale: the loopback arms would otherwise wait out real-time budgets (T_suspect >= 2 s, epoch settle, 20 s budget).
// The shipped values stay untouched (the DLL never sets Config::mesh_time_div); the pure-rule tests (test_fo_pure) still use them.
constexpr int kDiv  = 8;
int           g_div = kDiv; // 1 for the arms that measure real-time cadence (the bandwidth arm)
int           g_leg_dms[1 + MAXC];
uint32_t      g_room_next        = 5000;
bool          g_no_migration     = false; // mp:U62 -- the negative arm: `[net] hub_migration=0`
int           g_fo_budget        = 0;     // mp:U63 -- `[net] failover_budget_ms` for the next arm (0 = the default)
int           g_log_ix[1 + MAXC] = {0, 1, 2, 3, 4};

int      g_leg_age[1 + MAXC] = {100, 100, 100, 100, 100}; // mp:U64 -- the leg_age_ms hook answer per endpoint (ms since its relay leg last answered)
int      leg_hook(void *ctx) { return *(int *)ctx; }
int      leg_age_hook(void *ctx) { return g_leg_age[(int *)ctx - g_leg_dms]; }
uint32_t room_hook(void *) { return ++g_room_next; }

void fill_cfg(Config &c, int role, int port, unsigned short bind_port, bool relay_sim, int rx_timeout) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port           = port;
    c.net.player_id      = role == 0 ? 0 : 1;
    c.net.log            = 1;
    c.net.host_assign    = 1;
    c.net.ping_ms        = 200;
    c.net.rx_timeout_ms  = rx_timeout; // -1 = a suite that stops to assert must not have a watchdog dropping its peers
    c.redundancy         = 3;
    c.bind_port          = bind_port;
    c.no_hub_migration   = g_no_migration;
    c.failover_budget_ms = g_fo_budget;
    c.mesh_time_div      = g_div;              // every mesh/failover clock runs 1/kDiv of its shipped length (Config::mesh_time_div)
    c.leg_overhead       = relay_sim ? 34 : 0; // a relayed endpoint dials its tunnel on loopback; here the dial IS loopback
}

struct Arm {
    int  clients;
    bool relay_sim;
    int  rx_timeout;
    int  base;
    int  ids[MAXC]; // player id of client i
    bool up;
};

Endpoint &host(Arm &) { return g_e[0]; }
Endpoint &cli(Arm &, int i) { return g_e[1 + i]; }

bool bring_up(Arm &a, bool (*pre)(Arm &, int) = nullptr) {
    for (int i = 0; i <= a.clients; ++i) new (&g_e[i]) Endpoint();
    Config ch, cc;
    fill_cfg(ch, 0, a.base, (unsigned short)a.base, a.relay_sim, a.rx_timeout);
    for (int i = 0; i <= a.clients; ++i) {
        g_e[i].set_log(ep_log, &g_log_ix[i]);
        Endpoint::MeshHooks hk;
        hk.leg_rtt_dms = leg_hook;
        hk.leg_age_ms  = leg_age_hook;
        hk.mint_room   = room_hook;
        hk.ctx         = &g_leg_dms[i];
        g_e[i].set_mesh_hooks(hk);
        if (pre) pre(a, i);
    }
    bool ok = g_e[0].start(ch, TEST_PSK, true);
    for (int i = 0; i < a.clients; ++i) {
        fill_cfg(cc, 1, a.base, (unsigned short)(a.base + 1 + i), a.relay_sim, a.rx_timeout);
        ok &= g_e[1 + i].start(cc, TEST_PSK, true);
    }
    a.up = ok;
    return ok;
}

void tear_down(Arm &a) {
    for (int i = a.clients; i >= 0; --i) g_e[i].stop();
    a.up = false;
}

bool admitted(Arm &a) {
    if (g_e[0].peer_count() != a.clients) return false;
    for (int i = 0; i < a.clients; ++i)
        if (!g_e[1 + i].id_assigned()) return false;
    for (int i = 0; i < a.clients; ++i) a.ids[i] = g_e[1 + i].local_player_id();
    return true;
}

Endpoint::MeshStatus st(Endpoint &e) {
    Endpoint::MeshStatus s;
    e.mesh_status(s);
    return s;
}

// Every endpoint holds the same epoch, digest and ranking, and that ranking is elect() of the matrix.
bool same_epoch(Arm &a, uint32_t *epoch_out = nullptr, uint32_t *tier_out = nullptr, int *winner_out = nullptr, bool quiet = false) {
    const Endpoint::MeshStatus h = st(g_e[0]);
    if (h.epoch == 0) return false;
    bool ok = true;
    for (int i = 0; i < a.clients; ++i) {
        const Endpoint::MeshStatus c = st(g_e[1 + i]);
        if (c.epoch != h.epoch || c.digest != h.digest || c.rank_n != h.rank_n || memcmp(c.rank, h.rank, h.rank_n) != 0 || c.S != h.S) {
            if (!quiet) printf("    client %d: epoch %u digest %08x vs host %u %08x\n", i, c.epoch, c.digest, h.epoch, h.digest);
            ok = false;
        }
    }
    M::Election el;
    M::elect(h.M, h.S, el);
    bool agree = el.n == h.rank_n;
    for (int i = 0; agree && i < el.n; ++i) agree = el.rank[i] == h.rank[i];
    if (!agree) ok = false;
    if (epoch_out) *epoch_out = h.epoch;
    if (tier_out) *tier_out = h.tier;
    if (winner_out) *winner_out = h.rank_n ? h.rank[0] : -1;
    return ok;
}

bool all_acked(Arm &a) {
    const Endpoint::MeshStatus h = st(g_e[0]);
    return h.epoch != 0 && h.all_acked_ms != 0 && (h.acked_mask & h.S) == h.S && a.clients >= 1;
}

// ---- the direct arm -----------------------------------------------------------------------------
void direct_arm(int clients, int base) {
    printf("  -- C. %d-endpoint mesh (host + %d clients, direct)\n", clients + 1, clients);
    Arm a{};
    a.clients      = clients;
    a.base         = base;
    a.rx_timeout   = -1;
    g_div          = 1; // the bandwidth + stranger-datagram checks count probes per second: they run at the shipped cadence
    const DWORD t0 = GetTickCount();
    check("endpoints started", bring_up(a));
    DWORD      t_cov = 0;
    const bool acked = wait_for(
        [&] {
            const Endpoint::MeshStatus h = st(g_e[0]);
            if (t_cov == 0 && h.cov_total > 0 && h.cov_have == h.cov_total) t_cov = GetTickCount() - t0;
            return admitted(a) && all_acked(a);
        },
        9000);
    const DWORD t_ack = GetTickCount() - t0;
    checkf(acked, "the epoch ack round completes");
    if (!acked) {
        tear_down(a);
        g_div = kDiv;
        return;
    }
    checkf(t_cov != 0 && t_cov <= 5000, "every pair has a matrix entry within 5 s of start (%u ms)", (unsigned)t_cov);
    checkf(t_ack <= 5000, "the first epoch is published, delivered and acked by all within 5 s of start (%u ms)", (unsigned)t_ack);
    uint32_t epoch = 0, tier = 0;
    int      winner = -1;
    checkf(same_epoch(a, &epoch, &tier, &winner), "every endpoint holds the same epoch, digest, survivor set and ranking");
    const Endpoint::MeshStatus h = st(g_e[0]);
    checkf(epoch == 1, "the first epoch is epoch 1 (got %u)", epoch);
    checkf(tier == M::TIER_DIRECT, "the election ran on measured direct edges (tier %u)", tier);
    checkf(h.rank_n == clients, "the ranking lists every client (%d of %d)", h.rank_n, clients);
    checkf(h.cov_have == h.cov_total && h.cov_total == clients * (clients - 1) / 2, "the matrix covers all %d pairs (%d)", clients * (clients - 1) / 2, h.cov_have);
    bool pairs = true, rtt_sane = true;
    for (int i = 0; i < clients; ++i) {
        const Endpoint::MeshStatus c = st(g_e[1 + i]);
        for (int j = 0; j < clients; ++j) {
            if (i == j) continue;
            if (c.first_pair_ms[a.ids[j]] == 0) pairs = false;
        }
        check("a client's probes were answered (echo_rx > 0)", c.echo_rx > 0 || clients < 2);
        check("a client's ack went out and the host counted it", c.frames_tx >= 3);
        check("no bad probe / frame / ranking mismatch", c.bad_probe == 0 && c.bad_frame == 0 && c.rank_mismatch == 0);
    }
    for (int i = 1; i <= clients; ++i)
        for (int j = i + 1; j <= clients; ++j) {
            const uint16_t d = M::edge(h.M, a.ids[i - 1], a.ids[j - 1]);
            if (d == M::NONE || d > 20000) rtt_sane = false; // a loopback round trip is far below 2 s
        }
    check("every client measured an echo from every other client", pairs);
    check("every measured round trip is a real, small number (loopback)", rtt_sane);
    check("the dial info lists each client's brokered address and no relay room (a direct match)", [&] {
        for (int i = 0; i < clients; ++i)
            if (h.dial[a.ids[i]].n == 0 || h.room[a.ids[i]] != 0) return false;
        return true;
    }());
    check("the host persisted each client's latest acked epoch", [&] {
        for (int i = 0; i < clients; ++i)
            if (h.last_acked_epoch[a.ids[i]] != 1 || h.last_acked_digest[a.ids[i]] != h.digest) return false;
        return true;
    }());
    {
        M::Epoch held;
        check("a client's held epoch is readable (M5's input) and equals the host's", cli(a, 0).mesh_held_epoch(held) && held.epoch == 1 && held.digest == h.digest);
    }

    // ---- bandwidth, in steady state (the warm-up burst is over) -----------------------------------------
    const bool bw = clients >= 3; // the 4-endpoint arm is the bandwidth arm (more peers, the worse case)
    if (bw) Sleep(3600);
    Endpoint::MeshStatus b0[MAXC];
    for (int i = 0; i < clients; ++i) b0[i] = st(g_e[1 + i]);
    const DWORD w0 = GetTickCount();
    Sleep(bw ? 5000 : 200);
    const double secs          = (double)(GetTickCount() - w0) / 1000.0;
    double       worst_payload = 0, worst_wire = 0, worst_frames = 0, worst_total = 0;
    for (int i = 0; i < clients; ++i) {
        const Endpoint::MeshStatus b1     = st(g_e[1 + i]);
        const double               others = (double)(clients - 1);
        const double               dg     = (double)((b1.probe_tx + b1.echo_tx) - (b0[i].probe_tx + b0[i].echo_tx));
        const double               bytes  = (double)(b1.probe_bytes_tx - b0[i].probe_bytes_tx);
        const double               fb     = (double)(b1.frame_bytes_tx - b0[i].frame_bytes_tx);
        const double               pp     = bytes / secs / others; // payload bytes/s per other peer, one direction
        const double               wire   = (bytes + 28.0 * dg) / secs / others;
        worst_payload                     = std::max(worst_payload, pp);
        worst_wire                        = std::max(worst_wire, wire);
        worst_frames                      = std::max(worst_frames, fb / secs);
        worst_total                       = std::max(worst_total, (bytes + 28.0 * dg + fb) / secs);
    }
    if (bw) {
        printf("     bandwidth (%d clients, %.1f s steady): per other peer per direction %.0f B/s probe+echo payload, %.0f B/s on the wire (+28 B IP/UDP per datagram); "
               "FLAG_MESH stream frames %.0f B/s per client; whole mesh cost per client %.0f B/s on the wire\n",
               clients, secs, worst_payload, worst_wire, worst_frames, worst_total);
        printf("     plan section 3's estimate: ~60 B per probe, 1 probe per peer pair per second -> 60 B/s per other peer per direction\n");
        checkf(worst_payload <= 60.0, "probe+echo payload per other peer per direction (%.0f B/s) is within section 3's 60 B/s", worst_payload);
        checkf(worst_wire <= 75.0, "...and its on-the-wire cost (%.0f B/s) is within 25 %% of it", worst_wire);
        checkf(worst_frames <= 150.0, "the FLAG_MESH stream frames (row + ack) stay small (%.0f B/s per client)", worst_frames);
    }

    // ---- a stranger's datagram is not a probe -----------------------------------------------------------------
    {
        const Endpoint::MeshStatus c0 = st(g_e[1]);
        SOCKET                     s  = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in                to;
        memset(&to, 0, sizeof(to));
        to.sin_family      = AF_INET;
        to.sin_port        = htons((u_short)(base + 1));
        to.sin_addr.s_addr = inet_addr("127.0.0.1");
        uint8_t junk[64];
        for (int i = 0; i < 64; ++i) junk[i] = (uint8_t)(i * 13 + 5);
        junk[0] = 0x41; // the family byte, so it parses as a T0 header
        junk[1] = 0;
        sendto(s, (const char *)junk, 60, 0, (sockaddr *)&to, sizeof(to));
        // a well-formed datagram sealed under the WRONG mesh key still carries a connection id we do not know
        uint8_t k2[32];
        for (int i = 0; i < 32; ++i) k2[i] = (uint8_t)(0xA0 + i);
        M::ProbeKeys pk;
        M::probe_keys(k2, a.ids[1 % clients], pk);
        uint8_t body[M::PROBE_BODY];
        M::probe_body(M::PR_PROBE, a.ids[1 % clients], a.ids[0], 1, 0, body);
        uint8_t      pkt[U::MAX_DATAGRAM];
        U::Verdict   why = U::Verdict::Ok;
        const size_t n   = U::packet_encode(U::PKT_DATA, pk.cid, 1, pk.enc, pk.mac, body, sizeof(body), pkt, why);
        sendto(s, (const char *)pkt, (int)n, 0, (sockaddr *)&to, sizeof(to));
        closesocket(s);
        Sleep(300);
        const Endpoint::MeshStatus c1 = st(g_e[1]);
        check("a stranger's datagrams (junk, and a probe sealed under a foreign mesh key) are neither probes nor echoes",
              c1.probe_rx - c0.probe_rx <= 2 && c1.echo_rx >= c0.echo_rx && c1.bad_probe == c0.bad_probe && c1.rank_mismatch == 0);
        check("...and the arm is still healthy afterwards", same_epoch(a, nullptr, nullptr, nullptr, true));
    }
    tear_down(a);
    g_div = kDiv;
}

// ---- D1: a client that never answers a probe ------------------------------------------------------------
bool pre_mute_last(Arm &a, int i) {
    if (i == a.clients) g_e[i].mesh_test_no_probe(true); // the last client
    return true;
}
void muted_arm(int base) {
    printf("  -- D. an unmeasured client: the election falls back, and everybody still agrees\n");
    Arm a{};
    a.clients    = 3;
    a.base       = base;
    a.rx_timeout = -1;
    check("endpoints started", bring_up(a, pre_mute_last));
    uint32_t   epoch = 0, tier = 0;
    int        winner = -1;
    const bool ok     = wait_for([&] { return admitted(a) && all_acked(a); }, 14000);
    checkf(ok, "the epoch is still published (by the deadline) and acked by all");
    if (ok) {
        checkf(same_epoch(a, &epoch, &tier, &winner), "every endpoint holds the same epoch and ranking");
        const Endpoint::MeshStatus h = st(g_e[0]);
        checkf(tier == M::TIER_ID, "no candidate reaches everybody, no relay: the lowest-id tier (tier %u)", tier);
        int lo = 99;
        for (int i = 0; i < a.clients; ++i) lo = std::min(lo, a.ids[i]);
        checkf(winner == lo, "the winner is the lowest player id (%d, expected %d)", winner, lo);
        check("the matrix is NOT complete (the muted client's pairs have no entry)", h.cov_have < h.cov_total);
        const Endpoint::MeshStatus c0 = st(g_e[1]);
        check("the muted client's pairs have no first echo on a measuring client", c0.first_pair_ms[a.ids[2]] == 0);
        check("the measured pair (clients 0 and 1) does have one", c0.first_pair_ms[a.ids[1]] != 0);
    }
    tear_down(a);
}

// ---- D2: e+1 only after every ack of e --------------------------------------------------------------------
bool pre_noack_last(Arm &a, int i) {
    if (i == a.clients) g_e[i].mesh_test_no_ack(true);
    return true;
}
void noack_arm(int base) {
    printf("  -- D. succession epochs: e+1 only after every client acked e\n");
    Arm a{};
    a.clients    = 3;
    a.base       = base;
    a.rx_timeout = 1000; // the host must notice a client that stops (2500 shipped-scale; 5 pings at the arm's 200 ms cadence)
    check("endpoints started", bring_up(a, pre_noack_last));
    if (!wait_for([&] { return admitted(a); }, 8000)) {
        check("clients admitted", false);
        tear_down(a);
        return;
    }
    const int  silent_id = a.ids[2]; // client 2 never acks
    const bool published = wait_for([&] { return st(g_e[0]).epoch >= 1; }, 9000);
    checkf(published, "epoch 1 is published");
    long max_spread = 0;
    auto spread     = [&] {
        uint32_t lo = 0xffffffffu, hi = 0;
        for (int i = 0; i < a.clients; ++i) {
            if (!g_e[1 + i].started()) continue;
            const uint32_t e = st(g_e[1 + i]).epoch;
            lo               = std::min(lo, e);
            hi               = std::max(hi, e);
        }
        if (hi >= lo && lo != 0xffffffffu) max_spread = std::max(max_spread, (long)(hi - lo));
    };
    wait_for([&] { spread(); return (st(g_e[0]).acked_mask & ((1u << a.ids[0]) | (1u << a.ids[1]))) == ((1u << a.ids[0]) | (1u << a.ids[1])); }, 5000);
    for (int k = 0; k < 15; ++k) {
        Sleep(100);
        spread();
    }
    Endpoint::MeshStatus h = st(g_e[0]);
    checkf(h.epoch == 1 && !(h.acked_mask >> silent_id & 1u), "with one client not acking, the host is still on epoch 1 and the ack set lacks it");
    checkf(h.epoch_resends > 0, "...and re-sends epoch 1 to the silent client (%ld re-sends)", h.epoch_resends);
    // a roster change while epoch 1 is outstanding must NOT start epoch 2
    g_e[2].stop(); // client 1 leaves; the host's watchdog notices after 1 s
    wait_for([&] { spread(); return g_e[0].peer_count() == 2; }, 9000);
    for (int k = 0; k < 20; ++k) {
        Sleep(100);
        spread();
    }
    h = st(g_e[0]);
    checkf(g_e[0].peer_count() == 2 && h.epoch == 1, "client 1 left, the roster changed, and the host STILL holds epoch 1 (it never sends e+1 before every ack of e): epoch %u", h.epoch);
    // the silent client's acks come back
    g_e[3].mesh_test_no_ack(false);
    const bool e2 = wait_for([&] { spread(); return st(g_e[0]).epoch == 2 && all_acked(a); }, 12000);
    checkf(e2, "after the silent client acks, epoch 2 is published and acked by the survivors");
    if (e2) {
        h = st(g_e[0]);
        check("epoch 2's survivor set is the two remaining clients", h.S == (uint8_t)((1u << a.ids[0]) | (1u << a.ids[2])) && h.rank_n == 2);
        const Endpoint::MeshStatus c0 = st(g_e[1]), c2 = st(g_e[3]);
        check("both survivors hold epoch 2 with the host's digest", c0.epoch == 2 && c2.epoch == 2 && c0.digest == h.digest && c2.digest == h.digest);
        check("the host remembers the latest acked epoch per peer (survivors at 2, the departed at 1 or less)",
              h.last_acked_epoch[a.ids[0]] == 2 && h.last_acked_epoch[a.ids[2]] == 2 && h.last_acked_epoch[a.ids[1]] <= 1);
    }
    checkf(max_spread <= 1, "at no instant did two clients hold epochs more than 1 apart (max spread %ld)", max_spread);
    tear_down(a);
}

// ---- D3: relayed clients ---------------------------------------------------------------------------------
void relay_arm(int base) {
    printf("  -- D. relayed clients (simulated by the leg overhead): legs, no probes, a room each\n");
    Arm a{};
    a.clients    = 3;
    a.relay_sim  = true;
    a.base       = base;
    a.rx_timeout = -1;
    g_leg_dms[1] = 600;
    g_leg_dms[2] = 200; // client index 1: the shortest leg
    g_leg_dms[3] = 900;
    g_leg_dms[0] = 0;
    check("endpoints started", bring_up(a));
    const bool ok = wait_for([&] { return admitted(a) && all_acked(a); }, 12000);
    checkf(ok, "the epoch ack round completes");
    if (ok) {
        uint32_t epoch = 0, tier = 0;
        int      winner = -1;
        checkf(same_epoch(a, &epoch, &tier, &winner), "every endpoint holds the same epoch and ranking");
        const Endpoint::MeshStatus h = st(g_e[0]);
        checkf(tier == M::TIER_RELAY, "relayed clients: the relay tier decided (tier %u)", tier);
        checkf(winner == a.ids[1], "the client with the shortest relay leg is rank 1 (id %d, expected %d)", winner, a.ids[1]);
        long probes = 0;
        for (int i = 0; i < a.clients; ++i) probes += st(g_e[1 + i]).probe_tx + st(g_e[1 + i]).echo_tx;
        checkf(probes == 0, "a relayed client sends no direct probe (its socket is a loopback tunnel): %ld datagrams", probes);
        check("every pair still has a matrix entry (the relay estimate leg_i + leg_j)", h.cov_have == h.cov_total && h.cov_total == 3);
        check("the dial info of a relayed client carries no direct address", [&] {
            for (int i = 0; i < a.clients; ++i)
                if (h.dial[a.ids[i]].n != 0) return false;
            return true;
        }());
        uint32_t rooms[3];
        bool     nz = true;
        for (int i = 0; i < a.clients; ++i) {
            rooms[i] = h.room[a.ids[i]];
            nz       = nz && rooms[i] != 0;
        }
        check("each candidate has a pre-minted relay room, non-zero and distinct", nz && rooms[0] != rooms[1] && rooms[1] != rooms[2] && rooms[0] != rooms[2]);
        M::Epoch held;
        check("a client holds the SAME rooms the host published", cli(a, 2).mesh_held_epoch(held) && held.room[a.ids[1]] == h.room[a.ids[1]] && held.room[a.ids[0]] == rooms[0]);
        check("the matrix carries the legs", h.M.leg[a.ids[0]] == 600 && h.M.leg[a.ids[1]] == 200 && h.M.leg[a.ids[2]] == 900 && h.M.relay_ok == 1);
    }
    tear_down(a);
}

// ---- D4: the capability gate -------------------------------------------------------------------------------
void legacy_arm(int base) {
    printf("  -- D. the capability gate (a pre-mesh build -- the U59/U60 capability byte -- is refused at join)\n");
    {
        Arm a{};
        a.clients    = 1;
        a.base       = base;
        a.rx_timeout = -1;
        for (int i = 0; i <= 1; ++i) new (&g_e[i]) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base, (unsigned short)base, false, -1);
        fill_cfg(cc, 1, base, (unsigned short)(base + 1), false, -1);
        g_e[0].set_log(ep_log, nullptr);
        g_e[1].set_log(ep_log, nullptr);
        g_e[1].mesh_test_cap(1); // an old CLIENT: HELLO carries capability 1
        g_e[0].start(ch, TEST_PSK, true);
        g_e[1].start(cc, TEST_PSK, true);
        const bool refused = wait_for([&] { return g_e[0].osq_counters().legacy_refused > 0; }, 6000);
        checkf(refused, "a client whose HELLO carries capability 1 (a U59/U60 build) is refused by the host");
        Sleep(150);
        check("...and holds no seat", g_e[0].peer_count() == 0);
        g_e[1].stop();
        g_e[0].stop();
    }
    {
        for (int i = 0; i <= 1; ++i) new (&g_e[i]) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base + 2, (unsigned short)(base + 2), false, -1);
        fill_cfg(cc, 1, base + 2, (unsigned short)(base + 3), false, -1);
        g_e[0].set_log(ep_log, nullptr);
        g_e[1].set_log(ep_log, nullptr);
        g_e[0].mesh_test_cap(1); // an old HOST: WELCOME carries capability 1
        g_e[0].start(ch, TEST_PSK, true);
        g_e[1].start(cc, TEST_PSK, true);
        const bool refused = wait_for([&] { return g_e[1].osq_counters().legacy_refused > 0; }, 6000);
        checkf(refused, "a host whose WELCOME carries capability 1 is refused by the client");
        g_e[1].stop();
        g_e[0].stop();
    }
    {
        // mp:U63 -- a U61/U62 build (capability 2: the mesh and the handover, no crash failover) is refused too: it would sit
        // out a failover the others run (user decision Q5, same gate).
        for (int i = 0; i <= 1; ++i) new (&g_e[i]) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base + 6, (unsigned short)(base + 6), false, -1);
        fill_cfg(cc, 1, base + 6, (unsigned short)(base + 7), false, -1);
        g_e[0].set_log(ep_log, nullptr);
        g_e[1].set_log(ep_log, nullptr);
        g_e[1].mesh_test_cap(2);
        g_e[0].start(ch, TEST_PSK, true);
        g_e[1].start(cc, TEST_PSK, true);
        const bool refused = wait_for([&] { return g_e[0].osq_counters().legacy_refused > 0; }, 6000);
        checkf(refused, "a client whose HELLO carries capability 2 (a U61/U62 build) is refused by the host (mp:U63)");
        g_e[1].stop();
        g_e[0].stop();
    }
    {
        // control: the CURRENT capability on both sides joins (the gate is not simply shut)
        for (int i = 0; i <= 1; ++i) new (&g_e[i]) Endpoint();
        Config ch, cc;
        fill_cfg(ch, 0, base + 4, (unsigned short)(base + 4), false, -1);
        fill_cfg(cc, 1, base + 4, (unsigned short)(base + 5), false, -1);
        g_e[0].set_log(ep_log, nullptr);
        g_e[1].set_log(ep_log, nullptr);
        g_e[0].start(ch, TEST_PSK, true);
        g_e[1].start(cc, TEST_PSK, true);
        check("control: a current client and host join", wait_for([&] { return g_e[0].peer_count() == 1 && g_e[1].id_assigned(); }, 6000));
        g_e[1].stop();
        g_e[0].stop();
    }
}

} // namespace

// ---- U62: the planned handover -------------------------------------------------------------------------
// The hub leaves with `hub_leave`; the elected successor rehomes as hub, the others re-dial with their old ids,
// frames flow among the survivors through the new hub, the mesh publishes a new epoch without the departed host.
// mode 1 = the successor never answers (hl_test_mute): the hub re-issues the list without it and rank 2 takes over.
// mode 2 = MUTATION: hub_migration off -> hub_leave does nothing and the survivors' hub is simply gone.
void handover_arm(int clients, int base, int mode) {
    printf("  -- U62. planned handover, %d-endpoint mesh%s\n", clients + 1,
           mode == 1 ? " (successor mute: re-issue to rank 2)" : mode == 2 ? " (MUTATION: migration off)"
                                                                           : "");
    Arm a{};
    a.clients      = clients;
    a.base         = base;
    a.rx_timeout   = -1;
    g_no_migration = (mode == 2);
    check("endpoints started", bring_up(a));
    const bool acked = wait_for([&] { return admitted(a) && all_acked(a); }, 9000);
    check("the first epoch is acked", acked);
    if (!acked) {
        tear_down(a);
        g_no_migration = false;
        return;
    }
    uint32_t epoch0 = 0;
    int      winner = -1;
    check("one epoch everywhere", same_epoch(a, &epoch0, nullptr, &winner));
    const Endpoint::MeshStatus h0        = st(g_e[0]);
    const int                  first     = h0.rank[0];
    const int                  second    = h0.rank_n > 1 ? h0.rank[1] : -1;
    int                        first_idx = -1, second_idx = -1;
    for (int i = 0; i < clients; ++i) {
        if (a.ids[i] == first) first_idx = i;
        if (a.ids[i] == second) second_idx = i;
    }
    if (mode == 1 && first_idx >= 0) g_e[1 + first_idx].hl_test_mute(true);
    const int rc = g_e[0].hub_leave(mode == 1 ? 8000 : 5000);
    if (mode == 2) {
        checkf(rc == Endpoint::HL_NOTHING || rc == Endpoint::HL_TIMEOUT, "migration off: nothing handed over (rc %d)", rc);
        tear_down(a);
        g_no_migration = false;
        return;
    }
    g_no_migration = false;
    checkf(rc == Endpoint::HL_HANDED, "hub_leave reports HANDED (rc %d)", rc);
    const int succ_idx = mode == 1 ? second_idx : first_idx;
    const int dead_idx = mode == 1 ? first_idx : -1; // the muted "successor" is a gone process in this arm
    const int live     = clients - (mode == 1 ? 1 : 0);
    const int succ_id  = mode == 1 ? second : first;
    g_e[0].stop(); // the old host's process is gone
    const bool settled = wait_for(
        [&] {
            for (int i = 0; i < clients; ++i) {
                if (i == dead_idx) continue;
                Endpoint::HubStatus s;
                g_e[1 + i].hub_status(s);
                if (s.changes < 1 || s.hub_id != succ_id) return false;
            }
            return true;
        },
        12000);
    check("every survivor reports one hub change and the elected successor as the hub", settled);
    for (int i = 0; i < clients; ++i) {
        if (i == dead_idx) continue;
        Endpoint::HubStatus s;
        g_e[1 + i].hub_status(s);
        if (i == succ_idx) check("the successor is the hub (role 0, rehomed)", s.role == 0 && s.rehomed);
        else check("a survivor is a client of the new hub (role 1, rehomed)", s.role == 1 && s.rehomed && s.old_hub == 0 && s.new_hub == succ_id);
        const bool rdone = wait_for(
            [&] {
                Endpoint::HubStatus t;
                g_e[1 + i].hub_status(t);
                return t.reconcile_done && !t.unrecoverable && !t.aborted;
            },
            8000);
        check("the reconcile finished and nothing was unrecoverable", rdone);
    }
    unsigned char payload[8] = {0x31, 0, 0, 0, 1, 2, 3, 4};
    int           want = 0, got = 0;
    for (int i = 0; i < clients; ++i)
        for (int j = 0; j < clients; ++j) {
            if (i == j || i == dead_idx || j == dead_idx) continue;
            for (int k = 0; k < 10; ++k) {
                payload[1] = (uint8_t)k;
                if (g_e[1 + i].send(a.ids[j], payload, sizeof(payload)) > 0) ++want;
            }
        }
    wait_for(
        [&] {
            for (int i = 0; i < clients; ++i)
                for (;;) {
                    if (i == dead_idx) break;
                    int           sender = -1;
                    unsigned char buf[256];
                    int           len = (int)sizeof(buf);
                    if (!g_e[1 + i].recv(&sender, buf, &len)) break;
                    if (len == 8 && buf[0] == 0x31) ++got;
                }
            return got >= want;
        },
        8000);
    checkf(want > 0 && got >= want, "frames flow among the survivors after the handover (%d of %d)", got, want);
    const bool restarted = live < 3 || wait_for(
                                           [&] {
                                               const Endpoint::MeshStatus hs = st(g_e[1 + succ_idx]);
                                               if (hs.epoch <= epoch0 || (hs.S & 1u)) return false;
                                               for (int i = 0; i < clients; ++i) {
                                                   if (i == succ_idx || i == dead_idx) continue;
                                                   const Endpoint::MeshStatus c = st(g_e[1 + i]);
                                                   if (c.epoch != hs.epoch || c.digest != hs.digest) return false;
                                               }
                                               return true;
                                           },
                                           15000);
    // two survivors (one client of the new hub) have nobody to rank: no epoch is published (plan 6.5; M5 owns it)
    if (live >= 3) check("the new hub published a new succession epoch (> old) without the departed host, and every survivor holds it", restarted);
    for (int i = clients - 1; i >= 0; --i) g_e[1 + i].stop();
}

// ---- U63: the crash failover ------------------------------------------------------------------------------
// The pure rules first: the FO datagram codec, T_suspect, THE quorum function, the candidate walk.
void test_fo_pure() {
    printf("  -- U63. crash failover: codec, T_suspect, quorum, candidate walk\n");
    uint8_t b[M::FO_BODY];
    M::fo_body(M::PR_FO_STATE, 2, 3, 77, (uint8_t)(M::FOF_SUSPECT | M::FOF_REPLY), 9, 5, b);
    uint8_t  kind = 0, fl = 0, arg = 0;
    int      from = -1, to = -1;
    uint32_t seq = 0, ep = 0;
    check("a FO_STATE round-trips", M::fo_body_decode(b, M::FO_BODY, kind, from, to, seq, fl, ep, arg) && kind == M::PR_FO_STATE &&
                                        from == 2 && to == 3 && seq == 77 && fl == (M::FOF_SUSPECT | M::FOF_REPLY) && ep == 9 && arg == 5);
    bool all_refused = true;
    for (int n = 0; n < M::FO_BODY; ++n) all_refused &= !M::fo_body_decode(b, n, kind, from, to, seq, fl, ep, arg);
    uint8_t longer[M::FO_BODY + 1];
    memcpy(longer, b, M::FO_BODY);
    longer[M::FO_BODY] = 0;
    all_refused &= !M::fo_body_decode(longer, M::FO_BODY + 1, kind, from, to, seq, fl, ep, arg);
    check("every strict prefix and a trailing byte is refused", all_refused);
    uint8_t bad[M::FO_BODY];
    memcpy(bad, b, sizeof(bad));
    bad[0] = M::PR_PROBE;
    check("a probe kind is not a FO body", !M::fo_body_decode(bad, M::FO_BODY, kind, from, to, seq, fl, ep, arg));
    memcpy(bad, b, sizeof(bad));
    bad[1] = 9;
    check("a sender id past the table is refused", !M::fo_body_decode(bad, M::FO_BODY, kind, from, to, seq, fl, ep, arg));
    uint32_t nonce = 0;
    check("a 13-byte FO body is not a probe body", !M::probe_body_decode(b, M::FO_BODY, kind, from, to, nonce, fl));
    M::fo_body(M::PR_FO_REDIRECT, 1, 2, 3, 0, 4, 6, b);
    check("a REDIRECT round-trips", M::fo_body_decode(b, M::FO_BODY, kind, from, to, seq, fl, ep, arg) && kind == M::PR_FO_REDIRECT && arg == 6);

    check("T_suspect floor is 2 s", M::fo_suspect_ms(0, 0) == 2000 && M::fo_suspect_ms(100, 20) == 2000);
    check("T_suspect = 4 SRTT + 4 RTTVAR above the floor", M::fo_suspect_ms(900, 200) == 4400);
    check("T_suspect is bounded", M::fo_suspect_ms(1e9, 1e9) == 60000);

    // THE quorum rule (plan Q1): a strict majority of the seated humans of the epoch.
    auto q = [](std::initializer_list<int> members, std::initializer_list<int> group) { return M::quorum_ok(mask(members), mask(group)); };
    check("3 seated, 2 survivors together: quorum", q({0, 1, 2}, {1, 2}));
    check("3 seated, a lone survivor: no quorum", !q({0, 1, 2}, {1}));
    check("4 seated, 3 together: quorum", q({0, 1, 2, 3}, {1, 2, 3}));
    check("4 seated, an even split 2/2: NO quorum on either side (Q1)", !q({0, 1, 2, 3}, {1, 2}) && !q({0, 1, 2, 3}, {0, 3}));
    check("5 seated, 3 of 5 plays on and 2 of 5 ends", q({0, 1, 2, 3, 4}, {2, 3, 4}) && !q({0, 1, 2, 3, 4}, {1, 2}));
    check("peers outside the seated set do not count", !q({0, 1, 2, 3}, {1, 2, 6, 7}) && q({0, 1, 2}, {1, 2, 6, 7}));
    check("no members, no quorum", !M::quorum_ok(0, mask({1, 2})));
    M::Epoch e;
    e.S   = (uint8_t)mask({1, 2, 3});
    e.hub = 0;
    check("quorum_members = the epoch's clients plus its hub", M::quorum_members(e) == mask({0, 1, 2, 3}));
    check("a spectator does not count toward the quorum (U54 Q4)",
          M::quorum_members(e, mask({2})) == mask({0, 1, 3}) && M::quorum_ok(M::quorum_members(e, mask({2})), mask({1, 3})) &&
              !M::quorum_ok(M::quorum_members(e, mask({2, 3})), mask({1})));

    const uint8_t rank[3] = {2, 1, 3};
    check("the walk takes the first live candidate", M::fo_next_candidate(rank, 3, 0) == 2);
    check("...skips a failed/dead one", M::fo_next_candidate(rank, 3, mask({2})) == 1);
    check("...and answers none when everyone is skipped", M::fo_next_candidate(rank, 3, mask({1, 2, 3})) == -1);
    check("...an id outside the list is not a candidate", M::fo_next_candidate(rank, 3, mask({0})) == 2);
}

// The endpoints. The hub's PROCESS is gone (stop(): the socket closes, nothing is said). `mode`:
//   0  the hub crashes: every survivor reports ONE hub change to the same elected hub, within the stall budget, frames flow
//   1  the hub AND the first candidate crash together: the second candidate takes over (a candidate that never answers is skipped)
//   2  a survivor whose first choice is NOT the elected hub (fo_test_force_first): REDIRECT sends it on, and it follows the hub
//   3  the hub and one more survivor crash, leaving 2 of 4 seated humans: an even split, NO quorum -- both sides end (Q1)
//   4  MUTATION: `[net] hub_migration=0` -- nothing happens (U55's outcome)
//   5  the hub and the only other client crash: a lone survivor cannot corroborate, and the bounded budget ends it
//   7  the hub is ALIVE and one client's own receive path dies: it suspects, nobody corroborates, nothing happens to the match
//   8  the hub is ALIVE and a NON-hub client's process dies (the mp:U58 shape: a silent client, not a silent hub):
//      the other client and the hub never suspect anything, no election, no hub change
//   6  the hub crashes and the first candidate is MUTE (alive, becomes a hub in its own eyes, but no survivor can hear it):
//      the others skip it, the second candidate becomes THE hub, and the mute one -- a hub nobody joined -- ends: no split brain
void failover_arm(int clients, int base, int mode) {
    static const char *NM[] = {"crash", "hub + first candidate crash (the second takes over)", "REDIRECT of a survivor that dialled a non-elected candidate",
                               "minority (even split) ends", "MUTATION: hub_migration=0", "lone survivor: the budget ends it",
                               "a MUTE first candidate (up, but nobody hears it): the second takes over, the mute one ends",
                               "ONE survivor loses its own link, the hub is alive: no corroboration, no failover (a flaky link must not hijack a live hub)",
                               "a NON-hub client dies, the hub is alive: nobody suspects the hub, no failover"};
    printf("  -- U63. %d-endpoint mesh, %s\n", clients + 1, NM[mode]);
    Arm a{};
    a.clients      = clients;
    a.base         = base;
    a.rx_timeout   = -1;
    g_no_migration = (mode == 4);
    g_fo_budget    = (mode == 5) ? 6000 / kDiv : 0;
    for (auto &v : g_elected) v.clear();
    check("endpoints started", bring_up(a));
    for (int i = 0; i <= clients; ++i) g_e[i].set_in_match(true);
    const bool acked = wait_for([&] { return admitted(a) && all_acked(a); }, 9000);
    check("the first epoch is acked", acked);
    g_no_migration = false;
    g_fo_budget    = 0;
    if (!acked) {
        tear_down(a);
        return;
    }
    uint32_t epoch0 = 0;
    check("one epoch everywhere", same_epoch(a, &epoch0));
    const Endpoint::MeshStatus h0 = st(g_e[0]);
    int                        idx_of[8];
    for (int i = 0; i < 8; ++i) idx_of[i] = -1;
    for (int i = 0; i < clients; ++i) idx_of[a.ids[i] & 7] = i;
    const int first = h0.rank[0], second = h0.rank_n > 1 ? h0.rank[1] : -1, third = h0.rank_n > 2 ? h0.rank[2] : -1;
    const int fi = idx_of[first & 7], si = second >= 0 ? idx_of[second & 7] : -1, ti = third >= 0 ? idx_of[third & 7] : -1;
    if (mode == 2 && ti >= 0) g_e[1 + ti].fo_test_force_first(second);
    if (mode == 6) {
        g_e[1 + fi].fo_test_mute(true);
        g_e[1 + fi].mesh_test_no_probe(true);
    }
    uint32_t    dead   = 0; // survivors that are gone too
    const DWORD t_kill = GetTickCount();
    if (mode == 8) g_e[2].stop();                    // a client's process is gone: the hub and the other client stay up
    else if (mode == 7) g_e[1].set_rx_loss(1000, 7); // client 0's receive path is dead: it hears neither the hub nor anyone
    else g_e[0].stop();                              // the hub's process is gone: no LEAVING, no DISCONNECT -- silence
    if (mode == 1) {
        g_e[1 + fi].stop();
        dead |= 1u << fi;
    }
    if (mode == 3 && ti >= 0) {
        g_e[1 + ti].stop();
        dead |= 1u << ti;
    }
    if (mode == 5 && si >= 0) {
        g_e[1 + si].stop();
        dead |= 1u << si;
    }
    if (mode == 6) dead |= 1u << fi; // up, but not a survivor of this failover (its own phase is asserted below)
    auto      live   = [&](int i) { return !(dead >> i & 1u); };
    const int winner = (mode == 1 || mode == 6) ? second : first;
    const int widx   = (mode == 1 || mode == 6) ? si : fi;

    if (mode == 8) {
        Sleep(7000 / kDiv); // > 2 x T_suspect: a hub-loss suspicion on the remaining client would show by now
        bool                ok = true;
        Endpoint::HubStatus h;
        g_e[0].hub_status(h);
        ok &= h.role == 0 && h.changes == 0 && h.failover == Endpoint::FO_IDLE;
        check("the hub is alive, unchanged and not failing over", ok);
        Endpoint::HubStatus s;
        g_e[1].hub_status(s);
        checkf(s.failover == Endpoint::FO_IDLE && s.changes == 0 && s.role == 1,
               "the surviving client never suspected the (alive) hub (phase %d, changes %d)", s.failover, s.changes);
        tear_down(a);
        return;
    }
    if (mode == 7) {
        // (the kill above is skipped for this mode; the hub is alive -- see the guard on g_e[0].stop())
        Sleep(7000 / kDiv);
        bool                ok = true;
        Endpoint::HubStatus h;
        g_e[0].hub_status(h);
        ok &= h.role == 0 && h.changes == 0;
        check("the hub is alive and unchanged", ok);
        for (int i = 0; i < clients; ++i) {
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            if (i == 0) {
                checkf(s.failover == Endpoint::FO_SUSPECT && s.changes == 0 && s.role == 1,
                       "the deaf client suspects but is NOT corroborated: still SUSPECT, no election (phase %d)", s.failover);
            } else {
                checkf(s.failover == Endpoint::FO_IDLE && s.changes == 0 && s.role == 1, "survivor %d never entered a failover (phase %d)", i, s.failover);
            }
        }
        tear_down(a);
        return;
    }
    if (mode == 4) {
        Sleep(6500 / kDiv);
        bool quiet = true;
        for (int i = 0; i < clients; ++i) {
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            quiet &= s.failover == Endpoint::FO_IDLE && s.changes == 0 && s.role == 1;
        }
        check("migration off: no failover, no hub change on any survivor (U55's timeline)", quiet);
        tear_down(a);
        return;
    }
    if (mode == 3 || mode == 5) {
        const int  want  = mode == 3 ? Endpoint::FO_MINORITY : Endpoint::FO_FAILED;
        const bool ended = wait_for(
            [&] {
                for (int i = 0; i < clients; ++i) {
                    if (!live(i)) continue;
                    Endpoint::HubStatus s;
                    g_e[1 + i].hub_status(s);
                    if (s.failover != want) return false;
                }
                return true;
            },
            mode == 3 ? 12000 : 14000);
        checkf(ended, "every surviving side reports %s", mode == 3 ? "MINORITY (ends)" : "FAILED (budget)");
        for (int i = 0; i < clients; ++i) {
            if (!live(i)) continue;
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            checkf(!s.failover_active, "survivor %d: the failover is over (the game's timers resume)", i);
            checkf(s.changes == 0, "survivor %d: no hub change happened", i);
            if (mode == 3) checkf(s.fo_group == 2, "survivor %d counted a group of 2 (of 4 seated)", i);
        }
        tear_down(a);
        return;
    }

    // modes 0..2: a hub is elected and everyone follows it
    const bool settled = wait_for(
        [&] {
            for (int i = 0; i < clients; ++i) {
                if (!live(i)) continue;
                Endpoint::HubStatus s;
                g_e[1 + i].hub_status(s);
                if (s.changes < 1 || s.hub_id != winner || s.failover != Endpoint::FO_DONE) return false;
            }
            return true;
        },
        16000);
    const DWORD took = GetTickCount() - t_kill;
    checkf(settled, "every survivor is DONE with the same elected hub (player %d)", winner);
    checkf(took <= 6500, "the failover finished %u ms after the crash (<= 6.5 s: T_suspect 2 s + corroboration + dial)", (unsigned)took);
    printf("     failover took %u ms\n", (unsigned)took);
    for (int i = 0; i < clients; ++i) {
        if (!live(i)) continue;
        Endpoint::HubStatus s;
        g_e[1 + i].hub_status(s);
        if (i == widx) check("the elected hub is role 0 and rehomed", s.role == 0 && s.rehomed);
        else check("a survivor is a client of it, with the crash flag set", s.role == 1 && s.rehomed && s.change_crash && s.new_hub == winner);
        if (i != widx) checkf(s.old_hub == 0, "survivor %d records the crashed hub (player 0) as old (%d)", i, s.old_hub);
        // the SAME "hub elected <id> epoch <e>" line on every survivor
        int        id   = -1;
        unsigned   e    = 0;
        const bool have = !g_elected[i + 1].empty() && sscanf(strstr(g_elected[i + 1].back().c_str(), "hub elected "), "hub elected %d epoch %u", &id, &e) == 2;
        checkf(have && id == winner && e == epoch0, "survivor %d logged `hub elected %d epoch %u` (got %d / %u)", i, winner, (unsigned)epoch0, id, e);
        const bool rdone = wait_for(
            [&] {
                Endpoint::HubStatus t;
                g_e[1 + i].hub_status(t);
                return t.reconcile_done && !t.unrecoverable && !t.aborted;
            },
            8000);
        check("the reconcile finished and nothing was unrecoverable", rdone);
    }
    if (mode == 2) {
        Endpoint::HubStatus t3, t2;
        g_e[1 + ti].hub_status(t3);
        g_e[1 + si].hub_status(t2);
        checkf(t3.fo_redirects_rx >= 1 && t2.fo_redirects_tx >= 1, "the REDIRECT happened: the dialler got %ld, the non-elected candidate sent %ld",
               (long)t3.fo_redirects_rx, (long)t2.fo_redirects_tx);
        check("...and it followed the elected hub, not the candidate it dialled first", t3.hub_id == first && t3.role == 1);
    }
    if (mode == 6) {
        const bool ended = wait_for(
            [&] {
                Endpoint::HubStatus s;
                g_e[1 + fi].hub_status(s);
                return s.failover == Endpoint::FO_MINORITY;
            },
            12000);
        check("the MUTE first candidate -- a hub nobody joined -- ends as a minority (no split brain)", ended);
    }
    unsigned char payload[8] = {0x31, 0, 0, 0, 1, 2, 3, 4};
    int           want = 0, got = 0;
    for (int i = 0; i < clients; ++i)
        for (int j = 0; j < clients; ++j) {
            if (i == j || !live(i) || !live(j)) continue;
            for (int k = 0; k < 10; ++k) {
                payload[1] = (uint8_t)k;
                if (g_e[1 + i].send(a.ids[j], payload, sizeof(payload)) > 0) ++want;
            }
        }
    wait_for(
        [&] {
            for (int i = 0; i < clients; ++i)
                for (;;) {
                    if (!live(i)) break;
                    int           sender = -1;
                    unsigned char buf[256];
                    int           len = (int)sizeof(buf);
                    if (!g_e[1 + i].recv(&sender, buf, &len)) break;
                    if (len == 8 && buf[0] == 0x31) ++got;
                }
            return got >= want;
        },
        8000);
    checkf(want > 0 && got >= want, "frames flow among the survivors after the crash (%d of %d)", got, want);
    for (int i = clients - 1; i >= 0; --i) g_e[1 + i].stop();
}


// ---- U64 (HM-M6): the partition arms and the returning old host --------------------------------------------------------
// The test hook is Endpoint::set_rx_block_port: two endpoints that block each other's port are cut apart while both stay UP, which
// the crash arms above (stop()) cannot model. On loopback endpoint i listens on port `base + i`.
//   0  the LIVE hub is cut off from all three clients (a partition, not a crash): they corroborate each other, elect one hub and
//      play on (3 of 4); the old hub is untouched. Then the cut HEALS: the old hub's traffic (old connection ids) reaches the
//      survivors and disturbs nothing -- no second election, no hub change, frames still flow.
//   1  an EVEN split {hub, client 0} | {client 1, client 2}: the far side (2 of 4, no strict majority) ends as MINORITY on BOTH
//      of its members; the hub's side is untouched. (The hub never judges a quorum: what it still holds is its roster.)
//   2  the hub crashes, the survivors fail over, and the OLD HOST RELAUNCHES at the same address (a new process: new keys, no
//      conns): the survivors keep their hub, do not fail over again and keep exchanging frames.
//   3  as 2, but the relaunch happens at once, WHILE the survivors are still failing over.
void block_between(int base, int a, int b, bool on) {
    g_e[a].set_rx_block_port((unsigned short)(base + b), on);
    g_e[b].set_rx_block_port((unsigned short)(base + a), on);
}

// ten frames from every live endpoint to every other, all must arrive
int flow_frames(Arm &a, uint32_t dead, int *want_out) {
    unsigned char payload[8] = {0x31, 0, 0, 0, 1, 2, 3, 4};
    int           want = 0, got = 0;
    auto          live = [&](int i) { return !(dead >> i & 1u); };
    for (int i = 0; i < a.clients; ++i)
        for (int j = 0; j < a.clients; ++j) {
            if (i == j || !live(i) || !live(j)) continue;
            for (int k = 0; k < 10; ++k) {
                payload[1] = (uint8_t)k;
                if (g_e[1 + i].send(a.ids[j], payload, sizeof(payload)) > 0) ++want;
            }
        }
    wait_for(
        [&] {
            for (int i = 0; i < a.clients; ++i)
                for (;;) {
                    if (!live(i)) break;
                    int           sender = -1;
                    unsigned char buf[256];
                    int           len = (int)sizeof(buf);
                    if (!g_e[1 + i].recv(&sender, buf, &len)) break;
                    if (len == 8 && buf[0] == 0x31) ++got;
                }
            return got >= want;
        },
        8000);
    *want_out = want;
    return got;
}

void u64_arm(int clients, int base, int mode) {
    static const char *NM[] = {"a LIVE hub partitioned from all clients: they elect and play on, then the cut heals",
                               "an even split {hub, client} | {client, client}: the far side ends as MINORITY on both members",
                               "the old host RELAUNCHES at its address after the failover: the survivors are undisturbed",
                               "the old host relaunches at once, DURING the failover"};
    printf("  -- U64. %d-endpoint mesh, %s\n", clients + 1, NM[mode]);
    Arm a{};
    a.clients    = clients;
    a.base       = base;
    a.rx_timeout = -1;
    for (auto &v : g_elected) v.clear();
    check("endpoints started", bring_up(a));
    for (int i = 0; i <= clients; ++i) g_e[i].set_in_match(true);
    const bool acked = wait_for([&] { return admitted(a) && all_acked(a); }, 9000);
    check("the first epoch is acked", acked);
    if (!acked) {
        tear_down(a);
        return;
    }
    uint32_t epoch0 = 0;
    check("one epoch everywhere", same_epoch(a, &epoch0));
    const Endpoint::MeshStatus h0    = st(g_e[0]);
    const int                  first = h0.rank[0];
    if (mode == 0) {
        for (int i = 1; i <= clients; ++i) block_between(base, 0, i, true);
        const bool settled = wait_for(
            [&] {
                for (int i = 0; i < clients; ++i) {
                    Endpoint::HubStatus s;
                    g_e[1 + i].hub_status(s);
                    if (s.changes < 1 || s.hub_id != first || s.failover != Endpoint::FO_DONE) return false;
                }
                return true;
            },
            16000);
        checkf(settled, "the three clients are DONE with the same elected hub (player %d), the old hub still alive", first);
        Endpoint::HubStatus h;
        g_e[0].hub_status(h);
        check("the cut-off hub is untouched: still a hub, no hub change, no failover", h.role == 0 && h.changes == 0 && h.failover == Endpoint::FO_IDLE);
        int want = 0;
        int got  = flow_frames(a, 0, &want);
        checkf(want > 0 && got >= want, "frames flow among the three survivors during the partition (%d of %d)", got, want);
        for (int i = 1; i <= clients; ++i) block_between(base, 0, i, false);
        Sleep(5000 / kDiv); // the old hub's pings and data (old connection ids) now reach the survivors
        bool calm = true;
        for (int i = 0; i < clients; ++i) {
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            calm &= s.changes == 1 && s.hub_id == first && s.failover != Endpoint::FO_SUSPECT && s.failover != Endpoint::FO_ELECT;
            calm &= g_elected[i + 1].size() == 1;
        }
        check("after the heal: no survivor changed hub or failed over a second time", calm);
        got = flow_frames(a, 0, &want);
        checkf(want > 0 && got >= want, "...and frames still flow among the survivors (%d of %d)", got, want);
    } else if (mode == 1) {
        block_between(base, 0, 2, true);
        block_between(base, 0, 3, true);
        block_between(base, 1, 2, true);
        block_between(base, 1, 3, true);
        const bool ended = wait_for(
            [&] {
                for (int i = 1; i < clients; ++i) {
                    Endpoint::HubStatus s;
                    g_e[1 + i].hub_status(s);
                    if (s.failover != Endpoint::FO_MINORITY) return false;
                }
                return true;
            },
            14000);
        check("both members of the far side (2 of 4, no strict majority) report MINORITY", ended);
        for (int i = 1; i < clients; ++i) {
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            checkf(s.fo_group == 2 && s.changes == 0, "far-side member %d counted a group of 2 and announced no hub change (group %d, changes %d)", i, s.fo_group, s.changes);
        }
        Endpoint::HubStatus h, c0;
        g_e[0].hub_status(h);
        g_e[1].hub_status(c0);
        check("the hub's side is untouched: the hub still a hub, its client still a client, no failover", h.role == 0 && h.changes == 0 && c0.role == 1 && c0.changes == 0 && c0.failover == Endpoint::FO_IDLE);
    } else {
        g_e[0].stop(); // the hub's process is gone
        auto relaunch = [&] {
            new (&g_e[0]) Endpoint();
            g_e[0].set_log(ep_log, nullptr);
            Config ch;
            fill_cfg(ch, 0, base, (unsigned short)base, false, -1);
            return g_e[0].start(ch, TEST_PSK, true);
        };
        bool up = false;
        if (mode == 3) up = relaunch();
        const bool settled = wait_for(
            [&] {
                for (int i = 0; i < clients; ++i) {
                    Endpoint::HubStatus s;
                    g_e[1 + i].hub_status(s);
                    if (s.changes < 1 || s.hub_id != first || s.failover != Endpoint::FO_DONE) return false;
                }
                return true;
            },
            16000);
        checkf(settled, "every survivor is DONE with the elected hub (player %d)%s", first, mode == 3 ? ", although a new process listens at the old hub's address" : "");
        if (mode == 2) up = relaunch();
        check("the old host's address is listening again (a new process)", up);
        Sleep(5000 / kDiv);
        bool calm = true;
        for (int i = 0; i < clients; ++i) {
            Endpoint::HubStatus s;
            g_e[1 + i].hub_status(s);
            calm &= s.changes == 1 && s.hub_id == first && (s.failover == Endpoint::FO_DONE || s.failover == Endpoint::FO_IDLE); // DONE re-arms to IDLE after 3 s
            calm &= g_elected[i + 1].size() == 1;
        }
        check("the survivors kept their hub: no second election, no hub change", calm);
        check("the relaunched host holds no peer", g_e[0].peer_count() == 0);
        int       want = 0;
        const int got  = flow_frames(a, 0, &want);
        checkf(want > 0 && got >= want, "frames still flow among the survivors (%d of %d)", got, want);
    }
    for (int i = clients; i >= 0; --i) g_e[i].stop();
}

// mp:U64 -- STABLE rooms: the next epoch (a roster change) keeps the room of every candidate that stays, so survivors that hold e and
// e+1 when the hub dies dial the SAME room for the same candidate (U61 risk 3). A real watchdog (2.5 s) is needed to notice the leaver.
void rooms_stable_arm(int base) {
    printf("  -- U64. relayed epochs: a roster change keeps every remaining candidate's pre-minted room\n");
    Arm a{};
    a.clients    = 3;
    a.relay_sim  = true;
    a.base       = base;
    a.rx_timeout = 2500;
    g_leg_dms[1] = 600;
    g_leg_dms[2] = 200;
    g_leg_dms[3] = 900;
    g_leg_dms[0] = 0;
    for (int i = 0; i <= 3; ++i) g_leg_age[i] = 100;
    check("endpoints started", bring_up(a));
    const bool acked = wait_for([&] { return admitted(a) && all_acked(a); }, 12000);
    check("the first epoch is acked", acked);
    if (acked) {
        const Endpoint::MeshStatus h     = st(g_e[0]);
        const uint32_t             epoch = h.epoch, r0 = h.room[a.ids[0]], r1 = h.room[a.ids[1]];
        check("every candidate has a room", r0 != 0 && r1 != 0 && h.room[a.ids[2]] != 0);
        g_e[3].stop(); // the third client leaves; the hub's watchdog drops it and the roster changes
        const bool next = wait_for(
            [&] {
                const Endpoint::MeshStatus x = st(g_e[0]);
                return x.epoch > epoch && x.all_acked_ms != 0 && (x.acked_mask & x.S) == x.S;
            },
            20000);
        check("the roster change publishes the next epoch and it is acked", next);
        const Endpoint::MeshStatus h2 = st(g_e[0]);
        check("...whose remaining candidates keep the rooms they were given", h2.room[a.ids[0]] == r0 && h2.room[a.ids[1]] == r1);
    }
    tear_down(a);
}

// mp:U64 -- a hub that loses SEVERAL clients in the same watchdog pass (it is partitioned from all of them, or they all die) must
// report EVERY one through take_dead_peer(); the latch was one slot and the earlier ids were overwritten, so the game removed only the
// last and the sim stalled on a human nobody dropped (found by the 4-peer partition arm: the old hub took 57 s to end instead of 8).
void dead_queue_arm(int base) {
    printf("  -- U64. a hub that loses three clients at once reports all three dead peers\n");
    Arm a{};
    a.clients    = 3;
    a.base       = base;
    a.rx_timeout = 2000;
    check("endpoints started", bring_up(a));
    const bool acked = wait_for([&] { return admitted(a); }, 9000);
    check("three clients admitted", acked);
    if (acked) {
        for (int i = 3; i >= 1; --i) g_e[i].stop();
        uint32_t    seen = 0;
        const DWORD t0   = GetTickCount();
        while ((int)(GetTickCount() - t0) < 9000 && seen != 0x0e) {
            const int d = g_e[0].take_dead_peer();
            if (d >= 0 && d < 8) seen |= 1u << d;
            else Sleep(20);
        }
        checkf(seen == 0x0e, "take_dead_peer reported players 1, 2 and 3 (mask %02x)", (unsigned)seen);
    }
    tear_down(a);
}

// A RELAYED survivor whose own relay leg is dead is a group of one: it must report MINORITY (not run out the 20 s budget as FAILED).
// (The relay tunnel itself is a hook here, so only the verdict of the isolated side is asserted.)
void isolated_relay_arm(int base) {
    printf("  -- U64. relayed survivor with a dead relay leg: MINORITY, not a budget FAILED\n");
    Arm a{};
    a.clients    = 3;
    a.relay_sim  = true;
    a.base       = base;
    a.rx_timeout = -1;
    for (int i = 0; i <= 3; ++i) g_leg_age[i] = 100;
    check("endpoints started", bring_up(a));
    for (int i = 0; i <= a.clients; ++i) g_e[i].set_in_match(true);
    const bool acked = wait_for([&] { return admitted(a) && all_acked(a); }, 12000);
    check("the first epoch is acked", acked);
    if (acked) {
        g_leg_age[1]   = 60000; // client 0's relay leg has been silent for a minute
        const DWORD t0 = GetTickCount();
        g_e[0].stop();
        const bool ended = wait_for(
            [&] {
                Endpoint::HubStatus s;
                g_e[1].hub_status(s);
                return s.failover == Endpoint::FO_MINORITY;
            },
            12000);
        const DWORD took = GetTickCount() - t0;
        check("the isolated relayed survivor reports MINORITY", ended);
        checkf(took >= 5000 / (DWORD)kDiv && took <= 9500 / (DWORD)kDiv + 500, "...after the isolation window (%u ms: FO_ISOLATED_MS from the suspicion, far inside the 20 s budget)", (unsigned)took);
        Endpoint::HubStatus s;
        g_e[1].hub_status(s);
        check("...and it is no longer in a failover and announced no hub change", !s.failover_active && s.changes == 0);
    }
    g_leg_age[1] = 100;
    for (int i = a.clients; i >= 1; --i) g_e[i].stop();
}

// mp:U69 -- mesh_test_delay_ms is honoured only when the harness is armed.
static void test_delay_gate() {
    bool ig = true;
    check("delay gate: harness armed honours the delay", mh::netudp::mesh_test_delay_gate(40, true, &ig) == 40 && !ig);
    check("delay gate: no harness ignores it and reports it", mh::netudp::mesh_test_delay_gate(40, false, &ig) == 0 && ig);
    check("delay gate: unset is silent either way", mh::netudp::mesh_test_delay_gate(0, false, &ig) == 0 && !ig);
    check("delay gate: a negative value is never honoured", mh::netudp::mesh_test_delay_gate(-5, true, &ig) == 0 && !ig);
    check("delay gate: null ignored pointer is fine", mh::netudp::mesh_test_delay_gate(7, false, nullptr) == 0);
}

int run_meshtest() {
    printf("=== meshtest (mp:U61 / HM-M3: mesh probes, brokering, RTT matrix, succession epochs, elect()) ===\n");
    const DWORD t0 = GetTickCount();
    WSADATA     wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // MH_MESHTEST_ONLY=fo runs just the U63 failover arms (iterating on them: ~70 s instead of ~2 min).
    const char *only    = getenv("MH_MESHTEST_ONLY");
    const bool  fo_only = only && strcmp(only, "fo") == 0;
    // MH_MESHTEST_ONLY=u64 runs just the mp:U64 arms .
    const bool u64_only = only && strcmp(only, "u64") == 0;
    if (!fo_only && !u64_only) {
        test_delay_gate();
        test_elect_table();
        test_elect_fuzz();
        test_codecs();
        direct_arm(2, 40100);
        direct_arm(3, 40120);
        muted_arm(40140);
        relay_arm(40160);
        noack_arm(40180);
        legacy_arm(40200);
        handover_arm(2, 40220, 0);
        handover_arm(3, 40240, 0);
        handover_arm(3, 40260, 1);
        handover_arm(2, 40280, 2);
    }
    if (!u64_only) {
        test_fo_pure();
        failover_arm(2, 40300, 0);
        failover_arm(3, 40320, 0);
        failover_arm(4, 40340, 1); // 5 seated: hub + first candidate lost leaves 3 of 5, a majority
        failover_arm(3, 40360, 2);
        failover_arm(3, 40380, 3);
        failover_arm(2, 40400, 4);
        failover_arm(2, 40420, 5);
        failover_arm(4, 40440, 6);
        failover_arm(3, 40460, 7);
        failover_arm(2, 40480, 8); // mp:U58 shape: a silent NON-hub client must not read as hub loss
    }
    u64_arm(3, 40500, 0);      // mp:U64 -- the live hub partitioned from every client, then the heal
    u64_arm(3, 40520, 1);      // mp:U64 -- an even split: the far side ends as MINORITY on both members
    u64_arm(3, 40540, 2);      // mp:U64 -- the old host relaunches after the failover
    u64_arm(3, 40560, 3);      // mp:U64 -- ...and during it
    isolated_relay_arm(40580); // mp:U64 -- a relayed survivor with a dead relay leg
    dead_queue_arm(40620);     // mp:U64 -- three clients lost in one pass are all reported
    rooms_stable_arm(40600);   // mp:U64 -- relayed epochs keep each candidate's room
    printf("=== meshtest: %d checks, %d failures, %u ms ===\n", g_checks, g_fails, (unsigned)(GetTickCount() - t0));
    return g_fails ? 1 : 0;
}
