//
// sim/sim_unit_fine_pos.cpp -- see sim_unit_fine_pos.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_get_coords_0044b141.asm,
// tmp/decomp/llm_strat_unit_calc_fine_axis_pos_0048c83f.asm,
// tmp/decomp/llm_strat_unit_calc_render_fine_y_0048ccb4.asm,
// tmp/decomp/llm_strat_unit_calc_interp_pixel_pos_0048ce21.asm), not from Ghidra's .c drafts: all
// four drafts' overall shape (the type-gated branch below UNIT_TYPE_A_HELI vs at-or-above it, the
// per-axis wrap/no-wrap split) was independently re-walked branch-by-branch against the raw
// CMP/JG/JNZ/IDIV/AND instructions and found to match -- the header's HAZARD notes on get_coords vs
// calc_fine_axis_pos (independent duplication), calc_render_fine_y's two type>=UNIT_TYPE_A_HELI-
// branch asymmetries, and the header banner's CORRECTION note (the exported .c plates' "airborne"/
// "ground" labels for these two branches are backwards) are the load-bearing findings from that walk.
//
#include "sim/sim_unit_fine_pos.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

// ---- llm_strat_unit_get_coords @0x0044b141 -----------------------------------------------------
//
// Computes BOTH axes independently (see the header banner) -- the type<UNIT_TYPE_A_HELI branch
// fetches the facing_step_offset pair ONCE and reuses it for both *out_x/*out_y (matching the .asm,
// which reads the dx table entry at 0x0044b1c3-0x0044b1d3 and the dy table entry at
// 0x0044b1d6-0x0044b1f9 before doing either axis's arithmetic); the type>=UNIT_TYPE_A_HELI branch
// computes each axis from _G_LLM_STRAT_MOVE_MICROSTEPS independently, matching the .asm's own two
// near-identical blocks (0x0044b28b-0x0044b2ef for X, 0x0044b2f1-0x0044b355 for Y).
void get_coords(const sim_view &v, uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    const unit     &u     = unit_of(v, player, unit_index);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    if (static_cast<int32_t>(proto.type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)) {
        // type < UNIT_TYPE_A_HELI (ground/vehicle/walker/soldier bulk): wrap-around interpolated
        // position. iVar3/remaining = 0x1f - move_microstep (0x0044b1a2-0x0044b1ad), shared by both
        // axes below (NOT recomputed per axis -- the .asm computes it exactly once here, unlike
        // calc_fine_axis_pos's own axis-parameterized version, which only ever needs one axis per
        // call anyway).
        const int32_t remaining = 0x1f - u.move_microstep;
        // dx (0x0044b1c3-0x0044b1d3) and dy (0x0044b1d6-0x0044b1f9), both from the SAME
        // facing_target -- see the header's DECLARED NEED on this table.
        const int32_t dx = v.facing_step_offset[u.facing_target].dx;
        const int32_t dy = v.facing_step_offset[u.facing_target].dy;

        const uint32_t sum_x = static_cast<uint32_t>(v.geom->big_width) +
                               static_cast<uint32_t>(dx) * static_cast<uint32_t>(remaining) +
                               static_cast<uint32_t>(u.x) * 0x20u + 0x10u;
        *out_x = static_cast<int32_t>(sum_x) % static_cast<int32_t>(v.geom->big_width);

        const uint32_t sum_y = static_cast<uint32_t>(v.geom->big_height) +
                               static_cast<uint32_t>(dy) * static_cast<uint32_t>(remaining) +
                               static_cast<uint32_t>(u.y) * 0x20u + 0x10u;
        *out_y = static_cast<int32_t>(sum_y) % static_cast<int32_t>(v.geom->big_height);
    } else {
        // type >= UNIT_TYPE_A_HELI (heli/plane/heli_mother): tile position + the per-heading/
        // per-microstep move offset. NO wrap, NO +0x10 -- matching 0x0044b28b-0x0044b355 exactly
        // (two independent MOVE_MICROSTEPS reads, one per axis, each re-deriving the row/proto base
        // the same way the .asm re-derives it).
        const int32_t idx =
            static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING + u.move_microstep;
        *out_x = static_cast<uint32_t>(u.x) * 0x20u +
                 static_cast<uint32_t>(v.move_microsteps[idx].x_off);
        *out_y = static_cast<uint32_t>(u.y) * 0x20u +
                 static_cast<uint32_t>(v.move_microsteps[idx].y_off);
    }
}

