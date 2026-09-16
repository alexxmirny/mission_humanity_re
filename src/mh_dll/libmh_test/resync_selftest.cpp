#include "lockstep/resync.h"
#include "lockstep/tx_emit_order.h"
#include "lockstep/turn_engine.h" // MAX_PLAYERS / PLAYER_ALIVE / PLAYER_HUMAN

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_checks, g_fails;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

using mh::lockstep::MAX_PLAYERS;
using mh::lockstep::PLAYER_ALIVE;
using mh::lockstep::PLAYER_HUMAN;
using mh::lockstep::resync_calls;
using mh::lockstep::resync_state;
using profile = mh::game::mh_llm_strat_player_profile;

namespace rd = mh::lockstep::detail;

// ---- the recorder ------------------------------------------------------------------------------
// resync_calls holds plain function pointers (it must: in production they are the generated thunks),
// so the recorder is a file-scope singleton rather than a lambda capture. Same arrangement as
// orders_selftest.cpp.
struct call_log {
    int                   broadcasts = 0;
    std::vector<double>   broadcast_exec_time;
    int                   extends = 0;
    std::vector<double>   extend_horizon;
    int                   returns     = 0;
    int                   delay_calls = 0;
    int32_t               delay_value = 0; // what sync_delay reports back
    int                   tick_calls  = 0;
    std::vector<uint32_t> tick_seq;         // scripted clock; past the end it keeps ticking, see below
    int32_t               side_lookup = -1; // what player_by_side_id reports back
    std::vector<int32_t>  side_args;
    void                  reset() { *this = call_log{}; }
};
call_log g_log;

int32_t rec_player_by_side_id(int32_t side_id) {
    g_log.side_args.push_back(side_id);
    return g_log.side_lookup;
}
void rec_broadcast_resync_state(double exec_time) {
    ++g_log.broadcasts;
    g_log.broadcast_exec_time.push_back(exec_time);
}
void rec_send_lockstep_extend(double horizon) {
    ++g_log.extends;
    g_log.extend_horizon.push_back(horizon);
}
// The scripted clock. Past the end of the script it KEEPS TICKING (one ms per read) rather than
// repeating its last value, and that detail is load-bearing rather than cosmetic: a repeating clock
// lets a WRONG implementation spin forever instead of failing. The signed-compare mutation does
// exactly that on the wrap case below -- with a frozen clock it hangs the suite (measured, and it
// cost a 19-minute mutation run before the driver grew a timeout), with a ticking one it terminates
// and fails the assertion that names it. A test that hangs on a bad body has not detected anything.
uint32_t rec_ticks_ms() {
    const size_t i = static_cast<size_t>(g_log.tick_calls++);
    if (g_log.tick_seq.empty()) return 0;
    if (i < g_log.tick_seq.size()) return g_log.tick_seq[i];
    return g_log.tick_seq.back() + static_cast<uint32_t>(i - g_log.tick_seq.size() + 1);
}
void    rec_menu_force_return_to_main() { ++g_log.returns; }
int32_t rec_sync_delay() {
    ++g_log.delay_calls;
    return g_log.delay_value;
}

const resync_calls &recording_calls() {
    static const resync_calls cc = {
        rec_player_by_side_id,
        rec_broadcast_resync_state,
        rec_send_lockstep_extend,
        rec_ticks_ms,
        rec_menu_force_return_to_main,
        rec_sync_delay,
    };
    return cc;
}

// ---- the fixture -------------------------------------------------------------------------------
// Every global the five read or write, as locals. Defaults are the shipping constants MEASURED out
// of the EN image (2000.0 / 2.0 at 0x005017f4 / 0x005017fc), so a case that does not care about the
// deadline still computes the real one.
struct fixture {
    profile      players[MAX_PLAYERS]{};
    int32_t      active_count         = -1; // poisoned: a body that never writes it must be caught
    int32_t      local_slot           = 0;
    int32_t      resync_trigger_count = 0;
    int32_t      resync_in_progress   = 0;
    uint32_t     resync_deadline_ms   = 0;
    double       horizon              = 0.0;
    double       game_clock           = 0.0;
    double       step_size            = 0.0;
    double       timeout_ms           = 2000.0;
    double       pad_ms               = 2.0;
    resync_state st{};

    fixture() {
        std::memset(players, 0, sizeof(players));
        st.players              = players;
        st.active_count         = &active_count;
        st.local_slot           = &local_slot;
        st.resync_trigger_count = &resync_trigger_count;
        st.resync_in_progress   = &resync_in_progress;
        st.resync_deadline_ms   = &resync_deadline_ms;
        st.horizon              = &horizon;
        st.game_clock           = &game_clock;
        st.step_size            = &step_size;
        st.resync_timeout_ms    = &timeout_ms;
        st.resync_pad_ms        = &pad_ms;
        g_log.reset();
    }
    void human(int slot) { players[slot].status_flags = PLAYER_ALIVE | PLAYER_HUMAN; }
};

} // namespace

