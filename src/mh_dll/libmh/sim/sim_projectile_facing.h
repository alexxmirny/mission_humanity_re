//
// sim/sim_projectile_facing.h -- mods:M1C [net] projectile_facing_fix (follows [net] improved_combat).
//
// llm_strat_projectile_tick 0x00440e1c re-aims a flying shot's sprite every tick (0x004413f6-0x00441468):
// `delta = dir_from_to(pos, impact) - facing; anim_loop_frame += delta * stride; anim_frame += delta * stride` with the
// 24-way facing 1..24. The difference is NOT wrapped: a shot whose direction crosses the 24/1 boundary (due south,
// facing 1 = (0,+1)) gets delta = +-23 and its frame jumps 23 strides into another animation -- the "explosion-like
// projectile midway" (2-almost repro, harness `MV PRJT`: W2 from 59,5 at 59,8, loop 2049 -> 2072 and back, frame 2074..2077).
// The direction of a near-axis shot flips across the boundary repeatedly as it flies (truncated interpolation), and the
// geometry is fixed per attacker/target pair, so every shot of that attacker shows it until the order or target changes.
//
// Rule with the key on:
//   * weapon types 5 and 6: delta = 0. Their start frame is chosen from llm_strat_dir_sector_to (a fine sector index, one
//     frame per sector -- W2: 128 frames 1924..2051), not from the 24-way facing, so a 24-way delta is the wrong unit:
//     even +-1 steps a frame that sits at a block edge out of the block. Every type 5/6 weapon in the shipped cfg is
//     non-homing (homing 2), so its direction never changes in flight and the spawn frame is already the right one.
//   * every other type: delta wrapped into -12..11 (the shorter way round the 24-way compass).
// The facing field itself is still stored as retail does. Integer only, no state.
//
// Header-only: the retail splice (mh/seams/combat_proto.cpp at 0x00441424) and the libmh twin (sim_projectile_tick.cpp,
// gated through reimpl_fixes -> set_fixes) apply the same rule.
//
#pragma once

#include <cstdint>

namespace mh::sim {

namespace projectile_facing_detail {
inline bool &flag() {
    static bool on = false;
    return on;
}
} // namespace projectile_facing_detail

inline void set_projectile_facing_fix(bool on) { projectile_facing_detail::flag() = on; }
inline bool projectile_facing_fix() { return projectile_facing_detail::flag(); }

// The facing difference the tick multiplies by the stride, under the fix.
inline int32_t projectile_facing_delta(int32_t delta, uint8_t weapon_type) {
    if (weapon_type == 5 || weapon_type == 6) return 0;
    while (delta > 11) delta -= 24;
    while (delta < -12) delta += 24;
    return delta;
}

} // namespace mh::sim
