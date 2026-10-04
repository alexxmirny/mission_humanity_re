#pragma once
//
// mesh.h -- mp:U61 (HM-M3): the SOCKET-FREE half of host migration's election machinery.
//
// WHAT THIS IS. The host-migration plan (mp:U57) sections 3-5. While the host lives, every client pair is
// probed directly (mesh probes), each client reports its row of measured round trips to the host, the
// host merges the rows into an RTT matrix and publishes SUCCESSION EPOCHS (matrix + ranked successor
// list + dial info), and every client acks each epoch. This file holds everything in that machinery
// that is pure arithmetic or pure byte work, so `net_selftest.exe meshtest` can assert it without a
// socket or a clock:
//
//   * `elect()`      the election rule of plan section 4, a pure function shared by every peer.
//   * `Matrix`       the merged RTT matrix + the relay-leg row, and `digest()` over what elect() reads.
//   * the codecs     of the five FLAG_MESH control frames and of the probe datagram body.
//   * the keys       a per-match mesh key -> per-SENDER connection id + enc/mac keys for probes.
//
// The sockets, the clocks and the state machine are Endpoint's (udp_mesh.cpp).
//
// UNITS. A round trip is a u16 in TENTHS OF A MILLISECOND ("dms"): a LAN's 0.2 ms must not read as 0
// (unmeasured) or tie every pair at 1 ms, and 6553.4 ms is more than any link this transport keeps.
// 0xFFFF = NONE = unmeasured.
//
#ifndef MH_NET_UDP_MESH_H
#define MH_NET_UDP_MESH_H

#include <stdint.h>
#include <string.h>

#include "mh_net_proto/net_crypto.h"

