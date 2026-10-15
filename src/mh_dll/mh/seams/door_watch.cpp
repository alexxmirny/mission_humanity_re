//
// seams/door_watch.cpp -- the garage-door WATCHDOG (field report 01M4XY20TX6QCEKDJGHYPMWM6A, triage
// tmp/report_triage_2026-10-14/garage.md + garage_repro.md). OBSERVE ONLY: reads sim state, writes
// nothing but log lines.
//
// THE BUG IT WAITS FOR. Reproduced offline (garage_repro.md): a garage's vehicles sat in EXIT_WAIT
// (0x22) for ~2500 steps because their owner had 0 IDLE population to crew them --
// llm_strat_storage_can_exit 0x0048a548 lets a unit out only while POP_STATS[p].human (the idle count)
// >= cfg Unit[proto].human (or the unit carries soldiers). door_mutex_unit stayed 0; the door stayed
// open because door_waiter_count rose 2 -> 6. The other shape the first triage guessed (a unit holding
// the mutex while the door never opens) is watched for too.
//
// THE TRIGGER. A storage building whose door is busy (door_waiter_count > 0 or door_mutex_unit != 0)
// with NO PROGRESS for more than STALL_STEPS match steps. Progress = the mutex changed hands or the
// waiter count went DOWN (a unit got out, or died) -- a queue walking out one by one keeps the waiter
// count up for minutes and is not a stall. Then ONE `; [door] STALLED` line names: the building
// (type/state/online_state/built_flags/efficiency/workers), the door fields, the owner's population
// ledger (idle vs total/employed/in-field), every unit of that player homed on this storage that is in
// a boarding/exit state with its crew requirement (cfg Unit human/soldier_count) and position, and the
// exit tile's passability. One `CLEARED` line follows when it moves again.
//
// COST. Every CHECK_EVERY steps, 8 players x the storage cap (25 stock) records, each a two-int test;
// the unit scan runs only for a line that is being written.
//
// DEFAULT ON, `[desync] door_watch=0` turns it off. Rides the [desync] per-step hook (desync_watch.cpp
// set_replay_plugins), so it is live in SP and MP alike, on the replay record's match-step axis.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "seams/door_watch.h"
#include "addr/mh_regions.gen.h" // mh::state::live(), the RIDs read below
#include "addr/mh_structs.gen.h" // the record layouts
#include "state/roster_caps.h"   // live_roster_caps(): the per-player caps the host bound
#include "net_internal.h"        // seam_log; g_ini

