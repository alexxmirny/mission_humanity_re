//
// lockstep/lt_peer_timing_reset.h -- llm_net_lockstep_peer_timing_reset @0x0049fef7 (batch D / d9,
// LT-lib_trans). Resets the 8-entry per-peer lockstep-timing table (_G_LLM_NET_LOCKSTEP_PEER_TIMING,
// engine_state::peer) at MP session start: each entry's `horizon` -> HORIZON_NONE (-1.0), each
// `order_marker` -> 0. Paired with record_peer_horizon (which advances horizon monotonically once
// the session is under way); single caller llm_strat_session_begin_multi (MP session start only --
// an SP run never reaches this function).
//
// ZERO OUTWARD CALLS -- only the inert `PUSH 0x20 / CALL utils_assert_stack_capacity` prologue probe
// (@0x0049feff), so this unit carries no `_calls` struct at all, same shape as
// sim/resid/sim_clock_resync.{h,cpp}.
//
// ---- THE RECORD LAYOUT: THE PLATE AND THE EXPORTED .c ARE STALE; THE GENERATED LAYER IS RIGHT -----
// The Ghidra plate on this function (and the exported .c's field access) still describe the table as
// based at 0xe58c44 with the DOUBLE as the first field. The assembly proves otherwise: for iteration
// i it stores `[i*0xc + 0xe58c44] = 0` (0x0049ff2a), `[i*0xc + 0xe58c48] = 0xbff00000` (0x0049ff34),
// then `[i*0xc + 0xe58c40] = 0` (0x0049ff42). Under the plate's model that third store lands 4 bytes
// BEFORE entry i (out of bounds for i=0) -- so the real base is 0x00e58c40 with the record
// `{int32_t order_marker; double horizon;}`. This is not a new finding: mh_addrs.gen.h:290 already
// carries the "BASE CORRECTED 2026-07-28" comment, and `mh::game::mh_llm_net_lockstep_peer_timing`
// (mh_structs.gen.h) is already laid out this way (order_marker @0x0, horizon @0x4) -- so
// `engine_state::peer` (declared in turn_engine.h) is ALREADY the corrected type, and this
// translation uses it as-is. The Ghidra label/type and the function's plate are still wrong; fixing
// those is conductor/LT0 work (context_D.md Ghidra gap 1), not something this translation works
// around with an offset.
//
// ---- THE SENTINEL IS A NUMBER, NOT A BIT PATTERN WE HAPPEN TO REPRODUCE -----------------------------
// The two dword stores (low=0, high=0xbff00000) are exactly the IEEE-754 bits of -1.0, and
// turn_engine.h already names this value `HORIZON_NONE` (citing this very function alongside
// reset_player_horizon @0x0049e2ad). Assigning `HORIZON_NONE` to the `double` field reproduces the
// bit pattern by construction -- the original writes it as two integer dwords rather than an x87
// store (there is no FLD/FST anywhere in this body; compare the `touches_floats` note in the report).
//
#pragma once
#include <cstdint>

#include "lockstep/turn_engine.h"

namespace mh::lockstep {
namespace detail {

// llm_net_lockstep_peer_timing_reset @0x0049fef7. Loop bound is MAX_PLAYERS (`CMP [i],0x8`
// @0x0049ff16); order_marker is 0 for all 8 entries.
void peer_timing_reset(const engine_state &st);

} // namespace detail

// Production wrapper: the logic applied to state(). Matches the original's committed
// `void __watcall llm_net_lockstep_peer_timing_reset(void)` signature.
void peer_timing_reset();

namespace detail {
} // namespace detail

} // namespace mh::lockstep
