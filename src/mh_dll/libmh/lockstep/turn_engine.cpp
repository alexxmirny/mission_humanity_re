#include "lockstep/turn_engine.h"
#include "lockstep/lockstep_state.h"            // SB-BIND T4: host_binds() -- the production-arm addresses
#include "sim/libtrans/sim_lt_invasion_alert.h" // LIB-TRANS-P direct edge

#include "lockstep/internal_call.h" // C8-d: MH_INTERNAL_CALL + MH_INTERNAL_EDGES_ENTRY_ROUTED
#include "lockstep/resync.h"        // C8-c: install_seam_*
#include "lockstep/tx_emit.h"       // C8-d: send_lockstep_extend, the production entry point
#include "lockstep/tx_emit_order.h" // C8-c: install_seam_send_order
#include "lockstep/net_session.h"
// LT1D/E wave 2 (2026-09-02): the five lib_trans lockstep-homed units' shadow installers.
#include "lockstep/lt_chat_ally_mask.h"
#include "lockstep/lt_peer_timing_reset.h"
#include "lockstep/lt_player_by_side_id.h"
#include "lockstep/lt_reload_snapshot_resync.h"
#include "lockstep/lt_time_query.h" // NET-SESSION: install_seam_session_globals_reset / _wait_screen_frame

#include "orders/order_queue.h" // ST1: the order counts come from their OWNER, not a second binding

#include "addr/mh_export.gen.h"  // MH_EXPORT_REPLACE / the entry-thunk shapes
#include "addr/mh_calls.gen.h"   // typed callables for the original functions we still call OUT to
#include "addr/mh_regions.gen.h" // the state region REGISTRY -- where this module's state lives

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h"        // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED
#include "config/config.h"         // F2A: the D11 selector behind sim_tick's default

