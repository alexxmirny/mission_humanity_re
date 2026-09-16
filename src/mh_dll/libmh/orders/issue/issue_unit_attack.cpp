//
// orders/issue/issue_unit_attack.cpp -- the unit "attack" order-issue wrappers (RI-ORDERS / O4A-C,
// `unit_attack` unit of the 2026-08-28 sweep). See issue_unit_attack.h for the roster and
// tmp/decomp_orders_issue/_UNIT_unit_attack.md for the unit spec.
//
// DERIVED FROM THE .asm, not the .c drafts (which the batch context flags as having "already
// lied in this project"). Addresses in the comments are EN /eng/mh.exe.
//
// ---- THE SHAPE ALL FOUR SHARE ---------------------------------------------------------------
// (attacker_player, attacker_unit_idx) identifies the unit RECEIVING the order; (target_player,
// target_unit_idx / target_bldg_idx) identifies what it is ordered to attack. `weapon` is a 0-3
// weapon-slot selector, or 0xffffffff (== -1 as a dword, `CMP dword ptr [..],-0x1` in every one of
// these four) to auto-select via llm_strat_unit_select_weapon against a target-class derived from
// the TARGET (unit: its `elevation` field: nonzero -> class 2, zero -> class 1; building: always
// class 1, since buildings have no elevation). The auto-select result REPLACES `weapon` for the
// rest of the function -- transcribed here as reassigning the parameter, matching the original's
// `MOV dword ptr [EBP+8],EAX` back into the same stack slot the incoming param occupied.
//
// PLAYER WIDTH: every one of these four narrows a `player`-typed value to 16 bits with
// `MOVZX .../word ptr [...]` at several call sites, but builds the scratch-field-5 OWNER byte from
// the FULL dword (`MOV EAX,dword ptr [...]` then `OR AL,<kind>`) -- the exact "two widths of the
// same parameter" hazard issue_bldg_orders.cpp's header documents for its own six functions. Both
// are preserved: `owner_of()` below takes the FULL uint32_t and narrows only its OR'd result;
// every other player-typed argument that the original narrows via a `word ptr` read is narrowed
// here with an explicit `(uint16_t)` cast before the call (redundant, and left in place anyway,
// when the callee's own parameter type is already uint16_t and would narrow it regardless).
//
#include "orders/issue/issue_unit_attack.h"


