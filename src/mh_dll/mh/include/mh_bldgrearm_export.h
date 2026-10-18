//
// mh_bldgrearm_export.h -- mods:D11: SHIFT+CLICK KEEPS THE JUST-PLACED BUILDING TYPE ARMED.
//
// THE PATH. The HUD build tab arms placement through `llm_strat_bldg_try_begin_placement` (0x00448c0b),
// which writes the type into `_G_LLM_BUILD_PLACEMENT_ID` (0x005d01dc). The LMB-down in the map view
// (`llm_strat_input_update`, 0x00442de5..) checks the footprint, issues the place order 0xea and sets
// `_G_LLM_STRAT_BUILD_PREVIEW_SUPPRESS_FLAG`; the LMB-UP then clears BOTH
// (`if (SUPPRESS) { BUILD_PLACEMENT_ID = 0; SUPPRESS = 0; }`, the 10-byte `mov [0x005d01dc],0` at
// 0x00443ae9) -- so the type is gone after every placement and the player re-picks it from the tab.
//
// THE CHANGE. That one 10-byte store (C7 05 DC 01 5D 00 00 00 00 00) becomes CALL rel32 + 5 NOPs to a
// thunk that keeps the id when ALL hold: `[ui] shift_rearm_build` is on, the tutorial is not running
// (`_G_LLM_GAME_TUTORIAL_STEP` == 0), a Shift key is held at the LMB-up, the local player still has a
// primary mothership building on this planet and the NEXT copy is affordable (a READ-ONLY copy of
// llm_bldg_pay_build_cost's pass 1, shared with seams/ui_bldg_build_probe.cpp). Otherwise it stores 0
// exactly like the stock instruction; when the chain stops for cost or an invention gate it prints the
// same refusal text the arm click prints. Without Shift the behaviour is byte-for-byte retail.
//
// WHAT IT NEVER DOES. It does not call llm_strat_bldg_try_begin_placement: that function's pay + regrant
// probe books +cost into the hashed resource_spent counter on the clicking peer alone (mp:D25/D35,
// dead-ends G246), live under [config] mode=original. The re-arm is a write of a MF_VIEW global; the
// orders are the same 0xea orders a player re-picking would issue, so nothing new crosses the wire.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Patch the LMB-up clear (best-effort: ini off, a wrong byte string, a promoted owner or a refused
// VirtualProtect leaves it stock and logs it). Returns 1 when armed. Never fails the process.
int MH_BldgRearm_Install(void);

#ifdef __cplusplus
}
#endif
