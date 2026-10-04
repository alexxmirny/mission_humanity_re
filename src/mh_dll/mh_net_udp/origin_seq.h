#pragma once
//
// origin_seq.h -- THE END-TO-END EXACTLY-ONCE LAYER FOR BROADCAST GAME FRAMES (mp:U59, HM-M1).
//
// Design: the host-migration plan (mp:U57), section 7.2-7.3. The foundation of host migration: frames in
// flight through a dead hub must reach every survivor exactly once, or the survivors hold different
// parts of the dead host's orders and desync. Per-connection reliable streams cannot repair that --
// the connection is the thing that died -- so the guarantee has to be END TO END.
//
// WHAT IT IS. Pure bookkeeping over bytes, no I/O, no windows.h, no allocation (the whole thing is
// one POD, valid when zero-filled -- Endpoint's constructor memsets it -- and reset() arms it). The
// Endpoint (udp_endpoint.cpp) owns one and is its only production caller; selftests drive it bare.
// Time is a parameter (a millisecond tick, wrap-safe), never read here.
//
//   * Every broadcast FLAG_DATA frame an endpoint ORIGINATES carries a 7-byte prefix at the head of
//     its payload: magic(1) | incarnation(2 LE) | seq(4 LE). `seq` counts per ORIGIN (the WireHdr
//     `src`, which a hub forwards untouched) from 1. seq 0 = an unsequenced frame (a unicast: the
//     census, docs section 7.2, found none in a match, only lobby traffic).
//   * Every peer DEDUPES by (origin, seq) and DELIVERS per origin IN ORDER. A frame ahead of the
//     frontier waits in a small bounded buffer for the hole to be filled by `rx` (reconcile) rather
//     than being delivered over it.
//   * Every peer RETAINS the frames it sent or received per origin in a byte ring, for WINDOW_MS
//     (30 s) and at most RING_POOL bytes / RING_ENTRIES frames per origin. Consecutive bare horizon
//     adverts COALESCE in the ring into one entry covering a seq range (they are latest-wins; the
//     inbound queue already evicts superseded ones), which is what bounds memory at the 1100-3500
//     adverts/s a stalled peer produces (udp wire doc, mp:T4b).
//   * RECONCILE is a pure primitive: every peer publishes a Report (per origin: highest contiguous
//     seq held, lowest seq still retained, incarnation); `plan()` turns the survivors' reports into
//     fetches so each reaches the MAX frontier any survivor holds; a holder `serve()`s the ranges and
//     the fetcher feeds them to `rx`, which dedupes. A needed seq below a holder's retention is
//     reported as UNRECOVERABLE -- the caller ends the match, it never guesses (plan section 7.2).
//
// WHY INCARNATION. A peer that leaves and rejoins (a lobby relink) restarts its seq at 1 while the
// host still holds `next = 57` for it; without a restart marker its new frames would all read as
// duplicates. A DIFFERENT incarnation from an origin resets that origin's state. It is a per-start()
// tag, not an ordering, so a restarted peer can never be "older".
//
// WHY A GAP TIMEOUT. In the star a hole cannot happen: each hop is a reliable ordered stream. One can
// still appear across a conn re-establishment that did not restart the sender. A hole that no
// reconcile fills within `gap_force_ms` is SKIPPED and counted loudly (gap_skipped) rather than
// stalling every later frame of that origin for good. Normal play must show zero; M2 sets it to 0
// (never skip) while a migration is in progress.
//
// WIRE VERSIONING (user decision Q5: refuse an old peer). PREFIX_BYTES + CAP_VERSION are the
// capability: the client's HELLO and the host's WELCOME carry the CAP_VERSION byte, and a DATA frame
// without MAGIC is a pre-layer build. The endpoint refuses such a peer at the transport.
//
#ifndef MH_NET_UDP_ORIGIN_SEQ_H
#define MH_NET_UDP_ORIGIN_SEQ_H

