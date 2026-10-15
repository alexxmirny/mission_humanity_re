// mh_crew_reminder.h -- hotfix rc9.4: THE "OUT OF PEOPLE TO DRIVE VEHICLES" REMINDER.
//
// Retail shows the advisor text "You have run out of people to drive vehicles" (text 0x7f human /
// 0xb0 alien, queued by llm_strat_race_alert_text_emit 0x00425b2a) ONCE: in
// llm_strat_unit_state_exit_storage_begin 0x0047efc6, when a LOCAL-player (PlayerSide) vehicle leaving
// a garage/shuttle lacks crew (idle population + shuttle-reserved passengers < Unit[proto].human, and no
// soldiers aboard). The unit then sits in EXIT_WAIT (0x22) and llm_strat_unit_state_exit_wait 0x0047f270
// re-checks llm_strat_storage_can_exit every tick in silence -- a field player with 0 idle people
// clicked "Exit all" ten times over 50 s and never learned why nothing moved.
//
// The fix ([ui] crew_shortage_reminder_s, default 30; 0 = retail): while such a unit waits, exit_wait
// calls the same emitter again once `period_s` seconds of GAME clock have passed since the text was last
// SHOWN -- by us or by retail (the emitter's LAST_PLAY_TIME stamp moving forward counts as a reminder).
// The emitter keeps its own 2 s cooldown and beeps after 6 suppressed calls, so we never call it inside
// that cooldown: a due reminder that would land there waits (the throttle is not advanced) and fires on
// the first tick the emitter would accept. DISPLAY-ONLY: the emitter writes its own plain statics
// (_G_LLM_STRAT_RACE_ALERT_TEXT_LAST_PLAY_TIME / _SUPPRESSED_COUNT, not hash regions) and the print
// queue; the throttle below is a per-process static. No sim write, no sim RNG.
//
// Two carriers, one rule: the retail splice (mh/seams/ui_crew_reminder.cpp) and libmh's exit_wait twin
// (libmh/sim/sim_unit_state_exit.cpp, reimpl_fixes::crew_shortage_reminder_s). Header-only so both
// compile it.
#pragma once

namespace mh::crew_reminder {

struct throttle {
    double last  = 0.0; // game clock of the last show we know of (ours or retail's)
    bool   armed = false;
};

// The emitter's text cooldown, _G_LLM_STRAT_RACE_ALERT_TEXT_COOLDOWN_SEC @0x005001ae (boot-constant
// double, read 2.0). The retail splice reads the live value; libmh's twin, which has no view of the
// emitter's statics, uses this.
inline constexpr double EMITTER_COOLDOWN_S = 2.0;

// True when a reminder should be emitted at game clock `now` (seconds); records it. `shown` is the
// emitter's LAST_PLAY_TIME (when the text was last queued), `cooldown` its cooldown.
//  - the clock went BACKWARDS (a load or a new match): our stamp belongs to another timeline -- forget it;
//  - a show we have not seen (shown > last, and not in the future) counts as a reminder: restart from it;
//  - due once period_s has passed since the last show (or on the first call);
//  - but never inside the emitter's cooldown: return false WITHOUT advancing, so a later tick retries.
// period_s <= 0: never (retail).
inline bool due(throttle &t, double now, int period_s, double shown, double cooldown) {
    if (period_s <= 0) return false;
    if (t.armed && now < t.last) t.armed = false;
    if (shown <= now && (!t.armed || shown > t.last)) {
        t.armed = true;
        t.last  = shown;
    }
    if (t.armed && now - t.last < (double)period_s) return false;
    if (now < shown + cooldown) return false;
    t.armed = true;
    t.last  = now;
    return true;
}

// libmh's stand-in for the emitter's LAST_PLAY_TIME: the emitter's own rule (queue + stamp only outside
// the cooldown), applied at every emitter call libmh makes. A backwards clock forgets the stamp (retail's
// reload path re-bases it). Returns true when the emitter will queue the text.
struct emitter_mirror {
    double stamp = -1.0e18;
};
// The mirror's stamp as of `now` (a backwards clock forgets it first).
inline double mirror_stamp(emitter_mirror &m, double now) {
    if (now < m.stamp) m.stamp = -1.0e18;
    return m.stamp;
}
inline bool mirror_emit(emitter_mirror &m, double now, double cooldown) {
    if (now < m.stamp) m.stamp = -1.0e18;
    if (now < m.stamp + cooldown) return false;
    m.stamp = now;
    return true;
}

} // namespace mh::crew_reminder
