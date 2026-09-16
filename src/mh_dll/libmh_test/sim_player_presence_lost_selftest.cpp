//
// sim_player_presence_lost_selftest.cpp -- `simtest` offline oracle for llm_strat_player_presence_lost
// (sim/sim_player_presence_lost.{h,cpp}, RI-SIM / SIM1F batch F; the batch done_when's
// centrepiece). The function is a player-elimination / win-loss resolver: an early "still has
// presence" bail-out, an SP-campaign path (enemy-eliminated announce / local defeat / system capture
// / victory) and an MP path (last-man-standing / no-other-human / all-allied evaluation), ending in a
// fanfare + a modal outcome dialog. Every outward call is effectful, so it is DO-NOT-ARM (no shadow)
// and this offline oracle is the ONLY proof.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_strat_player_presence_lost_00498089.asm).
// The done_when's two mandated cases are the FIRST two below: a SURVIVING-player run (early return, the
// outcome dialog must NOT fire) and a NO-SURVIVOR run (the dialog fires with the right code).
//
#include "sim/sim_player_presence_lost.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorded outward calls ----------------------------------------------------------------------
int32_t              g_sprintf_calls; // w_sprintf__vss
int32_t              g_print_text;    // game_ui_PrintTextMessage
std::vector<int32_t> g_snd_ids;       // llm_snd_play sound ids
struct ProgCall {
    uint16_t plr, inv;
};
std::vector<ProgCall> g_update_progress; // game_UpdateProgress
std::vector<int32_t>  g_queue_text;      // llm_ui_print_queue_text_id
int32_t               g_ansi_to_wide;    // llm_str_ansi_to_wide_scratch
int32_t               g_str_copy;        // utils_w_str_copy
int32_t               g_floating_red;    // llm_ui_print_floating_msg_red
int32_t               g_net_send;        // llm_net_lockstep_send_presence_lost
int32_t               g_overlay_dismiss; // llm_net_lockstep_overlay_dismiss
int32_t               g_dialog_calls;    // llm_ui_outcome_dialog call count
int32_t               g_dialog_last;     // ...and the last outcome code it saw

wchar_t g_scratch_dummy[8] = {0};

int32_t stub_sprintf(void *, const wchar_t *, const wchar_t *, const wchar_t *) {
    g_sprintf_calls++;
    return 0;
}
uint32_t stub_print_text(void *) {
    g_print_text++;
    return 0;
}
void  stub_snd(int32_t id, int32_t) { g_snd_ids.push_back(id); }
void  stub_update_progress(uint16_t plr, uint16_t inv) { g_update_progress.push_back({plr, inv}); }
void  stub_queue_text(int32_t id) { g_queue_text.push_back(id); }
void *stub_ansi_to_wide(char *) {
    g_ansi_to_wide++;
    return g_scratch_dummy;
}
void *stub_str_copy(void *, void *dst) {
    g_str_copy++;
    return dst;
}
int32_t stub_floating_red(void *) {
    g_floating_red++;
    return 0;
}
void    stub_net_send() { g_net_send++; }
int32_t stub_overlay_dismiss() {
    g_overlay_dismiss++;
    return 0;
}
int32_t stub_dialog(uint8_t outcome) {
    g_dialog_calls++;
    g_dialog_last = outcome;
    return 0;
}

const player_presence_lost_calls g_calls = {
    stub_sprintf, stub_print_text, stub_snd, stub_update_progress,
    stub_queue_text, stub_ansi_to_wide, stub_str_copy, stub_floating_red,
    stub_net_send, stub_overlay_dismiss, stub_dialog};

void clear_calls() {
    g_sprintf_calls = g_print_text = g_ansi_to_wide = g_str_copy = g_floating_red = 0;
    g_net_send = g_overlay_dismiss = g_dialog_calls = g_dialog_last = 0;
    g_snd_ids.clear();
    g_update_progress.clear();
    g_queue_text.clear();
}

uint32_t run(sim_fixture &fx, uint32_t player, uint32_t mode) {
    clear_calls();
    sim_store own = fx.store();
    return detail::player_presence_lost(fx.view(), own, g_calls, player, mode);
}

constexpr uint32_t STATUS_ALIVE = 0x2u;
constexpr uint32_t STATUS_HUMAN = 0x4u;
constexpr int32_t  SP           = 1; // SESSION_SP
constexpr int32_t  MP_LOCAL     = 2; // SESSION_MP_LOCAL
constexpr int32_t  MP_LOCKSTEP  = 3; // SESSION_MP_LOCKSTEP

} // namespace

