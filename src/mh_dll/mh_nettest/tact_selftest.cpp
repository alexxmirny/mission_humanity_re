//
// tact_selftest.cpp -- `net_selftest.exe tacttest`: the OFFLINE tactical oracle (RI-TACT /
// TACT-DOMAIN).
//
// WHY THIS SUITE EXISTS, AND WHAT IT IS NOT. Tactical already has a trajectory oracle
// (`tools/test_ui.py --tact-determinism`): a real mission driven headless through `--tactical`,
// hashed on the `llm_tact_frame` cadence, byte-identical across two arms and watched to fail at
// exactly the frame and region a deliberate poke touches. That instrument is an INTEGRATION oracle
// -- it says a run diverged, not which function did it -- and it needs the rig, the game files and
// minutes. This suite is the other half: it needs none of those, and it tests the things the
// trajectory oracle CANNOT test because it depends on them.
//
// THE PART THAT MATTERS: A TRAJECTORY ORACLE CANNOT AUDIT ITS OWN COVERAGE. `--tact-determinism`
// compares a combined hash over 14 slices. If one slice's length were wrong, or a slice were
// silently masked to nothing, or two slices aliased the same bytes, the two arms would STILL agree
// -- identically wrong -- and the run would print green. The rig's RED arm pokes ONE region per
// invocation and therefore proves one slice is read; proving all fourteen are read that way would
// be fourteen rig runs. Here it is fourteen memcmps.
//
// So every check below is about the INSTRUMENT and the arena it reads, asserted in BOTH directions:
//
//   T1  the region lengths and the struct strides are the same measurement  (record count x stride)
//   T2  the slice table is a real partition -- no dupes, no aliasing, no empty or excluded slice
//   T3  every one of the 14 slices is actually READ by the hash, and reads ONLY its own bytes
//   T4  the tile_objects mask is real: the per-frame fog byte is excluded and the occupancy byte
//       is not -- the negative arm, without which T3 would pass over a mask that dropped everything
//   T5  `tact_mutate_target(i)` -- the lever the rig's RED arm pulls -- points at slice i's live
//       bytes, so a green RED-arm run means what it says
//
// TACT0 (2026-08-24) ADDED THE OTHER HALF: a FIXTURE and the first per-function cases. Until then
// this suite deliberately had none, because nothing tactical was translated and a suite whose whole
// content is a `return 0` is the vacuous green this project keeps writing checks to avoid.
//
//   T6  the pilot slice, one case per facet of the state interface -- a body needing NO state
//       (a), the CONST view (b), the write store over tactical's own arena (c), and the
//       mode-shared planes (d). All four are mutation-checked by
//       tools/oneoff/2026-08-24-mutate-tact0-pilot.py, 8/8 caught by the assertion that names them.
//   T6e the shared grid's indexing, probed OFF THE DIAGONAL. Added because a transposition mutant
//       escaped its own assertion: the tactical sub-block is 128x128 and therefore SQUARE, so a
//       transposed clear covers the identical set of cells and no which-cells assertion can see it.
//   T7  `mh::tact::state()` re-resolves per call, so both halves follow a rebase -- and follow BACK,
//       which is the bound-once counterfactual.
//
// THE INSTRUMENT HALF IS AUDITED FOR HAVING RUN, and is no longer described by a magic number.
// It used to read "the 371 instrument checks above are unchanged", which went stale the moment the
// tactical state interface gained its 15th hash region: T2 and T3 iterate TACT_HASH_REGION_COUNT,
// so the five audits that existed when 371 was written now contribute 406, and the whole block is
// 576 over 11 audits. Nothing was removed -- the count is data-driven and moved for a correct
// reason, which is exactly why a literal-371 assertion would have failed on a good change.
// `run_tacttest` therefore gates the SHAPE: every audit in the roster runs and contributes at least
// one check, the roster is still 11 entries (a deletion is invisible to a loop over what remains),
// and the block scales with the region table. Both negative arms fail by name: deleting a roster
// entry trips the roster-size check, hollowing an audit out trips that audit's own line.
//
// THE ARENA IS LOCAL. Every region is `rebase()`d onto a heap buffer for the duration, exactly as
// state_selftest.cpp does: the stock tactical VAs (0x008260d0, 0x0085b4e0, ...) are unmapped in
// this process and a flat read of them would fault rather than fail. The rebases are reverted at
// the end of each block, so the suite leaves the registry as it found it.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/mode_planes.h"
#include "state/region_view.h"
#include <stdlib.h>

#include "tact/tact_journal.h"
#include "tact/tact_pilot.h"
#include "tact/tact_state.h"

#include "tact_test_support.h" // the shared fixture + ck helpers (split out at TACT1A)

namespace {

// The counters and the three check helpers live in tact_test_support.h since TACT1A, so a
// per-TU oracle file can share them; this file keeps the short names it already used.
using mh::tact::test::ck;
using mh::tact::test::ck_eq;

using mh::state::TACT_HASH_REGION_COUNT;
using mh::state::TACT_HASH_REGIONS;

// ---- T1: the region lengths and the record strides are ONE measurement ------------------------
//
// Each pair below is a length in `hash_manifest.json` and a stride recovered from the disassembly
// (the tactical-probe work Sect. 2). They are written down in two different files by two different
// tools, and nothing until now made them agree. A stride corrected in the struct header without the
// manifest following it produces a slice that hashes a fraction of the array -- green on the rig,
// blind to the tail.
void test_layout() {
    printf("-- T1: region length == record count x stride\n");

    ck_eq((uint32_t)sizeof(mh::game::mh_tact_unit_record), 0x5f4u,
          "tact_unit_record stride is 0x5f4");
    ck_eq((uint32_t)sizeof(mh::game::mh_llm_tact_unit_cmd_entry), 0xbu,
          "llm_tact_unit_cmd_entry stride is 0xb");
    ck_eq((uint32_t)offsetof(mh::game::mh_tact_unit_record, cmd_queue), 0x54u,
          "the command queue is based at +0x54");

    struct expect {
        const char *slice;
        uint32_t    count;
        uint32_t    stride;
        const char *why;
    };
    // Counts and strides: the tactical-probe work Sect. 2, recovered from the bodies that index them.
    static const expect E[] = {
        {"tact_units", 129u, 0x5f4u, "tact_units is tact_unit_record[129]"},
        {"tact_fx_pool", 1024u, 0x52u, "tact_fx_pool is llm_tact_fx[1024]"},
        {"tact_doors", 16u, 0x68u, "tact_doors is llm_tact_door[16]"},
        {"tact_teleports", 66u, 0x32u, "tact_teleports is llm_tact_teleport[66]"},
        {"tact_char_types", 16u, 0x6cu, "tact_char_types is llm_tact_character_type[16]"},
        {"tact_fx_types", 64u, 0x4au, "tact_fx_types is llm_tact_fx_type[64]"},
        {"tact_fov_stencil", 4096u, 1u, "tact_fov_stencil is undefined1[4096]"},
    };
    for (const expect &e : E) {
        bool found = false;
        for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
            if (strcmp(TACT_HASH_REGIONS[i].name, e.slice) != 0) continue;
            found = true;
            ck_eq(TACT_HASH_REGIONS[i].len, e.count * e.stride, e.why);
        }
        ck(found, e.slice);
    }

    // The tactical-OWNED footprint, i.e. everything except the two shared planes -- and, since
    // 2026-08-25, except an ALIAS slice, whose bytes are already counted under its source. 296,108
    // bytes is the number TACT-PREP registered; a slice added or dropped without a manifest review
    // moves it, and an alias counted as new footprint would move it for no reason at all.
    uint32_t owned = 0;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        const mh::state::hash_region &r = TACT_HASH_REGIONS[i];
        if (r.excluded) continue; // alias -- see T2
        if (r.rid != mh::state::RID_TILE_OBJECTS && r.rid != mh::state::RID_PASSABLE)
            owned += r.len;
    }
    ck_eq(owned, 296108u, "the tactical-owned arena is 296,108 bytes over 12 slices");
}

// ---- T2: the slice table is a partition, not a list -------------------------------------------
// An ALIAS slice re-emits another slice's bytes under a different traversal. It is marked
// `excluded` -- which in the TACTICAL table means "already counted under another name", not the
// strategic table's "left out of the state verdict" -- and it is the ONE sanctioned reason two
// slices may describe the same memory.
bool is_alias(const mh::state::hash_region &r) { return r.excluded; }

