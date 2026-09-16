#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_select_weapon.h" // UNIT_SELECT_WEAPON_NOT_FOUND (reused, not redeclared)

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// unit_set_state is an ALREADY-REIMPLEMENTED sibling (sim_unit_set_state.h/.cpp), bound to its own
// public wrapper in the live binding (same seam rationale sim_unit_fire_weapon.h documents for its own
// group-(b) siblings) -- the other three are ORIGINAL functions outside this batch.
struct unit_fire_at_target_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y);                                // llm_strat_unit_get_coords @0x0044b141
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049482b
    void (*unit_set_state)(uint16_t new_state);                             // mh::sim::unit_set_state (sibling) @0x004866c9
    void (*unit_fire_weapon)(uint32_t player, uint32_t unit_index, uint8_t weapon_slot,
                             uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                             int32_t target_fine_y); // llm_strat_unit_fire_weapon @0x0048bb6c
};

const unit_fire_at_target_calls &live_unit_fire_at_target_calls();

namespace detail {

// llm_strat_unit_fire_at_target_if_aimed @0x004862c6. See the header banner above. Pure read: no
// sim_store parameter (nothing in this function's own body writes sim state).
void unit_fire_at_target_if_aimed(const sim_view &v, const unit_fire_at_target_calls &c);

// llm_strat_unit_fire_at_target @0x00486506. The _if_aimed body plus the move_op_code/selected_weapon
// guards described above.
void unit_fire_at_target(const sim_view &v, const unit_fire_at_target_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state().read and live_unit_fire_at_target_calls(). Match the
// committed prototypes (sig_llm_strat_unit_fire_at_target_if_aimed / sig_llm_strat_unit_fire_at_target)
// exactly.
void unit_fire_at_target_if_aimed();
void unit_fire_at_target();

namespace detail {
} // namespace detail

} // namespace mh::sim