// ---- llm_strat_unit_calc_fine_axis_pos @0x0048c83f ---------------------------------------------
//
// Same type gate and same two formulas as get_coords, but axis-parameterized: only the requested
// axis's table entry / MOVE_MICROSTEPS field is read (matching the .asm, which branches on
// axis_is_x BEFORE doing any table/offset read, unlike get_coords which always does both).
// "remaining" (iVar2 = 0x1f - move_microstep) is computed ONLY inside the type<UNIT_TYPE_A_HELI
// branch (0x0048c89e-0x0048c8a9, AFTER the type-gate JG at 0x0048c885) -- the
// type>=UNIT_TYPE_A_HELI branch never touches move_microstep via this subtraction, so it is NOT
// hoisted above the type-gate here either.
int32_t calc_fine_axis_pos(const sim_view &v, uint16_t player, int32_t unit_idx, char axis_is_x) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    int32_t result;
    if (static_cast<int32_t>(proto.type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)) {
        const int32_t remaining = 0x1f - u.move_microstep;
        if (axis_is_x == 1) {
            const int32_t  dx    = v.facing_step_offset[u.facing_target].dx;
            const uint32_t sum_x = static_cast<uint32_t>(v.geom->big_width) +
                                   static_cast<uint32_t>(dx) * static_cast<uint32_t>(remaining) +
                                   static_cast<uint32_t>(u.x) * 0x20u + 0x10u;
            result = static_cast<int32_t>(sum_x) % static_cast<int32_t>(v.geom->big_width);
        } else {
            const int32_t  dy    = v.facing_step_offset[u.facing_target].dy;
            const uint32_t sum_y = static_cast<uint32_t>(v.geom->big_height) +
                                   static_cast<uint32_t>(dy) * static_cast<uint32_t>(remaining) +
                                   static_cast<uint32_t>(u.y) * 0x20u + 0x10u;
            result = static_cast<int32_t>(sum_y) % static_cast<int32_t>(v.geom->big_height);
        }
    } else {
        const int32_t idx =
            static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING + u.move_microstep;
        if (axis_is_x == 1) {
            result = static_cast<int32_t>(static_cast<uint32_t>(u.x) * 0x20u +
                                          static_cast<uint32_t>(v.move_microsteps[idx].x_off));
        } else {
            result = static_cast<int32_t>(static_cast<uint32_t>(u.y) * 0x20u +
                                          static_cast<uint32_t>(v.move_microsteps[idx].y_off));
        }
    }
    return result;
}

// ---- llm_strat_unit_calc_render_fine_y @0x0048ccb4 ---------------------------------------------
//
// The Y-only near-twin of calc_fine_axis_pos's Y arm. The type<UNIT_TYPE_A_HELI branch
// (0x0048ccfe-0x0048cd86) is IDENTICAL in shape to calc_fine_axis_pos's own type<UNIT_TYPE_A_HELI/Y
// branch (wrap via `% big_height`, the same "+0x10"). The type>=UNIT_TYPE_A_HELI branch
// (0x0048cd8b-0x0048ce0c) is NOT the same as calc_fine_axis_pos's type>=UNIT_TYPE_A_HELI/Y
// branch -- see the header's HAZARD (a)/(b): it subtracts `elevation`
// (0x0048cdff, byte offset +0x32 = the unit's `elevation` field) and masks with `general.bh_mask &`
// (0x0048ce05-0x0048ce0a) instead of a modulo. C's operator precedence already gets this right
// (`&` binds looser than `-`, so `bh_mask & (part - elevation)`), and that is also what the .asm
// computes (SUB before AND) -- reproduced explicitly with parens below so the ordering can't be
// misread by a future edit.
uint32_t calc_render_fine_y(const sim_view &v, uint16_t player, int32_t unit_idx) {
    const unit     &u     = unit_of(v, player, unit_idx);
    const cfg_unit &proto = v.cfg_units[u.unit_proto_id];

    uint32_t result;
    if (static_cast<int32_t>(proto.type) < static_cast<int32_t>(UNIT_TYPE_A_HELI)) {
        const int32_t  remaining = 0x1f - u.move_microstep;
        const int32_t  dy        = v.facing_step_offset[u.facing_target].dy;
        const uint32_t sum       = static_cast<uint32_t>(v.geom->big_height) +
                             static_cast<uint32_t>(dy) * static_cast<uint32_t>(remaining) +
                             static_cast<uint32_t>(u.y) * 0x20u + 0x10u;
        result = static_cast<uint32_t>(static_cast<int32_t>(sum) %
                                       static_cast<int32_t>(v.geom->big_height));
    } else {
        const int32_t idx =
            static_cast<int32_t>(u.move_heading) * MICROSTEPS_PER_HEADING + u.move_microstep;
        const uint32_t y_part = static_cast<uint32_t>(v.move_microsteps[idx].y_off) +
                                static_cast<uint32_t>(u.y) * 0x20u;
        // HAZARD (a)/(b): elevation subtraction + bh_mask AND, NOT a modulo -- real, load-bearing
        // asymmetry vs calc_fine_axis_pos's own type>=UNIT_TYPE_A_HELI/Y branch. Do not "fix" for
        // symmetry.
        result = v.geom->bh_mask & (y_part - static_cast<uint32_t>(u.elevation));
    }
    return result;
}