namespace {

constexpr uint32_t PLAYERS     = 8;
constexpr int      MAX_SLOTS   = 256; // tracking table width; a cap above it is clamped (and said once)
constexpr uint32_t CHECK_EVERY = 50;  // ~1 s at 50 steps/s
constexpr uint32_t STALL_STEPS = 500; // ~10 s: a unit walks through a door in ~100 steps (the report)
constexpr int      MAX_LINES   = 64;  // per match: stalls + clears, so a pathological match cannot flood
constexpr int      MAX_UNITS   = 12;  // waiting units listed per line
// The unit states between docking and leaving (garage.md: "a boarding state 0x1f..0x2b"); EXIT_WAIT
// is 0x22, EXIT_WALK_OUT 0x21.
constexpr uint16_t BOARD_LO = 0x1f, BOARD_HI = 0x2b;

bool     g_on         = false;
uint32_t g_last       = 0; // the last match step seen (a decrease = a new match)
int      g_lines      = 0;
bool     g_clamp_said = false;

struct track {
    uint32_t since;   // the match step the current no-progress episode began, 0 = idle
    int32_t  holder;  // door_mutex_unit at the last check
    int32_t  waiters; // door_waiter_count at the last check
    bool     logged;  // the STALLED line was written for this episode
};
track g_t[PLAYERS][MAX_SLOTS];

void reset() {
    memset(g_t, 0, sizeof(g_t));
    g_lines = 0;
}

template <class... A>
int put(char *b, int n, int cap, const char *fmt, A... a) {
    if (n >= cap - 1) return n;
    const int w = _snprintf_s(b + n, (size_t)(cap - n), _TRUNCATE, fmt, a...);
    return w < 0 ? cap - 1 : n + w;
}

void stall_line(uint32_t mstep, uint32_t p, int slot, const mh::game::mh_map_object_unit_storage &st,
                uint32_t since) {
    using namespace mh::state;
    const auto     caps  = live_roster_caps();
    const auto    &lv    = live();
    const auto    *bl    = reinterpret_cast<const mh::game::mh_map_object_building *>(lv.base[RID_BUILDINGS]);
    const auto    *un    = reinterpret_cast<const mh::game::mh_map_object_unit *>(lv.base[RID_UNITS]);
    const auto    *pass  = reinterpret_cast<const uint8_t *>(lv.base[RID_PASSABLE]);
    const auto    *pop   = reinterpret_cast<const mh::game::mh_llm_strat_pop_stats *>(lv.base[RID_STRAT_POP_STATS]);
    const auto    *cfu   = reinterpret_cast<const mh::game::mh_cfg_final_struct_Unit *>(lv.base[RID_UNIT]);
    const uint32_t n_cfu = REGIONS[RID_UNIT].size / (uint32_t)sizeof(mh::game::mh_cfg_final_struct_Unit);
    constexpr int  CAP   = 1400;
    char           b[CAP];
    int            n = put(b, 0, CAP, "; [door] STALLED %lu steps without progress (since match step %lu, now %lu): player %lu "
                                                 "storage %d -> building %ld",
                           (unsigned long)(mstep - since), (unsigned long)since, (unsigned long)mstep, (unsigned long)p, slot,
                           (long)st.b_index);
    if (st.b_index > 0 && st.b_index < caps.buildings) {
        const auto &bd = bl[p * (uint32_t)caps.buildings + (uint32_t)st.b_index];
        n              = put(b, n, CAP,
                             " type %u at (%u,%u) state %u online_state %d built_flags 0x%02X efficiency %.3f workers %u",
                             (unsigned)bd.building_id, (unsigned)bd.x, (unsigned)bd.y, (unsigned)bd.state, (int)bd.online_state,
                             (unsigned)bd.built_flags, bd.efficiency, (unsigned)bd.current_workers);
    }
    n = put(b, n, CAP, "; door_mutex_unit %ld waiters %ld docked %ld occupancy %ld", (long)st.door_mutex_unit,
            (long)st.door_waiter_count, (long)st.docked_count, (long)st.occupancy);
    if (pop) {
        const auto &ps = pop[p];
        n              = put(b, n, CAP, "; population idle %ld (total %ld, employed %ld, in field %ld)", (long)ps.human,
                             (long)ps.pop_total, (long)ps.workers_employed, (long)ps.human_in_field);
    }
    // Every unit of this player homed on this storage in a boarding/exit state: what the exit gate weighs.
    int listed = 0, more = 0;
    for (int32_t i = 1; i < caps.units; ++i) {
        const auto &u = un[p * (uint32_t)caps.units + (uint32_t)i];
        if (u.home_storage_slot != (uint8_t)slot || u.state < BOARD_LO || u.state > BOARD_HI) continue;
        if (listed >= MAX_UNITS) {
            ++more;
            continue;
        }
        long crew = -1, soldiers = -1;
        if (u.unit_proto_id < n_cfu) {
            crew     = (long)cfu[u.unit_proto_id].human;
            soldiers = (long)cfu[u.unit_proto_id].soldier_count;
        }
        n = put(b, n, CAP, "%s u%ld state 0x%02X proto %u crew %ld soldiers %ld at (%u,%u)",
                listed ? "," : "; waiting", (long)i, (unsigned)u.state, (unsigned)u.unit_proto_id, crew, soldiers,
                (unsigned)u.x, (unsigned)u.y);
        ++listed;
    }
    if (!listed) n = put(b, n, CAP, "; no unit homed here is in a boarding/exit state");
    if (more) n = put(b, n, CAP, " (+%d more)", more);
    if (st.door_mutex_unit > 0 && st.door_mutex_unit < caps.units) {
        const auto &u = un[p * (uint32_t)caps.units + (uint32_t)st.door_mutex_unit];
        n             = put(b, n, CAP, "; holder u%ld state 0x%02X order 0x%02X at (%u,%u) microstep %ld",
                            (long)st.door_mutex_unit, (unsigned)u.state, (unsigned)u.order, (unsigned)u.x, (unsigned)u.y,
                            (long)u.move_microstep);
    }
    const uint32_t ex = (uint32_t)st.exit_tile_x & 0xffu, ey = (uint32_t)st.exit_tile_y & 0xffu;
    n = put(b, n, CAP, "; exit tile (%ld,%ld) passable %u\n", (long)st.exit_tile_x, (long)st.exit_tile_y,
            (unsigned)pass[(ex << 8) | ey]);
    if (n >= CAP - 1) b[CAP - 2] = '\n';
    seam_log(b);
}

void clear_line(uint32_t mstep, uint32_t p, int s, const mh::game::mh_map_object_unit_storage &st,
                const track &t, const char *why) {
    if (!t.logged || g_lines >= MAX_LINES) return;
    ++g_lines;
    char b[260];
    wsprintfA(b, "; [door] CLEARED player %lu storage %d (building %ld) at match step %lu after %lu step(s): %s "
                 "(mutex %ld -> %ld, waiters %ld -> %ld)\n",
              (unsigned long)p, s, (long)st.b_index, (unsigned long)mstep, (unsigned long)(mstep - t.since), why,
              (long)t.holder, (long)st.door_mutex_unit, (long)t.waiters, (long)st.door_waiter_count);
    seam_log(b);
}

} // namespace