void test_manifest() {
    printf("-- T2: the slice table is a real partition, aliases excepted\n");
    ck_eq((uint32_t)TACT_HASH_REGION_COUNT, 15u, "15 tactical slices (14 + the sim-only alias)");

    int shared = 0, aliases = 0;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        const mh::state::hash_region &a = TACT_HASH_REGIONS[i];
        ck(a.len > 0, "a slice of length 0 would hash nothing and read green");
        if (is_alias(a)) {
            ++aliases;
            // The safety property the old blanket `!excluded` assertion was really protecting: an
            // excluded slice must not be the ONLY cover for its bytes, or the arena goes unwatched
            // while the table still looks full.
            bool covered = false;
            for (int j = 0; j < TACT_HASH_REGION_COUNT; ++j) {
                const mh::state::hash_region &b = TACT_HASH_REGIONS[j];
                if (j == i || b.excluded) continue;
                covered = covered || (b.rid == a.rid && b.offset == a.offset && b.len == a.len);
            }
            ck(covered,
               "an excluded (alias) slice is covered byte-for-byte by a NON-excluded slice -- "
               "otherwise excluding it would leave that arena unwatched");
            continue;
        }
        if (a.rid == mh::state::RID_TILE_OBJECTS || a.rid == mh::state::RID_PASSABLE) ++shared;
        for (int j = i + 1; j < TACT_HASH_REGION_COUNT; ++j) {
            const mh::state::hash_region &b = TACT_HASH_REGIONS[j];
            ck(strcmp(a.name, b.name) != 0,
               "two slices share a name -- mp_analyze reports the region that diverged, so a "
               "duplicate name is an ambiguous verdict");
            if (b.excluded) continue; // an alias overlaps its source ON PURPOSE
            if (a.rid != b.rid) continue;
            bool overlap = a.offset < b.offset + b.len && b.offset < a.offset + a.len;
            ck(!overlap, "two slices of one region overlap -- a poke would name both");
        }
    }
    ck_eq((uint32_t)aliases, 1u, "exactly one alias slice: tact_units_sim");
    ck(strcmp(TACT_HASH_REGIONS[mh::state::TIDX_TACT_UNITS_SIM].name, "tact_units_sim") == 0,
       "TIDX_TACT_UNITS_SIM names the alias -- region_view.h dispatches the sim emitter by INDEX");
    ck_eq((uint32_t)shared, 2u, "exactly two slices are the planes shared with the strategic sim");

    // The two shared planes appear in BOTH tables. They must describe the same bytes, or a tactical
    // divergence and a strategic one would be reported over different extents of one array.
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        const mh::state::hash_region &t = TACT_HASH_REGIONS[i];
        if (t.rid != mh::state::RID_TILE_OBJECTS && t.rid != mh::state::RID_PASSABLE) continue;
        for (int j = 0; j < mh::state::HASH_REGION_COUNT; ++j) {
            const mh::state::hash_region &s = mh::state::HASH_REGIONS[j];
            if (s.rid != t.rid || s.offset != t.offset) continue;
            ck_eq(t.len, s.len, "a shared plane is described identically in both slice tables");
        }
    }
}

// ---- the local arena --------------------------------------------------------------------------
//
// One heap buffer per DISTINCT region id behind the slice table, filled with a per-region byte
// pattern -- not zeroes, because a zeroed arena makes a hash that ignores its input
// indistinguishable from one that reads it.
struct arena {
    std::vector<std::vector<uint8_t>> bufs;
    std::vector<mh::state::region_id> rids;

    arena() {
        for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
            const mh::state::hash_region &r    = TACT_HASH_REGIONS[i];
            bool                          seen = false;
            for (mh::state::region_id q : rids) seen = seen || (q == r.rid);
            if (seen) continue;
            rids.push_back(r.rid);
            bufs.push_back(std::vector<uint8_t>((size_t)(r.offset + r.len)));
            std::vector<uint8_t> &b = bufs.back();
            for (size_t k = 0; k < b.size(); ++k)
                b[k] = (uint8_t)(k * 31u + (size_t)r.rid * 7u + 1u);
        }
        for (size_t k = 0; k < rids.size(); ++k)
            mh::state::rebase(rids[k], (uint32_t)(uintptr_t)bufs[k].data(),
                              (uint32_t)bufs[k].size());
    }
    ~arena() {
        for (mh::state::region_id r : rids) mh::state::unrebase(r);
    }
};

void hash_all(uint64_t *out) {
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) out[i] = mh::state::tact_hash_slice(i);
}

// The byte offset within a slice that the emitter is known to READ. Flat slices emit everything, so
// 0 is fine; tile_objects is masked and interleaved, so +0/+1 are partly masked and +7 is
// deliberately dropped -- +2 (building lo) is the nearest byte the emitter definitely reads.
uint32_t emitted_offset(const mh::state::hash_region &r) {
    return r.rid == mh::state::RID_TILE_OBJECTS ? 2u : 0u;
}

// ---- T3: every slice is read, and reads only itself -------------------------------------------
void test_slice_coverage() {
    printf("-- T3: all 15 slices are read, and each reads only its own bytes (aliases aside)\n");
    arena a;

    uint64_t base[TACT_HASH_REGION_COUNT], now[TACT_HASH_REGION_COUNT];
    hash_all(base);

    // THE NEGATIVE ARM FIRST. Re-hashing without touching anything must reproduce every hash. If it
    // does not, the emitter is reading something outside the arena and every result below is noise.
    hash_all(now);
    ck(memcmp(base, now, sizeof(base)) == 0,
       "re-hashing an untouched arena reproduces all 15 hashes");

    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        const mh::state::hash_region &r = TACT_HASH_REGIONS[i];
        uint8_t                      *p = mh::state::tact_mutate_target(i) + emitted_offset(r);
        char                          what[200];

        *p = (uint8_t)(*p ^ 0xffu);
        hash_all(now);

        snprintf(what, sizeof(what),
                 "slice %d (%s) is READ -- flipping one of its bytes changes its hash", i, r.name);
        ck(now[i] != base[i], what);

        for (int j = 0; j < TACT_HASH_REGION_COUNT; ++j) {
            if (j == i) continue;
            const mh::state::hash_region &o = TACT_HASH_REGIONS[j];
            // An alias pair describes the SAME bytes, so "the slices do not alias" is the wrong
            // assertion for it -- and asserting nothing would let a broken alias emitter (one that
            // read the wrong array) slip through. The positive form is the right one: byte 0 of
            // tact_units is `type`, which the sim emitter keeps, so both hashes MUST move.
            const bool alias_pair =
                o.rid == r.rid && o.offset == r.offset && o.len == r.len && (o.excluded ^ r.excluded);
            if (alias_pair) {
                snprintf(what, sizeof(what),
                         "%s and %s alias ON PURPOSE -- a poke at a byte both emit moves both",
                         r.name, o.name);
                ck(now[j] != base[j], what);
                continue;
            }
            snprintf(what, sizeof(what), "poking %s left %s alone -- the slices do not alias",
                     r.name, o.name);
            ck(now[j] == base[j], what);
        }

        *p = (uint8_t)(*p ^ 0xffu);
        hash_all(now);
        snprintf(what, sizeof(what), "reverting the poke to %s restores every hash", r.name);
        ck(memcmp(base, now, sizeof(base)) == 0, what);
    }
}

// ---- T4: the tile_objects mask is real, in both directions ------------------------------------
//
// `emit_tile_objects` masks the flag word with TILE_FLAGS_KEEP (0x3fff) and never emits byte +7 at
// all, because both carry per-FRAME fog that a camera pan changes -- hashing them would report
// input as a divergence. T3 alone cannot tell that mask apart from one that dropped the whole
// record: this is the arm that can.
void test_tile_objects_mask() {
    printf("-- T4: the tile_objects fog mask excludes the right bytes and only those\n");
    arena a;

    int idx = -1;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i)
        if (TACT_HASH_REGIONS[i].rid == mh::state::RID_TILE_OBJECTS) idx = i;
    ck(idx >= 0, "the tactical slice table carries tile_objects");
    if (idx < 0) return;

    uint8_t       *p    = mh::state::tact_mutate_target(idx);
    const uint64_t base = mh::state::tact_hash_slice(idx);

    p[7] ^= 0xffu; // visibility -- per-frame fog, deliberately not emitted
    ck(mh::state::tact_hash_slice(idx) == base, "the per-frame visibility byte (+7) is NOT hashed");
    p[7] ^= 0xffu;

    p[1] ^= 0xc0u; // flag bits 14-15, outside TILE_FLAGS_KEEP
    ck(mh::state::tact_hash_slice(idx) == base,
       "flag bits above TILE_FLAGS_KEEP are masked out of the hash");
    p[1] ^= 0xc0u;

    p[1] ^= 0x01u; // flag bit 8, inside the mask
    ck(mh::state::tact_hash_slice(idx) != base,
       "a flag bit INSIDE TILE_FLAGS_KEEP still changes the hash -- the mask is not total");
    p[1] ^= 0x01u;

    p[2] ^= 0xffu; // building lo -- occupancy, the reason the plane is hashed at all
    ck(mh::state::tact_hash_slice(idx) != base, "the occupancy byte (+2, building lo) IS hashed");
    p[2] ^= 0xffu;

    ck(mh::state::tact_hash_slice(idx) == base, "every poke above was reverted");
}

