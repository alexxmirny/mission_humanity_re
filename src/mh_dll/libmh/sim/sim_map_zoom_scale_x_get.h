//
// sim/sim_map_zoom_scale_x_get.h -- llm_map_zoom_scale_x_get (RI-SIM / SIM1F).
// Translated from the DISASSEMBLY (tmp/decomp/llm_map_zoom_scale_x_get_004a8016.asm), which agrees
// with the exported .c draft here (a straightforward stack-temp-and-FLD getter, nothing for the
// draft to get wrong).
//
//   llm_map_zoom_scale_x_get @0x004a8016 (0x37 B = 55 bytes), `double __watcall
//   llm_map_zoom_scale_x_get(void)`. Copies the 8 bytes of _G_LLM_MAP_ZOOM_SCALE_X into a stack
//   temp and FLDs it, returning the double in ST(0). Takes no arguments (EAX is pushed/popped as
//   part of the prologue/epilogue register save, never read or written). Pure read of a single
//   boot-loaded scalar (sim_view::zoom_scale_x, RID_MAP_ZOOM_SCALE_X) -- no other state touched, no
//   callee besides the inert `utils_assert_stack_capacity` stack probe (translator-brief rule 6:
//   omit it, it touches zero tracked regions).
//
// Sole caller in the whole image is llm_strat_spawn_debris_burst (sim_fx_debris_burst.cpp/.h, this
// same migration set), which immediately overwrites the returned value with the literal 1.0 -- so
// every live call site's result is a dead store. That is a fact about the CALLER, not a reason to
// special-case this getter: it is translated as the plain unconditional read the assembly performs.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_zoom_scale_x_get @0x004a8016. Returns *v.zoom_scale_x verbatim -- the FLD/stack-temp
// dance in the assembly has no observable effect beyond "read the 8 bytes and return them as a
// double", so it collapses to one dereference.
double zoom_scale_x_get(const sim_view &v);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_map_zoom_scale_x_get, addr/mh_export.gen.h) and the callable shape
// (mh::call::llm_map_zoom_scale_x_get, addr/mh_calls.gen.h) exactly.
double zoom_scale_x_get();

namespace detail {
} // namespace detail

} // namespace mh::sim