void door_watch_install() {
    g_on = mh_ini_get_int("desync", "door_watch", 1, g_ini) != 0;
    reset();
    if (g_on) {
        char b[240];
        wsprintfA(b, "; [door] watch armed: a storage door busy (waiters or mutex) with no progress > %lu match steps "
                     "logs one STALLED line, checked every %lu steps (observe only; [desync] door_watch=0 turns it "
                     "off)\n",
                  (unsigned long)STALL_STEPS, (unsigned long)CHECK_EVERY);
        seam_log(b);
    }
}

void door_watch_tick(uint32_t mstep) {
    if (!g_on || mstep == 0) return;
    if (mstep < g_last) reset(); // a new match (the replay record's axis restarted)
    g_last = mstep;
    if (mstep % CHECK_EVERY) return;
    using namespace mh::state;
    const auto caps  = live_roster_caps();
    int        slots = caps.storage;
    if (slots > MAX_SLOTS) {
        if (!g_clamp_said) {
            g_clamp_said = true;
            char b[160];
            wsprintfA(b, "; [door] storage cap %d > %d: only the first %d slots per player are watched\n", slots,
                      MAX_SLOTS, MAX_SLOTS);
            seam_log(b);
        }
        slots = MAX_SLOTS;
    }
    const auto *sto = reinterpret_cast<const mh::game::mh_map_object_unit_storage *>(live().base[RID_UNIT_STORAGE]);
    if (sto == nullptr || slots <= 0) return;
    for (uint32_t p = 0; p < PLAYERS; ++p) {
        for (int s = 1; s < slots; ++s) { // slot 0 is never used by the game (release_door_held_by_unit)
            const auto &st   = sto[p * (uint32_t)caps.storage + (uint32_t)s];
            track      &t    = g_t[p][s];
            const bool  busy = st.b_index != 0 && (st.door_mutex_unit != 0 || st.door_waiter_count != 0);
            if (!busy) {
                if (t.since != 0) clear_line(mstep, p, s, st, t, "the door is idle");
                t = {};
                continue;
            }
            if (t.since != 0 && (st.door_mutex_unit != t.holder || st.door_waiter_count < t.waiters)) {
                clear_line(mstep, p, s, st, t, "the door moved again");
                t = {};
            }
            if (t.since == 0) t.since = mstep;
            t.holder  = st.door_mutex_unit;
            t.waiters = st.door_waiter_count;
            if (!t.logged && mstep - t.since > STALL_STEPS && g_lines < MAX_LINES) {
                t.logged = true;
                ++g_lines;
                stall_line(mstep, p, s, st, t.since);
            }
        }
    }
}
