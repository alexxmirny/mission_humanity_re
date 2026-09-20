//
// world_snapshot_selftest.cpp -- the LIB-WORLD step-0 world fixture, watched to FAIL (tracker
// LIB-WORLD).
//
// THE CLAIM UNDER TEST, in the done_when's own words: importing the blob into a second in-binary
// process and hashing the bound regions reproduces the RECORDING PEER'S step-0 lockstep hash
// exactly -- and corrupting one field of the blob changes that hash, with the report naming the
// region.
//
// WHY THE SECOND PROCESS IS THIS ONE AND NOT A SECOND GAME. A second mh.exe replaying the same
// scenario would reproduce the step-0 hash by its OWN boot determinism, whatever the blob said, and
// the run would look like a pass while proving nothing about the fixture. This process has no game,
// no INIT.CFG and no map: it allocates an arena, binds every carried region onto it, POISONS it, and
// the only route from there to the recording peer's number is through the blob. The poison is what
// makes it a claim rather than a coincidence -- importing over zeroes would let a block that is
// never written still agree with a zero-filled expectation, and 0xCD agrees with nothing.
//
// THREE ARM KINDS, and the middle one exists because the first is not enough (LIB-BOOT proved it):
//
//   A. HASH-SLICE MUTATION. For each of the determinism-hash slices in turn, flip a byte INSIDE the
//      slice's window and require the imported LOCKSTEP hash to move, naming the region. This is the
//      done_when's negative arm. It walks candidate offsets rather than poking one, because a
//      masked field (fog bit, ctrl_group, the planets gfx windows) legitimately does NOT move the
//      hash -- and a slice where NO offset moves it is precisely the blind spot this arm is for, so
//      that is a FAILURE naming the slice rather than a silent skip.
//   B. PER-BLOCK MUTATION against the blob's own canonical hash -- all 829 blocks, so no block is
//      outside the blob's comparison. (The lockstep hash cannot see the other ~790: they are real
//      world state the fixture must carry, and no determinism slice reads them.)
//   C. PER-BLOCK CONTENT. Imported memory must equal the blob payload byte-for-byte. Carried from
//      LIB-BOOT, where mutating import() to SKIP a block showed arm B alone cannot see a
//      never-written block: poison-vs-mutated is also a hash difference, so B stayed green over a
//      block the importer had quietly stopped writing.
//
//   R. THE RNG CHANNELS (mp:R-rng-snapshot / refinement-plan X0, added 2026-09-17). The three arms
//      above ask whether the blob CARRIES the world; this one asks whether an importer can CONTINUE
//      it. R0 is a table gate -- every RNG channel region the sim reads is a named block. R1 is the
//      stream: seed the channels, spend N draws, capture, poison, import, and require the next 16
//      draws on channels 0/1/2 to be peer A's, bit for bit. See the ARM R banner further down.
//
// WHAT THIS FILE CANNOT DO. Without a `<blob>` argument there is no game and no recording, so the
// fixture is SYNTHESISED here: that arm proves the format, the refusals, the round trip and the
// coverage of all three comparisons. The clause that the imported world equals A REAL RECORDING
// PEER'S is `worldtest <blob>` against a capture the game wrote at step 0 -- two different claims,
// two different fixtures, exactly as boottest splits them.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "state/host_bind.h"      // pulls in libmh/include/libmh.h for libmh_region_bind
#include "state/boot_snapshot.h"  // the SHARED session-begun latch -- see the ordering arm
#include "state/region_runtime.h" // clear_region -- see empty_nav_pool() below
#include "state/world_snapshot.h"

// ARM R (the RNG channels, tracker mp:R-rng-snapshot / refinement-plan X0) draws through the
// TRANSLATED PRNG bodies rather than re-deriving the recurrence here, for the same reason
// sim_lt_rng_draws.h gives: one state, one implementation. A second copy of ROR16(state+0x9248,3)
// living in a test would agree with itself forever while the shipped body drifted.
#include "sim_test_support.h"                 // mh::sim::sim_fixture -- the offline store
#include "sim/libtrans/sim_lt_rng_draws.h"    // rand_below (ch0), rand_below_fx (ch1)
#include "sim/libtrans/sim_lt_rng_raw_step.h" // rand_below_ai (ch2), rand_state_advance

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

using namespace mh::state;
using namespace mh::state::world;

// ---- the arena ---------------------------------------------------------------------------------
//
// Every carried region bound onto memory this process owns. Not a convenience: the stock bases are
// game .bss addresses a console process cannot materialise (state_selftest.cpp's banner has the
// measurement -- the loader and the CRT heap own that whole span before main runs). Binding is also
// what the STANDALONE host does, so the fixture exercises the real path rather than a hosted
// shortcut.
struct arena {
    uint8_t *mem = nullptr;
    size_t   len = 0;

    bool bind_blocks() {
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) len += WORLD_SNAPSHOT_BLOCKS[i].len;
        mem = static_cast<uint8_t *>(std::malloc(len));
        if (mem == nullptr) return false;
        std::memset(mem, 0, len);
        libmh_region_bind *binds = static_cast<libmh_region_bind *>(
            std::malloc(sizeof(libmh_region_bind) * WORLD_SNAPSHOT_BLOCK_COUNT));
        if (binds == nullptr) return false;
        size_t off = 0;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
            binds[i].region_id = static_cast<uint32_t>(WORLD_SNAPSHOT_BLOCKS[i].rid);
            binds[i].base      = mem + off;
            binds[i].size      = WORLD_SNAPSHOT_BLOCKS[i].len;
            binds[i].count     = 0;
            off += WORLD_SNAPSHOT_BLOCKS[i].len;
        }
        const int rc = libmh_bind_regions(binds, WORLD_SNAPSHOT_BLOCK_COUNT);
        std::free(binds);
        return rc == WORLD_SNAPSHOT_BLOCK_COUNT;
    }

    void fill(uint8_t v) { std::memset(mem, v, len); }
    ~arena() { std::free(mem); }
};

// Deterministic pseudo-content, so a synthesised "capture" has structure rather than a constant. A
// constant-filled fixture would pass a mutation arm that a shifted-by-one importer also passes.
uint8_t pattern(size_t i) {
    return static_cast<uint8_t>((i * 131u + (i >> 8) * 17u + 7u) & 0xffu);
}

