//
// ai/ai_group_muster_pick.h -- two unrelated AI-group MEMBER-PICKING helpers (RI-AI / AI1C), sharing
// this translation unit only because both walk the same intrusive member chain
// (unit_group::head_unit -> unit::ai_group_next, the same walk ai_group_task_predicates.cpp's four
// functions use) and both pick one "best" member by a scalar score. Neither writes anything and
// neither calls another game function -- see the header banner on each for what they DO call
// (nothing marshallable; both inline utils_math_trunc's/the FPU compare's own instructions, per the
// note below).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so net_selftest.exe aitest can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state(); the split costs one inlined
// call.
namespace detail {

// THE DECLARED NEED THIS FILE USED TO CARRY IS MET (2026-08-06). `mh_map_object_unit::weapons` was
// emitted as `uint8_t weapons[76]` with the real shape only in a comment, so this header reached
// through it with `u.weapons[slot * 19]`. The Ghidra type was never the problem -- the field has
// been `map_unit_weapon[4]` there all along; `map_unit_weapon` simply was not in the DLL's struct
// manifest, and gen_dll_structs.py emits raw bytes for an element type it was not asked to export.
// Adding it to tools/data/dll_addr_manifest.json was the whole fix. Call sites now read
// `u.weapons[slot].weapon_id`, and the offsets the accessor was transcribing (slot stride 0x13,
// weapon_id at +0) are static_asserts in mh_structs.gen.h instead of arithmetic in a comment.

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- an x87-register-only leaf with no stack-passable signature) inlined verbatim,
// the same asm-derived shape as ai_mine_yield.cpp's x87_scale_and_trunc: round `running_total +
// *addend` toward zero via a temporary FPU control-word swap (RC=11 truncate), matching
// 0x004d6d0b-0x004d6d28's FILD/FADD/CALL-trunc/FISTP sequence. `running_total` is read back
// zero-extended to 64 bits before the FILD (the original stores an explicit 0 high dword at
// 0x004d6d0e) and only the low 32 bits of the result are kept (0x004d6d28 `MOV EBX,[ESP]`).
int32_t weapon_power_add_and_trunc(uint32_t running_total, const double *addend);

// llm_strat_ai_group_find_slowest_unit @0x004d6be3 (0xa2 bytes).
//
// Walks `player_id`'s ai_groups[group_index] member chain and returns the roster index of the member
// with the LOWEST cfg Unit.step_speed[player_id] (the group's pace-setter for movement). An empty
// group (head_unit == 0) returns 0, matching the loop's untouched initial value -- not a valid unit
// index, but that is what the original returns.
//
// THE RUNNING MINIMUM IS STORED AS A 32-BIT FLOAT, NOT A DOUBLE, even though cfg Unit.step_speed is a
// double column: the original's local (`local_10`) is loaded with `FLD float ptr`, compared against
// the double column at full (double) precision, and on an update is narrowed back to float with
// `FSTP float ptr`. So every update TRUNCATES the cfg value to float32 precision before the next
// comparison -- reproduce with a `float` accumulator (not `double`), not as a stylistic choice.
uint32_t group_find_slowest_unit(const ai_view &v, int32_t player_id, int32_t group_index);

// llm_strat_ai_group_pick_best_weapon_unit @0x004d6c85 (0xe2 bytes).
//
// Walks the same member chain and, per member, sums cfg Weapon.power[player_id] (TRUNCATED TOWARD
// ZERO after each addition, not once at the end -- see the x87 helper below) over that member's four
// weapon slots (unit::weapons[76], an UNTYPED raw array -- see the DECLARED NEED in the .cpp) whose
// mounted weapon is nonzero AND ground-capable (cfg Weapon.target & 1). Returns the member with the
// HIGHEST such total.
//
// THE COMPARISON IS UNSIGNED `>=`, NOT `>`: on a tie (including a fresh member's score-0 total tying
// the initial best_total of 0) the LATER member wins, and in particular the group's very FIRST member
// is always installed as the initial "best" candidate even when its own total is 0 -- reproduce the
// `>=`, do not narrow it to `>`.
uint32_t group_pick_best_weapon_unit(const ai_view &v, int32_t player_id, int32_t group_index);

} // namespace detail

uint32_t group_find_slowest_unit(int32_t player_id, int32_t group_index);
uint32_t group_pick_best_weapon_unit(int32_t player_id, int32_t group_index);


} // namespace mh::ai