int run_resynctest() {
    printf("=== resynctest (C8-c: the six residue functions over plain locals) ===\n");

    // ---- 1. sync_delay_stub ---------------------------------------------------------------------
    // Trivial to assert and NOT trivial to get wrong quietly: this constant IS the carrier of
    // [net] resync_wait_fix (dll_patch_manifest.json, carrier=migrated:lockstep). If it ever stops
    // being 0, the byte patch that used to hold the line is already retired and the MP hang comes
    // back with nothing naming it. The assertion exists so that change cannot be silent.
    check("sync_delay_stub returns 0 -- it IS the resync_wait_fix carrier", rd::sync_delay_stub() == 0);

    // ---- 2. sync_busywait -----------------------------------------------------------------------
    {
        // The normal path under our stub: 0 delay -> no abort, returns 1.
        // RULES OUT: an implementation that treats 0 as "no wait requested" and returns 0/aborts.
        fixture f;
        g_log.delay_value = 0;
        g_log.tick_seq    = {1000, 1000};
        const int32_t r   = rd::sync_busywait(recording_calls());
        check("sync_busywait returns 1 on a zero delay", r == 1);
        check("sync_busywait does not abort on a zero delay", g_log.returns == 0);
    }
    {
        // The abort path. Unreachable with OUR stub (0 is not < 0), reachable in the original, so it
        // is translated and tested rather than assumed away.
        // RULES OUT: dropping the branch as dead code. (The original's other abort step, the empty
        // llm_teardown_hook_stub, is not reproduced -- SIMABI-HOOKS 2026-09-10.)
        fixture f;
        g_log.delay_value = -1;
        g_log.tick_seq    = {1000};
        const int32_t r   = rd::sync_busywait(recording_calls());
        check("sync_busywait returns 0 on a negative delay", r == 0);
        check("sync_busywait returns to the menu on a negative delay", g_log.returns == 1);
        check("sync_busywait does not read the clock on the abort path", g_log.tick_calls == 0);
    }
    {
        // THE ONE THAT SEPARATES THE IMPLEMENTATIONS. The original's spin compare is `CMP` + `JC`,
        // i.e. UNSIGNED. Start the clock just below INT_MAX so target overflows into the negative
        // half of a SIGNED reading: unsigned says "still below target, keep spinning", signed says
        // "already past it, stop". Picked precisely because the two disagree here and agree almost
        // everywhere else -- an ordinary mid-range input would pass against both.
        // RULES OUT: translating JC as JL (a signed compare), which the decompile's `uint` operands
        // make easy to do while believing you matched it.
        fixture f;
        g_log.delay_value = 0x20;
        g_log.tick_seq    = {0x7FFFFFF0u, 0x7FFFFFF5u, 0x80000010u}; // 3rd reading reaches target
        const int32_t r   = rd::sync_busywait(recording_calls());
        // 1 start read + 2 compares: the 2nd reading is still below target unsigned, the 3rd is not.
        // A SIGNED compare would stop after the FIRST compare (target reads as negative), giving 2 --
        // so this count is the discriminator, not decoration.
        check("sync_busywait spins across the INT_MAX boundary (unsigned compare, not signed)",
              r == 1 && g_log.tick_calls == 3);
    }
    {
        // The REAL behaviour at the genuine 32-bit wrap, asserted as it is rather than as one might
        // wish it were: start+delay wraps below start, so the first compare already reports "past
        // the target" and the wait is skipped. That is what the original does. Recording it stops a
        // future reader from "fixing" it into a wrap-aware wait and calling that equivalence.
        fixture f;
        g_log.delay_value = 0x20;
        g_log.tick_seq    = {0xFFFFFFF0u, 0xFFFFFFF0u};
        const int32_t r   = rd::sync_busywait(recording_calls());
        check("sync_busywait skips the wait when start+delay wraps past 2^32 (faithful, not fixed)",
              r == 1 && g_log.tick_calls == 2); // 1 start + 1 compare that immediately exits
    }

    // ---- 3. count_active_players ----------------------------------------------------------------
    {
        // ALIVE && HUMAN, not either. Three slots, each with a different flag combination.
        // RULES OUT: `||` instead of `&&`, and testing only one of the two bits.
        fixture f;
        f.human(0);                                                     // ALIVE|HUMAN  -> counts
        f.players[1].status_flags = PLAYER_ALIVE;                       // ALIVE only   -> does not
        f.players[2].status_flags = PLAYER_HUMAN;                       // HUMAN only   -> does not
        f.players[3].status_flags = PLAYER_ALIVE | PLAYER_HUMAN | 0x10; // extra bits are ignored
        const int32_t r           = rd::count_active_players(f.st);
        check("count_active_players requires ALIVE *and* HUMAN", r == 2);
        check("count_active_players writes the global it returns", f.active_count == 2);
    }
    {
        // It RECOMPUTES; it does not accumulate. Calling twice must give the same answer.
        // RULES OUT: forgetting the leading `*st.active_count = 0`, which is invisible on a first
        // call from a zeroed fixture and doubles on every call after -- exactly the shape that
        // survives a naive test and desyncs a match.
        fixture f;
        f.human(0);
        f.human(1);
        rd::count_active_players(f.st);
        const int32_t second = rd::count_active_players(f.st);
        check("count_active_players recomputes rather than accumulating", second == 2 && f.active_count == 2);
    }
    {
        // The empty roster. RULES OUT: a body that leaves the global untouched when nothing matches
        // (the fixture poisons it to -1 so "never written" is distinguishable from "written 0").
        fixture       f;
        const int32_t r = rd::count_active_players(f.st);
        check("count_active_players writes 0 on an empty roster", r == 0 && f.active_count == 0);
    }

    // ---- 4. is_local_leader_peer ----------------------------------------------------------------
    {
        // THE WEAK PREDICATE, and the case that separates it from the obvious wrong reading. The
        // scan STOPS at the first qualifying slot; it does not keep looking for the local one. So
        // with slot 1 and slot 3 both qualifying and US at slot 3, the answer is NO.
        // RULES OUT: "are we a participant at all", which is what a paraphrase naturally produces
        // and which would answer YES here -- making every peer think it is the leader and all of
        // them force a resync at once, which is the exact failure this election exists to prevent.
        fixture f;
        f.human(1);
        f.human(3);
        f.local_slot      = 3;
        g_log.side_lookup = -1; // exclude nobody
        check("is_local_leader_peer says NO when a lower slot qualifies first",
              rd::is_local_leader_peer(f.st, recording_calls(), -1) == 0);
    }
    {
        // Same roster, us at the FIRST qualifying slot -> yes.
        fixture f;
        f.human(1);
        f.human(3);
        f.local_slot      = 1;
        g_log.side_lookup = -1;
        check("is_local_leader_peer says YES when the first qualifying slot is us",
              rd::is_local_leader_peer(f.st, recording_calls(), -1) == 1);
    }
    {
        // EXCLUSION actually excludes: same roster, us at slot 3, but slot 1 is the excluded peer.
        // RULES OUT: ignoring the exclude argument, or comparing it against the SIDE id instead of
        // the resolved slot index.
        fixture f;
        f.human(1);
        f.human(3);
        f.local_slot      = 3;
        g_log.side_lookup = 1; // player_by_side_id(7) -> slot 1
        check("is_local_leader_peer skips the excluded slot",
              rd::is_local_leader_peer(f.st, recording_calls(), 7) == 1);
        check("is_local_leader_peer resolves the SIDE id through player_by_side_id",
              g_log.side_args.size() == 1 && g_log.side_args[0] == 7);
    }
    {
        // Nobody qualifies -> 0, and the lookup still happens (the original calls it unconditionally;
        // short-circuiting on -1 would be an optimisation that changes the call trace).
        fixture f;
        f.local_slot      = 0;
        g_log.side_lookup = -1;
        check("is_local_leader_peer says NO on an empty roster",
              rd::is_local_leader_peer(f.st, recording_calls(), -1) == 0);
        check("is_local_leader_peer resolves the side id even for the 'exclude nobody' convention",
              g_log.side_args.size() == 1);
    }

    // ---- 5. force_resync ------------------------------------------------------------------------
    {
        // THE ORDERING THAT IS LOAD-BEARING. `MOV [TRIGGER_COUNT],0` at 0x0049d9ee comes BEFORE the
        // `CMP [IN_PROGRESS],1` at 0x0049d9f8, so a call that finds a resync already running still
        // clears the counter.
        // RULES OUT: tidying the early-out to the top of the function, which is the single most
        // natural edit anyone would make here and which would leave the counter ratcheting -- the
        // exact mechanism behind the spurious ~2 s freeze every 2.6 s that the trigger-reset fix
        // was written to stop.
        fixture f;
        f.resync_trigger_count = 99;
        f.resync_in_progress   = 1;
        g_log.tick_seq         = {5000};
        rd::force_resync(f.st, recording_calls());
        check("force_resync clears TRIGGER_COUNT even when a resync is already in progress",
              f.resync_trigger_count == 0);
        check("force_resync does nothing else when one is already in progress",
              g_log.broadcasts == 0 && g_log.extends == 0 && f.resync_deadline_ms == 0);
    }
    {
        // The full path.
        fixture f;
        f.resync_trigger_count = 7;
        f.resync_in_progress   = 0;
        f.game_clock           = 120.0;
        f.step_size            = 0.25;
        g_log.tick_seq         = {5000};
        rd::force_resync(f.st, recording_calls());
        check("force_resync latches IN_PROGRESS", f.resync_in_progress == 1);
        check("force_resync clears TRIGGER_COUNT", f.resync_trigger_count == 0);
        check("force_resync broadcasts once, with exec_time 2.0",
              g_log.broadcasts == 1 && g_log.broadcast_exec_time.size() == 1 &&
                  g_log.broadcast_exec_time[0] == 2.0);
        // 5000 + 2000 + 2. RULES OUT: dropping the pad, or using one constant for both.
        check("force_resync deadline = ticks + timeout + pad", f.resync_deadline_ms == 7002u);
        check("force_resync advances HORIZON by one step", f.horizon == 120.25);
        check("force_resync advertises the NEW horizon",
              g_log.extends == 1 && g_log.extend_horizon.size() == 1 && g_log.extend_horizon[0] == 120.25);
    }
    {
        // The deadline arithmetic WRAPS; it does not saturate. The original is `FISTP qword` followed
        // by a load of the LOW dword, so a deadline past 2^32 comes back as its low 32 bits.
        // RULES OUT: computing in uint32 (which would overflow differently), and any clamp-to-max
        // "safety" a reimplementation might add -- near the 49.7-day GetTickCount rollover the
        // deadline MUST wrap or it is unreachable and the resync never times out.
        fixture f;
        g_log.tick_seq = {0xFFFFFF00u};
        rd::force_resync(f.st, recording_calls());
        // The literal is redundant with the expression on purpose, and it earned its keep on the
        // first run: it caught a hand-arithmetic slip in this very assertion while the computed
        // value was right. A wrap constant nobody can check by eye is worth stating twice.
        const uint32_t expect = static_cast<uint32_t>(0xFFFFFF00ull + 2002ull); // wraps to 0x6D2
        check("force_resync deadline wraps past 2^32 rather than saturating",
              f.resync_deadline_ms == expect && expect == 0x6D2u);
    }

    // ---- 6. send_order --------------------------------------------------------------------------
    {
        // The record layout, over a heap buffer. The tag byte, then the 68-byte order, then a net
        // cursor advance of 0x45.
        // RULES OUT: writing the order at `cursor` instead of `cursor+1` (the original increments
        // BETWEEN the tag store and the copy), and a total advance of 0x44 or 0x46.
        using mh::lockstep::game_order_tx;
        using mh::lockstep::ORDER_RECORD_BYTES;
        using mh::lockstep::ORDER_RECORD_TAG;

        std::vector<uint8_t>         buf(1016, 0xCD);
        int32_t                      cursor = 4; // deliberately NOT 0, so an ignored cursor is visible
        mh::lockstep::order_tx_state st{buf.data(), &cursor};

        static int  g_ic_calls = 0;
        static bool g_ic_same  = false;
        g_ic_calls             = 0;
        game_order_tx order{};
        std::memset(&order, 0xA5, sizeof(order));
        static const game_order_tx *g_ic_expect = &order;
        struct H {
            static void check_cb(const game_order_tx *o, const char *tag) {
                ++g_ic_calls;
                g_ic_same = (o == g_ic_expect) && tag && std::strcmp(tag, "NetgameSendOrderAdd") == 0;
            }
        };
        const mh::lockstep::order_tx_calls calls = {H::check_cb};

        const int32_t r = mh::lockstep::detail::send_order(st, calls, &order);
        check("send_order returns the documented constant 0 (the original's EAX is a leftover)", r == 0);
        check("send_order writes the tag byte at the ORIGINAL cursor", buf[4] == ORDER_RECORD_TAG);
        check("send_order copies the order at cursor+1", std::memcmp(buf.data() + 5, &order, sizeof(order)) == 0);
        check("send_order advances the cursor by 0x45 in total", cursor == 4 + ORDER_RECORD_BYTES);
        check("send_order leaves the byte after the record untouched", buf[4 + ORDER_RECORD_BYTES] == 0xCD);
        check("send_order calls the integrity check once, with the original's caller tag",
              g_ic_calls == 1 && g_ic_same);
    }

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
