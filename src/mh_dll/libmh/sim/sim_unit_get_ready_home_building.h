//
// sim/sim_unit_get_ready_home_building.h -- is the current unit's home storage building ready to
// receive it back (RI-SIM / SIM1-G1 tail slice).
//
//   llm_strat_unit_get_ready_home_building @0x00484a14 (0x136 bytes)
//   `int __watcall llm_strat_unit_get_ready_home_building(void)` per the .asm header and the
//   committed export sig (`sig_llm_strat_unit_get_ready_home_building` = `int32_t(__cdecl*)(void)`)
//   -- NO PARAMETERS, ambient "current unit" query (_G_LLM_STRAT_CUR_UNIT/_CUR_PLAYER), same shape
//   as sim_unit_state_move_walker.h's sibling and every other CUR_UNIT helper in this batch.
//   Translated from the DISASSEMBLY
//   (tmp/decomp_sim/llm_strat_unit_get_ready_home_building_00484a14.asm); the Ghidra .c draft was
//   independently address-walked field-by-field and found faithful throughout (every offset/branch
//   below matches the draft's own reading -- see the .cpp for the address-by-address cross-check).
//
// ---- SHAPE (pure query, writes nothing) ------------------------------------------------------
//   1. (0x00484a2c-0x00484a3e) If CUR_UNIT.home_storage_slot == 0: return 0 (never docked/assigned).
//   2. (0x00484a43-0x00484a77) slot = unit_storage[CUR_PLAYER][home_storage_slot]; if
//      slot.b_index == 0: return 0 (slot not bound to any building).
//   3. (0x00484a7c-0x00484af6) building = buildings[CUR_PLAYER][slot.b_index]; if
//      building.energy <= 0.0 (destroyed/dead) OR building.online_state == 0 (not operational-phase)
//      OR building.built_flags != BUILT_FLAGS_OPERATIONAL (not connected+staffed): return 0.
//   4. (0x00484aff-0x00484b3d) Else: ask llm_strat_storage_type_accepts_unit(building.building_id,
//      CUR_UNIT.unit_proto_id); if it says no, return 0; otherwise return slot.b_index (the roster
//      building index CUR_UNIT should head home to).
//
// ---- FIELD-OFFSET CROSS-CHECK (all four building-field addresses resolve to ONE consistent base,
// 0xc3d2a0 = buildings[CUR_PLAYER][slot.b_index]'s address, matching struct offsets exactly):
//   built_flags @+0x4  -> 0xc3d2a4  (mh_map_object_building::built_flags,  offset 0x4)
//   online_state@+0x17 -> 0xc3d2b7  (mh_map_object_building::online_state, offset 0x17)
//   energy      @+0x19 -> 0xc3d2b9  (mh_map_object_building::energy,       offset 0x19)
//   building_id @+0x2  -> 0xc3d2a2  (mh_map_object_building::building_id,  offset 0x2)
// unit_storage's b_index read (`[EAX + 0xc727c0]`, no added offset) matches
// mh_map_object_unit_storage::b_index being the struct's FIRST field (offset 0).
//
// ---- THE FCOMP/JNC ENERGY TEST (x87, not an FP landmine here since it is a single scalar
// comparison against 0.0, no accumulated intermediate) -- FLDZ; FCOMP [energy]; FNSTSW AX; SAHF;
// JNC skips the online_state check when 0.0 >= energy, i.e. `energy <= 0.0`, matching the Ghidra
// draft's own reading. Reproduced as a plain `<=` compare (no x87 intermediate-precision hazard: a
// single load-and-compare against the literal 0.0 cannot diverge between x87 80-bit and any other
// width).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call, indirected through a `_calls` struct per sim_storage_find_home_for_unit.h's
// precedent (a single, provably-pure callee still goes through `_calls`, never `mh::call::` directly
// from `detail::`, so `detail::unit_get_ready_home_building` stays drivable by `net_selftest.exe
// simtest` over heap buffers).
struct unit_get_ready_home_building_calls {
    // llm_strat_storage_type_accepts_unit @0x00497b91 -- takes the raw building/unit ids and does its
    // own cfg lookup internally (pure query, no write, no other outward call).
    int32_t (*storage_type_accepts_unit)(uint32_t building_index, uint16_t unit_index);
};

const unit_get_ready_home_building_calls &live_unit_get_ready_home_building_calls();

namespace detail {

// llm_strat_unit_get_ready_home_building @0x00484a14. See the header banner for the full shape.
// Zero-arg, ambient cur_player/cur_unit, matching the original's void(void) signature. Pure read --
// writes nothing (no `own` parameter needed).
int32_t unit_get_ready_home_building(const sim_view &v, const unit_get_ready_home_building_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_get_ready_home_building_calls().
// Matches the committed prototype (sig_llm_strat_unit_get_ready_home_building) exactly.
int32_t unit_get_ready_home_building();

namespace detail {
} // namespace detail

} // namespace mh::sim
