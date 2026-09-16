//
//
// sim/resid/sim_start_tutorial.cpp -- see sim_start_tutorial.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_game_start_tutorial_004bafb1.asm), the Ghidra .c being a draft.
//
#include "sim/resid/sim_start_tutorial.h"

#include "addr/mh_calls.gen.h"   // typed callables for the effectful/frontier originals we still call OUT to
#include "state/boot_snapshot.h" // LIB-BOOT: the tutorial step table arrives in the snapshot
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

// ---- LIB-BOOT (2026-09-10): the tutorial step script -------------------------------------------
//
// llm_tutorial_load_script @0x004b9cc2 parses info\TUTORIAL.TXT out of the .rsr into
// _G_LLM_TUTORIAL_STEPS (+ its count and the forced-selection latch) and is read-only afterwards --
// the same shape as the cfg load, one file parsed once into a table. The post-cfg snapshot carries
// those three regions (tools/data/boot_snapshot_dispositions.json `carry_added`), so STANDALONE the
// data is already in memory when this runs and there is nothing to load.
//
// HOSTED IT STILL CALLS THE ORIGINAL, and that is not a hedge: mh.dll runs inside a game that never
// imports a snapshot, so the parse has to happen. This is the guarded shape the census counts as
// "guarded out of the standalone build" rather than as a VA dependency -- the same mechanism
// sim_register_state_handlers.cpp's ini read uses.
namespace {
int32_t load_tutorial_script() {
#ifdef MH_LIBMH_BUILD
    return 1; // the snapshot already carries the table; the original discards this return anyway
#else
    return mh::call::llm_tutorial_load_script();
#endif
}
} // namespace

const start_tutorial_calls &live_start_tutorial_calls() {
    static const start_tutorial_calls c = {
        mh::state::evt::set_display_mode,
        MH_LIBMH_BIND(game_SetEvent),
        mh::state::evt::fade_transition_run,
        mh::state::evt::wgtl_draw,
        mh::state::evt::present_flip,
        load_tutorial_script,
        mh::state::evt::tut_hint_layout,
        MH_CRT(utils_fill_data),
        +[](mh::sim::map_header *hdr) { return mh::host().cfg_ReadMapFile(hdr); },
        mh::host().wide_to_local,
        mh::state::evt::wgtl_center,
        MH_LIBMH_BIND(llm_strat_frame),
    };
    return c;
}

