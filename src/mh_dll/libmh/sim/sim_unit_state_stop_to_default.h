#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Both are
// ORIGINAL functions outside this batch -- neither needs reimplementing here; live_calls() is the
// only binder. Signatures copied verbatim from addr/mh_calls.gen.h.
struct unit_state_stop_to_default_calls {
    void (*path_free_slot)(uint16_t player, int32_t unit_index); // llm_strat_path_free_slot @0x004969e8
    void (*unit_set_state)(uint16_t new_state);                  // llm_strat_unit_set_state @0x004866c9
};

const unit_state_stop_to_default_calls &live_unit_state_stop_to_default_calls();

namespace detail {

// llm_strat_unit_state_stop_to_default @0x0047e1e1. See the header derivation above for the full
// shape and the declared need. Zero-arg, ambient cur_player/cur_index/cur_unit, matching the
// original's void(void) signature. Read-only on cur_unit (no field is written by this function); the
// only write is own.tick_budget().
void unit_state_stop_to_default(const sim_view &v, sim_store &own, const unit_state_stop_to_default_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_stop_to_default_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_stop_to_default) exactly.
void unit_state_stop_to_default();

namespace detail {
} // namespace detail

} // namespace mh::sim
