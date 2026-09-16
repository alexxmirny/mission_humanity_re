//
// ai/ai_construction_sites.h -- the mine/relay/turret expansion-site scanner (RI-AI / AI1A layer 3).
//
// One function: picks a buildable type from the player's four alternate mine-type candidates
// (player_data::ai_mine_alt_candidates), finds a placement site for it, and queues construction.
//
// Two entirely different site-search strategies live in one body, selected by a single bit
// (player_data::ai_build_plan_len_and_flag's 0x80000000 high bit, the "ring/spiral-scan-around-
// threat" mode documented on that field in addr/mh_structs.gen.h):
//   - NORMAL mode (bit clear): the ordinary "already queued a turret?" / "expand_gate_value /
//     building-count thresholds met?" / "turret-to-other ratio not exceeded?" gates all apply, and
//     the site search is a full torus sweep keyed off the per-player ai_tile_flags_grid plane.
//   - SPIRAL mode (bit set): ALL THREE of those gates are skipped entirely -- the function goes
//     straight to drawing a build type and spiralling out from a quadrant offset of the player's
//     home tile. This is not just "a different sweep", it also disables the ordinary pacing/ratio
//     checks; see the .cpp for the exact branch this comes from (0x004e4298/0x004e429f).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_scan_construction_sites @0x004e426b.
//
// Returns 1 when it placed NOTHING (every bail-out path shares the tail at LAB_004e42c5) and 0 when
// it queued a building. Ghidra had this typed `void` previously -- the return is real, it is
// what the caller uses to abort the remaining build categories for this tick, and it is compared by
// the shadow site.
//
// `category` (0..3) selects one of the four QUADRANT_DX2/DY2 offsets and is used ONLY on the spiral
// path; the normal full-map sweep ignores it entirely.
int32_t scan_construction_sites(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player, int32_t category);

} // namespace detail

int32_t scan_construction_sites(int32_t player, int32_t category);

} // namespace mh::ai
