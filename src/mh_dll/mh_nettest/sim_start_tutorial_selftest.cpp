//
// sim_start_tutorial_selftest.cpp -- `simtest` offline oracle for llm_game_start_tutorial
// @0x004bafb1 (sim/resid/sim_start_tutorial.h/.cpp, RI-SIM / sim_resid batch F).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification.
//
// A COMPOSITE oracle: the body's edge into detail::session_begin_multi is a direct C++ call (G21),
// so that sibling and its own three-deep closure RUN FOR REAL here, with only their OUTWARD calls
// substituted from sim_resid_sibling_mocks.h. That is deliberate -- the aggregate post-state is the
// thing under test -- and it is why the assertions below cover state this function never writes
// itself (PlayerSide, session_mode): those are the sibling's, and they are here to prove the edge
// fired with the arguments this function chose.
//
// Expected behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_game_start_tutorial_004bafb1.asm).
//
#include "sim/resid/sim_start_tutorial.h"

#include "sim_resid_sibling_mocks.h"
#include "sim_test_support.h"
#include "state/region_view.h" // SIMABI-STRING: the runtime's own hash_slice(), for T10's arm

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The four constant pointers the body stores as VALUES (its own literal MOV immediates).
constexpr uintptr_t WGT_LIST_TUTORIAL_INTRO   = 0x00653acbu;
constexpr uintptr_t WGT_LIST_TUTORIAL_WELCOME = 0x00653b63u;

// The body STORES the addresses above into ui_menu_widget_list (a libmh-side slot), but PASSES a
// LIBMH_SCR_WGTL_* id to the draw/centre calls -- an address cannot cross the host boundary (R4).
// Two encodings of the same lists, kept apart so a check cannot pass by conflating them.
constexpr uintptr_t WGTL_ID_TUTORIAL_INTRO   = 3u;
constexpr uintptr_t WGTL_ID_TUTORIAL_WELCOME = 4u;
constexpr uintptr_t TUTORIAL_WELCOME_TEXT    = 0x0065d3eeu;
constexpr uintptr_t TUTORIAL_MENU_REDRAW_CB  = 0x004baf11u;

// The sprite metrics the hint layout is derived from. Distinct and non-round so an oracle can tell
// width from height and catch the two being swapped.

struct Calls {
    // SIMABI-DISPLAY: a VOID request now, so there is no return to configure -- the arm below
    // asserts the CELL IS NOT WRITTEN, which is the thing that changed.
    int32_t              set_display_mode          = 0;
    int32_t              set_display_mode_last_arg = -1;
    std::vector<int32_t> set_event_args;
    // The body no longer SPINS on a fade tick -- it asks the host to complete the armed transition
    // once per transition, so what is countable here is the number of REQUESTS (two), not loop
    // iterations. The looping itself is the host's and is covered by the hosted oracle.
    int32_t                fade_run         = 0;
    int32_t                widget_list_draw = 0;
    std::vector<uintptr_t> widget_list_draw_args;
    int32_t                present_flip = 0, load_script = 0;
    // The hint widget's geometry is laid out host-side now; what libmh does is ASK, with the sprite
    // whose metrics feed it. Recording the argument keeps the sprite id itself under test.
    int32_t                hint_layout = 0;
    std::vector<int32_t>   hint_layout_args;
    int32_t                fill_data          = 0;
    void                  *fill_data_ptr      = nullptr;
    uint32_t               fill_data_size     = 0;
    uint8_t                fill_data_val      = 0xff;
    int32_t                read_map_file      = 0;
    const void            *read_map_file_arg  = nullptr;
    int32_t                wide_to_short      = 0;
    const void            *wide_to_short_src  = nullptr;
    char                  *wide_to_short_dst  = nullptr;
    int32_t                widget_list_center = 0;
    std::vector<uintptr_t> widget_list_center_args;
    int32_t                strat_frame = 0;
};
Calls g;

