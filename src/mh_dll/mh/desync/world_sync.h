//
// desync/world_sync.h -- mp:X3c: the PRODUCT WIRING of the host-authoritative world resync.
//
// The decisions are all in desync/world_sync_core.h (pure, proven by `net_selftest.exe wstest`); this
// module only gathers their inputs from the live game / transport and performs the actions they return:
//
//   HOST      on_tick_verdict() feeds the incident detector; step_begin() runs each peer's machine
//             (BEGIN -> capture -> channel-C send + META -> DONE / ABORT / cooldown).
//   MINORITY  on_frame() takes BEGIN / META / ABORT; step_begin() polls channel C, checks the blob
//             against META, imports it through libmh_import_world_resync, rewinds the desync counters
//             and hands the catch-up to net_lockstep (horizon mirror + capped fast-forward).
//
// EVERYTHING IS INERT AT `[desync] action=0` (the default): no frame is decoded, no history is kept, no
// call is made into libmh or the transport. Log prefix `; [worldsync]` -- NOT `[resync]`, which is the
// retail mode-8 barrier's prefix that check_resync_storm.py greps.
//
// MAIN THREAD ONLY (the desync module queues frames on the recv thread and drains them on the main thread).
//
// The module reaches OUTWARD (net_lockstep's mirror / fast-forward, the harness callback) only through
// the `hooks` table below, set by the seam layer. That keeps this TU linkable into net_selftest.exe,
// which has no net_lockstep.cpp and no harness.
//
#pragma once
#include <cstdint>

#include "desync/desync_wire2.h"
#include "desync/world_sync_core.h"

namespace mh::desync::world_sync {

// what the harness callback (MH_Harness_OnWorldSync) is told
enum harness_event : int {
    WS_CAPTURE = 1, // host: a world blob was just captured for step `step` (still inside the horizon hold)
    WS_IMPORT  = 2, // minority: a world blob for step `step` was just imported; `arg` = steps to rewind
};

// net_lockstep.cpp's fast-forward state, as seen from here
enum ff_state : int {
    FF_IDLE   = 0,
    FF_ACTIVE = 1, // horizon mirror on, TOTAL_GAME_TIME capped per frame
    FF_DONE   = 2, // backlog within two caps: TOTAL released, mirror off; waiting for end()
};

struct hooks {
    void (*horizon_hold)(int on);  // MH_Lockstep_HorizonHold: same thread must release
    int (*ff_begin)(int ff_steps); // MH_Lockstep_WorldSyncBegin: mirror + cap; returns ff_state
    int (*ff_state)(void);         // MH_Lockstep_WorldSyncState
    void (*ff_end)(void);          // MH_Lockstep_WorldSyncEnd: back to idle
    // MH_Harness_OnWorldSync (may be null): WS_CAPTURE passes the fresh blob, WS_IMPORT the blob just imported
    void (*harness)(int what, uint32_t step, int arg, const void *blob, uint32_t len);
    bool (*libmh_ok)(void); // libmh bound AND [promote] orders live
};
void set_hooks(const hooks &h);

// Read `[desync] resync_*` from the ini. `action` is `[desync] action` as desync_watch parsed it.
// `fp2` is the v2 manifest fingerprint every control frame carries. Logs the ARM line for action=1.
void              configure(const char *ini_path, int action, uint64_t fp2, void (*log)(const char *fmt, ...));
bool              enabled(); // action == 1
const ws::config &cfg();

// New match: drop every in-flight transfer, free the receive buffer, zero the machines and the history.
void session_reset();

// ---- the desync module's feeds ---------------------------------------------------------------------
// Every judged per-step comparison (ok and mismatch). `host_step` is the host's current step.
void on_tick_verdict(const v2::tick_verdict &v, uint32_t host_step);
// A control frame (types 6..9) drained on the main thread. `buf` is the whole datagram.
void on_frame(int sender, const uint8_t *buf, int len, uint32_t host_step);
// The top of step_common, BEFORE the step counter advances. `next_step` = g_step + 1, `digest_now` =
// the cumulative order digest before this step's fold. Returns true if the world was replaced (the
// caller has been rewound through desync::rewind_for_world_sync).
bool step_begin(uint32_t next_step, uint64_t digest_now);

// Counters for the match-end rollup.
struct tallies {
    int begins, captures, dones, aborts, imports, skips;
};
tallies counters();

} // namespace mh::desync::world_sync
