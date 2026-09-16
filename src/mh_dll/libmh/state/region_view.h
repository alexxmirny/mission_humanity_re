//
// state/region_view.h -- emit a hash-manifest slice into a sink (RI-STATE / ST6, phase 1).
//
// The determinism harness used to reach every region as `REGIONS[i].base` + `len` and hash, dump or
// poke the bytes itself. This is the indirection that lets a region stop being bytes at an address
// without every consumer having to know: a consumer asks a SLICE to emit itself into a SINK.
//
// PHASE 1 IS RAW ONLY, deliberately. Every slice is still the game's own .bss, so `emit` is a flat
// block for all but the three that carry a legacy verdict mask. No owner can claim a slice yet --
// that is phase 2, and it lands WITH the first owner (ST4, orders) rather than as speculative
// plumbing. What phase 1 buys is that harness.cpp stops dereferencing a base, so phase 2 changes one
// file instead of six call sites.
//
// WHY THE RAW VIEWS BRANCH ON MODE, and why that is not a design smell to fix here. For the three
// masked regions the PERSIST and VERDICT walks are genuinely different walks, not one walk with a
// mask:
//
//   tile_objects  VERDICT REORDERS the record -- flags lo/hi, building LO, unit LO, class_owner,
//                 then building HI, unit HI -- and drops p[+7] (per-frame visibility) entirely,
//                 while the seed blob writes the same region as a flat block.
//   units         VERDICT substitutes a zero for ctrl_group_id in every 0xe9-byte record.
//   rng_state     VERDICT substitutes a zero for slot 1 (the per-FRAME fx channel).
//
// The tile_objects ordering is ARBITRARY -- an artifact of how the function was written -- and it is
// transcribed exactly, not tidied. The reason USED to be "frozen by the golden: every per-region
// hash in every historical run becomes incomparable otherwise". That reason expired on 2026-08-25,
// when the hash step was widened from 1 byte to 8 and every historical value became incomparable
// anyway (the tactical-probe work 9s). What the freeze still buys is INTERNAL consistency -- the
// strategic and tactical walkers must feed the same stream for the same bytes -- so the order stays
// put, but do not cite cross-build comparability for it. A run's hash implementation is now
// self-identifying: harness.cpp hash_fingerprint_report emits a fingerprint of the live sink, and
// tools/test_ui.py soak_golden refuses to compare a golden recorded under a different one.
//
// It collapses at ownership. An owned region emits typed fields in declaration order and marks the
// fog byte `local()`; the reorder has nowhere to live and PERSIST/VERDICT become one traversal for
// real. Phase 1 records the gap rather than papering over it.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/region_owner.h"
#include "state/state_sink.h"

