//
// sim/sim_bldg_notify_state_change.h -- llm_strat_bldg_notify_state_change @0x00470c5c (0x4f B),
// translated from the DISASSEMBLY (tmp/decomp/llm_strat_bldg_notify_state_change_00470c5c.asm), NOT
// from the Ghidra .c draft's "general.change_flag2 = 0" line -- see the HAZARD note below for why that
// line is right anyway.
//
//   void __watcall llm_strat_bldg_notify_state_change(ushort player, uint building_id)
//   -- AX=player, EDX=building_id (asm header + the committed prototype,
//   addr/mh_export.gen.h's sig_llm_strat_bldg_notify_state_change:
//   `void(__cdecl*)(uint16_t, uint32_t)`).
//
// TRIVIAL BODY, THREE STEPS, ALL LITERAL FROM THE ASM:
//   1. 0x00470c79: MOV dword ptr [general+0x24], 0 -- see HAZARD below: this single dword store
//      zeroes BOTH change_flag (int16_t @0x24) and change_flag2 (int16_t @0x26) at once.
//   2. 0x00470c8a: CALL llm_strat_bldg_notify_ui(player, building_id) UNCONDITIONALLY.
//   3. 0x00470c8f-0x00470c9d: CMP dword ptr [general+0x24], 0; if nonzero, game_SetEvent(8)
//      (BUILD_UNITS_REFRESH per the strategic-sim notes' game::e::event table).
//
// ---- HAZARD: the dword store/compare at 0x00470c79 / 0x00470c8f is the SAME address BOTH TIMES,
// and it IS change_flag+change_flag2 combined, not a third, unrelated field --------------------------
// The raw instruction bytes of both the entry MOV and the post-call CMP encode the identical absolute
// address 0x00e153b4 (general's base 0x00e15390 + 0x24), which sim_state.h's own change_flag()/
// change_flag2() accessors are bound from (static_assert'd offsets 0x24/0x26, struct ends at 0x28 --
// no padding, no room for a third field at this offset). sim_unit_notify.h's own HAZARD note
// (llm_strat_unit_notify_ui, corrected 2026-08-12 after reimpl-verify caught the same shape) already
// names THIS function by address as one of the other two dword-width accesses to 0x00e153b4 in the
// image ("Every other access to this address elsewhere in the codebase (llm_strat_bldg_notify_
// state_change, llm_strat_sim_step) is likewise dword-width, consistent with the pair being
// read/written together by design"). So:
//   * step 1's dword store of 0 zeroes change_flag AND change_flag2 together -- the Ghidra .c draft's
//     two-statement rendering (general.change_flag = 0; general.change_flag2 = 0;) is the CORRECT
//     reading of that one wide store, not a decompiler fabrication of a second field.
//   * step 3's dword compare against 0 is checking the SAME two fields, re-read as a pair, after
//     llm_strat_bldg_notify_ui had the chance to set one of them (its unit-side twin,
//     llm_strat_unit_notify_ui, sets change_flag=1/change_flag2=0 together in its local-player arm --
//     see sim_unit_notify.cpp -- and the building-side notify_ui at 0x00470bdd, not yet translated,
//     is presumably the same shape). "general's field at +0x24" in the batch brief is this SAME
//     dword, not a separate struct member; there is no byte range left at offset 0x24 for anything
//     else. Reproduced here as `own.change_flag() != 0 || own.change_flag2() != 0`, which is bit-for-
//     bit equivalent to "the dword is nonzero" since a 32-bit word is zero iff both 16-bit halves are
//     zero.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event member 8 (the strategic-sim notes table) -- BUILD_UNITS_REFRESH. Named as a local
// constant citing the Ghidra table, same posture as sim_unit_population_remove.h's own
// EVENT_BUILD_PROJECTS_REFRESH (member 7) and sim_unit_notify.h's EVENT_INFO_REFRESH (member 6) --
// no shared generated C++ enum exists yet for game::e::event (checked mh_structs.gen.h and for any
// mh_enums.gen.h-shaped file: absent).
inline constexpr uint32_t EVENT_BUILD_UNITS_REFRESH = 8;

// The two external callees this closure reaches, indirected for offline testability like every other
// sim/ TU's `_calls` struct -- both are ORIGINAL functions outside this migration slice.
struct bldg_notify_state_change_calls {
    // llm_strat_bldg_notify_ui @0x00470bdd. AX=player (uint16_t), EDX=b_index (uint32_t) -- matches
    // mh_calls.gen.h's own signature for this callee exactly (same signature
    // sim_bldg_clear_flag_bit0_notify.h's clear_flag_bit0_notify_calls::notify_ui already uses).
    void (*notify_ui)(uint16_t player, uint32_t building_id);
    // game_SetEvent @0x00413a52.
    uint32_t (*set_event)(uint32_t type);
};

const bldg_notify_state_change_calls &live_bldg_notify_state_change_calls();

namespace detail {

// llm_strat_bldg_notify_state_change @0x00470c5c. See the header banner above.
void notify_state_change(sim_store &own, const bldg_notify_state_change_calls &c, uint16_t player,
                         uint32_t building_id);

} // namespace detail

// Live wrapper: the logic applied to state().own / live_bldg_notify_state_change_calls(). Matches the
// committed prototype (sig_llm_strat_bldg_notify_state_change) exactly.
void notify_state_change(uint16_t player, uint32_t building_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
