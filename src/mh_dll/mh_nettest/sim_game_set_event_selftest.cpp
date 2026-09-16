//
// sim_game_set_event_selftest.cpp -- `simtest` offline oracle for game_SetEvent
// (sim/sim_game_set_event.{h,cpp}, RI-SIM / SIM1F batch F). game_SetEvent is the
// strategic-HUD panel/notification dispatcher: a dense 24-case switch that sets UI-panel-state
// globals, a deferral ring-buffer path, a tutorial-mode suppression, and a trailing fallback-table
// pass. Pure decision logic over UI state with only two outward calls, so it is fully coverable
// offline -- every switch arm plus the defer/tutorial/fallback tails are exercised below.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/game_SetEvent_00413a52.asm), not the
// .cpp. reset() zeroes all UI state, so each case starts at panel mode 0 / page 0 and an empty
// (all-zero, inert) fallback table -- which is why the applied path returns 0 (fallback i*4 = 0)
// unless a case seeds the fallback table or the defer flag.
//
#include "sim/sim_game_set_event.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The two outward callees, recorded.
std::vector<int32_t> g_planet_open; // llm_ui_planet_select_screen_open(open_arg)
struct SndCall {
    int32_t id, vol;
};
std::vector<SndCall> g_snd; // llm_snd_play(id, vol)

int32_t stub_planet_open(int32_t open_arg) {
    g_planet_open.push_back(open_arg);
    return 0;
}
void stub_snd(int32_t id, int32_t vol) { g_snd.push_back({id, vol}); }

const game_set_event_calls g_calls = {stub_planet_open, stub_snd};

uint32_t run(sim_fixture &fx, game_e_event type) {
    g_planet_open.clear();
    g_snd.clear();
    sim_store own = fx.store();
    return detail::game_set_event(fx.view(), own, g_calls, type);
}

// event codes (dense 0..0x17), local to the test.
enum : uint32_t {
    EV_PANEL_SHOW_MAP = 0,
    EV_PANEL_SHOW_BUILD,
    EV_PANEL_SHOW_INFO,
    EV_BUILD_TAB_BUILDINGS,
    EV_BUILD_TAB_UNITS,
    EV_BUILD_TAB_PROJECTS,
    EV_INFO_REFRESH,
    EV_BUILD_PROJECTS_REFRESH,
    EV_BUILD_UNITS_REFRESH,
    EV_BUILD_OPEN_AUTOPAGE,
    EV_HUD_REDRAW_ALL,
    EV_ACTION_DENIED_FEEDBACK,
    EV_PANEL_SHOW_MAP_OBJECTS,
    EV_MAP_SUBMODE1_REFRESH,
    EV_MAP_OBJECTS_REFRESH,
    EV_BUILD_BUILDINGS_REFRESH, // 0x0f
    // real value 0x10 is a NO-OP hole (jump-table slot aliases the shared tail) -- the bodies below
    // live at 0x11..0x18, one higher than a naive dense reading (reimpl-verify 2026-08-18).
    EV_PANEL_SHOW_INFO_TAB0 = 0x11,
    EV_PANEL_SHOW_INFO_TAB1,  // 0x12
    EV_CHAT_INPUT_OPEN,       // 0x13
    EV_CHAT_INPUT_CLOSE,      // 0x14
    EV_BUILD_HOTKEY_TOGGLE,   // 0x15
    EV_INFO_HOTKEY_TOGGLE,    // 0x16
    EV_MAP_HOTKEY_TOGGLE,     // 0x17
    EV_PANEL_REFRESH_CURRENT, // 0x18
};

constexpr int32_t SP = 1; // SESSION_SP
constexpr int32_t MP = 2; // any non-SP

} // namespace

