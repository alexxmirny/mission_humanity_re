//
// sim_tutorial_step_driver_selftest.cpp -- `simtest` offline oracle for llm_tutorial_step_driver
// @0x004ba8af (sim/resid/sim_tutorial_step_driver.h/.cpp, RI-SIM / sim_resid batch F).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification, and here the
// reason is stronger than the domain rule: this body reaches SEVEN wall callees with no effect
// disposition (llm_gfx_draw_cursor_menu, llm_gfx_font_desc_for_flags, llm_gfx_present_flip,
// llm_menu_tutorial_uistate_restore, llm_ui_menu_bg_redraw_cb, llm_ui_widget_list_center,
// llm_ui_widget_list_draw), each of which an armed shadow arm would fire TWICE while the state
// comparison read clean.
//
// ONE CLAUSE OF SIM-RESID-F's done_when CANNOT BE MET AS WRITTEN, and this is where that is
// recorded. It asks this oracle to "assert the buildings/units it writes". IT WRITES NEITHER. The
// state matrix used to attribute `units` and `buildings` writes to this function; the 2026-09-01
// session removed them as PHANTOM (dead-ends G76 -- every roster deref here is a `MOVZX ... word
// ptr`, i.e. a READ, and the write verdict came from a mis-paired operand). What the function does
// with the rosters is SCAN them to decide whether an objective is met, so this oracle drives the
// decision from seeded roster records instead: the objective cases below are exercised in BOTH
// directions with the roster as the only difference, which reaches the edges the clause was after.
// The clause is corrected in the tracker rather than quietly satisfied here.
//
// Expected behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_tutorial_step_driver_004ba8af.asm).
//
#include "sim/resid/sim_tutorial_step_driver.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The two widget-list identities the body compares against / stores (its own literal MOV immediates
// at 0x004bacb0/0x004bacf0/0x004baec2/0x004baed6; see the .cpp's own constants).
constexpr uintptr_t WGT_LIST_STEP = 0x00653b17u;
constexpr uintptr_t WGT_LIST_DONE = 0x00653bafu;

// The body hands the CENTER/DRAW calls a LIBMH_SCR_WGTL_* id, not the widget-list address (R4 --
// an address cannot cross the host boundary). The addresses above are what the body STORES into
// ui_menu_widget_list (a libmh-side slot, checked as an address); these are what it PASSES. Two
// encodings of the same two lists, kept apart so a check cannot pass by conflating them.
constexpr uintptr_t WGTL_ID_STEP = 1u; // LIBMH_SCR_WGTL_TUTORIAL_STEP
constexpr uintptr_t WGTL_ID_DONE = 2u; // LIBMH_SCR_WGTL_TUTORIAL_DONE

struct Calls {
    int32_t              set_event = 0;
    std::vector<int32_t> set_event_args;
    int32_t              soldiers_query = 0;
    int32_t              soldiers_ret   = 1;
    int32_t              draw_cursor = 0, font_desc = 0, present_flip = 0;
    int32_t              uistate_restore = 0, hq_attack = 0;
    int32_t              redraw_behind = 0, input_update = 0, bg_redraw = 0;
    int32_t              list_center = 0, list_draw = 0;
    // unassign_workers is the one roster-adjacent EFFECT, so its arguments are recorded verbatim.
    struct Unassign {
        uint16_t player;
        uint32_t index;
        uint32_t count;
    };
    std::vector<Unassign> unassign;
    // The widget-list each center/draw call was handed, as a LIBMH_SCR_WGTL_* id rather than an
    // address (R4). The tail always names the STEP list; the done arm names DONE once, for the
    // list it assigned on the line before.
    std::vector<uintptr_t> list_args;
};
Calls g;

uint32_t stub_set_event(uint32_t t) {
    ++g.set_event;
    g.set_event_args.push_back((int32_t)t);
    return 0;
}
int32_t stub_soldiers(int32_t) {
    ++g.soldiers_query;
    return g.soldiers_ret;
}
void  stub_draw_cursor() { ++g.draw_cursor; }
void *stub_font_desc(uint32_t) {
    ++g.font_desc;
    return nullptr;
}
void    stub_present_flip() { ++g.present_flip; }
void    stub_uistate_restore() { ++g.uistate_restore; }
void    stub_hq_attack() { ++g.hq_attack; }
int32_t stub_unassign(uint16_t p, uint32_t i, uint32_t n) {
    g.unassign.push_back({p, i, n});
    return 0;
}
void    stub_redraw_behind() { ++g.redraw_behind; }
void    stub_input_update() { ++g.input_update; }
int32_t stub_bg_redraw() {
    ++g.bg_redraw;
    return 0;
}
void stub_list_center(int32_t list_id) {
    ++g.list_center;
    g.list_args.push_back((uintptr_t)list_id);
}
void stub_list_draw(int32_t list_id) {
    ++g.list_draw;
    g.list_args.push_back((uintptr_t)list_id);
}

