//
// ai/ai_queue_type_query.h -- the two build-queue membership predicates (RI-AI / AI1B, layer 4).
//
// llm_strat_ai_bldg_type_queue_has_pending @0x004d3a27 (108 B) and
// llm_strat_ai_bldg_type_already_queued   @0x004d3a93 ( 95 B).
//
// THEY ARE THE SAME LOOP WITH DIFFERENT MATCH KEYS, and that is the only thing about them worth
// getting right. Both walk player_data[player].ai_bldg_queue[0 .. ai_bldg_queue_count) and accept an
// entry whose status low nibble is 1 (a CONSTRUCTION entry). What differs is one hop:
//
//   already_queued   entry.tick_or_unit_id                 == argument   (0x004d3aad-0x004d3ab6)
//   queue_has_pending  Building[entry.tick_or_unit_id].type == argument   (0x004d3a41-0x004d3a55)
//
// So `already_queued` matches an exact cfg BUILDING id (the define index the queue entry stores)
// and `queue_has_pending` matches the cfg Building record's `type` field, i.e. the E_BUILDING CLASS
// (A_MINE 2 / H_MINE 0x16, A_TURRET 5 / H_TURRET 0x19, ...). The call sites make the difference
// visible: llm_strat_ai_react_resource_shortage loads a literal 1 or 0x15 before the has_pending
// call (0x004e574b/0x004e5752) and a player_data field before the already_queued one
// (0x004e577c) -- class constants in one, an id in the other.
//
// Neither writes anything, so a shadow site's whole verdict is the RETURN value -- the same shape
// ai_bldg_weapon_range.cpp already arms, and real evidence for exactly that reason. What it cannot
// certify is that the walk terminated where the original's did, only that it answered the same.
//
// TWO WIDTH DETAILS FROM THE LISTING, neither visible in Ghidra's C:
//   * the loop bound is an UNSIGNED compare (CMP EBX,dword ptr [.. count] / JC @0x004d3a85 and
//     @0x004d3ae4), so a negative count would run the loop, not skip it. Reproduced.
//   * both match keys are MOVZX byte loads compared against the FULL 32-bit argument, so an
//     argument above 0xff can never match. Reproduced by comparing the zero-extended byte.
//
// The status test is a byte test (MOV AL / AND AL,0xf / CMP AL,0x1), not a dword one; on this field
// that is the same thing, but the bodies are transcribed as they read.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// "Is a CONSTRUCTION entry for any building of this E_BUILDING class already queued?" Returns 1/0.
int32_t bldg_type_queue_has_pending(const ai_view &v, int32_t player_idx, uint32_t bldg_type);

// "Is a CONSTRUCTION entry for this exact cfg building id already queued?" Returns 1/0 in AL --
// committed as `bool`, which is why the callers' decompiles show a CONCAT31 Watcom artifact.
int32_t bldg_type_already_queued(const ai_view &v, int32_t player, uint32_t building_type);

} // namespace detail

int32_t bldg_type_queue_has_pending(int32_t player_idx, uint32_t bldg_type);
int32_t bldg_type_already_queued(int32_t player, uint32_t building_type);

} // namespace mh::ai