// ---- T8: the sim-only slice drops exactly the render-written window, and nothing else ----------
//
// This is the check the whole `TS` verdict rests on, and it has the same shape as T4: no rig run can
// distinguish a mask that drops the right five fields from one that drops half the record, because
// both report "sim identical" on a presentation-only divergence. Both directions are asserted -- the
// five that must vanish, and a hand-picked set that must NOT, chosen where the argument is live:
// `anim_state` gates kneel/mine-arm, `progress` drives the move offset, `cmd_wait_until_time` is a
// command deadline. An exclusion list is only honest while it holds what nobody can argue for.
void test_sim_slice() {
    printf("-- T8: tact_units_sim excludes the animation window, and ONLY it\n");
    arena a;

    constexpr int FULL = mh::state::TIDX_TACT_UNITS;
    constexpr int SIM  = mh::state::TIDX_TACT_UNITS_SIM;
    uint8_t      *rec  = mh::state::tact_mutate_target(FULL); // unit 0
    ck((uint32_t)(uintptr_t)mh::state::tact_mutate_target(SIM) == (uint32_t)(uintptr_t)rec,
       "the alias addresses the same bytes as its source");

    const uint64_t bf = mh::state::tact_hash_slice(FULL);
    const uint64_t bs = mh::state::tact_hash_slice(SIM);

    struct probe {
        uint32_t    off;
        const char *name;
        bool        in_sim; // must the SIM hash move?
    };
    static const probe P[] = {
        // dropped -- written from llm_tact_unit_render -> llm_tact_unit_update_anim only
        {0x2b, "anim_frame_time", false},
        {0x32, "anim_frame_time (last byte)", false},
        {0x33, "frame_index", false},
        {0x34, "anim_cycle_time", false},
        {0x3c, "frame_interval", false},
        {0x43, "frame_interval (last byte)", false},
        {0x45, "sprite_id", false},
        {0x46, "sprite_id (hi)", false},
        // kept -- and deliberately the ARGUABLE ones, plus the two bytes bracketing the window
        {0x2a, "cmd_wait_until_time (last byte, KEPT)", true},
        {0x23, "cmd_wait_until_time (KEPT)", true},
        {0x05, "anim_state (KEPT)", true},
        {0x44, "progress (KEPT -- the byte BETWEEN the two dropped windows)", true},
        {0x47, "move_path_slot (KEPT -- the byte AFTER sprite_id)", true},
        {0x00, "type (KEPT)", true},
        {0x4b, "hp (KEPT)", true},
        {0x53, "cmd_index (KEPT)", true},
    };
    char what[220];
    for (const probe &pr : P) {
        rec[pr.off] ^= 0xffu;
        const uint64_t nf = mh::state::tact_hash_slice(FULL);
        const uint64_t ns = mh::state::tact_hash_slice(SIM);
        snprintf(what, sizeof(what), "+0x%02x %s changes the FULL hash", pr.off, pr.name);
        ck(nf != bf, what);
        if (pr.in_sim) {
            snprintf(what, sizeof(what), "+0x%02x %s changes the SIM hash too", pr.off, pr.name);
            ck(ns != bs, what);
        } else {
            snprintf(what, sizeof(what), "+0x%02x %s is NOT in the SIM hash", pr.off, pr.name);
            ck(ns == bs, what);
        }
        rec[pr.off] ^= 0xffu;
    }
    ck(mh::state::tact_hash_slice(FULL) == bf && mh::state::tact_hash_slice(SIM) == bs,
       "every poke above was reverted");

    // The mask must apply to EVERY record, not just unit 0 -- a stride bug would leave 128 units
    // hashing their animation window into the sim verdict and the oracle would still look right.
    uint8_t *last = rec + 128u * mh::state::TACT_UNIT_STRIDE;
    last[0x33] ^= 0xffu; // frame_index of the LAST record
    ck(mh::state::tact_hash_slice(FULL) != bf, "unit 128's frame_index is in the full hash");
    ck(mh::state::tact_hash_slice(SIM) == bs,
       "unit 128's frame_index is NOT in the sim hash -- the mask strides the whole array");
    last[0x33] ^= 0xffu;
    last[0x05] ^= 0xffu; // anim_state of the LAST record
    ck(mh::state::tact_hash_slice(SIM) != bs, "unit 128's anim_state IS in the sim hash");
    last[0x05] ^= 0xffu;
}

// ---- T9: the two combined verdicts, and the compatibility the `T` line depends on --------------
void test_verdict_pair() {
    printf("-- T9: tact_hash_all -- `T` is unchanged, `TS` moves independently\n");
    arena a;

    uint64_t per[TACT_HASH_REGION_COUNT];
    auto     v = mh::state::tact_hash_all(per);

    // THE FOLD'S SHAPE, re-derived the old way -- over the non-alias slices, in table order -- and
    // compared. This is the assertion that would have caught an alias accidentally left
    // `excluded: false`.
    //
    // It used to be justified as COMPATIBILITY with "every historical tactical run and the shipped
    // goldens". That justification is gone as of 2026-08-25: the hash step widened to 8 bytes, so
    // no historical value is comparable regardless (the tactical-probe work 9s). The check is still
    // worth keeping for what it actually tests -- that adding a slice does not silently change what
    // the fold covers -- and the per-slice values it folds are computed live in this same run, so
    // it is self-contained.
    uint64_t legacy = 1469598103934665603ULL;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        if (TACT_HASH_REGIONS[i].excluded) continue;
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&per[i]);
        for (size_t k = 0; k < sizeof(per[i]); ++k) {
            legacy ^= b[k];
            legacy *= 1099511628211ULL;
        }
    }
    ck(v.combined == legacy,
       "the combined `T` hash is the pre-alias fold, byte for byte -- the alias is not folded in");
    ck(v.sim != v.combined,
       "the two verdicts are DIFFERENT values -- an accidental alias of the fold itself would make "
       "every 'sim identical' verdict vacuous");

    uint8_t *rec = mh::state::tact_mutate_target(mh::state::TIDX_TACT_UNITS);

    rec[0x33] ^= 0xffu; // frame_index -- presentation
    auto v1 = mh::state::tact_hash_all(per);
    ck(v1.combined != v.combined, "a presentation-only change moves the combined verdict");
    ck(v1.sim == v.sim, "a presentation-only change leaves the SIM verdict alone -- this is the "
                        "whole discrimination the oracle makes");
    rec[0x33] ^= 0xffu;

    rec[0x05] ^= 0xffu; // anim_state -- sim
    auto v2 = mh::state::tact_hash_all(per);
    ck(v2.combined != v.combined && v2.sim != v.sim,
       "a sim-state change moves BOTH verdicts");
    rec[0x05] ^= 0xffu;

    // A slice OUTSIDE tact_units must move both, or the sim fold quietly dropped 13 slices.
    uint8_t *fx = mh::state::tact_mutate_target(mh::state::TIDX_TACT_FX_POOL);
    *fx ^= 0xffu;
    auto v3 = mh::state::tact_hash_all(per);
    ck(v3.combined != v.combined && v3.sim != v.sim,
       "a change in a NON-unit slice moves both verdicts -- the sim fold covers the whole table");
    *fx ^= 0xffu;

    auto v4 = mh::state::tact_hash_all(per);
    ck(v4.combined == v.combined && v4.sim == v.sim, "every poke above was reverted");
}

// ---- T10: the ORDER JOURNAL's site classification (TACT-REC) ------------------------------------
//
// The journal's whole correctness question in one predicate. Both order seams are ALSO called by
// code that runs with no input; journalling one of THOSE issues it TWICE on replay -- once by the
// game, once by the journal -- and the duplicate is indistinguishable from a real order. No rig run
// can catch that: a replay with doubled orders still produces frames, still hashes, still compares
// equal to a SECOND replay with the same doubling. So the rule is pinned here instead.
//
// Every address below is a RETURN address (call site + 5, all 35 calls are `E8 rel32`) from the EN
// xref set of 2026-08-25, transcribed with its containing function. A build that moves any of these
// functions fails this test rather than silently reclassifying a site.
void test_journal_sites() {
    printf("-- T10: the order journal classifies all 35 seam call sites correctly\n");

    struct site {
        uint32_t    pc;
        bool        player;
        const char *fn;
    };
    static const site S[] = {
        // ---- llm_tact_unit_enqueue_command, 22 sites ------------------------------------------
        {0x00429f13u, true, "llm_tact_frame"},
        {0x0042a408u, true, "llm_tact_frame"},
        {0x0042a576u, true, "llm_tact_frame"},
        {0x0042a58fu, true, "llm_tact_frame"},
        {0x0042a5bdu, true, "llm_tact_frame"},
        {0x0042a5d6u, true, "llm_tact_frame"},
        {0x00436727u, true, "llm_tact_ui_sel_panel_multi_mode_tick"},
        // nested in the group seam -- journalled THERE, so they must NOT read as player here
        {0x0042b1d3u, false, "llm_tact_group_issue_order"},
        {0x0042b371u, false, "llm_tact_group_issue_order"},
        {0x0042b38fu, false, "llm_tact_group_issue_order"},
        // engine
        {0x00438ab3u, false, "llm_tact_mission_load"},
        {0x00430796u, false, "llm_tact_unit_cmd_queue_resubmit_run"},
        {0x00431b39u, false, "llm_tact_fx_update_projectile"},
        {0x004337dau, false, "llm_tact_unit_owner_tick"},
        {0x0043382eu, false, "llm_tact_unit_owner_tick"},
        {0x004338c3u, false, "llm_tact_unit_owner_tick"},
        {0x00433917u, false, "llm_tact_unit_owner_tick"},
        {0x00433995u, false, "llm_tact_unit_owner_tick"},
        {0x00433a2au, false, "llm_tact_unit_owner_tick"},
        {0x00433a7eu, false, "llm_tact_unit_owner_tick"},
        {0x00433afcu, false, "llm_tact_unit_owner_tick"},
        {0x00433bfeu, false, "llm_tact_unit_owner_tick"},
        // ---- llm_tact_group_issue_order, 13 sites, every one a UI function --------------------
        {0x00429e55u, true, "llm_tact_frame"},
        {0x00429e67u, true, "llm_tact_frame"},
        {0x0042a677u, true, "llm_tact_frame"},
        {0x0042a6fcu, true, "llm_tact_frame"},
        {0x0042a72cu, true, "llm_tact_frame"},
        {0x0042a7d5u, true, "llm_tact_frame"},
        {0x00435db9u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00435e72u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00435e84u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00435f42u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00435f54u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00436012u, true, "llm_tact_ui_order_buttons_minimap_tick"},
        {0x00436024u, true, "llm_tact_ui_order_buttons_minimap_tick"},
    };
    ck_eq((uint32_t)(sizeof(S) / sizeof(S[0])), 35u, "22 enqueue sites + 13 group sites");

    int  player = 0, engine = 0;
    char what[220];
    for (const site &e : S) {
        const bool got = mh::tact::tj_is_player(e.pc);
        snprintf(what, sizeof(what), "%08X in %s -> %s", e.pc, e.fn, e.player ? "PLAYER" : "engine");
        ck(got == e.player, what);
        e.player ? ++player : ++engine;
    }
    ck_eq((uint32_t)player, 20u, "20 player-caused sites (7 enqueue + 13 group)");
    ck_eq((uint32_t)engine, 15u, "15 that must be filtered out (12 engine + 3 nested)");

    // THE BOUNDARIES, because a range test is exactly as good as its ends. One byte outside each
    // range must classify as engine -- an off-by-one here silently journals the first instruction
    // of whatever function follows.
    ck(!mh::tact::tj_is_player(mh::tact::TJ_FRAME_LO - 1), "one byte below llm_tact_frame is engine");
    ck(mh::tact::tj_is_player(mh::tact::TJ_FRAME_LO), "llm_tact_frame's entry is in range");
    ck(mh::tact::tj_is_player(mh::tact::TJ_FRAME_HI - 1), "its last byte is in range");
    ck(!mh::tact::tj_is_player(mh::tact::TJ_FRAME_HI), "one past its end is engine");
    ck(!mh::tact::tj_is_player(mh::tact::TJ_UI_LO - 1), "one byte below the UI pair is engine");
    ck(mh::tact::tj_is_player(mh::tact::TJ_UI_HI - 1), "the UI pair's last byte is in range");
    ck(!mh::tact::tj_is_player(mh::tact::TJ_UI_HI), "one past the UI pair is engine");

    // The group seam sits between the two ranges; the enqueue seam sits after it. Neither entry may
    // read as player, or a seam that called itself would journal itself.
    ck(!mh::tact::tj_is_player(0x0042b09fu), "llm_tact_group_issue_order's own entry is not player");
    ck(!mh::tact::tj_is_player(0x0042b39du), "llm_tact_unit_enqueue_command's own entry is not player");

    // The direct-write offsets must match the record the watch pokes.
    ck_eq(mh::tact::TJ_DIRECT_OFF[mh::tact::TJ_F_GUN],
          (uint32_t)offsetof(mh::game::mh_tact_unit_record, active_gun), "TJ_F_GUN is active_gun");
    ck_eq(mh::tact::TJ_DIRECT_OFF[mh::tact::TJ_F_GROUP],
          (uint32_t)offsetof(mh::game::mh_tact_unit_record, squad_group_id),
          "TJ_F_GROUP is squad_group_id");
    ck_eq(mh::tact::TJ_DIRECT_OFF[mh::tact::TJ_F_DEF],
          (uint32_t)offsetof(mh::game::mh_tact_unit_record, def_stat), "TJ_F_DEF is def_stat");
    ck_eq(mh::tact::TJ_DIRECT_OFF[mh::tact::TJ_F_SEL],
          (uint32_t)offsetof(mh::game::mh_tact_unit_record, status), "TJ_F_SEL is status");

    // THE MASKS, and the selection one is the whole reason they exist. `status` also carries engine
    // bits (8 = FIRE, 0x60 = active/animated); a journal that wrote the byte wholesale would stamp
    // the RECORDING's engine state over the replay's own, which is a corruption no comparison would
    // attribute to the journal.
    ck_eq((uint32_t)mh::tact::TJ_DIRECT_MASK[mh::tact::TJ_F_SEL], 0x01u,
          "selection is bit 0 ONLY -- llm_tact_group_issue_order qualifies on `status & 1`");
    for (int f = 0; f < mh::tact::TJ_F_COUNT; ++f)
        ck(mh::tact::TJ_DIRECT_MASK[f] != 0, "a zero mask would journal a field it can never apply");
    ck_eq((uint32_t)mh::tact::TJ_F_COUNT, 4u, "four direct-write actions, selection included");
}

