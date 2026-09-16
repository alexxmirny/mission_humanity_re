//
// orders/issue/issue_bldg_depart.cpp -- the building "depart" order family (RI-ORDERS / O4A-C).
//
// DERIVED FROM THE .asm, not the .c (tmp/decomp_orders_issue's drafts carry a stale
// "unaff_EBX...__watcall register-storage artifact (unresolved)" note on all six depart wrappers --
// it is resolved: EBX is simply this function's own third parameter, storage=EBX:4, exactly as
// documented on the .asm header and consistent with the rest of this domain's __mh_watcall_ebx_volatile
// convention). Addresses in the comments are EN /eng/mh.exe.
//
// THE SIX DEPART/DEPART_ENQUEUE WRAPPERS SHARE ONE SHAPE, worth stating once:
//     ECX = order_code   EBX = order_code (arg)   EAX = player; OR AL,0x40; MOVZX EDX,AX
//     EAX(unit_id) = MOVZX word [bldg_idx]
// i.e. the call is dispatch/enqueue(unit_id = bldg_idx, player = player | KIND_BLDG, op_code =
// order_code, arg = order_code) -- param0 and order_code are the SAME constant at all six sites (a
// property of this family, not a domain rule; see _CONTEXT.md). Each also writes ONE scratch slot,
// index 6, with its own third parameter (the destination planet index) BEFORE the dispatch/enqueue
// call. PLAYER IS NARROWED TO 16 BITS for the unit_id/index side (MOVZX word) but the OWNER byte is
// built from the FULL dword (`MOV EAX,dword ptr [..]` then `OR AL,0x40`) -- the same two-widths-of-
// one-parameter shape issue_bldg_orders.cpp's bldg_at()/owner_of() document; preserved here the same
// way, via owner_of().
//
#include "orders/issue/issue_bldg_depart.h"


namespace mh::orders::issue {

namespace {

// buildings[player][index], with the original's own arithmetic: the player half narrowed to 16
// bits, the index half not. Same helper as issue_bldg_orders.cpp's bldg_at(), duplicated here rather
// than shared across translation units (each unit is self-contained; see the unit spec).
mh::game::mh_map_object_building &bldg_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.buildings[(int32_t)(uint16_t)player * v.caps.buildings + index];
}

// The owner byte the container receives: the kind nibble OR'd into the low byte of the FULL `player`
// dword (not the narrowed one) -- same as issue_bldg_orders.cpp's owner_of().
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_bldg_order_shuttle_depart @0x0046e840 -- order 0xd2/0xd2 --------------------------
void detail::bldg_order_shuttle_depart(const issue_view &, const order_sink &s, const issue_calls &,
                                       uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xd2, 0xd2);
}

// ---- llm_strat_bldg_order_shuttle_depart_enqueue @0x0046e88f -- IMMEDIATE lane, order 0xd2/0xd2 ---
void detail::bldg_order_shuttle_depart_enqueue(const issue_view &, const order_sink &s,
                                               const issue_calls &, uint32_t         player,
                                               uint16_t bldg_idx, int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.enqueue(bldg_idx, owner_of(player, KIND_BLDG), 0xd2, 0xd2);
}

// ---- llm_strat_bldg_order_port_depart @0x0046ed4c -- order 0xd3/0xd3 -----------------------------
// Unconditional at THIS function's own level: the "not the currently-viewed planet" guard the .c
// draft's plate attributes to this function actually belongs to its caller,
// bldg_order_depart_dispatch_by_type (CMP EAX,[G_PLANET_INDEX] / JZ at 0x0046efc5, guarding ONLY the
// port arm) -- this wrapper's own .asm is straight-line, no compare at all.
void detail::bldg_order_port_depart(const issue_view &, const order_sink &s, const issue_calls &,
                                    uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xd3, 0xd3);
}