namespace mh::state {

// The bit the fog occupies in tile_objects' flags word; the low 14 bits are state.
inline constexpr uint16_t TILE_FLAGS_KEEP = 0x3fffu;
// The rng slot drawn per RENDERED FRAME (effects). Slots 0 (strategic) and 2 (AI) are drawn only
// from the deterministic sim path and MUST match; see the rng slot map in mh_regions.gen.h.
inline constexpr uint32_t RNG_SLOT_FX = 1;

// ---- the raw views -------------------------------------------------------------------------

inline void emit_units(const uint8_t *p, uint32_t len, state_sink &s) {
    constexpr uint32_t STRIDE = sizeof(mh::game::mh_map_object_unit);
    constexpr uint32_t OFF    = offsetof(mh::game::mh_map_object_unit, ctrl_group_id);
    for (uint32_t o = 0; o + STRIDE <= len; o += STRIDE) {
        s.bytes(&p[o], OFF);
        s.local(&p[o + OFF], 1); // ctrl_group_id: local-player selection, not sim state
        s.bytes(&p[o + OFF + 1], STRIDE - OFF - 1);
    }
}

// ---- soldiers: mask anim_change_count, advanced ONLY by drawing (2026-08-28) --------------------
// llm_strat_crew_soldier is 0x1d bytes; +0x1b `anim_change_count` is the idle-animation nudge
// counter. It is the THIRD field of the class this file already masks twice, and it is the clearest
// case of the three: MEASURED against the state matrix, its only accessors in the whole image are
//   llm_strat_render_bldg_docked_unit   read 0x004518b0, INC 0x00451955
//   llm_strat_render_tile_object        read 0x00452443, INC 0x004524e8
//   llm_strat_unit_soldier_get_sprite_screen_pos  read 0x00491166
// -- two renderers that read-then-increment it as they DRAW, and a sprite-screen-position helper.
// NOTHING in the sim reads or writes it, so it can never influence simulation; a divergence here is
// a wrong VERDICT, not a desync. And it diverges by construction in multiplayer: peers hold
// different camera positions, so a soldier drawn by one peer and off-screen for the other gets a
// different count. Same reasoning as the fog mask (frame-driven, not sim-driven) and the
// ctrl_group_id mask (peer-local), both above.
//
// THE NEIGHBOURING BYTE IS NOT MASKED, and the boundary is the point: +0x1c `idle_wander_flag` is
// set by the sim's 1% idle wander roll (llm_strat_unit_update_soldiers, llm_strat_unit_state_move_
// walker both write it) -- RNG-driven simulation state that MUST stay in the verdict. Masking the
// record's tail rather than the single byte would have swallowed it.
inline void emit_soldiers(const uint8_t *p, uint32_t len, state_sink &s) {
    constexpr uint32_t STRIDE = sizeof(mh::game::mh_llm_strat_crew_soldier);
    constexpr uint32_t OFF    = offsetof(mh::game::mh_llm_strat_crew_soldier, anim_change_count);
    for (uint32_t o = 0; o + STRIDE <= len; o += STRIDE) {
        s.bytes(&p[o], OFF);
        s.local(&p[o + OFF], 1);                    // anim_change_count: advanced per DRAW, never read by the sim
        s.bytes(&p[o + OFF + 1], STRIDE - OFF - 1); // idle_wander_flag stays: sim RNG state
    }
}

// ---- planets: mask the per-planet GRAPHICS bytes (LIFT-TABLE S2, 2026-09-09) -------------------
// cfg_final_struct_Planet is 0x427 bytes and TWO non-contiguous windows of it are graphics, not
// simulation -- 102 of 1063 bytes, with 0x98 bytes of sim between them:
//
//   +0x38d  bank[100]                      written ONLY by cfg_final_planet_FillBankData; read ONLY
//                                          by llm_gfx_planet_bank_needed / llm_gfx_load_banks /
//                                          llm_gfx_load_planet_extra_sprite_banks -- all gfx.
//   +0x425  tlo_index                      cfg_GetTloIndex's 8-way table pick, whose only consumer
//   +0x426  soldier_sprite_bank_offset     is the sprite-bank selection (`SHL AL,6`) beneath it.
//
// WHY: the sim-facing interface must be clear of gfx (docs/libmh-abi.md sec 0a). LIFT-TABLE S3 hands
// cfg_final_planet_Construct's contiguous graphics tail to the host, and while these bytes are in
// the determinism hash that host entry would be writing hashed state -- which is exactly what made
// cfg_final_planet_FillBankData's `notify: 1` a real bug rather than a label error. The region is
// the unit of ADDRESS bookkeeping, not of adjudication: data reached only by renderers does not
// belong in the verdict merely because it shares a region with sim fields.
//
// EVERYTHING ELSE IN THE RECORD STAYS HASHED, INCLUDING THE STRINGS, and the boundary is the point
// (the emit_soldiers over-mask arm, one field over). `map_name` (+0x10f) is the cheapest early
// desync signal available; `tlo_file` (+0x20e) has a RUNTIME writer -- map_ReadMap @0x004a3361, at
// every map load -- so excluding it would silence a NON-gfx path. `info_txt` and `asteriods` have no
// reader at all and stay in for the same reason: *unread* is not *gfx*.
//
// PERSIST is untouched, so the save blob still carries all 1063 bytes: `local()` degrades to a raw
// write outside VERDICT mode, and the save format is deliberately not part of this change.
inline void emit_planets(const uint8_t *p, uint32_t len, state_sink &s) {
    using planet              = mh::game::mh_cfg_final_struct_Planet;
    constexpr uint32_t STRIDE = sizeof(planet);
    constexpr uint32_t BANK   = offsetof(planet, bank);
    constexpr uint32_t BANK_N = sizeof(planet::bank);
    constexpr uint32_t TLO    = offsetof(planet, tlo_index);
    constexpr uint32_t TLO_N  = STRIDE - TLO; // tlo_index + soldier_sprite_bank_offset, the tail
    static_assert(BANK + BANK_N < TLO, "the two gfx windows must stay disjoint and ordered");
    for (uint32_t o = 0; o + STRIDE <= len; o += STRIDE) {
        s.bytes(&p[o], BANK);                                // header .. info_flc: sim + strings
        s.local(&p[o + BANK], BANK_N);                       // bank[100]: gfx, FillBankData only
        s.bytes(&p[o + BANK + BANK_N], TLO - BANK - BANK_N); // source_mul .. enemy: sim
        s.local(&p[o + TLO], TLO_N);                         // tlo_index + sprite bank offset: gfx
    }
}

inline void emit_tile_objects(const uint8_t *p, uint32_t len, state_sink &s) {
    if (s.mode == sink_mode::PERSIST) { // the seed blob has always written this flat
        s.bytes(p, len);
        return;
    }
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        s.masked((uint16_t)(p[i] | (p[i + 1] << 8)), TILE_FLAGS_KEEP);
        s.bytes(&p[i + 2], 1); // building lo
        s.bytes(&p[i + 4], 1); // unit     lo
        s.bytes(&p[i + 6], 1); // class_owner
        s.bytes(&p[i + 3], 1); // building hi   <- the order is arbitrary and frozen by the golden
        s.bytes(&p[i + 5], 1); // unit     hi
        // p[i + 7] (visibility) deliberately not emitted -- per-frame fog
    }
}

