//
// state/inc_state.h -- the INCREMENTAL state core (mp:D39): a shadow of the hash manifest, a per-step
// dirty-block scan against it, and a masked SUM-OF-BLOCK-HASHES per region that is maintained by
// patching only the blocks that changed.
//
// WHY. The determinism hash (hash_slice, region_view.h) is a sequential FNV over each region's
// VERDICT stream: 2.8 MB, 3985 us per full walk on a real 66,586-step field replay (commit 631ba431).
// The same replay changed 432 bytes per step on average (~75 64-byte blocks). A chained hash cannot
// use that -- FNV folds every word into everything before it, so a change at offset k re-hashes
// [k, end). A SUM of independent block hashes can: a changed block subtracts its old contribution and
// adds its new one, and the cost of a step becomes the scan (memcmp against the shadow, ~320-440
// us/step measured by the dirty probe) plus ~75 block hashes. The user chose this design over a
// checkpointed FNV on 2026-09-27 (TL-HARN-INCHASH); the price is that the VALUES are new, so every
// consumer carries HASH_KIND and never compares a kind-2 number with a kind-1 one.
//
// WHO CALLS IT. Three consumers, one core:
//   mp:D40  the state recorder (mh.dll)      prime() -> keyframe from shadow(); update(sink) -> the
//                                            sink receives exact changed byte RUNS per step
//   mp:D44  per-step in-band detection       update(null_sink) -> state_hash() every step
//   TL-HARN-INCHASH  the determinism harness  update() -> region_hash(i) as the per-region columns
//
// WHAT IS HASHED. Exactly the bytes the VERDICT stream reads, and nothing else. The VERDICT stream is,
// for every region in today's manifest, a GATHER of the region's own bytes with some bits forced to
// zero (local() fields, masked() bits) and, for tile_objects, a reordering. So it can be restated as a
// per-BYTE keep mask over the raw slice (keep_byte below); the hash covers `byte & keep`. The reorder
// does not matter to an order-free sum -- a block hash is positional, the sum over blocks is not.
// Masked bytes are still SHADOWED and still REPORTED raw to the run sink: the recorder stores raw
// bytes and applies VERDICT offline (the D40 file format). They are only kept out of the hash.
//
// THE MASK TABLE (mirrors region_view.h; the selftest `inchashtest` derives it independently from the
// production emitters and asserts equality bit for bit, under both values of every knob):
//   units        stride 0xe9: +ctrl_group_id                       -> 0x00 when mask_ctrl_group
//   soldiers     stride 0x1d: +0x1b anim_change_count              -> 0x00 when mask_soldier_anim
//   planets      stride 0x427: bank[100] @+0x38d, +0x425..+0x426   -> 0x00 when mask_planets_gfx
//   tile_objects stride 8: +1 -> 0x3f (fog bits 14-15 of the flags word), +7 (visibility) -> 0x00
//   rng_state    slot 1 (+4..+7, the per-frame fx channel)         -> 0x00
//   order_queue  records >= order_queue_count (clamped 0..300)     -> 0x00   (DYNAMIC, see below)
//   units/soldiers/planets/tile_objects/rng_state: bytes past the last WHOLE record are never
//   emitted (the emitters loop `o + STRIDE <= len`) -> 0x00. Today every such len is an exact
//   multiple, so this arm is empty, but it is what the stream says.
//
// THE ONE CROSS-REGION DEPENDENCY. order_queue's keep mask is a function of order_queue_count's
// VALUE (emit_order_queue and mh::orders::emit_region both hash the dead slots as zeros). A byte of
// order_queue_count therefore moves order_queue's hash without any order_queue byte changing. update()
// reads the live count before scanning order_queue and, when the live prefix moved, re-hashes the
// blocks between the old and new boundary from the shadow.
//
// FLAT-BYTES CHECK (D39 scope). Every one of the 63 slices is read at hash_base(i) as flat bytes by
// its VERDICT emitter, with two OWNED regions: order_queue and order_queue_count, served by
// mh::orders::emit_region. That owner emits codec::encode(queue[i]) per record, which is the identity
// on the raw 68-byte record on a little-endian build (order_codec.h static_asserts sizeof == 68;
// inchashtest re-checks the identity), plus the same dead-slot local() rule -- so it is flat bytes too.
// first_nonflat_slice() below names any OTHER owned slice, so a future owner that emits typed fields
// is refused loudly rather than hashed wrong.
//
// BLOCKS ARE REGION-RELATIVE, NOT ADDRESS-ALIGNED, and that is a deliberate break from the probe. The
// dirty probe aligned its 64-byte blocks on the LIVE ADDRESS so its 4 KB grain would mean "dirty
// page". This hash is a WIRE value: two peers (or a peer before and after a host bind) hold the same
// region at different addresses, and address-aligned block boundaries would make the same content hash
// differently. Block b of slice i is bytes [64b, 64b+64) of the slice; the last block may be short and
// its length is mixed in.
//
// 32-BIT BUILD. The block hash is two independent murmur3_32 lanes over the block's sixteen masked
// little-endian 32-bit words -- 32-bit multiplies only (native IMUL), no __allmul in the hot loop. The
// 64-bit part is the running SUM, which is an ADD/ADC pair.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/region_view.h" // TILE_FLAGS_KEEP, RNG_SLOT_FX, ORDER_QUEUE_CAP/_RECORD, owner_serves

