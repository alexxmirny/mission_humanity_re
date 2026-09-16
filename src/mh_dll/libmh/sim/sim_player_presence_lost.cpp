//
// sim/sim_player_presence_lost.cpp -- see sim_player_presence_lost.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_player_presence_lost_00498089.asm), the Ghidra .c being a draft.
//
#include "sim/sim_player_presence_lost.h"

#include "addr/mh_calls.gen.h"      // typed callables for the effectful originals we still call OUT to
#include "lockstep/overlay_hoist.h" // LIB-ABI stage E: the hoisted dismiss-rule/GAME_MODE writes
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const player_presence_lost_calls &live_player_presence_lost_calls() {
    static const player_presence_lost_calls c = {
        MH_CRT(w_sprintf__vss),
        mh::state::evt::text_print_u32,
        mh::state::evt::snd_play,
        MH_LIBMH_BIND(game_UpdateProgress),
        mh::state::evt::text_queue_id,
        mh::host().ansi_to_wide_scratch,
        MH_CRT(utils_w_str_copy),
        mh::state::evt::text_float_red_i32,
        MH_LIBMH_BIND(llm_net_lockstep_send_presence_lost),
        mh::state::evt::overlay_dismiss_i32,
        mh::state::evt::outcome_dialog_i32,
        &mh::lockstep::live_overlay_hoist_ops(), // LIB-ABI stage E
    };
    return c;
}

namespace {

// player_profile.status_flags bits (E_STRAT_PLAYER_STATUS) -- b1 ALIVE, b2 HUMAN. libmh/sim/ keeps its
// own copy rather than depending on ai/ai_state.h, exactly like sim_combat_credit_planet_conquest_kills.h.
inline constexpr uint32_t STATUS_ALIVE = 0x2u;
inline constexpr uint32_t STATUS_HUMAN = 0x4u;

// UTF-16 "%s (%s)" @0x0050163b -- the literal this function's three w_sprintf sites push (byte-identical
// to the 0x005009e8 / 0x005017bc copies other TUs cite; read-memory confirmed this slice).
constexpr const wchar_t *TEXT_FMT_NAME_PAREN = L"%s (%s)";

// The System record's stride in int32 UNITS (0x8c bytes / 4 = 35), same as sim_view::
// system_define_index_base's comment: index `[system_idx*(0x8c/4) + n]`, n=0 invention, n=1 name.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;

// SESSION_MP_LOCAL (mode 2) is not in sim_event_codes.h (which carries only SESSION_SP / _MP_LOCKSTEP);
// named here for the lockstep 3 -> 2 downgrade.
constexpr int32_t SESSION_MP_LOCAL    = 2;
constexpr int32_t PLANET_COUNT        = 0x20; // planets 1..0x1f are walked
constexpr int32_t SHUTTLES_PER_PLAYER = 10;

// Text ids and sound ids used below (all as the original's immediates).
constexpr int32_t TEXT_ENEMY_ELIMINATED = 0x7b; // "%s eliminated" (SP)
constexpr int32_t TEXT_SYSTEM_CAPTURED  = 0x7c; // "%s captured" (SP)
constexpr int32_t TEXT_VICTORY          = 0x7d; // victory / you-win queue text
constexpr int32_t TEXT_DEFEAT           = 0x7e; // defeat / you-lose queue text
constexpr int32_t TEXT_PLAYER_ELIM_MP   = 0xa7; // "<name> was eliminated" (MP floating message)
constexpr int32_t SND_WIN               = 0xa5; // victory fanfare
constexpr int32_t SND_LOSS              = 0xa6; // defeat fanfare
constexpr int32_t SND_VOL               = 100;

} // namespace

