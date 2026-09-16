//
// sim/resid/sim_session_state_reset.cpp -- see sim_session_state_reset.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_session_state_reset_00453ee4.asm), the Ghidra .c being
// a draft.
//
#include "sim/resid/sim_session_state_reset.h"

#include "addr/mh_calls.gen.h"           // typed callables for the frontier originals we call OUT to
#include "sim/resid/sim_new_game_init.h" // intra-slice sibling, called DIRECTLY (rule 2)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const session_state_reset_calls &live_session_state_reset_calls() {
    static const session_state_reset_calls c = {
        MH_LIBMH_BIND(time_GetCurrentTime),
        mh::state::evt::cam_jump_queue_clear,
        MH_LIBMH_BIND(llm_strat_invasion_alert_reset_all),
        MH_LIBMH_BIND(llm_strat_prod_reset_system),
    };
    return c;
}

namespace {

// Ghidra enum /Manual/game/E_PLANET_STATUS: UNKNOWN=0 -- no committed C++ enum exists yet (rule 17a
// declared_needs, already raised independently by sim_prod_deliver_arrivals.h / this batch's own
// sim_session_clear_presence_flag.cpp for the SAME region); reused here as the same file-local
// literal per that precedent rather than inventing a fresh spelling.
inline constexpr int32_t PLANET_STATUS_UNKNOWN = 0;

inline constexpr int32_t PLANET_COUNT = 0x20; // planet slots 0..0x1f, all reset (unlike the
                                              // 1..0x1f walks elsewhere in this batch -- this loop
                                              // genuinely starts at 0, see the .asm CMP/JL pair).

// The System[] record's stride in int32 UNITS (0x8c bytes / 4 = 35) and the int-index of the field
// this function's two reads land on (byte offset 0x10 from the record base -> int-index 4). See the
// header's "ADDRESS ARITHMETIC" note for the struct-shift caveat this inherits.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;
constexpr int32_t SYSTEM_FIELD_INDEX = 4;

} // namespace