const tutorial_step_driver_calls g_calls = {
    &stub_set_event,
    &stub_soldiers,
    &stub_draw_cursor,
    &stub_font_desc,
    &stub_present_flip,
    &stub_uistate_restore,
    &stub_hq_attack,
    &stub_unassign,
    &stub_redraw_behind,
    &stub_input_update,
    &stub_bg_redraw,
    &stub_list_center,
    &stub_list_draw,
};

// Clears the RECORDS only; soldiers_ret is mock CONFIGURATION and is set by the case. Following
// sim_invasion_due_check_selftest.cpp's own note about a reset that silently reconfigured a case.
void reset_calls(int32_t soldiers_ret) {
    g              = Calls{};
    g.soldiers_ret = soldiers_ret;
}

void run(sim_fixture &fx, int32_t soldiers_ret = 1) {
    reset_calls(soldiers_ret);
    sim_store     own = fx.store();
    const int32_t rc  = detail::tutorial_step_driver(fx.view(), own, g_calls);
    // THE RETURN IS THE CONTRACT, NOT AN AFTERTHOUGHT -- asserted on EVERY case, which is why it
    // lives here rather than in one test. This body is never called: its VA is installed as
    // _G_LLM_UI_MENU_ASYNC_CALLBACK_A and llm_ui_menu_async_callback_pump @0x004b7bdf UNINSTALLS
    // any callback that returns nonzero, so 0 means "keep me installed" and anything else stops
    // the tutorial dead after one tick. That shipped (fixed 2026-09-09, EN v397): the original's
    // real `return 0` was typed `void`, the generated thunk returned garbage EAX, and THIS SUITE
    // WAS ALREADY GREEN because it called the body and discarded the result. A body whose return
    // nothing checks has an untested half.
    ck_eq((uint32_t)rc, 0u,
          "tutorial_step_driver must return 0 -- a nonzero return uninstalls it from the async-callback pump");
}

// Seeds one op into a step's list (reakcje / panel / komenda). Trailing operands are zeroed, so the
// body's NUL-terminated operand walk stops where the case intends.
void put_op(tutorial_step_op *ops, int32_t slot, uint8_t opcode, uint32_t operand0) {
    ops[slot].opcode = opcode;
    for (int32_t i = 0; i < 8; ++i) {
        ops[slot].operands[i] = (i == 0) ? operand0 : 0u;
    }
}

void put_building(sim_fixture &fx, uint32_t player, int32_t idx, uint16_t cfg_id, uint8_t type,
                  uint16_t state, uint16_t workers = 0) {
    building &b                   = fx.buildings[player * BUILDINGS_PER_PLAYER + idx];
    b.building_id                 = cfg_id;
    b.state                       = state;
    b.current_workers             = workers;
    fx.cfg_buildings[cfg_id].type = type;
}

void put_unit(sim_fixture &fx, uint32_t player, int32_t idx, uint16_t proto, uint32_t type,
              uint16_t state) {
    unit &u                  = fx.units[player * UNITS_PER_PLAYER + idx];
    u.unit_proto_id          = proto;
    u.state                  = state;
    fx.cfg_units[proto].type = type;
}

} // namespace