// The offset of a block's PAYLOAD inside a blob, and its length -- read out of the BLOB'S OWN header
// words rather than recomputed, so the arms walk it the way any third-party consumer has to.
bool block_span(const uint8_t *blob, int idx, size_t *off, uint32_t *len) {
    size_t o = sizeof(blob_header);
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        uint32_t l = 0;
        std::memcpy(&l, blob + o + 4, 4);
        if (i == idx) {
            *off = o + 8u;
            *len = l;
            return true;
        }
        o += 8u + l;
    }
    return false;
}

// The payload offset of a HASH SLICE -- its region's block, plus the slice's own offset inside it.
bool slice_span(const uint8_t *blob, int h, size_t *off, uint32_t *len) {
    const hash_region &r = HASH_REGIONS[h];
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        if (WORLD_SNAPSHOT_BLOCKS[i].rid != r.rid) continue;
        size_t   boff = 0;
        uint32_t blen = 0;
        if (!block_span(blob, i, &boff, &blen)) return false;
        if (r.offset + r.len > blen) return false;
        *off = boff + r.offset;
        *len = r.len;
        return true;
    }
    return false;
}

uint8_t *dup_blob(const uint8_t *blob, size_t n) {
    uint8_t *m = static_cast<uint8_t *>(std::malloc(n));
    std::memcpy(m, blob, n);
    return m;
}

// ---- ARM A: the done_when's negative arm --------------------------------------------------------
//
// For every hash slice, find a byte whose corruption MOVES the imported lockstep hash. Offsets are
// walked rather than guessed: a masked or `local()` field legitimately does not move it, and a slice
// where nothing does is the blind spot -- reported as a failure naming the slice, never skipped.
int arm_hash_slice_mutation(arena &a, const uint8_t *blob, size_t n, uint32_t masks) {
    // THE BASELINE IS A CLEAN IMPORT IN THIS PROCESS, not the value recorded in the header, and that
    // distinction is not pedantry -- it was MEASURED. The first version of this arm compared each
    // mutated import against the header's recorded hash, and a deliberate mutation that dropped one
    // slice from lockstep_hash()'s fold left it reporting 61/61 GREEN: with the whole fold shifted,
    // every probe "moved" the number away from a value nothing was going to reproduce anyway. An arm
    // whose reference is a constant cannot tell "this byte matters" from "nothing matches any more".
    // Against a clean-import baseline the same mutation makes the dropped slice report BLIND, which
    // is the answer the arm exists to give. (The header value is still checked -- by the primary
    // clause, which is a different question asked of a different number.)
    uint64_t want_combined = 0;
    a.fill(0xCD);
    if (import(blob, n) != WORLD_OK) {
        printf("  FAIL: arm A could not import the clean blob for its baseline\n");
        return 1;
    }
    lockstep_hash(masks, &want_combined, nullptr);

    // Spread the probes rather than clustering at the head: a record-structured slice can have a
    // masked field at a fixed stride, and 24 evenly-spaced offsets plus the two ends cover both the
    // small scalars and the big arrays without walking 2.79 MB per slice.
    const int PROBES = 26;
    int       caught = 0, blind = 0;
    for (int h = 0; h < HASH_REGION_COUNT; ++h) {
        size_t   off = 0;
        uint32_t len = 0;
        if (!slice_span(blob, h, &off, &len) || len == 0) {
            ++blind;
            printf("  FAIL: slice %d (%s) has no carried block -- it cannot be in the comparison\n",
                   h, HASH_REGIONS[h].name);
            continue;
        }
        bool moved = false;
        for (int k = 0; k < PROBES && !moved; ++k) {
            const uint32_t o   = (len <= 1u) ? 0u : (uint32_t)((uint64_t)len * k / PROBES);
            uint8_t       *mut = dup_blob(blob, n);
            mut[off + o] ^= 0x5Au;
            a.fill(0xCD);
            if (import(mut, n) == WORLD_OK) {
                uint64_t got = 0;
                lockstep_hash(masks, &got, nullptr);
                if (got != want_combined) moved = true;
            }
            std::free(mut);
        }
        if (moved) {
            ++caught;
        } else {
            ++blind;
            printf("  FAIL: slice %d (%s, %u B) -- NO probed byte moved the imported lockstep hash. "
                   "Either the slice is entirely masked, or the blob is not what the hash reads.\n",
                   h, HASH_REGIONS[h].name, len);
        }
    }
    printf("  arm A (hash-slice mutation): %d/%d slices move the imported lockstep hash\n", caught,
           HASH_REGION_COUNT);
    return blind;
}

// ---- ARM B: per-block mutation against the blob's own canonical hash ----------------------------
int arm_block_mutation(arena &a, const uint8_t *blob, size_t n, uint64_t want_content) {
    int seen = 0, missed = 0;
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        size_t   off = 0;
        uint32_t len = 0;
        if (!block_span(blob, i, &off, &len) || len == 0) {
            ++missed;
            printf("  FAIL: block %d (%s) has no payload span\n", i, WORLD_SNAPSHOT_BLOCKS[i].name);
            continue;
        }
        uint8_t *mut = dup_blob(blob, n);
        mut[off + len / 2u] ^= 0x5Au;
        a.fill(0xCD);
        if (import(mut, n) != WORLD_OK) {
            ++missed;
            printf("  FAIL: block %d (%s) -- the mutated blob would not import\n", i,
                   WORLD_SNAPSHOT_BLOCKS[i].name);
        } else if (canonical_hash() != want_content) {
            ++seen;
        } else {
            ++missed;
            printf("  FAIL: block %d (%s) -- a flipped byte did NOT move the content hash\n", i,
                   WORLD_SNAPSHOT_BLOCKS[i].name);
        }
        std::free(mut);
    }
    printf("  arm B (per-block mutation): %d/%d blocks inside the comparison\n", seen,
           WORLD_SNAPSHOT_BLOCK_COUNT);
    return missed;
}