// ---- the TACTICAL tile_objects window (2026-08-25) ----------------------------------------------
//
// The plane is `map::tile_object_data[256][256]`, 8 bytes an element, 512 KB -- and it was the
// single largest item in the tactical per-frame hash: 48.4% of 1.03 MB, and 393,216 sink calls a
// frame on its own, since VERDICT mode emits SIX fields per element. Measured cost of the whole
// oracle was +3,966 us/frame against a game that simulates in 945 (the tactical-probe work 9q).
//
// A MISSION USES ONE SIXTEENTH OF IT. `llm_tact_mission_start` sets width = height = 0x80 and
// `llm_tact_map_reset` (0x0042e713) wipes `tile_objects[col][row]` over 0..0x7f -- the 128x128
// top-left sub-block of the declared [256][256] grid, indexed through the same 256-element row
// stride (Sect. 5, and the mirror `map_LoadPlanetFromDisk` on exit). Nothing tactical writes
// outside that window.
//
// SO NARROWING LOSES NO SIGNAL, and that is worth stating precisely rather than assuming. The bytes
// outside the window are strategic content that (a) no tactical code writes and (b) both arms of an
// A/B comparison inherit identically from the same save. They cannot differ between two arms, so
// hashing them can never turn a verdict red -- it is 384 KB per frame of guaranteed agreement.
//
// WHAT IT DOES COST: `T` and `TS` change VALUE. Sect. 9l's design deliberately kept `T`
// byte-identical to every historical run, and this ends that. It is safe for the verdicts because
// both tactical verdicts are A-vs-B within one invocation -- there is no stored golden to
// invalidate -- but a hash quoted in an older note or report will not reproduce. The user asked for
// this trade explicitly (2026-08-25).
inline constexpr uint32_t TACT_TILE_DIM     = 128u; // llm_tact_mission_start: width = height = 0x80
inline constexpr uint32_t TILE_PLANE_STRIDE = 256u; // the DECLARED grid the sub-block is indexed in
inline constexpr uint32_t TILE_ELEM         = 8u;

