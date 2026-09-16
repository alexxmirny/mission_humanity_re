//
// tact/tact_unit_vision.cpp -- see tact_unit_vision.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_fov_probe_far_cell_and_door_state_0042e220.asm,
// llm_tact_unit_vision_add_0042e385.asm, llm_tact_unit_vision_remove_0042e53f.asm), not from
// Ghidra's .c.
//
#include "tact/tact_unit_vision.h"

#include "addr/mh_calls.gen.h"  // llm_tact_vision_cone_setup / llm_tact_fov_raycast_stencil (Law 4 sibling-TU frontier stubs)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_vision_calls &live_unit_vision_calls() {
    static const unit_vision_calls c = {
        MH_LIBMH_BIND(llm_tact_vision_cone_setup),
        MH_LIBMH_BIND(llm_tact_fov_raycast_stencil),
    };
    return c;
}

namespace detail {

void fov_probe_far_cell_and_door_state(tact_store &own, mh::state::mode_planes &planes,
                                       const unit_vision_calls &c, int32_t unit_idx,
                                       int32_t *out_far_col, uint32_t *out_far_row,
                                       int32_t *out_cell2_col, uint32_t *out_cell2_row,
                                       int32_t *out_door_animating_flag) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042e235-0x0042e2b5: stamp the CALLING unit's own position/facing/vision into the FOV
    // scratch globals, then raycast. (The dead `.type` read at 0x0042e241-0x0042e24f into an
    // immediately-overwritten local is omitted -- it is never read before being clobbered by the
    // following pos_col stamp, so it has no observable effect.)
    own.fov_col()         = u.pos_col;
    own.fov_row()         = u.pos_row;
    own.fov_angle_base()  = (static_cast<int32_t>(u.facing_dir) - 1) * 15;
    own.fov_angle_width() = u.vision_angle;
    own.fov_dist()        = u.vision_dist;
    c.fov_raycast_stencil();

    // @0x0042e2ba-0x0042e31c: unpack the two packed (col<<8)|row cells the raycast just produced.
    // Both loads are MOVZX (unsigned 16-bit): EDX/EAX top 16 bits are zero, so `SAR reg,0x1f`
    // always yields 0, so the CF the following SHL/SBB consume is always 0, so the whole
    // sign-adjustment term is provably 0 for every possible 16-bit input -- the sequence reduces
    // exactly to `hi = raw >> 8`, `lo = raw % 256` (see tact_unit_vision.h and uncertainties[]).
    const uint16_t hibit = static_cast<uint16_t>(own.fov_nearest_hibit_cell());
    *out_far_col         = hibit >> 8;
    *out_far_row         = static_cast<uint32_t>(hibit & 0xff);

    const uint16_t low = static_cast<uint16_t>(own.fov_nearest_low_cell());
    *out_cell2_col     = low >> 8;
    *out_cell2_row     = static_cast<uint32_t>(low & 0xff);

    *out_door_animating_flag = 0;

    // @0x0042e325-0x0042e335: only probe the far cell when BOTH unpacked coords are (signed) > 0.
    if (*out_far_col <= 0 || static_cast<int32_t>(*out_far_row) <= 0) return;

    // @0x0042e337-0x0042e350: the occupant id parked at the far cell (tile_objects[...].building,
    // field +2, NOT bias-adjusted -- far_col/far_row are already absolute tile coordinates).
    const int32_t cell2 =
        planes.tile_object_at(*out_far_col, static_cast<int32_t>(*out_far_row)).building;

    // @0x0042e353-0x0042e37c: re-index _G_LLM_TACT_UNITS BY THAT OCCUPANT ID (IMUL by 0x5f4, the
    // tact_unit record stride) and read .anim_state off of it -- doors are modeled as unit-table
    // entries; anim_state 2 or 3 means "this door is mid-animation". Unchecked, like every other
    // unit_at() call in this closure.
    const uint8_t door_anim = own.unit_at(cell2).anim_state;
    if (door_anim == 2 || door_anim == 3) *out_door_animating_flag = 1;
}