// ---- ARM C: per-block CONTENT ------------------------------------------------------------------
//
// LIB-BOOT's lesson, carried forward verbatim: arm B alone cannot see a block the importer never
// writes, because poison-vs-mutated is ALSO a hash difference. This compares imported memory against
// the blob payload byte-for-byte.
int arm_block_content(arena &a, const uint8_t *blob, size_t n) {
    a.fill(0xCD);
    if (import(blob, n) != WORLD_OK) {
        printf("  FAIL: arm C could not import the clean blob\n");
        return 1;
    }
    int wrong = 0;
    for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i) {
        size_t   off = 0;
        uint32_t len = 0;
        if (!block_span(blob, i, &off, &len)) continue;
        const uint8_t *live = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(live_base(WORLD_SNAPSHOT_BLOCKS[i].rid)));
        if (live == nullptr || std::memcmp(live, blob + off, len) != 0) {
            ++wrong;
            if (wrong <= 5)
                printf("  FAIL: block %d (%s) -- imported memory differs from the blob payload\n", i,
                       WORLD_SNAPSHOT_BLOCKS[i].name);
        }
    }
    printf("  arm C (per-block content): %d checked, %d wrong\n", WORLD_SNAPSHOT_BLOCK_COUNT, wrong);
    return wrong;
}

// ---- the nav carriers, emptied before a synthetic capture ---------------------------------------
//
// FORMAT 2's capture serializes the live map-region pool, which it can only reach by WALKING it from
// MAP_REGION_LIST_HEAD and MAP_REGION_POOL_FREE_HEAD. Those two are ordinary bound regions, so a
// pattern-filled arena puts pseudo-random bytes in them -- a non-null pointer to nowhere,
// dereferenced on the first hop. (Measured: rc=139, no output.) A synthetic arena has no pool, so
// saying so explicitly is both the truth and the fix. BY_INDEX and the GRID are cleared for the same
// reason: every non-null entry in them must be a pool node or the capture refuses.
void empty_nav_pool() {
    static const mh::state::region_id NAV_CARRIERS[] = {
        mh::state::RID_MAP_REGION_LIST_HEAD, mh::state::RID_MAP_REGION_POOL_FREE_HEAD,
        mh::state::RID_MAP_REGION_BY_INDEX, mh::state::RID_MAP_REGION_GRID};
    for (size_t i = 0; i < sizeof(NAV_CARRIERS) / sizeof(NAV_CARRIERS[0]); ++i)
        mh::state::clear_region(NAV_CARRIERS[i]);
}

// ================================================================================================
// ARM R -- THE RNG CHANNELS (tracker mp:R-rng-snapshot, refinement-plan X0)
// ================================================================================================
//
// WHAT THE ITEM ACTUALLY ASKS, AND WHY THE TWO HALVES ARE SEPARATE ARMS. "Add RNG channels to the
// resync snapshot" is two claims wearing one sentence:
//
//   R0 -- MEMBERSHIP. Every RNG channel the strategic sim READS is a named member of the snapshot
//         set. That is a statement about the generated tables, it is checkable without running a
//         single draw, and it is the half that ROTS: the world block table is generated from
//         tools/data/world_snapshot_schema.json + _dispositions.json, so a future exclusion could
//         drop the divisor or the seed byte and every other arm here would stay green (the
//         GENERATOR already refuses to exclude a region backing a determinism-hash slice, which
//         covers rng_state and NOTHING ELSE -- the other two back no slice). This arm is the hard
//         gate that makes such an exclusion a red test rather than a silent loss.
//
//   R1 -- THE STREAM. Carrying the bytes is not the claim anybody cares about; REPRODUCING THE
//         DRAWS is. A second peer that imports a step-N capture and then draws must get peer A's
//         numbers, in order, on every live channel. That is what join-in-progress and MP save/load
//         rest on (X1/X3/X4), and it is provable offline -- no rig, no wire, no second process --
//         because the PRNG's entire input is 16 bytes of bound region plus the divisor.
//
// WHY THIS IS NOT CIRCULAR. The draws are made through the SHIPPED translated bodies
// (mh::sim::detail::rand_below / _fx / _ai / rand_state_advance), over the bytes that actually live
// in the arena -- loaded from live_base(RID_STRAT_RNG_STATE) before each draw and stored back after
// it, so the arena IS the channel state and the capture/import path is the only thing between peer
// A's stream and peer B's. The poison fill is what makes it a claim: 0xCD agrees with nothing, so a
// B that reproduces A's 16 draws did so out of the blob. And the arm is watched to go RED three
// ways -- before the import, after a one-byte corruption of the carried rng_state, and against a
// vacuity check that the 16 draws are not all the same number.
//
// WHAT IS DELIBERATELY *NOT* MASKED HERE. The determinism verdict masks slot 1 (the fx channel,
// drawn per rendered FRAME -- D3, harness.cpp's rng_state banner). A SNAPSHOT is the opposite
// problem: the importing peer must continue every channel, masked or not, or its fx stream forks
// from the host's for the rest of the match. So R1 compares all sixteen bytes byte-for-byte and
// draws ch1 alongside ch0 and ch2.

// The RNG channels the strategic sim reads, and who reads them. Slot 3 of rng_state is unreachable
// (no call site anywhere supplies it) but is carried anyway -- it is free and pinned at 0.
// NOT here, on purpose: net_discovery's xorshift, which is a non-sim beacon nonce and touches no
// hashed region; and the Watcom CRT rand() (llm_rand @0x004da48b), quarantined to cosmetic use.
struct rng_channel_region {
    mh::state::region_id rid;
    const char          *role;
};
const rng_channel_region RNG_CHANNEL_REGIONS[] = {
    {mh::state::RID_STRAT_RNG_STATE,
     "uint[4]: slot 0 strategic (llm_rand_below), 1 fx (llm_rand_below_fx), 2 AI "
     "(llm_rand_below_ai / llm_rand_state_advance), 3 unreachable"},
    {mh::state::RID_STRAT_RNG_NORM_DIVISOR,
     "the 65535.0 constant llm_rand_state_advance normalises through (fdiv qword [0x603f0c])"},
    {mh::state::RID_STRAT_RNG_SEED_BYTE,
     "the agreed ch0 seed session_begin_multi stamps from cfg_blob[0x14] and re-seeds from"},
};