namespace mh::state::inc {

// Carried by every consumer beside the value. 1 = the FNV VERDICT walk (hash_slice); 2 = this.
inline constexpr uint32_t HASH_KIND = 2;

inline constexpr uint32_t BLOCK   = 64;      // hash + copy grain (region-relative)
inline constexpr uint32_t CHUNK   = 4096;    // first-level memcmp grain; a clean chunk skips 64 blocks at once
inline constexpr uint32_t RUN_GAP = 8;       // the D40 STEP chunk: the writer merges gaps of <= 8 bytes
inline constexpr uint32_t RUN_MAX = 0xffffu; // STEP chunk runs carry a u16 length

// The harness's three A/B knobs (mh_harness.ini [harness]); false = that region's unmasked arm,
// exactly as hash_slice(i, mask_ctrl_group, mask_soldier_anim, mask_planets_gfx) treats it.
struct knobs {
    bool mask_ctrl_group   = true;
    bool mask_soldier_anim = true;
    bool mask_planets_gfx  = true;
};

// ---- the static masks --------------------------------------------------------------------------

// The byte count of order_queue's LIVE prefix for a given count value: the same clamp as
// emit_order_queue (region_view.h) and mh::orders::emit_region.
inline uint32_t order_queue_live_bytes(int32_t count, uint32_t len) {
    if (count < 0) count = 0;
    if (count > (int32_t)ORDER_QUEUE_CAP) count = (int32_t)ORDER_QUEUE_CAP;
    const uint32_t head = (uint32_t)count * ORDER_QUEUE_RECORD;
    return head > len ? len : head;
}

// The keep bits of byte `off` of slice `i` (length `len`). `oq_live` only matters for order_queue.
inline uint8_t keep_byte(int i, uint32_t off, uint32_t len, const knobs &k, uint32_t oq_live) {
    switch (i) {
        case HIDX_UNITS: {
            constexpr uint32_t STRIDE = sizeof(mh::game::mh_map_object_unit);
            constexpr uint32_t OFF    = offsetof(mh::game::mh_map_object_unit, ctrl_group_id);
            if (off >= len - len % STRIDE) return 0;
            return (k.mask_ctrl_group && off % STRIDE == OFF) ? 0 : 0xff;
        }
        case HIDX_SOLDIERS: {
            constexpr uint32_t STRIDE = sizeof(mh::game::mh_llm_strat_crew_soldier);
            constexpr uint32_t OFF    = offsetof(mh::game::mh_llm_strat_crew_soldier, anim_change_count);
            if (off >= len - len % STRIDE) return 0;
            return (k.mask_soldier_anim && off % STRIDE == OFF) ? 0 : 0xff;
        }
        case HIDX_PLANETS: {
            using planet              = mh::game::mh_cfg_final_struct_Planet;
            constexpr uint32_t STRIDE = sizeof(planet);
            constexpr uint32_t BANK   = offsetof(planet, bank);
            constexpr uint32_t BANK_N = sizeof(planet::bank);
            constexpr uint32_t TLO    = offsetof(planet, tlo_index);
            if (off >= len - len % STRIDE) return 0;
            if (!k.mask_planets_gfx) return 0xff;
            const uint32_t r = off % STRIDE;
            return ((r >= BANK && r < BANK + BANK_N) || r >= TLO) ? 0 : 0xff;
        }
        case HIDX_TILE_OBJECTS: {
            if (off >= (len & ~7u)) return 0;
            const uint32_t r = off & 7u;
            if (r == 0) return (uint8_t)(TILE_FLAGS_KEEP & 0xffu); // flags word, low byte
            if (r == 1) return (uint8_t)(TILE_FLAGS_KEEP >> 8);    // fog bits 14-15 of the flags word
            if (r == 7) return 0;                                  // p[+7], per-frame visibility
            return 0xff;
        }
        case HIDX_RNG_STATE:
            if (off >= (len & ~3u)) return 0;
            return (off / 4u == RNG_SLOT_FX) ? 0 : 0xff;
        case HIDX_ORDER_QUEUE: return off < oq_live ? 0xff : 0;
        default: return 0xff;
    }
}

// Whether slice `i` has ANY non-0xff keep byte for some knob/count -- the hot path skips the mask
// build for the other 57.
inline bool has_mask(int i) {
    return i == HIDX_UNITS || i == HIDX_SOLDIERS || i == HIDX_PLANETS || i == HIDX_TILE_OBJECTS ||
           i == HIDX_RNG_STATE || i == HIDX_ORDER_QUEUE;
}

// The lowest-index hash slice whose VERDICT emission is NOT a gather of its own bytes at
// hash_base(i), or -1. The only owner whose stream is proven flat is mh::orders (order_queue +
// order_queue_count; see the header). Any other owned slice would be hashed from bytes its owner
// does not emit.
inline int first_nonflat_slice() {
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const hash_region &r = HASH_REGIONS[i];
        if (!owner_serves(r.rid, r.offset, r.len)) continue;
        if (r.rid == RID_STRAT_ORDER_QUEUE || r.rid == RID_STRAT_ORDER_QUEUE_COUNT) continue;
        return i;
    }
    return -1;
}

