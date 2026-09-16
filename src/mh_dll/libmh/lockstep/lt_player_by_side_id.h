//
// lockstep/lt_player_by_side_id.h -- llm_strat_player_by_side_id @0x0049e388 (lib_trans batch D / d8,
// tmp/prep_lib_trans/context_D.md unit `d8_player_by_side_id`).
//
// Map a side_id (the wire/protocol identifier carried in lockstep messages) to a PLAYER INDEX: a
// linear scan of _G_LLM_STRAT_PLAYERS[0..MAX_PLAYERS) for `.side_id == side_id`. Returns the index,
// or -1 if no slot carries that side_id. NOTE the two id spaces are distinct and freely mixed at call
// sites: side_id is the wire identifier, the return is an index into _G_LLM_STRAT_PLAYERS; a -1
// return is NOT checked by several net callers (see the plate on the original).
//
// THIS UNIT NEEDS NO NEW BINDINGS. `engine_state` (turn_engine.h) already carries
// `const player_profile *players` (RID_STRAT_PLAYERS) and `MAX_PLAYERS == 8`; both are read here and
// nothing else. There is no `<tu>_calls` struct: the assembly makes zero outward calls (only the
// inert `assert_stack_capacity` prologue probe, dropped per the reimpl-loop "settled once" rule --
// it touches zero tracked state regions), so there is nothing for a calls table to hold.
//
// ---- 98-byte assembly, five things a paraphrase would lose ----------------------------------------
//   1. ZERO WRITES. The field read is `MOV EAX,[EAX*0x740 + 0xcff79c]` at 0x0049e3c1 -- an int32_t
//      dword load, plain `CMP`/`JNZ` against the int32_t EAX parameter. 0xcff79c - 0xcff060 (the
//      _G_LLM_STRAT_PLAYERS base) is 0x73c, and `offsetof(mh_llm_strat_player_profile, side_id) ==
//      0x73c` is static_asserted in mh_structs.gen.h -- no widening, no masking, no derived offset.
//   2. NO ACTIVE/ALIVE FILTER. Every slot 0..7 is tested regardless of `status_flags`. An unused slot
//      whose `side_id` happens to match IS returned. Do not add a plausible-looking liveness guard.
//   3. FIRST MATCH WINS. The loop returns the instant it finds one (0x0049e3cc-0x0049e3d2); -1 is
//      produced only after all 8 slots have been tried and failed (0x0049e3d6).
//   4. `MOV EAX,[EBP-0x1c]` at 0x0049e3b2, immediately before the `INC` at the loop-increment label,
//      is a dead load Watcom emits there -- it writes nothing live and the decompiler correctly drops
//      it. Not reproduced; noted so a reviewer does not read the omission as a missing statement.
//   5. Callers do not uniformly check the -1 sentinel (per the original plate) -- that is the nine
//      callers' contract, not licence to change what this function returns.
//
// C8-d FOLLOW-UP (conductor work, not this file's): `lockstep/resync.cpp`'s live_resync_calls() and
// every one of the other 8 call sites listed at turn_engine.h's `player_by_side_id` members currently
// bind `mh::call::llm_strat_player_by_side_id` as an OUTWARD call. Once this body is armed and
// promoted, those edges should become `MH_INTERNAL_CALL` to `mh::lockstep::player_by_side_id`, exactly
// as resync.cpp already does for `lockstep_broadcast_resync_state` / `send_lockstep_extend`.
//
// MP-shaped row: 9 callers, all lockstep message handlers (tx_emit.cpp, tx_emit_ctrl.cpp,
// rx_dispatch.cpp, timekeeper.cpp, resync.cpp). A single-player run reaches it zero times; a 2-peer
// run enters it many times per exchange -- the cheapest row in batch D to get rig evidence for.
//
#pragma once

#include <cstdint>

#include "lockstep/turn_engine.h" // engine_state (players, MAX_PLAYERS) -- no new bindings needed

namespace mh::lockstep {

namespace detail {

// llm_strat_player_by_side_id @0x0049e388. Pure over `st.players`; see the banner above for the five
// behavioural properties (full unconditional scan, no liveness filter, first match wins, -1 sentinel).
int32_t player_by_side_id(const engine_state &st, int32_t side_id);

} // namespace detail

// ---- production entry point (bound to the live game state) ----------------------------------------
int32_t player_by_side_id(int32_t side_id);

namespace detail {

// The shadow-differential-oracle install for this site. Declared in `detail` per this unit's brief;
// the CONDUCTOR declares the matching entry in turn_engine.h and calls it from install_shadow().

} // namespace detail

} // namespace mh::lockstep