// ---- R0: membership in the snapshot set and in the determinism manifest -------------------------
int arm_rng_membership() {
    int bad = 0;
    for (size_t c = 0; c < sizeof(RNG_CHANNEL_REGIONS) / sizeof(RNG_CHANNEL_REGIONS[0]); ++c) {
        const mh::state::region_id rid  = RNG_CHANNEL_REGIONS[c].rid;
        const uint32_t             want = mh::state::REGIONS[rid].reach;
        int                        at   = -1;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i)
            if (WORLD_SNAPSHOT_BLOCKS[i].rid == rid) {
                at = i;
                break;
            }
        if (at < 0) {
            ++bad;
            printf("  FAIL: RNG channel region %s is NOT carried by WORLD_SNAPSHOT_BLOCKS -- a "
                   "snapshot that omits it cannot continue the stream (%s)\n",
                   mh::state::REGIONS[rid].name, RNG_CHANNEL_REGIONS[c].role);
            continue;
        }
        if (WORLD_SNAPSHOT_BLOCKS[at].len != want) {
            ++bad;
            printf("  FAIL: RNG channel region %s is carried SHORT: block %d is %u B, the region "
                   "reaches %u B\n",
                   mh::state::REGIONS[rid].name, at, WORLD_SNAPSHOT_BLOCKS[at].len, want);
            continue;
        }
        printf("    %-32s block %3d  %5u B  %s\n", mh::state::REGIONS[rid].name, at,
               WORLD_SNAPSHOT_BLOCKS[at].len, RNG_CHANNEL_REGIONS[c].role);
    }

    // rng_state must ALSO be in the determinism manifest, and cover every slot -- the 0x40-vs-16
    // sizing bug (D3) is the precedent for checking the EXTENT rather than the presence.
    uint32_t covered = 0;
    for (int h = 0; h < mh::state::HASH_REGION_COUNT; ++h)
        if (mh::state::HASH_REGIONS[h].rid == mh::state::RID_STRAT_RNG_STATE) {
            const uint32_t end = mh::state::HASH_REGIONS[h].offset + mh::state::HASH_REGIONS[h].len;
            if (end > covered) covered = end;
        }
    if (covered < mh::state::REGIONS[mh::state::RID_STRAT_RNG_STATE].reach) {
        ++bad;
        printf("  FAIL: the determinism manifest covers only %u of rng_state's %u bytes -- a slot "
               "outside it can diverge without the verdict noticing\n",
               covered, mh::state::REGIONS[mh::state::RID_STRAT_RNG_STATE].reach);
    }
    printf("  arm R0 (RNG channel membership): %d region(s) checked, %d missing/short; the "
           "determinism manifest covers %u/%u bytes of rng_state\n",
           (int)(sizeof(RNG_CHANNEL_REGIONS) / sizeof(RNG_CHANNEL_REGIONS[0])), bad, covered,
           mh::state::REGIONS[mh::state::RID_STRAT_RNG_STATE].reach);
    return bad;
}

// ---- the draw rig: the shipped PRNG bodies, over the bytes in the arena --------------------------
struct arena_rng {
    mh::sim::sim_fixture fx;
    uint32_t            *state   = nullptr; // the arena's _G_LLM_STRAT_RNG_STATE[4]
    double              *divisor = nullptr; // ... and its _G_LLM_STRAT_RNG_NORM_DIVISOR
    uint8_t             *seed    = nullptr; // ... and its _G_LLM_STRAT_RNG_SEED_BYTE

    bool attach() {
        fx.reset();
        state = reinterpret_cast<uint32_t *>(
            static_cast<uintptr_t>(live_base(mh::state::RID_STRAT_RNG_STATE)));
        divisor = reinterpret_cast<double *>(
            static_cast<uintptr_t>(live_base(mh::state::RID_STRAT_RNG_NORM_DIVISOR)));
        seed = reinterpret_cast<uint8_t *>(
            static_cast<uintptr_t>(live_base(mh::state::RID_STRAT_RNG_SEED_BYTE)));
        return state != nullptr && divisor != nullptr && seed != nullptr;
    }

    // ONE draw of the mixed stream. The kind rotates so all three LIVE channels are exercised and an
    // importer that carried only the hashed ones would be caught: k%4 == 1 is the fx channel the
    // determinism verdict masks.
    uint32_t draw(int k) {
        std::memcpy(fx.rng_state.data(), state, 16u);
        fx.rng_norm_divisor         = *divisor;
        mh::sim::sim_store      st  = fx.store();
        const mh::sim::sim_view vw  = fx.view();
        uint32_t                out = 0;
        switch (k & 3) {
            case 0: out = static_cast<uint32_t>(mh::sim::detail::rand_below(st, 1000)); break;
            case 1: out = static_cast<uint32_t>(mh::sim::detail::rand_below_fx(st, 1000u)); break;
            case 2: out = static_cast<uint32_t>(mh::sim::detail::rand_below_ai(st, 1000u)); break;
            default: {
                // The double is compared by its BITS, not by a tolerance: this is a bit-exact replay
                // claim, and a near-miss here is a desync a fortnight later.
                const double d = mh::sim::detail::rand_state_advance(vw, st, 2);
                uint64_t     b = 0;
                std::memcpy(&b, &d, sizeof(b));
                out = static_cast<uint32_t>(b ^ (b >> 32));
            } break;
        }
        std::memcpy(state, fx.rng_state.data(), 16u);
        return out;
    }

    void draw_n(uint32_t *out, int n, int k0) {
        for (int i = 0; i < n; ++i) out[i] = draw(k0 + i);
    }
};

