#include "orders/issue/issue_unit_storage.h"

namespace mh::orders::issue {

namespace {

// buildings[player][index] -- same arithmetic as bldg_at() in issue_bldg_orders.cpp. Duplicated per
// the sweep's per-TU-copy convention (an anonymous-namespace helper cannot cross translation units).
const mh::game::mh_map_object_building &bldg_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.buildings[(int32_t)(uint16_t)player * v.caps.buildings + index];
}

// units[player][index] -- player half narrowed to 16 bits (`MOVZX EAX,word` / `IMUL EAX,EAX,0x5b04`),
// index half not (`IMUL EDX,index,0xe9`), exactly as issue_state.h documents for the unit side.
const mh::game::mh_map_object_unit &unit_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.units[(int32_t)(uint16_t)player * v.caps.units + index];
}

// unit_storage[player][index]. Stride derived from BOTH functions' own multiplier immediates:
// `IMUL EDX,player16,0x17d4` (per-player stride) and `IMUL EAX,index,0xf4` (per-slot stride,
// sizeof(mh_map_object_unit_storage) == 0xf4 confirmed against the struct's own static_asserts).
// 0x17d4 / 0xf4 == 25 exactly -- a domain-local constant, not shared with any other unit in this
// sweep, so it stays here rather than in issue_state.h (same reasoning as BUILDINGS_PER_PLAYER
// living there because multiple units share it).
inline constexpr int32_t                    UNIT_STORAGE_PER_PLAYER = mh::state::STOCK_ROSTER_CAPS.storage;
const mh::game::mh_map_object_unit_storage &storage_at(const issue_view &v, uint32_t player,
                                                       int32_t index) {
    return v.unit_storage[(int32_t)(uint16_t)player * v.caps.storage + index];
}

