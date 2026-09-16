//
//
// sim/resid/sim_tutorial_step_driver.cpp -- see sim_tutorial_step_driver.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_tutorial_step_driver_004ba8af.asm), the Ghidra .c being a
// draft (see the header's "on trusting the .c" note).
//
#include "sim/resid/sim_tutorial_step_driver.h"

#include "addr/mh_calls.gen.h" // typed callables for the effectful/frontier originals we still call OUT to
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const tutorial_step_driver_calls &live_tutorial_step_driver_calls() {
    static const tutorial_step_driver_calls c = {
        MH_LIBMH_BIND(game_SetEvent),
        MH_LIBMH_BIND(llm_bldg_first_occupied_unit_slot_has_soldiers),
        mh::state::evt::cursor_menu_draw,
        mh::state::evt::font_desc_for_flags,
        mh::state::evt::present_flip,
        mh::state::evt::tut_uistate_restore,
        MH_LIBMH_BIND(llm_strat_ai_start_hq_attack_scenario),
        MH_LIBMH_BIND(llm_strat_bldg_unassign_workers),
        MH_LIBMH_BIND(llm_strat_frame_redraw_behind_dialog),
        mh::host().apply_frame_input,
        mh::state::evt::menu_bg_redraw,
        // R4: these two now take a LIBMH_SCR_WGTL_* id, not a widget-list address -- the member
        // types changed with them (see the header), so these are the two binder lines in this
        // struct that are NOT signature-identical swaps.
        mh::state::evt::wgtl_center,
        mh::state::evt::wgtl_draw,
    };
    return c;
}

namespace {

// llm_strat_bldg_state members this function reads (0x004ba914/0x004baad1/0x004baade), transcribed
// from the disassembly's CMP immediates -- same bare-uint16_t-field reasoning, and the same VALUES,
// as sim_bldg_add_workers.cpp's/sim_advisor_tick.cpp's own BLDG_STATE_* blocks; not shared with them
// (anonymous-namespace, different TU) so redeclared here.
inline constexpr uint16_t BLDG_STATE_CONSTRUCTION  = 0x64;
inline constexpr uint16_t BLDG_STATE_LAND_ACTIVATE = 0x85;

// cfg_enum_E_UNIT_TYPE value 0x15 = A_HELI_SHUTTLE. The translating session could only see Ghidra's
// per-function decompile annotation and rightly refused to assert the name on that alone; the
// conductor read the enum itself (category /Manual/cfg/enum, i.e. HAND-named, not an LLM guess) and
// it corroborates -- 0x13/0x14 are A_/H_HELI_MOTHER, 0x15/0x16 A_/H_HELI_SHUTTLE, 0x17/0x18
// A_/H_HELI_CARGO, three consecutive alien/human pairs. Kept local per the same "redeclare per-TU"
// precedent as the BLDG_STATE_* constants above rather than widened into sim_order_enqueue.h's
// UNIT_TYPE_* block, which no other TU in this closure needs.
inline constexpr int32_t UNIT_TYPE_A_HELI_SHUTTLE = 0x15;

// The step's hint-widget label. The asm computes `_G_LLM_TUTORIAL_STEPS + (step-1)*0x60c + 0x80`
// and stores that ADDRESS into llm_ui_widget.label (0x004bad1e-0x004bad2e, 0x004bad50-0x004bad60) --
// an address escape, never a dereference, so the pointee type never matters to this function.
// llm_tutorial_step.body is an INLINE char16_t[512] at +0x80 (Ghidra: wchar_t[512]), while .label is
// a char*, so the cast is the widget field's type, not a reinterpretation of the text: this codebase
// already stores wide pointers through .label elsewhere (see the text_ptrs stores below).
inline char *tutorial_body_label(const sim_view &v, int32_t step) {
    return reinterpret_cast<char *>(const_cast<char16_t *>(v.tutorial_steps[step - 1].body));
}

// The tutorial-owned widget-list IDENTITY addresses this function compares against and stores --
// see the header's "literal CONSTANT-POINTER stores" note (same shape as sim_start_tutorial.cpp's
// own _G_LLM_UI_WGT_LIST_TUTORIAL_WELCOME/_INTRO literals).
inline constexpr uintptr_t WGT_LIST_TUTORIAL_STEP_ADDR = 0x00653b17u; // _G_LLM_UI_WGT_LIST_TUTORIAL_STEP
inline constexpr uintptr_t WGT_LIST_TUTORIAL_DONE_ADDR = 0x00653bafu; // _G_LLM_UI_WGT_LIST_TUTORIAL_DONE

} // namespace

