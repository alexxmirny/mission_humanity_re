//
// desync/desync_wire2.h -- the in-band desync watch, WIRE_VERSION 2 (mp:D44): compare EVERY sim step
// on the incremental state hash, and localise a divergence live to region + offset.
//
// WHAT CHANGED FROM v1. v1 (desync_watch.h) hashes the whole 2.8 MB manifest with the FNV VERDICT walk
// every `every`=50 steps (4-5 ms a walk) and sends the 63 per-region hashes. It can name a region but
// never an offset, and a divergence that heals inside one interval is invisible to it. v2 reads the
// per-region hashes the shared per-step tracker (state_tracker.h, mp:D39's incremental core) already
// keeps up to date, so a comparison costs a fold of 63 numbers and can run every step:
//   * TICK frames carry only (step, state hash) per step plus the cumulative order digest -- the
//     per-region vector is NOT sent every step (it would be ~26 KB/s per peer);
//   * on the first mismatching step S of an incident, both peers run the same deterministic exchange
//     (REGIONS -> GROUPS -> BLOCKS -> BYTES, below), each over the state AT S rebuilt from the tracker's
//     undo journal, and log `region + offset` at S on both sides. No snapshot file is involved.
//
// ONE SYMMETRIC RULE DRIVES THE EXCHANGE. Every peer, on receiving level L of an incident from a peer,
// first publishes its OWN level L if it has not yet (so both sides always hold both), then compares
// and publishes level L+1 for whatever differs. Both peers compare the same pair of values, so they
// reach the same conclusions and publish the same keys without any request/response bookkeeping, and
// the host's relay makes it work for N peers. Every frame is a broadcast; a frame for a step other
// than the peer's active incident is ignored (earliest incident step wins -- see localiser::on_frame).
//
// WIRE COMPATIBILITY (the D31 precedent, generalised). Every v2 frame starts with v1's magic and puts
// VERSION=2 where v1 reads its version, and keeps the step at byte 16 where v1 has it (the UDP
// transport's bulk-selftest trigger reads it there). A v1 build's frame_is_sane() rejects the version
// and counts a bad frame -- it neither crashes nor misreads (desynctest proves it on the real v1
// parser). A v2 build that receives a v1 frame knows that peer is old and falls back to the FNV walk
// at the v1 cadence for it (desync_watch.cpp, "FALLBACK").
//
// Everything here is pure (no game, no socket, no Windows) so `net_selftest.exe desynctest` runs it.
//
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "desync/desync_watch.h"