void stub_set_display_mode(int32_t m) {
    ++g.set_display_mode;
    g.set_display_mode_last_arg = m;
}
uint32_t stub_set_event(uint32_t t) {
    g.set_event_args.push_back((int32_t)t);
    return 0;
}
void stub_fade_run() { ++g.fade_run; }
void stub_widget_list_draw(int32_t list_id) {
    ++g.widget_list_draw;
    g.widget_list_draw_args.push_back((uintptr_t)list_id);
}
void    stub_present_flip() { ++g.present_flip; }
int32_t stub_load_script() {
    ++g.load_script;
    return 0;
}
void stub_hint_layout(int32_t sprite_id) {
    ++g.hint_layout;
    g.hint_layout_args.push_back(sprite_id);
}
void *stub_fill_data(void *ptr, uint32_t size, uint8_t val) {
    ++g.fill_data;
    g.fill_data_ptr  = ptr;
    g.fill_data_size = size;
    g.fill_data_val  = val;
    // Do the fill for real -- the table is state a case asserts on, and a mock that skips it logs
    // identically to one that works (the same reasoning as mock_ngi_calls' fill_data).
    if (ptr != nullptr && size != 0) memset(ptr, val, size);
    return ptr;
}
uint32_t stub_read_map_file(map_header *h) {
    ++g.read_map_file;
    g.read_map_file_arg = h;
    // The real cfg_ReadMapFile POPULATES the header from the file on disk. The tlo name is what
    // this function copies into the injected planet slot afterwards, so the mock has to supply one
    // or the strcpy assertion below would pass over a zero-length string either way.
    if (h != nullptr) {
        memcpy(h->tlo_name, "TUT.TLO", 8);
        h->width  = 64;
        h->height = 48;
    }
    return 0;
}
// The bytes the stub codec "converts" to. FILE-SCOPE rather than a `Calls` field on purpose: run()
// resets `g` before every call, so a case that has to choose the codec's OUTPUT (T10's mutation arm)
// has nowhere else to put it. Default is what every pre-existing case saw.
const char *g_wide_to_short_out = "AI";

char *stub_wide_to_short(void *src, char *dst) {
    ++g.wide_to_short;
    g.wide_to_short_src = src;
    g.wide_to_short_dst = dst;
    if (dst != nullptr) memcpy(dst, g_wide_to_short_out, strlen(g_wide_to_short_out) + 1);
    return dst;
}
void stub_widget_list_center(int32_t list_id) {
    ++g.widget_list_center;
    g.widget_list_center_args.push_back((uintptr_t)list_id);
}
void stub_strat_frame() { ++g.strat_frame; }

const start_tutorial_calls g_calls = {
    &stub_set_display_mode,
    &stub_set_event,
    &stub_fade_run,
    &stub_widget_list_draw,
    &stub_present_flip,
    &stub_load_script,
    &stub_hint_layout,
    &stub_fill_data,
    &stub_read_map_file,
    &stub_wide_to_short,
    &stub_widget_list_center,
    &stub_strat_frame,
};

// Clears the RECORDS. The size-mode return knob went with SIMABI-DISPLAY's convert -- the request
// is void, so there is nothing left for a case to reconfigure here.
int32_t run(sim_fixture &fx) {
    g             = Calls{};
    sibling_rec() = sibling_record{};
    sim_store own = fx.store();
    return detail::start_tutorial(fx.view(), own, g_calls, mock_sbm_calls(), mock_ssr_calls(),
                                  mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());
}

} // namespace

