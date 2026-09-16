//
// tact/tact_squad_status.h -- TACT1A: the per-frame squad-status HP refresh.
//
//   llm_tact_squad_sync_hp @0x00438f09 (0xfc)
//
// Writes `.energy_pct` in the squad-status blackboard (_G_LLM_SQUAD_STATUS, RID_SQUAD_STATUS) for
// every occupied slot -- the ONLY channel that survives a tactical excursion back into strategic
// mode. Its write escapes through a COMPUTED address (slot_index << 4),
// which is exactly why the instruction-sweep write-set preflight reported this row "0 direct, 0
// transitive write cells: NOT SHADOWABLE" before the region existed at all -- squad_status_at()
// below is what makes it a real, named write instead.
//
// CALLED ONCE, AT THE EXIT-CONFIRM MOMENT -- NOT every tactical frame. Both of llm_tact_frame's
// call sites (0x00429c7d, 0x00429de9) are on the exit-confirmation arm, immediately before
// llm_tact_mission_end_return_to_strategic -- confirmed via find-cross-references' call-site
// context, not assumed from the function's earlier prose description. This means a force-entered
// rig scenario that never drives the mission-exit input sequence reports this site as
// NOT COVERED (0 calls) -- a scenario gap, not a code problem -- so its evidence here is the
// offline oracle TACT1A's own done_when calls for, not a rig run.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_squad_sync_hp @0x00438f09.
//
// For slot in [0, _G_LLM_TACT_SQUAD_SIZE): if that slot's blackboard entry currently holds
// energy_pct <= 0 (0x00438f46/0x0043 8f4d, JLE), skip it entirely -- no write, the slot is left
// exactly as it was. This is the blackboard's own "slot unused" convention (unit_proto_id/
// energy_pct both start at 0 when llm_strat_bldg_gather_nearby_squad_status clears the table).
//
// Otherwise index _G_LLM_TACT_UNITS at (slot+1) -- 1-based, matching TACT_UNIT_FIRST_SLOT -- and
// pick ONE of three outcomes, all mutually exclusive:
//   1. units[slot+1].hp == 0        (0x00438f5d/0x00438f65, JNZ)  -> energy_pct := 0
//   2. hp != 0 AND owner != 0       (0x00438f86/0x00438f8d, JNZ)  -> energy_pct := 0
//   3. hp != 0 AND owner == 0                                      -> energy_pct :=
//        max(1, hp*100 / character_types[units[slot+1].type].energy)   (0x00438f9c-0x00438fd5:
//        signed IDIV at 0x00438fc3, then a SEPARATE floor-to-1 at 0x00438fce if the division
//        truncated to exactly 0 -- not the IDIV's own behaviour, an explicit extra branch).
// Cases 1 and 2 both write a literal 0 but from DIFFERENT instructions/addresses -- reproduced as
// the original's own two stores, not merged into one "hp==0 || owner!=0" test, so a future reader
// diffing against the disassembly finds the same instruction count.
void squad_sync_hp(const tact_view &v, tact_store &own);

} // namespace detail

void squad_sync_hp();


} // namespace mh::tact