namespace mh {
namespace netudp {
namespace mesh {

constexpr int      MAXP         = 8;      // == osq::ORIGINS == MH_NET_MAX_PEERS: player ids 0..7
constexpr uint16_t NONE         = 0xFFFF; // an unmeasured edge / leg
constexpr int      MAX_CANDS    = 4;      // addresses brokered per peer (the punch path's is 8; 4 bounds the probe fan-out)
constexpr int      MAX_FRAME    = 1024;   // every control frame here is far below the transport's 2048
constexpr uint8_t  FRAME_EXTRA  = 0;      // reserved

// ---- the five FLAG_MESH frame kinds (first payload byte) ---------------------------------------------
enum Kind : uint8_t {
    MK_CAND   = 1, // client -> host : my reachable candidates (+ am I relayed)
    MK_BROKER = 2, // host -> client : the mesh key + every OTHER client's candidates
    MK_ROW    = 3, // client -> host : my row of the RTT matrix + my relay-leg round trip
    MK_EPOCH  = 4, // host -> client : a SUCCESSION epoch
    MK_ACK    = 5, // client -> host : epoch + digest, "I hold it"
    // mp:U62 (HM-M4) -- the PLANNED HANDOVER of the hub (plan section 5.4).
    MK_LEAVING   = 6, // hub -> every client : "I am leaving; successor = the first listed id" + dial info
    MK_LEAVE_ACK = 7, // client -> leaving hub : "got it" (flag bit 0: and I am the successor, now the hub)
};

// ---- an address ---------------------------------------------------------------------------------------
struct Addr4 {
    uint8_t  ip[4]; // network order (the bytes of sockaddr_in::sin_addr)
    uint16_t port;  // HOST order
};
inline bool addr_eq(const Addr4 &a, const Addr4 &b) { return a.port == b.port && memcmp(a.ip, b.ip, 4) == 0; }

struct CandList {
    uint8_t n;
    Addr4   a[MAX_CANDS];
    CandList() : n(0) { memset(a, 0, sizeof(a)); }
    bool add(const Addr4 &x) {
        for (int i = 0; i < n; ++i)
            if (addr_eq(a[i], x)) return false;
        if (n >= MAX_CANDS) return false;
        a[n++] = x;
        return true;
    }
};

// ---- the matrix ---------------------------------------------------------------------------------------
// rtt[i][j] is what PLAYER i measured toward j (its own row), NONE when it has not (or its echoes went
// stale). leg[i] is player i's own round trip to the relay (NONE: not relayed / not measured).
struct Matrix {
    uint16_t rtt[MAXP][MAXP];
    uint16_t leg[MAXP];
    uint8_t  relay_ok; // a relay is configured for this match: the host dialled through it, so every peer reaches it
    Matrix() { clear(); }
    void clear() {
        for (int i = 0; i < MAXP; ++i) {
            leg[i] = NONE;
            for (int j = 0; j < MAXP; ++j) rtt[i][j] = NONE;
        }
        relay_ok = 0;
    }
};

inline int popcnt(uint32_t m) {
    int n = 0;
    while (m) {
        n += (int)(m & 1u);
        m >>= 1;
    }
    return n;
}

// The edge between two clients: the mean of the two directions when both were measured, the one that
// exists when only one was, NONE when neither. One direction is enough because a probe answered by an
// echo is a round trip: it proves BOTH halves of the path at that moment.
inline uint16_t edge(const Matrix &M, int i, int j) {
    if (i == j) return 0;
    const uint16_t a = M.rtt[i][j], b = M.rtt[j][i];
    if (a != NONE && b != NONE) return (uint16_t)(((uint32_t)a + b + 1) / 2);
    return a != NONE ? a : b;
}

// A matrix entry for REPORTING (coverage): the direct edge if measured, else the relayed estimate
// leg_i + leg_j when both legs are known and a relay carries the match, else NONE. The election itself
// does NOT read this -- it reads direct edges (tier 1) and legs (tier 2) separately, per plan section 4.
inline uint16_t pair_rtt(const Matrix &M, int i, int j) {
    const uint16_t e = edge(M, i, j);
    if (e != NONE) return e;
    if (M.relay_ok && M.leg[i] != NONE && M.leg[j] != NONE) {
        const uint32_t s = (uint32_t)M.leg[i] + M.leg[j];
        return (uint16_t)(s >= NONE ? NONE - 1 : s);
    }
    return NONE;
}

// How many of the C(|S|,2) pairs of S have an entry (pair_rtt != NONE).
inline void coverage(const Matrix &M, uint32_t S, int *have, int *total) {
    int h = 0, t = 0;
    for (int i = 0; i < MAXP; ++i)
        for (int j = i + 1; j < MAXP; ++j)
            if ((S >> i & 1u) && (S >> j & 1u)) {
                ++t;
                if (pair_rtt(M, i, j) != NONE) ++h;
            }
    if (have) *have = h;
    if (total) *total = t;
}

// FNV-1a 32 over exactly what elect() and coverage() read, for the members of S in ascending id order.
inline uint32_t digest(const Matrix &M, uint32_t S) {
    uint32_t h = 2166136261u;
    auto     mix = [&](uint32_t b) {
        h ^= (b & 0xffu);
        h *= 16777619u;
    };
    mix(S);
    mix(M.relay_ok);
    for (int i = 0; i < MAXP; ++i) {
        if (!(S >> i & 1u)) continue;
        mix((uint32_t)M.leg[i]);
        mix((uint32_t)M.leg[i] >> 8);
        for (int j = 0; j < MAXP; ++j) {
            if (!(S >> j & 1u)) continue;
            mix((uint32_t)M.rtt[i][j]);
            mix((uint32_t)M.rtt[i][j] >> 8);
        }
    }
    return h;
}

// ---- elect() ------------------------------------------------------------------------------------------
// Plan section 4. S is the candidate survivors (a bit per player id) and EVERY member is a candidate
// (the caller removes the dead hub from S first). The cost of candidate c is the worst relayed pair
// through c: top1 + top2 of { d(c,k) : k in S, k != c } (top1 alone with one other survivor, 0 with none).
//
//   tier 1  DIRECT  c has a measured edge to every other member of S. Ranked by cost, then by the SUM of
//                   d(c, .), then by lowest player id.
//   tier 2  RELAY   nobody is tier 1, a relay carries the match and EVERY member has a measured leg:
//                   d(i,c) = leg_i + leg_c, so cost is minimised by the shortest leg_c; ties by lowest id.
//   tier 3  ID      no usable measurement at all: the lowest player id (retail's own leader rule).
//
// When SOME but not all candidates are tier 1 the rest follow them, shortest known leg first then
// lowest id (a candidate some peer cannot probe is a poor hub, but it is still a candidate of last resort).
// `rank[0..n)` is the whole of S in preference order; the result depends on nothing but (M, S).
enum Tier : uint8_t { TIER_NONE = 0, TIER_DIRECT = 1, TIER_RELAY = 2, TIER_ID = 3 };

struct Election {
    int      n;           // |S|
    uint8_t  rank[MAXP];  // player ids, best first
    uint32_t cost[MAXP];  // per rank slot: tier-1 cost in dms (0xFFFFFFFF where not tier 1)
    uint32_t sum[MAXP];   // per rank slot: tier-1 sum in dms
    uint8_t  tier;        // the tier rank[0] was chosen in (TIER_NONE when S is empty)
    int      winner;      // rank[0], -1 when S is empty
};

inline void elect(const Matrix &M, uint32_t S, Election &out) {
    memset(&out, 0, sizeof(out));
    out.winner = -1;
    out.tier   = TIER_NONE;
    uint32_t cost[MAXP], sum[MAXP];
    bool     elig[MAXP];
    int      ids[MAXP], n = 0;
    for (int c = 0; c < MAXP; ++c) {
        cost[c] = 0xFFFFFFFFu;
        sum[c]  = 0xFFFFFFFFu;
        elig[c] = false;
        if (S >> c & 1u) ids[n++] = c;
    }
    out.n = n;
    if (n == 0) return;
    bool any = false, all_legs = M.relay_ok != 0;
    for (int a = 0; a < n; ++a) {
        const int c = ids[a];
        if (M.leg[c] == NONE) all_legs = false;
        bool     ok = true;
        uint32_t t1 = 0, t2 = 0, s = 0;
        for (int b = 0; b < n; ++b) {
            const int k = ids[b];
            if (k == c) continue;
            const uint16_t d = edge(M, c, k);
            if (d == NONE) {
                ok = false;
                break;
            }
            s += d;
            if (d >= t1) {
                t2 = t1;
                t1 = d;
            } else if (d > t2) {
                t2 = d;
            }
        }
        if (ok) {
            elig[c] = true;
            cost[c] = t1 + t2;
            sum[c]  = s;
            any     = true;
        }
    }
    // order: a simple insertion sort over at most eight ids with an explicit comparator.
    auto before = [&](int x, int y) -> bool { // true when x is preferred over y
        if (any) {
            if (elig[x] != elig[y]) return elig[x];
            if (elig[x]) {
                if (cost[x] != cost[y]) return cost[x] < cost[y];
                if (sum[x] != sum[y]) return sum[x] < sum[y];
                return x < y;
            }
            // both ineligible: shortest known leg first, then id
            const bool lx = M.relay_ok && M.leg[x] != NONE, ly = M.relay_ok && M.leg[y] != NONE;
            if (lx != ly) return lx;
            if (lx && M.leg[x] != M.leg[y]) return M.leg[x] < M.leg[y];
            return x < y;
        }
        if (all_legs) {
            if (M.leg[x] != M.leg[y]) return M.leg[x] < M.leg[y];
            return x < y;
        }
        return x < y;
    };
    for (int a = 1; a < n; ++a) {
        const int v = ids[a];
        int       b = a - 1;
        while (b >= 0 && before(v, ids[b])) {
            ids[b + 1] = ids[b];
            --b;
        }
        ids[b + 1] = v;
    }
    for (int a = 0; a < n; ++a) {
        out.rank[a] = (uint8_t)ids[a];
        out.cost[a] = elig[ids[a]] ? cost[ids[a]] : 0xFFFFFFFFu;
        out.sum[a]  = elig[ids[a]] ? sum[ids[a]] : 0xFFFFFFFFu;
    }
    out.winner = ids[0];
    out.tier   = any ? TIER_DIRECT : (all_legs ? TIER_RELAY : TIER_ID);
}

// ---- byte helpers -------------------------------------------------------------------------------------
inline void     put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void     put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
inline uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

// ---- candidate list: n(1) | n x { ip(4) port(2 LE) } ---------------------------------------------------
inline int cands_encode(const CandList &c, uint8_t *out, int cap) {
    const int need = 1 + 6 * c.n;
    if (cap < need) return 0;
    out[0] = c.n;
    for (int i = 0; i < c.n; ++i) {
        memcpy(out + 1 + 6 * i, c.a[i].ip, 4);
        put16(out + 5 + 6 * i, c.a[i].port);
    }
    return need;
}
// Returns bytes consumed, or 0 for a refusal (truncated, or more than MAX_CANDS -- never a shorter list).
inline int cands_decode(const uint8_t *in, int len, CandList &c) {
    c = CandList();
    if (len < 1 || in[0] > MAX_CANDS || len < 1 + 6 * in[0]) return 0;
    c.n = in[0];
    for (int i = 0; i < c.n; ++i) {
        memcpy(c.a[i].ip, in + 1 + 6 * i, 4);
        c.a[i].port = (uint16_t)get16(in + 5 + 6 * i);
    }
    return 1 + 6 * c.n;
}

// ---- MK_CAND:  kind | relayed(1) | cands ---------------------------------------------------------------
inline int cand_frame_encode(bool relayed, const CandList &c, uint8_t *out, int cap) {
    if (cap < 2) return 0;
    out[0]      = MK_CAND;
    out[1]      = relayed ? 1 : 0;
    const int k = cands_encode(c, out + 2, cap - 2);
    return k ? 2 + k : 0;
}
inline bool cand_frame_decode(const uint8_t *in, int len, bool &relayed, CandList &c) {
    if (len < 3 || in[0] != MK_CAND) return false;
    relayed = (in[1] & 1) != 0;
    const int k = cands_decode(in + 2, len - 2, c);
    return k != 0 && 2 + k == len;
}

// ---- MK_BROKER: kind | key(32) | n(1) | n x { id(1) relayed(1) cands } -----------------------------------
struct BrokerEntry {
    uint8_t  id;
    bool     relayed;
    CandList cands;
};
struct Broker {
    uint8_t     key[mh_net_proto::KEY_LEN];
    int         n;
    BrokerEntry e[MAXP];
};
inline int broker_encode(const Broker &b, uint8_t *out, int cap) {
    int o = 0;
    if (cap < 1 + (int)mh_net_proto::KEY_LEN + 1) return 0;
    out[o++] = MK_BROKER;
    memcpy(out + o, b.key, mh_net_proto::KEY_LEN);
    o += (int)mh_net_proto::KEY_LEN;
    out[o++] = (uint8_t)b.n;
    for (int i = 0; i < b.n; ++i) {
        if (cap - o < 2) return 0;
        out[o++]    = b.e[i].id;
        out[o++]    = b.e[i].relayed ? 1 : 0;
        const int k = cands_encode(b.e[i].cands, out + o, cap - o);
        if (!k) return 0;
        o += k;
    }
    return o;
}
inline bool broker_decode(const uint8_t *in, int len, Broker &b) {
    memset(&b, 0, sizeof(b));
    const int hdr = 1 + (int)mh_net_proto::KEY_LEN + 1;
    if (len < hdr || in[0] != MK_BROKER) return false;
    memcpy(b.key, in + 1, mh_net_proto::KEY_LEN);
    b.n = in[hdr - 1];
    if (b.n > MAXP) return false;
    int o = hdr;
    for (int i = 0; i < b.n; ++i) {
        if (len - o < 2) return false;
        b.e[i].id      = in[o++];
        b.e[i].relayed = (in[o++] & 1) != 0;
        if (b.e[i].id >= MAXP) return false;
        const int k = cands_decode(in + o, len - o, b.e[i].cands);
        if (!k) return false;
        o += k;
    }
    return o == len;
}

// ---- MK_ROW: kind | n(1) | n x { peer(1) rtt(2) } | leg(2) | relayed(1) -------------------------------
struct Row {
    int      n;
    uint8_t  peer[MAXP];
    uint16_t rtt[MAXP];
    uint16_t leg;
    bool     relayed;
};
inline int row_encode(const Row &r, uint8_t *out, int cap) {
    const int need = 2 + 3 * r.n + 3;
    if (cap < need) return 0;
    int o    = 0;
    out[o++] = MK_ROW;
    out[o++] = (uint8_t)r.n;
    for (int i = 0; i < r.n; ++i) {
        out[o++] = r.peer[i];
        put16(out + o, r.rtt[i]);
        o += 2;
    }
    put16(out + o, r.leg);
    o += 2;
    out[o++] = r.relayed ? 1 : 0;
    return o;
}
inline bool row_decode(const uint8_t *in, int len, Row &r) {
    memset(&r, 0, sizeof(r));
    if (len < 2 || in[0] != MK_ROW || in[1] > MAXP) return false;
    r.n = in[1];
    if (len != 2 + 3 * r.n + 3) return false;
    int o = 2;
    for (int i = 0; i < r.n; ++i) {
        r.peer[i] = in[o++];
        if (r.peer[i] >= MAXP) return false;
        r.rtt[i] = (uint16_t)get16(in + o);
        o += 2;
    }
    r.leg     = (uint16_t)get16(in + o);
    o += 2;
    r.relayed = (in[o] & 1) != 0;
    return true;
}

// ---- MK_EPOCH ---------------------------------------------------------------------------------------
//   kind | epoch(4) | digest(4) | hub(1) | S(1) | relay_ok(1) | tier(1) | n(1) | rank[n]
//        | for i in S: for j in S: rtt[i][j](2)  | for i in S: leg[i](2)
//        | for i in S: id(1) cands room(4)
struct Epoch {
    uint32_t epoch;
    uint32_t digest;
    uint8_t  hub;
    uint8_t  S;
    uint8_t  tier;
    int      n;
    uint8_t  rank[MAXP];
    Matrix   M;
    CandList dial[MAXP];
    uint32_t room[MAXP]; // pre-minted relay room per candidate, 0 = none (a direct match)
    Epoch() : epoch(0), digest(0), hub(0), S(0), tier(0), n(0) {
        memset(rank, 0, sizeof(rank));
        memset(room, 0, sizeof(room));
    }
};
inline int epoch_encode(const Epoch &e, uint8_t *out, int cap) {
    int o = 0;
    if (cap < 14 + MAXP) return 0;
    out[o++] = MK_EPOCH;
    put32(out + o, e.epoch);
    o += 4;
    put32(out + o, e.digest);
    o += 4;
    out[o++] = e.hub;
    out[o++] = e.S;
    out[o++] = e.M.relay_ok;
    out[o++] = e.tier;
    out[o++] = (uint8_t)e.n;
    for (int i = 0; i < e.n; ++i) out[o++] = e.rank[i];
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        for (int j = 0; j < MAXP; ++j) {
            if (!(e.S >> j & 1u)) continue;
            if (cap - o < 2) return 0;
            put16(out + o, e.M.rtt[i][j]);
            o += 2;
        }
    }
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        if (cap - o < 2) return 0;
        put16(out + o, e.M.leg[i]);
        o += 2;
    }
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        if (cap - o < 1) return 0;
        out[o++]    = (uint8_t)i;
        const int k = cands_encode(e.dial[i], out + o, cap - o);
        if (!k || cap - o - k < 4) return 0;
        o += k;
        put32(out + o, e.room[i]);
        o += 4;
    }
    return o;
}
inline bool epoch_decode(const uint8_t *in, int len, Epoch &e) {
    e = Epoch();
    if (len < 14 || in[0] != MK_EPOCH) return false;
    int o    = 1;
    e.epoch  = get32(in + o);
    o += 4;
    e.digest = get32(in + o);
    o += 4;
    e.hub        = in[o++];
    e.S          = in[o++];
    e.M.relay_ok = in[o++] & 1;
    e.tier       = in[o++];
    e.n          = in[o++];
    if (e.n > MAXP || e.hub >= MAXP || len - o < e.n) return false;
    for (int i = 0; i < e.n; ++i) {
        e.rank[i] = in[o++];
        if (e.rank[i] >= MAXP || !(e.S >> e.rank[i] & 1u)) return false;
    }
    if (e.n != popcnt(e.S)) return false;
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        for (int j = 0; j < MAXP; ++j) {
            if (!(e.S >> j & 1u)) continue;
            if (len - o < 2) return false;
            e.M.rtt[i][j] = (uint16_t)get16(in + o);
            o += 2;
        }
    }
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        if (len - o < 2) return false;
        e.M.leg[i] = (uint16_t)get16(in + o);
        o += 2;
    }
    for (int i = 0; i < MAXP; ++i) {
        if (!(e.S >> i & 1u)) continue;
        if (len - o < 1 || in[o] != (uint8_t)i) return false;
        ++o;
        const int k = cands_decode(in + o, len - o, e.dial[i]);
        if (!k || len - o - k < 4) return false;
        o += k;
        e.room[i] = get32(in + o);
        o += 4;
    }
    return o == len;
}

