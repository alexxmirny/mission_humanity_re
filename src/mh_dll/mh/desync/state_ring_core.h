//
// desync/state_ring_core.h -- the PURE core of the mp:D41 state ring: a bounded RAM window of the
// last N seconds of hash-manifest state, held as ONE base image (the state at the window's oldest
// step) plus the STEP chunks (docs/state-record.md v1) of every step after it, and serialised on
// demand into a RING file (flags RING: header, KEYF of the base, the STEP chunks, END).
//
// PURE ON PURPOSE, like state_record.h. No Windows, no game memory, no threads: the game-side ring
// (state_ring.cpp) and the selftest (`net_selftest.exe staterectest`) drive the SAME object, so the
// bound and the round trip the selftest proves are the ones the game runs.
//
// THE DESIGN: ROLL THE OLDEST DELTA INTO THE BASE ("rolling keyframe").
// A ring whose file starts with a keyframe needs the full state at its OLDEST step. Copying the
// 2.8 MB manifest every step to keep one is out of the question, and keeping two keyframe slots
// re-keyed every K steps doubles the window's memory and makes its length saw between N and N+K.
// Instead the base image is kept at the oldest step and moved FORWARD one step at a time: when a
// step ages out of the window (or the byte budget needs its room), its STEP chunk is popped and its
// runs are applied to the base -- the same bytes the reader would apply. So:
//   memory   = base image (sum of region lens, ~2.79 MB)  +  cap_bytes of STEP chunks (fixed, one
//              allocation)  +  the per-step scratch (one step's chunk, typically ~1.3 KB; freed back
//              to SCRATCH_KEEP after an outsized step). Nothing else grows. Measured chunk size: ~1.3 KB
//              per step (431 changed B in ~102 exact runs, coalesced over gaps <= 8, + run headers).
//   per step = build the STEP chunk (the recorder's append) + copy it into the ring + pop and apply
//              ONE chunk of about the same size once the window is full: a few KB of memcpy.
//   window   = exactly keep_steps steps (the base's step is newest - keep_steps), unless the byte
//              budget bites first, which is logged at the dump as the coverage actually held.
// A step whose chunk alone exceeds the run-up budget (a load, a wholesale rewrite) cannot be held as
// a delta: the caller RE-KEYS -- base := the current full state, window empty -- and says so.
//
// THE TWO PHASES. `rolling` keeps the window sliding. trigger() (the first detected desync) freezes
// it: nothing ages out any more, and the next tail_steps steps are appended after it, into the part
// of the cap the run-up was never allowed to use (tail_reserve), so the run-up is never evicted to
// make room for the tail. When the tail is complete (or the cap is full, or the caller must stop),
// detach() hands the storage to whoever writes the file, and the core is empty until alloc() again.
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "desync/state_record.h"

namespace mh::desync::sring {

namespace srec = mh::desync::srec;

// The sim's nominal rate, for turning the [desync] seconds keys into steps (the D40 measurements and
// the desync watch's own cadence comment use the same 50 steps/s).
inline constexpr uint32_t STEPS_PER_S = 50;
// Steps of run-up kept on top of the configured window, so the window still reaches ring_s BEFORE
// the mismatching step once the detection latency (one sample cadence + a link RTT) is paid.
inline constexpr uint32_t DETECT_SLACK_STEPS = 250; // 5 s
inline constexpr size_t   SCRATCH_KEEP       = 256u * 1024u;

struct config {
    uint32_t keep_steps;   // STEP chunks the rolling window holds (its base is newest - keep_steps)
    uint32_t tail_steps;   // steps appended after trigger()
    uint32_t cap_bytes;    // STEP-chunk storage, run-up + tail (one allocation)
    uint32_t tail_reserve; // the part of cap_bytes the run-up may never use
};

enum class phase : uint8_t { empty,
                             rolling,
                             tail };
enum class push : uint8_t {
    ok,
    rekey,     // rolling: this step's chunk does not fit the run-up budget -> caller re-keys at it
    tail_full, // tail: the cap is full -> this step was NOT stored; the caller finishes the dump
    gap,       // the step is not newest+1 -> caller re-keys (rolling) or finishes (tail)
    oom,       // the scratch could not grow -> as `rekey`/finish, and the caller logs it
};

// The storage a finished ring hands to its writer. Owned: release() frees it.
struct frozen {
    uint8_t *base;
    uint64_t base_len;
    uint8_t *ring;
    size_t   cap, head, used;
    uint32_t base_step, last_step, chunks;
    void     release() {
        free(base);
        free(ring);
        base = ring = nullptr;
        cap = head = used = 0;
        chunks            = 0;
    }
};

class core {
public:
    // Allocate the base image and the chunk ring for regions of these lengths (copied). False on OOM
    // (nothing held). Re-alloc with the same shape is a no-op.
    bool alloc(const uint32_t *lens, int n, const config &c) {
        if (base_ && n == n_ && c.cap_bytes == cfg_.cap_bytes) {
            cfg_ = c;
            return true;
        }
        release();
        if (n <= 0 || n > MAXR || c.cap_bytes == 0 || c.tail_reserve >= c.cap_bytes) return false;
        n_     = n;
        total_ = 0;
        for (int i = 0; i < n; ++i) {
            off_[i]  = total_;
            lens_[i] = lens[i];
            total_ += lens[i];
        }
        base_ = static_cast<uint8_t *>(malloc((size_t)total_));
        ring_ = static_cast<uint8_t *>(malloc(c.cap_bytes));
        if (!base_ || !ring_) {
            release();
            return false;
        }
        cfg_ = c;
        cap_ = c.cap_bytes;
        clear();
        return true;
    }
    void release() {
        free(base_);
        free(ring_);
        base_ = ring_ = nullptr;
        cap_          = 0;
        scratch_.release();
        pop_.release();
        clear();
    }
    bool          allocated() const { return base_ != nullptr; }
    const config &cfg() const { return cfg_; }