void unit_vision_add(tact_store &own, mh::state::mode_planes &planes, const unit_vision_calls &c,
                     int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042e3a0-0x0042e3bb: skip the whole update iff this is an ENEMY unit (owner==1) AND the
    // see-enemy reveal flag is off. Proceed otherwise (owner!=1, or SEE_ENEMY_FLAG set).
    if (u.owner == 1 && own.see_enemy_flag() == 0) return;

    const int32_t cur_col = u.pos_col;
    const int32_t cur_row = u.pos_row;

    // @0x0042e3e2-0x0042e439: same scratch-stamp + raycast as the probe function above, but reached
    // through llm_tact_vision_cone_setup rather than doing it inline. The four "dead_outptrN" args
    // are addresses of uninitialized caller-frame scratch that callee never reads.
    c.vision_cone_setup(cur_col, cur_row, (static_cast<int32_t>(u.facing_dir) - 1) * 15,
                        u.vision_angle, u.vision_dist, nullptr, nullptr, nullptr, nullptr);

    // @0x0042e43e-0x0042e535: walk the 64x64 stencil -- outer var `i` ([EBP-0x40]), inner var `j`
    // ([EBP-0x3c]); stencil index = j*64+i, tile coord = (cur_col+j-31, cur_row+i-31). See
    // tact_unit_vision.h for the folded -31/-31 bias derivation.
    for (int32_t i = 0; i < 64; ++i) {
        for (int32_t j = 0; j < 64; ++j) {
            // @0x0042e472-0x0042e482: JBE on an unsigned byte compared to 0 is just "== 0".
            if (own.fov_stencil_at(j * 64 + i) == 0) continue;

            const int32_t x    = cur_col + j - 31;
            const int32_t y    = cur_row + i - 31;
            auto         &tile = planes.tile_object_at(x, y);

            // @0x0042e4a1: refcount++.
            ++tile.visibility;
            // @0x0042e4be-0x0042e524: flags[1] is the HIGH byte of the little-endian flags word --
            // AND/OR immediates here are 0x4000 (FOGGED)/0x8000 (EXPLORED) shifted into that byte.
            // Two separate RMWs in the original, same net effect as `(flags[1] & 0xbf) | 0x80`.
            tile.flags[1] &= 0xbf; // clear FOGGED (0x4000)
            tile.flags[1] |= 0x80; // set EXPLORED (0x8000)
        }
    }
}

void unit_vision_remove(tact_store &own, mh::state::mode_planes &planes, const unit_vision_calls &c,
                        int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042e55a-0x0042e575: identical guard to unit_vision_add.
    if (u.owner == 1 && own.see_enemy_flag() == 0) return;

    const int32_t cur_col = u.pos_col;
    const int32_t cur_row = u.pos_row;

    c.vision_cone_setup(cur_col, cur_row, (static_cast<int32_t>(u.facing_dir) - 1) * 15,
                        u.vision_angle, u.vision_dist, nullptr, nullptr, nullptr, nullptr);

    for (int32_t i = 0; i < 64; ++i) {
        for (int32_t j = 0; j < 64; ++j) {
            if (own.fov_stencil_at(j * 64 + i) == 0) continue;

            const int32_t x    = cur_col + j - 31;
            const int32_t y    = cur_row + i - 31;
            auto         &tile = planes.tile_object_at(x, y);

            // @0x0042e65b-0x0042e681: POST-decrement test -- the CMP runs AFTER the DEC, so only
            // when the refcount reaches EXACTLY 0 do both fog bits get set.
            if (--tile.visibility == 0) {
                tile.flags[1] |= 0xc0; // set FOGGED (0x4000) + EXPLORED (0x8000)
            }
        }
    }
}

} // namespace detail

void fov_probe_far_cell_and_door_state(int32_t unit_idx, int32_t *out_far_col,
                                       uint32_t *out_far_row, int32_t *out_cell2_col,
                                       uint32_t *out_cell2_row, int32_t *out_door_animating_flag) {
    tact_state st = state();
    detail::fov_probe_far_cell_and_door_state(st.own, st.own.planes(), live_unit_vision_calls(),
                                              unit_idx, out_far_col, out_far_row, out_cell2_col,
                                              out_cell2_row, out_door_animating_flag);
}

void unit_vision_add(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_vision_add(st.own, st.own.planes(), live_unit_vision_calls(), unit_idx);
}

void unit_vision_remove(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_vision_remove(st.own, st.own.planes(), live_unit_vision_calls(), unit_idx);
}

} // namespace mh::tact