namespace mh::desync::v2 {

inline constexpr uint16_t VERSION = 2;

enum frame_type : uint8_t {
    T_TICK    = 1, // per-step state hashes of `count` consecutive steps + the order digest at the last
    T_REGIONS = 2, // incident: the sender's per-region hashes at step S
    T_GROUPS  = 3, // incident: region `region`, one hash per 64-block group
    T_BLOCKS  = 4, // incident: region `region`, group `aux`: its block hashes
    T_BYTES   = 5, // incident: region `region`, group `aux`: the raw bytes of up to 16 listed blocks
    // mp:X3c -- world-resync CONTROL frames (ws_*, below). They ride the desync frames, so no transport
    // ABI moves and the game's lockstep wire is untouched. hdr.aux = the DESTINATION player; a receiver
    // drops a frame that is not for it, and the host relay forwards them unchanged. count is always 1.
    T_WS_BEGIN = 6, // host -> peer: you diverged at incident step `step`; a capture follows
    T_WS_META  = 7, // host -> peer: capture step S (in `step`), digest, per-source counters, blob length, root
    T_WS_DONE  = 8, // peer -> host: import + catch-up finished (live step, rc, fast-forward ms)
    T_WS_ABORT = 9, // host -> peer: give up (`reason`, ws_abort_reason)
};

// The common 24-byte prefix. Byte-compatible with v1's sample_wire up to `step` (magic @0, version @4,
// manifest fingerprint @8, step @16).
#pragma pack(push, 1)
struct hdr {
    uint32_t magic;       // WIRE_MAGIC
    uint16_t version;     // VERSION
    uint8_t  type;        // frame_type
    uint8_t  count;       // entries in the body (see frame_size)
    uint64_t manifest_fp; // fp_v2(): the v1 manifest fingerprint with the hash kind folded in
    uint32_t step;        // TICK: first step of the batch; incident frames: the incident step S
    uint16_t region;      // incident frames: the hash-manifest index; 0 otherwise
    uint16_t aux;         // GROUPS: 0; BLOCKS/BYTES: the group index; 0 otherwise
};
#pragma pack(pop)
static_assert(sizeof(hdr) == 24, "v2 header is 24 bytes");
static_assert(offsetof(hdr, step) == offsetof(sample_wire, step), "step sits where v1 keeps it");
static_assert(offsetof(hdr, version) == offsetof(sample_wire, version), "version sits where v1 reads it");

inline constexpr int HDR_BYTES = (int)sizeof(hdr);

// Sizes. The bodies are u64 arrays except BYTES, whose entries are {u16 block index, 64 bytes}.
inline constexpr int      TICK_MAX_STEPS   = 32; // steps one TICK may carry
inline constexpr int      GROUP_BLOCKS     = 64; // blocks per level-1 group
inline constexpr int      MAX_GROUPS       = 128;
inline constexpr uint32_t MAX_BLOCKS       = (uint32_t)GROUP_BLOCKS * MAX_GROUPS; // 8192 = a 512 KB region
inline constexpr int      BYTES_ENTRY      = 2 + 64;
inline constexpr int      BYTES_MAX_BLOCKS = 16;
inline constexpr int      REGIONS_MAX      = MAX_REGIONS;

inline int tick_size(int steps) { return HDR_BYTES + 8 * steps + 8; }
inline int regions_size(int n) { return HDR_BYTES + 8 * n; }
inline int groups_size(int g) { return HDR_BYTES + 8 * g; }
inline int blocks_size(int b) { return HDR_BYTES + 8 * b; }
inline int bytes_size(int k) { return HDR_BYTES + BYTES_ENTRY * k; }

// ---- mp:X3c world-resync control bodies (little-endian, packed; one body per frame, count == 1) -----
inline constexpr int WS_SRC_SLOTS  = 9; // per-source admission counters: owner nibbles 0..7 + the 0xf0 slot (8)
inline constexpr int WS_ROOT_BYTES = 8;
#pragma pack(push, 1)
struct ws_begin_body {
    uint32_t flags; // reserved, 0
};
struct ws_meta_body {
    uint64_t digest_prev;         // the host's cumulative order digest BEFORE step S's fold
    uint32_t n_src[WS_SRC_SLOTS]; // admission counters at S, per source
    uint32_t blob_len;            // bytes of the channel-C blob this META describes
    uint8_t  root[WS_ROOT_BYTES]; // the blob root (first 8 bytes), so the receiver pairs META with blob
};
struct ws_done_body {
    uint32_t live_step; // the importer's step T when it went LIVE
    int32_t  rc;        // 0 = ok, negative = refused/failed (e.g. -30 = admission ring too short)
    uint32_t ff_ms;     // wall ms the catch-up took
};
struct ws_abort_body {
    uint32_t reason; // ws_abort_reason
};
#pragma pack(pop)
static_assert(sizeof(ws_begin_body) == 4 && sizeof(ws_meta_body) == 56 && sizeof(ws_done_body) == 12 &&
                  sizeof(ws_abort_body) == 4,
              "world-resync bodies are fixed-size");

enum ws_abort_reason : uint32_t {
    WS_ABORT_NONE         = 0,
    WS_ABORT_ROSTER       = 1, // a slot's ALIVE bit changed after BEGIN (the D26 exclusion)
    WS_ABORT_GAMEOVER     = 2, // the match is ending (host or peer left SESSION_MODE 3, R4)
    WS_ABORT_TIMEOUT      = 3, // resync_timeout_ms elapsed
    WS_ABORT_CAPTURE      = 4, // capture kept failing (retry budget spent)
    WS_ABORT_PENDING_OVFL = 5, // ORDER_PENDING overflowed: the admission counters are void (R5)
    WS_ABORT_PEER_GONE    = 6, // the target peer left / is no longer an active human
    WS_ABORT_RING_SHORT   = 7, // reserved: peer reports the admission ring cannot reach n_src
    WS_ABORT_COUNT
};

inline constexpr int ws_body_size(uint8_t type) {
    return type == T_WS_BEGIN   ? (int)sizeof(ws_begin_body)
           : type == T_WS_META  ? (int)sizeof(ws_meta_body)
           : type == T_WS_DONE  ? (int)sizeof(ws_done_body)
           : type == T_WS_ABORT ? (int)sizeof(ws_abort_body)
                                : -1;
}
inline int ws_size(uint8_t type) {
    const int b = ws_body_size(type);
    return b < 0 ? -1 : HDR_BYTES + b;
}

// The largest v2 frame: a full BYTES frame (1080 B) -- under the transports' 2048-byte payload cap.
inline constexpr int MAX_FRAME = HDR_BYTES + BYTES_ENTRY * BYTES_MAX_BLOCKS;
static_assert(MAX_FRAME >= HDR_BYTES + 8 * MAX_GROUPS, "a GROUPS frame fits MAX_FRAME");
static_assert(MAX_FRAME >= HDR_BYTES + 8 * MAX_REGIONS, "a REGIONS frame fits MAX_FRAME");
static_assert(MAX_FRAME <= 2048, "v2 frames stay under MH_NET_MAX_PAYLOAD");

// The v2 fingerprint: the v1 manifest fingerprint with the hash KIND folded in, so a peer hashing with
// a different kind reads as a build mismatch rather than a desync.
inline uint64_t fp_v2(uint64_t manifest_fp, uint32_t hash_kind) {
    return fnv1a(&hash_kind, sizeof(hash_kind), manifest_fp);
}

// 0 = not a desync frame (or too short to say); otherwise the version word.
inline int peek_version(const uint8_t *buf, int len) {
    if (buf == nullptr || len < 6) return 0;
    uint32_t m;
    uint16_t v;
    memcpy(&m, buf, 4);
    memcpy(&v, buf + 4, 2);
    return m == WIRE_MAGIC ? (int)v : 0;
}

// The exact byte count a frame of this type/count must have, or -1 if the count is out of range.
// `nregions` bounds REGIONS (the sender's region count must equal ours: same fingerprint).
inline int frame_size(uint8_t type, int count, int nregions) {
    switch (type) {
        case T_TICK: return (count >= 1 && count <= TICK_MAX_STEPS) ? tick_size(count) : -1;
        case T_REGIONS: return (count == nregions && count >= 1 && count <= REGIONS_MAX) ? regions_size(count) : -1;
        case T_GROUPS: return (count >= 1 && count <= MAX_GROUPS) ? groups_size(count) : -1;
        case T_BLOCKS: return (count >= 1 && count <= GROUP_BLOCKS) ? blocks_size(count) : -1;
        case T_BYTES: return (count >= 1 && count <= BYTES_MAX_BLOCKS) ? bytes_size(count) : -1;
        case T_WS_BEGIN:
        case T_WS_META:
        case T_WS_DONE:
        case T_WS_ABORT: return count == 1 ? ws_size(type) : -1;
        default: return -1;
    }
}

// Validate a received v2 frame in isolation (header + exact length). The fingerprint is judged by the
// caller: a wrong one is a build mismatch, not a malformed frame.
inline bool frame_ok(const uint8_t *buf, int len, int nregions, hdr &out) {
    if (buf == nullptr || len < HDR_BYTES || len > MAX_FRAME) return false;
    memcpy(&out, buf, sizeof(out));
    if (out.magic != WIRE_MAGIC || out.version != VERSION) return false;
    const int want = frame_size(out.type, out.count, nregions);
    if (want < 0 || want != len) return false;
    if (out.type == T_TICK && (out.region != 0 || out.aux != 0)) return false;
    // World-resync control: aux is the destination player (0..7), region unused.
    if (out.type >= T_WS_BEGIN && out.type <= T_WS_ABORT && (out.region != 0 || out.aux >= 8)) return false; // 8 == MAX_SENDERS
    return true;
}

inline void put_hdr(uint8_t *buf, uint8_t type, uint8_t count, uint64_t fp, uint32_t step, uint16_t region,
                    uint16_t aux) {
    hdr h = {WIRE_MAGIC, VERSION, type, count, fp, step, region, aux};
    memcpy(buf, &h, sizeof(h));
}

// ---- mp:X3c world-resync control frames: encode / decode --------------------------------------------
// Encoders write header + body and return the frame length. Decoders take the RAW datagram, validate the
// whole frame (magic, version, type, count 1, exact length, fingerprint, region 0, aux < 8) and fill the
// header and body; they return false on anything else, leaving `out` unspecified. The caller judges the
// destination (hdr.aux) itself: a frame not for us is dropped, not malformed.
template <class Body>
inline int ws_put(uint8_t *buf, uint8_t type, uint64_t fp, uint32_t step, int dst, const Body &b) {
    put_hdr(buf, type, 1, fp, step, 0, (uint16_t)dst);
    memcpy(buf + HDR_BYTES, &b, sizeof(b));
    return HDR_BYTES + (int)sizeof(b);
}
inline int ws_put_begin(uint8_t *buf, uint64_t fp, uint32_t incident_step, int dst, uint32_t flags = 0) {
    ws_begin_body b = {flags};
    return ws_put(buf, T_WS_BEGIN, fp, incident_step, dst, b);
}
inline int ws_put_meta(uint8_t *buf, uint64_t fp, uint32_t capture_step, int dst, const ws_meta_body &b) {
    return ws_put(buf, T_WS_META, fp, capture_step, dst, b);
}
inline int ws_put_done(uint8_t *buf, uint64_t fp, uint32_t capture_step, int dst, const ws_done_body &b) {
    return ws_put(buf, T_WS_DONE, fp, capture_step, dst, b);
}
inline int ws_put_abort(uint8_t *buf, uint64_t fp, uint32_t step, int dst, uint32_t reason) {
    ws_abort_body b = {reason};
    return ws_put(buf, T_WS_ABORT, fp, step, dst, b);
}

template <class Body>
inline bool ws_get(const uint8_t *buf, int len, uint8_t type, uint64_t want_fp, hdr &h, Body &b) {
    if (frame_ok(buf, len, 1, h) == false) return false;
    if (h.type != type || h.manifest_fp != want_fp || (int)sizeof(Body) != ws_body_size(type)) return false;
    memcpy(&b, buf + HDR_BYTES, sizeof(b));
    return true;
}
inline bool ws_get_begin(const uint8_t *buf, int len, uint64_t fp, hdr &h, ws_begin_body &b) {
    return ws_get(buf, len, T_WS_BEGIN, fp, h, b);
}
inline bool ws_get_meta(const uint8_t *buf, int len, uint64_t fp, hdr &h, ws_meta_body &b) {
    return ws_get(buf, len, T_WS_META, fp, h, b);
}
inline bool ws_get_done(const uint8_t *buf, int len, uint64_t fp, hdr &h, ws_done_body &b) {
    return ws_get(buf, len, T_WS_DONE, fp, h, b);
}
inline bool ws_get_abort(const uint8_t *buf, int len, uint64_t fp, hdr &h, ws_abort_body &b) {
    return ws_get(buf, len, T_WS_ABORT, fp, h, b);
}

// ---- the per-step comparison ---------------------------------------------------------------------
// OUR side: one entry per sim step (so a direct index is exact -- the v1 ring had to scan because its
// sampled steps were multiples of the cadence and aliased under a modulo). THEIR side: per sender, the
// states they sent for steps we have not reached yet. A step is judged the moment both halves exist.
inline constexpr int TICK_RING   = 256; // ~5 s of steps at 50 steps/s
inline constexpr int MAX_SENDERS = 8;

// On-screen notice persistence, in STEPS: one v1 sampling interval (every=50) of standing
// disagreement, which is what v1's "second consecutive mismatching sample" amounted to. D30's healed
// order divergences (7-18 steps) stay log-only exactly as they did.
inline constexpr int NOTIFY_PERSIST_STEPS = 50;
inline bool          should_notify_steps(int consecutive_mismatching_steps) {
    return consecutive_mismatching_steps == NOTIFY_PERSIST_STEPS;
}

struct tick_verdict {
    enum kind_t : int { ok = 0,
                        mismatch,
                        too_old,
                        order_ok,
                        order_mismatch } kind;
    int      sender;
    uint32_t step;
    uint64_t mine, theirs;
    bool     incident_start; // mismatch: the previous judged step from this sender agreed (or none)
    int      consecutive;    // mismatch: consecutive mismatching judged steps from this sender
};

struct ticker {
    struct mine_entry {
        uint32_t step; // 0 = empty
        uint64_t state;
        uint64_t order_digest;
        uint64_t per[MAX_REGIONS];
    };
    struct peer {
        int      sender; // -1 = free
        uint32_t step[TICK_RING];
        uint64_t state[TICK_RING];
        uint32_t od_step; // a pending order digest for a step we have not reached (0 = none)
        uint64_t od;
        bool     last_mismatch;
        int      consecutive;
        uint32_t judged, mismatching;
    };

