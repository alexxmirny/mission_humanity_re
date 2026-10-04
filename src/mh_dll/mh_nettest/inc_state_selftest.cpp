//
// inc_state_selftest.cpp -- `net_selftest.exe inchashtest`: the incremental state core (mp:D39,
// libmh/state/inc_state.h) against the determinism verdict it replaces.
//
// HOSTED, NOT STANDALONE, and for the same reason statetest is: every arm below moves every hash
// region onto a synthetic buffer with rebase(), so hash_base(i) -- the address the production
// emitters AND the tracker read -- lands in test memory. Standalone every stock base is 0.
//
// THE ORACLE IS THE PRODUCTION EMITTER, NOT A SECOND COPY OF THE MASK TABLE. Clause (a) asks that
// "flipping byte k changes the masked incremental hash IFF it changes the VERDICT stream". Doing that
// literally is O(len^2) per region (524288 bytes of tile_objects alone), so it is done in two steps:
//   1. PROVENANCE. The VERDICT stream of every region is a gather of the region's bytes with bits
//      forced to zero. Run the real emitter (emit_slice, with hash_slice's own mode choice) over
//      address-bit patterns -- byte k = 0xff iff bit b of k is set, for every b -- and each output
//      byte decodes to the input byte it copies and the bits it keeps. That yields, per input byte,
//      the bits that reach the stream: the VERDICT's own keep mask, derived without reading
//      region_view.h's constants.
//   2. THE MODEL IS CHECKED, NOT ASSUMED. Three random fills must reproduce the emitter's stream
//      exactly from (source, keep, constant) -- an emitter that shifted, combined or computed bits
//      would fail here -- and a sample of single-BIT flips (every bit of every byte for slices up
//      to 4 KB, 64 random bits for larger ones) is compared on the REAL stream, full length.
// The core's mask must then equal the derived one bit for bit, and a flip of kept bits must move the
// block hash while a flip of dropped bits must not, for every byte of every region.
//
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "addr/mh_regions.gen.h"
#include "desync/desync_watch.h" // fold_state: the kind-1 fold the kind-2 one must match
#include "orders/order_codec.h"  // the owned order_queue's codec: identity on the raw record
#include "state/inc_state.h"
#include "state/region_view.h"

#include "../mh_harness/hash_kind.h" // tooling:TL-HARN-INCHASH: the harness's kind switch, driven here

using namespace mh::state;
namespace inc = mh::state::inc;

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// Quiet variant for inner loops: count every assertion, print only the first few failures.
int  g_quiet_fails = 0;
void qcheck(bool ok, const char *fmt, int a, uint32_t b, uint32_t c) {
    ++g_checks;
    if (ok) return;
    ++g_fails;
    if (++g_quiet_fails <= 12) {
        printf("  FAIL: ");
        printf(fmt, a, b, c);
        printf("\n");
    }
}

uint32_t g_rng = 0x2545f491u;
uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ---- the synthetic manifest ---------------------------------------------------------------------
// One buffer per REGISTRY region the manifest touches (player_data's 24 slices share one), sized to
// cover every slice's reach, and every such region rebased onto it.
std::vector<std::vector<uint8_t>> g_bufs;
std::vector<int>                  g_rids;

void bind_synthetic() {
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const int rid  = (int)HASH_REGIONS[i].rid;
        bool      seen = false;
        for (int r : g_rids) seen = seen || r == rid;
        if (seen) continue;
        uint32_t need = REGIONS[rid].size;
        for (int j = 0; j < HASH_REGION_COUNT; ++j)
            if ((int)HASH_REGIONS[j].rid == rid && HASH_REGIONS[j].offset + HASH_REGIONS[j].len > need)
                need = HASH_REGIONS[j].offset + HASH_REGIONS[j].len;
        g_rids.push_back(rid);
        g_bufs.emplace_back(need + 64u);
        std::vector<uint8_t> &b = g_bufs.back();
        for (auto &x : b) x = (uint8_t)rnd();
        rebase((region_id)rid, (uint32_t)(uintptr_t)b.data(), need);
    }
}

void unbind_synthetic() {
    for (int rid : g_rids) unrebase((region_id)rid);
}

uint8_t *slice(int i) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(hash_base(i))); }

void set_oq_count(int32_t c) { memcpy(slice(HIDX_ORDER_QUEUE_COUNT), &c, sizeof(c)); }

// ---- the reference VERDICT stream ---------------------------------------------------------------
struct collect_sink final : state_sink {
    explicit collect_sink(sink_mode m) : state_sink(m) {}
    std::vector<uint8_t> v;
    void                 raw(const void *p, uint32_t n) override {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        v.insert(v.end(), b, b + n);
    }
};

bool unmasked_arm(int i, const inc::knobs &k) { // hash_slice's own mode choice, verbatim
    return (i == HIDX_UNITS && !k.mask_ctrl_group) || (i == HIDX_SOLDIERS && !k.mask_soldier_anim) ||
           (i == HIDX_PLANETS && !k.mask_planets_gfx);
}

bool owned(int i) {
    const hash_region &r = HASH_REGIONS[i];
    return owner_serves(r.rid, r.offset, r.len);
}

// The stream hash_slice(i, knobs) hashes. For an UNOWNED slice this is emit_slice itself. For the
// owned order_queue / order_queue_count the raw emitters stand in -- the owner's stream is the same
// bytes (codec identity, asserted below), but its pointers are cached at first use and never point
// at a test buffer.
void ref_stream(int i, const inc::knobs &k, std::vector<uint8_t> &out) {
    collect_sink s(unmasked_arm(i, k) ? sink_mode::PERSIST : sink_mode::VERDICT);
    if (owned(i)) {
        if (i == HIDX_ORDER_QUEUE)
            emit_order_queue(slice(i), HASH_REGIONS[i].len, s);
        else
            s.bytes(slice(i), HASH_REGIONS[i].len);
    } else {
        emit_slice(i, s);
    }
    out.swap(s.v);
}

// ---- clause (a): provenance -> keep mask, checked ------------------------------------------------
struct derived {
    std::vector<uint8_t> keep_in; // per input byte: the bits that reach the stream
    bool                 model_ok = true;
};

