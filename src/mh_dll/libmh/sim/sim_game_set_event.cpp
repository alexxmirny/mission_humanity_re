//
// sim/sim_game_set_event.cpp -- see sim_game_set_event.h. Translated from the DISASSEMBLY
// (tmp/decomp/game_SetEvent_00413a52.asm); the Ghidra .c draft agrees, but the defer-path ring
// arithmetic (0x00413a75-0x00413ae0), the dense 24-case switch and the trailing fallback pass
// (0x004140b6-0x004140fc) were each re-read against the raw instructions rather than trusted.
//
#include "sim/sim_game_set_event.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_events.h"

namespace mh::sim {

const game_set_event_calls &live_game_set_event_calls() {
    static const game_set_event_calls c = {
        mh::state::evt::planet_select_screen_open_i32,
        mh::state::evt::snd_play,
    };
    return c;
}

namespace {

// game::e::event members. The jump-table domain is 0x00..0x18 INCLUSIVE (asm: `CMP [EBP-0x20],0x18;
// JA <tail>`, 25 slots). Values 0x00..0x0f are dense and cross-checked with sibling pins
// (BUILD_PROJECTS_REFRESH=7, BUILD_BUILDINGS_REFRESH=15). BUT THERE IS A HOLE AT 0x10: the jump
// table's slot for value 0x10 (Ghidra caseD_10) IS the address 0x00414078 -- the shared
// tutorial-suppression + fallback tail every other case falls into -- so real value 0x10 is a NO-OP.
// The eight bodies after BUILD_BUILDINGS_REFRESH therefore live at 0x11..0x18, one HIGHER than a
// naive dense reading of the .c gives (an off-by-one caught by reimpl-verify 2026-08-18: the first
// draft mapped them 0x10..0x17 and dropped 0x18=PANEL_REFRESH_CURRENT). No enumerator/case for 0x10;
// it falls to `default` (a no-op) exactly like an out-of-range code.
enum : uint32_t {
    EV_PANEL_SHOW_MAP          = 0x00,
    EV_PANEL_SHOW_BUILD        = 0x01,
    EV_PANEL_SHOW_INFO         = 0x02,
    EV_BUILD_TAB_BUILDINGS     = 0x03,
    EV_BUILD_TAB_UNITS         = 0x04,
    EV_BUILD_TAB_PROJECTS      = 0x05,
    EV_INFO_REFRESH            = 0x06,
    EV_BUILD_PROJECTS_REFRESH  = 0x07,
    EV_BUILD_UNITS_REFRESH     = 0x08,
    EV_BUILD_OPEN_AUTOPAGE     = 0x09,
    EV_HUD_REDRAW_ALL          = 0x0a,
    EV_ACTION_DENIED_FEEDBACK  = 0x0b,
    EV_PANEL_SHOW_MAP_OBJECTS  = 0x0c,
    EV_MAP_SUBMODE1_REFRESH    = 0x0d,
    EV_MAP_OBJECTS_REFRESH     = 0x0e,
    EV_BUILD_BUILDINGS_REFRESH = 0x0f,
    // 0x10: NO-OP (jump-table slot aliases the shared tail) -- deliberately no case.
    EV_PANEL_SHOW_INFO_TAB0  = 0x11,
    EV_PANEL_SHOW_INFO_TAB1  = 0x12,
    EV_CHAT_INPUT_OPEN       = 0x13,
    EV_CHAT_INPUT_CLOSE      = 0x14,
    EV_BUILD_HOTKEY_TOGGLE   = 0x15,
    EV_INFO_HOTKEY_TOGGLE    = 0x16,
    EV_MAP_HOTKEY_TOGGLE     = 0x17,
    EV_PANEL_REFRESH_CURRENT = 0x18,
};

// The ACTION_DENIED/MAP_HOTKEY "planet-select in SP else sound" gate reads the session mode; llm_snd_play
// sound id for the denied-feedback beep.
inline constexpr int32_t DENIED_SND_ID  = 0xb3;
inline constexpr int32_t DENIED_SND_VOL = 100;
inline constexpr int32_t RING_MASK      = 0x100; // 256-entry deferral ring, index taken mod this

} // namespace

namespace detail {

// game_SetEvent @0x00413a52.
uint32_t game_set_event(const sim_view &v, sim_store &own, const game_set_event_calls &c,
                        game_e_event type) {
    // 0x00413a5f: save the panel mode BEFORE anything mutates it -- the tutorial suppression below
    // restores this exact value.
    const int32_t saved_mode = own.ui_panel_mode();

    // 0x00413a75-0x00413ae0: the DEFERRAL path. When the drain flag is set the event is not applied;
    // it is pushed onto the ring buffer (skipping an immediate duplicate of the current head) with a
    // trailing 0xffffffff terminator, and the function returns a ring position rather than acting.
    if (*v.ui_event_defer_active != 0) {
        // 0x00413a82-0x00413a93: a duplicate at the current head is dropped, returning that value.
        if (own.ui_event_queue_at(own.ui_event_queue_pos()) == static_cast<int32_t>(type)) {
            return static_cast<uint32_t>(own.ui_event_queue_at(own.ui_event_queue_pos()));
        }
        // 0x00413a95-0x00413ac2: advance head (mod 256, matching the signed IDIV -- pos is >=0 so it
        // is an ordinary modulo) and store the event there.
        own.ui_event_queue_pos()                        = (own.ui_event_queue_pos() + 1) % RING_MASK;
        own.ui_event_queue_at(own.ui_event_queue_pos()) = static_cast<int32_t>(type);
        // 0x00413ac8-0x00413ae0: terminate the next slot with 0xffffffff and return that slot * 4.
        const int32_t term_slot          = (own.ui_event_queue_pos() + 1) % RING_MASK;
        own.ui_event_queue_at(term_slot) = static_cast<int32_t>(0xffffffffu);
        return static_cast<uint32_t>(term_slot * 4);
    }

    // 0x00413b56-0x004140b0: the switch over the 0x00..0x18 jump-table domain (24 live bodies; value
    // 0x10 is a no-op hole -- see the enum banner). Codes > 0x18, and 0x10, fall through to the
    // tutorial/fallback tail below, exactly as the original's default jump-table entry.
    switch (type) {
        case EV_PANEL_SHOW_MAP:
            own.ui_panel_switch_pending() = 1;
            own.ui_panel_mode()           = 0;
            break;
        case EV_PANEL_SHOW_BUILD:
            if (own.ui_panel_mode() != 1) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 1;
                // Comma-operator in the .c: page 2 is cleared to 0 as a side effect, then if the local
                // player has no primary mother building on this planet it becomes 1 (units page).
                if (own.ui_panel_page() == 2) {
                    own.ui_panel_page() = 0;
                    if (v.profiles[*v.player_side].primary_mother_bldg[*v.planet_index] == 0) {
                        own.ui_panel_page() = 1;
                    }
                }
            }
            break;
        case EV_PANEL_SHOW_INFO:
            own.ui_panel_switch_pending() = 1;
            own.ui_panel_mode()           = 2;
            break;
        case EV_BUILD_TAB_BUILDINGS:
            if ((*v.ui_bldg_tab_select_blocked == 0) &&
                (v.profiles[*v.player_side].primary_mother_bldg[*v.planet_index] != 0)) {
                if (own.ui_panel_mode() != 1) {
                    own.ui_panel_switch_pending() = 1;
                    own.ui_panel_mode()           = 1;
                }
                own.ui_bldg_panel_refresh_pending() = 1;
                own.ui_panel_page()                 = 0;
            }
            break;
        case EV_BUILD_TAB_UNITS:
            if (own.ui_panel_mode() != 1) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 1;
            }
            own.ui_bldg_panel_refresh_pending() = 1;
            own.ui_panel_page()                 = 1;
            break;
        case EV_BUILD_TAB_PROJECTS:
            if (own.ui_panel_mode() != 1) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 1;
            }
            own.ui_bldg_panel_refresh_pending() = 1;
            own.ui_panel_page()                 = 2;
            break;
        case EV_INFO_REFRESH:
            if (own.ui_panel_mode() == 2) {
                own.ui_unit_panel_refresh_pending() = 1;
            }
            break;
        case EV_BUILD_PROJECTS_REFRESH:
            if ((own.ui_panel_mode() == 1) && (own.ui_panel_page() == 2)) {
                own.ui_bldg_panel_refresh_pending() = 1;
            }
            break;
        case EV_BUILD_UNITS_REFRESH:
            if ((own.ui_panel_mode() == 1) && (own.ui_panel_page() == 1)) {
                own.ui_bldg_panel_refresh_pending() = 1;
            }
            break;
        case EV_BUILD_OPEN_AUTOPAGE:
            own.ui_panel_switch_pending() = 1;
            own.ui_panel_mode()           = 1;
            // page = (no primary mother building here) ? 1 : 0
            own.ui_panel_page() =
                (v.profiles[*v.player_side].primary_mother_bldg[*v.planet_index] == 0) ? 1 : 0;
            break;
        case EV_HUD_REDRAW_ALL:
            own.ui_panel_switch_pending() = 1;
            own.ui_view_resize_pending()  = 1;
            break;
        case EV_ACTION_DENIED_FEEDBACK:
            if (*v.session_mode == SESSION_SP) {
                c.planet_select_screen_open(0);
                // R3b HOIST (LIFT-R3B, 2026-09-09): llm_ui_planet_select_screen_build @0x004c5de5
                // sets _G_LLM_GAME_MODE = 3 as its second statement, BEFORE any layout work and
                // before it re-enters the frame driver. GAME_MODE's readers are the per-frame mode
                // dispatchers (llm_strat_frame, frame_redraw_behind_dialog, tutorial_step_driver),
                // so a poll host that defers the screen-open would leave them dispatching on the
                // old mode for a frame. Re-stored here so libmh is the last writer either way.
                own.game_mode() = 3;
            } else {
                c.snd_play(DENIED_SND_ID, DENIED_SND_VOL);
            }
            break;
        case EV_PANEL_SHOW_MAP_OBJECTS:
            if (own.ui_panel_mode() != 0) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 0;
            }
            own.ui_mainpanel_refresh_pending() = 1;
            own.ui_mainpanel_tab_index()       = 2;
            break;
        case EV_MAP_SUBMODE1_REFRESH:
            if ((own.ui_panel_mode() == 0) && (own.ui_mainpanel_tab_index() == 1)) {
                own.ui_mainpanel_refresh_pending() = 1;
            }
            break;
        case EV_MAP_OBJECTS_REFRESH:
            if ((own.ui_panel_mode() == 0) && (own.ui_mainpanel_tab_index() == 2)) {
                own.ui_mainpanel_refresh_pending() = 1;
            }
            break;
        case EV_BUILD_BUILDINGS_REFRESH:
            if ((own.ui_panel_mode() == 1) && (own.ui_panel_page() == 0)) {
                own.ui_bldg_panel_refresh_pending() = 1;
            }
            break;
        case EV_PANEL_SHOW_INFO_TAB0:
            if (own.ui_panel_mode() != 2) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 2;
            }
            own.ui_unit_panel_refresh_pending() = 1;
            own.ui_unit_tab_toggle()            = 0;
            break;
        case EV_PANEL_SHOW_INFO_TAB1:
            if (own.ui_panel_mode() != 2) {
                own.ui_panel_switch_pending() = 1;
                own.ui_panel_mode()           = 2;
            }
            own.ui_unit_panel_refresh_pending() = 1;
            own.ui_unit_tab_toggle()            = 1;
            break;
        case EV_CHAT_INPUT_OPEN:
            own.chat_input_active()  = 1;
            own.chat_input_len()     = 0;
            own.chat_input_cursor()  = 0;
            own.chat_input_line()[0] = '\0';
            break;
        case EV_CHAT_INPUT_CLOSE:
            own.chat_input_active() = 0;
            break;
        case EV_BUILD_HOTKEY_TOGGLE:
            if (own.ui_panel_mode() == 1) {
                if ((own.ui_panel_page() == 2) || (own.ui_panel_page() == 0)) {
                    own.ui_panel_page() = 1;
                } else {
                    // On the units page: cycle back to buildings only if selection is not blocked and a
                    // primary mother building exists; otherwise leave the page as-is (break out entirely).
                    if ((*v.ui_bldg_tab_select_blocked != 0) ||
                        (v.profiles[*v.player_side].primary_mother_bldg[*v.planet_index] == 0)) {
                        break;
                    }
                    own.ui_panel_page() = 0;
                }
            } else {
                own.ui_panel_mode()           = 1;
                own.ui_panel_switch_pending() = 1;
            }
            own.ui_bldg_panel_refresh_pending() = 1;
            break;
        case EV_INFO_HOTKEY_TOGGLE:
            if (own.ui_panel_mode() == 2) {
                own.ui_unit_tab_toggle() = (own.ui_unit_tab_toggle() == 0) ? 1 : 0;
            } else {
                own.ui_panel_mode()           = 2;
                own.ui_panel_switch_pending() = 1;
            }
            own.ui_unit_panel_refresh_pending() = 1;
            break;
        case EV_MAP_HOTKEY_TOGGLE:
            if ((own.ui_panel_mode() == 0) && (*v.session_mode == SESSION_SP)) {
                c.planet_select_screen_open(0);
                // R3b HOIST (LIFT-R3B, 2026-09-09): llm_ui_planet_select_screen_build @0x004c5de5
                // sets _G_LLM_GAME_MODE = 3 as its second statement, BEFORE any layout work and
                // before it re-enters the frame driver. GAME_MODE's readers are the per-frame mode
                // dispatchers (llm_strat_frame, frame_redraw_behind_dialog, tutorial_step_driver),
                // so a poll host that defers the screen-open would leave them dispatching on the
                // old mode for a frame. Re-stored here so libmh is the last writer either way.
                own.game_mode() = 3;
            } else {
                own.ui_panel_mode()                = 0;
                own.ui_panel_switch_pending()      = 1;
                own.ui_mainpanel_tab_index()       = 2;
                own.ui_mainpanel_refresh_pending() = 1;
            }
            break;
        case EV_PANEL_REFRESH_CURRENT:
            if (own.ui_panel_mode() != 0) {
                if (own.ui_panel_mode() < 2) {
                    own.ui_bldg_panel_refresh_pending() = 1;
                } else if (own.ui_panel_mode() == 2) {
                    own.ui_unit_panel_refresh_pending() = 1;
                }
            }
            break;
        default:
            break;
    }

    // 0x004140b6-0x004140cc: tutorial mode suppresses a switch INTO the map panel -- undo the
    // pending flag and restore the panel mode to whatever it was on entry.
    if ((*v.tutorial_step != 0) && (own.ui_panel_mode() == 0)) {
        own.ui_panel_switch_pending() = 0;
        own.ui_panel_mode()           = saved_mode;
    }

    // 0x004140cf-0x004140fc: the trailing fallback pass. Walk the 0-terminated fallback table; the
    // LAST entry that differs from the current panel mode wins, and if it is positive it is forced as
    // the panel mode. `ret` starts as (count of entries scanned) * 4, overwritten by the forced id.
    int32_t forced = 0;
    int32_t i      = 0;
    for (; v.ui_panel_fallback_table[i] != 0; ++i) {
        if (v.ui_panel_fallback_table[i] != own.ui_panel_mode()) {
            forced = v.ui_panel_fallback_table[i];
        }
    }
    uint32_t ret = static_cast<uint32_t>(i * 4);
    if (forced > 0) {
        own.ui_panel_mode()           = forced;
        own.ui_panel_switch_pending() = 1;
        ret                           = static_cast<uint32_t>(forced);
    }
    return ret;
}

} // namespace detail

// ---- the public wrapper ---------------------------------------------------------------------------

uint32_t game_set_event(game_e_event type) {
    sim_state st = state();
    return detail::game_set_event(st.read, st.own, live_game_set_event_calls(), type);
}


} // namespace mh::sim