// ---- MK_ACK: kind | epoch(4) | digest(4) ----------------------------------------------------------------
constexpr int ACK_BYTES = 9;
inline void ack_encode(uint32_t epoch, uint32_t dig, uint8_t out[ACK_BYTES]) {
    out[0] = MK_ACK;
    put32(out + 1, epoch);
    put32(out + 5, dig);
}
inline bool ack_decode(const uint8_t *in, int len, uint32_t &epoch, uint32_t &dig) {
    if (len != ACK_BYTES || in[0] != MK_ACK) return false;
    epoch = get32(in + 1);
    dig   = get32(in + 5);
    return true;
}

// ---- MK_LEAVING / MK_LEAVE_ACK (mp:U62) -----------------------------------------------------------------
//
// MK_LEAVING:  kind | epoch(4) | gen(1) | hub(1) | n(1) | n x { id(1) relayed(1) cands room(4) }
//
// `n` lists EVERY surviving client in successor-preference order, so the receiver needs nothing it was not
// just handed: the first id is the successor, the rest are the roster the successor will seat, and each
// entry carries the dial info for that id (the address the leaving hub observed it at, then what it
// reported; the PRE-MINTED relay room of the held epoch in a relayed match). `gen` counts the hub's
// re-issues: the hub re-sends with the silent successor removed (generation + 1) when the first successor
// never acknowledges, and a survivor acts on the highest generation it has seen. `epoch` is the succession
// epoch the ranking came from (0 when none was published yet -- the list is then the lowest-id fallback);
// the survivors' next epoch is numbered above it (see Endpoint::hl_become_hub).
//
// MK_LEAVE_ACK:  kind | epoch(4) | gen(1) | flags(1)    bit 0 = "I am the successor and have switched role"
struct LeaveEntry {
    uint8_t  id;
    bool     relayed;
    CandList cands;
    uint32_t room;
};
struct Leaving {
    uint32_t   epoch;
    uint8_t    gen;
    uint8_t    hub;
    int        n;
    LeaveEntry e[MAXP];
    Leaving() : epoch(0), gen(0), hub(0), n(0) { memset(e, 0, sizeof(e)); }
};
inline int leaving_encode(const Leaving &l, uint8_t *out, int cap) {
    if (cap < 8 || l.n < 1 || l.n > MAXP) return 0;
    int o    = 0;
    out[o++] = MK_LEAVING;
    put32(out + o, l.epoch);
    o += 4;
    out[o++] = l.gen;
    out[o++] = l.hub;
    out[o++] = (uint8_t)l.n;
    for (int i = 0; i < l.n; ++i) {
        if (cap - o < 2) return 0;
        out[o++]    = l.e[i].id;
        out[o++]    = l.e[i].relayed ? 1 : 0;
        const int k = cands_encode(l.e[i].cands, out + o, cap - o);
        if (!k || cap - o - k < 4) return 0;
        o += k;
        put32(out + o, l.e[i].room);
        o += 4;
    }
    return o;
}
inline bool leaving_decode(const uint8_t *in, int len, Leaving &l) {
    l = Leaving();
    if (len < 8 || in[0] != MK_LEAVING) return false;
    int o   = 1;
    l.epoch = get32(in + o);
    o += 4;
    l.gen = in[o++];
    l.hub = in[o++];
    l.n   = in[o++];
    if (l.n < 1 || l.n > MAXP || l.hub >= MAXP) return false;
    for (int i = 0; i < l.n; ++i) {
        if (len - o < 2) return false;
        l.e[i].id      = in[o++];
        l.e[i].relayed = (in[o++] & 1) != 0;
        if (l.e[i].id >= MAXP || l.e[i].id == l.hub) return false;
        for (int j = 0; j < i; ++j)
            if (l.e[j].id == l.e[i].id) return false; // a duplicate id is a corrupt list, never a shorter one
        const int k = cands_decode(in + o, len - o, l.e[i].cands);
        if (!k || len - o - k < 4) return false;
        o += k;
        l.e[i].room = get32(in + o);
        o += 4;
    }
    return o == len;
}
constexpr int     LEAVE_ACK_BYTES = 7;
constexpr uint8_t LA_SUCCESSOR    = 1;
inline void leave_ack_encode(uint32_t epoch, uint8_t gen, uint8_t flags, uint8_t out[LEAVE_ACK_BYTES]) {
    out[0] = MK_LEAVE_ACK;
    put32(out + 1, epoch);
    out[5] = gen;
    out[6] = flags;
}
inline bool leave_ack_decode(const uint8_t *in, int len, uint32_t &epoch, uint8_t &gen, uint8_t &flags) {
    if (len != LEAVE_ACK_BYTES || in[0] != MK_LEAVE_ACK) return false;
    epoch = get32(in + 1);
    gen   = in[5];
    flags = in[6];
    return true;
}