// TEMPLATED ON THE SINK, and the point is the call it does NOT make. `state_sink::raw` is virtual,
// so every field emitted through a `state_sink&` costs an indirect call that cannot inline -- and
// this function emits SIX per 8-byte element, 98,304 of them per frame over the 128x128 window. With
// a concrete `hash_sink` (a `final` class) the call resolves statically and the FNV step inlines
// into the loop.
//
// The generic `state_sink&` path still exists and still works: passing one instantiates the same
// body with virtual dispatch, so PERSIST/seed callers are unaffected. This is a dispatch change, not
// a semantic one -- the emitted BYTES are identical, which is what tacttest T12 and the A/B arms
// check.
template <class S>
inline void emit_tile_objects_tactical(const uint8_t *p, uint32_t len, S &s) {
    if (s.mode == sink_mode::PERSIST) { // the seed blob has always written this flat -- keep it so
        sink_bytes(s, p, len);
        return;
    }
    // Same per-element emission and the SAME arbitrary-but-frozen field order as the strategic
    // walker, so the two stay one format; only the extent differs.
    for (uint32_t outer = 0; outer < TACT_TILE_DIM; ++outer) {
        const uint32_t row = outer * TILE_PLANE_STRIDE * TILE_ELEM;
        for (uint32_t inner = 0; inner < TACT_TILE_DIM; ++inner) {
            const uint32_t i = row + inner * TILE_ELEM;
            if (i + TILE_ELEM > len) return; // a short region is a bug elsewhere, not a read here
            // ONE call per element instead of six, and the BYTE SEQUENCE IS UNCHANGED -- that is
            // the whole reason this is safe. The six calls emitted, in order: the masked flags word
            // little-endian, then +2, +4, +6, +3, +5. Packing them into a buffer in that same order
            // and hashing it once feeds the sink exactly the same stream (tacttest T13 asserts this
            // shape explicitly), so no hash value moves.
            //
            // MEASURED TWICE, AND THE ANSWER CHANGED IN BETWEEN. Against the byte-serial hash this
            // was worth NOTHING -- 352 vs 349 frames/s, inside the noise -- because devirtualising
            // had already inlined the six calls into the loop. Against the WIDE hash it is worth
            // ~8% (497 -> 538 frames/s), because that sink carries a partial block between calls,
            // so each call now has real per-call work rather than none. An optimisation's value is
            // a property of the code around it, not of the optimisation; this one was correctly
            // rejected on its first measurement and correctly kept on its second.
            //
            // (The field order is odd -- +6 before +3 -- and is noted in the strategic walker as
            // "arbitrary and frozen by the golden". It is preserved here precisely BECAUSE it is
            // arbitrary: a rational-looking reordering would be a silent hash change.)
            const uint16_t flags     = (uint16_t)(p[i] | (p[i + 1] << 8));
            const uint16_t keep      = (s.mode == sink_mode::PERSIST) ? flags : (flags & TILE_FLAGS_KEEP);
            const uint8_t  packed[7] = {
                (uint8_t)(keep & 0xff), (uint8_t)(keep >> 8),
                p[i + 2], // building lo
                p[i + 4], // unit     lo
                p[i + 6], // class_owner
                p[i + 3], // building hi
                p[i + 5], // unit     hi
            };
            sink_bytes(s, packed, sizeof(packed));
            // p[i + 7] (visibility) deliberately not emitted -- per-frame fog
        }
    }
}

inline void emit_rng_state(const uint8_t *p, uint32_t len, state_sink &s) {
    for (uint32_t i = 0; i * 4 + 4 <= len; ++i) {
        if (i == RNG_SLOT_FX)
            s.local(&p[i * 4], 4);
        else
            s.bytes(&p[i * 4], 4);
    }
}

