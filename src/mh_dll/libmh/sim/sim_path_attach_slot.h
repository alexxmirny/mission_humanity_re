//
// sim/sim_path_attach_slot.h -- one function (RI-SIM / SIM1-G2):
//
//   llm_strat_path_attach_slot @0x00496a7b (0x5a bytes)
//
// `void __watcall llm_strat_path_attach_slot(int player, int unit_idx, int slot)`, matching the
// committed prototype exactly (addr/mh_calls.gen.h names the params param_1/param_2/param_3; the
// derivation below assigns them the names their use establishes: param_1*0x64+param_3 indexes
// `_G_LLM_STRAT_PATH_SLOT_FLAGS` at [player][slot], param_1*0x5b04+param_2*0xe9 indexes the `units`
// roster at [player][unit_idx] -- 0x5b04==23300==233(sizeof map_object_unit)*100 and 0xe9==233 --
// confirming param_1=player, param_2=unit_idx, param_3=slot).
//
// Three writes, all already-bound sim_state accessors: marks the slot in-use
// (`path_slot_flag_at(player,slot)=1`), records the slot id on the unit
// (`units[player][unit_idx].path_slot_id=(uint8_t)slot`, offset 170 -- the struct's own field,
// already documented "Index (0-99) of this unit's path buffer among the player's 100 slots"), and
// decrements the player's free-slot counter (`path_free_slot_count_at(player)--`). No callees besides
// the inert Watcom stack probe.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_path_attach_slot @0x00496a7b. Writes path_slot_flags, units[player][unit_idx].path_slot_id,
// path_free_slot_count -- all already-bound sim_store accessors.
void path_attach_slot(sim_store &own, int32_t player, int32_t unit_idx, int32_t slot);


} // namespace detail

// Public wrapper. Matches the committed prototype (mh::call::llm_strat_path_attach_slot) exactly.
void path_attach_slot(int32_t player, int32_t unit_idx, int32_t slot);

} // namespace mh::sim