// The epoch number a new hub's FIRST epoch carries. The old hub publishes e+1 only after every client acked
// e, so any survivor holds `its own held epoch` or one above it: numbering the new hub's first epoch two
// above the NEW HUB's held one is above everything any survivor holds (plan section 5.1, "never further
// apart"), so a survivor that already holds e+1 never sees the epoch number go backwards.
inline uint32_t successor_first_epoch(uint32_t held_epoch) { return held_epoch + 2; }

// ---- the probe datagram ---------------------------------------------------------------------------------
//
// A probe is an ordinary T0 datagram (PKT_DATA, the sealed packet format) so the relay's and the
// fixtures' decoders are untouched: nothing new on the packet-type axis. What marks it as a mesh probe
// is its connection id, which is derived from the per-match MESH KEY the host mints and hands each
// client over that client's encrypted stream (never derived from the deployment PSK alone -- R1c's
// lesson). The keys are per SENDER, not per pair, so one sequence counter per sender is the whole nonce
// discipline and a receiver keeps one replay window per sender:
//
//     cid(s) = HMAC(key, "mh-mesh-cid" | s)[0..8)    enc(s) = HMAC(key, "mh-mesh-enc" | s)
//                                                    mac(s) = HMAC(key, "mh-mesh-mac" | s)
//
// body: kind(1) from(1) to(1) nonce(4) flags(1) -- 8 bytes. An ECHO repeats the probe's nonce and is
// sealed under the ECHOER's keys. `flags` bit 0 is reserved for M5 ("I think the hub is silent").
constexpr uint8_t PR_PROBE = 1, PR_ECHO = 2;
constexpr int     PROBE_BODY = 8;
// mp:U63 (HM-M5) -- the CRASH-FAILOVER datagrams, sealed exactly like a probe (the per-sender mesh keys) but with a
// longer body, so they cannot be mistaken for one (probe_body_decode wants len == 8):
//   PR_FO_STATE    "this is what I think": flags (FOF_*), my held succession epoch, and `arg` = the candidate I
//                  will dial / become (0xFF = none yet). Sent every FO_STATE_MS by a peer whose hub is silent, and as
//                  the answer to one received (so a peer that has not noticed yet is heard, and a dead one is silent).
//   PR_FO_REDIRECT "not me -- ask `arg` instead": the answer of a non-hub candidate that can still reach a
//                  better-ranked candidate (plan 5.3 rule 2), or that already follows a hub.
constexpr uint8_t PR_FO_STATE = 3, PR_FO_REDIRECT = 4;
constexpr int     FO_BODY    = 13;
constexpr uint8_t FOF_SUSPECT = 1; // "my hub has been silent for T_suspect"
constexpr uint8_t FOF_HUB     = 2; // "I am the hub now" (rehomed as the hub)
constexpr uint8_t FOF_REPLY   = 4; // an answer to a FO_STATE: never answered in turn (no ping-pong)
constexpr uint8_t FO_NONE_ARG = 0xFF;