// ---- the dispatch ----------------------------------------------------------------------------
//
// By HASH_REGIONS[] index. Phase 2 replaces the three-way branch with an owner lookup that falls
// back to the raw block; the signature is what matters now, not the body.
inline void emit_slice(int i, state_sink &s) {
    const hash_region &r = HASH_REGIONS[i];
    // ST4: an OWNED region answers for itself. `owner_serves` also requires the slice to be the
    // WHOLE region -- a sub-window (one player's interior slot in player_data, say) has no meaning
    // in terms of a module's canonical stream, so it falls back to raw rather than being served a
    // stream that does not line up.
    if (owner_serves(r.rid, r.offset, r.len)) {
        owner_of(r.rid)->emit(r.rid, s);
        return;
    }
    // ST2: hash_base(i), not r.base. `r.base` is the STOCK address the manifest recorded; the
    // bytes are wherever the region's owner put them, which is the same thing until something
    // rebases and is the whole difference afterwards.
    const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(hash_base(i)));
    switch (i) {
        case HIDX_UNITS: emit_units(p, r.len, s); return;
        case HIDX_TILE_OBJECTS: emit_tile_objects(p, r.len, s); return;
        case HIDX_RNG_STATE: emit_rng_state(p, r.len, s); return;
        case HIDX_SOLDIERS: emit_soldiers(p, r.len, s); return;
        case HIDX_PLANETS: emit_planets(p, r.len, s); return;
        default: s.bytes(p, r.len); return;
    }
}

// The per-region determinism hash. `mask_ctrl_group=false` restores the unmasked units hash the
// harness ini can still ask for, by running the units slice through a PERSIST-mode traversal --
// which for units is exactly "do not substitute the ctrl_group byte". `mask_soldier_anim` is the
// same A/B for soldiers' anim_change_count, and exists for the same reason: a mask whose unmasked
// arm cannot be run is a mask nobody can show is doing anything. `mask_planets_gfx` is the third,
// added with emit_planets at LIFT-TABLE S2 and MANDATORY for the same reason -- a mask whose
// unmasked arm cannot be run is a mask nobody can audit.
inline uint64_t hash_slice(int i, bool mask_ctrl_group, bool mask_soldier_anim = true,
                           bool mask_planets_gfx = true) {
    const bool unmasked = (i == HIDX_UNITS && !mask_ctrl_group) ||
                          (i == HIDX_SOLDIERS && !mask_soldier_anim) ||
                          (i == HIDX_PLANETS && !mask_planets_gfx);
    hash_sink s(unmasked ? sink_mode::PERSIST : sink_mode::VERDICT);
    emit_slice(i, s);
    return s.finish();
}