// ---- llm_strat_unit_calc_interp_pixel_pos @0x0048ce21 -------------------------------------------
//
// NO Unit.type gate anywhere in this body (confirmed: neither 0x0048ce21-0x0048ce65 nor either
// branch reads unit_proto_id or Unit[]) -- unconditionally uses the type<UNIT_TYPE_A_HELI-style
// formula for whichever axis is requested. "remaining" (iVar2 = 0x1f - move_microstep,
// 0x0048ce53-0x0048ce5e) is computed ONCE, before the axis branch, and used by whichever axis runs
// -- matching the .asm, which (unlike calc_fine_axis_pos) computes it before the CMP on axis_is_x.
// NO "+0x10" constant on either axis (confirmed absent from both 0x0048ce67-0x0048cec6 and
// 0x0048cecb-0x0048cf2a) -- the one formula difference from get_coords/calc_fine_axis_pos's own
// type<UNIT_TYPE_A_HELI branches.
int32_t calc_interp_pixel_pos(const sim_view &v, uint16_t player, int32_t unit_idx, char axis_is_x) {
    const unit   &u         = unit_of(v, player, unit_idx);
    const int32_t remaining = 0x1f - u.move_microstep;

    int32_t result;
    if (axis_is_x == 1) {
        const int32_t  dx  = v.facing_step_offset[u.facing_target].dx;
        const uint32_t sum = static_cast<uint32_t>(u.x) * 0x20u +
                             static_cast<uint32_t>(dx) * static_cast<uint32_t>(remaining) +
                             static_cast<uint32_t>(v.geom->big_width);
        result = static_cast<int32_t>(sum) % static_cast<int32_t>(v.geom->big_width);
    } else {
        const int32_t  dy  = v.facing_step_offset[u.facing_target].dy;
        const uint32_t sum = static_cast<uint32_t>(u.y) * 0x20u +
                             static_cast<uint32_t>(dy) * static_cast<uint32_t>(remaining) +
                             static_cast<uint32_t>(v.geom->big_height);
        result = static_cast<int32_t>(sum) % static_cast<int32_t>(v.geom->big_height);
    }
    return result;
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

void get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    sim_state st = state();
    detail::get_coords(st.read, player, unit_index, out_x, out_y);
}

int32_t calc_fine_axis_pos(uint16_t player, int32_t unit_idx, char axis_is_x) {
    sim_state st = state();
    return detail::calc_fine_axis_pos(st.read, player, unit_idx, axis_is_x);
}

uint32_t calc_render_fine_y(uint16_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::calc_render_fine_y(st.read, player, unit_idx);
}

int32_t calc_interp_pixel_pos(uint16_t player, int32_t unit_idx, char axis_is_x) {
    sim_state st = state();
    return detail::calc_interp_pixel_pos(st.read, player, unit_idx, axis_is_x);
}


} // namespace mh::sim
