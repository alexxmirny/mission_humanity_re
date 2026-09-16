//
// ai/ai_site_dispatch.h -- pick the site scanner for a building TYPE (RI-AI / AI1B, layer 4).
//
// llm_strat_ai_bldg_production_type_dispatch @0x004e7cec, 235 bytes. Its caller
// (llm_strat_ai_bldg_queue_process_entry, reading @0x004e7e22) then takes
// _G_LLM_STRAT_AI_SITE_CANDIDATE_COUNT as the success flag and candidate [0] as the nearest tile.
//
// THIS FUNCTION OWNS THE RESET. `*site_candidate_count = 0` @0x004e7cfa is the ONLY constant store
// to that global anywhere in the image -- of its fifteen references the three scanners contribute
// only reads and INCs (0x004e6502, 0x004e6673, 0x004e682a). The scanners are APPEND-ONLY, which is
// what ai_state.h's ai_store comment has said since batch A layer 5 and what the Ghidra plate
// contradicted until 2026-08-03 (ghidra_findings 2026-08-03-1505-2). Dropping the store on the
// theory that a callee does it would leave the candidate list growing across AI ticks.
//
// THE DISPATCH is three race-paired cfg Building.type tests, each re-reading
// player_data[player].is_alien_race (0x004e7d16 / 0x004e7d5d / 0x004e7d98) and comparing against
// Building[building_id].type (0x004e7d2c / 0x004e7d73 / 0x004e7dae). The constants come from the
// game's own cfg::enum::E_BUILDING:
//
//     MINE      2 / 0x16  -> llm_strat_ai_bldg_scan_resource_site_candidates   (0x004e7d3f)
//     GARAGE    8 / 0x1c  -> llm_strat_ai_scan_build_site_candidates           (0x004e7dc1)
//     BARRACKS  7 / 0x1b  -> the same scanner (both branches converge on LAB_004e7dbf)
//     otherwise           -> llm_strat_ai_bldg_scan_grid_candidates            (0x004e7dca)
//
// then llm_strat_ai_sort_site_candidates_by_dist -- an ORDINARY call @0x004e7dcf followed by
// POP ECX / POP EBX / RET, not the tail call the old plate described.
//
// THE SCANNERS' SECOND ARGUMENT IS PASSED IMPLICITLY IN EDX, and this is the detail Ghidra's C
// cannot show. All three have a committed two-register __watcall prototype, yet every call site
// loads only EAX (MOV EAX,EBX @0x004e7d3d / 0x004e7dbf / 0x004e7dc8). EDX still carries this
// function's own `building_id` -- the body writes EAX, EBX and ECX only, and the three IMULs merely
// READ EDX -- so each scanner receives building_id without any site setting it. Passed explicitly
// below, because a C++ call has no ambient EDX to inherit.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

void bldg_production_type_dispatch(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t building_id);

} // namespace detail

void bldg_production_type_dispatch(uint32_t player_id, int32_t building_id);

} // namespace mh::ai