void run_game_set_event_tests() {
    sim_fixture fx;

    // ---- PANEL_SHOW_MAP: switch pending, mode -> 0 ------------------------------------------------
    fx.reset();
    fx.ui_panel_mode = 2;
    run(fx, EV_PANEL_SHOW_MAP);
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "PANEL_SHOW_MAP: switch_pending=1");
    ck_eq((uint32_t)fx.ui_panel_mode, 0u, "PANEL_SHOW_MAP: mode=0");

    // ---- PANEL_SHOW_BUILD from mode!=1, page==2, NO mother -> mode1, page 2->0->1 -----------------
    fx.reset();
    fx.ui_panel_mode                      = 0;
    fx.ui_panel_page                      = 2;
    fx.player_side                        = 3;
    fx.planet_index                       = 4;
    fx.profiles[3].primary_mother_bldg[4] = 0; // no mother -> page becomes 1
    run(fx, EV_PANEL_SHOW_BUILD);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "PANEL_SHOW_BUILD: mode=1");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "PANEL_SHOW_BUILD: switch_pending=1");
    ck_eq((uint32_t)fx.ui_panel_page, 1u, "PANEL_SHOW_BUILD: page 2->0->1 (no mother)");

    // ---- PANEL_SHOW_BUILD, page==2, mother PRESENT -> page 2->0 (stays 0) -------------------------
    fx.reset();
    fx.ui_panel_page                      = 2;
    fx.player_side                        = 3;
    fx.planet_index                       = 4;
    fx.profiles[3].primary_mother_bldg[4] = 9; // mother present -> page stays 0
    run(fx, EV_PANEL_SHOW_BUILD);
    ck_eq((uint32_t)fx.ui_panel_page, 0u, "PANEL_SHOW_BUILD: page 2->0 (mother present)");

    // ---- PANEL_SHOW_BUILD already mode==1 -> no-op ------------------------------------------------
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 2;
    run(fx, EV_PANEL_SHOW_BUILD);
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 0u, "PANEL_SHOW_BUILD mode==1: no switch");
    ck_eq((uint32_t)fx.ui_panel_page, 2u, "PANEL_SHOW_BUILD mode==1: page untouched");

    // ---- PANEL_SHOW_INFO: switch, mode -> 2 ------------------------------------------------------
    fx.reset();
    run(fx, EV_PANEL_SHOW_INFO);
    ck_eq((uint32_t)fx.ui_panel_mode, 2u, "PANEL_SHOW_INFO: mode=2");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "PANEL_SHOW_INFO: switch_pending=1");

    // ---- BUILD_TAB_BUILDINGS: not blocked + mother -> mode1/switch/bldg_refresh/page0 ------------
    fx.reset();
    fx.player_side                        = 2;
    fx.planet_index                       = 5;
    fx.profiles[2].primary_mother_bldg[5] = 1;
    fx.ui_bldg_tab_select_blocked         = 0;
    run(fx, EV_BUILD_TAB_BUILDINGS);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "BUILD_TAB_BUILDINGS: mode=1");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_TAB_BUILDINGS: bldg_refresh=1");
    ck_eq((uint32_t)fx.ui_panel_page, 0u, "BUILD_TAB_BUILDINGS: page=0");

    // ---- BUILD_TAB_BUILDINGS blocked -> no-op ----------------------------------------------------
    fx.reset();
    fx.player_side                        = 2;
    fx.planet_index                       = 5;
    fx.profiles[2].primary_mother_bldg[5] = 1;
    fx.ui_bldg_tab_select_blocked         = 1; // blocked
    run(fx, EV_BUILD_TAB_BUILDINGS);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 0u, "BUILD_TAB_BUILDINGS blocked: no refresh");
    ck_eq((uint32_t)fx.ui_panel_mode, 0u, "BUILD_TAB_BUILDINGS blocked: mode untouched");

    // ---- BUILD_TAB_UNITS -> mode1/switch/bldg_refresh/page1 --------------------------------------
    fx.reset();
    run(fx, EV_BUILD_TAB_UNITS);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "BUILD_TAB_UNITS: mode=1");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_TAB_UNITS: bldg_refresh=1");
    ck_eq((uint32_t)fx.ui_panel_page, 1u, "BUILD_TAB_UNITS: page=1");

    // ---- BUILD_TAB_PROJECTS -> mode1/switch/bldg_refresh/page2 -----------------------------------
    fx.reset();
    run(fx, EV_BUILD_TAB_PROJECTS);
    ck_eq((uint32_t)fx.ui_panel_page, 2u, "BUILD_TAB_PROJECTS: page=2");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_TAB_PROJECTS: bldg_refresh=1");

    // ---- INFO_REFRESH: mode==2 sets unit_refresh; mode!=2 no-op ----------------------------------
    fx.reset();
    fx.ui_panel_mode = 2;
    run(fx, EV_INFO_REFRESH);
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 1u, "INFO_REFRESH mode==2: unit_refresh=1");
    fx.reset();
    fx.ui_panel_mode = 1;
    run(fx, EV_INFO_REFRESH);
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 0u, "INFO_REFRESH mode!=2: no-op");

    // ---- BUILD_PROJECTS_REFRESH gated on mode==1 && page==2 --------------------------------------
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 2;
    run(fx, EV_BUILD_PROJECTS_REFRESH);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_PROJECTS_REFRESH gate met");
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 1; // wrong page
    run(fx, EV_BUILD_PROJECTS_REFRESH);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 0u, "BUILD_PROJECTS_REFRESH gate not met");

    // ---- BUILD_UNITS_REFRESH gated on mode==1 && page==1 ----------------------------------------
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 1;
    run(fx, EV_BUILD_UNITS_REFRESH);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_UNITS_REFRESH gate met");

    // ---- BUILD_OPEN_AUTOPAGE: page = (no mother) ? 1 : 0 ----------------------------------------
    fx.reset();
    fx.player_side                        = 1;
    fx.planet_index                       = 2;
    fx.profiles[1].primary_mother_bldg[2] = 0; // no mother -> page 1
    run(fx, EV_BUILD_OPEN_AUTOPAGE);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "BUILD_OPEN_AUTOPAGE: mode=1");
    ck_eq((uint32_t)fx.ui_panel_page, 1u, "BUILD_OPEN_AUTOPAGE: page=1 (no mother)");
    fx.reset();
    fx.player_side                        = 1;
    fx.planet_index                       = 2;
    fx.profiles[1].primary_mother_bldg[2] = 7; // mother -> page 0
    run(fx, EV_BUILD_OPEN_AUTOPAGE);
    ck_eq((uint32_t)fx.ui_panel_page, 0u, "BUILD_OPEN_AUTOPAGE: page=0 (mother present)");

    // ---- HUD_REDRAW_ALL: switch + view_resize ----------------------------------------------------
    fx.reset();
    run(fx, EV_HUD_REDRAW_ALL);
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "HUD_REDRAW_ALL: switch=1");
    ck_eq((uint32_t)fx.ui_view_resize_pending, 1u, "HUD_REDRAW_ALL: view_resize=1");

    // ---- ACTION_DENIED_FEEDBACK: SP -> planet_select(0); MP -> snd_play(0xb3,100) ---------------
    fx.reset();
    fx.session_mode = SP;
    run(fx, EV_ACTION_DENIED_FEEDBACK);
    ck(g_planet_open.size() == 1 && g_planet_open[0] == 0, "ACTION_DENIED SP: planet_select(0)");
    ck(g_snd.empty(), "ACTION_DENIED SP: no sound");
    fx.reset();
    fx.session_mode = MP;
    run(fx, EV_ACTION_DENIED_FEEDBACK);
    ck(g_snd.size() == 1 && g_snd[0].id == 0xb3 && g_snd[0].vol == 100,
       "ACTION_DENIED MP: snd_play(0xb3,100)");
    ck(g_planet_open.empty(), "ACTION_DENIED MP: no planet_select");

    // ---- PANEL_SHOW_MAP_OBJECTS: mode!=0 -> mode0/switch; mainpanel_refresh + tab=2 --------------
    fx.reset();
    fx.ui_panel_mode = 1;
    run(fx, EV_PANEL_SHOW_MAP_OBJECTS);
    ck_eq((uint32_t)fx.ui_panel_mode, 0u, "PANEL_SHOW_MAP_OBJECTS: mode=0");
    ck_eq((uint32_t)fx.ui_mainpanel_refresh_pending, 1u, "PANEL_SHOW_MAP_OBJECTS: mainpanel_refresh=1");
    ck_eq((uint32_t)fx.ui_mainpanel_tab_index, 2u, "PANEL_SHOW_MAP_OBJECTS: tab=2");

    // ---- MAP_SUBMODE1_REFRESH gated mode==0 && tab==1 -------------------------------------------
    fx.reset();
    fx.ui_panel_mode          = 0;
    fx.ui_mainpanel_tab_index = 1;
    run(fx, EV_MAP_SUBMODE1_REFRESH);
    ck_eq((uint32_t)fx.ui_mainpanel_refresh_pending, 1u, "MAP_SUBMODE1_REFRESH gate met");

    // ---- MAP_OBJECTS_REFRESH gated mode==0 && tab==2 -------------------------------------------
    fx.reset();
    fx.ui_panel_mode          = 0;
    fx.ui_mainpanel_tab_index = 2;
    run(fx, EV_MAP_OBJECTS_REFRESH);
    ck_eq((uint32_t)fx.ui_mainpanel_refresh_pending, 1u, "MAP_OBJECTS_REFRESH gate met");

    // ---- BUILD_BUILDINGS_REFRESH gated mode==1 && page==0 --------------------------------------
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 0;
    run(fx, EV_BUILD_BUILDINGS_REFRESH);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_BUILDINGS_REFRESH gate met");

    // ---- PANEL_SHOW_INFO_TAB0 / TAB1: mode->2, unit_refresh, unit_tab 0/1 ----------------------
    fx.reset();
    run(fx, EV_PANEL_SHOW_INFO_TAB0);
    ck_eq((uint32_t)fx.ui_panel_mode, 2u, "PANEL_SHOW_INFO_TAB0: mode=2");
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 1u, "PANEL_SHOW_INFO_TAB0: unit_refresh=1");
    ck_eq((uint32_t)fx.ui_unit_tab_toggle, 0u, "PANEL_SHOW_INFO_TAB0: unit_tab=0");
    fx.reset();
    run(fx, EV_PANEL_SHOW_INFO_TAB1);
    ck_eq((uint32_t)fx.ui_unit_tab_toggle, 1u, "PANEL_SHOW_INFO_TAB1: unit_tab=1");

    // ---- CHAT_INPUT_OPEN / CLOSE ---------------------------------------------------------------
    fx.reset();
    fx.chat_input_len     = 5;
    fx.chat_input_cursor  = 3;
    fx.chat_input_line[0] = 'x';
    run(fx, EV_CHAT_INPUT_OPEN);
    ck_eq((uint32_t)fx.chat_input_active, 1u, "CHAT_INPUT_OPEN: active=1");
    ck_eq((uint32_t)fx.chat_input_len, 0u, "CHAT_INPUT_OPEN: len=0");
    ck_eq((uint32_t)fx.chat_input_cursor, 0u, "CHAT_INPUT_OPEN: cursor=0");
    ck(fx.chat_input_line[0] == '\0', "CHAT_INPUT_OPEN: line[0]=0");
    fx.reset();
    fx.chat_input_active = 1;
    run(fx, EV_CHAT_INPUT_CLOSE);
    ck_eq((uint32_t)fx.chat_input_active, 0u, "CHAT_INPUT_CLOSE: active=0");

    // ---- BUILD_HOTKEY_TOGGLE: mode==1 page==2 -> page1; mode==1 page==1 (not blocked+mother) -> page0;
    //      mode!=1 -> mode1/switch ; all set bldg_refresh (except the early-break blocked case) -------
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 2;
    run(fx, EV_BUILD_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_page, 1u, "BUILD_HOTKEY_TOGGLE mode1 page2 -> page1");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_HOTKEY_TOGGLE: bldg_refresh=1");
    fx.reset();
    fx.ui_panel_mode                      = 1;
    fx.ui_panel_page                      = 1;
    fx.player_side                        = 0;
    fx.planet_index                       = 0;
    fx.profiles[0].primary_mother_bldg[0] = 4; // mother present, not blocked -> page 0
    run(fx, EV_BUILD_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_page, 0u, "BUILD_HOTKEY_TOGGLE mode1 page1 not-blocked+mother -> page0");
    fx.reset();
    fx.ui_panel_mode              = 1;
    fx.ui_panel_page              = 1;
    fx.ui_bldg_tab_select_blocked = 1; // blocked -> early break, page unchanged, NO bldg_refresh
    run(fx, EV_BUILD_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_page, 1u, "BUILD_HOTKEY_TOGGLE mode1 page1 blocked -> page unchanged");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 0u, "BUILD_HOTKEY_TOGGLE blocked: no bldg_refresh (early break)");
    fx.reset();
    fx.ui_panel_mode = 0; // mode!=1 -> switch to build
    run(fx, EV_BUILD_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "BUILD_HOTKEY_TOGGLE mode!=1 -> mode1");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "BUILD_HOTKEY_TOGGLE mode!=1 -> switch=1");
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "BUILD_HOTKEY_TOGGLE mode!=1 -> bldg_refresh=1");

    // ---- INFO_HOTKEY_TOGGLE: mode==2 flips unit_tab; mode!=2 -> mode2/switch; always unit_refresh --
    fx.reset();
    fx.ui_panel_mode      = 2;
    fx.ui_unit_tab_toggle = 0;
    run(fx, EV_INFO_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_unit_tab_toggle, 1u, "INFO_HOTKEY_TOGGLE mode2: unit_tab 0->1");
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 1u, "INFO_HOTKEY_TOGGLE: unit_refresh=1");
    fx.reset();
    fx.ui_panel_mode = 0; // mode!=2
    run(fx, EV_INFO_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_mode, 2u, "INFO_HOTKEY_TOGGLE mode!=2 -> mode2");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "INFO_HOTKEY_TOGGLE mode!=2 -> switch=1");

    // ---- MAP_HOTKEY_TOGGLE: mode==0 && SP -> planet_select; else map switch --------------------
    fx.reset();
    fx.ui_panel_mode = 0;
    fx.session_mode  = SP;
    run(fx, EV_MAP_HOTKEY_TOGGLE);
    ck(g_planet_open.size() == 1 && g_planet_open[0] == 0, "MAP_HOTKEY_TOGGLE mode0 SP: planet_select(0)");
    fx.reset();
    fx.ui_panel_mode = 1; // mode!=0 -> switch to map
    fx.session_mode  = SP;
    run(fx, EV_MAP_HOTKEY_TOGGLE);
    ck_eq((uint32_t)fx.ui_panel_mode, 0u, "MAP_HOTKEY_TOGGLE mode!=0: mode=0");
    ck_eq((uint32_t)fx.ui_mainpanel_tab_index, 2u, "MAP_HOTKEY_TOGGLE mode!=0: tab=2");
    ck_eq((uint32_t)fx.ui_mainpanel_refresh_pending, 1u, "MAP_HOTKEY_TOGGLE mode!=0: mainpanel_refresh=1");
    ck(g_planet_open.empty(), "MAP_HOTKEY_TOGGLE mode!=0: no planet_select");

    // ---- PANEL_REFRESH_CURRENT: mode1 -> bldg_refresh; mode2 -> unit_refresh; mode0 -> no-op ----
    fx.reset();
    fx.ui_panel_mode = 1;
    run(fx, EV_PANEL_REFRESH_CURRENT);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 1u, "PANEL_REFRESH_CURRENT mode1: bldg_refresh=1");
    fx.reset();
    fx.ui_panel_mode = 2;
    run(fx, EV_PANEL_REFRESH_CURRENT);
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 1u, "PANEL_REFRESH_CURRENT mode2: unit_refresh=1");
    fx.reset();
    fx.ui_panel_mode = 0;
    run(fx, EV_PANEL_REFRESH_CURRENT);
    ck_eq((uint32_t)fx.ui_bldg_panel_refresh_pending, 0u, "PANEL_REFRESH_CURRENT mode0: no-op");

    // ---- 0x10 is a NO-OP HOLE in the jump table: it must change NOTHING (falls to the tail) -------
    // (Regression lock for the reimpl-verify off-by-one: a dense reading would run the TAB0 body here.)
    fx.reset();
    fx.ui_panel_mode = 1;
    fx.ui_panel_page = 2;
    run(fx, (game_e_event)0x10);
    ck_eq((uint32_t)fx.ui_panel_mode, 1u, "0x10 no-op: mode unchanged (NOT set to 2 by a shifted TAB0)");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 0u, "0x10 no-op: switch_pending untouched");
    ck_eq((uint32_t)fx.ui_unit_panel_refresh_pending, 0u, "0x10 no-op: unit_refresh untouched");
    ck_eq((uint32_t)fx.ui_unit_tab_toggle, 0u, "0x10 no-op: unit_tab untouched");

    // ---- PANEL_REFRESH_CURRENT is the top of the domain (value 0x18), not 0x17 -------------------
    ck_eq((uint32_t)EV_PANEL_REFRESH_CURRENT, 0x18u, "domain top: PANEL_REFRESH_CURRENT == 0x18");

    // ---- TUTORIAL suppression: a switch INTO map mode is undone when tutorial_step != 0 ---------
    fx.reset();
    fx.ui_panel_mode = 2;       // on entry
    fx.tutorial_step = 1;       // tutorial active
    run(fx, EV_PANEL_SHOW_MAP); // would set mode=0, switch=1
    ck_eq((uint32_t)fx.ui_panel_mode, 2u, "TUTORIAL: mode restored to entry value (2), not 0");
    ck_eq((uint32_t)fx.ui_panel_switch_pending, 0u, "TUTORIAL: switch_pending suppressed to 0");

    // ---- DEFER path: defer_active != 0 queues the event instead of applying --------------------
    fx.reset();
    fx.ui_event_defer_active = 1;
    fx.ui_event_queue_pos    = 0;
    fx.ui_event_queue[0]     = -1; // head is the terminator, not a dup
    {
        uint32_t r = run(fx, EV_PANEL_SHOW_INFO); // type 2
        // advance pos 0->1, queue[1]=2, terminator at queue[2]=0xffffffff, return 2*4=8
        ck_eq((uint32_t)fx.ui_event_queue_pos, 1u, "DEFER: pos advanced to 1");
        ck_eq((uint32_t)fx.ui_event_queue[1], 2u, "DEFER: queued event 2 at pos 1");
        ck_eq((uint32_t)fx.ui_event_queue[2], 0xffffffffu, "DEFER: terminator at pos 2");
        ck_eq(r, 8u, "DEFER: returns term_slot*4 = 8");
        ck_eq((uint32_t)fx.ui_panel_mode, 0u, "DEFER: panel state NOT applied (mode still 0)");
    }
    // dedup: queuing the SAME event already at head returns it and does not advance
    fx.reset();
    fx.ui_event_defer_active = 1;
    fx.ui_event_queue_pos    = 4;
    fx.ui_event_queue[4]     = 7; // head already holds event 7
    {
        uint32_t r = run(fx, (game_e_event)7);
        ck_eq((uint32_t)fx.ui_event_queue_pos, 4u, "DEFER dedup: pos NOT advanced");
        ck_eq(r, 7u, "DEFER dedup: returns the duplicate value (7)");
    }

    // ---- FALLBACK pass: a positive id != current mode forces the panel mode ---------------------
    fx.reset();
    fx.ui_panel_mode              = 0; // PANEL_SHOW_MAP will keep mode 0
    fx.ui_panel_fallback_table[0] = 2; // a fallback id (2) differs from mode 0 -> forced
    fx.ui_panel_fallback_table[1] = 0; // terminator
    {
        uint32_t r = run(fx, EV_PANEL_SHOW_MAP);
        ck_eq((uint32_t)fx.ui_panel_mode, 2u, "FALLBACK: forced mode to table id 2");
        ck_eq((uint32_t)fx.ui_panel_switch_pending, 1u, "FALLBACK: switch_pending=1 on force");
        ck_eq(r, 2u, "FALLBACK: return is the forced id (2)");
    }
    // fallback entry equal to the current mode is NOT forced (returns i*4 = count*4)
    fx.reset();
    fx.ui_panel_mode              = 2;
    fx.ui_panel_fallback_table[0] = 2; // equals current mode -> not a candidate
    fx.ui_panel_fallback_table[1] = 0;
    {
        uint32_t r = run(fx, EV_PANEL_SHOW_INFO); // keeps mode 2
        ck_eq((uint32_t)fx.ui_panel_mode, 2u, "FALLBACK equal-to-mode: mode unchanged");
        ck_eq(r, 4u, "FALLBACK equal-to-mode: return = scanned_count(1)*4 = 4");
    }
}

} // namespace mh::sim::test
