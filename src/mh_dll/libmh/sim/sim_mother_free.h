#pragma once
//
// sim/sim_mother_free.h -- mods:A1, `[net] mother_free_build` / `[net] mother_free_build_ai`: build without a mothership.
//
// Retail reads `profiles[player].primary_mother_bldg[planet]` (0 = no landed, completed mother on this planet) as a
// gate at seven places: four game_SetEvent build-panel cases, llm_ui_bldg_panel_open (the initial page),
// llm_strat_bldg_try_begin_placement (silent refusal to arm) and the instant-construct arm of the order dispatcher
// (case 0x19, the only SIM site). With the key on the gate reads OPEN. That alone is not enough: a building placed with no
// connected neighbour gets built_flags 2 (staffed, not connected) and the tick budget of a building with built_flags != 3
// is zero, so the first plant never starts to build. So, second half of the rule, a POWER PLANT is a network root exactly
// when the owner has no primary mother on the planet: llm_strat_bldg_link_to_network_if_adjacent floods from it (as it does
// from a mother) and llm_strat_bldg_power_network_recompute seeds from it before it is online. A planet with a mother is
// untouched, so a vanilla grid never sees the rule.
//
// The AI is a separate option (`mother_free_build_ai`, default off): without it the rule covers human players only
// (player_data.ai_enabled == 0), so the AI is refused exactly as in retail.
//
// Both flags are host-set (F_HOSTSYNC): every peer holds the host's value. The retail byte splices
// (net_lockstep.cpp install_mother_free_build) read their own runtime copies; this header is the twin's store, written by
// lockstep::set_fixes. Header-only: inline function statics, one per module.
//
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace mother_free_detail {
struct flags {
    bool on = false;
    bool ai = false;
};
inline flags &store() {
    static flags f;
    return f;
}
} // namespace mother_free_detail

inline void set_mother_free_build(bool on, bool ai_too) {
    auto &f = mother_free_detail::store();
    f.on    = on;
    f.ai    = on && ai_too;
}
inline bool mother_free_build() { return mother_free_detail::store().on; }
inline bool mother_free_build_ai() { return mother_free_detail::store().ai; }

// Does the rule cover a player whose ai_enabled flag is `player_is_ai`?
inline bool mother_free_covers(bool player_is_ai) {
    const auto &f = mother_free_detail::store();
    return f.on && (!player_is_ai || f.ai);
}
inline bool mother_free_covers(const sim_view &v, uint32_t player) {
    return mother_free_covers(player_of(v, player & 7u).ai_enabled != 0);
}

// The primary-mother gate as the build sites read it: a mother is present, or the rule opens the gate for this player.
inline bool mother_gate_open(const sim_view &v, uint32_t player, int32_t planet) {
    return v.profiles[player].primary_mother_bldg[planet] != 0 || mother_free_covers(v, player);
}

// A power plant seeds the network (link_to_network / recompute) when the rule covers its owner and the owner has no
// primary mother on this planet.
inline bool mother_free_plant_root(const sim_view &v, uint32_t player, int32_t planet) {
    return v.profiles[player].primary_mother_bldg[planet] == 0 && mother_free_covers(v, player);
}

} // namespace mh::sim
