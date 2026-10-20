//
// sim/move/guards.h -- mods:M1H lane B: the two overrun guards of the route planner, the pure rules.
//
//   [net] move_detour_slot_guard   llm_strat_unit_path_detour 0x004209b2, step 3 (0x0042172e): when every candidate failed the
//                                  original recurses into the OCCUPANT of the blocking tile (`alt_unit_idx`). An occupant
//                                  that has no path slot (path_slot_id == 0xff) would index player*30000 + 255*300, past
//                                  the player's 100 slots -- the recursion reads and splices another player's path memory.
//                                  Guard: skip the recursion, return 1 ("no detour"), exactly as for alt == 0.
//   [net] move_flood_queue_guard   llm_map_region_flood_reachable 0x00424ee0 (0x00424f62): the flood's work queue is a
//                                  4096-slot ring (`& 0xfff`). A front larger than the ring makes the write cursor lap the
//                                  read cursor, overwriting live entries (or, when it laps exactly, ending the loop early
//                                  with a truncated field). Guard: stop the flood when 0xff8 entries are pending.
//
// Shared by the retail splices (mh/seams/net_lockstep.cpp) and the libmh twins (sim_unit_path_detour.cpp,
// sim_map_region_routing.cpp). Integer only, no state.
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

constexpr uint8_t  NO_PATH_SLOT        = 0xff;
constexpr uint32_t FLOOD_RING_MASK     = 0xfff;
constexpr uint32_t FLOOD_PENDING_LIMIT = 0xff8;

// True = do NOT recurse into the occupant (return "no detour").
inline bool detour_skip_occupant(bool fix, uint8_t occupant_path_slot_id) { return fix && occupant_path_slot_id == NO_PATH_SLOT; }

// True = stop the flood now (the ring has FLOOD_PENDING_LIMIT or more entries pending). Only meaningful with read != write.
inline bool flood_queue_full(bool fix, uint32_t read_idx, uint32_t write_idx) {
    return fix && ((write_idx - read_idx) & FLOOD_RING_MASK) >= FLOOD_PENDING_LIMIT;
}

} // namespace mh::sim::move
