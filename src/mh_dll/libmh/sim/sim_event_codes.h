//
// sim/sim_event_codes.h -- shared `_G_LLM_GAME_SESSION_MODE` / `game::e::event` constants used by
// more than one libmh/sim/ TU.
//
// libmh/sim/ deliberately does not include libmh/lockstep/ or libmh/orders/ (see sim_state.h's `order`/
// `unit`/`building` alias note for the reasoning), so it cannot reuse orders/order_queue.h's or
// lockstep/turn_engine.h's own copies of SESSION_MP_LOCKSTEP -- this is the sim domain's own
// binding of the identical CMP-immediate the assembly reads, same numeric value by construction.
//
// Hoisted here (SIM1A, 2026-08-11) after sim_unit_on_destroyed.h and
// sim_unit_remove_from_map.h independently declared the SAME two names at the same `mh::sim` scope
// -- harmless while each TU is compiled alone, but a hard ODR/redefinition error the moment one TU
// includes both (reimpl_probe.cpp, wiring both install_shadow_* calls, is exactly that TU).
//
// game::e::event is a real, documented 25-member Ghidra enum (the strategic-sim notes, resolved
// 2026-07-05) with no generated C++ enum anywhere in the tree yet (checked mh_structs.gen.h and for
// any mh_enums.gen.h-shaped file) -- EVENT_INFO_REFRESH is the one member both current callers need.
// Add more members here as new call sites need them, rather than re-declaring a third local copy.
//
#pragma once
#include <cstdint>

namespace mh::sim {

inline constexpr int32_t SESSION_SP          = 1; // single-player
inline constexpr int32_t SESSION_MP_LOCKSTEP = 3; // live MP lockstep

inline constexpr uint32_t EVENT_INFO_REFRESH = 6; // game::e::event member 6

} // namespace mh::sim