void derive_mask(int i, const inc::knobs &k, derived &d, int flip_samples) {
    const uint32_t       L = HASH_REGIONS[i].len;
    uint8_t             *P = slice(i);
    std::vector<uint8_t> Z, K, S;
    memset(P, 0, L);
    ref_stream(i, k, Z);
    memset(P, 0xff, L);
    ref_stream(i, k, K);
    const size_t SL = Z.size();
    qcheck(K.size() == SL, "region %d: stream length depends on content (%u vs %u)", i, (uint32_t)K.size(),
           (uint32_t)SL);
    std::vector<uint8_t>  keep_out(SL);
    std::vector<uint32_t> src(SL, 0);
    for (size_t j = 0; j < SL; ++j) keep_out[j] = (uint8_t)(K[j] & ~Z[j]);
    int bits = 0;
    while ((1u << bits) < L) ++bits;
    for (int b = 0; b < bits; ++b) {
        for (uint32_t x = 0; x < L; ++x) P[x] = ((x >> b) & 1u) ? 0xff : 0x00;
        ref_stream(i, k, S);
        for (size_t j = 0; j < SL; ++j) {
            if (!keep_out[j]) continue;
            const uint8_t v = (uint8_t)(S[j] & keep_out[j]);
            if (v != 0 && v != keep_out[j]) d.model_ok = false; // one output byte mixing two sources
            if (v) src[j] |= 1u << b;
        }
    }
    d.keep_in.assign(L, 0);
    for (size_t j = 0; j < SL; ++j) {
        if (!keep_out[j]) continue;
        if (src[j] >= L) {
            d.model_ok = false;
            continue;
        }
        d.keep_in[src[j]] |= keep_out[j];
    }
    // THE MODEL CHECK: three random fills reproduced from (src, keep, constant) alone.
    std::vector<uint8_t> R(L);
    for (int t = 0; t < 3; ++t) {
        for (uint32_t x = 0; x < L; ++x) R[x] = P[x] = (uint8_t)rnd();
        ref_stream(i, k, S);
        bool same = S.size() == SL;
        for (size_t j = 0; same && j < SL; ++j) {
            const uint8_t want = keep_out[j] ? (uint8_t)((R[src[j]] & keep_out[j]) | Z[j]) : Z[j];
            same               = S[j] == want;
        }
        if (!same) d.model_ok = false;
    }
    qcheck(d.model_ok, "region %d: VERDICT stream is a masked GATHER of its own bytes (%u B, %u B stream)", i, L,
           (uint32_t)SL);
    // DIRECT flip checks on the full real stream (P holds the last random fill R).
    std::vector<uint8_t> base;
    ref_stream(i, k, base);
    const bool exhaustive = L <= 4096;
    const int  nflips     = exhaustive ? (int)(L * 8) : flip_samples;
    for (int f = 0; f < nflips; ++f) {
        const uint32_t kk = exhaustive ? (uint32_t)f / 8u : rnd() % L;
        const uint32_t t  = exhaustive ? (uint32_t)f % 8u : rnd() % 8u;
        P[kk] ^= (uint8_t)(1u << t);
        ref_stream(i, k, S);
        P[kk] ^= (uint8_t)(1u << t);
        const bool changed  = S != base;
        const bool expected = ((d.keep_in[kk] >> t) & 1u) != 0;
        qcheck(changed == expected, "region %d: flipping bit %u of byte %u disagrees with the derived mask", i, t,
               kk);
    }
}

// The core's mask == the derived one, and the block hash honours it for every byte of the slice.
void check_core_mask(int i, const inc::knobs &k, const derived &d, uint32_t oq_live) {
    const uint32_t L   = HASH_REGIONS[i].len;
    uint8_t       *P   = slice(i);
    uint32_t       bad = 0, first_bad = L;
    for (uint32_t x = 0; x < L; ++x)
        if (inc::keep_byte(i, x, L, k, oq_live) != d.keep_in[x]) {
            if (first_bad == L) first_bad = x;
            ++bad;
        }
    qcheck(bad == 0, "region %d: core mask != VERDICT-derived mask at %u byte(s), first at +%u", i, bad, first_bad);
    // Hash-level IFF, every byte: kept bits move the block hash, dropped bits do not.
    uint32_t hbad = 0, hfirst = L;
    for (uint32_t x = 0; x < L; ++x) {
        const uint32_t b    = x / inc::BLOCK;
        const uint64_t h0   = inc::block_hash_masked(i, b, P, L, k, oq_live);
        const uint8_t  keep = inc::keep_byte(i, x, L, k, oq_live);
        bool           ok   = true;
        if (keep) {
            P[x] ^= keep;
            ok = ok && inc::block_hash_masked(i, b, P, L, k, oq_live) != h0;
            P[x] ^= keep;
        }
        if ((uint8_t)~keep) {
            P[x] ^= (uint8_t)~keep;
            ok = ok && inc::block_hash_masked(i, b, P, L, k, oq_live) == h0;
            P[x] ^= (uint8_t)~keep;
        }
        if (!ok) {
            if (hfirst == L) hfirst = x;
            ++hbad;
        }
    }
    qcheck(hbad == 0, "region %d: block hash violates the mask at %u byte(s), first at +%u", i, hbad, hfirst);
}

// ---- clause (b): the recorder mirror + run-shape assertions --------------------------------------
struct mirror_sink {
    std::vector<std::vector<uint8_t>> *m;
    int                                last_region = -1;
    uint32_t                           last_start  = 0;
    uint32_t                           last_end    = 0;
    uint32_t                           gap         = inc::RUN_GAP;
    uint32_t                           shape_bad   = 0;
    uint32_t                           runs        = 0;
    uint32_t                           n_rebased   = 0;
    void                               run(int r, uint32_t off, uint32_t len, const uint8_t *bytes) {
        ++runs;
        if (r < 0 || r >= HASH_REGION_COUNT || len == 0 || len > inc::RUN_MAX || off + len > HASH_REGIONS[r].len) {
            ++shape_bad;
        } else if (r < last_region || (r == last_region && off < last_end)) {
            ++shape_bad; // out of order or overlapping
        } else if (r == last_region && off <= last_end + gap && off + 1 - last_start <= inc::RUN_MAX) {
            ++shape_bad; // two runs that should have been one (a split is legal only at RUN_MAX)
        } else {
            memcpy((*m)[r].data() + off, bytes, len);
        }
        last_region = r;
        last_start  = off;
        last_end    = off + len;
    }
    void rebased(int) { ++n_rebased; }
    void reset_order() {
        last_region = -1;
        last_start = last_end = 0;
    }
};

bool live_equals_mirror(const std::vector<std::vector<uint8_t>> &m) {
    for (int i = 0; i < HASH_REGION_COUNT; ++i)
        if (memcmp(m[i].data(), slice(i), HASH_REGIONS[i].len) != 0) return false;
    return true;
}

