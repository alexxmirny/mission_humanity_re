#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI -- the already-named, Ghidra-dump-confirmed threshold (see banner CORRECTION note)
#include "sim/sim_state.h"

namespace mh::sim {

// Unit[proto].type < UNIT_TYPE_A_HELI (ground/vehicle/walker/soldier bulk, see the header banner's
// CORRECTION note) -> interpolates the facing_step_offset table; >= UNIT_TYPE_A_HELI (heli/plane/
// heli_mother, i.e. the actually-airborne classes) -> uses the per-heading/per-microstep
// _G_LLM_STRAT_MOVE_MICROSTEPS offset instead. Read off the `CMP dword[...],0xe` / `JG` pair
// present, identically, at the top of get_coords (0x0044b182/0x0044b189), calc_fine_axis_pos
// (0x0048c87e/0x0048c885), and calc_render_fine_y (0x0048ccf1/0x0048ccf8) -- calc_interp_pixel_pos
// has no such check at all (see the header banner). The original compares SIGNED (CMP+JG);
// reproduced as a signed compare below even though Unit.type's realistic domain (a small enum)
// makes the distinction moot. UNIT_TYPE_A_HELI itself is defined in sim_order_enqueue.h, not
// re-declared here (rule 17a -- use the database's existing name).

namespace detail {

// llm_strat_unit_get_coords @0x0044b141. Writes BOTH axes from ONE call -- see the header banner
// on why this is an independent transcription, not two calls to calc_fine_axis_pos. Signature
// mirrors the committed export/call shape (out params are raw fine_coord* here; the public
// wrapper below matches them exactly to addr/mh_calls.gen.h's committed
// `llm_strat_unit_get_coords(uint16_t, int32_t, int32_t*, int32_t*)` -- TACT1-P C6, 2026-09-04,
// was `void*, void*`).
void get_coords(const sim_view &v, uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);

// llm_strat_unit_calc_fine_axis_pos @0x0048c83f. axis_is_x: 1 => X, anything else => Y (the
// original tests `== 1`, not "truthy" -- reproduced as `== 1` below, matching
// sim_unit_mount_pos.cpp's identical convention for the same parameter name).
int32_t calc_fine_axis_pos(const sim_view &v, uint16_t player, int32_t unit_idx, char axis_is_x);

// llm_strat_unit_calc_render_fine_y @0x0048ccb4. See the header banner's two HAZARD notes (a)/(b)
// vs calc_fine_axis_pos's own Y arm: elevation subtraction + bh_mask AND on the
// type>=UNIT_TYPE_A_HELI branch, not a modulo.
uint32_t calc_render_fine_y(const sim_view &v, uint16_t player, int32_t unit_idx);

// llm_strat_unit_calc_interp_pixel_pos @0x0048ce21. NO Unit.type gate (see header banner) --
// unconditionally uses the type<UNIT_TYPE_A_HELI-style formula for both axes, but WITHOUT that
// branch's own "+0x10" constant (confirmed absent: neither 0x0048ce53-0x0048cec6 nor
// 0x0048cecb-0x0048cf2a contains an `ADD ..,0x10`, unlike get_coords/calc_fine_axis_pos's own
// type<UNIT_TYPE_A_HELI branches which both do).
int32_t calc_interp_pixel_pos(const sim_view &v, uint16_t player, int32_t unit_idx, char axis_is_x);

} // namespace detail

// Live wrappers: the logic applied to state().read. Signatures match the committed
// __watcall / __mh_watcall_ebx_volatile shapes already in addr/mh_calls.gen.h exactly.
void     get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
int32_t  calc_fine_axis_pos(uint16_t player, int32_t unit_idx, char axis_is_x);
uint32_t calc_render_fine_y(uint16_t player, int32_t unit_idx);
int32_t  calc_interp_pixel_pos(uint16_t player, int32_t unit_idx, char axis_is_x);

namespace detail {
} // namespace detail

} // namespace mh::sim
