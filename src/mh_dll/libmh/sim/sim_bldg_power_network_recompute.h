//
// sim/sim_bldg_power_network_recompute.h -- llm_strat_bldg_power_network_recompute @0x00491c76
// (0x425 bytes), translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_power_network_recompute_00491c76.asm).
//
// Per-player recompute of the building POWER-connectivity network, run after a power change. Pure
// DELEGATOR: every effect flows through four callees, all reached via the ORIGINAL addresses
// (`mh::call::`) rather than a same-batch sibling's translated body -- see
// tmp/decomp/_CONTEXT_sim1b_slice4.md's "callees stay original" rule, which applies to this
// function's own siblings in the same batch too. This body makes NO direct state write itself.
//
//   (0) If the player has no primary mother building set for the current planet
//       (profiles[player].primary_mother_bldg[planet] == 0), re-elect one:
//       mh::call::llm_strat_mother_reelect_primary(player, landing_x[planet], landing_y[planet]).
//       Return value discarded (the original never reads EAX after this call).
//
//   (1) Walk buildings[player][1..99] (slot 0 holds the live-count, per the same `.index`-as-
//       occupancy-and-count idiom sim_bldg_refresh_all_buildings.h documents), calling
//       mh::call::llm_strat_bldg_clear_flag_bit0_notify(player, slot) on every occupied slot.
//       DIFFERENT LOOP-EXIT TEST than that sibling's own walk: the remaining-count local is tested
//       with `!= 0` (JNZ, 0x00491d0a-0x00491d14), NOT the signed `> 0` (JG) that sibling's header
//       documents for its own walk -- verified from the raw CMP/JNZ bytes, not assumed. Preserved
//       as-is: with an int32_t countdown the two tests only diverge if the count is driven negative,
//       which (as in the sibling) an inaccurate `.index` count can do.
//
//   (2) live_count = trunc_toward_zero(buildings[player][0].energy) (via the ORIGINAL
//       utils_math_trunc @0x004d0596, x87-register-only ABI -- see the trunc_to_int32() note in the
//       .cpp). Same `!= 0` loop-exit test as (1). For each occupied slot whose `.energy` (the HP/
//       charge stat, NOT the POWER resource -- see docs/conventions.md#energy-is-not-power) is > 0.0, decrement live_count
//       UNCONDITIONALLY (before the type/state gate below is even evaluated -- 0x00491dad-0x00491db3
//       is unconditional on the energy check alone), then call
//       mh::call::llm_strat_bldg_propagate_network_connectivity(player, slot) if EITHER:
//         - cfg_buildings[buildings[player][slot].building_id].type is H_MOTHER/A_MOTHER AND
//           (profiles[player].primary_mother_bldg[planet] == slot OR buildings[..][slot].online_state
//           == 0), OR
//         - that type is H_PLANT/A_PLANT AND buildings[..][slot].online_state != 0.
//
//   (3) Walk buildings[player][1..99] again by the SAME `.index`-occupancy idiom as (1) (a third,
//       independent live_count read/decrement pass), calling
//       mh::call::llm_bldg_set_connected_flag(player, slot) if the slot's cfg type is H_SHUTTLE/
//       A_SHUTTLE, OR H_PORT/A_PORT, OR ( (type is H_MOTHER/A_MOTHER AND state != CONSTRUCTION AND
//       primary_mother_bldg[planet] != slot) OR state == DISMANTLING ) -- the DISMANTLING arm is a
//       TOP-LEVEL alternative, reached for every type that fails the shuttle/port checks (mother or
//       not), not nested under the mother gate -- verified from the control flow (LAB_00491fcf's
//       non-mother path falls straight through to the DISMANTLING check at LAB_00492064, same as the
//       mother path's own fallthrough).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The four external callees this closure reaches, indirected for offline testability -- same reason
// as sim_bldg_refresh_all_buildings.h's `refresh_all_buildings_calls`. Pointer types match
// addr/mh_calls.gen.h's committed signatures exactly (aggregate-initializer binding in
// live_power_network_recompute_calls() requires it).
struct power_network_recompute_calls {
    // llm_strat_mother_reelect_primary @0x00498aad. `__mh_watcall_ebx_volatile` (EAX=player,
    // EDX=x, EBX=y) -- no special handling needed here, the generated mh::call:: thunk hides it.
    // Return value discarded by the original caller.
    int32_t (*mother_reelect_primary)(int32_t player, int32_t x, int32_t y);
    // llm_strat_bldg_clear_flag_bit0_notify @0x0049663d.
    void (*bldg_clear_flag_bit0_notify)(uint16_t player, int32_t building_index);
    // llm_strat_bldg_propagate_network_connectivity @0x0049209b.
    void (*bldg_propagate_network_connectivity)(uint16_t player, int32_t b_index);
    // llm_bldg_set_connected_flag @0x004965d6.
    void (*bldg_set_connected_flag)(uint16_t player, int32_t b_index);
};

const power_network_recompute_calls &live_power_network_recompute_calls();

namespace detail {

// llm_strat_bldg_power_network_recompute @0x00491c76. See the header banner above for the
// derivation. Read-only over sim_view -- this function makes no direct state write of its own; every
// effect is a call to one of the four original callees.
//
// `player` is uint16_t here matching the committed ABI directly (the asm header's own storage=AX:2 --
// unlike most sibling functions in this closure, the original does NOT stash a full 32-bit stack
// dword and re-narrow through MOVZX at each use; it stores the incoming AX into a 32-bit local and
// the Ghidra .c draft's `& 0xffff` masking at every access is therefore a no-op here, not a
// behaviour-carrying narrowing -- so it is not reproduced as a separate masking step).
void power_network_recompute(const sim_view &v, const power_network_recompute_calls &gc,
                             uint16_t player);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter type matches the committed prototype (addr/mh_export.gen.h's
// sig_llm_strat_bldg_power_network_recompute, `void(__cdecl *)(uint16_t player)`).

void power_network_recompute(uint16_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