namespace mh::lockstep {

// The invasion-alert line the sim tick prints, folded into ONE injectable call because formatting it
// needs G_TEXT_PTRS and the Planets table, neither of which belongs in the turn engine's state:
//     w_sprintf(&G_TEXT_TMP, u"%s (%s)", G_TEXT_PTRS[0xad], G_TEXT_PTRS[Planets[i].name])
namespace {
// Stride and field offset are the raw operands, not a struct model: `IMUL EAX,i,0x427` at 0x0043f4b3
// and `MOV EAX,[EAX + 0xbe6da4]` at 0x0043f4ba, i.e. Planets + 4.
constexpr int32_t PLANET_STRIDE   = 0x427;
constexpr int32_t PLANET_NAME_OFF = 0x4;
constexpr int32_t ALERT_TEXT_ID   = 0xad; // the operand is the folded address 0x005846c0 =
                                          // G_TEXT_PTRS + 0xad*4 (0x0043f4c9)

// u__s___s_ @0x005009e8, the UTF-16 "%s (%s)" the original pushes at 0x0043f4cf as a folded operand.
// A C++ literal, not a registry-bound pointer -- see timekeeper.cpp's identical constant for the
// derivation (sprintf format punctuation in .rdata, the localized half arrives as an ARGUMENT).
constexpr const wchar_t *TEXT_FMT_LABEL_PAREN_NAME = L"%s (%s)";

void live_sprintf_alert_text(int32_t planet_index) {
    const host_bind_state hb       = host_binds();
    const int32_t         name_idx = *reinterpret_cast<const int32_t *>(
        hb.planets + planet_index * PLANET_STRIDE + PLANET_NAME_OFF);
    // Through the generated fixed-arity shim. `ADD ESP,0x10` at 0x0043f4e0 is four dwords, i.e.
    // __cdecl, which is what mh::call::w_sprintf__vss carries -- from the DB rather than from the
    // hand-rolled function-pointer cast this used to spell (gen_dll_calls refuses VARARGS, but it
    // does emit the per-arity variants, so the old "no generated form exists" premise was stale).
    MH_CRT(w_sprintf__vss)(hb.text_scratch, TEXT_FMT_LABEL_PAREN_NAME,
                           static_cast<const wchar_t *>(hb.text_ptrs[ALERT_TEXT_ID]),
                           static_cast<const wchar_t *>(hb.text_ptrs[name_idx]));
}
} // namespace

const game_calls &live_calls() {
    static const game_calls gc = {
        MH_INTERNAL_CALL(llm_net_send_lockstep_extend, mh::lockstep::send_lockstep_extend),
        // EXCEPTED from the direct-call rule (lint_internal_edges EXCEPT_ENTRY_ROUTED): the DETERMINISM
        // HARNESS owns llm_strat_sim_step's entry (its per-step golden-hash + the SIM-CUT exactly-once
        // probe live in sim_step_detour), so sim_step's promotion is harness-mediated (C6/SIM1F --
        // MH_Harness_RebindSimStep). This edge MUST stay entry-routed: a direct call to mh::sim::sim_step
        // would bypass on_sim_step (no hash, no SIM-CUT probe) and void every determinism run. Same class
        // as llm_strat_time_tick just below.
        MH_LIBMH_BIND(llm_strat_sim_step),
        mh::state::evt::snd_ambient_tick,
        MH_LIBMH_BIND(llm_strat_advisor_tick),
        mh::state::evt::text_print,
        MH_LIBMH_BIND(llm_strat_order_release_due),
        MH_INTERNAL_CALL(llm_strat_invasion_alert_poll, mh::sim::invasion_alert_poll),
        MH_LIBMH_BIND(llm_strat_invasion_due_check),
        live_sprintf_alert_text,
        // EXCEPTED from the direct-call rule, by the user's decision recorded in the C8 detail:
        // llm_strat_time_tick is C2 class LIVE (mixed) -- its clock recompute and FPS ring are
        // unconditional single-player code -- so it stays promoted but REVERTABLE, and a revertable
        // seam's callers must keep consulting the entry.
        MH_LIBMH_BIND(llm_strat_time_tick),
        MH_INTERNAL_CALL(llm_game_reload_snapshot_resync_clocks, mh::lockstep::reload_snapshot_resync_clocks), // REBOUND 2026-09-02: translated (LT1D d6)
        MH_LIBMH_BIND(llm_strat_clock_resync_units_and_buildings),
        MH_INTERNAL_CALL(llm_net_lockstep_dispatch, mh::lockstep::dispatch),
        // STAYS ENTRY-ROUTED: a mh::orders seam, outside the rule's scope (see rx_dispatch.cpp).
        MH_LIBMH_BIND(llm_strat_order_schedule),
    };
    return gc;
}

// The shadow arm's call set. Which entries are stubbed and which are NOT is the side-effect rule
// applied per call, and the two kept REAL are kept real deliberately:
//   order_release_due   -- writes four ORDER regions that the sites DECLARE via extra_regions, so the
//                          restore between arms undoes it. Stubbing it would GUARANTEE a false
//                          divergence, because those regions are compared.
//   invasion_alert_poll -- pure, and its return picks a branch. A stub would put the two arms on
//                          different branches.
const game_calls &inert_calls() {
    static const game_calls gc = {
        [](double) {},                                // send_lockstep_extend -- socket
        []() {},                                      // sim_step -- the entire game
        []() {},                                      // ambient_tick -- audio device
        [](double) {},                                // advisor_tick -- calls PrintTextMessage
        [](void *) {},                                // print_text_message -- UI queue
        MH_PROMOTED_ROW(llm_strat_order_release_due), // REAL: declared regions
        MH_INTERNAL_CALL(llm_strat_invasion_alert_poll,
                         mh::sim::invasion_alert_poll), // REAL: pure, and it picks a branch
        MH_PROMOTED_ROW(llm_strat_invasion_due_check),  // REAL: writes zero regions
        [](int32_t) {},                                 // sprintf_alert_text -- only reached behind the
                                                        // inert print, so it buys nothing
        []() -> int32_t { return 0; },                  // time_tick -- 18 regions + its own escapes
        [](double) {},                                  // reload_snapshot_resync_clocks -- units/buildings
        [](double) {},                                  // clock_resync_units_and_buildings
        []() {},                                        // dispatch -- reads the socket, answers peers
        []() -> int32_t { return 0; },                  // order_schedule -- broadcasts
    };
    return gc;
}

// Logging, mirroring mh::orders: the module owns a logger pointer and the seam layer wires it to
// seam_log at arm time, so mh/lockstep stays free of a dependency on mh/seams.
namespace {
void (*g_log)(const char *) = nullptr;
}

void say(const char *fmt, ...) {
    if (!g_log) return;
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(line);
}

void set_logger(void (*fn)(const char *)) { g_log = fn; }

namespace {

// ---- x87 comparison semantics ---------------------------------------------------------------------
//
// EVERY double comparison in this module is `FCOMP` + `FNSTSW AX` + `SAHF` + a `Jcc`, and the x87
// UNORDERED result (either operand NaN) sets **C3=C2=C0=1**, which SAHF maps to **ZF=CF=PF=1**. So a
// NaN compares as *both* "equal" and "below" at once, and which way the branch actually goes depends
// entirely on which `Jcc` the compiler emitted. C++ comparison operators are uniformly FALSE on NaN,
// so a literal `==` / `<` translation silently flips exactly those branches.
//
// Whether that matters depends on the Jcc, and this module has all three cases:
//
//   JBE  (CF|ZF -> skip)  -- unordered SKIPS, and `a > b` in C++ is also false on NaN.  SAME, and
//                            `commit_horizon` / `extend_if_near_horizon` use plain `>` for that reason.
//   JZ   (ZF -> treat as equal)     -- unordered is treated as EQUAL by the original.   DIFFERENT.
//   JNC  (CF==0 -> skip, so CF -> take) -- unordered TAKES the branch.                  DIFFERENT.
//
// The two helpers below reproduce the second and third cases; each is named for the Jcc it stands in
// for. Found by the adversarial review (2026-07-28), which caught two live instances after this file
// had already documented the phenomenon for a third and then not applied it — the same failure class
// as O2's `release_due` unordered-`JBE` finding. Being faithful costs one expression, and "we chose
// to differ from the original here" is a claim that would need a reason we do not have.
// Both helpers now live in turn_engine.h, because batch C's rx_dispatch.cpp needs them too. The
// reasoning stays here, where it was derived.

} // namespace

namespace detail {

// The barrier's participation test, written once because four functions in this module -- plus five
// more loops in batch C's RX dispatcher -- run the identical three-part filter: ALIVE, HUMAN, and not
// the local side. The original inlines it each time as two `TEST byte ptr [i*0x740 + PLAYERS],imm`
// plus a `MOVZX EAX,word [PlayerSide]` compare.
//
// The flag tests are on the LOW BYTE of a 4-byte field. For bits 1 and 2 that is the same answer as
// masking the whole word, which is why this reads status_flags as a uint32 -- see the field comment
// in addr/mh_structs.gen.h.
bool participates(const engine_state &st, int32_t i) {
    const uint32_t f = st.players[i].status_flags;
    if ((f & PLAYER_ALIVE) == 0) return false;
    if ((f & PLAYER_HUMAN) == 0) return false;
    return static_cast<int32_t>(*st.player_side) != i;
}

// llm_net_lockstep_commit_horizon @0x0049c189 -- THE BARRIER.
//
//     committed = min(own advertised horizon, every OTHER active peer's advertised horizon)
//
// Start from our own request and walk it down. Only ALIVE+HUMAN peers count, and the local side is
// skipped because its slot in _G_LLM_NET_PEER_HORIZON is never written -- a peer's own horizon lives
// in _G_LLM_STRAT_LOCKSTEP_HORIZON. Including self would compare against a stale/never-set slot.
//
// Solo (no other active peer) therefore leaves committed == horizon, which is what lets a
// single-peer session run at full speed through the same code path.
void commit_horizon(const engine_state &st) {
    *st.committed = *st.horizon;
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if (!participates(st, i)) continue;
        // FCOMP/SAHF/JBE: strictly-greater takes the store. An unordered compare (either side NaN)
        // sets CF+ZF and takes the JBE, i.e. skips -- the same answer `>` gives on NaN in C++.
        if (*st.committed > st.peer_horizon[i]) *st.committed = st.peer_horizon[i];
    }
}

// llm_net_lockstep_extend_if_near_horizon @0x0049c112 -- the KEEPALIVE.
//
// Called only from llm_ui_paged_list_frame (0x004cabe7): while the local player sits in a paged list
// during a live match the sim is not pumping, so nothing else would extend our advertised horizon and
// every other peer would block at the barrier. This keeps the horizon walking forward from the UI.
//
// TWO FLOATING-POINT NOTES, both load-bearing:
//
//  1. THE SUMMATION ORDER IS THE ORIGINAL'S, left to right: ((clock + margin_2) + margin_1) plus the
//     scaled lookahead (FLD clock / FADD 0xe58bd0 / FADD 0xe58bc8 / FLD arg / FMUL / FADDP). Both
//     margins are .bss doubles that no instruction in the image writes, so both are 0.0 in every
//     observed run and the order cannot actually matter today -- it is preserved because "0.0 today"
//     is an observation about the shipped image, not a guarantee, and adding 0.0 is free.
//
//  2. x87 vs SSE2 PRECISION. The original evaluates this whole expression in 80-bit x87 registers and
//     compares the un-rounded result against the 64-bit horizon; MSVC compiles us to SSE2, which
//     rounds after the multiply and again after each add. A knife-edge case can therefore branch
//     differently. Every other function in this module only COMPARES and COPIES doubles, where the
//     two are identical; this is the one expression with arithmetic ahead of a compare, so it is the
//     one place to look first if the shadow oracle reports a rare divergence here.
void extend_if_near_horizon(const engine_state &st, const game_calls &calls, double lookahead_scale) {
    const double reach = ((*st.game_clock + *st.extend_margin_2) + *st.extend_margin_1) +
                         lookahead_scale * *st.keepalive_margin_mul;
    if (reach > *st.horizon) {
        // NOTE the two different multipliers: the TEST uses KEEPALIVE_MARGIN_MUL (0x501798) and the
        // BUMP uses KEEPALIVE_STEP_MUL (0x5017a0). Swapping them compiles, runs, and quietly changes
        // how far ahead the peer advertises.
        *st.horizon += lookahead_scale * *st.keepalive_step_mul;
        calls.send_lockstep_extend(*st.horizon);
        commit_horizon(st);
    }
}

// llm_net_lockstep_find_horizon_match_side @0x0049c22c -- "which peer is the barrier waiting on?"
//
// Returns the side_id of the first active peer whose advertised horizon EQUALS the committed one,
// i.e. the peer that is holding the barrier where it is. 0 when there is none. llm_strat_time_tick
// uses it three times to name the peer in the stall/wait overlays.
//
// The equality test is `FCOMP` + `JNZ`, so an UNORDERED compare counts as a MATCH and the original
// returns that peer's side_id — see x87_equal_or_unordered above. A plain `==` would skip instead.
int32_t find_horizon_match_side(const engine_state &st) {
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if (!participates(st, i)) continue;
        if (x87_equal_or_unordered(*st.committed, st.peer_horizon[i])) return st.players[i].side_id;
    }
    return 0;
}

