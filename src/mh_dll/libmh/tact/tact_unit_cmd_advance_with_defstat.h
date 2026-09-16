//
// tact/tact_unit_cmd_advance_with_defstat.h -- TACT1B: command-queue op 0xb.
//
//   llm_tact_unit_cmd_advance_with_defstat @0x0042f8e4 (0x51)
//
// The op-0xb queue-entry handler: latches the head queue entry's arg0 into def_stat (the AI
// "defense stance" byte llm_tact_unit_owner_tick switches on), then dispatches the real
// dequeue/advance (llm_tact_unit_cmd_advance, frontier -- stays original per Law 4).
//
// VERIFICATION DEBT DRAIN (2026-08-26, TACT1B). Op 0xb is genuinely unreachable by
// migration_sweep.py's tact-save-11 TACT-SYNTH run: re-derived from the listings, not guessed --
// tmp/decomp_tact/llm_tact_unit_weapons_tick_0042f338.asm's dispatch (`CMP word ptr
// [EBP+-0x2c],0xb` / `JZ 0x0042f8a0` @0x0042f64e-0x0042f653) is the ONLY caller, and its own `op`
// local traces back through llm_tact_unit_owner_tick and llm_tact_unit_move_tick's cmd_queue reads
// to whatever op byte sits in a unit's queue -- which for a PLAYER-issued command is enqueued by
// exactly the 10 sites TACT-SYNTH's own header banner enumerates (llm_tact_frame x6,
// llm_tact_group_issue_order x3, llm_tact_ui_sel_panel_multi_mode_tick x1), and NONE of those 10
// pass op 0xb as a literal (checked every one against tmp/decomp_tact/llm_tact_frame_00429b1a.asm
// and llm_tact_group_issue_order_0042b09f.asm: their enqueue/group-issue call sites load EDX from
// either a hardcoded op in {9,6,0x7f,4,5} or forward group_issue_order's OWN op parameter, which
// the UI's own button set restricts to {1,2,0x40,0x46,0x47,0x7f}). The one remaining enqueue site,
// llm_tact_mission_load @0x00438aae, loads its op from a stack local fed by the mission SCRIPT's own
// data, not a literal -- so op 0xb is mission-data-driven, and no player action or TACT-SYNTH
// extension can manufacture it. That makes this function the SAME shape as
// llm_tact_squad_sync_hp (tact_squad_status.h/.cpp): armable, but the only path to it is a scenario
// the rig cannot be made to hit, so the outward call is indirected through a `_calls` struct (this
// codebase's established sim/ pattern -- sim_prod_shuttle_depart.h documents the same rationale)
// and proven offline instead (tact_unit_cmd_advance_with_defstat_selftest.cpp).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call this function makes, indirected via mh::call:: for offline testability --
// same shape as sim/'s `_calls` structs (see sim_prod_shuttle_depart.h for the full rationale: a
// direct mh::call:: call cannot be driven by net_selftest.exe tacttest through detail:: alone).
struct unit_cmd_advance_with_defstat_calls {
    void (*unit_cmd_advance)(int32_t unit_idx,
                             int32_t cmd_slot_index); // llm_tact_unit_cmd_advance @0x00431229
};

const unit_cmd_advance_with_defstat_calls &live_unit_cmd_advance_with_defstat_calls();

namespace detail {

// llm_tact_unit_cmd_advance_with_defstat @0x0042f8e4.
//
// 1. @0x0042f915-0x0042f91b: def_stat = (uint8_t)cmd_queue[cmd_slot_index].arg0 -- only the LOW
//    byte of the ushort arg0 field is read (a single-byte MOV, not the full word).
// 2. @0x0042f921-0x0042f927: llm_tact_unit_cmd_advance(unit_idx, cmd_slot_index) (frontier, via `c`).
void unit_cmd_advance_with_defstat(tact_store &own, const unit_cmd_advance_with_defstat_calls &c,
                                   int32_t unit_idx, int32_t cmd_slot_index);

} // namespace detail

void unit_cmd_advance_with_defstat(int32_t unit_idx, int32_t cmd_slot_index);


} // namespace mh::tact
