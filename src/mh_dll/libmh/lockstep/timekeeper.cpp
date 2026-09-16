//
// lockstep/timekeeper.cpp -- llm_strat_time_tick @0x0043eea3, reimplemented (RI-LOCKSTEP / L1 batch D).
//
// Spec: tmp/l1/llm_strat_time_tick_0043eea3.asm. The header carries the contract and the reason this
// function is un-shadowable; this file is the translation and the notes are about what a plausible
// translation gets WRONG.
//
// TEN THINGS A CAREFUL-LOOKING TRANSLATION STILL GETS WRONG HERE. Each is a real difference, each was
// read off the disassembly rather than the decompiler output, and each is marked at its site below.
//
//  (1) THE TWO CLOCK GLOBALS ARE NOT "current" AND "last". `LAST_GAME_TIME` (0xe587b9) is the one
//      carried across frames: it is read first, then overwritten with `now`. `CURRENT_GAME_TIME`
//      (0xe587b1) is a COPY made afterwards and read only by the FPS ring. Ghidra's listing prints
//      the two assignments in the opposite order, which is harmless there and a bug if copied.
//  (2) `over` IS COMPUTED ONCE, before the sync-wait machine runs, and reused after it. Recomputing
//      it after the peer-timeout branch would be wrong the moment that branch ever moved TOTAL.
//  (3) THE CLAMP SUBTRACTS `over` FROM *BOTH* TOTAL_GAME_TIME AND GAME_TIME_DELTA. Dropping the
//      second is the classic version of this bug and it silently doubles the effective sim rate.
//  (4) `over < 0` AND `over >= 0` DO NOT RESET THE SAME THINGS. Both clear SYNC_WAIT_ACTIVE and
//      reload the countdown and the elapsed timer -- but only the `< 0` path also DISARMS
//      PEER_TIMEOUT_ELAPSED back to -1.0. So a peer-timeout that is already counting survives a
//      transient un-stall of the not-parked kind but not of the parked kind.
//  (5) EVERY FLOAT TEST IS AN x87 Jcc, AND THE UNORDERED DIRECTION DIFFERS PER SITE. Six of them; each
//      is written through the helper named for its Jcc, never through a bare C++ operator.
//  (6) THE STALL PREDICATE IS `delta < sim_step_interval`, TESTED AFTER THE CLAMP. It is not a
//      comparison against zero, and the order matters: before the clamp it would almost never fire.
//  (7) THE RETRY GATE'S ARITHMETIC IS `elapsed - (60 - countdown) > 1.0`, with the `(60 - countdown)`
//      term an INTEGER loaded through FILD. It is a "one ack per elapsed second" pacer, not a simple
//      countdown, and simplifying it to `elapsed > 1.0` changes the retry cadence.
//  (8) THE RESET AT THE END OF THE RETRY BLOCK FIRES ON `countdown <= 0 || overlay_result >= 0`.
//      Reading the two `Jcc`s as a single condition is easy to get backwards -- `JLE` jumps INTO the
//      reset, `JL` jumps PAST it.
//  (9) THE ADAPTIVE FLOOR READS TWO DIFFERENT .rdata SLOTS. `0x00500a10` is the numerator in the
//      compare and `0x00500a18` the numerator in the assignment. They hold the same value in this
//      image, so folding them to one constant passes every test and encodes an assumption the binary
//      does not make. Two fields, deliberately.
// (10) THE RETURN IS `(idx + 1) / 20`, FROM AN `IDIV` WHOSE REMAINDER IS WHAT GETS STORED BACK. It is
//      a 20-frame tick signal, not a status code, and the stored index is the remainder.
//
#include "lockstep/lockstep_state.h" // SB-BIND T4: host_binds() -- the production-arm addresses
#include "lockstep/turn_engine.h"
#include "lockstep/lt_player_by_side_id.h" // LIB-TRANS-P direct edge

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "lockstep/internal_call.h" // MH_INTERNAL_CALL -- the rebound clock read
#include "lockstep/lt_time_query.h"
#include "lockstep/internal_call.h" // MH_INTERNAL_CALL -- C8-d, the intra-closure edges
#include "lockstep/overlay_hoist.h" // LIB-ABI stage E: the hoisted GAME_MODE/overlay-latch writes
#include "lockstep/resync.h"        // is_local_leader_peer / count_active_players
#include "lockstep/tx_emit.h"       // send_lockstep_extend, keepalive, broadcast_player_leave,
                                    // player_remove, player_remove_timeout

