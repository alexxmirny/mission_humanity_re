#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability -- a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body untestable
// by net_selftest.exe simtest / the offline fixture (same reasoning as every other module here).
//
// NAMED `footprint_random_offset_calls`, not the bare `calls` sim_order_enqueue.h already uses at
// `mh::sim` namespace scope -- reimpl_probe.cpp includes every sim TU's header, so a second `calls`
// here would be an ODR redefinition (the exact collision sim_bldg_placement_preview.h's own comment
// documents having hit once already).
struct footprint_random_offset_calls {
    // llm_rand_below @0x00499f49. ADVANCES THE RNG -- see the shadow-hazard banner above. Not a pure
    // query, a real outward call.
    int32_t (*rand_below)(int32_t upper_bound);
};

const footprint_random_offset_calls &live_footprint_random_offset_calls();

namespace detail {

// llm_strat_bldg_footprint_random_offset @0x00449c61.
//
// `player`/`unit_index` are param_1 (EAX) / param_2 (EDX) of the committed __mh_watcall_ebx_volatile
// prototype -- DEAD register carriers: the asm stores them to stack slots at 0x00449c76/0x00449c79
// and never reads either slot again for the rest of the body (verified against the full 224-byte
// listing). Kept in the signature for fidelity to the committed prototype (addr/mh_calls.gen.h),
// marked unused.
//
// `target_player`/`target_bldg_idx` are a2 (EBX) / param_4 (ECX) -- the building being targeted,
// indexed as `buildings[target_player][target_bldg_idx]` (0x00449c82-0x00449c92).
//
// `out_fine_x`/`out_fine_y` are IN/OUT: the caller's EXISTING fine position, read, nudged by the
// jittered delta, and written back -- not pure out-params (matching bldg_calc_placement_corner_from_
// center's out_col/out_row being plain pointers, but here the pointee is read before it is written).
void bldg_footprint_random_offset(const sim_view &v, const footprint_random_offset_calls &c, uint32_t player,
                                  uint32_t unit_index, int32_t target_player, int32_t target_bldg_idx,
                                  uint32_t *out_fine_x, uint32_t *out_fine_y);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation) -- the boundary carries the committed pointee,
// `uint32_t *` out-params (TACT1-P C6, 2026-09-04), same convention as sim_bldg_placement_preview.h's
// bldg_calc_placement_corner_from_center.
void bldg_footprint_random_offset(uint32_t param_1, uint32_t param_2, int32_t a2, int32_t param_4,
                                  uint32_t *param_5, uint32_t *param_6);

namespace detail {
} // namespace detail

} // namespace mh::sim
