//
// sim/sim_debug_roll_random.h -- llm_debug_roll_random, the admin-table debug hook (RI-SIM / SIM1C).
//
// One function: llm_debug_roll_random @0x0049fc6f (0x53 bytes), batch C layer 2. It is
// dispatch_admin_order's arm 3 (order_code 0xe7, admin_arm::DEBUG_ROLL_RANDOM in
// sim_order_dispatch.h) -- a zero-arg diagnostic hook, not part of any decode/enqueue family, so it
// gets its own small module rather than being folded into sim_order_enqueue.h.
//
// Rolls llm_rand_below(100) and formats "random <clock>  <value>" into G_TEXT_TMP via
// w_sprintf's measured `vdi` shape (dst, format, double, int) -- see tools/data/varargs_shapes.json
// and SIM-VARARGS. Nothing reads the buffer back or checks w_sprintf's return; the write exists for
// its side effect on a scratch buffer that carries no MF_HASH/MF_SAVE/MF_MEASURED (sim_state.h's
// text_scratch() note).
//
// SHADOW HAZARD: llm_rand_below ADVANCES THE RNG. It is a callee, not a direct write this function's
// own state-matrix row measures, so `_G_LLM_STRAT_RNG_STATE` must be declared as an extra_regions
// claim on this site (the shadow manifest) -- the exact trap SIM1C hit
// on bldg_footprint_random_offset (the order-dispatch notes "the rig found a REAL divergence").
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct debug_roll_random_calls {
    int32_t (*rand_below)(int32_t upper_bound);                                         // llm_rand_below
    int32_t (*w_sprintf__vdi)(void *dst, const wchar_t *format, double a0, int32_t a1); // w_sprintf
};

const debug_roll_random_calls &live_debug_roll_random_calls();

namespace detail {

// llm_debug_roll_random @0x0049fc6f.
void debug_roll_random(const sim_view &v, sim_store &own, const debug_roll_random_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().own and live_debug_roll_random_calls(). Matches the
// original's __watcall (no-arg) shape.
void debug_roll_random();


} // namespace mh::sim