// ---- the TACTICAL slice set (TACT-PREP) -------------------------------------------------------
//
// A second, independent slice table over TACT_HASH_REGIONS[], hashed on the llm_tact_frame cadence.
// It is not a mode of the strategic emitter because the two never run in the same frame: mode 6
// dispatches to llm_tact_frame and llm_strat_sim_step is not called at all, which is the mechanical
// reason the shipped hash has always been blind to tactical (the tactical-probe work Sect. 2).
//
// No owner lookup: nothing in mh:: owns a tactical region yet, so every slice is raw. When one is
// migrated this gains the same `owner_serves` branch emit_slice has, for the same reason.
// ---- the SIM-ONLY unit slice (TACT-REC, 2026-08-25) ------------------------------------------
//
// `tact_units_sim` is an ALIAS of `tact_units`: the same 129 records at the same address, walked a
// second time with the render-written animation window marked local(). Both are hashed, and the
// pair is the verdict -- full differs + sim identical means presentation drift, both differ means
// something the simulation owns moved. A single MASKED hash would have deleted the signal instead
// of splitting it, which is the trap the tactical-probe work 9e sprang once already.
//
// WHY THESE FIVE FIELDS AND NOTHING ELSE. `llm_tact_unit_update_anim` (0x0042c547) is reachable
// ONLY through `llm_tact_unit_render` <- `llm_tact_render_view`'s camera-viewport tile scan (9k),
// so what it writes is a function of what the camera shows. Every field below is written from that
// path and has no reader outside it:
//
//   anim_frame_time  +0x2b  8  timestamp of the last frame advance (+ the llm_rand deadline jitter)
//   frame_index      +0x33  1  the animation frame being drawn
//   anim_cycle_time  +0x34  8  timestamp of the cycle start
//   frame_interval   +0x3c  8  seconds per animation frame, restamped per cycle
//   sprite_id        +0x45  2  "current display sprite id (output; drawn by llm_tact_unit_render)"
//
// The first four are contiguous, [0x2b, 0x44); sprite_id is [0x45, 0x47), and the byte between them
// is `progress`, which is KEPT.
//
// WHAT IS DELIBERATELY NOT DROPPED, and this is the load-bearing half of the design. `anim_state`
// (+0x05) gates STAND / kneel / mine-arm, `progress` (+0x44) drives the move offset, and
// `cmd_wait_until_time` (+0x23) is a command deadline -- all three are arguable as simulation, so
// they stay IN and a divergence in any of them turns this hash red. An exclusion list is only
// honest while it contains what nobody can argue for.
inline constexpr uint32_t TACT_ANIM_LOCAL_LO   = 0x2bu; // anim_frame_time .. frame_interval
inline constexpr uint32_t TACT_ANIM_LOCAL_HI   = 0x44u; // exclusive; +0x44 is `progress`, KEPT
inline constexpr uint32_t TACT_SPRITE_LOCAL_LO = 0x45u; // sprite_id
inline constexpr uint32_t TACT_SPRITE_LOCAL_HI = 0x47u; // exclusive
inline constexpr uint32_t TACT_UNIT_STRIDE     = 0x5f4u;

template <class S>
inline void emit_tact_units_sim(const uint8_t *p, uint32_t len, S &s) {
    for (uint32_t o = 0; o + TACT_UNIT_STRIDE <= len; o += TACT_UNIT_STRIDE) {
        const uint8_t *rec = &p[o];
        sink_bytes(s, rec, TACT_ANIM_LOCAL_LO);
        sink_local(s, rec + TACT_ANIM_LOCAL_LO, TACT_ANIM_LOCAL_HI - TACT_ANIM_LOCAL_LO);
        sink_bytes(s, rec + TACT_ANIM_LOCAL_HI, TACT_SPRITE_LOCAL_LO - TACT_ANIM_LOCAL_HI); // progress
        sink_local(s, rec + TACT_SPRITE_LOCAL_LO, TACT_SPRITE_LOCAL_HI - TACT_SPRITE_LOCAL_LO);
        sink_bytes(s, rec + TACT_SPRITE_LOCAL_HI, TACT_UNIT_STRIDE - TACT_SPRITE_LOCAL_HI);
    }
}

template <class S>
inline void tact_emit_slice(int i, S &s) {
    const hash_region &r = TACT_HASH_REGIONS[i];
    const uint8_t     *p =
        reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(tact_hash_base(i)));
    // tile_objects interleaves per-FRAME fog bits with per-step occupancy in tactical exactly as it
    // does in strategic, so it gets the same mask rather than a flat read -- otherwise a camera pan
    // (which is input, not simulation) would read as a divergence. Nothing else in the tactical set
    // mixes the two rates.
    if (r.rid == RID_TILE_OBJECTS) {
        // The TACTICAL window, not the whole plane -- see emit_tile_objects_tactical. The strategic
        // emitter above still walks all 256x256, because strategic code writes all of it.
        emit_tile_objects_tactical(p, r.len, s);
        return;
    }
    // Dispatched by INDEX, not by rid: tact_units_sim shares tact_units' rid, offset and length by
    // construction -- being the same bytes is the point of an alias slice.
    if (i == TIDX_TACT_UNITS_SIM) {
        emit_tact_units_sim(p, r.len, s);
        return;
    }
    sink_bytes(s, p, r.len);
}

