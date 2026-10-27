//
// sim/move/twin_hit_c.h -- mods:M1H lane C: the libmh twins' hit counters. The mh.dll splice stubs bump g_mv_hit[] (seams/move_carrier.h)
// and the carrier report logs `; move hits ...`; a twin runs inside libmh.dll where that array is not reachable, so it counts here and
// says so in the match log itself: `; move twin hits <site>=<n>` on the 1st, 2nd, 4th, 8th ... hit of a site (powers of two: bounded
// output, deterministic, no clock). The site names are the carrier's own (k_hit_name in move_carrier.cpp), so a row greps one vocabulary.
// Counting never feeds back into the sim.
//
#pragma once

#include <cstdint>

#include "ai/ai_state.h" // mh::ai::ai_say -- the shared log sink

namespace mh::sim::mvc {

enum twin_site : int { TS_BLOCKED,
                       TS_ND,
                       TS_MK,
                       TS_MK2,
                       TS_SPEED_COMMIT,
                       TS_SPEED_WALK,
                       TS_ARR_RETRY,
                       // mods:M1C keys (attack_range_footprint branches, projectile_facing_fix): counted only with the key ON.
                       TS_ARF_ISSUE_REPOS,
                       TS_ARF_ISSUE_ALT,
                       TS_ARF_JIT_REPOS,
                       TS_ARF_JIT_ALT,
                       TS_ARF_JIT_WALK,
                       TS_ARF_WALK,
                       TS_ARF_FIRE,
                       TS_ARF_REAIM,
                       TS_ARF_STEP,
                       TS_PFF,
                       TS_REVERSE,
                       TS_TURN,
                       TS_CAPOK,
                       TS_EQ, // mods:M1BE move_enter_queue: an entering unit waits behind an entering friend
                       TS_DH, // mods:M1BE storage_door_hold: the open door was kept open for an entering unit
                       TS_QF, // mp:U75 move_queue_flank_guard: the shuffle search refused a corner-cutting diagonal or an over-deep descent
                       TS_COUNT };

inline const char *twin_site_name(int s) {
    static const char *const names[TS_COUNT] = {"blocked", "nudge", "mk", "mk2", "speed_commit", "speed_walk", "arr_retry",
                                                "arf_issue_repos", "arf_issue_alt", "arf_jit_repos", "arf_jit_alt", "arf_jit_walk", "arf_walk",
                                                "arf_fire", "arf_reaim", "arf_step", "proj_facing", "reverse", "turn", "replan_cap", "enter_queue", "door_hold", "queue_flank"};
    return (s >= 0 && s < TS_COUNT) ? names[s] : "?";
}

inline uint32_t *twin_hit_counts() {
    static uint32_t counts[TS_COUNT];
    return counts;
}

inline void twin_hit(twin_site s) {
    const uint32_t n = ++twin_hit_counts()[s];
    if ((n & (n - 1)) == 0) mh::ai::ai_say("; move twin hits %s=%u (libmh twin)\n", twin_site_name(s), n);
}

} // namespace mh::sim::mvc