// The owner byte the container receives: the kind nibble OR'd into the LOW BYTE of the FULL `player`
// dword (`MOV EAX,player; OR AL,kind; MOVZX EDX,AX`) -- same idiom as issue_bldg_orders.cpp's
// owner_of(), duplicated per the sweep's per-TU-copy convention.
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_unit_order_exit_storage @0x0046a6b9 -----------------------------------------------
//
// Full re-trace of the raw assembly (register/stack slots, not the .c's renumbered locals):
//   default_x = unit_storage[player][storage_index].exit_tile_x   (0x0046a6da-0046a6f3)
//   default_y = unit_storage[player][storage_index].exit_tile_y   (0x0046a6f6-0046a70f)
//   b = buildings[player][unit_storage[player][storage_index].b_index]           (0x0046a712-0046a73b)
//   GUARD (short-circuit, exactly as the original evaluates it -- storage_type_accepts_unit is only
//   CALLED when online_state passed): b.online_state == 0  ||
//     issue_calls.storage_type_accepts_unit(b.building_id, units[player][unit_index].unit_proto_id) == 0
//   (0x0046a73b JZ / 0x0046a78f CALL / 0x0046a794 TEST+JNZ) -- both land on the SAME fallback target
//   (LAB_0046a798 -> JMP 0x0046a8d4) when they fail, so it is one guard for one branch, not two.
//
//   FALLBACK (guard failed) @0x0046a8d4: read the CURRENT order_seq_id_by_player[player] (pushed as
//   the 5th arg BEFORE the call, so unit_order_move gets the pre-increment value), call
//   detail::unit_order_move(player, unit_index, default_x, default_y, seq), THEN
//   order_seq_id_by_player[player]++ and, if that wrapped to 0, ++ again (0x0046a8f2-0046a90f) --
//   never emit seq id 0.
//
//   ACCEPTED (guard passed) @0x0046a79d: scratch_reset(); scratch_set_field(2, storage_index)
//   ALWAYS. Then, keyed on cfg_units[unit_proto_id].move_op_arg == 0xa (0x0046a7cf-0046a7d6):
//     == 0xa: scratch_set_field(0, default_x); scratch_set_field(1, default_y);
//             dispatch(unit_index, player|KIND_UNIT, 0x24, 0x38);            [golden site 1]
//     != 0xa: GUARD units[..].elevation < cfg_units[..].elevation -> RETURN, no dispatch, no
//             session-mode branch at all (0x0046a848 JL 0x0046a90f -- the function's RET, not the
//             join point at 0x0046a89d). Otherwise issue_calls.storage_get_approach_tile(player,
//             unit_index, &x, &y, storage_index) OVERWRITES (x, y), then
//             scratch_set_field(0,x); scratch_set_field(1,y);
//             dispatch(unit_index, player|KIND_UNIT, 0x29, 0xb);             [golden site 2]
//   Either dispatch falls into the join point @0x0046a89d: session_mode == 3 (MP lockstep) ->
//   scratch_reset(); dispatch(unit_index, player&0xffff, 0xfb, 0xfb)         [golden site 3]
//   else -> issue_calls.unit_notify_status(player&0xffff, unit_index, 0).
//
// `param_4`/`param_5` are the original's ECX/Stack[0x4] inputs: both are OVERWRITTEN with
// exit_tile_x/exit_tile_y before either is ever read (confirmed: no read of either slot precedes its
// first write in the listing), so they are genuinely dead inputs, not a translation gap. Carried in
// the signature per the domain's "all of the original's parameters, always" rule.
void detail::unit_order_exit_storage(const issue_view &v, const order_sink &s, const issue_calls &c,
                                     uint32_t player, uint32_t unit_index, int32_t storage_index,
                                     int32_t param_4, int32_t param_5) {
    (void)param_4; // dead input -- overwritten with exit_tile_x before ever read; see the note above
    (void)param_5; // dead input -- overwritten with exit_tile_y before ever read

    const mh::game::mh_map_object_unit_storage &st        = storage_at(v, player, storage_index);
    const int32_t                               default_x = st.exit_tile_x;
    const int32_t                               default_y = st.exit_tile_y;

    const mh::game::mh_map_object_building &b = bldg_at(v, player, st.b_index);
    const mh::game::mh_map_object_unit     &u = unit_at(v, player, (int32_t)unit_index);

    const bool accepted = b.online_state != 0 &&
                          c.storage_type_accepts_unit(b.building_id, u.unit_proto_id) != 0;

    if (!accepted) {
        uint8_t &seq = v.order_seq_id_by_player[player & 0xffff];
        detail::unit_order_move(v, s, c, player & 0xffff, (int32_t)unit_index, default_x, default_y,
                                (int32_t)seq);
        ++seq;
        if (seq == 0) ++seq;
        return;
    }

    s.scratch_reset();
    s.scratch_set_field(2, storage_index);

    const mh::game::mh_cfg_final_struct_Unit &cfg = v.cfg_units[u.unit_proto_id];
    if (cfg.move_op_arg == 0xa) {
        s.scratch_set_field(0, default_x);
        s.scratch_set_field(1, default_y);
        s.dispatch((uint16_t)unit_index, owner_of(player, KIND_UNIT), 0x24, 0x38);
    } else {
        if (u.elevation < cfg.elevation) return; // no dispatch, no session-mode branch -- full return

        // uint32_t: storage_get_approach_tile's committed out-params are `uint *` (TACT1-P C6,
        // 2026-09-04). Both are read back below as plain dwords.
        uint32_t approach_x = (uint32_t)default_x;
        uint32_t approach_y = (uint32_t)default_y;
        c.storage_get_approach_tile((uint16_t)player, (uint16_t)unit_index, &approach_x, &approach_y,
                                    (uint32_t)storage_index);
        s.scratch_set_field(0, approach_x);
        s.scratch_set_field(1, approach_y);
        s.dispatch((uint16_t)unit_index, owner_of(player, KIND_UNIT), 0x29, 0xb);
    }

    if (*v.session_mode == 3) { // MP lockstep regime
        s.scratch_reset();
        s.dispatch((uint16_t)unit_index, player & 0xffff, 0xfb, 0xfb);
    } else {
        c.unit_notify_status(player & 0xffff, (int32_t)unit_index, 0);
    }
}