// ---- llm_strat_bldg_order_port_depart_enqueue @0x0046ed9b -- IMMEDIATE lane, order 0xd3/0xd3 ------
void detail::bldg_order_port_depart_enqueue(const issue_view &, const order_sink &s,
                                            const issue_calls &, uint32_t player, uint16_t bldg_idx,
                                            int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.enqueue(bldg_idx, owner_of(player, KIND_BLDG), 0xd3, 0xd3);
}

// ---- llm_strat_bldg_order_mother_depart @0x0046edea -- order 0xd4/0xd4 ---------------------------
void detail::bldg_order_mother_depart(const issue_view &, const order_sink &s, const issue_calls &,
                                      uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xd4, 0xd4);
}

// ---- llm_strat_bldg_order_mother_depart_enqueue @0x0046ee39 -- IMMEDIATE lane, order 0xd4/0xd4 ----
void detail::bldg_order_mother_depart_enqueue(const issue_view &, const order_sink &s,
                                              const issue_calls &, uint32_t         player,
                                              uint16_t bldg_idx, int32_t dest_planet_idx) {
    s.scratch_set_field(6, dest_planet_idx);
    s.enqueue(bldg_idx, owner_of(player, KIND_BLDG), 0xd4, 0xd4);
}

// ---- llm_strat_bldg_order_depart_dispatch_by_type @0x0046ee88 ------------------------------------
// A TYPE SWITCH over the three sibling wrappers above, keyed on
// cfg_buildings[buildings[player][bldg_idx].building_id].type (re-fetched fresh at every one of the
// 6 comparison sites in the original -- 6 identical address computations, the same codegen artifact
// issue_bldg_orders.cpp's repair_cycle_start documents for its 3 reads; no call happens between them,
// so nothing can write the building record in between, and it is read once here):
//
//   type == 0x21 || type == 0x0d  -> shuttle_depart   (0x0046eeba..0x0046eef7, JZ/fallthrough to
//                                     LAB_0046eef9)
//   type == 0x1a || type == 0x06  -> mother_depart    (0x0046ef20..0x0046ef5d, JZ/fallthrough to
//                                     LAB_0046ef5f)
//   type == 0x20 || type == 0x0c  -> port_depart, BUT ONLY IF dest_planet_idx != G_PLANET_INDEX
//                                     (0x0046ef83..0x0046efcb: JZ/fallthrough to LAB_0046efc2, which
//                                     then does `CMP EAX,[G_PLANET_INDEX]; JZ <end>` before the call
//                                     at 0x0046efd7) -- THIS is the port "not the current planet"
//                                     guard; it lives here, not in port_depart itself.
//   anything else                 -> falls through to LAB_0046efdc with NO call at all: no order is
//                                     emitted (the default/no-match arm the .asm's own fallthrough
//                                     implements; there is no explicit `default:` label to fall to).
//
// `player` is narrowed to 16 bits for the bldg_at() lookup, matching every comparison site
// (`MOVZX EAX,word ptr [EBP-0x10]`); the FULL `player` is still what gets forwarded to the sibling
// wrappers, which do their own OR AL,0x40 on it (see owner_of() above) -- this function does not
// build an owner byte itself, it only routes.
void detail::bldg_order_depart_dispatch_by_type(const issue_view &v, const order_sink &s,
                                                const issue_calls &c, uint32_t player,
                                                int32_t bldg_idx, int32_t dest_planet_idx) {
    const mh::game::mh_map_object_building       &b          = bldg_at(v, player, bldg_idx);
    const mh::game::mh_cfg_final_struct_Building &cfg        = v.cfg_buildings[b.building_id];
    const uint16_t                                bldg_idx16 = (uint16_t)bldg_idx;

    if (cfg.type == 0x21 || cfg.type == 0x0d) {
        detail::bldg_order_shuttle_depart(v, s, c, player, bldg_idx16, dest_planet_idx);
        return;
    }
    if (cfg.type == 0x1a || cfg.type == 0x06) {
        detail::bldg_order_mother_depart(v, s, c, player, bldg_idx16, dest_planet_idx);
        return;
    }
    if (cfg.type == 0x20 || cfg.type == 0x0c) {
        if (dest_planet_idx == *v.planet_index) return; // JZ 0x0046efcb -- same planet, no order
        detail::bldg_order_port_depart(v, s, c, player, bldg_idx16, dest_planet_idx);
        return;
    }
    // no matching type: falls through to the end with no call (0x0046efdc), matching the original.
}

