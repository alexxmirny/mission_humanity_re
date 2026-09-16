//
// sim/sim_bldg_grant_type_resources.h -- credit a building TYPE's configured resource grants to a
// player (RI-SIM / SIM1B building_tick machinery slice):
//
//   llm_strat_bldg_grant_type_resources @0x004937bf (124 bytes)
//   void __watcall llm_strat_bldg_grant_type_resources(game_t_Player player_idx,
//                                                       cfg_t_building_index building_type_idx)
//   (committed prototype -- tools/data/dll_call_protos.json's own ctypes: player_idx -> uint32_t,
//   building_type_idx -> int32_t; matches sim_bldg_pay_costs.h's uint32_t/int32_t pair for the same
//   two semantic typedefs).
//
// Walks `Building[building_type_idx].resource[0..6]` (CFG_RESOURCE_SLOTS, sim_state.h) and credits
// each (id, val) pair to `player_idx` via llm_resource_add -- an ORIGINAL callee, already committed
// (mh_calls.gen.h) and already used by sim_unit_refund.cpp's `unit_refund_build_cost_by_health`.
// Indirected through a one-member `grant_type_resources_calls` struct, same reasoning as every other
// module here (a direct `mh::call::` inside `detail::` would be untestable by net_selftest.exe
// simtest / the offline fixture) -- matching sim_unit_refund.h's own one-member `_calls` struct.
//
// NO SHARED-WRITE REGION OF ITS OWN: `writes_shared: []` / `writes_island: []` in
// tools/data/sim_migration.json (checked, both empty) -- this function writes `player_resources`
// only THROUGH llm_resource_add, an original callee; see sim_state.h's own comment on why
// `player_resources` has "no writer directly here" in this closure.
//
// ---- THE ADDRESSING (0x004937e3-0x00493830) -----------------------------------------------------
// `Building[building_type_idx].resource[i]` (IMUL building_type_idx,0x842 [sizeof(cfg_building)] +
// i*8 [sizeof one (id,val) pair], base 0xd9f376 for `.id` / 0xd9f37a for `.val`) -- the SAME
// `resource` array and the SAME base address (0xd9f376) sim_bldg_pay_costs.h's own derivation cites
// for `llm_bldg_pay_build_cost`'s walk (that header's point (3): "0xd9f376 vs 0xd9f3b6, exactly 0x40
// apart" for `resource` vs `resource_2`) -- confirming this is `v.cfg_buildings[building_type_idx]
// .resource[i]`, ordinary indexing, no new accessor needed.
//
// ---- THE LOOP GUARD, id-READ-BEFORE-BOUND-CHECK (same evaluation order as sim_bldg_pay_costs.h /
// sim_unit_refund.cpp's precedent) -------------------------------------------------------------
// The asm reads `resource[i].id` FIRST on every loop-body entry (0x004937f2), tests it against 0
// (UNDEFINED, 0x004937fb/0x004937ff -- JZ exits), and ONLY THEN tests `i < 7` (0x00493801/0x00493805
// -- JL continues, otherwise falls through to the same exit). Reproduced literally as an unbounded
// `for(;;)` with two ordered breaks, matching sim_bldg_pay_costs.h's/sim_unit_refund.cpp's identical
// shape. The cfg parser only ever fills 4 of the 7 declared slots (sim_state.h's own
// CFG_RESOURCE_SLOTS comment), so `i==7`'s OOB read (into whatever field follows `resource` on
// `cfg_building`) is unreached by any shipped cfg -- and is PROVABLY INERT regardless, by the same
// general argument sim_bldg_pay_costs.h's own header gives for its identical loop shape: since `i`
// only ever increments by 1 from 0, the first time it can equal 7 is only after slots 0..6 all had a
// non-UNDEFINED id (the id-check never fired early), and at that exact point the SECOND check
// (`i < 7`) is guaranteed false -- so the loop exits on the very next check regardless of what
// garbage bytes the OOB `resource[7].id` read happens to contain. The argument is generic to the
// "id-check, then bound-check, unbounded ++i from 0" shape and does not depend on which field
// follows `resource` in memory, so it transfers here unmodified. Not carried into uncertainties[].
//
// `player_idx` is truncated to its low 16 bits (`MOVZX ..., word ptr [player_idx]`, 0x00493821)
// EVERY time it feeds the call -- computed once here (`player_idx & 0xffffu`), matching
// sim_bldg_pay_costs.cpp's/sim_unit_refund.cpp's own single-local truncation rather than
// re-deriving it per iteration (the original re-derives it inside the loop too, but the value never
// changes across iterations, so hoisting is behaviourally identical -- same reasoning
// sim_unit_refund.cpp's header gives for relocating its own dead early read).
//
// Return type void; no early return anywhere in the body (only the one shared loop-exit path).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. See the header banner on why this is a one-member
// struct rather than a direct `mh::call::` inside `detail::` -- same shape as
// sim_bldg_pay_costs.h's `pay_costs_calls` / sim_unit_refund.h's own single-member `_calls` struct.
struct grant_type_resources_calls {
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount); // llm_resource_add @0x00497f4a
};

const grant_type_resources_calls &live_grant_type_resources_calls();

namespace detail {

// llm_strat_bldg_grant_type_resources @0x004937bf. See the header banner for the full derivation.
void bldg_grant_type_resources(const sim_view &v, const grant_type_resources_calls &c,
                               uint32_t player_idx, int32_t building_type_idx);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

void bldg_grant_type_resources(uint32_t player_idx, int32_t building_type_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
