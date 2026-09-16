//
// sim/sim_landing_queries.cpp -- see sim_landing_queries.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_count_landing_spots_00454d8d.asm,
// tmp/decomp/llm_strat_set_landing_site_00454fe3.asm). The count loop's status-before-bound test
// order and the set writer's three profile stores + `(uint8_t)spot_index` return were re-derived
// from the listings per house rules.
//
#include "sim/sim_landing_queries.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t count_landing_spots(const sim_view &v) {
    int32_t count = 0;
    // 0x00454d??: the test reads .status BEFORE checking i < 16 (see the header note), so the count
    // of the leading `.status != -1` run, capped at 16.
    for (int32_t i = 0; v.landing_spots[i].status != -1 && i < 16; i = i + 1) {
        count = count + 1;
    }
    return count;
}

uint8_t set_landing_site(sim_store &own, uint32_t player, uint32_t planet, int32_t x, int32_t y,
                         int32_t spot_index) {
    player_profile &p            = own.profile_at(static_cast<int32_t>(player));
    p.landing_x[planet]          = x;
    p.landing_y[planet]          = y;
    p.landing_spot_index[planet] = spot_index;
    return static_cast<uint8_t>(spot_index);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t count_landing_spots() {
    const sim_view v = state().read;
    return detail::count_landing_spots(v);
}

uint8_t set_landing_site(uint32_t player, uint32_t planet, uint32_t x, uint32_t param_4,
                         uint32_t param_5) {
    sim_state st = state();
    return detail::set_landing_site(st.own, player, planet, static_cast<int32_t>(x),
                                    static_cast<int32_t>(param_4), static_cast<int32_t>(param_5));
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