bool live_equals_shadow(const inc::tracker &t) {
    for (int i = 0; i < HASH_REGION_COUNT; ++i)
        if (memcmp(t.shadow(i), slice(i), HASH_REGIONS[i].len) != 0) return false;
    return true;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A byte the VERDICT drops, for the "masked write" op: units ctrl_group_id of a random record.
uint32_t a_masked_units_byte() {
    constexpr uint32_t STRIDE = sizeof(mh::game::mh_map_object_unit);
    constexpr uint32_t OFF    = offsetof(mh::game::mh_map_object_unit, ctrl_group_id);
    return (rnd() % (HASH_REGIONS[HIDX_UNITS].len / STRIDE)) * STRIDE + OFF;
}

// ---- tooling:TL-HARN-INCHASH: the harness's hash-kind half (mh_harness/hash_kind.h) --------------
// The harness hashes every sim step with hk::per_fnv (kind 1) or hk::per_inc (kind 2) and folds both
// with hk::fold. Asserted here, over the same synthetic manifest: kind 1 is EXACTLY the loop and the
// fingerprint line it replaced; kind 2 is the core; both localise a write to the same slice and fold
// `state` over the same set; and the two kinds' values differ (so a consumer MUST refuse to compare).
void check_harness_hash_kind(const inc::knobs &kn) {
    namespace hk = mh::harness_hk;
    printf("-- TL-HARN-INCHASH: hash_kind.h (the harness's per-step kind switch)\n");

    // parse_kind: the accepted spellings, and a refusal (0) for anything else.
    check("parse_kind: 1 / fnv / FNV / empty / null -> kind 1",
          hk::parse_kind("1") == 1 && hk::parse_kind("fnv") == 1 && hk::parse_kind(" FNV ") == 1 &&
              hk::parse_kind("") == 1 && hk::parse_kind(nullptr) == 1);
    check("parse_kind: 2 / inc / INC -> kind 2",
          hk::parse_kind("2") == 2 && hk::parse_kind("inc") == 2 && hk::parse_kind("\tINC") == 2);
    check("parse_kind: 0 / 3 / inc2 / garbage / an over-long value -> 0 (refused by name, never defaulted)",
          hk::parse_kind("0") == 0 && hk::parse_kind("3") == 0 && hk::parse_kind("inc2") == 0 &&
              hk::parse_kind("x") == 0 && hk::parse_kind("incremental") == 0);

    // Kind 1 == the replaced loop, verbatim (hash_slice per slice, FNV-1a fold, state skips excluded).
    uint64_t f1[HASH_REGION_COUNT];
    hk::per_fnv(f1, kn);
    uint64_t comb = 1469598103934665603ULL, st = 1469598103934665603ULL;
    bool     same = true;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const uint64_t v = hash_slice(i, kn.mask_ctrl_group, kn.mask_soldier_anim, kn.mask_planets_gfx);
        same             = same && v == f1[i];
        comb             = hk::fnv1a64(&v, 8, comb);
        if (!HASH_REGIONS[i].excluded) st = hk::fnv1a64(&v, 8, st);
    }
    const hk::step_hash h1 = hk::fold(f1);
    check("kind 1: per_fnv is hash_slice per slice under the three knobs", same);
    check("kind 1: fold() is the harness's old combined/state fold", h1.combined == comb && h1.state == st);
    check("fold(): `state` is inc::fold_state / desync::fold_state over the same per[]", h1.state == inc::fold_state(f1));

    // Kind 2 == the core, from a FRESH tracker (the harness attaches its own at arm; update primes).
    std::vector<uint8_t> arena(inc::tracker::arena_bytes());
    static inc::tracker  t2;
    t2.attach(arena.data(), kn);
    uint64_t f2[HASH_REGION_COUNT];
    hk::per_inc(t2, f2);
    bool eq = true, differ = false;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        eq     = eq && f2[i] == inc::recompute_live(i, kn);
        differ = differ || f2[i] != f1[i];
    }
    check("kind 2: the first per_inc primes, and every slice == a from-scratch recompute", eq && t2.primed());
    check("kind 2: fold().state == inc::fold_state (the core's own state hash)",
          hk::fold(f2).state == t2.state_hash());
    check("the two kinds' VALUES differ -- a cross-kind comparison would read as a desync", differ);

    // Both kinds localise one write to the same slice; an excluded slice moves `combined` only.
    int inc_i = -1, exc_i = -1;
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        if (i == HIDX_ORDER_QUEUE_COUNT) continue; // its VALUE re-masks order_queue: two slices move
        if (inc_i < 0 && !HASH_REGIONS[i].excluded && !inc::has_mask(i) && HASH_REGIONS[i].len >= 16) inc_i = i;
        if (exc_i < 0 && HASH_REGIONS[i].excluded && !inc::has_mask(i) && HASH_REGIONS[i].len >= 16) exc_i = i;
    }
    check("premise: an included and an excluded unmasked slice exist", inc_i >= 0 && exc_i >= 0);
    if (inc_i >= 0 && exc_i >= 0) {
        for (int pass = 0; pass < 2; ++pass) {
            const int I = pass ? exc_i : inc_i;
            uint64_t  a1[HASH_REGION_COUNT], a2[HASH_REGION_COUNT], b1[HASH_REGION_COUNT], b2[HASH_REGION_COUNT];
            hk::per_fnv(a1, kn);
            hk::per_inc(t2, a2);
            slice(I)[HASH_REGIONS[I].len / 2] ^= 0x5a; // the D11 poke, in miniature
            hk::per_fnv(b1, kn);
            hk::per_inc(t2, b2);
            int moved1 = 0, moved2 = 0, where1 = -1, where2 = -1;
            for (int i = 0; i < HASH_REGION_COUNT; ++i) {
                if (a1[i] != b1[i]) ++moved1, where1 = i;
                if (a2[i] != b2[i]) ++moved2, where2 = i;
            }
            const hk::step_hash fa1 = hk::fold(a1), fb1 = hk::fold(b1), fa2 = hk::fold(a2), fb2 = hk::fold(b2);
            if (!pass) {
                check("a poke of an INCLUDED slice: both kinds move exactly that slice, and `state`",
                      moved1 == 1 && moved2 == 1 && where1 == I && where2 == I && fa1.state != fb1.state &&
                          fa2.state != fb2.state);
            } else {
                check("a poke of an EXCLUDED slice: both kinds move that slice and `combined`, never `state`",
                      moved1 == 1 && moved2 == 1 && where1 == I && where2 == I && fa1.state == fb1.state &&
                          fa2.state == fb2.state && fa1.combined != fb1.combined && fa2.combined != fb2.combined);
            }
        }
        check("after the pokes: kind 2 still == a from-scratch recompute", t2.verify() == 0);
    }

    // The fingerprint: kind 1 is the pre-item computation, byte for byte; kind 2 is a different value.
    {
        static const uint8_t VEC[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14};
        hash_sink            a(sink_mode::VERDICT);
        a.raw(VEC, sizeof(VEC));
        bool           ok1 = false, ok2 = false;
        const uint64_t fp1 = hk::fingerprint(hk::KIND_FNV, &ok1);
        const uint64_t fp2 = hk::fingerprint(hk::KIND_INC, &ok2);
        check("fingerprint kind 1 == the old VERDICT-sink value over the fixed vector, split ok",
              fp1 == a.finish() && ok1);
        check("fingerprint kind 2: split ok, differs from kind 1, stable", ok2 && fp2 != fp1 &&
                                                                               fp2 == hk::fingerprint(2, nullptr));
        char old[256], l1[256], l2[256];
        std::snprintf(old, sizeof(old), "; HASH FINGERPRINT %08X%08X split=%s input_epoch=%lu build=%s\n",
                      (unsigned)(fp1 >> 32), (unsigned)(fp1 & 0xffffffffu), "ok", 7ul, "0.2.0+abc");
        hk::format_fingerprint_line(l1, sizeof(l1), 1, fp1, true, 7ul, "0.2.0+abc");
        hk::format_fingerprint_line(l2, sizeof(l2), 2, fp2, true, 7ul, "0.2.0+abc");
        check("the kind-1 fingerprint LINE is byte-identical to the pre-item format (no hash_kind token)",
              strcmp(l1, old) == 0);
        check("the kind-2 line carries ` hash_kind=2` after build= (HARNESS_EPOCH_RE still parses it)",
              strstr(l2, " build=0.2.0+abc hash_kind=2\n") != nullptr && strstr(l2, " input_epoch=7 ") != nullptr);
    }

    // The cost line: 3 steps of 2670.0 us at a 10 MHz counter; a shadow; the inc totals.
    {
        hk::cost_acc p, s;
        for (int i = 0; i < 3; ++i) p.add(26700);
        s.add(39570);
        hk::inc_totals t;
        t.steps           = 3;
        t.changed_bytes   = 1296;
        t.rehashed_blocks = 225;
        t.verifies        = 1;
        char l[400];
        hk::format_cost_line(l, sizeof(l), 2, p, 10000000ull, 1, &s, &t);
        check("the HASH COST line states kind, steps, mean/max us, the shadow and the inc totals",
              strstr(l, "; [harness] HASH COST kind=2 steps=3 mean_us=2670.0 max_us=2670.0 total_ms=8") == l &&
                  strstr(l, " | shadow kind=1 steps=1 mean_us=3957.0 max_us=3957.0") != nullptr &&
                  strstr(l, " | inc changed_bytes/step=432 rehashed_blocks/step=75 verify=1 bad=0\n") != nullptr);
        hk::format_cost_line(l, sizeof(l), 1, p, 10000000ull, 2, nullptr, nullptr);
        check("...and without a shadow or a core, only the primary", strchr(l, '|') == nullptr);
    }
}

} // namespace

