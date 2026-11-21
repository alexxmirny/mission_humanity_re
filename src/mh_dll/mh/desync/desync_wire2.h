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
    uint64_t manifest_fp; // wire-only hash kind/layout revision/actual geometry fingerprint
    uint32_t step;        // TICK: first step of the batch; incident frames: the incident step S
    uint16_t region;      // incident frames: the hash-manifest index; 0 otherwise
    uint16_t aux;         // GROUPS: window; BLOCKS/BYTES: group in window (body starts with u16 window)
};
#pragma pack(pop)
static_assert(sizeof(hdr) == 24, "v2 header is 24 bytes");
static_assert(offsetof(hdr, step) == offsetof(sample_wire, step), "step sits where v1 keeps it");
static_assert(offsetof(hdr, version) == offsetof(sample_wire, version), "version sits where v1 reads it");

inline constexpr int HDR_BYTES = (int)sizeof(hdr);

// Localization windows are coordinates, not hash/recorder regions. Hash identities stay absolute.
inline constexpr int      TICK_MAX_STEPS   = 32;
inline constexpr int      GROUP_BLOCKS     = 64;
inline constexpr int      MAX_GROUPS       = 128; // groups per 512 KiB window
inline constexpr uint32_t WINDOW_BLOCKS    = (uint32_t)GROUP_BLOCKS * MAX_GROUPS;
inline constexpr int      BYTES_ENTRY      = 2 + 64;
inline constexpr int      BYTES_MAX_BLOCKS = 16;
inline constexpr int      REGIONS_MAX      = MAX_REGIONS;

inline int tick_size(int steps) { return HDR_BYTES + 8 * steps + 8; }
inline int regions_size(int n) { return HDR_BYTES + 8 * n; }
inline int groups_size(int g) { return HDR_BYTES + 8 * g; }
inline int blocks_size(int b) { return HDR_BYTES + 2 + 8 * b; }
inline int bytes_size(int k) { return HDR_BYTES + 2 + BYTES_ENTRY * k; }

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

// BYTES: u16 window followed by at most 16 {u16 block-in-window, byte[64]} entries.
inline constexpr int MAX_FRAME = HDR_BYTES + 2 + BYTES_ENTRY * BYTES_MAX_BLOCKS;
static_assert(MAX_FRAME >= HDR_BYTES + 8 * MAX_GROUPS, "a GROUPS frame fits MAX_FRAME");
static_assert(MAX_FRAME >= HDR_BYTES + 8 * MAX_REGIONS, "a REGIONS frame fits MAX_FRAME");
static_assert(MAX_FRAME <= 2048, "v2 frames stay under MH_NET_MAX_PAYLOAD");

