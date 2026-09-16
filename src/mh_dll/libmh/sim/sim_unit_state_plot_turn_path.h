#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All three
// are ORIGINAL functions outside this batch -- none needs reimplementing here; live_calls() is the only
// binder. Signatures copied verbatim from addr/mh_calls.gen.h.
struct unit_state_plot_turn_path_calls {
    // llm_rand_below @0x00499f49. ADVANCES THE RNG -- see the shadow-hazard banner above. Not a pure
    // query, a real outward call.
    int32_t (*rand_below)(int32_t upper_bound);
    void (*unit_set_state)(uint16_t new_state);                                    // llm_strat_unit_set_state @0x004866c9
    void (*path_attach_slot)(int32_t player, int32_t unit_index, int32_t slot_id); // @0x00496a7b
};

const unit_state_plot_turn_path_calls &live_unit_state_plot_turn_path_calls();

namespace detail {

// llm_strat_unit_state_plot_turn_path @0x00485d43. See the header derivation above for the full shape
// and the declared needs. Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's
// void(void) signature. Reads path_slot_flags/dir_remap_table/cur_unit via `v`/`own` (own is used for
// the flag scan too -- see the declared-need note above on why); writes path_buffer_at() entries and
// cur_unit().path_cursor through `own`.
void unit_state_plot_turn_path(const sim_view &v, sim_store &own, const unit_state_plot_turn_path_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_plot_turn_path_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_plot_turn_path) exactly.
void unit_state_plot_turn_path();

namespace detail {
} // namespace detail

} // namespace mh::sim
