//
// sim/sim_unit_state_attack_unit.cpp -- see sim_unit_state_attack_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_attack_unit_00485008.asm) -- see the header banner
// for the weapon-pick-result float/MM0 decompiler artifact and the Unit.weapon_facing_tolerance tolerance
// derivation.
//
#include "sim/sim_unit_state_attack_unit.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT (shared there)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_attack_unit_calls &live_unit_state_attack_unit_calls() {
    static const unit_state_attack_unit_calls c = {
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_order_move_auto),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        MH_LIBMH_BIND(llm_strat_unit_fire_weapon),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_state (order/state) literals -- NOT backed by a real Ghidra enum, same finding
// as sim_unit_state_move_walker.cpp / sim_unit_update_rotation.cpp for this same field; values +
// names copied from sim_unit_update_rotation.cpp's identical STATE_ prefix (own local constants, not
// folded into a shared name, matching every sibling TU's stated reasoning for the identical situation).
inline constexpr uint16_t STATE_GROUP_MARSHAL = 0x0a;      // 0x0048526f / 0x00485433
inline constexpr uint16_t STATE_ATTACK_UNIT   = 0x1a;      // 0x00485274 (kept as `state` while `order`
                                                           // re-plans to GROUP_MARSHAL)
inline constexpr uint16_t STATE_ATTACK_UNIT_RETURN = 0x1b; // 0x004850cc / 0x004851c2

// llm_strat_unit_notify_status's status-code literals this function passes -- also unbacked (no Ghidra
// enum), named locally per move_walker.cpp's identical convention for the same two codes.
inline constexpr uint32_t NOTIFY_STILL_CHASING = 0x66; // "in progress" (moving/engaging)
inline constexpr uint32_t NOTIFY_REPLAN        = 1;    // exhausted/blocked -> re-plan bailout

// fine->tile: plain truncating division by 32. Every libmh/sim/ TU with this idiom (sim_order_enqueue.cpp,
// sim_bldg_state_destroyed.cpp, ...) writes this same file-local helper rather than the raw SAR/SHL/
// SBB/SAR sequence, because C++'s truncating-toward-zero signed division is value-for-value identical
// to that sequence for a power-of-two divisor (this codebase's own established, repeatedly-cross-
// checked precedent) -- UNLIKE sim_unit_state_move_walker.cpp, no batch-context override applies to
// THIS function, so the established simplification is used.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

