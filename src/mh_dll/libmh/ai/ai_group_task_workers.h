#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_unit_group_assign_by_type @0x004d585e.
//
// Enqueues an exit-storage order for `unit_id` (storage_slot passed straight through as the
// callee's storage_idx, with two trailing zero args -- 0x004d5879/0x004d587e/0x004d5866), then
// classifies the unit via the four unit_is_ai_* predicates, checked in this exact order with
// short-circuit semantics matching the assembly's branch shape (IsAiGround, then only if that was
// zero IsAiSoldier -- both land on type code 2; then IsAiPlane -> 4; then IsAiHeli -> 3), and moves
// it into player_data[player].ai_groups[type_code] via group_member_move(player,
// /*src*/group_index, /*dst*/type_code, unit_id). If NONE of the four classifiers match, the
// function returns without calling group_member_move at all (0x004d58cb JZ straight to the shared
// epilogue) -- no move, and the already-issued exit-storage order stands. Reproduced, not "fixed"
// into an always-moves shape.
void unit_group_assign_by_type(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, int32_t group_index, int32_t unit_id,
                               int32_t storage_slot);

// llm_strat_ai_group_rally_formup_worker @0x004d5dfc.
//
// Computes the group's centroid (group_compute_centroid) and stores it as the group's own rally
// anchor, THEN walks the intrusive member chain (head_unit, then each member's own ai_group_next)
// moving every member toward a spiral-offset tile around that anchor.
//
// THE STACK-OFFSET LABELS IN THE EXPORTED .c ARE WRONG AND DISAGREE WITH THE OFFSETS THE ASSEMBLY
// ACTUALLY READS/WRITES (the .c calls the loop counter's slot `local_18` while the assembly
// initialises [EBP-0x14]; it calls the centroid outputs `local_1c`/`local_20` while the assembly's
// LEA instructions before the CALL take the addresses of [EBP-0x18] and [EBP-0x1c] -- Ghidra's own
// stack-slot renaming is internally inconsistent here, not merely absent). The assignment used below
// is read off the raw addresses and cross-checked against the struct's own static-asserted field
// offsets, not trusted from either the .c or from register-argument-position guesswork alone:
//   - group_compute_centroid's calling convention is EAX=player, EDX=group_index, EBX=out_x (3rd
//     positional arg), ECX=out_y (4th) -- so out_x is &[EBP-0x18], out_y is &[EBP-0x1c] (LEA ECX
//     comes first in the instruction stream but loads the LOWER-numbered slot; instruction order is
//     not argument order).
//   - the two post-call stores write [ESI+0xe7e457] from [EBP-0x18] (out_x/centroid_x) THEN
//     [ESI+0xe7e45b] from [EBP-0x1c] (out_y/centroid_y), where ESI is the flat
//     player*0x288fc+group_index*0xa66 group-record offset (independently verified: the six-
//     instruction shift/add sequence at 0x004d5e1f-0x004d5e35 computes player_id*0x288fc exactly,
//     the game_player_data per-player stride).
//   - mh_structs.gen.h static_asserts active_param_a at group-record offset 0x2f and active_param_b
//     at 0x33; group-record base 0 is player_data+0x10568 (0xe7e432 - 0xa, since head_unit is
//     static_assert'd at +0xa and read at absolute 0xe7e432 two instructions earlier in this same
//     function) -- so 0xe7e457 IS active_param_a and 0xe7e45b IS active_param_b, exactly.
// So: active_param_a = centroid_x (the group_compute_centroid out_x result), active_param_b =
// centroid_y. This also matches the field comments already committed on both
// (mh_llm_strat_ai_unit_group::active_param_a "anchor X" / active_param_b "anchor Y") and the later
// per-member use, which masks the dx-summed value with map_width_mask and the dy-summed value with
// map_height_mask -- consistent with a=X, b=Y, not the reverse.
//
// Per member (spiral index starting at 0, incrementing once per member visited): tile =
// ((spiral_offsets[i].dx + centroid_x) & *map_width_mask, (spiral_offsets[i].dy + centroid_y) &
// *map_height_mask); unit_flag_and_move(player, member, tile.x, tile.y). No cap on the spiral index
// against SITE_SCAN_SPIRAL_RADIUS or any other bound -- the original walks it once per member with no
// range check, reproduced as-is.
void group_rally_formup_worker(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index);