// Wire-only revision and actual runtime extents. The recorder/schema fingerprint is not changed.
inline constexpr uint32_t LOCALIZATION_LAYOUT = 2;
inline uint64_t           fp_v2(uint64_t manifest_fp, uint32_t hash_kind, const uint32_t *lengths, int nregions) {
    uint64_t fp      = fnv1a(&hash_kind, sizeof(hash_kind), manifest_fp);
    fp               = fnv1a(&LOCALIZATION_LAYOUT, sizeof(LOCALIZATION_LAYOUT), fp);
    const uint32_t n = (uint32_t)nregions;
    fp               = fnv1a(&n, sizeof(n), fp);
    return fnv1a(lengths, sizeof(uint32_t) * (size_t)nregions, fp);
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
    virtual uint32_t       peers_present() const               = 0; // current sender-slot bitmask, not incident evidence
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
// Native pump opportunities are spaced >=20 ms. Only novel protocol work refreshes this deadline.
inline constexpr uint32_t LOC_STALL_PUMPS = 256;

inline uint32_t nblocks_of(uint32_t len) { return len / 64u + (len % 64u != 0); }
inline uint32_t ngroups_of(uint32_t len) {
    const uint32_t n = nblocks_of(len);
    return n / GROUP_BLOCKS + (n % GROUP_BLOCKS != 0);
}
inline uint32_t nwindows_of(uint32_t len) {
    const uint32_t n = ngroups_of(len);
    return n / MAX_GROUPS + (n % MAX_GROUPS != 0);
}

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
    // Caller-owned 8-byte aligned arena: group sums plus per-peer page receipt bitmaps.
    static size_t scratch_bytes(uint32_t max_region_len) {
        const size_t groups = ngroups_of(max_region_len);
        const size_t words  = (nwindows_of(max_region_len) + 31u) / 32u;
        return LOC_MAX_REGIONS * (groups * sizeof(uint64_t) + MAX_SENDERS * words * sizeof(uint32_t));
    }
    bool attach(void *mem, size_t bytes, uint32_t max_region_len) {
        const size_t need = scratch_bytes(max_region_len);
        if (!mem || (reinterpret_cast<uintptr_t>(mem) & 7u) || !need || bytes < need) return false;
        capacity_   = max_region_len;
        page_words_ = (nwindows_of(max_region_len) + 31u) / 32u;
        uint8_t *p  = static_cast<uint8_t *>(mem);
        for (slot &s : slots_) {
            s.gh = reinterpret_cast<uint64_t *>(p);
            p += (size_t)ngroups_of(max_region_len) * sizeof(uint64_t);
            s.pages = reinterpret_cast<uint32_t *>(p);
            p += MAX_SENDERS * (size_t)page_words_ * sizeof(uint32_t);
        }
        reset(0);
        return true;
    }
    void reset(int max_incidents) {
        memset(&k_, 0, sizeof(k_));
        max_incidents_ = max_incidents;
        incidents_     = 0;
        refused_step_  = 0;
        frames_sent_ = bytes_sent_ = ignored_ = 0;
        nres_                                 = 0;
        service_serial_ = last_advance_ = 0;
        clear_slots();
    }
    bool              active() const { return k_.active; }
    uint32_t          step() const { return k_.s; }
    int               incidents() const { return incidents_; }
    bool              evidence_ok() const { return k_.evidence_ok; }
    uint32_t          frames_sent() const { return frames_sent_; }
    uint32_t          bytes_sent() const { return bytes_sent_; }
    uint32_t          ignored() const { return ignored_; }
    int               nresults() const { return nres_; }
    uint32_t          omitted_groups() const { return k_.more_groups; }
    bool              stalled() const { return k_.stalled; }
    const loc_result &result(int i) const { return res_[i]; }
    bool              progressing() const {
        if (!k_.active || k_.stalled) return false;
        if (!k_.sent_regions) return true;
        for (const slot &s : slots_) {
            if (s.region < 0 || s.bad) continue;
            if (needed(s) && s.next_page < s.nwindows) return true;
            for (const peer &p : s.peers) {
                if (!p.active) continue;
                if (!p.ready) return true;
                for (int i = 0; i < p.nselected; ++i) {
                    const group &g = p.selected[i];
                    if (!g.sent_blocks || !g.got_blocks || !g.sent_bytes || !g.got_bytes) return true;
                }
            }
        }
        return false;
    }
    // One call per host network service opportunity, never per received frame. One broadcast max.
    bool pump(loc_host &h) {
        if (!k_.active) return false;
        ++service_serial_;
        retire_departed(h);
        if (progressing() && service_serial_ - last_advance_ > LOC_STALL_PUMPS) {
            k_.stalled    = true;
            refused_step_ = k_.s;
            say(h, "; [desync] LOCALISE step=%lu: descent unavailable/stalled after %lu idle service opportunities\n",
                (unsigned long)k_.s, (unsigned long)LOC_STALL_PUMPS);
        }
        if (k_.stalled) return false;
        uint8_t buf[MAX_FRAME];
        if (!k_.sent_regions) {
            k_.sent_regions = true;
            advance();
            put_hdr(buf, T_REGIONS, (uint8_t)h.nregions(), h.fp(), k_.s, 0, 0);
            memcpy(buf + HDR_BYTES, k_.per, sizeof(uint64_t) * (size_t)h.nregions());
            emit(h, buf, regions_size(h.nregions()));
            return true;
        }
        for (slot &s : slots_) {
            if (s.region < 0 || s.bad || !needed(s) || s.next_page >= s.nwindows) continue;
            const uint32_t w = s.next_page++, lo = w * MAX_GROUPS;
            advance();
            const uint32_t n = s.ngroups - lo < MAX_GROUPS ? s.ngroups - lo : MAX_GROUPS;
            put_hdr(buf, T_GROUPS, (uint8_t)n, h.fp(), k_.s, (uint16_t)s.region, (uint16_t)w);
            memcpy(buf + HDR_BYTES, s.gh + lo, n * sizeof(uint64_t));
            emit(h, buf, groups_size((int)n));
            return true;
        }
        for (slot &s : slots_) {
            if (s.region < 0 || s.bad) continue;
            for (peer &p : s.peers) {
                if (!p.active || !p.ready) continue;
                for (int i = 0; i < p.nselected; ++i) {
                    group         &g           = p.selected[i];
                    const uint16_t w           = (uint16_t)(g.id / MAX_GROUPS);
                    const uint16_t local_group = (uint16_t)(g.id % MAX_GROUPS);
                    const bool     recovery    = g.need_recovery && g.got_blocks && !g.got_bytes && !g.recovery_sent;
                    if (!g.sent_blocks || recovery) {
                        if (g.sent_blocks) {
                            g.recovery_sent = true;
                            advance();
                        } else {
                            g.sent_blocks = true;
                            advance();
                        }
                        const int n = group_len(s, g.id);
                        load_group(h, s, g.id);
                        put_hdr(buf, T_BLOCKS, (uint8_t)n, h.fp(), k_.s, (uint16_t)s.region, local_group);
                        memcpy(buf + HDR_BYTES, &w, 2);
                        memcpy(buf + HDR_BYTES + 2, block_scratch_, (size_t)n * sizeof(uint64_t));
                        emit(h, buf, blocks_size(n));
                        return true;
                    }
                    if (!g.got_blocks || g.sent_bytes) continue;
                    g.sent_bytes = true;
                    if (!g.published_bytes) {
                        g.published_bytes = true;
                        advance();
                    }
                    uint16_t  list[BYTES_MAX_BLOCKS];
                    const int n = block_list(g, list);
                    if (!n) {
                        g.got_bytes = true;
                        continue;
                    }
                    put_hdr(buf, T_BYTES, (uint8_t)n, h.fp(), k_.s, (uint16_t)s.region, local_group);
                    memcpy(buf + HDR_BYTES, &w, 2);
                    const uint8_t *ev = h.evidence(s.region);
                    for (int j = 0; j < n; ++j) {
                        uint8_t *d = buf + HDR_BYTES + 2 + BYTES_ENTRY * j;
                        memcpy(d, &list[j], 2);
                        memset(d + 2, 0, 64);
                        const uint32_t b   = (uint32_t)w * WINDOW_BLOCKS + list[j];
                        const uint32_t off = b * 64u, len = h.region_len(s.region);
                        const uint32_t count = len - off < 64u ? len - off : 64u;
                        memcpy(d + 2, ev + off, count);
                    }
                    emit(h, buf, bytes_size(n));
                    return true;
                }
            }
        }
        return false;
    }
    void on_local_mismatch(loc_host &h, uint32_t s) {
        if (s == refused_step_) return; // delayed TICK verdicts cannot resurrect a retired/stalled incident
        if (k_.active) {
            if (s == k_.s) return;
            if (s < k_.s) {
                restart(h, s, false);
                return;
            }
            if (progressing() || s - k_.s < LOC_SETTLE_STEPS) return;
        }
        if (incidents_ < max_incidents_) restart(h, s, true);
    }
    // frame_ok precedes this API; geometry and fingerprint precede all incident/slot mutation.
    void on_frame(loc_host &h, int sender, const hdr &f, const uint8_t *body) {
        if (sender < 0 || sender >= MAX_SENDERS || !body || f.manifest_fp != h.fp() ||
            !geometry_ok(h, f, body)) {
            ++ignored_;
            return;
        }
        if (f.step == refused_step_ || (k_.active && f.step == k_.s && (k_.retired & (1u << sender)))) {
            ++ignored_;
            return;
        }
        if (!k_.active || f.step != k_.s) {
            if ((k_.active && f.step > k_.s) || (!k_.active && incidents_ >= max_incidents_) ||
                f.step == refused_step_) {
                ++ignored_;
                return;
            }
            if (!restart(h, f.step, !k_.active)) return;
        }
        if (f.type == T_REGIONS) {
            on_regions_frame(h, sender, body);
            return;
        }
        slot *s = slot_of(f.region);
        if (!s || s->bad || !s->peers[sender].active) {
            ++ignored_;
            return;
        }
        peer &p = s->peers[sender];
        if (f.type == T_GROUPS) on_groups_frame(h, sender, *s, p, f, body);
        else if (f.type == T_BLOCKS) on_blocks_frame(h, *s, p, f, body);
        else on_bytes_frame(h, sender, *s, p, f, body);
    }

private:
    struct group {
        uint32_t id;
        uint64_t differences;
        bool     sent_blocks, got_blocks, sent_bytes, got_bytes;
        bool     need_recovery, recovery_sent, published_bytes;
    };
    struct early_blocks {
        uint32_t id;
        uint64_t differences;
        bool     got_blocks, need_recovery;
    };
    struct peer {
        bool         active, ready, early_bytes;
        uint32_t     received, ndiff;
        int          nselected, nearly;
        group        selected[LOC_MAX_GROUPS_PER_REGION];
        early_blocks early[LOC_MAX_GROUPS_PER_REGION];
    };
    struct slot {
        int       region = -1;
        bool      bad;
        uint32_t  nblocks, ngroups, nwindows, next_page;
        uint64_t *gh    = nullptr;
        uint32_t *pages = nullptr;
        peer      peers[MAX_SENDERS];
    };
    static bool bit(const uint32_t *b, uint32_t i) { return (b[i / 32] >> (i % 32)) & 1u; }
    static void set_bit(uint32_t *b, uint32_t i) { b[i / 32] |= 1u << (i % 32); }
    static bool needed(const slot &s) {
        for (const peer &p : s.peers)
            if (p.active) return true;
        return false;
    }
    void retire_departed(loc_host &h) {
        const uint32_t present  = h.peers_present();
        uint32_t       departed = 0;
        for (slot &s : slots_) {
            if (s.region < 0) continue;
            for (int sender = 0; sender < MAX_SENDERS; ++sender) {
                peer &p = s.peers[sender];
                if (p.active && !(present & (1u << sender))) {
                    p.active = false;
                    departed |= 1u << sender;
                }
            }
        }
        departed &= ~k_.retired;
        k_.retired |= departed;
        if (departed) {
            advance();
            say(h, "; [desync] LOCALISE step=%lu: retired departed sender mask=%02X\n",
                (unsigned long)k_.s, (unsigned)departed);
        }
    }
    void advance() { last_advance_ = service_serial_; }
    void clear_slots() {
        scratch_region_ = -1;
        for (slot &s : slots_) s.region = -1;
    }
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
    bool geometry_ok(loc_host &h, const hdr &f, const uint8_t *body) const {
        if (f.type == T_REGIONS)
            return f.region == 0 && f.aux == 0 && f.count == h.nregions() && h.nregions() > 0 &&
                   h.nregions() <= MAX_REGIONS;
        if (f.type < T_GROUPS || f.type > T_BYTES || f.region >= h.nregions() || h.excluded(f.region))
            return false;
        const uint32_t len = h.region_len(f.region), ng = ngroups_of(len), nw = nwindows_of(len);
        if (f.type == T_GROUPS) {
            const uint32_t w = f.aux;
            if (w >= nw) return false;
            const uint32_t lo = w * MAX_GROUPS;
            return f.count == (ng - lo < MAX_GROUPS ? ng - lo : MAX_GROUPS);
        }
        uint16_t w;
        memcpy(&w, body, 2);
        if (w >= nw || f.aux >= MAX_GROUPS) return false;
        const uint32_t g = (uint32_t)w * MAX_GROUPS + f.aux;
        if (g >= ng) return false;
        const uint32_t nb = nblocks_of(len), lo = g * GROUP_BLOCKS;
        if (f.type == T_BLOCKS)
            return f.count == (nb - lo < GROUP_BLOCKS ? nb - lo : GROUP_BLOCKS);
        if (!f.count || f.count > BYTES_MAX_BLOCKS) return false;
        uint16_t previous = 0;
        for (int j = 0; j < f.count; ++j) {
            uint16_t b;
            memcpy(&b, body + 2 + BYTES_ENTRY * j, 2);
            if (b >= WINDOW_BLOCKS || b / GROUP_BLOCKS != f.aux ||
                (uint32_t)w * WINDOW_BLOCKS + b >= nb || (j && b <= previous)) return false;
            const uint32_t off = ((uint32_t)w * WINDOW_BLOCKS + b) * 64u;
            const uint32_t n   = len - off < 64u ? len - off : 64u;
            for (uint32_t q = n; q < 64u; ++q)
                if (body[2 + BYTES_ENTRY * j + 2 + q] != 0) return false;
            previous = b;
        }
        return true;
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
        clear_slots();
        nres_ = 0;
        if (new_incident) ++incidents_;
        k_.active = true;
        k_.s      = s;
        advance();
        memcpy(k_.per, per, sizeof(uint64_t) * (size_t)n);
        k_.evidence_ok = h.materialize(s);
        if (!k_.evidence_ok)
            say(h, "; [desync] LOCALISE step=%lu: block history no longer reaches this step -- region level only\n",
                (unsigned long)s);
        return true;
    }
    slot *slot_of(int r) {
        for (slot &s : slots_)
            if (s.region == r) return &s;
        return nullptr;
    }
    slot *track(loc_host &h, int r) {
        if (slot *s = slot_of(r)) return s;
        if (!k_.evidence_ok || h.region_len(r) > capacity_) return nullptr;
        slot *s = nullptr;
        for (slot &v : slots_)
            if (v.region < 0) {
                s = &v;
                break;
            }
        if (!s || !s->gh || !s->pages) return nullptr;
        s->region    = r;
        s->bad       = false;
        s->nblocks   = nblocks_of(h.region_len(r));
        s->ngroups   = ngroups_of(h.region_len(r));
        s->nwindows  = nwindows_of(h.region_len(r));
        s->next_page = 0;
        memset(s->peers, 0, sizeof(s->peers));
        memset(s->pages, 0, MAX_SENDERS * (size_t)page_words_ * sizeof(uint32_t));
        memset(s->gh, 0, (size_t)s->ngroups * sizeof(uint64_t));
        uint64_t sum = 0;
        for (uint32_t b = 0; b < s->nblocks; ++b) {
            const uint64_t hash = h.block_hash(r, b);
            sum += hash;
            s->gh[b / GROUP_BLOCKS] += hash;
        }
        if (sum != k_.per[r]) {
            s->bad = true;
            say(h, "; [desync] LOCALISE step=%lu region=%d %s: SELF-CHECK FAILED -- the state rebuilt from the "
                   "undo journal does not reproduce this region's hash at the step; not localising it\n",
                (unsigned long)k_.s, r, h.region_name(r));
        }
        return s;
    }
    static int group_len(const slot &s, uint32_t g) {
        const uint32_t remain = s.nblocks - g * GROUP_BLOCKS;
        return (int)(remain < GROUP_BLOCKS ? remain : GROUP_BLOCKS);
    }
    void load_group(loc_host &h, const slot &s, uint32_t id) {
        if (scratch_region_ == s.region && scratch_group_ == id) return;
        scratch_region_ = s.region;
        scratch_group_  = id;
        const int n     = group_len(s, id);
        for (int j = 0; j < n; ++j)
            block_scratch_[j] = h.block_hash(s.region, id * GROUP_BLOCKS + (uint32_t)j);
    }
    static group *selected(peer &p, uint32_t id) {
        for (int i = 0; i < p.nselected; ++i)
            if (p.selected[i].id == id) return &p.selected[i];
        return nullptr;
    }
    static early_blocks *early_of(peer &p, uint32_t id) {
        int at = 0;
        while (at < p.nearly && p.early[at].id < id) ++at;
        if (at < p.nearly && p.early[at].id == id) return &p.early[at];
        if (at >= LOC_MAX_GROUPS_PER_REGION) return nullptr;
        if (p.nearly < LOC_MAX_GROUPS_PER_REGION) ++p.nearly;
        for (int j = p.nearly - 1; j > at; --j) p.early[j] = p.early[j - 1];
        p.early[at]    = {};
        p.early[at].id = id;
        return &p.early[at];
    }
    static bool bytes_differ(loc_host &h, const slot &s, const hdr &f, const uint8_t *body) {
        uint16_t w;
        memcpy(&w, body, 2);
        const uint8_t *ev  = h.evidence(s.region);
        const uint32_t len = h.region_len(s.region);
        for (uint32_t j = 0; j < f.count; ++j) {
            const uint8_t *entry = body + 2 + BYTES_ENTRY * j;
            uint16_t       local_block;
            memcpy(&local_block, entry, 2);
            const uint32_t off = ((uint32_t)w * WINDOW_BLOCKS + local_block) * 64u;
            const uint32_t n   = len - off < 64u ? len - off : 64u;
            for (uint32_t q = 0; q < n; ++q)
                if ((ev[off + q] ^ entry[2 + q]) & h.keep(s.region, off + q)) return true;
        }
        return false;
    }
    static void nominate(peer &p, uint32_t id) {
        int at = 0;
        while (at < p.nselected && p.selected[at].id < id) ++at;
        if (at == LOC_MAX_GROUPS_PER_REGION || (at < p.nselected && p.selected[at].id == id)) return;
        if (p.nselected < LOC_MAX_GROUPS_PER_REGION) ++p.nselected;
        for (int i = p.nselected - 1; i > at; --i) p.selected[i] = p.selected[i - 1];
        p.selected[at]    = {};
        p.selected[at].id = id;
    }
    static int block_list(const group &g, uint16_t *list) {
        int n = 0;
        for (uint32_t b = 0; b < GROUP_BLOCKS && n < BYTES_MAX_BLOCKS; ++b)
            if ((g.differences >> b) & 1u)
                list[n++] = (uint16_t)((g.id % MAX_GROUPS) * GROUP_BLOCKS + b);
        return n;
    }
    void on_regions_frame(loc_host &h, int sender, const uint8_t *body) {
        if (k_.regions_seen & (1u << sender)) return;
        k_.regions_seen |= 1u << sender;
        advance();
        int  nd = 0, first = -1;
        char names[320] = {};
        int  at         = 0;
        for (int r = 0; r < h.nregions(); ++r) {
            uint64_t theirs;
            memcpy(&theirs, body + sizeof(uint64_t) * (size_t)r, sizeof(theirs));
            if (h.excluded(r) || theirs == k_.per[r]) continue;
            if (first < 0) first = r;
            if (at < (int)sizeof(names) - 40)
                at += snprintf(names + at, sizeof(names) - (size_t)at, " %s", h.region_name(r));
            if (nd++ < LOC_MAX_REGIONS)
                if (slot *s = track(h, r)) s->peers[sender].active = true;
        }
        h.on_regions(sender, k_.s, first, nd);
        say(h, "; [desync] LOCALISE step=%lu peer=%d: %d region(s) differ at the first bad step:%s%s\n",
            (unsigned long)k_.s, sender, nd, names,
            k_.evidence_ok ? "" : " (block history gone: region level only)");
        if (nd > LOC_MAX_REGIONS)
            say(h, "; [desync] LOCALISE step=%lu peer=%d: %d more differing region(s) not opened\n",
                (unsigned long)k_.s, sender, nd - LOC_MAX_REGIONS);
    }
    void on_groups_frame(loc_host &h, int sender, slot &s, peer &p, const hdr &f, const uint8_t *body) {
        uint32_t *pages = s.pages + (size_t)sender * page_words_;
        if (bit(pages, f.aux)) return;
        set_bit(pages, f.aux);
        ++p.received;
        advance();
        const uint32_t lo = (uint32_t)f.aux * MAX_GROUPS;
        for (uint32_t j = 0; j < f.count; ++j) {
            uint64_t theirs;
            memcpy(&theirs, body + 8 * j, 8);
            if (theirs == s.gh[lo + j]) continue;
            ++p.ndiff;
            nominate(p, lo + j);
        }
        if (p.received != s.nwindows) return;
        p.ready = true;
        for (int i = 0; i < p.nselected; ++i) {
            p.selected[i].need_recovery = p.early_bytes;
            for (int j = 0; j < p.nearly; ++j)
                if (p.selected[i].id == p.early[j].id) {
                    p.selected[i].got_blocks  = p.early[j].got_blocks;
                    p.selected[i].differences = p.early[j].differences;
                    p.selected[i].need_recovery |= p.early[j].need_recovery;
                }
        }
        k_.more_groups += p.ndiff - (uint32_t)p.nselected;
        if (p.ndiff > (uint32_t)p.nselected)
            say(h, "; [desync] LOCALISE step=%lu peer=%d region=%d %s: %lu more differing 4 KB group(s) not opened\n",
                (unsigned long)k_.s, sender, s.region, h.region_name(s.region),
                (unsigned long)(p.ndiff - (uint32_t)p.nselected));
    }
    void on_blocks_frame(loc_host &h, slot &s, peer &p, const hdr &f, const uint8_t *body) {
        uint16_t w;
        memcpy(&w, body, 2);
        const uint32_t id        = (uint32_t)w * MAX_GROUPS + f.aux;
        uint64_t       group_sum = 0;
        for (uint32_t j = 0; j < f.count; ++j) {
            uint64_t hash;
            memcpy(&hash, body + 2 + 8 * j, 8);
            group_sum += hash;
        }
        // Broadcast BLOCKS for another peer's descent are not candidates for our early cache.
        // This is precisely the GROUPS comparison, including sum wrap and keep-mask semantics.
        if (group_sum == s.gh[id]) return;
        uint64_t differences = 0;
        load_group(h, s, id);
        for (uint32_t j = 0; j < f.count; ++j) {
            uint64_t theirs;
            memcpy(&theirs, body + 2 + 8 * j, 8);
            if (theirs != block_scratch_[j]) differences |= uint64_t(1) << j;
        }
        if (!p.ready) {
            // Early receipts do not choose the four-group budget. Retain only the lowest four keys.
            early_blocks *e = early_of(p, id);
            if (!e || e->got_blocks) return;
            e->differences = differences;
            e->got_blocks  = true;
            // A preceding/following BYTES may outrun the final page; request one replay once ready.
            e->need_recovery = true;
            advance();
            return;
        }
        group *g = selected(p, id);
        if (!g) {
            ++ignored_;
            return;
        }
        if (!g->got_blocks) advance();
        g->differences = differences;
        g->got_blocks  = true;
        // Duplicate BLOCKS replay BYTES only. A selected group may make one explicit recovery
        // request after its prerequisites; there is no automatic BLOCKS-on-BLOCKS echo loop.
        g->sent_bytes = false;
        if (!differences) g->got_bytes = true;
    }
    void on_bytes_frame(loc_host &h, int sender, slot &s, peer &p, const hdr &f, const uint8_t *body) {
        uint16_t w;
        memcpy(&w, body, 2);
        const uint32_t id = (uint32_t)w * MAX_GROUPS + f.aux;
        // This single peer-level replay indication survives BYTES -> final GROUPS -> BLOCKS.
        // It cannot occupy/evict a group-cache entry or open a group outside the final four.
        if (!p.ready) {
            if (!p.early_bytes && bytes_differ(h, s, f, body)) {
                p.early_bytes = true;
                advance();
            }
            return;
        }
        group *g = selected(p, id);
        if (!g) {
            ++ignored_;
            return;
        }
        if (!g->got_blocks) {
            // Replay is requested only AFTER BLOCKS establishes membership, so the replay cannot
            // outrun its prerequisite. One request per group; no raw early payload buffer/retry loop.
            if (!g->need_recovery && bytes_differ(h, s, f, body)) {
                g->need_recovery = true;
                advance();
            }
            return;
        }
        uint16_t  list[BYTES_MAX_BLOCKS];
        const int count = block_list(*g, list);
        if (count != f.count) {
            ++ignored_;
            return;
        }
        for (int j = 0; j < count; ++j) {
            uint16_t b;
            memcpy(&b, body + 2 + BYTES_ENTRY * j, 2);
            if (b != list[j]) {
                ++ignored_;
                return;
            }
        }
        if (g->got_bytes) return;
        g->got_bytes = true;
        advance();
        const uint8_t *ev  = h.evidence(s.region);
        const uint32_t len = h.region_len(s.region);
        for (int j = 0; j < count; ++j) {
            const uint32_t b = (uint32_t)w * WINDOW_BLOCKS + list[j], off0 = b * 64u;
            const uint32_t n      = len - off0 < 64u ? len - off0 : 64u;
            const uint8_t *theirs = body + 2 + BYTES_ENTRY * j + 2;
            int            first = -1, nk = 0;
            for (uint32_t q = 0; q < n; ++q) {
                if (!((ev[off0 + q] ^ theirs[q]) & h.keep(s.region, off0 + q))) continue;
                if (first < 0) first = (int)q;
                ++nk;
            }
            if (first < 0 || nres_ >= LOC_MAX_LINES) continue;
            const uint32_t off = off0 + (uint32_t)first;
            res_[nres_++]      = {sender, s.region, off, b, nk, ev[off], theirs[first]};
            char desc[160];
            h.describe(s.region, off, desc, sizeof(desc));
            say(h, "; [desync] LOCALISED step=%lu peer=%d region=%d %s +0x%lX%s%s: %d byte(s) differ in its "
                   "64-byte block %lu, first mine=%02X theirs=%02X\n",
                (unsigned long)k_.s, sender, s.region, h.region_name(s.region), (unsigned long)off,
                desc[0] ? " = " : "", desc, nk, (unsigned long)b, (unsigned)ev[off], (unsigned)theirs[first]);
        }
    }
    struct incident {
        bool     active, evidence_ok, sent_regions, stalled;
        uint32_t s, regions_seen, more_groups, retired;
        uint64_t per[MAX_REGIONS];
    } k_                  = {};
    int        incidents_ = 0, max_incidents_ = 0;
    uint32_t   refused_step_ = 0, frames_sent_ = 0, bytes_sent_ = 0, ignored_ = 0;
    uint32_t   capacity_ = 0, page_words_ = 0;
    uint32_t   service_serial_ = 0, last_advance_ = 0;
    loc_result res_[LOC_MAX_LINES];
    int        nres_ = 0;
    uint64_t   block_scratch_[GROUP_BLOCKS];
    int        scratch_region_ = -1;
    uint32_t   scratch_group_  = 0;
    slot       slots_[LOC_MAX_REGIONS];
};

} // namespace mh::desync::v2
