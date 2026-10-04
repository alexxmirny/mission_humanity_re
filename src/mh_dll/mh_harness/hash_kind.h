//
// mh_harness/hash_kind.h -- WHICH FUNCTION the harness hashes every sim step with (tooling:TL-HARN-INCHASH).
//
// TWO KINDS, ONE MANIFEST, ONE FOLD.
//   kind 1  the FNV VERDICT walk: mh::state::hash_slice over every hash-manifest slice, every step.
//           The value every stored artifact carries -- libref fixtures, soak goldens, UI-REC oracles,
//           the rc4/rc5 field recordings -- and the one every standalone re-deriver computes
//           (libref_host, world::lockstep_hash, the in-band desync watch).
//   kind 2  the incremental core (mp:D39, state/inc_state.h): a masked SUM of 64-byte block hashes
//           per slice, patched from a per-step dirty scan against a shadow. Same bytes (the core's
//           keep masks are proved equal to the VERDICT stream by `net_selftest.exe inchashtest`),
//           different VALUES: an order-free sum cannot equal a sequential FNV.
// Both kinds produce one 64-bit value per manifest slice (`per[]`), and the per-step `combined` /
// `state` columns are the SAME FNV-1a-64 fold over `per[]` for both (state skips `excluded`), so the
// log's shape -- step lines, R lines, RD rows, the stop breakdown -- is identical and only the numbers
// differ. That is exactly why every consumer carries the kind beside the numbers and refuses to
// compare across it.
//
// WHERE THE KIND IS WRITTEN. On the `; HASH FINGERPRINT` line: kind 1 prints the line EXACTLY as it
// was before this item (so every kind-1 log, golden and arm-order baseline is byte-unchanged) and a
// reader takes "no `hash_kind=` token" as kind 1; kind 2 appends ` hash_kind=2`, and its fingerprint
// VALUE is the block hash's, so even a reader that only compares fingerprints refuses the pair. A
// match segment (mh_match_harness.log) repeats the token on its `; [match] segment OPEN` line, since
// the fingerprint line lives in the process log. LAST statement wins: if kind 2 is refused at the
// first step (see first_nonflat_slice in inc_state.h), the harness writes a second, kind-1
// fingerprint line before any hash line.
//
// SCOPE. The STRATEGIC hash manifest only. The tactical T/TS/TR lines (tact_hash_all) hash a
// different manifest the incremental core does not cover and stay FNV whatever the knob says; the
// SNAPCAP/SNAPIMP rows and the world blob's lockstep pair stay FNV too, because their other half is
// re-derived by code that only knows FNV (snapshot import, world::lockstep_hash, libref_host).
//
// HEADER-ONLY AND FREE OF THE MODULE, like config1.h: `net_selftest.exe inchashtest` drives these
// exact functions over its synthetic manifest (every slice rebased onto a test buffer).
//
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "state/inc_state.h"
#include "state/region_view.h"