#include <stdint.h>
#include <string.h>

namespace mh {
namespace netudp {
namespace osq {

constexpr int      ORIGINS      = 8;       // == MH_NET_MAX_PEERS == the lobby's eight slots
constexpr uint8_t  MAGIC        = 0xC3;    // no lobby/lockstep message type starts with it (<= 0x2x)
constexpr int      PREFIX_BYTES = 7;       // magic | incarnation(2) | seq(4)
constexpr uint8_t  CAP_VERSION  = 3;       // the HELLO / WELCOME payload byte (1 = mp:U59 exactly-once layer, 2 = mp:U61 host-migration mesh: a peer that cannot be brokered/probed/ranked is refused, Q5; 3 = mp:U63 crash failover: a peer that cannot speak FO_STATE/REDIRECT would sit out a failover the others run, so it is refused too)
constexpr int      PAYLOAD_MAX  = 2048;    // == MH_NET_MAX_PAYLOAD (static_assert in udp_endpoint.cpp)
constexpr uint32_t WINDOW_MS    = 30000;   // retention window (plan section 7.2)
constexpr uint32_t RING_POOL    = 128u * 1024u; // bytes of retained payload, PER ORIGIN
constexpr int      RING_ENTRIES = 2048;    // retained frames (or coalesced ranges), PER ORIGIN
constexpr int      AHEAD_SLOTS  = 16;      // frames waiting above a hole, PER ORIGIN
constexpr uint32_t GAP_FORCE_MS = 5000;    // default; 0 = never skip a hole

// ---- the prefix ---------------------------------------------------------------------------------
inline void prefix_encode(uint8_t *out, uint16_t inc, uint32_t seq) {
    out[0] = MAGIC;
    out[1] = (uint8_t)(inc & 0xff);
    out[2] = (uint8_t)(inc >> 8);
    out[3] = (uint8_t)(seq & 0xff);
    out[4] = (uint8_t)((seq >> 8) & 0xff);
    out[5] = (uint8_t)((seq >> 16) & 0xff);
    out[6] = (uint8_t)((seq >> 24) & 0xff);
}
// False when the payload is too short or does not open with MAGIC (a pre-layer peer).
inline bool prefix_decode(const uint8_t *p, int len, uint16_t &inc, uint32_t &seq) {
    if (p == nullptr || len < PREFIX_BYTES || p[0] != MAGIC) return false;
    inc = (uint16_t)(p[1] | (p[2] << 8));
    seq = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 24);
    return true;
}

// Wrap-safe "a is older than b" over a millisecond tick.
inline bool tick_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

// ---- the retention ring: one origin's recent frames, bounded in bytes AND entries ---------------
//
// Entries are contiguous in seq (lo of each = hi of the previous + 1) because only frames that were
// DELIVERED (or sent) in order are added. The payload bytes live in `pool`, a circular byte arena
// allocated front to back; when a payload does not fit at the tail it wraps to offset 0 (the unused
// tail is simply skipped). The oldest entries are evicted until the new one fits.
struct Ring {
    struct Ent {
        uint32_t lo, hi; // the seq range this entry stands for (lo == hi unless coalesced)
        uint32_t t;      // tick of the newest frame in it
        uint32_t off;
        uint16_t len;
        uint8_t  sup; // bare horizon advert: eligible to coalesce with its neighbour
    };
    Ent      ent[RING_ENTRIES];
    uint8_t  pool[RING_POOL];
    int      head;    // index of the oldest entry
    int      count;   // live entries
    uint32_t ho;      // pool offset where the oldest live payload starts
    uint32_t te;      // pool offset one past the newest payload
    uint8_t  wrapped; // the live bytes are [ho, POOL) + [0, te)
    uint32_t early_evict; // entries evicted for SPACE while younger than WINDOW_MS (the bound bit)

