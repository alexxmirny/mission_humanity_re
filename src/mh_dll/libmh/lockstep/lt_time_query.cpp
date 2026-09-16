//
// lockstep/lt_time_query.cpp -- see lt_time_query.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/time_GetCurrentTime_00427616.asm), not from the Ghidra .c draft beside it
// (though in this one case the draft's `(double)(uint)_G_LLM_GAME_CLOCK_RAW_TICKS /
// _G_LLM_GAME_CLOCK_TICKS_PER_SECOND` already agrees with the asm -- see the header's derivation).
//
#include "lockstep/lt_time_query.h"

#include "addr/mh_addrs.gen.h"   // the two clock constants (registry-pinned)
#include "addr/mh_regions.gen.h" // mh::state::ptr / RID_GAME_CLOCK_RAW_TICKS / RID_GAME_CLOCK_TICKS_PER_SECOND

namespace mh::lockstep {

namespace detail {

// 0x0042762f: MOV EAX,[raw_ticks]. 0x00427634/0x00427637: spilled to a stack qword with the high
// dword explicitly zeroed -- zero-extension, not sign-extension (see the header derivation: binding
// `raw_ticks` as `const uint32_t *` and reading through it is what makes `static_cast<double>` do
// the zero-extending conversion instead of a sign-extending one). 0x0042763e/0x00427641: FILD the
// zero-extended 64-bit integer, FDIV by the memory operand `*ticks_per_second` (not folded as a
// literal -- see the header). 0x00427647/0x0042764a: store/reload through a local double, which is
// what makes the original's return an honest rounded 64-bit double rather than an unrounded 80-bit
// x87 intermediate; plain `double` arithmetic already has that rounding, so nothing extra is written
// here to reproduce it.
double time_GetCurrentTime(const lt_time_query_state &s) {
    return static_cast<double>(*s.raw_ticks) / *s.ticks_per_second;
}

} // namespace detail

// ---- the public wrapper ------------------------------------------------------------------------------

double time_GetCurrentTime() { return detail::time_GetCurrentTime(time_query_state()); }


} // namespace mh::lockstep
