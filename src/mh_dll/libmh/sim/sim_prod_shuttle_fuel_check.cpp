//
// sim/sim_prod_shuttle_fuel_check.cpp -- see sim_prod_shuttle_fuel_check.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_prod_shuttle_fuel_check_004934b6.asm), not from the Ghidra .c draft
// (the draft was cross-checked and found correct -- see the header banner -- but the body below is
// transcribed from the raw opcodes per house rules, not copied from it).
//
#include "sim/sim_prod_shuttle_fuel_check.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_fuel_check_calls &live_prod_shuttle_fuel_check_calls() {
    static const prod_shuttle_fuel_check_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
        MH_LIBMH_BIND(game_SpendResource),
    };
    return c;
}

namespace detail {

int32_t prod_shuttle_fuel_check(const sim_view &v, sim_store &own, const prod_shuttle_fuel_check_calls &c,
                                uint16_t player, int32_t building_index, int32_t dest_planet) {
    // 0x004934d5-0x004934e6: same-planet bail -- nothing checked, nothing charged.
    if (*v.planet_index == dest_planet) return 0;

    // 0x004934eb-0x00493525: building_id/shuttle_slot off the building instance.
    const building     &b            = building_of(v, static_cast<uint32_t>(player), building_index);
    const uint16_t      building_id  = b.building_id;
    const uint8_t       shuttle_slot = b.shuttle_slot;
    const cfg_building &cb           = v.cfg_buildings[building_id];

    // ---- PASS 1: the shortage scan (0x00493533-0x004935d0) --------------------------------------
    // id-read-before-bound-check order, no short-circuit -- walks every slot even after a shortage is
    // found, per header banner (byte-identical shape to sim_bldg_pay_costs.cpp's accumulation).
    int32_t shortage_code = 0;
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = static_cast<int32_t>(cb.fuel[i].id); // 0x00493542
        if (resource_id == 0) break;                                     // 0x0049354f
        if (!(i < CFG_RESOURCE_SLOTS)) break;                            // 0x00493555

        // 0x0049358c-0x00493592: held = player_resources[player][id] + reserved[player][slot][id].
        const int32_t held = player_resource_of(v, player, resource_id) +
                             v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot]
                                 .resources_reserved[resource_id];
        if (held < cb.fuel[i].val) { // 0x004935a3/0x004935a9
            // 0x004935ab-0x004935c2: first shortage sets id+0x89; any later one collapses to bare 0x89.
            shortage_code = (shortage_code == 0) ? (resource_id + 0x89) : 0x89;
        }
    }
    if (shortage_code != 0) return shortage_code; // 0x004935d0-0x004935dc: skip pass 2 entirely.

    // ---- PASS 2: the charge (0x004935e1-0x004936f3), only reached when pass 1 found nothing short --
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = static_cast<int32_t>(cb.fuel[i].id); // 0x004935f7
        if (resource_id == 0) break;                                     // 0x00493604
        if (!(i < CFG_RESOURCE_SLOTS)) break;                            // 0x0049360a

        const int32_t have = player_resource_of(v, player, resource_id); // 0x0049362f
        if (have < cb.fuel[i].val) {                                     // 0x00493635/0x0049363b
            const int32_t amount   = cb.fuel[i].val - have;              // 0x0049365f/0x00493665
            int32_t      &reserved = own.prod_shuttle_slot_at(player, shuttle_slot).resources_reserved[resource_id];
            if (amount <= reserved) {                                              // 0x0049368c/0x00493692
                reserved -= amount;                                                // 0x004936b2
                c.resource_add(static_cast<int32_t>(player), resource_id, amount); // 0x004936c2
            }
        }
        // 0x004936e3: unconditional -- both sub-branches above fall through to this same spend.
        c.spend_resource(static_cast<int32_t>(player), resource_id, cb.fuel[i].val);
    }
    return 0; // 0x004936f3-0x004936fa
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_fuel_check(uint16_t player, int32_t building_index, int32_t dest_planet) {
    sim_state st = state();
    return detail::prod_shuttle_fuel_check(st.read, st.own, live_prod_shuttle_fuel_check_calls(), player,
                                           building_index, dest_planet);
}


} // namespace mh::sim