    mine_entry mine[TICK_RING];
    peer       peers[MAX_SENDERS];
    uint32_t   newest; // highest step we have put (0 = none)
    int        nregions;
    uint32_t   unjudged_overflow; // their samples lost to a full sender table

    void clear(int n) {
        memset(this, 0, sizeof(*this));
        nregions = n;
        for (int i = 0; i < MAX_SENDERS; ++i) peers[i].sender = -1;
    }

    const mine_entry *find_mine(uint32_t s) const {
        const mine_entry &e = mine[s % TICK_RING];
        return (s != 0 && e.step == s) ? &e : nullptr;
    }

    peer *peer_of(int sender) {
        for (int i = 0; i < MAX_SENDERS; ++i)
            if (peers[i].sender == sender) return &peers[i];
        for (int i = 0; i < MAX_SENDERS; ++i)
            if (peers[i].sender < 0) {
                memset(&peers[i], 0, sizeof(peers[i]));
                peers[i].sender = sender;
                return &peers[i];
            }
        return nullptr;
    }

    template <class F>
    void judge(peer &p, uint32_t s, uint64_t mine_state, uint64_t theirs, F &&emit) {
        tick_verdict v = {tick_verdict::ok, p.sender, s, mine_state, theirs, false, 0};
        ++p.judged;
        if (mine_state == theirs) {
            p.last_mismatch = false;
            p.consecutive   = 0;
        } else {
            v.kind           = tick_verdict::mismatch;
            v.incident_start = !p.last_mismatch;
            p.last_mismatch  = true;
            v.consecutive    = ++p.consecutive;
            ++p.mismatching;
        }
        emit(v);
    }