struct ProbeKeys {
    uint8_t cid[8];
    uint8_t enc[mh_net_proto::KEY_LEN];
    uint8_t mac[mh_net_proto::KEY_LEN];
};
inline void probe_keys(const uint8_t key[mh_net_proto::KEY_LEN], int sender, ProbeKeys &k) {
    static const char *L[3] = {"mh-mesh-cid", "mh-mesh-enc", "mh-mesh-mac"};
    uint8_t            full[3][mh_net_proto::SHA256_LEN];
    for (int t = 0; t < 3; ++t) {
        uint8_t msg[16];
        int     n = 0;
        while (L[t][n]) {
            msg[n] = (uint8_t)L[t][n];
            ++n;
        }
        msg[n++] = (uint8_t)sender;
        mh_net_proto::hmac_sha256(key, mh_net_proto::KEY_LEN, msg, (size_t)n, full[t]);
    }
    memcpy(k.cid, full[0], 8);
    memcpy(k.enc, full[1], mh_net_proto::KEY_LEN);
    memcpy(k.mac, full[2], mh_net_proto::KEY_LEN);
}
inline void probe_body(uint8_t kind, int from, int to, uint32_t nonce, uint8_t flags, uint8_t out[PROBE_BODY]) {
    out[0] = kind;
    out[1] = (uint8_t)from;
    out[2] = (uint8_t)to;
    put32(out + 3, nonce);
    out[7] = flags;
}
inline bool probe_body_decode(const uint8_t *in, int len, uint8_t &kind, int &from, int &to, uint32_t &nonce,
                              uint8_t &flags) {
    if (len != PROBE_BODY || (in[0] != PR_PROBE && in[0] != PR_ECHO) || in[1] >= MAXP || in[2] >= MAXP) return false;
    kind  = in[0];
    from  = in[1];
    to    = in[2];
    nonce = get32(in + 3);
    flags = in[7];
    return true;
}

