//
// watchdog_selftest.cpp -- `net_selftest.exe watchdogtest`: the link watchdog's timing decisions,
// with no game, no rig and no wall clock.
//
// WHY THIS EXISTS, and why it is the ONLY evidence available. D16's failure is a MUTUAL drop on a
// healthy link: the watchdog needs g_conn_cs both to ping and to judge silence, MH_Net_Send holds
// that lock across a blocking send() bounded only by SO_SNDTIMEO (5 s), so a stalled link starves
// the watchdog -- it emits no keepalives, the peer reads that as OUR silence and drops us, and when
// we finally take the lock every last_rx has aged past the timeout so we drop the peer in the same
// pass. Both ends, on a link that was carrying data fine moments earlier.
//
// No rig run can show this:
//   * it needs a link slow enough for send() to block, which a LAN never is -- it reproduced on a
//     relayed internet game and on nothing else (2026-08-02);
//   * the load-bearing half is a drop that must NOT happen, and a run cannot demonstrate the
//     absence of an event -- a green run and a run that never reached the condition look identical
//     (the [[gate-vacuous-pass]] shape this repo already refuses elsewhere).
// So the decision is pure (include/mh_net_watchdog.h) and asserted here, exactly as ui_test's
// stall_abort is.
//
// The third block is the one that matters most: it rules out the VACUOUS fix. "Never drop anybody"
// would pass the first two blocks and silently retire the entire R-live feature.
//
#include <stdio.h>

#include "include/mh_net_watchdog.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// The shipping numbers, so the cases are the ones the field actually runs.
constexpr unsigned TICK = 250;   // watch_thread's sleep
constexpr unsigned PING = 1000;  // [net] ping_ms  -- also the "a pass is late" threshold
constexpr unsigned RXTO = 10000; // [net] rx_timeout_ms

// How long a peer looks silent after a pass, once the blind interval is credited back.
unsigned silence_after_credit(unsigned raw_silent_ms, unsigned elapsed_ms) {
    const unsigned blind = mh_watchdog_blind_ms(elapsed_ms, TICK, PING);
    return raw_silent_ms - blind;
}

} // namespace