    template <class F>
    void judge_order(peer &p, uint32_t s, uint64_t theirs_od, F &&emit) {
        const mine_entry *m = find_mine(s);
        if (!m) return;
        tick_verdict v = {m->order_digest == theirs_od ? tick_verdict::order_ok : tick_verdict::order_mismatch,
                          p.sender,
                          s,
                          m->order_digest,
                          theirs_od,
                          false,
                          0};
        emit(v);
    }

    // Our own step. Judges every held sample of every sender for this step.
    template <class F>
    void put_mine(uint32_t s, uint64_t state, uint64_t order_digest, const uint64_t *per, F &&emit) {
        mine_entry &e  = mine[s % TICK_RING];
        e.step         = s;
        e.state        = state;
        e.order_digest = order_digest;
        memcpy(e.per, per, sizeof(uint64_t) * (size_t)(nregions < 0 ? 0 : nregions));
        if (s > newest) newest = s;
        for (int i = 0; i < MAX_SENDERS; ++i) {
            peer &p = peers[i];
            if (p.sender < 0) continue;
            const int k = (int)(s % TICK_RING);
            if (p.step[k] == s) {
                p.step[k] = 0;
                judge(p, s, state, p.state[k], emit);
            }
            if (p.od_step == s) {
                p.od_step = 0;
                judge_order(p, s, p.od, emit);
            }
        }
    }

    // A received TICK: `count` consecutive steps from `first`, and the order digest at the last one.
    template <class F>
    void put_theirs(int sender, uint32_t first, int count, const uint64_t *states, uint64_t od, F &&emit) {
        peer *p = peer_of(sender);
        if (!p) {
            unjudged_overflow += (uint32_t)count;
            return;
        }
        for (int j = 0; j < count; ++j) {
            const uint32_t s = first + (uint32_t)j;
            if (s == 0) continue;
            if (s <= newest) {
                const mine_entry *m = find_mine(s);
                if (m) {
                    judge(*p, s, m->state, states[j], emit);
                } else {
                    tick_verdict v = {tick_verdict::too_old, sender, s, 0, states[j], false, 0};
                    emit(v);
                }
            } else {
                const int k = (int)(s % TICK_RING);
                p->step[k]  = s;
                p->state[k] = states[j];
            }
        }
        const uint32_t last = first + (uint32_t)(count - 1);
        if (last <= newest) {
            judge_order(*p, last, od, emit);
        } else {
            p->od_step = last;
            p->od      = od;
        }
    }
};

// ---- the undo journal ----------------------------------------------------------------------------
// The tracker hands every changed 64-byte block's OLD bytes to this ring just before overwriting its
// shadow (inc_state.h `journal`). Newest-first replay of the entries of steps C..S+1 turns the shadow
// (the state at the latest step C) back into the state at S. Bounded: when full the oldest entry goes,
// and `floor` rises to its step -- the earliest step that can still be rebuilt.
struct undo_journal {
    struct entry {
        uint32_t step;
        uint32_t block;
        uint16_t region;
        uint16_t n;
        uint8_t  bytes[64];
    };

