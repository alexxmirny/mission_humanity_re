#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// _DAT_00501442/_DAT_0050144a/_DAT_00501452 -- see the header banner above; read-memory-confirmed by
// the conductor (batch context tmp/decomp_g1/_CONTEXT.md). _DAT_0050144a is added (not subtracted),
// so it is committed here with its actual NEGATIVE sign, matching the asm's own FADD.
inline constexpr double CORPSE_FOW_DECAY_RING_BUDGET_THRESHOLD = 5.0;  // DAT_00501442
inline constexpr double CORPSE_FOW_DECAY_RING_BUDGET_ADD_BACK  = -5.0; // DAT_0050144a (negative)
inline constexpr double CORPSE_FOW_DECAY_SINGLE_STEP_THRESHOLD = 20.0; // DAT_00501452

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// three are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_state_corpse_fow_decay_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp).
struct unit_state_corpse_fow_decay_calls {
    void (*unit_free_slot)(uint32_t player, int32_t unit_index); // llm_strat_unit_free_slot @0x00487b25
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y,
                             uint8_t radius); // llm_strat_fow_remove_sight @0x00496868
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight); // @0x0049681a
};

const unit_state_corpse_fow_decay_calls &live_unit_state_corpse_fow_decay_calls();

namespace detail {

// llm_strat_unit_state_corpse_fow_decay @0x004822dc. See the header derivation above for the full
// shape. Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void)
// signature.
void unit_state_corpse_fow_decay(const sim_view &v, sim_store &own,
                                 const unit_state_corpse_fow_decay_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_corpse_fow_decay_calls(). Matches
// the committed prototype (sig_llm_strat_unit_state_corpse_fow_decay) exactly.
void unit_state_corpse_fow_decay();

namespace detail {
} // namespace detail

} // namespace mh::sim