// llm_net_lockstep_record_peer_horizon @0x0049ff58 -- the RX side of the barrier.
//
// Stores what one peer just advertised and answers "are we all in step?". Three things the decompile
// does not make obvious, all read off the disassembly:
//
//  * THE STALE TEST REJECTS EQUAL, not just older: `horizon <= stored` returns without storing. So a
//    repeated advertisement of the same horizon is a no-op, and the table is monotonic per peer.
//  * THE REJECT PATH RETURNS 1, the same value as "everything agrees". It is not an error code; the
//    caller reads it as "no reason to stall".
//  * THE TWO LOOPS TEST DIFFERENT THINGS and the second only runs if the first agreed: first that
//    every active peer's horizon equals the one just received, then that every active peer's
//    ORDER MARKER equals the LOCAL side's. Same time but different order streams is a desync, and
//    that is the case the second loop exists to catch.
//
// Note both loops include the LOCAL side (they use the two flag tests without the `!= PlayerSide`
// exclusion that `participates` applies) -- deliberate, and the reason the marker loop can compare
// against PlayerSide's own entry at all.
int32_t record_peer_horizon(const engine_state &st, int32_t player_index, double horizon,
                            int32_t order_marker) {
    if (horizon <= st.peer[player_index].horizon) return 1;

    st.peer[player_index].horizon      = horizon;
    st.peer[player_index].order_marker = order_marker;

    bool all_at_horizon = true;
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        const uint32_t f = st.players[i].status_flags;
        if ((f & PLAYER_ALIVE) == 0 || (f & PLAYER_HUMAN) == 0) continue;
        // `FCOMP` + `JZ`: unordered counts as EQUAL, so a NaN does NOT clear the flag. This one is
        // NOT in a safe direction — getting it backwards suppresses the marker check exactly when the
        // data is already corrupt, i.e. it would hide a desync signal.
        if (!x87_equal_or_unordered(st.peer[i].horizon, horizon)) all_at_horizon = false;
    }

    int32_t in_sync = 1;
    if (all_at_horizon) {
        const int32_t local = static_cast<int32_t>(*st.player_side);
        for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
            const uint32_t f = st.players[i].status_flags;
            if ((f & PLAYER_ALIVE) == 0 || (f & PLAYER_HUMAN) == 0) continue;
            if (st.peer[i].order_marker != st.peer[local].order_marker) in_sync = 0;
        }
    }
    return in_sync;
}

// llm_net_lockstep_reset_player_horizon @0x0049e230 -- a slot is being (re)initialised.
//
// THE PARAMETER IS USED FOR EXACTLY ONE THING: clearing that player's ROW of the 8x8 peer-state
// matrix. Everything else in the loop is indexed by the loop counter and runs over ALL eight slots,
// which is easy to misread as per-player work and is the reason the row index is spelled out here.
//
// Per slot: promote a positive PENDING horizon into the live PEER_HORIZON (a horizon that arrived
// while the slot was resetting is not thrown away), then re-arm PENDING to the -1.0 sentinel. The
// promotion test is `0 < pending`, so the sentinel and zero both fail it.
//
// Then re-run the barrier, because PEER_HORIZON just changed under it. That call is OURS (a plain
// C++ call, not a trip back through the original) -- it writes only COMMITTED_HORIZON, which the
// shadow site declares via extra_regions, so it is the "declared, therefore run it for real" case.
void reset_player_horizon(const engine_state &st, int32_t player_idx) {
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        st.peer_state[player_idx * MAX_PLAYERS + i] = 0;
        // `FLDZ` + `FCOMP pending[i]` + `JNC`: the branch is taken on CF, which is set both when
        // 0.0 < pending AND when the compare is unordered -- so the original PROMOTES a NaN. A plain
        // `0.0 < pending` would drop it.
        if (x87_below_or_unordered(0.0, st.peer_horizon_pending[i]))
            st.peer_horizon[i] = st.peer_horizon_pending[i];
        st.peer_horizon_pending[i] = HORIZON_NONE;
    }
    commit_horizon(st);
}

// llm_net_lockstep_no_players @0x0049bcee -- `PLAYER_COUNT == 0`.
//
// NO STATIC CALLER (checked 2026-07-28 against the whole reference set, not just Ghidra's function
// list). Reimplemented because it is two lines and completes the family, NOT armed: a shadow site on
// an unreachable function can only ever produce a zero-call log line, which reads identically to a
// clean one.
int32_t no_players(const engine_state &st) { return *st.player_count == 0 ? 1 : 0; }

// ================================ batch B: the clock ==============================================

// llm_strat_sim_tick @0x0043f3eb -- THE MODE-3 CATCH-UP LOOP, and the reason the sim is deterministic
// across peers at all.
//
//   mode 3: while (clock + sub_step <= TOTAL_GAME_TIME) { delta = sub_step; clock += sub_step;
//                                                        release_due(clock) if any pending;
//                                                        sim_step(); }
//   else:   delta = TOTAL_GAME_TIME - clock; if (delta > 0) { clock = TOTAL_GAME_TIME; sim_step(); }
//
// THE WHOLE DIFFERENCE BETWEEN THE TWO BRANCHES is that mode 3 advances in a FIXED quantum and takes
// as many whole quanta as fit, while single-player takes one variable step straight to the target.
// That is what makes the mode-3 sequence a function of (clock, sub_step, TOTAL_GAME_TIME) alone and
// therefore reproducible on every peer. TOTAL_GAME_TIME has already been clamped to the committed
// horizon by llm_strat_time_tick before this runs, which is where the barrier actually bites: the
// leftover fraction is not simulated, it is carried (docs/mp-lockstep -- the "clamp-discard").
//
// WHY THIS IS SHADOWABLE DESPITE CALLING sim_step, which is the entire game. The loop's condition
// reads GAME_CLOCK, SIM_STEP_INTERVAL, TOTAL_GAME_TIME and ORDER_PENDING_COUNT, and the state matrix's
// COMPLETE writer lists for those four are:
//     GAME_CLOCK          SwitchToPlanet, reload_snapshot_resync_clocks, advance_sim_clock,
//                         session_state_reset, sim_clock_advance, sim_tick
//     SIM_STEP_INTERVAL   session_state_reset
//     TOTAL_GAME_TIME     session_state_reset, time_tick
//     ORDER_PENDING_COUNT FillDefaults, order_pending_enqueue, order_release_due, order_schedule
// sim_step appears in none of them, and of the order writers only release_due runs inside this loop
// (pending_enqueue is reached from the RX dispatcher, schedule from the pump). So an INERT sim_step
// cannot change how many iterations our arm takes -- and if that reasoning were wrong, the oracle
// would say so, because GAME_CLOCK is compared.
void sim_tick(const engine_state &st, const game_calls &calls, const reimpl_fixes &fx) {
    // `fx.rig_fixed_step_loop` forces the mode-3 branch in single-player -- a RIG shape, never shipped
    // on. See its declaration for why, and for the order-release check that says it is safe here.
    if (*st.session_mode == SESSION_MP_LOCKSTEP || fx.rig_fixed_step_loop) {
        // `FCOMP`/`JA`: unordered EXITS the loop, and `<=` in C++ is likewise false on NaN. Same.
        while (*st.game_clock_w + *st.sim_step_interval <= *st.total_game_time) {
            *st.game_time_delta = *st.sim_step_interval;
            *st.game_clock_w    = *st.sim_step_interval + *st.game_clock_w;
            if (*st.order_pending_count != 0) calls.order_release_due(*st.game_clock_w);
            calls.sim_step();
        }
    } else {
        *st.game_time_delta = *st.total_game_time - *st.game_clock_w;
        // `FLDZ`/`FCOMP delta`/`JNC`: taken when 0 >= delta, so the body runs on 0 < delta OR
        // UNORDERED. Same shape as reset_player_horizon's promotion test.
        if (x87_below_or_unordered(0.0, *st.game_time_delta)) {
            *st.game_clock_w = *st.total_game_time;
            calls.sim_step();
        }
    }

    // The tail is frame housekeeping rather than clock work, but it is inside this function and so it
    // is part of being equivalent to it. invasion_alert_poll is PURE (its only call is
    // assert_stack_capacity and it has no absolute-address store), so it runs for real even in the
    // shadow arm -- it has to, because its return picks the branch below.
    calls.ambient_tick();
    const int32_t planet = calls.invasion_alert_poll(*st.game_clock_w);
    if (-1 < planet) {
        calls.sprintf_alert_text(planet);
        *st.floating_msg_active = 1;
        calls.print_text_message(st.text_tmp);
    }
    calls.advisor_tick(*st.game_clock_w);
}

