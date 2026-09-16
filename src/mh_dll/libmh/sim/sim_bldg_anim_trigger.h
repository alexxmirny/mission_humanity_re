#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_start_special_anim @0x00475b75. `player`/`b_index` are the building coordinate
// (player is already a committed 16-bit param, no masking needed). `unused_ebx`/`unused_ecx` are
// DEAD (see the header derivation) -- kept for prototype fidelity. `timestamp` is a genuine
// committed `double` stamped verbatim into anim_dur[0..3].
void bldg_start_special_anim(const sim_view &v, sim_store &own, uint16_t player, int32_t b_index,
                             uint32_t unused_ebx, uint32_t unused_ecx, double timestamp);

// llm_strat_bldg_anim_state_trigger @0x00475e51. `param_1`/`param_2` are the (player, index)
// building coordinate (param_1 masked to its low 16 bits by the asm before use, same as the
// liftoff-anim siblings' own param_1). `param_3`/`param_4` are DEAD (see the header derivation).
// `param_5`/`param_6` are a raw dword pair combined bit-for-bit into the double stamped into
// anim_dur[0].
void bldg_anim_state_trigger(const sim_view &v, sim_store &own, uint32_t param_1, int32_t param_2,
                             uint32_t param_3, uint32_t param_4, uint32_t param_5, uint32_t param_6);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the originals' committed prototypes exactly
// (addr/mh_export.gen.h's sig_llm_strat_bldg_start_special_anim /
// sig_llm_strat_bldg_anim_state_trigger).
void bldg_start_special_anim(uint16_t player, int32_t b_index, uint32_t unused_ebx, uint32_t unused_ecx,
                             double timestamp);
void bldg_anim_state_trigger(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                             uint32_t param_5, uint32_t param_6);

namespace detail {
} // namespace detail

} // namespace mh::sim