    // Bytes this ring holds allocated right now (base + chunk cap + both scratches).
    size_t ram_bytes() const { return base_ ? (size_t)total_ + cap_ + scratch_.cap + pop_.cap : 0; }

    // base := the full state (parts[i] = region i's bytes), at `step`; window empty; phase rolling.
    void rekey(uint32_t step, const uint8_t *const *parts) {
        for (int i = 0; i < n_; ++i) memcpy(base_ + off_[i], parts[i], lens_[i]);
        head_ = used_ = 0;
        chunks_       = 0;
        base_step_ = last_step_ = step;
        ph_                     = phase::rolling;
        ++rekeys_;
    }

    // One step's runs, streamed (the step_builder contract: `bytes` points into a contiguous image
    // of the region at `off`, runs ascending).
    void begin_step(uint32_t step) {
        scratch_.len = 0;
        scratch_.oom = false;
        sb_.begin(scratch_, step);
        cur_ = step;
    }
    void run(int region, uint32_t off, uint32_t len, const uint8_t *bytes) { sb_.run(region, off, len, bytes); }

    push end_step() {
        const size_t n = sb_.end();
        push         r = push::ok;
        if (n == 0 || scratch_.oom) r = push::oom;
        else if (cur_ != last_step_ + 1) r = push::gap;
        else if (ph_ == phase::rolling) {
            const size_t budget = cap_ - cfg_.tail_reserve;
            if (n > budget) r = push::rekey;
            else {
                while (chunks_ > 0 && (used_ + n > budget || chunks_ + 1 > cfg_.keep_steps)) {
                    if (chunks_ + 1 <= cfg_.keep_steps) ++budget_pops_; // the BYTES forced it, not age
                    if (!pop_oldest()) {
                        r = push::oom;
                        break;
                    }
                }
                if (r == push::ok) store(n);
            }
        } else if (ph_ == phase::tail) {
            if (used_ + n > cap_) r = push::tail_full;
            else store(n);
        } else {
            r = push::gap;
        }
        if (scratch_.cap > SCRATCH_KEEP) { // an outsized step: give the memory back
            scratch_.release();
        }
        return r;
    }

    // rolling -> tail. `mismatch_step` is the step the desync was detected AT (<= the newest step).
    bool trigger(uint32_t mismatch_step) {
        if (ph_ != phase::rolling) return false;
        ph_            = phase::tail;
        trigger_step_  = last_step_;
        mismatch_step_ = mismatch_step;
        return true;
    }
    bool tail_complete() const { return ph_ == phase::tail && last_step_ - trigger_step_ >= cfg_.tail_steps; }

    // Hand the storage over (the core is empty and unallocated afterwards).
    frozen detach() {
        frozen f;
        f.base      = base_;
        f.base_len  = total_;
        f.ring      = ring_;
        f.cap       = cap_;
        f.head      = head_;
        f.used      = used_;
        f.base_step = base_step_;
        f.last_step = last_step_;
        f.chunks    = chunks_;
        base_ = ring_ = nullptr;
        cap_          = 0;
        scratch_.release();
        pop_.release();
        clear();
        return f;
    }

    // The session is over or restarting: forget the window, keep the allocation.
    void clear() {
        head_ = used_ = peak_ = 0;
        chunks_               = 0;
        base_step_ = last_step_ = trigger_step_ = mismatch_step_ = cur_ = 0;
        ph_                                                             = phase::empty;
        rekeys_ = budget_pops_ = 0;
    }

    phase    ph() const { return ph_; }
    size_t   used() const { return used_; }
    size_t   peak_used() const { return peak_; }
    size_t   cap() const { return cap_; }
    uint32_t base_step() const { return base_step_; }
    uint32_t last_step() const { return last_step_; }
    uint32_t chunks() const { return chunks_; }
    uint32_t trigger_step() const { return trigger_step_; }
    uint32_t mismatch_step() const { return mismatch_step_; }
    uint32_t rekeys() const { return rekeys_; }
    uint32_t budget_pops() const { return budget_pops_; } // chunks evicted by the byte budget, not by age
    uint64_t state_bytes() const { return total_; }

private:
    static constexpr int MAXR = 256;