// ---- the block hash ----------------------------------------------------------------------------

inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// h(region, block, masked bytes). `keep` may be null (all 0xff); `n` <= BLOCK, and a short block is
// zero-padded with its length mixed in, so "ab" and "ab\0" differ.
inline uint64_t block_hash(uint32_t region, uint32_t block, const uint8_t *p, uint32_t n, const uint8_t *keep) {
    uint32_t w[BLOCK / 4];
    if (n == BLOCK) {
        memcpy(w, p, BLOCK);
    } else {
        memset(w, 0, sizeof(w));
        memcpy(w, p, n);
    }
    if (keep) {
        uint32_t m[BLOCK / 4];
        if (n == BLOCK) {
            memcpy(m, keep, BLOCK);
        } else {
            memset(m, 0, sizeof(m));
            memcpy(m, keep, n);
        }
        for (uint32_t j = 0; j < BLOCK / 4; ++j) w[j] &= m[j];
    }
    // Two murmur3_32 lanes with unrelated seeds and constants: 32-bit IMULs only.
    uint32_t a = 0x243f6a88u ^ (region * 0x9e3779b1u) ^ block;
    uint32_t b = 0xb7e15162u ^ (block * 0x85ebca77u) ^ (region << 20) ^ n;
    for (uint32_t j = 0; j < BLOCK / 4; ++j) {
        uint32_t k1 = w[j] * 0xcc9e2d51u;
        k1          = rotl32(k1, 15) * 0x1b873593u;
        a ^= k1;
        a           = rotl32(a, 13) * 5u + 0xe6546b64u;
        uint32_t k2 = w[j] * 0x27d4eb2fu;
        k2          = rotl32(k2, 17) * 0x165667b1u;
        b ^= k2;
        b = rotl32(b, 11) * 9u + 0x7a3c5e1du;
    }
    const uint32_t lo = fmix32(a ^ n);
    const uint32_t hi = fmix32(b ^ rotl32(lo, 16));
    return ((uint64_t)hi << 32) | lo;
}

