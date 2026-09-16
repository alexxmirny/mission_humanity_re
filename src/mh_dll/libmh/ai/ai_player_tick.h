#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state()/live_calls(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_player_tick @0x004e8b9d.
void player_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player);

// `trunc(sqrt((double)radius_sq))`, exactly as 0x004e8caf-0x004e8ccc builds it: FILD a
// zero-extended uint32 into an x87 double, FSQRT (CRT_004da9f0's non-negative arm), then
// utils_math_trunc's FSTCW/FLDCW-toward-zero/FRNDINT/FLDCW-restore, FISTP, low dword only. See the
// .cpp for why CRT_004da9f0's error arm (a negative ST0) is not reproduced as a branch. Exposed for
// the offline test, matching ai_mine_yield.h's x87_scale_and_trunc / ai_group_muster_pick.h's
// weapon_power_add_and_trunc.
int32_t x87_sqrt_and_trunc(uint32_t radius_sq);

} // namespace detail

void player_tick(int32_t player);

} // namespace mh::ai
