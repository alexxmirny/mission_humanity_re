//
// sim/resid/sim_bldg_try_begin_placement.cpp -- see sim_bldg_try_begin_placement.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_bldg_try_begin_placement_00448c0b.asm), the Ghidra
// .c being a draft.
//
#include "sim/resid/sim_bldg_try_begin_placement.h"

#include "sim/sim_bldg_pay_costs.h" // bldg_can_afford_build_cost: the side-effect-free probe (mp:D25)
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::sim {

const bldg_try_begin_placement_calls &live_bldg_try_begin_placement_calls() {
    static const bldg_try_begin_placement_calls c = {
        mh::sim::bldg_can_afford_build_cost,
        mh::state::evt::text_queue_id,
    };
    return c;
}

namespace detail {

// ---- llm_strat_bldg_try_begin_placement @0x00448c0b ------------------------------------------
int bldg_try_begin_placement(const sim_view &v, sim_store &own, const bldg_try_begin_placement_calls &c,
                             uint32_t player_idx, int32_t building_idx) {
    // 0x00448c28-0x00448c3d: _G_LLM_STRAT_PLAYERS[player_idx].primary_mother_bldg[*G_PLANET_INDEX].
    // See the header's "THE ADDRESSING" note for the offset derivation (0xcff274 - 0x214 = base of
    // _G_LLM_STRAT_PLAYERS). Read-only here -- this function never writes the profile table, so the
    // const `profiles` view is the right half, not sim_store::profile_at().
    if (v.profiles[player_idx].primary_mother_bldg[*v.planet_index] == 0) {
        // 0x00448c44-0x00448c4d: no mothership on this planet -- refuse silently, no message.
        return 0;
    }

    // 0x00448c4f-0x00448c5b: the original PAYS here (llm_bldg_pay_build_cost) and re-grants at
    // 0x00448c90. mp:D25: the probe is now side-effect-free -- same verdict, same text id, no
    // stock movement, no gains-counter booking (see the header). The payment itself is build order
    // 0x19's, on every peer.
    const int text_id = c.can_afford_build_cost(player_idx, building_idx);

    if (text_id != 0) {
        // 0x00448c64-0x00448c87: can't afford it. The 0/print/1 sequence around the print is a
        // SUPPRESSION bracket whose ORDER is the observable -- reproduced instruction-for-instruction
        // (flag=0, then the print, then flag=1), not as a wrapped RAII-style helper.
        own.floating_msg_queue_active() = 0;
        c.print_queue_text_id(text_id);
        own.floating_msg_queue_active() = 1;
        return 0;
    }

    // 0x00448c89-0x00448cb0: affordable -- arm build-placement mode and clear the two UI fields.
    // The original's grant_type_resources call at 0x00448c90 is deliberately absent (mp:D25): it
    // only ever undid the payment above, and its counter booking was the desync.
    own.build_placement_id()     = building_idx; // 0x00448c98: _G_LLM_BUILD_PLACEMENT_ID
    own.ctrl_group_at(0).count   = 0;            // 0x00448c9d: first dword of _G_LLM_STRAT_CTRL_GROUPS
    own.ui_selected_bldg_index() = 0;            // 0x00448ca7: 16-bit store
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int bldg_try_begin_placement(uint32_t player_idx, int32_t building_idx) {
    sim_state st = state();
    return detail::bldg_try_begin_placement(st.read, st.own, live_bldg_try_begin_placement_calls(),
                                            player_idx, building_idx);
}

} // namespace mh::sim