void unit_state_attack_unit(const sim_view &v, sim_store &own, const unit_state_attack_unit_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    // 0x00485020-0x0048502a: this state handler consumes the WHOLE tick budget unconditionally -- no
    // incremental spend/carryover the way move_walker's microstep gate has.
    own.tick_budget() = 0.0;

    // 0x00485034-0x00485054: snapshot the primary target's (owner, slot) ONCE, before either cleanup
    // arm below zeroes target_ref/target_index -- the weapon-pick elevation check far below reuses
    // THIS snapshot, not a fresh re-read (matching the asm's own EBP-0x1c/EBP-0x30 reuse).
    const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
    const uint16_t target_slot  = static_cast<uint16_t>(u.target_index);

    // 0x0048506a-0x00485075: FLDZ/FCOMP(0.0, target.energy) -- JC (taken) means 0.0 < energy (target
    // ALIVE); fallthrough (not taken) means energy<=0 (DEAD).
    if (v.units[target_owner * v.caps.units + target_slot].energy <= 0.0) {
        // ---- 0x0048507b-0x00485143: target is dead -- release, clear, notify, maybe return home, stop.
        c.target_release_ref(player, unit_index, 1u);
        u.target_ref   = 0;
        u.target_index = 0;
        c.unit_notify_status(player, unit_index, NOTIFY_STILL_CHASING);
        if (u.state == STATE_ATTACK_UNIT_RETURN && (u.x != u.home_x || u.y != u.home_y)) {
            c.unit_order_move_auto(player, unit_index, u.home_x, u.home_y);
        }
        c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // ---- 0x00485144-...: target still alive -- in weapon range? ------------------------------------
    const int32_t target_class =
        c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                       static_cast<int32_t>(static_cast<uint16_t>(u.target_index)));
    const int32_t in_range = c.unit_in_weapon_range(player, unit_index, fine_to_tile(u.target_fine_x),
                                                    fine_to_tile(u.target_fine_y), target_class);

    if (in_range == 0) {
        if (u.state == STATE_ATTACK_UNIT_RETURN) {
            // 0x004851cd-0x0048525e: out of range while returning home -- same release/clear/maybe-
            // move-home/STOP_TO_DEFAULT sequence as the dead-target arm above (duplicated in the
            // original, not shared).
            c.target_release_ref(player, unit_index, 1u);
            u.target_ref   = 0;
            u.target_index = 0;
            if (u.x != u.home_x || u.y != u.home_y) {
                c.unit_order_move_auto(player, unit_index, u.home_x, u.home_y);
            }
            c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        } else {
            // 0x0048526f-0x00485295: still chasing -- re-plan via GROUP_MARSHAL; `state` STAYS
            // ATTACK_UNIT (only `order` changes -- see llm_strat_unit_set_state_order's field comment).
            c.unit_set_state_order(STATE_GROUP_MARSHAL, STATE_ATTACK_UNIT);
            c.unit_notify_status(player, unit_index, NOTIFY_REPLAN);
        }
        return;
    }

    // ---- 0x0048529b-...: in range -- check facing tolerance before firing --------------------------
    int32_t own_fine_x = 0, own_fine_y = 0;
    c.unit_get_coords(player, unit_index, &own_fine_x, &own_fine_y);
    const int32_t target_dir = c.dir_from_to(own_fine_x, own_fine_y, u.target_fine_x, u.target_fine_y);

    // 0x004852da-0x004852fd: HALF the per-unit-type facing-tolerance field (see the header's DECLARED
    // NEED for Unit.weapon_facing_tolerance -- this function halves it; the sibling
    // sim_unit_fire_at_target2_if_aimed.cpp uses the SAME field un-halved for its own tolerance check).
    const int32_t tol            = static_cast<int32_t>(v.cfg_units[u.unit_proto_id].weapon_facing_tolerance) / 2;
    const int32_t facing_current = u.facing_current;

    if (tol == 0) {
        // 0x0048536d-0x00485379: zero tolerance -- require an EXACT facing match.
        if (facing_current != target_dir) return;
    } else {
        // 0x00485300-0x0048536b: two mirrored circular-tolerance tests. Mathematically the De Morgan
        // negation of sim_unit_fire_at_target2_if_aimed.cpp's merged check_a/check_b form (re-derived
        // and cross-checked against that file), but kept here as the literal branchy return-early
        // shape because that is what THIS function's own disassembly implements (a separate
        // conditional-jump chain per direction, not a single merged boolean) -- flagged in
        // uncertainties as a reassembled-control-flow judgment call is NOT needed here since nothing
        // was collapsed; this transcribes each JG/JL pair directly.
        const bool blocked_a = (target_dir < facing_current) && (tol < facing_current - target_dir) &&
                               (facing_current - target_dir < 0x18 - tol);
        if (blocked_a) return;
        const bool blocked_b = (facing_current < target_dir) && (tol < target_dir - facing_current) &&
                               (target_dir - facing_current < 0x18 - tol);
        if (blocked_b) return;
    }

    // ---- 0x0048537f-...: aligned enough -- fire, or pick a weapon first -----------------------------
    if (u.selected_weapon != 100) {
        c.unit_fire_weapon(player, static_cast<uint32_t>(unit_index), u.selected_weapon,
                           static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)), target_slot,
                           u.target_fine_x, u.target_fine_y);
        return;
    }

    // 0x004853da-0x00485406: no weapon selected yet -- pick ground/air by the target's elevation
    // (reusing the target_owner/target_slot snapshot from the top of the function).
    const uint32_t target_mask =
        v.units[target_owner * v.caps.units + target_slot].elevation == 0 ? WEAPON_TARGET_GROUND
                                                                          : WEAPON_TARGET_AIR;
    // 0x00485417-0x0048541c: the callee's result comes back in AL (uint8_t) -- the Ghidra .c draft's
    // `unkfloat1 Var7 = ...; bStack_1c = SUB41((float)Var7,0)` reading of this call is decompiler noise
    // (an MM0-return artifact, per the translator brief's item 1), not a real float path;
    // mh_calls.gen.h's own committed `llm_strat_unit_select_weapon` prototype returns uint8_t.
    const uint8_t picked_weapon = c.unit_select_weapon(player, unit_index, target_mask);
    if (picked_weapon != 100) {
        c.unit_set_state_order(STATE_GROUP_MARSHAL, u.state);
        c.unit_notify_status(player, unit_index, NOTIFY_REPLAN);
        u.selected_weapon = picked_weapon;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_attack_unit() {
    sim_state st = state();
    detail::unit_state_attack_unit(st.read, st.own, live_unit_state_attack_unit_calls());
}


} // namespace mh::sim
