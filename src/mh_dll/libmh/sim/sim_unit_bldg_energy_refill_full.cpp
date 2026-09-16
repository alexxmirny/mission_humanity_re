//
// sim/sim_unit_bldg_energy_refill_full.cpp -- see sim_unit_bldg_energy_refill_full.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_unit_bldg_energy_refill_full_0049a8c2.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_unit_bldg_energy_refill_full.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_bldg_energy_refill_full_calls &live_unit_bldg_energy_refill_full_calls() {
    static const unit_bldg_energy_refill_full_calls gc = {
        MH_LIBMH_BIND(llm_strat_bldg_update_charge_pips),
        MH_LIBMH_BIND(llm_strat_refresh_building),
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
        MH_LIBMH_BIND(llm_strat_unit_update_damage_smoke),
        MH_LIBMH_BIND(llm_strat_unit_notify_ui),
    };
    return gc;
}

namespace detail {

void unit_bldg_energy_refill_full(const sim_view &v, sim_store &own,
                                  const unit_bldg_energy_refill_full_calls &gc,
                                  uint32_t player_and_flags, uint32_t target_index) {
    // 0x0049a8df-0x0049a8e5: player = player_and_flags & 0xf, computed ONCE and used, masked, in
    // BOTH arms below -- neither arm ever indexes with the raw player_and_flags value.
    const uint32_t player = player_and_flags & 0xfu;

    // 0x0049a8e8-0x0049a8ef: TEST player_and_flags,0x80; JZ 0x0049a944 (the building arm's label).
    // Bit 0x80 CLEAR -> building arm; bit 0x80 SET -> unit arm (falls straight through from entry).
    if ((player_and_flags & 0x80u) == 0u) {
        // ---- building arm (0x0049a944-0x0049a9a1) ----------------------------------------------
        building &b = own.building_at(player, (int32_t)target_index);

        // 0x0049a954-0x0049a95b: buildings[player][target_index].building_id selects the cfg row;
        // 0x0049a971-0x0049a977: Building[building_id].energy is FLD'd and FSTP'd straight into
        // buildings[player][target_index].energy -- a plain COPY (max-out), not an accumulation.
        b.energy = v.cfg_buildings[b.building_id].energy;

        // 0x0049a984 / 0x0049a990 / 0x0049a99c, IN THIS ORDER, all (masked_player, target_index).
        gc.bldg_update_charge_pips((uint16_t)player, target_index);
        gc.refresh_building((uint16_t)player, (int32_t)target_index);
        gc.bldg_notify_ui((uint16_t)player, target_index);
    } else {
        // ---- unit arm (0x0049a8f1-0x0049a942) ---------------------------------------------------
        unit &u = own.unit_at(player, (int32_t)target_index);

        // 0x0049a901-0x0049a908: units[player][target_index].unit_proto_id selects the cfg row;
        // 0x0049a91e-0x0049a924: Unit[unit_proto_id].energy is FLD'd and FSTP'd straight into
        // units[player][target_index].energy -- same plain-copy shape as the building arm.
        u.energy = v.cfg_units[u.unit_proto_id].energy;

        // 0x0049a931 / 0x0049a93d, IN THIS ORDER, both (masked_player, target_index).
        gc.unit_update_damage_smoke(player, (int32_t)target_index);
        gc.unit_notify_ui(player, target_index);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_bldg_energy_refill_full(uint32_t player_and_flags, uint32_t target_index) {
    sim_state st = state();
    detail::unit_bldg_energy_refill_full(st.read, st.own, live_unit_bldg_energy_refill_full_calls(),
                                         player_and_flags, target_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
