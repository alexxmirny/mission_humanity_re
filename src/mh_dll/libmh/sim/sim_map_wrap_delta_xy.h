//
// sim/sim_map_wrap_delta_xy.h -- two toroidal single-axis wrap-delta primitives (RI-SIM / SIM1-G2),
// siblings of the already-migrated `llm_map_wrap_delta_row` (sim_map_wrap_delta_row.h,
// SIM1F -- same shape, a DIFFERENT address, the row/height-axis twin of wrap_delta_y below):
//
//   llm_map_wrap_delta_x @0x004940a9 (0x88 bytes)
//   llm_map_wrap_delta_y @0x00494131 (0x88 bytes)
//
// `int __watcall llm_map_wrap_delta_x(int pos_a, undefined4 unused_param, int pos_b)` -- matches the
// committed prototype exactly (addr/mh_calls.gen.h): only 3 registers (EAX/EDX/EBX) are read back
// after the prologue spill; a 4th register (ECX) IS loaded by every live call site (confirmed against
// tmp/decomp_sim/llm_strat_mine_scan_deposit_slot_0047ab20.asm, which loads all four registers
// immediately before each call, ECX included) but is never read inside this function's own body --
// same "dead 4th register, committed prototype correctly omits it" situation the wrap_delta_row
// header documents for its own x1/x2. Computes the signed shortest X/column distance between pos_a
// and pos_b on a torus that wraps at `sim_view::map_width`, folding the raw delta across half the
// wrap extent when it exceeds it.
//
// `int __watcall llm_map_wrap_delta_y(int x1, int y1, int x2, int y2)` -- matches the committed
// prototype exactly. x1/x2 are spilled but never read again (confirmed against the .asm) -- same
// dead-parameter shape as `llm_map_wrap_delta_row`, just the X-axis-named-but-dead pair instead of
// Y. The real computation is the signed shortest Y/row distance between y1 and y2, wrapping at
// `sim_view::map_height`.
//
// Both are pure (no callee besides the inert Watcom stack probe, no write of any kind) -- 0 tracked
// write cells per tmp/state_matrix.json, matching this slice's own write-set preflight (NOT
// DIFFERENTIALLY VERIFIABLE). No entry-point seam in the .cpp; evidence is an offline oracle plus adversarial
// review, same posture as sim_map_wrap_delta_row.cpp.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_wrap_delta_x @0x004940a9. `unused_param` is accepted for signature parity with the
// committed prototype and every live call site, but dead in the body -- see the header banner.
int32_t wrap_delta_x(const sim_view &v, int32_t pos_a, uint32_t unused_param, int32_t pos_b);

// llm_map_wrap_delta_y @0x00494131. `x1`/`x2` are accepted for signature parity (every live call
// site passes real values) but dead in the body -- see the header banner.
int32_t wrap_delta_y(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2);

} // namespace detail

// Public wrappers. Match the committed prototypes (sig_llm_map_wrap_delta_x/_y, addr/mh_export.gen.h)
// and the callable shapes (mh::call::llm_map_wrap_delta_x/_y, addr/mh_calls.gen.h) exactly.
int32_t wrap_delta_x(int32_t pos_a, uint32_t unused_param, int32_t pos_b);
int32_t wrap_delta_y(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

} // namespace mh::sim
