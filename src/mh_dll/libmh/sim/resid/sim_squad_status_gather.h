#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The COMMANDO value of cfg_unit.soldier_type -- see the declared-need note above. Cited from
// squad_status_slot.is_commando's own Ghidra field comment (mh_structs.gen.h:1405), not invented.
inline constexpr uint32_t SOLDIER_TYPE_COMMANDO = 2;

// The two constants the .asm reads as literals: the squad-scan radius in tiles (LB, @0x0044d487) and
// the SQUAD_STATUS capacity (@0x0044d48e / the same 64 the zero-fill loop and the overflow guard both
// bound against).
inline constexpr int32_t SQUAD_SCAN_RADIUS_TILES = 0xf;  // 15
inline constexpr int32_t SQUAD_STATUS_CAPACITY   = 0x40; // 64

// The outward calls both functions make, indirected so detail:: stays testable under simtest. All
// three are frontier originals (none is a sim_resid sibling), so they are reached through
// mh::call:: in live_squad_status_gather_calls(), never called directly (translator-brief 3b).
struct squad_status_gather_calls {
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // llm_strat_tile_dist_wrapped @0x0049404e
    uint32_t (*save_planet_to_disk)(uint32_t planet_index, uint32_t param_2);     // map_SavePlanetToDisk @0x00447bb3
    void (*mission_start)();                                                      // llm_tact_mission_start @0x004290ea
};

const squad_status_gather_calls &live_squad_status_gather_calls();

namespace detail {

// llm_strat_bldg_gather_nearby_squad_status @0x0044d468. Reads the buildings/units/cfg rosters and
// the map wrap masks through `v`, writes the 64-slot SQUAD_STATUS roster, SQUAD_STATUS_COUNT and the
// five SQUAD_BB_* scalars through `own`, reaches its one frontier callee (tile_dist_wrapped) through
// `c`. Returns the final slot count, matching the original's `int` return.
int32_t bldg_gather_nearby_squad_status(const sim_view &v, sim_store &own,
                                        const squad_status_gather_calls &c, int32_t scan_player,
                                        int32_t bldg_owner, int32_t bldg_idx);

// llm_strat_try_enter_tactical_mission @0x0044d34f. Calls bldg_gather_nearby_squad_status above as a
// same-TU sibling (plain detail:: call, G21), then writes GAME_MODE through `own` and reaches
// save_planet_to_disk/mission_start through `c`. void return, matching the original.
void try_enter_tactical_mission(const sim_view &v, sim_store &own, const squad_status_gather_calls &c,
                                uint32_t player, uint32_t target_owner, uint32_t target_bldg_idx);

} // namespace detail

// Live wrappers: the logic applied to state() and live_squad_status_gather_calls().
int32_t bldg_gather_nearby_squad_status(int32_t scan_player, int32_t bldg_owner, int32_t bldg_idx);
void    try_enter_tactical_mission(uint32_t player, uint32_t target_owner, uint32_t target_bldg_idx);

} // namespace mh::sim
