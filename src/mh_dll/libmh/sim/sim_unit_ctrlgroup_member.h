#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls (both functions' tail notify-thunk call) -------------------------------
//
// Indirected for the same reason as sim_unit_ctrl_group.h's table and sim_order_enqueue.h's `calls`:
// a direct `mh::call::` inside a `detail::` body reaches into the live game image, which makes the
// body untestable by `net_selftest.exe simtest` (no game, no rig) and unusable in the offline
// fixture. Production binds this to `mh::call::llm_strat_order_ctrlgrp_select_member`/`_flash_member`
// (`live_unit_ctrlgroup_member_calls()`); simtest binds it to recording stubs.
struct unit_ctrlgroup_member_calls {
    void (*order_ctrlgrp_select_member)(uint32_t side, uint16_t unit_id,
                                        int32_t group_index); // llm_strat_order_ctrlgrp_select_member
    void (*order_ctrlgrp_flash_member)(uint16_t side, uint16_t unit_id,
                                       int32_t group_index); // llm_strat_order_ctrlgrp_flash_member
};

const unit_ctrlgroup_member_calls &live_unit_ctrlgroup_member_calls();

namespace detail {

// llm_strat_unit_ctrlgroup_add_member @0x00449401. See the header derivation above.
//
// PARAMETER ORDER (confirmed against the .asm header, not assumed): EAX=unit_id (int), EDX=
// count_ptr (int32_t*, the group's own `.count` field address per the caller's convention, but
// received here as an independent pointer -- see the header note on why it is not re-derived from
// group_idx), EBX=group_idx (int, the 3rd/EBX-register arg -- same out-of-declared-order shape
// sim_unit_ctrl_group.h's contains_unit already documents for this calling convention).
void unit_ctrlgroup_add_member(const sim_view &v, sim_store &own, const unit_ctrlgroup_member_calls &c,
                               int32_t unit_id, int32_t *count_ptr, int32_t group_idx);

// llm_strat_unit_ctrlgroup_remove_member @0x0044947e. See the header derivation above.
//
// PARAMETER ORDER: EAX=unit_idx (uint), EDX=count_ptr (int32_t*), EBX=group_idx (int) -- same shape
// as add_member's.
void unit_ctrlgroup_remove_member(const sim_view &v, sim_store &own, const unit_ctrlgroup_member_calls &c,
                                  uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx);

} // namespace detail

// Live wrappers: the logic applied to state() (and live_unit_ctrlgroup_member_calls()). Match the
// originals' committed __watcall shapes EXACTLY (mh_export.gen.h's sig_llm_strat_unit_ctrlgroup_
// add_member/_remove_member typedefs) -- the committed signature now carries `int32_t *` for the
// count-pointer parameter (TACT1-P C6, 2026-09-04), matching detail:: exactly, so no cast is needed
// at this boundary.
void unit_ctrlgroup_add_member(int32_t param_1, int32_t *param_2, int32_t a2);
void unit_ctrlgroup_remove_member(uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
