//
// ai/ai_group_move_helpers.h -- two small AI movement-support predicates/pickers, both leaves with
// no callees, RI-AI batch B/C (2026-08-07). Scheduled together because neither belongs with the
// other's callers -- pick_owned_tile_or_home is used by group-relocation/expansion callers already
// spread across several files (ai_group_expansion.cpp, ai_group_relocation.cpp,
// ai_group_task_lifecycle.cpp, ai_army_milestone.cpp), and unit_is_order_pending likewise
// (ai_group_relocation.cpp, ai_group_task_movement.cpp, ai_group_task_workers.cpp and others), so
// there is no single existing file either belongs beside.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_pick_owned_tile_or_home @0x004e7138.
//
// Picks a uniformly-random tile the player "owns" on their influence grid (ai_tile_flags_grid[x][y]
// == 1, exactly -- a literal byte-equality test, not a bit test; the grid also encodes other flag
// values via grid_stamp_seeds, but this function only ever matches the byte value 1, which the AI's
// direct-ownership stamping writes, per the disassembly not per any inference from the seed family).
//
// Two passes over the WHOLE map (width x height): the first counts owned tiles and draws
// `llm_rand_below_ai(count)` to pick a target rank; the second re-walks in the SAME (x outer, y
// inner) order and returns the rank-th owned tile the moment the countdown reaches 0. If the player
// owns NO tile (count == 0, so rand_below_ai(0) returns 0 and the second pass's `iVar1 == 0` check
// never happens because the count itself is 0 -- the second walk's OUTER bound `width <= uVar2` is
// hit immediately on x=0 for width==0, but for width>0 with zero owned tiles the inner walk simply
// never matches and the outer loop runs the full width with no return), the function falls through
// to the player's own home tile (player_data::ai_home_tile_x/y) instead.
void pick_owned_tile_or_home(const ai_view &v, const ai_calls &gc, int32_t player, uint32_t *out_x,
                             uint32_t *out_y);

// llm_strat_ai_unit_is_order_pending @0x004d41ff.
//
// Tests bit 0x80 of unit::order_status_flags (the high byte of the ushort straddling
// engagement_flags/order_status_flags at record offset 0xe2, read as one 16-bit load in the
// original) and returns it left-shifted back into bit 0x8000, i.e. either 0 or 0x8000 -- reproduced
// exactly rather than normalised to 0/1, per the translator brief's "don't improve" rule (every real
// caller only tests the result against 0). NOTE: the struct's own field comment describes bit 0x80
// of order_status_flags as mirroring the engagement-COMMITTED flag, not "order pending" -- this
// function's name and this reading are the disassembly's own, and the tension between the two is
// left as a standing observation rather than resolved by renaming either side.
uint32_t unit_is_order_pending(const ai_view &v, uint32_t player, uint32_t unit_id);

} // namespace detail

void     pick_owned_tile_or_home(int32_t player, uint32_t *out_x, uint32_t *out_y);
uint32_t unit_is_order_pending(uint32_t player, uint32_t unit_id);

} // namespace mh::ai