// ---- R1: capture at step N, import on a fresh peer, continue the stream --------------------------
int arm_rng_stream(arena &a, uint32_t masks) {
    const int WARMUP = 37; // "peer A has stepped to N": N draws already spent on all three channels
    const int DRAWS  = 16; // the done_when's "next 16 RNG draws"
    int       bad    = 0;

    for (size_t i = 0; i < a.len; ++i) a.mem[i] = pattern(i);
    empty_nav_pool();

    arena_rng r;
    if (!r.attach()) {
        printf("  FAIL: arm R could not reach the RNG channel regions in the arena\n");
        return 1;
    }
    // A session's agreed starting state, written where the game writes it. Distinct, non-symmetric
    // per channel (sim_test_support.h's fixture rule): three equal seeds would let a body that
    // ticked the WRONG slot pass.
    r.state[0] = 0x1234u;
    r.state[1] = 0xbeefu;
    r.state[2] = 0x0777u;
    r.state[3] = 0u;
    *r.divisor = 65535.0;
    *r.seed    = 0x5Au;

    for (int i = 0; i < WARMUP; ++i) (void)r.draw(i);

    // ---- the capture at step N ------------------------------------------------------------------
    const size_t   need = capture_capacity();
    uint8_t       *blob = static_cast<uint8_t *>(std::malloc(need));
    size_t         got  = 0;
    capture_params p;
    p.step       = 1000u + static_cast<uint32_t>(WARMUP);
    p.mask_flags = masks;
    p.game_clock = 0x1112131415161718ULL;
    lockstep_hash(p.mask_flags, &p.lockstep_combined, &p.lockstep_state);
    const int crc = capture(blob, need, &got, p);
    if (crc != WORLD_OK) {
        printf("  FAIL: arm R capture at step N refused, rc=%d\n", crc);
        std::free(blob);
        return 1;
    }
    blob_header h;
    std::memcpy(&h, blob, sizeof(h));

    uint8_t rng_at_n[16];
    std::memcpy(rng_at_n, r.state, 16u);
    const uint8_t seed_at_n = *r.seed;
    uint8_t       div_at_n[8];
    std::memcpy(div_at_n, r.divisor, 8u);

    // ---- peer A keeps drawing --------------------------------------------------------------------
    uint32_t a_draws[16];
    r.draw_n(a_draws, DRAWS, WARMUP);
    uint8_t a_after[16];
    std::memcpy(a_after, r.state, 16u);

    bool all_same = true;
    for (int i = 1; i < DRAWS; ++i)
        if (a_draws[i] != a_draws[0]) all_same = false;
    if (all_same) {
        ++bad;
        printf("  FAIL: arm R's 16 reference draws are all %lu -- a constant stream would be "
               "reproduced by an importer that carried nothing\n",
               (unsigned long)a_draws[0]);
    }

    // ---- the poisoned peer, BEFORE the import: it must NOT already agree -------------------------
    a.fill(0xCD);
    uint32_t poison_draws[16];
    r.draw_n(poison_draws, DRAWS, WARMUP);
    if (std::memcmp(poison_draws, a_draws, sizeof(a_draws)) == 0) {
        ++bad;
        printf("  FAIL: a 0xCD-poisoned peer already reproduces peer A's draws -- arm R would be "
               "vacuous\n");
    }

    // ---- peer B: import the step-N capture and continue ------------------------------------------
    a.fill(0xCD);
    const int irc = import(blob, got);
    if (irc != WORLD_OK) {
        ++bad;
        printf("  FAIL: arm R's step-N capture would not import, rc=%d\n", irc);
        std::free(blob);
        return bad;
    }
    if (std::memcmp(r.state, rng_at_n, 16u) != 0) {
        ++bad;
        printf("  FAIL: rng_state is not byte-identical after the import (all four slots, "
               "UNMASKED -- the fx slot the determinism verdict masks is carried too)\n");
    }
    if (*r.seed != seed_at_n) {
        ++bad;
        printf("  FAIL: the RNG seed byte did not survive the import\n");
    }
    if (std::memcmp(r.divisor, div_at_n, 8u) != 0) {
        ++bad;
        printf("  FAIL: the RNG normalisation divisor did not survive the import\n");
    }
    {
        uint64_t combined = 0, state_only = 0;
        lockstep_hash(masks, &combined, &state_only);
        if (combined != h.lockstep_combined || state_only != h.lockstep_state) {
            ++bad;
            printf("  FAIL: the imported peer does not reproduce the step-N lockstep hash pair\n");
        }
        if (canonical_hash() != h.base.content_hash) {
            ++bad;
            printf("  FAIL: the imported peer does not reproduce the step-N content hash\n");
        }
    }

    uint32_t b_draws[16];
    r.draw_n(b_draws, DRAWS, WARMUP);
    if (std::memcmp(b_draws, a_draws, sizeof(a_draws)) != 0) {
        ++bad;
        printf("  FAIL: the imported peer's next %d draws differ from peer A's\n", DRAWS);
        for (int i = 0; i < DRAWS; ++i)
            if (a_draws[i] != b_draws[i])
                printf("        draw %2d (channel %d): A=%lu B=%lu\n", i, (WARMUP + i) & 3,
                       (unsigned long)a_draws[i], (unsigned long)b_draws[i]);
    }
    if (std::memcmp(r.state, a_after, 16u) != 0) {
        ++bad;
        printf("  FAIL: after the same 16 draws the imported peer's channel state is not peer A's\n");
    }

    // ---- the negative: the draws really come from the CARRIED bytes -------------------------------
    {
        int at = -1;
        for (int i = 0; i < WORLD_SNAPSHOT_BLOCK_COUNT; ++i)
            if (WORLD_SNAPSHOT_BLOCKS[i].rid == mh::state::RID_STRAT_RNG_STATE) {
                at = i;
                break;
            }
        size_t   off = 0;
        uint32_t len = 0;
        if (at < 0 || !block_span(blob, at, &off, &len) || len < 16u) {
            ++bad;
            printf("  FAIL: arm R could not find the carried rng_state payload to corrupt\n");
        } else {
            uint8_t *mut = dup_blob(blob, got);
            mut[off] ^= 0x5Au; // slot 0, the strategic channel
            a.fill(0xCD);
            uint32_t c_draws[16];
            if (import(mut, got) != WORLD_OK) {
                ++bad;
                printf("  FAIL: the rng-corrupted blob would not import (the arm needs it to)\n");
            } else {
                r.draw_n(c_draws, DRAWS, WARMUP);
                if (std::memcmp(c_draws, a_draws, sizeof(a_draws)) == 0) {
                    ++bad;
                    printf("  FAIL: corrupting the carried rng_state did NOT change the replayed "
                           "stream -- the draws are not coming from the blob\n");
                }
            }
            std::free(mut);
        }
    }

    printf("  arm R1 (RNG stream): capture at step %lu after %d draws; import reproduces the next "
           "%d draws on channels 0/1/2 and the %d-region hash, %d failure(s)\n",
           (unsigned long)p.step, WARMUP, DRAWS, mh::state::HASH_REGION_COUNT, bad);
    std::free(blob);
    return bad;
}

} // namespace

