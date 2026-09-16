//
// sim/sim_group_scratch_centroid.h -- the strategic unit-group order state machine's
// toroidal-wrapped centroid over the GROUP-MOVE MEMBER SCRATCH ARRAY.
//
// One function: llm_strat_group_scratch_compute_centroid @0x0048d36f (0x1ce bytes).
//
// WHY THIS LIVES IN sim/ AND NOT ai/ (moved 2026-08-07). It was translated under RI-AI item AI1E as
// `mh::ai::group_scratch_compute_centroid`, on the strength of its then-name
// llm_strat_ai_group_scratch_compute_centroid. That name was wrong. The whole group-move family has
// ZERO AI callers: this function's sole caller is llm_strat_unit_group_step_ground, i.e. unit state
// 0x0b GROUP_STEP in the GENERIC strategic state machine that every player's units are ticked
// through -- the AI reaches it exactly the way a human player does, by setting a unit's order.
// Nothing about the translation changed in the move; it is the same body, still armed, still T1.
//
// NOT a variant of the AI group-struct centroid (mh::ai::group_compute_centroid,
// ai/ai_group_centroid.h, @0x004d5c17). The two read entirely different group representations and
// disagree on the wrap arithmetic (this one uses true `%` modulo; the other ANDs with a
// power-of-two mask). Do not unify them.
//
// STATE ACCESS. Was a hand-rolled 4-member `group_view` bound straight off the registry, on the
// argument that "a 4-member local view beats exporting ai_view into a subsystem that is not the AI".
// That argument was right about ai_view and wrong as a pattern: it made this file its own state
// interface, and 306 more sim functions doing the same would be 306 of them. SIM0 built the real one
// (sim/sim_state.h), so this file now reads through `sim_view` like every other sim TU. The body did
// not change -- only where the four pointers come from.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The logic over an EXPLICIT state, so `net_selftest.exe simtest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state().read; the split costs one inlined
// call.
namespace detail {

// llm_strat_group_scratch_compute_centroid @0x0048d36f.
//
// Anchors on group member 0 (v.scratch[0].unit_idx, NOT a group struct's head_unit -- this function
// has no concept of a group record at all, only the flat scratch array its caller has already
// populated via llm_strat_group_move_register_member) and accumulates each subsequent member's
// (dx, dy) relative to that anchor, wrapping each axis independently into a signed half-extent band
// via a two-sided compare-and-adjust (the SAME shape as ai_group_centroid.h's group_compute_centroid,
// not a modulo mid-loop). The loop runs unconditionally for member indices [1, member_count) -- no
// idle/parked skip here (unlike the sibling), because this array holds only whatever members the
// caller decided to include.
//
// UNLIKE THE SIBLING (0x004d5c17), THERE IS NO EMPTY-GROUP SPECIAL CASE. Even when member_count is 0
// or 1, member 0's row is still read and its (x, y) becomes the (only) input to the final wrap -- the
// function never returns early and never substitutes a home-tile fallback. Preserve this exactly: a
// reimplementation that special-cases member_count <= 1 changes behaviour whenever row 0 of the array
// happens to be stale or unpopulated on a 0/1-member call.
//
// Final step, per axis: divide the summed delta by (member_count - 1) -- SIGNED IDIV, only when
// member_count > 1, skipped (raw sum used as-is) otherwise -- then wrap (anchor + sum) into
// [0, extent) via TRUE MODULO: (anchor + sum + extent) % extent, where C++'s `%` on int32_t matches
// the original's IDIV remainder exactly, sign included. This is a DIFFERENT final step from the
// sibling, which ANDs with a power-of-two wrap mask instead -- do not unify the two.
void group_scratch_compute_centroid(const sim_view &v, int32_t player, int32_t member_count,
                                    int32_t *out_x, int32_t *out_y);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shape
// (sig_llm_strat_group_scratch_compute_centroid): the original returns via two out-pointers.
void group_scratch_compute_centroid(int32_t player, int32_t member_count, int32_t *out_x, int32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