    entry   *e       = nullptr;
    uint32_t cap     = 0;
    uint32_t head    = 0; // next write
    uint32_t count   = 0;
    uint32_t floor   = 0; // the earliest rebuildable step
    bool     have    = false;
    uint32_t cur     = 0; // the step being journaled (set by the caller before each tracker update)
    uint64_t evicted = 0;

    void attach(void *mem, size_t bytes) {
        e    = static_cast<entry *>(mem);
        cap  = (uint32_t)(bytes / sizeof(entry));
        head = count = 0;
        have         = false;
        floor        = 0;
    }
    // A prime at step `s`: the shadow is the state at s and nothing older can be rebuilt.
    void clear(uint32_t s) {
        head = count = 0;
        floor        = s;
        have         = cap > 0; // no memory: nothing past the current step can be rebuilt
    }
    void add(int region, uint32_t block, const uint8_t *b, uint32_t n) {
        if (cap == 0 || !have) return;
        if (count == cap) {
            const entry &o = e[(head + cap - count) % cap];
            if (o.step > floor) floor = o.step;
            --count;
            ++evicted;
        }
        entry &d = e[head];
        d.step   = cur;
        d.block  = block;
        d.region = (uint16_t)region;
        d.n      = (uint16_t)(n > 64 ? 64 : n);
        memcpy(d.bytes, b, d.n);
        head = (head + 1) % cap;
        ++count;
    }
    bool covers(uint32_t s) const { return have && s >= floor; }
    // f(region, block, bytes, n) for every entry of a step > s, newest first.
    template <class F>
    void undo_to(uint32_t s, F &&f) const {
        for (uint32_t k = 0; k < count; ++k) {
            const entry &d = e[(head + cap - 1 - k) % cap];
            if (d.step <= s) break;
            f((int)d.region, d.block, d.bytes, (uint32_t)d.n);
        }
    }
};

// ---- the localiser -------------------------------------------------------------------------------
// What the localiser needs from its host (the live binding in desync_watch.cpp, or two synthetic peers
// in desynctest). `materialize(S)` makes evidence(r) the state of every region AT step S; the block
// hash and keep mask are the tracker's (inc_state.h), computed over that evidence.
struct loc_host {
    virtual ~loc_host()                                        = default;
    virtual int            nregions() const                    = 0;
    virtual uint32_t       region_len(int r) const             = 0;
    virtual const char    *region_name(int r) const            = 0;
    virtual bool           excluded(int r) const               = 0;
    virtual uint64_t       fp() const                          = 0;
    virtual bool           per_at(uint32_t s, uint64_t *out)   = 0; // my per-region hashes at s (step ring)
    virtual bool           materialize(uint32_t s)             = 0;
    virtual const uint8_t *evidence(int r)                     = 0;
    virtual uint64_t       block_hash(int r, uint32_t b)       = 0; // over evidence(r), masked
    virtual uint8_t        keep(int r, uint32_t off)           = 0; // the VERDICT keep bits of that byte
    virtual void           send(const uint8_t *frame, int len) = 0;
    virtual void           log(const char *line)               = 0;
    // "record 12 +0x1b of map_object_unit" or "" -- cheap naming, optional.
    virtual void describe(int r, uint32_t off, char *out, size_t cap) {
        (void)r;
        (void)off;
        if (cap) out[0] = 0;
    }
    // The first diverging region is known (or -1): the watch completes its `*** DESYNC` line here.
    virtual void on_regions(int sender, uint32_t s, int first_region, int ndiff) {
        (void)sender;
        (void)s;
        (void)first_region;
        (void)ndiff;
    }
};

inline constexpr int      LOC_MAX_REGIONS           = 4;   // diverging regions taken below region level
inline constexpr int      LOC_MAX_GROUPS_PER_REGION = 4;   // diverging groups opened per region
inline constexpr int      LOC_MAX_LINES             = 16;  // LOCALISED lines per incident
inline constexpr uint32_t LOC_SETTLE_STEPS          = 100; // a later incident only replaces one this old

inline uint32_t nblocks_of(uint32_t len) { return (len + 63u) / 64u; }
inline int      ngroups_of(uint32_t len) { return (int)((nblocks_of(len) + GROUP_BLOCKS - 1) / GROUP_BLOCKS); }

struct loc_result { // one LOCALISED finding, kept for the selftest and the rollup
    int      sender;
    int      region;
    uint32_t off;
    uint32_t block;
    int      nbytes; // kept bytes that differ in the block
    uint8_t  mine, theirs;
};

class localiser {
public:
    // A new match. The slot arrays are large (the block hashes of LOC_MAX_REGIONS regions), so only
    // their headers are cleared.
    void reset(int max_incidents) {
        memset(&k_, 0, sizeof(k_));
        max_incidents_ = max_incidents;
        incidents_     = 0;
        refused_step_  = 0;
        frames_sent_ = bytes_sent_ = ignored_ = 0;
        nres_                                 = 0;
        for (int i = 0; i < LOC_MAX_REGIONS; ++i) slots_[i].region = -1;
    }