// ---- mp:U63 (HM-M5): crash failover -- the pure rules ------------------------------------------------------
//
// body: kind(1) from(1) to(1) seq(4) flags(1) epoch(4) arg(1) -- 13 bytes, PR_FO_STATE or PR_FO_REDIRECT.
inline void fo_body(uint8_t kind, int from, int to, uint32_t seq, uint8_t flags, uint32_t epoch, uint8_t arg,
                    uint8_t out[FO_BODY]) {
    out[0] = kind;
    out[1] = (uint8_t)from;
    out[2] = (uint8_t)to;
    put32(out + 3, seq);
    out[7] = flags;
    put32(out + 8, epoch);
    out[12] = arg;
}
inline bool fo_body_decode(const uint8_t *in, int len, uint8_t &kind, int &from, int &to, uint32_t &seq,
                           uint8_t &flags, uint32_t &epoch, uint8_t &arg) {
    if (len != FO_BODY || (in[0] != PR_FO_STATE && in[0] != PR_FO_REDIRECT) || in[1] >= MAXP || in[2] >= MAXP)
        return false;
    kind  = in[0];
    from  = in[1];
    to    = in[2];
    seq   = get32(in + 3);
    flags = in[7];
    epoch = get32(in + 8);
    arg   = in[12];
    return true;
}