void run_player_presence_lost_tests() {
    sim_fixture fx;

    // ================================================================================================
    // done_when CASE 1 -- a SURVIVING player: the early presence check bails out, and the outcome
    // dialog must NOT fire. Player 1 is alive, this is a natural loss (mode 0), and still holds a
    // building on the current planet.
    // ================================================================================================
    fx.reset();
    fx.session_mode                   = SP;
    fx.planet_index                   = 4;
    fx.profiles[1].status_flags       = STATUS_ALIVE;
    fx.profiles[1].buildings_alive[4] = 3; // still has a building here
    fx.profiles[1].units_alive[4]     = 0;
    {
        uint32_t r = run(fx, 1, 0);
        ck_eq(r, 0u, "SURVIVE: returns units_alive[planet] (0, per the asm's final load)");
        ck_eq((uint32_t)g_dialog_calls, 0u, "SURVIVE: outcome dialog does NOT fire");
        ck_eq((uint32_t)g_queue_text.size(), 0u, "SURVIVE: no queue text");
        ck_eq((uint32_t)fx.profiles[1].status_flags, STATUS_ALIVE, "SURVIVE: alive bit untouched");
    }
    // A surviving player via a UNIT (buildings 0, units > 0) also bails and returns the unit count.
    fx.reset();
    fx.session_mode                   = SP;
    fx.planet_index                   = 4;
    fx.profiles[1].status_flags       = STATUS_ALIVE;
    fx.profiles[1].buildings_alive[4] = 0;
    fx.profiles[1].units_alive[4]     = 5;
    {
        uint32_t r = run(fx, 1, 0);
        ck_eq(r, 5u, "SURVIVE(unit): returns units_alive[planet] = 5");
        ck_eq((uint32_t)g_dialog_calls, 0u, "SURVIVE(unit): no dialog");
    }

    // ================================================================================================
    // done_when CASE 2 -- NO survivor: the LOCAL player is fully eliminated in SP, the outcome dialog
    // fires with the defeat code (4) and the loss fanfare (0xa6) plays. current_system 9 holds no
    // planets, so both system scans are empty; no shuttle remains.
    // ================================================================================================
    fx.reset();
    fx.session_mode             = SP;
    fx.player_side              = 0; // me == player -> local defeat
    fx.planet_index             = 0;
    fx.current_system           = 9; // no planet has system_index 9
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN;
    // no buildings/units anywhere (reset zeroed them), no shuttle (reset zeroed them)
    {
        uint32_t r = run(fx, 0, 0);
        ck_eq((uint32_t)g_dialog_calls, 1u, "DEFEAT: outcome dialog fires once");
        ck_eq((uint32_t)g_dialog_last, 4u, "DEFEAT: dialog code = 4");
        ck(g_queue_text.size() == 1 && g_queue_text[0] == 0x7e, "DEFEAT: queue text 0x7e (you lose)");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa6, "DEFEAT: loss fanfare 0xa6");
        (void)r;
    }

    // ================================================================================================
    // SP -- full SYSTEM CAPTURE, LOW system, message not yet shown -> outcome 3 (captured banner) +
    // win fanfare (0xa5). me=0 conquers; player 1 is the last enemy, with no presence and no other
    // alive player. planet 1 is in system 0, already explored + acquired.
    // ================================================================================================
    fx.reset();
    fx.session_mode   = SP;
    fx.player_side    = 0;
    fx.planet_index   = 1;
    fx.current_system = 1; // < 3 -> low system (nonzero so the reset-zeroed planets,
                           // all system 0, are NOT swept as members of this system)
    fx.system_lost_msg_shown_flag                    = 0;
    fx.profiles[0].status_flags                      = STATUS_ALIVE | STATUS_HUMAN; // conqueror
    fx.profiles[1].status_flags                      = STATUS_ALIVE;                // the losing enemy (cleared below)
    fx.cfg_planets[1].system_index                   = 1;                           // the only planet in this system
    fx.cfg_planets[1].invention_index                = 2;
    fx.planet_status[1]                              = 1; // explored (not UNKNOWN)
    fx.progress[0 * PROGRESS_ROW_COUNT + 2].acquired = 1; // planet 1's invention already owned
    {
        uint32_t r = run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_calls, 1u, "SYS-CAPTURE: dialog fires");
        ck_eq((uint32_t)g_dialog_last, 3u, "SYS-CAPTURE: dialog code = 3");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa5, "SYS-CAPTURE: win fanfare 0xa5");
        ck((uint32_t)(fx.profiles[1].status_flags & STATUS_ALIVE) == 0u, "SYS-CAPTURE: enemy alive bit cleared");
        // Two banners fire in a full SP capture: the enemy-eliminated ANNOUNCE (me != player) then the
        // system-captured banner -- so both w_sprintf and the text print run twice.
        ck(g_sprintf_calls == 2 && g_print_text == 2, "SYS-CAPTURE: announce + 'captured' banners both printed");
        (void)r;
    }

    // ---- SP full system capture, HIGH system (>=3) -> outcome 5 (victory) + win fanfare ------------
    fx.reset();
    fx.session_mode                                  = SP;
    fx.player_side                                   = 0;
    fx.planet_index                                  = 1;
    fx.current_system                                = 5; // >= 3
    fx.profiles[0].status_flags                      = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[1].status_flags                      = STATUS_ALIVE;
    fx.cfg_planets[1].system_index                   = 5;
    fx.cfg_planets[1].invention_index                = 2;
    fx.planet_status[1]                              = 1;
    fx.progress[0 * PROGRESS_ROW_COUNT + 2].acquired = 1;
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_last, 5u, "SP-VICTORY: dialog code = 5");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa5, "SP-VICTORY: win fanfare 0xa5");
    }

    // ---- SP enemy-eliminated ANNOUNCE + early return (unexplored planet blocks the capture) --------
    // outcome 1 is transient and never reaches the dialog. Exercises the announce (w_sprintf +
    // print_text), the grant (update_progress), and the debug-cheat resource-yield-cut store.
    fx.reset();
    fx.session_mode                                  = SP;
    fx.player_side                                   = 0;
    fx.planet_index                                  = 3; // > 2, so the yield-cut branch is reachable
    fx.current_system                                = 0;
    fx.debug_campaign_cheat                          = 1; // enable the debug branch
    fx.profiles[0].status_flags                      = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[1].status_flags                      = STATUS_ALIVE;
    fx.cfg_planets[3].system_index                   = 0;
    fx.cfg_planets[3].invention_index                = 4;
    fx.cfg_planets[7].system_index                   = 0; // another planet in the system...
    fx.planet_status[7]                              = 0; // ...still UNKNOWN -> blocks capture, early return
    fx.player_race                                   = 0;
    fx.progress[0 * PROGRESS_ROW_COUNT + 4].acquired = 0; // not yet owned -> grant fires
    {
        uint32_t r = run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_calls, 0u, "ANNOUNCE: dialog does NOT fire (early return)");
        ck(g_sprintf_calls >= 1 && g_print_text == 1, "ANNOUNCE: 'eliminated' banner");
        ck(g_update_progress.size() == 1 && g_update_progress[0].plr == 0,
           "ANNOUNCE: invention granted to the local player");
        ck(g_snd_ids.size() == 1, "ANNOUNCE: grant sound played");
        ck_eq_d(fx.debug_resource_yield_cut, 0.5, "ANNOUNCE: debug resource-yield-cut set to 0.5");
        (void)r;
    }

    // ================================================================================================
    // MP -- the LOCAL player is dropped: defeat (4), and the peers are told (net_send_presence_lost).
    // ================================================================================================
    fx.reset();
    fx.session_mode             = MP_LOCAL;
    fx.player_side              = 2;
    fx.profiles[2].status_flags = STATUS_ALIVE | STATUS_HUMAN;
    {
        run(fx, 2, 0);
        ck_eq((uint32_t)g_dialog_last, 4u, "MP-DEFEAT: dialog code = 4");
        ck_eq((uint32_t)g_net_send, 1u, "MP-DEFEAT: presence-lost sent to peers");
        ck(g_queue_text.size() == 1 && g_queue_text[0] == 0x7e, "MP-DEFEAT: queue text 0x7e");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa6, "MP-DEFEAT: loss fanfare 0xa6");
    }

    // ---- MP -- last man standing -> victory (5) ---------------------------------------------------
    fx.reset();
    fx.session_mode             = MP_LOCAL;
    fx.player_side              = 0; // me survives
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[1].status_flags = STATUS_ALIVE | STATUS_HUMAN; // the dropped player (cleared in-fn)
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_last, 5u, "MP-LASTMAN: dialog code = 5 (victory)");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa5, "MP-LASTMAN: win fanfare 0xa5");
        ck((uint32_t)(fx.profiles[1].status_flags & STATUS_ALIVE) == 0u, "MP-LASTMAN: dropped player's alive bit cleared");
    }

    // ---- MP -- another HUMAN still alive -> early return, no dialog --------------------------------
    fx.reset();
    fx.session_mode             = MP_LOCAL;
    fx.player_side              = 0;
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN; // me
    fx.profiles[1].status_flags = STATUS_ALIVE | STATUS_HUMAN; // dropped
    fx.profiles[2].status_flags = STATUS_ALIVE | STATUS_HUMAN; // another human still playing
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_calls, 0u, "MP-CONTINUE: dialog does NOT fire (another human alive)");
    }

    // ---- MP -- ally-victory rule: everyone left is allied -> victory (5) ---------------------------
    fx.reset();
    fx.session_mode              = MP_LOCAL;
    fx.player_side               = 0;
    fx.mp_ally_victory_rule_flag = 1;
    fx.profiles[0].status_flags  = STATUS_ALIVE | STATUS_HUMAN; // me
    fx.profiles[1].status_flags  = STATUS_ALIVE | STATUS_HUMAN; // dropped
    fx.profiles[2].status_flags  = STATUS_ALIVE | STATUS_HUMAN; // survivor, allied with me
    // mutual alliance between me (0) and player 2
    fx.store().player_relation_at(0, 2) = 1;
    fx.store().player_relation_at(2, 0) = 1;
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_last, 5u, "MP-ALLY-VICTORY: dialog code = 5");
    }
    // Same board WITHOUT the ally rule -> not a victory; another human alive -> early return.
    fx.reset();
    fx.session_mode                     = MP_LOCAL;
    fx.player_side                      = 0;
    fx.mp_ally_victory_rule_flag        = 0; // rule OFF
    fx.profiles[0].status_flags         = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[1].status_flags         = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[2].status_flags         = STATUS_ALIVE | STATUS_HUMAN;
    fx.store().player_relation_at(0, 2) = 1;
    fx.store().player_relation_at(2, 0) = 1;
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_calls, 0u, "MP-ALLY-RULE-OFF: no victory, early return (human alive)");
    }

    // ---- MP -- all-AI-remain continue: outcome 6 (natural), win fanfare ----------------------------
    // me=0 (human), player 1 (human) dropped, player 2 alive but AI (not human) and not allied.
    fx.reset();
    fx.session_mode             = MP_LOCAL;
    fx.player_side              = 0;
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN; // me
    fx.profiles[1].status_flags = STATUS_ALIVE | STATUS_HUMAN; // dropped human
    fx.profiles[2].status_flags = STATUS_ALIVE;                // AI, not allied
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)g_dialog_last, 6u, "MP-CONTINUE6: dialog code = 6 (natural, all-AI remain)");
        ck(g_snd_ids.size() == 1 && g_snd_ids[0] == 0xa5, "MP-CONTINUE6: win fanfare (6 -> 0xa5)");
        ck(g_queue_text.size() == 1 && g_queue_text[0] == 0x7d, "MP-CONTINUE6: victory queue text (was human)");
    }
    // Forced removal (mode != 0) -> outcome 8, and 8 gets NO fanfare. In the forced path the net drop
    // handler has ALREADY cleared the dropped player's ALIVE bit before calling, so player 1 is not
    // alive here (mode != 0 means the function itself does NOT clear it); the surviving roster is me
    // (human) + player 2 (AI) -> not victory, no other human, forced -> code 8.
    fx.reset();
    fx.session_mode             = MP_LOCAL;
    fx.player_side              = 0;
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN;
    fx.profiles[1].status_flags = STATUS_HUMAN; // already dropped (no ALIVE bit)
    fx.profiles[2].status_flags = STATUS_ALIVE; // surviving AI
    {
        run(fx, 1, 1 /* forced */);
        ck_eq((uint32_t)g_dialog_last, 8u, "MP-CONTINUE8: dialog code = 8 (forced removal)");
        ck_eq((uint32_t)g_snd_ids.size(), 0u, "MP-CONTINUE8: NO fanfare for code 8");
    }

    // ---- MP -- lockstep session downgrade to local when no human remains ---------------------------
    fx.reset();
    fx.session_mode             = MP_LOCKSTEP; // 3
    fx.player_side              = 0;
    fx.profiles[0].status_flags = STATUS_ALIVE | STATUS_HUMAN; // me (last human)
    fx.profiles[1].status_flags = STATUS_ALIVE | STATUS_HUMAN; // dropped
    {
        run(fx, 1, 0);
        ck_eq((uint32_t)fx.session_mode, (uint32_t)MP_LOCAL, "MP-LOCKSTEP: session downgraded 3->2");
        ck(g_overlay_dismiss >= 1, "MP-LOCKSTEP: overlay dismissed");
    }
}

} // namespace mh::sim::test