#include <cstdint>
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::lockstep {

namespace {

// The "<something> (<player name>)" red floating line. Two emit sites in the original -- the
// peer-timeout drop at 0x0043efd0 and the leave-consensus fallback at 0x0043f255 -- and they are
// instruction-for-instruction identical, so they are one call here.
//
// The operands are raw: `IMUL EDX,pidx,0x740` + `MOV EAX,0xcff060` + `ADD EAX,0x714` is
// _G_LLM_STRAT_PLAYERS[pidx].name, and `PUSH dword ptr [0x005846a4]` is the FOLDED address
// G_TEXT_PTRS + 0xa6*4 rather than an index computation.
constexpr int32_t PLAYER_STRIDE   = 0x740; // IMUL ...,0x740
constexpr int32_t PLAYER_NAME_OFF = 0x714; // ADD EAX,0x714
constexpr int32_t LEFT_TEXT_ID    = 0xa6;  // 0x005846a4 = G_TEXT_PTRS + 0xa6*4

// u__s___s_ @0x005009e8, the UTF-16 "%s (%s)" the original pushes as a folded operand. Spelled as
// a C++ literal rather than bound through the registry, on libmh/sim/'s established precedent
// (sim_bldg_completion_dispatch.cpp's TEXT_FMT_NAME_REASON, and the two TUs its comment cites):
// this is sprintf FORMAT PUNCTUATION in .rdata, not localized text -- the localized text is what
// G_TEXT_PTRS supplies as an ARGUMENT to it -- so an identical literal produces identical output
// and removes an address. NOTE it is a distinct object from the same text at 0x005017bc that
// rx_dispatch.cpp's handlers use; the originals really do carry two copies, and the note stays here
// so nobody "discovers" that and collapses something in the binary.
constexpr const wchar_t *TEXT_FMT_LABEL_PAREN_NAME = L"%s (%s)";

void live_print_player_left_alert(int32_t player_idx) {
    const host_bind_state hb   = host_binds();
    char                 *name = hb.players_bytes +
                 static_cast<uintptr_t>(player_idx) * PLAYER_STRIDE + PLAYER_NAME_OFF;
    void *wide = mh::host().ansi_to_wide_scratch(name);
    // w_sprintf through the GENERATED fixed-arity shim (mh::call::w_sprintf__vss) rather than a
    // hand-rolled `int(__cdecl *)(...)` cast at mh::addr::w_sprintf. The old comment here said the
    // generator "refuses varargs by design" and inferred that a typed pointer was therefore the
    // complete interop; the first half is still true and the inference no longer is -- gen_dll_calls
    // emits per-arity variants (__v/__vs/__vss/__vsss/...) for exactly this call, so the calling
    // convention now comes from the DB instead of from a comment.
    MH_CRT(w_sprintf__vss)(hb.text_scratch, TEXT_FMT_LABEL_PAREN_NAME,
                           static_cast<const wchar_t *>(hb.text_ptrs[LEFT_TEXT_ID]),
                           static_cast<const wchar_t *>(wide));
    mh::state::evt::text_float_red(hb.text_scratch);
}

// The sync-wait machine's "stand down" reload, written at three of its four exits. The FOURTH site
// (the `over < 0` path) does this AND disarms the peer timeout -- see note (4); that extra store is
// at the call site, not folded in here, because folding it would make the two paths look identical.
void stand_down(const timekeeper_state &s) {
    *s.sync_wait_active     = 0;
    *s.sync_retry_countdown = SYNC_RETRY_RELOAD;
    *s.sync_wait_elapsed    = 1.0;
}

// The three-statement "advertise a fresh horizon" phrase, emitted verbatim at three sites
// (0x0043f0b7, 0x0043f15c, 0x0043f1c6). Identical every time, including the order.
void advertise_horizon(const timekeeper_state &s, const timekeeper_calls &c) {
    *s.horizon = *s.game_clock + *s.lockstep_step_size;
    c.send_lockstep_extend(*s.horizon);
    c.commit_horizon();
}

// ---- U20: the de-sync icon counters, on the impure edge -----------------------------------------
//
// The counting itself lives in the seam layer (net_lockstep.cpp owns g_icon_calls/g_icon_shown and
// the mh_lockstep.log columns that print them). mh/lockstep must not depend on mh/seams -- the same
// rule desync_watch's log sink and turn_engine.cpp:158's seam_log pointer follow -- so the module
// owns two nullable sinks and the seam layer wires them at arm time.
//
// WHY THIS EXISTS AT ALL: before it, the only writer of those two counters was the naked
// wait_overlay_gate_thunk spliced into the ORIGINAL call site at 0x0043f0fc. That site is inside
// llm_strat_time_tick, promoted by default since SHIP_PROMOTE_LOCKSTEP -- so on every shipped build
// the columns read 0, which looks like "no de-sync icons" and is actually "nobody counted".
void (*g_note_wanted)() = nullptr;
void (*g_note_shown)()  = nullptr;

void live_note_overlay_wanted() {
    if (g_note_wanted) g_note_wanted();
}

// The `icon_shown` half. A wrapper rather than a bare mh::call:: binding for the same reason
// live_print_player_left_alert three slots up is one -- the impure edge is where a side effect that
// is not part of the original's behaviour belongs.
int32_t live_wait_player_overlay_show(int32_t player_idx) {
    if (g_note_shown) g_note_shown();
    return mh::state::evt::wait_player_overlay_show_i32(player_idx);
}

} // namespace

void set_icon_counters(void (*wanted)(), void (*shown)()) {
    g_note_wanted = wanted;
    g_note_shown  = shown;
}

const timekeeper_calls &timekeeper_live_calls() {
    static const timekeeper_calls tc = {
        // NOT MH_INTERNAL_CALL, DELIBERATELY, and this is the one edge in the closure that must
        // stay ENTRY-ROUTED forever (un-rebound at LIB-TRANS-P, 2026-09-02, after the wave-2 rebind
        // froze every pinned run's clock): time_GetCurrentTime's ENTRY @0x00427616 is the
        // determinism harness's pin_wallclock INSTRUMENT POINT -- the pin whole-body-replaces that
        // entry with the deterministic counter, and a direct call to our body reads the REAL clock
        // underneath it, so time_tick and the pin disagree and the game clock parks at the pin base
        // (measured: chain-on_sim_tick advanced the pinned clock 8000+s while GAME_CLOCK sat at
        // 1000). The same doctrine as C4/C6: the instrument owns the entry; callers reach the
        // function THROUGH it. Promotion of the row still exists -- sim_lt_promote installs our
        // body AT that entry in unharnessed runs and YIELDS to the pin in harnessed ones -- so this
        // entry-routed edge reaches ours when promoted, the pin when pinned, the original
        // otherwise: exactly right in all three configs.
        MH_LIBMH_BIND(time_GetCurrentTime),
        mh::state::evt::overlay_dismiss_i32,
        MH_INTERNAL_CALL(llm_net_send_lockstep_extend, mh::lockstep::send_lockstep_extend),
        // commit_horizon and find_horizon_match_side are OURS (batch A). Law 4: once a function is
        // inside the closure, the caller reaches it as a plain C++ call rather than through a seam.
        // That is also why the promoted commit_horizon seam counter reads lower than its shadow run's
        // -- only ORIGINAL callers still go through it.
        //
        // C8-d moved these two from a hand-written lambda to MH_INTERNAL_CALL. Same generated code in
        // the default build -- the point is that they now come BACK to entry routing under
        // MH_INTERNAL_EDGES_ENTRY_ROUTED like the other 17, instead of being two edges that a future
        // shadow campaign over the closure interior would silently fail to reach.
        MH_INTERNAL_CALL(llm_net_lockstep_commit_horizon, mh::lockstep::commit_horizon),
        MH_INTERNAL_CALL(llm_net_lockstep_find_horizon_match_side,
                         mh::lockstep::find_horizon_match_side),
        MH_INTERNAL_CALL(llm_strat_player_by_side_id, mh::lockstep::player_by_side_id),
        MH_INTERNAL_CALL(llm_net_player_remove_timeout, mh::lockstep::player_remove_timeout),
        MH_INTERNAL_CALL(llm_net_player_remove, mh::lockstep::player_remove),
        MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
        live_note_overlay_wanted,      // U20: "the game WANTED the icon", counted BEFORE the gate
        live_wait_player_overlay_show, // U20: ...and this one counts what was actually drawn
        MH_INTERNAL_CALL(llm_net_send_lockstep_keepalive, mh::lockstep::send_lockstep_keepalive),
        mh::state::evt::sync_overlay_show,
        MH_INTERNAL_CALL(llm_net_lockstep_is_local_leader_peer, mh::lockstep::is_local_leader_peer),
        MH_INTERNAL_CALL(llm_net_lockstep_count_active_players, mh::lockstep::count_active_players),
        MH_INTERNAL_CALL(llm_net_lockstep_broadcast_player_leave,
                         mh::lockstep::lockstep_broadcast_player_leave),
        live_print_player_left_alert,
        &live_overlay_hoist_ops(), // LIB-ABI stage E
    };
    return tc;
}

namespace detail {

int32_t time_tick(const timekeeper_state &s, const timekeeper_calls &c, const reimpl_fixes &fx,
                  double now) {
    // ---- 1. advance game time -------------------------------------------------------------------
    // NOTE (1): `prev` comes out of LAST_GAME_TIME, which is then overwritten; CURRENT_GAME_TIME is a
    // copy made after. 0x0043eebb: MOV EAX,[0xe587b9] (x2, the two halves) -> local; 0x0043eed0:
    // FSTP [0xe587b9]; 0x0043eed6-dc: FLD [0xe587b9] / FSTP [0xe587b1].
    const double prev    = *s.last_game_time;
    *s.last_game_time    = now;
    *s.current_game_time = *s.last_game_time;

    // FLD [b9]; FSUB local; FLD game_speed; FMULP  ->  (now - prev) * game_speed
    *s.game_time_delta = (*s.last_game_time - prev) * *s.game_speed;
    *s.total_game_time = *s.game_time_delta + *s.total_game_time;

    if (*s.session_mode == SESSION_MP_LOCKSTEP) {
        // NOTE (2): computed ONCE here, consumed twice below.
        const double over = *s.total_game_time - *s.committed;

        // 0x0043ef27: FLDZ; FCOMP over; JA -> the "we still have room" path. NOTE (5): JA is not taken
        // when unordered, so a NaN overshoot lands in the PARKED branch.
        if (detail::x87_above(0.0, over)) {
            // ---- not parked: the sim has room before the barrier ----
            if (*s.sync_wait_active == 1) {
                stand_down(s);
                *s.peer_timeout_elapsed = -1.0; // NOTE (4): ONLY this path disarms the peer timeout
                // C3, migrated `resync_trigger_reset`. THIS stand_down site and not the other one: the
                // byte splice was at 0x0043f2a5, identifiable as this path because it is the only
                // recovery exit that also disarms the peer timeout, and the patch's own rationale named
                // all four per-episode siblings as reset here. Applying it at both sites would change
                // behaviour the byte patch never touched.
                if (fx.resync_trigger_reset) *s.resync_trigger_count = 0;
                {
                    // LIB-ABI stage E hoist: latch the !=4 guard BEFORE (the callee reads it),
                    // store the folded dismiss rule AFTER (overlay_hoist.h's ordering law).
                    const bool not4 = hoist_mode_not4(c.hoist);
                    c.overlay_dismiss();
                    hoist_dismiss(c.hoist, not4);
                }
            }
        } else {
            // ---- parked (or NaN): we are at or past the committed horizon ----
            *s.sync_wait_elapsed = *s.game_time_delta + *s.sync_wait_elapsed;

            // 0x0043ef47: FLDZ; FCOMP peer_timeout_elapsed; JA skips. So the timer runs only while it
            // is armed (>= 0.0), and a NaN keeps running rather than being treated as disarmed.
            if (!detail::x87_above(0.0, *s.peer_timeout_elapsed)) {
                *s.peer_timeout_elapsed = *s.game_time_delta + *s.peer_timeout_elapsed;
                // 0x0043ef6a: FLD elapsed; FCOMP secs; JBE skips -> fire only on a strict, ordered >.
                if (!detail::x87_below_equal_or_unordered(*s.peer_timeout_elapsed,
                                                          *s.peer_timeout_secs)) {
                    const int32_t side = c.find_horizon_match_side();
                    const int32_t pidx = c.player_by_side_id(side);
                    // `MOVZX EDX,word [PlayerSide]` + `pidx*8` -- peer_state is [8][8] row-major and
                    // this indexes row `pidx`, column `PlayerSide`, i.e. "what does the LOCAL side
                    // think of that peer".
                    const uint8_t st = s.peer_state[pidx * MAX_PLAYERS + *s.player_side];
                    if (st == PEER_STATE_NONE && (*s.status_flags & LS_HORIZON_PENDING) != 0) {
                        c.player_remove_timeout(side);
                        c.print_player_left_alert(pidx);
                        *s.status_flags          = static_cast<uint8_t>(*s.status_flags & ~LS_HORIZON_PENDING);
                        *s.lockstep_player_count = *s.lockstep_player_count - 1;
                        *s.lobby_scan_host_count = *s.lobby_scan_host_count - 1;
                        c.time_resync_and_tick();
                    }
                }
            }

            // NOTE (3): the clamp takes the overshoot off BOTH accumulators.
            *s.total_game_time = *s.total_game_time - over;
            *s.game_time_delta = *s.game_time_delta - over;

            // NOTE (6): the STALL predicate, and it is tested AFTER the clamp.
            // 0x0043f030: FLD delta; FCOMP sim_step_interval; JC -> parked machine (unordered too).
            if (detail::x87_below_or_unordered(*s.game_time_delta, *s.sim_step_interval)) {
                int32_t overlay_result = -1;

                // 0x0043f083: FLDZ; FCOMP delta; JBE skips -> clamp a negative delta up to zero. The
                // original zeroes both dwords rather than storing a computed 0.0, which is the same
                // bit pattern for +0.0.
                if (detail::x87_above(0.0, *s.game_time_delta)) *s.game_time_delta = 0.0;

                if (*s.sync_wait_active == 0) {
                    *s.sync_wait_active = 1;
                    advertise_horizon(s, c);
                } else {
                    // Note the ORDER here differs from the arm branch: commit FIRST, then look up who
                    // we are waiting on, then show the overlay. No horizon is advertised.
                    c.commit_horizon();
                    const int32_t side = c.find_horizon_match_side();
                    const int32_t pidx = c.player_by_side_id(side);
                    // UNCONDITIONAL since C8-e: this used to branch on the migrated `defang_tt_wait`
                    // (0 = stock, 1 = suppress, 2 = show only on sustained silence). All three
                    // defangs were dropped as a scope decision -- resync_trigger_gate supersedes
                    // them as the root-cause fix -- so this is back to stock behaviour, which is
                    // what mode 0, the shipped default, already was.
                    //
                    // U20 -- and note the ORDER, which is the whole design. `note_overlay_wanted`
                    // fires FIRST and UNCONDITIONALLY: it is the `icon_calls` edge, so it has to see
                    // the frames the gate suppresses or `icon_calls` collapses onto `icon_shown` and
                    // the delta that measures the gate is gone. The gate's predicate is the one
                    // sync_overlay_show is already gated on 30 lines below (`countdown <
                    // SYNC_OVERLAY_AFTER` = genuine multi-second silence), and this call's return is
                    // DISCARDED -- which is why suppressing it cannot change control flow and the
                    // mode-8 sibling's cannot be gated the same way. Countdown is read here at its
                    // pre-decrement value, the same instant the retired byte thunk read it.
                    c.note_overlay_wanted();
                    if (!fx.desync_icon_gate || *s.sync_retry_countdown < SYNC_OVERLAY_AFTER) {
                        // LIB-ABI stage E hoist: the show's arming guard reads the PRE-call list
                        // and the TIGHTER mode==2 (0x004c7e8b) -- latch it, call, then store the
                        // unconditional prologue pair + the armed mode write.
                        const bool armed = hoist_slot_free(c.hoist) && hoist_mode_is2(c.hoist);
                        c.wait_player_overlay_show(pidx);
                        hoist_wait_player(c.hoist, pidx, armed);
                    }
                }

                // NOTE (7): one ack per elapsed second. `(60 - countdown)` is how many acks have gone
                // out; FILD makes it a double; FSUBR gives elapsed - that; FLD1/FCOMPP compares 1.0
                // against it and JC takes the retry when 1.0 < it (and when unordered).
                const double acks_sent = static_cast<double>(SYNC_RETRY_RELOAD - *s.sync_retry_countdown);
                if (*s.sync_retry_countdown != 0 &&
                    detail::x87_below_or_unordered(1.0, *s.sync_wait_elapsed - acks_sent)) {
                    const int32_t side      = c.find_horizon_match_side();
                    *s.sync_retry_countdown = *s.sync_retry_countdown - 1;
                    c.send_lockstep_ack(side);

                    // 0x0043f145: FLD clock; FADD step; FCOMP horizon; JBE skips -> re-advertise only
                    // on a strict, ordered increase.
                    if (!detail::x87_below_equal_or_unordered(*s.game_clock + *s.lockstep_step_size,
                                                              *s.horizon)) {
                        advertise_horizon(s, c);
                    }

                    // `CMP countdown,0x37; JG skip` -> run it while countdown <= 0x37, i.e. < 0x38.
                    // The migrated `defang_tt_sync` branch was here and is gone with the other two
                    // (C8-e scope decision). Worth keeping the equivalence it rested on, because it
                    // is the reason the byte patch was safe at all: the patch NOPped a 5-byte CALL,
                    // and overlay_result is pre-initialised to -1 above, so "do not call" and "call
                    // NOPped" agreed on the value note (8) branches on. If that initialiser ever
                    // changes, that equivalence changes with it -- and the archive's restore
                    // procedure depends on it.
                    if (*s.sync_retry_countdown < SYNC_OVERLAY_AFTER) {
                        // LIB-ABI stage E hoist, the two-level one: the original's armed branch
                        // writes mode 3, but an answer >= 0 makes it tail-call overlay_dismiss,
                        // which re-writes the mode by the dismiss rule. The dismiss's own !=4
                        // latch collapses to the pre-call value (armed implies !=4; unarmed
                        // leaves the mode untouched), so ONE latch serves both arms.
                        //
                        // R9 ORDERING LAW -- READ, CLEAR, then EMIT. The original fused an arm
                        // and a poll into one call and returned the answer; the arm is now a
                        // screen request and the answer is libmh's to read. The read must come
                        // FIRST because the hosted arm routes the emit onto the SAME original,
                        // which samples-and-clears the answer itself -- reading after it would
                        // see the -1 it just wrote and lose the button press. Reading first is
                        // faithful: the answer is written by the modal's button callback on an
                        // EARLIER frame, so no arm can produce the answer its own call reads.
                        // Our clear then makes the original's own clear the idempotent second
                        // write, exactly as every other stage-E hoist does.
                        const bool not4  = hoist_mode_not4(c.hoist);
                        const bool armed = hoist_slot_free(c.hoist) && not4;
                        overlay_result   = hoist_overlay_result(c.hoist);
                        if (overlay_result >= 0) {
                            // NO screen request on this arm, and that is exact rather than a
                            // shortcut: an answer exists only while the LOCKSTEP_SYNC list is
                            // installed, so the original's arm guard (slot free) cannot hold on
                            // the frame an answer is read. What the original DID do here is the
                            // clear and the tail dismiss -- at this instant, before the reset
                            // block below dismisses again, which it also did.
                            hoist_result_clear(c.hoist);
                            c.overlay_dismiss();
                            hoist_dismiss(c.hoist, not4);
                        } else {
                            if (armed) hoist_mode_set(c.hoist, 3);
                            c.sync_overlay_show();
                        }
                    }

                    // NOTE (8): `JLE 0x0043f1a8` enters the reset, `JL 0x0043f28b` skips past it.
                    if (*s.sync_retry_countdown <= 0 || overlay_result >= 0) {
                        *s.sync_retry_countdown = SYNC_RETRY_RELOAD;
                        *s.sync_wait_elapsed    = 1.0;
                        advertise_horizon(s, c);

                        if (side == *s.local_player_index) {
                            // The original calls llm_net_lockstep_hook_stub @0x0049bc88 here and
                            // nothing else; the body is a stack probe and a return, so the call is
                            // not reproduced (SIMABI-HOOKS). The BRANCH is load-bearing and stays:
                            // it is what keeps a self-match out of the leader/removal path below.
                        } else if (c.is_local_leader_peer(side) != 0 &&
                                   (*s.status_flags & LS_HORIZON_PENDING) == 0) {
                            if (c.count_active_players() > LEAVE_CONSENSUS_MIN) {
                                c.broadcast_player_leave(side);
                                *s.status_flags =
                                    static_cast<uint8_t>(*s.status_flags | LS_HORIZON_PENDING);
                            } else {
                                c.player_remove(side);
                                c.print_player_left_alert(c.player_by_side_id(side));
                            }
                        }
                        {
                            // LIB-ABI stage E hoist -- fresh latch: the sync_overlay_show hoist
                            // above may have just stored the mode this guard reads.
                            const bool not4 = hoist_mode_not4(c.hoist);
                            c.overlay_dismiss();
                            hoist_dismiss(c.hoist, not4);
                        }
                    }
                }

                // The nag counter DECAYS only while it is still above the floor -- it is not a plain
                // countdown to zero. `CMP [nag],0x1e; JL skip; DEC`.
                if (*s.stall_nag_count >= STALL_NAG_FLOOR) *s.stall_nag_count = *s.stall_nag_count - 1;
            } else if (*s.sync_wait_active == 1) {
                // Un-stalled while parked. NOTE (4) again: no peer-timeout disarm on this path.
                stand_down(s);
                const bool not4 = hoist_mode_not4(c.hoist); // LIB-ABI stage E hoist
                c.overlay_dismiss();
                hoist_dismiss(c.hoist, not4);
            }
        }

        // ---- 3a. the game's own adaptive lookahead controller ---------------------------------
        // 0x0043f2e6: FLD adapt_next_time; FCOMP game_clock; JA skips the whole retune. So it runs
        // when next_time <= clock, and ALSO when either is NaN.
        if (!detail::x87_above(*s.adapt_next_time, *s.game_clock)) {
            // `LEA EAX,[EAX+EAX*4]` on an unsigned count, then `CMP EAX,stall_count; JA` skips -- an
            // UNSIGNED compare against a signed global. Kept unsigned, because a negative stall count
            // would then read as huge and GROW the step, which is what the original does.
            const uint32_t grow_threshold = *s.active_player_count * 5u;
            if (!(grow_threshold > static_cast<uint32_t>(*s.stall_count))) {
                // FLD step; FCOMP step_max; JNC skips -> grow only while strictly below the cap, and
                // not when unordered.
                if (!detail::x87_above_equal(*s.lockstep_step_size, *s.step_max))
                    *s.lockstep_step_size = *s.lockstep_step_size * *s.step_grow_mul;
            }
            // `CMP stall_count,0; JG skip` -- a SIGNED test here, unlike the unsigned one above.
            if (*s.stall_count <= 0) *s.lockstep_step_size = *s.lockstep_step_size / *s.step_shrink_div;

            // NOTE (9): two distinct .rdata numerators, compare vs assign.
            // FLD1; FADD fps; FDIVR num_cmp; FCOMP step; JBE skips.
            const double denom = 1.0 + *s.fps_estimate;
            if (!detail::x87_below_equal_or_unordered(*s.step_min_fps_num_cmp / denom,
                                                      *s.lockstep_step_size)) {
                *s.lockstep_step_size = *s.step_min_fps_num_set / denom;
            }
            *s.stall_count     = 0;
            *s.adapt_next_time = *s.game_clock + *s.adapt_interval_secs;
        }
    }

    // ---- 3b. the FPS ring. Runs in EVERY session mode -- it is outside the mode-3 block ----------
    const int32_t idx      = *s.frame_time_ring_idx;
    *s.fps_estimate        = *s.fps_window_num / (*s.current_game_time - s.frame_time_ring[idx]);
    s.frame_time_ring[idx] = *s.current_game_time;

    // NOTE (10): one IDIV -- quotient returned, remainder stored back.
    const int32_t next     = idx + 1;
    *s.frame_time_ring_idx = next % FRAME_TIME_RING_SLOTS;
    return next / FRAME_TIME_RING_SLOTS;
}

} // namespace detail

// The production wrapper, and the split is the whole reason this function has unit-test evidence
// instead of no evidence: it owns the ONE unrestorable input (see timekeeper_state).
int32_t time_tick() {
    promoted::live(9, "time_tick"); // C8-d: counted here, not at the entry thunk

    const timekeeper_calls &c = timekeeper_live_calls();
    return detail::time_tick(timekeeper(), c, fixes(), c.get_current_time());
}

} // namespace mh::lockstep