// llm_strat_advance_sim_clock @0x0044d0ed -- one step of the PLANET-TRANSITION catch-up.
//
// Called only from llm_strat_planet_transition_tick, once per frame while its counter runs. Advances
// the clock by the frame's delta and, if that overshoots TOTAL_GAME_TIME, GIVES THE OVERSHOOT BACK by
// subtracting it from the delta as well as pinning the clock -- so the caller's next step starts from
// a delta that already accounts for the clamp. Same clamp-discard shape as the mode-3 loop, one level
// out.
void advance_sim_clock(const engine_state &st, const game_calls &calls) {
    *st.game_clock_w = *st.game_time_delta + *st.game_clock_w;
    // `FCOMP`/`JBE`: unordered SKIPS, matching C++ `<`.
    if (*st.total_game_time < *st.game_clock_w) {
        *st.game_time_delta = *st.game_time_delta - (*st.game_clock_w - *st.total_game_time);
        *st.game_clock_w    = *st.total_game_time;
    }
    calls.invasion_due_check(); // writes zero regions -- real
    calls.sim_step();           // the whole sim -- inert in the shadow arm
}

// llm_net_lockstep_pump @0x0049c088, MINUS its first statement -- see the wrapper below for why the
// call to llm_strat_order_schedule is a PARAMETER here.
//
//     if (schedule flushed nothing) and (clock + step*KEEPALIVE_MUL > horizon):
//         horizon = clock + step;  send_lockstep_extend(horizon);  commit_horizon()
//     dispatch()
//
// NOTE the guard is on schedule's RESULT, not on whether it ran: a frame that actually flushed orders
// has already advertised a horizon with them, so the keepalive stands down for that frame. Getting
// this backwards would double-advertise on every order frame.
void pump_after_schedule(const engine_state &st, const game_calls &calls, int32_t schedule_result) {
    if (schedule_result == 0 &&
        *st.horizon < *st.lockstep_step_size * *st.pump_refill_frac + *st.game_clock_w) {
        *st.horizon = *st.game_clock_w + *st.lockstep_step_size;
        calls.send_lockstep_extend(*st.horizon);
        commit_horizon(st);
    }
    calls.dispatch();
}

// llm_strat_sim_clock_advance @0x0044d238 -- the SwitchToPlanet fast-forward, minus its time_tick call.
//
// Returns how many sub-steps the caller should run: 0 if there is no time to make up, 100 if the
// remaining delta is at least a whole sub-step (a cap, not a count), else the whole number of
// sub-steps that fit. `d` is the time still owed; anything past CATCHUP_LIMIT_SECS is handed to the
// clock and to the unit/building resync directly rather than simulated.
//
// TWO THINGS THE DISASSEMBLY SAYS AND THE DECOMPILE DOES NOT:
//  * The `G_PLANET_STATUS[G_PLANET_INDEX] != 0` test at 0x0044d25f is VACUOUS -- both arms load the
//    same 300.0 (0x0044d268 and 0x0044d278 are byte-identical stores). Reproduced as an unconditional
//    constant; the read has no effect.
//  * The prototype was `void` until this batch. It is not: the result local is written on every path
//    and SwitchToPlanet propagates EAX. See tools/applied/2026-07-28-041027-prototypes.json.
int32_t sim_clock_advance_after_time_tick(const engine_state &st, const game_calls &calls) {
    int32_t steps = 0;
    calls.reload_snapshot_resync_clocks(*st.game_clock_w);

    double owed = *st.total_game_time - *st.game_clock_w;
    if (x87_below_or_unordered(0.0, owed)) { // `FLDZ`/`FCOMP`/`JNC` again
        if (owed > CATCHUP_LIMIT_SECS) {     // `JBE` -> unordered skips, matches `>`
            const double skipped = owed - CATCHUP_LIMIT_SECS;
            owed                 = owed - skipped; // i.e. exactly CATCHUP_LIMIT_SECS, written the
                                                   // long way because that is what the original does
            *st.game_clock_w = *st.game_clock_w + skipped;
            calls.clock_resync_units_and_buildings(*st.game_clock_w);
        }
        *st.game_time_delta = owed / DELTA_DIVISOR;
        // `FCOMP sub_step`/`JC`: CF is set when delta < sub_step OR unordered, and the branch is
        // TAKEN then -- so the capped answer is the ORDERED delta >= sub_step case.
        if (x87_below_or_unordered(*st.game_time_delta, *st.sim_step_interval)) {
            *st.game_time_delta = *st.sim_step_interval;
            // THE CAST IS A TRUNCATION AND THAT IS FAITHFUL, despite what the call at 0x0044d334
            // looks like. The original is `FDIV / CALL 0x004d0596 / FISTP`, and 0x004d0596 was named
            // `round` -- which made the batch-B review report this line as a divergence. Its body:
            //     FSTCW [ESP] / PUSH [ESP] / MOV byte ptr [ESP+1],0x1f / FLDCW [ESP] / FRNDINT
            // Byte 1 of the control word is bits 8..15, so 0x1f puts RC (bits 11:10) at 11 = ROUND
            // TOWARD ZERO. And 373 of its 374 call sites are immediately followed by FISTP, i.e. it
            // is the compiler's (int)double helper, which C requires to truncate. Renamed to `trunc`
            // in Ghidra so the next reader is not sent the same way.
            steps = static_cast<int32_t>(owed / *st.sim_step_interval);
        } else {
            steps = CLOCK_ADVANCE_CAPPED;
        }
    }
    return steps;
}

} // namespace detail

// ---- production entry points ---------------------------------------------------------------------
//
// C8-d MOVED THE LIVENESS COUNTER DOWN TO HERE, and the reason is a defect the first promoted run
// after the direct-call conversion exposed. The counter used to live in the `promoted::` entry-thunk
// wrappers below, so it counted CALLS THAT ARRIVED AT THE ORIGINAL ENTRY ADDRESS. Once our bodies
// call each other directly, a seam reached only from inside the closure never touches its entry --
// and `dispatch`, which runs every frame, logged ZERO calls in a run where it plainly ran.
//
// That is not a cosmetic loss. "READ THE LIVENESS LINES BEFORE BELIEVING A PASS" is the standing
// defence against the O3 failure -- an asymmetric run that came back ALL PAIRS IDENTICAL with the
// promotion never installed -- and after the conversion 21 of the 33 seams would have read zero
// forever, which is exactly what a never-installed seam reads. A defence that cannot distinguish
// "ran" from "was never armed" is not a defence.
//
// Counting HERE instead counts the work: the entry thunk forwards to these, and so does every direct
// internal edge. The `promoted::` wrappers below are now pure forwards.
void commit_horizon() {
    promoted::live(0, "commit_horizon");
    detail::commit_horizon(state());
}
int32_t find_horizon_match_side() {
    promoted::live(1, "find_horizon_match_side");
    return detail::find_horizon_match_side(state());
}
void reset_player_horizon(int32_t player_idx) {
    promoted::live(3, "reset_player_horizon");
    detail::reset_player_horizon(state(), player_idx);
}
int32_t no_players() { return detail::no_players(state()); }