// ---- mp:D42 mask dump: derive tools/data/state_masks.json FROM keep_byte, not a second hand-kept
//      copy of the mask table. `net_selftest.exe inchashtest --dump-masks <file>` samples keep_byte
//      for every byte of every region (default knobs -- the production VERDICT's own default, all
//      mask arms on) to find each region's periodic keep-pattern, or names it DYNAMIC when the
//      answer depends on the live order_queue_count (today: only order_queue), asserts the derived
//      pattern reproduces keep_byte exactly, and serializes the result deterministically.
//      `--check-masks <file>` re-derives the same text and byte-compares it against a committed
//      copy, so a keep_byte edit that is not followed by a regenerate goes red without either side
//      needing a JSON parser (state_record.py's reader parses the committed file; this dumper never
//      reads its own output back through anything but a byte comparison). See docs/state-record.md
//      and tools/state_record.py's module docstring for the schema this feeds.
namespace {

// More than any known region's real stride (planets, the largest: 0x427 = 1063 bytes). A region
// that needs a longer search is either a future stride this dump does not yet know how to name, or
// not periodic at all -- either way FAILING here (see find_period) beats silently embedding hundreds
// of KB of raw bytes into the committed file.
constexpr uint32_t MASK_PERIOD_CAP = 4096;

struct region_mask {
    std::string          name;
    uint32_t             len         = 0;
    bool                 excluded    = false;
    bool                 dynamic     = false;
    uint32_t             record_size = 0, cap = 0; // dynamic only
    std::string          count_region;             // dynamic only
    uint32_t             stride = 0;               // static only: keep.size()
    std::vector<uint8_t> keep;                     // static only: one period
};

// Does keep_byte's answer for this region depend on the live order_queue count? Sampled at the two
// extremes (oq_live = 0, i.e. nothing live, and oq_live = len, i.e. everything live) over every byte
// -- cheap (len is at most 218400) and exact: keep_byte's only oq_live-conditioned arm is a single
// `<` comparison, so any dependence at all shows up between these two bounds.
bool region_is_dynamic(int i, uint32_t len, const inc::knobs &k) {
    for (uint32_t off = 0; off < len; ++off)
        if (inc::keep_byte(i, off, len, k, 0) != inc::keep_byte(i, off, len, k, len)) return true;
    return false;
}

// The minimal P in [1, cap] with keep_byte(i, off, len, k, 0) == keep_byte(i, off % P, len, k, 0) for
// every off < len, or 0 if none is found within cap.
uint32_t find_period(int i, uint32_t len, const inc::knobs &k, uint32_t cap) {
    const uint32_t limit = len < cap ? len : cap;
    for (uint32_t p = 1; p <= limit; ++p) {
        bool ok = true;
        for (uint32_t off = 0; off < len && ok; ++off)
            ok = inc::keep_byte(i, off, len, k, 0) == inc::keep_byte(i, off % p, len, k, 0);
        if (ok) return p;
    }
    return 0;
}

// Derives every region's mask. Returns false the moment anything cannot be verified (see the
// check() calls for what) -- a caller must not trust `out` for a region that failed, and must not
// commit a file derived while this returned false.
bool derive_masks(std::vector<region_mask> &out) {
    const inc::knobs k; // default: every mask arm on -- what a real recorded match compares under
    bool             ok = true;
    out.clear();
    out.reserve(HASH_REGION_COUNT);
    for (int i = 0; i < HASH_REGION_COUNT; ++i) {
        const hash_region &r = HASH_REGIONS[i];
        region_mask        m;
        m.name     = r.name;
        m.len      = r.len;
        m.excluded = r.excluded;
        m.dynamic  = region_is_dynamic(i, r.len, k);
        if (m.dynamic) {
            // The only formula this dump knows how to name: order_queue's live-byte boundary
            // (inc::order_queue_live_bytes, mirrored from mh::orders::emit_region). A DIFFERENT
            // region going dynamic, or this one's formula no longer matching keep_byte, fails loudly
            // rather than emitting a mask that silently stops describing it.
            const bool is_oq = (i == HIDX_ORDER_QUEUE);
            check("mask dump: the only dynamic region is order_queue", is_oq);
            ok = ok && is_oq;
            if (!is_oq) {
                out.push_back(m);
                continue;
            }
            bool          formula_ok = true;
            const int32_t counts[]   = {-7, 0, 1, 137, 299, 300, 301, 5000};
            for (int32_t c : counts) {
                const uint32_t live = inc::order_queue_live_bytes(c, r.len);
                for (uint32_t off = 0; off < r.len; ++off)
                    formula_ok = formula_ok && inc::keep_byte(i, off, r.len, k, live) == (off < live ? 0xffu : 0u);
            }
            check("mask dump: order_queue's boundary is exactly order_queue_live_bytes(count, len)", formula_ok);
            ok             = ok && formula_ok;
            m.record_size  = ORDER_QUEUE_RECORD;
            m.cap          = ORDER_QUEUE_CAP;
            m.count_region = HASH_REGIONS[HIDX_ORDER_QUEUE_COUNT].name;
        } else {
            const uint32_t p = find_period(i, r.len, k, MASK_PERIOD_CAP);
            char           what[160];
            snprintf(what, sizeof(what), "mask dump: %s has a keep-pattern period <= %u", r.name, MASK_PERIOD_CAP);
            check(what, p != 0);
            ok = ok && (p != 0);
            if (p == 0) {
                out.push_back(m);
                continue;
            }
            m.stride = p;
            m.keep.resize(p);
            for (uint32_t j = 0; j < p; ++j) m.keep[j] = inc::keep_byte(i, j, r.len, k, 0);
            // Reproduction, full length -- the property the whole dump exists to guarantee.
            bool repro = true;
            for (uint32_t off = 0; off < r.len && repro; ++off)
                repro = inc::keep_byte(i, off, r.len, k, 0) == m.keep[off % p];
            char what2[160];
            snprintf(what2, sizeof(what2), "mask dump: %s's period reproduces keep_byte for every byte", r.name);
            check(what2, repro);
            ok = ok && repro;
        }
        out.push_back(m);
    }
    return ok;
}

void json_append_keep_array(std::string &s, const std::vector<uint8_t> &v) {
    s += "[";
    for (size_t j = 0; j < v.size(); ++j) {
        if (j) s += ", ";
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)v[j]);
        s += buf;
    }
    s += "]";
}