    void clear() {
        head = count = 0;
        ho = te = 0;
        wrapped = 0;
    }
    bool     empty() const { return count == 0; }
    const Ent &at(int i) const { return ent[(head + i) % RING_ENTRIES]; }
    Ent       &at(int i) { return ent[(head + i) % RING_ENTRIES]; }
    uint32_t oldest_lo() const { return count ? at(0).lo : 0; }
    uint32_t newest_hi() const { return count ? at(count - 1).hi : 0; }

    void evict_front() {
        if (count == 0) return;
        const uint32_t old_ho = ho;
        head                  = (head + 1) % RING_ENTRIES;
        --count;
        if (count == 0) {
            ho = te = 0;
            wrapped = 0;
            return;
        }
        ho = at(0).off;
        if (wrapped && ho < old_ho) wrapped = 0; // the high part is gone: only [ho, te) is live
    }
    void expire(uint32_t now) {
        while (count && (int32_t)(now - at(0).t) > (int32_t)WINDOW_MS) evict_front();
    }
    // Reserve `n` (>= 1) contiguous pool bytes, evicting the oldest as needed. False if n > POOL.
    bool place(uint32_t n, uint32_t now, uint32_t &off) {
        if (n > RING_POOL) return false;
        for (;;) {
            if (count == 0) {
                ho = 0;
                off = 0;
                te = n;
                wrapped = 0;
                return true;
            }
            if (!wrapped) {
                if (RING_POOL - te >= n) {
                    off = te;
                    te += n;
                    return true;
                }
                if (ho >= n) { // wrap: [0, n) is free because the oldest payload starts at ho >= n
                    wrapped = 1;
                    off     = 0;
                    te      = n;
                    return true;
                }
            } else if (ho - te >= n) {
                off = te;
                te += n;
                return true;
            }
            if (!tick_before(now, at(0).t) && (int32_t)(now - at(0).t) <= (int32_t)WINDOW_MS) ++early_evict;
            evict_front();
        }
    }
    // Append [lo, hi] with its payload. `sup` frames coalesce into a sup neighbour of equal length.
    void add(uint32_t lo, uint32_t hi, uint32_t now, const uint8_t *data, int len, bool sup) {
        expire(now);
        if (sup && count) {
            Ent &n = at(count - 1);
            if (n.sup && n.len == (uint16_t)len && n.hi + 1 == lo) {
                if (len) memcpy(pool + n.off, data, (size_t)len);
                n.hi = hi;
                n.t  = now;
                return;
            }
        }
        if (count == RING_ENTRIES) {
            if (!tick_before(now, at(0).t) && (int32_t)(now - at(0).t) <= (int32_t)WINDOW_MS) ++early_evict;
            evict_front();
        }
        uint32_t off = 0;
        if (!place(len > 0 ? (uint32_t)len : 1u, now, off)) return; // cannot happen: len <= PAYLOAD_MAX
        Ent &e = ent[(head + count) % RING_ENTRIES];
        e.lo   = lo;
        e.hi   = hi;
        e.t    = now;
        e.off  = off;
        e.len  = (uint16_t)len;
        e.sup  = sup ? 1 : 0;
        if (len) memcpy(pool + off, data, (size_t)len);
        ++count;
    }
};

// ---- the reconcile vocabulary --------------------------------------------------------------------
struct Report {
    uint32_t front[ORIGINS];  // highest contiguous seq held (an origin's own: highest sent); 0 = none
    uint32_t oldest[ORIGINS]; // lowest seq still retained; 0 = nothing retained
    uint16_t inc[ORIGINS];    // the incarnation the numbers belong to
};

struct Fetch {
    uint8_t  origin;
    uint8_t  to_peer; // index into the reports passed to plan(): who needs the frames
    uint8_t  holder;  // index into the reports: who serves them
    uint32_t from, to; // inclusive seq range
};

constexpr int REPORT_WIRE_BYTES = 1 + ORIGINS * 10; // version | per origin: front(4) oldest(4) inc(2)

inline int report_encode(const Report &r, uint8_t *out) {
    int o = 0;
    out[o++] = CAP_VERSION;
    for (int i = 0; i < ORIGINS; ++i) {
        for (int b = 0; b < 4; ++b) out[o++] = (uint8_t)(r.front[i] >> (8 * b));
        for (int b = 0; b < 4; ++b) out[o++] = (uint8_t)(r.oldest[i] >> (8 * b));
        out[o++] = (uint8_t)(r.inc[i] & 0xff);
        out[o++] = (uint8_t)(r.inc[i] >> 8);
    }
    return o;
}
inline bool report_decode(const uint8_t *in, int len, Report &r) {
    if (len != REPORT_WIRE_BYTES || in[0] != CAP_VERSION) return false;
    int o = 1;
    for (int i = 0; i < ORIGINS; ++i) {
        r.front[i] = r.oldest[i] = 0;
        for (int b = 0; b < 4; ++b) r.front[i] |= (uint32_t)in[o++] << (8 * b);
        for (int b = 0; b < 4; ++b) r.oldest[i] |= (uint32_t)in[o++] << (8 * b);
        r.inc[i] = (uint16_t)(in[o] | (in[o + 1] << 8));
        o += 2;
    }
    return true;
}

// One served range on the wire (a batch is these back to back): origin(1) inc(2) lo(4) hi(4) len(2) bytes.
constexpr int RANGE_HDR_BYTES = 13;
inline int range_encode(uint8_t *out, int cap, int origin, uint16_t inc, uint32_t lo, uint32_t hi,
                        const uint8_t *data, int len) {
    if (cap < RANGE_HDR_BYTES + len) return 0;
    out[0] = (uint8_t)origin;
    out[1] = (uint8_t)(inc & 0xff);
    out[2] = (uint8_t)(inc >> 8);
    for (int b = 0; b < 4; ++b) out[3 + b] = (uint8_t)(lo >> (8 * b));
    for (int b = 0; b < 4; ++b) out[7 + b] = (uint8_t)(hi >> (8 * b));
    out[11] = (uint8_t)(len & 0xff);
    out[12] = (uint8_t)(len >> 8);
    if (len) memcpy(out + RANGE_HDR_BYTES, data, (size_t)len);
    return RANGE_HDR_BYTES + len;
}
// Returns the bytes consumed, 0 on a malformed/truncated record.
inline int range_decode(const uint8_t *in, int avail, int &origin, uint16_t &inc, uint32_t &lo,
                        uint32_t &hi, const uint8_t *&data, int &len) {
    if (avail < RANGE_HDR_BYTES) return 0;
    origin = in[0];
    inc    = (uint16_t)(in[1] | (in[2] << 8));
    lo = hi = 0;
    for (int b = 0; b < 4; ++b) lo |= (uint32_t)in[3 + b] << (8 * b);
    for (int b = 0; b < 4; ++b) hi |= (uint32_t)in[7 + b] << (8 * b);
    len = in[11] | (in[12] << 8);
    if (origin >= ORIGINS || len > PAYLOAD_MAX || lo == 0 || hi < lo || avail < RANGE_HDR_BYTES + len) return 0;
    data = in + RANGE_HDR_BYTES;
    return RANGE_HDR_BYTES + len;
}

// THE PLANNER. Pure: reports in, fetches out. For each origin, the target is the highest frontier any
// survivor holds; every survivor below it is assigned the lowest-index holder of that maximum whose
// retention still covers what it lacks. A survivor with NO state for an origin starts from the
// holder's oldest retained seq (it joined after the origin's earlier frames; nothing older is its
// to need). Returns the number of fetches written (capped at `cap`); sets *unrecoverable when some
// needed range is not retained by any max-holder (or the incarnations disagree) -- the caller ends
// the match. target[] receives the per-origin goal.
inline int plan(const Report *rep, int n, Fetch *out, int cap, bool *unrecoverable, uint32_t *target) {
    int nf = 0;
    if (unrecoverable) *unrecoverable = false;
    for (int o = 0; o < ORIGINS; ++o) {
        uint32_t best = 0;
        int      bi   = -1;
        for (int p = 0; p < n; ++p)
            if (rep[p].front[o] > best) {
                best = rep[p].front[o];
                bi   = p;
            }
        if (target) target[o] = best;
        if (bi < 0) continue;
        const uint16_t inc = rep[bi].inc[o];
        for (int p = 0; p < n; ++p) {
            if (rep[p].front[o] >= best) continue;
            if (rep[p].front[o] != 0 && rep[p].inc[o] != inc) { // a different life of that origin
                if (unrecoverable) *unrecoverable = true;
                continue;
            }
            int h = -1;
            for (int q = 0; q < n; ++q) {
                if (rep[q].front[o] != best || rep[q].inc[o] != inc) continue;
                const uint32_t start = rep[p].front[o] ? rep[p].front[o] + 1 : rep[q].oldest[o];
                if (rep[q].oldest[o] != 0 && rep[q].oldest[o] <= start) {
                    h = q;
                    break;
                }
            }
            if (h < 0) {
                if (unrecoverable) *unrecoverable = true;
                continue;
            }
            if (nf < cap) {
                Fetch &f   = out[nf];
                f.origin   = (uint8_t)o;
                f.to_peer  = (uint8_t)p;
                f.holder   = (uint8_t)h;
                f.from     = rep[p].front[o] ? rep[p].front[o] + 1 : rep[h].oldest[o];
                f.to       = best;
                ++nf;
            } else if (unrecoverable) {
                *unrecoverable = true; // the caller's buffer was too small: refuse rather than half-plan
            }
        }
    }
    return nf;
}

// ---- the layer ----------------------------------------------------------------------------------
struct Counters {
    long delivered;      // frames handed to the application, once each
    long dup;            // arrivals below the frontier (already delivered)
    long ahead;          // arrivals buffered above a hole
    long ahead_dropped;  // arrivals the ahead buffer had no room for (reconcile refetches them)
    long gap_skipped;    // holes skipped by the gap timeout (normal play: ZERO)
    long resets;         // origin restarts (a new incarnation)
    long legacy_refused; // peers refused for lacking the layer (counted by the endpoint)
    long early_evict;    // retention evictions of frames younger than the window (the bound biting)
    long skip_origin, skip_from, skip_to; // mp:U63 diagnostics: the last hole skipped (origin, [from, to))
};

struct Layer {
    struct Ahead {
        uint8_t  used;
        uint8_t  sup;
        uint16_t len;
        uint32_t lo, hi;
        uint8_t  data[PAYLOAD_MAX];
    };
    struct Origin {
        uint8_t  have;     // a sequence is established for this origin
        uint16_t inc;
        uint32_t next;     // remote: next seq to deliver. local: next seq to assign
        uint32_t ahead_t;  // tick the oldest waiting frame arrived (0-free: see ahead_n)
        int      ahead_n;
        Ring     ring;
        Ahead    ahead[AHEAD_SLOTS];
    };
    Origin   o[ORIGINS];
    uint16_t my_inc;
    int      local;        // this endpoint's own origin id, or -1
    uint32_t gap_force_ms;
    Counters c;

