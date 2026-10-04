#pragma once
// mh_infoguard_export.h -- mp:U73: THE ENTITY INFO SCREEN IS NEVER OPENED FOR AN ENTITY THAT CAN ONLY
// PRODUCE "Error: Cannot find info text:".
//
// THE FIELD REPORT (2026-10-04, rc6): opening the info panel of an alien spaceport showed the modal
// "Error: Cannot find info text:" with an EMPTY key. llm_ui_entity_info_screen_open (EN 0x004cad31,
// __watcall EAX = entity_id, EDX = kind; UNIT 0 / BUILDING 1 / PROJECT 2) reads
// {Unit,Building,Projects}[entity_id].info_txt, searches the INFO.Txt index and, on no match, raises
// that modal. Every configured record has a non-empty INFO_TXT, so an empty key means the id was a
// BLANK SLOT (id 0 / unconfigured), not a missing text.
//
// THE ROOT CAUSE (mp:U73). Seven callers turn a list ROW into an id
// with no bounds check against the list as it is NOW. For the docked-unit info on a storage building
// (llm_strat_ui_storage_bldg_panel, call at 0x0041796c) the row is captured by a widget callback
// (llm_ui_storage_panel_request_show_info_slot -> _G_LLM_UI_STORAGE_PANEL_SHOW_INFO_SLOT_REQUEST =
// row+1) and CONSUMED on a later panel tick as
// units[side][unit_storage[side][slot].docked_units[row + scroll - 1]].unit_proto_id. Between the two
// the docked list can shrink: a unit launches or dies, and in a LOCKSTEP match the panel's own
// dead-docked purge is skipped (D38 row 5, ui_storage_panel_purge.cpp) so the SIM's sub-tick purge
// compacts the list on its own schedule. docked_units[row-1] then names a stale or zero slot ->
// units[side][0].unit_proto_id == 0 -> Unit[0].info_txt == "" -> the modal with an empty key.
//
// THE FIX, in two layers (both mh.dll, UI only, no libmh / sim change):
//   1. THE CONSUME SITE (storage panel). The 7-byte `CMP [REQUEST],0` at 0x00417902 becomes
//      `CALL consume_thunk` + NOPs; the thunk validates the latched request against the docked list as it
//      is NOW (row inside docked_count, the docked slot inside the roster, the unit's prototype
//      non-zero) and clears the latch when it does not, so retail's own JZ skips the open. One
//      `; [info] guard: storage-panel info request DROPPED ...` line names why.
//   2. THE ENTRY (every caller). A trampoline hook on llm_ui_entity_info_screen_open checks kind, the
//      id against the 100-deep cfg tables, the resolved key (incl. the PROJECT fallback, which borrows a
//      dependent building/unit's key) for emptiness, and the key's presence in the INFO.Txt index once
//      it has been built. A failing call returns at once: no screen, no modal, nothing touched (the
//      function's first act is to change the game mode state, so refusing before it is the only clean
//      refusal). One `; [info] guard: entity info open REFUSED kind=.. id=.. reason=.. key=".." caller=..`
//      line names the caller's return address and the panels' scroll/count state.
//
// KNOBS. [net] info_guard (default 1; 0 = no hook, the reproduction arm; not under [config]
// mode=original). RIG-ONLY: info_guard_test_entry=N corrupts the N-th call that reaches the entry (to
// the blank UNIT 0), info_guard_test_row=N pushes the N-th storage request 40 rows past the list.
// Both are 0 (off) in every shipped ini. Best-effort: a hook or patch that is refused is logged.
#ifdef __cplusplus
extern "C" {
#endif
int MH_InfoGuard_Install(void);
#ifdef __cplusplus
}
#endif