namespace detail {

// ---- llm_strat_player_presence_lost @0x00498089 --------------------------------------------------
uint32_t player_presence_lost(const sim_view &v, sim_store &own, const player_presence_lost_calls &c,
                              uint32_t player, uint32_t mode) {
    // The original RE-READS PlayerSide (0xe58354) and G_PLANET_INDEX (0xe58366) from memory at every
    // use site, including sites after effectful call-outs. Snapshotting them here is equivalent because
    // NEITHER is written by any callee in player_presence_lost_calls: the only writers of PlayerSide
    // are llm_strat_planet_session_begin / llm_strat_session_begin_multi, and of G_PLANET_INDEX are
    // SwitchToPlanet / llm_strat_session_begin_multi / llm_strat_session_state_reset -- all
    // session-init / planet-navigation functions, none reachable from the UI-text / sound / progress
    // leaf helpers this function calls (verified via ReVA writer xrefs, 2026-08-18; the adversarial
    // reimpl-verify review raised this exact caching question). *v.current_system is equally stable
    // (its only writer, llm_strat_session_state_reset, is likewise not a callee) and read live below.
    const int32_t planet = *v.planet_index;
    const int32_t me     = *v.player_side;

    // 0x004980ad-0x00498132: EARLY PRESENCE CHECK. If the player is still ALIVE, this is a natural
    // loss check (mode==0), and they still hold a building OR a unit on the CURRENT planet, they are
    // not eliminated -- return their current-planet unit count (an incidental value callers ignore).
    if ((v.profiles[player].status_flags & STATUS_ALIVE) != 0 && mode == 0 &&
        (v.profiles[player].buildings_alive[planet] > 0 ||
         v.profiles[player].units_alive[planet] > 0)) {
        return static_cast<uint32_t>(v.profiles[player].units_alive[planet]);
    }

    uint8_t outcome = 0; // [EBP-0x14]: the E_GAME_OUTCOME code the fanfare/dialog tail consumes.

    if (*v.session_mode == SESSION_SP) {
        // ==== 0x00498349: SP CAMPAIGN PATH ==========================================================
        if (me == static_cast<int32_t>(player)) {
            // 0x00498355: the LOCAL player lost presence -> outcome starts 0 (transient), overwritten
            // to 4 (defeat) below unless a surviving shuttle causes an early return.
            outcome = 0;
        } else {
            // 0x00498365: an ENEMY may have been cleared. If any OTHER alive non-local player still
            // holds a building/unit on the current planet, the enemy is not gone -- early return.
            for (int32_t other = 0; other < MAX_PLAYERS; ++other) {
                if ((v.profiles[other].status_flags & STATUS_ALIVE) != 0 && me != other) {
                    if (v.profiles[other].buildings_alive[planet] > 0)
                        return static_cast<uint32_t>(planet * 4 + other * 0x740);
                    if (v.profiles[other].units_alive[planet] > 0)
                        return static_cast<uint32_t>(planet * 4 + other * 0x740);
                }
            }
            // 0x004983d5: the enemy is cleared from this planet -> announce + grant.
            c.w_sprintf_vss(own.text_scratch(), TEXT_FMT_NAME_PAREN, v.text_ptrs[TEXT_ENEMY_ELIMINATED],
                            v.text_ptrs[v.cfg_planets[planet].name]);
            c.ui_print_text_message(own.text_scratch());
            // 0x00498412: grant the planet's invention to the local human, unless already acquired.
            const int32_t inv = v.cfg_planets[planet].invention_index;
            if (v.progress[me * PROGRESS_ROW_COUNT + inv].acquired != 1) {
                // 0x00498441-0x0049846a: grant fanfare -- sound id 7 (race 2) shifted by 0x12, else 7.
                const int32_t snd = (*v.player_race == 2) ? 0x12 : 0;
                c.snd_play(snd + 7, SND_VOL);
                c.update_progress(static_cast<uint16_t>(me),
                                  static_cast<uint16_t>(v.cfg_planets[planet].invention_index));
                // 0x0049848c: a debug cheat halves resource yield when a >2 planet is lost.
                if (planet > 2 && *v.debug_campaign_cheat != 0) {
                    // 0x0049849e/0x004984a8: store the double 0.5 (low 0 / high 0x3fe00000).
                    own.debug_resource_yield_cut() = 0.5;
                }
            }
            outcome = 1; // 0x004984b2
        }

        // 0x004984bd: does `player` still hold ANY presence anywhere in the CURRENT SYSTEM? If a
        // building, a unit, or an UNACQUIRED planet remains, they are not eliminated -- early return.
        for (int32_t p = 1; p < PLANET_COUNT; ++p) {
            if (v.cfg_planets[p].system_index != *v.current_system) continue;
            if (v.profiles[player].buildings_alive[p] > 0)
                return static_cast<uint32_t>(p * 4 + static_cast<int32_t>(player) * 0x740);
            if (v.profiles[player].units_alive[p] > 0)
                return static_cast<uint32_t>(p * 4 + static_cast<int32_t>(player) * 0x740);
            const int32_t pinv = v.cfg_planets[p].invention_index;
            if (v.progress[me * PROGRESS_ROW_COUNT + pinv].acquired != 1)
                return static_cast<uint32_t>(pinv * 3 + static_cast<uint32_t>(me) * 900);
        }

        if (me == static_cast<int32_t>(player)) {
            // 0x0049856a: the LOCAL player. If a shuttle slot is still loaded, they are not gone.
            for (int32_t slot = 1; slot < SHUTTLES_PER_PLAYER; ++slot) {
                if (v.prod_shuttle_slots[player * SHUTTLES_PER_PLAYER + slot].type_ref_id != 0)
                    return static_cast<uint32_t>(slot * 0x31c + static_cast<int32_t>(player) * 0x1f18);
            }
            outcome = 4; // 0x004985ad: defeat
            c.print_queue_text_id(TEXT_DEFEAT);
        } else {
            // 0x004985c0: an ENEMY was fully cleared from the system. If any planet in the system is
            // still UNKNOWN (unexplored) and not the current one, hold off (early return).
            for (int32_t p = 1; p < PLANET_COUNT; ++p) {
                if (v.cfg_planets[p].system_index == *v.current_system && v.planet_status[p] == 0 &&
                    p != planet)
                    return static_cast<uint32_t>(p);
            }
            // 0x0049860f: clear the eliminated enemy's ALIVE bit.
            own.profile_at(player).status_flags &= ~STATUS_ALIVE;
            // 0x0049861d: if any OTHER player besides the local one is still alive, do not declare a
            // full system capture -- return the local side index.
            for (int32_t other = 0; other < MAX_PLAYERS; ++other) {
                if ((v.profiles[other].status_flags & STATUS_ALIVE) != 0 && me != other)
                    return static_cast<uint32_t>(me);
            }
            // 0x00498659: the whole system is captured. A low-index system with the message not yet
            // shown (and no debug cheat) prints the "captured" banner (outcome 3); otherwise victory
            // (outcome 5), with the debug-cheat XOR-decoded string reveal.
            if (*v.current_system >= 3 || *v.debug_campaign_cheat != 0 ||
                *v.system_lost_msg_shown_flag != 0) {
                outcome = 5;
                if (*v.debug_campaign_cheat != 0) {
                    // 0x00498687-0x004986e9: DEAD debug path (cheat always 0 in retail). Copy cheat
                    // string [1] into scratch, XOR each wchar by 0x7a, print it red.
                    void *w = c.ansi_to_wide_scratch(v.cheat_cmd_table[1]);
                    c.w_str_copy(w, own.text_scratch());
                    wchar_t *s = own.text_scratch();
                    int32_t  i = 0;
                    for (; s[i] != 0; ++i)
                        s[i] = static_cast<wchar_t>(static_cast<uint16_t>(s[i]) ^ 0x7a);
                    s[i] = 0;
                    c.print_floating_msg_red(own.text_scratch());
                }
                c.print_queue_text_id(TEXT_VICTORY);
            } else {
                outcome = 3;
                c.w_sprintf_vss(
                    own.text_scratch(), TEXT_FMT_NAME_PAREN, v.text_ptrs[TEXT_SYSTEM_CAPTURED],
                    v.text_ptrs[v.system_define_index_base[*v.current_system * SYSTEM_STRIDE_INTS + 1]]);
                c.ui_print_text_message(own.text_scratch());
            }
            // 0x0049873b: grant the system's invention to the local human.
            c.update_progress(
                static_cast<uint16_t>(me),
                static_cast<uint16_t>(v.system_define_index_base[*v.current_system * SYSTEM_STRIDE_INTS]));
        }
    } else {
        // ==== 0x0049814b: MP PATH (session_mode 2/3) ================================================
        // uVar6 tracks the original's incidental return value (a loop counter / an outward-call
        // result); callers ignore it, but it is reproduced for faithfulness on the early-return paths.
        uint32_t ret       = 0;
        bool     was_human = false;
        if (mode == 0) {
            // 0x00498158: natural loss -> clear ALIVE now, then note whether this was a human.
            own.profile_at(player).status_flags &= ~STATUS_ALIVE;
            if ((v.profiles[player].status_flags & STATUS_HUMAN) != 0) was_human = true;
        }

        if (me == static_cast<int32_t>(player)) {
            // 0x00498189: the LOCAL player was dropped -> defeat + tell the peers.
            outcome = 4;
            c.print_queue_text_id(TEXT_DEFEAT);
            c.net_send_presence_lost();
        } else {
            // 0x004981a1: another player left. Evaluate the surviving roster.
            bool last_man_standing = true; // bVar4 [EBP-0x38]: no OTHER alive player
            bool no_other_human    = true; // bVar3 [EBP-0x3c]: no OTHER alive HUMAN
            bool all_allied        = true; // bVar5 [EBP-0x40]: every OTHER alive player is allied

            if (was_human && *v.tutorial_step == 0) {
                // 0x004981c7: floating "<name> was eliminated" message.
                void *w = c.ansi_to_wide_scratch(const_cast<char *>(v.profiles[player].name));
                c.w_sprintf_vss(own.text_scratch(), TEXT_FMT_NAME_PAREN, v.text_ptrs[TEXT_PLAYER_ELIM_MP],
                                static_cast<const wchar_t *>(w));
                ret = static_cast<uint32_t>(c.print_floating_msg_red(own.text_scratch()));
            }

            for (int32_t other = 0; other < MAX_PLAYERS; ++other) {
                if ((v.profiles[other].status_flags & STATUS_ALIVE) != 0 && me != other) {
                    last_man_standing = false;
                    // 0x00498243: mutually-allied iff relation[me][other]==1 AND relation[other][me]==1.
                    const bool mutual_ally = own.player_relation_at(static_cast<uint32_t>(me),
                                                                    static_cast<uint32_t>(other)) == 1 &&
                                             own.player_relation_at(static_cast<uint32_t>(other),
                                                                    static_cast<uint32_t>(me)) == 1;
                    if (!mutual_ally) all_allied = false;
                    if ((v.profiles[other].status_flags & STATUS_HUMAN) != 0) no_other_human = false;
                }
                ret = static_cast<uint32_t>(other); // 0x00498374: uVar6 = iter
            }

            // 0x004982a8: a lockstep session with no human left downgrades to local and drops the overlay.
            if (*v.session_mode == SESSION_MP_LOCKSTEP && no_other_human) {
                own.session_mode() = SESSION_MP_LOCAL;
                // LIB-ABI stage E hoist (overlay_hoist.h's ordering law): latch, call, store.
                const bool not4 = mh::lockstep::hoist_mode_not4(c.hoist);
                ret             = static_cast<uint32_t>(c.net_overlay_dismiss());
                mh::lockstep::hoist_dismiss(c.hoist, not4);
            }

            // 0x004982c8: victory if last man standing, or (ally-victory rule on) everyone left is allied.
            if (last_man_standing || (*v.mp_ally_victory_rule_flag != 0 && all_allied)) {
                outcome = 5;
                c.print_queue_text_id(TEXT_VICTORY);
            } else {
                // 0x004982ef: not victory.
                if (!no_other_human) return ret;         // another human still playing
                if (!was_human && mode == 0) return ret; // a non-human natural loss: nothing to show
                outcome = (mode == 0) ? 6 : 8;           // continue: natural (6) vs forced (8)
                if (was_human) c.print_queue_text_id(TEXT_VICTORY);
            }
        }

        // 0x0049832c: a still-lockstep session downgrades here too (a no-op if already downgraded above).
        if (*v.session_mode == SESSION_MP_LOCKSTEP) {
            own.session_mode() = SESSION_MP_LOCAL;
            const bool not4    = mh::lockstep::hoist_mode_not4(c.hoist); // LIB-ABI stage E hoist
            c.net_overlay_dismiss();
            mh::lockstep::hoist_dismiss(c.hoist, not4);
        }
    }

    // 0x00498758: END FANFARE + OUTCOME DIALOG. outcome 3/5/6 -> victory fanfare (0xa5); 4 -> loss
    // fanfare (0xa6); 0/1/2/7/>=8 -> no fanfare. Then the modal outcome dialog with the code.
    if (outcome >= 4) {
        if (outcome == 4)
            c.snd_play(SND_LOSS, SND_VOL);
        else if (outcome <= 6)
            c.snd_play(SND_WIN, SND_VOL);
    } else if (outcome == 3) {
        c.snd_play(SND_WIN, SND_VOL);
    }
    // LIB-ABI stage E hoist: the call was inside the return -- split into a temporary so the
    // callee's unconditional GAME_MODE=3 (@0x004c6cec) can be re-stored after it.
    const uint32_t dialog_ret = static_cast<uint32_t>(c.outcome_dialog(outcome));
    own.game_mode()           = 3;
    // R3b: outcome_dialog's gated panel pair -- see overlay_hoist.h.
    mh::lockstep::hoist_outcome_panel(c.hoist);
    return dialog_ret;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t player_presence_lost(uint32_t player, uint32_t mode) {
    sim_state st = state();
    return detail::player_presence_lost(st.read, st.own, live_player_presence_lost_calls(), player,
                                        mode);
}

} // namespace mh::sim
