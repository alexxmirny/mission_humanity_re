//
// sim/sim_spawn_invasion_force.h -- llm_strat_spawn_invasion_force @0x004dd446, the AI per-player
// region initializer (RI-SIM / SIM1F batch F, -- the LAST function of the whole
// 305-function SIM migration).
//
// Fully initializes player_data[player] as an active AI INVASION faction (ai_invasion_force = 1, the
// reinforcement-only AI, as against llm_strat_spawn_ai_base's full-economy AI). It:
//   * parses the global + per-planet AI.SCR scripts (which is what populates the period constants),
//   * raises _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT to player+1 if this player is new,
//   * marks the profile ALIVE (bit 0x2),
//   * stamps the whole ai_* strategy/tactic block: race/home-tile, the resource-need + established +
//     phase/enabled/invasion flags, the invasion-points budget, the three staggered AI think-clocks
//     (ai_clock_{m,t,s} = (float)player * {move,tactic,strategy}_period * the 0.125 stagger scale),
//     the start-unit budget, the attack-milestone + map/turret-rescan latches,
//   * zeroes the per-player scan/spend grids (resource_spent[4], the two 32x4 spend rings, the tile
//     flag grid seeded from map::g::passable, the 32 ai_groups' member_count/current_param, the three
//     int[8] intel/relation rows, the per-unit-type train-queue counts),
//   * creates 5 AI task groups (goals 1/2/6/6/6) with matching group_task_enqueue calls,
//   * seeds the build-candidate priorities, and returns llm_strat_ai_invasion_spawn_reinforcements.
//
// DO-NOT-ARM / offline-oracle only. Same class as sim_player_presence_lost / sim_bldg_apply_damage:
// its tail call llm_strat_ai_invasion_spawn_reinforcements SPAWNS UNITS (and can play sounds / issue
// orders through its own callees), and llm_strat_ai_scr_parse does file I/O -- effects a shadow site
// would DOUBLE-FIRE under snapshot/restore. It is also SCENARIO-GAPPED (invasion forces spawn only in
// specific campaign/skirmish setups), so a live-game shadow arm would rarely even be reached. There
// is NO entry-point seam; the only verification is the offline oracle net_selftest simtest
// (sim_spawn_invasion_force_selftest.cpp), which drives the direct field-initialization (the whole
// observable body -- the four delegated helpers are stubbed and their calls asserted).
//
// ---- Translated from the DISASSEMBLY (tmp/decomp/llm_strat_spawn_invasion_force_004dd446.asm); the
// .c is a draft. The x87 clock formula (FILD player / FST float / FMUL float-period / FMUL
// double-stagger / FSTP float) was reproduced in that operand order; the dword store of 4 at
// ai_phase_flags (a byte, followed by 3 pad bytes) is a plain field write.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest.
struct spawn_invasion_force_calls {
    void (*ai_scr_parse)(char *filename); // llm_strat_ai_scr_parse @0x004ddb31
    int32_t (*sprintf_ai_scr)(void *dst, const char *fmt,
                              int32_t planet);  // utils_sprintf__vi @0x004cfb9c
    int32_t (*ai_group_create)(int32_t player); // llm_strat_ai_group_create @0x004d4d85
    void (*ai_group_task_enqueue)(int32_t player, int32_t group, uint16_t a2, uint32_t p4, uint32_t p5,
                                  uint32_t p6, uint32_t p7, uint32_t p8,
                                  uint16_t p9);                  // llm_strat_ai_group_task_enqueue @0x004d4c20
    void (*ai_init_build_candidate_priorities)(int32_t player);  // @0x004dc7be
    int32_t (*ai_invasion_spawn_reinforcements)(int32_t player); // @0x004e8773
};

const spawn_invasion_force_calls &live_spawn_invasion_force_calls();

namespace detail {

// llm_strat_spawn_invasion_force @0x004dd446. Reads planet/AI-config inputs through `v`, writes the
// whole player_data[player] AI block + the profile ALIVE bit + the active-player counter through
// `own`, and reaches the effectful/delegated AI helpers through `c`. Returns the original's result
// (the value of llm_strat_ai_invasion_spawn_reinforcements).
uint32_t spawn_invasion_force(const sim_view &v, sim_store &own, const spawn_invasion_force_calls &c,
                              uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                              int32_t home_tile_y, int32_t invasion_points);

} // namespace detail

// Live wrapper: the logic applied to state() and live_spawn_invasion_force_calls().
uint32_t spawn_invasion_force(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                              int32_t home_tile_y, int32_t invasion_points);

} // namespace mh::sim