// The keep bytes of block [off, off+n) of slice i into `out` (n <= BLOCK).
inline void fill_keep(int i, uint32_t off, uint32_t n, uint32_t len, const knobs &k, uint32_t oq_live, uint8_t *out) {
    for (uint32_t j = 0; j < n; ++j) out[j] = keep_byte(i, off + j, len, k, oq_live);
}

inline uint64_t block_hash_masked(int i, uint32_t b, const uint8_t *region_bytes, uint32_t len, const knobs &k,
                                  uint32_t oq_live) {
    const uint32_t off = b * BLOCK;
    const uint32_t n   = (len - off) < BLOCK ? (len - off) : BLOCK;
    if (!has_mask(i)) return block_hash((uint32_t)i, b, region_bytes + off, n, nullptr);
    uint8_t keep[BLOCK];
    fill_keep(i, off, n, len, k, oq_live, keep);
    return block_hash((uint32_t)i, b, region_bytes + off, n, keep);
}

// The from-scratch hash of slice i over explicit bytes. Pure: the selftests and recompute_live() both
// land here, so "incremental == recompute" compares against the one definition.
inline uint64_t region_hash_bytes(int i, const uint8_t *p, uint32_t len, const knobs &k, uint32_t oq_live) {
    uint64_t       s  = 0;
    const uint32_t nb = (len + BLOCK - 1) / BLOCK;
    for (uint32_t b = 0; b < nb; ++b) s += block_hash_masked(i, b, p, len, k, oq_live);
    return s;
}

// The live order_queue prefix, read where emit_order_queue reads it.
inline uint32_t live_order_queue_bytes() {
    int32_t c;
    memcpy(&c, reinterpret_cast<const void *>(static_cast<uintptr_t>(hash_base(HIDX_ORDER_QUEUE_COUNT))), sizeof(c));
    return order_queue_live_bytes(c, HASH_REGIONS[HIDX_ORDER_QUEUE].len);
}

// From scratch, over LIVE memory at hash_base(i) -- independent of any shadow, which is the point of
// the periodic self-check: a stale shadow disagrees here.
inline uint64_t recompute_live(int i, const knobs &k) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(hash_base(i)));
    return region_hash_bytes(i, p, HASH_REGIONS[i].len, k, i == HIDX_ORDER_QUEUE ? live_order_queue_bytes() : 0);
}