// ---- T5: the rig's RED-arm lever points where it claims ----------------------------------------
void test_mutate_target() {
    printf("-- T5: tact_mutate_target(i) addresses slice i's live bytes\n");
    arena a;
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        const mh::state::hash_region &r = TACT_HASH_REGIONS[i];
        ck((uint32_t)(uintptr_t)mh::state::tact_mutate_target(i) ==
               mh::state::live_base(r.rid) + r.offset,
           "the poke lever follows the rebase, so `tact_poke_idx` names a real slice");
    }
}

} // namespace

// The fixture (`mh::tact::tact_fixture`) moved to tact_test_support.h at TACT1A -- see
// that file for why, and for the two seeding rules every case here still obeys.

namespace {

// ---- T6: the pilot slice, against the interface -------------------------------------------------
//
// FOUR FUNCTIONS, ONE PER FACET (no state / view / store / shared planes). These are the first
// original-vs-ours cases in this suite; until TACT0 it deliberately had none, because nothing was
// translated and a suite whose whole content is `return 0` is the vacuous green this project keeps
// writing checks to avoid.

void test_pilot_quantize() {
    printf("-- T6a: llm_tact_quantize_facing_dir -- no view, no store\n");
    // Chosen where the branches DIVIDE, not where they agree: 24 is the fold boundary and is NOT
    // folded (the original's JLE), 25 is the first value that folds, and span 8 with dir 22 lands
    // past the boundary after centring. A table of small in-range values would pass under a wrong
    // comparison operator.
    ck_eq((uint32_t)mh::tact::detail::quantize_facing_dir(3, 23), 7u,
          "span 3, dir 23 -> centred 24 (NOT folded, the boundary is exclusive) -> (24-1)/3 = 7");
    ck_eq((uint32_t)mh::tact::detail::quantize_facing_dir(3, 24), 0u,
          "span 3, dir 24 -> centred 25 -> folds to 1 -> (1-1)/3 = 0");
    ck_eq((uint32_t)mh::tact::detail::quantize_facing_dir(8, 22), 0u,
          "span 8, dir 22 -> centred 26 -> folds to 2 -> (2-1)/8 = 0");
    ck_eq((uint32_t)mh::tact::detail::quantize_facing_dir(8, 20), 2u,
          "span 8, dir 20 -> centred 24 (not folded) -> (24-1)/8 = 2");
    ck_eq((uint32_t)mh::tact::detail::quantize_facing_dir(1, 1), 0u,
          "span 1, dir 1 -> centred 1 (span/2 truncates to 0) -> (1-1)/1 = 0");
}

void test_pilot_mouse_in_rect() {
    printf("-- T6b: llm_tact_ui_mouse_in_rect -- the CONST view\n");
    mh::tact::tact_fixture fx;
    fx.sidebar_mouse_x          = 10;
    fx.sidebar_mouse_y          = 20;
    const mh::tact::tact_view v = fx.view();

    ck_eq((uint32_t)mh::tact::detail::ui_mouse_in_rect(v, 0, 0, 100, 100), 1u,
          "cursor strictly inside -> 1");
    // The four boundary cases, which are the only ones that can tell a half-open rectangle from a
    // closed one. A test using only interior points would pass under either reading.
    ck_eq((uint32_t)mh::tact::detail::ui_mouse_in_rect(v, 10, 20, 100, 100), 1u,
          "cursor exactly ON the low edge is INSIDE (x0/y0 inclusive)");
    ck_eq((uint32_t)mh::tact::detail::ui_mouse_in_rect(v, 11, 20, 100, 100), 0u,
          "one past the low x edge -> outside");
    ck_eq((uint32_t)mh::tact::detail::ui_mouse_in_rect(v, 0, 0, 10, 100), 0u,
          "cursor exactly ON the high edge is OUTSIDE (x1 exclusive)");
    ck_eq((uint32_t)mh::tact::detail::ui_mouse_in_rect(v, 0, 0, 100, 20), 0u,
          "cursor exactly ON the high edge is OUTSIDE (y1 exclusive)");
}

void test_pilot_reset_hp() {
    printf("-- T6c: llm_tact_units_reset_hp_for_active -- the STORE (own arena)\n");
    mh::tact::tact_fixture fx;
    // Distinct, non-default, non-symmetric seeds: if two slots a translation could have swapped held
    // the same number, the swap would pass (the sim-state fixture rule).
    for (int32_t i = 0; i < mh::tact::TACT_UNIT_SLOTS; ++i) {
        fx.units[(size_t)i].type = 1;
        fx.units[(size_t)i].hp   = (uint16_t)(0x100 + i);
    }
    // The two ends of the loop, which are where an off-by-one lives and nowhere else.
    fx.units[0].hp                                     = 0xbeef; // slot 0 is SKIPPED
    fx.units[(size_t)mh::tact::TACT_UNIT_LAST_SLOT].hp = 0xcafe; // slot 0x80 IS included
    // A despawned unit: type >= 0x80 means "already removed", and its HP must be left alone.
    fx.units[5].type = 0x81;
    fx.units[5].hp   = 0xd00d;
    fx.units[7].type = 0x80; // exactly the boundary -- see the assertion below

    mh::tact::tact_store own = fx.store();
    mh::tact::detail::units_reset_hp_for_active(own);

    ck_eq(fx.units[0].hp, 0xbeefu, "slot 0 is NOT touched -- the loop starts at 1");
    ck_eq(fx.units[1].hp, 0u, "slot 1 (the first) is cleared");
    ck_eq(fx.units[(size_t)mh::tact::TACT_UNIT_LAST_SLOT].hp, 0u,
          "slot 0x80 IS cleared -- the bound is `i < 0x81`, inclusive of 0x80");
    ck_eq(fx.units[5].hp, 0xd00du, "a DESPAWNED unit (type >= 0x80) keeps its HP");
    // THE BOUNDARY OF THE UNSIGNED COMPARE, which is the only seed that separates `type < 0x80`
    // from `type <= 0x80`. Slot 7 holds exactly 0x80 -- the first value that counts as despawned --
    // so its HP must survive. (There is deliberately no "past the loop" case: the array is [129],
    // indices 0..0x80, and the loop covers 1..0x80, so slot 0 IS the only slot outside it.)
    ck_eq(fx.units[7].hp, (uint32_t)(0x100 + 7),
          "type == 0x80 exactly is already despawned -- HP survives (the compare is `< 0x80`)");
}

void test_pilot_preview_clear() {
    printf("-- T6d: llm_tact_move_path_preview_clear -- the SHARED PLANES\n");
    mh::tact::tact_fixture fx;
    // Seed the WHOLE [256][256] grid, not just the sub-block, so "cleared the right rectangle" and
    // "cleared everything" are distinguishable. Both bytes of the two-readings pair are seeded,
    // because the function must clear exactly ONE of them.
    for (uint32_t i = 0; i < 256u * 256u; ++i) {
        fx.tile_objects[i].unit[0] = (uint8_t)(0x40u + (i & 0x1fu));
        fx.tile_objects[i].unit[1] = (uint8_t)(0x80u + (i & 0x3fu));
    }
    mh::state::mode_planes planes = fx.planes();
    mh::tact::detail::move_path_preview_clear(planes);

    ck_eq(mh::state::tile_overlay(planes, 0, 0), 0u, "(0,0) overlay cleared");
    ck_eq(mh::state::tile_overlay(planes, 0x7f, 0x7f), 0u,
          "(0x7f,0x7f) -- the last cell of the sub-block -- cleared");
    // THE DISCRIMINATING CHECKS. A clear that ran over the full 256x256 grid, or that used the wrong
    // row stride, changes exactly these and nothing the assertions above would notice.
    ck_eq(mh::state::tile_overlay(planes, 0x80, 0), (uint32_t)(0x80u + ((0x80u * 256u) & 0x3fu)),
          "column 0x80 is OUTSIDE the tactical sub-block and is NOT cleared");
    ck_eq(mh::state::tile_overlay(planes, 0, 0x80), (uint32_t)(0x80u + (0x80u & 0x3fu)),
          "row 0x80 is OUTSIDE the tactical sub-block and is NOT cleared");
    // The OTHER byte of the pair must survive -- clearing it would destroy a strategic unit's
    // player field, which is the whole reason the two readings cannot coexist.
    ck_eq(mh::state::tile_occupancy(planes, 3, 4), (uint32_t)(0x40u + (((3u * 256u) + 4u) & 0x1fu)),
          "unit[0] (the occupancy half) is untouched -- only unit[1] is the overlay");
}

// ---- T6e: the shared grid's indexing, probed ASYMMETRICALLY ------------------------------------
//
// ADDED AFTER A MUTANT ESCAPED ITS OWN ASSERTION. The mutation run transposed
// `mode_planes::tile_object_at` to (y<<8)|x and T6d still went red -- but via the occupancy check,
// not via either "outside the sub-block" check. The reason is worth writing down: the tactical
// sub-block is 128x128, i.e. SQUARE, so a transposed clear covers the IDENTICAL set of cells. No
// assertion about WHICH CELLS a square clear touched can ever detect a transpose.
//
// So the indexing is probed directly, at a coordinate where x and y differ, against the raw backing
// store. This is the check that names the bug; T6d's checks are about the rectangle's extent, which
// is a different property.
void test_shared_grid_indexing() {
    printf("-- T6e: mode_planes tile indexing is column-major, probed off the diagonal\n");
    mh::tact::tact_fixture fx;
    for (uint32_t i = 0; i < 256u * 256u; ++i) fx.tile_objects[i].class_owner = 0;
    mh::state::mode_planes planes = fx.planes();

    // (1,0) must land at linear record 256, NOT at record 1. Off the diagonal on purpose.
    planes.tile_object_at(1, 0).class_owner = 0x5a;
    ck_eq(fx.tile_objects[256].class_owner, 0x5au,
          "tile_object_at(1,0) is record 256 -- column-major, (x<<8)|y");
    ck_eq(fx.tile_objects[1].class_owner, 0u,
          "and NOT record 1 -- a transposed index would put it here");

    // The passability plane must agree with the tile grid, since a mission indexes both with the
    // same (col,row) pair. Two planes that disagreed about their stride would corrupt each other's
    // reads with no assertion in T6d able to see it.
    for (uint32_t i = 0; i < 256u * 256u; ++i) fx.passable[i] = 0;
    planes.passable_at(1, 0) = 0x33;
    ck_eq(fx.passable[256], 0x33u, "passable_at(1,0) is byte 256 -- the same stride as the tile grid");
    ck_eq(fx.passable[1], 0u, "and NOT byte 1");

    // The pathfinder arena's three strides, likewise off the diagonal: owner 1 / slot 2 / entry 3
    // must be waypoint 1*30000 + 2*300 + 3, and no permutation of those factors gives the same
    // answer.
    for (size_t i = 0; i < fx.path_buffers.size(); ++i) fx.path_buffers[i].heading = 0;
    planes.path_waypoint_at(1, 2, 3).heading = 0x77;
    ck_eq(fx.path_buffers[(size_t)(1 * mh::state::PATH_WAYPOINTS_PER_OWNER +
                                   2 * mh::state::PATH_WAYPOINTS_PER_SLOT + 3)]
              .heading,
          0x77u, "path_waypoint_at(1,2,3) uses owner*30000 + slot*300 + entry");
}

// ---- T7: the interface FOLLOWS A REBASE ---------------------------------------------------------
//
// The counterfactual matters more than the positive: a view bound ONCE at startup would keep serving
// the abandoned .bss and every consumer would agree with it, silently. `state()` re-resolves per
// call, so moving a region moves both halves.
void test_state_follows_rebase() {
    printf("-- T7: tact state() re-resolves, so both halves follow a rebase\n");
    std::vector<uint8_t> buf =
        std::vector<uint8_t>((size_t)(mh::tact::TACT_UNIT_SLOTS * sizeof(mh::tact::tact_unit)));
    for (size_t k = 0; k < buf.size(); ++k) buf[k] = 0;

    const uint32_t stock = mh::state::live_base(mh::state::RID_TACT_UNITS);
    mh::state::rebase(mh::state::RID_TACT_UNITS, (uint32_t)(uintptr_t)buf.data(),
                      (uint32_t)buf.size());
    {
        mh::tact::tact_state st = mh::tact::state();
        ck((const void *)st.read.units == (const void *)buf.data(),
           "the READ view follows the rebase");
        // The write half has no pointer to compare -- that is W2 working -- so it is proven by
        // WRITING through the accessor and reading the relocated bytes back raw.
        st.own.unit_at(2).hp = 0x1234;
        const uint8_t *raw   = buf.data() + 2 * sizeof(mh::tact::tact_unit) +
                             offsetof(mh::tact::tact_unit, hp);
        ck_eq((uint32_t)(raw[0] | (raw[1] << 8)), 0x1234u,
              "the WRITE store follows too -- the byte landed in the relocated buffer");
    }
    mh::state::unrebase(mh::state::RID_TACT_UNITS);
    {
        mh::tact::tact_state st = mh::tact::state();
        ck((uint32_t)(uintptr_t)st.read.units == stock,
           "and it follows BACK -- nothing was cached (the bound-once counterfactual)");
    }
}

// ---- T11: the journal FIELD SCANNER -------------------------------------------------------------
//
// Added after the first real 4.4-minute recording exposed the defect this pins. The recorder prints
// the mouse deltas signed (%ld) because they ARE signed -- moving left is dx=-1 -- while the scanner
// accepted digits only. A record containing a negative field therefore short-counted, failed
// tj_load's `got_n == need` test, and was discarded as malformed: 2,154 of the 9,311
// mouse events, every leftward and every upward motion. The run still logged "TJ REPLAY ARMED" and
// still replayed. It just replayed a mutilated session.
//
// The scanner was the one piece of the journal format that lived in harness.cpp, where no offline
// test could reach it; it moved into tact/tact_journal.h so this test could exist. That is the
// general lesson worth keeping: a format whose PARSER is untestable is a format with an untested
// half, and the half that silently discards input is the worse one to leave uncovered.
void test_journal_scanner() {
    printf("-- T11: the journal field scanner round-trips signed fields\n");
    using mh::tact::tj_scan_u32;

    struct one {
        const char *text;
        bool        ok;
        uint32_t    want;
        const char *what;
    };
    static const one CASES[] = {
        {"0", true, 0u, "zero"},
        {"15831", true, 15831u, "a real frame index"},
        {"  42", true, 42u, "leading spaces are skipped"},
        {"4294967295", true, 0xffffffffu, "u32 max"},
        {"-1", true, (uint32_t)-1, "dx = -1 -- THE case that was being dropped"},
        {"-3", true, (uint32_t)-3, "dy = -3, verbatim from the real journal"},
        {"+5", true, 5u, "an explicit plus"},
        {"-0", true, 0u, "negative zero is zero"},
        {"", false, 0u, "empty is a parse FAILURE, not a silent zero"},
        {"-", false, 0u, "a lone sign is a parse failure"},
        {"x9", false, 0u, "non-numeric is a parse failure"},
    };
    for (const one &c : CASES) {
        const char *end = c.text;
        while (*end) ++end;
        uint32_t    v = 0xdeadbeefu;
        const char *r = tj_scan_u32(c.text, end, &v);
        ck((r != nullptr) == c.ok, c.what);
        if (c.ok && r) ck_eq(v, c.want, c.what);
    }

    // A WHOLE mouse record in the exact shape the recorder emits, negatives in both delta slots.
    // Reaching 9 fields is precisely what tj_load() requires before it accepts a line, so this is
    // the assertion that fails on the old scanner.
    {
        const char  rec[] = "28 1 0 332 211 -1 -3 0 2271";
        const char *p = rec, *e = rec + sizeof(rec) - 1;
        uint32_t    v[9] = {0};
        int         n    = 0;
        for (; n < 9; ++n) {
            p = tj_scan_u32(p, e, &v[n]);
            if (!p) break;
        }
        ck(n == 9, "a whole mouse record parses -- all 9 fields, not a short count");
        if (n == 9) {
            ck_eq(v[0], 28u, "mouse record: frame");
            ck_eq(v[3], 332u, "mouse record: x");
            ck_eq(v[4], 211u, "mouse record: y");
            ck((int32_t)v[5] == -1, "mouse record: dx survives as -1");
            ck((int32_t)v[6] == -3, "mouse record: dy survives as -3");
            ck_eq(v[8], 2271u, "mouse record: the timestamp is carried verbatim");
        }
    }

    // MUTATION CHECK. A scanner that dropped the sign would pass everything above if the
    // expectations were written to match it, so pin the DIFFERENCE rather than the values: a
    // negative field must not read back as its positive twin.
    {
        const char sn[] = "-2", sp[] = "2";
        uint32_t   neg = 0, pos = 0;
        tj_scan_u32(sn, sn + 2, &neg);
        tj_scan_u32(sp, sp + 1, &pos);
        ck(neg != pos, "mutation: -2 and 2 must NOT scan to the same value");
        ck((int32_t)neg == -(int32_t)pos, "mutation: -2 is exactly the negation of 2");
    }
}

// ---- T11b: the hook-free ORDER watch ------------------------------------------------------------
//
// The `Q` record exists because the `E`/`G` ones are trampolines on the ORIGINAL order entries, and
// a promoted llm_tact_frame calls its own translated sibling instead -- measured on both recorded
// sessions 2026-09-04: ZERO E and ZERO G records survive `[promote] tact_frame=1`, while the
// mission outcome, survivor set, end frame and pinned RNG series are byte-identical. An order diff
// that goes silent exactly when our code starts running is not a gate, and it had to be printed as
// ADVISORY for precisely that reason.
//
// Two things are worth pinning offline, and neither needs a rig:
//
//   THE GEOMETRY, because the watch reads raw bytes at a stride. Slot 1 lands on an ODD address
//   (0x54 + 0xb), so every uint16_t field in every odd slot is unaligned -- which is why the readers
//   are byte-wise rather than a struct cast, and why a reader that "simplified" to a cast would
//   still pass slot 0 and quietly misread the other 127.
//
//   THE PREDICATE, because it decides what the journal contains. Its whole job is the asymmetry: a
//   slot going LIVE or changing while live is an order; a slot going EMPTY is the sim draining its
//   own queue and must not be recorded, or a real mission would bury its orders under thousands of
//   dequeues. The stale-interrupt_flag case is the trap that makes this subtle: llm_tact_unit_cmd_-
//   advance zeroes op and the four args but NOT the flag byte (see the struct comment), so a naive
//   "any byte moved" predicate emits a phantom record on every dequeue whose flag differs.
void test_journal_queue_watch() {
    printf("-- T11b: the hook-free order watch -- geometry and record predicate\n");
    using namespace mh::tact;

    // Geometry. The constants are asserted against the generated record in harness.cpp; here it is
    // the STRIDE ARITHMETIC that gets pinned, which is the part harness.cpp's static_asserts cannot
    // see (they check the type, not tj_q_off's indexing of it).
    ck_eq(tj_q_off(0), 0x54u, "slot 0 is cmd_queue[0] at +0x54");
    ck_eq(tj_q_off(1), 0x5fu, "slot 1 is +0xb further -- and ODD, hence the byte-wise readers");
    ck_eq(tj_q_off(127), 0x54u + 0xbu * 127u, "the last real slot");
    ck_eq(tj_q_off(127) + 0xbu, 0x5d4u, "the queue ends exactly where the ATTACK record begins");
    ck_eq(tj_q_off(128), 0x5d4u, "pseudo-slot 128 is the immediate ATTACK/AIM record");
    ck_eq(tj_q_off(129), 0x5dfu, "pseudo-slot 129 is the immediate FACE/TURN record");
    ck_eq(tj_q_off(128) + 0xbu, tj_q_off(129), "the two immediate records tile with no gap");
    ck_eq((uint32_t)TJ_Q_TOTAL, 130u, "128 queue slots + the 2 immediate records");

    // Field readers, on an entry whose every byte is distinct so a mixed-up index cannot pass.
    {
        const uint8_t e[TJ_Q_STRIDE] = {0x07, 0x01, 0x00, 0x22, 0x11, 0x44, 0x33,
                                        0x66, 0x55, 0x88, 0x77};
        ck_eq((uint32_t)e[0], 7u, "interrupt_flag is the entry's leading byte");
        ck_eq((uint32_t)tj_q_op(e), 1u, "op is the little-endian uint16 at +1");
        ck_eq((uint32_t)tj_q_arg(e, 0), 0x1122u, "arg0 at +3");
        ck_eq((uint32_t)tj_q_arg(e, 1), 0x3344u, "arg1 at +5");
        ck_eq((uint32_t)tj_q_arg(e, 2), 0x5566u, "arg2 at +7");
        ck_eq((uint32_t)tj_q_arg(e, 3), 0x7788u, "arg3 at +9");
    }

    // The predicate. `live` and `live2` differ only in arg0, which is the minimum a rewrite can be.
    {
        const uint8_t empty[TJ_Q_STRIDE] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint8_t live[TJ_Q_STRIDE]  = {0, 1, 0, 5, 0, 9, 0, 0, 0, 0, 0};
        const uint8_t live2[TJ_Q_STRIDE] = {0, 1, 0, 6, 0, 9, 0, 0, 0, 0, 0};
        // A dequeue leaves op and the args zeroed but the flag byte STALE -- the exact shape the
        // naive predicate turns into a phantom record.
        const uint8_t drained[TJ_Q_STRIDE] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

        ck(tj_q_record(empty, live), "an APPEND is recorded");
        ck(tj_q_record(live, live2), "an in-place REWRITE is recorded (the op-1 MOVE redirect)");
        ck(!tj_q_record(live, live), "an unchanged live slot is NOT recorded every frame");
        ck(!tj_q_record(live, empty), "a DEQUEUE is not an order -- must not be recorded");
        ck(!tj_q_record(live, drained),
           "a dequeue leaving interrupt_flag STALE is still not recorded -- the op==0 early-out");
        ck(!tj_q_record(empty, empty), "an empty slot that stayed empty is silent");

        // MUTATION CHECK, aimed at the two ways this predicate plausibly gets written wrong. A
        // predicate testing only `op` misses the rewrite; one testing only "any byte moved" fires
        // on the stale-flag dequeue. Pin that no single relaxation satisfies both rows.
        ck(tj_q_record(live, live2) && !tj_q_record(live, drained),
           "mutation: op-only would miss the rewrite, bytes-only would fire on the drain");
    }
}

// ---- T12: the TACTICAL tile_objects window -----------------------------------------------------
//
// The tactical hash walks the 128x128 top-left sub-block of the [256][256] plane rather than all of
// it, because a mission writes only that (llm_tact_map_reset wipes tile_objects[col][row] over
// 0..0x7f at the 256-element stride -- the tactical-probe work Sect. 5). That cut 48.4% of the
// per-frame hash volume, which makes the window a PERFORMANCE claim resting on a CORRECTNESS claim,
// and the correctness half is the one that can rot: if a mission ever wrote outside the window the
// oracle would go blind there silently, since a hash that never reads a byte cannot report it.
//
// So this pins both halves mechanically: the window must be sensitive to every byte INSIDE it, and
// provably indifferent to every byte outside. The second assertion is the load-bearing one and is
// exactly what makes the optimisation safe to keep.
static void test_tile_window() {
    printf("-- T12: the tactical tile_objects window is the 128x128 sub-block, exactly\n");
    using namespace mh::state;

    constexpr uint32_t LEN   = 256u * 256u * 8u;
    auto              *plane = (uint8_t *)calloc(LEN, 1);
    if (!plane) {
        ck(false, "T12: allocation");
        return;
    }

    auto window_hash = [&]() {
        hash_sink h(sink_mode::VERDICT);
        emit_tile_objects_tactical(plane, LEN, h);
        return h.finish();
    };
    const uint64_t base = window_hash();

    // INSIDE: every corner of the sub-block must move the hash. Corners rather than a sweep because
    // an off-by-one in either index shows up at exactly one of them -- a mid-block probe would pass
    // against a window one row or one column wrong.
    struct pt {
        uint32_t    outer, inner;
        const char *what;
    } const inside[] = {
        {0, 0, "first element"},
        {0, TACT_TILE_DIM - 1, "end of the first row"},
        {TACT_TILE_DIM - 1, 0, "start of the last row"},
        {TACT_TILE_DIM - 1, TACT_TILE_DIM - 1, "last element of the sub-block"},
        {63, 63, "the middle"},
    };
    for (const pt &q : inside) {
        const uint32_t i = (q.outer * TILE_PLANE_STRIDE + q.inner) * TILE_ELEM;
        plane[i + 2] ^= 0xff; // building lo -- a field the emitter reads
        const bool moved = window_hash() != base;
        plane[i + 2] ^= 0xff;
        ck(moved, q.what);
    }

    // OUTSIDE: the bytes the window skips must be invisible. These are the 384 KB per frame the
    // narrowing stops reading, and the claim is that no tactical code writes them -- so if one ever
    // does, the verdict must not be the thing that notices, because it cannot.
    struct pt const outside[] = {
        {0, TACT_TILE_DIM, "one column PAST the window on row 0"},
        {TACT_TILE_DIM, 0, "one row PAST the window"},
        {TACT_TILE_DIM, TACT_TILE_DIM, "the far diagonal corner"},
        {TILE_PLANE_STRIDE - 1, TILE_PLANE_STRIDE - 1, "the last element of the whole plane"},
        {200, 12, "outside by the OUTER index only"},
        {12, 200, "outside by the INNER index only"},
    };
    for (const pt &q : outside) {
        const uint32_t i = (q.outer * TILE_PLANE_STRIDE + q.inner) * TILE_ELEM;
        for (uint32_t b = 0; b < TILE_ELEM; ++b) plane[i + b] ^= 0xff; // the WHOLE element
        const bool moved = window_hash() != base;
        for (uint32_t b = 0; b < TILE_ELEM; ++b) plane[i + b] ^= 0xff;
        ck(!moved, q.what);
    }

    // The fog bit stays masked inside the window too -- the narrowing must not have quietly changed
    // WHAT is emitted, only HOW MUCH. A camera pan writes visibility, and that is input, not sim.
    {
        plane[7] ^= 0xff; // element 0's visibility byte
        const bool moved = window_hash() != base;
        plane[7] ^= 0xff;
        ck(!moved, "visibility is still not emitted (per-frame fog)");
    }
    {
        plane[1] ^= 0x40; // a flags bit ABOVE TILE_FLAGS_KEEP (0x3fff)
        const bool moved = window_hash() != base;
        plane[1] ^= 0x40;
        ck(!moved, "the fog bit in flags is still masked out");
    }
    {
        plane[0] ^= 0x01; // a flags bit INSIDE the keep mask
        const bool moved = window_hash() != base;
        plane[0] ^= 0x01;
        ck(moved, "a kept flags bit still moves the hash");
    }

    // PERSIST mode is the seed blob and has always written this plane FLAT -- narrowing the verdict
    // must not narrow the blob, or a restored seed would differ from the one that was captured.
    {
        hash_sink flat(sink_mode::PERSIST);
        emit_tile_objects_tactical(plane, LEN, flat);
        hash_sink win(sink_mode::VERDICT);
        emit_tile_objects_tactical(plane, LEN, win);
        plane[(200 * TILE_PLANE_STRIDE + 200) * TILE_ELEM] ^= 0xff;
        hash_sink flat2(sink_mode::PERSIST);
        emit_tile_objects_tactical(plane, LEN, flat2);
        ck(flat2.finish() != flat.finish(), "PERSIST still covers the WHOLE plane, not the window");
        plane[(200 * TILE_PLANE_STRIDE + 200) * TILE_ELEM] ^= 0xff;
    }

    free(plane);
}

// ---- T13: the hash is SPLIT-INVARIANT ----------------------------------------------------------
//
// The one property a wider hash step can silently destroy. `raw()` is called with whatever length
// the emitter happens to hand it -- 1, 2, 7, 0x5f4 -- and the verdict must depend on the BYTES, not
// on how they were chopped into calls. Byte-serial FNV had this for free; a block hash has it only
// because hash_sink carries a partial block between calls.
//
// Not hypothetical: packing tile_objects' six per-element calls into one (identical bytes, identical
// order) would, under a hash that consumed only whole blocks per call, have changed every tactical
// hash -- an invisible break caused by a refactor that touched no semantics at all.
void test_hash_split_invariance() {
    printf("-- T13: the hash depends on the bytes, not on how they were split into calls\n");
    using namespace mh::state;

    uint8_t buf[173];
    for (uint32_t i = 0; i < sizeof(buf); ++i) buf[i] = (uint8_t)(i * 37 + 11);

    hash_sink one(sink_mode::VERDICT);
    one.raw(buf, sizeof(buf));
    const uint64_t whole = one.finish();

    const uint32_t chunks[] = {1, 2, 3, 5, 7, 8, 9, 16, 64};
    for (uint32_t c : chunks) {
        hash_sink h(sink_mode::VERDICT);
        for (uint32_t i = 0; i < sizeof(buf); i += c) {
            const uint32_t n = (i + c <= sizeof(buf)) ? c : (uint32_t)sizeof(buf) - i;
            h.raw(&buf[i], n);
        }
        ck(h.finish() == whole, "split into fixed chunks hashes the same as one call");
    }
    {
        // The literal old-vs-new tile_objects shape, on one 8-byte element.
        hash_sink six(sink_mode::VERDICT);
        six.raw(&buf[0], 2);
        six.raw(&buf[2], 1);
        six.raw(&buf[3], 1);
        six.raw(&buf[4], 1);
        six.raw(&buf[5], 1);
        six.raw(&buf[6], 1);
        hash_sink packed(sink_mode::VERDICT);
        packed.raw(&buf[0], 7);
        ck(six.finish() == packed.finish(),
           "the six-call and packed tile_objects shapes agree (the refactor that could have broken)");
    }
    {
        hash_sink h(sink_mode::VERDICT);
        uint32_t  i = 0, step = 1;
        while (i < sizeof(buf)) {
            const uint32_t n = (i + step <= sizeof(buf)) ? step : (uint32_t)sizeof(buf) - i;
            h.raw(&buf[i], n);
            i += n;
            step = (step * 3 + 1) % 11 + 1;
        }
        ck(h.finish() == whole, "a ragged split hashes the same as one call");
    }
    {
        hash_sink h(sink_mode::VERDICT);
        h.raw(buf, sizeof(buf));
        const uint64_t a = h.finish();
        ck(h.finish() == a, "finish() is idempotent");
    }
    // ...and the hash must still be SENSITIVE: an invariance test passes trivially if the hash
    // ignores its input, which is the failure mode this whole file guards against.
    {
        hash_sink h(sink_mode::VERDICT);
        buf[100] ^= 0x01;
        h.raw(buf, sizeof(buf));
        buf[100] ^= 0x01;
        ck(h.finish() != whole, "one flipped bit still changes the hash");
    }
    {
        hash_sink a(sink_mode::VERDICT), b(sink_mode::VERDICT);
        a.raw(buf, 3);
        b.raw(buf, 4);
        ck(a.finish() != b.finish(), "a 3-byte tail does not alias a 4-byte one");
    }
}

} // namespace