int32_t record_peer_horizon(int32_t player_index, double horizon, int32_t order_marker) {
    promoted::live(2, "record_peer_horizon");
    return detail::record_peer_horizon(state(), player_index, horizon, order_marker);
}

void extend_if_near_horizon(double lookahead_scale) {
    promoted::live(4, "extend_if_near_horizon");
    detail::extend_if_near_horizon(state(), live_calls(), lookahead_scale);
}

void sim_tick() {
    promoted::live(10, "sim_tick");
    detail::sim_tick(state(), live_calls(), fixes());
}
void advance_sim_clock() {
    promoted::live(7, "advance_sim_clock");
    detail::advance_sim_clock(state(), live_calls());
}

// THE PUMP'S FIRST STATEMENT LIVES HERE, NOT IN detail::pump_after_schedule, and the split is the
// whole reason this function can be reasoned about at all.
//
//     if (STAGING_COUNT != 0) result = order_schedule();
//
// order_schedule BROADCASTS: its write-set escapes to the socket, so a shadow arm must bind an inert
// one -- and an inert stub returns 0 where the original returns the flushed byte count. The pump then
// BRANCHES on exactly that value. So the two arms would take different branches on every frame that
// carried an order, and the site would report a divergence that is an artifact of the stub. That is
// why llm_net_lockstep_pump is NOT shadowable at all (the turn-engine notes), and why the part that IS
// testable takes the result as a parameter instead.
void pump() {
    promoted::live(5, "pump");
    const engine_state &st = state();
    const game_calls   &gc = live_calls();
    int32_t             r  = 0;
    if (*st.order_staging_count != 0) r = gc.order_schedule();
    detail::pump_after_schedule(st, gc, r);
}

// Same shape, same reason: sim_clock_advance READS TOTAL_GAME_TIME, which the time_tick it just
// called has written. An inert time_tick leaves our arm reading the restored pre-state while the
// original read the post-state, so every call would diverge; running time_tick for real would mean
// declaring its eighteen regions AND its own outward calls. Not shadowable -- unit-tested instead.
int32_t sim_clock_advance() {
    const engine_state &st = state();
    const game_calls   &gc = live_calls();
    gc.time_tick();
    return detail::sim_clock_advance_after_time_tick(st, gc);
}

} // namespace mh::lockstep

// ---- differential-oracle bindings ------------------------------------------------------------------
// Two rules govern this batch, and both are about
// which functions may be armed TOGETHER rather than about any individual body:
//
//  * NESTED SITES DO NOT COMPOSE. Sites hook the callee ENTRY, so arming commit_horizon in the same
//    run as extend_if_near_horizon or reset_player_horizon makes the ORIGINAL arm of those two
//    re-enter commit_horizon's dispatcher and run its two arms inside ours. Same for
//    llm_net_lockstep_pump and llm_strat_order_schedule, which also call commit_horizon. Two ini
//    fragments: {commit_horizon, record_peer_horizon, find_horizon_match_side} in one,
//    {extend_if_near_horizon, reset_player_horizon} in the other.
//  * THE ONE ESCAPING CALL is llm_net_send_lockstep_extend, which puts the advertised horizon on the
//    wire. Inert in the shadow arm -- a second send would advertise every extension twice to every
//    peer and could move the barrier the oracle is watching.
//
// Our internal call to commit_horizon from the other two is a plain C++ call into this module, so it
// never re-enters a shadow site; it writes COMMITTED_HORIZON, declared on both sites via
// extra_regions, and therefore runs for real.


// ---- L1-P: promotion -----------------------------------------------------------------------------
//
// PROVE OUR CODE ACTUALLY RAN. O3's first asymmetric run came back ALL PAIRS IDENTICAL with the
// promotion never installed -- an asymmetric test whose asymmetry evaporates does not fail, it
// PASSES. So every seam logs its FIRST call and then widens (1/100/1000/10000/100000): a
// milestone-only counter is silent exactly when the real volume is lower than the guess, which is the
// safe-looking direction. commit_horizon alone ran 499 000 times in 3000 steps on the host, so the
// spread matters.
namespace mh::lockstep::promoted {

// 9 install_promotion seams + the two that arrive by REBIND rather than install: time_tick (C4, via the
// pacing detour) and sim_tick (C6, via the determinism harness's detour).
long g_calls[11];
bool g_any_installed = false;

// NOT `inline`: since C8-d the PRODUCTION ENTRY POINTS call this too (see the note at the forward
// declaration above them), and those are in this same TU but ahead of this definition.
void live(int slot, const char *name) {
    const long n = ++g_calls[slot];
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mh::lockstep::say("; [promote] %s: call #%ld (OURS is live)\n", name, n);
}

// clang-format off
void    commit_horizon()                    { mh::lockstep::commit_horizon(); }
int32_t find_horizon_match_side()           { return mh::lockstep::find_horizon_match_side(); }
int32_t record_peer_horizon(int32_t p, double h, int32_t m) { return mh::lockstep::record_peer_horizon(p, h, m); }
void    reset_player_horizon(int32_t i)     { mh::lockstep::reset_player_horizon(i); }
void    extend_if_near_horizon(double s)    { mh::lockstep::extend_if_near_horizon(s); }
void    pump()                              { mh::lockstep::pump(); }
void    dispatch()                          { mh::lockstep::dispatch(); }
void    advance_sim_clock()                 { mh::lockstep::advance_sim_clock(); }
int32_t sim_clock_advance()                 { return mh::lockstep::sim_clock_advance(); }
int32_t time_tick()                         { return mh::lockstep::time_tick(); }
void    sim_tick()                          { mh::lockstep::sim_tick(); }
// clang-format on

} // namespace mh::lockstep::promoted

MH_EXPORT_REPLACE(llm_net_lockstep_commit_horizon, mh::lockstep::promoted::commit_horizon)
MH_EXPORT_REPLACE(llm_net_lockstep_find_horizon_match_side, mh::lockstep::promoted::find_horizon_match_side)
MH_EXPORT_REPLACE(llm_net_lockstep_record_peer_horizon, mh::lockstep::promoted::record_peer_horizon)
MH_EXPORT_REPLACE(llm_net_lockstep_reset_player_horizon, mh::lockstep::promoted::reset_player_horizon)
MH_EXPORT_REPLACE(llm_net_lockstep_extend_if_near_horizon, mh::lockstep::promoted::extend_if_near_horizon)
MH_EXPORT_REPLACE(llm_net_lockstep_pump, mh::lockstep::promoted::pump)
MH_EXPORT_REPLACE(llm_net_lockstep_dispatch, mh::lockstep::promoted::dispatch)
MH_EXPORT_REPLACE(llm_strat_advance_sim_clock, mh::lockstep::promoted::advance_sim_clock)
MH_EXPORT_REPLACE(llm_strat_sim_clock_advance, mh::lockstep::promoted::sim_clock_advance)
// time_tick is declared here like the other nine, but it is NEVER INSTALLED through its installer --
// the pacing detour owns that entry (C4). What the rebind needs is the THUNK's address, which the macro
// gives us for free; taking it does not patch anything. Declaring it here anyway is deliberate: it is
// what puts llm_strat_time_tick into gen_dll_patches.py's promotable set, so the C1 interlock starts
// covering the four byte patches aimed inside its body from the moment the rebind can fire.
MH_EXPORT_REPLACE(llm_strat_time_tick, mh::lockstep::promoted::time_tick)
// sim_tick, same story one instrument over (C6): the DETERMINISM HARNESS owns this entry, so this
// declaration exists for its thunk address, not to install anything. Declaring it also puts
// llm_strat_sim_tick into the promotable set, which is what makes the C1 interlock able to see a byte
// patch aimed inside it. There are none today -- every registered patch in that neighbourhood lands
// below 0x0043f3eb, i.e. inside time_tick -- so this adds coverage rather than changing behaviour.
MH_EXPORT_REPLACE(llm_strat_sim_tick, mh::lockstep::promoted::sim_tick)