// ---- llm_strat_bldg_order_depart_confirm_dispatch @0x00415b41 ------------------------------------
// Drains whichever of the two staged UI slots is nonzero into the dispatcher above, then clears the
// slot it used; ui_planet_sel_action_target is cleared UNCONDITIONALLY at the very end
// (LAB_00415bb1), whether slot A ran, slot B ran, or NEITHER did (both asm paths -- JZ from the A
// test at 0x00415b60 and JZ from the B test at 0x00415b8d -- reach it). Register load order at each
// call site is EBX(planet target) then EDX(slot value) then EAX(PlayerSide); reproduced here as
// three named reads in that order for documentation, though none of the three globals is written
// before its own dispatch call returns, so the read order has no observable effect. Slot A is tested
// FIRST and, per the .c draft's plate, has no producer in this build (every writer of it stores 0) --
// transcribed as written regardless, since a dead arm is still the original's control flow.
void detail::bldg_order_depart_confirm_dispatch(const issue_view &v, const order_sink &s,
                                                const issue_calls &c) {
    if (*v.ui_depart_pending_bldg_a != 0) {
        const int32_t  planet = *v.ui_planet_sel_action_target; // EBX, loaded first
        const int32_t  bldg   = *v.ui_depart_pending_bldg_a;    // EDX, loaded second
        const uint32_t player = *v.player_side;                 // EAX, loaded third
        detail::bldg_order_depart_dispatch_by_type(v, s, c, player, bldg, planet);
        *v.ui_depart_pending_bldg_a = 0; // 0x00415b7a
    } else if (*v.ui_depart_pending_bldg_b != 0) {
        const int32_t  planet = *v.ui_planet_sel_action_target; // EBX, loaded first
        const int32_t  bldg   = *v.ui_depart_pending_bldg_b;    // EDX, loaded second
        const uint32_t player = *v.player_side;                 // EAX, loaded third
        detail::bldg_order_depart_dispatch_by_type(v, s, c, player, bldg, planet);
        *v.ui_depart_pending_bldg_b = 0; // 0x00415ba7
    }
    *v.ui_planet_sel_action_target = 0; // 0x00415bb1 -- unconditional, both/neither branch taken
}

// ---- the public forms ---------------------------------------------------------------------------

void bldg_order_shuttle_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_shuttle_depart(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                      dest_planet_idx);
}
void bldg_order_shuttle_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_shuttle_depart_enqueue(live_view(), live_sink(), live_calls(), player,
                                              bldg_idx, dest_planet_idx);
}
void bldg_order_port_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_port_depart(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                   dest_planet_idx);
}
void bldg_order_port_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_port_depart_enqueue(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                           dest_planet_idx);
}
void bldg_order_mother_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_mother_depart(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                     dest_planet_idx);
}
void bldg_order_mother_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_mother_depart_enqueue(live_view(), live_sink(), live_calls(), player,
                                             bldg_idx, dest_planet_idx);
}
void bldg_order_depart_dispatch_by_type(uint32_t player, int32_t bldg_idx, int32_t dest_planet_idx) {
    detail::bldg_order_depart_dispatch_by_type(live_view(), live_sink(), live_calls(), player,
                                               bldg_idx, dest_planet_idx);
}
void bldg_order_depart_confirm_dispatch() {
    detail::bldg_order_depart_confirm_dispatch(live_view(), live_sink(), live_calls());
}


} // namespace mh::orders::issue