namespace detail {

// ---- llm_tutorial_step_driver @0x004ba8af ------------------------------------------------------
int32_t tutorial_step_driver(const sim_view &v, sim_store &own, const tutorial_step_driver_calls &c) {
    // 0x004ba8c7-0x004ba936: AI-disable scan. Runs unconditionally of the active/idle gate below.
    // buildings[1] is a HARDCODED player-1 roster (0xc43d44 == building_of base + 1*0x6aa4 +
    // 0*0x111 -- see header banner derivation), not PlayerSide-relative; preserved literally.
    if (own.tutorial_hq_attack_scenario_done() == 0 && own.ai_enabled_mut() != 0) {
        for (int32_t i = 0; i < v.caps.buildings; ++i) {
            const building &b = building_of(v, 1, i);
            if (v.cfg_buildings[b.building_id].type == BUILDING_TYPE_A_MOTHER && b.state != BLDG_STATE_LAND_ACTIVATE) {
                own.ai_enabled_mut() = 0; // 0x004ba91e
                break;
            }
        }
    }

    // 0x004ba936-0x004ba949: the active/idle gate. _G_LLM_UI_MENU_ASYNC_CALLBACK_B -- a genuine
    // dynamic read (not an address escape), non-null means "already handled elsewhere this frame",
    // so just restore tutorial UI state and return.
    if (*v.ui_menu_async_callback_b != nullptr) {
        c.menu_tutorial_uistate_restore();
        return 0; // every asm path converges on the single epilogue at 0x004baefd, which returns 0
    }

    // 0x004ba949-0x004ba967: restamp the 4-colour UI palette from the tutorial's own source palette.
    // ONE 48-byte block copy (REP MOVSD x12, REP MOVSB x0 -- 0x30 is exactly divisible by 4, no
    // remainder byte). The SOURCE region is int[5][3] = 60 bytes; only the first 4 rows are copied,
    // because the destination is int[4][3]. The fifth row is not read here and is not a mistake.
    for (int32_t i = 0; i < 12; ++i) {
        own.gfx_ui_color_at(i) = v.tutorial_colors_rgb[i];
    }

    // 0x004ba968-0x004bacaa: evaluate the CURRENT step's objective list. `step` is read once, before
    // any increment -- the objectives list this loop walks belongs to the step BEFORE this frame's
    // possible advance (own.tutorial_step_mut() below only mutates AFTER this loop finishes).
    const int32_t step    = own.tutorial_step_mut();
    int32_t       pending = 0; // EBP-0x20 in the asm; incremented once per (objective,type) pair, decremented at most once per pair on a match

    for (int32_t oi = 0; oi < 4; ++oi) { // tutorial_steps[step - 1].reakcje[4] ("Reakcje", the completion conditions)
        const tutorial_step_op &obj = v.tutorial_steps[step - 1].reakcje[oi];
        if (obj.opcode == 0) break; // 0x004ba996/0x004ba999 -- NUL-terminated list

        for (int32_t ti = 0; obj.operands[ti] != 0; ++ti) { // 0x004ba9a6-0x004ba9ac -- NUL-terminated type list; no extra bound, matching the asm's unbounded walk (see header uncertainty)
            const int32_t target_type = obj.operands[ti];
            ++pending; // 0x004ba9e4-0x004ba9e7, unconditional

            switch (static_cast<tutorial_objective_op>(obj.opcode)) {
                case tutorial_objective_op::select_building_of_type: { // caseD_0 @0x004bab74
                    const building &sel = building_of(v, static_cast<uint32_t>(*v.player_side),
                                                      static_cast<int32_t>(own.ui_selected_bldg_index()));
                    if (v.cfg_buildings[sel.building_id].type == target_type && own.ui_selected_bldg_index() != 0) {
                        own.tutorial_forced_bldg_selection() = own.ui_selected_bldg_index(); // 0x004babbe
                        --pending;                                                           // 0x004babc6
                    }
                    break;
                }
                case tutorial_objective_op::unit_of_type_exists: { // caseD_5 @0x004babd7 -- units[0], any state (target -1 never equals a zero-extended state)
                    for (int32_t i = 0; i < v.caps.units; ++i) {
                        const unit &u = unit_of(v, 0, i);
                        if (v.cfg_units[u.unit_proto_id].type == static_cast<uint32_t>(target_type)) {
                            --pending; // 0x004bac27
                            break;
                        }
                    }
                    break;
                }
                case tutorial_objective_op::unit_of_type_not_parked: { // caseD_6 @0x004babce -- units[0], state != UNIT_STATE_PARKED(0x1f)
                    for (int32_t i = 0; i < v.caps.units; ++i) {
                        const unit &u = unit_of(v, 0, i);
                        if (v.cfg_units[u.unit_proto_id].type == static_cast<uint32_t>(target_type) && u.state != UNIT_STATE_PARKED) {
                            --pending; // 0x004bac27
                            break;
                        }
                    }
                    break;
                }
                case tutorial_objective_op::unit_of_type_state_dead: { // caseD_7 @0x004bac3a -- units[1], state == 4
                    for (int32_t i = 0; i < v.caps.units; ++i) {
                        const unit &u = unit_of(v, 1, i);
                        if (v.cfg_units[u.unit_proto_id].type == static_cast<uint32_t>(target_type) && u.state == 4) {
                            --pending; // 0x004bac81
                            break;
                        }
                    }
                    break;
                }
                case tutorial_objective_op::building_of_type_placed: // caseD_9 @0x004baa6a -- falls into caseD_8's shared tail (LAB_004baa84)
                    own.tutorial_pending_build_placement_id() = 0;   // 0x004baa6a
                    [[fallthrough]];
                case tutorial_objective_op::building_of_type_under_construction_or_better: { // caseD_8 @0x004baa7d
                    const bool is_placed_variant = static_cast<tutorial_objective_op>(obj.opcode) ==
                                                   tutorial_objective_op::building_of_type_placed;
                    const int32_t state_threshold = is_placed_variant ? 0x63 : static_cast<int32_t>(BLDG_STATE_CONSTRUCTION);
                    for (int32_t i = 0; i < v.caps.buildings; ++i) { // buildings[0], hardcoded player 0
                        const building &b = building_of(v, 0, i);
                        if (v.cfg_buildings[b.building_id].type == target_type && b.state > state_threshold &&
                            b.state != BLDG_STATE_LAND_ACTIVATE) {
                            if (b.state == BLDG_STATE_CONSTRUCTION) { // 0x004baadb-0x004baae3
                                c.strat_bldg_unassign_workers(static_cast<uint16_t>(*v.player_side), static_cast<uint32_t>(i),
                                                              static_cast<uint32_t>(b.current_workers) - 1u); // 0x004baaf9
                            }
                            if (target_type != UNIT_TYPE_A_HELI_SHUTTLE ||
                                c.bldg_first_occupied_unit_slot_has_soldiers(b.building_id) != 0) {
                                own.tutorial_forced_bldg_selection() = 0; // 0x004bab1c
                                --pending;                                // 0x004bab19
                                break;
                            }
                        }
                    }
                    break;
                }
                case tutorial_objective_op::building_placement_id_pending: { // caseD_a @0x004bab3c
                    const int32_t placement_id = own.build_placement_id();
                    if (placement_id != 0 && v.cfg_buildings[placement_id].type == target_type) {
                        own.tutorial_pending_build_placement_id() = placement_id; // 0x004bab64
                        --pending;                                                // 0x004bab6c
                    }
                    break;
                }
                case tutorial_objective_op::building_of_type_complete: { // caseD_b @0x004baa0c -- buildings[1], state == 4
                    for (int32_t i = 0; i < v.caps.buildings; ++i) {
                        const building &b = building_of(v, 1, i);
                        if (v.cfg_buildings[b.building_id].type == target_type && b.state == 4) {
                            --pending; // 0x004baa54
                            break;
                        }
                    }
                    break;
                }
                default:
                    // opcodes 3, 4, 5, 6 (in-range, no case in switchdataD_004ba9b4) and anything the
                    // bounds check rejects (opcode 0, 1, or >= 14) all reach the SAME merge label
                    // (caseD_1 @0x004bac92) -- a real, reachable default arm that leaves the objective
                    // unmet (`pending` stays incremented from the pre-switch increment above).
                    break;
            }
        }
    }

    // 0x004bacaa-0x004baeb8: if every objective in the current step is met AND the tutorial-done
    // widget list is not already the active one, advance to the next step (or show the done dialog).
    if (pending == 0 && own.ui_menu_widget_list() != reinterpret_cast<widget_list *>(WGT_LIST_TUTORIAL_DONE_ADDR)) {
        const int32_t new_step = ++own.tutorial_step_mut(); // 0x004bacc9-0x004baccf

        if (new_step < *v.tutorial_step_count) {
            // 0x004bad36-0x004bad7b: lay out the NEW step's hint widget (font-desc return discarded,
            // matching the asm's dead stack store) and its body text (address escape, see header).
            c.gfx_font_desc_for_flags(own.ui_tutorial_hint_widget().flags);
            own.ui_tutorial_hint_widget().label = tutorial_body_label(v, new_step);

            // 0x004bad7e-0x004bada2: the new step's komenda list -- opcode 0xe fires the HQ-attack
            // scenario script trigger; NUL-terminated (opcode 0 stops the walk).
            for (int32_t ki = 0; ki < 4; ++ki) { // tutorial_steps[...].komenda[4] ("Komenda", the scripted AI commands)
                const tutorial_step_op &k = v.tutorial_steps[new_step - 1].komenda[ki];
                if (k.opcode == 0) break;
                if (k.opcode == 0xe) {
                    c.strat_ai_start_hq_attack_scenario();
                }
            }

            // 0x004bada2-0x004badce: reset the two UI-limit scalars, then walk the new step's panel
            // list ("Panel"), NUL-terminated.
            own.tutorial_build_type_filter() = 0;
            own.tutorial_rmb_limit_flag()    = 0;

            for (int32_t pi = 0; pi < 4; ++pi) {
                const tutorial_step_op &p = v.tutorial_steps[new_step - 1].panel[pi];
                if (p.opcode == 0) break;

                if (p.opcode < 4) {
                    if (p.opcode == 3) { // 0x004badfc-0x004bae20
                        own.tutorial_build_type_filter() = p.operands[0];
                        if (own.tutorial_build_type_filter() >= 0) {
                            c.game_set_event(9); // EV_BUILD_OPEN_AUTOPAGE (sim_game_set_event.cpp's local enum, value cited by literal)
                        }
                    }
                    // opcode 0, 1, 2: no-op (0x004bae00 JNZ falls straight through to continue)
                } else if (p.opcode == 4) { // 0x004bae38-0x004bae62 -- append into the panel fallback table's first free (zero) slot
                    int32_t slot = 0;
                    while (own.ui_panel_fallback_table_at(slot) != 0) {
                        ++slot;
                    }
                    own.ui_panel_fallback_table_at(slot) = p.operands[0];
                } else if (p.opcode == 5) { // 0x004bae22-0x004bae36
                    own.tutorial_rmb_limit_flag() = 1;
                    c.game_set_event(9); // EV_BUILD_OPEN_AUTOPAGE
                }
                // opcode > 5: no-op (0x004badf7 JMP straight to continue)
            }
        } else {
            // 0x004bacdc-0x004bad31: the tutorial is DONE -- show the outcome dialog reusing the
            // mission-outcome modal's title/message widgets, swap in the done widget list, and
            // re-stamp the (now past-the-end) step's hint label the same way as the in-progress arm.
            own.ui_outcome_dlg_title_widget().label   = reinterpret_cast<char *>(const_cast<wchar_t *>(v.text_ptrs[0x2b2]));
            own.ui_outcome_dlg_message_widget().label = reinterpret_cast<char *>(const_cast<wchar_t *>(v.text_ptrs[0x2b1]));
            own.ui_menu_widget_list()                 = reinterpret_cast<widget_list *>(WGT_LIST_TUTORIAL_DONE_ADDR);
            // The original centres `*ui_menu_widget_list()`, which the line above just set to the
            // DONE list -- so the argument is provably that constant and crosses as its id (R4).
            c.ui_widget_list_center(LIBMH_SCR_WGTL_TUTORIAL_DONE);
            c.gfx_font_desc_for_flags(own.ui_tutorial_hint_widget().flags); // return discarded
            own.ui_tutorial_hint_widget().label = tutorial_body_label(v, new_step);
        }
    }

    // 0x004bae74-0x004bae92: the modal-dialog-active bit (bit 0x10 of the low byte -- the asm's
    // `AND byte,0xef` / `OR byte,0x10` only ever touch that one bit, so a full-width &=/|= is
    // equivalent).
    if (own.ui_menu_widget_list() == nullptr) {
        c.strat_input_update();
        own.dlg_state_flags() &= ~0x10;
    } else {
        own.dlg_state_flags() |= 0x10;
    }

    // 0x004bae92-0x004baeb8: re-apply a forced building selection if one is pending and differs from
    // the current selection.
    if (own.tutorial_forced_bldg_selection() != 0 &&
        own.ui_selected_bldg_index() != own.tutorial_forced_bldg_selection()) {
        own.ui_selected_bldg_index() = static_cast<uint16_t>(own.tutorial_forced_bldg_selection());
    }

    // 0x004baeb8-0x004baed6: redraw the frame/backdrop and the step widget list (constant pointer,
    // see header).
    c.strat_frame_redraw_behind_dialog();
    c.ui_menu_bg_redraw_cb(); // return discarded
    c.ui_widget_list_center(LIBMH_SCR_WGTL_TUTORIAL_STEP);
    c.ui_widget_list_draw(LIBMH_SCR_WGTL_TUTORIAL_STEP);

    // 0x004baed6-0x004baefd: done -> restore tutorial UI state; still running in strategic mode ->
    // draw cursor + flip.
    if (own.ui_menu_widget_list() == reinterpret_cast<widget_list *>(WGT_LIST_TUTORIAL_DONE_ADDR)) {
        c.menu_tutorial_uistate_restore();
    } else if (own.game_mode() == 3) {
        c.gfx_draw_cursor_menu();
        c.gfx_present_flip();
    }

    // 0x004baefd-0x004baf04: return 0. See the header -- the async-callback pump uninstalls this
    // driver on any nonzero return, so this constant is what keeps the tutorial ticking.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t tutorial_step_driver() {
    sim_state st = state();
    return detail::tutorial_step_driver(st.read, st.own, live_tutorial_step_driver_calls());
}

} // namespace mh::sim
