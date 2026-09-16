//
// ai/ai_launch_storage.h -- llm_strat_ai_unit_launch_from_storage_enqueue @0x004d58e7
// (AI1B, antichain layer 4).
//
// Twenty-two bytes, and all of them are one adapter: mask the player to its low nibble and TAIL-JUMP
// into llm_strat_unit_order_auto_launch_from_storage_enqueue @0x0046cf2c with the other three
// arguments untouched in EDX/EBX/ECX.
//
//   PUSH 0x4 / CALL assert_stack_capacity   @0x004d58e7  -- the Watcom stack probe, no state
//   XOR AH,AH / AND AL,0xf / MOVZX EAX,AX   @0x004d58f1  -- EAX := player & 0xf, upper bits cleared
//   JMP 0x0046cf2c                          @0x004d58f8  -- a tail call; the callee's RET is ours
//
// The mask is a no-op at every observed call site (players are 0..7 and all six sites pass a loop
// index in EDI), so it is reproduced for faithfulness, not because anything exercises it.
//
// WHAT THIS FUNCTION DOES, from the six call sites rather than from its own body: it sends a unit
// that is sitting inside a storage/hangar building out toward a target tile. Every site is an AI
// group handler -- group_home_guard_replenish (twice), army_milestone_advance_or_attack,
// group_task_drain_reserve_attack, group_task_recruit_from_pool3 and _pool4 -- and each loads the
// unit index with a MOVZX from a word field of the group or the player record, then the target x/y
// from a pair of adjacent player_data dwords, then the player in EAX.
//
// IT IS AN ESCAPE FOR EVERY *CALLER*, AND NOT FOR ITSELF. Seen from a shadowed caller that declares
// none of the order regions, running this twice would launch the unit twice and no restore could
// take that back -- which is why ai_state.cpp stubs it as `inert_launch` in the shadow call set and
// has done since batch A layer 2. Its OWN site is different, because a site declares the regions its
// body reaches:
//
//   closure = llm_strat_unit_order_auto_launch_from_storage_enqueue
//             -> llm_strat_order_scratch_reset  (writes _G_LLM_STRAT_ORDER_SCRATCH_ARGS)
//             -> llm_strat_order_scratch_set_field (same region)
//             -> llm_strat_order_enqueue        (writes _G_LLM_STRAT_ORDER_QUEUE + _COUNT)
//
// and nothing else: all three are leaves whose only callee is assert_stack_capacity, and the state
// matrix credits exactly those three regions with 1 + 1 + (5 + 1 rmw) = the 9 transitive writes the
// matrix attributes to this adapter's closure. Declaring all three lets both arms enqueue for real;
// the harness restores the pre-state before our arm and hands the game the ORIGINAL's post-state, so
// exactly one order reaches the queue and the comparison is over the real queued bytes.
//
// WHAT THAT COMPARISON CAN AND CANNOT CATCH -- stated because it bounds the tier, not to hedge. Both
// arms call the SAME original callee, so the only thing under test is the marshalling: a swapped
// target_x/target_y, a dropped unit index, or a player written without the nibble mask would each
// change the bytes llm_strat_order_enqueue writes and show up as a DIVERGENCE. What it cannot test
// is the mask itself, because no live call has a player above 15. That half is covered offline in
// net_selftest aitest instead.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {
// `player` is the raw AL byte; the mask is applied here, not by the caller.
void unit_launch_from_storage_enqueue(const ai_calls &gc, uint8_t player, int32_t unit_id,
                                      uint32_t target_x, uint32_t target_y);
// The masked player the adapter forwards. Split out so the offline test can exercise the mask
// without issuing an order.
uint32_t launch_player_nibble(uint8_t player);
} // namespace detail

void unit_launch_from_storage_enqueue(uint8_t player, int32_t unit_id, uint32_t target_x,
                                      uint32_t target_y);

} // namespace mh::ai