    void reset(uint16_t incarnation) {
        for (int i = 0; i < ORIGINS; ++i) {
            o[i].have    = 0;
            o[i].inc     = 0;
            o[i].next    = 0;
            o[i].ahead_n = 0;
            o[i].ahead_t = 0;
            o[i].ring.clear();
            o[i].ring.early_evict = 0;
            for (int s = 0; s < AHEAD_SLOTS; ++s) o[i].ahead[s].used = 0;
        }
        my_inc       = incarnation;
        local        = -1;
        gap_force_ms = GAP_FORCE_MS;
        memset(&c, 0, sizeof(c));
    }
    // The origin id this endpoint sends as. A host-assign client learns it from WELCOME, after
    // start(); frames for that origin arriving later are our own and are dropped as duplicates.
    void set_local(int id) { local = (id >= 0 && id < ORIGINS) ? id : -1; }
    uint16_t incarnation() const { return my_inc; }
    Counters counters() const {
        Counters r = c;
        for (int i = 0; i < ORIGINS; ++i) r.early_evict += (long)o[i].ring.early_evict;
        return r;
    }

    // ---- send side ----
    // Assign the next sequence for a frame this endpoint originates and retain it. Returns the seq
    // (>= 1), or 0 if `origin` is not a valid id (the frame then goes out unsequenced).
    uint32_t tx(int origin, const uint8_t *payload, int len, bool sup, uint32_t now) {
        if (origin < 0 || origin >= ORIGINS || len < 0 || len > PAYLOAD_MAX) return 0;
        Origin &g = o[origin];
        if (!g.have || g.inc != my_inc) { // first frame of this life
            g.ring.clear();
            g.have = 1;
            g.inc  = my_inc;
            g.next = 1;
        }
        const uint32_t seq = g.next++;
        g.ring.add(seq, seq, now, payload, len, sup);
        return seq;
    }