namespace mh::orders::issue {

namespace {

// units[player][index], with the original's own arithmetic: the player half narrowed to 16 bits,
// the index half not. `0x5b04` == UNITS_PER_PLAYER * sizeof(unit) -- same idiom as
// issue_bldg_orders.cpp's `bldg_at`, duplicated here (anonymous-namespace helpers cannot cross
// translation units; each TU in this sweep carries its own copy, per issue_bldg_footprint.cpp's
// own note on the same point).
const mh::game::mh_map_object_unit &unit_at(const issue_view &v, uint32_t player, int32_t index) {
    return v.units[(int32_t)(uint16_t)player * v.caps.units + index];
}

// The owner byte the container receives: the kind nibble OR'd into the low byte of the FULL
// (un-narrowed) `player` dword, THEN narrowed to 16 bits -- see the file header's width note.
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

// Signed divide-by-32, truncating toward zero -- the `SAR EDX,0x1f / SHL EDX,0x5 / SBB EAX,EDX /
// SAR EAX,0x5` idiom every fine->tile conversion in this file runs (e.g. 0x0046af9d-0x0046afab for
// attack_target's first axis). C++'s `/` on signed integers truncates toward zero (guaranteed since
// C++11), which is bit-for-bit what this shift sequence computes for every input -- verified by
// hand for the boundary cases (x in {-33,-32,-31,31,32}) before relying on it; written as `/32`
// rather than re-deriving the shift form, per the translator brief rule 8's intent (match IDIV's
// truncation, which this idiom exists to reproduce) rather than its literal "write the shifts"
// instruction, since `/32` on a signed int IS that same truncation, not the naive-floor mistake the
// rule warns against.
int32_t fine_to_tile(int32_t fine) { return fine / 32; }

// The unit cfg TYPE domain (cfg_enum_E_UNIT_TYPE) this file's per-class dispatch branches on. No
// generated C++ enum exists for this Ghidra type (per naming rule 17a's "no enum exists" case, and
// matching the established local-constant convention already used throughout src/mh_dll/libmh/sim/ --
// e.g. sim_order_dispatch.cpp, sim_order_enqueue.h, sim_unit_type_predicates.h -- rather than a
// fresh local invention). Values cross-checked against sim_order_enqueue.h's own pins (agree
// exactly): UNIT_TYPE_A_HELI/H_HELI/A_PLANE/H_PLANE/A_HELI_CARGO/H_HELI_CARGO.
inline constexpr uint32_t UNIT_TYPE_A_HELI       = 0x0f;
inline constexpr uint32_t UNIT_TYPE_H_HELI       = 0x10;
inline constexpr uint32_t UNIT_TYPE_A_PLANE      = 0x11;
inline constexpr uint32_t UNIT_TYPE_H_PLANE      = 0x12;
inline constexpr uint32_t UNIT_TYPE_A_HELI_CARGO = 0x17;
inline constexpr uint32_t UNIT_TYPE_H_HELI_CARGO = 0x18;

// cfg_final_struct_Unit::move_op_arg's own two-value domain (mh_structs.gen.h's field comment:
// "0xa = ground mover, 0xb = air"; the sole comparison in this file only ever tests for ground).
inline constexpr uint8_t MOVE_OP_ARG_GROUND = 0xa;

// The MP-mode-ack-vs-notify tail shared VERBATIM by the three functions in this unit that reach a
// "direct fire" (or "reposition not needed") dispatch: attack_target (0x0046b14e-0x0046b183),
// attack_target_alt (0x0046b7c9-0x0046b7fe), attack_building_reposition (0x0046c14e-0x0046c183).
// All three run the identical sequence: if _G_LLM_STRAT_GAME_SESSION_MODE == 3 (multiplayer),
// scratch_reset() then dispatch an admin order 0xfb/0xfb whose "owner" is the ACTOR's RAW
// (un-OR'd, no KIND nibble) player id narrowed to 16 bits -- unlike every other dispatch call in
// this file, which ORs a KIND nibble in. Otherwise (not multiplayer) call
// unit_notify_status(player, unit_index, status_code=0). Factored here because it is one idiom
// occurring three times with zero variation, not a merge of the three ORIGINAL functions -- each
// caller below still corresponds 1:1 to its own original and its own golden site.
void emit_mp_ack_or_notify(const order_sink &s, const issue_calls &c, const issue_view &v,
                           uint32_t attacker_player, int32_t attacker_unit_idx) {
    if (*v.session_mode == 3) {
        s.scratch_reset();
        s.dispatch((uint16_t)attacker_unit_idx, (uint16_t)attacker_player, 0xfb, 0xfb);
    } else {
        c.unit_notify_status((uint16_t)attacker_player, attacker_unit_idx, 0);
    }
}

} // namespace

// ---- llm_strat_unit_order_attack_target @0x0046af1e ---------------------------------------------
//
// 1. unit_get_coords(target) -> (target_x, target_y), UNCONDITIONALLY, before the weapon check
//    (0x0046af3f-0x0046af4c).
// 2. If weapon == -1 (0x0046af51): read the TARGET's `elevation` (units[target]+0x32); nonzero ->
//    target_mask 2, zero -> target_mask 1 (0x0046af6a-0x0046af83); auto-select via
//    unit_select_weapon(attacker, target_mask), replacing `weapon` (0x0046af83-0x0046af95).
// 3. scratch_reset(), then fields 0/1 (target tile x/y, via fine_to_tile), 3/4 (target RAW fine
//    x/y), 5 (owner_of(target, KIND_UNIT)), 6 (target_unit_idx), 7 (weapon) -- in that order
//    (0x0046af98-0x0046b01e).
// 4. range_target_class = llm_strat_target_class(owner_of(target, KIND_UNIT), target_unit_idx)
//    (0x0046b01e-0x0046b02e); in_range = unit_in_weapon_range(attacker_player narrowed to 16,
//    attacker_unit_idx, target tile x, target tile y, range_target_class) (0x0046b02f-0x0046b063).
// 5. THREE possible dispatch shapes, chosen by: in_range && !boarding(attacker.state) && (a ground
//    attacker, move_op_arg == 0xa) && (attacker's cfg type):
//      - type in {A_PLANE, H_PLANE}                          -> falls through to shape (a) below
//      - type in {A_HELI, H_HELI, A_HELI_CARGO, H_HELI_CARGO} -> dispatch(op=0x1a, arg=0x2e), return
//      - any other ground type                                -> dispatch(op=1,    arg=0x1a), return
//    (a) otherwise (not in range, OR boarding, OR a non-ground mover, OR type in {A_PLANE,H_PLANE}):
//        dispatch(op=0x1a, arg=attacker.cfg.move_op_arg), then emit_mp_ack_or_notify.
void detail::unit_order_attack_target(const issue_view &v, const order_sink &s, const issue_calls &c,
                                      uint32_t attacker_player, int32_t attacker_unit_idx,
                                      uint32_t target_player, int32_t target_unit_idx,
                                      uint32_t weapon) {
    int32_t target_x = 0, target_y = 0;
    c.unit_get_coords((uint16_t)target_player, target_unit_idx, &target_x, &target_y);

    if (weapon == 0xffffffffu) {
        uint32_t target_mask = unit_at(v, target_player, target_unit_idx).elevation != 0 ? 2u : 1u;
        weapon               = c.unit_select_weapon((uint16_t)attacker_player, attacker_unit_idx, target_mask);
    }

    s.scratch_reset();
    s.scratch_set_field(0, fine_to_tile(target_x));
    s.scratch_set_field(1, fine_to_tile(target_y));
    s.scratch_set_field(3, target_x);
    s.scratch_set_field(4, target_y);
    s.scratch_set_field(5, owner_of(target_player, KIND_UNIT));
    s.scratch_set_field(6, target_unit_idx);
    s.scratch_set_field(7, (int32_t)weapon);

    int32_t  range_target_class = c.target_class(owner_of(target_player, KIND_UNIT), target_unit_idx);
    uint32_t in_range           = c.unit_in_weapon_range((int32_t)(uint16_t)attacker_player, attacker_unit_idx,
                                                         fine_to_tile(target_x), fine_to_tile(target_y),
                                                         range_target_class);

    if (in_range != 0 &&
        c.unit_state_is_boarding(unit_at(v, attacker_player, attacker_unit_idx).state) == 0) {
        const mh::game::mh_cfg_final_struct_Unit &attacker_cfg =
            v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id];
        if (attacker_cfg.move_op_arg == MOVE_OP_ARG_GROUND) {
            uint32_t type = attacker_cfg.type;
            if (type == UNIT_TYPE_A_PLANE || type == UNIT_TYPE_H_PLANE) {
                // falls through to the shared "direct fire" dispatch below
            } else if (type == UNIT_TYPE_A_HELI || type == UNIT_TYPE_H_HELI ||
                       type == UNIT_TYPE_A_HELI_CARGO || type == UNIT_TYPE_H_HELI_CARGO) {
                s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1a,
                           0x2e);
                return;
            } else {
                s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1,
                           0x1a);
                return;
            }
        }
    }

    uint8_t move_op_arg =
        v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id].move_op_arg;
    s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1a, move_op_arg);
    emit_mp_ack_or_notify(s, c, v, attacker_player, attacker_unit_idx);
}