namespace mh::lockstep {

// The entry thunk for a promoted llm_strat_time_tick, for the pacing detour to fall through to
// INSTEAD of the stolen-prologue trampoline (C4). Taking its address installs nothing.
void *time_tick_entry_thunk() {
#ifdef MH_LIBMH_BUILD
    return nullptr; // LIB-REF-SPLIT: no pacing detour standalone, so no thunk is generated
#else
    return (void *)mh_export_thunk_llm_strat_time_tick;
#endif
}

// Same, for a promoted llm_strat_sim_tick, for the determinism harness's detour to fall through to
// (C6). Taking its address installs nothing.
void *sim_tick_entry_thunk() {
#ifdef MH_LIBMH_BUILD
    return nullptr; // LIB-REF-SPLIT: no determinism-harness detour standalone
#else
    return (void *)mh_export_thunk_llm_strat_sim_tick;
#endif
}

// Set by install_promotion, consumed by net_lockstep.cpp once the detour that owns the entry is armed.
bool g_want_time_tick = false;
bool g_want_sim_tick  = false;

bool promotion_active() { return promoted::g_any_installed; }

// C3: the migrated fixes' live knob values. Defaults are all-off, which is the FAITHFUL STOCK
// behaviour -- a build that never calls set_fixes therefore behaves exactly as the original did, which
// is what keeps the asymmetric oracle meaningful.
reimpl_fixes        g_fixes;
const reimpl_fixes &fixes() { return g_fixes; }
void                set_fixes(const reimpl_fixes &f) { g_fixes = f; }

// The seam vocabulary `lockstep_seams=` may name. The first nine are install_promotion's table in the
// same order; the last two arrive by REBIND rather than install -- time_tick tenth, through the pacing
// detour (C4), and sim_tick eleventh, through the determinism harness's detour (C6). ONE list, so a
// name cannot be valid in the parser and unknown at the install site, or vice versa.
const char *const SEAM_NAMES[] = {"commit_horizon",
                                  "find_horizon_match_side",
                                  "record_peer_horizon",
                                  "reset_player_horizon",
                                  "extend_if_near_horizon",
                                  "pump",
                                  "dispatch",
                                  "advance_sim_clock",
                                  "sim_clock_advance",
                                  "time_tick",
                                  "sim_tick",
                                  // ---- C8-c: the residue closure. Bodies live in resync.cpp and
                                  // tx_emit_order.cpp (MH_EXPORT_REPLACE defines its installer in
                                  // its own TU), but the vocabulary and the accounting stay here so
                                  // `lockstep_seams=` cannot know a name the install site does not.
                                  "sync_delay_stub",
                                  "sync_busywait",
                                  "count_active_players",
                                  "is_local_leader_peer",
                                  "force_resync",
                                  "send_order",
                                  // ---- C8-d: the sixteen wire emitters, folded in from the retired
                                  // `[promote] wire` key. Their MH_EXPORT_REPLACE installers are
                                  // static in tx_emit.cpp, so what the table below drives is one
                                  // non-static wrapper each (tx_emit.h) -- the C8-c arrangement.
                                  "send_lockstep_extend",
                                  "send_lockstep_keepalive",
                                  "broadcast_player_leave",
                                  "player_remove",
                                  "player_remove_timeout",
                                  "chat_send_team",
                                  "chat_send_all",
                                  "send_buf_flush",
                                  "send_presence_lost",
                                  "send_slot_reset",
                                  "send_horizon_ack",
                                  "send_horizon_desync",
                                  "send_lockstep_kick",
                                  "send_lockstep_step_size",
                                  "broadcast_resync_state",
                                  "send_lockstep_resync_resume",
                                  // ---- NET-SESSION: the two ORIGINAL netcode writers SB-NET and
                                  // SB-BOOT could not except. Bodies in net_session.cpp, same
                                  // static-installer arrangement as C8-c's. They join THIS key
                                  // rather than getting their own because they are the same
                                  // closure's state: session_globals_reset sets the pacing baseline
                                  // the engine below reads every tick, and wait_screen_frame ends
                                  // the resync whose other half is already in this set.
                                  "session_globals_reset",
                                  "wait_screen_frame",
                                  nullptr};

int seam_count() {
    int n = 0;
    while (SEAM_NAMES[n]) ++n;
    return n;
}

namespace detail {

subset_result parse_seam_subset(const char *want, bool *mask, int n, char *bad, int bad_cap) {
    for (int i = 0; i < n; ++i) mask[i] = false;
    if (bad && bad_cap) bad[0] = 0;
    if (!want || !*want) {
        for (int i = 0; i < n; ++i) mask[i] = true;
        return subset_result::full; // absent/empty = the shipping default, unchanged since L1-P P0
    }

    int selected = 0;
    for (const char *p = want; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') ++p;
        size_t len = static_cast<size_t>(p - start);
        while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) --len;
        if (!len) continue;

        int hit = -1;
        for (int i = 0; i < n && SEAM_NAMES[i]; ++i)
            // Whole-token equality, never a prefix: `pump` must not also select a hypothetical
            // `pump_2`, and `commit` must not select `commit_horizon`. A near-miss that silently
            // promotes something the author did not name is exactly what this function prevents.
            if (strlen(SEAM_NAMES[i]) == len && _strnicmp(start, SEAM_NAMES[i], len) == 0) {
                hit = i;
                break;
            }
        if (hit < 0) {
            if (bad && bad_cap) {
                size_t k = len < static_cast<size_t>(bad_cap) - 1 ? len : static_cast<size_t>(bad_cap) - 1;
                memcpy(bad, start, k);
                bad[k] = 0;
            }
            // CLEAR what we had already accepted. A refusal must leave NOTHING selected: the tokens
            // before the typo are not a smaller-but-valid request, they are half of a request that was
            // rejected, and a caller that forgot to check the result would otherwise install them.
            // Refuse, never narrow -- that is the whole point of validating.
            for (int i = 0; i < n; ++i) mask[i] = false;
            return subset_result::unknown_token;
        }
        if (!mask[hit]) ++selected;
        mask[hit] = true;
    }
    // Syntactically present but naming nothing (",,", " , "). Distinct from ABSENT: the author meant to
    // narrow the set and ended up promoting NOTHING, which is the shape that passes while proving
    // nothing -- the L1-P P0 lesson.
    return selected ? subset_result::ok : subset_result::empty;
}

} // namespace detail