namespace mh::harness_hk {

inline constexpr int KIND_FNV = 1;
inline constexpr int KIND_INC = 2;
static_assert(mh::state::inc::HASH_KIND == (uint32_t)KIND_INC, "the core's HASH_KIND is kind 2");

inline const char *kind_name(int k) {
    return k == KIND_INC ? "incremental block sum" : k == KIND_FNV ? "FNV VERDICT walk" : "unknown";
}

// `[harness] hash_kind` -> 1 or 2, or 0 when the value is not one of the spellings below. Accepted,
// case-insensitive, surrounding blanks ignored: "1" / "fnv" and "2" / "inc". An empty value is kind 1
// (the key absent). A 0 is REFUSED by the caller by name, never silently read as a default.
inline int parse_kind(const char *s) {
    if (s == nullptr) return KIND_FNV;
    while (*s == ' ' || *s == '\t') ++s;
    char   b[8];
    size_t n = 0;
    while (s[n] && n < sizeof(b) - 1) {
        char c = s[n];
        b[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (s[n]) return 0; // longer than any accepted spelling
    while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t')) --n;
    b[n] = '\0';
    if (n == 0) return KIND_FNV;
    if (!strcmp(b, "1") || !strcmp(b, "fnv")) return KIND_FNV;
    if (!strcmp(b, "2") || !strcmp(b, "inc")) return KIND_INC;
    return 0;
}

// ---- the fold (both kinds) ----------------------------------------------------------------------
// HASH-INPUT BEGIN harness_hash_kind_step (tools/data/hash_input_epoch.json)

inline uint64_t fnv1a64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

struct step_hash {
    uint64_t combined; // every slice, manifest order
    uint64_t state;    // the non-`excluded` slices only
};

// The harness's per-step fold, verbatim: FNV-1a-64 over the 8 bytes of each slice hash in manifest
// order. `state` equals inc::fold_state / mh::desync::fold_state over the same per[] (inchashtest).
inline step_hash fold(const uint64_t *per) {
    step_hash h{1469598103934665603ULL, 1469598103934665603ULL};
    for (int i = 0; i < mh::state::HASH_REGION_COUNT; ++i) {
        h.combined = fnv1a64(&per[i], sizeof(per[i]), h.combined);
        if (!mh::state::HASH_REGIONS[i].excluded) h.state = fnv1a64(&per[i], sizeof(per[i]), h.state);
    }
    return h;
}

// ---- the per-slice hashes -----------------------------------------------------------------------

// Kind 1: the VERDICT walk, one hash_slice per slice, under the harness's three mask knobs.
inline void per_fnv(uint64_t *per, const mh::state::inc::knobs &k) {
    for (int i = 0; i < mh::state::HASH_REGION_COUNT; ++i)
        per[i] = mh::state::hash_slice(i, k.mask_ctrl_group, k.mask_soldier_anim, k.mask_planets_gfx);
}

// Kind 2: one incremental step (primes on first use), then the running sums.
inline void per_inc(mh::state::inc::tracker &t, uint64_t *per) {
    mh::state::inc::null_sink s;
    t.update(s);
    t.region_hashes(per);
}
// HASH-INPUT END harness_hash_kind_step

// ---- the fingerprint ----------------------------------------------------------------------------
// A fingerprint of the LIVE hash implementation over a fixed vector, so a consumer that stores hashes
// for a later run can refuse a comparison across an algorithm change nobody remembered to announce.

inline const uint8_t *fp_vec() {
    static const uint8_t VEC[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14};
    return VEC;
}

// Kind 1 -- the pre-TL-HARN-INCHASH computation, unchanged: the VERDICT sink over the vector, and the
// same vector fed in a different split (the sink must be split-invariant).
inline uint64_t fingerprint_fnv(bool *split_ok) {
    const uint8_t        *v = fp_vec();
    mh::state::hash_sink a(mh::state::sink_mode::VERDICT);
    a.raw(v, 21);
    mh::state::hash_sink b(mh::state::sink_mode::VERDICT);
    b.raw(v, 3);
    b.raw(v + 3, 8);
    b.raw(v + 11, 10);
    if (split_ok) *split_ok = a.finish() == b.finish();
    return a.finish();
}

// Kind 2 -- the block hash over the cases it can get wrong: a whole block, a short tail (length mixed
// in), and a masked block; and the "split" check pins the partition: a region's sum is the sum of its
// 64-byte blocks, region-relative, the last one short. Index 0xffffffff has no mask arm.
inline uint64_t fingerprint_inc(bool *split_ok) {
    namespace inc = mh::state::inc;
    uint8_t       buf[85];
    for (uint32_t j = 0; j < sizeof(buf); ++j) buf[j] = (uint8_t)(j * 7u + 3u);
    uint8_t keep[inc::BLOCK];
    for (uint32_t j = 0; j < inc::BLOCK; ++j) keep[j] = (j % 3u) ? 0xffu : 0x0fu;
    const inc::knobs k;
    const uint64_t   whole  = inc::region_hash_bytes(-1, buf, sizeof(buf), k, 0);
    const uint64_t   parts  = inc::block_hash(0xffffffffu, 0, buf, inc::BLOCK, nullptr) +
                          inc::block_hash(0xffffffffu, 1, buf + inc::BLOCK, sizeof(buf) - inc::BLOCK, nullptr);
    const uint64_t   tail   = inc::block_hash(0, 0, fp_vec(), 21, nullptr);
    const uint64_t   masked = inc::block_hash(7, 3, buf, inc::BLOCK, keep);
    if (split_ok) *split_ok = whole == parts;
    uint64_t h = 1469598103934665603ULL;
    h          = fnv1a64(&whole, 8, h);
    h          = fnv1a64(&tail, 8, h);
    h          = fnv1a64(&masked, 8, h);
    return h;
}

inline uint64_t fingerprint(int kind, bool *split_ok) {
    return kind == KIND_INC ? fingerprint_inc(split_ok) : fingerprint_fnv(split_ok);
}

// `; HASH FINGERPRINT <fp> split=<ok|BROKEN> input_epoch=<E> build=<B>[ hash_kind=2]`. Kind 1 is
// byte-for-byte the line every earlier build wrote (mp_analyze HARNESS_EPOCH_RE, soak_test
// hash_fingerprint and the arm-order templates read it); only kind 2 carries the token.
inline int format_fingerprint_line(char *out, size_t cap, int kind, uint64_t fp, bool split_ok,
                                   unsigned long epoch, const char *build) {
    char tail[24] = "";
    if (kind != KIND_FNV) std::snprintf(tail, sizeof(tail), " hash_kind=%d", kind);
    return std::snprintf(out, cap, "; HASH FINGERPRINT %08X%08X split=%s input_epoch=%lu build=%s%s\n",
                         (unsigned)(fp >> 32), (unsigned)(fp & 0xffffffffu), split_ok ? "ok" : "BROKEN", epoch,
                         build ? build : "?", tail);
}

// ---- the cost ledger (item 4: before/after per-step cost, read off the run's own log) ----------

struct cost_acc {
    uint64_t ticks = 0, max_ticks = 0;
    uint32_t n     = 0;
    void     add(uint64_t t) {
        ticks += t;
        if (t > max_ticks) max_ticks = t;
        ++n;
    }
};

// Tenths of a microsecond, so a 267 us mean prints as `267.0` and a 3957 us one as `3957.0`.
inline unsigned long tenths_us(uint64_t ticks, uint64_t qpf, uint32_t n) {
    if (qpf == 0 || n == 0) return 0;
    return (unsigned long)((ticks * 10000000ULL) / qpf / n);
}

// `; [harness] HASH COST kind=2 steps=N mean_us=X.Y max_us=X.Y total_ms=T[ | shadow kind=1 steps=..
// mean_us=.. max_us=..][ | inc changed_bytes/step=.. rehashed_blocks/step=.. verify=V bad=B]`
// `shadow` = the other kind computed beside the primary only to time it (`hash_cost_both=1`).
struct inc_totals {
    uint64_t changed_bytes = 0, rehashed_blocks = 0;
    uint32_t steps = 0, verifies = 0, verify_bad = 0;
};

inline int format_cost_line(char *out, size_t cap, int kind, const cost_acc &prim, uint64_t qpf, int shadow_kind,
                            const cost_acc *shadow, const inc_totals *inc) {
    const unsigned long pm = tenths_us(prim.ticks, qpf, prim.n), px = tenths_us(prim.max_ticks, qpf, 1);
    int w = std::snprintf(out, cap, "; [harness] HASH COST kind=%d steps=%lu mean_us=%lu.%lu max_us=%lu.%lu total_ms=%lu",
                          kind, (unsigned long)prim.n, pm / 10, pm % 10, px / 10, px % 10,
                          (unsigned long)(qpf ? prim.ticks * 1000ULL / qpf : 0));
    if (w < 0 || (size_t)w >= cap) return w;
    if (shadow && shadow->n) {
        const unsigned long sm = tenths_us(shadow->ticks, qpf, shadow->n), sx = tenths_us(shadow->max_ticks, qpf, 1);
        w += std::snprintf(out + w, cap - (size_t)w, " | shadow kind=%d steps=%lu mean_us=%lu.%lu max_us=%lu.%lu",
                           shadow_kind, (unsigned long)shadow->n, sm / 10, sm % 10, sx / 10, sx % 10);
        if (w < 0 || (size_t)w >= cap) return w;
    }
    if (inc && inc->steps) {
        w += std::snprintf(out + w, cap - (size_t)w,
                           " | inc changed_bytes/step=%lu rehashed_blocks/step=%lu verify=%lu bad=%lu",
                           (unsigned long)(inc->changed_bytes / inc->steps),
                           (unsigned long)(inc->rehashed_blocks / inc->steps), (unsigned long)inc->verifies,
                           (unsigned long)inc->verify_bad);
        if (w < 0 || (size_t)w >= cap) return w;
    }
    w += std::snprintf(out + w, cap - (size_t)w, "\n");
    return w;
}

} // namespace mh::harness_hk