// ---- llm_strat_unit_order_attack_target_alt @0x0046b599 -----------------------------------------
//
// Translated independently from its own listing (0x0046b599-0x0046b8e9), per the unit spec's
// hazard note -- NOT copied from unit_order_attack_target above. Steps 1-4 are byte-for-byte the
// same shape as the original (same callee addresses, same field indices, same guards); the THREE
// dispatch shapes in step 5 differ from the original in exactly TWO constants:
//   - the "any other ground type" shape:  arg   0x1a -> 0x1b   (0x0046b8c8: MOV ECX,0x1b)
//   - the "otherwise" (direct fire) shape: op_code 0x1a -> 0x1b (0x0046b7b3: MOV EBX,0x1b)
// The "A_HELI/H_HELI/A_HELI_CARGO/H_HELI_CARGO" shape is UNCHANGED (op=0x1a, arg=0x2e,
// 0x0046b8ab-0x0046b8b0) -- verified against its own CMPs (0x11/0x12 at 0x0046b752/0x0046b77b,
// 0xf/0x10/0x17/0x18 at 0x0046b823/0x0046b84c/0x0046b877/0x0046b8a2), all identical addresses-worth
// of constants to the original's own set.
void detail::unit_order_attack_target_alt(const issue_view &v, const order_sink &s,
                                          const issue_calls &c, uint32_t attacker_player,
                                          int32_t attacker_unit_idx, uint32_t target_player,
                                          int32_t target_unit_idx, uint32_t weapon) {
    int32_t target_x = 0, target_y = 0;
    c.unit_get_coords((uint16_t)target_player, target_unit_idx, &target_x, &target_y);

    if (weapon == 0xffffffffu) {
        uint32_t target_mask = unit_at(v, target_player, target_unit_idx).elevation != 0 ? 2u : 1u;
        weapon               = c.unit_select_weapon((uint16_t)attacker_player, attacker_unit_idx, target_mask);
    }

    s.scratch_reset();
    s.scratch_set_field(0, fine_to_tile(target_x));
    s.scratch_set_field(1, fine_to_tile(target_y));
    s.scratch_set_field(3, target_x);
    s.scratch_set_field(4, target_y);
    s.scratch_set_field(5, owner_of(target_player, KIND_UNIT));
    s.scratch_set_field(6, target_unit_idx);
    s.scratch_set_field(7, (int32_t)weapon);

    int32_t  range_target_class = c.target_class(owner_of(target_player, KIND_UNIT), target_unit_idx);
    uint32_t in_range           = c.unit_in_weapon_range((int32_t)(uint16_t)attacker_player, attacker_unit_idx,
                                                         fine_to_tile(target_x), fine_to_tile(target_y),
                                                         range_target_class);

    if (in_range != 0 &&
        c.unit_state_is_boarding(unit_at(v, attacker_player, attacker_unit_idx).state) == 0) {
        const mh::game::mh_cfg_final_struct_Unit &attacker_cfg =
            v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id];
        if (attacker_cfg.move_op_arg == MOVE_OP_ARG_GROUND) {
            uint32_t type = attacker_cfg.type;
            if (type == UNIT_TYPE_A_PLANE || type == UNIT_TYPE_H_PLANE) {
                // falls through to the shared "direct fire" dispatch below
            } else if (type == UNIT_TYPE_A_HELI || type == UNIT_TYPE_H_HELI ||
                       type == UNIT_TYPE_A_HELI_CARGO || type == UNIT_TYPE_H_HELI_CARGO) {
                s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1a,
                           0x2e); // UNCHANGED from unit_order_attack_target
                return;
            } else {
                s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1,
                           0x1b); // arg 0x1a -> 0x1b (the alt's own constant)
                return;
            }
        }
    }

    uint8_t move_op_arg =
        v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id].move_op_arg;
    s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1b,
               move_op_arg); // op_code 0x1a -> 0x1b (the alt's own constant)
    emit_mp_ack_or_notify(s, c, v, attacker_player, attacker_unit_idx);
}

