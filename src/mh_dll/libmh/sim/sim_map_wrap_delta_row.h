//
// sim/sim_map_wrap_delta_row.h -- llm_map_wrap_delta_row (RI-SIM / SIM1F).
//
// One function: llm_map_wrap_delta_row @0x0049446e (0x88 B), `int __watcall
// llm_map_wrap_delta_row(int x1, int y1, int x2, int y2)`. Despite the four-parameter signature,
// x1/x2 are NEVER READ by the body (confirmed against both the .asm and every live call site --
// sim_projectile_tick.cpp/sim_weapon_projectile_spawn.cpp always pass 0 for both) -- this is the
// signed shortest Y/row distance between y1 and y2 on a torus that wraps at
// `sim_view::geom->big_height`, folding the raw delta across half the wrap extent when it exceeds
// it. Pure function: reads exactly one scalar off `sim_view::geom` and touches nothing else, no
// callee besides the inert Watcom stack probe (translator-brief rule 6, omitted).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_wrap_delta_row @0x0049446e. x1/x2 are accepted for signature parity with the original
// (and every existing call site) but are dead in the body -- see the .asm, which never loads
// [EBP-0x24]/[EBP-0x1c] (the x1/x2 stack homes) after spilling them.
int32_t wrap_delta_row(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_map_wrap_delta_row, addr/mh_export.gen.h) and the callable shape
// (mh::call::llm_map_wrap_delta_row, addr/mh_calls.gen.h) exactly.
int32_t wrap_delta_row(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

namespace detail {
} // namespace detail

} // namespace mh::sim