// C8-f: `default_on` is the SHIPPING DEFAULT for `[promote] lockstep`, PASSED IN rather than read
// from a header here. The value lives in mh/seams/net_internal.h (SHIP_PROMOTE_*) with the rest of
// the ship defaults, and this module must not include a seams header -- the layering lint exists to
// keep binary-bound seam code out of the reimplementation. So the seams layer supplies it at the one
// call site, which also keeps "what ships" answerable by reading one block instead of two.
//
// F2A: that call site now passes `mh::config::ours_default(SHIP_PROMOTE_LOCKSTEP)`, so the whole
// closure follows the D11 selector -- `[config] mode=original` returns 0 here and this function
// exits at the gate below. `sim_tick`'s own default is derived DIRECTLY (twice, further down) rather
// than left to that early return: it is read in a branch the gate already guarantees, so leaving it
// a literal `1` would make the right answer an accident of control flow instead of a derivation.
int install_promotion(const char *ini_path, int default_on) {
    if (!ini_path) return 0;

    // ---- C8-d's RETIRED-KEY REFUSAL MOVED (fork F2E) ---------------------------------------------
    //
    // This function used to refuse the run when it found `[promote] wire` or `wire_seams` -- keys the
    // C8-d merge retired, whose survival in a fragment meant the author believed sixteen emitters
    // would be promoted by something that no longer reads them. F2E generalised that judgement rather
    // than deleting it: the WHOLE `[promote]` section is retired now, so the refusal belongs where the
    // section is read at all -- mh::config::detail::refuse_retired_sections, which fires before any
    // installer runs and names the file and the section. A per-key check here would be a second,
    // weaker copy of a refusal that has already terminated the process.
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        bool (*install)();
    };
    const entry seams[] = {
        {"commit_horizon", mh_export_install_llm_net_lockstep_commit_horizon},
        {"find_horizon_match_side", mh_export_install_llm_net_lockstep_find_horizon_match_side},
        {"record_peer_horizon", mh_export_install_llm_net_lockstep_record_peer_horizon},
        {"reset_player_horizon", mh_export_install_llm_net_lockstep_reset_player_horizon},
        {"extend_if_near_horizon", mh_export_install_llm_net_lockstep_extend_if_near_horizon},
        {"pump", mh_export_install_llm_net_lockstep_pump},
        {"dispatch", mh_export_install_llm_net_lockstep_dispatch},
        {"advance_sim_clock", mh_export_install_llm_strat_advance_sim_clock},
        {"sim_clock_advance", mh_export_install_llm_strat_sim_clock_advance},
        // C8-c: the six residue seams. Their MH_EXPORT_REPLACE macros -- and therefore their
        // `static` installers -- are in resync.cpp and tx_emit_order.cpp, so what appears here is a
        // non-static one-line wrapper from each. The table stays single: this is the function that
        // owns the ok/skipped accounting and the SHIP-vs-DIAGNOSTIC banner, and a second table
        // somewhere else would mean a run could be "whole closure" by one count and partial by
        // another.
        {"sync_delay_stub", install_seam_sync_delay_stub},
        {"sync_busywait", install_seam_sync_busywait},
        {"count_active_players", install_seam_count_active_players},
        {"is_local_leader_peer", install_seam_is_local_leader_peer},
        {"force_resync", install_seam_force_resync},
        {"send_order", install_seam_send_order},
        // C8-d: the wire emitters, formerly install_wire_promotion's own table behind `[promote]
        // wire`. Order matches SEAM_NAMES; the wrappers live in tx_emit.cpp for the same
        // static-installer reason the six above do.
        {"send_lockstep_extend", install_seam_send_lockstep_extend},
        {"send_lockstep_keepalive", install_seam_send_lockstep_keepalive},
        {"broadcast_player_leave", install_seam_broadcast_player_leave},
        {"player_remove", install_seam_player_remove},
        {"player_remove_timeout", install_seam_player_remove_timeout},
        {"chat_send_team", install_seam_chat_send_team},
        {"chat_send_all", install_seam_chat_send_all},
        {"send_buf_flush", install_seam_send_buf_flush},
        {"send_presence_lost", install_seam_send_presence_lost},
        {"send_slot_reset", install_seam_send_slot_reset},
        {"send_horizon_ack", install_seam_send_horizon_ack},
        {"send_horizon_desync", install_seam_send_horizon_desync},
        {"send_lockstep_kick", install_seam_send_lockstep_kick},
        {"send_lockstep_step_size", install_seam_send_lockstep_step_size},
        {"broadcast_resync_state", install_seam_broadcast_resync_state},
        {"send_lockstep_resync_resume", install_seam_send_lockstep_resync_resume},
        // NET-SESSION. Order matches SEAM_NAMES; wrappers in net_session.cpp.
        {"session_globals_reset", install_seam_session_globals_reset},
        {"wait_screen_frame", install_seam_wait_screen_frame},
    };
    constexpr int N = static_cast<int>(sizeof(seams) / sizeof(seams[0]));

    // PER-SEAM GATING (L1-P step P0). `[promote] lockstep=1` alone is all-or-nothing, which makes a
    // red asymmetric run unbisectable -- nine seams and no way to ask which one moved the state. An
    // optional `lockstep_seams=pump,dispatch` narrows the install set; ABSENT or empty keeps the
    // all-nine default, so the shipping meaning of `lockstep=1` is unchanged.
    //
    // ---- C8-d: AND IT NOW ONLY WORKS IN THE ENTRY-ROUTED BUILD --------------------------------------
    //
    // A subset means "install these seams; the rest run the original". That sentence is only true
    // while our promoted bodies reach each other THROUGH the original entry addresses, because that
    // is what makes an un-installed seam fall back. C8-d makes those edges direct C++ calls, so in
    // the default build a narrowed set would install fewer entries while our code went on calling
    // our code regardless -- a run that names one configuration and executes another. That is worse
    // than having no bisect: it is a bisect that lies.
    //
    // So the facility is BUILD-GATED rather than deleted. Under MH_INTERNAL_EDGES_ENTRY_ROUTED=1 the
    // pre-C8-d topology is restored exactly and a subset means what it always meant, so that build --
    // the one a shadow campaign over the closure interior already needs -- is also the one that can
    // bisect. In the ship build the key is REFUSED, loudly, and names the way to get it back.
    //
    // (The ledger's C8-d text says "a partial install is no longer expressible". It is no longer
    // expressible in the SHIPPING build, which is what that line is protecting; keeping it alive in
    // the diagnostic build costs nothing, keeps ~50 lines of tested parser and its interlocktest
    // cases honest, and gives the entry-routed build a second reason to exist. Recorded here so the
    // deviation is not read as an oversight.)
    //
    // ---- AND IT LIVES IN `[bisect]` NOW, NOT `[promote]` (fork F2E) -------------------------------
    //
    // The seven per-row promotion ladders died with the `[promote]` section, and this key looks like
    // one of them. It is not, and the difference is the one the drop was drawn along: those ladders
    // changed which bodies a SHIPPING run executed, one key at a time, behind the selector's back.
    // This key cannot change a shipping run at all -- the refusal immediately below is unconditional
    // outside MH_INTERNAL_EDGES_ENTRY_ROUTED, a build nothing ships. So it survives as what it always
    // was, a diagnostic, and moves into a section named for that. Same move, same reason, as
    // `[save] verify`.
    char want[256] = {0};
    GetPrivateProfileStringA("bisect", "lockstep_seams", "", want, sizeof(want), ini_path);
#if !MH_INTERNAL_EDGES_ENTRY_ROUTED
    if (want[0]) {
        say("; [promote] lockstep: REFUSED -- `lockstep_seams` needs the ENTRY-ROUTED build (C8-d). "
            "In this build our promoted bodies call each other DIRECTLY and never consult a seam "
            "knob, so a subset would describe a configuration that is not the one that runs. "
            "Rebuild with MH_INTERNAL_EDGES_ENTRY_ROUTED=1 to bisect, or omit the key.\n");
        say("; [promote] lockstep: RUN-CONFIG: REFUSED (lockstep_seams in a direct-call build) -- "
            "this run is NOT promoted\n");
        return 0;
    }