int run_watchdogtest() {
    printf("=== watchdogtest (D16: a watchdog may only charge a peer for time it was WATCHING) ===\n");

    // ---- 1. what counts as a blind pass ---------------------------------------------------------
    check("an on-time pass is not blind", mh_watchdog_blind_ms(TICK, TICK, PING) == 0);
    check("a pass shorter than the tick is not blind", mh_watchdog_blind_ms(10, TICK, PING) == 0);
    // RULES OUT the vacuous-credit implementation: ordinary scheduling jitter must NOT be credited,
    // or every pass forgives a few ms, the deadline never arrives and the watchdog never fires.
    check("ordinary jitter (400 ms pass) is not blind", mh_watchdog_blind_ms(400, TICK, PING) == 0);
    check("a pass one ping interval late is exactly at the threshold",
          mh_watchdog_blind_ms(TICK + PING, TICK, PING) == PING);
    // A send blocked on SO_SNDTIMEO is the real generator of these.
    check("a 5 s blocked send is credited in full",
          mh_watchdog_blind_ms(TICK + 5000, TICK, PING) == 5000);

    // ---- 2. THE BUG. A live peer must survive a starved watchdog ---------------------------------
    {
        // The 2026-08-02 shape: two consecutive SO_SNDTIMEO stalls hold the conn lock ~10 s. The
        // peer is fine and has been answering; we simply could not look. Un-credited, `now - last_rx`
        // is 10250 ms and the peer is dropped -- and symmetrically it drops us, because our silence
        // was equally involuntary.
        const unsigned elapsed = TICK + 10000; // one pass, 10 s of it spent blocked
        const unsigned raw     = 10250;        // how silent the peer LOOKS afterwards
        check("without crediting, a starved watchdog would drop a live peer",
              mh_watchdog_should_drop(raw, RXTO));
        check("crediting the blind interval keeps the live peer",
              !mh_watchdog_should_drop(silence_after_credit(raw, elapsed), RXTO));
    }
    {
        // Same, split across two 5 s stalls, because that is what two blocked sends actually produce.
        unsigned silent = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const unsigned elapsed = TICK + 5000;
            silent += elapsed;
            silent = silence_after_credit(silent, elapsed);
        }
        check("two 5 s stalls in a row still do not drop a live peer",
              !mh_watchdog_should_drop(silent, RXTO));
    }

    // ---- 3. THE CLAUSE THAT STOPS A VACUOUS FIX --------------------------------------------------
    // A genuinely dead peer must STILL be dropped. Without this, "credit everything" passes block 2
    // and quietly retires R-live -- the feature whose whole purpose is noticing a blackholed link
    // (the internet-play notes; the 2026-07-26 session where a host advertised into a dead socket for
    // ~90 s and started the match anyway).
    {
        // The watchdog is running normally -- every pass on time -- and the peer says nothing.
        unsigned silent  = 0;
        bool     dropped = false;
        for (int pass = 0; pass < 60 && !dropped; ++pass) { // 60 * 250 ms = 15 s of real watching
            silent += TICK;
            silent  = silence_after_credit(silent, TICK); // on-time pass -> credits nothing
            dropped = mh_watchdog_should_drop(silent, RXTO) != 0;
        }
        check("a truly silent peer is still dropped when the watchdog is healthy", dropped);
    }
    {
        // And it is dropped at the RIGHT time, not early -- the other half of linktest's contract
        // ("a silent peer is dropped, a quiet one is not").
        check("a peer quiet for 9.9 s is not dropped yet", !mh_watchdog_should_drop(9900, RXTO));
        check("a peer quiet for exactly the timeout is dropped", mh_watchdog_should_drop(RXTO, RXTO));
    }
    {
        // The documented escape hatch for a peer parked under a debugger (rx_timeout_ms=-1 -> 0).
        check("rx_timeout_ms=0 disables the drop entirely", !mh_watchdog_should_drop(600000, 0));
    }

    // ---- 4. `now - last_rx` IS NOT THE SILENCE (2026-08-30) --------------------------------------
    // MEASURED, on a LAN determinism run that had been carrying data all match:
    //     [08:22:31.068] net: conn 0 silent 4294967280 ms (keepalives: sent 57, received 57)
    //     [08:22:31.068] net: conn 0 dropped -- no data from peer within the link timeout
    //     [08:22:31.069] ; GameRecv sender=0 len=9 type=0x02        <-- the "dead" peer, 1 ms later
    // 4294967280 is -16 as unsigned: last_rx was SIXTEEN MILLISECONDS AHEAD of the watchdog's `now`.
    // Two threads, two GetTickCount() readings -- the watchdog samples `now` before taking
    // g_conn_cs, the recv thread stamps last_rx with no lock -- so a frame arriving while the
    // watchdog waits for the lock times a peer OUT OF THE FUTURE. The subtraction wraps and sails
    // past any timeout. Both peers then eliminated each other and the match desynced.
    //
    // The direction matters and is what the fix turns on: a negative difference means "heard from
    // even more recently than we thought", which is ZERO silence. It is the only reading that is not
    // absurd, and the raw subtraction picks the most absurd one available.
    {
        check("a last_rx stamped 16 ms in the future is zero silence, not 4.29e9",
              mh_watchdog_silence_ms(1000000u, 1000016u) == 0u);
        check("...and therefore does not drop a live peer",
              !mh_watchdog_should_drop(mh_watchdog_silence_ms(1000000u, 1000016u), RXTO));
        check("the raw subtraction is what would have dropped it",
              mh_watchdog_should_drop(1000000u - 1000016u, RXTO));

        // The blind-time credit reaches the same state from the other side: it pushes last_rx
        // FORWARD, so a peer heard from 100 ms ago plus a 1.2 s credit lands 1.1 s in the future.
        const unsigned now = 5000000u, last_rx = now - 100u, credit = 1200u;
        check("a credit larger than the silence would put last_rx in the future",
              mh_watchdog_silence_ms(now, last_rx + credit) == 0u);

        // NOT VACUOUS: ordinary silence still measures as itself, and a real timeout still fires.
        check("ordinary silence is unchanged", mh_watchdog_silence_ms(1000000u, 990000u) == 10000u);
        check("a real timeout still drops",
              mh_watchdog_should_drop(mh_watchdog_silence_ms(1000000u, 990000u), RXTO));
        // And it survives GetTickCount()'s 49.7-day wrap, which is the whole reason the test is
        // "difference > 2^31" rather than "last_rx > now".
        check("a stamp from just before the tick wrap still reads as ordinary silence",
              mh_watchdog_silence_ms(5000u, 0xFFFFFFFFu - 4999u) == 10000u);
    }

    printf("=== %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
