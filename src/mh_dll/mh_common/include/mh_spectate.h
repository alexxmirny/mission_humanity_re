//
// mh_spectate.h -- mp:U54 (spectate after defeat): the PURE roster rules, shared by the libmh twin
// (sim_player_presence_lost.cpp, order_queue.cpp) and by mh.dll's retail byte-patch carriers
// (seams/net_lockstep.cpp), so the two configurations cannot disagree about who is a spectator or when a
// match is decided.
//
// A defeated human in a lockstep match with at least TWO other humans still alive is not dropped: it stays
// in the session as a SPECTATOR -- its sim keeps stepping with the survivors (per-step hash identical),
// it just is not a barrier member. Its roster flags are ALIVE off, HUMAN off, DEFEATED on and GONE OFF (the
// flip every SURVIVOR makes for it, pinned at the elimination step since mp:U56 -- minus GONE): GONE is bit 3,
// "AI-controlled / written off", the mark the leader's `packet from a written-off peer -> re-broadcast CTL_KICK`
// test (rx_dispatch.cpp, 0x0049c311) reads, and a spectator's own traffic must not be kicked. A peer that QUIT
// carries GONE (mark_player_gone), which is what tells the two apart. Its own orders are dropped before enqueue.
//
// WHEN THE MATCH IS DECIDED (the survivors' own end-of-match rules, restated from the spectator's seat):
//   * the survivors' retail evaluation (llm_strat_player_presence_lost, me != player) ends the lockstep
//     session for a survivor X as soon as no OTHER human is alive (X downgrades to a local sim vs the AI)
//     or X is the last man standing, or -- with the ally-victory rule on -- every other alive player is
//     mutually allied with X. Seen from outside that is: alive humans <= 1, or alive players <= 1, or
//     (rule on and all alive players pairwise mutually allied).
//   * the spectator ends its own match the moment THAT is true, because nobody is left to step with.
// It lives in mh_common/include, outside the mh::lockstep namespace, so mh.dll's retail carriers can use it in
// configuration (1) without a reference into the libmh closure (tools/check_net_lockstep_refs.py).
#pragma once
#include <cstdint>

namespace mh::spectate {

inline constexpr uint32_t ST_ALIVE    = 0x02u;
inline constexpr uint32_t ST_HUMAN    = 0x04u;
inline constexpr uint32_t ST_GONE     = 0x08u;
inline constexpr uint32_t ST_DEFEATED = 0x10u;

// What the rules read: the 8 status_flags words and the relation matrix (relation[a][b] == 1 : a treats b as
// an ally; "mutual" is both directions), plus the ally-victory rule flag.
struct roster {
    uint32_t flags[8];
    uint8_t  relation[8][8];
    bool     ally_rule;
};

struct verdict {
    int  alive;           // alive players, `me` excluded
    int  humans;          // alive HUMAN players, `me` excluded
    bool pairwise_allied; // alive >= 2 and every pair among them mutually allied
    bool allied_with_me;  // alive >= 1 and every alive player mutually allied with `me`
};

inline bool mutual_ally(const roster &r, int a, int b) {
    return r.relation[a][b] == 1 && r.relation[b][a] == 1;
}

inline verdict evaluate(const roster &r, int me) {
    verdict v{0, 0, true, true};
    int     idx[8];
    for (int i = 0; i < 8; ++i) {
        if (i == me || (r.flags[i] & ST_ALIVE) == 0) continue;
        idx[v.alive++] = i;
        if (r.flags[i] & ST_HUMAN) ++v.humans;
    }
    for (int a = 0; a < v.alive; ++a) {
        if (me >= 0 && me < 8 && !mutual_ally(r, me, idx[a])) v.allied_with_me = false;
        for (int b = a + 1; b < v.alive; ++b)
            if (!mutual_ally(r, idx[a], idx[b])) v.pairwise_allied = false;
    }
    if (v.alive < 2) v.pairwise_allied = false;
    if (v.alive < 1) v.allied_with_me = false;
    return v;
}

// The survivors have decided the match (see the file header).
inline bool decided(const roster &r, const verdict &v) {
    return v.humans <= 1 || v.alive <= 1 || (r.ally_rule && v.pairwise_allied);
}

// The spectator's side won: the team rule is on and every player still alive is its mutual ally.
inline bool side_won(const roster &r, const verdict &v) { return r.ally_rule && v.allied_with_me; }

// Does a human `me` that has just lost its last presence (ALIVE already cleared) become a spectator?
// `in_lockstep` = session mode 3; `natural` = presence_lost mode 0.
inline bool becomes_spectator(const roster &r, int me, bool key_on, bool in_lockstep, bool natural) {
    if (!key_on || !in_lockstep || !natural) return false;
    return !decided(r, evaluate(r, me));
}

// Is the LOCAL player (`me`) a spectator right now? Pure state, so every peer reads the same: a defeated
// human whose HUMAN bit is off and GONE bit on while the session is STILL lockstep (any other defeat
// downgrades the session to a local one before anything can ask).
inline bool is_spectator_flags(uint32_t f) {
    return (f & (ST_ALIVE | ST_HUMAN | ST_GONE)) == 0 && (f & ST_DEFEATED) != 0;
}
inline bool is_spectator(uint32_t me_flags, bool in_lockstep, bool key_on) {
    return key_on && in_lockstep && is_spectator_flags(me_flags);
}

// The flags a human that just lost its last presence is left with (HUMAN off, DEFEATED on; GONE on unless it spectates).
inline uint32_t eliminated_flags(uint32_t f, bool spectates) {
    f &= ~ST_HUMAN;
    f |= ST_DEFEATED | (spectates ? 0u : ST_GONE);
    return f;
}

} // namespace mh::spectate