// llm_strat_ai_group_scatter_random_worker @0x004d5f4c.
//
// Same centroid + member-chain-walk shape as group_rally_formup_worker (and the SAME out_x/out_y
// register assignment resolved the same way: group_compute_centroid's out_x is &[EBP-0x1c], out_y is
// &[EBP-0x20], confirmed by the later load order at 0x004d5fb0/0x004d5fb3 -- EDX gets [EBP-0x20] and
// EAX gets [EBP-0x1c] immediately before the random_point_near call, and random_point_near's own
// convention is EAX=center_x, EDX=center_y per N1 above, so [EBP-0x1c]=centroid_x,
// [EBP-0x20]=centroid_y), but does not move every member unconditionally: per member, first checks
// unit_is_order_pending(player, member); if that is NONZERO the member is skipped entirely (no
// random_point_near call, no move) and the walk continues to the member's own ai_group_next. Only for
// a member with no pending order does it call random_point_near(centroid_x, centroid_y,
// class_or_radius, &x, &y) and then unit_flag_and_move(player, member, x, y). `class_or_radius` (the
// function's own third parameter, EBX) is passed through to random_point_near completely unchanged --
// this file does not interpret it.
void group_scatter_random_worker(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 int32_t player_id, int32_t group_index,
                                 uint32_t class_or_radius);

// llm_strat_ai_random_point_near @0x004d5eaf (RI-AI batch C, 2026-08-07). N1's body -- the ai_calls
// slot itself was wired 2026-08-06 (layer 4); this is the translation that lets shadow_calls()
// diverge from a real implementation instead of always routing to the original.
//
// angle = rand_state_advance(2) * ANGLE_SCALE, where ANGLE_SCALE is the literal double at
// DAT_00504813 (read via ReVA: bit pattern 0x401921FB544486E0) -- close to but NOT bit-identical to
// the IEEE-754 double 2*pi (0x401921FB54442D18 differs in the low mantissa bits), so it is reproduced
// via its exact bit pattern, never recomputed from a math constant. Then:
//   dx = (int32_t)floor(sin(angle) * (double)(uint32_t)radius)   -- sin via math_fsin_reduce_loop
//   dy = (int32_t)floor(cos(angle) * (double)(uint32_t)radius)   -- cos via math_cos_impl
//   *out_x = (x + dx) & *map_width_mask
//   *out_y = (y + dy) & *map_height_mask
// `radius` is ZERO-EXTENDED to 64 bits before the int-to-double conversion (the original builds the
// FILD operand by storing the 32-bit value into the low dword and a literal 0 into the high dword,
// not by sign-extending it), so a negative `radius` argument converts as a huge positive value --
// reproduced via `(uint32_t)radius`, not `(int32_t)radius`. Ghidra's own decompile loses the
// sin(angle) value into an `extraout_ST1` placeholder (both fsin_reduce_loop and cos_impl take their
// argument on ST0 and return on ST0, and the decompiler cannot track the FPU stack far enough to see
// that the sin result is still sitting one slot down when cos_impl's result is popped) -- re-derived
// from the raw x87 sequence in the .asm instead of trusted from the .c.
void random_point_near(const ai_view &v, const ai_calls &gc, int32_t x, int32_t y, int32_t radius,
                       int32_t *out_x, int32_t *out_y);

} // namespace detail

void unit_group_assign_by_type(uint32_t player, int32_t group_index, int32_t unit_id,
                               int32_t storage_slot);
void group_rally_formup_worker(int32_t player_id, int32_t group_index);
void group_scatter_random_worker(int32_t player_id, int32_t group_index, uint32_t class_or_radius);
void random_point_near(int32_t x, int32_t y, int32_t radius, int32_t *out_x, int32_t *out_y);

} // namespace mh::ai
