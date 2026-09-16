#pragma once
#include <cstdint>

namespace mh::lockstep {

// Everything time_GetCurrentTime reads, as typed pointers. Bound to the live game by time_query_state();
// bound to plain locals by the offline selftest.
struct lt_time_query_state {
    const uint32_t *raw_ticks;      // _G_LLM_GAME_CLOCK_RAW_TICKS (0x00e654e4) -- READ ONLY here;
                                    // written by the game's own GetTickCount wrappers, not this
                                    // unit. HOST-VOLATILE: never add this to a hash/save set.
    const double *ticks_per_second; // _G_LLM_GAME_CLOCK_TICKS_PER_SECOND (0x00500204), .rdata 100.0
};

lt_time_query_state time_query_state();

namespace detail {

// time_GetCurrentTime @0x00427616. `(double)(uint32_t)*raw_ticks / *ticks_per_second`, matching the
// FILD-zero-extend + FDIV-by-memory + store/reload-truncate sequence exactly -- see the header
// derivation above.
double time_GetCurrentTime(const lt_time_query_state &s);


} // namespace detail

// The live wrapper: the logic applied to time_query_state(). Matches the original's committed __watcall
// prototype exactly (no arguments, double return).
double time_GetCurrentTime();

} // namespace mh::lockstep