namespace mh::tact::test {
void run_mission_parse_tests();
void run_cfg_keyword_tests();
void run_mission_disposition_tests();
void run_character_parse_tests();
// TACT1A batch A (2026-08-26): the offline oracle for llm_tact_squad_sync_hp, which
// the rig arms correctly but never calls under a no-input scenario (see tact_squad_status.h).
void run_squad_status_tests();
// TACT1B (2026-08-26, verification debt drain): the offline oracle for
// llm_tact_unit_cmd_advance_with_defstat -- op 0xb is mission-script-data-driven and unreachable by
// TACT-SYNTH (see tact_unit_cmd_advance_with_defstat.h).
void run_unit_cmd_advance_with_defstat_tests();
// TACT1A/TACT1B (2026-08-26): the offline oracle for llm_tact_move_path_preview_walk
// (pins the out-pointer result the rig's region-only comparison cannot see) and for
// llm_tact_unit_cmd_teleport_jump_tick (structurally blocked on open TACT-CUT2 for rig-arming --
// see tact_unit_cmd_teleport_jump_tick.h).
void run_move_path_preview_walk_tests();
void run_unit_cmd_teleport_jump_tick_tests();
// (2026-08-26, the 2026-08-26 run): llm_tact_teleport_cmdqueue_jump, structurally
// blocked on open TACT-CUT2 for rig-arming the same way its caller above is -- see
// tact_teleport_cmdqueue_jump.h.
void run_teleport_cmdqueue_jump_tests();
void run_facing_to_delta_tests();
void run_calc_dir24_tests();
// The offline oracle for llm_tact_unit_cmd_stance_on/_off -- proves the status-bit
// flip and the dispatch-skip condition (mocking the one outward call), deliberately asserting
// nothing about wander_check_time's value -- see tact_unit_cmd_stance.h's header banner for why a
// clean rig verdict is not obtainable here (a live-clock-read hazard, not a translation bug).
void run_unit_cmd_stance_tests();
// (2026-08-26): the offline oracle for llm_tact_unit_teleport, structurally
// blocked on the open TACT-CUT2 item for rig-arming the same way its two callers above are (its own
// body is the closure's entry into the two ungated effectful callees) -- see tact_unit_teleport.h.
void run_unit_teleport_tests();
// TACT1A batch A (2026-08-26): the offline oracle for llm_strat_squad_assault_resolve --
// structurally un-shadow-armable, since its own two direct outward calls are themselves two of the
// seven TACT-CUT2-ungated `effectful` shared callees it reaches -- see
// tact_squad_assault_resolve.h.
void run_squad_assault_resolve_tests();
// TACT1A batch A (2026-08-26): the offline oracle for llm_tact_map_reset -- its own tail call
// (mission_load) reaches the same TACT-CUT2-ungated effectful callees mission_load itself does --
// see tact_map_reset.h.
void run_map_reset_tests();
// TACT1C (2026-08-26): the offline oracle for llm_tact_unit_despawn -- its own closure
// is mostly unregistered presentation scratch, not worth declaring to rig-arm a 189-byte function --
// see tact_unit_despawn.h.
void run_unit_despawn_tests();
// TACT1A (2026-08-26): the offline oracle for llm_tact_mission_end_return_to_strategic -- one of
// its nine outward calls, llm_snd_ambient_reseed_planet_event_times, is itself one of TACT-CUT2's
// ungated `effectful` shared callees (a real PRNG advance) -- see
// tact_mission_end_return_to_strategic.h.
void run_mission_end_return_to_strategic_tests();
// TACT1A/TACT1C central batch: the offline oracles for the seven-function fan-out. Five are
// structurally un-rig-armable (each TU header carries its BFS-vs-ungated-effectful derivation);
// unit_death_tick's sole callee drags the despawn-style presentation cascade (mocked instead);
// unit_kneel_tick's oracle is defence-in-depth on its pure arms only -- the shadow arm is that
// row's primary instrument (its outward calls are direct mh::call:: and cannot be mocked).
void run_mission_start_tests();
void run_unit_update_anim_tests();
void run_unit_weapons_tick_tests();
void run_fx_update_projectile_tests();
void run_unit_spawn_tests();
void run_unit_death_tick_tests();
void run_unit_kneel_tick_tests();
// TACT1A's final row: the POZ top-level orchestrator, offline (five ungated-effectful names in its
// closure -- see tact_mission_load.h); the oracle drives it with REAL shipped mission text through
// a mocked file layer and carries the adversarial review's two counterexamples as regressions.
void run_mission_load_tests();
// TACT1C batch (2026-08-26): the offline oracles for the eight-function fan-out. Two (scatter,
// muzzle offset) are STRUCTURALLY un-shadowable (out-pointer-only writes); four (fire_weapon,
// unit_destroy, fx_spawn, fx_splash_damage) are OFFLINE by measured region-count/effect-reach cost,
// not by choice -- see each TU's own header banner for its derivation.
void run_unit_get_muzzle_offset_tests();
void run_fx_splash_damage_tests();
void run_weapon_calc_scatter_tests();
// Offline: the only reachable scenario is --tact-synth, which is the exact scenario
// the wander_check_time investigation already spent three hypotheses on without resolving
// -- see this file's own banner.
void run_unit_stand_tick_tests();
void run_unit_fire_weapon_tests();
void run_fx_spawn_tests();
void run_unit_destroy_tests();
void run_unit_vision_tests();
// TACT1D (2026-08-28): the FIRST oracle to execute llm_tact_fov_raycast_stencil's body --
// tact_unit_vision_selftest.cpp MOCKS it, which is how the AH/AL map-edge wrap bug survived a
// verified/T1 row. See the file banner for what it can and cannot yet cover.
void run_fov_raycast_tests();
// TACT1B (2026-08-28): the item's two outstanding acceptance clauses -- the cmd_queue wrap at
// index 0x80 with its STALE interrupt_flag, and the weapons-tick stall trap REPRODUCED (not fixed).
void run_cmd_queue_wrap_tests();
void run_owner_enqueue_fix_tests(); // TACT1-P red-audit pins, 2026-09-02
void run_door_tests();
void run_ambient_sound_tests();
void run_cam_follow_selection_tick_tests();
void run_scroll_target_proximity_tick_tests();
void run_view_shift_tests();
void run_select_next_unit_tests();
void run_sidebar_dispatch_tests();
void run_ui_sel_panel_init_tests();
void run_update_units_and_fx_tests();
void run_selection_clear_unless_ctrl_tests();
void run_camera_center_on_tile_tests();
void run_selection_panel_refresh_tests();
void run_active_unit_count_hud_draw_tests();
void run_squad_roster_refresh_tests();
void run_ui_sel_panel_single_mode_tick_tests();
void run_ui_order_buttons_minimap_tick_tests();
void run_ui_sel_panel_multi_mode_tick_tests();
} // namespace mh::tact::test

