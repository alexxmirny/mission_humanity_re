//
// lockstep/overlay_hoist.cpp -- see overlay_hoist.h. Address evidence per store is in the
// header banner and the stage-E split spec (tracker LIB-ABI, 2026-09-03).
//
#include "lockstep/overlay_hoist.h"
#include "lockstep/turn_engine.h" // fixes(): [net] overlay_dialog_guard

#include "addr/mh_addrs.gen.h"
#include "addr/mh_structs.gen.h" // the player profile the panel-page hoist indexes
#include "addr/mh_regions.gen.h" // the state region REGISTRY -- this file is a BINDER, see below

namespace mh::lockstep {

// The `RID_*` enumerators live in `mh::state`; this whole file names them.
using namespace mh::state;

// THIS TU IS A BINDER, and tools/check_sim_addresses.py names it as one of libmh/lockstep/'s two
// (the other being lockstep_state.cpp, where the twelve translated-logic state structs are bound).
// It qualifies on the same grounds libmh/orders/ has two: every function below is a one-line read or
// write of a single game global, handed to the engine as an entry in `overlay_hoist_ops`. There is
// no translated logic here to keep away from an address -- the file IS the address-to-op adapter --
// so routing these nine reads through a bound struct would add an indirection and guarantee nothing
// that re-resolving on every call does not already give.
//
// SB-BIND: each is `mh::state::ptr<T>(RID_X)`, resolved fresh per call, so a host that binds a
// relocated region is followed here. Four of these regions did not exist in the registry until T4 --
// they had been in the addr manifest since 2026-09-03 and nothing noticed, which is the gate hole
// commit 4927de6e closed.

namespace {

inline uint8_t &game_mode_ref() { return *mh::state::ptr<uint8_t>(RID_GAME_MODE); }

inline uintptr_t widget_list() { return *mh::state::ptr<const uintptr_t>(RID_UI_MENU_WIDGET_LIST); }

// The widget-list identities the dismiss rule compares against. These are ADDRESSES USED AS VALUES:
// the game stores a pointer to whichever widget list is current, and "is the slot free" is a
// comparison against two known lists, not a read through them. Under a relocated bind the stored
// pointer is whatever the game wrote, so the comparand has to come from the same live table -- a
// stock `mh::addr::` constant would stop matching the moment either list moved.
inline uintptr_t wgt_gameplay_hud() {
    return reinterpret_cast<uintptr_t>(mh::state::ptr<void>(RID_UI_WGT_LIST_GAMEPLAY_HUD));
}

inline uintptr_t wgt_lockstep_sync() {
    return reinterpret_cast<uintptr_t>(mh::state::ptr<void>(RID_UI_WGT_LIST_LOCKSTEP_SYNC));
}

} // namespace

void outcome_dialog_panel_hoist() {
    // The gate the callee reads at 0x004c6c5e. Closed -> the original wrote nothing, so neither
    // does the hoist; that asymmetry is the whole point of reading the flag rather than guessing.
    if (*mh::state::ptr<const int32_t>(RID_PLANET_SEL_SCREEN_OPEN) == 0) return;

    // game_SetEvent's EV_BUILD_OPEN_AUTOPAGE case, transcribed -- the same three stores
    // sim_game_set_event.cpp:167-171 makes, over the same regions.
    *mh::state::ptr<int32_t>(RID_STRAT_UI_PANEL_SWITCH_PENDING) = 1;
    *mh::state::ptr<int32_t>(RID_STRAT_UI_PANEL_MODE)           = 1;
    const auto   *profiles                                      = mh::state::ptr<const mh::game::mh_llm_strat_player_profile>(RID_STRAT_PLAYERS);
    const int16_t side                                          = *mh::state::ptr<const int16_t>(RID_PLAYERSIDE);
    const int32_t planet                                        = *mh::state::ptr<const int32_t>(RID_G_PLANET_INDEX);
    *mh::state::ptr<int32_t>(RID_STRAT_UI_PANEL_PAGE) =
        (profiles[side].primary_mother_bldg[planet] == 0) ? 1 : 0;
}

namespace {

// U44 -- the stall overlay's own ledger. UI-only, never hashed: whether the screen the overlay code
// is about to dismiss is one lockstep armed, and the dialog a kick modal displaced.
struct overlay_ledger {
    bool ours = false; // the icon / kick modal is lockstep's own, armed and not yet dismissed
    // latched by overlay_mode_not4() for the dismiss that follows it
    bool    latch_ours = false;
    uint8_t latch_mode = 0;
    struct {
        bool     valid = false;
        uint32_t list = 0, dialog = 0, cb = 0, cb_a = 0, cb_b = 0, flags = 0;
        uint8_t  menu_state = 0;
    } stash;
};
overlay_ledger g_ov;

} // namespace

bool overlay_mode_not4() {
    g_ov.latch_mode = game_mode_ref();
    g_ov.latch_ours = g_ov.ours;
    return game_mode_ref() != 4;
}


bool overlay_mode_is2() { return game_mode_ref() == 2; }

bool overlay_slot_free() {
    const uintptr_t list = widget_list();
    return list == 0 || list == wgt_gameplay_hud();
}

void overlay_show_hoist(bool armed) {
    if (!mh::lockstep::fixes().overlay_dialog_guard || !armed) return;
    // A foreign screen is mode 3 with a free list that lockstep did not arm: a dialog (the building
    // dialog runs with list NULL). Stash its UI state; the show then owns a clean screen.
    if (game_mode_ref() == 3 && !g_ov.ours && !g_ov.stash.valid) {
        auto &st                                        = g_ov.stash;
        st.list                                         = *mh::state::ptr<const uint32_t>(RID_UI_MENU_WIDGET_LIST);
        st.dialog                                       = *mh::state::ptr<const uint32_t>(RID_UI_ACTIVE_DIALOG);
        st.cb                                           = *mh::state::ptr<const uint32_t>(RID_UI_MENU_ASYNC_CALLBACK);
        st.cb_a                                         = *mh::state::ptr<const uint32_t>(RID_UI_MENU_ASYNC_CALLBACK_A);
        st.cb_b                                         = *mh::state::ptr<const uint32_t>(RID_UI_MENU_ASYNC_CALLBACK_B);
        st.flags                                        = *mh::state::ptr<const uint32_t>(RID_DLG_STATE_FLAGS);
        st.menu_state                                   = *mh::state::ptr<const uint8_t>(RID_UI_MENU_STATE);
        st.valid                                        = true;
        *mh::state::ptr<uint32_t>(RID_UI_ACTIVE_DIALOG) = 0;
    }
    g_ov.ours = true;
}

void overlay_dismiss_hoist(bool not4) {
    *mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_WAIT_PLAYER_IDX) = -1;
    if (!not4) return;
    if (mh::lockstep::fixes().overlay_dialog_guard && !g_ov.latch_ours) {
        // Not lockstep's screen. Hosted, the original has just forced mode 2/3 over it; put the
        // pre-call mode back. Unhosted nothing wrote, and this is the identity.
        game_mode_ref() = g_ov.latch_mode;
        return;
    }
    const uintptr_t list = widget_list();
    // The fold: LOCKSTEP_SYNC counts as "slot free" because the real arm nulls it before the
    // mode write -- see the header banner's host-arm-independence note.
    const bool free_after =
        list == 0 || list == wgt_gameplay_hud() || list == wgt_lockstep_sync();
    game_mode_ref() = free_after ? 2 : 3;
    g_ov.ours       = false;
    if (g_ov.stash.valid) {
        // Give the player's dialog back exactly as the modal found it. Hosted, the original
        // dismiss has already nulled A / the list and cleared the flags; unhosted nothing ran.
        const auto &st                                          = g_ov.stash;
        *mh::state::ptr<uint32_t>(RID_UI_MENU_WIDGET_LIST)      = st.list;
        *mh::state::ptr<uint32_t>(RID_UI_ACTIVE_DIALOG)         = st.dialog;
        *mh::state::ptr<uint32_t>(RID_UI_MENU_ASYNC_CALLBACK)   = st.cb;
        *mh::state::ptr<uint32_t>(RID_UI_MENU_ASYNC_CALLBACK_A) = st.cb_a;
        *mh::state::ptr<uint32_t>(RID_UI_MENU_ASYNC_CALLBACK_B) = st.cb_b;
        *mh::state::ptr<uint32_t>(RID_DLG_STATE_FLAGS)          = st.flags;
        *mh::state::ptr<uint8_t>(RID_UI_MENU_STATE)             = st.menu_state;
        game_mode_ref()                                         = 3;
        g_ov.stash.valid                                        = false;
    }
}

