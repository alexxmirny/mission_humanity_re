//
// sim/move/enter_queue.h -- mods:M1BE: soldiers queueing at a storage door (user playtest 0.2.1-rc22, tmp/m1be/REPORT.md). Header-only; the
// retail splices (mh/seams/move_proto_nudge.cpp, move_proto_blocked.cpp, door_hold.cpp) and the libmh twins (sim_unit_state_move_walker.cpp,
// sim_bldg_anim_state_online.cpp) run the same code, so a unit decides the same way in both configurations.
//
// THE PROBLEM. A group ordered into a storage (ORDER 0x24 "enter", home_storage_slot = the storage, every member aims at the ONE door
// tile) enters one unit at a time: the unit on the door tile boards, the next one hops onto the tile (~1 s), and the door, which is
// open only while a unit STANDS on the door tile (mutex or waiter), closes in the gap (+3.6 s per unit at low power). The blocked-unit
// keys of M1H (move_block_wait_ms 1.5 s, move_keep_sidestep, move_repath_cooldown_ms) also treat the queue as a traffic jam: a unit
// blocked behind an entering friend sidesteps, replans onto an occupied goal and takes marshal strikes; 4 strikes and it goes idle
// outside (units 7, 8 of the report).
//
//   move_enter_queue (B)   a unit with ORDER 0x24 is EXEMPT from those three keys (the retail ~10 s wait, no kept sidestep, no
//                          cooldown), and a unit with ORDER 0x24 whose next tile holds an own friend with the SAME order and the
//                          SAME storage WAITS for it (no detour request, so no sidestep is spliced into its path and no replan).
//                          Behind a friend that does not enter, everything is as before.
//   storage_door_hold (C)  the door stays open (the open state 2 does not go to closing) while an own unit with ORDER 0x24 for this
//                          storage stands within DH_RADIUS tiles of the door tile and is still on the map. Only the "close" decision
//                          of the open state is touched: leaving units already keep the door open through the mutex, and a closed
//                          door still opens on the retail rule (mutex or waiter).
//
// DETERMINISM: integer arithmetic only, units scanned in index order, no clock, no rand. Reads unit records, tile objects and the
// storage record; writes nothing. No new state.
//
// W (the world):  unit_t *unit(uint32_t player, int32_t idx)  (null out of range);  tile_t &tile(uint32_t x, uint32_t y);
//                 uint32_t mask_x(); uint32_t mask_y();  int32_t unit_limit()  (records per player row);
//                 void hit_eq(); void hit_dh();  the hit counters.
//
#pragma once

#include <cstdint>
#include <cstring>

namespace mh::sim::mvc {

constexpr uint16_t EQ_ORDER_ENTER = 0x24;
constexpr int      DH_RADIUS      = 2; // tiles (Chebyshev, wrapped) from the door tile

namespace eqd {

inline uint64_t raw_bits(const double &d) {
    uint64_t v;
    std::memcpy(&v, &d, sizeof(v));
    return v;
}
template <class U>
inline bool alive(const U &u) {
    const uint64_t b = raw_bits(u.energy);
    return (b >> 63) == 0 && (b << 1) != 0;
}
inline int wrap_dist(uint32_t from, uint32_t to, uint32_t mask) {
    int d = static_cast<int>((to - from) & mask);
    if (d > static_cast<int>((mask + 1) / 2)) d = static_cast<int>(mask + 1) - d;
    return d;
}

} // namespace eqd

// ---- B: the walker asks "do I wait for the unit on my next tile?" -----------------------------------------------------------
// (sx, sy) = the walker's tile, (dx, dy) = the tile it wants; both hold units. true = wait (the caller's detour check returns 0).
template <class W>
inline bool eq_wait_behind_enterer(W &w, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    const auto &tS = w.tile(sx, sy);
    const auto &tT = w.tile(dx, dy);
    if ((tS.class_owner & 0x80u) == 0 || (tT.class_owner & 0x80u) == 0) return false; // a building / terrain blocks: retail
    const uint32_t ap = tS.class_owner & 0xfu, bp = tT.class_owner & 0xfu;
    if (ap != bp) return false; // enemy / ally: retail
    const int32_t ai = tS.building, bi = tT.building;
    if (ai == bi) return false;
    auto *A = w.unit(ap, ai);
    auto *B = w.unit(bp, bi);
    if (!A || !B || A->x != sx || A->y != sy || B->x != dx || B->y != dy) return false;
    if (A->order != EQ_ORDER_ENTER || B->order != EQ_ORDER_ENTER) return false;
    if (A->home_storage_slot != B->home_storage_slot) return false; // another storage: retail
    if (!eqd::alive(*B)) return false;
    return true;
}

// ---- C: may the open door close? ----------------------------------------------------------------------------------------------
// player / slot = the building's owner and storage slot; (ex, ey) = the storage's door tile (exit_tile_x / y). true = an entering
// friend is near enough: the door stays open.
template <class W>
inline bool dh_entering_near(W &w, uint32_t player, int32_t slot, int32_t ex, int32_t ey) {
    const int32_t  lim = w.unit_limit();
    const uint32_t mx = w.mask_x(), my = w.mask_y();
    for (int32_t idx = 1; idx < lim; ++idx) {
        auto *u = w.unit(player, idx);
        if (!u || u->order != EQ_ORDER_ENTER || u->home_storage_slot != slot) continue;
        if (!eqd::alive(*u)) continue;
        // still OUTSIDE: a docked or walking-in unit is off the tile map (board frees its tile)
        const auto &t = w.tile(u->x, u->y);
        if (t.building != idx || t.class_owner != static_cast<uint8_t>(player | 0x80u)) continue;
        if (eqd::wrap_dist(u->x, static_cast<uint32_t>(ex), mx) <= DH_RADIUS && eqd::wrap_dist(u->y, static_cast<uint32_t>(ey), my) <= DH_RADIUS) {
            w.hit_dh();
            return true;
        }
    }
    return false;
}

} // namespace mh::sim::mvc
