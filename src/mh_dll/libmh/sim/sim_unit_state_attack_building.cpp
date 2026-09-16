//
// sim/sim_unit_state_attack_building.cpp -- see sim_unit_state_attack_building.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_state_attack_building_00484e10.asm) -- see the header
// banner for the aliased-out-pointer preserved bug and the unbacked-enum literal derivation.
//
#include "sim/sim_unit_state_attack_building.h"

#include "addr/mh_calls.gen.h"          // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"                // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h"      // UNIT_STATE_STOP_TO_DEFAULT (shared there)
#include "sim/sim_unit_select_weapon.h" // UNIT_SELECT_WEAPON_NOT_FOUND (shared there)
#include "addr/mh_rebind.gen.h"         // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_attack_building_calls &live_unit_state_attack_building_calls() {
    static const unit_state_attack_building_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_dir_from_to),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_fire_weapon),
    };
    return c;
}

namespace detail {

void unit_state_attack_building(const sim_view &v, sim_store &own,
                                const unit_state_attack_building_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    // 0x00484e28-0x00484e36: unconditional entry drain -- this state never carries a budget shortfall
    // forward (unlike move_walker's mid-step gates); every tick either fires, re-plans, or does
    // nothing, and the budget is simply zeroed regardless.
    own.tick_budget() = 0.0;

    // 0x00484e3c-0x00484e76: are we facing the target yet?
    int32_t self_fine_x = 0, self_fine_y = 0;
    c.unit_get_coords(player, unit_index, &self_fine_x, &self_fine_y);
    const int32_t facing_needed =
        c.dir_from_to(self_fine_x, self_fine_y, u.target_fine_x, u.target_fine_y);

    if (static_cast<int32_t>(u.facing_current) != facing_needed) {
        // 0x00484e85: not turned to face the target yet -- nothing to do this tick (turning happens in
        // a different state).
        return;
    }

    // 0x00484e8b-0x00484ec4: target building still standing?
    const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
    const uint16_t target_slot  = static_cast<uint16_t>(u.target_index);
    if (v.buildings[target_owner * v.caps.buildings + target_slot].energy <= 0.0) {
        // 0x00484ec6-0x00484f09: destroyed -- release, clear, stop. No notify call on this path
        // (verified against the asm -- unlike move_walker's ATTACK_* cascades, this one skips it).
        c.target_release_ref(player, unit_index, 1u);
        u.target_ref   = 0;
        u.target_index = 0;
        c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // 0x00484f0e-0x00484f17: target still standing.
    if (u.selected_weapon != UNIT_SELECT_WEAPON_NOT_FOUND) {
        // 0x00484f19-0x00484f64: a weapon is already selected -- fire it. Register/stack argument
        // order re-derived from the CALL site (player, unit_index, selected_weapon, target_ref,
        // target_index, target_fine_x, target_fine_y), matching llm_strat_unit_fire_weapon's committed
        // prototype.
        c.unit_fire_weapon(player, unit_index, u.selected_weapon,
                           static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)), target_slot,
                           u.target_fine_x, u.target_fine_y);
        return;
    }

    // 0x00484f69-0x00484f88: no weapon selected yet -- try to pick one.
    const uint8_t selected = c.unit_select_weapon(player, unit_index, 1u);
    if (selected == UNIT_SELECT_WEAPON_NOT_FOUND) {
        // 0x00484f88: nothing usable this tick -- leave selected_weapon at its sentinel and bail.
        return;
    }

    // 0x00484f8a-0x00484ffb: a weapon slot was found -- re-fetch the target building's fine coords
    // (PRESERVED BUG: both out-pointers alias target_fine_x -- see the header banner; this passes the
    // two identical pointers literally and relies on the real llm_strat_bldg_get_coords, called
    // through mh::call::, to reproduce the original's own memory effect), re-plan around the building,
    // notify, and commit the newly selected weapon.
    c.bldg_get_coords(static_cast<uint16_t>(target_owner), static_cast<int32_t>(target_slot),
                      &u.target_fine_x, &u.target_fine_x);
    c.unit_set_state_order(ATTACK_BUILDING_ORDER_GROUP_MARSHAL, ATTACK_BUILDING_STATE_ATTACK_BUILDING);
    c.unit_notify_status(player, unit_index, ATTACK_BUILDING_NOTIFY_REPLAN);
    u.selected_weapon = selected;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_attack_building() {
    sim_state st = state();
    detail::unit_state_attack_building(st.read, st.own, live_unit_state_attack_building_calls());
}


} // namespace mh::sim
