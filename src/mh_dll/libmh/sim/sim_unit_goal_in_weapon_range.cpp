//
// sim/sim_unit_goal_in_weapon_range.cpp -- see sim_unit_goal_in_weapon_range.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_goal_in_weapon_range_0044988f.asm).
//
#include "sim/sim_unit_goal_in_weapon_range.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_goal_in_weapon_range_calls &live_unit_goal_in_weapon_range_calls() {
    static const unit_goal_in_weapon_range_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
    };
    return c;
}

namespace detail {

int32_t unit_goal_in_weapon_range(const sim_view &v, const unit_goal_in_weapon_range_calls &c,
                                  int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                  int32_t target_class) {
    const unit &u = unit_of(v, static_cast<uint32_t>(player), unit_idx);

    // 0x004498c0-0x004498d0: no weapon selected at all -> never in range.
    if (u.selected_weapon == UNIT_SELECT_WEAPON_NOT_FOUND) return 0;

    // 0x004498d5-0x00449909: the unit's own MOVE-ORDER GOAL tile vs the caller-supplied candidate
    // tile -- NOT the unit's current tile (that's sim_unit_in_weapon_range's own check). Computed
    // unconditionally on this (post-sentinel) path, once, and reused by both arms below.
    const int32_t dist =
        c.tile_dist_wrapped(static_cast<int32_t>(u.goal_x), static_cast<int32_t>(u.goal_y), tile_x,
                            tile_y);

    // target_class==1 selects the GROUND bit of Weapon.target; every OTHER value selects the AIR bit
    // -- same two-way branch on a nominally three-way domain as sim_unit_in_weapon_range.cpp, read
    // directly off THIS function's own JNZ at 0x00449987/0x00449a35, not assumed from the sibling.
    const uint8_t wanted_bit = (target_class == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;

    // Shared by both arms: does this weapon slot's cfg record match the wanted class AND cover `dist`?
    // 0x004499af-0x004499e5 (scan arm) / 0x00449a6f-0x00449aa5 (single-slot arm): identical range
    // test, range_max compared first (JG skips on dist > range_max), then range_min (JGE matches on
    // dist >= range_min) -- i.e. range_min[player] <= dist <= range_max[player], inclusive both ends.
    auto weapon_covers = [&](uint8_t weapon_id) {
        const cfg_weapon &w = v.cfg_weapons[weapon_id];
        return (w.target & wanted_bit) != 0 && dist <= w.range_max[player] &&
               w.range_min[player] <= dist;
    };

    if (u.selected_weapon == UNIT_SELECT_WEAPON_FOUND) {
        // 0x00449935-0x004499f6: scan mount slots 0..3, first enabled+matching+in-range slot wins.
        for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
            const unit_weapon &uw = u.weapons[slot];
            if (uw.enabled_2 == 0) continue; // 0x00449965: JZ skip to next slot
            if (weapon_covers(uw.weapon_id)) return 1;
        }
        return 0; // 0x004499f6->0x00449aae: scan exhausted with no match.
    }

    // 0x004499fb-0x00449aae: single fixed slot `weapons[selected_weapon]` -- NOT gated on enabled_2
    // (unlike the scan arm above; preserved, not an oversight -- same asymmetry
    // sim_unit_in_weapon_range.cpp documents for its own single-slot arm).
    const unit_weapon &uw = u.weapons[u.selected_weapon];
    return weapon_covers(uw.weapon_id) ? 1 : 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_goal_in_weapon_range(int32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                                  int32_t target_kind) {
    sim_state st = state();
    return detail::unit_goal_in_weapon_range(st.read, live_unit_goal_in_weapon_range_calls(), player,
                                             unit_idx, target_x, target_y, target_kind);
}


} // namespace mh::sim