// Timing (plan section 2 and 5.2; every figure is [U] until the rig has said otherwise).
constexpr uint32_t FO_SUSPECT_MIN_MS = 2000; // T_suspect's floor: two missed 1 Hz pings
constexpr uint32_t FO_CORROB_MS      = 1000; // a peer that already hears "hub silent" from another survivor suspects at this much silence
constexpr uint32_t FO_STATE_MS       = 250;  // FO_STATE cadence while a failover runs
constexpr uint32_t FO_FRESH_MS       = 1500; // a FO_STATE / probe / echo younger than this proves the peer is alive and reachable
constexpr uint32_t FO_DEAD_MS        = 1000; // a candidate silent this long after corroboration is dead TO ME (four unanswered FO_STATEs)
constexpr uint32_t FO_REDIAL_MS      = 1500; // how long one candidate is dialled (split over its addresses) before the next
constexpr uint32_t FO_QUORUM_MS      = 6000; // the new hub waits this long for its roster to re-dial before it judges the quorum
constexpr uint32_t FO_ISOLATED_MS    = 5000; // mp:U64: a RELAYED survivor whose own relay leg has been dead this long is alone (a group of 1)
constexpr uint32_t FO_BUDGET_MS      = 20000; // the whole failover is bounded: past this a true partition ends (U55's path)