void run_tutorial_step_driver_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the AI-disable scan (0x004ba8c7-0x004ba936). Gated on the one-shot latch being CLEAR and
    // the AI still enabled; scans buildings[1] -- a HARDCODED player-1 roster, NOT PlayerSide-relative
    // -- for an A_MOTHER whose state is not LAND_ACTIVATE(0x85). Three arms, so a body that dropped
    // either gate or read the wrong player fails at least one.
    // =================================================================================================
    {
        fx.reset();
        fx.player_side                      = 4; // deliberately NOT 1: the scan must ignore PlayerSide
        fx.ai_enabled                       = 1;
        fx.tutorial_hq_attack_scenario_done = 0;
        fx.ui_menu_async_callback_b         = (const void *)1; // take the early return; T2 covers it
        put_building(fx, 1, 3, /*cfg*/ 5, BUILDING_TYPE_A_MOTHER, /*state*/ 0x64);
        run(fx);
        ck_eq((uint32_t)fx.ai_enabled, 0u,
              "T1a: an A_MOTHER in buildings[1] not in LAND_ACTIVATE disables the AI, 0x004ba91e");
    }
    {
        fx.reset();
        fx.ai_enabled                       = 1;
        fx.tutorial_hq_attack_scenario_done = 0;
        fx.ui_menu_async_callback_b         = (const void *)1;
        fx.player_side                      = 4;
        put_building(fx, 1, 3, 5, BUILDING_TYPE_A_MOTHER, /*state*/ 0x85); // LAND_ACTIVATE
        put_building(fx, 4, 3, 5, BUILDING_TYPE_A_MOTHER, /*state*/ 0x64); // decoy under PlayerSide
        run(fx);
        ck_eq((uint32_t)fx.ai_enabled, 1u,
              "T1b: a LAND_ACTIVATE mother does not disable the AI, and PlayerSide's roster is not scanned");
    }
    {
        fx.reset();
        fx.ai_enabled                       = 1;
        fx.tutorial_hq_attack_scenario_done = 1; // the one-shot latch is SET
        fx.ui_menu_async_callback_b         = (const void *)1;
        put_building(fx, 1, 3, 5, BUILDING_TYPE_A_MOTHER, 0x64);
        run(fx);
        ck_eq((uint32_t)fx.ai_enabled, 1u, "T1c: the latch suppresses the scan entirely, 0x004ba8c7");
    }

    // =================================================================================================
    // T2 -- the active/idle gate (0x004ba936). A non-null callback slot means "handled elsewhere":
    // restore UI state and RETURN, before the palette copy and before any objective is evaluated. The
    // AI scan ABOVE it still ran (T1a proves it), which is the ordering the asm has and which a naive
    // "early return at the top of the function" translation would lose.
    // =================================================================================================
    {
        fx.reset();
        fx.ui_menu_async_callback_b = (const void *)0x1234;
        fx.tutorial_step            = 1;
        fx.tutorial_step_count      = 4;
        fx.gfx_ui_color[0]          = 999; // must survive: the palette copy is BELOW the gate
        run(fx);
        ck_eq((uint32_t)g.uistate_restore, 1u, "T2: the idle arm calls menu_tutorial_uistate_restore once");
        ck_eq((uint32_t)fx.gfx_ui_color[0], 999u, "T2: the idle arm returns BEFORE the palette copy");
        ck_eq((uint32_t)g.list_draw, 0u, "T2: the idle arm returns before the widget redraw");
        ck_eq((uint32_t)fx.tutorial_step, 1u, "T2: the idle arm cannot advance the step");
    }

    // =================================================================================================
    // T3 -- the palette copy (0x004ba949-0x004ba967). EXACTLY 12 int32 (REP MOVSD x12) out of a
    // FIVE-row source, because the destination is int[4][3]. The fixture's source carries five
    // distinct rows, so a copy that ran 15 would overrun the destination and one that ran 9 would
    // leave the destination tail at its seeded sentinel.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step       = 1;
        fx.tutorial_step_count = 4;
        for (int32_t i = 0; i < 12; ++i) fx.gfx_ui_color[(size_t)i] = -1;
        run(fx);
        bool all = true;
        for (int32_t i = 0; i < 12; ++i) {
            if (fx.gfx_ui_color[(size_t)i] != fx.tutorial_colors_rgb[(size_t)i]) all = false;
        }
        ck(all, "T3: all 12 destination ints take the source palette's first 4 rows, 0x004ba95f");
        ck_eq((uint32_t)fx.tutorial_colors_rgb[12], 51u,
              "T3: the source's FIFTH row is untouched -- the region is 60 B, the copy is 48");
    }

    // =================================================================================================
    // T4 -- THE SCRIPTED STEP SEQUENCE (SIM-RESID-F's done_when). Three scripted steps, each with a
    // DIFFERENT reakcje opcode, driven one frame at a time; the step advances only when that step's
    // objective is actually satisfied by the roster. STEPS EXERCISED: 3 (> 1, as the clause requires),
    // asserted at the end of the block.
    //
    //   step 1: opcode 13 (building_of_type_complete)  -- buildings[1], state == 4
    //   step 2: opcode  8 (unit_of_type_not_parked)    -- units[0], state != PARKED(0x1f)
    //   step 3: opcode  7 (unit_of_type_exists)        -- units[0], ANY state
    //
    // Steps 1 and 2 are driven TWICE: once with the roster NOT satisfying (no advance) and once with
    // it satisfying (advance). That is the both-directions coverage the roster clause was after.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step       = 1;
        fx.tutorial_step_count = 4; // steps 1..3 are real; reaching 4 would be "done"
        fx.player_side         = 0;

        put_op(fx.tutorial_steps[0].reakcje, 0, 13, 0x21);
        put_op(fx.tutorial_steps[1].reakcje, 0, 8, 0x22);
        put_op(fx.tutorial_steps[2].reakcje, 0, 7, 0x23);
        // Step 2's komenda fires the HQ-attack trigger (opcode 0xe) when step 2 is LAID OUT, i.e. on
        // the frame that advances 1 -> 2. Step 3's panel exercises all three panel effects.
        put_op(fx.tutorial_steps[1].komenda, 0, 0xe, 1);
        put_op(fx.tutorial_steps[2].panel, 0, 3, 0x31);
        put_op(fx.tutorial_steps[2].panel, 1, 4, 0x32);
        put_op(fx.tutorial_steps[2].panel, 2, 5, 0);

        int32_t steps_exercised = 0;

        // --- step 1, unmet: a player-1 building of the right type but the WRONG state -------------
        put_building(fx, 1, 2, /*cfg*/ 0x21, /*type*/ 0x21, /*state*/ 5);
        run(fx);
        ck_eq((uint32_t)fx.tutorial_step, 1u,
              "T4/1-unmet: state != 4 leaves the objective pending, no advance");
        ck_eq((uint32_t)g.hq_attack, 0u, "T4/1-unmet: no komenda runs while the step has not advanced");

        // --- step 1, met -> advances to 2, and step 2's komenda fires -----------------------------
        fx.buildings[1 * BUILDINGS_PER_PLAYER + 2].state = 4;
        run(fx);
        ++steps_exercised;
        ck_eq((uint32_t)fx.tutorial_step, 2u,
              "T4/1-met: buildings[1] state==4 satisfies opcode 13, step -> 2");
        ck_eq((uint32_t)g.hq_attack, 1u,
              "T4/1-met: the NEW step's komenda opcode 0xe fires start_hq_attack_scenario once, 0x004bad94");
        ck(fx.ui_tutorial_hint_widget.label ==
               reinterpret_cast<char *>(const_cast<char16_t *>(fx.tutorial_steps[1].body)),
           "T4/1-met: the hint label is the NEW step's .body address (steps[new_step-1]), 0x004bad60");
        ck_eq((uint32_t)fx.tutorial_build_type_filter, 0u,
              "T4/1-met: step 2 has no panel ops, so the filter stays at the reset 0, 0x004bada2");

        // --- step 2, unmet: a unit of the right type but PARKED -----------------------------------
        put_unit(fx, 0, 5, /*proto*/ 0x22, /*type*/ 0x22, UNIT_STATE_PARKED);
        run(fx);
        ck_eq((uint32_t)fx.tutorial_step, 2u, "T4/2-unmet: a PARKED unit does not satisfy opcode 8");

        // --- step 2, met -> advances to 3, and step 3's panel ops all run -------------------------
        fx.units[0 * UNITS_PER_PLAYER + 5].state = 7; // deployed
        run(fx);
        ++steps_exercised;
        ck_eq((uint32_t)fx.tutorial_step, 3u,
              "T4/2-met: a non-PARKED unit of the type satisfies opcode 8, step -> 3");
        ck_eq((uint32_t)fx.tutorial_build_type_filter, 0x31u,
              "T4/2-met: panel opcode 3 writes the build-type filter from operands[0], 0x004bae1a");
        ck_eq((uint32_t)fx.ui_panel_fallback_table[0], 0x32u,
              "T4/2-met: panel opcode 4 appends operands[0] into the fallback table's first zero slot, 0x004bae5c");
        ck_eq((uint32_t)fx.tutorial_rmb_limit_flag, 1u,
              "T4/2-met: panel opcode 5 sets the RMB limit flag, 0x004bae22");
        ck_eq((uint32_t)g.set_event, 2u,
              "T4/2-met: panel opcodes 3 (filter >= 0) and 5 each fire one game_SetEvent, 0x004bae14/0x004bae30");
        ck(g.set_event_args.size() == 2 && g.set_event_args[0] == 9 && g.set_event_args[1] == 9,
           "T4/2-met: both game_SetEvent args are EV_BUILD_OPEN_AUTOPAGE (literal 9)");

        // --- step 3, met immediately: opcode 7 accepts a unit of the type in ANY state -------------
        put_unit(fx, 0, 6, /*proto*/ 0x23, /*type*/ 0x23, UNIT_STATE_PARKED);
        run(fx);
        ++steps_exercised;
        ck_eq((uint32_t)fx.tutorial_step, 4u,
              "T4/3-met: opcode 7 ignores state entirely, so even a PARKED unit satisfies it, step -> 4");

        ck_eq((uint32_t)steps_exercised, 3u,
              "T4: STEPS EXERCISED = 3 (the done_when requires > 1) -- the sequence ran 1 -> 2 -> 3 -> 4");
    }

    // =================================================================================================
    // T5 -- opcode 10/11's shared tail (caseD_8/caseD_9, 0x004baa6a-0x004bab19): the one arm with a
    // side effect, and the one with the A_HELI_SHUTTLE special case. The two opcodes differ in exactly
    // two ways, and BOTH matter here:
    //   * opcode 11 zeroes tutorial_pending_build_placement_id BEFORE scanning (0x004baa6a); opcode 10
    //     does not;
    //   * the state threshold is 0x63 for opcode 11 and 0x64 for opcode 10 (0x004baa74 / 0x004baa7d),
    //     compared with JG at 0x004baaca -- STRICTLY GREATER.
    //
    // AND THAT MAKES THE SIDE EFFECT REACHABLE FROM ONLY ONE OF THEM. The unassign_workers call at
    // 0x004baaf9 is guarded by `state == CONSTRUCTION(0x64)` while the enclosing gate demands
    // `state > threshold`. For opcode 10 that is `state > 0x64 && state == 0x64` -- unsatisfiable, so
    // the side effect is DEAD on that opcode; only opcode 11 (threshold 0x63) can reach it. That is a
    // property of the ORIGINAL, transcribed faithfully, not of the translation. This case was first
    // written against opcode 10, recorded zero unassign calls, and failed -- which is how the dead arm
    // surfaced; it now exercises the opcode that can actually fire it.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step                       = 1;
        fx.tutorial_step_count                 = 4;
        fx.player_side                         = 2;
        fx.tutorial_pending_build_placement_id = 77;
        put_op(fx.tutorial_steps[0].reakcje, 0, 11, 0x15); // 0x15 == A_HELI_SHUTTLE, threshold 0x63
        put_building(fx, 0, 4, /*cfg*/ 0x15, /*type*/ 0x15, /*state*/ 0x64, /*workers*/ 6);
        run(fx, /*soldiers_ret=*/0); // the shuttle has NO soldiers -> does not satisfy
        ck_eq((uint32_t)fx.tutorial_step, 1u,
              "T5a: an A_HELI_SHUTTLE whose first occupied slot has no soldiers does NOT satisfy, 0x004bab0a");
        ck(g.unassign.size() == 1,
           "T5a: unassign_workers fires anyway -- it precedes the satisfaction check, 0x004baaf9");
        if (g.unassign.size() == 1) {
            ck_eq((uint32_t)g.unassign[0].player, 2u, "T5a: unassign's player arg is PlayerSide, 0x004baaeb");
            ck_eq(g.unassign[0].index, 4u, "T5a: unassign's index arg is the roster slot");
            ck_eq(g.unassign[0].count, 5u, "T5a: unassign's count arg is current_workers - 1, 0x004baaf3");
        }
        ck_eq((uint32_t)fx.tutorial_pending_build_placement_id, 0u,
              "T5a: opcode 11 zeroes the pending placement id before scanning, 0x004baa6a");
    }
    {
        // The SAME roster under opcode 10, whose threshold is 0x64: state 0x64 is not > 0x64, so the
        // building is skipped entirely -- no match, no side effect, no advance. This is the arm that
        // makes T5a's threshold claim falsifiable rather than decorative.
        fx.reset();
        fx.tutorial_step                       = 1;
        fx.tutorial_step_count                 = 4;
        fx.tutorial_pending_build_placement_id = 77;
        put_op(fx.tutorial_steps[0].reakcje, 0, 10, 0x15);
        put_building(fx, 0, 4, 0x15, 0x15, /*state*/ 0x64, /*workers*/ 6);
        run(fx, /*soldiers_ret=*/1);
        ck(g.unassign.empty(), "T5b: opcode 10's threshold is 0x64, so state 0x64 never enters the body");
        ck_eq((uint32_t)fx.tutorial_step, 1u, "T5b: ... and the objective stays unmet");
        ck_eq((uint32_t)fx.tutorial_pending_build_placement_id, 77u,
              "T5b: opcode 10 does NOT zero the pending placement id -- only opcode 11 does, 0x004baa6a");
    }
    {
        // Opcode 10's SATISFYING arm: state 0x65 clears the 0x64 threshold, the type is not the
        // shuttle so the soldier query is short-circuited, and the forced selection is cleared.
        fx.reset();
        fx.tutorial_step                  = 1;
        fx.tutorial_step_count            = 4;
        fx.tutorial_forced_bldg_selection = 9;
        put_op(fx.tutorial_steps[0].reakcje, 0, 10, 0x16); // NOT A_HELI_SHUTTLE
        put_building(fx, 0, 4, 0x16, 0x16, /*state*/ 0x65, /*workers*/ 3);
        run(fx, /*soldiers_ret=*/0); // must be ignored: the type gate short-circuits the query
        ck_eq((uint32_t)fx.tutorial_step, 2u, "T5c: state 0x65 > threshold 0x64 satisfies opcode 10");
        ck_eq((uint32_t)g.soldiers_query, 0u,
              "T5c: a non-shuttle type short-circuits the soldier query entirely, 0x004bab00");
        ck_eq((uint32_t)fx.tutorial_forced_bldg_selection, 0u,
              "T5c: the satisfying arm clears the forced building selection, 0x004bab1c");
        ck(g.unassign.empty(), "T5c: state 0x65 is not CONSTRUCTION, so no unassign fires");
    }

    // =================================================================================================
    // T6 -- the DONE arm (0x004bacdc-0x004bad31 + 0x004baed6). Advancing to a step number that is NOT
    // below tutorial_step_count shows the outcome dialog: both outcome widgets take text pointers, the
    // menu widget list becomes the DONE list, and the tail calls uistate_restore INSTEAD of the
    // cursor/flip pair. The list handed to center/draw is still the STEP list either way.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step       = 1;
        fx.tutorial_step_count = 2; // advancing to 2 is NOT < 2 -> done
        fx.game_mode           = 3; // strategic: the cursor/flip pair would fire on the not-done arm
        fx.text_ptrs[0x2b1]    = (const wchar_t *)0xAAAA0001u;
        fx.text_ptrs[0x2b2]    = (const wchar_t *)0xAAAA0002u;
        run(fx);
        ck_eq((uint32_t)fx.tutorial_step, 2u, "T6: the step still advances on the done arm, 0x004bacc9");
        ck((uintptr_t)fx.ui_menu_widget_list == WGT_LIST_DONE,
           "T6: the menu widget list becomes _G_LLM_UI_WGT_LIST_TUTORIAL_DONE, 0x004bacf0");
        ck(fx.ui_outcome_dlg_title_widget.label == (char *)0xAAAA0002u,
           "T6: the outcome TITLE widget takes text_ptrs[0x2b2], 0x004bacdc");
        ck(fx.ui_outcome_dlg_message_widget.label == (char *)0xAAAA0001u,
           "T6: the outcome MESSAGE widget takes text_ptrs[0x2b1]");
        ck_eq((uint32_t)g.uistate_restore, 1u, "T6: the done tail restores UI state, 0x004baedd");
        ck_eq((uint32_t)g.draw_cursor, 0u, "T6: the done tail does NOT draw the cursor");
        ck_eq((uint32_t)g.present_flip, 0u, "T6: the done tail does NOT flip");
        // THREE list calls, in this order: the done arm centres the DONE list it has just installed
        // (0x004bacfa), then the unconditional tail centres and draws the STEP list
        // (0x004baec2/0x004baecc). The tail does NOT follow the installed list -- it uses its own
        // literal -- which is the distinction this check exists for.
        ck(g.list_args.size() == 3 && g.list_args[0] == WGTL_ID_DONE &&
               g.list_args[1] == WGTL_ID_STEP && g.list_args[2] == WGTL_ID_STEP,
           "T6: center(DONE) on the done arm, then the tail's center/draw on the STEP list");
    }

    // =================================================================================================
    // T7 -- the tail: the modal-dialog bit, the forced-selection re-apply, and the strategic-mode
    // cursor/flip. None of it is reached by the objective loop, so a body that returned early after
    // advancing would lose all of it silently.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step                  = 1;
        fx.tutorial_step_count            = 4;
        fx.game_mode                      = 3;
        fx.dlg_state_flags                = 0;
        fx.ui_menu_widget_list            = nullptr; // -> input_update + clear bit 0x10
        fx.tutorial_forced_bldg_selection = 6;
        fx.ui_selected_bldg_index         = 2;
        run(fx);
        ck_eq((uint32_t)g.input_update, 1u, "T7a: a null widget list runs strat_input_update, 0x004bae7c");
        ck_eq((uint32_t)(fx.dlg_state_flags & 0x10), 0u, "T7a: ... and clears the modal bit, 0x004bae86");
        ck_eq((uint32_t)fx.ui_selected_bldg_index, 6u,
              "T7a: a pending forced selection differing from the current one is re-applied, 0x004baeb2");
        ck_eq((uint32_t)g.redraw_behind, 1u, "T7a: the frame redraw runs once");
        ck_eq((uint32_t)g.bg_redraw, 1u, "T7a: the menu-backdrop callback runs once");
        ck_eq((uint32_t)g.draw_cursor, 1u, "T7a: strategic mode draws the cursor, 0x004baef1");
        ck_eq((uint32_t)g.present_flip, 1u, "T7a: ... and flips, 0x004baef6");
    }
    {
        fx.reset();
        fx.tutorial_step       = 1;
        fx.tutorial_step_count = 4;
        fx.game_mode           = 6; // NOT strategic
        fx.dlg_state_flags     = 0;
        fx.ui_menu_widget_list = reinterpret_cast<widget_list *>(WGT_LIST_STEP); // non-null, not DONE
        run(fx);
        ck_eq((uint32_t)g.input_update, 0u, "T7b: a non-null widget list skips strat_input_update");
        ck_eq((uint32_t)(fx.dlg_state_flags & 0x10), 0x10u, "T7b: ... and SETS the modal bit, 0x004bae8e");
        ck_eq((uint32_t)g.draw_cursor, 0u, "T7b: outside strategic mode the cursor is not drawn, 0x004baee9");
        ck_eq((uint32_t)g.present_flip, 0u, "T7b: ... and there is no flip");
    }

    // =================================================================================================
    // T8 -- the DEFAULT arm. Opcodes 3/4/5/6 are IN RANGE of the bounds check but have no case in
    // switchdataD_004ba9b4: they alias caseD_1, the same merge label an out-of-range opcode reaches,
    // and leave the objective PENDING (the pre-switch increment stands). A translation that treated
    // an unknown opcode as "satisfied" would advance the step on every one of these.
    // =================================================================================================
    {
        const uint8_t no_case_opcodes[] = {3, 4, 5, 6, 1, 20};
        for (uint8_t opcode : no_case_opcodes) {
            fx.reset();
            fx.tutorial_step       = 1;
            fx.tutorial_step_count = 4;
            put_op(fx.tutorial_steps[0].reakcje, 0, opcode, 0x40);
            run(fx);
            ck_eq((uint32_t)fx.tutorial_step, 1u,
                  "T8: an opcode with no case leaves the objective unmet (default arm, caseD_1)");
        }
    }

    // =================================================================================================
    // T9 -- an EMPTY objective list leaves `pending` at 0, so the step advances on the first frame.
    // This is the vacuous-pass shape every case above has to be read against: T4's advances happen
    // because the objective was satisfied, not because the list was empty.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_step       = 1;
        fx.tutorial_step_count = 4; // reakcje[0].opcode == 0 -> the walk stops immediately
        run(fx);
        ck_eq((uint32_t)fx.tutorial_step, 2u, "T9: an empty objective list is vacuously satisfied, 0x004ba996");
    }
}

} // namespace mh::sim::test
