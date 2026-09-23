//
// mh_canceltask_export.h -- mp:D28: THE BUILDING DIALOG'S "CANCEL TASK -> YES" APPLIES LOCALLY.
//
// THE PATH. The building info dialog (`llm_ui_dlg_building_construction_status`, 0x004c70b9) offers
// "Are you sure you want to cancel repair / task / upgrade?" (texts 0x301/0x302/0x303) with a
// `&Yes` item (text 0x265) whose callback `llm_ui_building_finish_order_cb` (0x004c703b) calls
// `llm_bldg_finish_current_order(PlayerSide, _G_LLM_UI_SELECTED_BUILDING_INDEX)` DIRECTLY and
// closes the dialog. That callee is the order dispatcher's own "close out the in-progress order"
// hook (its other 8 callers are all arms of llm_strat_order_queue_dispatch's building table): it
// un-consumes a production slot / refunds a project's or an upgrade's resources, unassigns workers
// and sets `buildings[p][i].state = Building[id].state_transition_ids[1]` (the type's idle state).
// Every one of those cells is HASHED (`buildings`, `productions`, `labs`, `player_resources`). On the
// clicking peer they change; on the other peers nothing happens -- the dialog is local UI and no
// order was issued -- so the building keeps working on one side and idles on the other, and the
// match desyncs for good on the next region hash. D25's sibling, found by its root classification.
//
// THE FIX. A CALL-SPLICE on the one instruction: `CALL llm_bldg_finish_current_order` at 0x004c7060
// (E8 46 9C FA FF; EAX = PlayerSide, EDX = the selected building index at that point) is redirected
// to a thunk that, IN A LOCKSTEP MATCH, issues the equivalent BUILDING ORDER instead --
// `llm_strat_order_dispatch(EAX = building index, EDX = PlayerSide | 0x40, EBX = param0 = S,
// ECX = order_code = S)` with S = `Building[building_id].state_transition_ids[1]`, the very state the
// local call would have set. The building table's index-0 arm (`sim_order_dispatch_bldg.cpp`,
// "EVERY param0 the table does not name") runs `llm_bldg_finish_current_order` and then stores
// param0 into `state` on EVERY peer at the order's commit -- the same effect, replicated. Outside a
// lockstep match (single player, a hosted lobby that is not in a match) the thunk calls the original
// so retail behaviour is untouched. The dialog closes either way (the callback's second call).
//
// WHAT CHANGES vs RETAIL IN A MATCH: the cancel lands at the order's commit (one lockstep horizon
// later, not the same frame); the order arm's `energy_gate` / `online_state` refusal applies (an
// offline building's task cannot be cancelled by order -- it could not have been started by one
// either); `cycle_progress` is zeroed by the arm (retail's local path left it, harmless in an idle
// state). Off with `[net] cancel_task_order=0` -- the d28_canceltask_local reproduction arm.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Apply the splice (best-effort: a wrong byte string, a promoted parent or a refused VirtualProtect
// leaves the callback untouched and logs it). Returns 1 when spliced, 0 otherwise. Never fails the
// process.
int MH_CancelTask_Install(void);

#ifdef __cplusplus
}
#endif