namespace detail {

// ---- llm_strat_session_state_reset @0x00453ee4 -----------------------------------------------------
void session_state_reset(const sim_view &v, sim_store &own, const session_state_reset_calls &c,
                         int32_t reset_flag, const new_game_init_calls &c_ngi) {
    // 0x00453eff/f06: two per-session control-mask bytes, zeroed unconditionally.
    own.game_human_player_mask() = 0;
    own.player_control_mask()    = 0;

    // 0x00453f0d-0x00453f31: _G_LLM_NET_BW_STAT[0..1] zeroed -- the sim only zeroes netcode's
    // bandwidth accumulator at session entry, per sim_state.h's region_ownership class A note.
    for (int32_t i = 0; i < 2; ++i) own.net_bw_stat_at(i) = 0;

    // 0x00453f33-0x00453f98: the session clock quintet. CURRENT_GAME_TIME/TOTAL_GAME_TIME/
    // GAME_TIME_DELTA/_G_LLM_STRAT_GAME_CLOCK all zeroed; LAST_GAME_TIME takes the fresh wall-clock
    // reading; SIM_STEP_INTERVAL is set to the literal double 0.1 (bit pattern
    // 0x3fb999999999999a, confirmed from the two MOVs at 0x00453f52/0x00453f5c).
    own.current_game_time()   = 0.0;
    own.last_game_time()      = c.time_get_current_time();
    own.sim_step_interval()   = 0.1;
    own.game_time_delta_mut() = 0.0; // DECLARED NEED (see header banner)
    own.total_game_time()     = 0.0;
    own.game_clock_mut()      = 0.0; // DECLARED NEED (see header banner)

    // 0x00453fa2/0x00453fac: build placement id cleared; the camera pan-target column reset to
    // "no target" (-1, i.e. 0xffffffff as int32_t).
    own.build_placement_id() = 0;
    own.cam_pan_target_col() = -1;

    c.cam_jump_queue_clear();
    c.invasion_alert_reset_all();

    if (reset_flag == 0 || reset_flag == 2) {
        // ==== 0x00453fd0: FULL NEW-GAME / NEW-SYSTEM RESET (reset_flag 0 or 2) =====================
        own.game_speed()          = 1.0; // bit pattern 0 / 0x3ff00000
        own.cheat_penalty_score() = 0;

        // 0x00453ff5-0x0045400f: pin CurrentSystem to system 1 (the fixed campaign start system) and
        // re-derive G_PLANET_INDEX from the System[] table (see the header's address-arithmetic
        // note for the field this offset actually denotes).
        own.current_system_mut() = 1; // DECLARED NEED (see header banner)
        own.planet_index_mut()   =    // DECLARED NEED (see header banner)
            v.system_define_index_base[own.current_system_mut() * SYSTEM_STRIDE_INTS +
                                       SYSTEM_FIELD_INDEX];

        own.bldg_completion_accum_mut() = 0;

        // 0x0045401e-0x004540a0: reset all 32 planet slots' status/timers/int-table.
        for (int32_t i = 0; i < PLANET_COUNT; ++i) {
            own.planet_status_at(i)           = PLANET_STATUS_UNKNOWN;
            own.planet_time_at(i)             = 0.0;
            own.planet_mother_lost_time_at(i) = 0.0;
            own.planet_invasion_time_at(i)    = -1.0; // bit pattern 0 / 0xbff00000
            own.planet_int_table_at(i)        = 0;
        }

        // 0x004540a2: the intra-slice sibling, called DIRECTLY (sim_resid batch context rule 2) --
        // both functions live in namespace mh::sim::detail, so this is an unqualified call.
        new_game_init(v, own, c_ngi);

        // 0x004540a7-0x00454119: the per-session flag/byte reset run.
        own.foreign_bldg_change_flag_mut()   = 0;
        own.debug_tap_flag()                 = 0;
        own.prod_complete_throttle()         = 0;
        own.debug_resource_yield_cut()       = 0.0;
        own.ui_bldg_tab_select_blocked_mut() = 0; // DECLARED NEED (see header banner)
        own.bldg_completion_slot_count_mut() = 4; // DECLARED NEED (see header banner)
        own.lockstep_step_mult()             = 1;
        own.planet_transition_state()        = 0;
        // NOTE: the original's store here is a 4-byte MOV of 0 (`MOV dword ptr
        // [_G_LLM_STRAT_FLOATING_MSG_SUPPRESS_FLAG],0`), but the bound accessor is uint8_t& (the
        // region is registered 1 byte wide). Value 0 is bit-identical at either width; see
        // uncertainties[].
        own.floating_msg_suppress_flag()     = 0;
        own.outer_planet_landed_flag()       = 0;
        own.outer_planet_land_state()        = 0;
        own.system_lost_msg_shown_flag_mut() = 0; // DECLARED NEED (see header banner)
    } else {
        // ==== 0x0045411f: SOFT / CONTINUE PATH (reset_flag == 1, e.g. planet-to-planet transition) ==
        ++own.current_system_mut(); // DECLARED NEED (see header banner)
        own.planet_index_mut() =    // DECLARED NEED (see header banner)
            v.system_define_index_base[own.current_system_mut() * SYSTEM_STRIDE_INTS +
                                       SYSTEM_FIELD_INDEX];

        c.prod_reset_system();

        // 0x00454146-0x0045416d: all 8 player profiles' mother_established cleared.
        for (int32_t i = 0; i < MAX_PLAYERS; ++i) own.profile_at(i).mother_established = 0;

        // 0x0045416d-0x004541b7: dead tail, intentionally NOT translated -- see uncertainties[] in
        // the translator's report. Both edges of `CMP byte ptr [_G_LLM_RSR_SOURCE_MODE],0xff / JZ
        // end` converge on the same target: the JZ goes straight to `end`, and the fallthrough's
        // very next comparison (0x1bc <= 0x1f4, a compile-time-constant TRUE) also always branches
        // to `end`. So no input reaches the 100-iteration write loop guarded behind it; Ghidra's own
        // decompiler independently marks those blocks "Removing unreachable block". Nothing is
        // omitted from the OBSERVABLE behaviour by leaving it untranslated.
    }
}

} // namespace detail

// ---- the public wrapper -----------------------------------------------------------------------------

void session_state_reset(int32_t reset_flag) {
    sim_state st = state();
    detail::session_state_reset(st.read, st.own, live_session_state_reset_calls(), reset_flag);
}

} // namespace mh::sim
