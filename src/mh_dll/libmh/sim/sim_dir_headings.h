#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the two outward calls, shared by both functions -----------------------------------------
struct dir_headings_calls {
    // llm_strat_map_wrapped_delta @0x004945de. Out-params match mh_calls.gen.h's own committed
    // `double*` signature exactly (see header banner).
    void (*wrapped_delta)(int32_t, int32_t, int32_t, int32_t, double *, double *);
    double (*atan)(double x); // llm_math_atan @0x004daa52
};

const dir_headings_calls &live_dir_headings_calls();

namespace detail {

// llm_strat_dir_from_to @0x0049482b. Returns 1..24. Call order into wrapped_delta is
// (x2, y1, x1, y2, &dx, &dy) -- read directly off the register loads at 0x00494854-0x0049485d, NOT
// the natural (x1,y1,x2,y2) reading.
int32_t dir_from_to(const sim_view &v, const dir_headings_calls &c, int32_t x1, int32_t y1, int32_t x2,
                    int32_t y2);

// llm_strat_dir_sector_to @0x004948ff. Returns 1..128. Call order into wrapped_delta is
// (x1, y0, x0, y1, &dx, &dy) -- read directly off the register loads at 0x00494928-0x00494931, and
// DIFFERENT from dir_from_to's own scramble (the function's own Ghidra plate already flags this;
// see it, not a re-derivation, per the batch context).
int32_t dir_sector_to(const sim_view &v, const dir_headings_calls &c, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1);

} // namespace detail

// Live wrappers: the logic applied to state().read and live_dir_headings_calls(). Signatures match
// the committed sig_llm_strat_dir_from_to / sig_llm_strat_dir_sector_to shapes (mh_export.gen.h) --
// both plain int32_t x4 -> int32_t, __cdecl (this project's default free-function convention), even
// though dir_sector_to's ORIGINAL is __mh_watcall_ecx_ebx_volatile: that convention describes how the
// game's own call sites reach the original function, not a constraint on this C++ body, which the
// shadow trampoline (conductor-owned, addr/mh_shadow.gen.h) adapts.
int32_t dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
int32_t dir_sector_to(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

namespace detail {
} // namespace detail

} // namespace mh::sim