    // ---- receive side ----
    enum class Rx { Delivered, Duplicate, Ahead, Dropped };

    // One frame (or one coalesced range [lo, hi] from a reconcile) from `origin`. `sink` is called as
    // sink(origin, data, len) for every frame that becomes deliverable, in order; the return says what
    // happened to THIS arrival (Delivered also covers "delivered, and a waiting run behind it too").
    template <class Sink>
    Rx rx(int origin, uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *p, int len, bool sup,
          uint32_t now, Sink &&sink) {
        if (origin < 0 || origin >= ORIGINS || len < 0 || len > PAYLOAD_MAX || lo == 0 || hi < lo)
            return Rx::Dropped;
        if (origin == local) { // our own frame, handed back by a reconcile
            ++c.dup;
            return Rx::Duplicate;
        }
        Origin &g = o[origin];
        if (!g.have || g.inc != inc) {
            if (g.have) ++c.resets;
            g.ring.clear();
            g.ahead_n = 0;
            for (int s = 0; s < AHEAD_SLOTS; ++s) g.ahead[s].used = 0;
            g.have = 1;
            g.inc  = inc;
            g.next = lo; // late joiner / restarted origin: its first frame IS the base
        }
        if (hi < g.next) {
            ++c.dup;
            return Rx::Duplicate;
        }
        if (lo <= g.next) {
            deliver_one(origin, g, hi, p, len, sup, now, sink);
            drain_ahead(origin, g, now, sink);
            return Rx::Delivered;
        }
        // above a hole: wait for it
        for (int s = 0; s < AHEAD_SLOTS; ++s)
            if (g.ahead[s].used && g.ahead[s].lo == lo && g.ahead[s].hi == hi) {
                ++c.dup;
                return Rx::Duplicate;
            }
        int slot = -1;
        for (int s = 0; s < AHEAD_SLOTS; ++s)
            if (!g.ahead[s].used) {
                slot = s;
                break;
            }
        if (slot < 0) { // full: keep the LOWER seqs (they unblock delivery), refuse the new one
            ++c.ahead_dropped;
            return Rx::Dropped;
        }
        Ahead &a = g.ahead[slot];
        a.used   = 1;
        a.sup    = sup ? 1 : 0;
        a.len    = (uint16_t)len;
        a.lo     = lo;
        a.hi     = hi;
        if (len) memcpy(a.data, p, (size_t)len);
        if (g.ahead_n == 0) g.ahead_t = now ? now : 1;
        ++g.ahead_n;
        ++c.ahead;
        return Rx::Ahead;
    }