namespace detail {

// ---- llm_game_start_tutorial @0x004bafb1 -----------------------------------------------------
int32_t start_tutorial(const sim_view &v, sim_store &own, const start_tutorial_calls &c,
                       const session_begin_multi_calls &c_sbm, const session_state_reset_calls &c_ssr,
                       const new_game_init_calls &c_ngi, const land_players_on_planet_calls &c_lpop,
                       const planet_map_session_init_calls &c_pmsi) {
    // 0x004bafd0-0x004bafe6: save/restore-save the view size mode, then force size mode 0.
    // SIMABI-DISPLAY (2026-09-10): the second line's STORE is gone with the host entry. The
    // original assigned the call's return -- the APPLIED mode, which the original re-derives from
    // WindowWidth after the resize -- into _G_LLM_VIEW_SIZE_MODE. libmh cannot compute that value,
    // and the cell it landed in is MF_VIEW-only and in neither hash table, so the request crosses
    // as a record and the host performs its own store. The SAVE above is libmh's own state copy
    // and stays, in order, reading the cell before the request moves it.
    own.view_size_mode_save() = own.view_size_mode();
    c.set_display_mode(0);

    // 0x004bafe6-0x004bafeb: force a full HUD redraw. 0xa == EV_HUD_REDRAW_ALL (the enumerator name
    // lives in sim/sim_game_set_event.cpp's anonymous namespace and is not reachable from here).
    c.game_set_event(0xa);

    // 0x004baff0-0x004bb004: arm the fade-out-of-main-menu / fade-into-race-select transition, and
    // clear the one-shot HQ-attack-scenario latch. WIDTH NOTE: the asm stores a 4-byte immediate 0
    // into a region documented uint8_t -- see the header's uncertainties.
    own.ui_fade_transition().src_screen_id = *v.ui_screen_main_menu_id;
    own.ui_fade_transition().dst_screen_id = *v.ui_screen_racebck_id;
    own.tutorial_hq_attack_scenario_done() = 0;

    // 0x004bb00e-0x004bb015: the transition is armed above; ask the host to complete it. The
    // original spun here on the tick entry -- a libmh loop may not poll a host entry, so the wait
    // is stated once and performed host-side (mh.dll runs the same tick loop at this instant).
    c.fade_transition_run();

    // 0x004bb017-0x004bb030: clear the deferred race-select overlay index, draw the tutorial-intro
    // widget list, flip, and load the step script (return discarded -- the asm clobbers EAX with the
    // very next instruction).
    own.ui_race_sel_pending_gfx_idx() = 0;
    c.ui_widget_list_draw(LIBMH_SCR_WGTL_TUTORIAL_INTRO); // R4: an id, not the 0x00653acb address
    c.gfx_present_flip();
    c.tutorial_load_script();

    // 0x004bb035-0x004bb0a6: lay out the tutorial hint widget. label is a CONSTANT POINTER (see
    // header); the font_desc_for_flags return is discarded (stored to a dead stack local in the asm).
    ui_widget &hint = own.ui_tutorial_hint_widget();
    // 0x006573ae in the asm -- a LITERAL, and it is _G_LLM_TUTORIAL_STEPS[0].body (base 0x0065732e
    // + 0x80). Written through the binding now that the region is bound rather than as the raw
    // constant the draft carried: same address, but the derivation is checkable and the
    // translation lint can see the region. An address escape -- the widget stores it, nothing here
    // dereferences it (llm_tutorial_step.body is an inline char16_t[512]; .label is a char *).
    hint.label = reinterpret_cast<char *>(const_cast<char16_t *>(v.tutorial_steps[0].body));
    // 0x004bb03a-0x004bb0a6: the geometry the two sprite-metric queries feed. Both queries and the
    // four assignments they derive are the HOST's now: the metrics have no other consumer, and the
    // hint widget is unhashed and host-relocatable, so nothing libmh owns depends on the result.
    // libmh keeps .label (above) and reads .flags; it never reads the geometry back.
    c.tut_hint_layout(0x42);

    // 0x004bb0a9: enter tutorial step 1.
    own.tutorial_step_mut() = 1;

    // 0x004bb0b3-0x004bb0d6: the four-link chained store (all ultimately 0), transcribed as the
    // literal read-then-store chain the asm performs -- see
    // _G_LLM_TUTORIAL_RESET_SLOT_0050A678's manifest comment.
    own.tutorial_rmb_limit_flag()             = 0;
    own.tutorial_pending_build_placement_id() = own.tutorial_rmb_limit_flag();
    own.tutorial_reset_slot_0050a678()        = own.tutorial_pending_build_placement_id();
    own.tutorial_build_type_filter()          = own.tutorial_reset_slot_0050a678();

    // 0x004bb0db-0x004bb0e7: bulk-zero the whole 4-entry (16-byte) UI panel fallback table.
    c.fill_data(&own.ui_panel_fallback_table_at(0), 0x10, 0);

    // 0x004bb0ec-0x004bb10b: stamp the hardcoded tutorial map path/name (fixed-length block copies,
    // matching the compile-time-constant string lengths incl. NUL: 6 and 12 bytes), then load it.
    std::memcpy(own.current_map_data().path_unc, "Dane\\", 6);
    std::memcpy(own.current_map_data().map_name, "TUTORIAL.MP", 12);
    c.cfg_read_map_file(&own.current_map_data());

    // 0x004bb110-0x004bb17f: the fixed 1v1 player setup.
    player_desc &p0     = own.player_desc_at(0);
    p0.controller_flags = 7;
    p0.race_or_faction  = 1;
    p0.color_or_team    = 3;
    p0.scenario_side_id = 0;
    p0.name[0]          = '1';
    p0.name[1]          = '\0';

    player_desc &p1     = own.player_desc_at(1);
    p1.controller_flags = 0xb;
    p1.race_or_faction  = 2;
    p1.color_or_team    = 1;
    p1.scenario_side_id = -1;
    c.wide_to_short_str(const_cast<wchar_t *>(v.text_ptrs[0x2ac]), p1.name);

    own.player_relation_at(0, 1) = 2;
    own.player_relation_at(1, 0) = 2;
    own.chat_target_mask()       = 0xff;
    own.player_control_mask()    = 0;

    // 0x004bb186-0x004bb1aa: disable the remaining six slots, then stamp the injected-map planet
    // slot index.
    for (int32_t i = 2; i < 8; ++i) {
        own.player_desc_at(i).controller_flags = 0;
    }
    own.injected_map_planet_slot() = 0x1f;

    // 0x004bb1b1-0x004bb21c: copy the now-loaded map's path/name/tlo name into the injected planet
    // slot. Plain NUL-terminated string copies (the asm's byte-pair loop stops at and includes the
    // first NUL, i.e. strcpy semantics -- rule 9, strcpy is CRT-safe). All three destinations are
    // now real char arrays: .tlo_file was a lone `undefined1` when this was written, which is why
    // the draft reached it through a raw cast; the Ghidra retype (2026-09-01, EN v385) made it
    // MH_STR like its two siblings, so the cast is gone and the per-offset static_assert covers it.
    cfg_planet &planet31 = own.cfg_planet_at(0x1f);
    std::strcpy(planet31.path_unc, own.current_map_data().path_unc);
    std::strcpy(planet31.map_name, own.current_map_data().map_name);
    std::strcpy(planet31.tlo_file, own.current_map_data().tlo_name);

    // 0x004bb21d-0x004bb256: neutral per-channel economy on the injected planet.
    for (int32_t i = 0; i < 4; ++i) {
        planet31.source_mul[i] = 1;
        planet31.source_add[i] = 0;
    }

    // 0x004bb256-0x004bb25b: boot the session through the sibling (SIBLING EDGE, see header banner).
    // Return value discarded -- the asm never stores/tests EAX after this call.
    detail::session_begin_multi(v, own, c_sbm, &own.current_map_data(), c_ssr, c_ngi, c_lpop, c_pmsi);

    // 0x004bb260-0x004bb26f: clear the unit-overlay flag mask; re-arm the fade transition to fade
    // out of the race-select screen.
    own.show_unit_flags()                  = 0;
    own.ui_fade_transition().src_screen_id = *v.ui_screen_racebck_id;

    // 0x004bb274-0x004bb27b: the second armed transition, completed host-side like the first. The
    // hashed work above (the player_desc setup and session_begin_multi) still runs BETWEEN the two
    // requests, in the original order -- only the waiting moved.
    c.fade_transition_run();

    // 0x004bb27d-0x004bb2c3: stamp the shared menu-title widget (label reused for a wide pointer,
    // not a bug -- see header), point the tutorial welcome widget's label at a CONSTANT POINTER
    // (unlabelled in Ghidra), swap in the tutorial-welcome widget list and its async redraw
    // callback (both CONSTANT POINTERS -- see header), and set the DLG_STATE_FLAGS bits (two
    // literal stores, see header note).
    own.ui_wgt_menu_screen_title().label = reinterpret_cast<char *>(const_cast<wchar_t *>(v.text_ptrs[0x2b2]));
    own.ui_wgt_tutorial_welcome().label  = reinterpret_cast<char *>(0x0065d3eeu);        // unlabelled in Ghidra
    own.ui_menu_widget_list()            = reinterpret_cast<widget_list *>(0x00653b63u); // _G_LLM_UI_WGT_LIST_TUTORIAL_WELCOME
    own.ui_menu_async_callback_a()       = reinterpret_cast<void *>(0x004baf11u);        // llm_tutorial_menu_redraw_cb
    own.dlg_state_flags() |= 1;                                                          // 0x004bb2a5, byte OR
    own.dlg_state_flags() = (own.dlg_state_flags() & 0xfffffffd) | 2;                    // 0x004bb2b4-0x004bb2bd, dword AND/OR

    // 0x004bb2c3-0x004bb2d2: position the shared frame/backdrop widget and run one strategic frame.
    // Reached in the asm as (*ui_menu_widget_list())->frame->x, which is the SAME region as
    // ui_wgt_frame_menu_panel() (see sim_state.h's own comment on that accessor) -- written directly.
    own.ui_wgt_frame_menu_panel().x = -0x50;
    c.strat_frame();

    // 0x004bb2d7-0x004bb2e6: centre the (now tutorial-welcome) widget list, and run a second
    // strategic frame.
    // The original centres `*ui_menu_widget_list()`, set to the WELCOME list a few lines above, so
    // the argument is provably that constant and crosses as its id (R4).
    c.ui_widget_list_center(LIBMH_SCR_WGTL_TUTORIAL_WELCOME);
    c.strat_frame();

    // 0x004bb2e6-0x004bb2f9: unconditional constant return.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t start_tutorial() {
    sim_state st = state();
    return detail::start_tutorial(st.read, st.own, live_start_tutorial_calls());
}

} // namespace mh::sim
