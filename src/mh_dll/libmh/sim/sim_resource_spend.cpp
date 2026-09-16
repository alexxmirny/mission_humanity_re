//
// sim/sim_resource_spend.cpp -- see sim_resource_spend.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_econ_track_unit_resource_spend_004e215f.asm,
// llm_strat_bldg_record_resource_expenditure_stats_004e685b.asm), not from Ghidra's C drafts.
//
#include "sim/sim_resource_spend.h"

namespace mh::sim {
namespace detail {

void accumulate_resource_spend(sim_store &own, uint32_t player, const cfg_resource *costs) {
    // UNSIGNED bound (CMP ECX,0x7 / JC @0x004e21d9). Seven, and the early exit below is what
    // actually ends the walk on the shipped cfg -- see the header.
    for (uint32_t i = 0; i < (uint32_t)CFG_RESOURCE_SLOTS; ++i) {
        const uint32_t id = costs[i].id;  // dword load @0x004e2189
        if (id == 0) return;              // JZ straight to the epilogue @0x004e2190
        const int32_t val = costs[i].val; // @0x004e21b4

        player_data &pd = own.player_at(player);
        // ADD dword ptr [.. + 0xe7df44],ESI @0x004e21ba -- resource_spend_total is indexed by the
        // resource id DIRECTLY (int[8], no id-0 element is ever touched).
        pd.resource_spend_total[id] += val;

        // Re-read per iteration exactly as the original does (MOV EAX,[.. + 0xe7df64] @0x004e21c0
        // inside the loop). Nothing in the loop writes it, so hoisting would be equivalent -- it is
        // written this way because the listing is, and because this module does not cache reads.
        const int32_t cursor = pd.ai_spend_ring_cursor;
        // ADD dword ptr [.. + EAX*4 + 0xe7e164],ESI @0x004e21d1. The displacement is the array base
        // MINUS FOUR because ids are 1-based; hence the `- 1`. See the header.
        pd.ai_spend_rate_numer_ring[cursor * SPEND_RING_RESOURCE_COLS + (int32_t)id - 1] += val;
    }
}

void econ_track_unit_resource_spend(const sim_view &v, sim_store &own, int32_t player_idx,
                                    int32_t unit_idx) {
    accumulate_resource_spend(own, (uint32_t)player_idx, v.cfg_units[unit_idx].resource);
}

void bldg_record_resource_expenditure_stats(const sim_view &v, sim_store &own, int32_t player_id,
                                            int32_t building_id) {
    accumulate_resource_spend(own, (uint32_t)player_id, v.cfg_buildings[building_id].resource);
}

} // namespace detail

void econ_track_unit_resource_spend(int32_t player_idx, int32_t unit_idx) {
    sim_state st = state();
    detail::econ_track_unit_resource_spend(st.read, st.own, player_idx, unit_idx);
}

void bldg_record_resource_expenditure_stats(int32_t player_id, int32_t building_id) {
    sim_state st = state();
    detail::bldg_record_resource_expenditure_stats(st.read, st.own, player_id, building_id);
}

} // namespace mh::sim
