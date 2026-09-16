//
// sim/sim_bldg_placement_preview.cpp -- see sim_bldg_placement_preview.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_placement_check_and_preview_00453a6d.asm,
// tmp/decomp/llm_bldg_calc_placement_corner_from_center_0048d054.asm), not from the Ghidra .c drafts.
//
#include "sim/sim_bldg_placement_preview.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const placement_preview_calls &live_placement_preview_calls() {
    static const placement_preview_calls gc = {
        MH_LIBMH_BIND(llm_fx_anim_seq_cancel),
        MH_LIBMH_BIND(llm_strat_fx_anim_spawn),
    };
    return gc;
}

namespace {
// cfg_enum_E_BUILDING members llm_bldg_placement_check_and_preview's shuttle-only arm gates on
// (0x00453ba6-0x00453bc4: CMP .type,0xd / CMP .type,0x21). File-scoped rather than namespace mh::sim
// scope to avoid colliding with sim_order_enqueue.h's own BUILDING_TYPE_A_SHUTTLE/H_SHUTTLE (same
// values, same enum, independently named there for its own closure -- see that header's own
// BUILDING_TYPE_* block; duplicating the NAME at namespace scope in a second header risks an ODR
// redefinition if a future TU ever includes both).
inline constexpr uint8_t SHUTTLE_TYPE_A = 0x0d;
inline constexpr uint8_t SHUTTLE_TYPE_H = 0x21;
} // namespace

namespace detail {

int32_t bldg_placement_check_and_preview(const sim_view &v, const placement_preview_calls &gc, int32_t origin_x,
                                         int32_t origin_y, int32_t building_index) {
    int32_t denied = 0;

    // Cancel any in-flight preview overlay before drawing a fresh one (0x00453a93-0x00453aa2).
    gc.fx_anim_seq_cancel(v.anim_place_seq_ids[0]); // _G_LLM_ANIM_PLACE_DENIED
    gc.fx_anim_seq_cancel(v.anim_place_seq_ids[1]); // _G_LLM_ANIM_PLACE_ALLOWED

    const cfg_building &cb = v.cfg_buildings[building_index];

    // The 10x10 footprint mask -- row=dx (outer loop, 0x00453aae), col=dy (inner loop, 0x00453ac8);
    // area[dx][dy] address = Building_base + building_index*0x842 + dx*0xa + dy (0x00453adb), which
    // is exactly `cb.area[dx][dy]` for `uint8_t area[10][10]`.
    for (int32_t dx = 0; dx < 10; ++dx) {
        for (int32_t dy = 0; dy < 10; ++dy) {
            if (cb.area[dx][dy] == 0) continue;

            // Re-read the masks and the game clock at every tile, matching the asm's own redundant
            // reads (0x00453af8-0x00453b17 / 0x00453b65-0x00453b94) rather than hoisting them --
            // translator brief rule 16: `const` on sim_view is not a promise of stability.
            const uint32_t tile_x = (uint32_t)(origin_x + dx) & map_width_mask(v);
            const uint32_t tile_y = (uint32_t)(origin_y + dy) & map_height_mask(v);

            int32_t kind;
            if (v.passable[(tile_x << 8) | tile_y] == 0 ||
                tile_at(v, (int32_t)tile_x, (int32_t)tile_y).building != 0) {
                kind   = v.anim_place_seq_ids[0]; // DENIED
                denied = 1;
            } else {
                kind = v.anim_place_seq_ids[1]; // ALLOWED
            }
            gc.fx_anim_spawn(tile_x << 5, tile_y << 5, (uint32_t)kind, *v.game_clock, 0);
        }
    }

    // The separate landing-pad tile, checked/previewed the same way -- A_SHUTTLE/H_SHUTTLE cfg types
    // only (0x00453ba6-0x00453bc4 gates entry; 0x00453bca-0x00453be7 reads the pad offsets).
    if (cb.type == SHUTTLE_TYPE_A || cb.type == SHUTTLE_TYPE_H) {
        const int32_t pad_dx = cb.shuttle_pad_offset_x;
        const int32_t pad_dy = cb.shuttle_pad_offset_y;

        const uint32_t tile_x = (uint32_t)(origin_x + pad_dx) & map_width_mask(v);
        const uint32_t tile_y = (uint32_t)(origin_y + pad_dy) & map_height_mask(v);

        int32_t kind;
        if (v.passable[(tile_x << 8) | tile_y] == 0 ||
            tile_at(v, (int32_t)tile_x, (int32_t)tile_y).building != 0) {
            kind   = v.anim_place_seq_ids[0];
            denied = 1;
        } else {
            kind = v.anim_place_seq_ids[1];
        }
        gc.fx_anim_spawn(tile_x << 5, tile_y << 5, (uint32_t)kind, *v.game_clock, 0);
    }

    return denied;
}

void bldg_calc_placement_corner_from_center(const sim_view &v, uint16_t unit_index, int32_t center_x,
                                            int32_t center_y, uint32_t *out_col, uint32_t *out_row) {
    const cfg_unit     &proto = v.cfg_units[unit_index];
    const cfg_building &cb    = v.cfg_buildings[proto.equivalent];

    // width/height are read at their declared (byte) width, matching the asm's MOVZX-from-byte
    // operand exactly (0x0048d089 / 0x0048d0bf) -- no widening. `(dim - 1)` promotes to `int` (usual
    // arithmetic conversion on a uint8_t operand), so this is a plain signed int subtraction even
    // when width/height == 0.
    //
    // The division-by-2 is C's truncating `/`, which reproduces the asm's SAR/SBB bias-correction
    // idiom (0x0048d090-0x0048d098 for width, 0x0048d0c6-0x0048d0ce for height) value-for-value on
    // negatives too: for x=(dim-1) with sign=(x>>31), the idiom computes ((x - sign) >> 1) [SAR], and
    // that is exactly truncating division by 2 -- e.g. x=-3 -> idiom gives -1, C's -3/2 gives -1;
    // x=-4 -> idiom gives -2, C's -4/2 gives -2. Same reasoning sim_order_enqueue.cpp's fine_to_tile()
    // documents for its own truncating-divide idiom (there divisor 32, here divisor 2).
    const int32_t half_w = (int32_t)(cb.width - 1) / 2;
    const int32_t half_h = (int32_t)(cb.height - 1) / 2;

    // SUB then AND, in that order (0x0048d09d/0x0048d0a7 for col; 0x0048d0d3/0x0048d0dd for row) --
    // i.e. `(center - half) & mask`, not `center - (half & mask)`.
    *out_col = (uint32_t)(center_x - half_w) & map_width_mask(v);
    *out_row = (uint32_t)(center_y - half_h) & map_height_mask(v);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t bldg_placement_check_and_preview(int32_t origin_x, int32_t origin_y, int32_t building_index) {
    const sim_view v = state().read;
    return detail::bldg_placement_check_and_preview(v, live_placement_preview_calls(), origin_x, origin_y,
                                                    building_index);
}

void bldg_calc_placement_corner_from_center(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                            uint32_t *out_col, uint32_t *out_row) {
    const sim_view v = state().read;
    detail::bldg_calc_placement_corner_from_center(v, unit_index, center_x, center_y, out_col, out_row);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
