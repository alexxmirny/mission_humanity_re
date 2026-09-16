//
// orders/issue/issue_unit_move.cpp -- the single-unit move-order wrappers (RI-ORDERS / O4-0, O4A-C
// sweep, batch B unit `unit_move`).
//
// DERIVED FROM THE .asm, not the .c -- the .c drafts for this family carry a plausible PLATE but the
// register-level detail below came from the listings at tmp/decomp_orders_issue/. Addresses in the
// comments are EN /eng/mh.exe.
//
// THE SHARED SHAPE, worth stating once. All five wrappers stage the target tile into scratch slots
// 0/1 (order.args[0]/[1] -> unit goal x/y, +0x86/+0x87), then emit ONE order whose op_code/arg pair
// selects the move behaviour, with the owner byte built as `player | KIND_UNIT` (0x80) from the FULL
// player dword (`MOV EAX,player; OR AL,0x80; MOVZX EDX,AX`) -- same "owner uses the full dword, the
// index multiply narrows to 16 bits first" idiom issue_bldg_orders.cpp documents for buildings, here
// for units[player][idx] (stride 0xe9, player-half multiplier 0x5b04 == 100*0xe9).
//
// THE DOMAIN-WIDE "MP ECHO" PATTERN, also worth stating once: llm_strat_unit_order_move,
// _move_default, _move_confirmed_with_bump and (on its long-hop branch) _move_relative all follow
// their primary dispatch with the SAME session-mode branch --
//   session_mode == 3 (MP lockstep): scratch_reset(); dispatch(unit_id, RAW player, 0xfb, 0xfb)
//   else:                            issue_calls.unit_notify_status(player, unit_idx, 0)
// and the RAW-player detail is real: the 0xfb/0xfb echo's owner is NOT owner_of(player,KIND_UNIT) --
// there is no `OR AL,0x80` on this second call at any of the four sites, only a plain
// `MOVZX EDX,word [player]`. Transcribed as `(uint16_t)player`, not `owner_of(player, KIND_UNIT)`.
//
#include "orders/issue/issue_unit_move.h"


