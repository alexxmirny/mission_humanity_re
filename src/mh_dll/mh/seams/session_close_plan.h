//
// seams/session_close_plan.h -- the SEQUENCING of a match's close, with the actions bound late
// (tooling:TL-HARN-CLEANCLOSE).
//
// THE GAP THIS CLOSES. mp_session_close (net_discovery.cpp) is where a match writes its end-of-match
// evidence: the desync detector's rollup (mh::desync::match_end, mp:RM1/D31), the inbound-queue
// rollup + match boundary (MH_Net_QueueMatchBoundary, mp:U41b) and SESSION_END + session.json. Every
// real exit seam reaches it -- gameover, quit, leave, timeout. A DETERMINISM run reaches none of
// them: the harness stops hashing at stop_step, the process plays on, and the runner kills it from
// outside (taskkill /f). So not one determinism run on the rig ever carried those lines, and two
// rows read the silence as something else (mp:D31's "no sample reached a comparison", mp:U41c's
// missing rollups).
//
// THE FIX IS A SECOND ENTRY INTO THE SAME CLOSE, taken by the harness at its stop step: the match
// is closed IN PLACE -- the same record, rollups and SESSION_END, in the same order, into the same
// session directory -- and the directory is NOT switched back to the process folder. Why not the
// full close, directory switch included:
//   * the process keeps running until the runner kills it (exit_on_stop=0 in every determinism
//     shape), and every mh.dll stream re-resolves its folder per line. With the switch, the post-stop
//     tail of mh_lockstep.log / mh_net.log would land in the PROCESS folder, and ui_test's
//     pull_peer_logs appends session folders AFTER the process folder -- so the tail would be read
//     FIRST, ahead of the match. analyze_lockstep takes rows[0] as the start of the run (span, the
//     icon-rate deltas): those numbers would silently change meaning on every determinism run.
//   * the lobby-level resets the full close also does (the chat codepage, the map-transfer
//     bookkeeping, U40's re-host/re-dial) are about the NEXT lobby, and a determinism run has none.
// Everything the in-place close skips is still done by the first REAL exit seam that follows, if one
// ever does (a long run that reaches gameover): that close switches the directory, runs the resets
// and U40, and writes no SECOND record -- the match already has one.
//
// THE ONE CASE WITH NO SESSION: the force-entry path (mp_run) never opens a lobby, so there is no
// session to close. There the harness stop writes the two rollups alone (no SESSION_END -- there is
// no session record to end), and only if a transport is running and no session was EVER opened: a
// process whose session already closed through a real seam has had its rollups, and a second pair
// would split one match's counters across two lines.
//
// OS-FREE AND HEADER-ONLY so net_selftest.exe's sessiondirtest drives these exact functions with
// recording ops -- the only way to prove the sequencing without staging a match on the rig.
//
#pragma once

namespace mh::session_close {

// The per-process state the two entries share. Zero-initialised is the boot state.
struct state {
    bool closed_in_place; // the open session already has its record (a harness stop wrote it)
    bool ever_opened;     // a session was opened at least once in this process
    bool stop_done;       // the harness stop is one-shot
};

// What the live file binds (net_discovery.cpp) and what the selftest records. Plain function
// pointers: the live side has no state to capture, and a test that needs one uses a global.
struct ops {
    bool (*session_active)();           // MH_RunDir_SessionActive
    bool (*net_started)();              // MH_Net_IsStarted
    void (*resets)();                   // the lobby-level resets (codepage, map-transfer bookkeeping)
    void (*record)(const char *reason); // desync rollup + queue boundary + SESSION_END + session.json
    void (*end_dir)();                  // MH_RunDir_SessionEnd -- the directory switch
    void (*tail)(const char *reason);   // U40: re-host / re-dial at the match-end reasons
    void (*rollups_only)();             // desync rollup + queue boundary, with no session to end
    void (*detector_stop)();            // mh::desync::stop_sampling -- the harness stop only
};

inline void on_open(state &s) {
    s.ever_opened     = true;
    s.closed_in_place = false; // a NEW session has no record yet
}

// Every real exit seam's close (mp_session_close). Idempotent: only the first call on an open session
// acts. The ORDER is the pre-TL-HARN-CLEANCLOSE order exactly -- resets, record, directory, U40 -- and a
// session the harness already closed in place skips only the record.
inline bool close(state &s, const ops &o, const char *reason) {
    if (!o.session_active()) return false;
    o.resets();
    if (!s.closed_in_place) o.record(reason);
    s.closed_in_place = false;
    o.end_dir();
    o.tail(reason);
    return true;
}

enum class stop_result {
    closed_in_place, // an open session got its record; the directory stays
    rollups_only,    // no session ever opened (force-entry): the two rollups alone
    nothing,         // no transport, or the match already closed through a real seam
    repeated,        // a second harness stop in one process -- ignored
};

inline const char *stop_result_name(stop_result r) {
    switch (r) {
        case stop_result::closed_in_place: return "in_place";
        case stop_result::rollups_only: return "rollups_only";
        case stop_result::nothing: return "nothing";
        default: return "repeated";
    }
}

// The harness's stop-step entry (MH_Session_HarnessStop). Never switches the directory, never runs
// the lobby resets or U40 -- see the header comment for why.
//
// AND IT STOPS THE DESYNC DETECTOR after its rollup. The game plays on until the runner's kill, and
// the detector keeps sampling on its own hook (the chained promoted root, or the ship trampoline) --
// measured: two more STATUS lines per peer after the rollup, with the counters match_end() had just
// zeroed. mp_analyze reads the LAST STATUS line as the peer's verdict, so without the stop that
// verdict would describe a few post-match seconds instead of the match. The next
// session_begin_multi restarts sampling (desync::session_reset), so a later match is unaffected.
inline stop_result harness_stop(state &s, const ops &o, const char *reason) {
    if (s.stop_done) return stop_result::repeated;
    s.stop_done = true;
    if (o.session_active()) {
        if (s.closed_in_place) return stop_result::nothing;
        o.record(reason);
        o.detector_stop();
        s.closed_in_place = true;
        return stop_result::closed_in_place;
    }
    if (!s.ever_opened && o.net_started()) {
        o.rollups_only();
        o.detector_stop();
        return stop_result::rollups_only;
    }
    return stop_result::nothing;
}

} // namespace mh::session_close