int run_tacttest() {
    printf("== tacttest (TACT-DOMAIN: the offline tactical oracle) ==\n");
    // THE INSTRUMENT HALF RUNS FIRST AND IS AUDITED FOR HAVING RUN (TACT1's acceptance).
    //
    // The clause this satisfies asked for "the original 371 instrument checks intact ALONGSIDE the
    // per-function cases". 371 IS A STALE CONSTANT AND CANNOT BE THE GATE: test_manifest and
    // test_slice_coverage iterate TACT_HASH_REGION_COUNT, so the count moves whenever the tactical
    // state interface gains a region -- it went 14 -> 15 after TACT0, and the five audits that
    // existed when 371 was written now contribute 406. A literal-371 assertion would fail on a
    // CORRECT change, which is how such an assertion gets deleted rather than fixed.
    //
    // So gate the SHAPE instead, which is what "intact" actually means: every audit still runs and
    // still contributes at least one check (the real failure mode -- an audit quietly dropped from
    // this list, or hollowed out into a no-op, which no total can see), and the block scales with
    // the region table rather than collapsing to a constant.
    struct instrument_audit {
        const char *name;
        void (*fn)();
    };
    static const instrument_audit kInstrumentAudits[] = {
        {"T1 layout", test_layout},
        {"T2 manifest", test_manifest},
        {"T3 slice coverage", test_slice_coverage},
        {"T4 tile_objects mask", test_tile_objects_mask},
        {"T3 tile window", test_tile_window},
        {"T3 hash split invariance", test_hash_split_invariance},
        {"T3 sim slice", test_sim_slice},
        {"T3 verdict pair", test_verdict_pair},
        {"T3 journal sites", test_journal_sites},
        {"T3 journal scanner", test_journal_scanner},
        {"T3 journal queue watch", test_journal_queue_watch},
        {"T5 mutate target", test_mutate_target},
    };
    for (const instrument_audit &a : kInstrumentAudits) {
        const int before = mh::tact::test::g_checks;
        a.fn();
        ck(mh::tact::test::g_checks > before, a.name);
    }
    // The per-audit loop cannot see an audit DELETED from the table -- it only walks what is
    // there -- so the roster size is asserted too. This constant is hand-maintained on purpose:
    // adding an audit is a deliberate act and bumping it is the cheap half of that act.
    ck_eq((uint32_t)(sizeof(kInstrumentAudits) / sizeof(kInstrumentAudits[0])), 12u,
          "all 12 instrument audits are still in the roster");
    const int instrument_checks = mh::tact::test::g_checks;
    ck(instrument_checks >= 25 * (int)TACT_HASH_REGION_COUNT,
       "the instrument half scales with the region table (>= 25 checks per hash region)");
    printf("-- instrument half: %d checks over %d audits, %d hash regions\n", instrument_checks,
           (int)(sizeof(kInstrumentAudits) / sizeof(kInstrumentAudits[0])),
           (int)TACT_HASH_REGION_COUNT);
    test_pilot_quantize();
    test_pilot_mouse_in_rect();
    test_pilot_reset_hp();
    test_pilot_preview_clear();
    test_shared_grid_indexing();
    test_state_follows_rebase();
    mh::tact::test::run_mission_parse_tests();
    mh::tact::test::run_cfg_keyword_tests();
    mh::tact::test::run_mission_disposition_tests();
    mh::tact::test::run_character_parse_tests();
    mh::tact::test::run_squad_status_tests();
    mh::tact::test::run_unit_cmd_advance_with_defstat_tests();
    mh::tact::test::run_move_path_preview_walk_tests();
    mh::tact::test::run_unit_cmd_teleport_jump_tick_tests();
    mh::tact::test::run_teleport_cmdqueue_jump_tests();
    mh::tact::test::run_facing_to_delta_tests();
    mh::tact::test::run_calc_dir24_tests();
    mh::tact::test::run_unit_cmd_stance_tests();
    mh::tact::test::run_unit_teleport_tests();
    mh::tact::test::run_squad_assault_resolve_tests();
    mh::tact::test::run_map_reset_tests();
    mh::tact::test::run_unit_despawn_tests();
    mh::tact::test::run_mission_end_return_to_strategic_tests();
    mh::tact::test::run_mission_start_tests();
    mh::tact::test::run_unit_update_anim_tests();
    mh::tact::test::run_unit_weapons_tick_tests();
    mh::tact::test::run_fx_update_projectile_tests();
    mh::tact::test::run_unit_spawn_tests();
    mh::tact::test::run_unit_death_tick_tests();
    mh::tact::test::run_unit_kneel_tick_tests();
    mh::tact::test::run_mission_load_tests();
    mh::tact::test::run_unit_get_muzzle_offset_tests();
    mh::tact::test::run_fx_splash_damage_tests();
    mh::tact::test::run_weapon_calc_scatter_tests();
    mh::tact::test::run_unit_stand_tick_tests();
    mh::tact::test::run_unit_fire_weapon_tests();
    mh::tact::test::run_fx_spawn_tests();
    mh::tact::test::run_unit_destroy_tests();
    mh::tact::test::run_unit_vision_tests();
    mh::tact::test::run_fov_raycast_tests();
    mh::tact::test::run_cmd_queue_wrap_tests();
    mh::tact::test::run_owner_enqueue_fix_tests(); // TACT1-P red-audit pins, 2026-09-02
    mh::tact::test::run_door_tests();
    mh::tact::test::run_ambient_sound_tests();
    mh::tact::test::run_cam_follow_selection_tick_tests();
    mh::tact::test::run_scroll_target_proximity_tick_tests();
    mh::tact::test::run_view_shift_tests();
    mh::tact::test::run_select_next_unit_tests();
    mh::tact::test::run_sidebar_dispatch_tests();
    mh::tact::test::run_ui_sel_panel_init_tests();
    mh::tact::test::run_update_units_and_fx_tests();
    mh::tact::test::run_selection_clear_unless_ctrl_tests();
    mh::tact::test::run_camera_center_on_tile_tests();
    mh::tact::test::run_selection_panel_refresh_tests();
    mh::tact::test::run_active_unit_count_hud_draw_tests();
    mh::tact::test::run_squad_roster_refresh_tests();
    mh::tact::test::run_ui_sel_panel_single_mode_tick_tests();
    mh::tact::test::run_ui_order_buttons_minimap_tick_tests();
    mh::tact::test::run_ui_sel_panel_multi_mode_tick_tests();

    printf("%d checks, %d failures\n", mh::tact::test::g_checks,
           mh::tact::test::g_fails);
    return mh::tact::test::g_fails ? 1 : 0;
}