    // Timer hook: skip a hole that has outlived gap_force_ms. Cheap when nothing waits.
    template <class Sink>
    void tick(uint32_t now, Sink &&sink) {
        if (gap_force_ms == 0) return;
        for (int i = 0; i < ORIGINS; ++i) {
            Origin &g = o[i];
            if (g.ahead_n == 0) continue;
            if ((int32_t)(now - g.ahead_t) < (int32_t)gap_force_ms) continue;
            uint32_t lowest = 0xffffffffu;
            for (int s = 0; s < AHEAD_SLOTS; ++s)
                if (g.ahead[s].used && g.ahead[s].lo < lowest) lowest = g.ahead[s].lo;
            if (lowest != 0xffffffffu && lowest > g.next) {
                ++c.gap_skipped;
                c.skip_origin = i;
                c.skip_from   = (long)g.next;
                c.skip_to     = (long)lowest;
                g.next = lowest;
            }
            drain_ahead(i, g, now, sink);
            if (g.ahead_n) g.ahead_t = now ? now : 1;
        }
    }

    // ---- reconcile ----
    void report(Report &r) const {
        for (int i = 0; i < ORIGINS; ++i) {
            const Origin &g = o[i];
            r.front[i]      = g.have ? g.next - 1 : 0;
            r.oldest[i]     = g.have ? g.ring.oldest_lo() : 0;
            r.inc[i]        = g.have ? g.inc : 0;
        }
    }
    // Hand every retained entry overlapping [from, to] of `origin` to fn(inc, lo, hi, data, len, sup).
    // Returns the number of entries, or -1 if `from` is below the retention (the caller must treat that
    // as unrecoverable), or 0 if nothing is held there.
    template <class Fn>
    int serve(int origin, uint32_t from, uint32_t to, Fn &&fn) const {
        if (origin < 0 || origin >= ORIGINS || from == 0 || to < from) return 0;
        const Origin &g = o[origin];
        if (!g.have || g.ring.empty()) return 0;
        if (from < g.ring.oldest_lo()) return -1;
        int n = 0;
        for (int i = 0; i < g.ring.count; ++i) {
            const Ring::Ent &e = g.ring.at(i);
            if (e.hi < from) continue;
            if (e.lo > to) break;
            fn(g.inc, e.lo, e.hi, g.ring.pool + e.off, (int)e.len, e.sup != 0);
            ++n;
        }
        return n;
    }

private:
    template <class Sink>
    void deliver_one(int origin, Origin &g, uint32_t hi, const uint8_t *p, int len, bool sup, uint32_t now,
                     Sink &sink) {
        const uint32_t lo = g.next; // a partly-overlapping range is retained from the first NEW seq
        g.next            = hi + 1;
        g.ring.add(lo, hi, now, p, len, sup);
        ++c.delivered;
        sink(origin, p, len);
    }
    template <class Sink>
    void drain_ahead(int origin, Origin &g, uint32_t now, Sink &sink) {
        for (bool again = g.ahead_n > 0; again;) {
            again = false;
            for (int s = 0; s < AHEAD_SLOTS; ++s) {
                Ahead &a = g.ahead[s];
                if (!a.used) continue;
                if (a.hi < g.next) { // overtaken: a duplicate by now
                    a.used = 0;
                    --g.ahead_n;
                    ++c.dup;
                    again = true;
                } else if (a.lo <= g.next) {
                    // copy out first: the sink may re-enter nothing, but a slot must not be live while
                    // its bytes are being handed over and the ring is being written
                    uint8_t  tmp[PAYLOAD_MAX];
                    const int n = a.len;
                    if (n) memcpy(tmp, a.data, (size_t)n);
                    const uint32_t hi  = a.hi;
                    const bool     sup = a.sup != 0;
                    a.used             = 0;
                    --g.ahead_n;
                    deliver_one(origin, g, hi, tmp, n, sup, now, sink);
                    again = true;
                }
            }
        }
        if (g.ahead_n == 0) g.ahead_t = 0;
    }
};

} // namespace osq
} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_ORIGIN_SEQ_H