namespace mh::orders::issue {

namespace {

// units[player][index], with the SAME narrowing idiom issue_bldg_orders.cpp's bldg_at() documents:
// the player half narrowed to 16 bits before the multiply, the index half not.
const mh::game::mh_map_object_unit &unit_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.units[(int32_t)(uint16_t)player * v.caps.units + index];
}

// unit_order_move_relative's OWN x/y read does NOT narrow `player` first -- see the function body
// and `uncertainties` for why this is transcribed as a separate helper rather than reusing unit_at().
const mh::game::mh_map_object_unit &unit_at_unnarrowed(const issue_view &v, uint32_t player,
                                                       int32_t index) {
    return v.units[player * v.caps.units + index];
}

// The owner byte the container receives: the kind nibble OR'd into the low byte of the FULL `player`
// dword -- same idiom as issue_bldg_orders.cpp's owner_of(), duplicated per the sweep's per-TU-copy
// convention.
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_unit_order_move @0x00469fe6 -------------------------------------------------------
// op_code = cfg_units[proto].move_op_code (+0xeb), arg = cfg_units[proto].move_op_arg (+0xec) --
// both read off the SAME units[player][idx].unit_proto_id lookup. The original reloads the units[]
// address twice (once per field read, 0x0046a030-0x0046a058 and 0x0046a05b-0x0046a083) -- a codegen
// artifact with no observable effect (same call as bldg_order_repair_cycle_start's re-derived
// building row in issue_bldg_orders.cpp), so it is computed once here.
// Scratch: [0]=target_x, [1]=target_y, [0xc]=group_id (order.args[12] -> unit +0xc6, move_group_id).
void detail::unit_order_move(const issue_view &v, const order_sink &s, const issue_calls &c,
                             uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                             int32_t group_id) {
    const mh::game::mh_cfg_final_struct_Unit &proto =
        v.cfg_units[unit_at(v, player, unit_idx).unit_proto_id];

    s.scratch_reset();
    s.scratch_set_field(0, target_x);
    s.scratch_set_field(1, target_y);
    s.scratch_set_field(0xc, group_id);

    const uint16_t unit_id = (uint16_t)unit_idx;
    s.dispatch(unit_id, owner_of(player, KIND_UNIT), proto.move_op_code, proto.move_op_arg);

    if (*v.session_mode == 3) {
        s.scratch_reset();
        s.dispatch(unit_id, (uint16_t)player, 0xfb, 0xfb); // RAW player -- see file banner
    } else {
        c.unit_notify_status((uint16_t)player, unit_idx, 0);
    }
}

// ---- llm_strat_unit_order_move_default @0x0046a402 ------------------------------------------------
// Same shape as unit_order_move, but op_code is the CONSTANT 0x10 (not cfg_units[proto].move_op_code)
// -- only move_op_arg is read from the unit's proto row. Scratch/MP-echo pattern identical.
void detail::unit_order_move_default(const issue_view &v, const order_sink &s, const issue_calls &c,
                                     uint32_t player, int32_t unit_idx, int32_t target_x,
                                     int32_t target_y, int32_t group_id) {
    const mh::game::mh_cfg_final_struct_Unit &proto =
        v.cfg_units[unit_at(v, player, unit_idx).unit_proto_id];

    s.scratch_reset();
    s.scratch_set_field(0, target_x);
    s.scratch_set_field(1, target_y);
    s.scratch_set_field(0xc, group_id);

    const uint16_t unit_id = (uint16_t)unit_idx;
    s.dispatch(unit_id, owner_of(player, KIND_UNIT), 0x10, proto.move_op_arg);

    if (*v.session_mode == 3) {
        s.scratch_reset();
        s.dispatch(unit_id, (uint16_t)player, 0xfb, 0xfb); // RAW player -- see file banner
    } else {
        c.unit_notify_status((uint16_t)player, unit_idx, 0);
    }
}

// ---- llm_strat_unit_order_move_confirmed_with_bump @0x0046accf ------------------------------------
// op_code CONSTANT 0x18, arg = cfg_units[proto].move_op_arg. Same scratch/dispatch/MP-echo shape as
// the two above (but only TWO scratch slots -- there is no group_id parameter/slot-0xc write here).
//
// AFTER that, unconditionally (regardless of which MP-echo branch fired), a bump/collision check for
// the LOCAL VIEWER ONLY (`(uint16_t)player == *v.player_side`, a 16-bit `CMP AX,[PlayerSide]`):
//   corner = issue_calls.bldg_calc_placement_corner_from_center(proto_id, target_x, target_y)
//   hit    = issue_calls.bldg_placement_check_and_preview(corner.col, corner.row, proto.equivalent)
//   hit != 0 -> issue_calls.snd_play(0xb3, 100)     -- THE DOMAIN'S ONE EFFECTFUL CALL, see notes.
// `equivalent` (cfg_final_struct_Unit +0x1a3) is the unit's "bumps into this building type" pairing
// (docs/structs.md), read here for the placement/preview check, not a duplicate unit lookup.
void detail::unit_order_move_confirmed_with_bump(const issue_view &v, const order_sink &s,
                                                 const issue_calls &c, uint32_t player,
                                                 int32_t unit_idx, int32_t target_x,
                                                 int32_t target_y) {
    const uint16_t                            proto_id = unit_at(v, player, unit_idx).unit_proto_id;
    const mh::game::mh_cfg_final_struct_Unit &proto    = v.cfg_units[proto_id];

    s.scratch_reset();
    s.scratch_set_field(0, target_x);
    s.scratch_set_field(1, target_y);

    const uint16_t unit_id = (uint16_t)unit_idx;
    s.dispatch(unit_id, owner_of(player, KIND_UNIT), 0x18, proto.move_op_arg);

    if (*v.session_mode == 3) {
        s.scratch_reset();
        s.dispatch(unit_id, (uint16_t)player, 0xfb, 0xfb); // RAW player -- see file banner
    } else {
        c.unit_notify_status((uint16_t)player, unit_idx, 0);
    }

    if ((uint16_t)player == *v.player_side) {
        // uint32_t: bldg_calc_placement_corner_from_center's committed out-params are `uint *`
        // (TACT1-P C6, 2026-09-04). The preview call below takes them by value as int32_t.
        uint32_t corner_col = 0, corner_row = 0;
        c.bldg_calc_placement_corner_from_center(proto_id, target_x, target_y, &corner_col,
                                                 &corner_row);
        const int32_t hit =
            c.bldg_placement_check_and_preview(corner_col, corner_row, proto.equivalent);
        if (hit != 0) {
            c.snd_play(0xb3, 100); // volume 100 (0x64)
        }
    }
}

// ---- llm_strat_unit_order_move_relative @0x0046c87a -----------------------------------------------
// Computes the toroidal-wrapped tile delta from (target_x,target_y) to the unit's OWN (x,y) via
// issue_calls.tile_delta_wrapped -- the wrap masks (issue_view.geom) live INSIDE that callee, this
// wrapper never reads v.geom directly. abs() both components, takes their MAX (Chebyshev distance),
// and picks:
//   distance < 2 (0 or 1 tile away): dispatch(op_code=1, arg=0x33), RETURN -- no MP echo, no
//                                    notify_status at all on this branch.
//   distance >= 2:                  dispatch(op_code=0x33, arg=0xa), then the usual MP-echo /
//                                    notify_status branch -- EXCEPT notify_status is passed the RAW,
//                                    un-truncated `player` here (see uncertainties).
//
// NOTE the x/y READ ITSELF does not narrow `player` to 16 bits before the units[] index multiply
// (`IMUL EDX,dword [player],0x5b04` operates on the full 32-bit stack slot at 0x0046c8a3/0x0046c8ba)
// -- unlike unit_at()'s documented convention and unlike every OTHER site in this unit. Transcribed
// via the dedicated unit_at_unnarrowed() helper rather than folded into unit_at(); see uncertainties.
void detail::unit_order_move_relative(const issue_view &v, const order_sink &s, const issue_calls &c,
                                      uint32_t player, int32_t unit_idx, int32_t target_x,
                                      int32_t target_y) {
    const mh::game::mh_map_object_unit &u = unit_at_unnarrowed(v, player, unit_idx);

    int32_t dx = 0, dy = 0;
    c.tile_delta_wrapped(target_x, target_y, u.x, u.y, &dx, &dy);
    dx = (dx < 0) ? -dx : dx;
    dy = (dy < 0) ? -dy : dy;

    s.scratch_reset();
    s.scratch_set_field(0, target_x);
    s.scratch_set_field(1, target_y);

    const int32_t  chebyshev = (dx <= dy) ? dy : dx;
    const uint16_t unit_id   = (uint16_t)unit_idx;

    if (chebyshev < 2) {
        s.dispatch(unit_id, owner_of(player, KIND_UNIT), 1, 0x33);
        return;
    }

    s.dispatch(unit_id, owner_of(player, KIND_UNIT), 0x33, 0xa);
    if (*v.session_mode == 3) {
        s.scratch_reset();
        s.dispatch(unit_id, (uint16_t)player, 0xfb, 0xfb); // RAW player -- see file banner
    } else {
        c.unit_notify_status(player, unit_idx, 0); // RAW player, NOT truncated -- see uncertainties
    }
}

// ---- llm_strat_unit_order_scatter_from_spawn @0x0046a626 ------------------------------------------
// op_code CONSTANT 0x10, arg = cfg_units[proto].move_op_arg -- the same pair as unit_order_default,
// but through the IMMEDIATE lane (llm_strat_order_enqueue @0x00466094), and unconditionally
// notify_status afterward (there is no session-mode / MP-echo branch in this function at all).
// `player`'s committed storage is AX:2 (already 16-bit on entry, per the .asm header) -- kept as
// uint16_t rather than widened to uint32_t like the other four wrappers in this unit.
void detail::unit_order_scatter_from_spawn(const issue_view &v, const order_sink &s,
                                           const issue_calls &c, uint16_t player, int32_t unit_idx,
                                           int32_t target_x, int32_t target_y) {
    const mh::game::mh_cfg_final_struct_Unit &proto =
        v.cfg_units[unit_at(v, player, unit_idx).unit_proto_id];

    s.scratch_reset();
    s.scratch_set_field(0, target_x);
    s.scratch_set_field(1, target_y);

    const uint16_t unit_id = (uint16_t)unit_idx;
    s.enqueue(unit_id, owner_of(player, KIND_UNIT), 0x10, proto.move_op_arg);
    c.unit_notify_status(player, unit_idx, 0);
}

// ---- the public forms -----------------------------------------------------------------------------

void unit_order_move(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                     int32_t group_id) {
    detail::unit_order_move(live_view(), live_sink(), live_calls(), player, unit_idx, target_x,
                            target_y, group_id);
}
void unit_order_move_default(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                             int32_t group_id) {
    detail::unit_order_move_default(live_view(), live_sink(), live_calls(), player, unit_idx,
                                    target_x, target_y, group_id);
}
void unit_order_move_confirmed_with_bump(uint32_t player, int32_t unit_idx, int32_t target_x,
                                         int32_t target_y) {
    detail::unit_order_move_confirmed_with_bump(live_view(), live_sink(), live_calls(), player,
                                                unit_idx, target_x, target_y);
}
void unit_order_move_relative(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y) {
    detail::unit_order_move_relative(live_view(), live_sink(), live_calls(), player, unit_idx,
                                     target_x, target_y);
}
void unit_order_scatter_from_spawn(uint16_t player, int32_t unit_idx, int32_t target_x,
                                   int32_t target_y) {
    detail::unit_order_scatter_from_spawn(live_view(), live_sink(), live_calls(), player, unit_idx,
                                          target_x, target_y);
}


} // namespace mh::orders::issue