// T_suspect = max(2000 ms, 4 x SRTT + 4 x RTTVAR) of the hub link (plan section 2).
inline uint32_t fo_suspect_ms(double srtt_ms, double rttvar_ms) {
    const double t = 4.0 * srtt_ms + 4.0 * rttvar_ms;
    const uint32_t v = t > 60000.0 ? 60000u : (t < 0.0 ? 0u : (uint32_t)t);
    return v > FO_SUSPECT_MIN_MS ? v : FO_SUSPECT_MIN_MS;
}

// THE QUORUM RULE -- the ONE place that says who counts (user decision Q1: only a strict-majority group plays on).
//
// `quorum_members` is the set the majority is measured against: the SEATED, CONNECTED HUMAN peers at the last
// succession epoch (every client in the epoch plus the hub that published it) MINUS the SPECTATORS. User decision
// (Q4 of U54, 2026-10-03): a defeated player who keeps watching does NOT count toward the majority. The transport has
// no notion of defeat; the spectator mask (bit per player id) is set through Endpoint::set_spectator, which U54's
// spectate flow will drive (default: never set, so today every seated human counts). This is the only function that
// decides who counts.
inline uint32_t quorum_members(const Epoch &e, uint32_t spectators = 0) {
    return ((uint32_t)e.S | (1u << (e.hub & 7))) & ~spectators;
}
// A group plays on only when it holds MORE THAN HALF of the members; an even split ends on both sides.
inline bool quorum_ok(uint32_t members, uint32_t group) {
    const int total = popcnt(members);
    const int have  = popcnt(members & group);
    return total > 0 && have * 2 > total;
}

// The candidate a peer dials (or becomes): the first id of `rank[0..n)` that is not in `skip`. -1 when every
// candidate is skipped. A pure function of the epoch's ranking and two masks, so the failover's order is the
// same on every peer that holds the same epoch and agrees on who is dead.
inline int fo_next_candidate(const uint8_t *rank, int n, uint32_t skip) {
    for (int i = 0; i < n; ++i)
        if (!(skip >> (rank[i] & 7) & 1u)) return rank[i];
    return -1;
}

// ---- when to publish a new epoch (plan section 4, "Hysteresis") ---------------------------------------
// Pure, so the selftest can drive it. `cur_cost`/`new_cost` are tier-1 costs in dms of the CURRENT
// epoch's winner under the CURRENT matrix and of the new winner; a switch must beat the incumbent by
// more than 20 % AND more than 10 ms (100 dms).
constexpr uint32_t REELECT_MS    = 10000; // how often the host re-evaluates
constexpr uint32_t MIN_PUBLISH_MS = 30000; // at most one hysteresis-driven epoch per this
inline bool beats_incumbent(uint32_t cur_cost, uint32_t new_cost) {
    if (cur_cost == 0xFFFFFFFFu) return new_cost != 0xFFFFFFFFu; // the incumbent became unusable
    if (new_cost >= cur_cost) return false;
    const uint32_t gain = cur_cost - new_cost;
    return gain > 100u && (uint64_t)gain * 5u > (uint64_t)cur_cost; // > 10 ms and > 20 %
}

} // namespace mesh
} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_MESH_H