#endif

    // C5: parse ONCE, VALIDATE, and refuse rather than narrow. An unknown token used to match nothing
    // and silently shrink the promotion set; an all-separator list used to select nothing and still
    // print a SUBSET banner. Both produce a run that reads as promoted and is not.
    // CAPACITY, not a magic number. parse_seam_subset writes mask[0..seam_count()), so this array
    // must not be smaller than SEAM_NAMES -- and it silently WAS about to be when C8-c took the list
    // from 11 names to 17 against a `bool mask[16]`. A stack smash that only fires when someone adds
    // a seam is the worst possible failure shape, so the size is now stated once and checked at
    // runtime rather than assumed.
    // 64, raised from 32 by C8-d when folding in the sixteen wire seams took SEAM_NAMES from 17 to
    // 33. THE RUNTIME REFUSAL BELOW IS WHAT CAUGHT IT -- interlocktest went red on `the seam list
    // still fits the selection mask` the first time the merged list was built, which is precisely the
    // failure C8-c added this cap to convert from a stack smash into a message.
    constexpr int MASK_CAP       = 64;
    bool          mask[MASK_CAP] = {false};
    char          bad[64]        = {0};
    const int     NAMES          = seam_count();
    if (NAMES > MASK_CAP) {
        say("; [promote] lockstep: REFUSED -- SEAM_NAMES has %d entries but the selection mask holds "
            "%d. This is a build error, not a configuration one: raise MASK_CAP. NOTHING was "
            "installed.\n",
            NAMES, MASK_CAP);
        return 0;
    }
    const detail::subset_result sel = detail::parse_seam_subset(want, mask, NAMES, bad, sizeof(bad));
    if (sel == detail::subset_result::unknown_token) {
        say("; [promote] lockstep: REFUSED -- lockstep_seams names an unknown seam '%s'. NOTHING was "
            "installed; a typo must not silently narrow the closure. Valid names: ",
            bad);
        for (int i = 0; i < NAMES; ++i) say("%s%s", SEAM_NAMES[i], i + 1 < NAMES ? ", " : "\n");
        say("; [promote] lockstep: RUN-CONFIG: REFUSED (bad lockstep_seams) -- this run is NOT promoted\n");
        return 0;
    }
    if (sel == detail::subset_result::empty) {
        say("; [promote] lockstep: REFUSED -- lockstep_seams='%s' selects NO seams. An empty subset "
            "installs nothing while still reading as a promoted run, which is how an asymmetric test "
            "whose asymmetry evaporated gets waved through. Omit the key for the full set.\n",
            want);
        say("; [promote] lockstep: RUN-CONFIG: REFUSED (empty lockstep_seams) -- this run is NOT promoted\n");
        return 0;
    }
    const bool subset = (sel == detail::subset_result::ok);

    // The mask is authoritative from here: every token was validated above, so a name that is not in
    // SEAM_NAMES cannot reach this point.
    auto selected = [&](const char *name) -> bool {
        for (int i = 0; i < NAMES; ++i)
            if (strcmp(SEAM_NAMES[i], name) == 0) return mask[i];
        return false;
    };

    int ok = 0, skipped = 0;
    for (const entry &e : seams) {
        if (!selected(e.name)) {
            ++skipped;
            say("; [promote] lockstep: seam %s SKIPPED (not in lockstep_seams)\n", e.name);
            continue;
        }
        if (e.install()) {
            ++ok;
        } else {
            // install_export_ok already logged WHY (an entry-byte guard mismatch means the DLL was
            // built against a different image, or something else already patched that entry).
            say("; [promote] lockstep: seam %s REFUSED -- turn engine is NOT promoted\n", e.name);
        }
    }
    if (subset) {
        // A SUBSET run is deliberately partial, so it must NOT print the "treat this run as invalid"
        // banner -- that line is the signal for an install FAILURE, and a bisect run that carries it
        // is indistinguishable from a broken one. It still must not read as the full promotion
        // either, hence its own banner naming the selection.
        say("; [promote] lockstep: SUBSET run -- %d/%d seams installed, %d skipped (lockstep_seams=%s)\n",
            ok, N, skipped, want);
        if (ok + skipped != N)
            say("; [promote] lockstep: %d seam(s) REFUSED in a subset run -- treat this run as invalid\n",
                N - ok - skipped);
    } else if (ok != N) {
        say("; [promote] lockstep: %d/%d seams installed -- PARTIAL, treat this run as invalid\n", ok, N);
    } else {
        say("; [promote] lockstep: ALL %d seams installed -- mh::lockstep is LIVE (sim_tick is %s)\n", ok,
            mh::config::ours_run()
                ? "OURS (default since LIB-TRANS-P) -- it rebinds onto the harness's detour (C6)"
                : "ORIGINAL -- `[config] mode=original` (C6)");
    }
    promoted::g_any_installed = ok > 0;

    // The TENTH seam, and it arrives by a different route (C4). llm_strat_time_tick's entry is OWNED by
    // the pacing detour, which ships armed, so it cannot be installed here -- the request is recorded
    // and mh/seams/net_lockstep.cpp performs the REBIND once that detour exists. Recording it here
    // rather than reading the ini twice keeps `lockstep_seams=` meaning one thing in one place.
    g_want_time_tick = selected("time_tick");
    if (g_want_time_tick) say("; [promote] lockstep: seam time_tick REQUESTED (installs by rebind -- see C4)\n");

    // The ELEVENTH seam, sim_tick (C6). DEFAULT 1 SINCE LIB-TRANS-P (2026-09-02) -- the "joins the
    // default set only ... as a change with its own gate" clause this comment used to end on was
    // DISCHARGED by that item: the lib_trans frame pair calls OUR sim_tick directly (its installer
    // refuses without it), and the -P ladder measured the whole configuration -- SP oracle
    // IDENTICAL 8000 steps/5 channels, golden replay MATCHED both arms, 2-peer asymmetric ALL
    // PAIRS IDENTICAL, a one-line negative arm DIVERGING at step 2. The key stays a key:
    // `[promote] sim_tick=0` (rollback_original.ini / lt_golden_stock.ini) restores the original
    // fall-through, and the pre--P L1-P evidence still describes THAT rolled-back configuration.
    //
    // It is still a member of SEAM_NAMES so `lockstep_seams=sim_tick` names something real and the
    // vocabulary stays one list -- but selection alone is not enough; the key must also be set.
    g_want_sim_tick = selected("sim_tick") && mh::config::ours_run();
    if (g_want_sim_tick)
        say("; [promote] lockstep: seam sim_tick REQUESTED (installs by rebind onto the determinism "
            "harness's detour -- see C6; default since LIB-TRANS-P)\n");

    // C5: the run's shape, MACHINE-READABLE. The banners above are prose -- a runner cannot act on
    // them, so a diagnostic bisect run and a shipping run have until now been distinguishable only by
    // whoever read the log. This one line is what lets tools/ (C7) refuse to report a ship-config
    // verdict over a partial promotion. Per the settled policy: per-seam gating is a DIAGNOSTIC
    // facility; whole-closure is the only shape a ship verdict may be read from.
    const int wanted = subset ? (ok + skipped) : N;
    if (subset)
        say("; [promote] lockstep: RUN-CONFIG: DIAGNOSTIC (subset %d/%d seams%s) -- NOT a ship config; "
            "no ship verdict may be read from this run\n",
            ok, N, g_want_time_tick ? " + time_tick" : "");
    else if (ok != wanted)
        say("; [promote] lockstep: RUN-CONFIG: INVALID (%d/%d seams installed) -- treat this run as "
            "unusable, not as un-promoted\n",
            ok, N);
    else
        say("; [promote] lockstep: RUN-CONFIG: SHIP (whole closure%s)\n",
            g_want_time_tick ? " incl. time_tick" : "");
    return ok;
}

bool time_tick_requested() { return g_want_time_tick; }
bool sim_tick_requested() { return g_want_sim_tick; }


} // namespace mh::lockstep