    void ring_in(const uint8_t *src, size_t n) {
        size_t at = (head_ + used_) % cap_;
        size_t a  = cap_ - at < n ? cap_ - at : n;
        memcpy(ring_ + at, src, a);
        memcpy(ring_, src + a, n - a);
        used_ += n;
    }
    void store(size_t n) {
        ring_in(scratch_.p, n);
        ++chunks_;
        last_step_ = cur_;
        if (used_ > peak_) peak_ = used_;
    }
    // Pop the oldest chunk and apply its runs to the base (base_step advances to its step).
    bool pop_oldest() {
        uint8_t hdr[srec::CHUNK_HDR];
        ring_out(head_, hdr, sizeof(hdr));
        uint32_t plen;
        memcpy(&plen, hdr + 4, 4);
        const size_t tot = srec::CHUNK_HDR + (size_t)plen;
        pop_.len         = 0;
        pop_.oom         = false;
        uint8_t *d       = pop_.grow(tot);
        if (!d) return false;
        ring_out(head_, d, tot);
        apply(d + srec::CHUNK_HDR, plen);
        head_ = (head_ + tot) % cap_;
        used_ -= tot;
        --chunks_;
        if (pop_.cap > SCRATCH_KEEP) pop_.release();
        return true;
    }
    void ring_out(size_t at, uint8_t *dst, size_t n) const {
        size_t a = cap_ - at < n ? cap_ - at : n;
        memcpy(dst, ring_ + at, a);
        memcpy(dst + a, ring_, n - a);
    }
    void apply(const uint8_t *p, uint32_t n) {
        uint32_t step, rc;
        memcpy(&step, p, 4);
        memcpy(&rc, p + 4, 4);
        size_t q = 8;
        for (uint32_t r = 0; r < rc && q + srec::RUN_HDR <= n; ++r) {
            uint16_t reg, len;
            uint32_t off;
            memcpy(&reg, p + q, 2);
            memcpy(&off, p + q + 2, 4);
            memcpy(&len, p + q + 6, 2);
            q += srec::RUN_HDR;
            if (reg < n_ && (uint64_t)off + len <= lens_[reg]) memcpy(base_ + off_[reg] + off, p + q, len);
            q += len;
        }
        base_step_ = step;
    }

    config             cfg_   = {};
    int                n_     = 0;
    uint64_t           total_ = 0;
    uint64_t           off_[MAXR];
    uint32_t           lens_[MAXR];
    uint8_t           *base_ = nullptr;
    uint8_t           *ring_ = nullptr;
    size_t             cap_ = 0, head_ = 0, used_ = 0, peak_ = 0;
    uint32_t           chunks_ = 0, base_step_ = 0, last_step_ = 0, cur_ = 0;
    uint32_t           trigger_step_ = 0, mismatch_step_ = 0;
    uint32_t           rekeys_ = 0, budget_pops_ = 0;
    phase              ph_      = phase::empty;
    srec::buf          scratch_ = {};
    srec::buf          pop_     = {};
    srec::step_builder sb_;
};

// ---- the RING file (docs/state-record.md "Ring dumps") -------------------------------------------
// `put(ctx, p, n)` receives the file's bytes in order; returns false to stop (a failed write).
// `header` is the complete header (put_header with FLAG_RING). The KEYF's CRC is computed over the
// base in place, so nothing the size of the state is copied.
using put_fn = bool (*)(void *ctx, const void *p, size_t n);

inline uint64_t file_bytes(const srec::buf &header, const frozen &f) {
    return header.len + srec::keyf_chunk_bytes(f.base_len) + f.used + srec::CHUNK_HDR + 16;
}

inline bool serialize(const srec::buf &header, const frozen &f, put_fn put, void *ctx) {
    if (!put(ctx, header.p, header.len)) return false;
    uint8_t k[srec::CHUNK_HDR + 4];
    srec::poke32(k, srec::TAG_KEYF);
    srec::poke32(k + 4, (uint32_t)(4 + f.base_len));
    srec::poke32(k + 12, f.base_step);
    srec::poke32(k + 8, srec::crc32(f.base, (size_t)f.base_len, srec::crc32(k + 12, 4)));
    if (!put(ctx, k, sizeof(k)) || !put(ctx, f.base, (size_t)f.base_len)) return false;
    if (f.used) {
        const size_t a = f.cap - f.head < f.used ? f.cap - f.head : f.used;
        if (!put(ctx, f.ring + f.head, a)) return false;
        if (f.used > a && !put(ctx, f.ring, f.used - a)) return false;
    }
    srec::buf e = {};
    srec::put_end(e, f.last_step, f.last_step - f.base_step + 1,
                  header.len + srec::keyf_chunk_bytes(f.base_len) + f.used);
    const bool ok = !e.oom && put(ctx, e.p, e.len);
    e.release();
    return ok;
}

} // namespace mh::desync::sring
