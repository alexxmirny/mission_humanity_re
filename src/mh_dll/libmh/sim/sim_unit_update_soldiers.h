//
// sim/sim_unit_update_soldiers.h -- the per-tick soldier-squad idle-animation/walk-interpolation
// update (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_update_soldiers @0x0047e612 (0x450 bytes), `void __watcall` with NO
// parameters -- everything it touches is reached through two AMBIENT per-tick globals a driver outside
// this batch sets before calling in: _G_LLM_STRAT_CUR_UNIT (the unit whose squad is being ticked) and
// _G_LLM_STRAT_CUR_PLAYER (that unit's owning player).
//
// THIS TU DOES NOT COMPILE STANDALONE -- see the .cpp's DECLARED NEED block. Four things it depends on
// are not yet wired into sim_state.h / addr/mh_regions.gen.h / addr/mh_structs.gen.h: the CUR_UNIT and
// CUR_PLAYER globals, _G_LLM_STRAT_TICK_BUDGET, and a typed store accessor for the soldier roster (the
// region itself, RID_STRAT_SOLDIERS, already exists -- only its element type and an accessor are
// missing). Same situation sim_unit_passive_engage.cpp documents for its one missing accessor; this
// file just has four gaps instead of one.
//
// GUARD (0x0047e63d-0x0047e645): the WHOLE function is skipped -- no RNG draw, no write, no visit --
// when CUR_UNIT->energy is ORDERED <= 0.0. A NaN energy does NOT skip (unordered sets the same flag
// bit an ordered "greater" result does); see the .cpp for the FCOMP/JNC derivation, the same idiom
// sim_unit_passive_engage_tick's own energy guard already documents.
//
// WITH ENERGY>0 (or NaN): draws llm_rand_below(100) UNCONDITIONALLY (channel-2/AI RNG -- see the
// SHADOW HAZARD note in the .cpp), then -- only when CUR_UNIT->state==STOP_TO_DEFAULT(0x1) AND that
// roll==0 (a ~1% chance per tick) -- draws a SECOND roll in [0, Unit[CUR_UNIT->unit_proto_id].
// soldier_count) to pick one soldier SLOT (a 0-based position in the walk below, not a roster id), and
// unconditionally sets idle_wander_flag=1 on the squad's HEAD soldier
// (_G_LLM_STRAT_SOLDIERS[CUR_PLAYER][CUR_UNIT->unit_above]) -- the head, not the picked slot.
//
// Then walks the squad's linked list starting at that same head index, chained via next_soldier,
// terminated by next_soldier==0 -- a DO-WHILE, so a squad with unit_above==0 (no soldiers recorded)
// still visits _G_LLM_STRAT_SOLDIERS[player][0] once. Record 0 is documented (docs/structs.md) as the
// roster's own "used-slot count" metadata slot, not a real soldier; this function's writes never touch
// its owner_unit/next_soldier fields (offsets 0/2), so nothing tracked there is corrupted, but the
// visit itself is real original control flow and is reproduced, not guarded away.
//
// PER VISITED SOLDIER (0x0047e6b5 loop body):
//   walk_duration == 0.0 (idle): if this is the ONE soldier at the picked slot (0-based walk position,
//     compared against a running visit counter) -- draws a THIRD roll (channel-2, [0,3)) to nudge
//     sprite_frame by one of {-3,0,3}, clamps/wraps the result into [1,0x15], and bumps
//     anim_change_count. Every OTHER idle soldier in the walk does nothing this tick.
//   walk_duration != 0.0 (mid-walk): advances walk_elapsed by _G_LLM_STRAT_TICK_BUDGET, then either
//     interpolates cur_x/cur_y toward end_x/y via the ORIGINAL utils_math_trunc (see the .cpp --
//     unmarshallable, reproduced as its own instruction sequence, never std::trunc) once
//     elapsed<duration, or finalizes the walk (start=end, cur=start, walk_duration=0.0) once
//     elapsed>=duration.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
//
// utils_math_trunc is deliberately NOT a member here -- it is
// `MH_UNAVAILABLE__parameter_storage_not_marshallable` in mh_calls.gen.h (an x87-register-only leaf,
// ST0 in / ST0 out, no stack-passable signature) and is reproduced inline in the .cpp instead, matching
// ai_opponent_relations.cpp's trunc_float_to_int32 precedent for an ORDINARY (non-compiler-inlined)
// `CALL utils_math_trunc` -- which is exactly what this function's two call sites are.
struct unit_update_soldiers_calls {
    int32_t (*rand_below)(int32_t upper_bound); // llm_rand_below @0x00499f49 -- channel-2/AI RNG
};

const unit_update_soldiers_calls &live_unit_update_soldiers_calls();

namespace detail {

// llm_strat_unit_update_soldiers @0x0047e612. See the header banner above and the .cpp for the full
// per-instruction derivation.
void unit_update_soldiers(const sim_view &v, sim_store &own, const unit_update_soldiers_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_update_soldiers_calls(). Matches the
// original's committed __watcall (no-arg) shape.
void unit_update_soldiers();

namespace detail {
} // namespace detail

} // namespace mh::sim
