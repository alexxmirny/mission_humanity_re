#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_PLANE / UNIT_TYPE_H_PLANE -- the real
                                   // cfg_enum_E_UNIT_TYPE members this dispatch branches on, already
                                   // named there (SIM1C, 2026-08-08); NOT redeclared here per
                                   // translator-brief rule 17a.
#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest. Both
// are ORIGINAL functions outside this batch -- neither needs reimplementing here;
// live_unit_state_group_step_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h.
struct unit_state_group_step_calls {
    void (*group_step_plane)();  // llm_strat_unit_group_step_plane  @0x00484b4a
    void (*group_step_ground)(); // llm_strat_unit_group_step_ground @0x00483011
};

const unit_state_group_step_calls &live_unit_state_group_step_calls();

namespace detail {

// llm_strat_unit_state_group_step @0x00482f7f. See the header derivation above for the full shape.
// No parameters -- operates on `v`'s ambient cur_unit/cur_player/cur_index alone, matching the
// original's void(void) signature. No `sim_store &own` -- this function's own body writes nothing.
void unit_state_group_step(const sim_view &v, const unit_state_group_step_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_state_group_step_calls(). Matches
// the committed prototype (sig_llm_strat_unit_state_group_step) exactly.
void unit_state_group_step();

namespace detail {
} // namespace detail

} // namespace mh::sim
