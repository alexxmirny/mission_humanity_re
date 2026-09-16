#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_start_liftoff_anim_shuttle @0x00475ce3. param_1(EAX)/param_2(EDX) are the (player,
// index) building coordinate (param_1 masked to its low 16 bits by the asm before use, same as every
// other sim/ site that reads a player id out of a wider register). param_3(EBX)/param_4(ECX) are DEAD
// (see the header derivation) -- kept for prototype fidelity, marked unused. param_5/param_6 are
// GAME_CLOCK's raw low/high dwords (see the header derivation) -- combined via memcpy into the same
// double value stamped into anim_dur[0..3].
void bldg_start_liftoff_anim_shuttle(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                                     uint32_t param_3, uint32_t param_4, uint32_t param_5, uint32_t param_6);

// llm_strat_bldg_start_liftoff_anim_mother @0x00475ef9. Same param shape/roles as the shuttle sibling
// above, just a single anim_dur[0] write instead of a four-entry loop, and a different cfg `anim`
// sub-index (5, not 10) feeding anim[0] instead of anim[3].
void bldg_start_liftoff_anim_mother(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                                    uint32_t param_3, uint32_t param_4, uint32_t param_5, uint32_t param_6);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the originals' committed prototypes exactly
// (addr/mh_export.gen.h's sig_llm_strat_bldg_start_liftoff_anim_{shuttle,mother}).
void bldg_start_liftoff_anim_shuttle(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                                     uint32_t param_5, uint32_t param_6);
void bldg_start_liftoff_anim_mother(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                                    uint32_t param_5, uint32_t param_6);

namespace detail {
} // namespace detail

} // namespace mh::sim