inline uint64_t tact_hash_slice(int i) {
    hash_sink s(sink_mode::VERDICT);
    tact_emit_slice(i, s);
    return s.finish();
}

// The two tactical verdicts, from ONE pass over the table.
//
//   `combined` -- every slice whose bytes are not already counted elsewhere, i.e. the whole table
//                 minus the alias. `excluded` carries a DIFFERENT meaning in this table than in the
//                 strategic one: there it marks a region left out of the state verdict, here it
//                 marks a slice already counted under another name. tact_selftest asserts that the
//                 only excluded tactical slice is an alias of a slice that IS counted, so the arena
//                 still cannot go unwatched.
//   `sim`      -- the same set with tact_units replaced by tact_units_sim.
//
// Computed together because the caller needs both every frame and the per-slice hashes are the
// expensive part; `per` is filled for the positional `TR` line and must hold
// TACT_HASH_REGION_COUNT entries.
struct tact_verdict {
    uint64_t combined;
    uint64_t sim;
};

inline uint64_t tact_fold(uint64_t h, uint64_t v) {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&v);
    for (uint32_t i = 0; i < sizeof(v); ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

inline tact_verdict tact_hash_all(uint64_t *per) {
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) per[i] = tact_hash_slice(i);
    tact_verdict v{1469598103934665603ULL, 1469598103934665603ULL};
    for (int i = 0; i < TACT_HASH_REGION_COUNT; ++i) {
        if (TACT_HASH_REGIONS[i].excluded) continue; // the alias -- its bytes are in tact_units
        v.combined = tact_fold(v.combined, per[i]);
        v.sim      = tact_fold(v.sim, (i == TIDX_TACT_UNITS) ? per[TIDX_TACT_UNITS_SIM] : per[i]);
    }
    return v;
}

// The tactical mutation hook -- the negative-test target, and it exists for the same reason
// mutate_target does: a newly-hashed region is worth nothing until it has been watched to FAIL.
inline uint8_t *tact_mutate_target(int i) {
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(tact_hash_base(i)));
}

// The inverse of emit_slice in PERSIST mode. Phase 1: every slice is raw, so a flat read over the
// region is exactly what emit_slice(PERSIST) wrote -- which is what keeps existing seed blobs
// injectable. Phase 2 routes an owned slice to its module's load_state instead.
inline void fill_slice(int i, state_source &src) {
    const hash_region &r = HASH_REGIONS[i];
    if (owner_serves(r.rid, r.offset, r.len)) {
        owner_of(r.rid)->load(r.rid, src);
        return;
    }
    src.bytes(reinterpret_cast<void *>(static_cast<uintptr_t>(hash_base(i))), r.len);
}

// ---- the mutation hook ------------------------------------------------------------------------
//
// region_poke's replacement. A NEWLY-HASHED REGION IS WORTH NOTHING UNTIL IT HAS BEEN WATCHED TO
// FAIL, so this has to survive an ownership move -- which is exactly why it is declared here rather
// than left as `*(uint8_t*)base ^= 1` at the call site. Phase 3 gives an owner its own override;
// phase 1 keeps the historical behaviour (increment byte 0) so the existing negative tests still
// mean what they meant.
// `off` is a byte offset INTO the region (default 0 = the historical behaviour). It exists because
// byte 0 of a region is not always a byte the poke may safely touch: `players`' byte 0 is
// Players[0].race, live state the sim reads, while its name[32] at +0x10 is read by nothing -- and
// D11's own doctrine is that the poke goes into UNCLAIMED state, so that the divergence it proves is
// the ORACLE's coverage and not a second-order behaviour change. Clamped, so a mistyped offset pokes
// the region's last byte rather than the next region's first.
inline uint8_t *mutate_target(int i, uint32_t off = 0) {
    const hash_region &r = HASH_REGIONS[i];
    if (r.len && off >= r.len) off = r.len - 1;
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(hash_base(i))) + off;
}

} // namespace mh::state