    bool              active() const { return k_.active; }
    uint32_t          step() const { return k_.s; }
    int               incidents() const { return incidents_; }
    bool              evidence_ok() const { return k_.evidence_ok; }
    uint32_t          frames_sent() const { return frames_sent_; }
    uint32_t          bytes_sent() const { return bytes_sent_; }
    uint32_t          ignored() const { return ignored_; }
    int               nresults() const { return nres_; }
    const loc_result &result(int i) const { return res_[i]; }

    // Our own per-step comparison found the first mismatching step `s` of an incident.
    void on_local_mismatch(loc_host &h, uint32_t s) {
        if (k_.active) {
            if (s == k_.s) return;
            if (s < k_.s) {
                restart(h, s, false);
                return;
            }
            if (s < k_.s + LOC_SETTLE_STEPS) return; // the same flapping incident: keep localising the first
        }
        if (incidents_ >= max_incidents_) return;
        restart(h, s, true);
    }

    // A validated incident frame (REGIONS/GROUPS/BLOCKS/BYTES) from `sender`.
    void on_frame(loc_host &h, int sender, const hdr &f, const uint8_t *body) {
        if (!k_.active || f.step != k_.s) {
            // Earliest wins: a peer localising an EARLIER step than ours is closer to the first bad
            // step, and both sides converge on it. A later one is the peer's to move.
            if (k_.active && f.step > k_.s) {
                ++ignored_;
                return;
            }
            if (!k_.active && incidents_ >= max_incidents_) {
                ++ignored_;
                return;
            }
            if (f.step == refused_step_) {
                ++ignored_;
                return;
            }
            if (!restart(h, f.step, !k_.active)) return;
        }
        switch (f.type) {
            case T_REGIONS: on_regions_frame(h, sender, body); break;
            case T_GROUPS: on_groups_frame(h, sender, f, body); break;
            case T_BLOCKS: on_blocks_frame(h, sender, f, body); break;
            case T_BYTES: on_bytes_frame(h, sender, f, body); break;
            default: ++ignored_; break;
        }
    }

private:
    struct slot {
        int      region; // -1 = free
        bool     bad;    // the rebuilt evidence did not reproduce this region's hash at S
        bool     sent_groups;
        bool     noted_more;
        uint32_t sent_blocks[MAX_GROUPS / 32];
        uint32_t sent_bytes[MAX_GROUPS / 32];
        uint32_t nblocks;
        uint64_t bh[MAX_BLOCKS];
        uint64_t gh[MAX_GROUPS];
    };

    static bool bit(const uint32_t *b, int i) { return (b[i >> 5] >> (i & 31)) & 1u; }
    static void set_bit(uint32_t *b, int i) { b[i >> 5] |= 1u << (i & 31); }