void wait_player_hoist(int32_t player_idx, bool armed) {
    *mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_OVERLAY_RESULT)  = -1;
    *mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_WAIT_PLAYER_IDX) = player_idx;
    if (armed) {
        game_mode_ref() = 3;
        g_ov.ours       = true;
    } // U44: the dialog's own frame function is still on the stack on the frame the modal armed (the
    // stall is detected from inside its redraw) and re-publishes ACTIVE_DIALOG after we nulled it.
    // This runs on every parked frame, so the stash stays authoritative until the modal is gone.
    if (mh::lockstep::fixes().overlay_dialog_guard && g_ov.stash.valid)
        *mh::state::ptr<uint32_t>(RID_UI_ACTIVE_DIALOG) = 0;
}

int32_t overlay_result() {
    return *mh::state::ptr<const int32_t>(RID_NET_LOCKSTEP_OVERLAY_RESULT);
}

void overlay_result_clear() {
    *mh::state::ptr<int32_t>(RID_NET_LOCKSTEP_OVERLAY_RESULT) = -1;
}

void game_mode_saved_set(uint8_t mode) {
    *mh::state::ptr<uint8_t>(RID_GAME_MODE_SAVED) = mode;
}

uint8_t game_mode_saved() {
    return *mh::state::ptr<const uint8_t>(RID_GAME_MODE_SAVED);
}

void game_mode_set(uint8_t mode) { game_mode_ref() = mode; }

const overlay_hoist_ops &live_overlay_hoist_ops() {
    static const overlay_hoist_ops ops = {
        overlay_mode_not4,
        overlay_mode_is2,
        overlay_slot_free,
        overlay_dismiss_hoist,
        wait_player_hoist,
        overlay_result,
        overlay_result_clear,
        game_mode_saved,
        game_mode_set,
        outcome_dialog_panel_hoist,
        overlay_show_hoist,
    };
    return ops;
}

} // namespace mh::lockstep