// Deterministic (insertion order == HASH_REGIONS order, fixed key order, fixed spacing) so
// --check-masks can byte-compare a fresh derivation against the committed file with no parser.
std::string masks_to_json(const std::vector<region_mask> &regions) {
    std::string s;
    s += "{\n";
    s += "  \"schema_version\": 1,\n";
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", (unsigned)inc::HASH_KIND);
    s += "  \"hash_kind\": " + std::string(buf) + ",\n";
    s += "  \"generated_by\": \"net_selftest.exe inchashtest --dump-masks (mp:D42)\",\n";
    snprintf(buf, sizeof(buf), "%d", HASH_REGION_COUNT);
    s += "  \"region_count\": " + std::string(buf) + ",\n";
    s += "  \"regions\": {\n";
    for (size_t i = 0; i < regions.size(); ++i) {
        const region_mask &m = regions[i];
        s += "    \"" + m.name + "\": {";
        s += m.excluded ? "\"excluded\": true, " : "\"excluded\": false, ";
        snprintf(buf, sizeof(buf), "%u", m.len);
        s += std::string("\"len\": ") + buf;
        if (m.dynamic) {
            char rb[16], cb[16];
            snprintf(rb, sizeof(rb), "%u", m.record_size);
            snprintf(cb, sizeof(cb), "%u", m.cap);
            s += ", \"dynamic\": {\"count_region\": \"" + m.count_region + "\", \"record_size\": " + rb +
                 ", \"cap\": " + cb + "}";
        } else {
            snprintf(buf, sizeof(buf), "%u", m.stride);
            s += std::string(", \"stride\": ") + buf + ", \"keep\": ";
            json_append_keep_array(s, m.keep);
        }
        s += "}";
        s += (i + 1 < regions.size()) ? ",\n" : "\n";
    }
    s += "  }\n";
    s += "}\n";
    return s;
}

bool write_text_file(const char *path, const std::string &text) {
    FILE *fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t n = fwrite(text.data(), 1, text.size(), fp);
    fclose(fp);
    return n == text.size();
}

std::string read_text_file(const char *path, bool &existed) {
    FILE *fp = nullptr;
    existed  = fopen_s(&fp, path, "rb") == 0 && fp != nullptr;
    if (!existed) return std::string();
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string s;
    s.resize(n > 0 ? (size_t)n : 0);
    if (n > 0) fread(&s[0], 1, (size_t)n, fp);
    fclose(fp);
    return s;
}

int run_dump_masks(const char *path) {
    printf("=== inchashtest --dump-masks: deriving %s from keep_byte (mp:D42) ===\n", path);
    std::vector<region_mask> regions;
    const bool               ok_derive = derive_masks(regions);
    if (!ok_derive) {
        printf("  REFUSING to write %s -- the derivation itself failed (see the checks above); the "
               "committed file, if any, is left untouched\n",
               path);
        printf("=== inchashtest --dump-masks: %d checks, %d failures, nothing written ===\n", g_checks, g_fails);
        return 1;
    }
    const std::string json     = masks_to_json(regions);
    const bool        ok_write = write_text_file(path, json);
    if (!ok_write) printf("  FAIL: could not write %s\n", path);
    printf("=== inchashtest --dump-masks: %d checks, %d failures, %u regions, %zu bytes written to %s ===\n",
           g_checks, g_fails, (unsigned)regions.size(), json.size(), path);
    return (ok_write && g_fails == 0) ? 0 : 1;
}

int run_check_masks(const char *path) {
    printf("=== inchashtest --check-masks: re-deriving and comparing against %s (mp:D42) ===\n", path);
    bool              existed   = false;
    const std::string committed = read_text_file(path, existed);
    if (!existed) {
        printf("  FAIL: %s does not exist -- generate it with `inchashtest --dump-masks %s`\n", path, path);
        return 1;
    }
    std::vector<region_mask> regions;
    const bool               ok_derive = derive_masks(regions);
    if (!ok_derive) {
        printf("  FAIL: keep_byte could not be re-derived cleanly (see the checks above) -- fix that before "
               "trusting a comparison against %s\n",
               path);
        return 1;
    }
    const std::string fresh = masks_to_json(regions);
    if (fresh != committed) {
        printf("  FAIL: %s is STALE -- a fresh derivation from keep_byte does not match the committed file.\n", path);
        printf("        Regenerate with: net_selftest.exe inchashtest --dump-masks %s\n", path);
        size_t d = 0;
        while (d < fresh.size() && d < committed.size() && fresh[d] == committed[d]) ++d;
        printf("        first byte differing at offset %zu of %zu (fresh) / %zu (committed)\n", d, fresh.size(),
               committed.size());
        const size_t ctx = 40;
        printf("        fresh:     ...%.*s...\n", (int)ctx, fresh.c_str() + (d < ctx ? 0 : d - 8));
        printf("        committed: ...%.*s...\n", (int)ctx, committed.c_str() + (d < ctx ? 0 : d - 8));
        return 1;
    }
    printf("=== inchashtest --check-masks: IDENTICAL (%zu bytes, %u regions) ===\n", fresh.size(),
           (unsigned)regions.size());
    return 0;
}

} // namespace

