#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The five outward calls this function makes, indirected via mh::call:: for testability -- same shape
// as every other sim/ TU (see sim_prod_shuttle_depart.h's header banner for why this is not a direct
// call to a sibling's public mh::sim:: wrapper).
struct prod_spawn_arrived_unit_calls {
    uint32_t (*unit_create)(uint32_t x, uint32_t y, uint16_t unit, uint16_t player,
                            uint8_t is_ship); // llm_strat_unit_create @0x00463860
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col,
                        int32_t tile_row); // event record: the hosted sink runs the original
                                           // offscreen_snd_volume+snd_play pair at emit
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_id, double game_clock,
                              uint32_t param_5); // llm_strat_fx_anim_spawn @0x00464855
    void (*unit_order_exit_storage_auto)(uint32_t player, uint32_t unit_id, int32_t storage_idx,
                                         uint32_t x, uint32_t y); // llm_strat_unit_order_exit_storage_auto @0x0046ab08
};

const prod_spawn_arrived_unit_calls &live_prod_spawn_arrived_unit_calls();

namespace detail {

// llm_strat_prod_spawn_arrived_unit @0x0048f31e. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Returns the new unit id, 0 on
// rejection (gate failed) or creation failure (no room).
uint32_t prod_spawn_arrived_unit(const sim_view &v, sim_store &own,
                                 const prod_spawn_arrived_unit_calls &c, uint16_t player,
                                 uint32_t slot, uint32_t x, uint32_t y, int32_t storage_idx);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_prod_spawn_arrived_unit) exactly.
uint32_t prod_spawn_arrived_unit(uint16_t player, uint32_t slot, uint32_t x, uint32_t y,
                                 int32_t storage_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