// The state-only fold: FNV-1a-64 over the 8 bytes of each NON-excluded region hash in manifest order.
// The same algorithm and skip rule as mh::desync::fold_state (inchashtest asserts they agree), so a
// kind-2 state hash is folded exactly as a kind-1 one is.
inline uint64_t fold_state(const uint64_t *per) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        if (HASH_REGIONS[i].excluded) continue;
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&per[i]);
        for (int j = 0; j < 8; ++j) {
            h ^= b[j];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

// ---- the run sink ------------------------------------------------------------------------------
// `run`: bytes [off, off+len) of slice `region` changed this step (coalesced over gaps <= the update's
// `gap`); `bytes` points at the LIVE bytes, valid for the duration of the call. `rebased`: the slice's
// live base moved since the last update (the runs that follow are relative to the OLD shadow content,
// so a recorder needs nothing special -- it is told so it can log it).
struct null_sink {
    void run(int, uint32_t, uint32_t, const uint8_t *) {}
    void rebased(int) {}
};

// ---- the undo journal (mp:D44) ------------------------------------------------------------------
// Optional. When set, update() hands it every 64-byte block's OLD shadow bytes immediately before the
// block is overwritten with the live ones -- i.e. the block as it was at the previous update. That is
// what lets a consumer rebuild the manifest as it stood some steps ago (the in-band detector's live
// localisation needs the state at the FIRST mismatching step, which it only learns about a link
// latency later). prime() does not call it: a prime is the start of history, not a change.
struct journal {
    virtual ~journal()                                                                       = default;
    virtual void old_block(int region, uint32_t block, const uint8_t *old_bytes, uint32_t n) = 0;
};

struct update_stats {
    uint32_t changed_bytes;   // exact, all regions
    uint32_t changed_blocks;  // 64-B blocks whose bytes changed
    uint32_t rehashed_blocks; // blocks whose hash was recomputed (changed + order_queue boundary moves)
    uint32_t runs;            // runs handed to the sink
    uint32_t rebased;         // slices whose live base moved (re-primed)
};

// ---- the tracker -------------------------------------------------------------------------------
// Allocation-free: the caller hands attach() one arena of arena_bytes() (VirtualAlloc in mh.dll, a
// heap block in the selftests), so this header carries no allocator and no Windows dependency.
class tracker {
public:
    static constexpr int N = HASH_REGION_COUNT;

    static size_t arena_bytes() {
        size_t t = 0;
        for (int i = 0; i < N; ++i) t += slot_bytes(HASH_REGIONS[i].len);
        return t;
    }

    void set_journal(journal *j) { j_ = j; }

    void attach(void *arena, const knobs &k) {
        k_         = k;
        primed_    = false;
        uint8_t *m = static_cast<uint8_t *>(arena);
        for (int i = 0; i < N; ++i) {
            region_slot &s = r_[i];
            s.len          = HASH_REGIONS[i].len;
            s.nblocks      = (s.len + BLOCK - 1) / BLOCK;
            s.bh           = reinterpret_cast<uint64_t *>(m);
            s.shadow       = m + (size_t)s.nblocks * 8u;
            s.base         = 0;
            s.sum          = 0;
            bytes_[i]      = 0;
            first_[i]      = s.len;
            m += slot_bytes(s.len);
        }
        memset(&last, 0, sizeof(last));
    }

    // Copy every live slice into the shadow and hash it from scratch.
    void prime() {
        for (int i = 0; i < N; ++i) {
            region_slot &s = r_[i];
            s.base         = hash_base(i);
            memcpy(s.shadow, live(i), s.len);
            first_[i] = s.len;
            bytes_[i] = 0;
        }
        oq_live_ = live_order_queue_bytes();
        for (int i = 0; i < N; ++i) rehash_all(i);
        memset(&last, 0, sizeof(last));
        primed_ = true;
    }

    // One step: scan live vs shadow, patch the changed blocks' hashes, copy them into the shadow, and
    // hand the changed byte runs to `sink` (gap 0 = exact runs). Primes on first use.
    template <class Sink>
    void update(Sink &sink, uint32_t gap = RUN_GAP) {
        memset(&last, 0, sizeof(last));
        if (!primed_) {
            prime();
            return;
        }
        for (int i = 0; i < N; ++i) {
            region_slot   &s     = r_[i];
            const uint32_t now   = hash_base(i);
            const bool     moved = now != s.base;
            if (moved) {
                s.base = now;
                ++last.rebased;
                sink.rebased(i);
            }
            uint32_t oq_lo = 0, oq_hi = 0; // order_queue blocks to re-mask: [oq_lo, oq_hi)
            if (i == HIDX_ORDER_QUEUE) {
                const uint32_t nl = live_order_queue_bytes();
                if (nl != oq_live_) {
                    oq_lo    = nl < oq_live_ ? nl : oq_live_;
                    oq_hi    = nl < oq_live_ ? oq_live_ : nl;
                    oq_live_ = nl;
                }
            }
            scan_region(i, sink, gap);
            if (moved) {
                rehash_all(i); // re-prime: the whole block array from the (now current) shadow
                continue;
            }
            if (oq_hi > oq_lo) {
                const uint32_t b1 = (oq_hi + BLOCK - 1) / BLOCK;
                for (uint32_t b = oq_lo / BLOCK; b < b1 && b < s.nblocks; ++b) rehash(i, b);
            }
        }
    }

    uint64_t region_hash(int i) const { return r_[i].sum; }
    void     region_hashes(uint64_t *out) const {
        for (int i = 0; i < N; ++i) out[i] = r_[i].sum;
    }
    uint64_t state_hash() const {
        uint64_t per[N];
        region_hashes(per);
        return fold_state(per);
    }

    // The periodic self-check: recompute every slice from LIVE memory and count disagreements with the
    // incremental sums. `first_bad` (optional) receives the first disagreeing index, or -1.
    // Returns the number of NON-excluded slices whose incremental sum disagrees with a recompute. An
    // EXCLUDED slice that disagrees is counted in `*bad_excluded` instead, and is not an instrument
    // fault: the excluded set is the peer-local, in-flight state (the lockstep horizons, the frame
    // ring, the pacing clocks), and some of it is written OFF the sim thread. Measured on the rig
    // (2026-09-27, 2-peer determinism run): `ls_horizon` moved between update() and this recompute,
    // which run back to back in one function on the sim thread -- so another thread wrote it.
    int verify(int *first_bad = nullptr, int *bad_excluded = nullptr) const {
        int bad = 0, first = -1, bad_ex = 0;
        for (int i = 0; i < N; ++i) {
            if (recompute_live(i, k_) == r_[i].sum) continue;
            if (HASH_REGIONS[i].excluded) {
                ++bad_ex;
                continue;
            }
            if (first < 0) first = i;
            ++bad;
        }
        if (first_bad) *first_bad = first;
        if (bad_excluded) *bad_excluded = bad_ex;
        return bad;
    }

    const uint8_t *shadow(int i) const { return r_[i].shadow; }
    uint32_t       changed_bytes(int i) const { return bytes_[i]; } // last update, exact
    uint32_t       first_changed(int i) const { return first_[i]; } // last update; len = clean
    bool           primed() const { return primed_; }
    const knobs   &mask_knobs() const { return k_; }

    update_stats last;

private:
    struct region_slot {
        uint8_t  *shadow;
        uint64_t *bh;
        uint32_t  len, nblocks, base;
        uint64_t  sum;
    };

    static size_t slot_bytes(uint32_t len) {
        const size_t nb = (len + BLOCK - 1) / BLOCK;
        return nb * 8u + (((size_t)len + 7u) & ~(size_t)7u);
    }

    static const uint8_t *live(int i) { return reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(hash_base(i))); }

    void rehash(int i, uint32_t b) {
        region_slot &s = r_[i];
        s.sum -= s.bh[b];
        s.bh[b] = block_hash_masked(i, b, s.shadow, s.len, k_, oq_live_);
        s.sum += s.bh[b];
        ++last.rehashed_blocks;
    }

    void rehash_all(int i) {
        region_slot &s = r_[i];
        s.sum          = 0;
        for (uint32_t b = 0; b < s.nblocks; ++b) {
            s.bh[b] = block_hash_masked(i, b, s.shadow, s.len, k_, oq_live_);
            s.sum += s.bh[b];
        }
    }

    template <class Sink>
    void scan_region(int i, Sink &sink, uint32_t gap) {
        region_slot   &s    = r_[i];
        const uint8_t *p    = live(i);
        uint8_t       *sh   = s.shadow;
        bool           have = false;
        uint32_t       rs = 0, re = 0, nbytes = 0, first = s.len;
        for (uint32_t c = 0; c < s.len; c += CHUNK) {
            const uint32_t cn = (s.len - c) < CHUNK ? (s.len - c) : CHUNK;
            if (memcmp(p + c, sh + c, cn) == 0) continue;
            for (uint32_t off = c; off < c + cn; off += BLOCK) {
                const uint32_t n = (c + cn - off) < BLOCK ? (c + cn - off) : BLOCK;
                if (memcmp(p + off, sh + off, n) == 0) continue;
                for (uint32_t q = off; q < off + n; ++q) {
                    if (p[q] == sh[q]) continue;
                    ++nbytes;
                    if (first == s.len) first = q;
                    if (have && q - re <= gap && q + 1 - rs <= RUN_MAX) {
                        re = q + 1;
                        continue;
                    }
                    if (have) emit_run(sink, i, p, rs, re);
                    rs   = q;
                    re   = q + 1;
                    have = true;
                }
                if (j_) j_->old_block(i, off / BLOCK, sh + off, n);
                memcpy(sh + off, p + off, n);
                ++last.changed_blocks;
                rehash(i, off / BLOCK);
            }
        }
        if (have) emit_run(sink, i, p, rs, re);
        bytes_[i] = nbytes;
        first_[i] = first;
        last.changed_bytes += nbytes;
    }

    template <class Sink>
    void emit_run(Sink &sink, int i, const uint8_t *p, uint32_t rs, uint32_t re) {
        ++last.runs;
        sink.run(i, rs, re - rs, p + rs);
    }

    region_slot r_[N];
    uint32_t    bytes_[N];
    uint32_t    first_[N];
    knobs       k_;
    uint32_t    oq_live_ = 0;
    bool        primed_  = false;
    journal    *j_       = nullptr;
};

} // namespace mh::state::inc
