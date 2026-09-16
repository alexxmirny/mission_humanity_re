//
// sim/sim_resource_add_spend.h -- the two low-level player-resource ledger primitives (RI-SIM / SIM1F):
//
//   llm_resource_add   @0x00497f4a (0x4a bytes) -- unconditional credit
//   game_SpendResource @0x00497f94 (0x78 bytes) -- clamped-to-zero debit
//
// Bundled in one TU because they are the SAME subsystem (llm_resource_add's own plate comment names
// game_SpendResource directly as its "<=0 delta" counterpart) and share the one outward callee,
// game_UpdateResourceStats.
//
// Both write `player_resources[player][resource_id]` (RID_PLAYER_RESOURCES) through the brand-new
// `sim_store::player_resource_at(player, resource_id)` (added this slice) -- the FIRST direct writers
// of this region in the whole 307-function sim closure; every earlier caller reached it by calling one
// of these two, never by writing it directly (see sim_state.h's own comment on the accessor, and
// sim_resource_decay.cpp's `decay_excess_resources`, which is exactly such an earlier indirect caller).
//
// Do NOT confuse `player_resources` with `resource_spend_total` (a DIFFERENT array, a per-player
// running total keyed by resource id with no per-player row dimension the same way -- see
// sim_state.h's own comment distinguishing the two); neither function here touches that array.
//
// ---- llm_resource_add ---------------------------------------------------------------------------
//
// `void __watcall llm_resource_add(int player, int resource_index, int amount)` per the .asm header
// (player=EAX, resource_index=EDX, amount=EBX) -- matches mh_calls.gen.h's
// `llm_resource_add(int32_t player, int32_t resource_index, int32_t amount)`.
//
//   player_resources[player][resource_index] += amount;
//   game_UpdateResourceStats(player, amount, resource_index);
//
// No clamp, no branch -- unconditional add, positive or negative `amount` alike.
//
// ---- game_SpendResource ---------------------------------------------------------------------------
//
// `void __mh_watcall_ebx_volatile game_SpendResource(int player, int res_id, int amount)` per the .asm
// header (player=EAX, res_id=EDX, amount=EBX) -- matches mh_calls.gen.h's
// `game_SpendResource(int32_t player, int32_t res_id, int32_t amount)`.
//
//   spend = amount;
//   if (player_resources[player][res_id] < amount) spend = player_resources[player][res_id];  // 0x00497fc2 CMP EAX,mem / JLE
//   player_resources[player][res_id] -= spend;
//   game_UpdateResourceStats(player, -spend, res_id);
//
// The CMP compares `amount` (EAX) against the CURRENT resource value (memory operand) and takes the
// JLE (skip-the-clamp) branch when `amount <= current` -- i.e. the clamp block at 0x00497fca-0x00497fdc
// runs only when `current < amount`, in which case `spend` is overwritten with `current` itself
// (whatever its sign). The subsequent subtract is then always `current -= spend`: in the un-clamped
// case that is `current -= amount` (an ordinary deduction); in the clamped case it is `current -=
// current`, i.e. the balance is forced to exactly 0 regardless of whether `current` was positive or
// already negative -- this IS the "clamped so it never goes negative" behavior the plate describes, not
// a bug to fix by clamping to `amount` or to 0 directly. Reproduce the two-step
// compare-then-subtract shape verbatim rather than collapsing it to `max(0, current - amount)`, since
// `game_UpdateResourceStats`'s second argument is `-spend` (the ACTUAL delta applied), which a collapsed
// formula would have to re-derive rather than carry through directly.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the one outward call, shared by both functions -----------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// game_UpdateResourceStats's own committed prototype (addr/mh_calls.gen.h) is `(uint32_t player,
// uint32_t amount, uint32_t res_id)` -- note the parameter ORDER is (player, amount, res_id), not
// (player, res_id, amount): the asm's own register-to-call-argument mapping (EAX->player, EBX->amount,
// EDX->res_id at both call sites) is what fixes this, not either caller's own local parameter order.
struct resource_add_spend_calls {
    void (*update_resource_stats)(uint32_t player, uint32_t amount,
                                  uint32_t res_id); // game_UpdateResourceStats @0x004dbeec
};

const resource_add_spend_calls &live_resource_add_spend_calls();

// The logic over an explicit view + calls table, matching every other sim TU's split.
namespace detail {

// llm_resource_add @0x00497f4a. See the header banner for the full derivation. Writes
// player_resources[player][resource_index] through `own`.
void resource_add(sim_store &own, const resource_add_spend_calls &c, int32_t player,
                  int32_t resource_index, int32_t amount);

// game_SpendResource @0x00497f94. See the header banner for the full derivation. Writes
// player_resources[player][res_id] through `own`.
void spend_resource(sim_store &own, const resource_add_spend_calls &c, int32_t player, int32_t res_id,
                    int32_t amount);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly.
void resource_add(int32_t player, int32_t resource_index, int32_t amount);
void spend_resource(int32_t player, int32_t res_id, int32_t amount);

namespace detail {
} // namespace detail

} // namespace mh::sim