int run_worldtest(int argc, char **argv);

// ---- THE OFFLINE ORACLE (the done_when's clause 2) ----------------------------------------------
//
// `net_selftest worldtest <blob>` imports a REAL step-0 capture -- one the game wrote at the instant
// before its first sim step -- into an arena this process allocated, and re-derives the LOCKSTEP
// hash. The blob's header carries the value the RUNNING GAME's own hash loop produced over its live
// memory at that same instant (harness.cpp hands the capture the very `combined`/`state` variables
// it is about to print). So the comparison is: one hash implementation, two processes, two different
// memories, one filled by a running game and one by the import path.
//
// IT IS NOT CIRCULAR. The header hash is not a checksum of the file -- import() never validates
// against it, and mutating a payload byte does not make the import fail. It is a statement about the
// RECORDING PEER'S STATE, recorded at the moment that state existed.
static int run_blob_oracle(const char *path) {
    printf("=== worldtest --blob (LIB-WORLD: import a REAL step-0 capture and reproduce the "
           "recording peer's lockstep hash) ===\n");
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) {
        printf("  FAIL: cannot open %s\n", path);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint8_t     *blob = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(n)));
    const size_t rd   = std::fread(blob, 1, static_cast<size_t>(n), f);
    std::fclose(f);
    if (rd != static_cast<size_t>(n)) {
        printf("  FAIL: short read of %s\n", path);
        return 1;
    }

    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    printf("  %s: %ld bytes, %lu blocks, schema=%08lX, content=%08lX%08lX\n", path, n,
           (unsigned long)h.base.block_count, (unsigned long)h.base.schema,
           (unsigned long)(h.base.content_hash >> 32),
           (unsigned long)(h.base.content_hash & 0xffffffffu));
    printf("  recorded at step %lu: lockstep=%08lX%08lX state=%08lX%08lX masks=%lu clock=%08lX%08lX\n",
           (unsigned long)h.step, (unsigned long)(h.lockstep_combined >> 32),
           (unsigned long)(h.lockstep_combined & 0xffffffffu),
           (unsigned long)(h.lockstep_state >> 32),
           (unsigned long)(h.lockstep_state & 0xffffffffu), (unsigned long)h.mask_flags,
           (unsigned long)(h.game_clock >> 32), (unsigned long)(h.game_clock & 0xffffffffu));
    printf("  pointer census: %lu dword(s) equal to a bound region's base, %lu inside the registry "
           "span\n",
           (unsigned long)h.region_head_ptrs, (unsigned long)h.registry_span_ptrs);

    // A STALE BLOB IS DIAGNOSED HERE AND THE ARM STOPS, rather than being carried into the arms
    // below. Everything past this point walks the payload at `sizeof(blob_header) + 8*i + len`, which
    // assumes the blob was written by a build whose header is the same SIZE and whose block table is
    // the same SHAPE. Hand it a blob from a build where either differs and the walk reads a length
    // out of the middle of somebody else's field and runs off the end of the buffer -- a segfault
    // with no output at all, because stdout is lost on abnormal exit. That is exactly what a
    // schema-1 blob did to this arm the first time FORMAT moved (2026-09-11): rc=139, zero bytes
    // printed, for the entirely ordinary situation of a fixture that needs re-recording. The refusal
    // codes already say all of this precisely; the arm just has to stop and read them out.
    if (std::memcmp(h.base.magic, MAGIC, sizeof(h.base.magic)) != 0) {
        printf("  FAIL: %s is not a world blob (bad magic)\n", path);
        return 1;
    }
    if (h.base.format != FORMAT) {
        printf("  FAIL: %s is FORMAT %lu; this build reads FORMAT %lu -- STALE FIXTURE, re-record it\n",
               path, (unsigned long)h.base.format, (unsigned long)FORMAT);
        return 1;
    }
    if (h.base.schema != schema_fingerprint()) {
        printf("  FAIL: %s was captured against a different block table (schema %08lX, this build "
               "%08lX) -- STALE FIXTURE, re-record it\n",
               path, (unsigned long)h.base.schema, (unsigned long)schema_fingerprint());
        return 1;
    }

    // A HASH IS ONLY COMPARABLE UNDER ITS OWN IMPLEMENTATION AND MANIFEST. Checked before anything
    // else, because a mismatch here makes every number below incomparable rather than wrong -- and
    // reporting "the fixture is broken" for what is actually "the fixture is stale" is the wrong
    // diagnosis at the worst moment.
    ck(h.hash_sink_fp == hash_sink_fingerprint(),
       "the blob was captured under THIS build's hash_sink implementation");
    ck(h.hash_manifest_fp == hash_manifest_fingerprint(),
       "the blob was captured under THIS build's hash manifest");
    ck(h.hash_slice_count == static_cast<uint32_t>(HASH_REGION_COUNT), "slice count agrees");
    ck(h.step == 1u, "the capture is the PRE-BODY state of the first sim step (step 0)");

    arena a;
    if (!a.bind_blocks()) {
        printf("  FAIL: could not bind the %d carried regions\n", WORLD_SNAPSHOT_BLOCK_COUNT);
        return 1;
    }
    printf("  bound %d block(s), %lu bytes of arena\n", WORLD_SNAPSHOT_BLOCK_COUNT,
           (unsigned long)a.len);

    // POISON FIRST, then show the poison does not already agree -- with the CONTENT hash and with
    // the LOCKSTEP hash separately, because they are two different claims and only one of them is
    // the done_when's.
    a.fill(0xCD);
    const uint64_t poisoned_content  = canonical_hash();
    uint64_t       poisoned_lockstep = 0;
    lockstep_hash(h.mask_flags, &poisoned_lockstep, nullptr);
    ck(poisoned_content != h.base.content_hash,
       "the poisoned arena does not already hash to the captured content value");
    ck(poisoned_lockstep != h.lockstep_combined,
       "the poisoned arena does not already reproduce the recording peer's lockstep hash");

    const int rc = import(blob, static_cast<size_t>(n));
    ck(rc == WORLD_OK, "libmh imports the real step-0 capture");
    if (rc != WORLD_OK) {
        // Same reason as the staleness bail above: the arms below walk the payload, and a blob that
        // could not be imported is a blob whose layout we have no right to assume.
        printf("        import rc=%d -- stopping before the arms\n", rc);
        printf("=== worldtest --blob: %d checks, %d failures ===\n", g_checks, g_fails);
        return 1;
    }

    const uint64_t content  = canonical_hash();
    uint64_t       combined = 0, state = 0;
    lockstep_hash(h.mask_flags, &combined, &state);
    printf("  after import: content=%08lX%08lX lockstep=%08lX%08lX state=%08lX%08lX\n",
           (unsigned long)(content >> 32), (unsigned long)(content & 0xffffffffu),
           (unsigned long)(combined >> 32), (unsigned long)(combined & 0xffffffffu),
           (unsigned long)(state >> 32), (unsigned long)(state & 0xffffffffu));
    ck(content == h.base.content_hash, "the imported world reproduces the blob's content hash");
    ck(combined == h.lockstep_combined,
       "THE CLAUSE: the imported world reproduces the RECORDING PEER'S step-0 lockstep hash");
    ck(state == h.lockstep_state, "... and its state-only sibling");

    if (arm_hash_slice_mutation(a, blob, static_cast<size_t>(n), h.mask_flags))
        ck(false, "every hash slice is inside the imported comparison");
    else
        ck(true, "every hash slice is inside the imported comparison");

    if (arm_block_mutation(a, blob, static_cast<size_t>(n), h.base.content_hash))
        ck(false, "every block of the REAL blob is inside the content comparison");
    else
        ck(true, "every block of the REAL blob is inside the content comparison");

    ck(arm_block_content(a, blob, static_cast<size_t>(n)) == 0,
       "every block's imported memory equals the blob payload byte-for-byte");

    std::free(blob);
    printf("=== worldtest --blob: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

int run_worldtest(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-' && std::strcmp(argv[i], "worldtest") != 0)
            return run_blob_oracle(argv[i]);

    printf("=== worldtest (LIB-WORLD: the step-0 world fixture, and the arms that make it mean "
           "something) ===\n");

    arena a;
    if (!a.bind_blocks()) {
        printf("  FAIL: could not bind the %d carried regions\n", WORLD_SNAPSHOT_BLOCK_COUNT);
        return 1;
    }
    printf("  bound %d block(s), %lu bytes of arena\n", WORLD_SNAPSHOT_BLOCK_COUNT,
           (unsigned long)a.len);

    // ---- 1. a synthetic "capture" ---------------------------------------------------------------
    for (size_t i = 0; i < a.len; ++i) a.mem[i] = pattern(i);

    // THE NAV CARRIERS MUST BE EMPTY BEFORE A CAPTURE, and this arm is why the rule needs saying.
    // FORMAT 2's capture serializes the live map-region pool, which it can only reach by WALKING it
    // from MAP_REGION_LIST_HEAD and MAP_REGION_POOL_FREE_HEAD. Those two are ordinary bound regions,
    // so the pattern fill above puts pseudo-random bytes in them -- a non-null pointer to nowhere,
    // dereferenced on the first hop. (Measured: rc=139, no output.) A synthetic arena has no pool, so
    // saying so explicitly is both the truth and the fix. BY_INDEX and the GRID are cleared for the
    // same reason: every non-null entry in them must be a pool node or the capture refuses.
    empty_nav_pool();

    const size_t need = capture_capacity();
    uint8_t     *blob = static_cast<uint8_t *>(std::malloc(need));
    size_t       got  = 0;

    capture_params p;
    p.step       = 1u;
    p.mask_flags = MASK_CTRL_GROUP | MASK_SOLDIER_ANIM | MASK_PLANETS_GFX;
    p.game_clock = 0x4041424344454647ULL;
    // The synthetic capture is handed the hash of the arena it is about to read, so the header's
    // recorded pair is the truth about THIS memory -- the same relationship the real capture has
    // with the run's own variables.
    lockstep_hash(p.mask_flags, &p.lockstep_combined, &p.lockstep_state);

    const int crc = capture(blob, need, &got, p);
    ck(crc == WORLD_OK, "capture succeeds over a fully bound arena");
    if (crc != WORLD_OK) {
        printf("        capture rc=%d\n", crc);
        return 1;
    }
    // FORMAT 2: the blocks end at blob_size() and the nav trailer follows, so the total is no longer
    // a compile-time constant. The stronger statement is the one that still holds -- the trailer
    // starts exactly where the blocks end, and the two lengths account for every byte written.
    ck(got > blob_size(), "capture writes the blocks AND a nav trailer");

    blob_header h;
    std::memcpy(&h, blob, sizeof(h));
    ck(h.base.schema == schema_fingerprint(), "the blob carries this build's schema fingerprint");
    ck(h.base.block_count == static_cast<uint32_t>(WORLD_SNAPSHOT_BLOCK_COUNT),
       "block count recorded");
    ck(h.hash_sink_fp == hash_sink_fingerprint(), "the hash-sink fingerprint is recorded");
    ck(h.hash_manifest_fp == hash_manifest_fingerprint(), "the hash-manifest fingerprint is recorded");
    printf("  capture: %lu bytes, content=%08lX%08lX lockstep=%08lX%08lX ptrs head=%lu span=%lu\n",
           (unsigned long)got, (unsigned long)(h.base.content_hash >> 32),
           (unsigned long)(h.base.content_hash & 0xffffffffu),
           (unsigned long)(h.lockstep_combined >> 32),
           (unsigned long)(h.lockstep_combined & 0xffffffffu), (unsigned long)h.region_head_ptrs,
           (unsigned long)h.registry_span_ptrs);

    // ---- 2. the round trip ----------------------------------------------------------------------
    a.fill(0xCD);
    uint64_t poisoned_lockstep = 0;
    lockstep_hash(p.mask_flags, &poisoned_lockstep, nullptr);
    ck(canonical_hash() != h.base.content_hash, "the poisoned arena does not already agree (content)");
    ck(poisoned_lockstep != h.lockstep_combined,
       "the poisoned arena does not already agree (lockstep)");
    ck(import(blob, got) == WORLD_OK, "the blob imports");
    ck(canonical_hash() == h.base.content_hash, "the round trip reproduces the content hash");
    {
        uint64_t combined = 0, state = 0;
        lockstep_hash(p.mask_flags, &combined, &state);
        ck(combined == h.lockstep_combined, "the round trip reproduces the lockstep hash");
        ck(state == h.lockstep_state, "... and the state-only one");
    }

    // ---- 3. THE CAPTURE'S OWN SELF-CHECK, watched to fire ---------------------------------------
    //
    // capture() refuses when a hash slice's live bytes are not in the blob. That refusal is the arm
    // against a fixture that is right about its own hash and empty of everything the oracle reads,
    // so it must be seen to fire. Un-binding a hashed region is the closest thing to that state a
    // test can construct -- the engine then refuses UNBOUND first, which is the same refusal one
    // step earlier, and either way a capture with a hole does not produce a file.
    {
        const region_id r    = HASH_REGIONS[0].rid;
        const uint32_t  keep = live_base(r);
        const uint32_t  ksz  = live_size(r);
        bind(r, 0u, 0u);
        size_t    tmp = 0;
        const int rc2 = capture(blob, need, &tmp, p);
        ck(rc2 == WORLD_ERR_UNBOUND,
           "a capture with an UNBOUND hashed region refuses instead of writing a hole");
        bind(r, keep, ksz);
        // ... and the world is capturable again once it is whole, so the refusal is about the hole
        // rather than about this fixture having become unusable.
        ck(capture(blob, need, &tmp, p) == WORLD_OK, "re-binding it makes the capture succeed again");
        std::memcpy(&h, blob, sizeof(h));
    }

    // ---- 4. the refusals ------------------------------------------------------------------------
    {
        ck(import(nullptr, 16) == WORLD_ERR_ARG, "a null blob is refused");
        ck(import(blob, sizeof(blob_header) - 1) == WORLD_ERR_ARG, "a blob shorter than the header");

        uint8_t *mut = dup_blob(blob, got);

        std::memcpy(mut, blob, got);
        mut[0] ^= 0xffu;
        ck(import(mut, got) == WORLD_ERR_MAGIC, "a wrong magic is refused");

        // A LIB-BOOT blob must not import as a world blob. Distinct MAGIC is the whole guard, and a
        // guard nobody watches fire is a guard nobody has.
        std::memcpy(mut, blob, got);
        {
            static const uint8_t BOOT_MAGIC[8] = {'M', 'H', 'B', 'O', 'O', 'T', 0, 1};
            std::memcpy(mut, BOOT_MAGIC, 8);
            ck(import(mut, got) == WORLD_ERR_MAGIC, "a LIB-BOOT blob is refused by the world importer");
        }

        std::memcpy(mut, blob, got);
        blob_header bh;
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.format += 1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == WORLD_ERR_MAGIC, "an unknown format is refused");

        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.schema ^= 0x1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == WORLD_ERR_SCHEMA,
           "a blob captured against a different block table is refused");

        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.block_count += 1u;
        std::memcpy(mut, &bh, sizeof(bh));
        ck(import(mut, got) == WORLD_ERR_SCHEMA, "a disagreeing block count is refused");

        std::memcpy(mut, blob, got);
        // `blob_size() - 1`, not `got - 1`: since FORMAT 2 `got` includes the nav trailer, so lopping
        // one byte off the END no longer lands inside the BLOCK payload and the block engine -- which
        // is what this arm is about -- would be right to accept it. Cutting one byte short of where
        // the blocks end is the truncation this refusal exists for, and it keeps the arm measuring
        // the engine rather than the trailer. The trailer's own truncation refusal is navtest's.
        ck(import(mut, blob_size() - 1u) == WORLD_ERR_TRUNCATED,
           "a blob truncated inside its block payload is refused");
        ck(import_nav(mut, blob_size()) == WORLD_ERR_TRUNCATED,
           "a blob whose nav trailer is missing entirely is refused");

        std::memcpy(mut, blob, got);
        {
            uint32_t bogus = 0xffffffffu;
            std::memcpy(mut + sizeof(blob_header), &bogus, 4);
            ck(import(mut, got) == WORLD_ERR_UNKNOWN_RID, "a block naming no known region is refused");
        }

        // AND THE REFUSALS MUST NOT HAVE WRITTEN ANYTHING.
        a.fill(0xCD);
        const uint64_t poisoned = canonical_hash();
        std::memcpy(mut, blob, got);
        std::memcpy(&bh, mut, sizeof(bh));
        bh.base.schema ^= 0x1u;
        std::memcpy(mut, &bh, sizeof(bh));
        import(mut, got);
        ck(canonical_hash() == poisoned, "a REFUSED import writes nothing at all");

        // The ordering latch: importing a world over a session that has already begun rewriting
        // boot-and-session regions is the same hazard LIB-BOOT's -4 exists for, and the two share
        // one latch on purpose.
        ::mh::state::boot::note_session_begun();
        ck(import(blob, got) == WORLD_ERR_SESSION_BEGUN,
           "an import after a session has begun is refused");
        ::mh::state::boot::reset_session_latch_for_test();

        std::free(mut);
        a.fill(0xCD);
        ck(import(blob, got) == WORLD_OK, "re-import the clean blob");
    }

    // ---- 5. the three arms ----------------------------------------------------------------------
    ck(arm_hash_slice_mutation(a, blob, got, h.mask_flags) == 0,
       "every hash slice is inside the imported lockstep comparison");
    ck(arm_block_mutation(a, blob, got, h.base.content_hash) == 0,
       "every block is inside the content comparison");
    ck(arm_block_content(a, blob, got) == 0,
       "every block's imported memory equals the blob payload byte-for-byte");

    // ---- 6. ARM R: the RNG channels (mp:R-rng-snapshot / X0) --------------------------------------
    //
    // Last, and over its OWN fill + capture, so nothing above changes meaning: arms A-C are about the
    // blob format and its coverage, this one is about whether an imported peer can keep drawing. It
    // leaves the arena holding a deliberately corrupted import, which is why it runs after them.
    ck(arm_rng_membership() == 0,
       "every RNG channel the sim reads is a named member of the snapshot set");
    ck(arm_rng_stream(a, h.mask_flags) == 0,
       "a step-N capture imported on a fresh peer reproduces the RNG stream draw-for-draw");

    std::free(blob);
    printf("=== worldtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
