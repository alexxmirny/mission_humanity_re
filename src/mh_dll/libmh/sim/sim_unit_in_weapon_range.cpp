//
// sim/sim_unit_in_weapon_range.cpp -- see sim_unit_in_weapon_range.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_in_weapon_range_0044965d.asm), cross-checked against the Ghidra .c draft
// (tmp/decomp/llm_strat_unit_in_weapon_range_0044965d.c), which this time matched the assembly
// branch-for-branch and field-for-field on a full manual re-trace.
//
#include "sim/sim_unit_in_weapon_range.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_in_weapon_range_calls &live_unit_in_weapon_range_calls() {
    static const unit_in_weapon_range_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
    };
    return c;
}

namespace detail {

uint32_t unit_in_weapon_range(const sim_view &v, const unit_in_weapon_range_calls &c, int32_t player,
                              int32_t unit_idx, int32_t tile_x, int32_t tile_y, int32_t target_class) {
    const unit &u = unit_of(v, static_cast<uint32_t>(player), unit_idx);

    // 0x0044968e-0x0044969e: no weapon selected at all -> never in range.
    if (u.selected_weapon == UNIT_SELECT_WEAPON_NOT_FOUND) return 0;

    // 0x004496a3-0x004496dc: the unit's OWN current tile vs the caller-supplied candidate tile,
    // computed unconditionally on this (post-sentinel) path, once, and reused by both arms below.
    const int32_t dist =
        c.tile_dist_wrapped(static_cast<int32_t>(u.x), static_cast<int32_t>(u.y), tile_x, tile_y);

    // target_class==1 selects the GROUND bit of Weapon.target; every OTHER value (2, or anything
    // else) selects the AIR bit -- a two-way branch on a nominally three-way (ground=1/air=2/none=0)
    // domain, read directly off the JNZ at 0x00449759/0x00449807, not "fixed" into a switch. See the
    // header banner for why target_class is llm_strat_target_class's return value, not a side selector.
    const uint8_t wanted_bit = (target_class == 1) ? WEAPON_TARGET_GROUND : WEAPON_TARGET_AIR;

    // Shared by both arms: does this weapon slot's cfg record match the wanted class AND cover `dist`?
    // 0x0044978c-0x004497af (scan arm) / 0x0044984c-0x0044986f (single-slot arm): identical range test,
    // range_max compared first (JG skips on dist > range_max), then range_min (JGE matches on
    // dist >= range_min) -- i.e. range_min[player] <= dist <= range_max[player], inclusive both ends.
    auto weapon_covers = [&](uint8_t weapon_id) {
        const cfg_weapon &w = v.cfg_weapons[weapon_id];
        return (w.target & wanted_bit) != 0 && dist <= w.range_max[player] &&
               w.range_min[player] <= dist;
    };

    if (u.selected_weapon == UNIT_SELECT_WEAPON_FOUND) {
        // 0x004496fc-0x004497c4: scan mount slots 0..3, first enabled+matching+in-range slot wins.
        for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS; ++slot) {
            const unit_weapon &uw = u.weapons[slot];
            if (uw.enabled_2 == 0) continue; // 0x00449733: JZ skip to next slot
            if (weapon_covers(uw.weapon_id)) return 1;
        }
        return 0; // 0x004497c4->0x0044987c: scan exhausted with no match.
    }

    // 0x004497c9-0x0044987c: single fixed slot `weapons[selected_weapon]` -- NOT gated on enabled_2
    // (unlike the scan arm above; preserved, not an oversight), and NOT re-checked against 4 again
    // (selected_weapon is 0..3 on this path by construction, having already failed both sentinel
    // tests above).
    const unit_weapon &uw = u.weapons[u.selected_weapon];
    return weapon_covers(uw.weapon_id) ? 1u : 0u;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                              int32_t target_class) {
    sim_state st = state();
    return detail::unit_in_weapon_range(st.read, live_unit_in_weapon_range_calls(), player, unit_idx,
                                        tile_x, tile_y, target_class);
}


} // namespace mh::sim
