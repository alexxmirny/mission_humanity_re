//
// sim/sim_unit_update_rotation.h -- the per-tick facing/turn-rate stepper (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_update_rotation @0x0047d741 (0x60b bytes), void(void) -- it takes NO
// parameters and operates entirely on three AMBIENT globals the outer per-unit dispatch loop sets
// before calling it: _G_LLM_STRAT_CUR_UNIT (a `unit *`), _G_LLM_STRAT_CUR_PLAYER, _G_LLM_STRAT_CUR_INDEX
// (both uint16_t). See the .cpp's top-of-file note for why this is a DECLARED NEED and how it is
// modelled.
//
// SHAPE: (1) a game-clock/rotation_clock budget gate -- if there is no rotation budget this tick,
// return; if the unit's turn_speed exceeds the budget, spend it all and return. (2) a ~24-arm
// state/order switch (see the .cpp) that computes a TARGET heading (via llm_strat_dir_from_to against
// the unit's target/target2 fine position, the cruise-path facing table, or facing_target itself, per
// state). (3) a stepping loop that advances facing_current toward the target heading one of 24
// compass steps at a time, taking the SHORTER rotational direction, budget-gated by turn_speed,
// syncing facing_target for non-independent units, and propagating the new heading to mounted
// soldiers via the ORIGINAL llm_strat_unit_soldiers_set_heading.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for offline testability, same reasoning as every other sim `calls` table (see
// sim_unit_passive_engage.h). update_target_tracking/update_target2_tracking are the sibling
// functions the SIM1A slice also translates (unit_target_tracking TU) -- called here
// through the ORIGINAL via mh::call:: regardless, per the task brief's "same per-function-shadow
// reasoning as sim_unit_tick.cpp": a second C++ body for the same address would fork one function's
// behaviour across two modules (Law 4).
struct unit_update_rotation_calls {
    // llm_strat_unit_update_target_tracking @0x00448ee1 -- return value discarded by every call site
    // in this function (matches the original: EAX is never read after the CALL).
    int32_t (*update_target_tracking)(uint32_t player, int32_t unit_idx);
    // llm_strat_unit_update_target2_tracking @0x00449062 -- return value discarded, same as above.
    int32_t (*update_target2_tracking)(uint32_t player, int32_t unit_idx);
    // llm_strat_unit_get_coords @0x0044b141. Out-params are `int32_t *` to match the committed
    // mh::call:: signature exactly (TACT1-P C6, 2026-09-04; was `void *` -- sim_order_enqueue.h's
    // `calls` table documents the same current shape).
    void (*get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
    // llm_strat_dir_from_to @0x0049482b -- pure query, no state write.
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    // llm_strat_unit_soldiers_set_heading @0x00489ab6 -- THE roster_write_via callee: it writes the
    // mounted soldiers' own heading/sprite state, which this function itself never touches directly.
    void (*soldiers_set_heading)(uint16_t player, int32_t unit_index, uint8_t sprite_frame);
};

const unit_update_rotation_calls &live_unit_update_rotation_calls();

namespace detail {

// llm_strat_unit_update_rotation @0x0047d741. See the .cpp for the full derivation; this comment
// carries only the shape, not the address-by-address proof (which lives in the .cpp alongside the
// code it justifies).
//
// GATE (0x0047d759-0x0047d7d4): budget = GAME_CLOCK - cur_unit.rotation_clock. If budget<=0.0 (or the
// game clock has not advanced), return -- nothing to spend. Otherwise cur_unit.rotation_clock is
// immediately set to GAME_CLOCK (so a later "rotation_clock -= budget" in either gate 2 or the loop's
// exhausted-budget arm nets back to the ORIGINAL rotation_clock, not a fresh one -- exact original
// arithmetic, not simplified away). If budget < cfg Unit[proto].turn_speed[player] (the WHOLE budget
// this tick is insufficient for even one step), spend it (rotation_clock -= budget) and return before
// computing any target heading at all.
//
// STATE/ORDER GATE (0x0047d7d4-0x0047d86b): if the unit is not independent AND its state is none of
// {ATTACK_UNIT, ATTACK_UNIT_RETURN, ATTACK_BUILDING, HOVER_ENGAGE, GROUP_MARSHAL, STOP_TO_DEFAULT,
// HOVER_DISENGAGE, HOVER_ENGAGE_2F}, return (no rotation applies to this state at all for a
// non-independent unit). Then, independently, if state==PARKED, return.
//
// TARGET HEADING (0x0047d86b-0x0047dc37): a nested state/order nested-if tree (reproduced literally,
// not collapsed to a switch, per the task's hazard note); every leaf either computes a heading via
// llm_strat_dir_from_to(own fine position, target/target2 fine position) -- optionally preceded by an
// update_target(2)_tracking call to refresh that target's fine position first -- reads the cruise-path
// facing table _G_LLM_STRAT_MOVE_MICROSTEPS[move_heading][move_microstep].facing, or falls through to
// the unconditional default `facing_target`. Two states (STOP_TO_DEFAULT, GROUP_MARSHAL) share one
// tail that can ALSO return early (with nothing done) if target2_ref==0.
//
// STEP LOOP (0x0047dc37-0x0047dd47): while facing_current != target_heading and budget>0.0 (NaN-safe
// idiom, see .cpp): if budget is short of turn_speed, spend it all (rotation_clock -= budget;
// budget=0.0) and stop; otherwise spend one turn_speed's worth, pick the SHORTER of the two rotational
// directions via the literal wraparound-threshold arithmetic (not a mod-24 simplification -- task
// hazard note), step facing_current by one of 24 compass positions with wraparound at 1/24, write it
// back, sync facing_target too UNLESS the unit is independent (asymmetry preserved), and propagate the
// new heading to mounted soldiers via the ORIGINAL llm_strat_unit_soldiers_set_heading iff
// Unit[[roster].unit_proto_id].soldier_count > 0 (roster-indexed, not cur_unit-indexed -- see .cpp).
void unit_update_rotation(const sim_view &v, sim_store &own, const unit_update_rotation_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_update_rotation_calls(). Matches the
// original's committed __watcall void(void) shape (sig_llm_strat_unit_update_rotation).
void unit_update_rotation();

namespace detail {
} // namespace detail

} // namespace mh::sim