    void say(loc_host &h, const char *fmt, ...) {
        char    line[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        h.log(line);
    }

    void emit(loc_host &h, const uint8_t *buf, int len) {
        h.send(buf, len);
        ++frames_sent_;
        bytes_sent_ += (uint32_t)len;
    }

    bool restart(loc_host &h, uint32_t s, bool new_incident) {
        const int n = h.nregions();
        uint64_t  per[MAX_REGIONS];
        if (n <= 0 || n > MAX_REGIONS || !h.per_at(s, per)) {
            if (refused_step_ != s) {
                refused_step_ = s;
                say(h, "; [desync] LOCALISE step=%lu: cannot -- this step is no longer in my step history\n",
                    (unsigned long)s);
            }
            return false;
        }
        memset(&k_, 0, sizeof(k_));
        for (int i = 0; i < LOC_MAX_REGIONS; ++i) slots_[i].region = -1;
        if (new_incident) ++incidents_;
        k_.active = true;
        k_.s      = s;
        memcpy(k_.per, per, sizeof(uint64_t) * (size_t)n);
        k_.evidence_ok = h.materialize(s);
        if (!k_.evidence_ok)
            say(h, "; [desync] LOCALISE step=%lu: block history no longer reaches this step -- region level only\n",
                (unsigned long)s);
        publish_regions(h);
        return true;
    }

    void publish_regions(loc_host &h) {
        if (k_.sent_regions) return;
        k_.sent_regions = true;
        const int n     = h.nregions();
        uint8_t   buf[MAX_FRAME];
        put_hdr(buf, T_REGIONS, (uint8_t)n, h.fp(), k_.s, 0, 0);
        memcpy(buf + HDR_BYTES, k_.per, sizeof(uint64_t) * (size_t)n);
        emit(h, buf, regions_size(n));
    }

    slot *slot_of(int r) {
        for (int i = 0; i < LOC_MAX_REGIONS; ++i)
            if (slots_[i].region == r) return &slots_[i];
        return nullptr;
    }

    // Take region r below region level: its block hashes at S from the evidence, checked against its
    // hash at S from the step ring -- a rebuild that does not reproduce the hash is not evidence.
    slot *track(loc_host &h, int r) {
        if (slot *s = slot_of(r)) return s;
        if (!k_.evidence_ok || r < 0 || r >= h.nregions()) return nullptr;
        const uint32_t nb = nblocks_of(h.region_len(r));
        if (nb > MAX_BLOCKS) return nullptr;
        slot *s = nullptr;
        for (int i = 0; i < LOC_MAX_REGIONS; ++i)
            if (slots_[i].region < 0) {
                s = &slots_[i];
                break;
            }
        if (!s) return nullptr;
        memset(s, 0, sizeof(*s));
        s->region    = r;
        s->nblocks   = nb;
        uint64_t sum = 0;
        for (uint32_t b = 0; b < nb; ++b) {
            s->bh[b] = h.block_hash(r, b);
            sum += s->bh[b];
            s->gh[b / GROUP_BLOCKS] += s->bh[b];
        }
        if (sum != k_.per[r]) {
            s->bad = true;
            say(h, "; [desync] LOCALISE step=%lu region=%d %s: SELF-CHECK FAILED -- the state rebuilt from the "
                   "undo journal does not reproduce this region's hash at the step (%08X%08X vs %08X%08X); not "
                   "localising it\n",
                (unsigned long)k_.s, r, h.region_name(r), (unsigned)(sum >> 32), (unsigned)sum,
                (unsigned)(k_.per[r] >> 32), (unsigned)k_.per[r]);
        }
        return s;
    }

    void publish_groups(loc_host &h, slot &s) {
        if (s.sent_groups || s.bad) return;
        s.sent_groups = true;
        const int g   = ngroups_of(h.region_len(s.region));
        uint8_t   buf[MAX_FRAME];
        put_hdr(buf, T_GROUPS, (uint8_t)g, h.fp(), k_.s, (uint16_t)s.region, 0);
        memcpy(buf + HDR_BYTES, s.gh, sizeof(uint64_t) * (size_t)g);
        emit(h, buf, groups_size(g));
    }

    int group_len(const slot &s, int g) const {
        const uint32_t lo = (uint32_t)g * GROUP_BLOCKS;
        const uint32_t hi = lo + GROUP_BLOCKS < s.nblocks ? lo + GROUP_BLOCKS : s.nblocks;
        return (int)(hi - lo);
    }

    void publish_blocks(loc_host &h, slot &s, int g) {
        if (s.bad || bit(s.sent_blocks, g)) return;
        set_bit(s.sent_blocks, g);
        const int k = group_len(s, g);
        uint8_t   buf[MAX_FRAME];
        put_hdr(buf, T_BLOCKS, (uint8_t)k, h.fp(), k_.s, (uint16_t)s.region, (uint16_t)g);
        memcpy(buf + HDR_BYTES, &s.bh[(uint32_t)g * GROUP_BLOCKS], sizeof(uint64_t) * (size_t)k);
        emit(h, buf, blocks_size(k));
    }

    void publish_bytes(loc_host &h, slot &s, int g, const uint16_t *blocks, int k) {
        if (s.bad || bit(s.sent_bytes, g) || k <= 0) return;
        set_bit(s.sent_bytes, g);
        uint8_t        buf[MAX_FRAME];
        const uint32_t len = h.region_len(s.region);
        const uint8_t *ev  = h.evidence(s.region);
        int            m   = 0;
        for (int j = 0; j < k && m < BYTES_MAX_BLOCKS; ++j) {
            const uint32_t b = blocks[j];
            if (b >= s.nblocks) continue;
            uint8_t *d = buf + HDR_BYTES + BYTES_ENTRY * m;
            memcpy(d, &blocks[j], 2);
            memset(d + 2, 0, 64);
            const uint32_t off = b * 64u;
            const uint32_t n   = len - off < 64u ? len - off : 64u;
            memcpy(d + 2, ev + off, n);
            ++m;
        }
        if (m == 0) return;
        put_hdr(buf, T_BYTES, (uint8_t)m, h.fp(), k_.s, (uint16_t)s.region, (uint16_t)g);
        emit(h, buf, bytes_size(m));
    }

    void on_regions_frame(loc_host &h, int sender, const uint8_t *body) {
        publish_regions(h);
        const int n = h.nregions();
        uint64_t  theirs[MAX_REGIONS];
        memcpy(theirs, body, sizeof(uint64_t) * (size_t)n);
        int  diff[MAX_REGIONS];
        int  nd = 0;
        char names[320];
        int  at  = 0;
        names[0] = 0;
        for (int r = 0; r < n; ++r) {
            if (h.excluded(r) || theirs[r] == k_.per[r]) continue;
            diff[nd++] = r;
            if (at < (int)sizeof(names) - 40) at += snprintf(names + at, sizeof(names) - (size_t)at, " %s", h.region_name(r));
        }
        h.on_regions(sender, k_.s, nd ? diff[0] : -1, nd);
        if (!k_.regions_logged) {
            k_.regions_logged = true;
            if (nd == 0)
                say(h, "; [desync] LOCALISE step=%lu peer=%d: no compared region differs at this step (only excluded "
                       "ones) -- nothing to localise\n",
                    (unsigned long)k_.s, sender);
            else
                say(h, "; [desync] LOCALISE step=%lu peer=%d: %d region(s) differ at the first bad step:%s%s\n",
                    (unsigned long)k_.s, sender, nd, names,
                    k_.evidence_ok ? "" : " (block history gone: region level only)");
        }
        for (int j = 0; j < nd && j < LOC_MAX_REGIONS; ++j)
            if (slot *s = track(h, diff[j])) publish_groups(h, *s);
    }

    void on_groups_frame(loc_host &h, int sender, const hdr &f, const uint8_t *body) {
        slot *s = track(h, (int)f.region);
        if (!s || s->bad) return;
        publish_groups(h, *s);
        const int g = ngroups_of(h.region_len(s->region));
        if ((int)f.count != g) {
            ++ignored_;
            return;
        }
        int opened = 0, more = 0;
        for (int k = 0; k < g; ++k) {
            uint64_t t;
            memcpy(&t, body + 8 * k, 8);
            if (t == s->gh[k]) continue;
            if (opened < LOC_MAX_GROUPS_PER_REGION) {
                ++opened;
                publish_blocks(h, *s, k);
            } else {
                ++more;
            }
        }
        if (more && !s->noted_more) {
            s->noted_more = true;
            say(h, "; [desync] LOCALISE step=%lu peer=%d region=%d %s: %d more differing 4 KB group(s) not opened\n",
                (unsigned long)k_.s, sender, s->region, h.region_name(s->region), more);
        }
    }

    void on_blocks_frame(loc_host &h, int sender, const hdr &f, const uint8_t *body) {
        (void)sender;
        slot *s = track(h, (int)f.region);
        if (!s || s->bad) return;
        const int g = (int)f.aux;
        if (g >= ngroups_of(h.region_len(s->region)) || (int)f.count != group_len(*s, g)) {
            ++ignored_;
            return;
        }
        publish_blocks(h, *s, g);
        uint16_t list[BYTES_MAX_BLOCKS];
        int      k = 0;
        for (int j = 0; j < (int)f.count && k < BYTES_MAX_BLOCKS; ++j) {
            uint64_t t;
            memcpy(&t, body + 8 * j, 8);
            const uint32_t b = (uint32_t)g * GROUP_BLOCKS + (uint32_t)j;
            if (t != s->bh[b]) list[k++] = (uint16_t)b;
        }
        publish_bytes(h, *s, g, list, k);
    }

    void on_bytes_frame(loc_host &h, int sender, const hdr &f, const uint8_t *body) {
        slot *s = track(h, (int)f.region);
        if (!s || s->bad) return;
        const int g = (int)f.aux;
        if (g >= ngroups_of(h.region_len(s->region))) {
            ++ignored_;
            return;
        }
        uint16_t list[BYTES_MAX_BLOCKS];
        for (int j = 0; j < (int)f.count; ++j) memcpy(&list[j], body + BYTES_ENTRY * j, 2);
        publish_bytes(h, *s, g, list, (int)f.count); // the same list: both sides compared the same pair
        const uint32_t len = h.region_len(s->region);
        const uint8_t *ev  = h.evidence(s->region);
        for (int j = 0; j < (int)f.count; ++j) {
            const uint32_t b = list[j];
            if (b >= s->nblocks || b / GROUP_BLOCKS != (uint32_t)g) continue;
            const uint8_t *theirs = body + BYTES_ENTRY * j + 2;
            const uint32_t off0   = b * 64u;
            const uint32_t n      = len - off0 < 64u ? len - off0 : 64u;
            int            first = -1, nk = 0;
            for (uint32_t q = 0; q < n; ++q) {
                if (((ev[off0 + q] ^ theirs[q]) & h.keep(s->region, off0 + q)) == 0) continue;
                if (first < 0) first = (int)q;
                ++nk;
            }
            if (first < 0) continue; // only masked bits differ -- cannot be what moved the hash
            const uint32_t off = off0 + (uint32_t)first;
            if (nres_ < LOC_MAX_LINES) {
                loc_result &r = res_[nres_++];
                r.sender      = sender;
                r.region      = s->region;
                r.off         = off;
                r.block       = b;
                r.nbytes      = nk;
                r.mine        = ev[off];
                r.theirs      = theirs[first];
                char desc[160];
                h.describe(s->region, off, desc, sizeof(desc));
                say(h, "; [desync] LOCALISED step=%lu peer=%d region=%d %s +0x%lX%s%s: %d byte(s) differ in its "
                       "64-byte block %lu, first mine=%02X theirs=%02X\n",
                    (unsigned long)k_.s, sender, s->region, h.region_name(s->region), (unsigned long)off,
                    desc[0] ? " = " : "", desc, nk, (unsigned long)b, (unsigned)r.mine, (unsigned)r.theirs);
            }
        }
    }

    // The active incident (cleared whole on every restart).
    struct incident {
        bool     active;
        bool     evidence_ok;
        bool     sent_regions;
        bool     regions_logged;
        uint32_t s;
        uint64_t per[MAX_REGIONS]; // my per-region hashes at s
    };
    incident   k_;
    int        incidents_     = 0;
    int        max_incidents_ = 0;
    uint32_t   refused_step_  = 0;
    uint32_t   frames_sent_ = 0, bytes_sent_ = 0, ignored_ = 0;
    loc_result res_[LOC_MAX_LINES];
    int        nres_ = 0;
    slot       slots_[LOC_MAX_REGIONS];
};

} // namespace mh::desync::v2