// ---- llm_strat_unit_order_auto_launch_from_storage @0x0046cd8b ------------------------------------
//
// `units[player][unit_index].state` gates two disjoint branches (0x0046cdbf CMP ..,0x1f / JNZ, then
// on that path's failure 0x0046cefb CMP ..,0x22 / JNZ -- a third value falls through to a bare RET,
// i.e. a no-op, matching the header's "any other state: no-op").
//
//   state == 0x1f: home_storage_slot = units[..].home_storage_slot (byte); GUARD
//     buildings[player][unit_storage[player][home_storage_slot].b_index].built_flags != 3 -> no-op
//     (0x0046ce13 JNZ 0x0046cee6, which JMPs straight to the function's end). Otherwise
//     scratch_reset(); then, keyed on x == 0xffffffff (0x0046ce25 CMP ..,-1 / JNZ):
//       ==: scratch_set_field(0, (unit_storage[..].exit_tile_x + 2) & geom.width_mask);
//           scratch_set_field(1, (unit_storage[..].exit_tile_y + 2) & geom.height_mask);
//       !=: scratch_set_field(0, x & geom.width_mask); scratch_set_field(1, y & geom.height_mask);
//     then unconditionally dispatch(unit_index, player|KIND_UNIT,
//       cfg_units[unit_proto_id].default_op_code, 0x20).                          [golden site 1]
//   state == 0x22: scratch_reset(); dispatch(unit_index, player|KIND_UNIT, 0x1f, 0x23).
//                                                                                  [golden site 2]
//
// `state` carries NO Ghidra enum (see the header) -- 0x1f/0x22 are transcribed as literals, not the
// .c draft's fabricated `PARKED`/`EXIT_WAIT` names (verified absent from mh_structs.gen.h and every
// generated enum table; declared as a needed enum below).
void detail::unit_order_auto_launch_from_storage(const issue_view &v, const order_sink &s,
                                                 const issue_calls &, uint32_t          player,
                                                 int32_t unit_index, uint32_t x, uint32_t y) {
    const mh::game::mh_map_object_unit &u = unit_at(v, player, unit_index);

    if (u.state == 0x1f) {
        const mh::game::mh_map_object_unit_storage &st = storage_at(v, player, u.home_storage_slot);
        if (bldg_at(v, player, st.b_index).built_flags != 3) return;

        s.scratch_reset();
        if (x == 0xffffffffu) {
            s.scratch_set_field(0, (int32_t)((uint32_t)(st.exit_tile_x + 2) & v.geom->width_mask));
            s.scratch_set_field(1, (int32_t)((uint32_t)(st.exit_tile_y + 2) & v.geom->height_mask));
        } else {
            s.scratch_set_field(0, (int32_t)(x & v.geom->width_mask));
            s.scratch_set_field(1, (int32_t)(y & v.geom->height_mask));
        }

        const mh::game::mh_cfg_final_struct_Unit &cfg = v.cfg_units[u.unit_proto_id];
        s.dispatch((uint16_t)unit_index, owner_of(player, KIND_UNIT), cfg.default_op_code, 0x20);
    } else if (u.state == 0x22) {
        s.scratch_reset();
        s.dispatch((uint16_t)unit_index, owner_of(player, KIND_UNIT), 0x1f, 0x23);
    }
}

// ---- the public forms -----------------------------------------------------------------------------

void unit_order_exit_storage(uint32_t player, uint32_t unit_index, int32_t storage_index,
                             int32_t param_4, int32_t param_5) {
    detail::unit_order_exit_storage(live_view(), live_sink(), live_calls(), player, unit_index,
                                    storage_index, param_4, param_5);
}
void unit_order_auto_launch_from_storage(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y) {
    detail::unit_order_auto_launch_from_storage(live_view(), live_sink(), live_calls(), player,
                                                unit_index, x, y);
}

} // namespace mh::orders::issue
