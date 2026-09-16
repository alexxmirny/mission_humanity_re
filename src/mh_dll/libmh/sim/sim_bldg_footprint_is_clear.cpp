//
// sim/sim_bldg_footprint_is_clear.cpp -- see sim_bldg_footprint_is_clear.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_footprint_is_clear_0049396b.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_footprint_is_clear.h"

#include "ai/ai_state.h"   // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/rng_trace.h" // C-prime level 4: the bytes AT THE READ INSTANT

namespace mh::sim {

namespace detail {

int32_t bldg_footprint_is_clear(const sim_view &v, int32_t x, int32_t y, int32_t building_type,
                                uint32_t viewer) {
    // 0x00493993-0x00493a85 outer / 0x004939ad-0x00493a80 inner: row-major 10x10 walk over
    // cfg_buildings[building_type].area[row][col] -- same loop shape as the sibling
    // llm_map_bldg_footprint_set_passable (sim_bldg_footprint_set_passable.cpp). `row` pairs with
    // `x`, `col` pairs with `y` (re-derived from the raw CMP/JL/JMP block structure).
    const cfg_building &b = v.cfg_buildings[building_type];
    // C-prime level 4. The cross-arm region dumps are sampled at the STEP BOUNDARY, but `passable`
    // and `tile_objects` are mutated DURING a step by every entity that ticks before this one -- so
    // "identical at the sample" does not prove "identical at the read". These folds are the bytes
    // this call actually saw, at the instant it saw them, which is the only comparison that settles
    // it. Two separate folds so a difference names the PLANE, not just the fact of one.
    uint32_t fold_pass = 2166136261u, fold_tile = 2166136261u, cells = 0u;
    auto     mixb    = [](uint32_t h, uint32_t byte) { return (h ^ (byte & 0xffu)) * 16777619u; };
    int32_t  fail_at = -1;
    for (int32_t row = 0; row < 10; ++row) {
        for (int32_t col = 0; col < 10; ++col) {
            // 0x004939c0-0x004939d7: `area[row][col] != 0` gates the three checks below; an
            // unoccupied cell is skipped entirely (JZ straight past every check, to the next cell).
            if (b.area[row][col] == 0) continue;

            // 0x004939dd-0x004939fa / 0x00493a05-0x00493a22 / 0x00493a45-0x00493a65: three
            // independent re-derivations of the SAME tile index, `general` reached through the
            // existing map_width_mask()/map_height_mask() helpers (see header banner for the address
            // cross-check).
            int32_t tile_x = (x + row) & static_cast<int32_t>(map_width_mask(v));
            int32_t tile_y = (y + col) & static_cast<int32_t>(map_height_mask(v));

            // Check 1 (0x004939f4-0x00493a03): passable == 0 -> FAIL. Read through the const view
            // (this function writes nothing) with the same (tile_x<<8)|tile_y packing
            // sim_store::passable_at() implements.
            const uint32_t pv = (uint32_t)v.passable[(tile_x << 8) | tile_y];
            fold_pass         = mixb(fold_pass, pv);
            ++cells;
            if (pv == 0) {
                fail_at = row * 10 + col;
                mh::sim::rng_trace_add_note(4u, (uint32_t)x << 16 | ((uint32_t)y & 0xffffu),
                                            (uint32_t)building_type, viewer,
                                            (uint32_t)0u << 24 | ((uint32_t)fail_at & 0xffffu) << 8 | cells,
                                            fold_pass, fold_tile);
                return 0;
            }

            // Check 2 (0x00493a27-0x00493a2e): class_owner != 0 -> FAIL. Together with check 1 this
            // is the draft's `(passable==0) || (class_owner!=0)` OR-clause -- both raw branches
            // converge on the same return-0 landing pad (0x00493a30) in the original.
            const tile_object &t = tile_at(v, tile_x, tile_y);
            fold_tile            = mixb(mixb(fold_tile, (uint32_t)t.class_owner), (uint32_t)t.visibility);
            if (t.class_owner != 0) {
                fail_at = row * 10 + col;
                mh::sim::rng_trace_add_note(4u, (uint32_t)x << 16 | ((uint32_t)y & 0xffffu),
                                            (uint32_t)building_type, viewer,
                                            (uint32_t)1u << 24 | ((uint32_t)fail_at & 0xffffu) << 8 | cells,
                                            fold_pass, fold_tile);
                return 0;
            }

            // Check 3 (0x00493a39-0x00493a6e): ONLY evaluated when the LOCAL viewer is asking
            // (PlayerSide == viewer, zero-extend-then-compare per the project's established idiom --
            // e.g. sim_order_dispatch_bldg.cpp / sim_unit_ctrl_group.cpp). visibility == 0 -> FAIL,
            // but only under this gate; a non-local viewer never fails a cell on fog.
            if (viewer == static_cast<uint32_t>(static_cast<uint16_t>(*v.player_side)) &&
                t.visibility == 0) {
                fail_at = row * 10 + col;
                mh::sim::rng_trace_add_note(4u, (uint32_t)x << 16 | ((uint32_t)y & 0xffffu),
                                            (uint32_t)building_type, viewer,
                                            (uint32_t)2u << 24 | ((uint32_t)fail_at & 0xffffu) << 8 | cells,
                                            fold_pass, fold_tile);
                return 0;
            }
        }
    }
    // 0x00493a85: outer loop exhausted (all 100 cells checked), none failed -- clear.
    mh::sim::rng_trace_add_note(4u, (uint32_t)x << 16 | ((uint32_t)y & 0xffffu),
                                (uint32_t)building_type, viewer,
                                (uint32_t)3u << 24 | ((uint32_t)fail_at & 0xffffu) << 8 | cells,
                                fold_pass, fold_tile);
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_footprint_is_clear(int32_t x, int32_t y, int32_t building_type, uint32_t viewer) {
    const sim_view v = state().read;
    return detail::bldg_footprint_is_clear(v, x, y, building_type, viewer);
}


} // namespace mh::sim