void run_start_tutorial_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the entry sequence (0x004bafd0-0x004bb015): the view-size save/force, the HUD event, both
    // fade-transition endpoints, the one-shot latch clear, and that the fade tick is a LOOP.
    // =================================================================================================
    {
        fx.reset();
        fx.view_size_mode                   = 2; // a distinctive prior mode, to be SAVED
        fx.ui_screen_main_menu_id           = 0x1101;
        fx.ui_screen_racebck_id             = 0x1102;
        fx.tutorial_hq_attack_scenario_done = 1;
        const int32_t ret                   = run(fx);

        ck_eq((uint32_t)ret, 1u, "T1: the return is the unconditional constant 1, 0x004bb2e6");
        ck_eq((uint32_t)fx.view_size_mode_save, 2u,
              "T1: the PRIOR view size mode is saved before the request, 0x004bafd0");
        // SIMABI-DISPLAY: this used to assert the cell held the entry's RETURN (9). The store
        // crossed to the host with the entry, so the claim inverts: libmh asks, and does NOT write.
        // The 2 is the prior mode still standing -- a reinstated libmh write fails here.
        ck_eq((uint32_t)fx.view_size_mode, 2u,
              "T1: libmh does NOT write the view size mode -- the store is the host's, 0x004bafe0");
        ck_eq((uint32_t)g.set_display_mode, 1u,
              "T1: exactly one display-mode request, 0x004bafd9");
        ck_eq((uint32_t)g.set_display_mode_last_arg, 0u, "T1: ... and its argument is 0, 0x004bafd9");
        ck(!g.set_event_args.empty() && g.set_event_args[0] == 0xa,
           "T1: the first game_SetEvent is EV_HUD_REDRAW_ALL (0xa), 0x004bafe6");
        ck_eq((uint32_t)fx.tutorial_hq_attack_scenario_done, 0u,
              "T1: the one-shot HQ-attack latch is cleared, 0x004bb004");
        // TWO transitions, TWO requests -- 0x004bb00e and 0x004bb274. The original spun on the tick
        // entry at each; the spin is the host's now, so what libmh owes is exactly one request per
        // armed transition. A body that dropped one, or asked twice for the same one, fails here.
        ck_eq((uint32_t)g.fade_run, 2u,
              "T1: the two armed fade transitions are each completed once, 0x004bb00e/0x004bb274");
    }

    // =================================================================================================
    // T2 -- the fade endpoints, asserted separately from T1 because they are the two NEW read
    // bindings and a swap between them is exactly the error a single combined check would hide. The
    // src endpoint is written TWICE: main-menu on entry, racebck again after the session begins.
    // =================================================================================================
    {
        fx.reset();
        fx.ui_screen_main_menu_id = 0x2201;
        fx.ui_screen_racebck_id   = 0x2202;
        run(fx);
        ck_eq((uint32_t)fx.ui_fade_transition.dst_screen_id, 0x2202u,
              "T2: dst_screen_id is the RACEBCK id, 0x004bb000");
        ck_eq((uint32_t)fx.ui_fade_transition.src_screen_id, 0x2202u,
              "T2: src_screen_id ends at the RACEBCK id -- the post-session re-arm, 0x004bb26a");
    }

    // =================================================================================================
    // T3 -- the tutorial hint widget layout (0x004bb035-0x004bb0a6). The label is an ADDRESS ESCAPE
    // into the step table; the four geometry fields are derived from two sprite metrics with four
    // different constants, so a swapped pair or a dropped subtraction is visible.
    // =================================================================================================
    {
        fx.reset();
        run(fx);
        ck(fx.ui_tutorial_hint_widget.label ==
               reinterpret_cast<char *>(const_cast<char16_t *>(fx.tutorial_steps[0].body)),
           "T3: the hint label is &tutorial_steps[0].body (base + 0x80), 0x004bb046");
        // The four geometry fields and the two sprite-metric queries that fed them are host-side
        // now (the metrics have no other consumer and the widget is unhashed + host_free), so what
        // is checkable here is the REQUEST and its sprite. The arithmetic itself is covered by the
        // hosted oracle, which compares the rendered result rather than the intermediate.
        ck_eq((uint32_t)g.hint_layout, 1u, "T3: the hint layout is requested once, 0x004bb035-0x004bb0a6");
        ck(g.hint_layout_args.size() == 1 && g.hint_layout_args[0] == 0x42,
           "T3: ... for UI sprite 0x42, the one the metrics are read from");
        ck_eq((uint32_t)g.load_script, 1u, "T3: the step script is loaded once, 0x004bb02b");
    }

    // =================================================================================================
    // T4 -- the tutorial scalar reset (0x004bb0a9-0x004bb0e7). All four chained-store links and the
    // whole 16-byte fallback table land at 0, and the step cursor at 1. Every one is SEEDED non-zero
    // first, so a body that skipped a link leaves its sentinel behind.
    // =================================================================================================
    {
        fx.reset();
        fx.tutorial_rmb_limit_flag             = 11;
        fx.tutorial_pending_build_placement_id = 12;
        fx.tutorial_reset_slot_0050a678        = 13;
        fx.tutorial_build_type_filter          = 14;
        fx.ui_race_sel_pending_gfx_idx         = 15;
        for (int32_t i = 0; i < 4; ++i) fx.ui_panel_fallback_table[(size_t)i] = 0x40 + i;
        run(fx);
        ck_eq((uint32_t)fx.tutorial_step, 1u, "T4: the tutorial enters step 1, 0x004bb0a9");
        ck_eq((uint32_t)fx.tutorial_rmb_limit_flag, 0u, "T4: chained store link 1 -> 0, 0x004bb0b3");
        ck_eq((uint32_t)fx.tutorial_pending_build_placement_id, 0u, "T4: link 2 -> 0, 0x004bb0bd");
        ck_eq((uint32_t)fx.tutorial_reset_slot_0050a678, 0u, "T4: link 3 -> 0, 0x004bb0c7");
        ck_eq((uint32_t)fx.tutorial_build_type_filter, 0u, "T4: link 4 -> 0, 0x004bb0d1");
        ck_eq((uint32_t)fx.ui_race_sel_pending_gfx_idx, 0u,
              "T4: the deferred race-select overlay index is cleared, 0x004bb017");
        ck_eq(g.fill_data_size, 0x10u, "T4: fill_data zeroes 0x10 bytes -- the WHOLE int[4] table, 0x004bb0e1");
        ck_eq((uint32_t)g.fill_data_val, 0u, "T4: ... with the value 0");
        ck(g.fill_data_ptr == (void *)fx.ui_panel_fallback_table.data(),
           "T4: ... starting at the table's first entry");
        bool cleared = true;
        for (int32_t i = 0; i < 4; ++i) {
            if (fx.ui_panel_fallback_table[(size_t)i] != 0) cleared = false;
        }
        ck(cleared, "T4: every fallback-table entry is 0 afterwards");
    }

    // =================================================================================================
    // T5 -- the hardcoded map (0x004bb0ec-0x004bb10b). The path and name are LITERALS, not config,
    // and cfg_ReadMapFile is handed the fixture's own current_map_data by address.
    // =================================================================================================
    {
        fx.reset();
        run(fx);
        ck(strcmp(fx.current_map_data.path_unc, "Dane\\") == 0,
           "T5: path_unc is the literal 'Dane\\', 0x004bb0ec");
        ck(strcmp(fx.current_map_data.map_name, "TUTORIAL.MP") == 0,
           "T5: map_name is the literal 'TUTORIAL.MP', 0x004bb0f8");
        ck_eq((uint32_t)g.read_map_file, 1u, "T5: cfg_ReadMapFile is called exactly once, 0x004bb106");
        ck(g.read_map_file_arg == (const void *)&fx.current_map_data,
           "T5: ... on current_map_data itself, not a copy");
    }

    // =================================================================================================
    // T6 -- the fixed 1v1 (0x004bb110-0x004bb1aa). Every field of both live slots, the two relations,
    // the two masks, the six disabled slots, and the injected-planet index.
    //
    // AND THE ALIASING: player_relation_at(a, b) lands inside player_desc[a].relation[b] -- one
    // region, two accessors. The fixture models that (players_raw aliases player_desc_slots since
    // 2026-09-01), so the relation assertions below read the DESCRIPTOR, which is where the game
    // would have put them.
    // =================================================================================================
    {
        fx.reset();
        fx.text_ptrs[0x2ac] = (const wchar_t *)0xBBBB0001u;
        for (int32_t i = 2; i < 8; ++i) fx.player_desc_slots[(size_t)i].controller_flags = 0x7f;
        run(fx);

        const player_desc &p0 = fx.player_desc_slots[0];
        ck_eq((uint32_t)p0.controller_flags, 7u, "T6: Players[0].controller_flags = 7, 0x004bb110");
        ck_eq((uint32_t)p0.race_or_faction, 1u, "T6: Players[0].race_or_faction = 1");
        ck_eq((uint32_t)p0.color_or_team, 3u, "T6: Players[0].color_or_team = 3");
        ck_eq((uint32_t)(int32_t)p0.scenario_side_id, 0u, "T6: Players[0].scenario_side_id = 0");
        ck(p0.name[0] == '1' && p0.name[1] == '\0', "T6: Players[0].name is the literal \"1\"");

        const player_desc &p1 = fx.player_desc_slots[1];
        ck_eq((uint32_t)p1.controller_flags, 0xbu, "T6: Players[1].controller_flags = 0xb");
        ck_eq((uint32_t)p1.race_or_faction, 2u, "T6: Players[1].race_or_faction = 2");
        ck_eq((uint32_t)p1.color_or_team, 1u, "T6: Players[1].color_or_team = 1");
        ck_eq((uint32_t)(int32_t)p1.scenario_side_id, (uint32_t)-1,
              "T6: Players[1].scenario_side_id = -1 (the neutral/AI marker)");
        ck_eq((uint32_t)g.wide_to_short, 1u, "T6: the AI name is converted once, 0x004bb160");
        ck(g.wide_to_short_src == (const void *)0xBBBB0001u,
           "T6: ... from text_ptrs[0x2ac], not another slot");
        ck(g.wide_to_short_dst == p1.name, "T6: ... into Players[1].name itself");

        ck_eq((uint32_t)p0.relation[1], 2u,
              "T6: relation(0,1) = 2 (enemy), and it lands INSIDE Players[0], 0x004bb16c");
        ck_eq((uint32_t)p1.relation[0], 2u, "T6: relation(1,0) = 2, symmetric, 0x004bb178");
        // chat_target_mask IS SET TO 0xff HERE AND THEN OVERWRITTEN. 0x004bb17c stores 0xff; the
        // sibling call at 0x004bb256 reaches session_begin_multi, which stores 0 at 0x00454414 --
        // after this function, unconditionally. So the tutorial's 0xff is DEAD in the composite, and
        // this asserts the composite's real post-state rather than the intermediate one. (The
        // opposite ordering is visible in T9: the sibling sets show_unit_flags = 1 and THIS function
        // clears it afterwards, so there the later write is the one that survives.) Both are
        // faithful to the original's call order; neither is a translation defect.
        ck_eq((uint32_t)fx.chat_target_mask, 0u,
              "T6: chat_target_mask ends at 0 -- the sibling's 0x00454414 overwrites this function's "
              "0xff at 0x004bb17c");
        ck_eq((uint32_t)fx.player_control_mask, 0u, "T6: player_control_mask = 0, 0x004bb182");

        bool disabled = true;
        for (int32_t i = 2; i < 8; ++i) {
            if (fx.player_desc_slots[(size_t)i].controller_flags != 0) disabled = false;
        }
        ck(disabled, "T6: slots 2..7 are all disabled -- the 0x7f sentinels are gone, 0x004bb186");
        ck_eq((uint32_t)fx.injected_map_planet_slot, 0x1fu,
              "T6: the injected-map planet slot index is 0x1f, 0x004bb1a4");
    }

    // =================================================================================================
    // T7 -- the injected planet slot (0x004bb1b1-0x004bb256). Three string copies from the map header
    // the loader just populated, and the neutral per-channel economy. The tlo_file copy is the one
    // that needed the Ghidra retype: it was modelled as a single byte, so a body written against the
    // old type could only have reached it through the pad blob.
    // =================================================================================================
    {
        fx.reset();
        // Seed a NEIGHBOURING slot so a body that indexed 0x1e or 0x20 is caught rather than passing
        // on a table that is zero everywhere else.
        strcpy(fx.cfg_planets[0x1e].tlo_file, "NEIGHBOUR");
        run(fx);
        cfg_planet &p31 = fx.cfg_planets[0x1f];
        ck(strcmp(p31.path_unc, "Dane\\") == 0, "T7: planet[0x1f].path_unc <- the loaded header, 0x004bb1b1");
        ck(strcmp(p31.map_name, "TUTORIAL.MP") == 0, "T7: planet[0x1f].map_name <- the loaded header");
        ck(strcmp(p31.tlo_file, "TUT.TLO") == 0,
           "T7: planet[0x1f].tlo_file <- the header's tlo_name -- a real string into a real char array");
        ck(strcmp(fx.cfg_planets[0x1e].tlo_file, "NEIGHBOUR") == 0,
           "T7: the neighbouring planet slot is untouched -- the index really is 0x1f");
        bool economy = true;
        for (int32_t i = 0; i < 4; ++i) {
            if (p31.source_mul[i] != 1 || p31.source_add[i] != 0) economy = false;
        }
        ck(economy, "T7: all four channels are source_mul=1 / source_add=0 (neutral), 0x004bb21d");
    }

    // =================================================================================================
    // T8 -- THE SIBLING EDGE. session_begin_multi runs for real, so the assertions here are of two
    // kinds: that its outward calls fired (the edge happened at all), and that ITS OWN writes landed
    // (it ran with the state this function had just set up, not with a fresh fixture).
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 1;
        fx.net_lobby_scan_host_count = 1; // <= 1 -> SESSION_MP_LOCAL(2)
        run(fx);
        ck_eq((uint32_t)sibling_rec().sbm_scenario_planet_clone, 1u,
              "T8: the sibling ran and cloned the scenario planet once, 0x004bb256");
        ck(sibling_rec().sbm_clone_last_blob == (const void *)&fx.current_map_data,
           "T8: ... handed THIS function's current_map_data, not some other blob");
        ck_eq((uint32_t)fx.player_side, 1u,
              "T8: the sibling's own write landed -- PlayerSide = net_local_player_slot");
        ck_eq((uint32_t)fx.session_mode, 2u,
              "T8: ... and session_mode = SESSION_MP_LOCAL(2) for host_count <= 1");
        ck(sibling_rec().sbm_player_profile_init > 0,
           "T8: the sibling initialised at least one player profile");
    }

    // =================================================================================================
    // T9 -- the tail (0x004bb260-0x004bb2e6). The widget/label stores are four CONSTANT POINTERS and
    // two text-table pointers; the DLG_STATE_FLAGS dance sets bit 0 and bit 1 and touches nothing
    // else; the frame panel is positioned and TWO strat_frame calls run, with the widget-list centre
    // between them taking the WELCOME list this function just installed.
    // =================================================================================================
    {
        fx.reset();
        fx.text_ptrs[0x2b2] = (const wchar_t *)0xCCCC0002u;
        fx.show_unit_flags  = 0x55;
        fx.dlg_state_flags  = static_cast<int32_t>(0xf0f0f0f0u); // every OTHER bit must survive
        run(fx);

        ck_eq((uint32_t)fx.show_unit_flags, 0u, "T9: the unit-overlay flag mask is cleared, 0x004bb260");
        ck(fx.ui_wgt_menu_screen_title.label == (char *)0xCCCC0002u,
           "T9: the menu title label takes text_ptrs[0x2b2], 0x004bb27d");
        ck(fx.ui_wgt_tutorial_welcome.label == (char *)TUTORIAL_WELCOME_TEXT,
           "T9: the welcome widget label is the constant pointer 0x0065d3ee");
        ck((uintptr_t)fx.ui_menu_widget_list == WGT_LIST_TUTORIAL_WELCOME,
           "T9: the menu widget list becomes _G_LLM_UI_WGT_LIST_TUTORIAL_WELCOME, 0x004bb296");
        ck(fx.ui_menu_async_callback_a == (void *)TUTORIAL_MENU_REDRAW_CB,
           "T9: the async callback is installed as llm_tutorial_menu_redraw_cb's ADDRESS, 0x004bb29e");

        ck_eq((uint32_t)(fx.dlg_state_flags & 3), 3u,
              "T9: DLG_STATE_FLAGS bits 0 and 1 are both set, 0x004bb2a5/0x004bb2bd");
        ck_eq((uint32_t)(fx.dlg_state_flags & ~3), 0xf0f0f0f0u & ~3u,
              "T9: ... and no other bit is disturbed by the byte-OR / dword-AND-OR dance");

        ck_eq((uint32_t)(int32_t)fx.ui_wgt_frame_menu_panel.x, (uint32_t)(-0x50),
              "T9: the frame/backdrop panel is positioned at x = -0x50, 0x004bb2c3");
        ck_eq((uint32_t)g.strat_frame, 2u, "T9: strat_frame runs exactly TWICE, 0x004bb2cf/0x004bb2e1");
        ck(g.widget_list_center_args.size() == 1 &&
               g.widget_list_center_args[0] == WGTL_ID_TUTORIAL_WELCOME,
           "T9: the single centre call takes the WELCOME list this function installed, 0x004bb2d7");
        ck(g.widget_list_draw_args.size() == 1 &&
               g.widget_list_draw_args[0] == WGTL_ID_TUTORIAL_INTRO,
           "T9: the single draw call took the INTRO list, back at 0x004bb021 -- a DIFFERENT constant");
        ck_eq((uint32_t)g.present_flip, 1u, "T9: exactly one flip, 0x004bb026");
    }

    // =================================================================================================
    // T10 -- SIMABI-STRING (2026-09-10): the POSITIVE hash-mutation arm for utils_wide_to_short_str.
    //
    // WHY IT EXISTS. The ledger gives this entry a STRICT contract on the claim that its OUTPUT lands
    // in hashed sim state, so a host whose codec returns different bytes desyncs. Until this arm the
    // only oracle for that claim was the selftest host's NO-OP TRAP -- a required entry called with no
    // implementation names itself and exits -- which proves the entry is REACHED and says nothing
    // about where its bytes go. This is the complement: flip ONE byte of what the codec returns at
    // this sim site and watch the NAMED slice's hash move.
    //
    // It hashes through the runtime's own hash_slice()/emit_slice() rather than a local checksum, with
    // RID_PLAYERS rebased onto the fixture's own Players[8] array -- so the bytes hashed are the bytes
    // the body wrote, walked by the same emitter the determinism gate uses.
    //
    // THE THIRD RUN IS NOT DECORATION: it restores the byte and must reproduce run A's hash exactly.
    // Without it the arm cannot tell "the codec's output is hashed" from "this hash is noisy".
    // =================================================================================================
    {
        namespace st = mh::state;

        const st::hash_region &hr = st::HASH_REGIONS[st::HIDX_PLAYERS];
        ck(hr.rid == st::RID_PLAYERS && hr.offset == 0,
           "T10: premise -- HIDX_PLAYERS is the WHOLE of RID_PLAYERS, so emit_slice takes the raw "
           "block path and the fixture array can stand in for it");
        ck_eq(hr.len, (uint32_t)(fx.player_desc_slots.size() * sizeof(player_desc)),
              "T10: premise -- the slice is exactly Players[8] (416 = 8 * 0x34)");
        ck(st::owner_of(st::RID_PLAYERS) == nullptr,
           "T10: premise -- nothing owns RID_PLAYERS, so emit_slice reads bytes at an address");

        std::vector<uint8_t> img_a((size_t)hr.len), img_b((size_t)hr.len);

        // One full run of the body with a chosen codec output, hashed at the named slice.
        auto arm = [&](const char *codec_out, std::vector<uint8_t> *img) -> uint64_t {
            fx.reset();
            fx.text_ptrs[0x2ac] = (const wchar_t *)0xBBBB0001u;
            g_wide_to_short_out = codec_out;
            run(fx);
            st::rebase(st::RID_PLAYERS, (uint32_t)(uintptr_t)fx.player_desc_slots.data(), hr.len);
            const uint64_t h = st::hash_slice(st::HIDX_PLAYERS, true);
            if (img != nullptr) memcpy(img->data(), fx.player_desc_slots.data(), (size_t)hr.len);
            st::unrebase(st::RID_PLAYERS);
            return h;
        };

        const uint64_t h_a  = arm("AI", &img_a);
        const uint64_t h_b  = arm("AJ", &img_b);  // ONE byte of the returned buffer flipped: 'I' -> 'J'
        const uint64_t h_r  = arm("AI", nullptr); // reverted
        g_wide_to_short_out = "AI";

        uint32_t differing = 0;
        for (uint32_t i = 0; i < hr.len; ++i)
            if (img_a[i] != img_b[i]) ++differing;
        printf("  [SIMABI-STRING] utils_wide_to_short_str -> HIDX_PLAYERS(\"%s\"): compared %u bytes, "
               "%u differing, hash %016llx -> %016llx, reverted %016llx\n",
               hr.name, hr.len, differing, (unsigned long long)h_a, (unsigned long long)h_b,
               (unsigned long long)h_r);

        ck(differing > 0,
           "T10: flipping ONE byte of the codec's returned buffer changes the hashed `players` slice "
           "-- the STRICT contract's premise, measured rather than argued");
        ck(h_b != h_a,
           "T10: ... and the slice HASH moves with it (hash_slice(HIDX_PLAYERS), the determinism "
           "gate's own emitter), which is the desync a wrong host codec would cause");
        ck(h_r == h_a,
           "T10: ... and reverting the byte restores the hash EXACTLY -- the control that keeps this "
           "arm from passing on a noisy hash");
    }
}

} // namespace mh::sim::test