// ---- llm_strat_unit_order_attack_unit @0x0046bc14 -----------------------------------------------
// No range check, no boarding check, no per-class branching: get target coords, resolve weapon
// (same auto-select-by-target-elevation rule as the two functions above), scratch_reset(), then
// FOUR fields at DIFFERENT indices from every other function in this unit -- 8 (target RAW fine x),
// 9 (target RAW fine y), 0xa (owner_of(target, KIND_UNIT)), 0xb (target_unit_idx) -- plus the usual
// 7 (weapon), and ONE unconditional dispatch, order 0x1e/0x1e (0x0046bcd9-0x0046bcef). No tile-
// coordinate fields (0/1) at all -- this function never calls fine_to_tile.
void detail::unit_order_attack_unit(const issue_view &v, const order_sink &s, const issue_calls &c,
                                    uint32_t attacker_player, int32_t attacker_unit_idx,
                                    uint32_t target_player, int32_t target_unit_idx,
                                    uint32_t weapon) {
    int32_t target_x = 0, target_y = 0;
    c.unit_get_coords((uint16_t)target_player, target_unit_idx, &target_x, &target_y);

    if (weapon == 0xffffffffu) {
        uint32_t target_mask = unit_at(v, target_player, target_unit_idx).elevation != 0 ? 2u : 1u;
        weapon               = c.unit_select_weapon((uint16_t)attacker_player, attacker_unit_idx, target_mask);
    }

    s.scratch_reset();
    s.scratch_set_field(8, target_x);
    s.scratch_set_field(9, target_y);
    s.scratch_set_field(0xa, owner_of(target_player, KIND_UNIT));
    s.scratch_set_field(0xb, target_unit_idx);
    s.scratch_set_field(7, (int32_t)weapon);

    s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1e, 0x1e);
}