int run_inchashtest_full() {
    printf("=== inchashtest (mp:D39: the incremental masked block-sum == the VERDICT comparison) ===\n");
    bind_synthetic();

    // ---- premises ------------------------------------------------------------------------------
    {
        bool all_moved = true;
        for (int i = 0; i < HASH_REGION_COUNT; ++i) all_moved = all_moved && is_rebased(HASH_REGIONS[i].rid);
        check("premise: every hash slice now reads a test buffer", all_moved);
        check("FLAT-BYTES: no hash slice is served by an owner other than mh::orders (first_nonflat_slice == -1)",
              inc::first_nonflat_slice() == -1);
        int n_owned = 0;
        for (int i = 0; i < HASH_REGION_COUNT; ++i) n_owned += owned(i) ? 1 : 0;
        printf("  owned hash slices in this image: %d (order_queue %s, order_queue_count %s)\n", n_owned,
               owned(HIDX_ORDER_QUEUE) ? "owned" : "raw", owned(HIDX_ORDER_QUEUE_COUNT) ? "owned" : "raw");
        // The owned order_queue's stream is codec::encode per record; it must be the raw record.
        bool ident = true;
        for (int t = 0; t < 256; ++t) {
            uint8_t raw[mh::orders::codec::RECORD_BYTES], enc[mh::orders::codec::RECORD_BYTES];
            for (auto &x : raw) x = (uint8_t)rnd();
            mh::orders::order o;
            memcpy(&o, raw, sizeof(o));
            mh::orders::codec::encode(o, enc);
            ident = ident && memcmp(raw, enc, sizeof(raw)) == 0;
        }
        check("FLAT-BYTES: the orders owner's codec::encode is the identity on the raw 68-byte record", ident);
    }

    // ---- fold ----------------------------------------------------------------------------------
    {
        uint64_t per[HASH_REGION_COUNT];
        bool     ex[HASH_REGION_COUNT];
        for (int i = 0; i < HASH_REGION_COUNT; ++i) {
            per[i] = ((uint64_t)rnd() << 32) | rnd();
            ex[i]  = HASH_REGIONS[i].excluded;
        }
        check("fold_state: the kind-2 fold is mh::desync::fold_state over the manifest's excluded column",
              inc::fold_state(per) == mh::desync::fold_state(per, ex, HASH_REGION_COUNT));
        uint64_t per2[HASH_REGION_COUNT];
        memcpy(per2, per, sizeof(per));
        per2[HIDX_PEER_HORIZON] ^= 1; // excluded
        check("fold_state: an EXCLUDED region does not move the state hash",
              inc::fold_state(per2) == inc::fold_state(per));
        per2[HIDX_UNITS] ^= 1;
        check("fold_state: an included region does", inc::fold_state(per2) != inc::fold_state(per));
    }

    // ---- (a) masks == VERDICT, every region, both knob values ----------------------------------
    {
        const auto t0 = std::chrono::steady_clock::now();
        inc::knobs on;
        inc::knobs off;
        off.mask_ctrl_group = off.mask_soldier_anim = off.mask_planets_gfx = false;
        set_oq_count(137);
        int regions_done = 0;
        for (int i = 0; i < HASH_REGION_COUNT; ++i) {
            if (i == HIDX_ORDER_QUEUE) continue; // below, over a range of counts
            const bool knob_region = i == HIDX_UNITS || i == HIDX_SOLDIERS || i == HIDX_PLANETS;
            for (int pass = 0; pass < (knob_region ? 2 : 1); ++pass) {
                const inc::knobs &k = pass ? off : on;
                derived           d;
                derive_mask(i, k, d, 64);
                check_core_mask(i, k, d, 0);
                ++regions_done;
                // Tie the reference to production: the collected stream, FNV'd, IS hash_slice's value.
                if (!owned(i)) {
                    std::vector<uint8_t> s;
                    ref_stream(i, k, s);
                    hash_sink hs(sink_mode::VERDICT);
                    hs.raw(s.data(), (uint32_t)s.size());
                    qcheck(hs.finish() == hash_slice(i, k.mask_ctrl_group, k.mask_soldier_anim, k.mask_planets_gfx),
                           "region %d: the reference stream is not the one hash_slice hashes (%u/%u)", i, 0, 0);
                }
            }
            if (!knob_region && !owned(i))
                qcheck(hash_slice(i, false, false, false) == hash_slice(i, true, true, true),
                       "region %d: a non-knob region's verdict moved with the knobs (%u/%u)", i, 0, 0);
        }
        // order_queue: the mask is a function of order_queue_count's VALUE.
        const int32_t counts[] = {-7, 0, 1, 137, 299, 300, 301, 5000};
        for (int32_t c : counts) {
            set_oq_count(c);
            derived d;
            derive_mask(HIDX_ORDER_QUEUE, on, d, 64);
            check_core_mask(HIDX_ORDER_QUEUE, on, d, inc::order_queue_live_bytes(c, HASH_REGIONS[HIDX_ORDER_QUEUE].len));
            ++regions_done;
        }
        printf("  (a) %d region/knob/count configurations derived from the production emitters in %.0f ms\n",
               regions_done, ms_since(t0));
        check("(a) ran every region (62 + 3 knob-off arms + 8 order_queue counts)", regions_done == 62 + 3 + 8);
    }

    // ---- the tracker ---------------------------------------------------------------------------
    std::vector<uint8_t> arena(inc::tracker::arena_bytes());
    static inc::tracker  tr; // ~1.5 KB of slots; static keeps it off the test's stack
    inc::knobs           kn;
    for (int i = 0; i < HASH_REGION_COUNT; ++i)
        for (uint32_t x = 0; x < HASH_REGIONS[i].len; ++x) slice(i)[x] = (uint8_t)rnd();
    set_oq_count(120);
    tr.attach(arena.data(), kn);
    {
        const auto t0 = std::chrono::steady_clock::now();
        tr.prime();
        const double prime_ms = ms_since(t0);
        const auto   t1       = std::chrono::steady_clock::now();
        const int    bad      = tr.verify();
        printf("  tracker: arena %u bytes, prime %.2f ms, full recompute %.2f ms\n", (unsigned)arena.size(), prime_ms,
               ms_since(t1));
        check("prime: every region hash equals a from-scratch recompute", bad == 0);
    }

    // ---- run shape: gap coalescing, block crossing, exact mode ----------------------------------
    {
        std::vector<std::vector<uint8_t>> mir(HASH_REGION_COUNT);
        for (int i = 0; i < HASH_REGION_COUNT; ++i) mir[i].assign(tr.shadow(i), tr.shadow(i) + HASH_REGIONS[i].len);
        mirror_sink ms{&mir};
        uint8_t    *p = slice(HIDX_STRAT_PLAYERS);
        p[10] ^= 1;
        p[19] ^= 1; // gap of 8 unchanged bytes -> ONE run [10, 20)
        p[60] ^= 1;
        p[68] ^= 1; // a second run [60, 69): crosses the 64-byte block boundary, gap 7 inside it
        tr.update(ms);
        check("runs: a gap of 8 coalesces, runs never overlap or touch within the gap, block crossings merge",
              ms.shape_bad == 0 && ms.runs == 2);
        p[100] ^= 1;
        p[110] ^= 1; // gap 9 -> two runs
        ms.reset_order();
        ms.runs = 0;
        tr.update(ms);
        check("runs: a gap of 9 does not coalesce", ms.shape_bad == 0 && ms.runs == 2);
        p[200] ^= 1;
        p[202] ^= 1;
        ms.reset_order();
        ms.runs = 0;
        ms.gap  = 0;
        tr.update(ms, 0);
        check("runs: gap 0 reports EXACT changed bytes", ms.shape_bad == 0 && ms.runs == 2 &&
                                                             tr.changed_bytes(HIDX_STRAT_PLAYERS) == 2 &&
                                                             tr.first_changed(HIDX_STRAT_PLAYERS) == 200);
        ms.reset_order();
        ms.runs = 0;
        tr.update(ms, 0);
        check("runs: a clean step reports nothing", ms.runs == 0 && tr.last.changed_bytes == 0);
        check("runs: the mirror rebuilt from runs equals live", live_equals_mirror(mir));
    }

    // ---- (b) incremental == recompute after a randomized 10k-step write sequence ---------------
    {
        std::vector<std::vector<uint8_t>> mir(HASH_REGION_COUNT);
        for (int i = 0; i < HASH_REGION_COUNT; ++i) mir[i].assign(tr.shadow(i), tr.shadow(i) + HASH_REGIONS[i].len);
        mirror_sink ms{&mir};
        int         verify_bad = 0, verify_runs = 0, mirror_bad = 0, masked_moved = 0, masked_writes = 0;
        uint64_t    total_blocks = 0;
        double      upd_ms       = 0.0;
        for (int step = 1; step <= 10000; ++step) {
            const int ops = (int)(rnd() % 7);
            for (int o = 0; o < ops; ++o) {
                const uint32_t kind = rnd() % 100;
                if (kind < 45) { // one byte, anywhere
                    const int i                           = (int)(rnd() % HASH_REGION_COUNT);
                    slice(i)[rnd() % HASH_REGIONS[i].len] = (uint8_t)rnd();
                } else if (kind < 70) { // a short run
                    const int      i   = (int)(rnd() % HASH_REGION_COUNT);
                    const uint32_t L   = HASH_REGIONS[i].len;
                    const uint32_t off = rnd() % L;
                    uint32_t       n   = 1 + rnd() % 300;
                    if (off + n > L) n = L - off;
                    for (uint32_t x = 0; x < n; ++x) slice(i)[off + x] = (uint8_t)rnd();
                } else if (kind < 80) { // order_queue_count: moves order_queue's mask
                    set_oq_count((int32_t)(rnd() % 320) - 5);
                } else if (kind < 90) { // a byte the VERDICT drops: the hash must NOT move
                    ms.reset_order();
                    tr.update(ms); // settle this step's earlier writes first
                    const uint64_t before = tr.region_hash(HIDX_UNITS);
                    const uint32_t x      = a_masked_units_byte();
                    slice(HIDX_UNITS)[x] ^= (uint8_t)(1 + rnd() % 255);
                    ms.reset_order();
                    tr.update(ms);
                    ++masked_writes;
                    if (tr.region_hash(HIDX_UNITS) != before) ++masked_moved;
                } else if (kind < 99) { // tile_objects fog / visibility bits only
                    const uint32_t e = (rnd() % (HASH_REGIONS[HIDX_TILE_OBJECTS].len / 8)) * 8;
                    slice(HIDX_TILE_OBJECTS)[e + 1] ^= 0xc0;
                    slice(HIDX_TILE_OBJECTS)[e + 7] = (uint8_t)rnd();
                } else { // a large sweep over a big region
                    const int      i = (rnd() & 1) ? HIDX_TILE_OBJECTS : HIDX_BUILDINGS;
                    const uint32_t L = HASH_REGIONS[i].len, off = rnd() % L;
                    const uint32_t n = (L - off) < 70000u ? (L - off) : 70000u;
                    memset(slice(i) + off, (int)(rnd() & 0xff), n);
                }
            }
            ms.reset_order();
            const auto t0 = std::chrono::steady_clock::now();
            tr.update(ms);
            upd_ms += ms_since(t0);
            total_blocks += tr.last.changed_blocks;
            if (step % 100 == 0 || step == 10000) {
                ++verify_runs;
                if (tr.verify() != 0) ++verify_bad;
                if (!live_equals_mirror(mir) || !live_equals_shadow(tr)) ++mirror_bad;
            }
        }
        printf("  (b) 10000 steps: %.1f us/update mean (%.1f changed blocks/step), %d verifies\n",
               upd_ms * 1000.0 / 10000.0, (double)total_blocks / 10000.0, verify_runs);
        check("(b) incremental == from-scratch recompute at every 100th step and at step 10000", verify_bad == 0);
        check("(b) the recorder mirror rebuilt from runs == live == shadow at every check", mirror_bad == 0);
        check("(b) every run was in range, in order and coalesced over gaps <= 8", ms.shape_bad == 0);
        check("(b) writes to masked bytes never moved the hash", masked_writes > 100 && masked_moved == 0);
    }

    // ---- update() honours the mask per BIT, through the tracker path ---------------------------
    {
        int bad = 0;
        for (int f = 0; f < 2000; ++f) {
            const int      i  = (int)(rnd() % HASH_REGION_COUNT);
            const uint32_t x  = rnd() % HASH_REGIONS[i].len;
            const uint32_t t  = rnd() % 8;
            const uint64_t h0 = tr.region_hash(i);
            const uint8_t  kb = inc::keep_byte(i, x, HASH_REGIONS[i].len, kn,
                                              i == HIDX_ORDER_QUEUE ? inc::live_order_queue_bytes() : 0);
            slice(i)[x] ^= (uint8_t)(1u << t);
            inc::null_sink ns;
            tr.update(ns);
            const bool moved = tr.region_hash(i) != h0;
            // order_queue_count also re-masks order_queue; its OWN hash is all-kept, so moved == true.
            if (moved != (((kb >> t) & 1u) != 0)) ++bad;
        }
        check("update(): a single-bit flip moves the region hash IFF the bit is kept (2000 random bits)", bad == 0);
        check("update(): still equal to recompute afterwards", tr.verify() == 0);
    }

    // ---- the cross-region dependency: order_queue_count re-masks order_queue --------------------
    {
        inc::null_sink ns;
        set_oq_count(10);
        tr.update(ns);
        const uint64_t h10 = tr.region_hash(HIDX_ORDER_QUEUE);
        set_oq_count(250);
        tr.update(ns);
        check("order_queue_count 10 -> 250 moves order_queue's hash with no order_queue byte written",
              tr.region_hash(HIDX_ORDER_QUEUE) != h10 && tr.verify() == 0);
        set_oq_count(10);
        tr.update(ns);
        check("...and back to 10 restores it exactly", tr.region_hash(HIDX_ORDER_QUEUE) == h10);
        set_oq_count(400); // clamps to 300
        tr.update(ns);
        const uint64_t h300 = tr.region_hash(HIDX_ORDER_QUEUE);
        set_oq_count(300);
        tr.update(ns);
        check("a count above 300 clamps (400 hashes as 300)", tr.region_hash(HIDX_ORDER_QUEUE) == h300);
    }

    // ---- rebase re-primes -----------------------------------------------------------------------
    {
        std::vector<std::vector<uint8_t>> mir(HASH_REGION_COUNT);
        for (int i = 0; i < HASH_REGION_COUNT; ++i) mir[i].assign(tr.shadow(i), tr.shadow(i) + HASH_REGIONS[i].len);
        mirror_sink          ms{&mir};
        const int            I   = HIDX_STRAT_PLAYERS;
        const auto           rid = HASH_REGIONS[I].rid;
        const uint32_t       L   = HASH_REGIONS[I].len;
        uint8_t             *old = slice(I);
        const uint64_t       h0  = tr.region_hash(I);
        std::vector<uint8_t> a(old, old + L), b(old, old + L);
        rebase(rid, (uint32_t)(uintptr_t)a.data(), L);
        tr.update(ms);
        check("rebase to identical bytes: counted, no runs, hash unchanged, == recompute",
              tr.last.rebased == 1 && ms.n_rebased == 1 && tr.last.runs == 0 && tr.region_hash(I) == h0 &&
                  tr.verify() == 0);
        b[5] ^= 0x5a;
        b[L - 1] ^= 0x01;
        ms.reset_order();
        rebase(rid, (uint32_t)(uintptr_t)b.data(), L);
        tr.update(ms);
        check("rebase to DIFFERENT bytes: the runs are the difference vs the old shadow, hash == recompute",
              tr.last.rebased == 1 && tr.last.runs == 2 && tr.region_hash(I) != h0 && tr.verify() == 0 &&
                  live_equals_mirror(mir));
        const uint64_t hb = tr.region_hash(I);
        a[7] ^= 0xff; // the ABANDONED buffer
        ms.reset_order();
        tr.update(ms);
        check("a write to the abandoned address is not followed (no stale shadow, no stale base)",
              tr.last.runs == 0 && tr.region_hash(I) == hb && tr.verify() == 0);
        // player_data: ONE registry region, 24 slices -- every one of them re-primes.
        const auto           pd  = HASH_REGIONS[HIDX_P0_LOCAL].rid;
        uint8_t             *pd0 = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(live_base(pd)));
        const uint32_t       pdn = live_size(pd);
        std::vector<uint8_t> moved(pd0, pd0 + pdn);
        moved[HASH_REGIONS[HIDX_P3_LOCAL].offset + 17] ^= 0x33;
        ms.reset_order();
        rebase(pd, (uint32_t)(uintptr_t)moved.data(), pdn);
        tr.update(ms);
        check("player_data rebase: all 24 slices re-prime, the one changed byte is the only run, == recompute",
              tr.last.rebased == 24 && tr.last.runs == 1 && tr.changed_bytes(HIDX_P3_LOCAL) == 1 &&
                  tr.verify() == 0 && live_equals_mirror(mir));
        // order_queue_count moves with a different value: order_queue must re-mask.
        int32_t nc = 33;
        rebase(HASH_REGIONS[HIDX_ORDER_QUEUE_COUNT].rid, (uint32_t)(uintptr_t)&nc, 4);
        ms.reset_order();
        tr.update(ms);
        check("order_queue_count rebased to a new VALUE: order_queue re-masks, == recompute",
              tr.last.rebased == 1 && tr.verify() == 0);
        // Leave nothing pointing at stack/locals for the rest of the suite.
        rebase(pd, (uint32_t)(uintptr_t)pd0, pdn);
        rebase(rid, (uint32_t)(uintptr_t)old, L);
        for (size_t r = 0; r < g_rids.size(); ++r)
            if (g_rids[r] == (int)HASH_REGIONS[HIDX_ORDER_QUEUE_COUNT].rid)
                rebase((region_id)g_rids[r], (uint32_t)(uintptr_t)g_bufs[r].data(), 4);
        inc::null_sink ns;
        tr.update(ns);
        check("rebased back: == recompute", tr.verify() == 0);
    }

    // ---- knobs change the value, as hash_slice's do ---------------------------------------------
    {
        inc::knobs off;
        off.mask_ctrl_group = off.mask_soldier_anim = off.mask_planets_gfx = false;
        const uint32_t x                                                   = a_masked_units_byte();
        slice(HIDX_UNITS)[x] ^= 0x11;
        const uint32_t L    = HASH_REGIONS[HIDX_UNITS].len;
        const uint64_t on0  = inc::region_hash_bytes(HIDX_UNITS, slice(HIDX_UNITS), L, kn, 0);
        const uint64_t off0 = inc::region_hash_bytes(HIDX_UNITS, slice(HIDX_UNITS), L, off, 0);
        slice(HIDX_UNITS)[x] ^= 0x22;
        check("mask_ctrl_group=1: ctrl_group_id is invisible",
              inc::region_hash_bytes(HIDX_UNITS, slice(HIDX_UNITS), L, kn, 0) == on0);
        check("mask_ctrl_group=0: ctrl_group_id is hashed",
              inc::region_hash_bytes(HIDX_UNITS, slice(HIDX_UNITS), L, off, 0) != off0);
    }

    check_harness_hash_kind(kn);

    unbind_synthetic();
    printf("=== inchashtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

// `inchashtest` with no flags runs the full suite above (unchanged: bind_synthetic + every check).
// `--dump-masks <file>` / `--check-masks <file>` (mp:D42) need none of that setup -- keep_byte is a
// pure function of (region, offset, len, knobs, oq_live), never of live memory -- so they run first
// and return without touching the registry.
int run_inchashtest(int argc, char **argv) {
    const char *dump_path = nullptr, *check_path = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-masks") == 0 && i + 1 < argc) dump_path = argv[++i];
        else if (strcmp(argv[i], "--check-masks") == 0 && i + 1 < argc) check_path = argv[++i];
    }
    if (dump_path) return run_dump_masks(dump_path);
    if (check_path) return run_check_masks(check_path);
    return run_inchashtest_full();
}
