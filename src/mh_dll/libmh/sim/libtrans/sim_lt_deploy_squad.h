#pragma once
#include <cstdint>

#include "sim/sim_fx_debris_burst.h" // sibling with its own _calls struct (rule 3c): detail::spawn_debris_burst
#include "sim/sim_state.h"
#include "sim/sim_unit_create_soldier.h" // sibling with NO _calls struct of its own: detail::create_soldier

namespace mh::sim {

// The still-original callees this TU reaches directly -- everything except the two in-tree
// siblings above (create_soldier, spawn_debris_burst), which are called as direct C++ per rule 3c.
struct deploy_squad_calls {
    int32_t (*get_starting_unit)(uint32_t race);        // game_GetStartingUnit @0x0045eded
    void (*cam_set_col)(int32_t col);                   // llm_map_cam_set_col @0x0044af33
    void (*cam_set_row)(int32_t row);                   // llm_map_cam_set_row @0x0044af6d
    void (*snd_play)(int32_t sound_id, int32_t volume); // llm_snd_play @0x00425233 (EFFECTFUL, verified)
    // llm_strat_unit_order_scatter_from_spawn @0x0046a626 -- ALREADY TRANSLATED, but in
    // mh::orders::issue (issue_unit_move.cpp), a module libmh/sim/ deliberately does not depend on
    // (see sim_state.h's `order` alias comment). Stays mh::call:: on purpose, through this struct
    // like every other outward call here -- not because it is untranslated, but because crossing
    // into mh::orders from mh::sim is out of scope for this closure.
    void (*scatter_from_spawn)(uint16_t player, int32_t unit_idx, uint32_t target_x, uint32_t target_y);
};

const deploy_squad_calls &live_deploy_squad_calls();

namespace detail {

// llm_strat_deploy_starting_squad @0x00465d16. Returns 1 and places the squad at the first clear
// 7x7 area found; returns 0 if the whole map has no such area. See the header banner for the full
// derivation of every branch below.
int32_t deploy_starting_squad(
    const sim_view &v, sim_store &own, const deploy_squad_calls &c,
    const debris_burst_calls &c_debris = live_debris_burst_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_deploy_squad_calls(). Matches the original's
// committed __watcall(void) -> int32_t shape (sig_llm_strat_deploy_starting_squad).
int32_t deploy_starting_squad();

} // namespace mh::sim