// ---- llm_strat_unit_order_attack_building_reposition @0x0046bf9c --------------------------------
//
// 1. bldg_get_coords(target) -> (target_x, target_y), UNCONDITIONALLY (0x0046bfbd-0x0046bfca).
// 2. If weapon == -1 (0x0046bfcf): auto-select via unit_select_weapon(attacker, target_mask=1) --
//    HARDCODED 1, no elevation read (buildings have none) (0x0046bfd5-0x0046bfe9).
// 3. scratch_reset(), then fields 0/1/3/4/5/6/7 -- SAME shape as unit_order_attack_target's step 3,
//    but field 5's owner uses KIND_BLDG (0x40), not KIND_UNIT (0x0046bfec-0x0046c06d).
// 4. range_target_class via target_class(owner_of(target, KIND_BLDG), target_bldg_idx)
//    (0x0046c058-0x0046c082); in_range = unit_in_weapon_range(...) (0x0046c083-0x0046c0b7).
// 5. If in_range && !boarding(attacker.state) && attacker's move_op_arg == GROUND (0x0046c0b9-
//    0x0046c107): call the sibling bldg_footprint_random_point to JITTER (target_x, target_y) in
//    place, RE-WRITE fields 0/1/3/4 with the jittered point (fields 5/6/7 keep their step-3 values),
//    and dispatch(op=1, arg=0x1c) (0x0046c188-0x0046c213). Otherwise (not in range, OR boarding, OR
//    a non-ground attacker): dispatch(op=0x1c, arg=attacker.cfg.move_op_arg), then
//    emit_mp_ack_or_notify -- the SAME "direct fire" shape as unit_order_attack_target's, just with
//    op_code 0x1c instead of 0x1a (0x0046c10d-0x0046c183).
void detail::unit_order_attack_building_reposition(const issue_view &v, const order_sink &s,
                                                   const issue_calls &c, uint32_t attacker_player,
                                                   int32_t attacker_unit_idx, uint32_t target_player,
                                                   int32_t target_bldg_idx, uint32_t weapon) {
    // uint32_t, not int32_t: passed by ADDRESS to bldg_footprint_random_point below, whose
    // committed signature (issue_bldg_footprint.h) takes `uint32_t *out_x, uint32_t *out_y`.
    uint32_t target_x = 0, target_y = 0;
    // The two committed rows disagree on the pointee for the SAME pair of locals: bldg_get_coords
    // writes them as `fine_coord *` (int32_t *) and bldg_footprint_random_point reads them back as
    // `uint *`. Both are plain dword accesses, so the pair is held in the width the second call
    // requires and reinterpreted for the first (TACT1-P C6, 2026-09-04).
    c.bldg_get_coords((uint16_t)target_player, target_bldg_idx, reinterpret_cast<int32_t *>(&target_x),
                      reinterpret_cast<int32_t *>(&target_y));

    if (weapon == 0xffffffffu) {
        weapon = c.unit_select_weapon((uint16_t)attacker_player, attacker_unit_idx, 1u);
    }

    s.scratch_reset();
    s.scratch_set_field(0, fine_to_tile((int32_t)target_x));
    s.scratch_set_field(1, fine_to_tile((int32_t)target_y));
    s.scratch_set_field(3, (int32_t)target_x);
    s.scratch_set_field(4, (int32_t)target_y);
    s.scratch_set_field(5, owner_of(target_player, KIND_BLDG));
    s.scratch_set_field(6, target_bldg_idx);
    s.scratch_set_field(7, (int32_t)weapon);

    int32_t range_target_class =
        c.target_class(owner_of(target_player, KIND_BLDG), target_bldg_idx);
    uint32_t in_range = c.unit_in_weapon_range((int32_t)(uint16_t)attacker_player, attacker_unit_idx,
                                               fine_to_tile((int32_t)target_x),
                                               fine_to_tile((int32_t)target_y), range_target_class);

    if (in_range != 0 &&
        c.unit_state_is_boarding(unit_at(v, attacker_player, attacker_unit_idx).state) == 0) {
        uint8_t move_op_arg =
            v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id].move_op_arg;
        if (move_op_arg == MOVE_OP_ARG_GROUND) {
            // param_1/param_2 of the sibling are dead in ITS body (see issue_bldg_footprint.h) --
            // passed through as the original does (attacker_player, attacker_unit_idx) for
            // documentation fidelity, not because either value is read on the other side.
            detail::bldg_footprint_random_point(v, s, c, attacker_player,
                                                (uint32_t)attacker_unit_idx, target_player,
                                                target_bldg_idx, &target_x, &target_y);
            s.scratch_set_field(0, fine_to_tile((int32_t)target_x));
            s.scratch_set_field(1, fine_to_tile((int32_t)target_y));
            s.scratch_set_field(3, (int32_t)target_x);
            s.scratch_set_field(4, (int32_t)target_y);
            s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1, 0x1c);
            return;
        }
    }

    uint8_t move_op_arg =
        v.cfg_units[unit_at(v, attacker_player, attacker_unit_idx).unit_proto_id].move_op_arg;
    s.dispatch((uint16_t)attacker_unit_idx, owner_of(attacker_player, KIND_UNIT), 0x1c, move_op_arg);
    emit_mp_ack_or_notify(s, c, v, attacker_player, attacker_unit_idx);
}

