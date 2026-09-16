#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER, UNIT_STATE_PARKED (shared sim/ constants, reused not reinvented)
#include "sim/sim_state.h"

namespace mh::sim {

// The derived discriminant this function's switch dispatches on (rule 17a: no Ghidra enum exists for
// this domain, so this is a LOCAL, DERIVED enum class over the raw opcode byte -- not a Ghidra name).
// Values are the objective op-list's opcode byte; only 2, 7, 8, 9, 10, 11, 12, 13 have a real case in
// switchdataD_004ba9b4 (see the header banner) -- 3, 4, 5, 6 and anything else hit the shared
// default/unmet arm (caseD_1).
enum class tutorial_objective_op : uint8_t {
    select_building_of_type                       = 2,  // caseD_0 @0x004bab74
    unit_of_type_exists                           = 7,  // caseD_5 @0x004babd7
    unit_of_type_not_parked                       = 8,  // caseD_6 @0x004babce
    unit_of_type_state_dead                       = 9,  // caseD_7 @0x004bac3a
    building_of_type_under_construction_or_better = 10, // caseD_8 @0x004baa7d
    building_of_type_placed                       = 11, // caseD_9 @0x004baa6a (falls into caseD_8's shared tail)
    building_placement_id_pending                 = 12, // caseD_a @0x004bab3c
    building_of_type_complete                     = 13, // caseD_b @0x004baa0c
};

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// are frontier originals reached through mh::call:: in live_tutorial_step_driver_calls() -- none are
// among this batch's sibling translations (_CONTEXT_EF.md section 3's callee table for this
// function).
struct tutorial_step_driver_calls {
    uint32_t (*game_set_event)(uint32_t type);                                                        // game_SetEvent @0x00413a52 -- literal 9 == EV_BUILD_OPEN_AUTOPAGE (sim_game_set_event.cpp's own anonymous enum, not reachable from here)
    int32_t (*bldg_first_occupied_unit_slot_has_soldiers)(int32_t building_index);                    // llm_bldg_first_occupied_unit_slot_has_soldiers @0x0049a025
    void (*gfx_draw_cursor_menu)();                                                                   // llm_gfx_draw_cursor_menu @0x004a5b4d
    void *(*gfx_font_desc_for_flags)(uint32_t style_flags);                                           // llm_gfx_font_desc_for_flags @0x004b6155 -- return always discarded here (matches the asm's dead stack store)
    void (*gfx_present_flip)();                                                                       // llm_gfx_present_flip @0x0042644a
    void (*menu_tutorial_uistate_restore)();                                                          // llm_menu_tutorial_uistate_restore @0x004ba683
    void (*strat_ai_start_hq_attack_scenario)();                                                      // llm_strat_ai_start_hq_attack_scenario @0x004ba7e8
    int32_t (*strat_bldg_unassign_workers)(uint16_t player, uint32_t building_index, uint32_t count); // llm_strat_bldg_unassign_workers @0x00491c08
    void (*strat_frame_redraw_behind_dialog)();                                                       // llm_strat_frame_redraw_behind_dialog @0x0043ed98
    void (*strat_input_update)();                                                                     // llm_strat_input_update @0x00441b88
    int32_t (*ui_menu_bg_redraw_cb)();                                                                // llm_ui_menu_bg_redraw_cb @0x004b7ff2 -- return always discarded here
    void (*ui_widget_list_center)(int32_t list_id);                                                   // LIFT-RESID: a LIBMH_SCR_WGTL_* id, not an
                                                                                                      // address (R4)                                                        // llm_ui_widget_list_center @0x004b6fb4
    void (*ui_widget_list_draw)(int32_t list_id);                                                     // LIFT-RESID: a LIBMH_SCR_WGTL_* id (R4)                                                   // llm_ui_widget_list_draw @0x004b7384
};

const tutorial_step_driver_calls &live_tutorial_step_driver_calls();

namespace detail {

// llm_tutorial_step_driver @0x004ba8af. Reads/writes buildings[1] (AI-disable scan, hardcoded
// player) through `own`/`v`'s building_of(); reads buildings[0]/[1] and units[0]/[1] (hardcoded
// players, see the header banner) through `v`'s building_of()/unit_of() -- this function does not
// WRITE any roster record directly, only reads them (the one roster-adjacent effect,
// unassign_workers, is an opaque call through `c`); writes the tutorial's own scalar/widget state
// through `own`; reaches every callee through `c`. void return (the asm's `local_18=0; return
// local_18` is the Watcom fake-return artifact over a genuinely void function).
// RETURNS 0, ALWAYS -- and the return is load-bearing, not a Watcom fake (0x004baefd:
// `MOV [EBP-0x18],0; MOV EAX,[EBP-0x18]`). This body is never CALLED: its only reference in the
// binary is a DATA store at 0x004baf6e installing its VA as _G_LLM_UI_MENU_ASYNC_CALLBACK_A, and
// llm_ui_menu_async_callback_pump @0x004b7bdf invokes the slot then UNINSTALLS it on a nonzero
// return (`TEST EAX,EAX` / `MOV [0x006542ad],0`). 0 means "not done, keep me installed". Ghidra
// typed it `void`, so the generated thunk returned whatever junk was in EAX and the pump
// uninstalled the tutorial driver after its FIRST tick -- the tutorial then never advanced
// again. Prototype corrected 2026-09-09; keep this `int32_t` and keep returning 0.
int32_t tutorial_step_driver(const sim_view &v, sim_store &own, const tutorial_step_driver_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_tutorial_step_driver_calls().
int32_t tutorial_step_driver();

} // namespace mh::sim
