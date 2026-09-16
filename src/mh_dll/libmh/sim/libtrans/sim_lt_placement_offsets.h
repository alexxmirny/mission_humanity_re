#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_ui_cursor_apply_anim_frame_offset @0x00486a8a, renamed llm_strat_facing_step_apply.
//
// `facing` indexes _G_LLM_STRAT_FACING_STEP_SIGN (sim_view::facing_step_offset, stride 8: {int32
// dx; int32 dy;}), boot-populated for indices 1..24 (index 0 is allocated but never written by
// llm_strat_unit_facing_offset_init, so facing==0 is a no-op by construction -- do not add a guard
// the original lacks). NO BOUNDS CHECK on `facing` either way -- reproduce the absence.
//
// The dx block runs to completion BEFORE the dy block is even loaded (two independent, sequential
// blocks in the original -- the two out-pointers are distinct at all three live call sites, but
// the order is the contract, not incidental):
//   dx == -1  ->  *out_fine_x -= 0x20   (one full tile in fine coords; BYTE-width, wraps mod 256)
//   dx == +1  ->  *out_fine_x += 0x20   (BYTE-width, wraps mod 256)
//   otherwise ->  *out_fine_x untouched -- this is an EXACT-EQUALITY test against {-1, +1}, not a
//                 sign test (asm: CMP v,-1;JL skip / CMP v,-1;JLE sub / CMP v,1;JZ add / else skip)
// then the identical three-way test for dy against *out_fine_y.
void facing_step_apply(const sim_view &v, char *out_fine_x, char *out_fine_y, int32_t facing);

// llm_ui_cursor_lookup_offset_pair @0x00486b17, renamed llm_strat_squad_placement_offset_lookup
// (todo:rename already carried in the manifest).
//
// Looks up one cell of the squad-placement offset table (sim_view::squad_placement_offset_table,
// byte[288] = llm_squad_placement_offset[6][6] @0xae3618, row stride 0x30, col stride 8) at
// row = `slot`, col = `soldier_count`, and copies its two BYTE fields (cell offset +0 -> *out_a,
// cell offset +4 -> *out_b). Only the LOW BYTE of each underlying 4-byte cell field is read (the
// asm is a plain `MOV DL, byte ptr [...]`) -- the upper 3 bytes are discarded, never read-and-
// truncated. Callers pass soldier_count = Unit[proto].soldier_count and slot = 1..soldier_count;
// the table is 6x6 and NEITHER index is bounds-checked here, matching the original exactly.
void squad_placement_offset_lookup(const sim_view &v, int32_t soldier_count, int32_t slot, char *out_a,
                                   char *out_b);

} // namespace detail

// Live wrappers: the logic applied to state().read. Match each original's committed __watcall
// register order (EAX, EDX, EBX[, ECX]).
void facing_step_apply(char *out_fine_x, char *out_fine_y, int32_t facing);
void squad_placement_offset_lookup(int32_t soldier_count, int32_t slot, char *out_a, char *out_b);

} // namespace mh::sim