// ---- the public forms ---------------------------------------------------------------------------

void unit_order_attack_target(uint32_t attacker_player, int32_t attacker_unit_idx,
                              uint32_t target_player, int32_t target_unit_idx, uint32_t weapon) {
    detail::unit_order_attack_target(live_view(), live_sink(), live_calls(), attacker_player,
                                     attacker_unit_idx, target_player, target_unit_idx, weapon);
}
void unit_order_attack_target_alt(uint32_t attacker_player, int32_t attacker_unit_idx,
                                  uint32_t target_player, int32_t target_unit_idx, uint32_t weapon) {
    detail::unit_order_attack_target_alt(live_view(), live_sink(), live_calls(), attacker_player,
                                         attacker_unit_idx, target_player, target_unit_idx, weapon);
}
void unit_order_attack_unit(uint32_t attacker_player, int32_t attacker_unit_idx,
                            uint32_t target_player, int32_t target_unit_idx, uint32_t weapon) {
    detail::unit_order_attack_unit(live_view(), live_sink(), live_calls(), attacker_player,
                                   attacker_unit_idx, target_player, target_unit_idx, weapon);
}
void unit_order_attack_building_reposition(uint32_t attacker_player, int32_t attacker_unit_idx,
                                           uint32_t target_player, int32_t target_bldg_idx,
                                           uint32_t weapon) {
    detail::unit_order_attack_building_reposition(live_view(), live_sink(), live_calls(),
                                                  attacker_player, attacker_unit_idx, target_player,
                                                  target_bldg_idx, weapon);
}


} // namespace mh::orders::issue
