//
// ai/ai_group_home_guard.h -- llm_strat_ai_group_home_guard_replenish (RI-AI / AI1C layer 2).
//
// llm_strat_ai_group_home_guard_replenish @0x004e6f39 (0x1ff bytes)
//
// Tops up the player's home-defense group (ai_groups[0]) toward a target headcount, then
// repositions it. Read from the LIVE Ghidra decompile at 0x004e6f39 (not the tmp/decomp_ai/*.c
// export, per the module rule) plus the raw .asm for register-level detail -- the decompile itself
// carries a plate saying two things in an EARLIER draft were wrong and were corrected by an
// AI-PREP adversarial-review pass on 2026-08-01 (dispatch-order and per-loop filter placement); both
// corrections are cross-checked against the .asm below and reproduced here.
//
// FIVE STEPS, IN ORDER:
//   1. llm_strat_ai_group1_drain_to_group0(player) -- unconditional, first thing done.
//   2. TARGET HEADCOUNT. Default: (housing.used_soldiers + housing.used_vehicles) *
//      HOME_GUARD_TARGET_PCT / 100. The add and the multiply are done in uint32_t and the DIVISION
//      signed, which is what the original's instructions are: Watcom's 2-operand IMUL truncates the
//      product to 32 bits (identical bits either way, but C signed overflow is UB where unsigned
//      wraparound is defined), then CDQ+IDIV divide that word SIGNED. Both halves matter -- see the
//      cast in the .cpp. HOME_GUARD_TARGET_PCT is a global int, value 80 (0x50).
//      OVERRIDDEN when ai_resource_shortage_state == 3: target becomes ai_groups[0].member_count +
//      ai_groups[2].member_count (both read as unsigned 16-bit), i.e. "whatever's already staged",
//      which makes the refill loop below a no-op under a resource crunch.
//   3. ROUTE IDLE FIGHTERS HOME (only when shortage_state != 3): walk ai_groups[0]'s member list,
//      and for every member whose cfg_units[unit_proto_id].ai_unit == FIGHTER (8), call
//      llm_strat_ai_route_unit_to_home_storage. The walk's `next` pointer is captured BEFORE the
//      call, from the member's OWN ai_group_next field, not by re-reading head_unit -- confirmed at
//      0x004e6fe7/0x004e6fee, which run before the FIGHTER test. NO idle/status check beyond
//      membership -- the earlier draft's plate correction (1) says this explicitly.
//   4. REFILL FROM POOL 2, no type filter (plate correction (2)): while target > (uint16)
//      ai_groups[0].member_count AND ai_groups[2].member_count != 0, launch ai_groups[2].head_unit
//      from storage to the home tile and move it into group 0. head_unit is RE-READ every
//      iteration (0x004e7045/0x004e704f: this loop, unlike step 3/5, re-reads the group struct
//      rather than caching a next-pointer, because group_member_move changes which unit is head).
//   5. SHORTAGE RESTOCK FROM POOL 4 (only when shortage_state == 3): same walk-with-captured-next
//      shape as step 3, over ai_groups[4], filtered to FIGHTER members, launching AND moving each
//      one into group 0 (0x004e70bb/0x004e70c2 capture next before the two calls).
//   Finally: llm_strat_ai_group_reposition_members(player, 0), unconditional.
//
// UNIT_LAUNCH_FROM_STORAGE_ENQUEUE IS INERT IN THE SHADOW ARM (steps 4 and 5) -- the SAME ai_calls
// slot batch A layer 2 already stubs, because it tail-jumps into a real unit-order path that cannot
// be rolled back. The group_member_move call right after it still runs for real and is what the
// shadow site compares; see ai_state.cpp's shadow_calls() comment at this site.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID.
struct home_guard_report {
    int32_t target              = 0;     // the computed (or shortage-overridden) headcount
    bool    shortage_override   = false; // target came from the shortage branch, not the housing pct
    int32_t routed_home         = 0;     // step 3: FIGHTER members routed group0 -> storage
    int32_t refilled_from_pool2 = 0;     // step 4: units launched+moved from group 2
    int32_t refilled_from_pool4 = 0;     // step 5: FIGHTER units launched+moved from group 4
};

// llm_strat_ai_group_home_guard_replenish @0x004e6f39.
home_guard_report group_home_guard_replenish(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id);

} // namespace detail

void group_home_guard_replenish(int32_t player_id);

} // namespace mh::ai
