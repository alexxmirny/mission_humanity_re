#pragma once
#include <cstdint>

#include "addr/mh_structs.gen.h" // mh::game::mh_cfg_pre_struct_Planet (the typed planet_data param)
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// four are frontier originals (per _CONTEXT_CE.md's measured callee table for this row), reached
// through mh::call:: only inside live_scenario_planet_clone_calls(), never called directly.
struct scenario_planet_clone_calls {
    // cfg_final_planet_Construct @0x0045b69c. The planet_data parameter is TYPED rather than
    // `const void *` since 2026-08-31 (EN v384): the generated callable picked up
    // cfg_pre_struct_Planet once that struct entered the manifest (EN v382) and mh_calls.gen.h was
    // regenerated.
    void (*cfg_final_planet_Construct)(int32_t define_index, int32_t invention_index, char *map_name,
                                       const mh::game::mh_cfg_pre_struct_Planet *planet_data,
                                       char                                     *path_unc);
    void (*str_char_subst)(char *str, uint8_t mode, char c1, char c2); // llm_str_char_subst @0x004546e8
    void *(*str_ansi_to_wide)(void *dst, char *src);                   // llm_str_ansi_to_wide @0x004cf379
    void (*snd_ambient_planet_clone)(uint32_t planet_id);              // @0x0045bc70
};

const scenario_planet_clone_calls &live_scenario_planet_clone_calls();

namespace detail {

// llm_strat_scenario_planet_clone @0x0045ba25. Reads Planets[1].icon_index and
// empty_name_str through `v`; writes the scenario-planet UTF-16 scratch buffer, G_TEXT_PTRS[0xa8],
// and Planets[0x1f].system_index through `own`; reaches every callee (all frontier) through `c`.
// `cfg_blob` stays a raw caller-owned `void *` (see declared_needs). void return, matching the
// original.
void scenario_planet_clone(const sim_view &v, sim_store &own, const scenario_planet_clone_calls &c,
                           void *cfg_blob);

} // namespace detail

// Live wrapper: the logic applied to state() and live_scenario_planet_clone_calls().
void scenario_planet_clone(void *cfg_blob);

} // namespace mh::sim
